#include "a0/phase0/cli_safety.hpp"

#include <array>

namespace a0::phase0 {
namespace {

bool IsDirectCaptureCommand(std::string_view command) noexcept {
    return command == "capture-single" || command == "capture-pair" || command == "stability";
}

bool IsRealHardwareCommand(std::string_view command) noexcept {
    constexpr std::array commands{
        std::string_view{"inventory"},
        std::string_view{"bind-identity"},
        std::string_view{"bind-cross-transport-identity"},
        std::string_view{"bind-single-identity-v3"},
        std::string_view{"verify-dual-identity"},
        std::string_view{"verify-dual-spools"},
        std::string_view{"sdk-status"},
        std::string_view{"wpd-status"},
        std::string_view{"spool-status"},
        std::string_view{"wpd-correlation-status"},
        std::string_view{"live-view"},
        std::string_view{"live-view-handoff"},
        std::string_view{"hybrid-capture-single"},
        std::string_view{"hybrid-capture-pair"},
        std::string_view{"hybrid-fault-single"},
        std::string_view{"hybrid-fault-pair"},
        std::string_view{"hybrid-interrupt-pair"},
        std::string_view{"pc-direct-capture-single"},
    };
    for (const auto candidate : commands) {
        if (candidate == command) return true;
    }
    return false;
}

} // namespace

std::optional<std::string> ValidateDirectCaptureSafety(
    std::string_view command,
    std::string_view transport,
    bool operator_gate_requested) {
    if (!IsDirectCaptureCommand(command)) return std::nullopt;
    if (transport != "fake") {
        return "legacy capture-single, capture-pair, and stability commands are fake-only; "
               "the approved hardware paths are hybrid-capture-single and hybrid-capture-pair";
    }
    if (operator_gate_requested) {
        return "operator gate is not valid for fake-only direct capture commands";
    }
    return std::nullopt;
}

std::optional<std::string> ValidateSdkStatusCliRouting(
    std::string_view command,
    std::string_view transport) {
    if (command != "sdk-status") return std::nullopt;
    if (transport != "sdk") {
        return "sdk-status must use the SDK-only read-status executor and cannot route to WPD or fake transport";
    }
    return std::nullopt;
}

SdkStatusIdentityRoute SelectSdkStatusIdentityRoute(
    std::string_view camera_alias,
    bool legacy_camera_map_explicit) noexcept {
    return camera_alias == "CAM-A" && !legacy_camera_map_explicit
        ? SdkStatusIdentityRoute::single_identity_v3
        : SdkStatusIdentityRoute::legacy_identity_v2;
}

std::optional<std::string> ValidateIdentityBindingArguments(
    std::string_view command,
    std::string_view transport,
    bool single_camera_connected_confirmed,
    bool transport_explicit) {
    const bool single_transport = command == "bind-identity";
    const bool cross_transport = command == "bind-cross-transport-identity";
    const bool single_v3 = command == "bind-single-identity-v3";
    if (command == "pc-direct-capture-single") return std::nullopt;
    if (!single_transport && !cross_transport && !single_v3) {
        if (single_camera_connected_confirmed) {
            return "single-camera-connected-confirmed is valid only for identity binding commands";
        }
        return std::nullopt;
    }
    if (cross_transport || single_v3) {
        if (transport_explicit) {
            return std::string(command) + " selects SDK and WPD internally and does not accept --transport";
        }
        if (!single_camera_connected_confirmed) {
            return std::string(command) + " requires --single-camera-connected-confirmed after all other D810 bodies are disconnected";
        }
        return std::nullopt;
    }
    if (!transport_explicit) {
        return "bind-identity requires an explicit --transport sdk or --transport wpd";
    }
    if (transport != "sdk" && transport != "wpd") {
        return "bind-identity supports only the real SDK and WPD transports";
    }
    if (!single_camera_connected_confirmed) {
        return "bind-identity requires --single-camera-connected-confirmed after all other D810 bodies are disconnected";
    }
    return std::nullopt;
}

std::optional<std::string> ValidatePcDirectCaptureArguments(
    std::string_view command,
    std::string_view transport,
    bool transport_explicit,
    int count,
    bool alias_explicit,
    bool single_camera_connected_confirmed,
    bool exclusive_camera_control_confirmed,
    bool pc_direct_save_confirmed,
    bool sdk_camera_map_explicit,
    bool wpd_camera_map_explicit,
    bool legacy_card_authority_requested,
    bool operator_gate_requested) {
    if (command != "pc-direct-capture-single") {
        if (pc_direct_save_confirmed || wpd_camera_map_explicit) {
            return "PC-direct flags are valid only for pc-direct-capture-single";
        }
        return std::nullopt;
    }
    if (transport_explicit) {
        return "pc-direct-capture-single selects SDK and read-only WPD internally and does not accept --transport";
    }
    if (transport != "sdk") {
        return "pc-direct-capture-single must route capture through the Nikon SDK";
    }
    if (count != 1) {
        return "pc-direct-capture-single permits exactly one capture attempt";
    }
    if (!alias_explicit) {
        return "pc-direct-capture-single requires explicit --alias CAM-A or CAM-B";
    }
    if (!single_camera_connected_confirmed) {
        return "pc-direct-capture-single requires --single-camera-connected-confirmed after the other D810 is disconnected";
    }
    if (!exclusive_camera_control_confirmed) {
        return "pc-direct-capture-single requires --exclusive-camera-control-confirmed";
    }
    if (!pc_direct_save_confirmed) {
        return "pc-direct-capture-single requires --pc-direct-save-confirmed";
    }
    if (!sdk_camera_map_explicit || !wpd_camera_map_explicit) {
        return "pc-direct-capture-single requires explicit --camera-map and --wpd-camera-map files";
    }
    if (legacy_card_authority_requested) {
        return "pc-direct-capture-single does not accept card-spool or delete authority flags";
    }
    if (operator_gate_requested) {
        return "pc-direct-capture-single does not accept an operator gate or fault scenario";
    }
    return std::nullopt;
}

bool RequiresHardwareProcessLease(
    std::string_view command,
    std::string_view transport) noexcept {
    // Recovery report generation can materialize a durable interrupted-pair
    // diagnosis. It must not race an active capture that is still appending
    // to the same event log, even though reporting opens no camera session.
    if (command == "report") return true;
    return transport != "fake" && IsRealHardwareCommand(command);
}

} // namespace a0::phase0
