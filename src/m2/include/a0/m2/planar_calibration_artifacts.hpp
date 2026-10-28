#pragma once

#include "a0/m2/draft_calibration_artifacts.hpp"
#include "a0/m2/planar_calibration.hpp"

#include <string>
#include <string_view>

namespace a0::m2::calibration {
inline constexpr std::string_view kPlanarCalibrationJobSchema = "a0.planar-calibration-job.v1";
inline constexpr std::string_view kPlanarCalibrationReportSchema = "a0.planar-calibration-report.v1";

// Where the overlap chart lies on the sheet, in the convention of
// referencePlacement: chart = R(angle) * document + translation. All zero
// makes the overlap chart frame the document frame (F-000-000 at the origin).
struct OverlapChartPlacement {
    double angle_radians;
    double translation_x_mm;
    double translation_y_mm;
};

struct PlanarCalibrationMetadata {
    std::string profile_id;
    std::string provenance_id;
    std::string measured_at_utc;
    DraftPaperOrientation orientation;
    DraftCameraOrder camera_order;
    OverlapChartPlacement overlap_placement;
};

// Development-only draft writer for the planar calibration (#374). The
// profile is the existing rig profile v2 draft: intrinsics are the NOMINAL
// K (the profile has no field for that; the tool ID a0-planar-calibrator and
// the report record it), documentToImage = H_c composed with the overlap
// placement, inputSha256 = the overlap view images, fitResidualPixels = the
// training and overlap fit. Owner-decision blocks, physical identities and
// rig measurements are null. Every camera needs evaluated validation views.
[[nodiscard]] DraftCalibrationArtifacts BuildPlanarCalibrationArtifacts(
    std::string_view observation_session_json, const PlanarCalibrationRequest& request,
    const PlanarCalibrationPolicy& policy, const PlanarCalibrationMetadata& metadata);

// Strict, bounded job JSON (a0.planar-calibration-job.v1). Malformed jobs or
// sessions and invalid policies throw invalid_argument; estimation or
// metadata failures return a diagnostic report with no profile.
[[nodiscard]] DraftCalibrationArtifacts RunPlanarCalibrationJob(
    std::string_view observation_session_json, std::string_view job_json);
} // namespace a0::m2::calibration
