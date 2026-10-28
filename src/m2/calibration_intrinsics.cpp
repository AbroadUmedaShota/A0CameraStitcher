#include "a0/m2/calibration_intrinsics.hpp"
#include "calibration_planar_pose.hpp"

#include "calibration_exact_predicates.hpp"
#include "calibration_jacobi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace a0::m2::calibration {
namespace {
using Matrix3 = std::array<double, 9>;
using Vector3 = std::array<double, 3>;
using jacobi_detail::kUnresolvedTailResolution;
using jacobi_detail::OneSidedJacobi;

constexpr std::uint32_t kMinimumViewsLowerBound = 2;
constexpr std::uint32_t kMinimumViewsUpperBound = 128;

[[noreturn]] void InvalidPolicy() {
    throw std::invalid_argument("undistorted intrinsics: invalid policy");
}
// Same range as AssessViewGeometry, so that it never throws on a valid session.
void ValidatePolicy(const UndistortedIntrinsicsPolicy& policy) {
    if (policy.minimum_views < kMinimumViewsLowerBound || policy.minimum_views > kMinimumViewsUpperBound
        || !std::isfinite(policy.relative_singular_cutoff) || !(policy.relative_singular_cutoff > 0)
        || !(policy.relative_singular_cutoff < 1)
        || policy.observation_model != ObservationModel::UndistortedAnalytic) InvalidPolicy();
}

// One chart point with its detected pixel: chart mm (x, y) and pixel (u, v).
struct Correspondence {
    double x, y, u, v;
};

Matrix3 Multiply(const Matrix3& a, const Matrix3& b) {
    Matrix3 product{};
    for (std::size_t row = 0; row < 3; ++row) for (std::size_t column = 0; column < 3; ++column) {
        double sum = 0;
        for (std::size_t k = 0; k < 3; ++k) sum += a[row * 3 + k] * b[k * 3 + column];
        product[row * 3 + column] = sum;
    }
    return product;
}

// ---------------------------------------------------------------------------
// Homography by DLT with Hartley normalization. The 2N x 9 system is reduced
// to a 9 x 9 triangular factor by Givens rotations, one row at a time, and the
// shared Jacobi decomposition is applied to that factor. The normal equations
// are never formed, so the condition number is not squared.

struct Hartley {
    double mean_a, mean_b, scale;
};
std::optional<Hartley> NormalizationOf(const std::vector<std::array<double, 2>>& points) {
    double sum_a = 0, sum_b = 0;
    for (const auto& point : points) {
        sum_a += point[0];
        sum_b += point[1];
    }
    const auto count = static_cast<double>(points.size());
    const double mean_a = sum_a / count, mean_b = sum_b / count;
    double distance = 0;
    for (const auto& point : points) distance += std::hypot(point[0] - mean_a, point[1] - mean_b);
    distance /= count;
    const double scale = std::sqrt(2.0) / distance;
    if (!std::isfinite(mean_a) || !std::isfinite(mean_b) || !std::isfinite(scale) || !(scale > 0)) return std::nullopt;
    return Hartley{mean_a, mean_b, scale};
}

void AddRow(jacobi_detail::Matrix<9>& triangular, std::array<double, 9> row) {
    for (std::size_t j = 0; j < 9; ++j) {
        if (row[j] == 0) continue;
        const double radius = std::hypot(triangular[j][j], row[j]);
        const double cosine = triangular[j][j] / radius, sine = row[j] / radius;
        for (std::size_t k = j; k < 9; ++k) {
            const double old_triangular = triangular[j][k], old_row = row[k];
            triangular[j][k] = cosine * old_triangular + sine * old_row;
            row[k] = cosine * old_row - sine * old_triangular;
        }
    }
}

std::optional<Matrix3> FitHomography(const std::vector<Correspondence>& points) {
    std::vector<std::array<double, 2>> chart, image;
    chart.reserve(points.size());
    image.reserve(points.size());
    for (const auto& point : points) {
        chart.push_back({point.x, point.y});
        image.push_back({point.u, point.v});
    }
    const auto chart_norm = NormalizationOf(chart);
    const auto image_norm = NormalizationOf(image);
    if (!chart_norm || !image_norm) return std::nullopt;

    jacobi_detail::Matrix<9> triangular(9);
    for (auto& row : triangular) row.fill(0);
    for (const auto& point : points) {
        const double x = chart_norm->scale * (point.x - chart_norm->mean_a);
        const double y = chart_norm->scale * (point.y - chart_norm->mean_b);
        const double u = image_norm->scale * (point.u - image_norm->mean_a);
        const double v = image_norm->scale * (point.v - image_norm->mean_b);
        AddRow(triangular, {-x, -y, -1, 0, 0, 0, u * x, u * y, u});
        AddRow(triangular, {0, 0, 0, -x, -y, -1, v * x, v * y, v});
    }
    const auto decomposition = OneSidedJacobi<9>(std::move(triangular));
    // The ninth value is the null direction. It is only required that the
    // eighth is resolved from the first, so an unresolved tail is expected.
    if (!decomposition.converged
        || !(decomposition.values[7] > kUnresolvedTailResolution * decomposition.values[0])) return std::nullopt;
    const Matrix3 normalized = decomposition.vectors[8];
    const Matrix3 image_inverse{1 / image_norm->scale, 0, image_norm->mean_a,
        0, 1 / image_norm->scale, image_norm->mean_b, 0, 0, 1};
    const Matrix3 chart_transform{chart_norm->scale, 0, -chart_norm->scale * chart_norm->mean_a,
        0, chart_norm->scale, -chart_norm->scale * chart_norm->mean_b, 0, 0, 1};
    auto h = Multiply(Multiply(image_inverse, normalized), chart_transform);

    double norm = 0;
    for (const auto value : h) {
        if (!std::isfinite(value)) return std::nullopt;
        norm = std::hypot(norm, value);
    }
    if (!(norm > 0) || !std::isfinite(norm)) return std::nullopt;
    for (auto& value : h) value /= norm;
    // Positive w at the centroid of the chart points used fixes the sign.
    const double weight = h[6] * chart_norm->mean_a + h[7] * chart_norm->mean_b + h[8];
    if (!std::isfinite(weight) || weight == 0) return std::nullopt;
    if (weight < 0) for (auto& value : h) value = -value;
    for (const auto value : h) if (!std::isfinite(value)) return std::nullopt;
    if (!exact_detail::ExactNonzeroDeterminant(h)) return std::nullopt;
    return h;
}

// ---------------------------------------------------------------------------
// Intrinsics from the null vector of the conditioned constraint rows.

std::optional<PinholeIntrinsics> IntrinsicsFromConic(const ConditionedConic& conic) {
    auto b = conic.b;
    for (const auto value : b) if (!std::isfinite(value)) return std::nullopt;
    if (b[0] == 0) return std::nullopt;
    const double sign = b[0] > 0 ? 1.0 : -1.0;
    for (auto& value : b) value *= sign;
    // b = [B11, B12, B22, B13, B23, B33]. B12 is not used: skew is zero by model.
    const double b11 = b[0], b22 = b[2], b13 = b[3], b23 = b[4], b33 = b[5];
    if (!(b22 > 0)) return std::nullopt;
    const double mu = b33 - (b13 * b13) / b11 - (b23 * b23) / b22;
    if (!(mu > 0)) return std::nullopt;
    const double scaled_fx = std::sqrt(mu / b11), scaled_fy = std::sqrt(mu / b22);
    const double scaled_cx = -b13 / b11, scaled_cy = -b23 / b22;
    PinholeIntrinsics k{conic.scale * scaled_fx, conic.scale * scaled_fy,
        conic.scale * scaled_cx + conic.center_x, conic.scale * scaled_cy + conic.center_y};
    if (!std::isfinite(k.fx) || !std::isfinite(k.fy) || !std::isfinite(k.cx) || !std::isfinite(k.cy)
        || !(k.fx > 0) || !(k.fy > 0)) return std::nullopt;
    return k;
}

// ---------------------------------------------------------------------------
// Pose from a homography: columns of K^-1 H, scaled by the mean of the first
// two column norms, then the nearest rotation through the SVD of the 3 x 3.

std::optional<ChartPose> EstimatePose(const PinholeIntrinsics& k, const Matrix3& h) {
    const auto column = [&](std::size_t c) {
        return Vector3{(h[c] - k.cx * h[6 + c]) / k.fx, (h[3 + c] - k.cy * h[6 + c]) / k.fy, h[6 + c]};
    };
    const auto m1 = column(0), m2 = column(1), m3 = column(2);
    const double length_sum = std::hypot(m1[0], m1[1], m1[2]) + std::hypot(m2[0], m2[1], m2[2]);
    if (!std::isfinite(length_sum) || !(length_sum > 0)) return std::nullopt;
    const double lambda = 2 / length_sum;
    const Vector3 r1{lambda * m1[0], lambda * m1[1], lambda * m1[2]};
    const Vector3 r2{lambda * m2[0], lambda * m2[1], lambda * m2[2]};
    const Vector3 translation{lambda * m3[0], lambda * m3[1], lambda * m3[2]};
    const Vector3 r3{r1[1] * r2[2] - r1[2] * r2[1], r1[2] * r2[0] - r1[0] * r2[2], r1[0] * r2[1] - r1[1] * r2[0]};
    std::array<std::array<double, 3>, 3> rows{};
    for (std::size_t i = 0; i < 3; ++i) rows[i] = {r1[i], r2[i], r3[i]};
    const auto decomposition = OneSidedJacobi<3>(jacobi_detail::Matrix<3>(rows.begin(), rows.end()));
    if (!decomposition.converged
        || !(decomposition.values[2] > kUnresolvedTailResolution * decomposition.values[0])) return std::nullopt;
    // R = sum_k (Q v_k / sigma_k) v_k^T
    ChartPose pose{};
    for (std::size_t i = 0; i < 3; ++i) for (std::size_t l = 0; l < 3; ++l) {
        double sum = 0;
        for (std::size_t k_index = 0; k_index < 3; ++k_index) {
            const auto& vector = decomposition.vectors[k_index];
            double product = 0;
            for (std::size_t m = 0; m < 3; ++m) product += rows[i][m] * vector[m];
            sum += product / decomposition.values[k_index] * vector[l];
        }
        pose.rotation[i * 3 + l] = sum;
    }
    pose.translation_mm = translation;
    for (const auto value : pose.rotation) if (!std::isfinite(value)) return std::nullopt;
    for (const auto value : pose.translation_mm) if (!std::isfinite(value)) return std::nullopt;
    return pose;
}

double Depth(const ChartPose& pose, double x, double y) {
    return pose.rotation[6] * x + pose.rotation[7] * y + pose.translation_mm[2];
}
bool AllInFront(const ChartPose& pose, const std::vector<Correspondence>& points) {
    for (const auto& point : points) {
        const double depth = Depth(pose, point.x, point.y);
        if (!std::isfinite(depth) || !(depth > 0)) return false;
    }
    return true;
}

struct ResidualSum {
    double squares = 0;
    double maximum = 0;
    std::uint32_t count = 0;
    void Merge(const ResidualSum& other) {
        squares += other.squares;
        maximum = std::max(maximum, other.maximum);
        count += other.count;
    }
};
// False if any error is not finite.
bool Accumulate(ResidualSum& sum, const PinholeIntrinsics& k, const ChartPose& pose, const std::vector<Correspondence>& points) {
    const auto& r = pose.rotation;
    const auto& t = pose.translation_mm;
    for (const auto& point : points) {
        const double x = r[0] * point.x + r[1] * point.y + t[0];
        const double y = r[3] * point.x + r[4] * point.y + t[1];
        const double z = r[6] * point.x + r[7] * point.y + t[2];
        const double error = std::hypot(k.fx * x / z + k.cx - point.u, k.fy * y / z + k.cy - point.v);
        if (!std::isfinite(error)) return false;
        sum.squares += error * error;
        sum.maximum = std::max(sum.maximum, error);
        ++sum.count;
    }
    return true;
}
std::optional<ResidualSummary> Summarize(const ResidualSum& sum) {
    if (sum.count == 0) return std::nullopt;
    const double rms = std::sqrt(sum.squares / static_cast<double>(sum.count));
    if (!std::isfinite(rms)) return std::nullopt;
    return ResidualSummary{rms, sum.maximum, sum.count};
}

// ---------------------------------------------------------------------------
// Session access.

struct Gate {
    bool insufficient = false;
    bool degenerate = false;
};
using Gates = std::map<std::string, Gate, std::less<>>;
Gates GatesOf(const CalibrationObservationSession& session) {
    Gates gates;
    for (const auto& diagnostic : DiagnoseObservationSession(session)) {
        if (!diagnostic.view_id) continue;
        if (diagnostic.code == SessionDiagnosticCode::InsufficientObservations) gates[*diagnostic.view_id].insufficient = true;
        else if (diagnostic.code == SessionDiagnosticCode::DegeneratePointConfiguration) gates[*diagnostic.view_id].degenerate = true;
    }
    return gates;
}
Gate GateOf(const Gates& gates, const std::string& id) {
    const auto found = gates.find(id);
    return found == gates.end() ? Gate{} : found->second;
}

// Chart coordinates and detected pixel of each named point, in list order.
// Parse guarantees that every named point is charted and detected.
std::optional<std::vector<Correspondence>> Gather(const CalibrationObservationSession& session,
    const SessionView& view, const std::vector<std::string>& ids) {
    std::unordered_map<std::string_view, PixelPosition> pixels; // looked up only, never iterated
    pixels.reserve(view.observations.size());
    for (const auto& observation : view.observations) {
        if (observation.pixel) pixels.emplace(observation.point_id, *observation.pixel);
    }
    std::vector<Correspondence> points;
    points.reserve(ids.size());
    for (const auto& id : ids) {
        const auto* chart = session.FindChartPoint(id);
        const auto pixel = pixels.find(id);
        if (chart == nullptr || pixel == pixels.end()) return std::nullopt;
        points.push_back({chart->x_mm, chart->y_mm, pixel->second.x, pixel->second.y});
    }
    return points;
}

IntrinsicsStatus StatusOf(ViewGeometryStatus status) {
    switch (status) {
    case ViewGeometryStatus::InsufficientViews: return IntrinsicsStatus::InsufficientViews;
    case ViewGeometryStatus::InsufficientDiversity: return IntrinsicsStatus::InsufficientViewDiversity;
    case ViewGeometryStatus::InconsistentLinearConstraints: return IntrinsicsStatus::InconsistentLinearConstraints;
    case ViewGeometryStatus::LinearConstraintsSufficient: break;
    case ViewGeometryStatus::NumericalFailure: break;
    }
    return IntrinsicsStatus::NumericalFailure;
}

// Homography, pose and residual of one validation view. The camera status is
// never changed by a failure here.
void EvaluateValidation(const CalibrationObservationSession& session, const SessionView& view,
    const PinholeIntrinsics& k, const Gates& gates, IntrinsicsViewResult& result, ResidualSum& pooled) {
    const auto gate = GateOf(gates, view.id);
    if (gate.insufficient) { result.outcome = ViewOutcome::InsufficientObservations; return; }
    if (gate.degenerate) { result.outcome = ViewOutcome::DegeneratePointConfiguration; return; }
    const auto pose_points = Gather(session, view, view.pose_point_ids);
    const auto measurement_points = Gather(session, view, view.measurement_point_ids);
    const auto h = pose_points ? FitHomography(*pose_points) : std::nullopt;
    if (!h || !measurement_points) { result.outcome = ViewOutcome::NumericalFailure; return; }
    result.chart_to_image = *h;
    const auto pose = EstimatePose(k, *h);
    if (!pose) { result.outcome = ViewOutcome::NumericalFailure; return; }
    if (!AllInFront(*pose, *pose_points) || !AllInFront(*pose, *measurement_points)) {
        result.outcome = ViewOutcome::ChartNotInFront;
        return;
    }
    ResidualSum sum;
    if (!Accumulate(sum, k, *pose, *measurement_points)) { result.outcome = ViewOutcome::NumericalFailure; return; }
    result.pose = *pose;
    result.residual = Summarize(sum);
    result.outcome = ViewOutcome::Evaluated;
    pooled.Merge(sum);
}

CameraIntrinsicsResult EvaluateCamera(const CalibrationObservationSession& session, CameraAlias camera,
    const UndistortedIntrinsicsPolicy& policy, const Gates& gates) {
    CameraIntrinsicsResult out{camera, IntrinsicsStatus::HomographyUnavailable, std::nullopt, std::nullopt, {},
        std::nullopt, HeldOutCoverage::NotReached, std::nullopt};
    // Training and validation views of this camera, in session order. Reference
    // views are not read at all.
    std::vector<const SessionView*> members;
    std::vector<std::size_t> training, validation;
    for (const auto& view : session.Views()) {
        if (view.camera != camera || view.role == ViewRole::Reference) continue;
        (view.role == ViewRole::Training ? training : validation).push_back(members.size());
        members.push_back(&view);
        out.views.push_back({view.id, view.role, ViewOutcome::NotReached, std::nullopt, std::nullopt, std::nullopt});
    }

    // Stage 1: homographies of all training views. One failure stops the
    // camera; the failing view is never dropped silently.
    std::vector<std::vector<Correspondence>> training_points(training.size());
    bool homographies_available = true;
    for (std::size_t t = 0; t < training.size(); ++t) {
        const auto& view = *members[training[t]];
        auto& result = out.views[training[t]];
        const auto gate = GateOf(gates, view.id);
        if (gate.insufficient) { result.outcome = ViewOutcome::InsufficientObservations; homographies_available = false; continue; }
        if (gate.degenerate) { result.outcome = ViewOutcome::DegeneratePointConfiguration; homographies_available = false; continue; }
        auto points = Gather(session, view, view.used_point_ids);
        const auto h = points ? FitHomography(*points) : std::nullopt;
        if (!h) { result.outcome = ViewOutcome::NumericalFailure; homographies_available = false; continue; }
        result.chart_to_image = *h;
        training_points[t] = std::move(*points);
    }
    if (!homographies_available) return out;

    // Stage 2: the existing linear-constraint preflight, result kept as is.
    std::vector<ViewHomography> homographies;
    homographies.reserve(training.size());
    for (const auto slot : training) homographies.push_back({out.views[slot].view_id, *out.views[slot].chart_to_image});
    const auto& group = session.Camera(camera);
    const ViewGeometryOptions options{group.width_pixels, group.height_pixels, policy.minimum_views, policy.relative_singular_cutoff};
    out.view_geometry = AssessViewGeometry(homographies, options);
    if (out.view_geometry->status != ViewGeometryStatus::LinearConstraintsSufficient) {
        out.status = StatusOf(out.view_geometry->status);
        return out;
    }

    // Stage 3: intrinsics. Not clamped; a nonphysical vector gives no value.
    if (!out.view_geometry->conditioned_conic) {
        out.status = IntrinsicsStatus::NumericalFailure; // unreachable: Sufficient always carries the vector
        return out;
    }
    const auto intrinsics = IntrinsicsFromConic(*out.view_geometry->conditioned_conic);
    if (!intrinsics) {
        out.status = IntrinsicsStatus::NonphysicalIntrinsics;
        return out;
    }
    out.intrinsics = *intrinsics;

    // Stage 4: pose and training residual of every training view.
    bool poses_available = true;
    ResidualSum pooled_training;
    for (std::size_t t = 0; t < training.size(); ++t) {
        auto& result = out.views[training[t]];
        const auto pose = EstimatePose(*intrinsics, *result.chart_to_image);
        if (!pose) { result.outcome = ViewOutcome::NumericalFailure; poses_available = false; continue; }
        if (!AllInFront(*pose, training_points[t])) { result.outcome = ViewOutcome::ChartNotInFront; poses_available = false; continue; }
        ResidualSum sum;
        if (!Accumulate(sum, *intrinsics, *pose, training_points[t])) {
            result.outcome = ViewOutcome::NumericalFailure;
            poses_available = false;
            continue;
        }
        result.pose = *pose;
        result.residual = Summarize(sum);
        result.outcome = ViewOutcome::Evaluated;
        pooled_training.Merge(sum);
    }
    if (!poses_available) {
        out.status = IntrinsicsStatus::PoseUnavailable;
        return out;
    }
    out.training_residual = Summarize(pooled_training);
    out.status = IntrinsicsStatus::UndistortedIntrinsicsEstimated;

    // Stage 5: held-out residual of the validation views.
    if (validation.empty()) {
        out.held_out_coverage = HeldOutCoverage::NoValidationView;
        return out;
    }
    bool all_evaluated = true;
    ResidualSum pooled_validation;
    for (const auto slot : validation) {
        EvaluateValidation(session, *members[slot], *intrinsics, gates, out.views[slot], pooled_validation);
        all_evaluated = all_evaluated && out.views[slot].outcome == ViewOutcome::Evaluated;
    }
    out.held_out_coverage = all_evaluated ? HeldOutCoverage::AllViewsEvaluated : HeldOutCoverage::SomeViewsNotEvaluated;
    out.held_out_residual = Summarize(pooled_validation);
    return out;
}
} // namespace

std::optional<ChartPose> pose_detail::EstimatePlanarPose(
    std::span<const PlanarObservation> observations, const PinholeIntrinsics& k) {
    if (observations.size() < kMinimumMappingPoints || !std::isfinite(k.fx) || !std::isfinite(k.fy)
        || !std::isfinite(k.cx) || !std::isfinite(k.cy) || !(k.fx > 0 && k.fy > 0)) return std::nullopt;
    std::vector<Correspondence> points;
    points.reserve(observations.size());
    for (const auto& point : observations) {
        if (!std::isfinite(point.x_mm) || !std::isfinite(point.y_mm)
            || !std::isfinite(point.u) || !std::isfinite(point.v)) return std::nullopt;
        points.push_back({point.x_mm, point.y_mm, point.u, point.v});
    }
    const auto h = FitHomography(points);
    if (!h) return std::nullopt;
    const auto pose = EstimatePose(k, *h);
    if (!pose || !AllInFront(*pose, points)) return std::nullopt;
    return pose;
}

std::optional<std::array<double, 9>> pose_detail::FitPlanarHomography(std::span<const PlanarObservation> observations) {
    if (observations.size() < kMinimumMappingPoints) return std::nullopt;
    std::vector<Correspondence> points;
    points.reserve(observations.size());
    for (const auto& point : observations) {
        if (!std::isfinite(point.x_mm) || !std::isfinite(point.y_mm)
            || !std::isfinite(point.u) || !std::isfinite(point.v)) return std::nullopt;
        points.push_back({point.x_mm, point.y_mm, point.u, point.v});
    }
    return FitHomography(points);
}

UndistortedIntrinsicsResult EstimateUndistortedIntrinsics(const CalibrationObservationSession& session,
    const UndistortedIntrinsicsPolicy& policy) {
    ValidatePolicy(policy);
    const auto gates = GatesOf(session);
    return UndistortedIntrinsicsResult{policy.observation_model, {
        EvaluateCamera(session, CameraAlias::CamA, policy, gates),
        EvaluateCamera(session, CameraAlias::CamB, policy, gates)}};
}

} // namespace a0::m2::calibration
