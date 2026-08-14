#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace a0::phase0 {

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
};

struct DualHardwareCameraAgentSafetyCounters {
    std::size_t camera_access_count{};
    std::size_t pair_dispatch_count{};
    std::size_t automatic_retry_count{};
};

[[nodiscard]] DualHardwareCameraAgentRequest ParseDualHardwareCameraAgentRequest(
    std::string_view json);

// AR-08a-1 intentionally owns only the pure software protocol boundary.
// Pair persistence and camera dispatch are introduced behind this dispatcher
// in a later task; until then every non-capability operation fails closed.
class DualHardwareCameraAgentDispatcher final {
public:
    [[nodiscard]] std::string Handle(std::string_view request_json) noexcept;

    [[nodiscard]] DualHardwareCameraAgentSafetyCounters SafetyCounters()
        const noexcept;
};

} // namespace a0::phase0
