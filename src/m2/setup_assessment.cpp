#include "a0/m2/setup_assessment.hpp"

#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace a0::m2 {
namespace {

constexpr double kMaximumPercent = 100.0;

void ValidateNonNegativeFinite(const double value, const std::string_view name) {
    if (!std::isfinite(value) || value < 0.0) {
        throw std::invalid_argument(std::string(name) + " must be finite and non-negative");
    }
}

void ValidateMetric(const CorrectionMetric& metric, const std::string_view name) {
    ValidateNonNegativeFinite(metric.measured, std::string(name) + " measured");
    ValidateNonNegativeFinite(metric.target_max, std::string(name) + " target maximum");
    ValidateNonNegativeFinite(metric.auto_correction_max, std::string(name) + " automatic correction maximum");
    if (metric.target_max > metric.auto_correction_max) {
        throw std::invalid_argument(std::string(name) + " target maximum exceeds automatic correction maximum");
    }
}

void AppendMetricDecision(
    const CorrectionMetric& metric,
    const std::string_view name,
    std::vector<std::string>& hard_blockers,
    std::vector<std::string>& corrections) {
    if (metric.measured > metric.auto_correction_max) {
        hard_blockers.emplace_back(std::string(name) + " exceeds automatic correction maximum");
    } else if (metric.measured > metric.target_max) {
        corrections.emplace_back(std::string(name) + " exceeds target maximum; transient correction is required");
    }
}

} // namespace

SetupAssessmentResult AssessSetup(const SetupAssessmentInput& input) {
    ValidateNonNegativeFinite(input.measured_overlap_percent, "measured overlap percent");
    ValidateNonNegativeFinite(input.minimum_overlap_percent, "minimum overlap percent");
    if (input.measured_overlap_percent > kMaximumPercent || input.minimum_overlap_percent > kMaximumPercent) {
        throw std::invalid_argument("overlap percent must be within 0 through 100");
    }

    ValidateMetric(input.registration_pixels, "registration pixels");
    ValidateMetric(input.rotation_degrees, "rotation degrees");
    ValidateMetric(input.scale_difference_percent, "scale difference percent");
    ValidateMetric(input.exposure_difference_ev, "exposure difference EV");
    ValidateMetric(input.color_delta_e, "color DeltaE");

    std::vector<std::string> hard_blockers;
    if (input.profile.status != ProfileStatus::approved) {
        hard_blockers.emplace_back("profile status is not approved");
    }
    if (input.profile.schema_version != kSupportedProfileSchemaVersion) {
        hard_blockers.emplace_back("profile schema version is unsupported");
    }
    if (input.profile.provenance.find_first_not_of(" \t\r\n") == std::string::npos) {
        hard_blockers.emplace_back("profile provenance is required");
    }
    if (input.profile.valid_until <= input.profile.measured_at) {
        hard_blockers.emplace_back("profile valid-until must be after measured-at");
    }
    if (input.profile.assessed_at < input.profile.measured_at) {
        hard_blockers.emplace_back("profile assessed-at must not precede measured-at");
    }
    if (input.profile.valid_until <= input.profile.assessed_at) {
        hard_blockers.emplace_back("profile is expired at assessed-at");
    }
    if (!input.full_coverage) {
        hard_blockers.emplace_back("full coverage is required");
    }
    if (!input.camera_settings_compatible) {
        hard_blockers.emplace_back("camera settings are incompatible");
    }
    if (input.measured_overlap_percent < input.minimum_overlap_percent) {
        hard_blockers.emplace_back("measured overlap is below the minimum");
    }

    std::vector<std::string> corrections;
    AppendMetricDecision(input.registration_pixels, "registration pixels", hard_blockers, corrections);
    AppendMetricDecision(input.rotation_degrees, "rotation degrees", hard_blockers, corrections);
    AppendMetricDecision(input.scale_difference_percent, "scale difference percent", hard_blockers, corrections);
    AppendMetricDecision(input.exposure_difference_ev, "exposure difference EV", hard_blockers, corrections);
    AppendMetricDecision(input.color_delta_e, "color DeltaE", hard_blockers, corrections);

    if (!hard_blockers.empty()) {
        return {SetupReadiness::physical_adjustment_required, false, std::move(hard_blockers)};
    }
    if (!corrections.empty()) {
        return {SetupReadiness::ready_auto_correction, true, std::move(corrections)};
    }
    return {SetupReadiness::ready, false, {}};
}

} // namespace a0::m2
