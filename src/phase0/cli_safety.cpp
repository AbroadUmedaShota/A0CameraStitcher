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

std::optional<std::string> ValidateIdentityBindingArguments(
    std::string_view command,
    std::string_view transport,
    bool single_camera_connected_confirmed,
    bool transport_explicit) {
    const bool single_transport = command == "bind-identity";
    const bool cross_transport = command == "bind-cross-transport-identity";
    if (!single_transport && !cross_transport) {
        if (single_camera_connected_confirmed) {
            return "single-camera-connected-confirmed is valid only for identity binding commands";
        }
        return std::nullopt;
    }
    if (cross_transport) {
        if (transport_explicit) {
            return "bind-cross-transport-identity selects SDK and WPD internally and does not accept --transport";
        }
        if (!single_camera_connected_confirmed) {
            return "bind-cross-transport-identity requires --single-camera-connected-confirmed after all other D810 bodies are disconnected";
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
