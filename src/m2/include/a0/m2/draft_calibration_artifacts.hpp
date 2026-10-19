#pragma once

#include "a0/m2/reference_document_mapping.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace a0::m2::calibration {
inline constexpr std::string_view kDraftCalibrationJobSchema = "a0.draft-calibration-job.v1";
inline constexpr std::string_view kDraftCalibrationReportSchema = "a0.draft-calibration-report.v1";

enum class DraftPaperOrientation { Landscape, Portrait };
enum class DraftCameraOrder { LeftRight, TopBottom };
struct CalibrationRigMeasurements {
    std::optional<double> baseline_mm;
    std::optional<double> overlap_mm;
    std::array<std::optional<double>, 2> camera_to_document_mm;
    std::array<std::optional<double>, 2> lens_focal_length_mm;
};
struct DraftCalibrationMetadata {
    std::string profile_id;
    std::string provenance_id;
    std::string measured_at_utc;
    DraftPaperOrientation orientation;
    DraftCameraOrder camera_order;
    CalibrationRigMeasurements rig;
};
struct DraftCalibrationArtifacts {
    bool ok{};
    std::string profile_json; // empty on any mapping/metadata failure
    std::string profile_fingerprint_sha256;
    std::string report_json;
};

// Development-only serialization. Always draft, owner-decision blocks and
// physical identity always null. D810 stored-order dimensions only; smaller
// synthetic calculations cannot be mislabeled as D810 profiles. The detailed
// report retains the validated observation session and every view result.
// No image is read; image hashes are declarations from the session.
[[nodiscard]] DraftCalibrationArtifacts BuildDraftCalibrationArtifacts(
    std::string_view observation_session_json, const ReferencePairRequest& request,
    const ReferenceMappingPolicy& policy, const DraftCalibrationMetadata& metadata);

// Strict, bounded job JSON used by the standalone development executable.
// Malformed jobs/sessions and invalid numerical policies throw invalid_argument.
// Mapping/metadata failures return a diagnostic report with no profile.
[[nodiscard]] DraftCalibrationArtifacts RunDraftCalibrationJob(
    std::string_view observation_session_json, std::string_view job_json);
} // namespace a0::m2::calibration
