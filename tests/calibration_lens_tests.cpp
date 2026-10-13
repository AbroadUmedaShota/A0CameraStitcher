#include "a0/m2/calibration_lens.hpp"

#include "calibration_lens_model.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Contract test of the lens intrinsics stage (Issue #290).
// Every session is built here from numbers: the true intrinsics, the true
// Brown-Conrady coefficients and the true pose of each view project the chart
// points, and only the resulting pixels reach the estimator. The projection in
// this file is written independently of the library (a different arrangement of
// the same formula); the library's own functions are called directly only by
// the Jacobian and physicality tests. No file is read or written, and no
// generator, renderer or profile is involved. MSVC maps long double to double,
// so the fixture is binary64 throughout.
namespace {
using namespace a0::m2::calibration;
namespace ld = a0::m2::calibration::lens_detail;

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
struct Dist { double k1, k2, k3, p1, p2; };
constexpr Intrinsics kTrue{1000, 1040, 651.8, 470.8};
constexpr Dist kTrueDist{-0.1, 0.05, 0.01, 1e-3, -5e-4};
constexpr Dist kNoDist{0, 0, 0, 0, 0};
struct Pose { std::array<double, 9> r; Vec3 t; };

// Apply three independent axis rotations to a basis vector (same order as the
// stage 2 test), so the rotation columns are the rotated basis.
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
// pose -> perspective division -> distortion -> K. The radial polynomial is
// expanded term by term here, unlike the Horner form of the library.
Projected Project(const Intrinsics& k, const Dist& d, const std::array<double, 9>& r, const Vec3& t, double x, double y, double z = 0) {
    const double px = r[0] * x + r[1] * y + r[2] * z + t[0];
    const double py = r[3] * x + r[4] * y + r[5] * z + t[1];
    const double pz = r[6] * x + r[7] * y + r[8] * z + t[2];
    const double xn = px / pz, yn = py / pz;
    const double r2 = xn * xn + yn * yn;
    const double radial = 1.0 + d.k1 * r2 + d.k2 * r2 * r2 + d.k3 * r2 * r2 * r2;
    const double xd = xn * radial + 2.0 * d.p1 * xn * yn + d.p2 * (r2 + 2.0 * xn * xn);
    const double yd = yn * radial + d.p1 * (r2 + 2.0 * yn * yn) + 2.0 * d.p2 * xn * yn;
    return {k.fx * xd + k.cx, k.fy * yd + k.cy, pz};
}
Projected ProjectTruth(const Intrinsics& k, const Dist& d, const Pose& pose, double x, double y, double z = 0) {
    return Project(k, d, pose.r, pose.t, x, y, z);
}
Projected ProjectEstimate(const PinholeIntrinsics& k, const BrownConradyDistortion& d, const ChartPose& pose, double x, double y) {
    return Project({k.fx, k.fy, k.cx, k.cy}, {d.k1, d.k2, d.k3, d.p1, d.p2}, pose.rotation, pose.translation_mm, x, y);
}

struct ChartPt { std::string id; double x, y; };
std::vector<ChartPt> Grid(int rows, int columns, double pitch, double x0, double y0) {
    std::vector<ChartPt> points;
    for (int r = 0; r < rows; ++r) for (int c = 0; c < columns; ++c) {
        points.push_back({"P" + std::to_string(r) + "_" + std::to_string(c), x0 + pitch * c, y0 + pitch * r});
    }
    return points;
}
// 17 columns x 13 rows at 25 mm, 221 points.
std::vector<ChartPt> Chart() { return Grid(13, 17, 25, -200, -150); }
std::vector<std::string> Ids(const std::vector<ChartPt>& points, std::size_t begin = 0, std::size_t end = std::numeric_limits<std::size_t>::max()) {
    std::vector<std::string> ids;
    for (std::size_t i = begin; i < points.size() && i < end; ++i) ids.push_back(points[i].id);
    return ids;
}

// ---------------------------------------------------------------------------
// Seeded normal noise: SplitMix64 and Box-Muller, two values per pair of draws.

class Gauss {
public:
    explicit Gauss(std::uint64_t seed) : state_(seed) {}
    double Next() {
        if (spare_) { const double value = *spare_; spare_.reset(); return value; }
        double u1;
        do { u1 = Uniform(); } while (u1 == 0);
        const double u2 = Uniform();
        const double radius = std::sqrt(-2.0 * std::log(u1));
        const double angle = 2.0 * 3.14159265358979323846 * u2;
        spare_ = radius * std::sin(angle);
        return radius * std::cos(angle);
    }
private:
    double Uniform() {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) / 9007199254740992.0;
    }
    std::uint64_t state_;
    std::optional<double> spare_;
};

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
    Dist d = kTrueDist;
    Pose pose{};
    std::vector<ChartPt> observed;     // detected observations, in order
    std::vector<std::string> used;     // usedPointIds, or posePointIds for a validation view
    std::vector<std::string> measurement;
    std::map<std::string, std::pair<double, double>> delta; // added to the pixel of the named point
    std::map<std::string, std::pair<double, double>> pinned; // pixel written as given, for a point that cannot project into the image
    std::shared_ptr<Gauss> noise;      // drawn from for every observation when set: u then v
    double sigma = 0;
    // The pixel written for each point, kept so that the tests can recompute residuals from it.
    mutable std::map<std::string, std::pair<double, double>> rendered;
};

std::string RenderView(const ViewSpec& view, unsigned hash_number) {
    std::vector<std::string> observations;
    bool inside = true;
    view.rendered.clear();
    for (const auto& point : view.observed) {
        const auto projected = ProjectTruth(view.k, view.d, view.pose, point.x, point.y);
        double u = projected.u, v = projected.v;
        const auto pin = view.pinned.find(point.id);
        const bool is_pinned = pin != view.pinned.end();
        if (is_pinned) { u = pin->second.first; v = pin->second.second; }
        const auto found = view.delta.find(point.id);
        if (found != view.delta.end()) { u += found->second.first; v += found->second.second; }
        if (view.noise) {
            u += view.sigma * view.noise->Next();
            v += view.sigma * view.noise->Next();
        }
        inside = inside && u >= -0.5 && u <= kWidth - 0.5 && v >= -0.5 && v <= kHeight - 0.5 && (is_pinned || projected.z > 0);
        view.rendered[point.id] = {u, v};
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

const Pose kPoseT1 = MakePose(0.30, 0.10, 0.05, 10, -5, 520);
const Pose kPoseT2 = MakePose(-0.25, 0.30, -0.08, -15, 10, 540);
const Pose kPoseT3 = MakePose(0.20, -0.35, 0.12, 5, 8, 500);
const Pose kPoseT4 = MakePose(-0.30, -0.20, 0.20, 20, -10, 530);
const Pose kPoseT5 = MakePose(0.10, 0.40, -0.15, -20, 5, 560);
const Pose kPoseT6 = MakePose(0.35, -0.15, -0.20, 0, 15, 510);
const Pose kPoseV1 = MakePose(0.15, 0.20, -0.10, 0, 0, 540);
const Pose kPoseV2 = MakePose(-0.20, -0.25, 0.10, 10, -10, 520);

ViewSpec FullView(std::string id, const Pose& pose, std::string camera = "CAM-A", std::string role = "training",
    const std::vector<ChartPt>& chart = Chart(), Dist d = kTrueDist) {
    ViewSpec view;
    view.id = std::move(id);
    view.camera = std::move(camera);
    view.role = std::move(role);
    view.d = d;
    view.pose = pose;
    view.observed = chart;
    view.used = Ids(chart);
    return view;
}
ViewSpec Training(const std::string& id, const Pose& pose, const std::string& camera, Dist d) {
    return FullView(id, pose, camera, "training", Chart(), d);
}
// Even-numbered points pose the view, odd-numbered ones are measured.
ViewSpec Validation(const std::string& id, const Pose& pose, const std::string& camera, Dist d) {
    auto view = FullView(id, pose, camera, "validation", Chart(), d);
    view.used.clear();
    for (std::size_t i = 0; i < view.observed.size(); ++i) (i % 2 == 0 ? view.used : view.measurement).push_back(view.observed[i].id);
    return view;
}
// T1 .. T6, V1, V2, R1 of the test plan for one camera.
std::vector<ViewSpec> Layout(const std::string& camera = "CAM-A", Dist d = kTrueDist) {
    std::vector<ViewSpec> views;
    views.push_back(Training("T1", kPoseT1, camera, d));
    views.back().used = Ids(views.back().observed, 0, 216); // the last five stay detected but unused
    views.push_back(Training("T2", kPoseT2, camera, d));
    views.push_back(Training("T3", kPoseT3, camera, d));
    views.push_back(Training("T4", kPoseT4, camera, d));
    views.push_back(Training("T5", kPoseT5, camera, d));
    views.push_back(Training("T6", kPoseT6, camera, d));
    views.push_back(Validation("V1", kPoseV1, camera, d));
    views.push_back(Validation("V2", kPoseV2, camera, d));
    views.push_back(FullView("R1", MakePose(0, 0, 0, 0, 0, 540), camera, "reference", Chart(), d));
    return views;
}
const std::array<Vec3, 3> kFrontalTranslations{{{0, 0, 700}, {30, -20, 650}, {-25, 15, 800}}};
std::vector<ViewSpec> Frontal(const std::string& prefix, const std::string& camera = "CAM-A", Dist d = kTrueDist) {
    std::vector<ViewSpec> views;
    int index = 1;
    for (const auto& t : kFrontalTranslations) {
        views.push_back(Training(prefix + std::to_string(index++), MakePose(0, 0, 0, t[0], t[1], t[2]), camera, d));
    }
    return views;
}

LensIntrinsicsPolicy Policy() {
    return {2, 1e-2, ObservationModel::DistortedAnalytic, 100, 1e-10, 1e-12, 1e-3};
}
UndistortedIntrinsicsPolicy Stage2Policy() {
    return {2, 1e-2, ObservationModel::UndistortedAnalytic};
}
LensIntrinsicsResult Run(const std::vector<ChartPt>& chart, const std::vector<ViewSpec>& views, const LensIntrinsicsPolicy& policy = Policy()) {
    return EstimateLensIntrinsics(MakeSession(chart, views), policy);
}
LensIntrinsicsResult Run(const std::vector<ViewSpec>& views, const LensIntrinsicsPolicy& policy = Policy()) {
    return Run(Chart(), views, policy);
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
void PutPose(std::string& out, const std::optional<ChartPose>& pose) {
    if (pose) { PutArray(out, pose->rotation); PutArray(out, pose->translation_mm); } else out += "pose:- ";
}
void PutSolver(std::string& out, const std::optional<SolverRecord>& solver) {
    if (!solver) { out += "solver:- "; return; }
    out += "solver:" + std::to_string(solver->iterations) + ' ' + std::to_string(solver->accepted_steps) + ' '
        + std::string(SolverStopReasonCode(solver->stop_reason)) + ' ';
    PutDouble(out, solver->initial_cost);
    PutDouble(out, solver->final_cost);
}
// Stage 2 result, as embedded in `initialization`.
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
void PutStage2View(std::string& out, const IntrinsicsViewResult& view) {
    out += "view:" + view.view_id + ' ' + std::string(ViewRoleCode(view.role)) + ' ' + std::string(ViewOutcomeCode(view.outcome)) + ' ';
    if (view.chart_to_image) PutArray(out, *view.chart_to_image); else out += "h:- ";
    PutPose(out, view.pose);
    PutResidual(out, view.residual);
}
std::string Stage2Fingerprint(const CameraIntrinsicsResult& camera, bool training_only = false) {
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
        PutStage2View(out, view);
    }
    PutResidual(out, camera.training_residual);
    if (!training_only) {
        out += std::string(HeldOutCoverageCode(camera.held_out_coverage)) + ' ';
        PutResidual(out, camera.held_out_residual);
    }
    return out;
}
// training_only leaves out the validation views and the held-out part, here and in stage 2.
std::string Fingerprint(const CameraLensResult& camera, bool training_only = false) {
    std::string out = std::string(CameraAliasCode(camera.camera)) + ' ' + std::string(LensStatusCode(camera.status)) + ' ';
    out += "init[" + Stage2Fingerprint(camera.initialization, training_only) + "] ";
    PutSolver(out, camera.solver);
    if (camera.intrinsics) {
        PutDouble(out, camera.intrinsics->fx);
        PutDouble(out, camera.intrinsics->fy);
        PutDouble(out, camera.intrinsics->cx);
        PutDouble(out, camera.intrinsics->cy);
    } else {
        out += "k:- ";
    }
    if (camera.distortion) {
        for (const auto value : {camera.distortion->k1, camera.distortion->k2, camera.distortion->k3, camera.distortion->p1, camera.distortion->p2}) PutDouble(out, value);
    } else {
        out += "d:- ";
    }
    if (camera.distortion_check) {
        out += "check:";
        PutDouble(out, camera.distortion_check->max_radius_squared);
        PutDouble(out, camera.distortion_check->minimum_radial_slope);
        PutDouble(out, camera.distortion_check->minimum_jacobian_determinant);
    } else {
        out += "check:- ";
    }
    for (const auto& view : camera.views) {
        if (training_only && view.role != ViewRole::Training) continue;
        out += "lensview:" + view.view_id + ' ' + std::string(ViewRoleCode(view.role)) + ' ' + std::string(LensViewOutcomeCode(view.outcome)) + ' ';
        PutPose(out, view.pose);
        PutResidual(out, view.residual);
        PutSolver(out, view.solver);
    }
    PutResidual(out, camera.training_residual);
    if (!training_only) {
        out += std::string(HeldOutCoverageCode(camera.held_out_coverage)) + ' ';
        PutResidual(out, camera.held_out_residual);
    }
    return out;
}
std::string Fingerprint(const LensIntrinsicsResult& result) {
    return std::string(ObservationModelCode(result.observation_model)) + '|' + std::string(DistortionModelCode(result.distortion_model)) + '|'
        + Fingerprint(result.cameras[0]) + '|' + Fingerprint(result.cameras[1]);
}

// ---------------------------------------------------------------------------
// Invariants that hold for every result.

const LensViewResult* FindView(const CameraLensResult& camera, const std::string& id) {
    for (const auto& view : camera.views) if (view.view_id == id) return &view;
    return nullptr;
}
bool ValidSummary(const std::optional<ResidualSummary>& summary) {
    if (!summary) return true;
    return summary->point_count >= 1 && std::isfinite(summary->rms_pixels) && std::isfinite(summary->max_pixels)
        && summary->rms_pixels >= 0 && summary->max_pixels >= 0
        && summary->rms_pixels <= summary->max_pixels * (1 + 1e-12) + 1e-300;
}
bool IsRotation(const std::array<double, 9>& r) {
    for (std::size_t i = 0; i < 3; ++i) for (std::size_t j = 0; j < 3; ++j) {
        double dot = 0;
        for (std::size_t k = 0; k < 3; ++k) dot += r[k * 3 + i] * r[k * 3 + j];
        if (!(std::abs(dot - (i == j ? 1.0 : 0.0)) <= 1e-9)) return false;
    }
    const double det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) + r[2] * (r[3] * r[7] - r[4] * r[6]);
    return std::abs(det - 1) <= 1e-9;
}
bool Near(double a, double b, double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance * (1 + std::max(std::abs(a), std::abs(b)));
}
bool Bitwise(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }

bool ConvergentStop(SolverStopReason reason) {
    return reason == SolverStopReason::RelativeCostReduction || reason == SolverStopReason::RelativeStepSize;
}
void CheckSolver(const SolverRecord& solver, const LensIntrinsicsPolicy& policy, const std::string& name) {
    Check(solver.iterations >= solver.accepted_steps, name + ": iterations >= accepted steps");
    Check(solver.iterations <= policy.max_iterations, name + ": iterations <= max_iterations");
    Check(std::isfinite(solver.initial_cost) && std::isfinite(solver.final_cost) && solver.initial_cost >= 0 && solver.final_cost >= 0,
        name + ": costs are finite and non-negative");
    Check(solver.final_cost <= solver.initial_cost, name + ": the cost never increases");
    Check(solver.accepted_steps > 0 || solver.final_cost == solver.initial_cost, name + ": without an accepted step the final cost is the initial cost");
    Check(!SolverStopReasonCode(solver.stop_reason).empty(), name + ": stop reason has a code");
    Check(solver.stop_reason != SolverStopReason::MaximumIterations || solver.iterations == policy.max_iterations,
        name + ": maximum-iterations means every allowed trial was used");
}
void Invariants(const LensIntrinsicsResult& result, const LensIntrinsicsPolicy& policy, const std::string& label) {
    Check(result.observation_model == ObservationModel::DistortedAnalytic, label + ": the model is echoed");
    Check(result.distortion_model == DistortionModel::BrownConradyK1K2K3P1P2, label + ": the distortion model is echoed");
    Check(result.cameras[0].camera == CameraAlias::CamA && result.cameras[1].camera == CameraAlias::CamB, label + ": cameras are [CAM-A, CAM-B]");
    for (const auto& camera : result.cameras) {
        const auto name = label + "/" + std::string(CameraAliasCode(camera.camera));
        const auto status = camera.status;
        const bool estimated = status == LensStatus::LensIntrinsicsEstimated;
        const bool init_failed = status == LensStatus::InitializationUnavailable;
        const bool past_solver = status == LensStatus::NonphysicalIntrinsics || status == LensStatus::InvalidDistortion
            || status == LensStatus::PoseUnavailable || estimated;
        const bool has_intrinsics = status == LensStatus::InvalidDistortion || status == LensStatus::PoseUnavailable || estimated;
        const bool has_distortion = status == LensStatus::PoseUnavailable || estimated;
        Check(!LensStatusCode(status).empty(), name + ": status has a code");
        Check(FailedStage(status).has_value() == !estimated, name + ": stage is null exactly for the success status");
        Check(camera.initialization.camera == camera.camera, name + ": the initialization is the stage 2 result of this camera");
        Check((camera.initialization.status == IntrinsicsStatus::UndistortedIntrinsicsEstimated) == !init_failed,
            name + ": the initialization is estimated exactly when the lens stage got past it");
        Check(camera.solver.has_value() == !init_failed, name + ": a solver record exists exactly when the solver ran");
        Check(camera.intrinsics.has_value() == has_intrinsics, name + ": intrinsics exist exactly from the distortion stage on");
        Check(camera.distortion_check.has_value() == has_intrinsics, name + ": the distortion check exists exactly with the intrinsics");
        Check(camera.distortion.has_value() == has_distortion, name + ": distortion exists exactly after the distortion stage");
        Check(camera.training_residual.has_value() == estimated, name + ": the training residual exists exactly on success");
        Check(ValidSummary(camera.training_residual) && ValidSummary(camera.held_out_residual), name + ": pooled residuals are valid summaries");
        if (camera.solver) {
            CheckSolver(*camera.solver, policy, name + " solver");
            const auto reason = camera.solver->stop_reason;
            Check((status == LensStatus::SolverNonconvergence) == (reason == SolverStopReason::MaximumIterations), name + ": nonconvergence is the iteration limit");
            Check((status == LensStatus::NumericalFailure) == (reason == SolverStopReason::NumericalFailure), name + ": numerical-failure is the solver failure");
            Check(past_solver == ConvergentStop(reason), name + ": a camera gets past the solver exactly on a convergent stop");
        }
        if (camera.intrinsics) {
            const auto& k = camera.intrinsics;
            Check(std::isfinite(k->fx) && std::isfinite(k->fy) && std::isfinite(k->cx) && std::isfinite(k->cy) && k->fx > 0 && k->fy > 0,
                name + ": the reported intrinsics are physical and finite");
        }
        if (camera.distortion) {
            const auto& d = *camera.distortion;
            Check(std::isfinite(d.k1) && std::isfinite(d.k2) && std::isfinite(d.k3) && std::isfinite(d.p1) && std::isfinite(d.p2), name + ": the coefficients are finite");
        }
        if (camera.distortion_check) {
            const auto& c = *camera.distortion_check;
            Check(std::isfinite(c.max_radius_squared) && std::isfinite(c.minimum_radial_slope) && std::isfinite(c.minimum_jacobian_determinant) && c.max_radius_squared >= 0,
                name + ": the distortion check is finite");
            const bool physical = c.minimum_radial_slope > 0 && c.minimum_jacobian_determinant > 0;
            Check(physical == (status != LensStatus::InvalidDistortion), name + ": invalid-distortion exactly when a check value is not positive");
        }
        // Views: the same list, in the same order, as the stage 2 result.
        Check(camera.views.size() == camera.initialization.views.size(), name + ": the view list has the length of the stage 2 list");
        std::uint32_t training_points = 0;
        double training_squares = 0, training_max = 0, held_squares = 0, held_max = 0;
        std::uint32_t held_points = 0;
        bool any_validation_evaluated = false, all_training_evaluated = true, all_validation_evaluated = true;
        for (std::size_t i = 0; i < camera.views.size() && i < camera.initialization.views.size(); ++i) {
            const auto& view = camera.views[i];
            const auto& base = camera.initialization.views[i];
            Check(view.view_id == base.view_id && view.role == base.role, name + ": view order and role follow the session: " + view.view_id);
            Check(view.role == ViewRole::Training || view.role == ViewRole::Validation, name + ": reference views are not listed: " + view.view_id);
            Check(!LensViewOutcomeCode(view.outcome).empty(), name + ": view outcome has a code");
            Check(ValidSummary(view.residual), name + ": view residual is a valid summary: " + view.view_id);
            if (view.pose) Check(IsRotation(view.pose->rotation), name + ": the pose rotation is orthonormal with determinant 1: " + view.view_id);
            if (view.pose) {
                bool finite = true;
                for (const auto value : view.pose->rotation) finite = finite && std::isfinite(value);
                for (const auto value : view.pose->translation_mm) finite = finite && std::isfinite(value);
                Check(finite, name + ": the pose is finite: " + view.view_id);
            }
            Check(view.outcome == LensViewOutcome::Evaluated ? (view.pose && view.residual) : (!view.pose && !view.residual),
                name + ": pose and residual exist exactly for an evaluated view: " + view.view_id);
            if (view.role == ViewRole::Training) {
                Check(!view.solver, name + ": a training view has no solver record of its own: " + view.view_id);
                if (view.outcome == LensViewOutcome::Evaluated) {
                    Check(has_distortion, name + ": a training view is evaluated only after the distortion stage: " + view.view_id);
                    training_points += view.residual->point_count;
                    training_squares += view.residual->rms_pixels * view.residual->rms_pixels * view.residual->point_count;
                    training_max = std::max(training_max, view.residual->max_pixels);
                } else {
                    all_training_evaluated = false;
                    if (!has_distortion) Check(view.outcome == LensViewOutcome::NotReached, name + ": a stopped camera does not reach its training views: " + view.view_id);
                }
            } else {
                if (!estimated) Check(view.outcome == LensViewOutcome::NotReached && !view.solver, name + ": a validation view is not reached: " + view.view_id);
                if (view.outcome == LensViewOutcome::Evaluated) {
                    any_validation_evaluated = true;
                    Check(view.solver && ConvergentStop(view.solver->stop_reason), name + ": an evaluated validation view converged: " + view.view_id);
                    held_points += view.residual->point_count;
                    held_squares += view.residual->rms_pixels * view.residual->rms_pixels * view.residual->point_count;
                    held_max = std::max(held_max, view.residual->max_pixels);
                } else {
                    all_validation_evaluated = false;
                }
                if (view.solver) CheckSolver(*view.solver, policy, name + " validation solver " + view.view_id);
                if (view.outcome == LensViewOutcome::SolverNonconvergence) Check(view.solver && view.solver->stop_reason == SolverStopReason::MaximumIterations, name + ": validation nonconvergence is the iteration limit");
                if (view.outcome == LensViewOutcome::InitializationUnavailable) Check(!view.solver, name + ": an unusable start has no solver record");
            }
        }
        if (!estimated) {
            Check(camera.held_out_coverage == HeldOutCoverage::NotReached && !camera.held_out_residual, name + ": a stopped camera has no held-out result");
        } else {
            Check(!camera.views.empty() && all_training_evaluated, name + ": every training view is evaluated on success");
            Check(camera.held_out_coverage != HeldOutCoverage::NotReached, name + ": a successful camera reports the held-out coverage");
            Check(camera.held_out_coverage != HeldOutCoverage::AllViewsEvaluated || (all_validation_evaluated && camera.held_out_residual),
                name + ": all validation views evaluated means a held-out residual");
            Check(camera.held_out_coverage != HeldOutCoverage::NoValidationView || !camera.held_out_residual, name + ": no validation view means no held-out residual");
            Check(camera.held_out_residual.has_value() == any_validation_evaluated, name + ": the held-out residual exists exactly when a validation view was evaluated");
            // Pooled residuals follow from the per-view summaries.
            if (camera.training_residual) {
                Check(camera.training_residual->point_count == training_points, name + ": the pooled training count is the sum of the views");
                Check(Near(camera.training_residual->rms_pixels * camera.training_residual->rms_pixels * training_points, training_squares, 1e-12),
                    name + ": the pooled training rms is sqrt(sum e^2 / N)");
                Check(Bitwise(camera.training_residual->max_pixels, training_max), name + ": the pooled training max is the largest view max");
                if (camera.solver) {
                    Check(Near(camera.solver->final_cost, training_squares, 1e-12), name + ": final_cost equals the sum of the squared training errors");
                }
            }
            if (camera.held_out_residual) {
                Check(camera.held_out_residual->point_count == held_points, name + ": the pooled held-out count is the sum of the views");
                Check(Near(camera.held_out_residual->rms_pixels * camera.held_out_residual->rms_pixels * held_points, held_squares, 1e-12),
                    name + ": the pooled held-out rms is sqrt(sum e^2 / N)");
                Check(Bitwise(camera.held_out_residual->max_pixels, held_max), name + ": the pooled held-out max is the largest view max");
            }
        }
        if (status == LensStatus::PoseUnavailable) Check(has_distortion && !camera.training_residual, name + ": pose-unavailable keeps the lens and has no pooled residual");
    }
}

// ---------------------------------------------------------------------------
// Independent probe points and residuals.

struct Probe { double x, y; };
std::vector<Probe> Probes() { // half a pitch off the chart grid, inside it
    std::vector<Probe> probes;
    for (int r = 0; r < 12; ++r) for (int c = 0; c < 16; ++c) probes.push_back({-187.5 + 25.0 * c, -137.5 + 25.0 * r});
    return probes;
}
// Pixel difference between the estimate and the truth over the probes, through the pose of one view.
struct ProbeStats { double max; double squares; std::size_t count; };
ProbeStats ProbeDifference(const CameraLensResult& camera, const ViewSpec& truth) {
    const auto* view = FindView(camera, truth.id);
    if (!view || !view->pose || !camera.intrinsics || !camera.distortion) return {std::numeric_limits<double>::infinity(), 0, 0};
    ProbeStats stats{0, 0, 0};
    for (const auto& probe : Probes()) {
        const auto estimate = ProjectEstimate(*camera.intrinsics, *camera.distortion, *view->pose, probe.x, probe.y);
        const auto exact = ProjectTruth(truth.k, truth.d, truth.pose, probe.x, probe.y);
        const double e = std::hypot(estimate.u - exact.u, estimate.v - exact.v);
        stats.max = std::max(stats.max, e);
        stats.squares += e * e;
        ++stats.count;
    }
    return stats;
}
double WorstProbe(const CameraLensResult& camera, const std::vector<ViewSpec>& specs, const std::vector<std::string>& ids) {
    double worst = 0;
    for (const auto& id : ids) {
        for (const auto& spec : specs) if (spec.id == id) worst = std::max(worst, ProbeDifference(camera, spec).max);
    }
    return worst;
}

struct Expected { double rms; double max; std::uint32_t n; };
// Residual of the named points of a view against the pixels that were written, through the reported lens and pose.
Expected ExpectedResidual(const ViewSpec& spec, const std::vector<std::string>& ids, const std::vector<ChartPt>& chart,
    const PinholeIntrinsics& k, const BrownConradyDistortion& d, const ChartPose& pose) {
    double squares = 0, maximum = 0;
    std::uint32_t n = 0;
    for (const auto& id : ids) {
        const auto point = std::find_if(chart.begin(), chart.end(), [&](const ChartPt& p) { return p.id == id; });
        const auto pixel = spec.rendered.find(id);
        if (point == chart.end() || pixel == spec.rendered.end()) continue;
        const auto projected = ProjectEstimate(k, d, pose, point->x, point->y);
        const double e = std::hypot(projected.u - pixel->second.first, projected.v - pixel->second.second);
        squares += e * e;
        maximum = std::max(maximum, e);
        ++n;
    }
    return {n ? std::sqrt(squares / n) : 0, maximum, n};
}
bool ResidualMatches(const std::optional<ResidualSummary>& got, const Expected& want) {
    return got && got->point_count == want.n && Near(got->rms_pixels, want.rms) && Near(got->max_pixels, want.max);
}

// ---------------------------------------------------------------------------
// Tests.

void TestPolicyAndCodes() {
    const auto session = MakeSession(Chart(), {Training("T1", kPoseT1, "CAM-A", kTrueDist), Training("T2", kPoseT2, "CAM-A", kTrueDist)});
    const auto reject = [&](LensIntrinsicsPolicy policy, const std::string& label) {
        Rejects([&] { (void)EstimateLensIntrinsics(session, policy); }, "A0: " + label);
    };
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    for (const auto minimum : {0u, 1u, 129u, 4294967295u}) { auto p = Policy(); p.minimum_views = minimum; reject(p, "minimum_views " + std::to_string(minimum)); }
    for (const double cutoff : {0.0, 1.0, -1e-2, 2.0, nan, inf, -inf}) { auto p = Policy(); p.relative_singular_cutoff = cutoff; reject(p, "cutoff " + Num(cutoff)); }
    for (const auto model : {ObservationModel::UndistortedAnalytic, static_cast<ObservationModel>(1), static_cast<ObservationModel>(3), static_cast<ObservationModel>(99)}) {
        auto p = Policy(); p.observation_model = model; reject(p, "observation model " + std::to_string(static_cast<int>(model)));
    }
    for (const auto limit : {0u, 1001u, 4294967295u}) { auto p = Policy(); p.max_iterations = limit; reject(p, "max_iterations " + std::to_string(limit)); }
    for (const double tolerance : {0.0, 1.0, -1e-10, 2.0, nan, inf, -inf}) {
        auto cost = Policy(); cost.relative_cost_tolerance = tolerance; reject(cost, "cost tolerance " + Num(tolerance));
        auto step = Policy(); step.relative_step_tolerance = tolerance; reject(step, "step tolerance " + Num(tolerance));
    }
    for (const double damping : {0.0, -1e-3, nan, inf, -inf}) { auto p = Policy(); p.initial_damping = damping; reject(p, "damping " + Num(damping)); }
    try {
        auto p = Policy(); p.max_iterations = 0;
        (void)EstimateLensIntrinsics(session, p);
    } catch (const std::invalid_argument& error) {
        Check(std::string(error.what()) == "lens intrinsics: invalid policy", "A0: the message names the category only");
    }
    // The edges of the accepted range are accepted (the two-view session stops in stage 2, so these runs are cheap).
    const auto accepts = [&](LensIntrinsicsPolicy policy, const std::string& label) {
        bool threw = false;
        try { (void)EstimateLensIntrinsics(session, policy); } catch (const std::invalid_argument&) { threw = true; }
        Check(!threw, "A0: accepted: " + label);
    };
    const double tiny = std::nextafter(0.0, 1.0), below_one = std::nextafter(1.0, 0.0);
    { auto p = Policy(); p.minimum_views = 2; accepts(p, "minimum_views 2"); }
    { auto p = Policy(); p.minimum_views = 128; accepts(p, "minimum_views 128"); }
    { auto p = Policy(); p.max_iterations = 1; accepts(p, "max_iterations 1"); }
    { auto p = Policy(); p.max_iterations = 1000; accepts(p, "max_iterations 1000"); }
    { auto p = Policy(); p.relative_singular_cutoff = tiny; accepts(p, "smallest positive cutoff"); }
    { auto p = Policy(); p.relative_singular_cutoff = below_one; accepts(p, "largest cutoff below one"); }
    { auto p = Policy(); p.relative_cost_tolerance = tiny; p.relative_step_tolerance = below_one; accepts(p, "extreme tolerances"); }
    { auto p = Policy(); p.relative_cost_tolerance = below_one; p.relative_step_tolerance = tiny; accepts(p, "swapped extreme tolerances"); }
    { auto p = Policy(); p.initial_damping = tiny; accepts(p, "smallest positive damping"); }
    { auto p = Policy(); p.initial_damping = std::numeric_limits<double>::max(); accepts(p, "largest damping"); }

    // Codes.
    Check(static_cast<int>(ObservationModel::UndistortedAnalytic) == 0 && static_cast<int>(ObservationModel::DistortedAnalytic) == 2,
        "code: observation model values; 1 is never assigned");
    Check(ObservationModelCode(ObservationModel::DistortedAnalytic) == "distorted-analytic", "code: distorted-analytic");
    Check(ObservationModelCode(ObservationModel::UndistortedAnalytic) == "undistorted-analytic", "code: undistorted-analytic");
    Check(ObservationModelCode(static_cast<ObservationModel>(1)).empty() && ObservationModelCode(static_cast<ObservationModel>(99)).empty(),
        "code: observation model out of range is empty");
    Check(DistortionModelCode(DistortionModel::BrownConradyK1K2K3P1P2) == "brown-conrady-k1k2k3-p1p2" && DistortionModelCode(static_cast<DistortionModel>(99)).empty(),
        "code: distortion model");
    const std::vector<std::pair<LensStatus, std::string_view>> statuses{
        {LensStatus::LensIntrinsicsEstimated, "lens-intrinsics-estimated"},
        {LensStatus::InitializationUnavailable, "initialization-unavailable"},
        {LensStatus::SolverNonconvergence, "solver-nonconvergence"},
        {LensStatus::NumericalFailure, "numerical-failure"},
        {LensStatus::NonphysicalIntrinsics, "nonphysical-intrinsics"},
        {LensStatus::InvalidDistortion, "invalid-distortion"},
        {LensStatus::PoseUnavailable, "pose-unavailable"}};
    const std::vector<std::optional<LensStage>> stages{std::nullopt, LensStage::Initialization, LensStage::Solver, LensStage::Solver,
        LensStage::Intrinsics, LensStage::Distortion, LensStage::Pose};
    Check(statuses.size() == 7, "code: seven statuses");
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        Check(LensStatusCode(statuses[i].first) == statuses[i].second, "code: status " + std::string(statuses[i].second));
        Check(FailedStage(statuses[i].first) == stages[i], "code: failed stage of " + std::string(statuses[i].second));
    }
    Check(LensStatusCode(static_cast<LensStatus>(99)).empty() && !FailedStage(static_cast<LensStatus>(99)), "code: status out of range is empty");
    Check(LensStageCode(LensStage::Initialization) == "initialization" && LensStageCode(LensStage::Solver) == "solver"
        && LensStageCode(LensStage::Intrinsics) == "intrinsics" && LensStageCode(LensStage::Distortion) == "distortion"
        && LensStageCode(LensStage::Pose) == "pose" && LensStageCode(static_cast<LensStage>(99)).empty(), "code: stages");
    Check(SolverStopReasonCode(SolverStopReason::RelativeCostReduction) == "relative-cost-reduction" && SolverStopReasonCode(SolverStopReason::RelativeStepSize) == "relative-step-size"
        && SolverStopReasonCode(SolverStopReason::MaximumIterations) == "maximum-iterations" && SolverStopReasonCode(SolverStopReason::NumericalFailure) == "numerical-failure"
        && SolverStopReasonCode(static_cast<SolverStopReason>(99)).empty(), "code: stop reasons");
    Check(LensViewOutcomeCode(LensViewOutcome::Evaluated) == "evaluated" && LensViewOutcomeCode(LensViewOutcome::InitializationUnavailable) == "initialization-unavailable"
        && LensViewOutcomeCode(LensViewOutcome::SolverNonconvergence) == "solver-nonconvergence" && LensViewOutcomeCode(LensViewOutcome::NumericalFailure) == "numerical-failure"
        && LensViewOutcomeCode(LensViewOutcome::ChartNotInFront) == "chart-not-in-front" && LensViewOutcomeCode(LensViewOutcome::NotReached) == "not-reached"
        && LensViewOutcomeCode(static_cast<LensViewOutcome>(99)).empty(), "code: view outcomes");
}

void TestSignature() {
    static_assert(std::is_same_v<decltype(&EstimateLensIntrinsics),
        LensIntrinsicsResult (*)(const CalibrationObservationSession&, const LensIntrinsicsPolicy&)>,
        "the API takes the session and the policy and nothing else");
    const LensIntrinsicsPolicy policy = Policy();
    const auto& [minimum, cutoff, model, iterations, cost_tolerance, step_tolerance, damping] = policy; // exactly seven fields, no hidden default
    Check(minimum == 2 && cutoff == 1e-2 && model == ObservationModel::DistortedAnalytic && iterations == 100 && cost_tolerance == 1e-10
        && step_tolerance == 1e-12 && damping == 1e-3, "S-a: the policy has exactly seven fields");
}

// E1: noise-free, distorted.
void TestNoiseFree() {
    const auto policy = Policy();
    const auto specs = Layout();
    const auto result = Run(specs, policy);
    Invariants(result, policy, "E1");
    const auto& a = result.cameras[0];
    Check(a.status == LensStatus::LensIntrinsicsEstimated, "E1: CAM-A is estimated");
    Check(a.solver && ConvergentStop(a.solver->stop_reason), "E1: the solver stopped on a convergence rule");
    Check(a.solver && a.solver->initial_cost > 1.0 && a.solver->final_cost < 1e-12, "E1: the cost falls from the closed-form start to the rounding floor");
    Check(a.intrinsics && a.distortion && a.distortion_check, "E1: K, coefficients and check are present");
    Check(a.distortion_check && a.distortion_check->minimum_radial_slope > 0 && a.distortion_check->minimum_jacobian_determinant > 0, "E1: the check passed");
    Check(a.distortion_check && a.distortion_check->max_radius_squared > 0.2 && a.distortion_check->max_radius_squared < 0.4, "E1: s_max is about 0.3");
    Check(a.training_residual && a.training_residual->point_count == 5U * 221U + 216U, "E1: the used points are counted (the unused five of T1 are not)");
    Check(a.training_residual && a.training_residual->max_pixels <= 1e-6, "E1: the training residual is at the rounding floor");
    // Points that are not in the session: the estimated map reproduces the truth through each training pose.
    for (const auto& id : {"T1", "T2", "T3", "T4", "T5", "T6"}) {
        for (const auto& spec : specs) if (spec.id == id) {
            const auto stats = ProbeDifference(a, spec);
            Check(stats.count == 192 && stats.max <= 1e-6, std::string("E1: probe difference of ") + id + " is at most 1e-6 px");
        }
    }
    Check(a.held_out_coverage == HeldOutCoverage::AllViewsEvaluated, "E1: both validation views are evaluated");
    for (const auto& id : {"V1", "V2"}) {
        const auto* view = FindView(a, id);
        Check(view && view->outcome == LensViewOutcome::Evaluated && view->residual && view->residual->point_count == 110 && view->residual->max_pixels <= 1e-6,
            std::string("E1: the measurement residual of ") + id + " is at most 1e-6 px over 110 points");
        Check(view && view->solver && ConvergentStop(view->solver->stop_reason), std::string("E1: the held-out pose of ") + id + " converged");
        // The held-out pose is the truth as well (not only a pose that happens to fit the measurement points).
        for (const auto& spec : specs) if (spec.id == id) {
            const auto stats = ProbeDifference(a, spec);
            Check(stats.count == 192 && stats.max <= 1e-6, std::string("E1: the held-out pose of ") + id + " reproduces the truth at points that are not in the session");
        }
    }
    Check(a.held_out_residual && a.held_out_residual->point_count == 220 && a.held_out_residual->max_pixels <= 1e-6, "E1: the pooled held-out residual covers 220 points");
    Check(a.views.size() == 8 && FindView(a, "R1") == nullptr, "E1: eight views are listed and the reference is not");
    // CAM-B has no view at all.
    Check(result.cameras[1].status == LensStatus::InitializationUnavailable && !result.cameras[1].solver && result.cameras[1].views.empty(),
        "E1: CAM-B without views stops in the initialization");
    // Information only: how far the coefficients are from the truth.
    if (a.distortion && a.intrinsics) {
        std::printf("info E1: iterations %u accepted %u  dk1 %.3e dk2 %.3e dk3 %.3e dp1 %.3e dp2 %.3e  dfx %.3e dfy %.3e dcx %.3e dcy %.3e\n",
            a.solver->iterations, a.solver->accepted_steps, a.distortion->k1 - kTrueDist.k1, a.distortion->k2 - kTrueDist.k2, a.distortion->k3 - kTrueDist.k3,
            a.distortion->p1 - kTrueDist.p1, a.distortion->p2 - kTrueDist.p2, a.intrinsics->fx - kTrue.fx, a.intrinsics->fy - kTrue.fy,
            a.intrinsics->cx - kTrue.cx, a.intrinsics->cy - kTrue.cy);
        for (const auto& id : {"V1", "V2"}) {
            const auto* view = FindView(a, id);
            if (view && view->solver) std::printf("info E1: held-out %s iterations %u accepted %u\n", id, view->solver->iterations, view->solver->accepted_steps);
        }
    }
}

// A2: the singular-value cutoff of the closed-form start has to suit distorted pixels.
// The pixels of a distorted view do not satisfy Zhang's constraints, so the sixth relative singular
// value is far above the rounding floor; a cutoff as small as the undistorted tests use calls it rank 6.
void TestCutoffForDistortedPixels() {
    const auto specs = Layout();
    const auto policy = Policy();
    const auto base = Run(specs, policy);
    const auto& geometry = base.cameras[0].initialization.view_geometry;
    Check(geometry && geometry->singular_values && geometry->numerical_rank && *geometry->numerical_rank == 5, "A2: the cutoff 1e-2 gives rank 5 for the distorted pixels");
    if (geometry && geometry->singular_values) {
        const auto& values = *geometry->singular_values;
        const double fifth = values[4] / values[0], sixth = values[5] / values[0];
        std::printf("info A2: relative singular values fifth %.3e sixth %.3e\n", fifth, sixth);
        Check(fifth >= 5e-2, "A2: the fifth relative singular value is at least 5 cutoffs above the cutoff");
        Check(sixth > 1e-4 && sixth <= 2e-3, "A2: the sixth is above 1e-4 (rank 6 at that cutoff) and at least 5 cutoffs below 1e-2");
    }
    auto strict = Policy();
    strict.relative_singular_cutoff = 1e-4;
    const auto result = Run(specs, strict);
    Invariants(result, strict, "A2");
    const auto& a = result.cameras[0];
    Check(a.status == LensStatus::InitializationUnavailable && a.initialization.status == IntrinsicsStatus::InconsistentLinearConstraints,
        "A2: the cutoff 1e-4 stops in the initialization with inconsistent-linear-constraints");
    Check(a.initialization.view_geometry && a.initialization.view_geometry->numerical_rank && *a.initialization.view_geometry->numerical_rank == 6, "A2: that is rank 6");
}

// RS: every residual is recomputed here from the pixels that were written.
void TestResidualsAreRecomputedIndependently() {
    auto specs = Layout();
    specs[0].delta["P1_4"] = {0.25, 0};   // T1, a used point
    specs[3].delta["P2_5"] = {0, -0.4};   // T4, a used point
    specs[6].delta["P0_3"] = {1.0, 0};    // V1, index 3: a measurement point
    specs[7].delta["P0_4"] = {0.5, 0.5};  // V2, index 4: a pose point
    const auto policy = Policy();
    const auto chart = Chart();
    const auto session = MakeSession(chart, specs);
    const auto result = EstimateLensIntrinsics(session, policy);
    Invariants(result, policy, "RS");
    const auto& a = result.cameras[0];
    Check(a.status == LensStatus::LensIntrinsicsEstimated && a.intrinsics && a.distortion, "RS: the perturbed session still succeeds");
    if (!a.intrinsics || !a.distortion) return;
    double squares = 0, maximum = 0;
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        const auto* view = FindView(a, specs[i].id);
        Check(view && view->pose, "RS: pose: " + specs[i].id);
        if (!view || !view->pose) continue;
        const auto want = ExpectedResidual(specs[i], specs[i].used, chart, *a.intrinsics, *a.distortion, *view->pose);
        Check(ResidualMatches(view->residual, want), "RS: the reported residual equals the recomputed one: " + specs[i].id);
        squares += want.rms * want.rms * want.n;
        maximum = std::max(maximum, want.max);
        count += want.n;
    }
    Check(a.training_residual && a.training_residual->point_count == count && Near(a.training_residual->rms_pixels, std::sqrt(squares / count))
        && Near(a.training_residual->max_pixels, maximum), "RS: the pooled training residual is sqrt(sum e^2 / N) and the largest error");
    Check(a.solver && Near(a.solver->final_cost, squares, 1e-12), "RS: final_cost is the sum of the squared errors (independently computed)");
    const auto* t1 = FindView(a, "T1");
    Check(t1 && t1->residual && t1->residual->max_pixels > 0.02 && t1->residual->max_pixels < 0.5 && t1->residual->rms_pixels > 0,
        "RS: a 0.25 px perturbation leaves a visible but bounded training residual");
    double held_squares = 0;
    std::uint32_t held_count = 0;
    for (std::size_t i = 6; i < 8; ++i) {
        const auto* view = FindView(a, specs[i].id);
        Check(view && view->pose && view->outcome == LensViewOutcome::Evaluated, "RS: validation pose: " + specs[i].id);
        if (!view || !view->pose) continue;
        const auto want = ExpectedResidual(specs[i], specs[i].measurement, chart, *a.intrinsics, *a.distortion, *view->pose);
        Check(ResidualMatches(view->residual, want), "RS: the validation residual equals the recomputed one over the measurement points: " + specs[i].id);
        held_squares += want.rms * want.rms * want.n;
        held_count += want.n;
        // The pose points are not measured: the same recomputation over them differs from the report.
        const auto over_pose_points = ExpectedResidual(specs[i], specs[i].used, chart, *a.intrinsics, *a.distortion, *view->pose);
        Check(!ResidualMatches(view->residual, over_pose_points), "RS: the reported validation residual is not the one over the pose points: " + specs[i].id);
    }
    Check(a.held_out_residual && a.held_out_residual->point_count == held_count && Near(a.held_out_residual->rms_pixels, std::sqrt(held_squares / held_count)),
        "RS: the pooled held-out residual is sqrt(sum e^2 / N) over the measurement points");
    const auto* v1 = FindView(a, "V1");
    Check(v1 && v1->residual && v1->residual->max_pixels > 0.5 && v1->residual->max_pixels < 1.5, "RS: the 1 px perturbation of a measurement point shows as a residual of about one pixel");
}

// TS: roles of the points are not mixed up, and unused data does not leak.
void TestRolesOfPoints() {
    const auto policy = Policy();
    const auto base_specs = Layout();
    const auto base = Run(base_specs, policy);
    const auto& a = base.cameras[0];
    Check(a.status == LensStatus::LensIntrinsicsEstimated, "TS: the baseline is estimated");
    const auto base_training = Fingerprint(a, true);
    const auto base_full = Fingerprint(base);
    const auto* base_v1 = FindView(a, "V1");
    // (a) a measurement point of V1 moves: the held-out pose is bit-identical, only the residual changes.
    {
        auto specs = base_specs;
        specs[6].delta["P0_3"] = {1.0, 0}; // index 3: measurement
        const auto result = Run(specs, policy);
        Invariants(result, policy, "TS-a");
        const auto& b = result.cameras[0];
        const auto* v1 = FindView(b, "V1");
        Check(v1 && base_v1 && v1->pose && base_v1->pose && v1->pose->rotation == base_v1->pose->rotation && v1->pose->translation_mm == base_v1->pose->translation_mm,
            "TS-a: a measurement point does not change the held-out pose (bitwise)");
        Check(v1 && v1->solver && base_v1->solver && v1->solver->iterations == base_v1->solver->iterations && Bitwise(v1->solver->final_cost, base_v1->solver->final_cost),
            "TS-a: nor the held-out solver record");
        Check(v1 && v1->residual && v1->residual->max_pixels > 0.5, "TS-a: but it changes the residual");
        Check(Fingerprint(b, true) == base_training, "TS-a: the training side is unchanged (bitwise)");
        const auto* v2 = FindView(b, "V2");
        const auto* base_v2 = FindView(a, "V2");
        Check(v2 && base_v2 && v2->residual && base_v2->residual && Bitwise(v2->residual->rms_pixels, base_v2->residual->rms_pixels), "TS-a: V2 is unchanged");
    }
    // (b) a pose point of V1 moves: the held-out pose changes; the training side does not.
    {
        auto specs = base_specs;
        specs[6].delta["P0_4"] = {1.0, 0}; // index 4: pose
        const auto result = Run(specs, policy);
        Invariants(result, policy, "TS-b");
        const auto& b = result.cameras[0];
        const auto* v1 = FindView(b, "V1");
        Check(v1 && base_v1 && v1->pose && base_v1->pose && (v1->pose->rotation != base_v1->pose->rotation || v1->pose->translation_mm != base_v1->pose->translation_mm),
            "TS-b: a pose point changes the held-out pose");
        Check(Fingerprint(b, true) == base_training, "TS-b: the training side is unchanged (bitwise)");
    }
    // (c) an unused point of T1 moves: nothing changes at all.
    {
        auto specs = base_specs;
        specs[0].delta["P12_16"] = {3.0, -2.0}; // the last point: detected, unused
        const auto result = Run(specs, policy);
        Check(Fingerprint(result) == base_full, "TS-c: an unused detected point changes nothing (bitwise)");
    }
    // (d) a used point of T2 moves by 0.25 px: K changes.
    {
        auto specs = base_specs;
        specs[1].delta["P6_8"] = {0.25, 0};
        const auto result = Run(specs, policy);
        Invariants(result, policy, "TS-d");
        const auto& b = result.cameras[0];
        Check(b.intrinsics && a.intrinsics && (!Bitwise(b.intrinsics->fx, a.intrinsics->fx) || !Bitwise(b.intrinsics->cx, a.intrinsics->cx)), "TS-d: a used training point changes K");
        Check(b.training_residual && b.training_residual->max_pixels > 0.02, "TS-d: and leaves a training residual");
    }
    // S-b: the reference view and the validation views never reach K, the coefficients, the check or the training views.
    {
        auto specs = base_specs;
        specs[8].delta["P3_3"] = {5.0, 5.0}; // R1
        const auto result = Run(specs, policy);
        Check(Fingerprint(result) == base_full, "S-b: moving a reference pixel changes nothing (bitwise)");
        specs = base_specs;
        specs[6].delta["P1_1"] = {2.0, 1.0};
        specs[7].delta["P2_2"] = {-2.0, 1.5};
        specs[7].delta["P1_2"] = {1.0, -3.0};
        const auto moved = Run(specs, policy);
        Check(Fingerprint(moved.cameras[0], true) == base_training, "S-b: moving validation pixels leaves the training side bit-identical");
    }
}

// N1: noisy, seeded.
void TestNoise() {
    const double sigma = 0.25;
    const auto policy = Policy();
    auto specs = Layout();
    const auto noise = std::make_shared<Gauss>(290);
    for (auto& spec : specs) if (spec.role != "reference") { spec.noise = noise; spec.sigma = sigma; }
    const auto result = Run(specs, policy);
    Invariants(result, policy, "N1");
    const auto& a = result.cameras[0];
    Check(a.status == LensStatus::LensIntrinsicsEstimated, "N1: estimated with noise");
    if (!a.training_residual || !a.held_out_residual) return;
    const double n = a.training_residual->point_count;
    const double parameters = 9 + 6 * 6;
    const double expected_training = sigma * std::sqrt(2.0) * std::sqrt(1.0 - parameters / (2.0 * n));
    Check(std::abs(a.training_residual->rms_pixels / expected_training - 1.0) <= 0.10, "N1: the training rms is within 10% of sigma sqrt(2) sqrt(1 - P / 2N)");
    const double held = a.held_out_residual->rms_pixels / (sigma * std::sqrt(2.0));
    Check(held >= 0.85 && held <= 1.20, "N1: the held-out rms is within [0.85, 1.20] of sigma sqrt(2)");
    double squares = 0;
    std::size_t count = 0;
    for (const auto& id : {"T1", "T2", "T3", "T4", "T5", "T6"}) for (const auto& spec : specs) if (spec.id == id) {
        const auto stats = ProbeDifference(a, spec);
        squares += stats.squares;
        count += stats.count;
    }
    const double probe_rms = std::sqrt(squares / static_cast<double>(count));
    Check(count == 6 * 192 && probe_rms <= 0.5 * sigma, "N1: the estimated map is within 0.5 sigma (rms) of the truth at points that are not in the session");
    std::printf("info N1: training %.4f x theory, held-out %.4f x sigma sqrt2, probe rms %.4f sigma, iterations %u\n",
        a.training_residual->rms_pixels / expected_training, held, probe_rms / sigma, a.solver ? a.solver->iterations : 0u);
}

// M1, M2: the iteration limit.
void TestIterationLimit() {
    const auto specs = Layout();
    const auto base = Run(specs);
    const auto& a = base.cameras[0];
    if (!a.solver) { Check(false, "M2: baseline has a solver record"); return; }
    {
        auto policy = Policy();
        policy.max_iterations = 1;
        const auto result = Run(specs, policy);
        Invariants(result, policy, "M1");
        const auto& b = result.cameras[0];
        Check(b.status == LensStatus::SolverNonconvergence, "M1: one trial is not enough");
        Check(b.solver && b.solver->iterations == 1 && b.solver->stop_reason == SolverStopReason::MaximumIterations
            && b.solver->initial_cost > b.solver->final_cost && b.solver->final_cost > 0 && b.solver->accepted_steps == 1, "M1: the record keeps the accepted step and the maximum-iterations stop");
        Check(!b.intrinsics && !b.distortion && !b.distortion_check && !b.training_residual, "M1: K, coefficients and check are null: the last iterate is not published");
        for (const auto& view : b.views) Check(view.outcome == LensViewOutcome::NotReached && !view.pose && !view.residual, "M1: views are not reached: " + view.view_id);
        Check(b.held_out_coverage == HeldOutCoverage::NotReached && !b.held_out_residual, "M1: held-out is not reached");
        Check(a.solver->initial_cost == b.solver->initial_cost, "M1: the start is the same");
    }
    // M3: the relative cost rule compares the reduction with the cost before the step.
    // The first step lowers the cost by the fraction rho; a tolerance above rho stops at trial 1 and one below it does not.
    {
        auto one_trial = Policy();
        one_trial.max_iterations = 1;
        const auto first_result = Run(specs, one_trial);
        const auto& first = first_result.cameras[0];
        if (first.solver) {
            const double rho = (first.solver->initial_cost - first.solver->final_cost) / first.solver->initial_cost;
            Check(rho > 0.5 && rho < 1.0, "M3: the first step removes most of the cost");
            auto above = Policy();
            above.relative_cost_tolerance = rho + 0.5 * (1.0 - rho);
            const auto stopped = Run(specs, above);
            Invariants(stopped, above, "M3a");
            const auto& s = stopped.cameras[0];
            Check(s.solver && s.solver->iterations == 1 && s.solver->accepted_steps == 1 && s.solver->stop_reason == SolverStopReason::RelativeCostReduction,
                "M3: a tolerance above the first reduction stops at trial 1 by the cost rule");
            auto below = Policy();
            below.relative_cost_tolerance = 0.5 * rho;
            const auto continued = Run(specs, below);
            Invariants(continued, below, "M3b");
            Check(continued.cameras[0].solver && continued.cameras[0].solver->iterations > 1, "M3: a tolerance below the first reduction goes on");
        } else {
            Check(false, "M3: the one-trial run has a solver record");
        }
    }
    const auto n = a.solver->iterations;
    Check(n >= 2, "M2: the baseline needs more than one trial");
    {
        auto policy = Policy();
        policy.max_iterations = n;
        const auto result = Run(specs, policy);
        const auto& b = result.cameras[0];
        Check(b.status == LensStatus::LensIntrinsicsEstimated && Fingerprint(b, true) == Fingerprint(a, true),
            "M2: the exact number of trials gives the same training result (bitwise)");
    }
    {
        auto policy = Policy();
        policy.max_iterations = n - 1;
        const auto result = Run(specs, policy);
        Invariants(result, policy, "M2");
        const auto& b = result.cameras[0];
        Check(b.status == LensStatus::SolverNonconvergence && b.solver && b.solver->iterations == n - 1, "M2: one trial fewer ends in solver-nonconvergence");
    }
}

// A held-out pose solve that does not converge while the training solve does. The
// validation views are drawn with a lens that differs from the training lens, so
// the pose-only solve has a model mismatch to absorb while the training solve,
// whose data are consistent, stops at its first trial.
void TestHeldOutIterationLimit() {
    auto specs = Layout("CAM-A", kNoDist);
    for (std::size_t i = 6; i < 8; ++i) specs[i].d = kTrueDist;
    const auto session = MakeSession(Chart(), specs);
    auto policy = Policy();
    policy.max_iterations = 1;
    const auto limited = EstimateLensIntrinsics(session, policy);
    Invariants(limited, policy, "HL");
    const auto& a = limited.cameras[0];
    Check(a.status == LensStatus::LensIntrinsicsEstimated && a.solver && a.solver->iterations == 1 && ConvergentStop(a.solver->stop_reason),
        "HL: the training solve converges within a single trial");
    for (const auto& id : {"V1", "V2"}) {
        const auto* view = FindView(a, id);
        Check(view && view->outcome == LensViewOutcome::SolverNonconvergence && !view->pose && !view->residual && view->solver
            && view->solver->stop_reason == SolverStopReason::MaximumIterations && view->solver->iterations == 1,
            std::string("HL: a held-out pose solve that needs more than the allowed trials is solver-nonconvergence: ") + id);
    }
    Check(a.held_out_coverage == HeldOutCoverage::SomeViewsNotEvaluated && !a.held_out_residual, "HL: coverage reports the missing views and there is no pooled residual");
    Check(a.training_residual.has_value() && a.intrinsics && a.distortion, "HL: the training result is kept");

    // With the default limit the same views converge, and the mismatch shows as a residual.
    const auto relaxed_policy = Policy();
    const auto relaxed = EstimateLensIntrinsics(session, relaxed_policy);
    Invariants(relaxed, relaxed_policy, "HL2");
    const auto& b = relaxed.cameras[0];
    Check(b.held_out_coverage == HeldOutCoverage::AllViewsEvaluated && b.held_out_residual && b.held_out_residual->rms_pixels > 0.05,
        "HL: within the default limit the views are evaluated and the lens mismatch is visible");
    for (const auto& id : {"V1", "V2"}) {
        const auto* view = FindView(b, id);
        Check(view && view->solver && view->solver->iterations > 1, std::string("HL: the pose solve really needs more than one trial: ") + id);
    }
    Check(Fingerprint(a, true) == Fingerprint(b, true), "HL: the limit does not touch the training side (bitwise)");
}

// A validation view that stage 2 could not pose is reported, and the camera still succeeds.
void TestValidationViewFailure() {
    const auto policy = Policy();
    auto specs = Layout();
    const auto base = Run(specs, policy);
    specs[6].used.resize(3); // V1: three pose points are not enough for a mapping
    const auto result = Run(specs, policy);
    Invariants(result, policy, "HV");
    const auto& a = result.cameras[0];
    Check(a.status == LensStatus::LensIntrinsicsEstimated, "HV: a validation failure does not change the camera status");
    const auto* v1 = FindView(a, "V1");
    Check(v1 && v1->outcome == LensViewOutcome::InitializationUnavailable && !v1->pose && !v1->residual && !v1->solver, "HV: V1 has no usable start");
    const auto* v2 = FindView(a, "V2");
    const auto* base_v2 = FindView(base.cameras[0], "V2");
    Check(v2 && base_v2 && v2->outcome == LensViewOutcome::Evaluated && v2->residual && base_v2->residual && Bitwise(v2->residual->rms_pixels, base_v2->residual->rms_pixels),
        "HV: V2 is evaluated as before");
    Check(a.held_out_coverage == HeldOutCoverage::SomeViewsNotEvaluated && a.held_out_residual && a.held_out_residual->point_count == 110,
        "HV: the pooled held-out residual covers V2 only");
    Check(a.views.size() == base.cameras[0].views.size(), "HV: the view list keeps its length");
    Check(Fingerprint(a, true) == Fingerprint(base.cameras[0], true), "HV: the training side is unchanged (bitwise)");
    // A camera without validation views has no held-out result.
    auto no_validation = Layout();
    no_validation.erase(no_validation.begin() + 6, no_validation.begin() + 8);
    const auto none = Run(no_validation, policy);
    Invariants(none, policy, "HN");
    Check(none.cameras[0].status == LensStatus::LensIntrinsicsEstimated && none.cameras[0].held_out_coverage == HeldOutCoverage::NoValidationView && !none.cameras[0].held_out_residual,
        "HN: no validation view means no held-out result");
}

// PH: the physicality predicates at their boundaries, by direct calls.
void TestPhysicality() {
    // Radial slope: g(s) = 1 + 3 k1 s + 5 k2 s^2 + 7 k3 s^3.
    Check(ld::MinimumRadialSlope(0, -0.05, 0, 2) == 0.0, "PH: g(2) is exactly 0 for k2 = -0.05 (invalid: not strictly positive)");
    Check(!(ld::MinimumRadialSlope(0, -0.05, 0, 2) > 0), "PH: the exact zero is not valid");
    Check(Near(ld::MinimumRadialSlope(0, -0.05, 0, 1), 0.75, 1e-14), "PH: s_max = 1 gives 0.75");
    Check(Near(ld::MinimumRadialSlope(0, -0.05, 0, 4), -3.0, 1e-14), "PH: s_max = 4 gives -3");
    Check(ld::RadialSlope(-1, 0.4, 0, 1.2) > 0 && ld::RadialSlope(-1, 0.4, 0, 0) > 0, "PH: both ends of (-1, 0.4, 0) on [0, 1.2] are positive");
    Check(Near(ld::MinimumRadialSlope(-1, 0.4, 0, 1.2), -0.125, 1e-12), "PH: the interior minimum -0.125 is found (an end-points-only check passes this case)");
    Check(Near(ld::MinimumRadialSlope(-1, 0.4, 0, 0.4), 0.12, 1e-12), "PH: the critical point 0.75 is outside [0, 0.4] and is ignored");
    Check(ld::MinimumRadialSlope(1, 0.4, 0, 1) == 1.0, "PH: a negative critical point is ignored");
    Check(ld::MinimumRadialSlope(0, 0, 0, 3) == 1.0, "PH: no coefficients: the slope is 1");
    Check(ld::MinimumRadialSlope(-0.5, 0.0, 0.0, 0) == 1.0, "PH: s_max = 0 gives g(0) = 1");
    {
        // A boundary that does not rest on rounding: k1 = 0.5 and k2 = -0.5 are powers of two, so every product and
        // sum in g(1) = 1 + 1.5 - 2.5 is exact and the minimum over [0, 1] (reached at the end) is exactly 0. The
        // neighbouring coefficients (one ulp either side) fall on opposite sides of the boundary.
        Check(ld::MinimumRadialSlope(0.5, -0.5, 0, 1) == 0.0, "PH: exact boundary: k = (0.5, -0.5, 0), s_max = 1 gives exactly 0");
        Check(ld::MinimumRadialSlope(0.5, std::nextafter(-0.5, 0.0), 0, 1) > 0, "PH: exact boundary: one ulp towards zero is strictly positive");
        Check(ld::MinimumRadialSlope(0.5, std::nextafter(-0.5, -1.0), 0, 1) < 0, "PH: exact boundary: one ulp away from zero is strictly negative");
        Check(!ld::PhysicalDistortion(ld::MinimumRadialSlope(0.5, -0.5, 0, 1), 1.0), "PH: exact boundary: the zero is not physical");
        // The derivative has a critical point at s = 0.3 (a maximum, g = 1.225); it must not hide the end. At s_max = 15/16 the minimum is g = 0.208984375.
        Check(Near(ld::MinimumRadialSlope(0.5, -0.5, 0, 0.9375), 0.208984375, 1e-14), "PH: a little inside the boundary the minimum is the value at the end");
    }
    Check(Near(ld::MinimumRadialSlope(-2, 2, -0.5, 2), -0.028342939507387088, 1e-9) && ld::RadialSlope(-2, 2, -0.5, 2) == 1.0,
        "PH: cubic with an interior negative minimum and a positive end");
    // Dense sampling of three cubics, including k3 of both signs.
    for (const auto& c : std::vector<std::array<double, 4>>{{-2, 2, -0.5, 2.0}, {-1.4, 0.9, 0.3, 0.8}, {-1.5, 1.2, -0.25, 1.6}, {-1, -0.5, 0.4, 1.5}, {-2, 0.5, 0.4, 1.5}}) {
        double sampled = std::numeric_limits<double>::infinity();
        for (int i = 0; i <= 200000; ++i) sampled = std::min(sampled, ld::RadialSlope(c[0], c[1], c[2], c[3] * i / 200000.0));
        const double closed = ld::MinimumRadialSlope(c[0], c[1], c[2], c[3]);
        Check(std::abs(closed - sampled) <= 1e-9 && ((closed > 0) == (sampled > 0)), "PH: the closed-form minimum matches dense sampling: k=(" + Num(c[0]) + "," + Num(c[1]) + "," + Num(c[2]) + ")");
    }
    // Both roots of the derivative matter: each of these has its negative minimum at one of them and positive ends.
    Check(Near(ld::MinimumRadialSlope(-1, -0.5, 0.4, 1.5), -1.7069, 1e-3) && ld::RadialSlope(-1, -0.5, 0.4, 1.5) > 0,
        "PH: the minimum at the root q / a is found while both ends are positive");
    Check(Near(ld::MinimumRadialSlope(-2, 0.5, 0.4, 1.5), -1.0952, 1e-3) && ld::RadialSlope(-2, 0.5, 0.4, 1.5) > 0,
        "PH: the minimum at the root c / q is found while both ends are positive");
    // Jacobian determinant of the distortion map.
    const BrownConradyDistortion p1_only{0, 0, 0, -0.5, 0};
    const double zero_det = ld::DistortionJacobianDeterminant(p1_only, 0, 1);
    Check(zero_det == 0.0 && !(zero_det > 0), "PH: p1 = -0.5 at (0, 1): the determinant is exactly 0 (and negative zero is invalid too)");
    Check(Near(ld::DistortionJacobianDeterminant(p1_only, 0, 0.5), -0.25, 1e-14), "PH: y = 0.5 gives -0.25");
    Check(Near(ld::DistortionJacobianDeterminant(p1_only, 0, 0.25), 0.1875, 1e-14), "PH: y = 0.25 gives 0.1875");
    for (const BrownConradyDistortion radial_only : {BrownConradyDistortion{-0.1, 0.05, 0.01, 0, 0}, BrownConradyDistortion{0.3, -0.2, 0.1, 0, 0}}) {
        double worst = 0;
        for (const double x : {-0.4, 0.0, 0.2, 0.5}) for (const double y : {-0.3, 0.1, 0.45}) {
            const double r2 = x * x + y * y;
            const double radial = 1 + r2 * (radial_only.k1 + r2 * (radial_only.k2 + r2 * radial_only.k3));
            const double slope = ld::RadialSlope(radial_only.k1, radial_only.k2, radial_only.k3, r2);
            worst = std::max(worst, std::abs(ld::DistortionJacobianDeterminant(radial_only, x, y) - radial * slope));
        }
        Check(worst <= 1e-14, "PH: without tangential terms the determinant is L(r2) g(r2)");
    }
    // Intrinsics.
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    Check(ld::PhysicalIntrinsics({1000, 1040, 651.8, 470.8}), "PH: ordinary intrinsics are physical");
    Check(!ld::PhysicalIntrinsics({0, 1040, 651.8, 470.8}) && !ld::PhysicalIntrinsics({1000, -1, 651.8, 470.8}) && !ld::PhysicalIntrinsics({1000, 0, 651.8, 470.8})
        && !ld::PhysicalIntrinsics({-1000, 1040, 651.8, 470.8}), "PH: zero or negative focal lengths are nonphysical");
    Check(!ld::PhysicalIntrinsics({nan, 1040, 651.8, 470.8}) && !ld::PhysicalIntrinsics({1000, nan, 651.8, 470.8}) && !ld::PhysicalIntrinsics({1000, 1040, nan, 470.8})
        && !ld::PhysicalIntrinsics({1000, 1040, 651.8, nan}) && !ld::PhysicalIntrinsics({inf, 1040, 651.8, 470.8}) && !ld::PhysicalIntrinsics({1000, 1040, inf, 470.8}),
        "PH: a non-finite value is nonphysical");
    Check(ld::PhysicalIntrinsics({1000, 1040, -5.0, 1e6}), "PH: the principal point is not constrained");
    // The rule that combines the two distortion observations.
    const double smallest = std::nextafter(0.0, 1.0);
    Check(ld::PhysicalDistortion(1.0, 1.0) && ld::PhysicalDistortion(smallest, smallest) && ld::PhysicalDistortion(0.75, 0.1875), "PH: positive slope and determinant are physical");
    Check(!ld::PhysicalDistortion(0.0, 1.0) && !ld::PhysicalDistortion(-0.0, 1.0) && !ld::PhysicalDistortion(1.0, 0.0) && !ld::PhysicalDistortion(1.0, -0.0),
        "PH: an exact zero of either value is not physical");
    Check(!ld::PhysicalDistortion(-smallest, 1.0) && !ld::PhysicalDistortion(1.0, -smallest) && !ld::PhysicalDistortion(-1.0, -1.0)
        && !ld::PhysicalDistortion(nan, 1.0) && !ld::PhysicalDistortion(1.0, nan) && !ld::PhysicalDistortion(inf, nan), "PH: negative or NaN values are not physical");
}

// PE: a truth whose radial map folds back inside the observed radius.
void TestInvalidDistortion() {
    const auto policy = Policy();
    const Dist folding{0, 2.4, -12.0, 0, 0};
    const auto specs = Layout("CAM-A", folding);
    const auto result = Run(specs, policy);
    Invariants(result, policy, "PE");
    const auto& a = result.cameras[0];
    Check(a.status == LensStatus::InvalidDistortion, "PE: invalid-distortion");
    Check(FailedStage(a.status) == LensStage::Distortion, "PE: the distortion stage stopped it");
    Check(a.solver && ConvergentStop(a.solver->stop_reason), "PE: the solver converged to the folding map; the check is what refuses it");
    Check(a.intrinsics.has_value() && !a.distortion && !a.training_residual, "PE: K is kept, the coefficients are not published");
    Check(a.distortion_check && a.distortion_check->minimum_radial_slope < 0 && a.distortion_check->minimum_jacobian_determinant < 0,
        "PE: both check values are negative");
    for (const auto& view : a.views) Check(view.outcome == LensViewOutcome::NotReached && !view.pose && !view.residual, "PE: views are not reached: " + view.view_id);
    Check(a.held_out_coverage == HeldOutCoverage::NotReached && !a.held_out_residual, "PE: held-out is not reached");
    if (a.distortion_check) {
        std::printf("info PE: s_max %.4f slope %.4f det %.4f iterations %u\n", a.distortion_check->max_radius_squared,
            a.distortion_check->minimum_radial_slope, a.distortion_check->minimum_jacobian_determinant, a.solver ? a.solver->iterations : 0u);
    }
}

// I1: a frontal-only camera has no start.
void TestInitializationUnavailable() {
    const auto policy = Policy();
    const auto specs = Frontal("F");
    const auto session = MakeSession(Chart(), specs);
    const auto result = EstimateLensIntrinsics(session, policy);
    Invariants(result, policy, "I1");
    const auto& a = result.cameras[0];
    Check(a.status == LensStatus::InitializationUnavailable && FailedStage(a.status) == LensStage::Initialization, "I1: frontal views give initialization-unavailable");
    Check(!a.solver && !a.intrinsics && !a.distortion && !a.distortion_check && !a.training_residual, "I1: nothing after the initialization");
    Check(a.initialization.status == IntrinsicsStatus::InsufficientViewDiversity, "I1: the stage 2 reason is carried");
    const auto direct = EstimateUndistortedIntrinsics(session, Stage2Policy());
    Check(Stage2Fingerprint(a.initialization) == Stage2Fingerprint(direct.cameras[0]), "I1: the initialization is bit-identical to calling stage 2 directly");
    Check(Stage2Fingerprint(result.cameras[1].initialization) == Stage2Fingerprint(direct.cameras[1]), "I1: and so is CAM-B's");
    for (const auto& view : a.views) Check(view.outcome == LensViewOutcome::NotReached, "I1: views are not reached: " + view.view_id);
}

// C1: the cameras do not influence each other.
void TestCamerasAreIndependent() {
    const auto policy = Policy();
    const auto a_only = Run(Layout("CAM-A"), policy);
    auto with_b = Layout("CAM-A");
    for (auto& view : Layout("CAM-B", Dist{-0.05, 0.02, 0.0, -2e-3, 1e-3})) { view.id = "B" + view.id; with_b.push_back(std::move(view)); }
    const auto both = Run(with_b, policy);
    Invariants(both, policy, "C1");
    Check(both.cameras[0].status == LensStatus::LensIntrinsicsEstimated && both.cameras[1].status == LensStatus::LensIntrinsicsEstimated, "C1: both cameras are estimated");
    Check(Fingerprint(both.cameras[0]) == Fingerprint(a_only.cameras[0]), "C1: CAM-A is bit-identical with or without CAM-B views");
    auto with_frontal = Layout("CAM-A");
    for (auto& view : Frontal("BF", "CAM-B")) with_frontal.push_back(std::move(view));
    const auto frontal = Run(with_frontal, policy);
    Invariants(frontal, policy, "C1b");
    Check(Fingerprint(frontal.cameras[0]) == Fingerprint(a_only.cameras[0]), "C1: CAM-A is bit-identical when CAM-B only has frontal views");
    Check(frontal.cameras[1].status == LensStatus::InitializationUnavailable && frontal.cameras[0].status == LensStatus::LensIntrinsicsEstimated,
        "C1: CAM-B fails on its own and CAM-A succeeds");
    // Reverse: CAM-B good, CAM-A frontal.
    std::vector<ViewSpec> reversed = Frontal("AF", "CAM-A");
    for (auto& view : Layout("CAM-B")) reversed.push_back(std::move(view));
    const auto flipped = Run(reversed, policy);
    const auto b_only = Run(Layout("CAM-B"), policy);
    Invariants(flipped, policy, "C1c");
    Check(Fingerprint(flipped.cameras[1]) == Fingerprint(b_only.cameras[1]), "C1: CAM-B is bit-identical when CAM-A has only frontal views");
    Check(flipped.cameras[0].status == LensStatus::InitializationUnavailable && flipped.cameras[1].status == LensStatus::LensIntrinsicsEstimated, "C1: the reverse is independent too");
    // Different lenses are recovered separately.
    const auto* b_view = FindView(both.cameras[1], "BT2");
    Check(b_view && b_view->pose && both.cameras[1].intrinsics && both.cameras[1].distortion, "C1: CAM-B has its own lens");
    const double dk1 = both.cameras[1].distortion ? std::abs(both.cameras[1].distortion->k1 - both.cameras[0].distortion->k1) : 0;
    Check(dk1 > 0.01, "C1: the two cameras have different coefficients");
}

// D1: determinism.
void TestDeterminism() {
    const auto json = BuildJson(Chart(), Layout());
    const auto policy = Policy();
    std::string first;
    for (int run = 0; run < 2; ++run) {
        const auto session = CalibrationObservationSession::Parse(json);
        for (int repeat = 0; repeat < 2; ++repeat) {
            const auto fingerprint = Fingerprint(EstimateLensIntrinsics(session, policy));
            if (first.empty()) first = fingerprint;
            Check(fingerprint == first, "D1: run " + std::to_string(run) + "/" + std::to_string(repeat) + " is bit-identical to the first");
        }
    }
}

// Z1: without distortion the lens stage reproduces stage 2.
void TestZeroDistortion() {
    const auto policy = Policy();
    const auto specs = Layout("CAM-A", kNoDist);
    const auto session = MakeSession(Chart(), specs);
    const auto result = EstimateLensIntrinsics(session, policy);
    Invariants(result, policy, "Z1");
    const auto& a = result.cameras[0];
    Check(a.status == LensStatus::LensIntrinsicsEstimated && a.intrinsics && a.distortion, "Z1: estimated");
    if (!a.intrinsics || !a.distortion) return;
    const auto direct = EstimateUndistortedIntrinsics(session, Stage2Policy());
    Check(Stage2Fingerprint(a.initialization) == Stage2Fingerprint(direct.cameras[0]), "Z1: the initialization is stage 2 itself (bitwise)");
    const auto& s2 = direct.cameras[0];
    Check(s2.intrinsics.has_value(), "Z1: stage 2 estimated");
    double worst = 0;
    if (s2.intrinsics) {
        for (const auto& spec : specs) {
            const auto* lens_view = FindView(a, spec.id);
            const IntrinsicsViewResult* base_view = nullptr;
            for (const auto& v : s2.views) if (v.view_id == spec.id) base_view = &v;
            if (!lens_view || !lens_view->pose || !base_view || !base_view->pose) continue;
            for (const auto& probe : Probes()) {
                const auto lens = ProjectEstimate(*a.intrinsics, *a.distortion, *lens_view->pose, probe.x, probe.y);
                const auto closed = ProjectEstimate(*s2.intrinsics, BrownConradyDistortion{0, 0, 0, 0, 0}, *base_view->pose, probe.x, probe.y);
                worst = std::max(worst, std::hypot(lens.u - closed.u, lens.v - closed.v));
            }
        }
    }
    Check(worst <= 1e-6, "Z1: the lens map equals the stage 2 map at the probes (worst " + std::to_string(worst) + ")");
    const double largest = std::max({std::abs(a.distortion->k1), std::abs(a.distortion->k2), std::abs(a.distortion->k3), std::abs(a.distortion->p1), std::abs(a.distortion->p2)});
    Check(largest <= 1e-9, "Z1: the coefficients stay at zero (largest " + std::to_string(largest) + ")");
    Check(a.training_residual && a.training_residual->max_pixels <= 1e-6, "Z1: the training residual is at the rounding floor");
}

// DM: a huge initial damping stops at the start; the record shows it.
void TestLargeDamping() {
    auto policy = Policy();
    policy.initial_damping = 1e12;
    const auto result = Run(Layout(), policy);
    Invariants(result, policy, "DM");
    const auto& a = result.cameras[0];
    Check(a.solver && a.solver->accepted_steps == 0 && a.solver->final_cost == a.solver->initial_cost && a.solver->iterations == 1
        && a.solver->stop_reason == SolverStopReason::RelativeStepSize, "DM: no step is taken and the record says so");
    Check(a.status == LensStatus::LensIntrinsicsEstimated && a.distortion && a.distortion->k1 == 0 && a.distortion->k2 == 0 && a.distortion->k3 == 0
        && a.distortion->p1 == 0 && a.distortion->p2 == 0, "DM: the start (zero distortion) is what is reported");
    Check(a.training_residual && a.training_residual->rms_pixels > 0.5, "DM: and its training residual shows that it does not fit");
}

// J1: the analytic Jacobian against central differences of the test's own projection.
void TestJacobian() {
    const Intrinsics k{1010, 1032, 648.3, 474.1};
    const Dist d{-0.12, 0.07, -0.02, 2e-3, -1.5e-3};
    const Pose pose = MakePose(0.27, -0.31, 0.11, 12, -7, 520);
    const PinholeIntrinsics lk{k.fx, k.fy, k.cx, k.cy};
    const BrownConradyDistortion ld_{d.k1, d.k2, d.k3, d.p1, d.p2};
    const ChartPose lp{pose.r, pose.t};
    const std::vector<std::array<double, 2>> points{{-200, -150}, {0, 0}, {175, -125}, {-150, 125}, {200, 150}};
    // Rodrigues' formula, written independently of the library.
    const auto rotate_by = [](const std::array<double, 3>& w, const std::array<double, 9>& r) {
        const double theta = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (theta == 0) return r;
        const double ux = w[0] / theta, uy = w[1] / theta, uz = w[2] / theta;
        const double c = std::cos(theta), s = std::sin(theta), o = 1 - c;
        const std::array<double, 9> e{c + ux * ux * o, ux * uy * o - uz * s, ux * uz * o + uy * s,
            uy * ux * o + uz * s, c + uy * uy * o, uy * uz * o - ux * s,
            uz * ux * o - uy * s, uz * uy * o + ux * s, c + uz * uz * o};
        std::array<double, 9> out{};
        for (std::size_t i = 0; i < 3; ++i) for (std::size_t j = 0; j < 3; ++j) {
            double sum = 0;
            for (std::size_t m = 0; m < 3; ++m) sum += e[i * 3 + m] * r[m * 3 + j];
            out[i * 3 + j] = sum;
        }
        return out;
    };
    std::array<double, 15> column_error{}, column_norm{};
    double pixel_worst = 0;
    for (const auto& point : points) {
        const auto analytic = ld::ProjectChartPoint(lk, ld_, lp, point[0], point[1]);
        Check(analytic.has_value(), "J1: the point projects");
        if (!analytic) continue;
        const auto truth = Project(k, d, pose.r, pose.t, point[0], point[1]);
        pixel_worst = std::max(pixel_worst, std::hypot(analytic->u - truth.u, analytic->v - truth.v));
        const std::array<double, 15> theta{k.fx, k.fy, k.cx, k.cy, d.k1, d.k2, d.k3, d.p1, d.p2, 0, 0, 0, pose.t[0], pose.t[1], pose.t[2]};
        for (std::size_t j = 0; j < 15; ++j) {
            const double h = 1e-6 * std::max(1.0, std::abs(theta[j]));
            const auto at = [&](double step) {
                Intrinsics kk = k;
                Dist dd = d;
                std::array<double, 9> r = pose.r;
                Vec3 t = pose.t;
                switch (j) {
                case 0: kk.fx += step; break;
                case 1: kk.fy += step; break;
                case 2: kk.cx += step; break;
                case 3: kk.cy += step; break;
                case 4: dd.k1 += step; break;
                case 5: dd.k2 += step; break;
                case 6: dd.k3 += step; break;
                case 7: dd.p1 += step; break;
                case 8: dd.p2 += step; break;
                case 9: case 10: case 11: { std::array<double, 3> w{0, 0, 0}; w[j - 9] = step; r = rotate_by(w, pose.r); break; }
                default: t[j - 12] += step; break;
                }
                return Project(kk, dd, r, t, point[0], point[1]);
            };
            const auto plus = at(h), minus = at(-h);
            const double du = (plus.u - minus.u) / (2 * h), dv = (plus.v - minus.v) / (2 * h);
            column_error[j] = std::max(column_error[j], std::hypot(du - analytic->jacobian[0][j], dv - analytic->jacobian[1][j]));
            column_norm[j] = std::max(column_norm[j], std::hypot(analytic->jacobian[0][j], analytic->jacobian[1][j]));
        }
    }
    double worst = 0;
    for (std::size_t j = 0; j < 15; ++j) {
        const double relative = column_error[j] / column_norm[j];
        worst = std::max(worst, relative);
        Check(column_norm[j] > 0 && relative <= 1e-6, "J1: Jacobian column " + std::to_string(j) + " matches the central difference");
    }
    Check(pixel_worst <= 1e-9, "J1: the library projection equals the test projection (worst " + std::to_string(pixel_worst) + " px)");
    std::printf("info J1: worst relative column error %.3e\n", worst);
    // The helpers around it.
    Check(!ld::ProjectChartPoint(lk, ld_, ChartPose{pose.r, {12, -7, -520}}, 0, 0).has_value(), "J1: a point behind the camera does not project");
    const auto identity = ld::RotationIncrement({0, 0, 0});
    Check(identity == std::array<double, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1}, "J1: a zero increment is the identity");
    const auto tiny = ld::RotationIncrement({1e-9, -2e-9, 3e-9});
    Check(IsRotation(tiny) && std::abs(tiny[1] + 3e-9) < 1e-15 && std::abs(tiny[2] - (-2e-9)) < 1e-15, "J1: a tiny increment is I + [w]x");
    const auto large = ld::RotationIncrement({0.4, -0.2, 0.7});
    const auto reference = rotate_by({0.4, -0.2, 0.7}, {1, 0, 0, 0, 1, 0, 0, 0, 1});
    double rot_diff = 0;
    for (std::size_t i = 0; i < 9; ++i) rot_diff = std::max(rot_diff, std::abs(large[i] - reference[i]));
    Check(IsRotation(large) && rot_diff <= 1e-14, "J1: Exp(w) equals Rodrigues' formula");
    // Left multiplication: Exp(w) R rotates the camera-side point q about the camera axes.
    const ChartPose rotated{rotate_by({0.0, 0.0, 0.5}, pose.r), pose.t};
    const auto before = ld::ProjectChartPoint(lk, ld_, lp, 50, 20, false), after = ld::ProjectChartPoint(lk, ld_, rotated, 50, 20, false);
    Check(before && after && std::hypot(before->u - after->u, before->v - after->v) > 1.0, "J1: a rotation of the pose moves the projection");
}

// Dense and large.
#ifdef NDEBUG
void TestLargeProblem() {
    const auto chart = Grid(9, 13, 20, -120, -80);
    std::vector<ViewSpec> specs;
    for (int i = 0; i < 128; ++i) {
        const double tau = 0.15 + 0.2 * (i % 4) / 3.0;
        const double angle = 2.39996 * i;
        specs.push_back(FullView("L" + std::to_string(i), MakePose(tau * std::cos(angle), tau * std::sin(angle), 0.2 * std::sin(0.7 * i),
            40 * std::cos(1.3 * i), 30 * std::sin(0.9 * i), 520 + 20 * (i % 3)), "CAM-A", "training", chart, kTrueDist));
    }
    auto policy = Policy();
    policy.minimum_views = 128;
    const auto result = Run(chart, specs, policy);
    Invariants(result, policy, "L1");
    const auto& a = result.cameras[0];
    Check(a.status == LensStatus::LensIntrinsicsEstimated, "L1: 128 views, 777 unknowns");
    Check(a.solver && a.solver->iterations <= 50 && ConvergentStop(a.solver->stop_reason), "L1: converges within 50 trials");
    Check(a.training_residual && a.training_residual->point_count == 128U * 117U, "L1: every point is used");
    double worst = 0;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto* view = FindView(a, specs[i].id);
        if (!view || !view->pose || !a.intrinsics || !a.distortion) { worst = std::numeric_limits<double>::infinity(); break; }
        for (const auto& p : chart) {
            const auto estimate = ProjectEstimate(*a.intrinsics, *a.distortion, *view->pose, p.x + 10, p.y + 10);
            const auto exact = ProjectTruth(specs[i].k, specs[i].d, specs[i].pose, p.x + 10, p.y + 10);
            worst = std::max(worst, std::hypot(estimate.u - exact.u, estimate.v - exact.v));
        }
    }
    Check(worst <= 1e-6, "L1: the estimated map equals the truth at points that are not in the session (worst " + std::to_string(worst) + " px)");
    if (a.solver) std::printf("info L1: iterations %u accepted %u\n", a.solver->iterations, a.solver->accepted_steps);
}
#endif

// ---------------------------------------------------------------------------
// Independent views of what the solver reports (added by the independent QA pass).

struct NormPoint { double x, y; };
NormPoint Normalized(const std::array<double, 9>& r, const Vec3& t, double x, double y) {
    const double px = r[0] * x + r[1] * y + t[0], py = r[3] * x + r[4] * y + t[1], pz = r[6] * x + r[7] * y + t[2];
    return {px / pz, py / pz};
}
double RadiusSquaredOf(const Pose& pose, double x, double y) {
    const auto n = Normalized(pose.r, pose.t, x, y);
    return n.x * n.x + n.y * n.y;
}
// The distortion map alone, once more written term by term (a third form next to Project and the library's).
NormPoint DistortOnly(const Dist& d, double x, double y) {
    const double r2 = x * x + y * y;
    const double radial = 1.0 + d.k1 * r2 + d.k2 * r2 * r2 + d.k3 * r2 * r2 * r2;
    return {x * radial + 2.0 * d.p1 * x * y + d.p2 * (r2 + 2.0 * x * x), y * radial + d.p1 * (r2 + 2.0 * y * y) + 2.0 * d.p2 * x * y};
}
// Determinant of the full 2x2 Jacobian by central differences (no use of the symmetry the library relies on).
double NumericDeterminant(const Dist& d, double x, double y) {
    const double h = 1e-5;
    const auto xp = DistortOnly(d, x + h, y), xm = DistortOnly(d, x - h, y), yp = DistortOnly(d, x, y + h), ym = DistortOnly(d, x, y - h);
    const double dxdx = (xp.x - xm.x) / (2 * h), dydx = (xp.y - xm.y) / (2 * h), dxdy = (yp.x - ym.x) / (2 * h), dydy = (yp.y - ym.y) / (2 * h);
    return dxdx * dydy - dxdy * dydx;
}
// d/dr of r * L(r), by central differences in r (not through the polynomial g of the library).
double NumericRadialSlope(const Dist& d, double r) {
    const auto map = [&](double q) { const double q2 = q * q; return q * (1.0 + d.k1 * q2 + d.k2 * q2 * q2 + d.k3 * q2 * q2 * q2); };
    const double h = 1e-6;
    return (map(r + h) - map(r - h)) / (2 * h);
}
std::vector<ViewSpec> NoisyLayout(std::uint64_t seed, double sigma) {
    auto specs = Layout();
    const auto noise = std::make_shared<Gauss>(seed);
    for (auto& spec : specs) if (spec.role != "reference") { spec.noise = noise; spec.sigma = sigma; }
    return specs;
}
std::vector<ViewSpec> WithIntrinsics(std::vector<ViewSpec> specs, const Intrinsics& k) {
    for (auto& spec : specs) spec.k = k;
    return specs;
}
const ChartPt& ChartPoint(const std::vector<ChartPt>& chart, const std::string& id) {
    return *std::find_if(chart.begin(), chart.end(), [&](const ChartPt& p) { return p.id == id; });
}

// SR: the solver record and the distortion check, recomputed from the session and the reported lens and poses.
void TestSolverRecordAndCheckRecomputed() {
    const auto chart = Chart();
    auto specs = NoisyLayout(293, 0.25);
    // Which points are used is chosen so that the largest radius belongs to one of the middle views (not the first or the
    // last, which a check reading one view only would pick) and so that detected-but-unused points reach farther out:
    // the three largest radii of that view are left out, and the other views use only radii below 0.98 of what is left.
    std::vector<std::vector<double>> radius(6);
    for (std::size_t v = 0; v < 6; ++v) for (const auto& point : chart) radius[v].push_back(RadiusSquaredOf(specs[v].pose, point.x, point.y));
    const auto fourth_largest = [&](std::size_t v) { auto sorted = radius[v]; std::sort(sorted.begin(), sorted.end(), std::greater<double>()); return sorted[3]; };
    std::size_t peak_view = 1;
    for (std::size_t v = 2; v < 5; ++v) if (fourth_largest(v) > fourth_largest(peak_view)) peak_view = v;
    const double peak = fourth_largest(peak_view);
    for (std::size_t v = 0; v < 6; ++v) {
        specs[v].used.clear();
        for (std::size_t i = 0; i < chart.size(); ++i) {
            if (v == peak_view ? radius[v][i] <= peak : radius[v][i] <= 0.98 * peak) specs[v].used.push_back(chart[i].id);
        }
    }
    const auto policy = Policy();
    const auto result = Run(chart, specs, policy);
    Invariants(result, policy, "SR");
    const auto& a = result.cameras[0];
    const bool ready = a.status == LensStatus::LensIntrinsicsEstimated && a.intrinsics && a.distortion && a.distortion_check && a.solver && a.initialization.intrinsics;
    Check(ready, "SR: estimated");
    if (!ready) return;

    // (a) initial_cost is the cost at the stage 2 start: its K, no distortion, its poses.
    const auto& k0 = *a.initialization.intrinsics;
    double initial = 0;
    for (std::size_t v = 0; v < 6; ++v) {
        const IntrinsicsViewResult* base = nullptr;
        for (const auto& candidate : a.initialization.views) if (candidate.view_id == specs[v].id) base = &candidate;
        Check(base && base->pose, "SR: stage 2 pose: " + specs[v].id);
        if (!base || !base->pose) return;
        for (const auto& id : specs[v].used) {
            const auto& point = ChartPoint(chart, id);
            const auto pixel = specs[v].rendered.at(id);
            const auto p = Project({k0.fx, k0.fy, k0.cx, k0.cy}, kNoDist, base->pose->rotation, base->pose->translation_mm, point.x, point.y);
            initial += (p.u - pixel.first) * (p.u - pixel.first) + (p.v - pixel.second) * (p.v - pixel.second);
        }
    }
    Check(Near(a.solver->initial_cost, initial, 1e-12), "SR: initial_cost is the cost at the closed-form start (recomputed here)");

    // (b) first-order optimality: at the reported solution the residual vector is orthogonal to every Jacobian
    // column (the cosine between them is near zero), for all 45 unknowns, on noisy data.
    const std::size_t unknowns = 9 + 6 * 6;
    std::vector<double> gradient(unknowns, 0.0), norm_squared(unknowns, 0.0);
    double cost = 0;
    for (std::size_t v = 0; v < 6; ++v) {
        const auto* view = FindView(a, specs[v].id);
        if (!view || !view->pose) { Check(false, "SR: pose: " + specs[v].id); return; }
        for (const auto& id : specs[v].used) {
            const auto& point = ChartPoint(chart, id);
            const auto pixel = specs[v].rendered.at(id);
            const auto p = ld::ProjectChartPoint(*a.intrinsics, *a.distortion, *view->pose, point.x, point.y);
            if (!p) { Check(false, "SR: the point projects"); return; }
            const double residual[2] = {p->u - pixel.first, p->v - pixel.second};
            cost += residual[0] * residual[0] + residual[1] * residual[1];
            for (std::size_t row = 0; row < 2; ++row) for (std::size_t c = 0; c < 15; ++c) {
                const std::size_t column = c < 9 ? c : 9 + 6 * v + (c - 9);
                gradient[column] += p->jacobian[row][c] * residual[row];
                norm_squared[column] += p->jacobian[row][c] * p->jacobian[row][c];
            }
        }
    }
    double worst_cosine = 0;
    for (std::size_t j = 0; j < unknowns; ++j) worst_cosine = std::max(worst_cosine, std::abs(gradient[j]) / (std::sqrt(norm_squared[j]) * std::sqrt(cost)));
    Check(Near(a.solver->final_cost, cost, 1e-12), "SR: final_cost is the cost at the reported solution (recomputed here)");
    Check(worst_cosine <= 1e-7, "SR: the reported solution is a stationary point of the cost (worst cosine " + Num(worst_cosine) + ")");

    // (c) the distortion check, over the used points only.
    const Dist reported{a.distortion->k1, a.distortion->k2, a.distortion->k3, a.distortion->p1, a.distortion->p2};
    std::size_t used_radius_view = 0;
    double used_radius = 0, all_radius = 0, used_det = std::numeric_limits<double>::infinity(), all_det = std::numeric_limits<double>::infinity();
    for (std::size_t v = 0; v < 6; ++v) {
        const auto* view = FindView(a, specs[v].id);
        for (const auto& point : chart) {
            const auto n = Normalized(view->pose->rotation, view->pose->translation_mm, point.x, point.y);
            const double s = n.x * n.x + n.y * n.y;
            const double det = NumericDeterminant(reported, n.x, n.y);
            all_radius = std::max(all_radius, s);
            all_det = std::min(all_det, det);
            if (std::find(specs[v].used.begin(), specs[v].used.end(), point.id) != specs[v].used.end()) {
                if (s > used_radius) { used_radius = s; used_radius_view = v; }
                used_det = std::min(used_det, det);
            }
        }
    }
    const auto& check = *a.distortion_check;
    Check(used_radius_view == peak_view && used_radius_view != 0 && used_radius_view != 5, "SR: the largest used radius lies in a middle view (not the first or the last)");
    Check(Near(check.max_radius_squared, used_radius, 1e-12), "SR: max_radius_squared is the largest radius over the used points");
    Check(all_radius > used_radius * 1.01, "SR: the left-out points really have a larger radius, so the check can tell used from detected");
    Check(std::abs(check.minimum_jacobian_determinant - used_det) <= 1e-7, "SR: minimum_jacobian_determinant is the smallest determinant over the used points (central differences)");
    Check(all_det < used_det - 1e-4, "SR: the left-out points really have a smaller determinant");
    double sampled = std::numeric_limits<double>::infinity();
    for (int i = 0; i <= 2000; ++i) sampled = std::min(sampled, NumericRadialSlope(reported, std::sqrt(check.max_radius_squared) * i / 2000.0));
    Check(std::abs(check.minimum_radial_slope - sampled) <= 1e-8, "SR: minimum_radial_slope is the minimum of d(rL)/dr over [0, r_max] (central differences)");
    std::printf("info SR: worst cosine %.3e, radius used %.5f all %.5f, determinant used %.5f all %.5f, slope %.6f\n", worst_cosine, used_radius, all_radius, used_det, all_det, check.minimum_radial_slope);
}

// N2: the noise bands come from the sampling theory, and hold for several seeds.
void TestNoiseAcrossSeeds() {
    const double sigma = 0.25;
    const double parameters = 9 + 6 * 6;
    const std::vector<std::uint64_t> seeds{11, 29, 290, 1013, 4242, 777777};
    double mean_training = 0;
    for (const auto seed : seeds) {
        const auto policy = Policy();
        const auto result = Run(NoisyLayout(seed, sigma), policy);
        Invariants(result, policy, "N2/" + std::to_string(seed));
        const auto& a = result.cameras[0];
        Check(a.status == LensStatus::LensIntrinsicsEstimated && a.training_residual && a.held_out_residual, "N2: estimated: seed " + std::to_string(seed));
        if (!a.training_residual || !a.held_out_residual) continue;
        // Training: the sum of squares over 2N residual components with P fitted parameters has 2N - P degrees of freedom,
        // so rms^2 has the mean 2 sigma^2 (1 - P / 2N) and the relative standard deviation of rms is 1 / sqrt(2 (2N - P)).
        const double n = a.training_residual->point_count;
        const double expected = sigma * std::sqrt(2.0) * std::sqrt(1.0 - parameters / (2.0 * n));
        const double ratio = a.training_residual->rms_pixels / expected;
        const double spread = 1.0 / std::sqrt(2.0 * (2.0 * n - parameters));
        Check(std::abs(ratio - 1.0) <= 6.0 * spread, "N2: training rms within 6 standard deviations of the theory: seed " + std::to_string(seed));
        mean_training += ratio / static_cast<double>(seeds.size());
        // Held-out: 2 Nh independent components of variance sigma^2 (relative standard deviation of rms: 1 / sqrt(4 Nh)); the
        // estimated map and pose add error, which can only raise the rms; 3 % covers it (the probes show about 0.2 sigma).
        const double held_n = a.held_out_residual->point_count;
        const double held_ratio = a.held_out_residual->rms_pixels / (sigma * std::sqrt(2.0));
        const double held_spread = 1.0 / std::sqrt(4.0 * held_n);
        Check(held_ratio >= 1.0 - 6.0 * held_spread && held_ratio <= 1.0 + 6.0 * held_spread + 0.03, "N2: held-out rms within 6 standard deviations of the theory: seed " + std::to_string(seed));
        std::printf("info N2: seed %llu training %.4f held-out %.4f\n", static_cast<unsigned long long>(seed), ratio, held_ratio);
    }
    // The mean over the seeds is tighter by sqrt(S): 4 standard deviations of the mean.
    const double dof = 2.0 * 1326 - parameters;
    Check(std::abs(mean_training - 1.0) <= 4.0 / std::sqrt(2.0 * dof * static_cast<double>(seeds.size())), "N2: the mean training ratio over the seeds is within 4 standard deviations of 1 (" + Num(mean_training) + ")");
}

// DT: the determinant alone makes a lens invalid (the radial slope stays 1), end to end.
void TestDeterminantOnlyInvalid() {
    const Intrinsics wide{500, 520, 640, 480}; // keeps the strongly tangential truths inside the image
    const auto policy = Policy();
    const auto chart = Chart();
    {
        const Dist control{0, 0, 0, -0.3, 0};
        const auto specs = WithIntrinsics(Layout("CAM-A", control), wide);
        const auto result = Run(specs, policy);
        Invariants(result, policy, "DT-control");
        const auto& a = result.cameras[0];
        Check(a.status == LensStatus::LensIntrinsicsEstimated && a.distortion_check && a.distortion_check->minimum_jacobian_determinant > 0.1, "DT: p1 = -0.3 is still physical (determinant stays positive)");
        for (const auto& id : {"T1", "T2", "T3", "T4", "T5", "T6"}) for (const auto& spec : specs) if (spec.id == id) {
            Check(ProbeDifference(a, spec).max <= 1e-6, std::string("DT: the strongly tangential lens is recovered: ") + id);
        }
    }
    for (const auto& [name, truth] : {std::pair<const char*, Dist>{"p1 = -0.5", {0, 0, 0, -0.5, 0}}, std::pair<const char*, Dist>{"p2 = -0.5", {0, 0, 0, 0, -0.5}}}) {
        const auto specs = WithIntrinsics(Layout("CAM-A", truth), wide);
        const auto result = Run(specs, policy);
        Invariants(result, policy, std::string("DT/") + name);
        const auto& a = result.cameras[0];
        const std::string label = std::string("DT: ") + name + ": ";
        Check(a.status == LensStatus::InvalidDistortion && FailedStage(a.status) == LensStage::Distortion, label + "invalid-distortion at the distortion stage");
        Check(a.solver && ConvergentStop(a.solver->stop_reason), label + "the solver converged to the folding map; the check refuses it");
        Check(a.distortion_check && Near(a.distortion_check->minimum_radial_slope, 1.0, 1e-9), label + "the radial slope is 1 (no radial term), so the radial test alone would accept it");
        Check(a.distortion_check && a.distortion_check->minimum_jacobian_determinant < 0, label + "the determinant is negative");
        Check(a.intrinsics && !a.distortion && !a.training_residual, label + "K is kept, the coefficients are not published");
        Check(a.intrinsics && std::abs(a.intrinsics->fx - wide.fx) <= 1e-6 && std::abs(a.intrinsics->fy - wide.fy) <= 1e-6, label + "the true K is recovered, so the data (not the fit) are what is refused");
        // The smallest determinant over the used points, from the true poses and the true coefficients.
        double expected = std::numeric_limits<double>::infinity();
        for (std::size_t v = 0; v < 6; ++v) for (const auto& id : specs[v].used) {
            const auto& point = ChartPoint(chart, id);
            const auto n = Normalized(specs[v].pose.r, specs[v].pose.t, point.x, point.y);
            expected = std::min(expected, NumericDeterminant(truth, n.x, n.y));
        }
        Check(a.distortion_check && std::abs(a.distortion_check->minimum_jacobian_determinant - expected) <= 1e-6, label + "the reported determinant is the one of the true map (" + Num(expected) + ")");
        for (const auto& view : a.views) Check(view.outcome == LensViewOutcome::NotReached, label + "views are not reached: " + view.view_id);
        Check(a.held_out_coverage == HeldOutCoverage::NotReached, label + "held-out is not reached");
        if (a.distortion_check && a.solver) std::printf("info DT %s: reported det %.6f expected %.6f smax %.5f iterations %u accepted %u final cost %.3e\n", name, a.distortion_check->minimum_jacobian_determinant, expected, a.distortion_check->max_radius_squared, a.solver->iterations, a.solver->accepted_steps, a.solver->final_cost);
    }
}

/// CB: a measurement point that stage 2 finds in front of the camera but the refined held-out pose puts behind it.
// Stage 2 judges the measurement points with its own closed-form pose, so a plainly misplaced point is stopped there
// (initialization-unavailable); chart-not-in-front of the lens stage needs a point whose depth changes sign between the
// two poses. The first pass reads both poses of a tilted validation view, the second pass puts a point between the roots.
ViewSpec TiltedValidation(const std::vector<std::string>& measurement_ids, const std::vector<ChartPt>& extra, double ay, double tz) {
    ViewSpec tilted;
    tilted.id = "V3";
    tilted.role = "validation";
    tilted.d = kTrueDist;
    tilted.pose = MakePose(0, ay, 0, 0, 0, tz);
    for (const auto& point : Chart()) {
        if (point.x >= -100 && point.x <= 100 && point.y >= -50 && point.y <= 50) { tilted.observed.push_back(point); tilted.used.push_back(point.id); }
    }
    for (const auto& id : measurement_ids) {
        const auto found = std::find_if(extra.begin(), extra.end(), [&](const ChartPt& p) { return p.id == id; });
        tilted.observed.push_back(found != extra.end() ? *found : ChartPoint(Chart(), id));
        tilted.measurement.push_back(id);
    }
    return tilted;
}
// x of the chart point (x, 0) whose depth is zero: depth = r6 x + t2.
double DepthRoot(const ChartPose& pose) { return -pose.translation_mm[2] / pose.rotation[6]; }

void TestMeasurementPointsBehindTheCamera() {
    const auto policy = Policy();
    const auto base = Run(Layout(), policy);
    for (const double ay : {1.1, -1.1}) {
        const std::string label = std::string("CB (tilt ") + (ay > 0 ? "+" : "-") + "63 deg): ";
        // Pass 1: measurement points that are plainly in front. Both poses of V3 are read.
        auto specs = Layout();
        specs.insert(specs.begin() + 8, TiltedValidation({"P6_1", "P6_2", "P6_3"}, {}, ay, 300));
        const auto first = Run(specs, policy);
        Invariants(first, policy, "CB-pass1");
        const auto& a = first.cameras[0];
        const auto* v3 = FindView(a, "V3");
        const IntrinsicsViewResult* start = nullptr;
        for (const auto& candidate : a.initialization.views) if (candidate.view_id == "V3") start = &candidate;
        Check(v3 && v3->outcome == LensViewOutcome::Evaluated && v3->residual && v3->residual->point_count == 3 && v3->residual->max_pixels <= 1e-6, label + "pass 1: the view is evaluated");
        Check(v3 && v3->pose && start && start->pose, label + "pass 1: the closed-form pose and the held-out pose exist");
        if (!(v3 && v3->pose && start && start->pose)) continue;
        const double closed_form_root = DepthRoot(*start->pose), held_out_root = DepthRoot(*v3->pose);
        Check(closed_form_root * held_out_root > 0 && std::abs(closed_form_root) > std::abs(held_out_root) + 1.0,
            label + "the closed-form pose reaches depth 0 farther out than the held-out pose (" + Num(closed_form_root) + " mm and " + Num(held_out_root) + " mm)");
        // Pass 2: three measurement points between the two roots, in front under the closed-form pose and behind under the held-out pose.
        const double middle = 0.5 * (closed_form_root + held_out_root);
        const std::vector<ChartPt> far_points{{"FAR_A", middle, 0}, {"FAR_B", middle, 25}, {"FAR_C", middle, -25}};
        auto chart = Chart();
        for (const auto& point : far_points) chart.push_back(point);
        auto tilted = TiltedValidation({"FAR_A", "FAR_B", "FAR_C"}, far_points, ay, 300);
        double offset = 0;
        for (const auto& point : far_points) { tilted.pinned[point.id] = {200.5 + offset, 300.25 + offset}; offset += 10; }
        auto specs_behind = Layout();
        specs_behind.insert(specs_behind.begin() + 8, tilted);
        const auto second = Run(chart, specs_behind, policy);
        Invariants(second, policy, "CB-pass2");
        const auto& b = second.cameras[0];
        const auto* w3 = FindView(b, "V3");
        const IntrinsicsViewResult* w3_start = nullptr;
        for (const auto& candidate : b.initialization.views) if (candidate.view_id == "V3") w3_start = &candidate;
        Check(w3_start && w3_start->outcome == ViewOutcome::Evaluated, label + "pass 2: stage 2 still finds the measurement points in front (so the lens stage is what refuses)");
        Check(b.status == LensStatus::LensIntrinsicsEstimated, label + "a validation failure does not change the camera status");
        Check(w3 && w3->outcome == LensViewOutcome::ChartNotInFront && !w3->pose && !w3->residual, label + "chart-not-in-front, without a pose or a residual");
        Check(w3 && w3->solver && ConvergentStop(w3->solver->stop_reason), label + "the pose solve itself converged and is recorded");
        Check(b.held_out_coverage == HeldOutCoverage::SomeViewsNotEvaluated, label + "coverage reports the view that was not evaluated");
        Check(b.held_out_residual && b.held_out_residual->point_count == 220, label + "the pooled held-out residual covers only V1 and V2");
        Check(Fingerprint(b, true) == Fingerprint(base.cameras[0], true), label + "the training side is unchanged by the extra validation view (bitwise)");
    }
}

// SN: singular normal equations. Two training views with four points each give 21 unknowns and 16 residual
// components, so J^T J has rank 16. The smallest positive damping is far below the rounding error of the
// factorization, so the first Cholesky factorizations fail (or give a step that is rejected) and the damping
// has to be raised by factors of ten, about 308 times, before a step is possible. Every such trial counts.
void TestSingularNormalEquations() {
    auto specs = Layout();
    specs.resize(2);
    for (auto& spec : specs) spec.used = {"P0_0", "P0_16", "P12_0", "P12_16"};
    auto policy = Policy();
    policy.max_iterations = 1000;
    const auto damped = Run(specs, policy);
    Invariants(damped, policy, "SN-damped");
    policy.initial_damping = std::nextafter(0.0, 1.0);
    const auto bare = Run(specs, policy);
    Invariants(bare, policy, "SN-bare");
    const auto& a = damped.cameras[0];
    const auto& b = bare.cameras[0];
    Check(a.status == LensStatus::LensIntrinsicsEstimated && a.solver && a.solver->iterations < 20, "SN: the default damping handles the rank-deficient problem within a few trials");
    Check(b.status == LensStatus::LensIntrinsicsEstimated && b.solver && ConvergentStop(b.solver->stop_reason), "SN: the smallest damping still ends in a convergence rule, not in a failure");
    Check(b.solver && b.solver->iterations >= 300 && b.solver->iterations <= 330, "SN: the damping is raised tenfold per trial from 5e-324 up to the rounding level (iterations " + std::to_string(b.solver ? b.solver->iterations : 0) + ")");
    Check(b.solver && b.solver->accepted_steps == 0 && b.solver->final_cost == b.solver->initial_cost, "SN: no step was accepted, and the record says so");
    Check(a.intrinsics && b.intrinsics && Bitwise(a.intrinsics->fx, b.intrinsics->fx) && Bitwise(a.intrinsics->cy, b.intrinsics->cy), "SN: both runs report the start (the problem is already at zero cost)");
}

} // namespace

int main() {
    try {
        TestPolicyAndCodes();
        TestSignature();
        TestNoiseFree();
        TestCutoffForDistortedPixels();
        TestResidualsAreRecomputedIndependently();
        TestRolesOfPoints();
        TestNoise();
        TestIterationLimit();
        TestHeldOutIterationLimit();
        TestValidationViewFailure();
        TestPhysicality();
        TestInvalidDistortion();
        TestInitializationUnavailable();
        TestCamerasAreIndependent();
        TestDeterminism();
        TestZeroDistortion();
        TestLargeDamping();
        TestJacobian();
        TestSolverRecordAndCheckRecomputed();
        TestNoiseAcrossSeeds();
        TestDeterminantOnlyInvalid();
        TestMeasurementPointsBehindTheCamera();
        TestSingularNormalEquations();
#ifdef NDEBUG
        TestLargeProblem();
#endif
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        ++failures;
    }
    std::cout << "calibration_lens checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
