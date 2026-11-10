#include "a0/m2/planar_calibration_artifacts.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/common/protocol_json.hpp"
#include "calibration_exact_predicates.hpp"
#include "draft_calibration_json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace a0::m2::calibration {
namespace {
using namespace a0::common::protocol_json;
using namespace draft_json;
using Matrix3 = std::array<double, 9>;
constexpr std::string_view kToolId = "a0-planar-calibrator";
constexpr std::string_view kToolVersion = "1.0.0";

// H_c (overlap chart mm -> undistorted pixel) composed with the sheet ->
// overlap chart placement, normalized to h22 = 1.
std::optional<Matrix3> SheetToImage(const Matrix3& h, const OverlapChartPlacement& placement) {
    if (!std::isfinite(placement.angle_radians) || !std::isfinite(placement.translation_x_mm) || !std::isfinite(placement.translation_y_mm))
        return std::nullopt;
    const double c = std::cos(placement.angle_radians), s = std::sin(placement.angle_radians);
    const Matrix3 transform{c, -s, placement.translation_x_mm, s, c, placement.translation_y_mm, 0, 0, 1};
    Matrix3 out{};
    for (std::size_t row = 0; row < 3; ++row) for (std::size_t column = 0; column < 3; ++column) {
        double sum = 0;
        for (std::size_t k = 0; k < 3; ++k) sum += h[row * 3 + k] * transform[k * 3 + column];
        out[row * 3 + column] = sum;
    }
    if (!std::isfinite(out[8]) || out[8] == 0) return std::nullopt;
    const double scale = out[8];
    for (auto& value : out) {
        value /= scale;
        if (!std::isfinite(value)) return std::nullopt;
    }
    out[8] = 1.0;
    if (!exact_detail::ExactNonzeroDeterminant(out)) return std::nullopt;
    return out;
}

std::string PlacementJson(const std::optional<PlanarPlacement>& value) {
    if (!value) return "null";
    return "{\"angleRadians\":" + N(value->angle_radians) + ",\"translationXMm\":" + N(value->translation_x_mm)
        + ",\"translationYMm\":" + N(value->translation_y_mm) + '}';
}
std::string LinesJson(const std::optional<StraightnessSummary>& value) {
    if (!value) return "null";
    return "{\"rms\":" + N(value->rms_pixels) + ",\"max\":" + N(value->max_pixels) + ",\"lineCount\":" + std::to_string(value->line_count)
        + ",\"pointCount\":" + std::to_string(value->point_count) + '}';
}
std::string StraightnessJson(const Straightness& value) {
    return "{\"raw\":" + LinesJson(value.raw) + ",\"undistorted\":" + LinesJson(value.undistorted) + '}';
}
std::string PolicyJson(const PlanarCalibrationPolicy& p) {
    return "{\"minimumTrainingViews\":" + std::to_string(p.minimum_training_views) + ",\"maxIterations\":" + std::to_string(p.max_iterations)
        + ",\"relativeCostTolerance\":" + N(p.relative_cost_tolerance) + ",\"relativeStepTolerance\":" + N(p.relative_step_tolerance)
        + ",\"initialDamping\":" + N(p.initial_damping) + ",\"maxUndistortionIterations\":" + std::to_string(p.max_undistortion_iterations)
        + ",\"undistortionTolerancePixels\":" + N(p.undistortion_tolerance_pixels) + '}';
}
// Declared values may be invalid; such a value is reported as null, never as JSON NaN.
std::string Finite(double value) { return std::isfinite(value) ? N(value) : "null"; }
std::string NominalJson(const NominalCamera& camera, const std::optional<PinholeIntrinsics>& k) {
    return "{\"focalLengthMm\":" + Finite(camera.focal_length_mm) + ",\"sensorWidthMm\":" + Finite(camera.sensor_width_mm)
        + ",\"sensorHeightMm\":" + Finite(camera.sensor_height_mm) + ",\"intrinsics\":" + (k ? K(*k) : "null") + '}';
}
// Seam disagreement in overlap chart mm and in output pixels (mm * dpi / 25.4).
std::string SeamJson(const PlanarSeamMeasure& measure, std::uint32_t dpi) {
    std::string out = "{\"outcome\":" + Q(PlanarSeamOutcomeCode(measure.outcome));
    if (!measure.disagreement) return out + ",\"pointCount\":0,\"meanMm\":null,\"p95Mm\":null,\"maxMm\":null,\"meanOutputPixels\":null,\"p95OutputPixels\":null,\"maxOutputPixels\":null}";
    const auto& d = *measure.disagreement;
    const auto pixels = [dpi](double mm) { return N(mm * dpi / 25.4); };
    return out + ",\"pointCount\":" + std::to_string(d.point_count) + ",\"meanMm\":" + N(d.mean_mm) + ",\"p95Mm\":" + N(d.p95_mm)
        + ",\"maxMm\":" + N(d.max_mm) + ",\"meanOutputPixels\":" + pixels(d.mean_mm) + ",\"p95OutputPixels\":" + pixels(d.p95_mm)
        + ",\"maxOutputPixels\":" + pixels(d.max_mm) + '}';
}
std::string CoverageJson(const PlanarCameraCoverage& coverage) {
    std::string out = "{\"imageCornerRadiusSquared\":" + (coverage.image_corner_radius_squared ? N(*coverage.image_corner_radius_squared) : "null")
        + ",\"fitPointCells\":[";
    for (std::size_t row = 0; row < 4; ++row) {
        out += row ? ",[" : "[";
        for (std::size_t column = 0; column < 4; ++column) out += (column ? "," : "") + std::to_string(coverage.fit_point_cells[row * 4 + column]);
        out += ']';
    }
    return out + "]}";
}
std::string CameraJson(const CameraPlanarResult& camera, const std::optional<Matrix3>& sheet_to_image, const PlanarCameraCoverage& coverage) {
    std::string out = "{\"camera\":" + Q(CameraAliasCode(camera.camera)) + ",\"status\":" + Q(PlanarCameraStatusCode(camera.status))
        + ",\"nominalIntrinsics\":" + (camera.nominal_intrinsics ? K(*camera.nominal_intrinsics) : "null")
        + ",\"solver\":" + Solver(camera.solver) + ",\"distortion\":" + (camera.distortion ? D(*camera.distortion) : "null")
        + ",\"overlapChartToImage\":" + (camera.document_to_image ? Matrix(*camera.document_to_image) : "null")
        + ",\"documentToImage\":" + (sheet_to_image ? Matrix(*sheet_to_image) : "null") + ",\"distortionCheck\":";
    if (!camera.distortion_check) out += "null";
    else {
        const auto& check = *camera.distortion_check;
        out += "{\"maxRadiusSquared\":" + N(check.max_radius_squared) + ",\"minimumRadialSlope\":" + N(check.minimum_radial_slope)
            + ",\"minimumJacobianDeterminant\":" + N(check.minimum_jacobian_determinant) + '}';
    }
    out += ",\"fitResidual\":" + Residual(camera.fit_residual) + ",\"overlapResidual\":" + Residual(camera.overlap_residual)
        + ",\"heldOutCoverage\":" + Q(HeldOutCoverageCode(camera.held_out_coverage)) + ",\"heldOutResidual\":" + Residual(camera.held_out_residual)
        + ",\"straightness\":{\"fit\":" + StraightnessJson(camera.fit_straightness) + ",\"heldOut\":" + StraightnessJson(camera.held_out_straightness)
        + "},\"views\":[";
    for (std::size_t i = 0; i < camera.views.size(); ++i) {
        if (i) out += ',';
        const auto& view = camera.views[i];
        out += "{\"viewId\":" + Q(view.view_id) + ",\"role\":" + Q(ViewRoleCode(view.role)) + ",\"overlap\":" + (view.overlap ? "true" : "false")
            + ",\"outcome\":" + Q(PlanarViewOutcomeCode(view.outcome)) + ",\"placement\":" + PlacementJson(view.placement)
            + ",\"residual\":" + Residual(view.residual) + ",\"solver\":" + Solver(view.solver)
            + ",\"straightness\":" + StraightnessJson(view.straightness) + '}';
    }
    return out + "],\"coverage\":" + CoverageJson(coverage) + '}';
}
const SessionView* FindView(const CalibrationObservationSession& session, const std::string& id) {
    for (const auto& view : session.Views()) if (view.id == id) return &view;
    return nullptr;
}

std::string Profile(const CalibrationObservationSession& session, const PlanarCalibrationResult& result,
    const std::array<Matrix3, 2>& sheet_to_image, const PlanarCalibrationRequest& request, const PlanarCalibrationMetadata& metadata) {
    std::string out = ProfileHead(metadata.profile_id, metadata.orientation, metadata.camera_order);
    for (std::size_t i = 0; i < 2; ++i) {
        if (i) out += ',';
        const auto& camera = result.cameras[i];
        out += CameraProjection(camera.camera, *camera.nominal_intrinsics, *camera.distortion, sheet_to_image[i]);
    }
    out += "},\"calibration\":{\"method\":\"chart-fiducial-planar\",\"chartId\":" + Q(session.ChartId())
        + ",\"provenanceId\":" + Q(metadata.provenance_id) + ",\"measuredAt\":" + Q(metadata.measured_at_utc)
        + ",\"tool\":{\"toolId\":" + Q(kToolId) + ",\"version\":" + Q(kToolVersion) + "},\"inputSha256\":{\"CAM-A\":"
        + Q(FindView(session, request.overlap_view_ids[0])->image_sha256) + ",\"CAM-B\":" + Q(FindView(session, request.overlap_view_ids[1])->image_sha256)
        + "},\"fitResidualPixels\":{\"CAM-A\":" + Residual(result.cameras[0].fit_residual, false)
        + ",\"CAM-B\":" + Residual(result.cameras[1].fit_residual, false) + "},\"rigMeasurements\":"
        + Measurements(CalibrationRigMeasurements{}) + "},\"outputRaster\":null,\"correctionEnvelope\":null,\"qualityContract\":null,\"approval\":null}";
    return out;
}

// Job parser: bounded before the shared JSON parser and exact keys at every
// object. No caller-controlled status/approval or automatic numerical defaults.
[[noreturn]] void InvalidJob() { throw std::invalid_argument("planar calibration: invalid job"); }
struct Failure { [[noreturn]] static void Fail(std::string_view, std::string_view) { InvalidJob(); } };
// `optional_present` is the number of optional keys the caller found in `value`;
// they are counted, so any other unknown key still fails the size check.
void Keys(const JsonValue& value, std::initializer_list<const char*> names, std::size_t optional_present = 0) {
    if (value.kind != JsonKind::object || value.object.size() != names.size() + optional_present) InvalidJob();
    for (const auto* name : names) if (!value.object.contains(name)) InvalidJob();
}
const JsonValue& At(const JsonValue& value, const char* key) {
    if (value.kind != JsonKind::object || !value.object.contains(key)) InvalidJob();
    return value.object.at(key);
}
std::string Text(const JsonValue& value) { if (value.kind != JsonKind::string) InvalidJob(); return value.string; }
double Number(const JsonValue& value) {
    if (value.kind != JsonKind::number) InvalidJob();
    double out{};
    const auto parsed = std::from_chars(value.string.data(), value.string.data() + value.string.size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr != value.string.data() + value.string.size() || !std::isfinite(out)) InvalidJob();
    return out;
}
std::uint32_t Integer(const JsonValue& value) {
    const double out = Number(value);
    if (out < 0 || out > 1000 || std::floor(out) != out) InvalidJob();
    return static_cast<std::uint32_t>(out);
}
// Names of the coefficients to solve, each at most once; an empty array solves none.
SolvedCoefficients Coefficients(const JsonValue& value) {
    if (value.kind != JsonKind::array || value.array.size() > 5) InvalidJob();
    SolvedCoefficients out{false, false, false, false, false};
    for (const auto& item : value.array) {
        const auto name = Text(item);
        bool* flag = name == "k1" ? &out.k1 : name == "k2" ? &out.k2 : name == "k3" ? &out.k3
            : name == "p1" ? &out.p1 : name == "p2" ? &out.p2 : nullptr;
        if (flag == nullptr || *flag) InvalidJob();
        *flag = true;
    }
    return out;
}
// {"solvedCoefficients": [...], "fixedToZeroCoefficients": [...]}, in k1 k2 k3 p1 p2 order.
std::string DistortionModelJson(const SolvedCoefficients& solved) {
    const std::array<std::pair<const char*, bool>, 5> all{{{"k1", solved.k1}, {"k2", solved.k2}, {"k3", solved.k3}, {"p1", solved.p1}, {"p2", solved.p2}}};
    std::string solved_names, fixed_names;
    for (const auto& [name, selected] : all) {
        auto& target = selected ? solved_names : fixed_names;
        if (!target.empty()) target += ',';
        target += Q(name);
    }
    return "{\"solvedCoefficients\":[" + solved_names + "],\"fixedToZeroCoefficients\":[" + fixed_names + "]}";
}
constexpr double kMaximumEdgeMarginPixels = 100000;
// {"marginPixels": m, "excludedFitPoints": {"CAM-A": n, "CAM-B": n}}: the fit points (training and overlap views) left
// out per camera; null counts when the estimate did not run.
std::string EdgeMarginJson(double margin, const std::optional<PlanarCalibrationResult>& result) {
    const auto count = [&](std::size_t i) { return result ? std::to_string(result->cameras[i].edge_excluded_points) : std::string("null"); };
    return "{\"marginPixels\":" + Finite(margin) + ",\"excludedFitPoints\":{\"CAM-A\":" + count(0) + ",\"CAM-B\":" + count(1) + "}}";
}
NominalCamera Nominal(const JsonValue& value) {
    Keys(value, {"focalLengthMm", "sensorWidthMm", "sensorHeightMm"});
    return {Number(At(value, "focalLengthMm")), Number(At(value, "sensorWidthMm")), Number(At(value, "sensorHeightMm"))};
}
} // namespace

DraftCalibrationArtifacts BuildPlanarCalibrationArtifacts(std::string_view json, const PlanarCalibrationRequest& request,
    const PlanarCalibrationPolicy& policy, const PlanarCalibrationMetadata& metadata) {
    const auto session = CalibrationObservationSession::Parse(json);
    DraftCalibrationArtifacts out;
    std::string rejection, profile_validation_failure;
    std::optional<PlanarCalibrationResult> result;
    std::optional<PlanarCalibrationMeasures> measures;
    std::array<std::optional<Matrix3>, 2> sheet_to_image{};
    if (session.Camera(CameraAlias::CamA).width_pixels != 7360 || session.Camera(CameraAlias::CamA).height_pixels != 4912
        || session.Camera(CameraAlias::CamB).width_pixels != 7360 || session.Camera(CameraAlias::CamB).height_pixels != 4912) {
        rejection = "unsupported-image-dimensions";
    } else {
        result = EstimatePlanarCalibration(session, request, policy);
        measures = MeasurePlanarCalibration(session, request, policy, *result);
        bool estimated = true, validation_evaluated = true;
        for (std::size_t i = 0; i < 2; ++i) {
            const auto& camera = result->cameras[i];
            estimated = estimated && camera.status == PlanarCameraStatus::Estimated;
            validation_evaluated = validation_evaluated && camera.held_out_coverage == HeldOutCoverage::AllViewsEvaluated
                && camera.held_out_residual && camera.held_out_residual->point_count > 0;
            if (camera.document_to_image) sheet_to_image[i] = SheetToImage(*camera.document_to_image, metadata.overlap_placement);
        }
        // A placement not declared flat on the document plane is refused first.
        if (!metadata.overlap_placement.coplanar_confirmed) rejection = "overlap-placement-invalid";
        else if (!estimated) rejection = "planar-calibration-unavailable";
        else if (!validation_evaluated) rejection = "validation-not-evaluated";
        else if (!sheet_to_image[0] || !sheet_to_image[1]) rejection = "overlap-placement-invalid";
        else {
            try {
                auto profile = Profile(session, *result, {*sheet_to_image[0], *sheet_to_image[1]}, request, metadata);
                const auto validated = RigProfileV2::Parse(profile);
                if (metadata.measured_at_utc < session.Provenance().measured_at_utc)
                    throw std::invalid_argument("draft calibration: measurement precedes observations");
                if (validated.IsApproved() || !validated.IsCalibrated()) throw std::invalid_argument("draft calibration: invalid generated state");
                out.profile_fingerprint_sha256 = validated.FingerprintSha256();
                out.profile_json = std::move(profile);
                out.ok = true;
            } catch (const std::invalid_argument& error) { rejection = "profile-metadata-invalid"; profile_validation_failure = error.what(); }
        }
    }
    const auto& placement = metadata.overlap_placement;
    std::string plane = "null";
    try { plane = draft_json::Plane(metadata.orientation, metadata.camera_order); } catch (const std::invalid_argument&) {}
    std::string report = "{\"schemaVersion\":" + Q(kPlanarCalibrationReportSchema) + ",\"result\":" + Q(out.ok ? "draft-produced" : "no-profile")
        + ",\"rejection\":" + (rejection.empty() ? "null" : Q(rejection)) + ",\"provenanceId\":" + Q(metadata.provenance_id)
        + ",\"profileValidationFailure\":" + (profile_validation_failure.empty() ? "null" : Q(profile_validation_failure))
        + ",\"profileId\":" + Q(metadata.profile_id) + ",\"measuredAt\":" + Q(metadata.measured_at_utc)
        + ",\"tool\":{\"toolId\":" + Q(kToolId) + ",\"version\":" + Q(kToolVersion) + '}'
        + ",\"profileFingerprintSha256\":" + (out.ok ? Q(out.profile_fingerprint_sha256) : "null")
        + ",\"sessionSha256\":" + Q(Sha256(json)) + ",\"session\":" + std::string(json)
        + ",\"numericalPolicy\":" + PolicyJson(policy)
        + ",\"model\":\"planar-rigid-placement-homography-brown-conrady-k1k2k3-p1p2\""
        // Only a reduced selection is recorded, so a job without the selection keeps its report bytes.
        + (request.solved.All() ? std::string() : ",\"distortionModel\":" + DistortionModelJson(request.solved))
        // Likewise only a margin above 0 is recorded, with the number of fit points it left out per camera.
        + (request.edge_margin_pixels > 0 ? ",\"edgeMargin\":" + EdgeMarginJson(request.edge_margin_pixels, result) : std::string())
        + ",\"intrinsicsSource\":\"nominal-not-estimated\",\"nominalCameras\":{";
    for (std::size_t i = 0; i < 2; ++i) {
        if (i) report += ',';
        const auto& group = session.Camera(i == 0 ? CameraAlias::CamA : CameraAlias::CamB);
        report += Q(i == 0 ? "CAM-A" : "CAM-B") + ':' + NominalJson(request.nominal[i], NominalIntrinsics(request.nominal[i], group.width_pixels, group.height_pixels));
    }
    report += "},\"overlapViews\":{\"CAM-A\":" + Q(request.overlap_view_ids[0]) + ",\"CAM-B\":" + Q(request.overlap_view_ids[1])
        + "},\"overlapPlacement\":{\"angleRadians\":" + Finite(placement.angle_radians) + ",\"translationXMm\":" + Finite(placement.translation_x_mm)
        + ",\"translationYMm\":" + Finite(placement.translation_y_mm) + ",\"coplanarConfirmed\":" + (placement.coplanar_confirmed ? "true" : "false")
        + "},\"documentPlane\":" + plane
        + ",\"fitResidualScope\":\"training and overlap usedPointIds, raw camera pixels\""
        + ",\"heldOutResidualScope\":\"validation measurementPointIds after a placement-only fit to posePointIds, raw camera pixels\""
        + ",\"quality\":\"not-assessed\",\"cameras\":";
    if (!result) report += "null";
    else report += '[' + CameraJson(result->cameras[0], sheet_to_image[0], measures->coverage[0]) + ','
        + CameraJson(result->cameras[1], sheet_to_image[1], measures->coverage[1]) + ']';
    report += ",\"seam\":";
    if (!measures) report += "null";
    else report += "{\"outputDpi\":" + std::to_string(metadata.seam_output_dpi)
        + ",\"scope\":\"chart points detected in both overlap views, each mapped to overlap chart mm by its camera's undistortion and documentToImage inverse\""
        + ",\"overlap\":" + SeamJson(measures->overlap_seam, metadata.seam_output_dpi)
        + ",\"checkerboardHeldOut\":" + SeamJson(measures->checkerboard_seam, metadata.seam_output_dpi) + '}';
    out.report_json = report + '}';
    return out;
}

DraftCalibrationArtifacts RunPlanarCalibrationJob(std::string_view session, std::string_view job_json) {
    if (job_json.empty() || job_json.size() > 65536) InvalidJob();
    // Jobs have a shallow, fixed structure; reject deep containers before the
    // recursive parser, including nesting hidden in an unknown field.
    int depth = 0;
    bool quoted = false, escaped = false;
    for (const char c : job_json) {
        if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; }
        else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 8) InvalidJob(); }
        else if (c == '}' || c == ']') { if (--depth < 0) InvalidJob(); }
    }
    const auto job = BasicJsonParser<Failure>(job_json).Parse();
    // Optional: distortionCoefficients, the coefficients to solve (the others stay 0). Absent: all five.
    const bool has_selection = job.kind == JsonKind::object && job.object.contains("distortionCoefficients");
    // Optional: edgeMarginPixels, observed points closer than this to an image border are left out of the fit. Absent or 0: none.
    const bool has_margin = job.kind == JsonKind::object && job.object.contains("edgeMarginPixels");
    Keys(job, {"schemaVersion", "profileId", "provenanceId", "measuredAt", "documentPlane", "overlapViews", "overlapPlacement", "nominalCameras",
        "numericalPolicy", "seamOutputDpi"}, (has_selection ? 1 : 0) + (has_margin ? 1 : 0));
    const double edge_margin = has_margin ? Number(At(job, "edgeMarginPixels")) : 0.0;
    if (edge_margin < 0 || edge_margin > kMaximumEdgeMarginPixels) InvalidJob();
    if (Text(At(job, "schemaVersion")) != kPlanarCalibrationJobSchema) InvalidJob();
    const auto& plane = At(job, "documentPlane"); Keys(plane, {"orientation", "cameraOrder"});
    PlanarCalibrationMetadata metadata;
    metadata.profile_id = Text(At(job, "profileId")); metadata.provenance_id = Text(At(job, "provenanceId")); metadata.measured_at_utc = Text(At(job, "measuredAt"));
    const auto orientation = Text(At(plane, "orientation")), order = Text(At(plane, "cameraOrder"));
    if (orientation != "landscape" && orientation != "portrait") InvalidJob();
    if (order != "CAM-A-left-CAM-B-right" && order != "CAM-A-top-CAM-B-bottom") InvalidJob();
    metadata.orientation = orientation == "landscape" ? DraftPaperOrientation::Landscape : DraftPaperOrientation::Portrait;
    metadata.camera_order = order == "CAM-A-left-CAM-B-right" ? DraftCameraOrder::LeftRight : DraftCameraOrder::TopBottom;
    const auto& placement = At(job, "overlapPlacement"); Keys(placement, {"angleRadians", "translationXMm", "translationYMm", "coplanarConfirmed"});
    if (At(placement, "coplanarConfirmed").kind != JsonKind::boolean) InvalidJob();
    metadata.overlap_placement = {Number(At(placement, "angleRadians")), Number(At(placement, "translationXMm")), Number(At(placement, "translationYMm")),
        At(placement, "coplanarConfirmed").boolean};
    const double dpi = Number(At(job, "seamOutputDpi"));
    if (dpi < 1 || dpi > 65535 || std::floor(dpi) != dpi) InvalidJob();
    metadata.seam_output_dpi = static_cast<std::uint32_t>(dpi);
    const auto& views = At(job, "overlapViews"); Keys(views, {"CAM-A", "CAM-B"});
    const auto& nominal = At(job, "nominalCameras"); Keys(nominal, {"CAM-A", "CAM-B"});
    const PlanarCalibrationRequest request{{Text(At(views, "CAM-A")), Text(At(views, "CAM-B"))},
        {Nominal(At(nominal, "CAM-A")), Nominal(At(nominal, "CAM-B"))},
        has_selection ? Coefficients(At(job, "distortionCoefficients")) : SolvedCoefficients{}, edge_margin};
    const auto& p = At(job, "numericalPolicy");
    Keys(p, {"minimumTrainingViews", "maxIterations", "relativeCostTolerance", "relativeStepTolerance", "initialDamping", "maxUndistortionIterations", "undistortionTolerancePixels"});
    const PlanarCalibrationPolicy policy{Integer(At(p, "minimumTrainingViews")), Integer(At(p, "maxIterations")), Number(At(p, "relativeCostTolerance")),
        Number(At(p, "relativeStepTolerance")), Number(At(p, "initialDamping")), Integer(At(p, "maxUndistortionIterations")),
        Number(At(p, "undistortionTolerancePixels"))};
    return BuildPlanarCalibrationArtifacts(session, request, policy, metadata);
}
} // namespace a0::m2::calibration
