#pragma once

#include "a0/phase0/dual_binding_camera_agent.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace a0::phase0 {

class DualHardwarePairJournalStore;

inline constexpr std::string_view kDefaultDualHardwareCameraAgentPipeName =
    "A0CameraStitcher.CameraAgent.HardwareDual.v2";

struct DualHardwareFakeCaptureOutcome {
    bool succeeded{};
    bool exact_recovered_object_deleted{true};
    bool spool_empty_after_delete{true};
};

enum class DualHardwarePairPreflightState { Ready, HardwarePending, BindingInvalidated, CleanupUnconfirmed };
struct DualHardwarePairPreflightOutcome {
    DualHardwarePairPreflightState state{DualHardwarePairPreflightState::HardwarePending};
    DualIdentityInvalidationReason binding_invalidation_reason{DualIdentityInvalidationReason::None};
    bool requires_rebinding{};
    bool host_terminal_after_reservation_close{};
};
struct DualHardwarePairCaptureCapabilities {
    bool ordinary_pair_capture_available{};
    bool capture_recovery_only_available{};
};

class DualHardwarePairCaptureBackend {
public:
    virtual ~DualHardwarePairCaptureBackend() = default;
    // Runs once for the complete pair before the durable journal crosses into
    // Dispatching. A false result is confirmed-undispatched: no shutter was
    // sent and the exact reservation remains eligible for explicit closure.
    [[nodiscard]] virtual DualHardwarePairPreflightOutcome PreflightPair(
        bool capture_recovery_only,
        std::int64_t watchdog_deadline_100ns) = 0;
    [[nodiscard]] virtual DualHardwarePairCaptureCapabilities Capabilities() const noexcept { return {true, true}; }
    [[nodiscard]] virtual DualIdentityInvalidationReason LastBindingInvalidationReason() const noexcept {
        return DualIdentityInvalidationReason::None;
    }
    [[nodiscard]] virtual DualHardwareFakeCaptureOutcome Capture(
        std::string_view alias,
        const std::filesystem::path& canonical_original_path,
        std::int64_t watchdog_deadline_100ns) = 0;
};

// Compatibility name retained for the existing deterministic contract-test
// backends. Production hosts inject DualHardwarePairCaptureBackend directly.
using DualHardwareFakePairCaptureBackend = DualHardwarePairCaptureBackend;

inline constexpr std::string_view kDualHardwareCameraAgentSchemaVersion =
    "a0.camera-agent.hardware-dual.v2";
inline constexpr std::string_view
    kDualHardwareCameraAgentCaptureRecoveryOnlySchemaVersion =
        "a0.camera-agent.hardware-dual-capture-recovery-only.v1";
inline constexpr std::string_view kDualHardwareCameraAgentMarker = "Hardware";

enum class DualHardwareCameraAgentOperation {
    get_dual_capabilities,
    reserve_pair_transaction,
    start_reserved_pair,
    start_reserved_capture_recovery_only,
    get_pair_transaction_result,
    close_reserved_pair_transaction,
};

class DualHardwareCameraAgentProtocolError final : public std::runtime_error {
public:
    DualHardwareCameraAgentProtocolError(std::string code, std::string message);

    [[nodiscard]] const std::string& Code() const noexcept;

private:
    std::string code_;
};

struct DualHardwareCameraAgentRequest {
    std::string request_id;
    DualHardwareCameraAgentOperation operation{
        DualHardwareCameraAgentOperation::get_dual_capabilities};
    std::string transaction_id;
    bool capture_recovery_only_protocol{};
    std::int64_t identity_observed_at_100ns{};
    std::int64_t identity_expires_at_100ns{};
    std::int64_t capture_profile_valid_until_100ns{};
    std::int64_t rig_profile_valid_until_100ns{};
    std::int64_t started_at_100ns{};
    std::int64_t watchdog_deadline_100ns{};
};

using DualHardwareUtcClock =
    std::function<std::chrono::system_clock::time_point()>;

struct DualHardwareCameraAgentSafetyCounters {
    std::size_t camera_access_count{};
    std::size_t pair_dispatch_count{};
    std::size_t automatic_retry_count{};
};

[[nodiscard]] DualHardwareCameraAgentRequest ParseDualHardwareCameraAgentRequest(
    std::string_view json);

// AR-08a-2B optionally connects the pure protocol boundary to one externally
// owned pair journal store. Camera dispatch remains unavailable and fail closed.
class DualHardwareCameraAgentDispatcher final {
public:
    DualHardwareCameraAgentDispatcher() noexcept = default;
    explicit DualHardwareCameraAgentDispatcher(
        std::shared_ptr<DualHardwarePairJournalStore> pair_store) noexcept;
    DualHardwareCameraAgentDispatcher(
        std::shared_ptr<DualHardwarePairJournalStore> pair_store,
        DualHardwareUtcClock utc_clock);
    DualHardwareCameraAgentDispatcher(
        std::shared_ptr<DualHardwarePairJournalStore> pair_store,
        DualHardwareUtcClock utc_clock,
        std::shared_ptr<DualHardwarePairCaptureBackend> capture_backend);

    [[nodiscard]] std::string Handle(std::string_view request_json) noexcept;

    [[nodiscard]] DualHardwareCameraAgentSafetyCounters SafetyCounters()
        const noexcept;

    // Named-pipe host lifecycle hooks (AR-08a-2B host integration). OnIdle is
    // a no-op because the Dual protocol has no idle-driven backend state.
    // ShouldStop becomes true only after a response has terminalized an
    // invalid binding, or after the exact reserved transaction blocked by a
    // fatal preflight has been closed before dispatch. Otherwise the host is
    // bounded by RunDualHardwareCameraAgentNamedPipeServer's fixed lifetime.
    void OnIdle() noexcept;
    [[nodiscard]] bool ShouldStop() const noexcept;

private:
    // Shared ownership keeps the injected store alive for every Handle call.
    // A null store preserves the fail-closed PairStoreUnavailable behavior.
    std::shared_ptr<DualHardwarePairJournalStore> pair_store_;
    DualHardwareUtcClock utc_clock_;
    std::shared_ptr<DualHardwarePairCaptureBackend> capture_backend_;
    DualHardwareCameraAgentSafetyCounters safety_counters_;
    bool should_stop_{};
    std::string terminal_pending_transaction_id_;
};

// Failure-injection seam for named-pipe host contract tests only. Production
// launchers always leave this at its default (no injected failure).
struct DualHardwareCameraAgentPipeFailureInjectionForTesting {
    bool fail_response_header_write{};
    bool fail_response_body_write{};
    bool fail_delivery_ack_wait{};
    bool fail_response_flush{};
    // Controls only the absolute host-lifetime clock in contract tests.
    // Frame/ACK timeouts keep their real monotonic clock. Empty in production.
    std::function<std::uint64_t()> lifetime_ticks_for_testing;
};

// Serves the Dual hardware v2 protocol over one dedicated local named pipe,
// reusing the exact same framing, current-logon access boundary, and
// bounded delivery-ACK semantics as RunHardwareCameraAgentNamedPipeServer (see
// hardware_camera_agent.hpp): 4-byte little-endian length prefix + UTF-8 JSON
// body, capped at kMaximumPipeFrameBytes (1 MiB), followed by a bounded
// one-byte delivery acknowledgment from the client. A missing, invalid, or
// late acknowledgment is a delivery failure, so a client can always
// distinguish "never dispatched" (exit kFailedBeforeDispatchExitCode) from
// "dispatched but delivery failed" (exit kDispatchedDeliveryFailureExitCode)
// without ever triggering a redispatch on the server side.
//
// Lifetime policy: identical to the Single-camera host -- a fixed absolute
// deadline (600s by default) computed once at process start and never
// extended, regardless of how much or how little the pipe is used
// (Orchestrator decision, 2026-08-17; see
// docs/HARDWARE_CAMERA_AGENT_DUAL_V2.md). The Dual host is still designed
// for persistent multi-request use within that window (capabilities /
// reserve / start / same-ID query issued as separate pipe connections over
// one long-running process); it just does not extend its own lifetime in
// response to that use. A rolling/idle-extended deadline was considered and
// rejected: it would let a steady trickle of requests, including rejected
// ones, keep the process alive indefinitely, which conflicts with the
// documented maximum-lifetime contract. Launching a Dual host with a fresh,
// unique pipe name for each logical session is the launcher's
// responsibility, not this function's. serve_once mode (used by tests and
// one-shot invocations) is unaffected: it always terminates after its
// single connection.
[[nodiscard]] int RunDualHardwareCameraAgentNamedPipeServer(
    std::string_view pipe_name,
    DualHardwareCameraAgentDispatcher& dispatcher,
    bool serve_once = false,
    DualHardwareCameraAgentPipeFailureInjectionForTesting failure_injection = {},
    std::optional<std::chrono::milliseconds> lifetime_budget_for_testing =
        std::nullopt);

} // namespace a0::phase0
