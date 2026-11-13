#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace a0::m2::experimental::content_registration {
namespace {
using render::BgrImage;
using render::Matrix3;
using render::OutputToCamera;
using render::Point;
namespace gate = seam_gate;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kDegrees = std::numbers::pi/180.0;

void Require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

Matrix3 Mul(const Matrix3& a, const Matrix3& b) {
    Matrix3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        double s = 0;
        for (int k = 0; k < 3; ++k) s += a[3*i+k]*b[3*k+j];
        r[3*i+j] = s;
    }
    return r;
}
const Matrix3 kIdentity{1,0,0,0,1,0,0,0,1};

Point DocumentOf(const PlaneFrame& f, double x, double y) {
    return {f.left_mm+(x+0.5)*f.pitch_mm, f.top_mm+(y+0.5)*f.pitch_mm};
}
// Displacement (output px) of the document point at sample index (x, y) under a document homography.
Point DisplacementUnder(const Matrix3& m, const PlaneFrame& f, double x, double y) {
    const Point doc = DocumentOf(f, x, y);
    const double w = m[6]*doc.x+m[7]*doc.y+m[8];
    return {((m[0]*doc.x+m[1]*doc.y+m[2])/w-doc.x)/f.pitch_mm, ((m[3]*doc.x+m[4]*doc.y+m[5])/w-doc.y)/f.pitch_mm};
}

std::array<double,6> ToArray(const PoseParameters& p) { return {p.rx_deg, p.ry_deg, p.rz_deg, p.tx_mm, p.ty_mm, p.tz_percent}; }
PoseParameters ToPose(const std::array<double,6>& a) { return {a[0], a[1], a[2], a[3], a[4], a[5]}; }
PoseParameters Plus(const PoseParameters& a, const PoseParameters& b) {
    return {a.rx_deg+b.rx_deg, a.ry_deg+b.ry_deg, a.rz_deg+b.rz_deg, a.tx_mm+b.tx_mm, a.ty_mm+b.ty_mm, a.tz_percent+b.tz_percent};
}
PoseParameters Minus(const PoseParameters& a, const PoseParameters& b) {
    return {a.rx_deg-b.rx_deg, a.ry_deg-b.ry_deg, a.rz_deg-b.rz_deg, a.tx_mm-b.tx_mm, a.ty_mm-b.ty_mm, a.tz_percent-b.tz_percent};
}
bool SamePose(const PoseParameters& a, const PoseParameters& b) {
    return a.rx_deg == b.rx_deg && a.ry_deg == b.ry_deg && a.rz_deg == b.rz_deg && a.tx_mm == b.tx_mm && a.ty_mm == b.ty_mm && a.tz_percent == b.tz_percent;
}
// Typical size of each pose component; the fit works on parameter / scale, which keeps the normal equations balanced.
constexpr std::array<double,6> kScale{1.0, 1.0, 1.0, 10.0, 10.0, 1.0};
// The same for the eight components of the surface mode: rx ry rz s tz h0 g_x g_y.
constexpr std::array<double,8> kScaleSurface{1.0, 1.0, 1.0, 10.0, 1.0, 10.0, 0.05, 0.05};

// Dense solve by Gaussian elimination with partial pivoting.
bool Solve(std::vector<double> a, std::vector<double> b, unsigned n, std::vector<double>& x) {
    for (unsigned c = 0; c < n; ++c) {
        unsigned pivot = c;
        for (unsigned r = c+1; r < n; ++r) if (std::abs(a[r*n+c]) > std::abs(a[pivot*n+c])) pivot = r;
        if (!(std::abs(a[pivot*n+c]) > 1e-300)) return false;
        if (pivot != c) { for (unsigned k = 0; k < n; ++k) std::swap(a[c*n+k], a[pivot*n+k]); std::swap(b[c], b[pivot]); }
        for (unsigned r = c+1; r < n; ++r) {
            const double f = a[r*n+c]/a[c*n+c];
            for (unsigned k = c; k < n; ++k) a[r*n+k] -= f*a[c*n+k];
            b[r] -= f*b[c];
        }
    }
    x.assign(n, 0.0);
    for (int r = static_cast<int>(n)-1; r >= 0; --r) {
        double s = b[r];
        for (unsigned k = r+1; k < n; ++k) s -= a[r*n+k]*x[k];
        x[r] = s/a[r*n+r];
    }
    return true;
}
bool Invert(const std::vector<double>& a, unsigned n, std::vector<double>& inverse) {
    inverse.assign(static_cast<std::size_t>(n)*n, 0.0);
    for (unsigned c = 0; c < n; ++c) {
        std::vector<double> e(n, 0.0), x;
        e[c] = 1;
        if (!Solve(a, e, n, x)) return false;
        for (unsigned r = 0; r < n; ++r) inverse[r*n+c] = x[r];
    }
    return true;
}
// Eigenvalues of a small symmetric matrix by cyclic Jacobi rotations (a fixed number of sweeps).
std::vector<double> SymmetricEigenvalues(std::vector<double> a, unsigned n) {
    for (int sweep = 0; sweep < 60; ++sweep) {
        double off = 0;
        for (unsigned p = 0; p < n; ++p) for (unsigned q = p+1; q < n; ++q) off += a[p*n+q]*a[p*n+q];
        if (!(off > 1e-30)) break;
        for (unsigned p = 0; p < n; ++p) for (unsigned q = p+1; q < n; ++q) {
            if (!(std::abs(a[p*n+q]) > 1e-300)) continue;
            const double theta = (a[q*n+q]-a[p*n+p])/(2*a[p*n+q]);
            const double t = (theta >= 0 ? 1.0 : -1.0)/(std::abs(theta)+std::sqrt(theta*theta+1));
            const double c = 1/std::sqrt(t*t+1), s = t*c;
            for (unsigned k = 0; k < n; ++k) {
                const double akp = a[k*n+p], akq = a[k*n+q];
                a[k*n+p] = c*akp-s*akq; a[k*n+q] = s*akp+c*akq;
            }
            for (unsigned k = 0; k < n; ++k) {
                const double apk = a[p*n+k], aqk = a[q*n+k];
                a[p*n+k] = c*apk-s*aqk; a[q*n+k] = s*apk+c*aqk;
            }
        }
    }
    std::vector<double> values(n);
    for (unsigned i = 0; i < n; ++i) values[i] = a[i*n+i];
    return values;
}
// Condition number of a symmetric positive matrix after scaling it to a unit diagonal.
double ConditionNumber(const std::vector<double>& m, unsigned n) {
    std::vector<double> s(n), a(m.size());
    for (unsigned i = 0; i < n; ++i) { if (!(m[i*n+i] > 0)) return kInfinity; s[i] = 1/std::sqrt(m[i*n+i]); }
    for (unsigned i = 0; i < n; ++i) for (unsigned j = 0; j < n; ++j) a[i*n+j] = m[i*n+j]*s[i]*s[j];
    const auto values = SymmetricEigenvalues(a, n);
    const double lo = *std::min_element(values.begin(), values.end()), hi = *std::max_element(values.begin(), values.end());
    return lo > 0 ? hi/lo : kInfinity;
}

// Runs a on this thread and b on another when `parallel`, else one after the other; rethrows the first exception.
template <class A, class B>
void Both(bool parallel, A&& a, B&& b) {
    if (!parallel) { a(); b(); return; }
    std::exception_ptr error_b;
    std::thread worker([&]() { try { b(); } catch (...) { error_b = std::current_exception(); } });
    std::exception_ptr error_a;
    try { a(); } catch (...) { error_a = std::current_exception(); }
    worker.join();
    if (error_a) std::rethrow_exception(error_a);
    if (error_b) std::rethrow_exception(error_b);
}

// Calls work(index, scratch) for every index below `count`, index i on thread i % threads, each thread with its own
// scratch buffer of `scratch_size` values. The indices do not share state, so the result does not depend on the number
// of threads. Rethrows the first exception in thread order.
template <class F>
void ForEachIndex(unsigned threads, std::size_t count, std::size_t scratch_size, F&& work) {
    const auto used = static_cast<unsigned>(std::max<std::size_t>(1, std::min<std::size_t>(threads, count)));
    std::vector<std::exception_ptr> errors(used);
    const auto run = [&](unsigned t) {
        try {
            std::vector<double> scratch(scratch_size);
            for (std::size_t i = t; i < count; i += used) work(i, scratch);
        } catch (...) { errors[t] = std::current_exception(); }
    };
    std::vector<std::thread> pool;
    try {
        for (unsigned t = 1; t < used; ++t) pool.emplace_back(run, t);
    } catch (const std::system_error&) {
        // A thread could not start: its indices, and those of the ones after it, are done on this thread below.
    }
    for (unsigned t = static_cast<unsigned>(pool.size())+1; t < used; ++t) run(t);
    run(0);
    for (auto& th : pool) th.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
}

// ---- The paper surface (design 11.3) -----------------------------------------------------------------------------------
// Pi(pi), G(p) Pi(pi) and the camera centres are registration_pose's (S1). A centre here is its CameraCentre with the
// table point as a Point.
struct Centre { Point xy{}; double height{}; };
Centre CentreOf(const CameraCentre& c) { return {{c.x_mm, c.y_mm}, c.height_mm}; }
CameraCentre CameraCentreOf(const Centre& c) { return {c.xy.x, c.xy.y, c.height}; }
// The calibrated centre of a camera (the zero pose of its own plane matrix).
Centre CalibratedCentreOf(const render::CameraProjection& camera) { return CentreOf(PoseModel(camera).Centre({})); }

// B's state: the pose and the paper plane.
struct State {
    PoseParameters pose;
    SurfacePlane plane;
};

// The model of B's document point: G(p) Pi(pi) X, with the gauge of the surface mode.
struct Model {
    const PoseModel* pose_model{};
    Centre centre_a;
    Point reference_mm{};   // the plane origin (SurfaceOriginMm)
    bool surface{};
    PoseParameters base;
    bool base_zero{true};
    Point gauge{};   // unit (tx, ty) of the free translation (surface mode)

    // G(p) Pi(pi) with B's centre after the move (registration_pose's RegisteredHomography); without the surface, or for a
    // zero plane, G(p) itself (the pose-only arithmetic).
    Matrix3 Transform(const State& s) const {
        if (!surface || s.plane.IsZero()) return pose_model->Homography(s.pose);
        return RegisteredHomography(*pose_model, CameraCentreOf(centre_a), reference_mm, s.pose, s.plane);
    }
    Centre CentreB(const PoseParameters& p) const { return CentreOf(pose_model->Centre(p)); }
    bool Calibrated(const State& s) const { return s.pose.IsZero() && s.plane.IsZero(); }
    State Base() const { return {base, {}}; }
    PoseParameters AddBase(const PoseParameters& delta) const { return base_zero ? delta : Plus(base, delta); }
    PoseParameters FromBase(const PoseParameters& pose) const { return base_zero ? pose : Minus(pose, base); }
    unsigned Count() const { return surface ? 8u : 6u; }
    // The parameters are differences from the base: pose-only rx ry rz tx ty tz; surface rx ry rz s tz h0 g_x g_y.
    State StateOf(const std::vector<double>& p) const {
        if (!surface) return {AddBase({p[0], p[1], p[2], p[3], p[4], p[5]}), {}};
        return {AddBase({p[0], p[1], p[2], p[3]*gauge.x, p[3]*gauge.y, p[4]}), {p[5], p[6], p[7]}};
    }
    std::vector<double> ParametersOf(const State& s) const {
        const PoseParameters d = FromBase(s.pose);
        if (!surface) return {d.rx_deg, d.ry_deg, d.rz_deg, d.tx_mm, d.ty_mm, d.tz_percent};
        return {d.rx_deg, d.ry_deg, d.rz_deg, d.tx_mm*gauge.x+d.ty_mm*gauge.y, d.tz_percent, s.plane.h0_mm, s.plane.slope_x, s.plane.slope_y};
    }
};

// ---- Observations and the fit --------------------------------------------------------------------------------------------
// A displacement measured with B placed by some state: where B shows A's patch at (x, y), minus (x, y).
struct Obs {
    double x{}, y{};
    bool one{};          // only the component along n is known
    Point n{};
    Point d{};
    double sigma{};      // already includes the proximity factor
};
double ResidualMagnitude(const Obs& o, Point predicted) {
    const double rx = o.d.x-predicted.x, ry = o.d.y-predicted.y;
    return o.one ? std::abs(o.n.x*rx+o.n.y*ry) : std::hypot(rx, ry);
}

// What is fitted: n parameters in physical units, their typical sizes, the prior sigmas (prior centre 0) and the
// document homography of a parameter vector.
struct Parametrization {
    unsigned n{};
    std::vector<double> scale, prior_sigma;
    std::function<Matrix3(const std::vector<double>&)> homography;   // may throw std::invalid_argument
};

struct Fit {
    bool ok{}, converged{};
    std::vector<double> parameters;
    std::vector<double> covariance;   // n x n, physical units
    std::vector<double> information;  // n x n normal matrix of the scaled parameters, prior included
    std::vector<double> rho;          // final Tukey weights (1 for all when not robust)
    double scale{};
    std::uint32_t inliers{};
};

// Fits the parameters so that obs follow the displacement they cause when B is placed by the inverse of
// `place_inverse` (the identity for B at its calibrated pose): the prediction at index (x, y) is
// place_inverse T(parameters) applied to the document point, as a displacement. Deterministic. With six pose
// parameters the arithmetic is the one of the pose-only estimator, operation for operation.
Fit FitModel(const std::vector<Obs>& obs, const Parametrization& par, const PlaneFrame& frame, const Matrix3& place_inverse,
    const std::vector<double>& start, const RegistrationDesign& dz, bool robust) {
    const unsigned n = par.n;
    Fit fit;
    fit.rho.assign(obs.size(), 1.0);
    if (obs.empty()) return fit;

    const auto matrix_of = [&](const std::vector<double>& q, Matrix3& m) {
        std::vector<double> p(n);
        for (unsigned i = 0; i < n; ++i) p[i] = q[i]*par.scale[i];
        try { m = Mul(place_inverse, par.homography(p)); }
        catch (const std::invalid_argument&) { return false; }
        for (const double v : m) if (!std::isfinite(v)) return false;
        return true;
    };
    const auto prior_cost = [&](const std::vector<double>& q) {
        double c = 0;
        for (unsigned i = 0; i < n; ++i) { const double r = q[i]*par.scale[i]/par.prior_sigma[i]; c += r*r; }
        return c;
    };
    const auto data_cost = [&](const std::vector<double>& q) {
        Matrix3 m{};
        if (!matrix_of(q, m)) return kInfinity;
        double cost = 0;
        for (std::size_t i = 0; i < obs.size(); ++i) {
            if (fit.rho[i] <= 0) continue;
            const auto& o = obs[i];
            const Point pred = DisplacementUnder(m, frame, o.x, o.y);
            const double rx = o.d.x-pred.x, ry = o.d.y-pred.y;
            const double sq = o.one ? std::pow(o.n.x*rx+o.n.y*ry, 2) : rx*rx+ry*ry;
            cost += fit.rho[i]*sq/(o.sigma*o.sigma);
        }
        return std::isfinite(cost) ? cost : kInfinity;
    };
    // Normal equations and gradient at q; the Jacobian is by central differences (the model is smooth).
    const auto linearize = [&](const std::vector<double>& q, std::vector<double>& normal, std::vector<double>& gradient, double& cost) {
        constexpr double kStep = 1e-4;
        Matrix3 base{};
        std::vector<Matrix3> plus(n), minus(n);
        bool good = matrix_of(q, base);
        for (unsigned k = 0; k < n && good; ++k) {
            auto qp = q, qm = q;
            qp[k] += kStep; qm[k] -= kStep;
            good = matrix_of(qp, plus[k]) && matrix_of(qm, minus[k]);
        }
        normal.assign(static_cast<std::size_t>(n)*n, 0.0); gradient.assign(n, 0.0); cost = 0;
        if (!good) { cost = kInfinity; return false; }
        std::vector<double> j0(n), j1(n), jn(n);
        for (std::size_t i = 0; i < obs.size(); ++i) {
            if (fit.rho[i] <= 0) continue;
            const auto& o = obs[i];
            const double w = fit.rho[i]/(o.sigma*o.sigma);
            const Point pred = DisplacementUnder(base, frame, o.x, o.y);
            const double r[2]{o.d.x-pred.x, o.d.y-pred.y};
            for (unsigned k = 0; k < n; ++k) {
                const Point a = DisplacementUnder(plus[k], frame, o.x, o.y), b = DisplacementUnder(minus[k], frame, o.x, o.y);
                j0[k] = (a.x-b.x)/(2*kStep); j1[k] = (a.y-b.y)/(2*kStep);
            }
            if (o.one) {
                const double rn = o.n.x*r[0]+o.n.y*r[1];
                for (unsigned k = 0; k < n; ++k) jn[k] = o.n.x*j0[k]+o.n.y*j1[k];
                cost += w*rn*rn;
                for (unsigned a = 0; a < n; ++a) {
                    gradient[a] += w*jn[a]*rn;
                    for (unsigned b = 0; b < n; ++b) normal[a*n+b] += w*jn[a]*jn[b];
                }
            } else {
                cost += w*(r[0]*r[0]+r[1]*r[1]);
                for (unsigned a = 0; a < n; ++a) {
                    gradient[a] += w*(j0[a]*r[0]+j1[a]*r[1]);
                    for (unsigned b = 0; b < n; ++b) normal[a*n+b] += w*(j0[a]*j0[b]+j1[a]*j1[b]);
                }
            }
        }
        // The weak prior.
        for (unsigned i = 0; i < n; ++i) {
            const double s = par.scale[i]/par.prior_sigma[i];
            normal[i*n+i] += s*s;
            gradient[i] += -s*(q[i]*par.scale[i]/par.prior_sigma[i]);
        }
        return true;
    };

    std::vector<double> q(n);
    for (unsigned i = 0; i < n; ++i) q[i] = start[i]/par.scale[i];
    double mu = 1e-3;
    const auto run_inner = [&]() {
        bool settled = false;
        for (std::uint32_t it = 0; it < dz.maximum_inner_iterations; ++it) {
            std::vector<double> normal, gradient;
            double cost{};
            if (!linearize(q, normal, gradient, cost)) return false;
            const double total = cost+prior_cost(q);
            bool improved = false;
            for (int attempt = 0; attempt < 30; ++attempt) {
                auto damped = normal;
                for (unsigned k = 0; k < n; ++k) damped[k*n+k] += mu*std::max(normal[k*n+k], 1e-12);
                std::vector<double> step;
                if (!Solve(damped, gradient, n, step)) { mu *= 10; continue; }
                auto trial = q;
                double largest = 0;
                for (unsigned k = 0; k < n; ++k) { trial[k] += step[k]; largest = std::max(largest, std::abs(step[k])); }
                const double trial_total = data_cost(trial)+prior_cost(trial);
                if (std::isfinite(trial_total) && trial_total <= total) {
                    q = trial; mu = std::max(mu/3, 1e-12); improved = true;
                    if (largest < 1e-9 || total-trial_total <= 1e-13*std::max(total, 1.0)) settled = true;
                    break;
                }
                mu *= 4;
            }
            if (!improved) settled = true;
            if (settled) break;
        }
        return settled;
    };

    double scale = dz.scale_floor_px;
    bool converged = !robust;
    // Tukey weights from the residuals at q and their median absolute deviation; returns how much the weights changed.
    // The weights are first set at the starting point, before any step, so that gross outliers cannot pull the first
    // least-squares step.
    bool failed = false;
    const auto reweight = [&]() {
        Matrix3 m{};
        if (!matrix_of(q, m)) { failed = true; return 0.0; }
        std::vector<double> magnitude(obs.size());
        for (std::size_t i = 0; i < obs.size(); ++i) magnitude[i] = ResidualMagnitude(obs[i], DisplacementUnder(m, frame, obs[i].x, obs[i].y));
        auto sorted = magnitude;
        std::nth_element(sorted.begin(), sorted.begin()+sorted.size()/2, sorted.end());
        scale = std::max(1.4826*sorted[sorted.size()/2], dz.scale_floor_px);
        const double c = dz.tukey_constant*scale;
        double change = 0;
        for (std::size_t i = 0; i < obs.size(); ++i) {
            const double t = magnitude[i]/c;
            const double next = t < 1 ? std::pow(1-t*t, 2) : 0.0;
            change = std::max(change, std::abs(next-fit.rho[i]));
            fit.rho[i] = next;
        }
        return change;
    };
    if (robust) { reweight(); if (failed) return fit; }
    const std::uint32_t outer_limit = robust ? dz.maximum_reweighting_iterations : 1;
    for (std::uint32_t outer = 0; outer < outer_limit; ++outer) {
        const bool settled = run_inner();
        if (!robust) { converged = settled; break; }
        const double change = reweight();
        if (failed) return fit;
        if (change < 1e-6 && settled) { converged = true; break; }
    }
    // Final linearization with the final weights, for the covariance.
    std::vector<double> normal, gradient;
    double cost{};
    if (!linearize(q, normal, gradient, cost)) return fit;
    std::uint32_t inlier_rows = 0;
    for (std::size_t i = 0; i < obs.size(); ++i) if (fit.rho[i] > 0) { ++fit.inliers; inlier_rows += obs[i].one ? 1u : 2u; }
    std::vector<double> inverse;
    if (!Invert(normal, n, inverse)) return fit;
    const double s2 = inlier_rows > n ? std::max(1.0, cost/static_cast<double>(inlier_rows-n)) : 1.0;
    fit.covariance.assign(static_cast<std::size_t>(n)*n, 0.0);
    for (unsigned a = 0; a < n; ++a) for (unsigned b = 0; b < n; ++b) fit.covariance[a*n+b] = inverse[a*n+b]*s2*par.scale[a]*par.scale[b];
    fit.parameters.assign(n, 0.0);
    for (unsigned i = 0; i < n; ++i) fit.parameters[i] = q[i]*par.scale[i];
    fit.information = std::move(normal);
    fit.scale = scale;
    fit.converged = converged;
    fit.ok = true;
    return fit;
}

ResidualSummary Summarize(std::vector<double> values) {
    ResidualSummary s;
    s.count = static_cast<std::uint32_t>(values.size());
    if (values.empty()) return s;
    std::sort(values.begin(), values.end());
    double sum = 0, squares = 0;
    for (const double v : values) { sum += v; squares += v*v; }
    s.mean = sum/static_cast<double>(values.size());
    s.rms = std::sqrt(squares/static_cast<double>(values.size()));
    const auto rank = static_cast<std::size_t>(std::ceil(0.95*static_cast<double>(values.size())));
    s.p95 = values[std::max<std::size_t>(rank, 1)-1];
    s.maximum = values.back();
    return s;
}

// ---- The fit window (surface mode) -----------------------------------------------------------------------------------
// The largest mean residual of a bin of the core: the two-directional inliers within `core_left` / `core_right` of the
// seam column, in the bins of V5 (the side, and the thirds of the rows the core's points span).
double CoreBias(const std::vector<Obs>& obs, const Fit& fit, const Matrix3& matrix, const PlaneFrame& frame, double seam, double core_left,
    double core_right, std::uint32_t minimum_points) {
    const auto in_core = [&](const Obs& o) { return o.x < seam ? seam-o.x <= core_left : o.x-seam <= core_right; };
    double y_low = kInfinity, y_high = -kInfinity;
    for (std::size_t i = 0; i < obs.size(); ++i) if (fit.rho[i] > 0 && in_core(obs[i])) { y_low = std::min(y_low, obs[i].y); y_high = std::max(y_high, obs[i].y); }
    struct Bin { double sx{}, sy{}; std::uint32_t n{}; };
    Bin bins[6];
    for (std::size_t i = 0; i < obs.size(); ++i) {
        const auto& o = obs[i];
        if (!(fit.rho[i] > 0) || o.one || !in_core(o)) continue;
        const Point predicted = DisplacementUnder(matrix, frame, o.x, o.y);
        const double position = y_high > y_low ? (o.y-y_low)/(y_high-y_low) : 0.0;
        auto& bin = bins[(o.x < seam ? 0 : 3)+std::min(2, static_cast<int>(position*3))];
        bin.sx += o.d.x-predicted.x; bin.sy += o.d.y-predicted.y; ++bin.n;
    }
    double worst = 0;
    for (const auto& bin : bins) if (bin.n >= minimum_points) worst = std::max(worst, std::hypot(bin.sx/bin.n, bin.sy/bin.n));
    return worst;
}

struct GrownFit {
    Fit fit;                 // over `obs`
    std::vector<Obs> obs;    // the points the fit took, in measuring order
    FitWindow window;
};

// The fit over the part of W that is one plane with the seam's neighbourhood. The points lie on the measurer's columns.
// The core (the columns within window_core_px of Omega + guard, widened until it holds enough points) is fitted first.
// Then the next columns outward are offered, the nearer side first, in groups of at least window_group_points points. A
// group is refused, and its side ends there, when
//   - more than a quarter of its points are outliers of the current fit (beyond the Tukey bound of the core's robust
//     scale): the surface there is not the plane of the columns taken (a gutter, a fold, the edge of the sheet). On one
//     plane outliers are a few in a hundred; a robust fit would carry a group that is half off the plane without moving,
//     and the window would then run on across the fold with points it does not use; or
//   - the fit with the group leaves a mean residual in a bin of the core that is more than window_core_bias_px above
//     what the core alone leaves: the plane that takes the group no longer fits next to the seam (a page that bends
//     gently, which the first test lets through).
// Nothing beyond a refused group is used. No random choice; the order of the points is kept. With `held`, the window of
// an earlier measurement is used as it is (the outer loop keeps the window once the placement has settled, so that the
// change between two iterations is the change of the fit and not of the window).
GrownFit FitGrown(const std::vector<Obs>& all, const Parametrization& par, const PlaneFrame& frame, const Matrix3& place_inverse,
    const std::vector<double>& start, const RegistrationSettings& s, const FitWindow* held = nullptr) {
    const auto& dz = s.design;
    const double seam = s.geometry.seam_column;
    GrownFit out;
    out.window.grown = true;
    const auto side_of = [&](const Obs& o) { return o.x < seam ? 0 : 1; };
    std::vector<double> columns[2];   // distances from the seam column, ascending
    for (const auto& o : all) columns[side_of(o)].push_back(std::abs(o.x-seam));
    for (auto& c : columns) { std::sort(c.begin(), c.end()); c.erase(std::unique(c.begin(), c.end()), c.end()); }
    out.window.left_available_px = columns[0].empty() ? 0.0 : columns[0].back();
    out.window.right_available_px = columns[1].empty() ? 0.0 : columns[1].back();
    std::size_t taken[2]{};   // columns taken on each side
    const auto reach = [&](int side, std::size_t count) { return count ? columns[side][count-1] : -1.0; };
    const auto subset = [&](std::size_t left, std::size_t right) {
        std::vector<Obs> v;
        const double limit[2]{reach(0, left), reach(1, right)};
        for (const auto& o : all) if (std::abs(o.x-seam) <= limit[side_of(o)]) v.push_back(o);
        return v;
    };
    if (held) {
        // The window of an earlier measurement, kept: the same columns, fitted to the new values.
        const double limit[2]{held->left_px, held->right_px};
        for (int side = 0; side < 2; ++side) while (taken[side] < columns[side].size() && columns[side][taken[side]] <= limit[side]) ++taken[side];
        out.obs = subset(taken[0], taken[1]);
        out.fit = FitModel(out.obs, par, frame, place_inverse, start, dz, true);
        out.window = *held;
        return out;
    }
    // The side whose next column is nearer (the left on a tie); -1 when both are used up.
    const auto nearer = [&](const bool open[2]) {
        if (open[0] && open[1]) return columns[0][taken[0]] <= columns[1][taken[1]] ? 0 : 1;
        return open[0] ? 0 : open[1] ? 1 : -1;
    };
    const double core_limit = static_cast<double>(s.geometry.exclusion_half_width_px())+dz.window_core_px;
    for (int side = 0; side < 2; ++side) while (taken[side] < columns[side].size() && columns[side][taken[side]] <= core_limit) ++taken[side];
    bool open[2]{taken[0] < columns[0].size(), taken[1] < columns[1].size()};
    const auto widen = [&]() {
        const int side = nearer(open);
        if (side < 0) return false;
        ++taken[side];
        open[side] = taken[side] < columns[side].size();
        return true;
    };
    const std::size_t needed = std::max<std::size_t>(dz.minimum_points, 2*static_cast<std::size_t>(dz.window_group_points));
    auto current = subset(taken[0], taken[1]);
    while (current.size() < needed && widen()) current = subset(taken[0], taken[1]);
    Fit fit = FitModel(current, par, frame, place_inverse, start, dz, true);
    while (!fit.ok && widen()) {
        current = subset(taken[0], taken[1]);
        fit = FitModel(current, par, frame, place_inverse, start, dz, true);
    }
    if (!fit.ok) { out.fit = std::move(fit); out.obs = std::move(current); return out; }
    const double core[2]{reach(0, taken[0]), reach(1, taken[1])};
    out.window.core_points = static_cast<std::uint32_t>(current.size());
    // The two bounds of the growth come from the fit of the core alone and stay as they are, so that columns taken later
    // cannot widen them (a page that bends away a little more with every column would otherwise be followed all the way):
    // the Tukey bound of the core's robust scale, and what the core's bins show plus the design's allowance.
    const double outlier_bound = dz.tukey_constant*fit.scale;
    double allowed_bias = 0;
    {
        Matrix3 matrix{};
        try { matrix = Mul(place_inverse, par.homography(fit.parameters)); } catch (const std::invalid_argument&) { out.fit = Fit{}; out.obs = std::move(current); return out; }
        allowed_bias = CoreBias(current, fit, matrix, frame, seam, core[0], core[1], dz.bin_minimum_points)+dz.window_core_bias_px;
    }

    bool cut[2]{};
    for (;;) {
        const int side = nearer(open);
        if (side < 0) break;
        // The next group of the side: whole columns, until it holds enough points.
        std::size_t next = taken[side];
        std::vector<const Obs*> group;
        while (next < columns[side].size() && group.size() < dz.window_group_points) {
            for (const auto& o : all) if (side_of(o) == side && std::abs(o.x-seam) == columns[side][next]) group.push_back(&o);
            ++next;
        }
        Matrix3 matrix{};
        bool refused = false;
        try { matrix = Mul(place_inverse, par.homography(fit.parameters)); } catch (const std::invalid_argument&) { refused = true; }
        if (!refused) {
            std::size_t outliers = 0;
            for (const auto* o : group) if (ResidualMagnitude(*o, DisplacementUnder(matrix, frame, o->x, o->y)) > outlier_bound) ++outliers;
            refused = 4*outliers > group.size();
        }
        Fit trial;
        std::vector<Obs> trial_obs;
        if (!refused) {
            std::size_t count[2]{taken[0], taken[1]};
            count[side] = next;
            trial_obs = subset(count[0], count[1]);
            trial = FitModel(trial_obs, par, frame, place_inverse, fit.parameters, dz, true);
            refused = !trial.ok;
            if (!refused) {
                try { matrix = Mul(place_inverse, par.homography(trial.parameters)); } catch (const std::invalid_argument&) { refused = true; }
            }
            refused = refused || CoreBias(trial_obs, trial, matrix, frame, seam, core[0], core[1], dz.bin_minimum_points) > allowed_bias;
        }
        if (refused) { cut[side] = true; open[side] = false; continue; }
        taken[side] = next;
        open[side] = taken[side] < columns[side].size();
        fit = std::move(trial);
        current = std::move(trial_obs);
    }
    out.window.left_px = std::max(0.0, reach(0, taken[0])); out.window.right_px = std::max(0.0, reach(1, taken[1]));
    out.window.left_cut = cut[0]; out.window.right_cut = cut[1];
    out.window.refused_points = static_cast<std::uint32_t>(all.size()-current.size());
    out.fit = std::move(fit);
    out.obs = std::move(current);
    return out;
}

// ---- Windows --------------------------------------------------------------------------------------------------------
struct RowWindow { std::uint32_t left_begin{}, left_end{}, right_begin{}, right_end{}; };

struct Layout {
    std::uint32_t width{}, height{};
    PlaneFrame frame;
    std::vector<gate::BandRow> coverage;       // both cameras at the calibrated pose, no inset
    std::vector<RowWindow> window_coarse;      // W on the coverage
    std::vector<RowWindow> window_fine;        // W on the coverage shrunk by the measurer's inset
};

std::vector<RowWindow> MakeWindows(const std::vector<gate::BandRow>& coverage, const RegistrationGeometry& g) {
    const std::uint32_t seam = g.seam_column, exclusion = g.exclusion_half_width_px();
    std::vector<RowWindow> out(coverage.size());
    for (std::size_t y = 0; y < coverage.size(); ++y) {
        const auto row = coverage[y];
        if (row.begin >= row.end) continue;
        const auto band_begin = static_cast<std::uint32_t>(std::max<std::int64_t>(row.begin, static_cast<std::int64_t>(seam)-g.band_half_width_px));
        const std::uint32_t band_end = static_cast<std::uint32_t>(std::min<std::int64_t>(row.end, static_cast<std::int64_t>(seam)+g.band_half_width_px+1));
        if (band_begin >= band_end) continue;
        RowWindow w;
        // Left of Omega +- guard: x < seam - exclusion.  Right: x > seam + exclusion.
        const std::int64_t left_limit = static_cast<std::int64_t>(seam)-static_cast<std::int64_t>(exclusion);
        const std::int64_t right_limit = static_cast<std::int64_t>(seam)+static_cast<std::int64_t>(exclusion)+1;
        if (left_limit > band_begin) { w.left_begin = band_begin; w.left_end = static_cast<std::uint32_t>(std::min<std::int64_t>(left_limit, band_end)); }
        if (right_limit < band_end) { w.right_begin = static_cast<std::uint32_t>(std::max<std::int64_t>(right_limit, band_begin)); w.right_end = band_end; }
        out[y] = w;
    }
    return out;
}

// The rows of the overlap of both coverages, plain and narrowed by an inset. The arithmetic is seam_gate::OverlapBand's
// (per row a scan in steps of 8 for a covered pixel, then a walk to both ends; then every row narrowed to what its
// neighbours within the inset cover, less the inset), so each result is OverlapBand's for its inset. The walk, which is
// most of the work, is done once for both and its rows are dealt to the threads.
struct Coverage { std::vector<gate::BandRow> plain, inset; };
Coverage CoverageOf(const gate::SeamGateGeometry& g, std::uint32_t aw, std::uint32_t ah, std::uint32_t bw, std::uint32_t bh, std::uint32_t inset, unsigned threads) {
    Require(static_cast<bool>(g.camera_a) && static_cast<bool>(g.camera_b), "content registration: both output-to-camera mappings are required");
    (void)render::PixelCount(g.width, g.height);
    const auto inside = [](const OutputToCamera& m, double x, double y, std::uint32_t w, std::uint32_t h) {
        Point raw{};
        if (!m({x, y}, raw)) return false;
        Require(std::isfinite(raw.x) && std::isfinite(raw.y), "content registration: mapping returned nonfinite camera coordinates");
        return raw.x >= 0 && raw.y >= 0 && raw.x < w && raw.y < h;
    };
    const auto both = [&](std::uint32_t x, std::uint32_t y) { return inside(g.camera_a, x, y, aw, ah) && inside(g.camera_b, x, y, bw, bh); };
    std::vector<std::int64_t> first(g.height, -1), last(g.height, -1);
    ForEachIndex(threads, g.height, 0, [&](std::size_t row, std::vector<double>&) {
        const auto y = static_cast<std::uint32_t>(row);
        std::int64_t seed = -1;
        for (std::uint32_t x = 0; x < g.width && seed < 0; x += 8) if (both(x, y)) seed = x;
        if (seed < 0) return;
        std::int64_t lo = seed; while (lo > 0 && both(static_cast<std::uint32_t>(lo-1), y)) --lo;
        std::int64_t hi = seed; while (hi+1 < g.width && both(static_cast<std::uint32_t>(hi+1), y)) ++hi;
        first[row] = lo; last[row] = hi;
    });
    const auto narrowed = [&](std::int64_t m) {
        std::vector<gate::BandRow> band(g.height);
        for (std::int64_t y = 0; y < g.height; ++y) {
            std::int64_t lo = 0, hi = g.width;
            bool ok = y-m >= 0 && y+m < g.height;
            for (std::int64_t v = y-m; ok && v <= y+m; ++v) {
                if (first[static_cast<std::size_t>(v)] < 0) { ok = false; break; }
                lo = std::max(lo, first[static_cast<std::size_t>(v)]); hi = std::min(hi, last[static_cast<std::size_t>(v)]);
            }
            lo += m; hi -= m;
            if (ok && lo <= hi) band[static_cast<std::size_t>(y)] = {static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(hi+1)};
        }
        return band;
    };
    return {narrowed(0), narrowed(inset)};
}

bool InWindow(const RowWindow& w, std::int64_t x) {
    return (x >= w.left_begin && x < w.left_end) || (x >= w.right_begin && x < w.right_end);
}

// ---- Stage 1: coarse -----------------------------------------------------------------------------------------------
struct Grid {
    std::int64_t x0{}, y0{};
    int cols{}, rows{};
    std::vector<float> energy;
    std::vector<char> valid;
    std::vector<float> colour;   // when asked for: the mean B, G, R of each cell (meaningful for valid cells)
    float At(int i, int j) const { return energy[static_cast<std::size_t>(j)*cols+i]; }
    const float* Colour(int i, int j) const { return colour.data()+(static_cast<std::size_t>(j)*cols+i)*3; }
    bool Valid(int i, int j) const { return i >= 0 && j >= 0 && i < cols && j < rows && valid[static_cast<std::size_t>(j)*cols+i]; }
};

bool SampleLuma(const BgrImage& image, Point raw, double& value) {
    if (!(raw.x >= 0 && raw.y >= 0 && raw.x < static_cast<double>(image.width)-1 && raw.y < static_cast<double>(image.height)-1)) return false;
    const int ix = static_cast<int>(std::floor(raw.x)), iy = static_cast<int>(std::floor(raw.y));
    const double fx = raw.x-ix, fy = raw.y-iy;
    const auto at = [&](int x, int y) {
        const std::uint8_t* const p = image.bgr.data()+(static_cast<std::size_t>(y)*image.width+x)*3;
        return (p[0]+p[1]+p[2])/3.0;
    };
    value = (1-fy)*((1-fx)*at(ix, iy)+fx*at(ix+1, iy))+fy*((1-fx)*at(ix, iy+1)+fx*at(ix+1, iy+1));
    return true;
}

// The three channels at the same position, by the same interpolation (the same bounds as SampleLuma).
void SampleBgr(const BgrImage& image, Point raw, double out[3]) {
    const int ix = static_cast<int>(std::floor(raw.x)), iy = static_cast<int>(std::floor(raw.y));
    const double fx = raw.x-ix, fy = raw.y-iy;
    const std::uint8_t* const p00 = image.bgr.data()+(static_cast<std::size_t>(iy)*image.width+ix)*3;
    const std::uint8_t* const p10 = p00+static_cast<std::size_t>(image.width)*3;
    for (int k = 0; k < 3; ++k) out[k] = (1-fy)*((1-fx)*p00[k]+fx*p00[3+k])+fy*((1-fx)*p10[k]+fx*p10[3+k]);
}

// Edge-energy cells: luminance is sampled every 2 output pixels, the forward-difference magnitude is averaged over the
// (step/2)^2 samples of a cell. A cell is valid when every sample it uses is covered. The energy is then smoothed by a 3x3
// mean over the valid cells. With `colour`, the mean of each channel over the same samples of the cell is kept too,
// smoothed the same way.
Grid BuildGrid(const BgrImage& image, const OutputToCamera& map, std::int64_t x0, std::int64_t y0, int cols, int rows, int step, bool colour = false) {
    Grid g;
    g.x0 = x0; g.y0 = y0; g.cols = cols; g.rows = rows;
    const int sub = step/2;
    const int sw = sub*cols+1, sh = sub*rows+1;
    std::vector<float> luma(static_cast<std::size_t>(sw)*sh, std::numeric_limits<float>::quiet_NaN());
    std::vector<double> colour_sum;
    if (colour) colour_sum.assign(static_cast<std::size_t>(cols)*rows*3, 0.0);
    for (int l = 0; l < sh; ++l) for (int k = 0; k < sw; ++k) {
        Point raw{};
        double v{};
        if (!map({static_cast<double>(x0+2*k), static_cast<double>(y0+2*l)}, raw)) continue;
        Require(std::isfinite(raw.x) && std::isfinite(raw.y), "content registration: mapping returned nonfinite camera coordinates");
        if (!SampleLuma(image, raw, v)) continue;
        luma[static_cast<std::size_t>(l)*sw+k] = static_cast<float>(v);
        if (colour && k < sub*cols && l < sub*rows) {
            double bgr[3];
            SampleBgr(image, raw, bgr);
            double* const cell = colour_sum.data()+(static_cast<std::size_t>(l/sub)*cols+k/sub)*3;
            for (int c = 0; c < 3; ++c) cell[c] += bgr[c];
        }
    }

    std::vector<float> energy(static_cast<std::size_t>(cols)*rows, 0.f);
    g.valid.assign(static_cast<std::size_t>(cols)*rows, 0);
    for (int j = 0; j < rows; ++j) for (int i = 0; i < cols; ++i) {
        double sum = 0;
        bool ok = true;
        for (int l = 0; l < sub && ok; ++l) for (int k = 0; k < sub; ++k) {
            const std::size_t at = static_cast<std::size_t>(j*sub+l)*sw+(i*sub+k);
            const float c = luma[at], r = luma[at+1], d = luma[at+sw];
            if (std::isnan(c) || std::isnan(r) || std::isnan(d)) { ok = false; break; }
            sum += std::hypot(static_cast<double>(r-c), static_cast<double>(d-c));
        }
        if (!ok) continue;
        g.valid[static_cast<std::size_t>(j)*cols+i] = 1;
        energy[static_cast<std::size_t>(j)*cols+i] = static_cast<float>(sum/(sub*sub));
    }
    g.energy.assign(energy.size(), 0.f);
    for (int j = 0; j < rows; ++j) for (int i = 0; i < cols; ++i) {
        if (!g.valid[static_cast<std::size_t>(j)*cols+i]) continue;
        double sum = 0; int n = 0;
        for (int dj = -1; dj <= 1; ++dj) for (int di = -1; di <= 1; ++di) {
            const int a = i+di, b = j+dj;
            if (a < 0 || b < 0 || a >= cols || b >= rows || !g.valid[static_cast<std::size_t>(b)*cols+a]) continue;
            sum += energy[static_cast<std::size_t>(b)*cols+a]; ++n;
        }
        g.energy[static_cast<std::size_t>(j)*cols+i] = static_cast<float>(sum/n);
    }
    if (colour) {
        // The cell colours, smoothed like the energy (a 3x3 mean over the valid cells): the two grids are compared at whole
        // cells, up to half a cell off the true place, and a thin line must not count as a different colour for that.
        g.colour.assign(colour_sum.size(), 0.f);
        for (int j = 0; j < rows; ++j) for (int i = 0; i < cols; ++i) {
            if (!g.valid[static_cast<std::size_t>(j)*cols+i]) continue;
            double sum[3]{};
            int n = 0;
            for (int dj = -1; dj <= 1; ++dj) for (int di = -1; di <= 1; ++di) {
                const int a = i+di, b = j+dj;
                if (a < 0 || b < 0 || a >= cols || b >= rows || !g.valid[static_cast<std::size_t>(b)*cols+a]) continue;
                const double* const cell = colour_sum.data()+(static_cast<std::size_t>(b)*cols+a)*3;
                for (int c = 0; c < 3; ++c) sum[c] += cell[c];
                ++n;
            }
            float* const out = g.colour.data()+(static_cast<std::size_t>(j)*cols+i)*3;
            for (int c = 0; c < 3; ++c) out[c] = static_cast<float>(sum[c]/(static_cast<double>(n)*sub*sub));
        }
    }
    return g;
}

// The agreement of the cell colours of A's cells `area` with B's cells `shift` cells further (B's grid index): the zero-mean
// correlation pooled over the three channels. NaN when B covers fewer than `minimum_n` of the cells or a side has no
// variation. The edge energy of a table, a hatching or a chart of marks repeats; what the lines enclose usually does not,
// and that is what this compares.
double ColourAgreement(const Grid& a, const Grid& b, const std::vector<std::pair<int,int>>& area, int shift_i, int shift_j, double minimum_n) {
    double n = 0, sa[3]{}, sb[3]{}, saa[3]{}, sbb[3]{}, sab[3]{};
    for (const auto& [i, j] : area) {
        const int bi = i+shift_i, bj = j+shift_j;
        if (!b.Valid(bi, bj)) continue;
        const float* const ca = a.Colour(i, j);
        const float* const cb = b.Colour(bi, bj);
        n += 1;
        for (int k = 0; k < 3; ++k) {
            const double va = ca[k], vb = cb[k];
            sa[k] += va; sb[k] += vb; saa[k] += va*va; sbb[k] += vb*vb; sab[k] += va*vb;
        }
    }
    if (n < minimum_n || n < 3) return kNaN;
    double covariance = 0, var_a = 0, var_b = 0;
    for (int k = 0; k < 3; ++k) { covariance += sab[k]-sa[k]*sb[k]/n; var_a += saa[k]-sa[k]*sa[k]/n; var_b += sbb[k]-sb[k]*sb[k]/n; }
    if (!(var_a > 1e-9) || !(var_b > 1e-9)) return kNaN;
    return covariance/std::sqrt(var_a*var_b);
}

// Largest displacement (output px, per axis) that a pose inside the envelope gives at the sample positions (pose-only
// mode from the calibrated pose).
Point EnvelopeReach(const PoseModel& model, const PlaneFrame& frame, const RegistrationEnvelope& e, const std::vector<Point>& samples) {
    Point reach{0, 0};
    const double limit[6]{e.tilt_deg, e.tilt_deg, e.rotation_deg, e.position_mm, e.position_mm, e.height_percent};
    for (unsigned mask = 0; mask < 64; ++mask) {
        std::array<double,6> p{};
        for (unsigned i = 0; i < 6; ++i) p[i] = ((mask >> i) & 1u ? 1.0 : -1.0)*limit[i];
        const Matrix3 g = model.Homography(ToPose(p));
        for (const auto& s : samples) {
            const Point d = DisplacementUnder(g, frame, s.x, s.y);
            reach.x = std::max(reach.x, std::abs(d.x)); reach.y = std::max(reach.y, std::abs(d.y));
        }
    }
    return reach;
}
// The same around the base pose, with the corners of the surface envelope (design 11.3 step 1: the largest move that
// B's range and the surface's range can give the band).
// `gradient`, when given, receives the largest change of a displacement component per output row (between samples of a
// column, which must be listed column by column in increasing rows).
Point ReachAroundBase(const Model& model, const PlaneFrame& frame, const RegistrationSettings& s, const std::vector<Point>& samples,
    double* gradient = nullptr) {
    Point reach{0, 0};
    if (gradient) *gradient = 0;
    const auto& e = s.envelope;
    const double limit[6]{e.tilt_deg, e.tilt_deg, e.rotation_deg, e.position_mm, e.position_mm, e.height_percent};
    const unsigned surface_masks = model.surface ? 8u : 1u;
    const double height = model.surface ? s.surface_envelope->height_mm : 0.0;
    const double slope = model.surface ? std::tan(s.surface_envelope->tilt_deg*kDegrees) : 0.0;
    const Matrix3 base_inverse = render::Invert(model.Transform(model.Base()));
    for (unsigned mask = 0; mask < 64; ++mask) for (unsigned sm = 0; sm < surface_masks; ++sm) {
        std::array<double,6> p{};
        for (unsigned i = 0; i < 6; ++i) p[i] = ((mask >> i) & 1u ? 1.0 : -1.0)*limit[i];
        State state{model.AddBase(ToPose(p)), {}};
        if (model.surface) state.plane = {(sm & 1u ? 1.0 : -1.0)*height, (sm & 2u ? 1.0 : -1.0)*slope, (sm & 4u ? 1.0 : -1.0)*slope};
        const Matrix3 g = Mul(base_inverse, model.Transform(state));
        for (std::size_t k = 0; k < samples.size(); ++k) {
            const Point d = DisplacementUnder(g, frame, samples[k].x, samples[k].y);
            reach.x = std::max(reach.x, std::abs(d.x)); reach.y = std::max(reach.y, std::abs(d.y));
            // The change of the displacement along the rows between neighbouring samples of the same column.
            if (gradient && k+1 < samples.size() && samples[k+1].x == samples[k].x && samples[k+1].y > samples[k].y) {
                const Point next = DisplacementUnder(g, frame, samples[k+1].x, samples[k+1].y);
                const double rows = samples[k+1].y-samples[k].y;
                *gradient = std::max({*gradient, std::abs(next.x-d.x)/rows, std::abs(next.y-d.y)/rows});
            }
        }
    }
    return reach;
}

Parametrization MakeParametrization(const Model& model, const RegistrationSettings& s) {
    const auto& dz = s.design;
    const auto& e = s.envelope;
    Parametrization par;
    par.n = model.Count();
    if (!model.surface) {
        par.scale.assign(kScale.begin(), kScale.end());
        const double limit[6]{e.tilt_deg, e.tilt_deg, e.rotation_deg, e.position_mm, e.position_mm, e.height_percent};
        for (unsigned i = 0; i < 6; ++i) par.prior_sigma.push_back(dz.prior_sigma_factor*limit[i]);
    } else {
        par.scale.assign(kScaleSurface.begin(), kScaleSurface.end());
        const double slope = std::tan(s.surface_envelope->tilt_deg*kDegrees);
        // The rig is fixed: B's components are held to the envelope's own size, so that a narrow window, which cannot tell
        // B's tilt from the paper's height, gives the paper what it cannot split. The data still moves B beyond the range
        // when the points say so (a prior of one sigma at the range's edge is weak against a few hundred points).
        const double limit[5]{e.tilt_deg, e.tilt_deg, e.rotation_deg, e.position_mm, e.height_percent};
        for (unsigned i = 0; i < 5; ++i) par.prior_sigma.push_back(dz.surface_pose_prior_sigma_factor*limit[i]);
        par.prior_sigma.push_back(dz.surface_prior_sigma_factor*s.surface_envelope->height_mm);
        par.prior_sigma.push_back(dz.surface_prior_sigma_factor*slope);
        par.prior_sigma.push_back(dz.surface_prior_sigma_factor*slope);
    }
    par.homography = [&model](const std::vector<double>& p) { return model.Transform(model.StateOf(p)); };
    return par;
}
// The two parameters of the uniform shift (G3): surface mode s and h0, pose-only mode tx and ty (differences from the base).
Parametrization MakeShiftParametrization(const Model& model, const RegistrationSettings& s) {
    Parametrization par;
    par.n = 2;
    par.scale = {10.0, 10.0};
    const double position = s.design.prior_sigma_factor*s.envelope.position_mm;
    if (model.surface) {
        par.prior_sigma = {position, s.design.surface_prior_sigma_factor*s.surface_envelope->height_mm};
        par.homography = [&model](const std::vector<double>& p) {
            return model.Transform({model.AddBase({0, 0, 0, p[0]*model.gauge.x, p[0]*model.gauge.y, 0}), {p[1], 0, 0}});
        };
    } else {
        par.prior_sigma = {position, position};
        par.homography = [&model](const std::vector<double>& p) { return model.Transform({model.AddBase({0, 0, 0, p[0], p[1], 0}), {}}); };
    }
    return par;
}
State ShiftState(const Model& model, const std::vector<double>& p) {
    if (model.surface) return {model.AddBase({0, 0, 0, p[0]*model.gauge.x, p[0]*model.gauge.y, 0}), {p[1], 0, 0}};
    return {model.AddBase({0, 0, 0, p[0], p[1], 0}), {}};
}

struct CoarseOutcome {
    bool ok{};
    std::string reason;
    State state;
    double radius_x{}, radius_y{};
    std::vector<CoarseBlock> blocks;
    std::uint32_t unique{}, consistent{};
    std::uint32_t anchors{}, consistent_anchors{};   // the anchors among them (unique, or resolved by colour)
    CoarseVote vote;
    CoarseShift shift;
};

// The vote of the surface mode over the blocks' peaks. Sets the blocks' voted peak and returns the vote.
//
// An anchor is a block with one place: unique, or resolved by colour. Only anchors are evidence, so a hypothesis is made
// from the peaks of anchors (one peak, or two peaks of different anchors) and is judged by the anchors that agree with
// it. A repeating block has a strong peak near almost any hypothesis: it never makes or decides one, it follows the
// winner with the strong peak nearest to it (`strong` holds every peak of a block that may agree: for an anchor its one
// peak, for a repeating block all of its strong peaks).
//
// The winner has the most anchors, then the most blocks, then the larger summed correlation, then comes first. Its
// rival is the best hypothesis that an anchor outside the winner's anchors agrees with: another explanation of the
// evidence. A hypothesis whose anchors are all the winner's is the same explanation, a little off, and is no rival.
CoarseVote Vote(std::vector<CoarseBlock>& blocks, const std::vector<std::vector<CoarsePeak>>& strong, double tolerance, double gradient_limit) {
    CoarseVote vote;
    vote.performed = true;
    vote.gradient_limit = gradient_limit;
    const auto anchor = [&](std::size_t b) { return blocks[b].status == CoarseBlockStatus::unique || blocks[b].status == CoarseBlockStatus::resolved; };
    struct Candidate { std::size_t block; double y; Point d; };
    std::vector<Candidate> candidates;   // the peaks of the anchors
    double y_sum = 0;
    std::size_t with_peaks = 0;
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        if (blocks[b].peaks.empty()) continue;
        ++with_peaks; y_sum += blocks[b].at.y;
        vote.candidates += static_cast<std::uint32_t>(blocks[b].peaks.size());
        if (anchor(b)) for (const auto& peak : blocks[b].peaks) candidates.push_back({b, blocks[b].at.y, peak.displacement});
    }
    if (with_peaks == 0) return vote;
    vote.reference_row = y_sum/static_cast<double>(with_peaks);
    if (candidates.empty()) return vote;   // nothing but repeating blocks: no hypothesis
    struct Hypothesis { Point c, g; };
    const auto predict = [&](const Hypothesis& h, double y) { return Point{h.c.x+h.g.x*(y-vote.reference_row), h.c.y+h.g.y*(y-vote.reference_row)}; };
    struct Scored {
        Hypothesis h;
        std::uint32_t support{}, anchors{};   // less the blocks it was made from beyond the first (they agree by construction)
        double correlation{};
        std::vector<char> agrees;             // per block: an anchor that agrees
    };
    std::vector<Scored> scored;
    const auto add = [&](const Hypothesis& h, std::uint32_t defining) {
        Scored s;
        s.h = h;
        s.agrees.assign(blocks.size(), 0);
        std::uint32_t support = 0, anchors = 0;
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            if (blocks[b].peaks.empty()) continue;
            const Point p = predict(h, blocks[b].at.y);
            double best = -kInfinity;
            for (const auto& peak : strong[b])
                if (std::hypot(peak.displacement.x-p.x, peak.displacement.y-p.y) <= tolerance) best = std::max(best, peak.correlation);
            if (best == -kInfinity) continue;
            ++support; s.correlation += best;
            if (anchor(b)) { ++anchors; s.agrees[b] = 1; }
        }
        s.support = support >= defining ? support-(defining-1) : 0;
        s.anchors = anchors >= defining ? anchors-(defining-1) : 0;
        scored.push_back(std::move(s));
    };
    for (const auto& c : candidates) add({{c.d.x+0.0, c.d.y+0.0}, {0, 0}}, 1);
    for (std::size_t i = 0; i < candidates.size(); ++i) for (std::size_t j = i+1; j < candidates.size(); ++j) {
        const auto& a = candidates[i];
        const auto& b = candidates[j];
        if (a.block == b.block || !(std::abs(b.y-a.y) > 0)) continue;
        const Point g{(b.d.x-a.d.x)/(b.y-a.y), (b.d.y-a.d.y)/(b.y-a.y)};
        // A gradient along the rows that B's and the surface's ranges cannot give is not a hypothesis.
        if (std::abs(g.x) > gradient_limit || std::abs(g.y) > gradient_limit) continue;
        add({{a.d.x+g.x*(vote.reference_row-a.y), a.d.y+g.y*(vote.reference_row-a.y)}, g}, 2);
    }
    vote.hypotheses = static_cast<std::uint32_t>(scored.size());
    const auto better = [&](const Scored& l, const Scored& r) {
        if (l.anchors != r.anchors) return l.anchors > r.anchors;
        if (l.support != r.support) return l.support > r.support;
        return l.correlation > r.correlation;
    };
    std::size_t winner = 0;
    for (std::size_t i = 1; i < scored.size(); ++i) if (better(scored[i], scored[winner])) winner = i;
    const auto& won = scored[winner];
    vote.support = won.support;
    vote.anchor_support = won.anchors;
    vote.offset = won.h.c; vote.gradient = won.h.g;
    for (const auto& s : scored) {
        bool outside = false, apart = false;
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            outside = outside || (s.agrees[b] && !won.agrees[b]);
            if (blocks[b].peaks.empty()) continue;
            const Point ps = predict(s.h, blocks[b].at.y), pw = predict(won.h, blocks[b].at.y);
            apart = apart || std::hypot(ps.x-pw.x, ps.y-pw.y) > tolerance;
        }
        if (outside) vote.rival_anchor_support = std::max(vote.rival_anchor_support, s.anchors);
        if (apart) vote.rival_support = std::max(vote.rival_support, s.support);
    }
    // The voted peak of each block.
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        auto& block = blocks[b];
        if (block.peaks.empty()) continue;
        const auto& peaks = strong[b];
        const Point p = predict(won.h, block.at.y);
        std::size_t chosen = peaks.size();
        double nearest = kInfinity;
        for (std::size_t k = 0; k < peaks.size(); ++k) {
            const double miss = std::hypot(peaks[k].displacement.x-p.x, peaks[k].displacement.y-p.y);
            if (miss <= tolerance && miss < nearest) { nearest = miss; chosen = k; }
        }
        if (chosen == peaks.size()) continue;
        block.voted = true;
        block.displacement = peaks[chosen].displacement;
        block.correlation = peaks[chosen].correlation;
        if (anchor(b)) ++vote.unique_supporters;
    }
    vote.decided = vote.anchor_support > vote.rival_anchor_support && vote.unique_supporters >= 1;
    return vote;
}

CoarseOutcome CoarseStage(const BgrImage& image_a, const BgrImage& image_b, const OutputToCamera& map_a, const OutputToCamera& map_b_base,
    const Layout& layout, const Model& model, const RegistrationSettings& s, unsigned threads) {
    const auto& dz = s.design;
    const bool legacy = !model.surface && model.base_zero;   // the pose-only estimator from the calibrated pose, unchanged
    CoarseOutcome out;
    out.state = model.Base();
    const int step = static_cast<int>(dz.coarse_step_px);
    // Extent of W.
    std::int64_t min_x = std::numeric_limits<std::int64_t>::max(), max_x = -1, min_y = -1, max_y = -1;
    for (std::uint32_t y = 0; y < layout.height; ++y) {
        const auto& w = layout.window_coarse[y];
        for (const auto& [b, e] : {std::pair{w.left_begin, w.left_end}, std::pair{w.right_begin, w.right_end}}) {
            if (b >= e) continue;
            min_x = std::min<std::int64_t>(min_x, b); max_x = std::max<std::int64_t>(max_x, e);
            if (min_y < 0) min_y = y;
            max_y = y+1;
        }
    }
    if (max_x < 0) { out.reason = "no-overlap"; return out; }
    // Radius of the search from the envelope (and the surface envelope).
    std::vector<Point> samples;
    for (int i = 0; i <= 4; ++i) for (int j = 0; j <= 6; ++j)
        samples.push_back({static_cast<double>(min_x)+(max_x-1-min_x)*i/4.0, static_cast<double>(min_y)+(max_y-1-min_y)*j/6.0});
    double gradient = 0;
    const Point reach = legacy ? EnvelopeReach(*model.pose_model, layout.frame, s.envelope, samples) : ReachAroundBase(model, layout.frame, s, samples, &gradient);
    // The vote's bound on the change along the rows: the ranges' largest change, with the same scale and margin as the radius.
    const double gradient_limit = gradient*dz.coarse_radius_scale+2.0*dz.coarse_radius_margin_px/std::max<double>(1.0, static_cast<double>(max_y-min_y));
    const auto cells_of = [&](double reach_px) {
        const double radius = std::min<double>(dz.coarse_maximum_radius_px, reach_px*dz.coarse_radius_scale+dz.coarse_radius_margin_px);
        return static_cast<int>(std::ceil(radius/step));
    };
    const int rx = cells_of(reach.x), ry = cells_of(reach.y);
    out.radius_x = rx*step; out.radius_y = ry*step;

    // Grids: A over W's extent, B (placed by the base) over the same extent widened by the search radius.
    const std::int64_t gx0 = min_x-min_x%step, gy0 = min_y-min_y%step;
    const int cols = static_cast<int>((max_x-gx0+step-1)/step), rows = static_cast<int>((max_y-gy0+step-1)/step);
    Grid a, b;
    const bool colour = model.surface;   // the cell colours tell the peaks of a repeating block apart
    Both(threads >= 2,
        [&]() { a = BuildGrid(image_a, map_a, gx0, gy0, cols, rows, step, colour); },
        [&]() { b = BuildGrid(image_b, map_b_base, gx0-static_cast<std::int64_t>(rx)*step, gy0-static_cast<std::int64_t>(ry)*step, cols+2*rx, rows+2*ry, step, colour); });

    const int block_cells = std::max(1, static_cast<int>(dz.coarse_block_rows)/step);
    const int span_x = 2*rx+1, span_y = 2*ry+1;
    struct Cell { int i, j; float value; };
    // B's cells for the search: 1 and the energy where the cell is valid, 0 and 0 where it is not, so that the sums of
    // an offset can be taken without a branch (adding 0 leaves a sum as it is, bit for bit).
    std::vector<double> b_mask(b.energy.size()), b_value(b.energy.size());
    for (std::size_t k = 0; k < b.energy.size(); ++k) { b_mask[k] = b.valid[k] ? 1.0 : 0.0; b_value[k] = b.valid[k] ? static_cast<double>(b.energy[k]) : 0.0; }
    struct Sums { double n, sa, sb, saa, sbb, sab; };
    std::vector<int> starts;
    for (int r0 = 0; r0 < rows; r0 += block_cells) starts.push_back(r0);
    out.blocks.assign(starts.size(), CoarseBlock{});
    std::vector<std::vector<CoarsePeak>> strong(starts.size());
    // One block: its correlation surface over the whole search window, and its peaks. The blocks share no state, so they
    // are dealt to the threads.
    const auto process = [&](std::size_t index, std::vector<double>& surface) {
        const int r0 = starts[index];
        const int r1 = std::min(rows, r0+block_cells);
        CoarseBlock& block = out.blocks[index];
        block.y0 = static_cast<std::uint32_t>(gy0+static_cast<std::int64_t>(r0)*step);
        block.rows = static_cast<std::uint32_t>((r1-r0)*step);
        std::vector<Cell> content;
        std::vector<std::pair<int,int>> area;   // every cell of the block inside W that A covers (surface mode)
        double sum_x = 0, sum_y = 0;
        for (int j = r0; j < r1; ++j) {
            const std::int64_t y = gy0+static_cast<std::int64_t>(j)*step+step/2;
            if (y < 0 || y >= static_cast<std::int64_t>(layout.height)) continue;
            const auto& w = layout.window_coarse[static_cast<std::size_t>(y)];
            for (int i = 0; i < cols; ++i) {
                if (!a.Valid(i, j)) continue;
                const std::int64_t xa = gx0+static_cast<std::int64_t>(i)*step, xb = xa+step-1;
                if (colour && InWindow(w, xa) && InWindow(w, xb)) area.emplace_back(i, j);
                if (a.At(i, j) < dz.coarse_content_level) continue;
                if (!(InWindow(w, xa) && InWindow(w, xb))) continue;
                content.push_back({i, j, a.At(i, j)});
                sum_x += static_cast<double>(xa)+(step-1)/2.0; sum_y += static_cast<double>(gy0+static_cast<std::int64_t>(j)*step)+(step-1)/2.0;
            }
        }
        if (content.size() < dz.coarse_minimum_cells) { block.status = CoarseBlockStatus::too_few_cells; return; }
        block.at = {sum_x/content.size(), sum_y/content.size()};
        std::fill(surface.begin(), surface.end(), kNaN);
        const double minimum_n = dz.coarse_minimum_overlap*static_cast<double>(content.size());
        // The masked zero-mean correlation at every offset. One row of offsets at a time: each offset's sums take the
        // cells in the order of `content` (the order of the plain loop over the offsets, so the sums are the same bit for
        // bit), and the inner loop runs along a row of B's cells. B's grid is A's widened by the radius, so every index
        // is inside it.
        std::vector<Sums> sums(static_cast<std::size_t>(span_x));
        for (int oy = -ry; oy <= ry; ++oy) {
            std::fill(sums.begin(), sums.end(), Sums{});
            for (const auto& c : content) {
                const std::size_t first = static_cast<std::size_t>(c.j+oy+ry)*b.cols+c.i;
                const double* const mask = b_mask.data()+first;
                const double* const value = b_value.data()+first;
                const double va = c.value, va2 = va*va;
                Sums* const s = sums.data();
                for (int k = 0; k < span_x; ++k) {
                    const double m = mask[k], vb = value[k];
                    s[k].n += m; s[k].sa += m*va; s[k].sb += vb; s[k].saa += m*va2; s[k].sbb += vb*vb; s[k].sab += va*vb;
                }
            }
            for (int k = 0; k < span_x; ++k) {
                const auto& s = sums[k];
                if (s.n < minimum_n || s.n < 3) continue;
                const double var_a = s.saa-s.sa*s.sa/s.n, var_b = s.sbb-s.sb*s.sb/s.n;
                if (!(var_a > 1e-9) || !(var_b > 1e-9)) continue;
                surface[static_cast<std::size_t>(oy+ry)*span_x+k] = (s.sab-s.sa*s.sb/s.n)/std::sqrt(var_a*var_b);
            }
        }
        // Best peak (first in scan order), then the highest other local maximum away from it.
        int bx = 0, by = 0;
        double best = -kInfinity;
        for (int oy = 0; oy < span_y; ++oy) for (int ox = 0; ox < span_x; ++ox) {
            const double v = surface[static_cast<std::size_t>(oy)*span_x+ox];
            if (!std::isnan(v) && v > best) { best = v; bx = ox; by = oy; }
        }
        if (best == -kInfinity) { block.status = CoarseBlockStatus::low_overlap; return; }
        block.correlation = best;
        if (!(best > 0)) { block.status = CoarseBlockStatus::flat; return; }
        if (best < dz.coarse_minimum_correlation) { block.status = CoarseBlockStatus::low_correlation; return; }
        if (bx == 0 || by == 0 || bx == span_x-1 || by == span_y-1) { block.status = CoarseBlockStatus::search_boundary; return; }
        const auto at = [&](int ox, int oy) { return surface[static_cast<std::size_t>(oy)*span_x+ox]; };
        const auto is_local = [&](int ox, int oy, double v) {
            for (int dj = -1; dj <= 1; ++dj) for (int di = -1; di <= 1; ++di) {
                const int u = ox+di, w = oy+dj;
                if ((di || dj) && u >= 0 && w >= 0 && u < span_x && w < span_y) {
                    const double other = at(u, w);
                    if (!std::isnan(other) && other > v) return false;
                }
            }
            return true;
        };
        // Parabola through the three scores along each axis.
        const auto vertex = [](double m, double c, double p) {
            if (std::isnan(m) || std::isnan(p)) return 0.0;
            const double denominator = m-2*c+p;
            return denominator < 0 ? std::clamp(0.5*(m-p)/denominator, -0.5, 0.5) : 0.0;
        };
        const auto refined = [&](int ox, int oy) {
            const double v = at(ox, oy);
            const double fx = vertex(at(ox-1, oy), v, at(ox+1, oy)), fy = vertex(at(ox, oy-1), v, at(ox, oy+1));
            return Point{(ox-rx+fx)*step, (oy-ry+fy)*step};
        };
        double second = -kInfinity;
        struct Local { int ox, oy; double v; };
        std::vector<Local> locals;
        // Surface mode: the strong local maxima on the border of the search window. One of them cannot be a place (its
        // peak may lie outside the window), but it is a rival of the places inside when their colours are compared.
        std::vector<Local> border;
        for (int oy = 0; oy < span_y; ++oy) for (int ox = 0; ox < span_x; ++ox) {
            const double v = surface[static_cast<std::size_t>(oy)*span_x+ox];
            if (std::isnan(v) || std::max(std::abs(ox-bx), std::abs(oy-by)) <= 2) continue;
            if (!is_local(ox, oy, v)) continue;
            second = std::max(second, v);
            if (!model.surface || !(v >= dz.coarse_secondary_ratio*best)) continue;
            (ox > 0 && oy > 0 && ox < span_x-1 && oy < span_y-1 ? locals : border).push_back({ox, oy, v});
        }
        const bool repeating = second >= dz.coarse_secondary_ratio*best;
        if (model.surface) {
            // The strong peaks: the best and the other strict local maxima within the secondary ratio, best first.
            std::stable_sort(locals.begin(), locals.end(), [](const Local& l, const Local& r) { return l.v > r.v; });
            // More strong peaks than are compared: the place may be one that is left out, so the block is not resolved.
            bool all_compared = locals.size()+border.size()+1 <= dz.coarse_alias_peaks;
            if (locals.size()+1 > dz.coarse_alias_peaks) locals.resize(dz.coarse_alias_peaks-1);
            locals.insert(locals.begin(), Local{bx, by, best});
            auto& peaks = strong[index];
            for (const auto& l : locals) peaks.push_back({refined(l.ox, l.oy), l.v, 0.0});
            if (repeating && locals.size() >= 2) {
                // The edge energy repeats. Compare the strong peaks by the colour of the block's cells: one that agrees,
                // with every other clearly behind, is the place (the block is resolved).
                const double minimum_area = dz.coarse_minimum_overlap*static_cast<double>(area.size());
                std::size_t first = locals.size();
                double first_score = -kInfinity, next_score = -kInfinity;
                for (std::size_t k = 0; k < locals.size(); ++k) {
                    const double score = ColourAgreement(a, b, area, locals[k].ox, locals[k].oy, minimum_area);
                    // A peak where B does not cover the block's cells cannot be compared. It is shown as no agreement, and
                    // because the place may be that one, the block is not resolved.
                    if (std::isnan(score)) all_compared = false;
                    peaks[k].colour_score = std::isnan(score) ? -1.0 : score;
                    if (peaks[k].colour_score > first_score) { next_score = first_score; first_score = peaks[k].colour_score; first = k; }
                    else if (peaks[k].colour_score > next_score) next_score = peaks[k].colour_score;
                }
                // The rivals on the border: the chosen peak has to lead them too. One that cannot be compared leaves the
                // block unresolved, like a peak inside.
                if (all_compared) for (const auto& l : border) {
                    const double score = ColourAgreement(a, b, area, l.ox, l.oy, minimum_area);
                    if (std::isnan(score)) { all_compared = false; break; }
                    next_score = std::max(next_score, score);
                }
                block.compared_peaks = static_cast<std::uint32_t>(locals.size()+border.size());
                block.colour_score = first_score;
                block.colour_margin = first_score-next_score;
                if (all_compared && first_score >= dz.coarse_alias_minimum_score && block.colour_margin >= dz.coarse_alias_margin) {
                    block.status = CoarseBlockStatus::resolved;
                    block.correlation = peaks[first].correlation;
                    const CoarsePeak chosen = peaks[first];
                    peaks.assign(1, chosen);
                    block.peaks = peaks;
                    return;
                }
            }
            // The peaks a hypothesis of the vote may be made from: the first K.
            block.peaks.assign(peaks.begin(), peaks.begin()+static_cast<std::ptrdiff_t>(std::min<std::size_t>(peaks.size(), dz.coarse_vote_peaks)));
        }
        if (repeating) { block.status = CoarseBlockStatus::repeating; return; }
        block.status = CoarseBlockStatus::unique;
        block.displacement = refined(bx, by);
    };
    ForEachIndex(threads, starts.size(), static_cast<std::size_t>(span_x)*span_y, process);
    for (const auto& block : out.blocks) if (block.status == CoarseBlockStatus::unique) ++out.unique;
    // Surface mode: the vote picks one peak per block; the voted blocks take the place of the unique blocks.
    if (model.surface) {
        for (auto& block : out.blocks) if (block.status == CoarseBlockStatus::unique) block.displacement = {};
        out.vote = Vote(out.blocks, strong, dz.coarse_consistency_px, gradient_limit);
        out.unique = 0;
        if (out.vote.candidates == 0) { out.reason = "coarse-no-unique-blocks"; return out; }
        if (!out.vote.decided) {
            bool any_unique = false;
            for (const auto& block : out.blocks) any_unique = any_unique || block.status == CoarseBlockStatus::unique || block.status == CoarseBlockStatus::resolved;
            out.reason = any_unique ? "coarse-ambiguous" : "repeating-only";
            for (auto& block : out.blocks) block.voted = false;
            return out;
        }
    }
    // The blocks the start is fitted to. Surface mode: the anchors that agree with the winner, and the repeating blocks
    // that follow it between the anchors' rows. A follower's peak was picked by the hypothesis, not found: between two
    // anchors the hypothesis is held at both ends, above and below them it is a guess and the peak may be the neighbouring
    // period, so those are taken only when the anchors alone are fewer than the fit needs.
    const auto is_anchor = [](const CoarseBlock& block) { return block.status == CoarseBlockStatus::unique || block.status == CoarseBlockStatus::resolved; };
    double anchor_low = kInfinity, anchor_high = -kInfinity;
    for (const auto& block : out.blocks) if (block.voted && is_anchor(block)) { anchor_low = std::min(anchor_low, block.at.y); anchor_high = std::max(anchor_high, block.at.y); }
    const bool few_anchors = model.surface && out.vote.unique_supporters < std::max<std::uint32_t>(1, s.limits.minimum_unique_blocks);
    const auto used = [&](const CoarseBlock& block) {
        if (!model.surface) return block.status == CoarseBlockStatus::unique;
        return block.voted && (is_anchor(block) || few_anchors || (block.at.y > anchor_low && block.at.y < anchor_high));
    };
    if (model.surface) for (const auto& block : out.blocks) if (used(block)) ++out.unique;
    // The anchors are counted apart: the followers help the start of the fit, but only an anchor is evidence of the place (V2).
    for (const auto& block : out.blocks) if (used(block) && is_anchor(block)) ++out.anchors;
    if (out.unique < 1) { out.reason = "coarse-no-unique-blocks"; return out; }

    // The fit the blocks give. Blocks that disagree with it are dropped, worst first.
    const auto par = MakeParametrization(model, s);
    const Matrix3 place_inverse = model.base_zero ? kIdentity : render::Invert(model.Transform(model.Base()));
    std::vector<std::size_t> kept;
    for (std::size_t i = 0; i < out.blocks.size(); ++i) if (used(out.blocks[i])) kept.push_back(i);
    const auto obs_of = [&](const std::vector<std::size_t>& set) {
        std::vector<Obs> obs;
        for (const auto i : set) {
            Obs o;
            o.x = out.blocks[i].at.x; o.y = out.blocks[i].at.y; o.d = out.blocks[i].displacement; o.sigma = dz.coarse_sigma_px;
            obs.push_back(o);
        }
        return obs;
    };
    const auto miss_of = [&](const Matrix3& g, std::size_t i) {
        const auto& blk = out.blocks[i];
        const Point p = DisplacementUnder(g, layout.frame, blk.at.x, blk.at.y);
        return std::hypot(blk.displacement.x-p.x, blk.displacement.y-p.y);
    };
    // The uniform shift (G3 candidate) over a set of blocks.
    const auto shift_over = [&](const std::vector<std::size_t>& set) {
        CoarseShift shift;
        shift.blocks = static_cast<std::uint32_t>(set.size());
        if (set.empty()) return shift;
        const auto shift_par = MakeShiftParametrization(model, s);
        const auto fit = FitModel(obs_of(set), shift_par, layout.frame, place_inverse, {0.0, 0.0}, dz, false);
        if (!fit.ok) return shift;
        const State state = ShiftState(model, fit.parameters);
        const Matrix3 g = Mul(place_inverse, model.Transform(state));
        for (const auto i : set) shift.maximum_miss_px = std::max(shift.maximum_miss_px, miss_of(g, i));
        shift.pose = state.pose; shift.surface = state.plane;
        if (model.surface) { shift.orthogonal_translation_mm = fit.parameters[0]; shift.surface_height_mm = fit.parameters[1]; }
        shift.determined = set.size() >= std::max<std::size_t>(1, s.limits.minimum_unique_blocks) && shift.maximum_miss_px <= dz.coarse_consistency_px;
        return shift;
    };
    const auto all_used = kept;
    if (out.unique < s.limits.minimum_unique_blocks) { out.reason = "unique-blocks"; out.shift = shift_over(all_used); return out; }

    State state = model.Base();
    const std::size_t minimum_blocks = std::max<std::size_t>(1, s.limits.minimum_unique_blocks);
    const std::vector<double> zero(par.n, 0.0);
    for (;;) {
        // A few blocks do not fix every component; the prior holds the rest.
        const auto fit = FitModel(obs_of(kept), par, layout.frame, place_inverse, zero, dz, false);
        if (!fit.ok) { out.reason = "coarse-inconsistent"; out.shift = shift_over(all_used); return out; }
        state = model.StateOf(fit.parameters);
        const Matrix3 g = legacy ? model.pose_model->Homography(state.pose) : Mul(place_inverse, model.Transform(state));
        std::size_t worst = kept.size();
        double worst_value = 0;
        for (std::size_t k = 0; k < kept.size(); ++k) {
            const double miss = miss_of(g, kept[k]);
            if (miss > worst_value) { worst_value = miss; worst = k; }
        }
        if (worst_value <= dz.coarse_consistency_px) break;
        if (kept.size() <= minimum_blocks) { out.reason = "coarse-inconsistent"; out.shift = shift_over(all_used); return out; }
        kept.erase(kept.begin()+static_cast<std::ptrdiff_t>(worst));
    }
    for (const auto i : kept) {
        out.blocks[i].consistent = true; ++out.consistent;
        if (is_anchor(out.blocks[i])) ++out.consistent_anchors;
    }
    out.state = state;
    out.shift = shift_over(kept);
    out.ok = true;
    return out;
}

// ---- Stage 2: the measurer on W ---------------------------------------------------------------------------------------
gate::SeamGateDesign FineDesign(const RegistrationDesign& dz) {
    auto design = gate::kSeamGateDesignCalibrated;
    design.point_spacing_px = dz.fine_point_spacing_px;
    design.block_px = 2*dz.fine_point_spacing_px;
    design.patch_px = dz.fine_patch_px;
    design.remeasure_patch_px = std::max(design.remeasure_patch_px, dz.fine_patch_px+32);
    design.search_radius_px = dz.fine_search_radius_px;
    design.maximum_search_radius_px = dz.fine_maximum_search_radius_px;
    design.strip_rows = design.block_px*((256+design.block_px-1)/design.block_px);
    design.global_search_radius_px = dz.fine_global_search_radius_px;
    design.global_maximum_evaluations = 1;
    design.global_minimum_anchors = 1;
    gate::ValidateSeamGateDesign(design);
    return design;
}

struct FineMeasurement {
    std::vector<Obs> obs;
    std::vector<gate::SeamPoint> points;   // both sides, left first
    std::uint32_t gate_points{};
    std::uint32_t contradicted{};
};

// B placed by `map_b_placed`; the displacement left is against that placement. The two sides of W are measured
// concurrently when `parallel` (the measurer's result does not depend on its thread count).
// With `held` (the fit window of an earlier measurement), only its columns are measured: the measurer's work grows with
// the area of the band, and the columns beyond the window would not be used. Each side's band is cut at a column of
// the measurer's lattice, so the points that are left are the points of the whole band at the same places.
FineMeasurement MeasureFine(const BgrImage& image_a, const BgrImage& image_b, const Layout& layout, const OutputToCamera& map_a,
    const OutputToCamera& map_b_placed, const RegistrationSettings& s, const gate::SeamGateDesign& design, unsigned threads, bool parallel,
    const FitWindow* held = nullptr) {
    FineMeasurement out;
    // Omega +- guard is excluded in the windows; the weight grows with the distance from it.
    const double exclusion = s.geometry.exclusion_half_width_px();
    const gate::SeamGateLimits limits{1e9, 0.0, "registration"};
    std::optional<gate::SeamMeasurement> measured[2];
    const auto side = [&](int which, unsigned side_threads) {
        gate::SeamGateGeometry geometry;
        geometry.width = layout.width; geometry.height = layout.height;
        geometry.camera_a = map_a; geometry.camera_b = map_b_placed;
        geometry.band.assign(layout.height, {});
        bool any = false;
        std::int64_t first_column = std::numeric_limits<std::int64_t>::max(), first_row = -1;
        for (std::uint32_t y = 0; y < layout.height; ++y) {
            const auto& w = layout.window_fine[y];
            const std::uint32_t b = which == 0 ? w.left_begin : w.right_begin, e = which == 0 ? w.left_end : w.right_end;
            if (b < e) { geometry.band[y] = {b, e}; any = true; first_column = std::min<std::int64_t>(first_column, b); if (first_row < 0) first_row = y; }
        }
        if (!any) return;
        if (held && held->grown) {
            // The measurer's lattice starts half a spacing inside the band's first column and first row. The left side is
            // cut half a spacing outside the window's farthest column, the right side just past its own; the cut is taken
            // only when it leaves the lattice where it was (otherwise the whole side is measured).
            const std::int64_t seam = s.geometry.seam_column, spacing = design.point_spacing_px;
            const std::int64_t low = seam-std::llround(held->left_px)-spacing/2, high = seam+std::llround(held->right_px)+1;
            auto cut = geometry.band;
            std::int64_t cut_column = std::numeric_limits<std::int64_t>::max(), cut_row = -1;
            for (std::uint32_t y = 0; y < layout.height; ++y) {
                auto& row = cut[y];
                if (row.begin >= row.end) continue;
                if (which == 0) row.begin = static_cast<std::uint32_t>(std::max<std::int64_t>(row.begin, low));
                else row.end = static_cast<std::uint32_t>(std::max<std::int64_t>(0, std::min<std::int64_t>(row.end, high)));
                if (row.begin >= row.end) { row = {}; continue; }
                cut_column = std::min<std::int64_t>(cut_column, row.begin);
                if (cut_row < 0) cut_row = y;
            }
            if (cut_row == first_row && (cut_column-first_column)%spacing == 0) geometry.band = std::move(cut);
        }
        measured[which] = gate::MeasureSeam(image_a, image_b, geometry, design, limits, nullptr, side_threads);
    };
    const unsigned side_threads = parallel ? std::max(1u, threads/2) : threads;
    Both(parallel, [&]() { side(0, side_threads); }, [&]() { side(1, side_threads); });
    for (int which = 0; which < 2; ++which) {
        if (!measured[which]) continue;
        auto& measurement = *measured[which];
        out.gate_points += static_cast<std::uint32_t>(measurement.points.size());
        out.contradicted += measurement.global.contradicted;
        for (const auto& p : measurement.points) {
            if (!p.Counted()) continue;
            Obs o;
            o.x = p.x; o.y = p.y; o.one = p.kind == gate::PointKind::one_directional;
            o.n = p.normal; o.d = p.displacement;
            const double dist = std::max(0.0, std::abs(static_cast<double>(p.x)-static_cast<double>(s.geometry.seam_column))-exclusion);
            o.sigma = std::max(p.uncertainty_px, s.design.sigma_floor_px)*std::sqrt(1.0+dist/s.design.proximity_length_px);
            out.obs.push_back(o);
        }
        std::move(measurement.points.begin(), measurement.points.end(), std::back_inserter(out.points));
    }
    return out;
}

struct Run {
    bool ok{};
    std::string reason;
    Fit fit;
    State state;                // the fitted state
    State placed;               // the state B was placed by when the last measurement was made
    std::vector<Obs> obs;       // that measurement
    std::vector<gate::SeamPoint> points;
    std::uint32_t outer{};
    bool converged{};
    double last_change{};
    std::vector<double> changes;   // per outer iteration
    std::uint32_t gate_points{}, contradicted{};
    FitWindow window;           // of the last fit (surface mode)
};

// Where an estimate is made: the document raster of a capture (`document` set), or a grid of the caller's (the live C).
// The stages read the place through this, so both run the same arithmetic.
struct Scene {
    std::uint32_t width{}, height{};
    PlaneFrame frame;
    OutputToCamera map_a, map_b0;        // A; B at the calibrated pose
    const PoseModel* pose_model{};
    // B placed by a state that is not the calibrated one.
    std::function<OutputToCamera(const State&, const Model&)> place;
    // A capture: the parameters that the surface's frame and the displays of the result are made from.
    const render::DocumentRenderParameters* document{};
    // A grid: the surface's frame, the measurer, the job and what is reported besides the result.
    Centre centre_a;
    Point reference_mm{}, reference_px{};
    const gate::SeamGateDesign* measurer{};
    const State* follow{};               // set: the follow-up job, started from this state
    double minimum_band_brightness{0};
    GridRegistrationResult* report{};
    bool Grid() const { return document == nullptr; }
};

// Mean luminance of an image over the band W through a mapping, every 4th sample of every 4th row. NaN when no sample is
// covered.
double BandBrightness(const BgrImage& image, const OutputToCamera& map, const std::vector<RowWindow>& windows) {
    double sum = 0;
    std::uint64_t count = 0;
    for (std::size_t y = 0; y < windows.size(); y += 4) {
        const auto& w = windows[y];
        for (const auto& [begin, end] : {std::pair{w.left_begin, w.left_end}, std::pair{w.right_begin, w.right_end}})
            for (std::uint32_t x = begin; x < end; x += 4) {
                Point raw{};
                double value{};
                if (!map({static_cast<double>(x), static_cast<double>(y)}, raw) || !std::isfinite(raw.x) || !std::isfinite(raw.y)) continue;
                if (!SampleLuma(image, raw, value)) continue;
                sum += value; ++count;
            }
    }
    return count ? sum/static_cast<double>(count) : kNaN;
}

RegistrationResult EstimateOn(const BgrImage& camera_a, const BgrImage& camera_b, const Scene& scene, const RegistrationSettings& settings,
    unsigned thread_count, const RegistrationControl& control);

}  // namespace

// ---- Public pieces ---------------------------------------------------------------------------------------------------
Point ResidualDisplacement(const PoseModel& model, const PlaneFrame& frame, const PoseParameters& placed, const PoseParameters& truth, double x, double y) {
    const Matrix3 m = Mul(render::Invert(model.Homography(placed)), model.Homography(truth));
    return DisplacementUnder(m, frame, x, y);
}

std::string_view RegistrationStateCode(RegistrationState state) {
    switch (state) {
    case RegistrationState::verified: return "verified";
    case RegistrationState::undetermined: return "undetermined";
    case RegistrationState::out_of_envelope: return "out-of-envelope";
    }
    throw std::invalid_argument("unknown registration state");
}
std::string_view EnvelopeComponentCode(EnvelopeComponent component) {
    switch (component) {
    case EnvelopeComponent::position: return "position";
    case EnvelopeComponent::rotation: return "rotation";
    case EnvelopeComponent::tilt: return "tilt";
    case EnvelopeComponent::height: return "height";
    case EnvelopeComponent::surface_height: return "surface-height";
    case EnvelopeComponent::surface_tilt: return "surface-tilt";
    }
    throw std::invalid_argument("unknown envelope component");
}
std::string_view CoarseBlockStatusCode(CoarseBlockStatus status) {
    switch (status) {
    case CoarseBlockStatus::too_few_cells: return "too-few-cells";
    case CoarseBlockStatus::low_overlap: return "low-overlap";
    case CoarseBlockStatus::flat: return "flat";
    case CoarseBlockStatus::low_correlation: return "low-correlation";
    case CoarseBlockStatus::search_boundary: return "search-boundary";
    case CoarseBlockStatus::repeating: return "repeating";
    case CoarseBlockStatus::unique: return "unique";
    case CoarseBlockStatus::resolved: return "resolved";
    }
    throw std::invalid_argument("unknown coarse block status");
}
std::string_view SecondStartCode(RegistrationDiagnostics::SecondStart value) {
    switch (value) {
    case RegistrationDiagnostics::SecondStart::not_run: return "not-run";
    case RegistrationDiagnostics::SecondStart::same_start: return "same-start";
    case RegistrationDiagnostics::SecondStart::agreed: return "agreed";
    case RegistrationDiagnostics::SecondStart::disagreed: return "disagreed";
    case RegistrationDiagnostics::SecondStart::out_of_reach: return "out-of-reach";
    }
    throw std::invalid_argument("unknown second start state");
}
std::string_view SurfaceStateCode(SurfaceState state) {
    switch (state) {
    case SurfaceState::disabled: return "disabled";
    case SurfaceState::not_estimated: return "not-estimated";
    case SurfaceState::estimated: return "estimated";
    }
    throw std::invalid_argument("unknown surface state");
}

PoseDisplay DisplayOf(const PoseParameters& pose, const PoseModel& model) {
    PoseDisplay d;
    d.tx_mm = pose.tx_mm; d.ty_mm = pose.ty_mm; d.position_mm = std::hypot(pose.tx_mm, pose.ty_mm);
    d.rotation_deg = pose.rz_deg;
    d.rx_deg = pose.rx_deg; d.ry_deg = pose.ry_deg; d.tilt_deg = std::hypot(pose.rx_deg, pose.ry_deg);
    d.height_percent = pose.tz_percent;
    d.height_mm = pose.tz_percent/100.0*model.PivotDepthMm();
    return d;
}

SurfaceDisplay DisplayOfSurface(const SurfacePlane& plane, const SurfaceFrame& frame) {
    SurfaceDisplay d;
    d.height_mm = plane.h0_mm;
    const double mean_height = 0.5*(frame.camera_a_height_mm+frame.camera_b_height_mm);
    d.baseline_equivalent_mm = plane.h0_mm*frame.baseline_length_mm/(mean_height-plane.h0_mm);
    d.tilt_deg = std::atan(std::hypot(plane.slope_x, plane.slope_y))/kDegrees;
    d.tilt_x_deg = std::atan(plane.slope_x)/kDegrees;
    d.tilt_y_deg = std::atan(plane.slope_y)/kDegrees;
    d.scale_factor = d.scale_minimum = d.scale_maximum = frame.camera_a_height_mm/(frame.camera_a_height_mm-plane.h0_mm);
    return d;
}

namespace {
// Where a document homography moves the document point (mm).
Point MovementMm(const Matrix3& g, Point doc) {
    const double w = g[6]*doc.x+g[7]*doc.y+g[8];
    return {(g[0]*doc.x+g[1]*doc.y+g[2])/w-doc.x, (g[3]*doc.x+g[4]*doc.y+g[5])/w-doc.y};
}
Point Unit(Point v) {
    const double n = std::hypot(v.x, v.y);
    return n > 1e-12 ? Point{v.x/n, v.y/n} : Point{0, 0};
}
double Wrap(double degrees) {
    double v = std::fmod(degrees, 360.0);
    if (v <= -180.0) v += 360.0;
    if (v > 180.0) v -= 360.0;
    return v;
}
}

OutputFrameDisplay OutputFrameOf(const render::CameraProjection& camera_a, const render::CameraProjection& camera_b, const render::OutputRaster& raster,
    std::uint32_t seam_column, const PoseParameters& base, const PoseParameters& pose, const SurfacePlane& surface) {
    OutputFrameDisplay out;
    const PoseModel model_b(camera_b);
    const Point view = model_b.PivotDocumentMm();   // B's view centre (calibrated; near the base's for a base inside the envelope)
    const Matrix3 base_inverse = render::Invert(model_b.Homography(base));
    const auto moved_by = [&](PoseParameters part) { return Mul(base_inverse, model_b.Homography(part)); };
    PoseParameters translation = base, tilt = base, turn = base;
    translation.tx_mm = pose.tx_mm; translation.ty_mm = pose.ty_mm;
    tilt.rx_deg = pose.rx_deg; tilt.ry_deg = pose.ry_deg;
    turn.rz_deg = pose.rz_deg;
    const Point moved = MovementMm(moved_by(translation), view);
    out.camera_motion_mm = {-moved.x, -moved.y};
    // A tilt turns about the pivot, so the view centre stays and the camera centre moves; the axis still passes through
    // the pivot, so the lens leans opposite to the centre's shift over the drawing.
    const Centre centre_base = CentreOf(model_b.Centre(base));
    const Centre centre_tilted = CentreOf(model_b.Centre(tilt));
    out.lens_direction = Unit({centre_base.xy.x-centre_tilted.xy.x, centre_base.xy.y-centre_tilted.xy.y});
    out.lens_tilt_deg = std::hypot(pose.rx_deg-base.rx_deg, pose.ry_deg-base.ry_deg);
    {
        const Matrix3 g = moved_by(turn);
        constexpr double kArm = 10.0;
        const Point a = MovementMm(g, view), b = MovementMm(g, {view.x+kArm, view.y});
        const double turned = std::atan2(b.y-a.y, kArm+b.x-a.x)/kDegrees;
        out.rotation_clockwise_deg = -turned;
    }
    out.surface_high_side = Unit({surface.slope_x, surface.slope_y});
    if (surface.h0_mm != 0) {
        // The B motion that makes the shift of h0 at the plane origin (registration_pose's Pi, with B's centre after the move).
        const Point origin = SurfaceOriginMm(raster, seam_column);
        const Centre a = CalibratedCentreOf(camera_a);
        const Centre b = CentreOf(model_b.Centre(pose));
        const Point shift = MovementMm(SurfaceHomography(CameraCentreOf(a), CameraCentreOf(b), origin, {surface.h0_mm, 0, 0}), origin);
        out.baseline_equivalent_motion_mm = {-shift.x, -shift.y};
    }
    return out;
}

CameraOrientation OrientationOf(const render::CameraProjection& camera) {
    CameraOrientation o;
    const Centre c = CalibratedCentreOf(camera);
    o.centre_mm = c.xy; o.height_mm = c.height;
    const Matrix3 inverse = render::Invert(camera.document_to_image);
    const auto document_of = [&](double u, double v) {
        const double w = inverse[6]*u+inverse[7]*v+inverse[8];
        return Point{(inverse[0]*u+inverse[1]*v+inverse[2])/w, (inverse[3]*u+inverse[4]*v+inverse[5])/w};
    };
    // The optical axis meets the table where the principal point looks (the lens leaves the principal point in place).
    const auto& i = camera.intrinsics;
    const Point axis = document_of(i.cx_pixels, i.cy_pixels);
    const Point lean{axis.x-c.xy.x, axis.y-c.xy.y};
    o.tilt_deg = std::atan2(std::hypot(lean.x, lean.y), c.height)/kDegrees;
    o.lens_direction = Unit(lean);
    // The image x axis over the drawing, a small step from the principal point.
    const Point step = document_of(i.cx_pixels+0.01*i.fx_pixels, i.cy_pixels);
    o.rotation_clockwise_deg = Wrap(std::atan2(step.y-axis.y, step.x-axis.x)/kDegrees);
    return o;
}

RelativeOrientation RelativeOrientationOf(const CameraOrientation& a, const CameraOrientation& b) {
    RelativeOrientation r;
    r.rotation_clockwise_deg = Wrap(b.rotation_clockwise_deg-a.rotation_clockwise_deg);
    // Unit optical axes toward the table: (tan(tilt) direction, -1), normalized.
    const auto axis = [](const CameraOrientation& o) {
        const double t = std::tan(o.tilt_deg*kDegrees);
        const double n = std::sqrt(1+t*t);
        return std::array<double,3>{t*o.lens_direction.x/n, t*o.lens_direction.y/n, -1/n};
    };
    const auto ua = axis(a), ub = axis(b);
    const double dot = ua[0]*ub[0]+ua[1]*ub[1]+ua[2]*ub[2];
    const double cx = ua[1]*ub[2]-ua[2]*ub[1], cy = ua[2]*ub[0]-ua[0]*ub[2], cz = ua[0]*ub[1]-ua[1]*ub[0];
    r.tilt_deg = std::atan2(std::sqrt(cx*cx+cy*cy+cz*cz), dot)/kDegrees;
    r.tilt_direction = Unit({ub[0]-ua[0], ub[1]-ua[1]});
    return r;
}

void ValidateRegistrationSettings(const RegistrationSettings& s) {
    const auto& e = s.envelope;
    const auto positive = [](double v) { return std::isfinite(v) && v > 0; };
    Require(positive(e.position_mm) && positive(e.rotation_deg) && positive(e.tilt_deg) && positive(e.height_percent),
        "content registration: the envelope is not set");
    const auto& l = s.limits;
    Require(std::isfinite(l.brown_lowe_alpha) && l.brown_lowe_alpha >= 0 && std::isfinite(l.brown_lowe_beta) && l.brown_lowe_beta >= 0 && l.brown_lowe_beta < 1,
        "content registration: the Brown and Lowe constants are not set");
    Require(l.minimum_unique_blocks >= 1, "content registration: the unique block count is not set");
    Require(positive(l.maximum_prediction_sigma_px) && positive(l.maximum_residual_rms_px) && positive(l.maximum_residual_p95_px) &&
        positive(l.maximum_bin_bias_px) && positive(l.convergence_px) && positive(l.start_agreement_px), "content registration: the limits are not set");
    const auto& d = s.design;
    Require(d.coarse_step_px >= 4 && d.coarse_step_px%4 == 0 && d.coarse_block_rows >= d.coarse_step_px && d.coarse_radius_scale >= 1 &&
        d.coarse_maximum_radius_px >= d.coarse_step_px && d.coarse_maximum_radius_px <= 4096 && d.coarse_content_level > 0 && d.coarse_minimum_cells >= 3 &&
        d.coarse_minimum_overlap > 0 && d.coarse_minimum_overlap <= 1 && d.coarse_minimum_correlation > 0 && d.coarse_minimum_correlation < 1 &&
        d.coarse_secondary_ratio > 0 && d.coarse_secondary_ratio <= 1 && positive(d.coarse_consistency_px) && positive(d.coarse_sigma_px),
        "content registration: invalid coarse design constants");
    Require(d.maximum_outer_iterations >= 1 && d.maximum_inner_iterations >= 1 && d.maximum_reweighting_iterations >= 1 && positive(d.tukey_constant) &&
        positive(d.scale_floor_px) && positive(d.sigma_floor_px) && positive(d.prior_sigma_factor) && positive(d.proximity_length_px) && d.minimum_points >= 6 &&
        d.bin_minimum_points >= 1, "content registration: invalid fit design constants");
    (void)FineDesign(d);
    const auto& g = s.geometry;
    Require(g.mixing_width_px >= 1 && g.band_half_width_px > g.exclusion_half_width_px(),
        "content registration: the band must reach past Omega and the guard");
    const auto& b = s.base_pose;
    Require(std::isfinite(b.rx_deg) && std::isfinite(b.ry_deg) && std::isfinite(b.rz_deg) && std::isfinite(b.tx_mm) && std::isfinite(b.ty_mm) &&
        std::isfinite(b.tz_percent), "content registration: the base pose is not finite");
    if (s.surface_envelope) {
        Require(positive(s.surface_envelope->height_mm) && positive(s.surface_envelope->tilt_deg) && s.surface_envelope->tilt_deg < 45,
            "content registration: the surface envelope is not set");
        Require(d.coarse_vote_peaks >= 1 && d.coarse_vote_peaks <= 16 && positive(d.surface_prior_sigma_factor) && d.minimum_points >= 8,
            "content registration: invalid surface design constants");
        Require(d.coarse_alias_peaks >= 2 && d.coarse_alias_peaks <= 4096 && d.coarse_alias_peaks >= d.coarse_vote_peaks && positive(d.coarse_alias_minimum_score) &&
            d.coarse_alias_minimum_score < 1 && positive(d.coarse_alias_margin) && d.coarse_alias_margin <= 1,
            "content registration: invalid constants of the repeating content");
        Require(d.window_group_points >= 1 && positive(d.window_core_bias_px) && positive(d.surface_pose_prior_sigma_factor) &&
            d.window_reach_px >= d.window_core_px && d.window_reach_px >= d.fine_point_spacing_px,
            "content registration: invalid constants of the fit window");
    }
}

RegistrationResult EstimateRegistration(const BgrImage& camera_a, const BgrImage& camera_b, const render::DocumentRenderParameters& parameters,
    const RegistrationSettings& settings, unsigned thread_count) {
    return EstimateRegistration(camera_a, camera_b, parameters, settings, thread_count, RegistrationControl{});
}

RegistrationResult EstimateRegistration(const BgrImage& camera_a, const BgrImage& camera_b, const render::DocumentRenderParameters& parameters,
    const RegistrationSettings& settings, unsigned thread_count, const RegistrationControl& control) {
    ValidateRegistrationSettings(settings);
    Require(thread_count >= 1 && thread_count <= 256, "content registration: thread count must be 1..256");
    for (const auto* image : {&camera_a, &camera_b})
        Require(image->bgr.size() == render::PixelCount(image->width, image->height)*3, "content registration: BGR buffer differs from dimensions");
    Require(parameters.layout == StitchLayout::camera_a_left_camera_b_right, "content registration: only the A-left, B-right layout is supported");
    Require(settings.geometry.seam_column < parameters.raster.width_pixels, "content registration: the seam column is outside the raster");
    const PoseModel pose_model(parameters.camera_b);
    Scene scene;
    scene.width = parameters.raster.width_pixels; scene.height = parameters.raster.height_pixels;
    scene.frame = FrameOfRaster(parameters.raster);
    scene.map_a = render::DocumentOutputToCamera(parameters.raster, parameters.camera_a);
    scene.map_b0 = render::DocumentOutputToCamera(parameters.raster, parameters.camera_b);
    scene.pose_model = &pose_model;
    scene.place = [&parameters](const State& state, const Model& model) {
        return HomographyCorrectedDocumentOutputToCamera(parameters.raster, parameters.camera_b, model.Transform(state));
    };
    scene.document = &parameters;
    return EstimateOn(camera_a, camera_b, scene, settings, thread_count, control);
}

RegistrationGeometry ScaleRegistrationGeometry(std::uint32_t grid_seam_column, std::uint32_t grid_mixing_width_px, std::uint32_t omega_margin_px,
    std::uint32_t guard_px, std::uint32_t band_half_width_px, double scale, std::uint32_t measurer_patch_px) {
    Require(std::isfinite(scale) && scale > 0 && scale <= 1, "content registration: the scale of a grid must be in (0, 1]");
    const auto up = [scale](std::uint32_t v) { return static_cast<std::uint32_t>(std::ceil(static_cast<double>(v)*scale)); };
    RegistrationGeometry g;
    g.seam_column = grid_seam_column;
    g.mixing_width_px = grid_mixing_width_px;
    g.omega_margin_px = up(omega_margin_px);
    g.guard_px = std::max(up(guard_px), measurer_patch_px/2+1);
    g.band_half_width_px = static_cast<std::uint32_t>(std::floor(static_cast<double>(band_half_width_px)*scale));
    return g;
}

GridRegistrationResult EstimateRegistrationOnGrid(const BgrImage& camera_a, const BgrImage& camera_b, const RegistrationGrid& grid,
    const GridRegistrationSettings& settings, const GridRegistrationRequest& request, unsigned thread_count, const RegistrationControl& control) {
    ValidateRegistrationSettings(settings.estimator);
    gate::ValidateSeamGateDesign(settings.measurer);
    const auto& dz = settings.estimator.design;
    Require(settings.measurer.point_spacing_px == dz.fine_point_spacing_px && settings.measurer.patch_px == dz.fine_patch_px &&
        settings.measurer.search_radius_px == dz.fine_search_radius_px && settings.measurer.maximum_search_radius_px == dz.fine_maximum_search_radius_px,
        "content registration: the fine constants differ from the measurer's");
    Require(2*settings.estimator.geometry.guard_px > settings.measurer.patch_px, "content registration: the guard does not clear half the measurer's patch");
    Require(std::isfinite(settings.minimum_band_brightness) && settings.minimum_band_brightness >= 0, "content registration: the brightness line is not set");
    Require(thread_count >= 1 && thread_count <= 256, "content registration: thread count must be 1..256");
    for (const auto* image : {&camera_a, &camera_b})
        Require(image->bgr.size() == render::PixelCount(image->width, image->height)*3, "content registration: BGR buffer differs from dimensions");
    Require(grid.width >= 1 && grid.height >= 1, "content registration: the grid is empty");
    (void)render::PixelCount(grid.width, grid.height);
    Require(std::isfinite(grid.frame.left_mm) && std::isfinite(grid.frame.top_mm) && std::isfinite(grid.frame.pitch_mm) && grid.frame.pitch_mm > 0,
        "content registration: the frame of the grid is not set");
    Require(static_cast<bool>(grid.camera_a) && static_cast<bool>(grid.camera_b) && grid.model_b != nullptr, "content registration: the grid has no mapping or model");
    Require(std::isfinite(grid.centre_a.x_mm) && std::isfinite(grid.centre_a.y_mm) && std::isfinite(grid.centre_a.height_mm) &&
        std::isfinite(grid.surface_origin_mm.x) && std::isfinite(grid.surface_origin_mm.y), "content registration: the surface frame of the grid is not finite");
    Require(settings.estimator.geometry.seam_column < grid.width, "content registration: the seam column is outside the grid");
    const State start{request.start_pose, request.start_surface};
    for (const double v : {start.pose.rx_deg, start.pose.ry_deg, start.pose.rz_deg, start.pose.tx_mm, start.pose.ty_mm, start.pose.tz_percent,
            start.plane.h0_mm, start.plane.slope_x, start.plane.slope_y})
        Require(std::isfinite(v), "content registration: the start of a follow-up is not finite");

    GridRegistrationResult out;
    Scene scene;
    scene.width = grid.width; scene.height = grid.height;
    scene.frame = grid.frame;
    scene.map_a = grid.camera_a;
    scene.map_b0 = grid.camera_b(PoseParameters{}, SurfacePlane{});
    scene.pose_model = grid.model_b;
    scene.place = [&grid](const State& state, const Model&) { return grid.camera_b(state.pose, state.plane); };
    scene.centre_a = CentreOf(grid.centre_a);
    scene.reference_mm = grid.surface_origin_mm;
    scene.reference_px = {(grid.surface_origin_mm.x-grid.frame.left_mm)/grid.frame.pitch_mm-0.5, (grid.surface_origin_mm.y-grid.frame.top_mm)/grid.frame.pitch_mm-0.5};
    scene.measurer = &settings.measurer;
    if (request.job == GridJob::follow) scene.follow = &start;
    scene.minimum_band_brightness = settings.minimum_band_brightness;
    scene.report = &out;
    out.registration = EstimateOn(camera_a, camera_b, scene, settings.estimator, thread_count, control);
    const auto& r = out.registration;
    out.stopped = std::find(r.reasons.begin(), r.reasons.end(), "time-budget") != r.reasons.end();
    if (request.job == GridJob::follow && r.has_estimate) {
        out.change_px = r.diagnostics.last_change_px;
        if (r.diagnostics.points > 0) out.inlier_fraction = static_cast<double>(r.diagnostics.inliers)/static_cast<double>(r.diagnostics.points);
    }
    return out;
}

namespace {
RegistrationResult EstimateOn(const BgrImage& camera_a, const BgrImage& camera_b, const Scene& scene, const RegistrationSettings& settings,
    unsigned thread_count, const RegistrationControl& control) {
    const auto& dz = settings.design;
    const auto& geometry = settings.geometry;
    const auto stopped = [&control]() { return static_cast<bool>(control.stop) && control.stop(); };
    // A grid asks the caller's stop at more places (a capture keeps the places it had).
    const auto stopped_on_grid = [&]() { return scene.Grid() && stopped(); };

    RegistrationResult result;
    const PoseModel& pose_model = *scene.pose_model;
    const PlaneFrame frame = scene.frame;
    const OutputToCamera& map_a = scene.map_a;
    const OutputToCamera& map_b0 = scene.map_b0;

    Layout layout;
    layout.width = scene.width; layout.height = scene.height;
    layout.frame = frame;
    gate::SeamGateGeometry base_geometry;
    base_geometry.width = layout.width; base_geometry.height = layout.height; base_geometry.camera_a = map_a; base_geometry.camera_b = map_b0;
    const auto design = scene.measurer ? *scene.measurer : FineDesign(dz);
    auto coverage = CoverageOf(base_geometry, camera_a.width, camera_a.height, camera_b.width, camera_b.height, gate::RequiredBandInsetPx(design), thread_count);
    layout.coverage = std::move(coverage.plain);
    const auto fine_coverage = std::move(coverage.inset);
    layout.window_coarse = MakeWindows(layout.coverage, geometry);
    // Surface mode: the fine stage keeps to the reach of the fit window (the coarse stage uses all of W).
    auto fine_geometry = geometry;
    if (settings.surface_envelope)
        fine_geometry.band_half_width_px = static_cast<std::uint32_t>(std::min<std::uint64_t>(geometry.band_half_width_px,
            static_cast<std::uint64_t>(geometry.exclusion_half_width_px())+dz.window_reach_px));
    layout.window_fine = MakeWindows(fine_coverage, fine_geometry);
    const auto empty_window = [](const std::vector<RowWindow>& w) {
        return std::all_of(w.begin(), w.end(), [](const RowWindow& r) { return r.left_begin >= r.left_end && r.right_begin >= r.right_end; });
    };
    if (settings.surface_envelope) result.surface.state = result.surface.estimate.state = SurfaceState::not_estimated;
    if (scene.document) {
        result.orientation.camera_a = OrientationOf(scene.document->camera_a);
        result.orientation.camera_b = OrientationOf(scene.document->camera_b);
        result.orientation.b_from_a = RelativeOrientationOf(result.orientation.camera_a, result.orientation.camera_b);
    }
    if (empty_window(layout.window_coarse) || empty_window(layout.window_fine)) { result.reasons.push_back("no-overlap"); return result; }

    // Rows where the seam column is covered: the reference point of the surface is the middle of them.
    std::uint32_t seam_first = layout.height, seam_last = 0;
    for (std::uint32_t y = 0; y < layout.height; ++y) {
        const auto& c = layout.coverage[y];
        if (c.begin < c.end && geometry.seam_column >= c.begin && geometry.seam_column < c.end) { seam_first = std::min(seam_first, y); seam_last = y; }
    }

    Model model;
    model.pose_model = &pose_model;
    model.surface = settings.surface_envelope.has_value();
    model.base = settings.base_pose;
    model.base_zero = settings.base_pose.IsZero();
    SurfaceFrame surface_frame;
    if (model.surface) {
        if (scene.document) {
            model.centre_a = CalibratedCentreOf(scene.document->camera_a);
            // The plane origin: the middle of the seam column (S1's SurfaceOriginMm convention).
            const double reference_row = (static_cast<double>(layout.height)-1.0)/2.0;
            surface_frame.reference_px = {static_cast<double>(geometry.seam_column), reference_row};
            surface_frame.reference_mm = SurfaceOriginMm(scene.document->raster, geometry.seam_column);
        } else {
            // A grid: the plane origin is the condition's (the caller's), so the surface means what it means in the capture.
            model.centre_a = scene.centre_a;
            surface_frame.reference_px = scene.reference_px;
            surface_frame.reference_mm = scene.reference_mm;
        }
        model.reference_mm = surface_frame.reference_mm;
        // The gauge: the free translation is the one that moves the band across the baseline at the reference point.
        const Centre centre_b = model.CentreB(model.base);
        const Point baseline{centre_b.xy.x-model.centre_a.xy.x, centre_b.xy.y-model.centre_a.xy.y};
        const double length = std::hypot(baseline.x, baseline.y);
        Require(length > 0, "content registration: the cameras share a centre");
        const Point e{baseline.x/length, baseline.y/length}, across{-e.y, e.x};
        const Matrix3 base_inverse = render::Invert(pose_model.Homography(model.base));
        double jt[2][2]{};
        constexpr double kTranslationStep = 1e-2;
        for (int k = 0; k < 2; ++k) {
            PoseParameters up = model.base, down = model.base;
            (k == 0 ? up.tx_mm : up.ty_mm) += kTranslationStep; (k == 0 ? down.tx_mm : down.ty_mm) -= kTranslationStep;
            const Point a = DisplacementUnder(Mul(base_inverse, pose_model.Homography(up)), frame, surface_frame.reference_px.x, surface_frame.reference_px.y);
            const Point b = DisplacementUnder(Mul(base_inverse, pose_model.Homography(down)), frame, surface_frame.reference_px.x, surface_frame.reference_px.y);
            jt[0][k] = (a.x-b.x)/(2*kTranslationStep); jt[1][k] = (a.y-b.y)/(2*kTranslationStep);
        }
        const double det = jt[0][0]*jt[1][1]-jt[0][1]*jt[1][0];
        Require(std::abs(det) > 0, "content registration: the translation does not move the band");
        Point w{(jt[1][1]*across.x-jt[0][1]*across.y)/det, (-jt[1][0]*across.x+jt[0][0]*across.y)/det};
        const double wn = std::hypot(w.x, w.y);
        model.gauge = {w.x/wn, w.y/wn};
        result.diagnostics.gauge_direction = model.gauge;
    }
    // B at the base pose (the zero plane: the homography is the pose's own).
    const OutputToCamera map_b_base = model.base_zero ? map_b0 : scene.place(model.Base(), model);

    auto& diag = result.diagnostics;

    // A grid: how bright the band is, and nothing is estimated from a band that is too dark (a live view at night).
    if (scene.report) {
        scene.report->band_brightness_a = BandBrightness(camera_a, map_a, layout.window_coarse);
        scene.report->band_brightness_b = BandBrightness(camera_b, map_b_base, layout.window_coarse);
        const auto dark = [&](double v) { return !(v >= scene.minimum_band_brightness); };
        if (dark(scene.report->band_brightness_a) || dark(scene.report->band_brightness_b)) { result.reasons.push_back("too-dark"); return result; }
    }

    // ---- Stage 1. A follow-up has none: it starts from the state it was given.
    State start_state = model.Base();
    if (scene.follow) {
        start_state = *scene.follow;
    } else {
        if (stopped_on_grid()) { result.reasons.push_back("time-budget"); return result; }
        const auto coarse = CoarseStage(camera_a, camera_b, map_a, map_b_base, layout, model, settings, thread_count);
        diag.coarse_radius_x_px = coarse.radius_x; diag.coarse_radius_y_px = coarse.radius_y;
        diag.coarse_blocks = coarse.blocks; diag.unique_blocks = coarse.unique; diag.consistent_blocks = coarse.consistent;
        diag.anchor_blocks = coarse.anchors; diag.consistent_anchor_blocks = coarse.consistent_anchors;
        diag.coarse_vote = coarse.vote; diag.coarse_shift = coarse.shift;
        if (!coarse.ok) {
            result.reasons.push_back(coarse.reason.empty() ? "coarse-no-unique-blocks" : coarse.reason);
            return result;
        }
        diag.coarse_pose = coarse.state.pose;
        start_state = coarse.state;
    }

    const auto par = MakeParametrization(model, settings);
    const unsigned n = par.n;

    // Output positions at which two states are compared: the seam column.
    std::vector<Point> samples;
    for (std::uint32_t y = 0; y < layout.height; y += 32) {
        const auto& c = layout.coverage[y];
        if (c.begin < c.end && geometry.seam_column >= c.begin && geometry.seam_column < c.end) samples.push_back({static_cast<double>(geometry.seam_column), static_cast<double>(y)});
    }
    // How far two states are apart where it matters: the displacement between them along the seam column.
    const auto difference = [&](const State& placed, const State& truth) {
        const Matrix3 m = Mul(render::Invert(model.Transform(placed)), model.Transform(truth));
        double worst = 0;
        for (const auto& s : samples) { const Point d = DisplacementUnder(m, frame, s.x, s.y); worst = std::max(worst, std::hypot(d.x, d.y)); }
        return worst;
    };

    // The uncertainty of the predicted displacement along the seam column (the quantity of V3): the largest one sigma over
    // the covered rows, from the covariance of a fit by numerical derivatives.
    const auto prediction_sigma = [&](const State& fitted, const std::vector<double>& covariance, std::uint32_t& rows_checked) {
        // Steps of the numerical derivative: pose-only rx ry rz tx ty tz; surface rx ry rz s tz h0 g_x g_y.
        constexpr double kStepSize[6]{1e-3, 1e-3, 1e-3, 1e-2, 1e-2, 1e-3};
        constexpr double kSurfaceStepSize[8]{1e-3, 1e-3, 1e-3, 1e-2, 1e-3, 1e-2, 1e-5, 1e-5};
        std::vector<Matrix3> plus(n), minus(n);
        const auto centre = model.ParametersOf(fitted);
        for (unsigned k = 0; k < n; ++k) {
            auto up = centre, down = centre;
            const double h = model.surface ? kSurfaceStepSize[k] : kStepSize[k];
            up[k] += h; down[k] -= h;
            plus[k] = model.Transform(model.StateOf(up)); minus[k] = model.Transform(model.StateOf(down));
        }
        std::vector<double> j0(n), j1(n);
        double worst = 0;
        rows_checked = 0;
        for (std::uint32_t y = 0; y < layout.height; y += 8) {
            const auto& c = layout.coverage[y];
            if (!(c.begin < c.end && geometry.seam_column >= c.begin && geometry.seam_column < c.end)) continue;
            for (unsigned k = 0; k < n; ++k) {
                const double h = model.surface ? kSurfaceStepSize[k] : kStepSize[k];
                const Point a = DisplacementUnder(plus[k], frame, geometry.seam_column, y), b = DisplacementUnder(minus[k], frame, geometry.seam_column, y);
                j0[k] = (a.x-b.x)/(2*h); j1[k] = (a.y-b.y)/(2*h);
            }
            const std::vector<double>* const rows_of[2]{&j0, &j1};
            double v[2][2]{};
            for (unsigned r = 0; r < 2; ++r) for (unsigned q = 0; q < 2; ++q) {
                double sum = 0;
                for (unsigned a = 0; a < n; ++a) for (unsigned b = 0; b < n; ++b) sum += (*rows_of[r])[a]*covariance[a*n+b]*(*rows_of[q])[b];
                v[r][q] = sum;
            }
            const double mean = (v[0][0]+v[1][1])/2, spread = std::sqrt(std::pow((v[0][0]-v[1][1])/2, 2)+v[0][1]*v[1][0]);
            worst = std::max(worst, std::sqrt(std::max(0.0, mean+spread)));
            ++rows_checked;
        }
        return worst;
    };

    // ---- Stages 2 and 3 from one start.
    // `window`, when given, is the fit window to use from the first measurement on (the second start takes the first's).
    // A follow-up measures and fits once.
    const std::uint32_t outer_limit = scene.follow ? 1u : dz.maximum_outer_iterations;
    // The measurer's first search radius and the largest it widens to, for a run. Both starts begin at the design's first radius. The
    // first (placed by the coarse stage) may widen as far as the design's maximum. The second is the calibrated pose, which the coarse
    // stage did not place, so the whole displacement of the pose is still to be found: in a capture it widens as far as the second-start
    // radius (the reach V6 had before the first radius was cut), no farther. A grid keeps its measurer's constants for both.
    const unsigned second_radius = scene.Grid() ? design.search_radius_px : std::clamp(dz.second_start_search_radius_px, design.search_radius_px, design.maximum_search_radius_px);
    const auto run = [&](State start, unsigned search_radius, unsigned fine_maximum_radius, unsigned threads, const FitWindow* window) {
        Run r;
        if (window) r.window = *window;
        State current = start;
        auto run_design = design;
        run_design.search_radius_px = search_radius;
        run_design.maximum_search_radius_px = std::max(search_radius, fine_maximum_radius);
        for (std::uint32_t k = 0; k < outer_limit; ++k) {
            if (stopped()) { r.reason = "time-budget"; r.outer = k; return r; }
            const bool calibrated = model.Calibrated(current);
            OutputToCamera placed;
            if (calibrated) placed = map_b0;
            else if (!scene.Grid()) placed = scene.place(current, model);
            else {
                // A grid's maker refuses a state that it cannot place (a surface that reaches a camera).
                try { placed = scene.place(current, model); }
                catch (const std::invalid_argument&) { r.reason = "fit-failed"; r.outer = k; r.placed = current; return r; }
            }
            r.placed = current;
            // Surface mode: the window is found on the first two measurements (the first is placed by the coarse start, which
            // can be several pixels off) and kept after that, so that the change between two iterations is the change of
            // the fit and not of the window. A kept window is also all that is measured.
            const FitWindow* const held = model.surface && (window || k >= 2) ? &r.window : nullptr;
            auto measured = MeasureFine(camera_a, camera_b, layout, map_a, placed, settings, run_design, threads, threads >= 2, held);
            r.gate_points = measured.gate_points; r.contradicted = measured.contradicted;
            r.points = std::move(measured.points);
            if (measured.obs.size() < dz.minimum_points) { r.reason = "too-few-points"; r.outer = k+1; r.obs = std::move(measured.obs); return r; }
            if (stopped_on_grid()) { r.reason = "time-budget"; r.outer = k+1; r.obs = std::move(measured.obs); return r; }
            const Matrix3 place_inverse = calibrated ? kIdentity : render::Invert(model.Transform(current));
            Fit fit;
            if (model.surface) {
                // The paper is one plane only as far as the next fold: the fit grows outward from the seam and stops there.
                auto grown = FitGrown(measured.obs, par, frame, place_inverse, model.ParametersOf(current), settings, held);
                r.window = grown.window;
                fit = std::move(grown.fit);
                if (fit.ok) measured.obs = std::move(grown.obs);
            } else {
                fit = FitModel(measured.obs, par, frame, place_inverse, model.ParametersOf(current), dz, true);
            }
            r.outer = k+1;
            if (!fit.ok) { r.reason = "fit-failed"; r.obs = std::move(measured.obs); return r; }
            const State next = model.StateOf(fit.parameters);
            r.last_change = difference(current, next);
            r.changes.push_back(r.last_change);
            // Surface mode: a change below the estimate's own one-sigma uncertainty at the seam is the scatter of the
            // measurement, not a movement of the fit (on paper that is not flat it does not go away with more iterations).
            double settled = settings.limits.convergence_px;
            if (model.surface) { std::uint32_t rows{}; settled = std::max(settled, prediction_sigma(next, fit.covariance, rows)); }
            r.obs = std::move(measured.obs);
            r.fit = std::move(fit);
            r.state = next;
            current = next;
            if (r.last_change <= settled) { r.converged = true; break; }
        }
        r.ok = true;
        return r;
    };

    // V6 needs a second run from the base. Pose-only mode: it does not depend on the first, so the two run concurrently.
    // Surface mode: it is fitted over the first's window (the two starts are compared on the same columns, and only those
    // are measured), so it runs after the first.
    const State base_state = model.Base();
    const bool same_start = SamePose(start_state.pose, base_state.pose) && start_state.plane.IsZero();
    const bool concurrent = !same_start && thread_count >= 2 && !model.surface && !scene.follow;
    const unsigned run_threads = concurrent ? std::max(1u, thread_count/2) : thread_count;
    Run first, second;
    bool second_ran = false;
    Both(concurrent,
        [&]() { first = run(start_state, design.search_radius_px, design.maximum_search_radius_px, run_threads, nullptr); },
        [&]() { if (concurrent) { second = run(base_state, design.search_radius_px, second_radius, run_threads, nullptr); second_ran = true; } });
    diag.outer_iterations = first.outer; diag.gate_points = first.gate_points; diag.global_contradicted = first.contradicted;
    diag.points = static_cast<std::uint32_t>(first.obs.size());
    diag.last_placement_calibrated = model.Calibrated(first.placed);
    diag.last_placement = diag.last_placement_calibrated ? kIdentity : model.Transform(first.placed);
    diag.last_points = first.points;
    diag.fit_window = first.window;
    if (!first.ok) { result.reasons.push_back(first.reason); return result; }
    const State fitted = first.state;
    const PoseParameters pose = fitted.pose;
    result.has_estimate = true;
    result.pose = pose;
    if (!model.surface) {
        std::copy(first.fit.covariance.begin(), first.fit.covariance.end(), result.covariance.begin());
    } else {
        // The pose covariance from the joint one: tx = s gauge.x, ty = s gauge.y (plus the base).
        result.joint_covariance = first.fit.covariance;
        double j[6][8]{};
        j[0][0] = j[1][1] = j[2][2] = 1; j[3][3] = model.gauge.x; j[4][3] = model.gauge.y; j[5][4] = 1;
        for (unsigned a = 0; a < 6; ++a) for (unsigned b = 0; b < 6; ++b) {
            double sum = 0;
            for (unsigned k = 0; k < 8; ++k) for (unsigned l = 0; l < 8; ++l) sum += j[a][k]*first.fit.covariance[k*8+l]*j[b][l];
            result.covariance[a*6+b] = sum;
        }
        for (unsigned a = 0; a < 3; ++a) for (unsigned b = 0; b < 3; ++b) result.surface.covariance[a*3+b] = first.fit.covariance[(5+a)*8+(5+b)];
        result.surface.state = SurfaceState::estimated;
        result.surface.plane = fitted.plane;
        const auto sd = [&](unsigned i) { return std::sqrt(std::max(0.0, result.surface.covariance[i*3+i])); };
        result.surface.sigma = {sd(0), sd(1), sd(2)};
        const Centre centre_b = model.CentreB(pose);
        surface_frame.camera_a_mm = model.centre_a.xy; surface_frame.camera_a_height_mm = model.centre_a.height;
        surface_frame.camera_b_mm = centre_b.xy; surface_frame.camera_b_height_mm = centre_b.height;
        const Point baseline{centre_b.xy.x-model.centre_a.xy.x, centre_b.xy.y-model.centre_a.xy.y};
        surface_frame.baseline_length_mm = std::hypot(baseline.x, baseline.y);
        surface_frame.baseline = {baseline.x/surface_frame.baseline_length_mm, baseline.y/surface_frame.baseline_length_mm};
        surface_frame.parallax_px_per_mm = surface_frame.baseline_length_mm/frame.pitch_mm/(0.5*(model.centre_a.height+centre_b.height)-fitted.plane.h0_mm);
        result.surface.frame = surface_frame;
        result.surface.display = DisplayOfSurface(fitted.plane, surface_frame);
        // The range of A's scale over the output raster: the height is affine, so the extremes are at the corners.
        for (const double x : {0.0, static_cast<double>(layout.width)-1.0}) for (const double y : {0.0, static_cast<double>(layout.height)-1.0}) {
            const Point mm = DocumentOf(frame, x, y);
            const double h = fitted.plane.h0_mm+fitted.plane.slope_x*(mm.x-surface_frame.reference_mm.x)+fitted.plane.slope_y*(mm.y-surface_frame.reference_mm.y);
            const double scale = model.centre_a.height/(model.centre_a.height-h);
            result.surface.display.scale_minimum = std::min(result.surface.display.scale_minimum, scale);
            result.surface.display.scale_maximum = std::max(result.surface.display.scale_maximum, scale);
        }
        auto& estimate = result.surface.estimate;
        estimate.state = SurfaceState::estimated;
        estimate.plane = fitted.plane;
        estimate.tilt_deg = result.surface.display.tilt_deg;
        estimate.covariance = result.surface.covariance;
        estimate.baseline_equivalent_mm = result.surface.display.baseline_equivalent_mm;
        estimate.scale = {result.surface.display.scale_factor, result.surface.display.scale_minimum, result.surface.display.scale_maximum};
    }
    const auto sigma_of = [&](unsigned i) { return std::sqrt(std::max(0.0, result.covariance[i*6+i])); };
    result.pose_sigma = {sigma_of(0), sigma_of(1), sigma_of(2), sigma_of(3), sigma_of(4), sigma_of(5)};
    const PoseParameters from_base = model.FromBase(pose);
    result.display = DisplayOf(from_base, pose_model);
    result.document_homography = model.Transform(fitted);
    if (scene.document) {
        const auto& parameters = *scene.document;
        result.display.output_frame = OutputFrameOf(parameters.camera_a, parameters.camera_b, parameters.raster, geometry.seam_column, model.base, pose, fitted.plane);
        auto moved = parameters.camera_b;
        moved.document_to_image = pose_model.CameraHomography(pose);
        result.orientation.camera_b = OrientationOf(moved);
        result.orientation.b_from_a = RelativeOrientationOf(result.orientation.camera_a, result.orientation.camera_b);
    }
    diag.converged = first.converged; diag.last_change_px = first.last_change; diag.change_history_px = first.changes;
    diag.inliers = first.fit.inliers; diag.robust_scale_px = first.fit.scale;
    diag.information_condition_number = ConditionNumber(first.fit.information, n);
    if (model.surface) {
        const auto& c = first.fit.covariance;
        for (const unsigned a : {5u, 6u, 7u}) for (const unsigned b : {0u, 1u, 2u, 4u}) {
            const double denominator = std::sqrt(c[a*8+a]*c[b*8+b]);
            if (denominator > 0) diag.maximum_surface_pose_correlation = std::max(diag.maximum_surface_pose_correlation, std::abs(c[a*8+b])/denominator);
        }
    }

    // ---- Stage 4: checks.
    // Residuals of the inliers against the final state. The points were measured with B placed by first.placed, so the
    // prediction is T(placed)^-1 T(fitted), as a displacement.
    const Matrix3 final_matrix = Mul(model.Calibrated(first.placed) ? kIdentity : render::Invert(model.Transform(first.placed)), model.Transform(fitted));
    std::vector<double> magnitudes, across, along;
    struct Bin { double sx{}, sy{}; std::uint32_t n{}; };
    Bin bins[6];
    // Surface mode: the baseline splits the residual into the paper's part (along) and the pose's (across).
    const Point along_unit = model.surface ? result.surface.frame.baseline : Point{1, 0};
    const Point across_unit{-along_unit.y, along_unit.x};
    constexpr double kMostlyAcross = 0.8;   // a one-directional point counts across when its normal is this much across
    double y_low = kInfinity, y_high = -kInfinity;
    // The shape of the residual is judged where the estimate is used: inliers within the proximity length of Omega + guard.
    const double exclusion = geometry.exclusion_half_width_px();
    const auto near_omega = [&](const Obs& o) { return std::abs(o.x-static_cast<double>(geometry.seam_column))-exclusion <= dz.proximity_length_px; };
    for (std::size_t i = 0; i < first.obs.size(); ++i) if (first.fit.rho[i] > 0 && near_omega(first.obs[i])) { y_low = std::min(y_low, first.obs[i].y); y_high = std::max(y_high, first.obs[i].y); }
    diag.observations.reserve(first.obs.size());
    for (std::size_t i = 0; i < first.obs.size(); ++i) {
        const auto& o = first.obs[i];
        const Point predicted = DisplacementUnder(final_matrix, frame, o.x, o.y);
        const double magnitude = ResidualMagnitude(o, predicted);
        diag.observations.push_back({o.x, o.y, o.one, o.n, o.d, o.sigma, magnitude, first.fit.rho[i]});
        if (!(first.fit.rho[i] > 0) || !near_omega(o)) continue;
        magnitudes.push_back(magnitude);
        if (model.surface) {
            if (o.one) {
                (std::abs(o.n.x*across_unit.x+o.n.y*across_unit.y) >= kMostlyAcross ? across : along).push_back(magnitude);
            } else {
                const double rx = o.d.x-predicted.x, ry = o.d.y-predicted.y;
                across.push_back(std::abs(rx*across_unit.x+ry*across_unit.y));
                along.push_back(std::abs(rx*along_unit.x+ry*along_unit.y));
            }
        }
        if (o.one) continue;
        const double position = y_high > y_low ? (o.y-y_low)/(y_high-y_low) : 0.0;
        const int third = std::min(2, static_cast<int>(position*3));
        const int side = o.x < static_cast<double>(geometry.seam_column) ? 0 : 1;
        auto& bin = bins[side*3+third];
        bin.sx += o.d.x-predicted.x; bin.sy += o.d.y-predicted.y; ++bin.n;
    }
    diag.residual = Summarize(magnitudes);
    for (const auto& bin : bins) if (bin.n >= dz.bin_minimum_points) {
        diag.maximum_bin_bias_px = std::max(diag.maximum_bin_bias_px, std::hypot(bin.sx/bin.n, bin.sy/bin.n));
        if (!model.surface) continue;
        diag.maximum_bin_bias_across_px = std::max(diag.maximum_bin_bias_across_px, std::abs(bin.sx*across_unit.x+bin.sy*across_unit.y)/bin.n);
        diag.maximum_bin_bias_along_px = std::max(diag.maximum_bin_bias_along_px, std::abs(bin.sx*along_unit.x+bin.sy*along_unit.y)/bin.n);
    }
    if (model.surface) { diag.residual_across = Summarize(across); diag.residual_along = Summarize(along); }

    // V1 Brown and Lowe.
    diag.brown_lowe_threshold = settings.limits.brown_lowe_alpha+settings.limits.brown_lowe_beta*static_cast<double>(diag.points);

    // V3: uncertainty of the predicted displacement along the seam column, every covered row.
    diag.maximum_prediction_sigma_px = prediction_sigma(fitted, first.fit.covariance, diag.seam_rows_checked);

    // V4: the envelope, per component, of p-hat - p_b (and the surface).
    {
        const PoseDisplay& d = result.display;
        const struct { EnvelopeComponent component; double value, limit; } rows[4]{
            {EnvelopeComponent::position, d.position_mm, settings.envelope.position_mm},
            {EnvelopeComponent::rotation, std::abs(d.rotation_deg), settings.envelope.rotation_deg},
            {EnvelopeComponent::tilt, d.tilt_deg, settings.envelope.tilt_deg},
            {EnvelopeComponent::height, std::abs(d.height_percent), settings.envelope.height_percent}};
        for (const auto& r : rows) {
            ComponentCheck c{r.component, r.value, r.limit, r.value > r.limit};
            result.components.push_back(c);
            if (c.exceeded) result.exceeded.push_back(c.component);
        }
        if (model.surface) {
            const ComponentCheck checks[2]{
                {EnvelopeComponent::surface_height, std::abs(fitted.plane.h0_mm), settings.surface_envelope->height_mm, std::abs(fitted.plane.h0_mm) > settings.surface_envelope->height_mm},
                {EnvelopeComponent::surface_tilt, result.surface.display.tilt_deg, settings.surface_envelope->tilt_deg, result.surface.display.tilt_deg > settings.surface_envelope->tilt_deg}};
            for (const auto& c : checks) { result.surface_components.push_back(c); if (c.exceeded) result.exceeded.push_back(c.component); }
            result.surface.estimate.exceeded = {checks[0].exceeded, checks[1].exceeded};
        }
    }

    // V6: the same estimate from the base as the start. A start at the base cannot see a displacement beyond the
    // measurer's first window, so a state that moves the band further than that is not held against the estimate.
    bool second_stopped = false;
    if (scene.follow) {
        // A follow-up has no second start.
    } else if (same_start) {
        diag.second_start = RegistrationDiagnostics::SecondStart::same_start;
    } else {
        if (!second_ran) second = run(base_state, design.search_radius_px, second_radius, thread_count, model.surface ? &first.window : nullptr);
        second_stopped = !second.ok && second.reason == "time-budget";
        double need = 0;
        {
            const Matrix3 g = model.base_zero && !model.surface ? pose_model.Homography(pose) : Mul(render::Invert(model.Transform(base_state)), model.Transform(fitted));
            for (const auto& s : samples) { const Point d = DisplacementUnder(g, frame, s.x, s.y); need = std::max(need, std::hypot(d.x, d.y)); }
            for (const auto& o : first.obs) { const Point d = DisplacementUnder(g, frame, o.x, o.y); need = std::max(need, std::hypot(d.x, d.y)); }
        }
        if (second.ok) {
            diag.second_start_difference_px = difference(fitted, second.state);
            diag.second_start = diag.second_start_difference_px <= settings.limits.start_agreement_px
                ? RegistrationDiagnostics::SecondStart::agreed : RegistrationDiagnostics::SecondStart::disagreed;
        } else {
            diag.second_start = RegistrationDiagnostics::SecondStart::disagreed;
        }
        if (diag.second_start == RegistrationDiagnostics::SecondStart::disagreed && need > 0.75*static_cast<double>(second_radius))
            diag.second_start = RegistrationDiagnostics::SecondStart::out_of_reach;
    }
    if (second_stopped) {
        // The caller stopped the second start: the fit is not kept (design addendum 11.25).
        result.reasons.assign(1, "time-budget");
        result.state = RegistrationState::undetermined;
        result.has_estimate = false;
        return result;
    }

    // Verdict.
    if (scene.follow) {
        // A follow-up is judged on V1 and V3 (design addendum 11.21); the caller judges how far it moved.
        if (!(static_cast<double>(diag.inliers) > diag.brown_lowe_threshold)) result.reasons.push_back("brown-lowe");
        if (!(diag.maximum_prediction_sigma_px <= settings.limits.maximum_prediction_sigma_px) || diag.seam_rows_checked == 0) result.reasons.push_back("prediction-uncertain");
        if (!result.reasons.empty()) result.state = RegistrationState::undetermined;
        else result.state = result.exceeded.empty() ? RegistrationState::verified : RegistrationState::out_of_envelope;
        return result;
    }
    if (!diag.converged) result.reasons.push_back("not-converged");
    if (!(static_cast<double>(diag.inliers) > diag.brown_lowe_threshold)) result.reasons.push_back("brown-lowe");
    // V2. Surface mode: the blocks counted include the repeating blocks that follow the vote's winner, and a block that repeats
    // has a strong peak next to almost any place, so it is no evidence of the one the winner gives. The line is therefore held
    // against the anchors that agree with the fit as well: one anchor with followers is not a verified estimate.
    const bool too_few_blocks = diag.consistent_blocks < settings.limits.minimum_unique_blocks || 2*diag.consistent_blocks < diag.unique_blocks;
    const bool too_few_anchors = model.surface && diag.consistent_anchor_blocks < settings.limits.minimum_unique_blocks;
    if (too_few_blocks || too_few_anchors) result.reasons.push_back("unique-blocks");
    if (!(diag.maximum_prediction_sigma_px <= settings.limits.maximum_prediction_sigma_px) || diag.seam_rows_checked == 0) result.reasons.push_back("prediction-uncertain");
    const auto shape_exceeded = [&](const ResidualSummary& r, double bias) {
        return !(r.rms <= settings.limits.maximum_residual_rms_px) || !(r.p95 <= settings.limits.maximum_residual_p95_px) || !(bias <= settings.limits.maximum_bin_bias_px);
    };
    if (model.surface) {
        // V5 on the residual across the baseline. The residual along it is the paper's shape (reported, no reason).
        if (shape_exceeded(diag.residual_across, diag.maximum_bin_bias_across_px)) result.reasons.push_back("residual-shape");
        diag.residual_along_exceeded = shape_exceeded(diag.residual_along, diag.maximum_bin_bias_along_px);
    } else if (shape_exceeded(diag.residual, diag.maximum_bin_bias_px)) {
        result.reasons.push_back("residual-shape");
    }
    if (diag.second_start == RegistrationDiagnostics::SecondStart::disagreed) result.reasons.push_back("starts-disagree");
    if (!result.reasons.empty()) result.state = RegistrationState::undetermined;
    else result.state = result.exceeded.empty() ? RegistrationState::verified : RegistrationState::out_of_envelope;
    return result;
}
}  // namespace
}
