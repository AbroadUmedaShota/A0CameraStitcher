#include "a0/m2/experimental/parallax_correction.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <utility>

namespace a0::m2::experimental::parallax_correction {
namespace {
using render::Point;
using seam_gate::PointKind;

void Require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(std::string("parallax correction: ")+message);
}
bool Finite(double v) {return std::isfinite(v);}
bool Finite(Point p) {return std::isfinite(p.x) && std::isfinite(p.y);}
bool Positive(double v) {return std::isfinite(v) && v > 0;}

double SmoothStep(double t) {
    if (!(t > 0)) return 0;
    if (t >= 1) return 1;
    return t*t*(3-2*t);
}

// The directions of the enclosure check: 16 half planes through the hole centre, none along an axis (lattice points on
// the axes would sit on the boundary).
struct Directions {
    std::array<double,16> c{}, s{};
    Directions() {
        for (std::size_t k = 0; k < c.size(); ++k) {
            const double a = (static_cast<double>(k)+0.5)*std::numbers::pi/8;
            c[k] = std::cos(a);
            s[k] = std::sin(a);
        }
    }
};

// A pixel rectangle, half-open [x0, x1) x [y0, y1), as registration_gate::GatePatch, in doubles for the gaps.
struct Rect {
    double x0{}, y0{}, x1{}, y1{};
};
Rect RectOf(const registration_gate::GatePatch& p) {
    return {static_cast<double>(p.x0), static_cast<double>(p.y0), static_cast<double>(p.x1), static_cast<double>(p.y1)};
}

// A point as the regression sees it.
struct Observation {
    double x{}, y{};
    double z{};            // the component along the baseline (usable points)
    double sigma{};        // its uncertainty as used (floor included)
    Rect footprint;        // the output pixels its measurement read
    std::size_t point{};   // index in the request
};
bool ByRow(const Observation& a, const Observation& b) {
    if (a.y != b.y) return a.y < b.y;
    if (a.x != b.x) return a.x < b.x;
    return a.point < b.point;
}

struct Prepared {
    std::vector<Observation> usable;     // sorted by row, column, index
    std::vector<Observation> unusable;   // lines across the baseline; same order
    double footprint_above{};            // the largest y - footprint.y0 of a point
    double footprint_below{};            // the largest footprint.y1 - y of a point
    std::uint32_t excluded{};            // points left out for the verify rows (the split)
    Directions directions;
};

bool SplitRows(const ParallaxRows& g) {return g.verify_period_rows > 0;}
bool VerifyRow(const ParallaxRows& g, std::size_t row) {
    return !SplitRows(g) || row%g.verify_period_rows == g.verify_phase;
}
// The pixels any gate point of a gate row may read (x in [x_s - omega, x_s + omega], the gate's footprint at each).
Rect RowPixels(const ParallaxRows& g, std::size_t row) {
    const double y = static_cast<double>(g.first_row)+static_cast<double>(row)*g.row_step;
    const double xs = g.seam_column, omega = g.omega_half_width_px;
    const auto& f = g.gate_footprint;
    return {xs-omega+static_cast<double>(f.x0), y+static_cast<double>(f.y0), xs+omega+static_cast<double>(f.x1), y+static_cast<double>(f.y1)};
}
// Whether a footprint shares a pixel with a verify row's gate points, those widened by iota on every side.
bool TouchesVerifyRow(const Rect& f, const ParallaxRows& g, double iota) {
    if (!SplitRows(g)) return false;
    for (std::size_t row = g.verify_phase; row < g.row_count; row += g.verify_period_rows) {
        const Rect v = RowPixels(g, row);
        if (f.x0 < v.x1+iota && v.x0-iota < f.x1 && f.y0 < v.y1+iota && v.y0-iota < f.y1) return true;
    }
    return false;
}

Prepared Prepare(const ParallaxRequest& request) {
    const auto& d = request.design;
    const Point e = request.baseline.direction;
    Prepared p;
    for (std::size_t i = 0; i < request.points.size(); ++i) {
        const auto& point = request.points[i];
        Observation o;
        o.x = point.position.x;
        o.y = point.position.y;
        o.footprint = RectOf(point.footprint);
        o.point = i;
        // The split: a point that read a verify row's pixels is in no estimate (11.5).
        if (TouchesVerifyRow(o.footprint, request.rows, d.hole_margin_px)) { ++p.excluded; continue; }
        p.footprint_above = std::max(p.footprint_above, o.y-o.footprint.y0);
        p.footprint_below = std::max(p.footprint_below, o.footprint.y1-o.y);
        const double sigma = std::max(point.uncertainty_px, d.sigma_floor_px);
        if (point.kind == PointKind::two_directional) {
            o.z = e.x*point.displacement.x+e.y*point.displacement.y;
            o.sigma = sigma;
            p.usable.push_back(o);
            continue;
        }
        const double along = point.normal.x*e.x+point.normal.y*e.y;
        if (std::abs(along) >= d.minimum_baseline_component) {
            o.z = (point.normal.x*point.displacement.x+point.normal.y*point.displacement.y)/along;
            o.sigma = sigma/std::abs(along);
            p.usable.push_back(o);
        } else {
            p.unusable.push_back(o);
        }
    }
    std::sort(p.usable.begin(), p.usable.end(), ByRow);
    std::sort(p.unusable.begin(), p.unusable.end(), ByRow);
    return p;
}

// A centre of the regression: where q is predicted (the kernel's origin) and the pixels its prediction must not share with
// a point (the hole): for a gate row, the pixels every gate point of the row may read; for a point (the fold lines), its own.
struct Centre {
    double x{}, y{};
    Rect avoid;
    bool hole{true};   // false: a gate row of the split without a hole of its own (avoid is the centre; every S is 1)
};
// The gaps between an observation's footprint and the centre's pixels, per axis: the number of pixels between them, negative
// where they overlap along that axis. They share a pixel exactly when both gaps are negative (PatchesShareNoPixel).
struct Window {
    double gap{};                // max(gap_x, gap_y) - iota: negative in the hole, the hole's edge S rises from 0 at 0
    double dx{}, dy{};           // from the kernel's origin
    bool inside{};               // within the hole plus the kernel's reach on both axes
};
Window WindowOf(const Observation& o, const Centre& c, const ParallaxDesign& d) {
    Window w;
    if (!c.hole) {
        w.gap = std::numeric_limits<double>::infinity();
        w.dx = o.x-c.x;
        w.dy = o.y-c.y;
        w.inside = std::abs(w.dx) <= d.kernel_reach_sigmas*d.kernel_sigma_x_px && std::abs(w.dy) <= d.kernel_reach_sigmas*d.kernel_sigma_y_px;
        return w;
    }
    const double gap_x = std::max(c.avoid.x0-o.footprint.x1, o.footprint.x0-c.avoid.x1)-d.hole_margin_px;
    const double gap_y = std::max(c.avoid.y0-o.footprint.y1, o.footprint.y0-c.avoid.y1)-d.hole_margin_px;
    w.gap = std::max(gap_x, gap_y);
    w.dx = o.x-c.x;
    w.dy = o.y-c.y;
    w.inside = gap_x <= d.kernel_reach_sigmas*d.kernel_sigma_x_px && gap_y <= d.kernel_reach_sigmas*d.kernel_sigma_y_px;
    return w;
}
// The rows of the sorted observations that can be within the window of a centre.
std::pair<std::size_t,std::size_t> RowRange(const std::vector<Observation>& sorted, const Centre& c, const Prepared& p, const ParallaxDesign& d) {
    const double reach = d.hole_margin_px+d.kernel_reach_sigmas*d.kernel_sigma_y_px;
    const double low = c.avoid.y0-p.footprint_below-reach, high = c.avoid.y1+p.footprint_above+reach;
    const auto first = std::lower_bound(sorted.begin(), sorted.end(), low, [](const Observation& o, double y) {return o.y < y;});
    const auto last = std::upper_bound(first, sorted.end(), high, [](double y, const Observation& o) {return y < o.y;});
    return {static_cast<std::size_t>(first-sorted.begin()), static_cast<std::size_t>(last-sorted.begin())};
}

struct Candidate {
    std::size_t observation{};
    std::array<double,6> basis{};
    double z{};
    double hole{};
    double w0{};
};

// Solves m x m by Gaussian elimination with partial pivoting. false when singular.
bool Solve(std::array<std::array<double,6>,6> a, std::array<double,6> b, std::size_t m, std::array<double,6>& x) {
    double scale = 0;
    for (std::size_t i = 0; i < m; ++i) scale = std::max(scale, std::abs(a[i][i]));
    if (!(scale > 0) || !std::isfinite(scale)) return false;
    for (std::size_t c = 0; c < m; ++c) {
        std::size_t pivot = c;
        for (std::size_t r = c+1; r < m; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        if (!(std::abs(a[pivot][c]) > 1e-13*scale)) return false;
        std::swap(a[c], a[pivot]);
        std::swap(b[c], b[pivot]);
        for (std::size_t r = c+1; r < m; ++r) {
            const double f = a[r][c]/a[c][c];
            if (f == 0) continue;
            for (std::size_t j = c; j < m; ++j) a[r][j] -= f*a[c][j];
            b[r] -= f*b[c];
        }
    }
    for (std::size_t c = m; c-- > 0;) {
        double v = b[c];
        for (std::size_t j = c+1; j < m; ++j) v -= a[c][j]*x[j];
        x[c] = v/a[c][c];
        if (!std::isfinite(x[c])) return false;
    }
    return true;
}

struct Fit {
    bool measured{};
    std::string reason;
    double q{}, sigma_q{}, n_eff{}, enclosure{};
    double rms{};                         // the points' leftover against the fitted surface, weighted by the first weights
    std::uint32_t points{};
    std::vector<Candidate> candidates;   // kept for the diagnostics
    std::vector<double> weights;          // final, per candidate
};

// The weighted solve shared by the regression of 11.4 and the one along a line (C1): the candidates carry their basis in the
// kernel's own coordinates, their value, the hole's factor S and their first weight. At least one candidate has a weight.
void SolveCandidates(const Prepared& p, const ParallaxDesign& d, std::size_t m, const std::vector<Candidate>& candidates, bool keep, Fit& fit) {
    // Buffers reused by the calls on one thread (no allocation per fit; the values do not depend on them).
    thread_local std::vector<double> w, residual, magnitudes;
    const auto keep_candidates = [&](const std::vector<double>& weights) {
        if (!keep) return;
        fit.candidates = candidates;
        fit.weights = weights;
    };
    w.resize(candidates.size());
    residual.resize(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) w[i] = candidates[i].w0;
    std::array<double,6> coef{};
    std::array<std::array<double,6>,6> normal{};
    double sw = 0;
    for (std::uint32_t iteration = 0;; ++iteration) {
        normal = {};
        std::array<double,6> rhs{};
        sw = 0;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const double wi = w[i];
            if (!(wi > 0)) continue;
            const auto& b = candidates[i].basis;
            const double wz = wi*candidates[i].z;
            sw += wi;
            for (std::size_t r = 0; r < m; ++r) {
                rhs[r] += wz*b[r];
                const double wb = wi*b[r];
                for (std::size_t s = r; s < m; ++s) normal[r][s] += wb*b[s];
            }
        }
        if (!(sw > 0)) { keep_candidates(w); fit.reason = "too-few-points"; return; }
        for (std::size_t r = 0; r < m; ++r) for (std::size_t s = 0; s < r; ++s) normal[r][s] = normal[s][r];
        for (std::size_t r = 1; r < m; ++r) normal[r][r] += d.ridge_weight*sw;
        if (!Solve(normal, rhs, m, coef)) { keep_candidates(w); fit.reason = "ill-conditioned"; return; }
        if (iteration == d.reweighting_iterations) break;
        // Tukey: the scale is the median absolute residual of the points in reach, never below tau_min.
        magnitudes.clear();
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto& b = candidates[i].basis;
            double fitted = 0;
            for (std::size_t r = 0; r < m; ++r) fitted += b[r]*coef[r];
            residual[i] = candidates[i].z-fitted;
            if (candidates[i].w0 > 0) magnitudes.push_back(std::abs(residual[i]));
        }
        const auto middle = magnitudes.begin()+static_cast<std::ptrdiff_t>(magnitudes.size()/2);
        std::nth_element(magnitudes.begin(), middle, magnitudes.end());
        const double scale = std::max(1.4826*(*middle), d.scale_floor_px);
        const double limit = d.tukey_constant*scale;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const double t = residual[i]/limit;
            w[i] = std::abs(t) < 1 ? candidates[i].w0*(1-t*t)*(1-t*t) : 0.0;
        }
    }
    keep_candidates(w);
    fit.q = coef[0];
    double sw2 = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) if (w[i] > 0) { sw2 += w[i]*w[i]; ++fit.points; }
    fit.n_eff = sw*sw/sw2;
    // sigma_q: the constant term's uncertainty propagated from the points' (the sandwich M^-1 N M^-1 with the ridge in M).
    std::array<double,6> unit{1, 0, 0, 0, 0, 0}, column{};
    if (!Solve(normal, unit, m, column)) { fit.reason = "ill-conditioned"; return; }
    double variance = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!(w[i] > 0)) continue;
        double lever = 0;
        for (std::size_t r = 0; r < m; ++r) lever += candidates[i].basis[r]*column[r];
        const double sigma = p.usable[candidates[i].observation].sigma;
        variance += w[i]*w[i]*sigma*sigma*lever*lever;
    }
    fit.sigma_q = std::sqrt(variance);
    // Enclosure: the smallest share of the weight on one side of a line through the centre.
    std::array<double,16> shares{};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!(w[i] > 0)) continue;
        const auto& b = candidates[i].basis;
        for (std::size_t k = 0; k < shares.size(); ++k) if (b[1]*p.directions.c[k]+b[2]*p.directions.s[k] > 0) shares[k] += w[i];
    }
    double enclosure = 1;
    for (const double share : shares) enclosure = std::min(enclosure, share/sw);
    fit.enclosure = enclosure;
    // How well the fitted surface explains the points it was fitted to: the RMS of the leftover over every point with a first
    // weight (Tukey's zeros included, so a shape the polynomial cannot follow is not hidden by the reweighting).
    {
        double sum = 0, total = 0;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (!(candidates[i].w0 > 0)) continue;
            const auto& b = candidates[i].basis;
            double fitted = 0;
            for (std::size_t r = 0; r < m; ++r) fitted += b[r]*coef[r];
            const double e = candidates[i].z-fitted;
            sum += candidates[i].w0*e*e;
            total += candidates[i].w0;
        }
        fit.rms = std::sqrt(sum/total);
    }
    if (!std::isfinite(fit.q) || !std::isfinite(fit.sigma_q) || !std::isfinite(fit.n_eff)) fit.reason = "ill-conditioned";
    else if (fit.n_eff < d.minimum_effective_points) fit.reason = "too-few-points";
    else if (fit.enclosure < d.minimum_enclosure_share) fit.reason = "one-sided";
    else if (fit.sigma_q > d.maximum_sigma_q_px) fit.reason = "uncertain";
    else fit.measured = true;
}

// The weighted local polynomial at a centre (11.4). The same function serves the gate rows and, for the fold lines,
// the points themselves.
Fit FitAt(const Prepared& p, const ParallaxDesign& d, const Centre& c, bool keep) {
    Fit fit;
    const std::size_t m = ParallaxCoefficientCount(d);
    // A buffer reused by the calls on one thread (no allocation per fit; the values do not depend on it).
    thread_local std::vector<Candidate> candidates;
    candidates.clear();
    bool hole_only = false;
    {
        const auto [first, last] = RowRange(p.usable, c, p, d);
        for (std::size_t k = first; k < last; ++k) {
            const auto& o = p.usable[k];
            const auto window = WindowOf(o, c, d);
            if (!window.inside) continue;
            const double hole = window.gap <= 0 ? 0.0 : SmoothStep(window.gap/d.hole_edge_px);
            const double u = window.dx/d.kernel_sigma_x_px, v = window.dy/d.kernel_sigma_y_px;
            const double kernel = std::exp(-0.5*(u*u+v*v));
            if (kernel < d.kernel_floor) continue;
            Candidate cand;
            cand.observation = k;
            cand.z = o.z;
            cand.basis = m == 3 ? std::array<double,6>{1, u, v, 0, 0, 0} : std::array<double,6>{1, u, v, u*u, u*v, v*v};
            cand.hole = hole;
            cand.w0 = kernel*hole/(o.sigma*o.sigma);
            if (!(cand.w0 > 0)) hole_only = true;
            candidates.push_back(cand);
        }
    }
    std::size_t active = 0;
    for (const auto& cand : candidates) if (cand.w0 > 0) ++active;
    if (active == 0) {
        if (keep) { fit.candidates = candidates; fit.weights.assign(candidates.size(), 0.0); }
        if (hole_only) { fit.reason = "too-few-points"; return fit; }
        bool lines = false;
        const auto [first, last] = RowRange(p.unusable, c, p, d);
        for (std::size_t k = first; k < last && !lines; ++k) lines = WindowOf(p.unusable[k], c, d).inside;
        fit.reason = lines ? "baseline-unmeasured" : "no-points";
        return fit;
    }
    SolveCandidates(p, d, m, candidates, keep, fit);
    return fit;
}

// The regression along a line (C1, 11.26): the weights and the solve of 11.4, with the kernel and the polynomial in the
// coordinates along (s) and across (t) the line through the centre, sigma along and across the design's. The hole is the
// same factor S of the same pixels, so a point the row's gate may read never has a weight. Reasons as FitAt's, and in-hole
// when every point in reach is in the hole.
Fit FitAlongLine(const Prepared& p, const ParallaxDesign& d, const Centre& c, double angle_deg, bool keep) {
    Fit fit;
    const std::size_t m = ParallaxCoefficientCount(d);
    thread_local std::vector<Candidate> candidates;
    candidates.clear();
    const double theta = angle_deg*std::numbers::pi/180;
    const double ax = std::cos(theta), ay = std::sin(theta);
    const double sa = d.fold_kernel_sigma_along_px, st = d.fold_kernel_sigma_across_px;
    // The kernel's reach as a box around the centre (the ellipse of reach_sigmas along and across the line).
    const double half_x = d.kernel_reach_sigmas*std::hypot(sa*ax, st*ay), half_y = d.kernel_reach_sigmas*std::hypot(sa*ay, st*ax);
    const auto first = std::lower_bound(p.usable.begin(), p.usable.end(), c.y-half_y, [](const Observation& o, double y) {return o.y < y;});
    const auto last = std::upper_bound(first, p.usable.end(), c.y+half_y, [](double y, const Observation& o) {return y < o.y;});
    bool hole_only = false;
    for (auto it = first; it != last; ++it) {
        const auto& o = *it;
        const double dx = o.x-c.x, dy = o.y-c.y;
        if (std::abs(dx) > half_x) continue;
        const double u = (dx*ax+dy*ay)/sa, v = (dy*ax-dx*ay)/st;
        const double kernel = std::exp(-0.5*(u*u+v*v));
        if (kernel < d.kernel_floor) continue;
        const double gap = WindowOf(o, c, d).gap;
        Candidate cand;
        cand.observation = static_cast<std::size_t>(it-p.usable.begin());
        cand.z = o.z;
        cand.basis = m == 3 ? std::array<double,6>{1, u, v, 0, 0, 0} : std::array<double,6>{1, u, v, u*u, u*v, v*v};
        cand.hole = gap <= 0 ? 0.0 : SmoothStep(gap/d.hole_edge_px);
        cand.w0 = kernel*cand.hole/(o.sigma*o.sigma);
        if (!(cand.w0 > 0)) hole_only = true;
        candidates.push_back(cand);
    }
    std::size_t active = 0;
    for (const auto& cand : candidates) if (cand.w0 > 0) ++active;
    if (active == 0) {
        if (keep) { fit.candidates = candidates; fit.weights.assign(candidates.size(), 0.0); }
        fit.reason = hole_only ? "in-hole" : "no-points";
        return fit;
    }
    SolveCandidates(p, d, m, candidates, keep, fit);
    return fit;
}

void ValidateRequest(const ParallaxRequest& r) {
    ValidateParallaxDesign(r.design);
    const auto& g = r.rows;
    Require(g.output_width > 0 && g.output_height > 0, "the output raster is empty");
    Require(g.seam_column < g.output_width, "the seam column is outside the output");
    Require(g.gate_footprint.x0 <= 0 && g.gate_footprint.x1 > 0 && g.gate_footprint.y0 <= 0 && g.gate_footprint.y1 > 0,
        "the gate footprint does not contain its point");
    Require(g.row_count >= 1 && g.row_step >= 1, "there are no gate rows");
    Require(static_cast<std::uint64_t>(g.first_row)+static_cast<std::uint64_t>(g.row_count-1)*g.row_step < g.output_height,
        "a gate row is outside the output");
    Require(r.design.weight_flat_px >= g.omega_half_width_px, "a is smaller than Omega's half width");
    Require(!SplitRows(g) || g.verify_phase < g.verify_period_rows, "the verify rows' phase is not below their period");
    const auto& b = r.baseline;
    Require(Finite(b.direction) && std::abs(std::hypot(b.direction.x, b.direction.y)-1) <= 1e-9, "the baseline direction is not a unit vector");
    Require(Positive(b.k_px_per_mm) && Positive(b.pixels_per_mm) && Positive(b.length_mm) && Positive(b.camera_a_height_mm) &&
        Positive(b.camera_b_height_mm) && Finite(b.paper_height_mm), "the baseline is not set");
    if (r.max_lift_mm) {
        Require(Positive(*r.max_lift_mm), "the lift line is not a positive finite number");
        Require(r.apply_beyond_line.has_value(), "the switch for amounts beyond the line is not set");
    }
    Require(r.thread_count >= 1 && r.thread_count <= 256, "thread_count is outside 1..256");
    for (const auto& p : r.points) {
        Require(Finite(p.position) && Finite(p.displacement), "a point is not finite");
        Require(Positive(p.uncertainty_px), "a point's uncertainty is not a positive finite number");
        Require(static_cast<double>(p.footprint.x0) <= p.position.x && p.position.x < static_cast<double>(p.footprint.x1) &&
            static_cast<double>(p.footprint.y0) <= p.position.y && p.position.y < static_cast<double>(p.footprint.y1),
            "a point's footprint does not contain the point");
        if (p.kind == PointKind::one_directional)
            Require(Finite(p.normal) && std::abs(std::hypot(p.normal.x, p.normal.y)-1) <= 1e-6, "a one-directional point's normal is not a unit vector");
    }
}

ParallaxResult EmptyResult(const ParallaxRequest& r) {
    ParallaxResult result;
    result.baseline = r.baseline.direction;
    result.k_px_per_mm = r.baseline.k_px_per_mm;
    result.baseline_geometry = r.baseline;
    result.design = r.design;
    result.seam_column = r.rows.seam_column;
    result.first_row = r.rows.first_row;
    result.row_step = r.rows.row_step;
    result.verify_period_rows = r.rows.verify_period_rows;
    result.verify_phase = r.rows.verify_phase;
    result.summary.points_input = static_cast<std::uint32_t>(r.points.size());
    // The hole for a point of the design's patch (not remeasured), away from the ends of the gate rows: the smallest |x - x_s|
    // on the row (right side) and |y - y_r| on the seam column (below) at which it shares no pixel. With the split, the pixels of
    // one verify row (the points left out around it); without it, the row's hole with the rows of the join.
    const auto point = PointFootprint(0, 0, r.design.point_patch_px);
    const auto& f = r.rows.gate_footprint;
    result.summary.hole_x_px = static_cast<double>(r.rows.omega_half_width_px)+static_cast<double>(f.x1-point.x0)+r.design.hole_margin_px;
    result.summary.hole_y_px = static_cast<double>(f.y1-point.y0)+r.design.hole_margin_px+
        (SplitRows(r.rows) ? 0.0 : static_cast<double>(HoleNeighbourRows(r.rows, r.design).below)*r.rows.row_step);
    if (r.max_lift_mm) result.lift.limit_mm = *r.max_lift_mm;
    return result;
}

// A gate row's centre: the pixels any gate point of the row may read (x in [x_s - omega, x_s + omega], the footprint at each),
// widened to the footprints of the gate rows that read a q this row's estimate enters (HoleNeighbourRows).
Centre RowCentre(const ParallaxRows& g, const ParallaxDesign& d, std::size_t row) {
    const auto y = [&](std::size_t i) {return static_cast<double>(g.first_row)+static_cast<double>(i)*g.row_step;};
    const auto rows = HoleNeighbourRows(g, d);
    const std::size_t low = row > rows.above ? row-rows.above : 0, high = std::min<std::size_t>(g.row_count-1, row+rows.below);
    const double xs = g.seam_column, omega = g.omega_half_width_px;
    const auto& f = g.gate_footprint;
    return {xs, y(row), {xs-omega+static_cast<double>(f.x0), y(low)+static_cast<double>(f.y0), xs+omega+static_cast<double>(f.x1), y(high)+static_cast<double>(f.y1)}};
}
// A gate row of the split: the kernel's origin and no hole (the points that read a verify row's pixels are left out already).
Centre SplitCentre(const ParallaxRows& g, std::size_t row) {
    const double y = static_cast<double>(g.first_row)+static_cast<double>(row)*g.row_step, xs = g.seam_column;
    return {xs, y, {xs, y, xs, y}, false};
}
// Whether the seam column's points enclose a gate row of the split along the seam: a point whose footprint meets the row's
// columns of Omega (RowPixels) within the kernel's reach above the row and one below it, or one on the row (within half the
// lattice's spacing). Otherwise the prediction at the row would be an extrapolation along the seam column.
bool EnclosedAlongSeam(const Prepared& p, const ParallaxRows& g, const ParallaxDesign& d, std::size_t row) {
    const auto c = SplitCentre(g, row);
    const Rect columns = RowPixels(g, row);
    const auto [first, last] = RowRange(p.usable, c, p, d);
    bool above = false, below = false;
    for (std::size_t k = first; k < last; ++k) {
        const auto& o = p.usable[k];
        if (!(o.footprint.x0 < columns.x1 && columns.x0 < o.footprint.x1)) continue;
        const auto w = WindowOf(o, c, d);
        if (!w.inside) continue;
        const double u = w.dx/d.kernel_sigma_x_px, v = w.dy/d.kernel_sigma_y_px;
        if (std::exp(-0.5*(u*u+v*v)) < d.kernel_floor) continue;
        if (std::abs(w.dy) < 0.5*d.column_spacing_px) return true;
        above = above || w.dy < 0;
        below = below || w.dy > 0;
        if (above && below) return true;
    }
    return false;
}
// The centre a gate row's estimate is made at: with the split, the row without a hole when the seam column's points enclose it,
// else (and without the split) the row with its hole.
Centre CentreOf(const Prepared& p, const ParallaxRequest& r, std::size_t row) {
    if (SplitRows(r.rows) && EnclosedAlongSeam(p, r.rows, r.design, row)) return SplitCentre(r.rows, row);
    return RowCentre(r.rows, r.design, row);
}

// The samples of a result on the gate rows, as the mapping takes them (no validation).
ParallaxProfile ProfileOf(const ParallaxResult& result) {
    ParallaxProfile profile;
    profile.baseline = result.baseline;
    profile.row_spacing_px = static_cast<double>(result.row_step);
    profile.first_row_px = static_cast<double>(result.first_row);
    profile.q_px.reserve(result.rows.size());
    for (const auto& row : result.rows) profile.q_px.push_back(row.q);
    profile.a_px = result.design.weight_flat_px;
    profile.l_px = result.design.weight_falloff_px;
    profile.seam_column = result.seam_column;
    return profile;
}

// Runs work(t) for t in [0, threads) on that many threads and rethrows the first error in thread order.
template <class Work> void RunThreads(unsigned threads, const Work& work) {
    std::vector<std::exception_ptr> errors(threads);
    const auto guarded = [&](unsigned t) {
        try { work(t); } catch (...) { errors[t] = std::current_exception(); }
    };
    {
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(guarded, t);
        guarded(0);
        for (auto& th : pool) th.join();
    }
    for (const auto& e : errors) if (e) std::rethrow_exception(e);
}

// The fold and gutter lines (11.8). Every point's leftover against the prediction from its surroundings (the same
// regression, with the hole of its own patch); the points whose leftover is large are the candidates (FoldCandidates). They
// vote in a Hough accumulator of fixed angle and offset steps (no random choice); the strongest cell with enough points over
// l_min becomes a line, takes the points within w_f of it, and the search repeats (SearchFoldLines). The report's lines are
// the search over every candidate; a gate row with candidates in its hole is tried along the lines of the search over the
// candidates outside its hole (11.5, C1 of 11.26).
struct FoldCandidate {
    double x{}, y{}, c{};
    std::size_t observation{};   // index in Prepared::usable
};
std::vector<FoldCandidate> FoldCandidates(const Prepared& p, const ParallaxRequest& request, ParallaxSummary& summary) {
    const auto& d = request.design;
    const std::size_t n = p.usable.size();
    std::vector<double> leftover(n, 0.0);
    std::vector<std::uint8_t> valid(n, 0);
    const unsigned threads = static_cast<unsigned>(std::max<std::size_t>(1, std::min<std::size_t>(request.thread_count, n)));
    RunThreads(threads, [&](unsigned t) {
        for (std::size_t i = t; i < n; i += threads) {
            const auto& o = p.usable[i];
            const auto fit = FitAt(p, d, {o.x, o.y, o.footprint}, false);
            if (!fit.measured) continue;
            leftover[i] = o.z-fit.q;
            valid[i] = 1;
        }
    });
    std::vector<double> magnitudes;
    for (std::size_t i = 0; i < n; ++i) if (valid[i]) magnitudes.push_back(std::abs(leftover[i]));
    summary.points_cross_validated = static_cast<std::uint32_t>(magnitudes.size());
    if (magnitudes.empty()) return {};
    const auto middle = magnitudes.begin()+static_cast<std::ptrdiff_t>(magnitudes.size()/2);
    std::nth_element(magnitudes.begin(), middle, magnitudes.end());
    const double scale = std::max(1.4826*(*middle), d.scale_floor_px);
    summary.fold_residual_scale_px = scale;
    const double threshold = std::max(d.fold_residual_sigmas*scale, d.fold_minimum_residual_px);
    std::vector<FoldCandidate> candidates;
    for (std::size_t i = 0; i < n; ++i)
        if (valid[i] && std::abs(leftover[i]) > threshold) candidates.push_back({p.usable[i].x, p.usable[i].y, leftover[i], i});
    summary.fold_candidates = static_cast<std::uint32_t>(candidates.size());
    return candidates;
}
// The line search over the candidates but those left_out marks (empty: every candidate). supports gets, per line, the positions
// of the candidates that make it.
using LineSupports = std::vector<std::vector<Point>>;
std::vector<FoldLine> SearchFoldLines(const std::vector<FoldCandidate>& candidates, const std::vector<std::uint8_t>& left_out,
    const ParallaxRequest& request, LineSupports& supports) {
    supports.clear();
    const auto& d = request.design;
    const auto& g = request.rows;
    const Point origin{static_cast<double>(g.seam_column), g.first_row+0.5*static_cast<double>(g.row_count-1)*g.row_step};
    const auto angles = static_cast<std::size_t>(std::ceil(180.0/d.hough_angle_step_deg-1e-9));
    const double step = d.hough_offset_step_px;
    const double seam = g.seam_column;
    std::vector<std::uint8_t> taken = left_out.empty() ? std::vector<std::uint8_t>(candidates.size(), 0) : left_out;
    std::vector<FoldLine> lines;
    std::vector<long long> bins(candidates.size());
    // A cell is one offset step at one angle, for one sign of c (a ridge and the opposite leftover beside it are not one
    // line). Its score is the sum of |c| of its points, so the core of a fold outweighs a line that only grazes it.
    const auto positive = [&](std::size_t i) {return candidates[i].c > 0;};
    while (lines.size() < d.fold_maximum_lines) {
        // The strongest cell whose points (the cell and its two neighbours, same sign) reach l_min. Ties keep the smaller
        // angle, then the positive sign, then the smaller offset.
        double best_score = 0;
        std::size_t best_angle = 0;
        long long best_bin = 0;
        bool best_positive = false, found = false;
        for (std::size_t j = 0; j < angles; ++j) {
            const double theta = static_cast<double>(j)*d.hough_angle_step_deg*std::numbers::pi/180;
            const double s = std::sin(theta), c = std::cos(theta);
            long long low = 0, high = -1;
            bool any = false;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (taken[i]) continue;
                const double rho = -(candidates[i].x-origin.x)*s+(candidates[i].y-origin.y)*c;
                bins[i] = static_cast<long long>(std::floor(rho/step+0.5));
                if (!any) { low = high = bins[i]; any = true; }
                low = std::min(low, bins[i]);
                high = std::max(high, bins[i]);
            }
            if (!any) break;
            const auto size = static_cast<std::size_t>(high-low+1);
            std::vector<double> score(2*size, 0.0);
            std::vector<std::size_t> count(2*size, 0);
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (taken[i]) continue;
                const std::size_t at = (positive(i) ? size : 0)+static_cast<std::size_t>(bins[i]-low);
                score[at] += std::abs(candidates[i].c);
                ++count[at];
            }
            for (const bool sign : {true, false}) for (long long b = low; b <= high; ++b) {
                const std::size_t at = (sign ? size : 0)+static_cast<std::size_t>(b-low);
                // The points a line collects: the cell's and its two neighbours' (as its extent below).
                std::size_t collected = count[at];
                if (b > low) collected += count[at-1];
                if (b < high) collected += count[at+1];
                if (collected < d.fold_minimum_points || !(score[at] > best_score)) continue;
                double first = 0, last = 0;
                bool started = false;
                for (std::size_t i = 0; i < candidates.size(); ++i) {
                    if (taken[i] || positive(i) != sign || std::abs(bins[i]-b) > 1) continue;
                    const double along = (candidates[i].x-origin.x)*c+(candidates[i].y-origin.y)*s;
                    if (!started) { first = last = along; started = true; }
                    first = std::min(first, along);
                    last = std::max(last, along);
                }
                if (last-first < d.fold_minimum_length_px) continue;
                best_score = score[at]; best_angle = j; best_bin = b; best_positive = sign; found = true;
            }
        }
        if (!found) break;

        const double angle = static_cast<double>(best_angle)*d.hough_angle_step_deg;
        const double theta = angle*std::numbers::pi/180;
        const double s = std::sin(theta), c = std::cos(theta);
        std::vector<std::size_t> support;
        Point centroid{0, 0};
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (taken[i] || positive(i) != best_positive) continue;
            const double rho = -(candidates[i].x-origin.x)*s+(candidates[i].y-origin.y)*c;
            if (std::abs(static_cast<long long>(std::floor(rho/step+0.5))-best_bin) > 1) continue;
            support.push_back(i);
            centroid.x += candidates[i].x;
            centroid.y += candidates[i].y;
        }
        centroid.x /= static_cast<double>(support.size());
        centroid.y /= static_cast<double>(support.size());
        double t_min = 0, t_max = 0;
        std::vector<double> values;
        for (std::size_t k = 0; k < support.size(); ++k) {
            const auto& q = candidates[support[k]];
            const double t = (q.x-centroid.x)*c+(q.y-centroid.y)*s;
            if (k == 0) t_min = t_max = t;
            t_min = std::min(t_min, t);
            t_max = std::max(t_max, t);
            values.push_back(q.c);
        }
        std::sort(values.begin(), values.end());
        FoldLine line;
        line.kind = FoldLineKind::fold;
        line.angle_deg = angle;
        line.first = {centroid.x+t_min*c, centroid.y+t_min*s};
        line.last = {centroid.x+t_max*c, centroid.y+t_max*s};
        line.length_px = t_max-t_min;
        line.points = static_cast<std::uint32_t>(support.size());
        line.step_px = values[values.size()/2];
        line.step_mm = -line.step_px/request.baseline.k_px_per_mm;
        line.step_sign = line.step_mm > 0 ? 1 : line.step_mm < 0 ? -1 : 0;
        const double a = line.first.x-seam, b = line.last.x-seam;
        line.crosses_seam = a*b < 0;
        line.distance_from_seam_px = line.crosses_seam ? 0.0 : std::min(std::abs(a), std::abs(b));
        if (line.crosses_seam) line.position_px = line.first.y+(seam-line.first.x)*(line.last.y-line.first.y)/(line.last.x-line.first.x);
        else line.position_px = std::abs(a) <= std::abs(b) ? line.first.y : line.last.y;
        line.near_seam = line.distance_from_seam_px <= d.fold_near_seam_px;
        if (line.near_seam && std::abs(c) <= std::sqrt(0.5)+1e-12) {
            const double right = d.fold_near_seam_px-std::min(a, b), left = d.fold_near_seam_px+std::max(a, b);
            line.shift_to_clear_px = right <= left ? right : -left;
        }
        lines.push_back(line);
        supports.emplace_back();
        for (const auto i : support) supports.back().push_back({candidates[i].x, candidates[i].y});

        // The line takes its points and every candidate within w_f of it (the leftover beside a fold is part of it).
        const double band = std::max(d.fold_exclusion_half_width_px, 1.5*step);
        for (const auto i : support) taken[i] = 1;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (taken[i]) continue;
            const double across = -(candidates[i].x-centroid.x)*s+(candidates[i].y-centroid.y)*c;
            const double along = (candidates[i].x-centroid.x)*c+(candidates[i].y-centroid.y)*s;
            if (std::abs(across) <= band && along >= t_min-band && along <= t_max+band) taken[i] = 1;
        }
    }
    return lines;
}

// C1 of 11.26 for one gate row: what the found lines did to it.
struct AlongRow {
    ParallaxRowFold fold{ParallaxRowFold::none};
    std::string reason;
    std::optional<std::uint32_t> line;
    double angle_deg{};
    Fit fit;   // the regression along the line (rows tried along it)
};
double AngleGap(double a, double b) {
    const double gap = std::fmod(std::abs(a-b), 180.0);
    return std::min(gap, 180-gap);
}
// The distance from a point to a line's segment, and the point's coordinate along the line from its first end.
struct SegmentPlace {
    double distance{}, along{};
};
SegmentPlace PlaceOn(const FoldLine& line, Point c) {
    const double theta = line.angle_deg*std::numbers::pi/180, ax = std::cos(theta), ay = std::sin(theta);
    const double s = (c.x-line.first.x)*ax+(c.y-line.first.y)*ay, t = (c.y-line.first.y)*ax-(c.x-line.first.x)*ay;
    const double beyond = s < 0 ? -s : s > line.length_px ? s-line.length_px : 0.0;
    return {std::hypot(t, beyond), s};
}
// The row is tried along the nearest line whose segment comes within fold_row_half_width_px of the row's centre (ties keep
// the earlier line). The prediction is used when
//   - the line goes on past the row's hole on both sides along it: the lines at its angle (within one step of the line search)
//     that come as near the row have, on each side, at least half of fold_minimum_points of their candidates beyond the hole's
//     edge, to the lattice's spacing (a fold found in two pieces, as one off the search's angles that drifts across the seam
//     column, still counts; a few stray candidates on one side do not: a row's own search lacks the candidates in its hole,
//     which would show a fold ending there);
//   - the regression along it, at the line's angle or one within fold_angle_refinement_deg (the one whose surface explains
//     its points best), is the row's own estimate;
//   - it differs from 11.4's by more than fold_minimum_change_px (otherwise the row keeps 11.4's, bit for bit);
//   - its surface explains its points at least 1 / fold_maximum_rms_ratio times better than 11.4's explains its own.
// lines are the row's own: the search over the candidates outside the row's hole (RowLines), so which lines are near the
// row, their ends and their angles do not rest on a candidate in the hole either. The candidates themselves are chosen by
// every point's leftover, and a point's leftover reads the points around it (FoldCandidates): that is not redone per row.
AlongRow AlongLineRow(const Prepared& p, const ParallaxRequest& request, const std::vector<FoldLine>& lines, const LineSupports& supports,
    const ParallaxSummary& summary, const Centre& c, double q_isotropic, const Fit& isotropic) {
    const auto& d = request.design;
    const Point centre{c.x, c.y};
    AlongRow out;
    double best = 0;
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const double distance = PlaceOn(lines[l], centre).distance;
        if (distance > d.fold_row_half_width_px || (out.line && !(distance < best))) continue;
        out.line = static_cast<std::uint32_t>(l);
        best = distance;
    }
    if (!out.line) return out;
    const auto& line = lines[*out.line];
    out.angle_deg = line.angle_deg;
    const double theta = line.angle_deg*std::numbers::pi/180, ax = std::cos(theta), ay = std::sin(theta);
    // The hole's half extent along the line, from g_x and g_y.
    constexpr double infinite = std::numeric_limits<double>::infinity();
    const double reach = std::min(std::abs(ax) > 1e-12 ? summary.hole_x_px/std::abs(ax) : infinite,
        std::abs(ay) > 1e-12 ? summary.hole_y_px/std::abs(ay) : infinite);
    const std::size_t need = std::max<std::size_t>(1, d.fold_minimum_points/2);
    std::size_t before = 0, after = 0;
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const auto& other = lines[l];
        if (AngleGap(other.angle_deg, line.angle_deg) > d.hough_angle_step_deg || PlaceOn(other, centre).distance > d.fold_row_half_width_px) continue;
        for (const auto& s : supports[l]) {
            const double t = (s.x-c.x)*ax+(s.y-c.y)*ay;
            if (t <= -reach+d.column_spacing_px) ++before;
            if (t >= reach-d.column_spacing_px) ++after;
        }
    }
    if (before < need || after < need) {
        out.fold = ParallaxRowFold::unpredictable;
        out.reason = "line-ends";
        return out;
    }
    // The line's angle is known to the search's step: the regression at the angles around it, the best explained taken
    // (ties keep the line's own angle, then the smaller angle).
    const std::uint32_t steps = d.fold_angle_refinement_steps;
    std::optional<Fit> chosen;
    Fit first;
    for (std::uint32_t k = 0; k <= 2*steps; ++k) {
        // The order 0, -1, +1, -2, +2, ... so a tie keeps the angle nearest the line's.
        const int j = k == 0 ? 0 : (k % 2 == 1 ? -static_cast<int>((k+1)/2) : static_cast<int>(k/2));
        const double angle = line.angle_deg+(steps == 0 ? 0.0 : d.fold_angle_refinement_deg*j/steps);
        auto fit = FitAlongLine(p, d, c, angle, false);
        if (k == 0) first = fit;
        if (!fit.measured || (chosen && !(fit.rms < chosen->rms))) continue;
        chosen = std::move(fit);
        out.angle_deg = angle;
    }
    if (!chosen) {
        out.fold = ParallaxRowFold::unpredictable;
        out.reason = first.reason;
        out.fit = std::move(first);
        return out;
    }
    out.fit = std::move(*chosen);
    if (std::abs(out.fit.q-q_isotropic) <= d.fold_minimum_change_px) {
        out.fold = ParallaxRowFold::agrees;
        return out;
    }
    if (!(isotropic.rms > 0) || !(out.fit.rms <= d.fold_maximum_rms_ratio*isotropic.rms)) {
        out.fold = ParallaxRowFold::unpredictable;
        out.reason = "not-explained";
        return out;
    }
    out.fold = ParallaxRowFold::predicted;
    return out;
}

// A gate row's lines (11.5): the search over the candidates outside the row's hole (a candidate is in it when its footprint
// shares a pixel with the row's centre, as the weights' S = 0). A row with no candidate in its hole has the report's lines;
// only the rows with one search again, over the candidates the report's search had, from the same leftovers.
struct RowLines {
    bool own{};                    // the row searched again (lines below); otherwise the report's lines are the row's
    std::vector<FoldLine> lines;
    LineSupports supports;
};
RowLines LinesOfRow(const Prepared& p, const ParallaxRequest& request, const std::vector<FoldCandidate>& candidates, const Centre& c,
    std::vector<std::uint8_t>& in_hole) {
    in_hole.assign(candidates.size(), 0);
    bool any = false;
    for (std::size_t k = 0; k < candidates.size(); ++k) {
        if (!(WindowOf(p.usable[candidates[k].observation], c, request.design).gap <= 0)) continue;
        in_hole[k] = 1;
        any = true;
    }
    RowLines out;
    if (!any) return out;
    out.own = true;
    out.lines = SearchFoldLines(candidates, in_hole, request, out.supports);
    return out;
}
// The report's line a row searched again names (ParallaxRow::fold_line): the one nearest the row's own line in angle, then
// nearest the row's centre (ties keep the earlier line). Only the name: the row's q rests on its own line.
std::uint32_t ReportLineOf(const std::vector<FoldLine>& report, const FoldLine& own, Point centre) {
    std::uint32_t best = 0;
    double best_gap = 0, best_distance = 0;
    for (std::size_t l = 0; l < report.size(); ++l) {
        const double gap = AngleGap(report[l].angle_deg, own.angle_deg), distance = PlaceOn(report[l], centre).distance;
        if (l > 0 && (gap > best_gap || (gap == best_gap && !(distance < best_distance)))) continue;
        best = static_cast<std::uint32_t>(l);
        best_gap = gap;
        best_distance = distance;
    }
    return best;
}
}

std::size_t ParallaxCoefficientCount(const ParallaxDesign& design) {return design.polynomial_degree == 1 ? 3 : 6;}

void ValidateParallaxDesign(const ParallaxDesign& d) {
    Require(Positive(d.column_spacing_px) && Positive(d.column_radius_px) && d.point_patch_px >= 2, "the point lattice is not set");
    Require(d.polynomial_degree == 1 || d.polynomial_degree == 2, "the degree is not 1 or 2");
    Require(Positive(d.kernel_sigma_x_px) && Positive(d.kernel_sigma_y_px) && Positive(d.kernel_reach_sigmas), "the kernel is not set");
    Require(Positive(d.kernel_floor) && d.kernel_floor < 1, "the kernel floor is not in (0, 1)");
    Require(Finite(d.hole_margin_px) && d.hole_margin_px >= 0 && Positive(d.hole_edge_px) && d.hole_neighbour_rows <= 64 &&
        d.hole_join_reach_rows <= 16, "the hole is not set");
    Require(Finite(d.ridge_weight) && d.ridge_weight >= 0, "the ridge weight is not set");
    Require(Positive(d.tukey_constant) && Positive(d.scale_floor_px) && d.reweighting_iterations <= 20, "the reweighting is not set");
    Require(Positive(d.sigma_floor_px), "the point floor is not set");
    Require(Positive(d.minimum_baseline_component) && d.minimum_baseline_component <= 1, "c_min is not in (0, 1]");
    Require(Finite(d.minimum_effective_points) && d.minimum_effective_points >= static_cast<double>(ParallaxCoefficientCount(d)),
        "n_min is below the number of coefficients");
    Require(Positive(d.maximum_sigma_q_px), "sigma_max is not set");
    Require(Finite(d.minimum_enclosure_share) && d.minimum_enclosure_share >= 0 && d.minimum_enclosure_share <= 0.5,
        "the enclosure share is not in [0, 0.5]");
    Require(Positive(d.row_blend_px), "delta_y is not set");
    Require(Positive(d.weight_flat_px) && Positive(d.weight_falloff_px) && Positive(d.maximum_strain), "the weight of the mapping is not set");
    Require(Positive(d.fold_minimum_length_px) && Positive(d.fold_exclusion_half_width_px) && Positive(d.fold_near_seam_px),
        "the fold lines are not set");
    Require(Positive(d.hough_angle_step_deg) && d.hough_angle_step_deg <= 45, "the angle step of the line search is not in (0, 45]");
    Require(Positive(d.hough_offset_step_px) && Positive(d.fold_residual_sigmas) && Finite(d.fold_minimum_residual_px) &&
        d.fold_minimum_residual_px >= 0, "the candidates of the line search are not set");
    Require(d.fold_minimum_points >= 2 && d.fold_maximum_lines >= 1 && d.fold_maximum_lines <= 64, "the counts of the line search are not set");
    Require(Positive(d.fold_kernel_sigma_along_px) && Positive(d.fold_kernel_sigma_across_px), "the kernel along a line is not set");
    Require(Finite(d.fold_row_half_width_px) && d.fold_row_half_width_px >= 0 && Finite(d.fold_minimum_change_px) && d.fold_minimum_change_px >= 0 &&
        Positive(d.fold_maximum_rms_ratio), "the rows predicted along a line are not set");
    Require(Finite(d.fold_angle_refinement_deg) && d.fold_angle_refinement_deg >= 0 && d.fold_angle_refinement_deg <= 45 &&
        d.fold_angle_refinement_steps <= 16, "the angles tried around a line are not set");
}

std::vector<ParallaxPoint> PointsFromSeamMeasurement(const seam_gate::SeamMeasurement& measurement) {
    std::vector<ParallaxPoint> points;
    for (const auto& p : measurement.points) {
        if (!p.Counted()) continue;
        ParallaxPoint point;
        point.position = {static_cast<double>(p.x), static_cast<double>(p.y)};
        point.displacement = p.displacement;
        point.uncertainty_px = p.uncertainty_px;
        point.kind = p.kind;
        point.normal = p.normal;
        point.footprint = registration_gate::GatePatchOf(p, measurement.design);
        points.push_back(point);
    }
    return points;
}

registration_gate::GatePatch PointFootprint(std::uint32_t x, std::uint32_t y, std::uint32_t patch_px) {
    Require(patch_px >= 2, "the patch side is not set");
    seam_gate::SeamGateDesign design{};
    design.patch_px = patch_px;
    seam_gate::SeamPoint point;
    point.x = x;
    point.y = y;
    return registration_gate::GatePatchOf(point, design);
}

HoleRows HoleNeighbourRows(const ParallaxRows& rows, const ParallaxDesign& design) {
    const auto& f = rows.gate_footprint;
    Require(f.y0 <= 0 && f.y1 > 0, "the gate footprint does not contain its point");
    Require(rows.row_step >= 1, "the row step is 0");
    const std::uint64_t step = rows.row_step;
    const auto steps = [step](std::int64_t pixels) {return (static_cast<std::uint64_t>(pixels)+step-1)/step;};
    HoleRows out;
    if (design.hole_join_reach_rows > 0) {
        const std::uint64_t r = design.hole_join_reach_rows;
        out.above = static_cast<std::uint32_t>(std::min<std::uint64_t>(r-1+steps(f.y1-1), UINT32_MAX));
        out.below = static_cast<std::uint32_t>(std::min<std::uint64_t>(r-1+steps(-f.y0), UINT32_MAX));
    }
    out.above = std::max(out.above, design.hole_neighbour_rows);
    out.below = std::max(out.below, design.hole_neighbour_rows);
    return out;
}

bool IsVerifyRow(const ParallaxRows& rows, std::size_t row) {
    Require(!SplitRows(rows) || rows.verify_phase < rows.verify_period_rows, "the verify rows' phase is not below their period");
    return VerifyRow(rows, row);
}

registration_gate::GatePatch VerifyRowPixels(const ParallaxRows& rows, std::size_t row) {
    const auto r = RowPixels(rows, row);
    return {static_cast<std::int64_t>(r.x0), static_cast<std::int64_t>(r.y0), static_cast<std::int64_t>(r.x1), static_cast<std::int64_t>(r.y1)};
}

registration_gate::GatePatch GateFootprint(const seam_gate::SeamGateDesign& gate_design, bool remeasured) {
    seam_gate::ValidateSeamGateDesign(gate_design);
    seam_gate::SeamPoint point;
    if (remeasured) {
        seam_gate::Remeasurement shifted, enlarged;
        shifted.variant = seam_gate::Variant::shifted;
        enlarged.variant = seam_gate::Variant::enlarged;
        point.remeasurements = {shifted, enlarged};
    }
    return registration_gate::GatePatchOf(point, gate_design);
}

BaselineGeometry MakeBaselineGeometry(const render::DocumentRenderParameters& calibrated, const content_registration::PoseParameters& pose,
    double paper_height_mm) {
    Require(Finite(paper_height_mm), "the paper height is not finite");
    Require(std::isfinite(pose.rx_deg) && std::isfinite(pose.ry_deg) && std::isfinite(pose.rz_deg) && std::isfinite(pose.tx_mm) &&
        std::isfinite(pose.ty_mm) && std::isfinite(pose.tz_percent), "the pose is not finite");
    // The centres the mapping's Pi(pi) is built with (registration_pose.hpp): A calibrated, B moved by the pose.
    const auto a = content_registration::PoseModel(calibrated.camera_a).Centre({});
    const auto b = content_registration::PoseModel(calibrated.camera_b).Centre(pose);
    const auto frame = content_registration::FrameOfRaster(calibrated.raster);
    BaselineGeometry baseline;
    const Point vector{b.x_mm-a.x_mm, b.y_mm-a.y_mm};
    baseline.length_mm = std::hypot(vector.x, vector.y);
    Require(Positive(baseline.length_mm), "the cameras have the same centre");
    baseline.direction = {vector.x/baseline.length_mm, vector.y/baseline.length_mm};
    baseline.camera_a_height_mm = a.height_mm;
    baseline.camera_b_height_mm = b.height_mm;
    baseline.paper_height_mm = paper_height_mm;
    Require(Positive(frame.pitch_mm), "the raster has no pitch");
    baseline.pixels_per_mm = 1.0/frame.pitch_mm;
    const double clearance = 0.5*(a.height_mm+b.height_mm)-paper_height_mm;
    Require(Positive(clearance), "the paper is not below the cameras");
    baseline.k_px_per_mm = baseline.pixels_per_mm*baseline.length_mm/clearance;
    Require(Positive(baseline.k_px_per_mm), "k is not finite");
    return baseline;
}

void FillUnmeasuredRows(std::vector<double>& q, const std::vector<bool>& measured, double row_step_px, double row_blend_px) {
    Require(q.size() == measured.size(), "the rows and their flags differ in size");
    Require(Positive(row_step_px) && Positive(row_blend_px), "the row spacing or delta_y is not set");
    const std::size_t n = q.size();
    constexpr std::size_t none = static_cast<std::size_t>(-1);
    std::vector<std::size_t> previous(n, none), next(n, none);
    for (std::size_t r = 0, last = none; r < n; ++r) { previous[r] = last; if (measured[r]) last = r; }
    for (std::size_t r = n, last = none; r-- > 0;) { next[r] = last; if (measured[r]) last = r; }
    const auto fade = [row_blend_px](double distance) {return 1-SmoothStep(distance/row_blend_px);};
    for (std::size_t r = 0; r < n; ++r) {
        if (measured[r]) continue;
        const std::size_t a = previous[r], b = next[r];
        const double da = a == none ? 0 : static_cast<double>(r-a)*row_step_px;
        const double db = b == none ? 0 : static_cast<double>(b-r)*row_step_px;
        if (a != none && b != none && da+db <= 2*row_blend_px) {
            const double t = da/(da+db);
            q[r] = (1-t)*q[a]+t*q[b];
            continue;
        }
        double v = 0;
        if (a != none) v += q[a]*fade(da);
        if (b != none) v += q[b]*fade(db);
        q[r] = v;
    }
}

ParallaxResult UnestimatedParallax(ParallaxState state, std::string reason) {
    Require(state != ParallaxState::applied, "an unestimated result cannot be applied");
    Require(!reason.empty(), "the reason is empty");
    ParallaxResult result;
    result.state = state;
    result.reasons.push_back(std::move(reason));
    return result;
}

ParallaxProfile ToParallaxProfile(const ParallaxResult& result) {
    Require(result.Applied(), "only an applied result goes into the mapping");
    Require(!result.rows.empty() && result.row_step > 0, "the result has no rows");
    auto profile = ProfileOf(result);
    ValidateParallaxProfile(profile);
    return profile;
}

std::vector<ParallaxPointWeight> ParallaxRowWeights(const ParallaxRequest& request, std::size_t row) {
    ValidateRequest(request);
    Require(row < request.rows.row_count, "the row is outside the gate rows");
    const auto prepared = Prepare(request);
    const auto fit = FitAt(prepared, request.design, CentreOf(prepared, request, row), true);
    std::vector<ParallaxPointWeight> out;
    out.reserve(fit.candidates.size());
    for (std::size_t i = 0; i < fit.candidates.size(); ++i)
        out.push_back({prepared.usable[fit.candidates[i].observation].point, fit.candidates[i].hole, fit.weights[i]});
    return out;
}

ParallaxRowFit ParallaxRowFitOf(const ParallaxRequest& request, std::size_t row, std::optional<double> angle_deg) {
    ValidateRequest(request);
    Require(row < request.rows.row_count, "the row is outside the gate rows");
    Require(!angle_deg || Finite(*angle_deg), "the angle is not finite");
    const auto prepared = Prepare(request);
    const auto centre = CentreOf(prepared, request, row);
    const auto fit = angle_deg ? FitAlongLine(prepared, request.design, centre, *angle_deg, true) : FitAt(prepared, request.design, centre, true);
    ParallaxRowFit out;
    out.measured = fit.measured;
    out.reason = fit.reason;
    out.q = fit.q;
    out.sigma_q = fit.sigma_q;
    out.n_eff = fit.n_eff;
    out.enclosure = fit.enclosure;
    out.rms = fit.rms;
    out.points = fit.points;
    out.weights.reserve(fit.candidates.size());
    for (std::size_t i = 0; i < fit.candidates.size(); ++i)
        out.weights.push_back({prepared.usable[fit.candidates[i].observation].point, fit.candidates[i].hole, fit.weights[i]});
    return out;
}

ParallaxResult EstimateParallax(const ParallaxRequest& request) {
    ValidateRequest(request);
    auto result = EmptyResult(request);
    if (!request.max_lift_mm) {
        result.state = ParallaxState::disabled;
        result.reasons = {"disabled"};
        return result;
    }
    const auto& d = request.design;
    const auto& g = request.rows;
    const auto prepared = Prepare(request);
    result.summary.points_baseline = static_cast<std::uint32_t>(prepared.usable.size());
    result.summary.points_excluded = prepared.excluded;

    // Every gate row on its own (rows are independent, so the thread count does not change a bit), at its centre (CentreOf).
    const std::size_t rows = g.row_count;
    std::vector<Centre> centres;
    centres.reserve(rows);
    for (std::size_t r = 0; r < rows; ++r) centres.push_back(CentreOf(prepared, request, r));
    std::vector<Fit> fits(rows);
    const unsigned threads = static_cast<unsigned>(std::min<std::size_t>(request.thread_count, rows));
    std::vector<std::vector<std::uint8_t>> used(threads, std::vector<std::uint8_t>(prepared.usable.size(), 0));
    std::vector<std::exception_ptr> errors(threads);
    const auto work = [&](unsigned t) {
        try {
            for (std::size_t r = t; r < rows; r += threads) {
                fits[r] = FitAt(prepared, d, centres[r], true);
                for (std::size_t i = 0; i < fits[r].candidates.size(); ++i)
                    if (fits[r].weights[i] > 0) used[t][fits[r].candidates[i].observation] = 1;
                fits[r].candidates.clear();
                fits[r].candidates.shrink_to_fit();
                fits[r].weights.clear();
                fits[r].weights.shrink_to_fit();
            }
        } catch (...) { errors[t] = std::current_exception(); }
    };
    {
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work, t);
        work(0);
        for (auto& th : pool) th.join();
    }
    for (const auto& e : errors) if (e) std::rethrow_exception(e);
    std::uint32_t points_used = 0;
    for (std::size_t i = 0; i < prepared.usable.size(); ++i) {
        bool any = false;
        for (const auto& flags : used) any = any || flags[i] != 0;
        if (any) ++points_used;
    }
    result.summary.points = points_used;

    std::vector<double> q(rows, 0.0);
    std::vector<bool> measured(rows, false);
    for (std::size_t r = 0; r < rows; ++r) if (fits[r].measured) { q[r] = fits[r].q; measured[r] = true; }
    // 11.4's rows alone (what S4 put into the mapping).
    std::vector<double> q_isotropic = q;
    FillUnmeasuredRows(q_isotropic, measured, static_cast<double>(g.row_step), d.row_blend_px);

    // The fold and gutter lines, whether or not the rows are applied (the warnings, S5 and C1 use them).
    std::vector<FoldCandidate> candidates;
    LineSupports supports;
    if (!prepared.usable.empty()) {
        candidates = FoldCandidates(prepared, request, result.summary);
        result.folds = SearchFoldLines(candidates, {}, request, supports);
    }

    // C1 of 11.26: the rows near a line, predicted along it. Each row on its own, from the points outside its hole, along the
    // lines of the candidates outside its hole (LinesOfRow; when the report's search finds no line, no row's search does).
    std::vector<AlongRow> along(rows);
    if (!result.folds.empty())
        RunThreads(threads, [&](unsigned t) {
            std::vector<std::uint8_t> in_hole;
            for (std::size_t r = t; r < rows; r += threads) {
                const auto& c = centres[r];
                const auto own = LinesOfRow(prepared, request, candidates, c, in_hole);
                if (!own.own) {
                    along[r] = AlongLineRow(prepared, request, result.folds, supports, result.summary, c, q_isotropic[r], fits[r]);
                    continue;
                }
                along[r] = AlongLineRow(prepared, request, own.lines, own.supports, result.summary, c, q_isotropic[r], fits[r]);
                if (along[r].line) along[r].line = ReportLineOf(result.folds, own.lines[*along[r].line], {c.x, c.y});
            }
        });
    for (std::size_t r = 0; r < rows; ++r) if (along[r].fold == ParallaxRowFold::predicted) { q[r] = along[r].fit.q; measured[r] = true; }
    FillUnmeasuredRows(q, measured, static_cast<double>(g.row_step), d.row_blend_px);

    const double k = request.baseline.k_px_per_mm;
    result.rows.resize(rows);
    for (std::size_t r = 0; r < rows; ++r) {
        auto& row = result.rows[r];
        row.y = centres[r].y;
        row.role = VerifyRow(g, r) ? ParallaxRowRole::verify : ParallaxRowRole::correct;
        row.hole = centres[r].hole;
        ++(row.role == ParallaxRowRole::verify ? result.summary.rows_verify : result.summary.rows_correct);
        if (row.hole) ++result.summary.rows_with_hole;
        row.q = q[r];
        row.q_isotropic = q_isotropic[r];
        row.lift_mm = -q[r]/k;
        const auto& fold = along[r];
        row.fold = fold.fold;
        row.fold_reason = fold.reason;
        row.fold_line = fold.line;
        row.fold_angle_deg = fold.angle_deg;
        const Fit& own = fold.fold == ParallaxRowFold::predicted ? fold.fit : fits[r];
        if (measured[r]) {
            row.state = ParallaxRowState::measured;
            row.sigma_q = own.sigma_q;
            row.n_eff = own.n_eff;
            row.points = own.points;
            ++result.summary.rows_measured;
        } else {
            row.state = ParallaxRowState::filled;
            row.reason = fits[r].reason;
            ++result.summary.rows_filled;
        }
        if (fold.line) {
            auto& line = result.folds[*fold.line];
            if (fold.fold == ParallaxRowFold::predicted) { ++line.rows_predicted; ++result.summary.rows_fold_predicted; }
            else if (fold.fold == ParallaxRowFold::agrees) { ++line.rows_agree; ++result.summary.rows_fold_agree; }
            else { ++line.rows_unpredictable; ++result.summary.rows_fold_unpredictable; }
        }
    }

    // The lift against the line, over measured rows (equal is inside).
    auto& lift = result.lift;
    for (const auto& row : result.rows) {
        if (row.state != ParallaxRowState::measured) continue;
        const double magnitude = std::abs(row.lift_mm);
        if (magnitude > lift.maximum_mm) { lift.maximum_mm = magnitude; lift.at_row = row.y; }
        if (magnitude > *request.max_lift_mm) {
            if (!lift.exceeded) lift.first_exceeded_row = row.y;
            lift.exceeded = true;
            lift.last_exceeded_row = row.y;
            ++lift.exceeded_rows;
        }
    }

    auto& summary = result.summary;
    for (const auto& row : result.rows) {
        if (std::abs(row.q) > summary.max_shift_px) { summary.max_shift_px = std::abs(row.q); summary.at_row = row.y; }
    }
    summary.max_shift_mm = summary.max_shift_px/request.baseline.pixels_per_mm;
    // The strain and the columns B moves in, with the mapping's own functions (fixed_composite_maps.hpp).
    summary.max_strain = ParallaxMaximumStrain(ProfileOf(result));
    {
        std::uint32_t reach = 0;
        while (ParallaxSeamWeight(static_cast<double>(reach)+1, d.weight_flat_px, d.weight_falloff_px) > 0) ++reach;
        summary.first_column = g.seam_column > reach ? g.seam_column-reach : 0;
        summary.last_column = static_cast<std::uint32_t>(std::min<std::uint64_t>(g.output_width-1, static_cast<std::uint64_t>(g.seam_column)+reach));
    }

    // Whether the rows go into the mapping.
    if (prepared.usable.empty()) result.reasons.push_back("baseline-unmeasured");
    else if (summary.rows_measured == 0) result.reasons.push_back("too-few-points");
    else {
        if (lift.exceeded && !*request.apply_beyond_line) result.reasons.push_back("beyond-lift-line");
        if (summary.max_strain > d.maximum_strain) result.reasons.push_back("strain-limit");
    }
    result.state = result.reasons.empty() ? ParallaxState::applied : ParallaxState::not_applied;
    for (auto& row : result.rows) row.applied = result.Applied();
    return result;
}
}
