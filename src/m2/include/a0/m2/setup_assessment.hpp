#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2 {

enum class SetupReadiness {
    ready,
    ready_auto_correction,
    physical_adjustment_required,
};

enum class ProfileStatus {
    draft,
    approved,
};

inline constexpr std::string_view kSupportedProfileSchemaVersion = "1.1.0";

struct ProfileTrust {
    ProfileStatus status;
    std::string schema_version;
    std::string provenance;
    std::chrono::sys_seconds measured_at;
    std::chrono::sys_seconds valid_until;
    std::chrono::sys_seconds assessed_at;
};

// Every value is a measured error or an approved upper bound; lower values are better.
struct CorrectionMetric {
    double measured;
    double target_max;
    double auto_correction_max;
};

struct SetupAssessmentInput {
    ProfileTrust profile;
    bool full_coverage;
    bool camera_settings_compatible;
    double measured_overlap_percent;
    double minimum_overlap_percent;
    CorrectionMetric registration_pixels;
    CorrectionMetric rotation_degrees;
    CorrectionMetric scale_difference_percent;
    CorrectionMetric exposure_difference_ev;
    CorrectionMetric color_delta_e;
};

struct SetupAssessmentResult {
    SetupReadiness readiness;
    bool apply_transient_correction;
    std::vector<std::string> reasons;
};

// Evaluates an already measured fixed-rig setup. It neither changes input nor applies correction.
// Throws std::invalid_argument when measurements or limits are non-finite, negative, or inconsistent.
[[nodiscard]] SetupAssessmentResult AssessSetup(const SetupAssessmentInput& input);

} // namespace a0::m2
