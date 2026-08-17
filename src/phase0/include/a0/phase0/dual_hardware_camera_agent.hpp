#pragma once

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

class DualHardwareFakePairCaptureBackend {
public:
    virtual ~DualHardwareFakePairCaptureBackend() = default;
    [[nodiscard]] virtual DualHardwareFakeCaptureOutcome Capture(
        std::string_view alias,
        const std::filesystem::path& canonical_original_path,
        std::int64_t watchdog_deadline_100ns) = 0;
};

inline constexpr std::string_view kDualHardwareCameraAgentSchemaVersion =
    "a0.camera-agent.hardware-dual.v2";
inline constexpr std::string_view kDualHardwareCameraAgentMarker = "Hardware";

enum class DualHardwareCameraAgentOperation {
    get_dual_capabilities,
    reserve_pair_transaction,
    start_reserved_pair,
    get_pair_transaction_result,
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
        std::shared_ptr<DualHardwareFakePairCaptureBackend> fake_backend);

    [[nodiscard]] std::string Handle(std::string_view request_json) noexcept;

    [[nodiscard]] DualHardwareCameraAgentSafetyCounters SafetyCounters()
        const noexcept;

    // Named-pipe host lifecycle hooks (AR-08a-2B host integration). Neither
    // method changes protocol, parsing, or capture semantics: the Dual
    // protocol has no idle-driven backend state and no operation that asks
    // the host to stop, so OnIdle is a no-op and ShouldStop is always false.
    // The host process's own lifetime bound (see
    // RunDualHardwareCameraAgentNamedPipeServer) is what keeps a Dual host
    // from running forever.
    void OnIdle() noexcept;
    [[nodiscard]] bool ShouldStop() const noexcept;

private:
    // Shared ownership keeps the injected store alive for every Handle call.
    // A null store preserves the fail-closed PairStoreUnavailable behavior.
    std::shared_ptr<DualHardwarePairJournalStore> pair_store_;
    DualHardwareUtcClock utc_clock_;
    std::shared_ptr<DualHardwareFakePairCaptureBackend> fake_backend_;
    DualHardwareCameraAgentSafetyCounters safety_counters_;
};

// Failure-injection seam for named-pipe host contract tests only. Production
// launchers always leave this at its default (no injected failure).
struct DualHardwareCameraAgentPipeFailureInjectionForTesting {
    bool fail_response_header_write{};
    bool fail_response_body_write{};
    bool fail_response_flush{};
};

// Serves the Dual hardware v2 protocol over one dedicated local named pipe,
// reusing the exact same framing, current-logon access boundary, and
// teardown-drain semantics as RunHardwareCameraAgentNamedPipeServer (see
// hardware_camera_agent.hpp): 4-byte little-endian length prefix + UTF-8 JSON
// body, capped at kMaximumPipeFrameBytes (1 MiB), and a FlushFileBuffers
// drain before DisconnectNamedPipe whenever a response was already dispatched
// but delivery failed (Issue #17 teardown-drain fix), so a client can always
// distinguish "never dispatched" (exit kFailedBeforeDispatchExitCode) from
// "dispatched but delivery failed" (exit kDispatchedDeliveryFailureExitCode)
// without ever triggering a redispatch on the server side.
//
// Lifetime policy: unlike the Single-camera host (fixed 600s from launch),
// the Dual host is designed for persistent multi-request use (capabilities /
// reserve / start / same-ID query issued as separate pipe connections over
// one long-running process). Serving each connection therefore extends a
// rolling 600s idle deadline instead of a single fixed one: the process
// exits after 600s with no *completed* request/response round trip, but
// otherwise stays available for as long as the operator keeps using it.
// serve_once mode (used by tests and one-shot invocations) is unaffected:
// it always terminates after its single connection.
[[nodiscard]] int RunDualHardwareCameraAgentNamedPipeServer(
    std::string_view pipe_name,
    DualHardwareCameraAgentDispatcher& dispatcher,
    bool serve_once = false,
    DualHardwareCameraAgentPipeFailureInjectionForTesting failure_injection = {},
    std::optional<std::chrono::milliseconds> lifetime_budget_for_testing =
        std::nullopt);

} // namespace a0::phase0
