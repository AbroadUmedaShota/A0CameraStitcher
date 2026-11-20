#include "a0/m2/experimental/content_registration.hpp"
#include "grid_sheet_tilt_detail.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace a0::m2::experimental::grid_sheet_tilt::detail {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kMaxLatticeIndex = 1e6;   // lattice lines further than this from the first lattice's origin are not counted

Mat3 Multiply(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[3 * i + j] += a[3 * i + k] * b[3 * k + j];
    return r;
}
// False for a singular matrix.
bool Inverse(const Mat3& m, Mat3& out) {
    const double c00 = m[4] * m[8] - m[5] * m[7], c01 = m[5] * m[6] - m[3] * m[8], c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (!std::isfinite(det) || std::abs(det) < 1e-300) return false;
    const double s = 1.0 / det;
    out = {c00 * s, (m[2] * m[7] - m[1] * m[8]) * s, (m[1] * m[5] - m[2] * m[4]) * s,
           c01 * s, (m[0] * m[8] - m[2] * m[6]) * s, (m[2] * m[3] - m[0] * m[5]) * s,
           c02 * s, (m[1] * m[6] - m[0] * m[7]) * s, (m[0] * m[4] - m[1] * m[3]) * s};
    return true;
}
Mat3 GOf(const LatticeParameters& p) { return {p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], 1.0}; }

// Gaussian elimination with partial pivoting for a small dense system. False when singular.
template <std::size_t N>
bool Solve(std::array<std::array<double, N>, N> a, std::array<double, N> b, std::array<double, N>& x) {
    for (std::size_t c = 0; c < N; ++c) {
        std::size_t pivot = c;
        for (std::size_t r = c + 1; r < N; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        if (!(std::abs(a[pivot][c]) > 1e-300)) return false;
        std::swap(a[c], a[pivot]); std::swap(b[c], b[pivot]);
        for (std::size_t r = c + 1; r < N; ++r) {
            const double f = a[r][c] / a[c][c];
            for (std::size_t k = c; k < N; ++k) a[r][k] -= f * a[c][k];
            b[r] -= f * b[c];
        }
    }
    for (std::size_t i = N; i-- > 0;) {
        double s = b[i];
        for (std::size_t k = i + 1; k < N; ++k) s -= a[i][k] * x[k];
        x[i] = s / a[i][i];
        if (!std::isfinite(x[i])) return false;
    }
    return true;
}

double Median(std::vector<double> v) {
    if (v.empty()) return 0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    return v[mid];
}
double ScaleOf(const std::vector<double>& r) {   // 1.4826 x median absolute deviation
    const double med = Median(r);
    std::vector<double> dev(r.size());
    for (std::size_t i = 0; i < r.size(); ++i) dev[i] = std::abs(r[i] - med);
    return 1.4826 * Median(std::move(dev));
}
double Tukey(double r, double c) {
    const double u = r / c;
    return std::abs(u) >= 1 ? 0.0 : (1 - u * u) * (1 - u * u);
}
double Wrap180(double degrees) {
    double v = std::fmod(degrees, 360.0);
    if (v <= -180.0) v += 360.0;
    if (v > 180.0) v -= 360.0;
    return v;
}

}   // namespace

// ---- lens -----------------------------------------------------------------------------------------------------------

Lens LensOf(const render::CameraProjection& projection) {
    const auto& i = projection.intrinsics;
    const auto& d = projection.distortion;
    return {i.fx_pixels, i.fy_pixels, i.cx_pixels, i.cy_pixels, d.k1, d.k2, d.k3, d.p1, d.p2};
}

void DistortNormalized(const Lens& lens, double x, double y, double& xd, double& yd) {
    const double r2 = x * x + y * y;
    const double radial = 1.0 + r2 * (lens.k1 + r2 * (lens.k2 + r2 * lens.k3));
    xd = x * radial + 2.0 * lens.p1 * x * y + lens.p2 * (r2 + 2.0 * x * x);
    yd = y * radial + lens.p1 * (r2 + 2.0 * y * y) + 2.0 * lens.p2 * x * y;
}

bool UndistortNormalized(const Lens& lens, double xd, double yd, double& x, double& y, double* jacobian) {
    x = xd; y = yd;
    for (int iteration = 0; iteration < 40; ++iteration) {
        double fx, fy;
        DistortNormalized(lens, x, y, fx, fy);
        const double ex = fx - xd, ey = fy - yd;
        const double r2 = x * x + y * y;
        const double radial = 1.0 + r2 * (lens.k1 + r2 * (lens.k2 + r2 * lens.k3));
        const double slope = lens.k1 + r2 * (2.0 * lens.k2 + r2 * 3.0 * lens.k3);
        const double a = radial + 2.0 * x * x * slope + 2.0 * lens.p1 * y + 6.0 * lens.p2 * x;
        const double b = 2.0 * x * y * slope + 2.0 * lens.p1 * x + 2.0 * lens.p2 * y;
        const double c = radial + 2.0 * y * y * slope + 6.0 * lens.p1 * y + 2.0 * lens.p2 * x;
        const double det = a * c - b * b;
        if (!std::isfinite(det) || !(det > 0)) return false;
        if (std::abs(ex) < 1e-13 && std::abs(ey) < 1e-13) {
            if (jacobian) { jacobian[0] = a; jacobian[1] = b; jacobian[2] = c; }
            return true;
        }
        x -= (c * ex - b * ey) / det;
        y -= (a * ey - b * ex) / det;
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
    }
    return false;
}

PreparedPoints PreparePoints(const std::vector<RidgePoint>& ridges, const Lens& lens, bool use_distortion) {
    PreparedPoints out;
    out.qx.reserve(ridges.size()); out.qy.reserve(ridges.size()); out.tx.reserve(ridges.size()); out.ty.reserve(ridges.size());
    out.source.reserve(ridges.size());
    for (std::size_t i = 0; i < ridges.size(); ++i) {
        const auto& r = ridges[i];
        const double xd = (r.x - lens.cx) / lens.fx, yd = (r.y - lens.cy) / lens.fy;
        double x = xd, y = yd, jac[3] = {1, 0, 1};
        if (use_distortion && !UndistortNormalized(lens, xd, yd, x, y, jac)) continue;
        // The line's direction in the picture is (-ny, nx); in the normalised plane it is the distortion's inverse
        // Jacobian applied to that direction in normalised pixels.
        const double u = -static_cast<double>(r.ny) / lens.fx, v = static_cast<double>(r.nx) / lens.fy;
        const double det = jac[0] * jac[2] - jac[1] * jac[1];
        const double dx = (jac[2] * u - jac[1] * v) / det, dy = (-jac[1] * u + jac[0] * v) / det;
        const double n = std::hypot(dx, dy);
        if (!(n > 0) || !std::isfinite(n)) continue;
        out.qx.push_back(x); out.qy.push_back(y); out.tx.push_back(dx / n); out.ty.push_back(dy / n);
        out.source.push_back(static_cast<std::uint32_t>(i));
    }
    return out;
}

// ---- lattice matrices -----------------------------------------------------------------------------------------------

Mat3 LatticeToPlane(const LatticeParameters& p) {
    Mat3 g_inverse{};
    if (!Inverse(GOf(p), g_inverse)) return {};
    // lattice (pitches) -> lattice / scale -> plane
    Mat3 h = Multiply(g_inverse, {1 / kLatticeScale, 0, 0, 0, 1 / kLatticeScale, 0, 0, 0, 1});
    const double s = h[8];
    if (!std::isfinite(s) || std::abs(s) < 1e-300) return {};
    for (auto& v : h) v /= s;
    return h;
}

LatticeParameters ParametersOfLatticeToPlane(const Mat3& h) {
    const Mat3 scaled = Multiply(h, {kLatticeScale, 0, 0, 0, kLatticeScale, 0, 0, 0, 1});
    Mat3 g{};
    if (!Inverse(scaled, g) || std::abs(g[8]) < 1e-300) return {};
    for (auto& v : g) v /= g[8];
    return {g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]};
}

bool PositivelyOriented(const Mat3& h) {
    if (!(h[8] > 0)) return false;
    return h[0] * h[4] - h[3] * h[1] > 0;
}

// ---- assignment and residuals ---------------------------------------------------------------------------------------

Assignment AssignPoints(const LatticeParameters& p, const PreparedPoints& points, double focal, double direction_match_deg, double gate_px) {
    const std::size_t n = points.size();
    Assignment a;
    a.distance_px.assign(n, 0.0f); a.family.assign(n, 0); a.index.assign(n, 0); a.ok.assign(n, 0);
    const Mat3 g = GOf(p);
    const double sin_limit = std::sin(direction_match_deg * kDegrees);
    for (std::size_t i = 0; i < n; ++i) {
        const double qx = points.qx[i], qy = points.qy[i];
        const double xx = g[0] * qx + g[1] * qy + g[2], yy = g[3] * qx + g[4] * qy + g[5], w = g[6] * qx + g[7] * qy + g[8];
        if (!(w > 1e-9)) continue;
        const double sx = xx / w, sy = yy / w;
        const double gx0 = (g[0] - sx * g[6]) / w * kLatticeScale, gx1 = (g[1] - sx * g[7]) / w * kLatticeScale;
        const double gy0 = (g[3] - sy * g[6]) / w * kLatticeScale, gy1 = (g[4] - sy * g[7]) / w * kLatticeScale;
        const double ngx = std::hypot(gx0, gx1), ngy = std::hypot(gy0, gy1);
        if (!(ngx > 0) || !(ngy > 0)) continue;
        // Lines x = const run perpendicular to the gradient of x: the sine of the angle between the tangent and the
        // direction of a family is the cosine of the angle between the tangent and that gradient.
        const double s1 = std::abs(points.tx[i] * gx0 + points.ty[i] * gx1) / ngx;
        const double s2 = std::abs(points.tx[i] * gy0 + points.ty[i] * gy1) / ngy;
        const bool first = s1 < s2;
        const double x = sx * kLatticeScale, y = sy * kLatticeScale;
        const double coordinate = first ? x : y;
        // A lattice index that does not fit the index type is no line of this picture (and its conversion would be undefined).
        if (!(std::abs(coordinate) < kMaxLatticeIndex)) continue;
        const double k = std::rint(coordinate);
        const double d = (coordinate - k) / (first ? ngx : ngy) * focal;
        a.distance_px[i] = static_cast<float>(d);
        a.family[i] = first ? 0 : 1;
        a.index[i] = static_cast<std::int32_t>(k);
        a.ok[i] = (std::min(s1, s2) < sin_limit) && (gate_px <= 0 || std::abs(d) < gate_px);
    }
    return a;
}

void LatticeResiduals(const LatticeParameters& p, const PreparedPoints& points, const std::vector<std::uint32_t>& indices,
    const std::vector<std::uint8_t>& family, const std::vector<std::int32_t>& index, double focal,
    std::vector<double>& residual, std::vector<double>* jacobian) {
    const std::size_t n = indices.size();
    residual.assign(n, 0.0);
    if (jacobian) jacobian->assign(n * 8, 0.0);
    for (std::size_t m = 0; m < n; ++m) {
        const std::uint32_t i = indices[m];
        const double qt[3] = {points.qx[i], points.qy[i], 1.0};
        const int fam = family[m];
        const double a0 = p[3 * fam], a1 = p[3 * fam + 1], a2 = p[3 * fam + 2];
        const double g20 = p[6], g21 = p[7];
        const double X = a0 * qt[0] + a1 * qt[1] + a2, W = g20 * qt[0] + g21 * qt[1] + 1.0;
        const double s = X / W, kappa = index[m] / kLatticeScale, u = s - kappa;
        const double gv0 = a0 - s * g20, gv1 = a1 - s * g21, ng = std::hypot(gv0, gv1);
        residual[m] = focal * W * u / ng;
        if (!jacobian) continue;
        double* row = jacobian->data() + m * 8;
        for (int j = 0; j < 8; ++j) {
            double dX = 0, dW = 0, dA0 = 0, dA1 = 0, dG20 = 0, dG21 = 0;
            if (j >= 3 * fam && j < 3 * fam + 3) {
                dX = qt[j - 3 * fam];
                if (j - 3 * fam == 0) dA0 = 1;
                if (j - 3 * fam == 1) dA1 = 1;
            } else if (j == 6) { dW = qt[0]; dG20 = 1; }
            else if (j == 7) { dW = qt[1]; dG21 = 1; }
            else continue;
            const double ds = (dX - s * dW) / W;
            const double dg0 = dA0 - ds * g20 - s * dG20, dg1 = dA1 - ds * g21 - s * dG21;
            const double dng = (gv0 * dg0 + gv1 * dg1) / ng;
            row[j] = focal * (dW * u / ng + W * ds / ng - W * u * dng / (ng * ng));
        }
    }
}

namespace {

struct StepResult {
    bool ok{};
    LatticeParameters parameters{};
    std::size_t inliers{};
    bool moved{};
    double shift_px{};   // largest change of a point's distance to its line made by the step (pixels)
};

// One robust Gauss-Newton step of the lattice fit with the assignment held fixed. The robust weights come from the
// residuals at the current parameters.
StepResult GaussNewtonStep(const PreparedPoints& points, double focal, const LatticeParameters& p, double gate_px,
    double direction_match_deg, std::uint32_t min_inliers) {
    StepResult out;
    out.parameters = p;
    const Assignment a = AssignPoints(p, points, focal, direction_match_deg, gate_px);
    std::vector<std::uint32_t> idx;
    std::vector<std::uint8_t> fam;
    std::vector<std::int32_t> k;
    std::vector<double> r0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!a.ok[i]) continue;
        idx.push_back(static_cast<std::uint32_t>(i)); fam.push_back(a.family[i]); k.push_back(a.index[i]); r0.push_back(a.distance_px[i]);
    }
    out.inliers = idx.size();
    if (idx.size() < min_inliers) return out;
    const double sigma = ScaleOf(r0);
    const double c = std::min(std::max(3.0 * sigma, 0.3), gate_px);
    std::vector<double> w(idx.size());
    for (std::size_t m = 0; m < idx.size(); ++m) w[m] = Tukey(r0[m], c);

    std::vector<double> r, jac;
    LatticeResiduals(p, points, idx, fam, k, focal, r, &jac);
    const auto cost_of = [&](const std::vector<double>& res) {
        double s = 0;
        for (std::size_t m = 0; m < res.size(); ++m) s += w[m] * res[m] * res[m];
        return s;
    };
    std::array<std::array<double, 8>, 8> normal{};
    std::array<double, 8> rhs{};
    for (std::size_t m = 0; m < idx.size(); ++m) {
        if (w[m] <= 0) continue;
        const double* row = jac.data() + m * 8;
        for (int i = 0; i < 8; ++i) {
            rhs[i] -= w[m] * row[i] * r[m];
            for (int j = i; j < 8; ++j) normal[i][j] += w[m] * row[i] * row[j];
        }
    }
    std::array<double, 8> scale{};
    for (int i = 0; i < 8; ++i) { scale[i] = std::sqrt(normal[i][i]) + 1e-30; for (int j = 0; j < i; ++j) normal[i][j] = normal[j][i]; }
    std::array<std::array<double, 8>, 8> scaled{};
    std::array<double, 8> scaled_rhs{};
    for (int i = 0; i < 8; ++i) {
        scaled_rhs[i] = rhs[i] / scale[i];
        for (int j = 0; j < 8; ++j) scaled[i][j] = normal[i][j] / (scale[i] * scale[j]) + (i == j ? 1e-10 : 0.0);
    }
    std::array<double, 8> x{};
    if (!Solve<8>(scaled, scaled_rhs, x)) return out;
    const double cost0 = cost_of(r);
    double factor = 1.0;
    for (int attempt = 0; attempt < 6; ++attempt, factor *= 0.5) {
        LatticeParameters trial{};
        for (int i = 0; i < 8; ++i) trial[i] = p[i] + factor * x[i] / scale[i];
        std::vector<double> rt;
        LatticeResiduals(trial, points, idx, fam, k, focal, rt, nullptr);
        bool finite = true;
        for (double v : rt) if (!std::isfinite(v)) { finite = false; break; }
        if (finite && cost_of(rt) <= cost0) {
            out.parameters = trial; out.ok = true;
            for (std::size_t m = 0; m < rt.size(); ++m) out.shift_px = std::max(out.shift_px, std::abs(rt[m] - r[m]));
            double change = 0;
            for (int i = 0; i < 8; ++i) change = std::max(change, std::abs(trial[i] - p[i]) / (std::abs(trial[i]) + 1e-3));
            out.moved = change >= 1e-7;
            return out;
        }
    }
    out.ok = true;   // no step lowers the cost: the fit stays where it is
    return out;
}

struct StageResult {
    bool ok{};
    double last_shift_px{std::numeric_limits<double>::infinity()};   // the last step of the row's last gate
};
StageResult FitStage(const PreparedPoints& points, double focal, LatticeParameters& p, const std::array<double, 4>& gates, const GridSheetDetectionSettings& s) {
    StageResult out;
    for (double gate : gates) {
        if (gate <= 0) break;
        for (std::uint32_t it = 0; it < s.fit_iterations; ++it) {
            const StepResult step = GaussNewtonStep(points, focal, p, gate, s.direction_match_deg, s.fit_min_inliers);
            if (step.inliers < s.fit_min_inliers || !step.ok) return out;
            p = step.parameters;
            out.last_shift_px = step.shift_px;
            if (!step.moved) break;
        }
    }
    out.ok = true;
    return out;
}

// Every `stride`-th point of `members`, at most about `max_points` of them.
PreparedPoints Subset(const PreparedPoints& points, const std::vector<std::uint32_t>& members, std::uint32_t max_points) {
    const std::size_t stride = members.size() / std::max<std::size_t>(max_points, 1) + 1;
    PreparedPoints sub;
    for (std::size_t m = 0; m < members.size(); m += stride) {
        const auto i = members[m];
        sub.qx.push_back(points.qx[i]); sub.qy.push_back(points.qy[i]); sub.tx.push_back(points.tx[i]); sub.ty.push_back(points.ty[i]);
        sub.source.push_back(points.source[i]);
    }
    return sub;
}

}   // namespace

LatticeFit FitLattice(const PreparedPoints& points, double focal, const InitialLattice& start, const GridSheetDetectionSettings& s) {
    LatticeFit fit;
    fit.parameters = start.parameters;
    for (std::size_t stage = 0; stage < s.radii.size(); ++stage) {
        const double radius = s.radii[stage];
        std::vector<std::uint32_t> inside;
        for (std::size_t i = 0; i < points.size(); ++i)
            if (std::abs(points.qx[i] - start.centre_x) < radius && std::abs(points.qy[i] - start.centre_y) < radius) inside.push_back(static_cast<std::uint32_t>(i));
        const PreparedPoints sub = Subset(points, inside, s.fit_max_points);
        LatticeParameters p = fit.parameters;
        const StageResult result = FitStage(sub, focal, p, s.gates_px[std::min<std::size_t>(stage, s.gates_px.size() - 1)], s);
        if (!result.ok) {
            // A later stage that runs out of points leaves the earlier result, which covers only a smaller window: it is
            // kept as evidence but does not count as converged.
            fit.ok = stage > 0;
            fit.converged = false;
            return fit;
        }
        fit.parameters = p;
        fit.last_step_px = result.last_shift_px;
        fit.converged = result.last_shift_px <= s.fit_settle_px;
    }
    fit.ok = true;
    return fit;
}

// ---- first lattice --------------------------------------------------------------------------------------------------

namespace {

struct Peak {
    double pitch{}, score{}, phase{};   // phase in length units, from the sum's argument
};
Peak BestPitch(const std::vector<double>& s, double low, double high, double step) {
    Peak best;
    best.score = -1;
    if (s.empty() || !(step > 0)) return best;
    for (double pitch = low; pitch < high; pitch += step) {
        double zr = 0, zi = 0;
        const double w = 2 * kPi / pitch;
        for (double v : s) { const double a = w * v; zr += std::cos(a); zi += std::sin(a); }
        const double score = std::hypot(zr, zi) / static_cast<double>(s.size());
        if (score > best.score) { best.score = score; best.pitch = pitch; best.phase = std::atan2(zi, zr) / (2 * kPi) * pitch; }
    }
    return best;
}
// |difference of two line angles| folded to [0, pi/2].
double DirectionDistance(double a, double b) {
    double d = std::fmod(a - b, kPi);
    if (d < 0) d += kPi;
    return std::min(d, kPi - d);
}

struct Directions {
    double th1{}, th2{};
};
// The strongest line direction among the angles, and the strongest within a window of the perpendicular.
Directions MainDirections(const std::vector<float>& angle, const std::vector<std::uint32_t>& members, int bins, double window_deg) {
    std::vector<double> h(static_cast<std::size_t>(bins), 0.0);
    for (auto i : members) {
        int b = static_cast<int>(angle[i] / kPi * bins);
        h[static_cast<std::size_t>(std::clamp(b, 0, bins - 1))] += 1;
    }
    std::vector<double> hs(h.size());
    for (int i = 0; i < bins; ++i) {
        double sum = 0;
        for (int j = -3; j <= 3; ++j) sum += h[static_cast<std::size_t>(((i + j) % bins + bins) % bins)];
        hs[static_cast<std::size_t>(i)] = sum / 7.0;
    }
    const auto centre = [&](int i) { return (i + 0.5) * kPi / bins; };
    int best = 0;
    for (int i = 1; i < bins; ++i) if (hs[static_cast<std::size_t>(i)] > hs[static_cast<std::size_t>(best)]) best = i;
    Directions d;
    d.th1 = centre(best);
    int best2 = -1;
    double value2 = -1;
    for (int i = 0; i < bins; ++i) {
        if (DirectionDistance(centre(i), d.th1 + kPi / 2) >= window_deg * kDegrees) continue;
        if (hs[static_cast<std::size_t>(i)] > value2) { value2 = hs[static_cast<std::size_t>(i)]; best2 = i; }
    }
    d.th2 = centre(best2 < 0 ? 0 : best2);
    return d;
}

}   // namespace

std::optional<InitialLattice> FindInitialLattice(const PreparedPoints& points, double focal, const GridSheetDetectionSettings& s) {
    const std::size_t n = points.size();
    if (n < s.window_min_points || n < s.direction_min_points) return std::nullopt;
    std::vector<float> angle(n);
    double min_x = 1e300, max_x = -1e300, min_y = 1e300, max_y = -1e300;
    for (std::size_t i = 0; i < n; ++i) {
        double a = std::atan2(points.ty[i], points.tx[i]);
        if (a < 0) a += kPi;
        if (a >= kPi) a -= kPi;
        angle[i] = static_cast<float>(a);
        min_x = std::min(min_x, points.qx[i]); max_x = std::max(max_x, points.qx[i]);
        min_y = std::min(min_y, points.qy[i]); max_y = std::max(max_y, points.qy[i]);
    }
    const double low = s.pitch_min_px / focal, high = s.pitch_max_px / focal;
    const double coarse = low / s.pitch_coarse_divisor;
    const double size = s.window_half_size;
    const double tolerance = s.direction_gate_deg * kDegrees;

    struct Candidate { double score; double cx, cy; };
    std::vector<Candidate> candidates;
    for (double cx = min_x; cx <= max_x + 1e-12; cx += size) {
        for (double cy = min_y; cy <= max_y + 1e-12; cy += size) {
            std::vector<std::uint32_t> members;
            for (std::size_t i = 0; i < n; ++i)
                if (std::abs(points.qx[i] - cx) < size && std::abs(points.qy[i] - cy) < size) members.push_back(static_cast<std::uint32_t>(i));
            if (members.size() < s.window_min_points) continue;
            const Directions d = MainDirections(angle, members, 180, s.second_direction_window_deg);
            double score = 1e9;
            for (double th : {d.th1, d.th2}) {
                std::vector<std::uint32_t> sel;
                for (auto i : members) if (DirectionDistance(angle[i], th) < tolerance) sel.push_back(i);
                if (sel.size() < s.direction_min_points) { score = 0; break; }
                const std::size_t stride = sel.size() / std::max<std::size_t>(s.window_scan_points, 1) + 1;
                std::vector<double> along;
                for (std::size_t m = 0; m < sel.size(); m += stride) {
                    const auto i = sel[m];
                    along.push_back((points.qx[i] - cx) * -std::sin(th) + (points.qy[i] - cy) * std::cos(th));
                }
                score = std::min(score, BestPitch(along, low, high, coarse).score);
            }
            candidates.push_back({score, cx, cy});
        }
    }
    if (candidates.empty()) return std::nullopt;
    const auto best = std::max_element(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
    if (!(best->score >= s.window_min_score)) return std::nullopt;

    const double ox = best->cx, oy = best->cy;
    std::vector<std::uint32_t> window;
    for (std::size_t i = 0; i < n; ++i)
        if (std::abs(points.qx[i] - ox) < size && std::abs(points.qy[i] - oy) < size) window.push_back(static_cast<std::uint32_t>(i));
    const Directions d = MainDirections(angle, window, 360, s.second_direction_window_deg);
    double normal[2][2], pitch[2], phase[2];
    int family = 0;
    for (double th : {d.th1, d.th2}) {
        normal[family][0] = -std::sin(th); normal[family][1] = std::cos(th);
        std::vector<std::uint32_t> sel;
        for (auto i : window) if (DirectionDistance(angle[i], th) < tolerance) sel.push_back(i);
        if (sel.size() < std::max<std::size_t>(s.lattice_min_points, 1)) return std::nullopt;
        const std::size_t stride = sel.size() / std::max<std::size_t>(s.init_scan_points, 1) + 1;
        std::vector<double> along;
        for (std::size_t m = 0; m < sel.size(); m += stride) {
            const auto i = sel[m];
            along.push_back((points.qx[i] - ox) * normal[family][0] + (points.qy[i] - oy) * normal[family][1]);
        }
        const Peak coarse_peak = BestPitch(along, low, high, coarse);
        const Peak fine_peak = BestPitch(along, coarse_peak.pitch * (1 - s.pitch_fine_span), coarse_peak.pitch * (1 + s.pitch_fine_span), s.pitch_fine_step);
        if (!(fine_peak.score > 0)) return std::nullopt;
        pitch[family] = fine_peak.pitch; phase[family] = fine_peak.phase;
        ++family;
    }
    if (normal[0][0] * normal[1][1] - normal[0][1] * normal[1][0] < 0) {
        normal[1][0] = -normal[1][0]; normal[1][1] = -normal[1][1];
        phase[1] = -phase[1];
    }
    Mat3 g{};
    for (int f = 0; f < 2; ++f) {
        g[3 * f] = normal[f][0] / pitch[f];
        g[3 * f + 1] = normal[f][1] / pitch[f];
        g[3 * f + 2] = (-(normal[f][0] * ox + normal[f][1] * oy) - phase[f]) / pitch[f];
        g[3 * f + 2] -= std::rint(g[3 * f] * ox + g[3 * f + 1] * oy + g[3 * f + 2]);
    }
    InitialLattice init;
    for (int f = 0; f < 2; ++f) for (int j = 0; j < 3; ++j) init.parameters[static_cast<std::size_t>(3 * f + j)] = g[static_cast<std::size_t>(3 * f + j)] / kLatticeScale;
    init.parameters[6] = 0; init.parameters[7] = 0;
    init.pitch_px = {pitch[0] * focal, pitch[1] * focal};
    init.centre_x = ox; init.centre_y = oy; init.score = best->score;
    return init;
}

// ---- lines, nodes, vanishing points ---------------------------------------------------------------------------------

std::vector<LineFit> FitLines(const PreparedPoints& points, const Assignment& a, double focal, std::size_t min_points) {
    std::vector<std::uint32_t> idx;
    for (std::size_t i = 0; i < points.size(); ++i) if (a.ok[i]) idx.push_back(static_cast<std::uint32_t>(i));
    const auto key = [&](std::uint32_t i) { return std::pair<int, std::int32_t>{a.family[i], a.index[i]}; };
    std::stable_sort(idx.begin(), idx.end(), [&](std::uint32_t l, std::uint32_t r) { return key(l) < key(r); });
    std::vector<LineFit> lines;
    for (std::size_t begin = 0; begin < idx.size();) {
        std::size_t end = begin;
        while (end < idx.size() && key(idx[end]) == key(idx[begin])) ++end;
        if (end - begin >= min_points) {
            LineFit l;
            l.family = key(idx[begin]).first; l.index = key(idx[begin]).second; l.points = end - begin;
            double mx = 0, my = 0;
            for (std::size_t m = begin; m < end; ++m) { mx += points.qx[idx[m]]; my += points.qy[idx[m]]; }
            mx /= static_cast<double>(l.points); my /= static_cast<double>(l.points);
            double sxx = 0, sxy = 0, syy = 0;
            for (std::size_t m = begin; m < end; ++m) {
                const double dx = points.qx[idx[m]] - mx, dy = points.qy[idx[m]] - my;
                sxx += dx * dx; sxy += dx * dy; syy += dy * dy;
            }
            const double theta = 0.5 * std::atan2(2 * sxy, sxx - syy);
            l.centre[0] = mx; l.centre[1] = my;
            l.direction[0] = std::cos(theta); l.direction[1] = std::sin(theta);
            l.normal[0] = -l.direction[1]; l.normal[1] = l.direction[0];
            double sum_sq = 0;
            l.t.reserve(l.points);
            for (std::size_t m = begin; m < end; ++m) {
                const double dx = points.qx[idx[m]] - mx, dy = points.qy[idx[m]] - my;
                l.t.push_back(dx * l.direction[0] + dy * l.direction[1]);
                const double r = (dx * l.normal[0] + dy * l.normal[1]) * focal;
                sum_sq += r * r;
            }
            std::sort(l.t.begin(), l.t.end());
            l.t_min = l.t.front(); l.t_max = l.t.back();
            l.rms_px = std::sqrt(sum_sq / static_cast<double>(l.points));
            lines.push_back(std::move(l));
        }
        begin = end;
    }
    return lines;
}

NodeFit FitNodes(const std::vector<LineFit>& lines, const Mat3& start, double focal, const GridSheetDetectionSettings& s) {
    NodeFit out;
    struct Node { double i, j, qx, qy; };
    std::vector<Node> nodes;
    const double gap = s.node_max_gap_px / focal;
    const auto supports = [&](const LineFit& l, double qx, double qy) {
        const double t = (qx - l.centre[0]) * l.direction[0] + (qy - l.centre[1]) * l.direction[1];
        if (t < l.t_min || t > l.t_max) return false;
        const auto upper = std::lower_bound(l.t.begin(), l.t.end(), t);
        double nearest = std::numeric_limits<double>::infinity();
        if (upper != l.t.end()) nearest = std::min(nearest, std::abs(*upper - t));
        if (upper != l.t.begin()) nearest = std::min(nearest, std::abs(*(upper - 1) - t));
        return nearest <= gap;
    };
    for (const auto& a : lines) {
        if (a.family != 0) continue;
        for (const auto& b : lines) {
            if (b.family != 1) continue;
            const double la[3] = {a.normal[0], a.normal[1], -(a.normal[0] * a.centre[0] + a.normal[1] * a.centre[1])};
            const double lb[3] = {b.normal[0], b.normal[1], -(b.normal[0] * b.centre[0] + b.normal[1] * b.centre[1])};
            const double x0 = la[1] * lb[2] - la[2] * lb[1], x1 = la[2] * lb[0] - la[0] * lb[2], x2 = la[0] * lb[1] - la[1] * lb[0];
            if (std::abs(x2) < 1e-9) continue;
            const double qx = x0 / x2, qy = x1 / x2;
            if (supports(a, qx, qy) && supports(b, qx, qy)) nodes.push_back({static_cast<double>(a.index), static_cast<double>(b.index), qx, qy});
        }
    }
    out.nodes = nodes.size();
    if (nodes.size() < s.node_min_count) return out;

    // H scaled: lattice / scale -> plane, last entry 1.
    Mat3 hs = Multiply(start, {kLatticeScale, 0, 0, 0, kLatticeScale, 0, 0, 0, 1});
    if (!(std::abs(hs[8]) > 1e-300)) return out;
    for (auto& v : hs) v /= hs[8];
    std::array<double, 8> p{hs[0], hs[1], hs[2], hs[3], hs[4], hs[5], hs[6], hs[7]};
    const std::size_t m = nodes.size();
    const auto residual = [&](const std::array<double, 8>& q, std::vector<double>& r) {
        r.resize(2 * m);
        for (std::size_t k = 0; k < m; ++k) {
            const double lx = nodes[k].i / kLatticeScale, ly = nodes[k].j / kLatticeScale;
            const double X = q[0] * lx + q[1] * ly + q[2], Y = q[3] * lx + q[4] * ly + q[5], W = q[6] * lx + q[7] * ly + 1.0;
            r[2 * k] = (X / W - nodes[k].qx) * focal;
            r[2 * k + 1] = (Y / W - nodes[k].qy) * focal;
        }
    };
    std::vector<double> r0, rp, rm;
    std::vector<double> w(2 * m, 1.0);
    for (int iteration = 0; iteration < 12; ++iteration) {
        residual(p, r0);
        const double sigma = ScaleOf(r0);
        const double c = std::max(4 * sigma, 0.5);
        for (std::size_t k = 0; k < r0.size(); ++k) w[k] = Tukey(r0[k], c);
        std::vector<std::array<double, 8>> jac(r0.size());
        for (int j = 0; j < 8; ++j) {
            const double h = 1e-6 * std::max(std::abs(p[static_cast<std::size_t>(j)]), 1e-2);
            auto pp = p, pm = p;
            pp[static_cast<std::size_t>(j)] += h; pm[static_cast<std::size_t>(j)] -= h;
            residual(pp, rp); residual(pm, rm);
            for (std::size_t k = 0; k < r0.size(); ++k) jac[k][static_cast<std::size_t>(j)] = (rp[k] - rm[k]) / (2 * h);
        }
        std::array<std::array<double, 8>, 8> normal{};
        std::array<double, 8> rhs{};
        for (std::size_t k = 0; k < r0.size(); ++k) {
            if (w[k] <= 0) continue;
            for (std::size_t i = 0; i < 8; ++i) {
                rhs[i] -= w[k] * jac[k][i] * r0[k];
                for (std::size_t j = 0; j < 8; ++j) normal[i][j] += w[k] * jac[k][i] * jac[k][j];
            }
        }
        std::array<double, 8> scale{};
        for (std::size_t i = 0; i < 8; ++i) scale[i] = std::sqrt(normal[i][i]) + 1e-30;
        std::array<std::array<double, 8>, 8> scaled{};
        std::array<double, 8> scaled_rhs{};
        for (std::size_t i = 0; i < 8; ++i) {
            scaled_rhs[i] = rhs[i] / scale[i];
            for (std::size_t j = 0; j < 8; ++j) scaled[i][j] = normal[i][j] / (scale[i] * scale[j]) + (i == j ? 1e-10 : 0.0);
        }
        std::array<double, 8> x{};
        if (!Solve<8>(scaled, scaled_rhs, x)) return out;
        double change = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            p[i] += x[i] / scale[i];
            change = std::max(change, std::abs(x[i] / scale[i]) / (std::abs(p[i]) + 1e-3));
        }
        if (change < 1e-8) break;
    }
    residual(p, r0);
    double sum = 0;
    std::size_t used = 0;
    for (std::size_t k = 0; k < r0.size(); ++k) if (w[k] > 0) { sum += r0[k] * r0[k]; ++used; }
    if (used == 0) return out;
    out.rms_px = std::sqrt(sum / static_cast<double>(used));
    Mat3 h{p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], 1.0};
    out.lattice_to_plane = Multiply(h, {1 / kLatticeScale, 0, 0, 0, 1 / kLatticeScale, 0, 0, 0, 1});
    out.ok = true;
    return out;
}

namespace {
// Eigenvector of the smallest eigenvalue of a symmetric 3x3 matrix, by cyclic Jacobi rotations.
std::array<double, 3> SmallestEigenvector(std::array<std::array<double, 3>, 3> a) {
    std::array<std::array<double, 3>, 3> v{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    for (int sweep = 0; sweep < 60; ++sweep) {
        const double off = std::abs(a[0][1]) + std::abs(a[0][2]) + std::abs(a[1][2]);
        if (off < 1e-300) break;
        for (int p = 0; p < 2; ++p) for (int q = p + 1; q < 3; ++q) {
            if (std::abs(a[p][q]) < 1e-300) continue;
            const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
            const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1));
            const double c = 1 / std::sqrt(t * t + 1), s = t * c;
            for (int k = 0; k < 3; ++k) {
                const double akp = a[k][p], akq = a[k][q];
                a[k][p] = c * akp - s * akq; a[k][q] = s * akp + c * akq;
            }
            for (int k = 0; k < 3; ++k) {
                const double apk = a[p][k], aqk = a[q][k];
                a[p][k] = c * apk - s * aqk; a[q][k] = s * apk + c * aqk;
            }
            for (int k = 0; k < 3; ++k) {
                const double vkp = v[k][p], vkq = v[k][q];
                v[k][p] = c * vkp - s * vkq; v[k][q] = s * vkp + c * vkq;
            }
        }
    }
    int smallest = 0;
    for (int i = 1; i < 3; ++i) if (a[i][i] < a[smallest][smallest]) smallest = i;
    return {v[0][static_cast<std::size_t>(smallest)], v[1][static_cast<std::size_t>(smallest)], v[2][static_cast<std::size_t>(smallest)]};
}
std::array<double, 3> Cross(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double Dot(const std::array<double, 3>& a, const std::array<double, 3>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
std::array<double, 3> Unit(const std::array<double, 3>& a) {
    const double n = std::sqrt(Dot(a, a));
    return {a[0] / n, a[1] / n, a[2] / n};
}
}   // namespace

VanishingFit FitVanishing(const std::vector<LineFit>& lines, const Mat3& h) {
    VanishingFit out;
    std::array<std::array<double, 3>, 2> vp{};
    for (int family = 0; family < 2; ++family) {
        std::array<std::array<double, 3>, 3> m{};
        std::size_t count = 0;
        for (const auto& l : lines) {
            if (l.family != family) continue;
            ++count;
            const double v[3] = {l.normal[0], l.normal[1], -(l.normal[0] * l.centre[0] + l.normal[1] * l.centre[1])};
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) m[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] += static_cast<double>(l.points) * v[i] * v[j];
        }
        if (count < 2) return out;
        vp[static_cast<std::size_t>(family)] = SmallestEigenvector(m);
    }
    // Lines x = const run along the lattice y axis: the first family's vanishing point is the y direction.
    std::array<double, 3> vy = vp[0], vx = vp[1];
    const std::array<double, 3> c0{h[0], h[3], h[6]}, c1{h[1], h[4], h[7]};
    if (Dot(vx, c0) < 0) for (auto& v : vx) v = -v;
    if (Dot(vy, c1) < 0) for (auto& v : vy) v = -v;
    const auto ex = Unit(vx);
    auto n = Cross(vx, vy);
    if (!(std::sqrt(Dot(n, n)) > 0)) return out;
    n = Unit(n);
    if (n[2] <= 0) return out;
    const auto ey = Cross(n, ex);
    const double ax = ex[2], ay = ey[2], az = n[2];   // the optical axis (0, 0, 1) in the sheet's basis
    const double lateral = std::hypot(ax, ay);
    out.tilt_deg = std::atan2(lateral, az) / kDegrees;
    if (lateral > 0) out.lean_sheet_deg = {out.tilt_deg * ax / lateral, out.tilt_deg * ay / lateral};
    out.image_x_azimuth_deg = Wrap180(std::atan2(ey[0], ex[0]) / kDegrees);
    out.ok = std::isfinite(out.tilt_deg);
    return out;
}

OrientationNumbers OrientationOfLattice(const render::CameraProjection& profile, const Mat3& h) {
    OrientationNumbers out;
    if (!PositivelyOriented(h)) return out;
    const auto& i = profile.intrinsics;
    const Mat3 k{i.fx_pixels, 0, i.cx_pixels, 0, i.fy_pixels, i.cy_pixels, 0, 0, 1};
    Mat3 pixel = Multiply(k, h);
    for (auto& v : pixel) v /= pixel[8];
    try {
        const render::CameraProjection camera(profile.intrinsics, profile.distortion, pixel);
        const auto o = content_registration::OrientationOf(camera);
        out.tilt_deg = o.tilt_deg;
        out.lean_sheet_deg = {o.tilt_deg * o.lens_direction.x, o.tilt_deg * o.lens_direction.y};
        out.image_x_azimuth_deg = o.rotation_clockwise_deg;
        out.ok = std::isfinite(out.tilt_deg) && std::isfinite(out.image_x_azimuth_deg);
    } catch (const std::invalid_argument&) {
        out.ok = false;
    }
    return out;
}

LatticeShape ShapeOfLattice(const Mat3& h) {
    LatticeShape out;
    const double c0[3] = {h[0], h[3], h[6]}, c1[3] = {h[1], h[4], h[7]};
    const double n0 = std::sqrt(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]);
    const double n1 = std::sqrt(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]);
    if (!(n0 > 0) || !(n1 > 0) || !std::isfinite(n0) || !std::isfinite(n1)) return out;
    const double cosine = (c0[0] * c1[0] + c0[1] * c1[1] + c0[2] * c1[2]) / (n0 * n1);
    out.aspect_ratio = n1 / n0;
    out.skew_deg = std::asin(std::clamp(cosine, -1.0, 1.0)) / kDegrees;
    return out;
}

// ---- the uncertainty evidence ---------------------------------------------------------------------------------------

double FormalTiltSigma(const LatticeParameters& p, const PreparedPoints& points, double focal, const render::CameraProjection& profile,
    const GridSheetDetectionSettings& s) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double gate = s.final_gate_px;
    const Assignment a = AssignPoints(p, points, focal, s.direction_match_deg, gate);
    std::vector<std::uint32_t> idx;
    std::vector<std::uint8_t> fam;
    std::vector<std::int32_t> k;
    std::vector<double> r0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!a.ok[i]) continue;
        idx.push_back(static_cast<std::uint32_t>(i)); fam.push_back(a.family[i]); k.push_back(a.index[i]); r0.push_back(a.distance_px[i]);
    }
    if (idx.size() < 9) return nan;
    // The weights of the fit's last step (GaussNewtonStep).
    const double c = std::min(std::max(3.0 * ScaleOf(r0), 0.3), gate);
    std::array<std::array<double, 8>, 8> normal{};
    double weighted_sq = 0, weight_sum = 0;
    constexpr std::size_t kChunk = 50000;   // bounds the Jacobian's memory
    std::vector<std::uint32_t> ci;
    std::vector<std::uint8_t> cf;
    std::vector<std::int32_t> ck;
    std::vector<double> r, jac;
    for (std::size_t begin = 0; begin < idx.size(); begin += kChunk) {
        const std::size_t end = std::min(idx.size(), begin + kChunk);
        ci.assign(idx.begin() + static_cast<std::ptrdiff_t>(begin), idx.begin() + static_cast<std::ptrdiff_t>(end));
        cf.assign(fam.begin() + static_cast<std::ptrdiff_t>(begin), fam.begin() + static_cast<std::ptrdiff_t>(end));
        ck.assign(k.begin() + static_cast<std::ptrdiff_t>(begin), k.begin() + static_cast<std::ptrdiff_t>(end));
        LatticeResiduals(p, points, ci, cf, ck, focal, r, &jac);
        for (std::size_t m = 0; m < ci.size(); ++m) {
            const double w = Tukey(r0[begin + m], c);
            if (w <= 0) continue;
            weighted_sq += w * r[m] * r[m];
            weight_sum += w;
            const double* row = jac.data() + m * 8;
            for (int i = 0; i < 8; ++i) for (int j = i; j < 8; ++j) normal[i][j] += w * row[i] * row[j];
        }
    }
    if (!(weight_sum > 9)) return nan;
    for (int i = 0; i < 8; ++i) for (int j = 0; j < i; ++j) normal[i][j] = normal[j][i];
    const double variance = weighted_sq / (weight_sum - 8);
    // The tilt's gradient against the eight parameters, by central differences of the decomposition.
    std::array<double, 8> gradient{};
    for (int j = 0; j < 8; ++j) {
        const double h = 1e-6 * std::max(std::abs(p[static_cast<std::size_t>(j)]), 1e-3);
        auto pp = p, pm = p;
        pp[static_cast<std::size_t>(j)] += h; pm[static_cast<std::size_t>(j)] -= h;
        const auto op = OrientationOfLattice(profile, LatticeToPlane(pp)), om = OrientationOfLattice(profile, LatticeToPlane(pm));
        if (!op.ok || !om.ok) return nan;
        gradient[static_cast<std::size_t>(j)] = (op.tilt_deg - om.tilt_deg) / (2 * h);
    }
    // gradient^T N^-1 gradient, with the normal equations scaled to unit diagonal.
    std::array<double, 8> scale{};
    for (int i = 0; i < 8; ++i) scale[static_cast<std::size_t>(i)] = std::sqrt(normal[i][i]) + 1e-30;
    std::array<std::array<double, 8>, 8> scaled{};
    std::array<double, 8> scaled_gradient{};
    for (int i = 0; i < 8; ++i) {
        scaled_gradient[static_cast<std::size_t>(i)] = gradient[static_cast<std::size_t>(i)] / scale[static_cast<std::size_t>(i)];
        for (int j = 0; j < 8; ++j) scaled[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = normal[i][j] / (scale[static_cast<std::size_t>(i)] * scale[static_cast<std::size_t>(j)]);
    }
    std::array<double, 8> x{};
    if (!Solve<8>(scaled, scaled_gradient, x)) return nan;
    double quadratic = 0;
    for (std::size_t i = 0; i < 8; ++i) quadratic += scaled_gradient[i] * x[i];
    if (!(quadratic >= 0) || !std::isfinite(quadratic)) return nan;
    return std::sqrt(variance * quadratic);
}

double HalfSplitSpread(const LatticeParameters& p, const PreparedPoints& points, const std::vector<RidgePoint>& ridges, const Assignment& used,
    double focal, const render::CameraProjection& profile, const GridSheetDetectionSettings& s) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> xs, ys;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!used.ok[i]) continue;
        xs.push_back(ridges[points.source[i]].x); ys.push_back(ridges[points.source[i]].y);
    }
    if (xs.empty()) return nan;
    const double mid_x = Median(xs), mid_y = Median(ys);
    const auto lean_of = [&](auto inside) -> std::optional<render::Point> {
        std::vector<std::uint32_t> members;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto& r = ridges[points.source[i]];
            if (inside(r.x, r.y)) members.push_back(static_cast<std::uint32_t>(i));
        }
        const PreparedPoints sub = Subset(points, members, s.fit_max_points);
        LatticeParameters q = p;
        if (!FitStage(sub, focal, q, s.gates_px.back(), s).ok) return std::nullopt;
        const auto o = OrientationOfLattice(profile, LatticeToPlane(q));
        if (!o.ok) return std::nullopt;
        return o.lean_sheet_deg;
    };
    const auto left = lean_of([&](double x, double) { return x < mid_x; }), right = lean_of([&](double x, double) { return x >= mid_x; });
    const auto top = lean_of([&](double, double y) { return y < mid_y; }), bottom = lean_of([&](double, double y) { return y >= mid_y; });
    if (!left || !right || !top || !bottom) return nan;
    return std::max(std::hypot(left->x - right->x, left->y - right->y), std::hypot(top->x - bottom->x, top->y - bottom->y));
}

// ---- coverage and flatness ------------------------------------------------------------------------------------------

double VisibleFraction(const std::vector<RidgePoint>& ridges, const PreparedPoints& points, const Assignment& a,
    std::uint32_t width, std::uint32_t height, std::uint32_t cell_px, std::uint32_t min_points) {
    if (cell_px == 0) return 0;
    const std::size_t cols = (width + cell_px - 1) / cell_px, rows = (height + cell_px - 1) / cell_px;
    std::vector<std::uint32_t> count(cols * rows, 0);
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!a.ok[i]) continue;
        const auto& r = ridges[points.source[i]];
        const std::size_t cx = std::min<std::size_t>(cols - 1, static_cast<std::size_t>(std::max(0.0f, r.x)) / cell_px);
        const std::size_t cy = std::min<std::size_t>(rows - 1, static_cast<std::size_t>(std::max(0.0f, r.y)) / cell_px);
        ++count[cy * cols + cx];
    }
    std::size_t covered = 0;
    for (auto c : count) if (c >= min_points) ++covered;
    const double area = static_cast<double>(covered) * cell_px * cell_px;
    return std::min(1.0, area / (static_cast<double>(width) * height));
}

FlatnessCheck CheckFlatness(const LatticeParameters& p, const PreparedPoints& points, const std::vector<RidgePoint>& ridges,
    std::uint32_t width, std::uint32_t height, double focal, double offset_px, const GridSheetDetectionSettings& s) {
    FlatnessCheck out;
    const std::uint32_t across = std::max<std::uint32_t>(s.flat_cells_across, 1);
    const double side = static_cast<double>(width) / across;
    const std::size_t cols = across, rows = static_cast<std::size_t>(std::ceil(height / side));
    struct Cell {
        std::size_t count{};
        double zr{}, zi{}, pitch_sum{};
    };
    std::vector<Cell> cells(cols * rows * 2);
    const Mat3 g = GOf(p);
    const double sin_limit = std::sin(s.direction_match_deg * kDegrees);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double qx = points.qx[i], qy = points.qy[i];
        const double xx = g[0] * qx + g[1] * qy + g[2], yy = g[3] * qx + g[4] * qy + g[5], w = g[6] * qx + g[7] * qy + g[8];
        if (!(w > 1e-9)) continue;
        const double sx = xx / w, sy = yy / w;
        const double gx0 = (g[0] - sx * g[6]) / w * kLatticeScale, gx1 = (g[1] - sx * g[7]) / w * kLatticeScale;
        const double gy0 = (g[3] - sy * g[6]) / w * kLatticeScale, gy1 = (g[4] - sy * g[7]) / w * kLatticeScale;
        const double ngx = std::hypot(gx0, gx1), ngy = std::hypot(gy0, gy1);
        if (!(ngx > 0) || !(ngy > 0)) continue;
        const double s1 = std::abs(points.tx[i] * gx0 + points.ty[i] * gx1) / ngx;
        const double s2 = std::abs(points.tx[i] * gy0 + points.ty[i] * gy1) / ngy;
        if (std::min(s1, s2) >= sin_limit) continue;
        const bool first = s1 < s2;
        const double coordinate = (first ? sx : sy) * kLatticeScale;
        const auto& r = ridges[points.source[i]];
        const std::size_t cx = std::min<std::size_t>(cols - 1, static_cast<std::size_t>(std::max(0.0f, r.x) / side));
        const std::size_t cy = std::min<std::size_t>(rows - 1, static_cast<std::size_t>(std::max(0.0f, r.y) / side));
        Cell& cell = cells[(cy * cols + cx) * 2 + (first ? 0 : 1)];
        ++cell.count;
        cell.zr += std::cos(2 * kPi * coordinate); cell.zi += std::sin(2 * kPi * coordinate);
        cell.pitch_sum += focal / (first ? ngx : ngy);
    }
    std::size_t flagged = 0;
    for (const auto& cell : cells) {
        if (cell.count < s.flat_cell_min_points) continue;
        ++out.judged_cells;
        const double coherence = std::hypot(cell.zr, cell.zi) / static_cast<double>(cell.count);
        const double offset = std::atan2(cell.zi, cell.zr) / (2 * kPi) * (cell.pitch_sum / static_cast<double>(cell.count));
        if (coherence > s.flat_cell_coherence && std::abs(offset) > offset_px) ++flagged;
    }
    // No judged cell: the flatness is unknown, not shown (NaN fails every acceptance comparison).
    out.off_lattice_fraction = out.judged_cells ? static_cast<double>(flagged) / static_cast<double>(out.judged_cells) : std::numeric_limits<double>::quiet_NaN();
    return out;
}

}   // namespace a0::m2::experimental::grid_sheet_tilt::detail
