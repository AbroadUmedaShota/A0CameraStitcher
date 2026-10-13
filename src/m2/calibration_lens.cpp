#include "a0/m2/calibration_lens.hpp"

#include "calibration_lens_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace a0::m2::calibration {
namespace {
using lens_detail::ProjectChartPoint;
using Matrix3 = std::array<double, 9>;

constexpr std::uint32_t kMinimumViewsLowerBound = 2;
constexpr std::uint32_t kMinimumViewsUpperBound = 128;
constexpr std::uint32_t kMaximumIterationsUpperBound = 1000;

[[noreturn]] void InvalidPolicy() {
    throw std::invalid_argument("lens intrinsics: invalid policy");
}
bool InOpenUnitInterval(double value) {
    return std::isfinite(value) && value > 0 && value < 1;
}
// Same range as stage 2 for the first two fields, so that stage 2 never throws on a valid policy.
void ValidatePolicy(const LensIntrinsicsPolicy& policy) {
    if (policy.minimum_views < kMinimumViewsLowerBound || policy.minimum_views > kMinimumViewsUpperBound
        || !InOpenUnitInterval(policy.relative_singular_cutoff)
        || policy.observation_model != ObservationModel::DistortedAnalytic
        || policy.max_iterations < 1 || policy.max_iterations > kMaximumIterationsUpperBound
        || !InOpenUnitInterval(policy.relative_cost_tolerance) || !InOpenUnitInterval(policy.relative_step_tolerance)
        || !std::isfinite(policy.initial_damping) || !(policy.initial_damping > 0)) InvalidPolicy();
}

// One chart point with its detected pixel: chart mm (x, y) and pixel (u, v).
struct Correspondence {
    double x, y, u, v;
};
using Points = std::vector<Correspondence>;

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
// Levenberg-Marquardt with Marquardt's diagonal scaling.
//
// theta = [fx fy cx cy k1 k2 k3 p1 p2 | (omega, t) per view]. The first nine
// are left out when the intrinsics are fixed (held-out pose estimation). The
// rotation of a view is kept as a matrix and updated by R <- Exp(omega) R.

struct Parameters {
    PinholeIntrinsics k;
    BrownConradyDistortion d;
    std::vector<ChartPose> poses;
};

// Sum of the squared residual components, or null if any point is behind the
// camera or any value is not finite.
std::optional<double> Cost(const Parameters& parameters, const std::vector<Points>& views) {
    double cost = 0;
    for (std::size_t v = 0; v < views.size(); ++v) for (const auto& point : views[v]) {
        const auto projection = ProjectChartPoint(parameters.k, parameters.d, parameters.poses[v], point.x, point.y, false);
        if (!projection) return std::nullopt;
        const double du = projection->u - point.u, dv = projection->v - point.v;
        cost += du * du + dv * dv;
    }
    if (!std::isfinite(cost)) return std::nullopt;
    return cost;
}

// A = J^T J (both triangles) and g = J^T r. Points are added in order, u row then v row.
bool Assemble(const Parameters& parameters, const std::vector<Points>& views, bool fixed, std::size_t n,
    std::vector<double>& a, std::vector<double>& g) {
    a.assign(n * n, 0.0);
    g.assign(n, 0.0);
    const std::size_t intrinsic_count = fixed ? 0 : lens_detail::kIntrinsicColumns;
    const std::size_t first_local = fixed ? lens_detail::kOmega : 0;
    const std::size_t used = lens_detail::kJacobianColumns - first_local;
    std::array<std::size_t, lens_detail::kJacobianColumns> local{}, global{};
    for (std::size_t v = 0; v < views.size(); ++v) {
        for (std::size_t c = 0; c < used; ++c) {
            local[c] = first_local + c;
            global[c] = local[c] < lens_detail::kIntrinsicColumns ? local[c]
                                                                  : intrinsic_count + 6 * v + (local[c] - lens_detail::kOmega);
        }
        for (const auto& point : views[v]) {
            const auto projection = ProjectChartPoint(parameters.k, parameters.d, parameters.poses[v], point.x, point.y);
            if (!projection) return false;
            const std::array<double, 2> residual{projection->u - point.u, projection->v - point.v};
            for (std::size_t row = 0; row < 2; ++row) {
                const auto& jacobian = projection->jacobian[row];
                for (std::size_t c1 = 0; c1 < used; ++c1) {
                    const double j1 = jacobian[local[c1]];
                    g[global[c1]] += j1 * residual[row];
                    double* a_row = a.data() + global[c1] * n;
                    for (std::size_t c2 = 0; c2 < used; ++c2) a_row[global[c2]] += j1 * jacobian[local[c2]];
                }
            }
        }
    }
    for (const auto value : g) if (!std::isfinite(value)) return false;
    for (const auto value : a) if (!std::isfinite(value)) return false;
    return true;
}

// In-place lower Cholesky factor of the symmetric matrix held in the lower
// triangle of l (row major). False if a pivot is not strictly positive and finite.
bool Factorize(std::vector<double>& l, std::size_t n) {
    double* base = l.data();
    for (std::size_t j = 0; j < n; ++j) {
        double* row_j = base + j * n;
        double pivot = row_j[j];
        for (std::size_t k = 0; k < j; ++k) pivot -= row_j[k] * row_j[k];
        if (!std::isfinite(pivot) || !(pivot > 0)) return false;
        const double diagonal = std::sqrt(pivot);
        row_j[j] = diagonal;
        for (std::size_t i = j + 1; i < n; ++i) {
            double* row_i = base + i * n;
            double sum = row_i[j];
            for (std::size_t k = 0; k < j; ++k) sum -= row_i[k] * row_j[k];
            row_i[j] = sum / diagonal;
        }
    }
    return true;
}

// Solves L L^T x = b in place (b becomes x).
void SolveFactored(const std::vector<double>& l, std::size_t n, std::vector<double>& b) {
    const double* base = l.data();
    for (std::size_t i = 0; i < n; ++i) {
        const double* row = base + i * n;
        double sum = b[i];
        for (std::size_t k = 0; k < i; ++k) sum -= row[k] * b[k];
        b[i] = sum / row[i];
    }
    for (std::size_t i = n; i-- > 0;) {
        double sum = b[i];
        for (std::size_t k = i + 1; k < n; ++k) sum -= base[k * n + i] * b[k];
        b[i] = sum / base[i * n + i];
    }
}

Parameters Apply(const Parameters& base, const std::vector<double>& delta, bool fixed) {
    Parameters next = base;
    std::size_t offset = 0;
    if (!fixed) {
        next.k.fx += delta[0];
        next.k.fy += delta[1];
        next.k.cx += delta[2];
        next.k.cy += delta[3];
        next.d.k1 += delta[4];
        next.d.k2 += delta[5];
        next.d.k3 += delta[6];
        next.d.p1 += delta[7];
        next.d.p2 += delta[8];
        offset = lens_detail::kIntrinsicColumns;
    }
    for (std::size_t v = 0; v < next.poses.size(); ++v) {
        const std::size_t o = offset + 6 * v;
        next.poses[v].rotation = Multiply(lens_detail::RotationIncrement({delta[o], delta[o + 1], delta[o + 2]}), base.poses[v].rotation);
        for (std::size_t j = 0; j < 3; ++j) next.poses[v].translation_mm[j] += delta[o + 3 + j];
    }
    return next;
}

// Euclidean norm of the parameters that carry a unit: K, the coefficients and
// the translations. The rotations are increments and enter as zero.
double ParameterNorm(const Parameters& parameters, bool fixed) {
    double sum = 0;
    if (!fixed) {
        for (const auto value : {parameters.k.fx, parameters.k.fy, parameters.k.cx, parameters.k.cy, parameters.d.k1,
                 parameters.d.k2, parameters.d.k3, parameters.d.p1, parameters.d.p2}) sum += value * value;
    }
    for (const auto& pose : parameters.poses) for (const auto value : pose.translation_mm) sum += value * value;
    return std::sqrt(sum);
}

struct LmOutcome {
    Parameters parameters;
    SolverRecord record;
};

// Null when the start is unusable (a point behind the camera or not finite).
std::optional<LmOutcome> RunLevenbergMarquardt(Parameters start, const std::vector<Points>& views,
    const LensIntrinsicsPolicy& policy, bool fixed) {
    const auto initial_cost = Cost(start, views);
    if (!initial_cost) return std::nullopt;
    const std::size_t n = (fixed ? 0 : lens_detail::kIntrinsicColumns) + 6 * views.size();
    LmOutcome outcome{std::move(start), SolverRecord{0, 0, *initial_cost, *initial_cost, SolverStopReason::MaximumIterations}};
    auto& current = outcome.parameters;
    auto& record = outcome.record;
    double cost = *initial_cost;
    double damping = policy.initial_damping;
    bool stopped = false;

    std::vector<double> a, g, factor(n * n), scale(n), step(n);
    if (!Assemble(current, views, fixed, n, a, g)) {
        record.stop_reason = SolverStopReason::NumericalFailure;
        return outcome;
    }
    while (record.iterations < policy.max_iterations && !stopped) {
        ++record.iterations;
        bool scale_ok = true;
        for (std::size_t j = 0; j < n; ++j) {
            scale[j] = std::sqrt(a[j * n + j]);
            if (!std::isfinite(scale[j]) || !(scale[j] > 0)) scale_ok = false;
        }
        if (!scale_ok) {
            record.stop_reason = SolverStopReason::NumericalFailure;
            stopped = true;
            break;
        }
        // (A^ + damping I) s^ = -g^ with A^ = S^-1 A S^-1 (unit diagonal) and g^ = S^-1 g.
        for (std::size_t i = 0; i < n; ++i) {
            double* row = factor.data() + i * n;
            const double* source = a.data() + i * n;
            for (std::size_t j = 0; j < i; ++j) row[j] = source[j] / (scale[i] * scale[j]);
            row[i] = 1.0 + damping;
            step[i] = -g[i] / scale[i];
        }
        if (!Factorize(factor, n)) {
            damping *= 10.0;
            if (!std::isfinite(damping)) { record.stop_reason = SolverStopReason::NumericalFailure; stopped = true; }
            continue;
        }
        SolveFactored(factor, n, step);
        double step_squares = 0;
        for (std::size_t j = 0; j < n; ++j) {
            step[j] /= scale[j];
            step_squares += step[j] * step[j];
        }
        const double step_norm = std::sqrt(step_squares);
        if (!std::isfinite(step_norm)) {
            damping *= 10.0;
            if (!std::isfinite(damping)) { record.stop_reason = SolverStopReason::NumericalFailure; stopped = true; }
            continue;
        }
        const double tolerance = policy.relative_step_tolerance;
        if (step_norm <= tolerance * (ParameterNorm(current, fixed) + tolerance)) {
            record.stop_reason = SolverStopReason::RelativeStepSize;
            stopped = true;
            break;
        }
        auto candidate = Apply(current, step, fixed);
        const auto candidate_cost = Cost(candidate, views);
        if (candidate_cost && *candidate_cost < cost) {
            const double previous_cost = cost;
            current = std::move(candidate);
            cost = *candidate_cost;
            record.final_cost = cost;
            ++record.accepted_steps;
            damping = std::max(damping / 10.0, std::numeric_limits<double>::min());
            if (previous_cost - cost <= policy.relative_cost_tolerance * previous_cost) {
                record.stop_reason = SolverStopReason::RelativeCostReduction;
                stopped = true;
                break;
            }
            if (!Assemble(current, views, fixed, n, a, g)) {
                record.stop_reason = SolverStopReason::NumericalFailure;
                stopped = true;
                break;
            }
        } else {
            damping *= 10.0;
            if (!std::isfinite(damping)) { record.stop_reason = SolverStopReason::NumericalFailure; stopped = true; }
        }
    }
    return outcome;
}

// ---------------------------------------------------------------------------
// Residuals.

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
// False if a point is behind the camera or an error is not finite.
bool Accumulate(ResidualSum& sum, const Parameters& parameters, const ChartPose& pose, const Points& points) {
    for (const auto& point : points) {
        const auto projection = ProjectChartPoint(parameters.k, parameters.d, pose, point.x, point.y, false);
        if (!projection) return false;
        const double error = std::hypot(projection->u - point.u, projection->v - point.v);
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
// Session access. The same rule as stage 2: Parse guarantees that every named
// point is charted and detected.

std::optional<Points> Gather(const CalibrationObservationSession& session, const SessionView& view,
    const std::vector<std::string>& ids) {
    std::unordered_map<std::string_view, PixelPosition> pixels; // looked up only, never iterated
    pixels.reserve(view.observations.size());
    for (const auto& observation : view.observations) {
        if (observation.pixel) pixels.emplace(observation.point_id, *observation.pixel);
    }
    Points points;
    points.reserve(ids.size());
    for (const auto& id : ids) {
        const auto* chart = session.FindChartPoint(id);
        const auto pixel = pixels.find(id);
        if (chart == nullptr || pixel == pixels.end()) return std::nullopt;
        points.push_back({chart->x_mm, chart->y_mm, pixel->second.x, pixel->second.y});
    }
    return points;
}

LensViewOutcome OutcomeOf(SolverStopReason reason) {
    return reason == SolverStopReason::MaximumIterations ? LensViewOutcome::SolverNonconvergence : LensViewOutcome::NumericalFailure;
}

// ---------------------------------------------------------------------------
// Held-out: K and the coefficients are fixed; the pose is estimated from the
// pose points, starting from the stage 2 pose of the same view, and the
// measurement points are measured.

void EvaluateValidation(const CalibrationObservationSession& session, const SessionView& view,
    const IntrinsicsViewResult& initial, const Parameters& fixed, const LensIntrinsicsPolicy& policy,
    LensViewResult& result, ResidualSum& pooled) {
    if (initial.outcome != ViewOutcome::Evaluated || !initial.pose) {
        result.outcome = LensViewOutcome::InitializationUnavailable;
        return;
    }
    auto pose_points = Gather(session, view, view.pose_point_ids);
    auto measurement_points = Gather(session, view, view.measurement_point_ids);
    if (!pose_points || !measurement_points) { result.outcome = LensViewOutcome::NumericalFailure; return; }
    Parameters start{fixed.k, fixed.d, {*initial.pose}};
    std::vector<Points> views;
    views.push_back(std::move(*pose_points));
    auto outcome = RunLevenbergMarquardt(std::move(start), views, policy, true);
    if (!outcome) { result.outcome = LensViewOutcome::InitializationUnavailable; return; }
    result.solver = outcome->record;
    if (outcome->record.stop_reason == SolverStopReason::MaximumIterations
        || outcome->record.stop_reason == SolverStopReason::NumericalFailure) {
        result.outcome = OutcomeOf(outcome->record.stop_reason);
        return;
    }
    const auto& pose = outcome->parameters.poses[0];
    ResidualSum sum;
    for (const auto& point : *measurement_points) {
        if (!ProjectChartPoint(fixed.k, fixed.d, pose, point.x, point.y, false)) { result.outcome = LensViewOutcome::ChartNotInFront; return; }
    }
    if (!Accumulate(sum, fixed, pose, *measurement_points)) { result.outcome = LensViewOutcome::NumericalFailure; return; }
    result.pose = pose;
    result.residual = Summarize(sum);
    result.outcome = LensViewOutcome::Evaluated;
    pooled.Merge(sum);
}

CameraLensResult EvaluateCamera(const CalibrationObservationSession& session, CameraAlias camera,
    const LensIntrinsicsPolicy& policy, CameraIntrinsicsResult initialization) {
    CameraLensResult out{camera, LensStatus::InitializationUnavailable, std::move(initialization), std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, {}, std::nullopt, HeldOutCoverage::NotReached, std::nullopt};
    // Training and validation views of this camera in session order, the same
    // list that stage 2 reports. Reference views are not read at all.
    std::vector<const SessionView*> members;
    std::vector<std::size_t> training, validation;
    for (const auto& view : session.Views()) {
        if (view.camera != camera || view.role == ViewRole::Reference) continue;
        (view.role == ViewRole::Training ? training : validation).push_back(members.size());
        members.push_back(&view);
        out.views.push_back({view.id, view.role, LensViewOutcome::NotReached, std::nullopt, std::nullopt, std::nullopt});
    }
    const auto& init = out.initialization;
    if (init.status != IntrinsicsStatus::UndistortedIntrinsicsEstimated || !init.intrinsics || init.views.size() != members.size()) return out;

    // Stage 2 start: its intrinsics, no distortion, and the pose of every training view.
    std::vector<Points> views;
    Parameters start{*init.intrinsics, BrownConradyDistortion{0, 0, 0, 0, 0}, {}};
    for (const auto slot : training) {
        const auto& initial = init.views[slot];
        auto points = Gather(session, *members[slot], members[slot]->used_point_ids);
        if (initial.outcome != ViewOutcome::Evaluated || !initial.pose || !points) return out;
        start.poses.push_back(*initial.pose);
        views.push_back(std::move(*points));
    }

    // Stage 1: Levenberg-Marquardt.
    auto outcome = RunLevenbergMarquardt(std::move(start), views, policy, false);
    if (!outcome) return out;
    out.solver = outcome->record;
    if (outcome->record.stop_reason == SolverStopReason::MaximumIterations) { out.status = LensStatus::SolverNonconvergence; return out; }
    if (outcome->record.stop_reason == SolverStopReason::NumericalFailure) { out.status = LensStatus::NumericalFailure; return out; }
    const auto& solved = outcome->parameters;

    // Physicality is judged once, on the converged parameters (never per
    // iteration: rejecting candidates would turn a boundary into a result).
    // The used points of the training views are evaluated at the final
    // parameters; an unprojectable point cannot occur because the solver only
    // accepts candidates whose points are all in front of the camera.
    bool poses_available = true;
    ResidualSum pooled_training;
    std::vector<std::optional<ResidualSummary>> view_residuals(training.size());
    std::vector<bool> view_available(training.size(), true);
    double max_radius_squared = 0;
    double minimum_determinant = std::numeric_limits<double>::infinity();
    for (std::size_t t = 0; t < training.size(); ++t) {
        ResidualSum sum;
        for (const auto& point : views[t]) {
            const auto projection = ProjectChartPoint(solved.k, solved.d, solved.poses[t], point.x, point.y, false);
            if (!projection) { view_available[t] = false; poses_available = false; break; }
            max_radius_squared = std::max(max_radius_squared, projection->radius_squared);
            minimum_determinant = std::min(minimum_determinant, projection->determinant);
        }
        if (view_available[t] && !Accumulate(sum, solved, solved.poses[t], views[t])) { view_available[t] = false; poses_available = false; }
        if (view_available[t]) {
            view_residuals[t] = Summarize(sum);
            pooled_training.Merge(sum);
        }
    }
    const double minimum_slope = lens_detail::MinimumRadialSlope(solved.d.k1, solved.d.k2, solved.d.k3, max_radius_squared);
    if (!std::isfinite(max_radius_squared) || !std::isfinite(minimum_slope) || !std::isfinite(minimum_determinant)) {
        out.status = LensStatus::NumericalFailure;
        return out;
    }
    if (!lens_detail::PhysicalIntrinsics(solved.k)) { out.status = LensStatus::NonphysicalIntrinsics; return out; }
    out.intrinsics = solved.k;
    out.distortion_check = DistortionCheck{max_radius_squared, minimum_slope, minimum_determinant};
    if (!lens_detail::PhysicalDistortion(minimum_slope, minimum_determinant)) { out.status = LensStatus::InvalidDistortion; return out; }
    out.distortion = solved.d;

    for (std::size_t t = 0; t < training.size(); ++t) {
        auto& result = out.views[training[t]];
        if (view_available[t]) {
            result.pose = solved.poses[t];
            result.residual = view_residuals[t];
            result.outcome = LensViewOutcome::Evaluated;
        } else {
            result.outcome = LensViewOutcome::ChartNotInFront;
        }
    }
    if (!poses_available) { out.status = LensStatus::PoseUnavailable; return out; }
    out.training_residual = Summarize(pooled_training);
    out.status = LensStatus::LensIntrinsicsEstimated;

    // Held-out residual of the validation views.
    if (validation.empty()) {
        out.held_out_coverage = HeldOutCoverage::NoValidationView;
        return out;
    }
    bool all_evaluated = true;
    ResidualSum pooled_validation;
    for (const auto slot : validation) {
        EvaluateValidation(session, *members[slot], init.views[slot], solved, policy, out.views[slot], pooled_validation);
        all_evaluated = all_evaluated && out.views[slot].outcome == LensViewOutcome::Evaluated;
    }
    out.held_out_coverage = all_evaluated ? HeldOutCoverage::AllViewsEvaluated : HeldOutCoverage::SomeViewsNotEvaluated;
    out.held_out_residual = Summarize(pooled_validation);
    return out;
}
} // namespace

LensIntrinsicsResult EstimateLensIntrinsics(const CalibrationObservationSession& session, const LensIntrinsicsPolicy& policy) {
    ValidatePolicy(policy);
    // Stage 2 reads its own policy type; its model is the undistorted one by construction.
    const UndistortedIntrinsicsPolicy stage2{policy.minimum_views, policy.relative_singular_cutoff, ObservationModel::UndistortedAnalytic};
    auto initialization = EstimateUndistortedIntrinsics(session, stage2);
    return LensIntrinsicsResult{policy.observation_model, DistortionModel::BrownConradyK1K2K3P1P2, {
        EvaluateCamera(session, CameraAlias::CamA, policy, std::move(initialization.cameras[0])),
        EvaluateCamera(session, CameraAlias::CamB, policy, std::move(initialization.cameras[1]))}};
}

} // namespace a0::m2::calibration
