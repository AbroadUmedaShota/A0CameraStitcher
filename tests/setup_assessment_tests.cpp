#include "a0/m2/setup_assessment.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using a0::m2::AssessSetup;
using a0::m2::CorrectionMetric;
using a0::m2::ProfileStatus;
using a0::m2::ProfileTrust;
using a0::m2::SetupAssessmentInput;
using a0::m2::SetupReadiness;

int failures = 0;

void Check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Callable>
void CheckInvalidArgument(Callable&& callable, const std::string& message) {
    try {
        callable();
        Check(false, message);
    } catch (const std::invalid_argument&) {
    }
}

SetupAssessmentInput ApprovedInput() {
    const CorrectionMetric metric{1.0, 2.0, 3.0};
    const ProfileTrust profile{
        ProfileStatus::approved,
        "1.1.0",
        "test-fixture",
        std::chrono::sys_seconds{std::chrono::seconds{100}},
        std::chrono::sys_seconds{std::chrono::seconds{200}},
        std::chrono::sys_seconds{std::chrono::seconds{150}},
    };
    return {profile, true, true, 25.0, 20.0, metric, metric, metric, metric, metric};
}

bool SameMetric(const CorrectionMetric& left, const CorrectionMetric& right) {
    return left.measured == right.measured && left.target_max == right.target_max
        && left.auto_correction_max == right.auto_correction_max;
}

bool SameProfile(const ProfileTrust& left, const ProfileTrust& right) {
    return left.status == right.status && left.schema_version == right.schema_version
        && left.provenance == right.provenance && left.measured_at == right.measured_at
        && left.valid_until == right.valid_until && left.assessed_at == right.assessed_at;
}

bool SameInput(const SetupAssessmentInput& left, const SetupAssessmentInput& right) {
    return SameProfile(left.profile, right.profile) && left.full_coverage == right.full_coverage
        && left.camera_settings_compatible == right.camera_settings_compatible
        && left.measured_overlap_percent == right.measured_overlap_percent
        && left.minimum_overlap_percent == right.minimum_overlap_percent
        && SameMetric(left.registration_pixels, right.registration_pixels)
        && SameMetric(left.rotation_degrees, right.rotation_degrees)
        && SameMetric(left.scale_difference_percent, right.scale_difference_percent)
        && SameMetric(left.exposure_difference_ev, right.exposure_difference_ev)
        && SameMetric(left.color_delta_e, right.color_delta_e);
}

void TestReadinessLevels() {
    const auto ready = AssessSetup(ApprovedInput());
    Check(ready.readiness == SetupReadiness::ready && !ready.apply_transient_correction && ready.reasons.empty(),
        "within target limits must be ready without correction");

    auto correction_input = ApprovedInput();
    correction_input.registration_pixels.measured = 2.5;
    const auto correction = AssessSetup(correction_input);
    Check(correction.readiness == SetupReadiness::ready_auto_correction && correction.apply_transient_correction
            && correction.reasons.size() == 1,
        "a target exceedance within automatic correction must require transient correction");

    auto physical_input = ApprovedInput();
    physical_input.color_delta_e.measured = 3.1;
    const auto physical = AssessSetup(physical_input);
    Check(physical.readiness == SetupReadiness::physical_adjustment_required && !physical.apply_transient_correction
            && physical.reasons.size() == 1,
        "an automatic correction exceedance must require physical adjustment");
}

void TestHardBlockers() {
    auto input = ApprovedInput();
    input.profile.status = ProfileStatus::draft;
    Check(AssessSetup(input).readiness == SetupReadiness::physical_adjustment_required, "draft profile must block setup");

    input = ApprovedInput();
    input.full_coverage = false;
    Check(AssessSetup(input).readiness == SetupReadiness::physical_adjustment_required, "incomplete coverage must block setup");

    input = ApprovedInput();
    input.camera_settings_compatible = false;
    Check(AssessSetup(input).readiness == SetupReadiness::physical_adjustment_required, "incompatible settings must block setup");

    input = ApprovedInput();
    input.measured_overlap_percent = 19.9;
    Check(AssessSetup(input).readiness == SetupReadiness::physical_adjustment_required, "low overlap must block setup");

    for (CorrectionMetric SetupAssessmentInput::*metric : {
             &SetupAssessmentInput::registration_pixels,
             &SetupAssessmentInput::rotation_degrees,
             &SetupAssessmentInput::scale_difference_percent,
             &SetupAssessmentInput::exposure_difference_ev,
             &SetupAssessmentInput::color_delta_e,
         }) {
        input = ApprovedInput();
        (input.*metric).measured = 3.1;
        Check(AssessSetup(input).readiness == SetupReadiness::physical_adjustment_required,
            "each metric automatic correction exceedance must block setup");
    }
}

void TestProfileTrustFailures() {
    auto input = ApprovedInput();
    input.profile.status = ProfileStatus::draft;
    const auto draft = AssessSetup(input);
    Check(draft.readiness == SetupReadiness::physical_adjustment_required
            && draft.reasons.front() == "profile status is not approved",
        "draft profile status must fail closed");

    input = ApprovedInput();
    input.profile.schema_version = "1.0.0";
    const auto version = AssessSetup(input);
    Check(version.readiness == SetupReadiness::physical_adjustment_required
            && version.reasons.front() == "profile schema version is unsupported",
        "unsupported profile schema version must fail closed");

    input = ApprovedInput();
    input.profile.provenance.clear();
    const auto provenance = AssessSetup(input);
    Check(provenance.readiness == SetupReadiness::physical_adjustment_required
            && provenance.reasons.front() == "profile provenance is required",
        "missing profile provenance must fail closed");

    input = ApprovedInput();
    input.profile.provenance = " \t\r\n";
    const auto whitespace_provenance = AssessSetup(input);
    Check(whitespace_provenance.readiness == SetupReadiness::physical_adjustment_required
            && whitespace_provenance.reasons.front() == "profile provenance is required",
        "whitespace-only profile provenance must fail closed");

    input = ApprovedInput();
    input.profile.valid_until = input.profile.measured_at;
    const auto invalid_window = AssessSetup(input);
    Check(invalid_window.readiness == SetupReadiness::physical_adjustment_required
            && invalid_window.reasons.front() == "profile valid-until must be after measured-at",
        "valid-until equal to measured-at must fail closed");

    input = ApprovedInput();
    input.profile.assessed_at = input.profile.measured_at - std::chrono::seconds{1};
    const auto before_measurement = AssessSetup(input);
    Check(before_measurement.readiness == SetupReadiness::physical_adjustment_required
            && before_measurement.reasons.front() == "profile assessed-at must not precede measured-at",
        "assessed-at before measured-at must fail closed");

    input = ApprovedInput();
    input.profile.valid_until = input.profile.assessed_at;
    const auto expired = AssessSetup(input);
    Check(expired.readiness == SetupReadiness::physical_adjustment_required
            && expired.reasons.front() == "profile is expired at assessed-at",
        "valid-until equal to assessed-at must fail closed");
}

void TestBoundaries() {
    auto input = ApprovedInput();
    input.measured_overlap_percent = input.minimum_overlap_percent;
    input.registration_pixels.measured = input.registration_pixels.target_max;
    input.rotation_degrees.target_max = input.rotation_degrees.auto_correction_max;
    input.rotation_degrees.measured = input.rotation_degrees.auto_correction_max;
    const auto result = AssessSetup(input);
    Check(result.readiness == SetupReadiness::ready && !result.apply_transient_correction,
        "equal minimum, target, and automatic correction bounds must be accepted");
}

void TestInvalidInput() {
    const auto invalid = std::numeric_limits<double>::quiet_NaN();
    const auto infinity = std::numeric_limits<double>::infinity();
    auto input = ApprovedInput();
    input.registration_pixels.measured = invalid;
    CheckInvalidArgument([&] { (void)AssessSetup(input); }, "NaN metric must be rejected");

    input = ApprovedInput();
    input.exposure_difference_ev.target_max = infinity;
    CheckInvalidArgument([&] { (void)AssessSetup(input); }, "infinite limit must be rejected");

    input = ApprovedInput();
    input.color_delta_e.auto_correction_max = -0.1;
    CheckInvalidArgument([&] { (void)AssessSetup(input); }, "negative limit must be rejected");

    input = ApprovedInput();
    input.minimum_overlap_percent = 100.1;
    CheckInvalidArgument([&] { (void)AssessSetup(input); }, "out-of-range minimum overlap must be rejected");

    input = ApprovedInput();
    input.measured_overlap_percent = -0.1;
    CheckInvalidArgument([&] { (void)AssessSetup(input); }, "negative measured overlap must be rejected");

    input = ApprovedInput();
    input.rotation_degrees.target_max = 3.1;
    CheckInvalidArgument([&] { (void)AssessSetup(input); }, "target maximum above automatic correction maximum must be rejected");
}

void TestInputImmutabilityAndDeterminism() {
    auto input = ApprovedInput();
    input.registration_pixels.measured = 2.5;
    const auto before = input;
    const auto first = AssessSetup(input);
    const auto second = AssessSetup(input);
    Check(SameInput(input, before), "assessment must not modify input");
    Check(first.readiness == second.readiness && first.apply_transient_correction == second.apply_transient_correction
            && first.reasons == second.reasons,
        "assessment must be deterministic");
}

} // namespace

int main() {
    try {
        TestReadinessLevels();
        TestHardBlockers();
        TestProfileTrustFailures();
        TestBoundaries();
        TestInvalidInput();
        TestInputImmutabilityAndDeterminism();
    } catch (const std::exception& error) {
        std::cerr << "UNEXPECTED: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
