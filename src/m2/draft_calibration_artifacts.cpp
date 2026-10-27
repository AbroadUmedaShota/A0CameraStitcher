#include "a0/m2/draft_calibration_artifacts.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/common/protocol_json.hpp"
#include "draft_calibration_json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <vector>

namespace a0::m2::calibration {
namespace {
using namespace a0::common::protocol_json;
constexpr std::string_view kToolId = "a0-draft-calibrator";
constexpr std::string_view kToolVersion = "1.0.0";

using namespace draft_json;
std::string Pose(const std::optional<ChartPose>& value) {
    if (!value) return "null";
    return "{\"rotation\":" + Numbers(value->rotation) + ",\"translationMm\":" + Numbers(value->translation_mm) + '}';
}
std::string Plane(const DraftCalibrationMetadata& metadata) { return draft_json::Plane(metadata.orientation, metadata.camera_order); }
std::string Profile(const CalibrationObservationSession& session, const ReferenceDocumentMappingResult& result,
    const DraftCalibrationMetadata& metadata) {
    std::string out = ProfileHead(metadata.profile_id, metadata.orientation, metadata.camera_order);
    for (std::size_t i = 0; i < 2; ++i) {
        if (i) out += ',';
        const auto& camera = (*result.mapping)[i];
        out += CameraProjection(camera.camera, camera.intrinsics, camera.distortion, camera.document_to_image);
    }
    out += "},\"calibration\":{\"method\":\"chart-fiducial-planar\",\"chartId\":" + Q(session.ChartId())
        + ",\"provenanceId\":" + Q(metadata.provenance_id) + ",\"measuredAt\":" + Q(metadata.measured_at_utc)
        + ",\"tool\":{\"toolId\":" + Q(kToolId) + ",\"version\":" + Q(kToolVersion) + "},\"inputSha256\":{\"CAM-A\":"
        + Q((*result.mapping)[0].reference_image_sha256) + ",\"CAM-B\":" + Q((*result.mapping)[1].reference_image_sha256)
        + "},\"fitResidualPixels\":{\"CAM-A\":" + Residual(result.cameras[0].residual, false)
        + ",\"CAM-B\":" + Residual(result.cameras[1].residual, false) + "},\"rigMeasurements\":" + Measurements(metadata.rig)
        + "},\"outputRaster\":null,\"correctionEnvelope\":null,\"qualityContract\":null,\"approval\":null}";
    return out;
}

std::string PolicyJson(const ReferenceMappingPolicy& policy) {
    const auto& p = policy.lens;
    return "{\"minimumViews\":" + std::to_string(p.minimum_views) + ",\"relativeSingularCutoff\":" + N(p.relative_singular_cutoff)
        + ",\"observationModel\":" + Q(ObservationModelCode(p.observation_model)) + ",\"maxIterations\":" + std::to_string(p.max_iterations)
        + ",\"relativeCostTolerance\":" + N(p.relative_cost_tolerance) + ",\"relativeStepTolerance\":" + N(p.relative_step_tolerance)
        + ",\"initialDamping\":" + N(p.initial_damping) + ",\"maxUndistortionIterations\":" + std::to_string(policy.max_undistortion_iterations)
        + ",\"undistortionTolerancePixels\":" + N(policy.undistortion_tolerance_pixels) + '}';
}
std::string LensJson(const CameraLensResult& lens) {
    std::string out = "{\"status\":" + Q(LensStatusCode(lens.status)) + ",\"initializationStatus\":" + Q(IntrinsicsStatusCode(lens.initialization.status))
        + ",\"intrinsics\":" + (lens.intrinsics ? K(*lens.intrinsics) : "null") + ",\"distortion\":" + (lens.distortion ? D(*lens.distortion) : "null")
        + ",\"solver\":" + Solver(lens.solver) + ",\"trainingResidual\":" + Residual(lens.training_residual)
        + ",\"heldOutCoverage\":" + Q(HeldOutCoverageCode(lens.held_out_coverage)) + ",\"heldOutResidual\":" + Residual(lens.held_out_residual);
    out += ",\"viewGeometry\":";
    const auto& geometry = lens.initialization.view_geometry;
    if (!geometry) out += "null";
    else out += "{\"status\":" + Q(StatusCode(geometry->status)) + ",\"viewCount\":" + std::to_string(geometry->view_count)
        + ",\"singularValues\":" + (geometry->singular_values ? Numbers(*geometry->singular_values) : "null")
        + ",\"numericalRank\":" + (geometry->numerical_rank ? std::to_string(*geometry->numerical_rank) : "null")
        + ",\"svdSweeps\":" + std::to_string(geometry->svd_sweeps) + '}';
    out += ",\"distortionCheck\":";
    if (!lens.distortion_check) out += "null";
    else {
        const auto& check = *lens.distortion_check;
        out += "{\"maxRadiusSquared\":" + N(check.max_radius_squared) + ",\"minimumRadialSlope\":" + N(check.minimum_radial_slope)
            + ",\"minimumJacobianDeterminant\":" + N(check.minimum_jacobian_determinant) + '}';
    }
    out += ",\"views\":[";
    for (std::size_t i = 0; i < lens.views.size(); ++i) {
        if (i) out += ',';
        const auto& view = lens.views[i];
        out += "{\"viewId\":" + Q(view.view_id) + ",\"role\":" + Q(ViewRoleCode(view.role)) + ",\"outcome\":" + Q(LensViewOutcomeCode(view.outcome))
            + ",\"pose\":" + Pose(view.pose) + ",\"residual\":" + Residual(view.residual) + ",\"solver\":" + Solver(view.solver) + '}';
    }
    out += "],\"initializationViews\":[";
    for (std::size_t i = 0; i < lens.initialization.views.size(); ++i) {
        if (i) out += ',';
        const auto& view = lens.initialization.views[i];
        out += "{\"viewId\":" + Q(view.view_id) + ",\"outcome\":" + Q(ViewOutcomeCode(view.outcome))
            + ",\"chartToImage\":" + (view.chart_to_image ? Numbers(*view.chart_to_image) : "null")
            + ",\"pose\":" + Pose(view.pose) + ",\"residual\":" + Residual(view.residual) + '}';
    }
    return out + "]}";
}

// Job parser: bounded before the shared JSON parser and exact keys at every
// object. No caller-controlled status/approval or automatic numerical defaults.
[[noreturn]] void InvalidJob() { throw std::invalid_argument("draft calibration: invalid job"); }
struct Failure { [[noreturn]] static void Fail(std::string_view, std::string_view) { InvalidJob(); } };
void Keys(const JsonValue& value, std::initializer_list<const char*> names) {
    if (value.kind != JsonKind::object || value.object.size() != names.size()) InvalidJob();
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
std::optional<double> Measurement(const JsonValue& value) { return value.kind == JsonKind::null_value ? std::nullopt : std::optional<double>{Number(value)}; }
std::array<std::optional<double>, 2> MeasurementPair(const JsonValue& value) {
    Keys(value, {"CAM-A", "CAM-B"});
    return {Measurement(At(value, "CAM-A")), Measurement(At(value, "CAM-B"))};
}
} // namespace

DraftCalibrationArtifacts BuildDraftCalibrationArtifacts(std::string_view json, const ReferencePairRequest& request,
    const ReferenceMappingPolicy& policy, const DraftCalibrationMetadata& metadata) {
    const auto session = CalibrationObservationSession::Parse(json);
    DraftCalibrationArtifacts out;
    std::string rejection;
    std::string profile_validation_failure;
    std::optional<ReferenceDocumentMappingResult> result;
    if (session.Camera(CameraAlias::CamA).width_pixels != 7360 || session.Camera(CameraAlias::CamA).height_pixels != 4912
        || session.Camera(CameraAlias::CamB).width_pixels != 7360 || session.Camera(CameraAlias::CamB).height_pixels != 4912) {
        rejection = "unsupported-image-dimensions";
    } else {
        result = EstimateReferenceDocumentMapping(session, request, policy);
        bool validation_evaluated = result->lens.has_value();
        if (result->lens) for (const auto& camera : result->lens->cameras) {
            validation_evaluated = validation_evaluated && camera.held_out_coverage == HeldOutCoverage::AllViewsEvaluated
                && camera.held_out_residual && camera.held_out_residual->point_count > 0;
        }
        if (!result->mapping) rejection = "reference-mapping-unavailable";
        else if (!validation_evaluated) rejection = "validation-not-evaluated";
        else {
            try {
                auto profile = Profile(session, *result, metadata);
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
    std::string report = "{\"schemaVersion\":" + Q(kDraftCalibrationReportSchema) + ",\"result\":" + Q(out.ok ? "draft-produced" : "no-profile")
        + ",\"rejection\":" + (rejection.empty() ? "null" : Q(rejection)) + ",\"provenanceId\":" + Q(metadata.provenance_id)
        + ",\"profileValidationFailure\":" + (profile_validation_failure.empty() ? "null" : Q(profile_validation_failure))
        + ",\"profileId\":" + Q(metadata.profile_id) + ",\"measuredAt\":" + Q(metadata.measured_at_utc)
        + ",\"tool\":{\"toolId\":" + Q(kToolId) + ",\"version\":" + Q(kToolVersion) + '}'
        + ",\"profileFingerprintSha256\":" + (out.ok ? Q(out.profile_fingerprint_sha256) : "null")
        + ",\"sessionSha256\":" + Q(Sha256(json)) + ",\"session\":" + std::string(json)
        + ",\"numericalPolicy\":" + PolicyJson(policy) + ",\"referencePair\":{\"CAM-A\":" + Q(request.view_ids[0]) + ",\"CAM-B\":" + Q(request.view_ids[1])
        + "},\"referencePlacement\":";
    if (!request.placement) report += "null";
    else {
        const auto& p = *request.placement;
        // Invalid/nonfinite placement values are diagnostics, never JSON NaN.
        const auto finite = [](double value) { return std::isfinite(value) ? N(value) : "null"; };
        report += "{\"angleRadians\":" + finite(p.angle_radians) + ",\"translationXMm\":" + finite(p.translation_x_mm)
            + ",\"translationYMm\":" + finite(p.translation_y_mm) + ",\"coplanarConfirmed\":" + (p.coplanar_confirmed ? "true" : "false") + '}';
    }
    std::string plane = "null", measurements = "null";
    try { plane = Plane(metadata); measurements = Measurements(metadata.rig); } catch (const std::invalid_argument&) {}
    report += ",\"documentPlane\":" + plane + ",\"rigMeasurements\":" + measurements
        + ",\"fitResidualScope\":\"reference usedPointIds, raw camera pixels\",\"quality\":\"not-assessed\",\"cameras\":[";
    for (std::size_t i = 0; i < 2; ++i) {
        if (i) report += ',';
        report += "{\"camera\":" + Q(i == 0 ? "CAM-A" : "CAM-B") + ",\"reference\":";
        if (!result) report += "null";
        else {
            const auto& reference = result->cameras[i];
            report += "{\"viewId\":" + Q(reference.view_id) + ",\"outcome\":" + Q(ReferenceMappingOutcomeCode(reference.outcome))
                + ",\"pose\":" + Pose(reference.pose) + ",\"residual\":" + Residual(reference.residual) + ",\"solver\":" + Solver(reference.solver) + '}';
        }
        report += ",\"lens\":" + (result && result->lens ? LensJson(result->lens->cameras[i]) : "null") + '}';
    }
    out.report_json = report + "]}";
    return out;
}

DraftCalibrationArtifacts RunDraftCalibrationJob(std::string_view session, std::string_view job_json) {
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
    Keys(job, {"schemaVersion", "profileId", "provenanceId", "measuredAt", "documentPlane", "referencePair", "referencePlacement", "numericalPolicy", "rigMeasurements"});
    if (Text(At(job, "schemaVersion")) != kDraftCalibrationJobSchema) InvalidJob();
    const auto& plane = At(job, "documentPlane"); Keys(plane, {"orientation", "cameraOrder"});
    DraftCalibrationMetadata metadata;
    metadata.profile_id = Text(At(job, "profileId")); metadata.provenance_id = Text(At(job, "provenanceId")); metadata.measured_at_utc = Text(At(job, "measuredAt"));
    const auto orientation = Text(At(plane, "orientation")), order = Text(At(plane, "cameraOrder"));
    if (orientation != "landscape" && orientation != "portrait") InvalidJob();
    if (order != "CAM-A-left-CAM-B-right" && order != "CAM-A-top-CAM-B-bottom") InvalidJob();
    metadata.orientation = orientation == "landscape" ? DraftPaperOrientation::Landscape : DraftPaperOrientation::Portrait;
    metadata.camera_order = order == "CAM-A-left-CAM-B-right" ? DraftCameraOrder::LeftRight : DraftCameraOrder::TopBottom;
    const auto& pair = At(job, "referencePair"); Keys(pair, {"CAM-A", "CAM-B"});
    ReferencePairRequest request{{Text(At(pair, "CAM-A")), Text(At(pair, "CAM-B"))}, {}};
    const auto& placement = At(job, "referencePlacement");
    if (placement.kind != JsonKind::null_value) {
        Keys(placement, {"angleRadians", "translationXMm", "translationYMm", "coplanarConfirmed"});
        if (At(placement, "coplanarConfirmed").kind != JsonKind::boolean) InvalidJob();
        request.placement = DocumentPlanePlacement{Number(At(placement, "angleRadians")), Number(At(placement, "translationXMm")),
            Number(At(placement, "translationYMm")), At(placement, "coplanarConfirmed").boolean};
    }
    const auto& p = At(job, "numericalPolicy");
    Keys(p, {"minimumViews", "relativeSingularCutoff", "maxIterations", "relativeCostTolerance", "relativeStepTolerance", "initialDamping", "maxUndistortionIterations", "undistortionTolerancePixels"});
    const ReferenceMappingPolicy policy{{Integer(At(p, "minimumViews")), Number(At(p, "relativeSingularCutoff")), ObservationModel::DistortedAnalytic,
        Integer(At(p, "maxIterations")), Number(At(p, "relativeCostTolerance")), Number(At(p, "relativeStepTolerance")), Number(At(p, "initialDamping"))},
        Integer(At(p, "maxUndistortionIterations")), Number(At(p, "undistortionTolerancePixels"))};
    const auto& rig = At(job, "rigMeasurements"); Keys(rig, {"baselineMm", "overlapMm", "cameraToDocumentMm", "lensFocalLengthMm"});
    metadata.rig = {Measurement(At(rig, "baselineMm")), Measurement(At(rig, "overlapMm")), MeasurementPair(At(rig, "cameraToDocumentMm")), MeasurementPair(At(rig, "lensFocalLengthMm"))};
    return BuildDraftCalibrationArtifacts(session, request, policy, metadata);
}
} // namespace a0::m2::calibration
