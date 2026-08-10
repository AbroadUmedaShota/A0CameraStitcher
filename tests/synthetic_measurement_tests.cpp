#include "a0/m2/synthetic_measurement.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
#include <stdexcept>

namespace {

using a0::m2::CorrectionProposalStatus;
using a0::m2::ImageMeasurements;
using a0::m2::ProfileStatus;
using a0::m2::ProfileTrust;
using a0::m2::RgbPixel;
using a0::m2::RigProfileContract;
using a0::m2::SyntheticImage;
using a0::m2::SyntheticMeasurement;
using a0::m2::SyntheticPairFixture;

int failures = 0;

void Check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void CheckNear(const double actual, const double expected, const double tolerance, const std::string& message) {
    Check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
        message + " (actual=" + std::to_string(actual) + ")");
}

constexpr std::size_t kWidth = 64;
constexpr std::size_t kHeight = 48;
constexpr double kCenterX = (static_cast<double>(kWidth) - 1.0) / 2.0;
constexpr double kCenterY = (static_cast<double>(kHeight) - 1.0) / 2.0;
constexpr double kBaseAngleRadians = 0.23;
constexpr double kSigmaMajor = 8.0;
constexpr double kSigmaMinor = 3.0;

double BaseLuminance(const double x, const double y) {
    const double dx = x - kCenterX;
    const double dy = y - kCenterY;
    const double cosine = std::cos(kBaseAngleRadians);
    const double sine = std::sin(kBaseAngleRadians);
    const double major = cosine * dx + sine * dy;
    const double minor = -sine * dx + cosine * dy;
    return std::exp(-0.5 * (major * major / (kSigmaMajor * kSigmaMajor)
        + minor * minor / (kSigmaMinor * kSigmaMinor)));
}

SyntheticImage MakeImage(
    const double scale,
    const double rotation_radians,
    const double shift_x,
    const double shift_y,
    const double exposure_multiplier,
    const RgbPixel& color_multiplier) {
    SyntheticImage image{kWidth, kHeight, {}};
    image.pixels.reserve(kWidth * kHeight);
    const double cosine = std::cos(rotation_radians);
    const double sine = std::sin(rotation_radians);
    for (std::size_t y = 0; y < kHeight; ++y) {
        for (std::size_t x = 0; x < kWidth; ++x) {
            const double translated_x = static_cast<double>(x) - kCenterX - shift_x;
            const double translated_y = static_cast<double>(y) - kCenterY - shift_y;
            const double base_x = (cosine * translated_x + sine * translated_y) / scale + kCenterX;
            const double base_y = (-sine * translated_x + cosine * translated_y) / scale + kCenterY;
            const double value = std::clamp(
                0.4 * BaseLuminance(base_x, base_y) * exposure_multiplier,
                0.0,
                1.0);
            image.pixels.push_back({
                value * color_multiplier.red,
                value * color_multiplier.green,
                value * color_multiplier.blue,
            });
        }
    }
    return image;
}

SyntheticPairFixture MakeFixture(
    const SyntheticImage& camera_b,
    const std::string& profile_provenance = "approved-rig-fixture") {
    return {
        "fixture-multi-metric-001",
        std::string(a0::m2::kSyntheticImagePairSchemaVersion),
        profile_provenance,
        "synthetic-moment-measurement-v1",
        MakeImage(1.0, 0.0, 0.0, 0.0, 1.0, {1.0, 1.0, 1.0}),
        camera_b,
        80.0,
    };
}

RigProfileContract ApprovedProfile() {
    const ProfileTrust trust{
        ProfileStatus::approved,
        "1.1.0",
        "approved-rig-fixture",
        std::chrono::sys_seconds{std::chrono::seconds{100}},
        std::chrono::sys_seconds{std::chrono::seconds{200}},
        std::chrono::sys_seconds{std::chrono::seconds{150}},
    };
    const a0::m2::CorrectionBound bound{0.5, 3.0};
    return {
        trust,
        {70.0, bound, bound, bound, bound, bound},
    };
}

bool SameMeasurements(const ImageMeasurements& left, const ImageMeasurements& right) {
    return left.shift_pixels == right.shift_pixels
        && left.rotation_degrees == right.rotation_degrees
        && left.scale_difference_percent == right.scale_difference_percent
        && left.exposure_difference_ev == right.exposure_difference_ev
        && left.color_delta_e == right.color_delta_e
        && left.overlap_percent == right.overlap_percent;
}

bool SameProfile(const ProfileTrust& left, const ProfileTrust& right) {
    return left.status == right.status
        && left.schema_version == right.schema_version
        && left.provenance == right.provenance
        && left.measured_at == right.measured_at
        && left.valid_until == right.valid_until
        && left.assessed_at == right.assessed_at;
}

bool SameBound(const a0::m2::CorrectionBound& left, const a0::m2::CorrectionBound& right) {
    return left.target_max == right.target_max && left.auto_correction_max == right.auto_correction_max;
}

bool SameEnvelope(const a0::m2::CorrectionEnvelope& left, const a0::m2::CorrectionEnvelope& right) {
    return left.minimum_overlap_percent == right.minimum_overlap_percent
        && SameBound(left.registration_pixels, right.registration_pixels)
        && SameBound(left.rotation_degrees, right.rotation_degrees)
        && SameBound(left.scale_difference_percent, right.scale_difference_percent)
        && SameBound(left.exposure_difference_ev, right.exposure_difference_ev)
        && SameBound(left.color_delta_e, right.color_delta_e);
}

void TestDeterministicMeasurements() {
    const auto fixture = MakeFixture(MakeImage(1.02, 0.026, 1.4, -0.7, 1.0, {1.0, 1.0, 1.0}));
    const SyntheticMeasurement first = a0::m2::MeasureSyntheticPair(fixture);
    const SyntheticMeasurement second = a0::m2::MeasureSyntheticPair(fixture);
    Check(SameMeasurements(first.measurements, second.measurements),
        "synthetic measurements must be deterministic");
    CheckNear(first.measurements.shift_pixels, std::hypot(1.4, -0.7), 0.12,
        "synthetic shift must be measured from fixture pixels");
    CheckNear(first.measurements.rotation_degrees, 0.026 * 180.0 / std::numbers::pi, 0.15,
        "synthetic rotation must be measured from fixture pixels");
    CheckNear(first.measurements.scale_difference_percent, 2.0, 0.25,
        "synthetic scale must be measured from fixture pixels");
    CheckNear(first.measurements.exposure_difference_ev, 0.0, 0.01,
        "synthetic exposure baseline must be zero");
    CheckNear(first.measurements.color_delta_e, 0.0, 0.01,
        "synthetic color baseline must be zero");
    CheckNear(first.measurements.overlap_percent, 80.0, 0.0,
        "fixture overlap must be retained");
}

void TestExposureAndColorMeasurements() {
    const auto exposure = a0::m2::MeasureSyntheticPair(
        MakeFixture(MakeImage(1.0, 0.0, 0.0, 0.0, 2.0, {1.0, 1.0, 1.0})));
    CheckNear(exposure.measurements.exposure_difference_ev, 1.0, 0.01,
        "synthetic exposure must be measured in EV");

    const auto color = a0::m2::MeasureSyntheticPair(
        MakeFixture(MakeImage(1.0, 0.0, 0.0, 0.0, 1.0, {0.8, 1.0, 1.0})));
    Check(color.measurements.color_delta_e > 1.0,
        "synthetic color difference must produce a positive DeltaE");
}

void TestBoundedProposalAndBoundaries() {
    auto profile = ApprovedProfile();
    const auto within_target = a0::m2::MeasureSyntheticPair(
        MakeFixture(MakeImage(1.002, 0.002, 0.2, 0.0, 1.01, {0.99, 1.0, 1.0})));
    const auto no_correction = a0::m2::ProposeBoundedCorrection(within_target, profile);
    Check(no_correction.status == CorrectionProposalStatus::not_required,
        "measurements within profile targets must not propose correction");

    const auto bounded = a0::m2::MeasureSyntheticPair(
        MakeFixture(MakeImage(1.02, 0.02, 1.0, 0.0, 1.0, {1.0, 1.0, 1.0})));
    const auto proposal = a0::m2::ProposeBoundedCorrection(bounded, profile);
    Check(proposal.status == CorrectionProposalStatus::accepted
            && proposal.apply_transient_correction,
        "bounded target exceedances must become transient correction proposals");
    Check(proposal.fixture_id == bounded.fixture_id
            && proposal.profile_provenance == profile.trust.provenance
            && proposal.measurement_provenance == bounded.measurement_provenance
            && SameMeasurements(proposal.measurements, bounded.measurements),
        "proposal must retain fixture provenance and all measured values");

    auto at_auto_limit = bounded;
    at_auto_limit.measurements.shift_pixels = 3.0;
    const auto boundary = a0::m2::ProposeBoundedCorrection(at_auto_limit, profile);
    Check(boundary.status == CorrectionProposalStatus::accepted,
        "automatic correction maximum equality must remain accepted");
}

void TestFailClosedAndNoMutation() {
    auto profile = ApprovedProfile();
    const auto profile_before = profile;
    const auto measured = a0::m2::MeasureSyntheticPair(
        MakeFixture(MakeImage(1.05, 0.0, 0.0, 0.0, 1.0, {1.0, 1.0, 1.0})));
    const auto over_limit = a0::m2::ProposeBoundedCorrection(measured, profile);
    Check(over_limit.status == CorrectionProposalStatus::rejected
            && !over_limit.apply_transient_correction,
        "over-limit measurements must fail closed");
    Check(SameProfile(profile.trust, profile_before.trust)
            && SameEnvelope(profile.correction_envelope, profile_before.correction_envelope),
        "over-limit decisions must not mutate the profile or its envelope");

    const auto incomplete = a0::m2::ProposeBoundedCorrection(measured, profile, false, true);
    Check(incomplete.status == CorrectionProposalStatus::rejected,
        "incomplete coverage must fail closed through the proposal seam");
    const auto incompatible = a0::m2::ProposeBoundedCorrection(measured, profile, true, false);
    Check(incompatible.status == CorrectionProposalStatus::rejected,
        "incompatible camera settings must fail closed through the proposal seam");

    auto mismatch = measured;
    mismatch.profile_provenance = "different-profile";
    const auto mismatch_result = a0::m2::ProposeBoundedCorrection(mismatch, profile);
    Check(mismatch_result.status == CorrectionProposalStatus::rejected,
        "profile provenance mismatch must fail closed");

    auto draft = profile;
    draft.trust.status = ProfileStatus::draft;
    const auto draft_result = a0::m2::ProposeBoundedCorrection(measured, draft);
    Check(draft_result.status == CorrectionProposalStatus::rejected,
        "unapproved profile must fail closed");

    auto malformed = measured;
    malformed.measurements.rotation_degrees = std::numeric_limits<double>::quiet_NaN();
    const auto malformed_result = a0::m2::ProposeBoundedCorrection(malformed, profile);
    Check(malformed_result.status == CorrectionProposalStatus::rejected,
        "malformed measured values must fail closed");
}

void TestMalformedFixtures() {
    auto malformed = MakeFixture(MakeImage(1.0, 0.0, 0.0, 0.0, 1.0, {1.0, 1.0, 1.0}));
    malformed.camera_b.pixels.pop_back();
    try {
        static_cast<void>(a0::m2::MeasureSyntheticPair(malformed));
        Check(false, "pixel-count mismatch must be rejected");
    } catch (const std::invalid_argument&) {
    }

    malformed = MakeFixture(MakeImage(1.0, 0.0, 0.0, 0.0, 1.0, {1.0, 1.0, 1.0}));
    malformed.schema_version = "m2.synthetic-image-pair.v0";
    try {
        static_cast<void>(a0::m2::MeasureSyntheticPair(malformed));
        Check(false, "unsupported fixture schema must be rejected");
    } catch (const std::invalid_argument&) {
    }
}

} // namespace

int main() {
    TestDeterministicMeasurements();
    TestExposureAndColorMeasurements();
    TestBoundedProposalAndBoundaries();
    TestFailClosedAndNoMutation();
    TestMalformedFixtures();
    if (failures != 0) {
        std::cerr << failures << " synthetic measurement contract test(s) failed\n";
        return 1;
    }
    std::cout << "synthetic measurement contracts passed\n";
    return 0;
}
