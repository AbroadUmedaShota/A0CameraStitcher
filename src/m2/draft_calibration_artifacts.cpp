#include "a0/m2/draft_calibration_artifacts.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/common/protocol_json.hpp"

#include <Windows.h>
#include <bcrypt.h>

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

std::string Q(std::string_view value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
std::string N(double value) {
    if (!std::isfinite(value)) throw std::invalid_argument("draft calibration: non-finite number");
    if (value == 0.0) return "0";
    char buffer[64];
    const auto encoded = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (encoded.ec != std::errc{}) throw std::invalid_argument("draft calibration: number encoding");
    return std::string(buffer, encoded.ptr);
}
std::string OptionalNumber(const std::optional<double>& value) { return value ? N(*value) : "null"; }
template<std::size_t Size> std::string Numbers(const std::array<double, Size>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < Size; ++i) { if (i) out += ','; out += N(values[i]); }
    return out + ']';
}
std::string Residual(const std::optional<ResidualSummary>& value, bool count = true) {
    if (!value) return "null";
    std::string out = "{\"rms\":" + N(value->rms_pixels) + ",\"max\":" + N(value->max_pixels);
    if (count) out += ",\"pointCount\":" + std::to_string(value->point_count);
    return out + '}';
}
std::string Solver(const std::optional<SolverRecord>& value) {
    if (!value) return "null";
    return "{\"iterations\":" + std::to_string(value->iterations) + ",\"acceptedSteps\":" + std::to_string(value->accepted_steps)
        + ",\"initialCost\":" + N(value->initial_cost) + ",\"finalCost\":" + N(value->final_cost)
        + ",\"stopReason\":" + Q(SolverStopReasonCode(value->stop_reason)) + '}';
}
std::string Pose(const std::optional<ChartPose>& value) {
    if (!value) return "null";
    return "{\"rotation\":" + Numbers(value->rotation) + ",\"translationMm\":" + Numbers(value->translation_mm) + '}';
}
std::string K(const PinholeIntrinsics& value) {
    return "{\"fxPixels\":" + N(value.fx) + ",\"fyPixels\":" + N(value.fy)
        + ",\"cxPixels\":" + N(value.cx) + ",\"cyPixels\":" + N(value.cy) + '}';
}
std::string D(const BrownConradyDistortion& value) {
    return "{\"model\":\"brown-conrady-k1k2k3-p1p2\",\"k1\":" + N(value.k1) + ",\"k2\":" + N(value.k2)
        + ",\"k3\":" + N(value.k3) + ",\"p1\":" + N(value.p1) + ",\"p2\":" + N(value.p2) + '}';
}
std::string PerCameraMeasurements(const std::array<std::optional<double>, 2>& values) {
    return "{\"CAM-A\":" + OptionalNumber(values[0]) + ",\"CAM-B\":" + OptionalNumber(values[1]) + '}';
}
std::string Measurements(const CalibrationRigMeasurements& rig) {
    return "{\"baselineMm\":" + OptionalNumber(rig.baseline_mm) + ",\"overlapMm\":" + OptionalNumber(rig.overlap_mm)
        + ",\"cameraToDocumentMm\":" + PerCameraMeasurements(rig.camera_to_document_mm)
        + ",\"lensFocalLengthMm\":" + PerCameraMeasurements(rig.lens_focal_length_mm) + '}';
}
std::string Plane(const DraftCalibrationMetadata& metadata) {
    if (metadata.orientation != DraftPaperOrientation::Landscape && metadata.orientation != DraftPaperOrientation::Portrait)
        throw std::invalid_argument("draft calibration: orientation");
    if (metadata.camera_order != DraftCameraOrder::LeftRight && metadata.camera_order != DraftCameraOrder::TopBottom)
        throw std::invalid_argument("draft calibration: camera order");
    const bool landscape = metadata.orientation == DraftPaperOrientation::Landscape;
    return "{\"paper\":\"ISO-216-A0\",\"orientation\":" + Q(landscape ? "landscape" : "portrait")
        + ",\"sheetWidthMm\":" + (landscape ? "1189" : "841") + ",\"sheetHeightMm\":" + (landscape ? "841" : "1189")
        + ",\"cameraOrder\":" + Q(metadata.camera_order == DraftCameraOrder::LeftRight ? "CAM-A-left-CAM-B-right" : "CAM-A-top-CAM-B-bottom") + '}';
}
std::string Profile(const CalibrationObservationSession& session, const ReferenceDocumentMappingResult& result,
    const DraftCalibrationMetadata& metadata) {
    std::string out = "{\"schemaVersion\":\"2.0.0\",\"status\":\"draft\",\"profileId\":" + Q(metadata.profile_id)
        + ",\"conventions\":{\"documentPlane\":\"mm-origin-sheet-top-left-x-right-y-down\",\"cameraPixel\":\"stored-order-sample-at-integer-index\","
          "\"matrix\":\"row-major-column-vector\",\"exifOrientation\":\"ignored\"},"
          "\"cameraModel\":{\"manufacturer\":\"Nikon\",\"model\":\"D810\",\"sensorWidthPixels\":7360,\"sensorHeightPixels\":4912},\"documentPlane\":"
        + Plane(metadata) + ",\"cameras\":{";
    for (std::size_t i = 0; i < 2; ++i) {
        if (i) out += ',';
        const auto& camera = (*result.mapping)[i];
        const auto& h = camera.document_to_image;
        out += Q(CameraAliasCode(camera.camera)) + ":{\"physicalIdentity\":null,\"projection\":{\"intrinsics\":" + K(camera.intrinsics)
            + ",\"distortion\":" + D(camera.distortion) + ",\"documentToImage\":["
            + Numbers(std::array<double, 3>{h[0], h[1], h[2]}) + ',' + Numbers(std::array<double, 3>{h[3], h[4], h[5]})
            + ',' + Numbers(std::array<double, 3>{h[6], h[7], h[8]}) + "]}}";
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

std::string Sha256(std::string_view bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("draft calibration: SHA-256 provider");
    struct AlgGuard { BCRYPT_ALG_HANDLE h; ~AlgGuard() { BCryptCloseAlgorithmProvider(h, 0); } } alg{algorithm};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("draft calibration: SHA-256 create");
    struct HashGuard { BCRYPT_HASH_HANDLE h; ~HashGuard() { BCryptDestroyHash(h); } } guard{hash};
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) < 0)
        throw std::runtime_error("draft calibration: SHA-256 data");
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw std::runtime_error("draft calibration: SHA-256 finish");
    std::string out;
    constexpr char hex[] = "0123456789abcdef";
    for (const auto c : digest) { out += hex[c >> 4]; out += hex[c & 15]; }
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
