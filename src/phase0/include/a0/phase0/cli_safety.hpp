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

// sdk-status must route through the SDK-only read/enumerate executor. It must
// never be accepted with a WPD or fake transport.
[[nodiscard]] std::optional<std::string> ValidateSdkStatusCliRouting(
    std::string_view command,
    std::string_view transport);

enum class SdkStatusIdentityRoute {
    single_identity_v3,
    legacy_identity_v2,
};

// CAM-A defaults to the SingleCamera identity-v3 authority. An explicitly
// supplied legacy camera map selects the preserved Dual/legacy v2 route.
[[nodiscard]] SdkStatusIdentityRoute SelectSdkStatusIdentityRoute(
    std::string_view camera_alias,
    bool legacy_camera_map_explicit) noexcept;

// Returns true only for commands that can open a real SDK or WPD session.
[[nodiscard]] bool RequiresHardwareProcessLease(
    std::string_view command,
    std::string_view transport) noexcept;

[[nodiscard]] std::optional<std::string> ValidateIdentityBindingArguments(
    std::string_view command,
    std::string_view transport,
    bool single_camera_connected_confirmed,
    bool transport_explicit = true);

} // namespace a0::phase0
