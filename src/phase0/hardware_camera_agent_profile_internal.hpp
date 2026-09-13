#pragma once

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace a0::phase0 {
struct SdkCameraStatus;

namespace detail {

struct SettingExpectation {
    std::optional<bool> available;
    std::optional<std::string> cap_type;
    std::optional<std::string> probe_state;
    std::optional<std::string> value_type;
    bool current_value_specified{};
    std::optional<std::uint32_t> current_value;
    bool current_index_specified{};
    std::optional<std::uint32_t> current_index;
    bool current_label_specified{};
    std::optional<std::string> current_label;
};

struct ApprovedCaptureProfile {
    std::string profile_id;
    std::uint32_t profile_version{};
    std::string selected_alias;
    std::string sha256;
    std::string expires_at_utc;
    FILETIME expires_at{};
    std::map<std::string, SettingExpectation> expected_settings;
};

// Private production boundary shared with contract tests, not a public API.
[[nodiscard]] ApprovedCaptureProfile ParseApprovedCaptureProfile(std::string_view json);
void ValidateApprovedCaptureProfileForShutterSession(
    const ApprovedCaptureProfile& profile,
    const SdkCameraStatus& shutter_status,
    const std::function<void()>& record_evidence);

} // namespace detail
} // namespace a0::phase0
