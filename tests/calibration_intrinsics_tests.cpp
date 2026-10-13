#include "a0/m2/calibration_intrinsics.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Contract test of the closed-form undistorted intrinsics (Issue #289).
// Every session is built here from numbers: the true intrinsics and pose of
// each view project the chart points, and only the resulting pixels reach the
// estimator. No production Jacobi, DLT, Zhang row or pose code is used, no
// file is read or written, and no generator, renderer or profile is involved.
// MSVC maps long double to double, so the fixture is binary64 throughout.
namespace {
using namespace a0::m2::calibration;

int checks = 0, failures = 0;
void Check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
template <class F> void Rejects(F action, const std::string& label) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, label);
}

// ---------------------------------------------------------------------------
// Fixture geometry.

constexpr std::uint32_t kWidth = 1280, kHeight = 960;
using Vec3 = std::array<double, 3>;
struct Intrinsics { double fx, fy, cx, cy; };
constexpr Intrinsics kTrue{1000, 1040, 651.8, 470.8};
constexpr Intrinsics kOtherB{1450, 760, 651.8, 470.8};
constexpr Intrinsics kOtherC{780, 1500, 651.8, 470.8};
struct Pose { std::array<double, 9> r; Vec3 t; };

// Apply three independent axis rotations to a basis vector (same order as the
// view geometry test), so the rotation columns are the rotated basis.
Vec3 RotateBasis(Vec3 p, double x, double y, double z) {
    const Vec3 after_x{p[0], std::cos(x) * p[1] - std::sin(x) * p[2], std::sin(x) * p[1] + std::cos(x) * p[2]};
    const Vec3 after_y{std::cos(y) * after_x[0] + std::sin(y) * after_x[2], after_x[1],
        -std::sin(y) * after_x[0] + std::cos(y) * after_x[2]};
    return {std::cos(z) * after_y[0] - std::sin(z) * after_y[1], std::sin(z) * after_y[0] + std::cos(z) * after_y[1], after_y[2]};
}
Pose MakePose(double ax, double ay, double az, double tx, double ty, double tz) {
    const auto c0 = RotateBasis({1, 0, 0}, ax, ay, az), c1 = RotateBasis({0, 1, 0}, ax, ay, az), c2 = RotateBasis({0, 0, 1}, ax, ay, az);
    return {{c0[0], c1[0], c2[0], c0[1], c1[1], c2[1], c0[2], c1[2], c2[2]}, {tx, ty, tz}};
}
struct Projected { double u, v, z; };
Projected Project(const Intrinsics& k, const std::array<double, 9>& r, const Vec3& t, double x, double y, double z = 0) {
    const double px = r[0] * x + r[1] * y + r[2] * z + t[0];
    const double py = r[3] * x + r[4] * y + r[5] * z + t[1];
    const double pz = r[6] * x + r[7] * y + r[8] * z + t[2];
    return {k.fx * px / pz + k.cx, k.fy * py / pz + k.cy, pz};
}
Projected ProjectTruth(const Intrinsics& k, const Pose& pose, double x, double y, double z = 0) {
    return Project(k, pose.r, pose.t, x, y, z);
}
Projected ProjectEstimate(const PinholeIntrinsics& k, const ChartPose& pose, double x, double y, double z = 0) {
    return Project({k.fx, k.fy, k.cx, k.cy}, pose.rotation, pose.translation_mm, x, y, z);
}

struct ChartPt { std::string id; double x, y; };
std::vector<ChartPt> Grid(int rows = 9, int columns = 13, double pitch = 20, double x0 = -120, double y0 = -80) {
    std::vector<ChartPt> points;
    for (int r = 0; r < rows; ++r) for (int c = 0; c < columns; ++c) {
        points.push_back({"P" + std::to_string(r) + "_" + std::to_string(c), x0 + pitch * c, y0 + pitch * r});
    }
    return points;
}
// Points that never overlap the grid; the lower two of each column (six in all) lie behind
// the camera for the plane used in test P1.
std::vector<ChartPt> BehindPoints() {
    std::vector<ChartPt> points;
    int index = 0;
    for (const double x : {-50.0, 0.0, 50.0}) for (const double y : {100.0, 200.0, 300.0, 400.0, -100.0, -200.0}) {
        points.push_back({"Q" + std::to_string(index++), x, y});
    }
    return points;
}
std::vector<std::string> Ids(const std::vector<ChartPt>& points, std::size_t begin = 0, std::size_t end = std::numeric_limits<std::size_t>::max()) {
    std::vector<std::string> ids;
    for (std::size_t i = begin; i < points.size() && i < end; ++i) ids.push_back(points[i].id);
    return ids;
}

// ---------------------------------------------------------------------------
// Session builder.

std::string Num(double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), result.ptr);
}
std::string Quote(const std::string& text) { return "\"" + text + "\""; }
std::string Hash(unsigned number) {
    char tail[16];
    std::snprintf(tail, sizeof tail, "%08x", number);
    return std::string(56, '0') + tail;
}
std::string Join(const std::vector<std::string>& items) {
    std::string text;
    for (std::size_t index = 0; index < items.size(); ++index) text += (index ? "," : "") + items[index];
    return text;
}
std::string IdList(const std::vector<std::string>& ids) {
    std::vector<std::string> quoted;
    for (const auto& id : ids) quoted.push_back(Quote(id));
    return "[" + Join(quoted) + "]";
}

struct ViewSpec {
    std::string id;
    std::string camera = "CAM-A";
    std::string role = "training";
    Intrinsics k = kTrue;
    Pose pose{};
    double y_scale = 1;                // the pixel is the projection of (x, y_scale * y); the session says (x, y)
    std::vector<ChartPt> observed;     // detected observations, in order
    std::vector<std::string> used;     // usedPointIds, or posePointIds for a validation view
    std::vector<std::string> measurement;
    std::map<std::string, std::pair<double, double>> delta; // added to the pixel of the named point
    double shift_x = 0;                // added to the x of every pixel
    bool allow_behind = false;         // some observed points may have non-positive depth
};

std::string RenderView(const ViewSpec& view, unsigned hash_number) {
    std::vector<std::string> observations;
    bool inside = true;
    for (const auto& point : view.observed) {
        const auto projected = ProjectTruth(view.k, view.pose, point.x, view.y_scale * point.y);
        double u = projected.u + view.shift_x, v = projected.v;
        const auto found = view.delta.find(point.id);
        if (found != view.delta.end()) { u += found->second.first; v += found->second.second; }
        inside = inside && u >= -0.5 && u <= kWidth - 0.5 && v >= -0.5 && v <= kHeight - 0.5
            && (view.allow_behind || projected.z > 0);
        observations.push_back("{\"pointId\":" + Quote(point.id) + ",\"state\":\"detected\",\"pixel\":{\"x\":" + Num(u)
            + ",\"y\":" + Num(v) + "}}");
    }
    Check(inside, "fixture: every projected pixel is inside the image: " + view.id);
    std::string text = "{\"viewId\":" + Quote(view.id) + ",\"camera\":" + Quote(view.camera) + ",\"imageSha256\":" + Quote(Hash(hash_number))
        + ",\"widthPixels\":" + std::to_string(kWidth) + ",\"heightPixels\":" + std::to_string(kHeight) + ",\"role\":" + Quote(view.role) + ",";
    if (view.role == "validation") {
        text += "\"posePointIds\":" + IdList(view.used) + ",\"measurementPointIds\":" + IdList(view.measurement) + ",";
    } else {
        text += "\"usedPointIds\":" + IdList(view.used) + ",";
    }
    return text + "\"observations\":[" + Join(observations) + "]}";
}
std::string CameraJson(const std::string& focus) {
    return "{\"widthPixels\":" + std::to_string(kWidth) + ",\"heightPixels\":" + std::to_string(kHeight)
        + ",\"lensLabel\":\"synthetic-lens\",\"focalLengthLabel\":\"f50mm\",\"focusLabel\":" + Quote(focus) + "}";
}
std::string BuildJson(const std::vector<ChartPt>& chart, const std::vector<ViewSpec>& views) {
    std::vector<std::string> chart_items;
    for (const auto& point : chart) {
        chart_items.push_back("{\"pointId\":" + Quote(point.id) + ",\"xMm\":" + Num(point.x) + ",\"yMm\":" + Num(point.y) + "}");
    }
    std::vector<std::string> view_items;
    unsigned hash_number = 1;
    for (const auto& view : views) view_items.push_back(RenderView(view, hash_number++));
    return "{\"schemaVersion\":\"a0.calibration-observation-session.v1\",\"sessionId\":\"synthetic-session\","
        "\"chartId\":\"synthetic-chart\",\"provenance\":{\"measuredAt\":\"2026-01-01T00:00:00Z\","
        "\"tool\":{\"toolId\":\"synthetic-fixture\",\"version\":\"1.0.0\"}},\"chartPoints\":[" + Join(chart_items)
        + "],\"cameras\":{\"CAM-A\":" + CameraJson("fixed-a") + ",\"CAM-B\":" + CameraJson("fixed-b")
        + "},\"views\":[" + Join(view_items) + "]}";
}
CalibrationObservationSession MakeSession(const std::vector<ChartPt>& chart, const std::vector<ViewSpec>& views) {
    return CalibrationObservationSession::Parse(BuildJson(chart, views));
}

// ---------------------------------------------------------------------------
// Views of the test plan.

const Pose kPoseT1 = MakePose(0.30, 0.10, 0.05, 10, -5, 700);
const Pose kPoseT2 = MakePose(-0.20, 0.35, -0.08, -15, 10, 750);
const Pose kPoseT3 = MakePose(0.25, -0.30, 0.12, 5, 8, 680);
const Pose kPoseV1 = MakePose(0.15, 0.20, -0.10, 0, 0, 720);

ViewSpec FullView(std::string id, const Pose& pose, Intrinsics k = kTrue, std::string camera = "CAM-A",
    std::string role = "training", const std::vector<ChartPt>& chart = Grid()) {
    ViewSpec view;
    view.id = std::move(id);
    view.camera = std::move(camera);
    view.role = std::move(role);
    view.k = k;
    view.pose = pose;
    view.observed = chart;
    view.used = Ids(chart);
    return view;
}
ViewSpec T1() {
    auto view = FullView("T1", kPoseT1);
    view.used = Ids(view.observed, 0, 112); // the last five stay detected but unused
    return view;
}
ViewSpec T2() { return FullView("T2", kPoseT2); }
ViewSpec T3() { return FullView("T3", kPoseT3); }
ViewSpec V1() {
    auto view = FullView("V1", kPoseV1, kTrue, "CAM-A", "validation");
    view.used.clear();
    for (std::size_t i = 0; i < view.observed.size(); ++i) (i % 2 == 0 ? view.used : view.measurement).push_back(view.observed[i].id);
    return view;
}
ViewSpec R1() { return FullView("R1", MakePose(0, 0, 0, 0, 0, 700), kTrue, "CAM-A", "reference"); }
std::vector<ViewSpec> G3Views() { return {T1(), T2(), T3(), V1(), R1()}; }
const std::array<Vec3, 3> kFrontalTranslations{{{0, 0, 700}, {30, -20, 650}, {-25, 15, 800}}};
std::vector<ViewSpec> Frontal(double ax, double ay, double az, const std::string& prefix, const std::string& camera = "CAM-A") {
    std::vector<ViewSpec> views;
    int index = 1;
    for (const auto& t : kFrontalTranslations) {
        views.push_back(FullView(prefix + std::to_string(index++), MakePose(ax, ay, az, t[0], t[1], t[2]), kTrue, camera));
    }
    return views;
}
ViewSpec KB(const std::string& camera = "CAM-A", const std::string& id = "KB") {
    return FullView(id, MakePose(-0.32, -0.14, 0.18, -10, -5, 700), kOtherB, camera);
}
ViewSpec KC(const std::string& camera = "CAM-A", const std::string& id = "KC") {
    return FullView(id, MakePose(0.11, 0.35, -0.20, -12, 6, 760), kOtherC, camera);
}

UndistortedIntrinsicsPolicy Policy(std::uint32_t minimum = 2, double cutoff = 1e-4) {
    return {minimum, cutoff, ObservationModel::UndistortedAnalytic};
}
UndistortedIntrinsicsResult Run(const std::vector<ChartPt>& chart, const std::vector<ViewSpec>& views,
    const UndistortedIntrinsicsPolicy& policy = Policy()) {
    return EstimateUndistortedIntrinsics(MakeSession(chart, views), policy);
}
UndistortedIntrinsicsResult Run(const std::vector<ViewSpec>& views, const UndistortedIntrinsicsPolicy& policy = Policy()) {
    return Run(Grid(), views, policy);
}

// ---------------------------------------------------------------------------
// Fingerprint: every double as its 16 hex digits, with the presence of every
// optional, the enums and the identifiers. Equal fingerprints mean bitwise equal.

void PutDouble(std::string& out, double value) {
    char text[20];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(value)));
    out += text;
    out += ' ';
}
template <std::size_t N> void PutArray(std::string& out, const std::array<double, N>& values) {
    for (const auto value : values) PutDouble(out, value);
}
void PutResidual(std::string& out, const std::optional<ResidualSummary>& residual) {
    if (!residual) { out += "residual:- "; return; }
    out += "residual:";
    PutDouble(out, residual->rms_pixels);
    PutDouble(out, residual->max_pixels);
    out += std::to_string(residual->point_count) + ' ';
}
void PutGeometry(std::string& out, const std::optional<ViewGeometryAssessment>& geometry) {
    if (!geometry) { out += "geometry:- "; return; }
    out += "geometry:" + std::string(StatusCode(geometry->status)) + ' ' + std::to_string(geometry->view_count) + ' '
        + std::to_string(geometry->svd_sweeps) + ' ';
    if (geometry->singular_values) PutArray(out, *geometry->singular_values); else out += "sv:- ";
    out += geometry->numerical_rank ? "rank:" + std::to_string(*geometry->numerical_rank) + ' ' : "rank:- ";
    if (geometry->conditioned_conic) {
        PutArray(out, geometry->conditioned_conic->b);
        PutDouble(out, geometry->conditioned_conic->center_x);
        PutDouble(out, geometry->conditioned_conic->center_y);
        PutDouble(out, geometry->conditioned_conic->scale);
    } else {
        out += "conic:- ";
    }
}
void PutView(std::string& out, const IntrinsicsViewResult& view) {
    out += "view:" + view.view_id + ' ' + std::string(ViewRoleCode(view.role)) + ' ' + std::string(ViewOutcomeCode(view.outcome)) + ' ';
    if (view.chart_to_image) PutArray(out, *view.chart_to_image); else out += "h:- ";
    if (view.pose) { PutArray(out, view.pose->rotation); PutArray(out, view.pose->translation_mm); } else out += "pose:- ";
    PutResidual(out, view.residual);
}
// training_only leaves out the validation views and the held-out part.
std::string Fingerprint(const CameraIntrinsicsResult& camera, bool training_only = false) {
    std::string out = std::string(CameraAliasCode(camera.camera)) + ' ' + std::string(IntrinsicsStatusCode(camera.status)) + ' ';
    PutGeometry(out, camera.view_geometry);
    if (camera.intrinsics) {
        PutDouble(out, camera.intrinsics->fx);
        PutDouble(out, camera.intrinsics->fy);
        PutDouble(out, camera.intrinsics->cx);
        PutDouble(out, camera.intrinsics->cy);
    } else {
        out += "k:- ";
    }
    for (const auto& view : camera.views) {
        if (training_only && view.role != ViewRole::Training) continue;
        PutView(out, view);
    }
    PutResidual(out, camera.training_residual);
    if (!training_only) {
        out += std::string(HeldOutCoverageCode(camera.held_out_coverage)) + ' ';
        PutResidual(out, camera.held_out_residual);
    }
    return out;
}
std::string Fingerprint(const UndistortedIntrinsicsResult& result) {
    return std::string(ObservationModelCode(result.observation_model)) + '|' + Fingerprint(result.cameras[0]) + '|' + Fingerprint(result.cameras[1]);
}

// ---------------------------------------------------------------------------
// Invariants that hold for every result.

const IntrinsicsViewResult* FindView(const CameraIntrinsicsResult& camera, const std::string& id) {
    for (const auto& view : camera.views) if (view.view_id == id) return &view;
    return nullptr;
}
bool ValidSummary(const std::optional<ResidualSummary>& summary) {
    if (!summary) return true;
    return summary->point_count >= 1 && std::isfinite(summary->rms_pixels) && std::isfinite(summary->max_pixels)
        && summary->rms_pixels >= 0 && summary->max_pixels >= 0
        && summary->rms_pixels <= summary->max_pixels * (1 + 1e-12) + 1e-300;
}
// Review additions: shape checks that hold for every pose and homography.
bool IsRotation(const std::array<double, 9>& r) {
    for (std::size_t i = 0; i < 3; ++i) for (std::size_t j = 0; j < 3; ++j) {
        double dot = 0;
        for (std::size_t k = 0; k < 3; ++k) dot += r[k * 3 + i] * r[k * 3 + j];
        if (!(std::abs(dot - (i == j ? 1.0 : 0.0)) <= 1e-9)) return false;
    }
    const double det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) + r[2] * (r[3] * r[7] - r[4] * r[6]);
    return std::abs(det - 1) <= 1e-9;
}
double FrobeniusNorm(const std::array<double, 9>& h) {
    double norm = 0;
    for (const auto value : h) norm = std::hypot(norm, value);
    return norm;
}
void Invariants(const UndistortedIntrinsicsResult& result, const std::string& label) {
    Check(result.cameras[0].camera == CameraAlias::CamA && result.cameras[1].camera == CameraAlias::CamB,
        label + ": cameras are [CAM-A, CAM-B]");
    for (const auto& camera : result.cameras) {
        const auto name = label + "/" + std::string(CameraAliasCode(camera.camera));
        const bool estimated = camera.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated;
        Check(!IntrinsicsStatusCode(camera.status).empty(), name + ": status has a code");
        Check(FailedStage(camera.status).has_value() == !estimated, name + ": stage is null exactly for the success status");
        Check(camera.view_geometry.has_value() == (camera.status != IntrinsicsStatus::HomographyUnavailable),
            name + ": view geometry is null exactly when the homography stage failed");
        const bool sufficient = camera.view_geometry && camera.view_geometry->status == ViewGeometryStatus::LinearConstraintsSufficient;
        Check((camera.view_geometry && camera.view_geometry->conditioned_conic.has_value()) == sufficient,
            name + ": the null vector exists exactly when the preflight is sufficient");
        Check(!camera.intrinsics || sufficient, name + ": intrinsics need a sufficient preflight");
        bool all_training_posed = true;
        for (const auto& view : camera.views) {
            Check(view.role == ViewRole::Training || view.role == ViewRole::Validation, name + ": reference views are not listed");
            Check(ValidSummary(view.residual), name + ": view residual is a valid summary: " + view.view_id);
            if (view.role == ViewRole::Training) all_training_posed = all_training_posed && view.pose.has_value();
            if (view.outcome == ViewOutcome::Evaluated) {
                Check(view.chart_to_image && view.pose && view.residual, name + ": an evaluated view has H, pose and residual: " + view.view_id);
            } else {
                Check(!view.pose && !view.residual, name + ": a view that is not evaluated has no pose or residual: " + view.view_id);
            }
            if (view.role == ViewRole::Validation && !estimated) {
                Check(view.outcome == ViewOutcome::NotReached && !view.chart_to_image, name + ": a validation view is not reached: " + view.view_id);
            }
        }
        Check(estimated == (camera.intrinsics && camera.training_residual && all_training_posed && !camera.views.empty()),
            name + ": success is exactly intrinsics, training residual and every training pose");
        Check(ValidSummary(camera.training_residual) && ValidSummary(camera.held_out_residual), name + ": pooled residuals are valid summaries");
        if (!estimated) {
            Check(camera.held_out_coverage == HeldOutCoverage::NotReached && !camera.held_out_residual && !camera.training_residual,
                name + ": a stopped camera has no pooled residual and no held-out result");
        } else {
            Check(camera.held_out_coverage != HeldOutCoverage::NotReached, name + ": a successful camera reports the held-out coverage");
            if (camera.held_out_coverage == HeldOutCoverage::AllViewsEvaluated) {
                Check(camera.held_out_residual.has_value(), name + ": all validation views evaluated means a held-out residual");
            }
            if (camera.held_out_coverage == HeldOutCoverage::NoValidationView) {
                Check(!camera.held_out_residual, name + ": no validation view means no held-out residual");
            }
        }
        if (camera.status == IntrinsicsStatus::PoseUnavailable) {
            Check(camera.intrinsics.has_value(), name + ": pose-unavailable keeps the intrinsics");
        }
        {   // Review additions: the status decides exactly which parts exist.
            const bool has_intrinsics = estimated || camera.status == IntrinsicsStatus::PoseUnavailable;
            Check(camera.intrinsics.has_value() == has_intrinsics, name + ": intrinsics exist exactly for the success and pose-unavailable statuses");
            bool any_gate_failure = false;
            for (const auto& view : camera.views) {
                if (!has_intrinsics) Check(!view.pose && !view.residual, name + ": nothing after the stopped stage: " + view.view_id);
                if (view.role == ViewRole::Training) {
                    if (camera.status != IntrinsicsStatus::HomographyUnavailable) {
                        Check(view.chart_to_image.has_value(), name + ": a training view keeps H once the homography stage passed: " + view.view_id);
                    }
                    if (estimated) Check(view.outcome == ViewOutcome::Evaluated, name + ": every training view is evaluated on success: " + view.view_id);
                    any_gate_failure = any_gate_failure || view.outcome == ViewOutcome::InsufficientObservations
                        || view.outcome == ViewOutcome::DegeneratePointConfiguration || view.outcome == ViewOutcome::NumericalFailure;
                }
                if (view.chart_to_image) Check(std::abs(FrobeniusNorm(*view.chart_to_image) - 1) <= 1e-12, name + ": H has Frobenius norm 1: " + view.view_id);
                if (view.pose) Check(IsRotation(view.pose->rotation), name + ": the pose rotation is orthonormal with determinant 1: " + view.view_id);
            }
            if (camera.status == IntrinsicsStatus::HomographyUnavailable) Check(any_gate_failure, name + ": the homography stage names the failing view");
            if (camera.view_geometry) {
                const auto geometry = camera.view_geometry->status;
                const bool past_geometry = estimated || camera.status == IntrinsicsStatus::PoseUnavailable
                    || camera.status == IntrinsicsStatus::NonphysicalIntrinsics;
                Check((geometry == ViewGeometryStatus::LinearConstraintsSufficient) == past_geometry, name + ": preflight is sufficient exactly when the camera passed it");
                if (geometry == ViewGeometryStatus::InsufficientViews) Check(camera.status == IntrinsicsStatus::InsufficientViews, name + ": status follows the preflight (views)");
                if (geometry == ViewGeometryStatus::InsufficientDiversity) Check(camera.status == IntrinsicsStatus::InsufficientViewDiversity, name + ": status follows the preflight (diversity)");
                if (geometry == ViewGeometryStatus::InconsistentLinearConstraints) Check(camera.status == IntrinsicsStatus::InconsistentLinearConstraints, name + ": status follows the preflight (inconsistent)");
                if (geometry == ViewGeometryStatus::NumericalFailure) Check(camera.status == IntrinsicsStatus::NumericalFailure, name + ": status follows the preflight (numerical)");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Independent probe points that are not in any session.

struct Probe { double x, y, z; };
std::vector<Probe> Probes() {
    std::vector<Probe> probes;
    for (int r = 0; r < 8; ++r) for (int c = 0; c < 12; ++c) probes.push_back({-110.0 + 20 * c, -70.0 + 20 * r, 0});
    for (const double z : {-40.0, 40.0}) for (const double x : {-100.0, 0.0, 100.0}) for (const double y : {-60.0, 0.0, 60.0}) {
        probes.push_back({x, y, z});
    }
    return probes;
}
// Largest pixel difference between the estimate and the truth over the probes.
double ProbeDifference(const CameraIntrinsicsResult& camera, const ViewSpec& truth) {
    const auto* view = FindView(camera, truth.id);
    if (!view || !view->pose || !camera.intrinsics) return std::numeric_limits<double>::infinity();
    double worst = 0;
    for (const auto& probe : Probes()) {
        const auto estimate = ProjectEstimate(*camera.intrinsics, *view->pose, probe.x, probe.y, probe.z);
        const auto exact = ProjectTruth(truth.k, truth.pose, probe.x, probe.y, probe.z);
        worst = std::max(worst, std::hypot(estimate.u - exact.u, estimate.v - exact.v));
    }
    return worst;
}

bool Bitwise(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }

// ---------------------------------------------------------------------------
// Tests.

void TestPolicyAndCodes() {
    const auto views = std::vector<ViewSpec>{T1(), T2()};
    const auto session = MakeSession(Grid(), views);
    for (const auto minimum : {0u, 1u, 129u, 4294967295u}) {
        Rejects([&] { (void)EstimateUndistortedIntrinsics(session, Policy(minimum)); }, "A0: minimum_views " + std::to_string(minimum));
    }
    for (const double cutoff : {0.0, 1.0, -1e-4, 2.0, std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        Rejects([&] { (void)EstimateUndistortedIntrinsics(session, Policy(2, cutoff)); }, "A0: cutoff " + Num(cutoff));
    }
    Rejects([&] { (void)EstimateUndistortedIntrinsics(session, {2, 1e-4, static_cast<ObservationModel>(1)}); }, "A0: unknown observation model");
    try {
        (void)EstimateUndistortedIntrinsics(session, Policy(1));
    } catch (const std::invalid_argument& error) {
        Check(std::string(error.what()) == "undistorted intrinsics: invalid policy", "A0: the message names the category only");
    }
    // The edges of the accepted range are accepted.
    {
        bool threw = false;
        try { (void)EstimateUndistortedIntrinsics(session, Policy(2)); } catch (const std::invalid_argument&) { threw = true; }
        Check(!threw, "A0: minimum_views 2 is accepted");
    }
    Check(EstimateUndistortedIntrinsics(session, Policy(128)).cameras[0].status == IntrinsicsStatus::InsufficientViews, "A0: minimum_views 128 is accepted");
    {
        bool threw = false;
        try { (void)EstimateUndistortedIntrinsics(session, Policy(2, std::nextafter(0.0, 1.0))); } catch (const std::invalid_argument&) { threw = true; }
        Check(!threw, "A0: the smallest positive cutoff passes the policy check");
    }
    const auto echoed = EstimateUndistortedIntrinsics(session, Policy());
    Check(echoed.observation_model == ObservationModel::UndistortedAnalytic, "A0: the model is echoed");

    // Codes.
    Check(ObservationModelCode(ObservationModel::UndistortedAnalytic) == "undistorted-analytic", "code: observation model");
    Check(ObservationModelCode(static_cast<ObservationModel>(1)).empty(), "code: observation model out of range is empty");
    const std::vector<std::pair<IntrinsicsStatus, std::string_view>> statuses{
        {IntrinsicsStatus::UndistortedIntrinsicsEstimated, "undistorted-intrinsics-estimated"},
        {IntrinsicsStatus::HomographyUnavailable, "homography-unavailable"},
        {IntrinsicsStatus::InsufficientViews, "insufficient-views"},
        {IntrinsicsStatus::InsufficientViewDiversity, "insufficient-view-diversity"},
        {IntrinsicsStatus::InconsistentLinearConstraints, "inconsistent-linear-constraints"},
        {IntrinsicsStatus::NumericalFailure, "numerical-failure"},
        {IntrinsicsStatus::NonphysicalIntrinsics, "nonphysical-intrinsics"},
        {IntrinsicsStatus::PoseUnavailable, "pose-unavailable"}};
    Check(statuses.size() == 8, "code: eight statuses");
    const std::vector<std::optional<IntrinsicsStage>> stages{std::nullopt, IntrinsicsStage::Homography, IntrinsicsStage::ViewGeometry,
        IntrinsicsStage::ViewGeometry, IntrinsicsStage::ViewGeometry, IntrinsicsStage::ViewGeometry, IntrinsicsStage::Intrinsics, IntrinsicsStage::Pose};
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        Check(IntrinsicsStatusCode(statuses[i].first) == statuses[i].second, "code: status " + std::string(statuses[i].second));
        Check(FailedStage(statuses[i].first) == stages[i], "code: failed stage of " + std::string(statuses[i].second));
    }
    Check(IntrinsicsStatusCode(static_cast<IntrinsicsStatus>(99)).empty() && !FailedStage(static_cast<IntrinsicsStatus>(99)),
        "code: status out of range is empty");
    Check(IntrinsicsStageCode(IntrinsicsStage::Homography) == "homography" && IntrinsicsStageCode(IntrinsicsStage::ViewGeometry) == "view-geometry"
        && IntrinsicsStageCode(IntrinsicsStage::Intrinsics) == "intrinsics" && IntrinsicsStageCode(IntrinsicsStage::Pose) == "pose"
        && IntrinsicsStageCode(static_cast<IntrinsicsStage>(99)).empty(), "code: stages");
    Check(ViewOutcomeCode(ViewOutcome::Evaluated) == "evaluated" && ViewOutcomeCode(ViewOutcome::InsufficientObservations) == "insufficient-observations"
        && ViewOutcomeCode(ViewOutcome::DegeneratePointConfiguration) == "degenerate-point-configuration"
        && ViewOutcomeCode(ViewOutcome::NumericalFailure) == "numerical-failure" && ViewOutcomeCode(ViewOutcome::ChartNotInFront) == "chart-not-in-front"
        && ViewOutcomeCode(ViewOutcome::NotReached) == "not-reached" && ViewOutcomeCode(static_cast<ViewOutcome>(99)).empty(), "code: outcomes");
    Check(HeldOutCoverageCode(HeldOutCoverage::NotReached) == "not-reached" && HeldOutCoverageCode(HeldOutCoverage::NoValidationView) == "no-validation-view"
        && HeldOutCoverageCode(HeldOutCoverage::AllViewsEvaluated) == "all-validation-views-evaluated"
        && HeldOutCoverageCode(HeldOutCoverage::SomeViewsNotEvaluated) == "some-validation-views-not-evaluated"
        && HeldOutCoverageCode(static_cast<HeldOutCoverage>(99)).empty(), "code: held-out coverage");
}

void TestSignature() {
    static_assert(std::is_same_v<decltype(&EstimateUndistortedIntrinsics),
        UndistortedIntrinsicsResult (*)(const CalibrationObservationSession&, const UndistortedIntrinsicsPolicy&)>,
        "the API takes the session and the policy and nothing else");
    const UndistortedIntrinsicsPolicy policy = Policy();
    const auto& [minimum, cutoff, model] = policy; // exactly three fields, no hidden default
    Check(minimum == 2 && cutoff == 1e-4 && model == ObservationModel::UndistortedAnalytic, "S-a: the policy has exactly three fields");
}

void TestFrontalAndSameTilt() {
    // G1: pure translations of a frontal plane.
    {
        const auto result = Run(Frontal(0, 0, 0, "F"));
        Invariants(result, "G1");
        const auto& a = result.cameras[0];
        Check(a.status == IntrinsicsStatus::InsufficientViewDiversity, "G1: status is insufficient-view-diversity");
        Check(a.view_geometry && a.view_geometry->status == ViewGeometryStatus::InsufficientDiversity, "G1: the preflight says the same");
        Check(a.view_geometry && a.view_geometry->numerical_rank && *a.view_geometry->numerical_rank == 2, "G1: rank is 2");
        Check(a.view_geometry && !a.view_geometry->conditioned_conic, "G1: no null vector");
        Check(!a.intrinsics && !a.training_residual, "G1: no intrinsics and no residual");
        Check(a.views.size() == 3, "G1: three views");
        for (const auto& view : a.views) {
            Check(view.chart_to_image && !view.pose && !view.residual && view.outcome == ViewOutcome::NotReached,
                "G1: H is kept, nothing after it: " + view.view_id);
        }
        Check(a.held_out_coverage == HeldOutCoverage::NotReached, "G1: held-out coverage is not-reached");
        Check(FailedStage(a.status) == IntrinsicsStage::ViewGeometry, "G1: the view-geometry stage stopped it");
    }
    // G2: the same tilt repeated.
    {
        const auto result = Run(Frontal(0.20, 0.10, 0.05, "S"));
        Invariants(result, "G2");
        const auto& a = result.cameras[0];
        Check(a.status == IntrinsicsStatus::InsufficientViewDiversity, "G2: status is insufficient-view-diversity");
        Check(a.view_geometry && a.view_geometry->numerical_rank && *a.view_geometry->numerical_rank == 3, "G2: rank is 3");
        Check(a.view_geometry && !a.view_geometry->conditioned_conic && !a.intrinsics && !a.training_residual, "G2: no null vector, intrinsics or residual");
        for (const auto& view : a.views) Check(view.chart_to_image && !view.pose && !view.residual, "G2: H only: " + view.view_id);
    }
}

void TestDifferentTilts() {
    const auto specs = G3Views();
    const auto result = Run(specs);
    Invariants(result, "G3");
    const auto& a = result.cameras[0];
    Check(a.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated, "G3: status is undistorted-intrinsics-estimated");
    Check(a.view_geometry && a.view_geometry->status == ViewGeometryStatus::LinearConstraintsSufficient, "G3: preflight is sufficient");
    Check(a.view_geometry && a.view_geometry->numerical_rank && *a.view_geometry->numerical_rank == 5, "G3: rank is 5");
    Check(a.view_geometry && a.view_geometry->view_count == 3, "G3: the preflight saw the three training views only");
    const auto* conic = a.view_geometry && a.view_geometry->conditioned_conic ? &*a.view_geometry->conditioned_conic : nullptr;
    Check(conic != nullptr, "G3: null vector present");
    if (conic) {
        Check(conic->center_x == 639.5 && conic->center_y == 479.5 && conic->scale == 640, "G3: image frame is (w-1)/2, (h-1)/2, max(w,h)/2");
        double norm = 0;
        for (const auto value : conic->b) norm = std::hypot(norm, value);
        Check(std::abs(norm - 1) <= 1e-12, "G3: the null vector has unit length");
    }
    Check(a.intrinsics.has_value() && a.training_residual.has_value(), "G3: intrinsics and training residual");
    Check(a.views.size() == 4, "G3: training and validation views are listed, the reference view is not");
    Check(!FindView(a, "R1"), "G3: reference view is not listed");
    Check(a.held_out_coverage == HeldOutCoverage::AllViewsEvaluated, "G3: all validation views evaluated");
    for (const auto* id : {"T1", "T2", "T3", "V1"}) {
        const auto* view = FindView(a, id);
        Check(view && view->outcome == ViewOutcome::Evaluated, std::string("G3: evaluated: ") + id);
    }
    const auto* t1 = FindView(a, "T1");
    Check(t1 && t1->residual && t1->residual->point_count == 112, "G3: T1 residual uses the 112 used points");
    Check(a.training_residual && a.training_residual->point_count == 112 + 117 + 117, "G3: pooled training residual counts every used point");
    const std::vector<ViewSpec> truths{specs[0], specs[1], specs[2], specs[3]};
    for (const auto& truth : truths) {
        const double difference = ProbeDifference(a, truth);
        Check(difference <= 1e-6, "G3: probe points differ from the truth by at most 1e-6 px: " + truth.id + " (" + Num(difference) + ")");
    }
    const auto* v1 = FindView(a, "V1");
    Check(v1 && v1->residual && v1->residual->point_count == 58, "G3: V1 residual has the 58 measurement points");
    Check(v1 && v1->residual && v1->residual->rms_pixels <= 1e-6 && v1->residual->max_pixels <= 1e-6, "G3: V1 residual is at most 1e-6 px");
    Check(a.held_out_residual && a.held_out_residual->point_count == 58 && a.held_out_residual->rms_pixels <= 1e-6
        && a.held_out_residual->max_pixels <= 1e-6, "G3: held-out residual is at most 1e-6 px");
    Check(a.training_residual && a.training_residual->rms_pixels <= 1e-6 && a.training_residual->max_pixels <= 1e-6,
        "G3: training residual is at most 1e-6 px");
    if (a.intrinsics) {
        // Information only: the coefficients are not part of any verdict.
        std::cout << "info: G3 coefficient differences fx=" << a.intrinsics->fx - kTrue.fx << " fy=" << a.intrinsics->fy - kTrue.fy
                  << " cx=" << a.intrinsics->cx - kTrue.cx << " cy=" << a.intrinsics->cy - kTrue.cy << '\n';
    }
    // CAM-B has no view at all: it is reported, not skipped.
    const auto& b = result.cameras[1];
    Check(b.status == IntrinsicsStatus::InsufficientViews && b.views.empty() && !b.intrinsics, "G3: CAM-B without views is insufficient-views");
    Check(b.view_geometry && b.view_geometry->view_count == 0 && b.view_geometry->numerical_rank && *b.view_geometry->numerical_rank == 1,
        "G3: CAM-B without views keeps the prior-only spectrum");

    // G4: intrinsics of different focal lengths cannot satisfy one linear system.
    {
        const auto mixed = Run({T1(), T2(), T3(), KB(), KC()});
        Invariants(mixed, "G4");
        const auto& m = mixed.cameras[0];
        Check(m.status == IntrinsicsStatus::InconsistentLinearConstraints, "G4: status is inconsistent-linear-constraints");
        Check(m.view_geometry && m.view_geometry->numerical_rank && *m.view_geometry->numerical_rank == 6, "G4: rank is 6");
        Check(!m.intrinsics && !m.training_residual && m.view_geometry && !m.view_geometry->conditioned_conic, "G4: no intrinsics");
        for (const auto& view : m.views) Check(!view.pose && view.chart_to_image, "G4: H only: " + view.view_id);
    }
}

void TestInsufficient() {
    {   // V1a: one view
        const auto result = Run({T1()});
        Invariants(result, "V1a");
        const auto& a = result.cameras[0];
        Check(a.status == IntrinsicsStatus::InsufficientViews && !a.intrinsics, "V1a: a single view is insufficient-views");
        Check(FailedStage(a.status) == IntrinsicsStage::ViewGeometry, "V1a: view-geometry stage");
    }
    {   // V1b: three views, four required
        const auto result = Run({T1(), T2(), T3()}, Policy(4));
        Invariants(result, "V1b");
        const auto& a = result.cameras[0];
        Check(a.status == IntrinsicsStatus::InsufficientViews, "V1b: minimum_views 4 with three views");
        Check(a.view_geometry && a.view_geometry->numerical_rank && *a.view_geometry->numerical_rank == 5, "V1b: the rank is still reported");
        Check(a.view_geometry && !a.view_geometry->conditioned_conic && !a.intrinsics, "V1b: no null vector and no intrinsics");
    }
    {   // V2: no validation view
        const auto result = Run({T1(), T2(), T3()});
        Invariants(result, "V2");
        const auto& a = result.cameras[0];
        Check(a.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated, "V2: succeeds without a validation view");
        Check(a.held_out_coverage == HeldOutCoverage::NoValidationView && !a.held_out_residual, "V2: no held-out residual, and the report says so");
    }
}

void TestViewGates() {
    {   // V3: too few points
        auto few = T1();
        few.used = Ids(few.observed, 0, 3);
        const auto result = Run({few, T2(), T3()});
        Invariants(result, "V3a");
        const auto& a = result.cameras[0];
        Check(a.status == IntrinsicsStatus::HomographyUnavailable, "V3a: three used points stop the camera at the homography stage");
        Check(!a.view_geometry && !a.intrinsics, "V3a: no view geometry, no intrinsics");
        const auto* view = FindView(a, "T1");
        Check(view && view->outcome == ViewOutcome::InsufficientObservations && !view->chart_to_image, "V3a: T1 is insufficient-observations");
        const auto* other = FindView(a, "T2");
        Check(other && other->chart_to_image && !other->pose && other->outcome == ViewOutcome::NotReached, "V3a: T2 keeps H and is not-reached");
        Check(FailedStage(a.status) == IntrinsicsStage::Homography, "V3a: homography stage");
    }
    {   // V3: five points on one row
        auto line = T1();
        line.used = Ids(line.observed, 0, 5);
        const auto result = Run({line, T2(), T3()});
        Invariants(result, "V3b");
        const auto& a = result.cameras[0];
        Check(a.status == IntrinsicsStatus::HomographyUnavailable && !a.view_geometry, "V3b: collinear points stop the camera");
        const auto* view = FindView(a, "T1");
        Check(view && view->outcome == ViewOutcome::DegeneratePointConfiguration && !view->chart_to_image, "V3b: T1 is degenerate-point-configuration");
    }
    {   // V4: a validation view with too few pose points does not change the camera status
        auto thin = FullView("V2", kPoseV1, kTrue, "CAM-A", "validation");
        thin.used = Ids(thin.observed, 0, 3);
        thin.measurement = Ids(thin.observed, 3, 8);
        auto specs = G3Views();
        specs.push_back(thin);
        const auto result = Run(specs);
        Invariants(result, "V4");
        const auto& a = result.cameras[0];
        Check(a.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated, "V4: camera still succeeds");
        const auto* view = FindView(a, "V2");
        Check(view && view->outcome == ViewOutcome::InsufficientObservations && !view->residual, "V4: the thin validation view is insufficient-observations");
        Check(a.held_out_coverage == HeldOutCoverage::SomeViewsNotEvaluated, "V4: coverage is some-validation-views-not-evaluated");
        Check(a.held_out_residual && a.held_out_residual->point_count == 58, "V4: held-out residual comes from the evaluated view only");
    }
}

void TestNonphysicalAndPose() {
    {   // N1: a second view whose observation is the chart with y halved
        ViewSpec stretched = FullView("N1", MakePose(0, 0.4, 0, 0, 0, 760));
        stretched.y_scale = 0.5;
        const auto result = Run({T1(), stretched});
        Invariants(result, "N1");
        const auto& a = result.cameras[0];
        Check(a.view_geometry && a.view_geometry->status == ViewGeometryStatus::LinearConstraintsSufficient
            && a.view_geometry->numerical_rank && *a.view_geometry->numerical_rank == 5, "N1: preflight is sufficient with rank 5");
        Check(a.view_geometry && a.view_geometry->conditioned_conic.has_value(), "N1: the raw null vector is reported");
        Check(a.status == IntrinsicsStatus::NonphysicalIntrinsics && !a.intrinsics, "N1: nonphysical-intrinsics and no clamped value");
        Check(FailedStage(a.status) == IntrinsicsStage::Intrinsics, "N1: intrinsics stage");
        for (const auto& view : a.views) Check(view.chart_to_image && !view.pose, "N1: H only: " + view.view_id);
    }
    {   // P1: a plane that crosses the camera centre plane
        auto chart = Grid();
        const auto behind = BehindPoints();
        chart.insert(chart.end(), behind.begin(), behind.end());
        ViewSpec crossing;
        crossing.id = "P1";
        crossing.pose = {{1, 0, 0, 0, 0, -1, 0, 1, 0}, {0, 10, 0}};
        crossing.observed = behind;
        crossing.used = Ids(behind);
        crossing.allow_behind = true;
        const auto result = Run(chart, {T1(), T2(), crossing});
        Invariants(result, "P1");
        const auto& a = result.cameras[0];
        Check(a.status == IntrinsicsStatus::PoseUnavailable, "P1: pose-unavailable");
        Check(FailedStage(a.status) == IntrinsicsStage::Pose, "P1: pose stage");
        Check(a.intrinsics.has_value() && !a.training_residual, "P1: intrinsics are kept, no pooled training residual");
        const auto* view = FindView(a, "P1");
        Check(view && view->outcome == ViewOutcome::ChartNotInFront && view->chart_to_image && !view->pose, "P1: the crossing view is chart-not-in-front");
        const auto* t1 = FindView(a, "T1");
        Check(t1 && t1->outcome == ViewOutcome::Evaluated && t1->pose, "P1: T1 keeps its pose");
        Check(a.held_out_coverage == HeldOutCoverage::NotReached, "P1: held-out is not reached");
    }
}

void TestCamerasAreIndependent() {
    auto mixed = G3Views();
    for (auto& view : Frontal(0, 0, 0, "B")) { view.camera = "CAM-B"; mixed.push_back(view); }
    const auto c1 = Run(mixed);
    Invariants(c1, "C1");
    Check(c1.cameras[0].camera == CameraAlias::CamA && c1.cameras[1].camera == CameraAlias::CamB, "C1: camera order");
    Check(c1.cameras[0].status == IntrinsicsStatus::UndistortedIntrinsicsEstimated, "C1: CAM-A succeeds");
    Check(c1.cameras[1].status == IntrinsicsStatus::InsufficientViewDiversity, "C1: CAM-B is insufficient-view-diversity");
    Check(c1.cameras[1].views.size() == 3 && c1.cameras[0].views.size() == 4, "C1: each camera lists its own views");

    auto other = G3Views();
    for (auto view : std::vector<ViewSpec>{T1(), T2(), T3(), KB("CAM-B", "BKB"), KC("CAM-B", "BKC")}) {
        view.camera = "CAM-B";
        view.id = "B" + view.id;
        other.push_back(view);
    }
    const auto c2 = Run(other);
    Invariants(c2, "C2");
    Check(c2.cameras[1].status == IntrinsicsStatus::InconsistentLinearConstraints, "C2: CAM-B is inconsistent");
    Check(Fingerprint(c1.cameras[0]) == Fingerprint(c2.cameras[0]), "C2: CAM-A is bitwise unchanged by what CAM-B holds");
    // The reverse direction: CAM-B alone on its own is identical to the same views as CAM-B next to CAM-A.
    std::vector<ViewSpec> b_only;
    for (auto view : std::vector<ViewSpec>{T1(), T2(), T3()}) { view.camera = "CAM-B"; view.id = "B" + view.id; b_only.push_back(view); }
    auto b_next = G3Views();
    for (auto view : b_only) b_next.push_back(view);
    Check(Fingerprint(Run(b_only).cameras[1]) == Fingerprint(Run(b_next).cameras[1]), "C2: CAM-B is bitwise unchanged by what CAM-A holds");
    Check(Run(b_only).cameras[1].status == IntrinsicsStatus::UndistortedIntrinsicsEstimated, "C2: CAM-B can succeed on its own");
}

void TestSeparationFromTruth() {
    const auto base_specs = G3Views();
    const auto base = Run(base_specs);
    const auto& a = base.cameras[0];
    // (b) one training pixel moves by 0.25 px: H and the spectrum of that view's preflight change.
    {
        auto specs = base_specs;
        specs[0].delta["P1_4"] = {0.25, 0};
        const auto moved = Run(specs);
        Invariants(moved, "S-b");
        const auto& m = moved.cameras[0];
        const auto* h_base = FindView(a, "T1");
        const auto* h_moved = FindView(m, "T1");
        bool h_changed = false;
        if (h_base && h_moved && h_base->chart_to_image && h_moved->chart_to_image) {
            for (std::size_t i = 0; i < 9; ++i) h_changed = h_changed || !Bitwise((*h_base->chart_to_image)[i], (*h_moved->chart_to_image)[i]);
        }
        Check(h_changed, "S-b: T1's homography changes when a used pixel moves");
        bool spectrum_changed = false;
        if (a.view_geometry && m.view_geometry && a.view_geometry->singular_values && m.view_geometry->singular_values) {
            for (std::size_t i = 0; i < 6; ++i) {
                spectrum_changed = spectrum_changed || !Bitwise((*a.view_geometry->singular_values)[i], (*m.view_geometry->singular_values)[i]);
            }
        }
        Check(spectrum_changed, "S-b: the singular values change");
        Check(m.intrinsics && a.intrinsics && !Bitwise(m.intrinsics->fx, a.intrinsics->fx), "S-b: the intrinsics change");
        for (const auto* id : {"T2", "T3"}) {
            const auto* before = FindView(a, id);
            const auto* after = FindView(m, id);
            bool same = before && after && before->chart_to_image && after->chart_to_image;
            for (std::size_t i = 0; same && i < 9; ++i) same = Bitwise((*before->chart_to_image)[i], (*after->chart_to_image)[i]);
            Check(same, std::string("S-b: the other views' homographies are bitwise unchanged: ") + id);
        }
    }
    // The cutoff is a caller policy: the same 0.25 px observation is rank 5 at 1e-4 and rank 6 at 1e-8.
    {
        auto specs = base_specs;
        specs[0].delta["P1_4"] = {0.25, 0};
        const auto loose = Run(specs, Policy(2, 1e-4));
        const auto tight = Run(specs, Policy(2, 1e-8));
        Check(loose.cameras[0].status == IntrinsicsStatus::UndistortedIntrinsicsEstimated, "S-b: cutoff 1e-4 still gives rank 5");
        Check(tight.cameras[0].status == IntrinsicsStatus::InconsistentLinearConstraints
            && tight.cameras[0].view_geometry && tight.cameras[0].view_geometry->numerical_rank
            && *tight.cameras[0].view_geometry->numerical_rank == 6, "S-b: cutoff 1e-8 reads the same observation as rank 6");
        if (tight.cameras[0].view_geometry && tight.cameras[0].view_geometry->singular_values) {
            const auto& sv = tight.cameras[0].view_geometry->singular_values;
            std::cout << "info: S-b smallest/largest singular value ratio=" << (*sv)[5] / (*sv)[0] << '\n';
        }
    }
    // (b') a detected but unused point may move arbitrarily.
    {
        auto specs = base_specs;
        specs[0].delta["P8_12"] = {5.0, -3.0};
        const auto moved = Run(specs);
        Check(Fingerprint(moved.cameras[0]) == Fingerprint(a), "S-b': an unused point does not change the result");
    }
    // (c) validation and reference pixels never reach the intrinsics.
    {
        auto specs = base_specs;
        specs[3].shift_x = 0.5;            // V1: a uniform shift is largely absorbed by the pose ...
        specs[3].delta["P0_1"] = {1.0, 0}; // ... so one measurement point also moves by itself
        specs[4].shift_x = 0.5; // R1
        const auto moved = Run(specs);
        Invariants(moved, "S-c");
        const auto& m = moved.cameras[0];
        Check(Fingerprint(m, true) == Fingerprint(a, true), "S-c: geometry, intrinsics and every training view are bitwise unchanged");
        Check(m.held_out_residual && a.held_out_residual && !Bitwise(m.held_out_residual->rms_pixels, a.held_out_residual->rms_pixels),
            "S-c: the held-out residual does change, so the change was visible to the estimator");
        Check(m.held_out_residual && m.held_out_residual->max_pixels > 0.5, "S-c: the moved measurement point shows in the held-out maximum");
    }
    // The chart ids a view does not name are not read: a reference-only change is invisible.
    {
        auto specs = base_specs;
        specs.pop_back(); // no reference view at all
        const auto no_reference = Run(specs);
        Check(Fingerprint(no_reference.cameras[0]) == Fingerprint(a), "S-c: dropping the reference view changes nothing");
    }
}

void TestDeterminism() {
    const auto json = BuildJson(Grid(), G3Views());
    const auto first = CalibrationObservationSession::Parse(json);
    const auto second = CalibrationObservationSession::Parse(json);
    const auto policy = Policy();
    const auto a = Fingerprint(EstimateUndistortedIntrinsics(first, policy));
    const auto b = Fingerprint(EstimateUndistortedIntrinsics(first, policy));
    const auto c = Fingerprint(EstimateUndistortedIntrinsics(second, policy));
    const auto d = Fingerprint(EstimateUndistortedIntrinsics(second, policy));
    Check(a == b && b == c && c == d, "D1: four runs give one fingerprint");
    Check(a.find("undistorted-intrinsics-estimated") != std::string::npos, "D1: the fingerprinted result is the successful one");
}

// ---------------------------------------------------------------------------
// Additions from the independent QA review of #289 (2026-10-13).

bool Near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

// Points far outside the chart hull (|x| >= 200 mm, |y| >= 150 mm), in plane and out of plane.
std::vector<Probe> OutsideProbes() {
    std::vector<Probe> probes;
    for (const double x : {-300.0, -200.0, 200.0, 300.0}) for (const double y : {-250.0, -150.0, 150.0, 250.0}) probes.push_back({x, y, 0});
    for (const double z : {-100.0, 100.0}) for (const double x : {-200.0, 200.0}) for (const double y : {-150.0, 150.0}) probes.push_back({x, y, z});
    return probes;
}
double ProbeDifferenceOver(const CameraIntrinsicsResult& camera, const ViewSpec& truth, const std::vector<Probe>& probes) {
    const auto* view = FindView(camera, truth.id);
    if (!view || !view->pose || !camera.intrinsics) return std::numeric_limits<double>::infinity();
    double worst = 0;
    for (const auto& probe : probes) {
        const auto estimate = ProjectEstimate(*camera.intrinsics, *view->pose, probe.x, probe.y, probe.z);
        const auto exact = ProjectTruth(truth.k, truth.pose, probe.x, probe.y, probe.z);
        worst = std::max(worst, std::hypot(estimate.u - exact.u, estimate.v - exact.v));
    }
    return worst;
}

// Residual of an estimate recomputed from the fixture, with no production code.
struct Expected { double rms = 0, max = 0; std::uint32_t n = 0; };
Expected ExpectedResidual(const ViewSpec& spec, const std::vector<std::string>& ids, const PinholeIntrinsics& k, const ChartPose& pose) {
    Expected out;
    double squares = 0;
    for (const auto& id : ids) {
        const auto point = std::find_if(spec.observed.begin(), spec.observed.end(), [&](const ChartPt& p) { return p.id == id; });
        const auto truth = ProjectTruth(spec.k, spec.pose, point->x, spec.y_scale * point->y);
        double u = truth.u + spec.shift_x, v = truth.v;
        const auto moved = spec.delta.find(id);
        if (moved != spec.delta.end()) { u += moved->second.first; v += moved->second.second; }
        const auto estimate = ProjectEstimate(k, pose, point->x, point->y);
        const double error = std::hypot(estimate.u - u, estimate.v - v);
        squares += error * error;
        out.max = std::max(out.max, error);
        ++out.n;
    }
    out.rms = std::sqrt(squares / static_cast<double>(out.n));
    return out;
}
bool ResidualMatches(const std::optional<ResidualSummary>& got, const Expected& want) {
    return got && got->point_count == want.n && Near(got->rms_pixels, want.rms) && Near(got->max_pixels, want.max);
}

std::vector<ViewSpec> AsCamB(std::vector<ViewSpec> views) {
    for (auto& view : views) { view.camera = "CAM-B"; view.id = "B" + view.id; }
    return views;
}
ViewSpec CrossingView() {
    ViewSpec crossing;
    crossing.id = "P1";
    crossing.pose = {{1, 0, 0, 0, 0, -1, 0, 1, 0}, {0, 10, 0}};
    crossing.observed = BehindPoints();
    crossing.used = Ids(crossing.observed);
    crossing.allow_behind = true;
    return crossing;
}

void TestProbesAreIndependentOfTheSession() {
    const auto specs = G3Views();
    const auto result = Run(specs);
    Invariants(result, "R-probe");
    const auto& a = result.cameras[0];
    const auto grid = Grid();
    const auto inside = Probes();
    const auto outside = OutsideProbes();
    bool overlap = false;
    for (const auto* list : {&inside, &outside}) for (const auto& probe : *list) for (const auto& point : grid) {
        overlap = overlap || (probe.z == 0 && probe.x == point.x && probe.y == point.y);
    }
    Check(!overlap, "R-probe: no probe point is a chart point of the session");
    Check(inside.size() == 114 && outside.size() == 24, "R-probe: probe counts");
    bool outside_hull = true;
    for (const auto& probe : outside) outside_hull = outside_hull && (std::abs(probe.x) >= 200 && std::abs(probe.y) >= 150);
    Check(outside_hull, "R-probe: every extra probe is outside the chart hull");
    for (std::size_t i = 0; i < 4; ++i) {
        bool in_front = true;
        for (const auto& probe : outside) in_front = in_front && ProjectTruth(specs[i].k, specs[i].pose, probe.x, probe.y, probe.z).z > 0;
        Check(in_front, "R-probe: fixture probes are in front of the camera: " + specs[i].id);
        const double difference = ProbeDifferenceOver(a, specs[i], outside);
        Check(difference <= 1e-6, "R-probe: points outside the chart differ from the truth by at most 1e-6 px: " + specs[i].id + " (" + Num(difference) + ")");
    }
}

void TestNumericalFailureStatuses() {
    {   // The preflight itself reports a numerical failure when the cutoff is below its own resolution.
        // Two training views give five rows for six unknowns, so the smallest column is zero by
        // structure (as in the #283 test), not by how the rounding of the homographies falls.
        const auto result = Run({T1(), T2(), V1(), R1()}, Policy(2, 1e-17));
        Invariants(result, "NF1");
        const auto& a = result.cameras[0];
        Check(a.status == IntrinsicsStatus::NumericalFailure, "NF1: status is numerical-failure");
        Check(a.view_geometry && a.view_geometry->status == ViewGeometryStatus::NumericalFailure, "NF1: the preflight says numerical-failure");
        Check(FailedStage(a.status) == IntrinsicsStage::ViewGeometry, "NF1: view-geometry stage");
        Check(!a.intrinsics && !a.training_residual && a.view_geometry && !a.view_geometry->conditioned_conic, "NF1: no intrinsics, no residual, no null vector");
        for (const auto& view : a.views) {
            if (view.role == ViewRole::Training) Check(view.chart_to_image && !view.pose && !view.residual, "NF1: H only: " + view.view_id);
            else Check(!view.chart_to_image && view.outcome == ViewOutcome::NotReached, "NF1: the validation view is not reached: " + view.view_id);
        }
        Check(a.held_out_coverage == HeldOutCoverage::NotReached && !a.held_out_residual, "NF1: held-out is not reached");
        Check(result.cameras[1].status == IntrinsicsStatus::InsufficientViews, "NF1: CAM-B is reported on its own");
    }
    {   // Zero focal length puts every pixel of the view on one point: the Hartley scale of the image is not finite.
        auto flat = T1();
        flat.k.fx = 0;
        flat.k.fy = 0;
        const auto result = Run({flat, T2(), T3()});
        Invariants(result, "NF2");
        const auto& a = result.cameras[0];
        Check(a.status == IntrinsicsStatus::HomographyUnavailable && !a.view_geometry && !a.intrinsics, "NF2: homography-unavailable, no view geometry");
        const auto* view = FindView(a, "T1");
        Check(view && view->outcome == ViewOutcome::NumericalFailure && !view->chart_to_image, "NF2: the Hartley normalization of a single-point image is a numerical-failure outcome");
    }
}

void TestOneCameraFailsAndTheOtherSurvives() {
    auto chart = Grid();
    const auto behind = BehindPoints();
    chart.insert(chart.end(), behind.begin(), behind.end());
    ViewSpec stretched = FullView("N1", MakePose(0, 0.4, 0, 0, 0, 760));
    stretched.y_scale = 0.5;
    auto thin = T1();
    thin.used = Ids(thin.observed, 0, 3);
    struct Case { std::string name; std::vector<ViewSpec> views; IntrinsicsStatus expected; };
    const std::vector<Case> cases{
        {"insufficient-views", {T1()}, IntrinsicsStatus::InsufficientViews},
        {"homography-unavailable", {thin, T2(), T3()}, IntrinsicsStatus::HomographyUnavailable},
        {"insufficient-view-diversity", Frontal(0, 0, 0, "F"), IntrinsicsStatus::InsufficientViewDiversity},
        {"inconsistent-linear-constraints", {T1(), T2(), T3(), KB(), KC()}, IntrinsicsStatus::InconsistentLinearConstraints},
        {"nonphysical-intrinsics", {T1(), stretched}, IntrinsicsStatus::NonphysicalIntrinsics},
        {"pose-unavailable", {T1(), T2(), CrossingView()}, IntrinsicsStatus::PoseUnavailable},
        {"no-view", {}, IntrinsicsStatus::InsufficientViews}};
    const auto good = G3Views();
    const auto good_b = AsCamB(good);
    const auto good_alone_a = Run(chart, good).cameras[0];
    const auto good_alone_b = Run(chart, good_b).cameras[1];
    Check(good_alone_a.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated && good_alone_b.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated,
        "C3: the good camera succeeds on its own");
    for (const auto& item : cases) {
        const std::string label = "C3/" + item.name;
        // CAM-B holds the failing views and comes first in the session; CAM-A is the good one.
        auto first = AsCamB(item.views);
        first.insert(first.end(), good.begin(), good.end());
        const auto a_survives = Run(chart, first);
        Invariants(a_survives, label + "/B-fails");
        Check(a_survives.cameras[1].status == item.expected, label + ": CAM-B reports its own failure");
        Check(a_survives.cameras[0].status == IntrinsicsStatus::UndistortedIntrinsicsEstimated, label + ": CAM-A keeps its success");
        Check(Fingerprint(a_survives.cameras[0]) == Fingerprint(good_alone_a), label + ": CAM-A is bitwise what it is alone");
        Check(ProbeDifference(a_survives.cameras[0], good[0]) <= 1e-6 && ProbeDifference(a_survives.cameras[0], good[3]) <= 1e-6,
            label + ": CAM-A still reproduces the truth");
        // CAM-A holds the failing views and CAM-B is the good one.
        auto second = item.views;
        second.insert(second.end(), good_b.begin(), good_b.end());
        const auto b_survives = Run(chart, second);
        Invariants(b_survives, label + "/A-fails");
        Check(b_survives.cameras[0].status == item.expected, label + ": CAM-A reports its own failure");
        Check(b_survives.cameras[1].status == IntrinsicsStatus::UndistortedIntrinsicsEstimated, label + ": CAM-B keeps its success");
        Check(Fingerprint(b_survives.cameras[1]) == Fingerprint(good_alone_b), label + ": CAM-B is bitwise what it is alone");
        Check(ProbeDifference(b_survives.cameras[1], good_b[0]) <= 1e-6 && ProbeDifference(b_survives.cameras[1], good_b[3]) <= 1e-6,
            label + ": CAM-B still reproduces the truth");
    }
}

void TestValidationViewFailuresDoNotStopTheCamera() {
    auto chart = Grid();
    const auto behind = BehindPoints();
    chart.insert(chart.end(), behind.begin(), behind.end());
    // A validation view whose plane crosses the camera: the pose points are in front, the measurement points are behind.
    ViewSpec crossing = CrossingView();
    crossing.id = "VX";
    crossing.role = "validation";
    crossing.used.clear();
    for (std::size_t i = 0; i < crossing.observed.size(); ++i) (crossing.observed[i].y > 0 ? crossing.used : crossing.measurement).push_back(crossing.observed[i].id);
    ViewSpec collinear = FullView("VD", kPoseV1, kTrue, "CAM-A", "validation");
    collinear.used = Ids(collinear.observed, 0, 5);
    collinear.measurement = Ids(collinear.observed, 5, 12);
    const auto training_only = Fingerprint(Run(chart, {T1(), T2(), T3()}).cameras[0], true);
    // The only validation view fails: the camera stays successful and the held-out residual is null, not zero.
    for (const auto& bad : {crossing, collinear}) {
        const auto result = Run(chart, {T1(), T2(), T3(), bad});
        const std::string label = "VF/" + bad.id;
        Invariants(result, label);
        const auto& a = result.cameras[0];
        const bool is_crossing = bad.id == "VX";
        Check(a.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated, label + ": the camera still succeeds");
        Check(a.held_out_coverage == HeldOutCoverage::SomeViewsNotEvaluated, label + ": coverage says some views were not evaluated");
        Check(!a.held_out_residual, label + ": N = 0 gives a null held-out residual");
        const auto* view = FindView(a, bad.id);
        Check(view && !view->pose && !view->residual, label + ": the failed validation view has no pose or residual");
        Check(view && view->outcome == (is_crossing ? ViewOutcome::ChartNotInFront : ViewOutcome::DegeneratePointConfiguration),
            label + ": the outcome names the reason");
        Check(view && view->chart_to_image.has_value() == is_crossing, label + ": H is kept only when it could be fitted");
        Check(Fingerprint(a, true) == training_only, label + ": the training result is bitwise what it is without the failed view");
    }
    // One failing and one good validation view: pooled from the good one only.
    const auto mixed = Run(chart, {T1(), T2(), T3(), V1(), crossing});
    Invariants(mixed, "VF/mixed");
    const auto& a = mixed.cameras[0];
    Check(a.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated && a.held_out_coverage == HeldOutCoverage::SomeViewsNotEvaluated,
        "VF/mixed: success with partial coverage");
    Check(a.held_out_residual && a.held_out_residual->point_count == 58, "VF/mixed: the held-out residual is pooled from the evaluated view only");
}

void TestChartBehindTheCamera() {
    // A chart wholly behind the camera gives the same pixels as the mirrored plane in front.
    // The estimator must pick the pose with positive depth: (-r1, -r2, r3, -t).
    auto behind = FullView("BH", MakePose(0.20, -0.25, 0.10, 5, 8, -700));
    behind.allow_behind = true;
    const auto result = Run({T1(), T2(), T3(), behind});
    Invariants(result, "SG");
    const auto& a = result.cameras[0];
    Check(a.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated, "SG: a view behind the camera is estimated as its mirrored plane in front");
    const auto* view = FindView(a, "BH");
    Check(view && view->pose && view->outcome == ViewOutcome::Evaluated, "SG: the view has a pose");
    if (view && view->pose) {
        bool in_front = true;
        for (const auto& point : Grid()) {
            const double depth = view->pose->rotation[6] * point.x + view->pose->rotation[7] * point.y + view->pose->translation_mm[2];
            in_front = in_front && depth > 0;
        }
        Check(in_front, "SG: every chart point has positive depth");
        const auto& r = view->pose->rotation;
        const auto& truth = behind.pose;
        Check(Near(r[0], -truth.r[0]) && Near(r[3], -truth.r[3]) && Near(r[6], -truth.r[6]) && Near(r[1], -truth.r[1]) && Near(r[4], -truth.r[4])
            && Near(r[7], -truth.r[7]) && Near(r[2], truth.r[2]) && Near(r[5], truth.r[5]) && Near(r[8], truth.r[8]), "SG: the rotation is (-r1, -r2, r3)");
        Check(Near(view->pose->translation_mm[0], -truth.t[0], 1e-6) && Near(view->pose->translation_mm[1], -truth.t[1], 1e-6)
            && Near(view->pose->translation_mm[2], -truth.t[2], 1e-6), "SG: the translation is the negated truth");
        std::vector<Probe> plane;
        for (const auto& probe : Probes()) if (probe.z == 0) plane.push_back(probe);
        Check(ProbeDifferenceOver(a, behind, plane) <= 1e-6, "SG: in-plane pixels agree with the truth");
    }
    for (const auto& truth : {T1(), T2(), T3()}) {
        Check(ProbeDifference(a, truth) <= 1e-6, "SG: the other views are unharmed: " + truth.id);
    }
}

void TestTruthSeparationReview() {
    const auto base_specs = G3Views();
    const auto base = Run(base_specs);
    const auto& a = base.cameras[0];
    {   // Validation and reference views that disagree with the geometry change nothing in the training result.
        auto specs = base_specs;
        specs[3].y_scale = 0.5;
        specs[4].y_scale = 0.5;
        const auto result = Run(specs);
        Invariants(result, "TS-garbage");
        Check(Fingerprint(result.cameras[0], true) == Fingerprint(a, true), "TS-garbage: the training result is bitwise unchanged");
        Check(result.cameras[0].held_out_residual && result.cameras[0].held_out_residual->rms_pixels > 1, "TS-garbage: the held-out residual shows the disagreement");
    }
    {   // A validation point named by neither list is not read.
        auto kept = V1();
        kept.measurement.pop_back();
        const std::string dropped = kept.observed[115].id;
        auto before_specs = base_specs;
        before_specs[3] = kept;
        auto after_specs = before_specs;
        after_specs[3].delta[dropped] = {7.0, -4.0};
        Check(Fingerprint(Run(after_specs)) == Fingerprint(Run(before_specs)), "TS-unlisted: an unlisted validation point does not change anything");
    }
    {   // posePointIds feed the validation homography; measurementPointIds only feed the residual.
        auto pose_moved = base_specs;
        pose_moved[3].delta["P0_0"] = {1.0, 0};
        const auto p = Run(pose_moved);
        const auto* h_base = FindView(a, "V1");
        const auto* h_pose = FindView(p.cameras[0], "V1");
        bool changed = false;
        if (h_base && h_pose && h_base->chart_to_image && h_pose->chart_to_image) {
            for (std::size_t i = 0; i < 9; ++i) changed = changed || !Bitwise((*h_base->chart_to_image)[i], (*h_pose->chart_to_image)[i]);
        }
        Check(changed, "TS-pose: moving a pose point changes the validation homography");
        Check(Fingerprint(p.cameras[0], true) == Fingerprint(a, true), "TS-pose: and the training result is bitwise unchanged");
        auto measurement_moved = base_specs;
        measurement_moved[3].delta["P0_1"] = {1.0, 0};
        const auto m = Run(measurement_moved);
        const auto* h_measure = FindView(m.cameras[0], "V1");
        bool same = h_base && h_measure && h_base->chart_to_image && h_measure->chart_to_image;
        for (std::size_t i = 0; same && i < 9; ++i) same = Bitwise((*h_base->chart_to_image)[i], (*h_measure->chart_to_image)[i]);
        Check(same, "TS-measurement: moving a measurement point leaves the validation homography bitwise unchanged");
        Check(h_measure && h_measure->residual && h_measure->residual->max_pixels >= 0.99 && h_measure->residual->max_pixels <= 1.01,
            "TS-measurement: the moved point shows as a residual of about one pixel");
    }
}

void TestResidualsAreRecomputedIndependently() {
    auto specs = G3Views();
    specs[0].delta["P1_4"] = {0.25, 0};
    specs[3].delta["P0_1"] = {1.0, 0};
    const auto result = Run(specs);
    Invariants(result, "RS");
    const auto& a = result.cameras[0];
    Check(a.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated && a.intrinsics, "RS: the perturbed session still succeeds");
    if (!a.intrinsics) return;
    double squares = 0;
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto* view = FindView(a, specs[i].id);
        Check(view && view->pose, "RS: pose: " + specs[i].id);
        if (!view || !view->pose) continue;
        const auto want = ExpectedResidual(specs[i], specs[i].used, *a.intrinsics, *view->pose);
        Check(ResidualMatches(view->residual, want), "RS: the reported residual equals the recomputed one: " + specs[i].id);
        squares += want.rms * want.rms * want.n;
        count += want.n;
    }
    Check(a.training_residual && a.training_residual->point_count == count && Near(a.training_residual->rms_pixels, std::sqrt(squares / count)),
        "RS: the pooled training residual is sqrt(sum e^2 / N)");
    const auto* t1 = FindView(a, "T1");
    Check(t1 && t1->residual && t1->residual->max_pixels > 0.05 && t1->residual->max_pixels < 0.5 && t1->residual->rms_pixels > 0,
        "RS: a 0.25 px perturbation leaves a visible but bounded training residual");
    const auto* v1 = FindView(a, "V1");
    Check(v1 && v1->pose, "RS: validation pose");
    if (v1 && v1->pose) {
        const auto want = ExpectedResidual(specs[3], specs[3].measurement, *a.intrinsics, *v1->pose);
        Check(ResidualMatches(v1->residual, want), "RS: the validation residual equals the recomputed one");
        Check(ResidualMatches(a.held_out_residual, want), "RS: the pooled held-out residual equals it for one view");
    }
}

#ifdef NDEBUG
// One view with every point of a 128 x 128 chart: the largest view the session accepts.
void TestLargeViews() {
    const auto chart = Grid(128, 128, 1.8, -115, -115);
    std::vector<ViewSpec> views;
    views.push_back(FullView("L1", kPoseT1, kTrue, "CAM-A", "training", chart));
    views.push_back(FullView("L2", kPoseT2, kTrue, "CAM-A", "training", chart));
    views.push_back(FullView("L3", kPoseT3, kTrue, "CAM-A", "training", chart));
    const auto result = Run(chart, views);
    Invariants(result, "L1");
    const auto& a = result.cameras[0];
    Check(a.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated, "L1: 16384 points per view");
    Check(a.training_residual && a.training_residual->point_count == 3U * 16384U && a.training_residual->max_pixels <= 1e-6,
        "L1: pooled residual covers every point and is at most 1e-6 px");
}
#endif
} // namespace

int main() {
    try {
        TestPolicyAndCodes();
        TestSignature();
        TestFrontalAndSameTilt();
        TestDifferentTilts();
        TestInsufficient();
        TestViewGates();
        TestNonphysicalAndPose();
        TestCamerasAreIndependent();
        TestSeparationFromTruth();
        TestDeterminism();
        TestProbesAreIndependentOfTheSession();
        TestNumericalFailureStatuses();
        TestOneCameraFailsAndTheOtherSurvives();
        TestValidationViewFailuresDoNotStopTheCamera();
        TestChartBehindTheCamera();
        TestTruthSeparationReview();
        TestResidualsAreRecomputedIndependently();
#ifdef NDEBUG
        TestLargeViews();
#endif
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        ++failures;
    }
    std::cout << "calibration_intrinsics checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
