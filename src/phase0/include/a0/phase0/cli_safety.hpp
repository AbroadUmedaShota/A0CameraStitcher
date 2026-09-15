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

// The experimental PC-direct route deliberately owns both transports in
// sequence: read-only WPD fingerprint, one SDK capture, SDK close/restore,
// then read-only WPD fingerprint. It accepts no legacy card-spool authority.
[[nodiscard]] std::optional<std::string> ValidatePcDirectCaptureArguments(
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
    bool operator_gate_requested);

} // namespace a0::phase0
