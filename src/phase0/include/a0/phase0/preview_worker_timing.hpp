#pragma once

#include "a0/phase0/agent_pipe_timing.hpp"
#include "a0/phase0/phase0.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Timing budget of the two-worker Live View trial. This header is the single
// source for every wait the parent (PreviewWorkerOwner) and the worker
// (WorkerPreviewDispatcher, RunWorkerPreviewNamedPipeServer, the PreviewWorker
// bootstrap) derive from SDK timeouts. Nothing here interrupts an SDK call:
// the worker passes these timeouts to NikonSdkTransport, and the waiting side
// computes how long it must wait from the same numbers. The pipe host's stage
// timeouts are not repeated here: they come from agent_pipe_timing.hpp.
namespace a0::phase0::experimental::preview_worker_timing {

using std::chrono::milliseconds;
using std::chrono::seconds;

// Timeouts the worker passes to NikonSdkTransport (the transport API takes
// seconds). The values are the literals worker_preview_dispatcher.hpp used
// before this table existed.
//   BeginWorkerPreviewSelection, OpenWorkerPreviewCandidate,
//   StartSelectedWorkerLiveView, SuspendSelectedWorkerPreview,
//   ResumeSelectedWorkerPreview:
inline constexpr seconds kSdkOperationTimeout{10};
//   ReadLiveViewFrame:
inline constexpr seconds kFrameTimeout{3};
//   StopLiveView, in suspend and in CloseForShutdown:
inline constexpr seconds kStopLiveViewTimeout{10};
//   Close, in CloseForShutdown:
inline constexpr seconds kCloseTimeout{10};

// G: one Abort grace. A pending SDK command that misses its deadline is
// abandoned with Abort and then polled for completion for
// kAbortGraceIterations (25) x kAsyncInterval (10 ms) = 250 ms in
// nikon_sdk_transport.cpp. Rounded up to 1 s for the pump work and scheduling
// between polls. An SDK call that ignores its deadline is outside this budget.
inline constexpr milliseconds kAbortGrace{1000};

// The worker pipe host's post-dispatch stages: response header write,
// response body write and ACK read, agent_pipe_timing::kResponseWriteTimeout
// (1000 ms) each: 3 s.
inline constexpr milliseconds kWorkerReplyStages = 3 * agent_pipe_timing::kResponseWriteTimeout;

// Process scheduling, the delegation-marker reads around each command, and the
// parent's pipe connect polling.
inline constexpr milliseconds kIpcSchedulingAllowance{2000};

// M: IPC margin added on top of the worker's worst reply time: one set of
// post-dispatch stages plus the scheduling allowance, 5 s.
inline constexpr milliseconds kIpcMargin = kWorkerReplyStages + kIpcSchedulingAllowance;

// Budget of one worker command, as the worker spends it.
struct WorkerOperationBudget {
    std::string_view operation;     // Wire name sent by the parent.
    milliseconds sdk;               // The command's own SDK calls: every deadline the transport derives from its timeout.
    milliseconds cleanup;           // CloseForShutdown after the command fails.
    std::uint32_t abort_graces{};   // SDK calls on that path that may each add one G.
};

inline constexpr std::size_t kWorkerOperationCount = 7;
using WorkerOperationBudgetTable = std::array<WorkerOperationBudget, kWorkerOperationCount>;

// Deadline counts are taken from nikon_sdk_transport.cpp; the timeout values
// above are what the dispatcher passes.
inline constexpr WorkerOperationBudgetTable kWorkerOperationBudgets{{
    // BeginWorkerPreviewSelection: OpenModule and WaitForSourceIds share one
    // deadline. A failure leaves no Live View, so cleanup is Close only.
    {"enumerate", kSdkOperationTimeout, kCloseTimeout, 2},
    // OpenWorkerPreviewCandidate: inventory and source open share one deadline.
    {"select", kSdkOperationTimeout, kCloseTimeout, 2},
    // StartSelectedWorkerLiveView: its inventory check (1), StartLiveView's own
    // inventory check (2), then StartLiveView's start deadline (3). The
    // dispatcher has not set started_ when start throws, so cleanup is Close
    // only; the transport's Close stops Live View inside its own deadline.
    {"start", 3 * kSdkOperationTimeout, kCloseTimeout, 2},
    // ReadLiveViewFrame: inventory check (1) and frame read (2), each with
    // kFrameTimeout. A failure stops Live View, then closes.
    {"frame", 2 * kFrameTimeout, kStopLiveViewTimeout + kCloseTimeout, 3},
    // StopLiveView, then SuspendSelectedWorkerPreview (inventory and source
    // close share one deadline). Stop was already attempted, so cleanup is
    // Close only.
    {"suspend", kStopLiveViewTimeout + kSdkOperationTimeout, kCloseTimeout, 3},
    // ResumeSelectedWorkerPreview: inventory and source open share one deadline.
    {"resume", kSdkOperationTimeout, kCloseTimeout, 2},
    // close is the cleanup itself: StopLiveView (when started), then Close.
    {"close", kStopLiveViewTimeout + kCloseTimeout, milliseconds{0}, 2},
}};

// The commands the parent sends, in the order PreviewCommissioning uses them.
inline constexpr std::array<std::string_view, kWorkerOperationCount> kWorkerOperations{
    "enumerate", "select", "start", "frame", "suspend", "resume", "close"};

[[nodiscard]] constexpr const WorkerOperationBudget* FindWorkerOperationBudget(
    const WorkerOperationBudgetTable& table, std::string_view operation) noexcept {
    for (const auto& entry : table)
        if (entry.operation == operation) return &entry;
    return nullptr;
}

// True when the table has exactly one entry for each command in kWorkerOperations.
[[nodiscard]] constexpr bool CoversEveryWorkerOperation(const WorkerOperationBudgetTable& table) noexcept {
    for (const auto operation : kWorkerOperations) {
        std::size_t found{};
        for (const auto& entry : table)
            if (entry.operation == operation) ++found;
        if (found != 1) return false;
    }
    return true;
}

// W: the longest the worker can take to answer the command, failure cleanup
// included, provided the SDK honors its deadlines.
[[nodiscard]] constexpr milliseconds WorkerReplyWorst(const WorkerOperationBudget& budget) noexcept {
    return budget.sdk + budget.cleanup + kAbortGrace * budget.abort_graces;
}

// D: how long the parent waits for the reply to the command.
[[nodiscard]] constexpr milliseconds ParentExchangeDeadline(const WorkerOperationBudget& budget) noexcept {
    return WorkerReplyWorst(budget) + kIpcMargin;
}

// S: startup allowance. Added to D only for the parent's first exchange with
// each worker, whatever the command: the worker may still be starting (process
// start, bootstrap read, delegation check, transport construction, pipe
// creation) when that command is sent. Same 9 s as the slack kept in
// kDefaultServingLifetime; see the note above the static_asserts at the end.
inline constexpr milliseconds kWorkerStartupAllowance{9000};

// D for one exchange: S is added when it is the parent's first exchange with
// that worker.
[[nodiscard]] constexpr milliseconds ParentExchangeDeadline(const WorkerOperationBudget& budget,
                                                            bool first_exchange) noexcept {
    return ParentExchangeDeadline(budget) + (first_exchange ? kWorkerStartupAllowance : milliseconds{0});
}

[[nodiscard]] constexpr std::optional<milliseconds> WorkerReplyWorst(std::string_view operation) noexcept {
    const auto* budget = FindWorkerOperationBudget(kWorkerOperationBudgets, operation);
    if (!budget) return std::nullopt;
    return WorkerReplyWorst(*budget);
}

[[nodiscard]] constexpr std::optional<milliseconds> ParentExchangeDeadline(std::string_view operation) noexcept {
    const auto* budget = FindWorkerOperationBudget(kWorkerOperationBudgets, operation);
    if (!budget) return std::nullopt;
    return ParentExchangeDeadline(*budget);
}

// --- Close step (PreviewWorkerOwner::Close) ---

// E_ok: exit window after a validated receipt with safeToExit=true whose ACK
// the parent wrote. The worker has finished its SDK shutdown before replying,
// so only process teardown remains. Same 5000 ms as the earlier success path.
inline constexpr milliseconds kExitWindowAfterSafeReceipt{5000};

// E_fail: exit window when the parent holds no validated receipt (no or
// invalid reply, ACK unconfirmed, or close not sent after an unconfirmed
// command). It is sized for a worker that is already shutting down: the close
// command's worst time (StopLiveView 10 s + Close 10 s + 2G = 22 s), one more
// set of reply stages (3 s) and M (5 s): 30 s with the default table. The
// extra reply stages are separate from those inside M: when close was not
// delivered, the worker may first finish the previous command's ACK stage
// (1 s) or reply stages before its own cleanup starts. A worker that has not
// learned of any failure keeps accepting commands until its serving lifetime
// ends, so it can still be running when this window ends.
[[nodiscard]] constexpr milliseconds ExitWindowWithoutReceipt(const WorkerOperationBudgetTable& table) noexcept {
    const auto* close = FindWorkerOperationBudget(table, "close");
    return (close ? WorkerReplyWorst(*close) : milliseconds{0}) + kWorkerReplyStages + kIpcMargin;
}

// --- Worker lifetimes carried by the bootstrap ---

// Upper bound of the trial: from owner construction to the end of the close
// step, and for the worker's serving lifetime. Same value as the capture
// transaction watchdog.
inline constexpr milliseconds kPreviewSessionLimit = Timeouts{}.transaction_watchdog;

// Operation deadline ("lifetimeMs"): after it the worker rejects every command
// except close. preview_commissioning_main.cpp uses the same 60 s.
inline constexpr milliseconds kDefaultOperationDeadline{60000};

// Serving lifetime ("servingMs"): until it the worker still accepts close;
// at it the worker closes itself. 170 s = the 161 s the default table needs
// (see ServingLifetimeFits) plus S (kWorkerStartupAllowance, 9 s) for process
// start and bootstrap delivery, and stays under kPreviewSessionLimit.
inline constexpr milliseconds kDefaultServingLifetime{170000};

// Test-only time scale for budget-derived parent waits. Production code paths
// never apply it (PreviewWorkerOwner compiles it out of SDK builds).
struct TimeScale {
    std::uint32_t numerator{1};
    std::uint32_t denominator{1};
    [[nodiscard]] constexpr bool Valid() const noexcept {
        return numerator != 0 && denominator != 0 && numerator <= denominator;
    }
    [[nodiscard]] constexpr milliseconds Apply(milliseconds value) const noexcept {
        return milliseconds{value.count() * numerator / denominator};
    }
};

// The longest parent wait for one non-close command: the last command may be
// sent right before the operation deadline.
[[nodiscard]] constexpr milliseconds LongestCommandExchange(const WorkerOperationBudgetTable& table) noexcept {
    milliseconds longest{0};
    for (const auto& entry : table)
        if (entry.operation != "close" && ParentExchangeDeadline(entry) > longest) longest = ParentExchangeDeadline(entry);
    return longest;
}

// The close sends of the close step: one close per worker, one after the
// other. The exit windows that follow are parent-only waits and need no
// serving worker, so they are not part of this sum; the parent bounds them
// with kPreviewSessionLimit instead.
[[nodiscard]] constexpr milliseconds CloseSendWorst(const WorkerOperationBudgetTable& table) noexcept {
    const auto* close = FindWorkerOperationBudget(table, "close");
    return close ? 2 * ParentExchangeDeadline(*close) : milliseconds{0};
}

// operation deadline + longest command exchange + close sends <= serving
// lifetime <= kPreviewSessionLimit, with 0 < operation deadline < serving
// lifetime. The parent checks this before arming the delegation marker.
[[nodiscard]] constexpr bool ServingLifetimeFits(milliseconds operation_deadline, milliseconds serving_lifetime,
                                                const WorkerOperationBudgetTable& table,
                                                TimeScale scale = {}) noexcept {
    return scale.Valid() && CoversEveryWorkerOperation(table) && operation_deadline > milliseconds{0} &&
        operation_deadline < serving_lifetime && serving_lifetime <= scale.Apply(kPreviewSessionLimit) &&
        operation_deadline + scale.Apply(LongestCommandExchange(table)) + scale.Apply(CloseSendWorst(table)) <=
            serving_lifetime;
}

static_assert(CoversEveryWorkerOperation(kWorkerOperationBudgets));
static_assert(kPreviewSessionLimit == seconds{180});
static_assert(ServingLifetimeFits(kDefaultOperationDeadline, kDefaultServingLifetime, kWorkerOperationBudgets));

// S needs no term in ServingLifetimeFits. PreviewCommissioning sends enumerate
// first to each worker, and that first D plus S stays within the longest
// command exchange even when it is sent right before the operation deadline
// (27 s + 9 s = 36 s <= 47 s). A worker whose first exchange is close received
// no command, so only the other worker can have a command running at the
// operation deadline; that close's extra S fits in the slack of the default
// serving lifetime (60 s + 47 s + 27 s + 36 s = 170 s).
static_assert(ParentExchangeDeadline(*FindWorkerOperationBudget(kWorkerOperationBudgets, "enumerate"), true) <=
              LongestCommandExchange(kWorkerOperationBudgets));
static_assert(kDefaultOperationDeadline + LongestCommandExchange(kWorkerOperationBudgets) +
                  CloseSendWorst(kWorkerOperationBudgets) + kWorkerStartupAllowance <=
              kDefaultServingLifetime);

} // namespace a0::phase0::experimental::preview_worker_timing
