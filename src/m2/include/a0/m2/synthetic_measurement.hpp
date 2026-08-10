#pragma once

#include "a0/m2/setup_assessment.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2 {

// This is a rights-cleared, normalized-value seam for deterministic synthetic tests only.
// It is not a decoder for real camera originals and does not establish A0 quality.
inline constexpr std::string_view kSyntheticImagePairSchemaVersion = "m2.synthetic-image-pair.v1";

struct RgbPixel {
    double red{};
    double green{};
    double blue{};
};

struct SyntheticImage {
    std::size_t width_pixels{};
    std::size_t height_pixels{};
    std::vector<RgbPixel> pixels;
};

struct SyntheticPairFixture {
    std::string fixture_id;
    std::string schema_version{kSyntheticImagePairSchemaVersion};
    std::string profile_provenance;
    std::string measurement_provenance;
    SyntheticImage camera_a;
    SyntheticImage camera_b;
    double overlap_percent{};
};

struct ImageMeasurements {
    double shift_pixels{};
    double rotation_degrees{};
    double scale_difference_percent{};
    double exposure_difference_ev{};
    double color_delta_e{};
    double overlap_percent{};
};

struct SyntheticMeasurement {
    std::string fixture_id;
    std::string fixture_schema_version;
    std::string profile_provenance;
    std::string measurement_provenance;
    ImageMeasurements measurements;
};

struct CorrectionBound {
    double target_max{};
    double auto_correction_max{};
};

struct CorrectionEnvelope {
    double minimum_overlap_percent{};
    CorrectionBound registration_pixels;
    CorrectionBound rotation_degrees;
    CorrectionBound scale_difference_percent;
    CorrectionBound exposure_difference_ev;
    CorrectionBound color_delta_e;
};

// The trust and envelope are supplied by the caller; this type deliberately does not
// provide defaults or mutate/persist a profile from measurement results.
struct RigProfileContract {
    ProfileTrust trust;
    CorrectionEnvelope correction_envelope;
};

enum class CorrectionProposalStatus {
    not_required,
    accepted,
    rejected,
};

struct CorrectionProposal {
    CorrectionProposalStatus status{CorrectionProposalStatus::rejected};
    std::string fixture_id;
    std::string profile_provenance;
    std::string measurement_provenance;
    ImageMeasurements measurements;
    SetupReadiness readiness{SetupReadiness::physical_adjustment_required};
    bool apply_transient_correction{};
    std::vector<std::string> reasons;
};

// Measures only the supplied synthetic fixture. Malformed fixtures fail closed by throwing
// std::invalid_argument; no profile or fixture is changed.
[[nodiscard]] SyntheticMeasurement MeasureSyntheticPair(const SyntheticPairFixture& fixture);

// Connects measured values to the caller-supplied profile envelope. Malformed input,
// provenance mismatch, unapproved/expired profiles, and over-limit values return rejected.
// The profile remains immutable and no thresholds are learned or updated.
[[nodiscard]] CorrectionProposal ProposeBoundedCorrection(
    const SyntheticMeasurement& measurement,
    const RigProfileContract& profile,
    bool full_coverage = true,
    bool camera_settings_compatible = true);

} // namespace a0::m2
