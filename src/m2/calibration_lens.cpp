#include "a0/m2/calibration_lens.hpp"
#include "a0/m2/reference_document_mapping.hpp"

#include "calibration_lens_model.hpp"
#include "calibration_levenberg_marquardt.hpp"
#include "calibration_planar_pose.hpp"
#include "calibration_exact_predicates.hpp"

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

struct LensProblem {
    const std::vector<Points>& views;
    bool fixed;
    std::size_t Size() const { return (fixed ? 0 : lens_detail::kIntrinsicColumns) + 6 * views.size(); }
    std::optional<double> Evaluate(const Parameters& parameters) const { return Cost(parameters, views); }
    bool Linearize(const Parameters& parameters, std::vector<double>& a, std::vector<double>& g) const {
        return Assemble(parameters, views, fixed, Size(), a, g);
    }
    Parameters Step(const Parameters& base, const std::vector<double>& delta) const { return Apply(base, delta, fixed); }
    double Norm(const Parameters& parameters) const { return ParameterNorm(parameters, fixed); }
};

// Null when the start is unusable (a point behind the camera or not finite).
std::optional<LmOutcome> RunLevenbergMarquardt(Parameters start, const std::vector<Points>& views,
    const LensIntrinsicsPolicy& policy, bool fixed) {
    auto outcome = lm_detail::Run(LensProblem{views, fixed}, std::move(start), policy);
    if (!outcome) return std::nullopt;
    return LmOutcome{std::move(outcome->parameters), outcome->record};
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

namespace {

std::optional<PixelPosition> UndistortReferencePixel(const PinholeIntrinsics& k,
    const BrownConradyDistortion& d, double u, double v, const ReferenceMappingPolicy& policy) {
    return lens_detail::UndistortPixel(k, d, u, v, policy.max_undistortion_iterations, policy.undistortion_tolerance_pixels);
}

std::optional<Matrix3> ComposeReferenceMapping(const PinholeIntrinsics& k, const ChartPose& pose,
    const DocumentPlanePlacement& placement) {
    const Matrix3 kk{k.fx, 0, k.cx, 0, k.fy, k.cy, 0, 0, 1};
    const auto& r = pose.rotation;
    const auto& t = pose.translation_mm;
    const Matrix3 plane{r[0], r[1], t[0], r[3], r[4], t[1], r[6], r[7], t[2]};
    const double c = std::cos(placement.angle_radians), s = std::sin(placement.angle_radians);
    const Matrix3 transform{c, -s, placement.translation_x_mm, s, c, placement.translation_y_mm, 0, 0, 1};
    auto h = Multiply(Multiply(kk, plane), transform);
    for (const double value : h) if (!std::isfinite(value)) return std::nullopt;
    if (h[8] == 0.0 || !exact_detail::ExactNonzeroDeterminant(h)) return std::nullopt;
    const double scale = h[8];
    for (auto& value : h) {
        value /= scale;
        if (!std::isfinite(value)) return std::nullopt;
    }
    h[8] = 1.0;
    if (!exact_detail::ExactNonzeroDeterminant(h)) return std::nullopt;
    return h;
}

std::optional<FixedDocumentCameraMapping> MapReference(const CalibrationObservationSession& session,
    const SessionView& view, const CameraLensResult& lens, const DocumentPlanePlacement& placement,
    const ReferenceMappingPolicy& policy, ReferenceViewMappingResult& result) {
    if (lens.status != LensStatus::LensIntrinsicsEstimated || !lens.intrinsics || !lens.distortion) {
        result.outcome = ReferenceMappingOutcome::LensUnavailable;
        return std::nullopt;
    }
    const auto raw = Gather(session, view, view.used_point_ids);
    if (!raw || raw->size() < kMinimumMappingPoints) {
        result.outcome = ReferenceMappingOutcome::PoseUnavailable;
        return std::nullopt;
    }
    const auto& k = *lens.intrinsics;
    const auto& d = *lens.distortion;
    std::vector<pose_detail::PlanarObservation> undistorted;
    undistorted.reserve(raw->size());
    for (const auto& point : *raw) {
        const auto pixel = UndistortReferencePixel(k, d, point.u, point.v, policy);
        if (!pixel) {
            result.outcome = ReferenceMappingOutcome::UndistortionUnavailable;
            return std::nullopt;
        }
        undistorted.push_back({point.x, point.y, pixel->x, pixel->y});
    }
    const auto seed = pose_detail::EstimatePlanarPose(undistorted, k);
    if (!seed) {
        result.outcome = ReferenceMappingOutcome::PoseUnavailable;
        return std::nullopt;
    }
    const auto refined = RunLevenbergMarquardt(Parameters{k, d, {*seed}}, {*raw}, policy.lens, true);
    if (!refined) {
        result.outcome = ReferenceMappingOutcome::PoseUnavailable;
        return std::nullopt;
    }
    result.solver = refined->record;
    if (refined->record.stop_reason == SolverStopReason::MaximumIterations
        || refined->record.stop_reason == SolverStopReason::NumericalFailure) {
        result.outcome = refined->record.stop_reason == SolverStopReason::MaximumIterations
            ? ReferenceMappingOutcome::SolverNonconvergence : ReferenceMappingOutcome::NumericalFailure;
        return std::nullopt;
    }
    const auto& pose = refined->parameters.poses[0];
    ResidualSum residual;
    const Parameters fixed{k, d, {pose}};
    if (!Accumulate(residual, fixed, pose, *raw)) {
        result.outcome = ReferenceMappingOutcome::NumericalFailure;
        return std::nullopt;
    }
    const auto mapping = ComposeReferenceMapping(k, pose, placement);
    if (!mapping) {
        result.outcome = ReferenceMappingOutcome::NumericalFailure;
        return std::nullopt;
    }
    result.pose = pose;
    result.residual = Summarize(residual);
    result.outcome = ReferenceMappingOutcome::MappingEstimated;
    return FixedDocumentCameraMapping{view.camera, view.id, view.image_sha256, k, d, *mapping};
}

} // namespace

ReferenceDocumentMappingResult EstimateReferenceDocumentMapping(const CalibrationObservationSession& session,
    const ReferencePairRequest& request, const ReferenceMappingPolicy& policy) {
    ValidatePolicy(policy.lens);
    if (policy.max_undistortion_iterations < 1 || policy.max_undistortion_iterations > 1000
        || !std::isfinite(policy.undistortion_tolerance_pixels) || !(policy.undistortion_tolerance_pixels > 0.0)) {
        throw std::invalid_argument("reference document mapping: invalid policy");
    }
    ReferenceDocumentMappingResult result{{{
        {CameraAlias::CamA, request.view_ids[0], ReferenceMappingOutcome::NotReached, {}, {}, {}},
        {CameraAlias::CamB, request.view_ids[1], ReferenceMappingOutcome::NotReached, {}, {}, {}}}}, {}, {}};
    std::array<const SessionView*, 2> references{};
    bool valid = true;
    for (std::size_t i = 0; i < references.size(); ++i) {
        for (const auto& view : session.Views()) if (view.id == request.view_ids[i]) references[i] = &view;
        auto& status = result.cameras[i].outcome;
        if (!references[i]) status = ReferenceMappingOutcome::ReferenceMissing;
        else if (references[i]->role != ViewRole::Reference) status = ReferenceMappingOutcome::ReferenceRoleMismatch;
        else if (references[i]->camera != result.cameras[i].camera) status = ReferenceMappingOutcome::ReferenceCameraMismatch;
        valid = valid && status == ReferenceMappingOutcome::NotReached;
    }
    if (!valid) return result;
    if (!request.placement) {
        for (auto& camera : result.cameras) camera.outcome = ReferenceMappingOutcome::PlacementMissing;
        return result;
    }
    const auto& placement = *request.placement;
    if (!placement.coplanar_confirmed || !std::isfinite(placement.angle_radians)
        || !std::isfinite(placement.translation_x_mm) || !std::isfinite(placement.translation_y_mm)) {
        for (auto& camera : result.cameras) camera.outcome = ReferenceMappingOutcome::PlacementInvalid;
        return result;
    }
    result.lens = EstimateLensIntrinsics(session, policy.lens);
    const auto a = MapReference(session, *references[0], result.lens->cameras[0], placement, policy, result.cameras[0]);
    const auto b = MapReference(session, *references[1], result.lens->cameras[1], placement, policy, result.cameras[1]);
    if (a && b) result.mapping = std::array<FixedDocumentCameraMapping, 2>{*a, *b};
    return result;
}

} // namespace a0::m2::calibration
