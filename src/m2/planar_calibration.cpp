#include "a0/m2/planar_calibration.hpp"

#include "calibration_exact_predicates.hpp"
#include "calibration_lens_model.hpp"
#include "calibration_levenberg_marquardt.hpp"
#include "calibration_planar_pose.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
using Placement = std::array<double, 3>; // theta, tx, ty

// Columns of one Jacobian row: H (8, h22 fixed to 1), the five coefficients, then theta, tx, ty.
constexpr std::size_t kDistortionColumn = 8, kPlacementColumn = 13, kCameraColumns = 13, kColumns = 16;
constexpr double kMaximumNominalMm = 10000;

bool InOpenUnitInterval(double value) { return std::isfinite(value) && value > 0 && value < 1; }
void ValidatePolicy(const PlanarCalibrationPolicy& p) {
    if (p.minimum_training_views < 1 || p.minimum_training_views > 128 || p.max_iterations < 1 || p.max_iterations > 1000
        || !InOpenUnitInterval(p.relative_cost_tolerance) || !InOpenUnitInterval(p.relative_step_tolerance)
        || !std::isfinite(p.initial_damping) || !(p.initial_damping > 0)
        || p.max_undistortion_iterations < 1 || p.max_undistortion_iterations > 1000
        || !std::isfinite(p.undistortion_tolerance_pixels) || !(p.undistortion_tolerance_pixels > 0))
        throw std::invalid_argument("planar calibration: invalid policy");
}

struct Correspondence { double x, y, u, v; };
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
std::optional<Matrix3> Inverse(const Matrix3& m) {
    const Matrix3 adjugate{
        m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
        m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
        m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
    const double determinant = m[0] * adjugate[0] + m[1] * adjugate[3] + m[2] * adjugate[6];
    if (!std::isfinite(determinant) || determinant == 0) return std::nullopt;
    Matrix3 inverse{};
    for (std::size_t i = 0; i < 9; ++i) {
        inverse[i] = adjugate[i] / determinant;
        if (!std::isfinite(inverse[i])) return std::nullopt;
    }
    return inverse;
}
std::optional<Matrix3> Normalized(Matrix3 m) {
    if (!std::isfinite(m[8]) || m[8] == 0) return std::nullopt;
    const double scale = m[8];
    for (auto& value : m) {
        value /= scale;
        if (!std::isfinite(value)) return std::nullopt;
    }
    m[8] = 1.0;
    return m;
}
// Nearest planar rigid motion of a (near) similarity, normalized to m22 = 1.
std::optional<Placement> PlacementOf(const Matrix3& chart_to_document) {
    const auto m = Normalized(chart_to_document);
    if (!m) return std::nullopt;
    const Placement out{std::atan2((*m)[3] - (*m)[1], (*m)[0] + (*m)[4]), (*m)[2], (*m)[5]};
    for (const auto value : out) if (!std::isfinite(value)) return std::nullopt;
    return out;
}

// ---------------------------------------------------------------------------
// Model: chart (x, y) -> T -> document (X, Y) -> H -> undistorted (U, V) ->
// K_nom^-1 -> Brown-Conrady -> K_nom -> raw (u, v).

struct Projection {
    double u = 0, v = 0;
    double radius_squared = 0; // of the undistorted normalized point
    double determinant = 0;    // of the distortion map at that point
    std::array<std::array<double, kColumns>, 2> jacobian{};
};

// Null when w <= 0 or any value is not finite. Without a placement, T = I.
std::optional<Projection> Project(const PinholeIntrinsics& k, const Matrix3& h, const BrownConradyDistortion& d,
    const Placement* placement, double x, double y, bool with_jacobian) {
    double c = 1, s = 0, X = x, Y = y;
    if (placement) {
        c = std::cos((*placement)[0]);
        s = std::sin((*placement)[0]);
        X = c * x - s * y + (*placement)[1];
        Y = s * x + c * y + (*placement)[2];
    }
    const double w = h[6] * X + h[7] * Y + 1.0;
    if (!std::isfinite(w) || !(w > 0)) return std::nullopt;
    const double U = (h[0] * X + h[1] * Y + h[2]) / w, V = (h[3] * X + h[4] * Y + h[5]) / w;
    const double xn = (U - k.cx) / k.fx, yn = (V - k.cy) / k.fy;
    const auto derivative = lens_detail::DistortionDerivativesAt(d, xn, yn);
    const auto distorted = lens_detail::DistortNormalized(d, xn, yn);
    Projection out;
    out.u = k.fx * distorted.x + k.cx;
    out.v = k.fy * distorted.y + k.cy;
    out.radius_squared = xn * xn + yn * yn;
    out.determinant = derivative.a * derivative.c - derivative.b * derivative.b;
    if (!std::isfinite(out.u) || !std::isfinite(out.v) || !std::isfinite(out.radius_squared)
        || !std::isfinite(out.determinant)) return std::nullopt;
    if (!with_jacobian) return out;

    // d(u, v) / d(U, V)
    const double g00 = derivative.a, g01 = k.fx * derivative.b / k.fy;
    const double g10 = k.fy * derivative.b / k.fx, g11 = derivative.c;
    const double inverse_w = 1.0 / w;
    const std::array<double, 8> du{X * inverse_w, Y * inverse_w, inverse_w, 0, 0, 0, -U * X * inverse_w, -U * Y * inverse_w};
    const std::array<double, 8> dv{0, 0, 0, X * inverse_w, Y * inverse_w, inverse_w, -V * X * inverse_w, -V * Y * inverse_w};
    auto& ju = out.jacobian[0];
    auto& jv = out.jacobian[1];
    for (std::size_t j = 0; j < 8; ++j) {
        ju[j] = g00 * du[j] + g01 * dv[j];
        jv[j] = g10 * du[j] + g11 * dv[j];
    }
    const double r2 = out.radius_squared, r4 = r2 * r2, r6 = r4 * r2;
    const std::size_t o = kDistortionColumn;
    ju[o] = k.fx * xn * r2; ju[o + 1] = k.fx * xn * r4; ju[o + 2] = k.fx * xn * r6;
    jv[o] = k.fy * yn * r2; jv[o + 1] = k.fy * yn * r4; jv[o + 2] = k.fy * yn * r6;
    ju[o + 3] = k.fx * 2.0 * xn * yn; ju[o + 4] = k.fx * (r2 + 2.0 * xn * xn);
    jv[o + 3] = k.fy * (r2 + 2.0 * yn * yn); jv[o + 4] = k.fy * 2.0 * xn * yn;
    if (placement) {
        // d(U, V) / d(X, Y), then d(X, Y) / d(theta) = (-(Y - ty), X - tx).
        const double ux = (h[0] - U * h[6]) * inverse_w, uy = (h[1] - U * h[7]) * inverse_w;
        const double vx = (h[3] - V * h[6]) * inverse_w, vy = (h[4] - V * h[7]) * inverse_w;
        const double xt = -(s * x + c * y), yt = c * x - s * y;
        const std::array<double, 3> dU{ux * xt + uy * yt, ux, uy}, dV{vx * xt + vy * yt, vx, vy};
        for (std::size_t j = 0; j < 3; ++j) {
            ju[kPlacementColumn + j] = g00 * dU[j] + g01 * dV[j];
            jv[kPlacementColumn + j] = g10 * dU[j] + g11 * dV[j];
        }
    }
    for (const auto& row : out.jacobian) for (const auto value : row) if (!std::isfinite(value)) return std::nullopt;
    return out;
}

// ---------------------------------------------------------------------------
// Levenberg-Marquardt problem. theta = [h0..h7 | k1 k2 k3 p1 p2 | (theta, tx, ty) per placement].
// The first thirteen are left out when the camera is fixed (held-out placement).

struct State {
    Matrix3 h; // h[8] == 1, not a parameter
    BrownConradyDistortion d;
    std::vector<Placement> placements;
};
struct View {
    Points points;
    std::optional<std::size_t> placement; // null: the overlap view, T = I
};

struct PlanarProblem {
    const PinholeIntrinsics& k;
    const std::vector<View>& views;
    bool fixed;
    std::size_t placement_count;

    std::size_t Size() const { return (fixed ? 0 : kCameraColumns) + 3 * placement_count; }
    std::optional<double> Evaluate(const State& state) const {
        double cost = 0;
        for (const auto& view : views) {
            const Placement* placement = view.placement ? &state.placements[*view.placement] : nullptr;
            for (const auto& point : view.points) {
                const auto projection = Project(k, state.h, state.d, placement, point.x, point.y, false);
                if (!projection) return std::nullopt;
                const double du = projection->u - point.u, dv = projection->v - point.v;
                cost += du * du + dv * dv;
            }
        }
        if (!std::isfinite(cost)) return std::nullopt;
        return cost;
    }
    bool Linearize(const State& state, std::vector<double>& a, std::vector<double>& g) const {
        const std::size_t n = Size();
        a.assign(n * n, 0.0);
        g.assign(n, 0.0);
        const std::size_t base = fixed ? 0 : kCameraColumns;
        std::array<std::size_t, kColumns> local{}, global{};
        for (const auto& view : views) {
            std::size_t used = 0;
            if (!fixed) for (std::size_t c = 0; c < kCameraColumns; ++c) { local[used] = c; global[used] = c; ++used; }
            if (view.placement) for (std::size_t c = 0; c < 3; ++c) {
                local[used] = kPlacementColumn + c;
                global[used] = base + 3 * *view.placement + c;
                ++used;
            }
            const Placement* placement = view.placement ? &state.placements[*view.placement] : nullptr;
            for (const auto& point : view.points) {
                const auto projection = Project(k, state.h, state.d, placement, point.x, point.y, true);
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
    State Step(const State& current, const std::vector<double>& delta) const {
        State next = current;
        std::size_t offset = 0;
        if (!fixed) {
            for (std::size_t j = 0; j < 8; ++j) next.h[j] += delta[j];
            next.d.k1 += delta[8];
            next.d.k2 += delta[9];
            next.d.k3 += delta[10];
            next.d.p1 += delta[11];
            next.d.p2 += delta[12];
            offset = kCameraColumns;
        }
        for (std::size_t p = 0; p < next.placements.size(); ++p) for (std::size_t j = 0; j < 3; ++j) next.placements[p][j] += delta[offset + 3 * p + j];
        return next;
    }
    // Norm of the parameters that carry a unit or scale: H, the coefficients
    // and the translations. The angles enter as zero, like the lens rotations.
    double Norm(const State& state) const {
        double sum = 0;
        if (!fixed) {
            for (std::size_t j = 0; j < 8; ++j) sum += state.h[j] * state.h[j];
            for (const auto value : {state.d.k1, state.d.k2, state.d.k3, state.d.p1, state.d.p2}) sum += value * value;
        }
        for (const auto& placement : state.placements) sum += placement[1] * placement[1] + placement[2] * placement[2];
        return std::sqrt(sum);
    }
};

// ---------------------------------------------------------------------------
// Residuals and straightness.

struct ResidualSum {
    double squares = 0, maximum = 0;
    std::uint32_t count = 0;
    void Add(double error) { squares += error * error; maximum = std::max(maximum, error); ++count; }
    void Merge(const ResidualSum& other) { squares += other.squares; maximum = std::max(maximum, other.maximum); count += other.count; }
};
std::optional<ResidualSummary> Summarize(const ResidualSum& sum) {
    if (sum.count == 0) return std::nullopt;
    const double rms = std::sqrt(sum.squares / static_cast<double>(sum.count));
    if (!std::isfinite(rms) || !std::isfinite(sum.maximum)) return std::nullopt;
    return ResidualSummary{rms, sum.maximum, sum.count};
}
// False if a point cannot be projected or an error is not finite.
bool Accumulate(ResidualSum& sum, const PinholeIntrinsics& k, const State& state, const Placement* placement, const Points& points) {
    for (const auto& point : points) {
        const auto projection = Project(k, state.h, state.d, placement, point.x, point.y, false);
        if (!projection) return false;
        const double error = std::hypot(projection->u - point.u, projection->v - point.v);
        if (!std::isfinite(error)) return false;
        sum.Add(error);
    }
    return true;
}

struct LineSum {
    ResidualSum distances;
    std::uint32_t lines = 0;
    bool failed = false;
    void Merge(const LineSum& other) { distances.Merge(other.distances); lines += other.lines; failed = failed || other.failed; }
    std::optional<StraightnessSummary> Summary() const {
        const auto summary = Summarize(distances);
        if (failed || !summary || lines == 0) return std::nullopt;
        return StraightnessSummary{summary->rms_pixels, summary->max_pixels, lines, summary->point_count};
    }
};
// Points that share x_mm (a chart column) or y_mm (a chart row), three or more
// per line, each measured from the total least squares line of its line.
LineSum Lines(const Points& points) {
    LineSum out;
    std::map<double, std::vector<std::size_t>> columns, rows; // ordered: deterministic
    for (std::size_t i = 0; i < points.size(); ++i) {
        columns[points[i].x].push_back(i);
        rows[points[i].y].push_back(i);
    }
    const auto fit = [&](const std::vector<std::size_t>& members) {
        if (members.size() < 3) return;
        double mean_u = 0, mean_v = 0;
        for (const auto i : members) { mean_u += points[i].u; mean_v += points[i].v; }
        mean_u /= static_cast<double>(members.size());
        mean_v /= static_cast<double>(members.size());
        double suu = 0, suv = 0, svv = 0;
        for (const auto i : members) {
            const double du = points[i].u - mean_u, dv = points[i].v - mean_v;
            suu += du * du; suv += du * dv; svv += dv * dv;
        }
        const double angle = 0.5 * std::atan2(2.0 * suv, suu - svv);
        const double nu = -std::sin(angle), nv = std::cos(angle);
        for (const auto i : members) {
            const double distance = std::abs(nu * (points[i].u - mean_u) + nv * (points[i].v - mean_v));
            if (!std::isfinite(distance)) { out.failed = true; return; }
            out.distances.Add(distance);
        }
        ++out.lines;
    };
    for (const auto& [x, members] : columns) fit(members);
    for (const auto& [y, members] : rows) fit(members);
    return out;
}
// Undistorted copy of the points; null when one cannot be undistorted.
std::optional<Points> Undistort(const Points& points, const PinholeIntrinsics& k, const BrownConradyDistortion& d,
    const PlanarCalibrationPolicy& policy) {
    Points out;
    out.reserve(points.size());
    for (const auto& point : points) {
        const auto pixel = lens_detail::UndistortPixel(k, d, point.u, point.v, policy.max_undistortion_iterations,
            policy.undistortion_tolerance_pixels);
        if (!pixel) return std::nullopt;
        out.push_back({point.x, point.y, pixel->x, pixel->y});
    }
    return out;
}
struct StraightnessSums {
    LineSum raw, undistorted;
    void Merge(const StraightnessSums& other) { raw.Merge(other.raw); undistorted.Merge(other.undistorted); }
    Straightness Summary() const { return {raw.Summary(), undistorted.Summary()}; }
};
StraightnessSums MeasureStraightness(const Points& points, const PinholeIntrinsics& k, const BrownConradyDistortion& d,
    const PlanarCalibrationPolicy& policy) {
    StraightnessSums out{Lines(points), {}};
    const auto undistorted = Undistort(points, k, d, policy);
    if (undistorted) out.undistorted = Lines(*undistorted);
    else out.undistorted.failed = true;
    return out;
}

// ---------------------------------------------------------------------------
// Session access. Parse guarantees that every named point is charted and detected.

std::optional<Points> Gather(const CalibrationObservationSession& session, const SessionView& view,
    const std::vector<std::string>& ids) {
    std::unordered_map<std::string_view, PixelPosition> pixels; // looked up only, never iterated
    pixels.reserve(view.observations.size());
    for (const auto& observation : view.observations) if (observation.pixel) pixels.emplace(observation.point_id, *observation.pixel);
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
// DLT of chart -> (u, v), normalized to h22 = 1.
std::optional<Matrix3> Homography(const Points& points) {
    std::vector<pose_detail::PlanarObservation> observations;
    observations.reserve(points.size());
    for (const auto& point : points) observations.push_back({point.x, point.y, point.u, point.v});
    const auto h = pose_detail::FitPlanarHomography(observations);
    return h ? Normalized(*h) : std::nullopt;
}
// Placement start of a view from its DLT and the camera's H.
std::optional<Placement> PlacementStart(const Points& points, const Matrix3& h_inverse) {
    const auto g = Homography(points);
    return g ? PlacementOf(Multiply(h_inverse, *g)) : std::nullopt;
}

PlanarViewOutcome OutcomeOf(SolverStopReason reason) {
    return reason == SolverStopReason::MaximumIterations ? PlanarViewOutcome::SolverNonconvergence : PlanarViewOutcome::NumericalFailure;
}
PlanarPlacement Public(const Placement& placement) { return {placement[0], placement[1], placement[2]}; }

// ---------------------------------------------------------------------------
// Held-out: H and the coefficients are fixed, the placement is fitted to the
// pose points from a DLT start (on undistorted pixels), and the measurement
// points are measured.

void EvaluateValidation(const CalibrationObservationSession& session, const SessionView& view, const PinholeIntrinsics& k,
    const State& solved, const Matrix3& h_inverse, const PlanarCalibrationPolicy& policy, PlanarViewResult& result,
    ResidualSum& pooled, StraightnessSums& straightness) {
    auto pose_points = Gather(session, view, view.pose_point_ids);
    auto measurement_points = Gather(session, view, view.measurement_point_ids);
    if (!pose_points || !measurement_points) { result.outcome = PlanarViewOutcome::NumericalFailure; return; }
    Points all = *pose_points;
    all.insert(all.end(), measurement_points->begin(), measurement_points->end());
    const auto lines = MeasureStraightness(all, k, solved.d, policy);
    result.straightness = lines.Summary();
    straightness.Merge(lines);
    const auto undistorted = Undistort(*pose_points, k, solved.d, policy);
    const auto start = undistorted ? PlacementStart(*undistorted, h_inverse) : std::nullopt;
    if (!start) { result.outcome = PlanarViewOutcome::InitializationUnavailable; return; }
    const std::vector<View> views{{std::move(*pose_points), std::size_t{0}}};
    auto outcome = lm_detail::Run(PlanarProblem{k, views, true, 1}, State{solved.h, solved.d, {*start}}, policy);
    if (!outcome) { result.outcome = PlanarViewOutcome::InitializationUnavailable; return; }
    result.solver = outcome->record;
    if (outcome->record.stop_reason == SolverStopReason::MaximumIterations
        || outcome->record.stop_reason == SolverStopReason::NumericalFailure) {
        result.outcome = OutcomeOf(outcome->record.stop_reason);
        return;
    }
    const auto& placement = outcome->parameters.placements[0];
    ResidualSum sum;
    if (!Accumulate(sum, k, solved, &placement, *measurement_points)) { result.outcome = PlanarViewOutcome::ChartNotInFront; return; }
    result.placement = Public(placement);
    result.residual = Summarize(sum);
    result.outcome = PlanarViewOutcome::Evaluated;
    pooled.Merge(sum);
}

// overlap_point_ids replaces the overlap view's usedPointIds (checkerboard
// measure only); null keeps the session's.
CameraPlanarResult EvaluateCamera(const CalibrationObservationSession& session, std::size_t index,
    const PlanarCalibrationRequest& request, const PlanarCalibrationPolicy& policy,
    const std::vector<std::string>* overlap_point_ids = nullptr) {
    const auto camera = index == 0 ? CameraAlias::CamA : CameraAlias::CamB;
    CameraPlanarResult out{camera, PlanarCameraStatus::NominalInvalid, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, {}, std::nullopt, std::nullopt, HeldOutCoverage::NotReached, std::nullopt, {}, {}};
    const auto& overlap_id = request.overlap_view_ids[index];
    std::vector<const SessionView*> members;
    std::optional<std::size_t> overlap;
    std::vector<std::size_t> training, validation;
    for (const auto& view : session.Views()) {
        if (view.camera != camera) continue;
        const bool is_overlap = view.id == overlap_id;
        if (is_overlap) overlap = members.size();
        if (view.role == ViewRole::Training) training.push_back(members.size());
        else if (view.role == ViewRole::Validation) validation.push_back(members.size());
        out.views.push_back({view.id, view.role, is_overlap,
            view.role == ViewRole::Reference && !is_overlap ? PlanarViewOutcome::NotUsed : PlanarViewOutcome::NotReached,
            std::nullopt, std::nullopt, std::nullopt, {}});
        members.push_back(&view);
    }
    const auto& group = session.Camera(camera);
    out.nominal_intrinsics = NominalIntrinsics(request.nominal[index], group.width_pixels, group.height_pixels);
    if (!out.nominal_intrinsics) return out;
    const auto& k = *out.nominal_intrinsics;
    if (!overlap) {
        bool elsewhere = false;
        for (const auto& view : session.Views()) elsewhere = elsewhere || view.id == overlap_id;
        out.status = elsewhere ? PlanarCameraStatus::OverlapCameraMismatch : PlanarCameraStatus::OverlapViewMissing;
        return out;
    }
    if (members[*overlap]->role != ViewRole::Reference) { out.status = PlanarCameraStatus::OverlapRoleMismatch; return out; }
    if (training.size() < policy.minimum_training_views) { out.status = PlanarCameraStatus::InsufficientViews; return out; }

    // Start: the overlap DLT is H_c (its placement is the identity), each
    // training placement is H_c^-1 composed with its own DLT, no distortion.
    out.status = PlanarCameraStatus::InitializationUnavailable;
    std::vector<std::size_t> fitted{*overlap}; // member slots in fit order: overlap first, then training
    fitted.insert(fitted.end(), training.begin(), training.end());
    std::vector<View> views;
    for (std::size_t f = 0; f < fitted.size(); ++f) {
        const auto& ids = f == 0 && overlap_point_ids ? *overlap_point_ids : members[fitted[f]]->used_point_ids;
        auto points = Gather(session, *members[fitted[f]], ids);
        if (!points) { out.views[fitted[f]].outcome = PlanarViewOutcome::NumericalFailure; return out; }
        views.push_back({std::move(*points), f == 0 ? std::nullopt : std::optional<std::size_t>{f - 1}});
    }
    const auto h_start = Homography(views[0].points);
    const auto h_start_inverse = h_start ? Inverse(*h_start) : std::nullopt;
    if (!h_start_inverse) { out.views[*overlap].outcome = PlanarViewOutcome::InitializationUnavailable; return out; }
    State start{*h_start, BrownConradyDistortion{0, 0, 0, 0, 0}, {}};
    for (std::size_t f = 1; f < fitted.size(); ++f) {
        const auto placement = PlacementStart(views[f].points, *h_start_inverse);
        if (!placement) { out.views[fitted[f]].outcome = PlanarViewOutcome::InitializationUnavailable; return out; }
        start.placements.push_back(*placement);
    }

    auto outcome = lm_detail::Run(PlanarProblem{k, views, false, training.size()}, std::move(start), policy);
    if (!outcome) return out;
    out.solver = outcome->record;
    if (outcome->record.stop_reason == SolverStopReason::MaximumIterations) { out.status = PlanarCameraStatus::SolverNonconvergence; return out; }
    if (outcome->record.stop_reason == SolverStopReason::NumericalFailure) { out.status = PlanarCameraStatus::NumericalFailure; return out; }
    const auto& solved = outcome->parameters;

    // Physicality is judged once, on the converged parameters, over the fitted points.
    double max_radius_squared = 0;
    double minimum_determinant = std::numeric_limits<double>::infinity();
    std::vector<ResidualSum> sums(views.size());
    for (std::size_t f = 0; f < views.size(); ++f) {
        const Placement* placement = views[f].placement ? &solved.placements[*views[f].placement] : nullptr;
        for (const auto& point : views[f].points) {
            const auto projection = Project(k, solved.h, solved.d, placement, point.x, point.y, false);
            if (!projection) {
                out.views[fitted[f]].outcome = PlanarViewOutcome::ChartNotInFront;
                out.status = PlanarCameraStatus::ChartNotInFront;
                return out;
            }
            max_radius_squared = std::max(max_radius_squared, projection->radius_squared);
            minimum_determinant = std::min(minimum_determinant, projection->determinant);
            sums[f].Add(std::hypot(projection->u - point.u, projection->v - point.v));
        }
    }
    const double minimum_slope = lens_detail::MinimumRadialSlope(solved.d.k1, solved.d.k2, solved.d.k3, max_radius_squared);
    if (!std::isfinite(max_radius_squared) || !std::isfinite(minimum_slope) || !std::isfinite(minimum_determinant)
        || !exact_detail::ExactNonzeroDeterminant(solved.h)) {
        out.status = PlanarCameraStatus::NumericalFailure;
        return out;
    }
    out.distortion_check = DistortionCheck{max_radius_squared, minimum_slope, minimum_determinant};
    if (!lens_detail::PhysicalDistortion(minimum_slope, minimum_determinant)) { out.status = PlanarCameraStatus::InvalidDistortion; return out; }
    out.distortion = solved.d;
    out.document_to_image = solved.h;

    ResidualSum pooled_fit;
    StraightnessSums fit_lines;
    for (std::size_t f = 0; f < views.size(); ++f) {
        auto& result = out.views[fitted[f]];
        result.placement = views[f].placement ? Public(solved.placements[*views[f].placement]) : PlanarPlacement{0, 0, 0};
        result.residual = Summarize(sums[f]);
        const auto lines = MeasureStraightness(views[f].points, k, solved.d, policy);
        result.straightness = lines.Summary();
        fit_lines.Merge(lines);
        result.outcome = PlanarViewOutcome::Evaluated;
        pooled_fit.Merge(sums[f]);
    }
    out.fit_residual = Summarize(pooled_fit);
    out.overlap_residual = Summarize(sums[0]);
    out.fit_straightness = fit_lines.Summary();
    out.status = PlanarCameraStatus::Estimated;

    if (validation.empty()) {
        out.held_out_coverage = HeldOutCoverage::NoValidationView;
        return out;
    }
    const auto h_inverse = Inverse(solved.h);
    bool all_evaluated = true;
    ResidualSum pooled_validation;
    StraightnessSums validation_lines;
    for (const auto slot : validation) {
        auto& result = out.views[slot];
        if (h_inverse) EvaluateValidation(session, *members[slot], k, solved, *h_inverse, policy, result, pooled_validation, validation_lines);
        else result.outcome = PlanarViewOutcome::NumericalFailure;
        all_evaluated = all_evaluated && result.outcome == PlanarViewOutcome::Evaluated;
    }
    out.held_out_coverage = all_evaluated ? HeldOutCoverage::AllViewsEvaluated : HeldOutCoverage::SomeViewsNotEvaluated;
    out.held_out_residual = Summarize(pooled_validation);
    out.held_out_straightness = validation_lines.Summary();
    return out;
}

// ---------------------------------------------------------------------------
// Report-only measures: seam disagreement and coverage.

// The named overlap view of a camera, only when it is that camera's reference view.
const SessionView* OverlapView(const CalibrationObservationSession& session, const PlanarCalibrationRequest& request, std::size_t index) {
    const auto camera = index == 0 ? CameraAlias::CamA : CameraAlias::CamB;
    for (const auto& view : session.Views())
        if (view.id == request.overlap_view_ids[index]) return view.camera == camera && view.role == ViewRole::Reference ? &view : nullptr;
    return nullptr;
}

// Checkerboard colour of every chart point: true when the rank of its x_mm
// among the distinct x_mm plus the rank of its y_mm among the distinct y_mm is even.
std::unordered_map<std::string_view, bool> CheckerboardEven(const CalibrationObservationSession& session) {
    std::map<double, std::size_t> columns, rows; // ordered: ranks
    for (const auto& point : session.ChartPoints()) { columns.emplace(point.x_mm, 0); rows.emplace(point.y_mm, 0); }
    std::size_t rank = 0;
    for (auto& entry : columns) entry.second = rank++;
    rank = 0;
    for (auto& entry : rows) entry.second = rank++;
    std::unordered_map<std::string_view, bool> even; // looked up only, never iterated
    even.reserve(session.ChartPoints().size());
    for (const auto& point : session.ChartPoints()) even.emplace(point.id, (columns.at(point.x_mm) + rows.at(point.y_mm)) % 2 == 0);
    return even;
}

// Raw pixel -> overlap chart mm through the undistortion and H^-1.
std::optional<std::array<double, 2>> DocumentOf(const PinholeIntrinsics& k, const BrownConradyDistortion& d, const Matrix3& h_inverse,
    const PixelPosition& pixel, const PlanarCalibrationPolicy& policy) {
    const auto undistorted = lens_detail::UndistortPixel(k, d, pixel.x, pixel.y, policy.max_undistortion_iterations,
        policy.undistortion_tolerance_pixels);
    if (!undistorted) return std::nullopt;
    const auto& m = h_inverse;
    const double w = m[6] * undistorted->x + m[7] * undistorted->y + m[8];
    if (!std::isfinite(w) || w == 0) return std::nullopt;
    const std::array<double, 2> out{(m[0] * undistorted->x + m[1] * undistorted->y + m[2]) / w,
        (m[3] * undistorted->x + m[4] * undistorted->y + m[5]) / w};
    if (!std::isfinite(out[0]) || !std::isfinite(out[1])) return std::nullopt;
    return out;
}

// Points detected in both overlap views, in CAM-A observation order. With a
// checkerboard, only the points of the colour `even`.
PlanarSeamMeasure Seam(const CalibrationObservationSession& session, const PlanarCalibrationRequest& request,
    const PlanarCalibrationPolicy& policy, const PlanarCalibrationResult& result,
    const std::unordered_map<std::string_view, bool>* checkerboard, bool even) {
    std::array<const SessionView*, 2> views{};
    std::array<Matrix3, 2> inverses{};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& camera = result.cameras[i];
        views[i] = OverlapView(session, request, i);
        if (camera.status != PlanarCameraStatus::Estimated || !camera.nominal_intrinsics || !camera.distortion
            || !camera.document_to_image || views[i] == nullptr) return {PlanarSeamOutcome::CameraNotEstimated, std::nullopt};
        const auto inverse = Inverse(*camera.document_to_image);
        if (!inverse) return {PlanarSeamOutcome::NumericalFailure, std::nullopt};
        inverses[i] = *inverse;
    }
    std::unordered_map<std::string_view, PixelPosition> b_pixels; // looked up only, never iterated
    b_pixels.reserve(views[1]->observations.size());
    for (const auto& observation : views[1]->observations) if (observation.pixel) b_pixels.emplace(observation.point_id, *observation.pixel);
    std::vector<double> distances;
    double sum = 0;
    for (const auto& observation : views[0]->observations) {
        if (!observation.pixel) continue;
        const auto b = b_pixels.find(observation.point_id);
        if (b == b_pixels.end()) continue;
        if (checkerboard) {
            const auto colour = checkerboard->find(observation.point_id);
            if (colour == checkerboard->end() || colour->second != even) continue;
        }
        const auto& a_camera = result.cameras[0];
        const auto& b_camera = result.cameras[1];
        const auto a_document = DocumentOf(*a_camera.nominal_intrinsics, *a_camera.distortion, inverses[0], *observation.pixel, policy);
        const auto b_document = DocumentOf(*b_camera.nominal_intrinsics, *b_camera.distortion, inverses[1], b->second, policy);
        if (!a_document || !b_document) return {PlanarSeamOutcome::NumericalFailure, std::nullopt};
        const double distance = std::hypot((*a_document)[0] - (*b_document)[0], (*a_document)[1] - (*b_document)[1]);
        if (!std::isfinite(distance)) return {PlanarSeamOutcome::NumericalFailure, std::nullopt};
        distances.push_back(distance);
        sum += distance;
    }
    if (distances.empty()) return {PlanarSeamOutcome::NoCommonPoints, std::nullopt};
    const std::size_t n = distances.size();
    const double mean = sum / static_cast<double>(n);
    std::sort(distances.begin(), distances.end());
    const std::size_t rank = (95 * n + 99) / 100; // ceil(0.95 n), 1-based
    if (!std::isfinite(mean)) return {PlanarSeamOutcome::NumericalFailure, std::nullopt};
    return {PlanarSeamOutcome::Measured, SeamDisagreement{mean, distances[rank - 1], distances.back(), static_cast<std::uint32_t>(n)}};
}

PlanarCameraCoverage Coverage(const CalibrationObservationSession& session, const PlanarCalibrationRequest& request, std::size_t index) {
    const auto camera = index == 0 ? CameraAlias::CamA : CameraAlias::CamB;
    PlanarCameraCoverage out{std::nullopt, {}};
    const auto& group = session.Camera(camera);
    if (const auto k = NominalIntrinsics(request.nominal[index], group.width_pixels, group.height_pixels)) {
        const double x = -k->cx / k->fx, y = -k->cy / k->fy;
        out.image_corner_radius_squared = x * x + y * y;
    }
    const auto* overlap = OverlapView(session, request, index);
    for (const auto& view : session.Views()) {
        if (view.camera != camera || (view.role != ViewRole::Training && &view != overlap)) continue;
        std::unordered_map<std::string_view, PixelPosition> pixels; // looked up only, never iterated
        pixels.reserve(view.observations.size());
        for (const auto& observation : view.observations) if (observation.pixel) pixels.emplace(observation.point_id, *observation.pixel);
        const auto cell = [](double coordinate, std::uint32_t size) {
            const double value = std::floor(4.0 * (coordinate + 0.5) / size);
            return value < 1 ? std::size_t{0} : value >= 3 ? std::size_t{3} : static_cast<std::size_t>(value);
        };
        for (const auto& id : view.used_point_ids) {
            const auto pixel = pixels.find(id);
            if (pixel == pixels.end()) continue; // Parse guarantees a detected pixel
            ++out.fit_point_cells[cell(pixel->second.y, view.height_pixels) * 4 + cell(pixel->second.x, view.width_pixels)];
        }
    }
    return out;
}
} // namespace

std::optional<PinholeIntrinsics> NominalIntrinsics(const NominalCamera& camera, std::uint32_t width, std::uint32_t height) {
    for (const auto value : {camera.focal_length_mm, camera.sensor_width_mm, camera.sensor_height_mm}) {
        if (!std::isfinite(value) || !(value > 0) || value > kMaximumNominalMm) return std::nullopt;
    }
    if (width < 1 || height < 1) return std::nullopt;
    const PinholeIntrinsics k{camera.focal_length_mm * width / camera.sensor_width_mm,
        camera.focal_length_mm * height / camera.sensor_height_mm, (width - 1.0) / 2.0, (height - 1.0) / 2.0};
    if (!lens_detail::PhysicalIntrinsics(k)) return std::nullopt;
    return k;
}

PlanarCalibrationResult EstimatePlanarCalibration(const CalibrationObservationSession& session,
    const PlanarCalibrationRequest& request, const PlanarCalibrationPolicy& policy) {
    ValidatePolicy(policy);
    return PlanarCalibrationResult{{EvaluateCamera(session, 0, request, policy), EvaluateCamera(session, 1, request, policy)}};
}

PlanarSeamMeasure MeasureOverlapSeam(const CalibrationObservationSession& session, const PlanarCalibrationRequest& request,
    const PlanarCalibrationPolicy& policy, const PlanarCalibrationResult& result) {
    ValidatePolicy(policy);
    return Seam(session, request, policy, result, nullptr, true);
}

PlanarCalibrationMeasures MeasurePlanarCalibration(const CalibrationObservationSession& session, const PlanarCalibrationRequest& request,
    const PlanarCalibrationPolicy& policy, const PlanarCalibrationResult& result) {
    ValidatePolicy(policy);
    PlanarCalibrationMeasures out{Seam(session, request, policy, result, nullptr, true), {PlanarSeamOutcome::CameraNotEstimated, std::nullopt},
        {Coverage(session, request, 0), Coverage(session, request, 1)}};
    const std::array<const SessionView*, 2> overlaps{OverlapView(session, request, 0), OverlapView(session, request, 1)};
    for (std::size_t i = 0; i < 2; ++i)
        if (overlaps[i] == nullptr || result.cameras[i].status != PlanarCameraStatus::Estimated) return out;
    // Estimate again with the even half of each overlap view, measure on the odd half.
    const auto checkerboard = CheckerboardEven(session);
    std::array<std::vector<std::string>, 2> even_ids;
    for (std::size_t i = 0; i < 2; ++i)
        for (const auto& id : overlaps[i]->used_point_ids) {
            const auto colour = checkerboard.find(id);
            if (colour != checkerboard.end() && colour->second) even_ids[i].push_back(id);
        }
    const PlanarCalibrationResult half{{EvaluateCamera(session, 0, request, policy, &even_ids[0]),
        EvaluateCamera(session, 1, request, policy, &even_ids[1])}};
    out.checkerboard_seam = Seam(session, request, policy, half, &checkerboard, false);
    return out;
}

} // namespace a0::m2::calibration
