#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace a0::phase0 {

// The legacy direct capture commands remain useful for deterministic fake
// contract tests, but no longer represent an approved hardware path.
[[nodiscard]] std::optional<std::string> ValidateDirectCaptureSafety(
    std::string_view command,
    std::string_view transport,
    bool operator_gate_requested);

// Returns true only for commands that can open a real SDK or WPD session.
[[nodiscard]] bool RequiresHardwareProcessLease(
    std::string_view command,
    std::string_view transport) noexcept;

} // namespace a0::phase0
