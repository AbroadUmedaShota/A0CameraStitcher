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
        std::string_view{"sdk-status"},
        std::string_view{"wpd-status"},
        std::string_view{"spool-status"},
        std::string_view{"wpd-correlation-status"},
        std::string_view{"live-view"},
        std::string_view{"live-view-handoff"},
        std::string_view{"hybrid-capture-single"},
        std::string_view{"hybrid-fault-single"},
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
               "the approved hardware path is hybrid-capture-single";
    }
    if (operator_gate_requested) {
        return "operator gate is not valid for fake-only direct capture commands";
    }
    return std::nullopt;
}

bool RequiresHardwareProcessLease(
    std::string_view command,
    std::string_view transport) noexcept {
    return transport != "fake" && IsRealHardwareCommand(command);
}

} // namespace a0::phase0
