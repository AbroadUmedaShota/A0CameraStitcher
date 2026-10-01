#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "a0/phase0/preview_commissioning.hpp"
#include "a0/phase0/preview_worker_reply.hpp"
#include "a0/phase0/preview_worker_timing.hpp"

namespace a0::phase0::experimental {
struct PreviewWorkerFailureObservation {
    std::size_t worker_index{};
    std::string operation;
    std::string category; // Bounded worker code, never an SDK exception message.
    // The whole request (length and body) was written to the worker's pipe.
    // false: the exchange ended before that (budget spent, worker already
    // ended, pipe unavailable or not the registered worker, or a write failed).
    bool request_written{};
    bool response_received{};
    bool response_validated{};
    bool ack_write_completed{}; // Local write only; not proof of server ACK processing.
    std::optional<PreviewWorkerReplyStatus> response_status;
    std::optional<PreviewWorkerCloseReceipt> reported_close;
};

// One point-in-time look at a child OS process through WaitForSingleObject and
// GetExitCodeProcess only. Deliberately separate from any SDK/IPC claim.
struct PreviewWorkerProcessCheck {
    bool wait_failed{};     // WaitForSingleObject returned neither WAIT_OBJECT_0 nor WAIT_TIMEOUT.
    bool exited{};          // WaitForSingleObject returned WAIT_OBJECT_0.
    bool code_available{};  // GetExitCodeProcess succeeded while exited.
    std::uint32_t exit_code{};
};

// The exit window Close() applied to one child, chosen from the last reply the
// parent validated for it. Every window starts when that child's close attempt
// ended (its close exchange returned, or Close() decided not to send close),
// and Close() observes both children after both close attempts.
enum class PreviewWorkerExitCheckKind {
    // Close() has not run the close step for this child yet (or was called off
    // the owner thread). There is no OS observation.
    not_checked,
    // A validated `closed` receipt (safeToExit=true) whose ACK the parent wrote:
    // wait up to E_ok (kExitWindowAfterSafeReceipt, 5000 ms).
    waited_after_close_ack,
    // The last validated receipt said safeToExit=false (a `quarantined` close
    // reply, or a `failed` reply with an unsafe receipt). Such a worker is held
    // for human recovery and is not expected to exit: one 0 ms look.
    instant_at_close_failure,
    // The last validated reply was not a `closed` close reply, but it carried
    // a receipt with safeToExit=true and the parent wrote its ACK: a `failed`
    // reply to an earlier command (close is then not sent), or a `failed`
    // reply to close itself. The worker has already shut down its SDK. Wait
    // up to E_ok.
    waited_after_failure_receipt,
    // No validated receipt: no or invalid response, ACK not written, or close
    // not sent after an unconfirmed command. The worker may still be running
    // its own cleanup: wait up to E_fail (ExitWindowWithoutReceipt).
    waited_for_worker_cleanup,
};

// The close exchange with one child, as the parent saw it.
struct PreviewWorkerCloseReply {
    // false: close was never attempted, because an earlier command left the
    // child terminal.
    bool attempted{};
    // The close request was written in full (request_written of the close
    // exchange). attempted && !sent: Close() tried, but nothing or only part
    // of the request left the parent.
    bool sent{};
    bool response_received{};
    bool response_validated{};
    bool ack_write_completed{}; // Local write only, as in PreviewWorkerFailureObservation.
    std::optional<PreviewWorkerReplyStatus> status;
    std::optional<PreviewWorkerCloseReceipt> receipt; // Worker-reported; diagnostic only.
};

// What the parent could independently confirm about one child OS process
// during Close().
struct PreviewWorkerExitObservation {
    PreviewWorkerExitCheckKind kind{PreviewWorkerExitCheckKind::not_checked};
    // The observation for the child's exit window: exited, or still running
    // when the look ended. Normally a wait that lasts until the window end.
    // When looked_after_window_end is set it is a 0 ms look taken after that
    // end, and an exit seen there may have happened after the window.
    PreviewWorkerProcessCheck at_close;
    // The window end was the owner's session end (construction +
    // kPreviewSessionLimit), earlier than the close attempt end plus the
    // chosen window.
    bool window_capped_by_session_limit{};
    // Observation of a waiting window (not the 0 ms quarantined look) started
    // at or after the window end, typically because the other child's close
    // exchange ran past it. Diagnostic only: the verdict does not depend on
    // when the exit was seen.
    bool looked_after_window_end{};
    // Present only when at_close did not see the process exit: a 0 ms look
    // taken by Close() after the exit windows of both children have ended.
    std::optional<PreviewWorkerProcessCheck> after_both_closes;
    // Latest 0 ms look taken by a repeated Close() call (which never sends
    // IPC), for a child no earlier look saw exit. repeated_close_looks counts
    // those looks, so a journal can tell a new look from one already written.
    std::optional<PreviewWorkerProcessCheck> at_repeated_close;
    std::uint32_t repeated_close_looks{};
    // Present once Close() has reached this child.
    std::optional<PreviewWorkerCloseReply> close_reply;
};

// Fixed/bounded diagnostic fields only. Never include SDK free-form errors.
[[nodiscard]] std::wstring FormatPreviewWorkerFailure(const PreviewWorkerFailureObservation& failure);

class PreviewWorkerStartupError final : public std::runtime_error {
public:
    explicit PreviewWorkerStartupError(bool workers_may_exist)
        : std::runtime_error("preview worker startup failed"), workers_may_exist_(workers_may_exist) {}
    bool WorkersMayExist() const noexcept { return workers_may_exist_; }
private:
    bool workers_may_exist_;
};

// Test-root timing (SDK-stub builds only; an SDK build rejects the test-root
// constructors and does not compile the scale or table override in).
struct PreviewWorkerTestTiming {
    std::chrono::milliseconds operation_deadline{}; // Bootstrap lifetimeMs.
    std::chrono::milliseconds serving_lifetime{};   // Bootstrap servingMs.
    // Applied to every budget-derived parent wait: exchange deadlines, exit
    // windows and kPreviewSessionLimit. The bootstrap values above are not scaled.
    preview_worker_timing::TimeScale scale{};
    // Replaces kWorkerOperationBudgets for this owner.
    std::optional<preview_worker_timing::WorkerOperationBudgetTable> budget_table;
};

// Internal experimental integration; hardware acceptance remains gated.
// Thread-affine: create, close and destroy on the lease-owning thread.
// Destruction does not close workers or clear an armed quarantine marker.
class PreviewWorkerOwner final {
public:
    // Operation deadline kDefaultOperationDeadline, serving lifetime
    // kDefaultServingLifetime.
    PreviewWorkerOwner();
    // Test root. `lifetime` is the operation deadline; the serving lifetime is
    // kPreviewSessionLimit and the budget is unscaled.
    PreviewWorkerOwner(std::string_view test_lease_name, const std::filesystem::path& test_marker_root,
                       const std::filesystem::path& worker_executable, std::chrono::milliseconds lifetime,
                       std::function<void(std::array<std::uint32_t, 2>)> after_spawn_for_testing = {},
                       bool inject_ack_write_failure_for_testing = false);
    // Test root with explicit lifetimes, time scale and budget table.
    PreviewWorkerOwner(std::string_view test_lease_name, const std::filesystem::path& test_marker_root,
                       const std::filesystem::path& worker_executable, const PreviewWorkerTestTiming& timing,
                       std::function<void(std::array<std::uint32_t, 2>)> after_spawn_for_testing = {});
    ~PreviewWorkerOwner();
    PreviewWorkerOwner(const PreviewWorkerOwner&) = delete;
    PreviewWorkerOwner& operator=(const PreviewWorkerOwner&) = delete;
    // First call: sends close at most once to each child (one after the other),
    // then observes both exits in their windows. Success needs a validated
    // `closed` receipt, exit code 0 inside the window, and a confirmed disarm
    // for both children. Later calls return the cached result, never send IPC,
    // and only take a 0 ms look at children not yet seen exiting.
    bool Close() noexcept;
    [[nodiscard]] std::optional<PreviewWorkerFailureObservation> FirstFailure() const;
    // Per-child OS exit evidence gathered by Close() (kind not_checked before
    // that). Available regardless of whether Close() returned true, so a
    // failure path can still persist both workers' exit evidence.
    [[nodiscard]] std::array<PreviewWorkerExitObservation, 2> ExitObservations() const noexcept;
    std::array<std::uint32_t, 2> ProcessIds() const noexcept;
    std::array<std::string, 2> Enumerate(std::size_t worker);
    std::vector<unsigned char> Preview(std::size_t worker, std::string_view candidate);
    void ConfirmAndSuspend(std::size_t worker, ObservedPreviewBody body);
    void StartBoth();
    std::vector<unsigned char> Read(ObservedPreviewBody body);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace a0::phase0::experimental
