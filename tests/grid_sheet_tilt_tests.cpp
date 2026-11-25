#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/grid_sheet_tilt.hpp"
#include "grid_sheet_scene.hpp"
#include "grid_sheet_tilt_detail.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace gst = a0::m2::experimental::grid_sheet_tilt;
namespace det = a0::m2::experimental::grid_sheet_tilt::detail;
namespace cr = a0::m2::experimental::content_registration;
namespace render = a0::m2::render;
namespace gs = grid_scene;
using render::Point;

namespace {
int checks = 0, failures = 0;
void Check(bool value, const std::string& what) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL " << what << '\n'; }
}
void CheckNear(double value, double expected, double tolerance, const std::string& what) {
    ++checks;
    if (!(std::abs(value - expected) <= tolerance)) {
        ++failures;
        std::cerr << "FAIL " << what << ": " << value << " expected " << expected << " +/- " << tolerance << '\n';
    }
}
constexpr double kPi = std::numbers::pi;
double Seconds(std::chrono::steady_clock::time_point since) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count(); }
Point Rotate(Point v, double degrees) {
    const double c = std::cos(degrees * gs::kDeg), s = std::sin(degrees * gs::kDeg);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}
double Wrap(double d) {
    d = std::fmod(d, 360.0);
    if (d <= -180) d += 360;
    if (d > 180) d -= 360;
    return d;
}
double Dist(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

// ---- settings for pictures drawn at a scale: the numbers that count pixels or points scale with it ------------------------
// The acceptance values are made up for these tests; they are not lines of any product.
gst::GridSheetTiltSettings TestSettings(double scale) {
    auto s = gst::WorkingDraftSettings();
    auto& d = s.detection;
    const auto at_least = [](double v, double floor) { return static_cast<std::uint32_t>(std::max(v, floor)); };
    d.pitch_min_px = 46 * scale; d.pitch_max_px = 128 * scale;
    d.window_min_points = at_least(3000 * scale, 60);
    d.direction_min_points = at_least(1000 * scale, 30);
    d.fit_min_inliers = at_least(200 * scale, 40);
    d.line_min_points = at_least(200 * scale, 30);
    d.coverage_cell_px = at_least(128 * scale, 8);
    d.flat_cell_min_points = at_least(150 * scale, 40);
    d.node_max_gap_px = std::max(15 * scale, 4.0);
    s.acceptance.min_ridge_points = 500;
    s.acceptance.min_lines_per_family = 12;
    s.acceptance.min_lattice_span = 12;
    s.acceptance.min_visible_fraction = 0.02;
    s.acceptance.max_residual_rms_px = 0.5;
    s.acceptance.max_method_spread_deg = 0.3;
    s.acceptance.max_off_lattice_cell_fraction = 0.1;
    s.acceptance.off_lattice_offset_px = std::max(4 * scale, 1.5);
    return s;
}

// ---- the rig of the tests ----------------------------------------------------------------------------------------------
// A and B stand over the table 400 mm apart, 1200 mm up, with their image axes turned about 180 degrees (a mounting).
struct Camera {
    gs::Rig rig;
    gs::Pose profile_pose;
    gs::Pose pose;
    render::CameraProjection projection() const { return gs::ProjectionOf(rig, profile_pose); }
    gst::GridSheetProfile profile() const { return gst::GridSheetProfile{projection(), rig.width, rig.height}; }
};
Camera MakeCamera(double scale, double x_mm, double mount_deg, double rx, double ry, double rz = 0, double profile_rx = 0.4, double profile_ry = -0.3,
    double height_mm = 1200) {
    Camera c;
    c.rig = gs::MakeRig(scale);
    c.profile_pose = gs::MakePose(x_mm, 0, height_mm, mount_deg, profile_rx, profile_ry);
    c.pose = gs::MakePose(x_mm, 0, height_mm, mount_deg, rx, ry, rz);
    return c;
}
gs::Scene SceneOf(const Camera& c, double mat_deg) {
    gs::Scene s;
    s.rig = c.rig; s.pose = c.pose; s.mat_deg = mat_deg;
    return s;
}
struct Run {
    gst::GridSheetTiltOutcome outcome;
    double seconds{};
};
Run Measure(const gs::Scene& scene, const Camera& camera, const gst::GridSheetTiltSettings& settings, const std::string& name = "CAM-T") {
    const render::BgrImage image = gs::Renderer(scene).Render();
    const auto start = std::chrono::steady_clock::now();
    Run r;
    r.outcome = gst::MeasureGridSheetTilt(name, gst::ViewOf(image), camera.profile(), settings);
    r.seconds = Seconds(start);
    return r;
}

struct Error {
    double tilt{}, lean_screen{}, lean_drawing{};
};
Error ErrorOf(const gst::GridSheetTilt& t, const gs::Truth& truth, const gs::Truth& profile_truth) {
    Error e;
    e.tilt = std::abs(t.tilt_deg - truth.tilt_deg);
    e.lean_screen = Dist(t.lean_screen_deg, truth.lean_screen_deg);
    const Point drawing = Rotate(truth.lean_screen_deg, profile_truth.phi_deg);
    e.lean_drawing = Dist(t.lean_drawing_deg, drawing);
    return e;
}
void Print(const std::string& name, const gst::GridSheetTiltOutcome& o, const gs::Truth& truth, const gs::Truth& profile_truth, double seconds) {
    std::ostringstream line;
    line << std::fixed << std::setprecision(4);
    line << "case " << name << ": ";
    if (!o.tilt) {
        line << "refused " << gst::RefusalCode(o.refusal);
    } else {
        const Error e = ErrorOf(*o.tilt, truth, profile_truth);
        line << "truth " << truth.tilt_deg << " got " << o.tilt->tilt_deg << " tilt_err " << e.tilt << " lean_screen_err " << e.lean_screen
             << " lean_drawing_err " << e.lean_drawing;
    }
    const auto& e = o.evidence;
    line << std::setprecision(3) << " rms " << e.residual_rms_px << " pts " << e.used_points << " used " << e.used_point_fraction << " lines " << e.lines[0] << "/"
         << e.lines[1] << " vis " << e.visible_fraction << " spread " << e.method_spread_deg << " off " << e.off_lattice_cell_fraction << " judged " << e.judged_cells
         << std::setprecision(5) << " aspect " << e.lattice_aspect_ratio << " skew " << e.lattice_skew_deg << " sigma " << e.tilt_formal_sigma_deg << " halves "
         << e.half_split_spread_deg << " converged " << e.fit_converged << " last step " << e.fit_last_step_px << std::setprecision(3) << " (" << seconds << " s)";
    std::cout << line.str() << '\n';
}

// ---- lens: the stages' own distortion model against the renderer's product function ------------------------------------------
void LensAgainstProduct() {
    const Camera c = MakeCamera(0.25, 0, 180, 0.5, -0.7);
    const auto projection = gs::ProjectionOf(c.rig, c.pose);
    const det::Lens lens = det::LensOf(projection);
    double worst = 0, worst_forward = 0;
    for (double x = -400; x <= 400; x += 80) {
        for (double y = -250; y <= 250; y += 50) {
            Point raw;
            if (!render::TryProjectDocument(projection, {x, y}, raw)) continue;
            const auto& h = projection.document_to_image;
            const double w = h[6] * x + h[7] * y + h[8];
            const double u = (h[0] * x + h[1] * y + h[2]) / w, v = (h[3] * x + h[4] * y + h[5]) / w;
            double qx, qy;
            Check(det::UndistortNormalized(lens, (raw.x - lens.cx) / lens.fx, (raw.y - lens.cy) / lens.fy, qx, qy), "the inverse converges");
            worst = std::max(worst, std::hypot(qx * lens.fx + lens.cx - u, qy * lens.fy + lens.cy - v));
            double xd, yd;
            det::DistortNormalized(lens, (u - lens.cx) / lens.fx, (v - lens.cy) / lens.fy, xd, yd);
            worst_forward = std::max(worst_forward, std::hypot(xd * lens.fx + lens.cx - raw.x, yd * lens.fy + lens.cy - raw.y));
        }
    }
    CheckNear(worst, 0, 1e-6, "the undistorted pixel matches the product's projection (px)");
    CheckNear(worst_forward, 0, 1e-9, "the forward model is the product's (px)");
    std::cout << "lens: inverse error " << worst << " px, forward error " << worst_forward << " px\n";
}

// ---- ridge detector --------------------------------------------------------------------------------------------------------
void RidgeAccuracy() {
    const int w = 480, h = 360;
    const double angles[] = {0, 20, 45, 80, 135};
    const double offsets[] = {0.0, 0.3, 0.7};
    double mean_all = 0, worst_all = 0;
    int cases = 0;
    for (double angle : angles) {
        for (double off : offsets) {
            const double th = angle * gs::kDeg, nx = -std::sin(th), ny = std::cos(th);
            const double px = 240.0 + off, py = 180.0 + 0.5 * off;
            std::vector<std::uint8_t> luma(static_cast<std::size_t>(w) * h);
            std::mt19937_64 rng(11);
            std::normal_distribution<double> noise(0, 1.0);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const double d = (x - px) * nx + (y - py) * ny;
                luma[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(std::clamp(std::lround(40 + 100 * std::exp(-d * d / (2 * 1.15 * 1.15)) + noise(rng)), 0l, 255l));
            }
            gst::ImageView view{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), gst::PixelFormat::luma8, luma.data(), 0};
            const auto r = det::DetectRidges(view, gst::GridSheetDetectionSettings{});
            double sum = 0, worst = 0, normal_error = 0;
            std::size_t n = 0;
            for (const auto& p : r.points) {
                if (p.x < 20 || p.x > w - 20 || p.y < 20 || p.y > h - 20) continue;
                const double d = std::abs((p.x - px) * nx + (p.y - py) * ny);
                sum += d; worst = std::max(worst, d);
                normal_error = std::max(normal_error, std::abs(p.nx * std::cos(th) + p.ny * std::sin(th)));   // against the line's direction (cos, sin)
                ++n;
            }
            Check(n > 300, "ridge points are found along a line (" + std::to_string(angle) + ", " + std::to_string(off) + ")");
            if (n == 0) continue;
            const double mean = sum / static_cast<double>(n);
            CheckNear(mean, 0, 0.06, "mean distance of ridge points to the line (px), angle " + std::to_string(angle));
            CheckNear(worst, 0, 0.25, "worst distance of ridge points to the line (px), angle " + std::to_string(angle));
            CheckNear(normal_error, 0, 0.1, "the ridge normal is perpendicular to the line, angle " + std::to_string(angle));
            mean_all += mean; worst_all = std::max(worst_all, worst); ++cases;
        }
    }
    std::cout << "ridge: mean distance " << mean_all / cases << " px, worst " << worst_all << " px over " << cases << " lines\n";
}

// Lines along or nearly along an image axis, noise free and rounded to 8 bits, strong and faint. A ridge normal taken from
// the cancelling form of the eigenvector points along such a line and throws the position off by up to a pixel.
// A line exactly half way between two pixel rows gives no points: from either pixel the one Newton step along the normal
// overshoots the half pixel (the profile is a Gaussian, not a parabola), so neither claims it. That loses points, it does
// not misplace them, and the points that are found must still be right.
void RidgeAlongImageAxes() {
    const int w = 240, h = 200;
    double worst_mean = 0, worst_point = 0, worst_normal = 0;
    for (double angle : {90.0, 89.9, 90.1, 0.0}) {
        for (double off : {0.0, 0.25, 0.5}) {
            for (double amplitude : {100.0, 20.0}) {
                const double th = angle * gs::kDeg, nx = -std::sin(th), ny = std::cos(th);
                const double px = 120.0 + off, py = 100.0 + off;
                std::vector<std::uint8_t> luma(static_cast<std::size_t>(w) * h);
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                    const double d = (x - px) * nx + (y - py) * ny;
                    luma[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(std::clamp(std::lround(40 + amplitude * std::exp(-d * d / (2 * 1.15 * 1.15))), 0l, 255l));
                }
                const gst::ImageView view{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), gst::PixelFormat::luma8, luma.data(), 0};
                const auto r = det::DetectRidges(view, gst::GridSheetDetectionSettings{});
                double sum = 0, worst = 0, normal_error = 0;
                std::size_t n = 0;
                for (const auto& p : r.points) {
                    if (p.x < 20 || p.x > w - 20 || p.y < 20 || p.y > h - 20) continue;
                    const double d = std::abs((p.x - px) * nx + (p.y - py) * ny);
                    sum += d; worst = std::max(worst, d);
                    normal_error = std::max(normal_error, std::abs(p.nx * std::cos(th) + p.ny * std::sin(th)));
                    ++n;
                }
                const std::string name = "angle " + std::to_string(angle).substr(0, 5) + " offset " + std::to_string(off).substr(0, 4) + " amplitude " + std::to_string(static_cast<int>(amplitude));
                // one point per row (or column) of the line's length inside the margin, at least; half way between pixel
                // rows exactly, none (see above), and nearly so, some
                if (off < 0.5) Check(n >= 150, "axis ridge points are found: " + name + " (" + std::to_string(n) + ")");
                else if (angle != 90.0 && angle != 0.0) Check(n >= 50, "axis ridge points are found: " + name + " (" + std::to_string(n) + ")");
                if (n == 0) continue;
                const double mean = sum / static_cast<double>(n);
                CheckNear(mean, 0, 0.06, "axis ridge mean distance (px): " + name);
                CheckNear(worst, 0, 0.25, "axis ridge worst distance (px): " + name);
                CheckNear(normal_error, 0, 0.1, "axis ridge normal is across the line: " + name);
                worst_mean = std::max(worst_mean, mean); worst_point = std::max(worst_point, worst); worst_normal = std::max(worst_normal, normal_error);
            }
        }
    }
    std::cout << "ridge along the axes: worst mean " << worst_mean << " px, worst point " << worst_point << " px, worst normal " << worst_normal << '\n';
}

void RidgeThreadsAndBands() {
    // Several lines in a picture that is not a multiple of the band size.
    const int w = 410, h = 333;
    std::vector<std::uint8_t> luma(static_cast<std::size_t>(w) * h);
    std::mt19937_64 rng(5);
    std::normal_distribution<double> noise(0, 1.5);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        double v = 40;
        for (double c : {60.3, 150.7, 270.2}) v += 90 * std::exp(-(x - c) * (x - c) / (2 * 1.2 * 1.2));
        for (double c : {70.5, 200.1}) v += 90 * std::exp(-(y - c) * (y - c) / (2 * 1.2 * 1.2));
        luma[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(std::clamp(std::lround(v + noise(rng)), 0l, 255l));
    }
    gst::ImageView view{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), gst::PixelFormat::luma8, luma.data(), 0};
    gst::GridSheetDetectionSettings a, b, c;
    a.worker_threads = 1; a.ridge_band_rows = 16;
    b.worker_threads = 3; b.ridge_band_rows = 64;
    c.worker_threads = 4; c.ridge_band_rows = 333;
    const auto ra = det::DetectRidges(view, a), rb = det::DetectRidges(view, b), rc = det::DetectRidges(view, c);
    Check(ra.points.size() > 500, "the lines are found");
    const auto same = [](const det::RidgeResult& x, const det::RidgeResult& y) {
        if (x.points.size() != y.points.size() || x.threshold != y.threshold) return false;
        for (std::size_t i = 0; i < x.points.size(); ++i)
            if (x.points[i].x != y.points[i].x || x.points[i].y != y.points[i].y || x.points[i].nx != y.points[i].nx || x.points[i].ny != y.points[i].ny) return false;
        return true;
    };
    Check(same(ra, rb) && same(ra, rc), "the ridge points do not depend on the thread count or the band size");
    // The same picture as luma and as grey BGR, and with padded rows, gives the same points.
    std::vector<std::uint8_t> bgr(static_cast<std::size_t>(w) * h * 3);
    for (std::size_t i = 0; i < luma.size(); ++i) bgr[3 * i] = bgr[3 * i + 1] = bgr[3 * i + 2] = luma[i];
    const gst::ImageView bgr_view{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), gst::PixelFormat::bgr8, bgr.data(), 0};
    Check(same(ra, det::DetectRidges(bgr_view, a)), "a grey BGR picture gives the points of the luma picture");
    const std::size_t stride = static_cast<std::size_t>(w) + 37;
    std::vector<std::uint8_t> padded(stride * h, 200);
    for (int y = 0; y < h; ++y) std::copy_n(luma.begin() + static_cast<std::ptrdiff_t>(y) * w, w, padded.begin() + static_cast<std::ptrdiff_t>(y * stride));
    const gst::ImageView padded_view{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), gst::PixelFormat::luma8, padded.data(), stride};
    Check(same(ra, det::DetectRidges(padded_view, a)), "a padded stride gives the same points");
}

// ---- the Jacobian of the lattice fit -----------------------------------------------------------------------------------------
void LatticeJacobian() {
    const Camera c = MakeCamera(0.25, 0, 180, 1.0, -2.0);
    const gs::Truth truth = gs::TruthOf(c.pose);
    (void)truth;
    // Points on a plane seen by the camera: random positions and directions in the normalised plane.
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> pos(-0.3, 0.3), ang(0, 2 * kPi);
    det::PreparedPoints pts;
    for (int i = 0; i < 400; ++i) {
        pts.qx.push_back(pos(rng)); pts.qy.push_back(pos(rng));
        const double a = ang(rng);
        pts.tx.push_back(std::cos(a)); pts.ty.push_back(std::sin(a));
        pts.source.push_back(0);
    }
    det::LatticeParameters p{2.1, 0.4, 0.13, -0.35, 1.9, -0.07, 0.03, -0.02};
    const auto a = det::AssignPoints(p, pts, 2562.0, 90.0, 0);   // every direction counts
    std::vector<std::uint32_t> idx;
    std::vector<std::uint8_t> fam;
    std::vector<std::int32_t> k;
    for (std::size_t i = 0; i < pts.size(); ++i) { idx.push_back(static_cast<std::uint32_t>(i)); fam.push_back(a.family[i]); k.push_back(a.index[i]); }
    std::vector<double> r, jac;
    det::LatticeResiduals(p, pts, idx, fam, k, 2562.0, r, &jac);
    double worst = 0;
    for (std::size_t i = 0; i < r.size(); ++i) worst = std::max(worst, std::abs(r[i] - a.distance_px[i]) / (1 + std::abs(r[i])));
    CheckNear(worst, 0, 1e-4, "the residual of the Jacobian routine is the assigned distance");
    double worst_relative = 0;
    for (int j = 0; j < 8; ++j) {
        const double h = 1e-6;
        auto pp = p, pm = p;
        pp[static_cast<std::size_t>(j)] += h; pm[static_cast<std::size_t>(j)] -= h;
        std::vector<double> rp, rm;
        det::LatticeResiduals(pp, pts, idx, fam, k, 2562.0, rp, nullptr);
        det::LatticeResiduals(pm, pts, idx, fam, k, 2562.0, rm, nullptr);
        for (std::size_t i = 0; i < r.size(); ++i) {
            const double numeric = (rp[i] - rm[i]) / (2 * h);
            const double analytic = jac[i * 8 + static_cast<std::size_t>(j)];
            worst_relative = std::max(worst_relative, std::abs(numeric - analytic) / (1 + std::abs(numeric)));
        }
    }
    CheckNear(worst_relative, 0, 1e-5, "the analytic Jacobian matches central differences");
}

// ---- a known lattice, no picture: assignment, fit and shape ----------------------------------------------------------------------
// The lattice-to-plane matrix (pitches to the normalised plane) of a camera over a sheet turned by `mat` degrees.
struct KnownLattice {
    det::Mat3 h{};
    double focal{};
};
KnownLattice LatticeOfPose(double rx, double ry, double rz, double mat) {
    const Camera c = MakeCamera(0.5, 0, 180, rx, ry, rz);
    const auto doc = gs::ProjectionOf(c.rig, c.pose);
    const double m = mat * gs::kDeg, cs = std::cos(m), sn = std::sin(m), pitch = 10, ox = 3.37, oy = 1.91;
    const gs::Mat3 a{pitch * cs, -pitch * sn, pitch * (-cs * ox + sn * oy), pitch * sn, pitch * cs, pitch * (-sn * ox - cs * oy), 0, 0, 1};
    const gs::Mat3 kinv{1 / c.rig.f, 0, -c.rig.cx / c.rig.f, 0, 1 / c.rig.f, -c.rig.cy / c.rig.f, 0, 0, 1};
    auto hq = gs::Mul(gs::Mul(kinv, doc.document_to_image), a);
    for (auto& v : hq) v /= hq[8];
    return {hq, c.rig.f};
}
Point PlaneOf(const det::Mat3& h, double i, double j) {
    const double w = h[6] * i + h[7] * j + h[8];
    return {(h[0] * i + h[1] * j + h[2]) / w, (h[3] * i + h[4] * j + h[5]) / w};
}
// Points along the lattice lines |index| <= lines, every `step` pitches, with their tangent turned by `turn_deg`.
det::PreparedPoints PointsOnLines(const det::Mat3& h, int lines, double step, double turn_deg) {
    det::PreparedPoints pts;
    const double c = std::cos(turn_deg * gs::kDeg), s = std::sin(turn_deg * gs::kDeg);
    for (int family = 0; family < 2; ++family) {
        for (int k = -lines; k <= lines; ++k) {
            for (double t = -lines; t <= lines + 1e-9; t += step) {
                const double i = family == 0 ? k : t, j = family == 0 ? t : k;
                const Point q = PlaneOf(h, i, j);
                const Point qa = PlaneOf(h, family == 0 ? i : i + 1e-4, family == 0 ? j + 1e-4 : j);
                double tx = qa.x - q.x, ty = qa.y - q.y;
                const double n = std::hypot(tx, ty);
                tx /= n; ty /= n;
                pts.qx.push_back(q.x); pts.qy.push_back(q.y);
                pts.tx.push_back(c * tx - s * ty); pts.ty.push_back(s * tx + c * ty);
                pts.source.push_back(0);
            }
        }
    }
    return pts;
}

void AssignmentDirectionGate() {
    const KnownLattice known = LatticeOfPose(1.5, -2.0, 0.2, 23);
    const det::LatticeParameters p = det::ParametersOfLatticeToPlane(known.h);
    // The direction gate: a point on a lattice line counts only when its own line direction is the family's.
    for (double turn : {0.0, 5.0, -5.0, 10.0, -10.0, 45.0, 90.0 - 20.0}) {
        const auto pts = PointsOnLines(known.h, 10, 0.5, turn);
        const auto a = det::AssignPoints(p, pts, known.focal, 7.0, 2.5);
        std::size_t ok = 0;
        double worst = 0;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (!a.ok[i]) continue;
            ++ok;
            worst = std::max(worst, std::abs(static_cast<double>(a.distance_px[i])));
        }
        const bool expected = std::abs(turn) < 7.0;
        const std::string name = "a point whose line is turned " + std::to_string(static_cast<int>(turn)) + " degrees from its family";
        Check(expected ? ok == pts.size() : ok == 0, name + (expected ? " is assigned" : " is not assigned"));
        if (expected) CheckNear(worst, 0, 1e-6, name + ": it lies on its line (px)");
    }
}

void LatticeShapes() {
    const KnownLattice known = LatticeOfPose(2.5, 1.0, -0.4, 37);
    const auto square = det::ShapeOfLattice(known.h);
    CheckNear(square.aspect_ratio, 1, 1e-9, "a square grid seen in a tilt has equal lattice steps");
    CheckNear(square.skew_deg, 0, 1e-7, "a square grid seen in a tilt has right angles");
    const auto stretched = det::ShapeOfLattice(gs::Mul(known.h, gs::Mat3{1, 0, 0, 0, 1.5, 0, 0, 0, 1}));
    CheckNear(stretched.aspect_ratio, 1.5, 1e-9, "a lattice whose second step is one and a half of the first");
    const auto sheared = det::ShapeOfLattice(gs::Mul(known.h, gs::Mat3{1, std::tan(2 * gs::kDeg), 0, 0, 1, 0, 0, 0, 1}));
    CheckNear(sheared.skew_deg, 2, 1e-7, "a sheared lattice is two degrees off a right angle");
}

// The robust fit: a quarter of the points of the lines on one side are 1.5 px off their line (within every gate). The
// robust weights leave them out; a plain least-squares fit would bend the lattice toward them.
void LatticeFitRobust() {
    const KnownLattice known = LatticeOfPose(2.0, -1.5, 0.3, 23);
    det::PreparedPoints pts = PointsOnLines(known.h, 72, 0.2, 0);
    std::mt19937_64 rng(17);
    std::normal_distribution<double> noise(0, 0.05);
    std::size_t moved = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        // the normal of the line in the plane, in pixels
        const double nx = -pts.ty[i], ny = pts.tx[i];
        double shift = noise(rng);
        if (i % 4 == 0 && pts.qx[i] > 0) { shift += 1.5; ++moved; }
        pts.qx[i] += nx * shift / known.focal; pts.qy[i] += ny * shift / known.focal;
    }
    det::InitialLattice start;
    start.parameters = det::ParametersOfLatticeToPlane(known.h);
    for (int j = 0; j < 6; ++j) start.parameters[static_cast<std::size_t>(j)] *= 1.003;
    start.parameters[2] += 0.002; start.parameters[5] -= 0.0015;
    start.parameters[6] = 0; start.parameters[7] = 0;   // the first lattice is affine
    start.centre_x = 0; start.centre_y = 0;
    const auto fit = det::FitLattice(pts, known.focal, start, gst::GridSheetDetectionSettings{});
    Check(fit.ok, "the robust fit runs");
    Check(fit.converged, "the robust fit settles");
    const det::Mat3 h = det::LatticeToPlane(fit.parameters);
    double worst = 0;
    for (int i = -60; i <= 60; i += 10)
        for (int j = -60; j <= 60; j += 10) worst = std::max(worst, Dist(PlaneOf(h, i, j), PlaneOf(known.h, i, j)) * known.focal);
    std::cout << "fit: " << moved << " of " << pts.size() << " points moved 1.5 px; worst node error " << worst << " px\n";
    CheckNear(worst, 0, 0.05, "points off their line by 1.5 px do not pull the lattice (px)");
    // The settle line decides: the last step's movement is reported, and a fit whose last step moved more than the line
    // is not converged.
    Check(std::isfinite(fit.last_step_px) && fit.last_step_px <= gst::GridSheetDetectionSettings{}.fit_settle_px, "the last step of the fit is reported");
    auto strict = gst::GridSheetDetectionSettings{};
    strict.fit_settle_px = 1e-300;
    const auto unsettled = det::FitLattice(pts, known.focal, start, strict);
    std::cout << "fit: last step " << fit.last_step_px << " px; with a settle line of 1e-300 px the last step was " << unsettled.last_step_px << " px\n";
    Check(unsettled.ok && (unsettled.last_step_px > strict.fit_settle_px) == !unsettled.converged, "the settle line decides whether the fit converged");
    Check(unsettled.last_step_px == 0 || !unsettled.converged, "a fit still moving above the settle line is not converged");
}

// ---- the decomposition: the lattice matrix of a known pose, no picture -----------------------------------------------------------
void Decomposition() {
    const double mats[] = {0, 37, 90, 123, -52, 180};
    const double poses[][2] = {{0, 0}, {0.5, 0}, {0, -1.0}, {2.1, 2.1}, {-3.5, 3.5}, {5, 0}};
    double worst_tilt = 0, worst_lean = 0, worst_phi = 0;
    for (const auto& pr : poses) {
        for (double mat : mats) {
            const Camera c = MakeCamera(0.5, 0, 180, pr[0], pr[1], 0.3);
            const auto doc = gs::ProjectionOf(c.rig, c.pose);
            const auto profile = c.projection();
            // lattice -> normalised plane = K^-1 H_doc A, with A: lattice (pitches) -> document mm
            const double m = mat * gs::kDeg, cs = std::cos(m), sn = std::sin(m), pitch = 10, ox = 3.37, oy = 1.91;
            const gs::Mat3 a{pitch * cs, -pitch * sn, pitch * (-cs * ox + sn * oy), pitch * sn, pitch * cs, pitch * (-sn * ox - cs * oy), 0, 0, 1};
            const gs::Mat3 kinv{1 / c.rig.f, 0, -c.rig.cx / c.rig.f, 0, 1 / c.rig.f, -c.rig.cy / c.rig.f, 0, 0, 1};
            auto hq = gs::Mul(gs::Mul(kinv, doc.document_to_image), a);
            for (auto& v : hq) v /= hq[8];
            Check(det::PositivelyOriented(hq), "a lattice of a real pose is positively oriented");
            const auto o = det::OrientationOfLattice(profile, hq);
            Check(o.ok, "the decomposition succeeds");
            const gs::Truth t = gs::TruthOf(c.pose);
            worst_tilt = std::max(worst_tilt, std::abs(o.tilt_deg - t.tilt_deg));
            const Point lean_lattice = Rotate(t.lean_doc_deg, -mat);   // lattice = Rm X / pitch, Rm turns vectors by -mat
            worst_lean = std::max(worst_lean, Dist(o.lean_sheet_deg, lean_lattice));
            worst_phi = std::max(worst_phi, std::abs(Wrap(o.image_x_azimuth_deg - (t.phi_deg - mat))));
        }
    }
    CheckNear(worst_tilt, 0, 1e-6, "tilt of the decomposition against the geometry (deg)");
    CheckNear(worst_lean, 0, 1e-6, "lean in the lattice frame against the geometry (deg)");
    CheckNear(worst_phi, 0, 1e-6, "azimuth of the picture's x axis in the lattice frame (deg)");
    // A mirrored lattice (swapped axes) is not accepted as a plane the camera looks at from above.
    const Camera c = MakeCamera(0.5, 0, 180, 1, 1);
    const auto doc = gs::ProjectionOf(c.rig, c.pose);
    const gs::Mat3 kinv{1 / c.rig.f, 0, -c.rig.cx / c.rig.f, 0, 1 / c.rig.f, -c.rig.cy / c.rig.f, 0, 0, 1};
    auto hq = gs::Mul(gs::Mul(kinv, doc.document_to_image), gs::Mat3{0, 1, 0, 1, 0, 0, 0, 0, 1});
    for (auto& v : hq) v /= hq[8];
    Check(!det::PositivelyOriented(hq), "a mirrored lattice is not positively oriented");
    Check(!det::OrientationOfLattice(c.projection(), hq).ok, "a mirrored lattice gives no orientation");
    std::cout << "decomposition: worst tilt " << worst_tilt << ", lean " << worst_lean << ", azimuth " << worst_phi << " deg\n";
}

// ---- accuracy on drawn pictures --------------------------------------------------------------------------------------------------
// Bounds set from the largest errors measured on these pictures (about 0.0003 degrees), with a wide margin.
constexpr double kTiltTolerance = 0.01, kLeanTolerance = 0.02, kDrawingTolerance = 0.02;
constexpr double kMapTolerancePitch = 0.001;   // the returned map against the drawn sheet, in lattice pitches

// The evidence a measured value carries.
void CheckEvidence(const gst::GridSheetTiltOutcome& o, const std::string& name) {
    const auto& e = o.evidence;
    Check(e.fit_converged, name + ": the fit settled");
    Check(e.ridge_points > 0 && e.used_points <= e.ridge_points && std::abs(e.used_point_fraction - static_cast<double>(e.used_points) / static_cast<double>(e.ridge_points)) < 1e-12,
        name + ": the share of the ridge points that were used is reported");
    CheckNear(e.lattice_aspect_ratio, 1, 0.002, name + ": the lattice is square");
    CheckNear(e.lattice_skew_deg, 0, 0.1, name + ": the lattice has right angles");
    Check(std::isfinite(e.tilt_formal_sigma_deg) && e.tilt_formal_sigma_deg > 0 && e.tilt_formal_sigma_deg < 0.01, name + ": the formal sigma is reported");
    Check(std::isfinite(e.half_split_spread_deg) && e.half_split_spread_deg < 0.05, name + ": the halves agree");
}

void Accuracy() {
    const double scale = 0.5;
    const auto settings = TestSettings(scale);
    // mount: the turn of the image axes about the optical axis, the same in the profile. 90 and 37 degrees tell the
    // direction of the turn into the drawing frame apart (180 does not: it turns both ways alike).
    struct Item { double rx, ry, rz, mat, mount; };
    const Item items[] = {
        {0, 0, 0, 35, 180},          {0.5, 0, 0, 0, 180},         {0, -1.0, 0.4, -52, 180},    {0.7, 0.7, -0.5, 123, 180},   {-1.4, 1.4, 0, 90, 180},
        {3.0, 0, 0.8, 37, 180},      {-2.1, 2.1, 0, 10, 180},     {3.5, -3.5, 0.3, -52, 180},  {0, 5.0, 0, 35, 180},         {5.0, 0, -0.6, 160, 180},
        {1.2, -2.0, 0.2, 20, 90},    {-2.5, -0.8, 0, 123, 37},    {0.4, 2.6, -0.3, -52, -63},
    };
    int index = 0;
    for (const auto& it : items) {
        const Camera c = MakeCamera(scale, 0, it.mount, it.rx, it.ry, it.rz);
        const Run run = Measure(SceneOf(c, it.mat), c, settings);
        const gs::Truth truth = gs::TruthOf(c.pose), profile_truth = gs::TruthOf(c.profile_pose);
        const std::string name = "accuracy " + std::to_string(index++) + " tilt " + std::to_string(truth.tilt_deg).substr(0, 5) + " mat " + std::to_string(static_cast<int>(it.mat)) +
            " mount " + std::to_string(static_cast<int>(it.mount));
        Print(name, run.outcome, truth, profile_truth, run.seconds);
        Check(run.outcome.tilt.has_value(), name + " is measured");
        if (!run.outcome.tilt) continue;
        const Error e = ErrorOf(*run.outcome.tilt, truth, profile_truth);
        CheckNear(e.tilt, 0, kTiltTolerance, name + " tilt");
        CheckNear(e.lean_screen, 0, kLeanTolerance, name + " lean (screen)");
        // the drawing frame assumes the camera's turn about its axis is the calibrated one; rz differs by a fraction of a degree
        CheckNear(e.lean_drawing, 0, kDrawingTolerance + truth.tilt_deg * std::abs(it.rz) * gs::kDeg, name + " lean (drawing)");
        Check(run.outcome.tilt->camera == "CAM-T", "the camera name is carried");
        Check(run.outcome.evidence.method_spread_deg < 0.2, name + " three methods agree");
        CheckEvidence(run.outcome, name);
    }
}

void Layouts() {
    const double scale = 0.5;
    const auto settings = TestSettings(scale);
    const Camera c = MakeCamera(scale, 0, 180, 1.2, -0.8, 0.2);
    const gs::Truth truth = gs::TruthOf(c.pose), profile_truth = gs::TruthOf(c.profile_pose);
    struct Variation {
        std::string name;
        double mat;
        double region;     // visible share of each side
        double background, amplitude, noise, line_sigma, thick_sigma, thick_gain;
        double tolerance;
    };
    const std::vector<Variation> variations = {
        {"mat 0", 0, 1, 40, 60, 1.5, 1.15, 1.6, 1.5, 0.01},
        {"mat 37", 37, 1, 40, 60, 1.5, 1.15, 1.6, 1.5, 0.01},
        {"mat 90", 90, 1, 40, 60, 1.5, 1.15, 1.6, 1.5, 0.01},
        {"mat 123", 123, 1, 40, 60, 1.5, 1.15, 1.6, 1.5, 0.01},
        {"seven tenths", 37, std::sqrt(0.7), 40, 60, 1.5, 1.15, 1.6, 1.5, 0.01},
        {"three tenths", 123, std::sqrt(0.3), 40, 60, 1.5, 1.15, 1.6, 1.5, 0.01},
        {"one tenth", 37, std::sqrt(0.1), 40, 60, 1.5, 1.15, 1.6, 1.5, 0.02},
        {"dark", 37, 1, 8, 10, 1.5, 1.15, 1.6, 1.5, 0.01},
        {"bright", 37, 1, 170, 70, 1.5, 1.15, 1.6, 1.5, 0.01},
        {"noisy", 37, 1, 40, 60, 6.0, 1.15, 1.6, 1.5, 0.01},
        {"blurred", 37, 1, 40, 60, 1.5, 2.2, 2.8, 1.2, 0.01},
        {"thick and thin", 37, 1, 40, 60, 1.5, 0.9, 2.6, 2.6, 0.01},
    };
    for (const auto& v : variations) {
        gs::Scene scene = SceneOf(c, v.mat);
        const double half = v.region / 2;
        scene.region[0] = 0.5 - half; scene.region[2] = 0.5 + half; scene.region[1] = 0.5 - half; scene.region[3] = 0.5 + half;
        scene.background = v.background; scene.amplitude = v.amplitude; scene.noise = v.noise;
        scene.line_sigma = v.line_sigma; scene.thick_sigma = v.thick_sigma; scene.thick_gain = v.thick_gain;
        auto s = settings;
        if (v.region < 0.5) { s.acceptance.min_lines_per_family = 6; s.acceptance.min_lattice_span = 6; }
        const Run run = Measure(scene, c, s);
        Print("layout " + v.name, run.outcome, truth, profile_truth, run.seconds);
        Check(run.outcome.tilt.has_value(), "layout " + v.name + " is measured");
        if (!run.outcome.tilt) continue;
        const Error e = ErrorOf(*run.outcome.tilt, truth, profile_truth);
        CheckNear(e.tilt, 0, v.tolerance, "layout " + v.name + " tilt");
        CheckNear(e.lean_screen, 0, v.tolerance * 2, "layout " + v.name + " lean (screen)");
    }
}

void Distractors() {
    const double scale = 0.5;
    const auto settings = TestSettings(scale);
    const Camera c = MakeCamera(scale, 0, 180, -1.0, 1.5, 0);
    const gs::Truth truth = gs::TruthOf(c.pose), profile_truth = gs::TruthOf(c.profile_pose);
    const double w = c.rig.width, h = c.rig.height;
    // a drawing: a bundle of thin lines at 20 degrees with uneven spacing; a paper: a rectangle's outline; slanted lines
    std::vector<gs::Segment> drawing, paper, slanted;
    std::mt19937_64 rng(9);
    std::uniform_real_distribution<double> gap(9, 31);
    const double th = 20 * gs::kDeg;
    for (double s = -0.6 * w; s < 0.9 * w; s += gap(rng)) {
        const double x0 = 0.3 * w + s * std::cos(th + kPi / 2) - 0.8 * w * std::cos(th), y0 = 0.5 * h + s * std::sin(th + kPi / 2) - 0.8 * w * std::sin(th);
        drawing.push_back({x0, y0, x0 + 1.6 * w * std::cos(th), y0 + 1.6 * w * std::sin(th), 50, 1.0});
    }
    paper = {{0.25 * w, 0.25 * h, 0.75 * w, 0.25 * h, 70, 1.2}, {0.75 * w, 0.25 * h, 0.75 * w, 0.8 * h, 70, 1.2},
             {0.75 * w, 0.8 * h, 0.25 * w, 0.8 * h, 70, 1.2}, {0.25 * w, 0.8 * h, 0.25 * w, 0.25 * h, 70, 1.2}};
    slanted = {{0, 0, w, h, 60, 1.3}, {0, h, w, 0, 60, 1.3}, {0.2 * w, 0, 0.9 * w, h, 60, 1.3}};
    struct Item { std::string name; std::vector<gs::Segment> extra; bool must_measure; };
    for (const auto& item : {Item{"drawing lines", drawing, false}, Item{"paper outline", paper, true}, Item{"slanted lines", slanted, true}}) {
        gs::Scene scene = SceneOf(c, 37);
        scene.extra = item.extra;
        const Run run = Measure(scene, c, settings);
        Print("distractor " + item.name, run.outcome, truth, profile_truth, run.seconds);
        // Not pulled off, or refused: either is an acceptable answer; a wrong value is not.
        if (run.outcome.tilt) {
            const Error e = ErrorOf(*run.outcome.tilt, truth, profile_truth);
            CheckNear(e.tilt, 0, 0.02, "distractor " + item.name + " tilt");
            CheckNear(e.lean_screen, 0, 0.03, "distractor " + item.name + " lean");
        } else {
            std::cout << "  refused: " << gst::RefusalCode(run.outcome.refusal) << '\n';
            Check(run.outcome.refusal == gst::GridSheetRefusal::methods_disagree || run.outcome.refusal == gst::GridSheetRefusal::residual_too_large ||
                run.outcome.refusal == gst::GridSheetRefusal::sheet_not_flat, "distractor " + item.name + " is refused for a reason that names the disturbance");
        }
        if (item.must_measure) Check(run.outcome.tilt.has_value(), "distractor " + item.name + " is still measured");
    }
}

void Refusals() {
    const double scale = 0.5;
    const auto settings = TestSettings(scale);
    const Camera c = MakeCamera(scale, 0, 180, 1.0, -1.0, 0);
    const auto expect = [&](const std::string& name, const Run& run, std::initializer_list<gst::GridSheetRefusal> allowed) {
        const bool ok = !run.outcome.tilt.has_value() && std::find(allowed.begin(), allowed.end(), run.outcome.refusal) != allowed.end();
        std::cout << "refusal " << name << ": " << gst::RefusalCode(run.outcome.refusal) << (run.outcome.tilt ? " (a value was given!)" : "") << '\n';
        Check(ok, "refused for the right reason: " + name);
        Check(!run.outcome.tilt.has_value(), "no value is given: " + name);
    };
    {   // no sheet: ground and noise
        gs::Scene s = SceneOf(c, 0);
        s.region[0] = s.region[1] = 0; s.region[2] = s.region[3] = 0;
        expect("blank", Measure(s, c, settings), {gst::GridSheetRefusal::too_few_ridge_points, gst::GridSheetRefusal::no_grid_found});
    }
    {   // lines in one direction only
        gs::Scene s = SceneOf(c, 20);
        s.lines_only_in_first_family = true;
        expect("one direction", Measure(s, c, settings), {gst::GridSheetRefusal::no_grid_found, gst::GridSheetRefusal::too_few_ridge_points});
    }
    {   // a small piece of sheet
        gs::Scene s = SceneOf(c, 20);
        s.region[0] = 0.45; s.region[2] = 0.55; s.region[1] = 0.45; s.region[3] = 0.60;
        expect("small piece", Measure(s, c, settings), {gst::GridSheetRefusal::too_few_lines, gst::GridSheetRefusal::span_too_small, gst::GridSheetRefusal::view_too_small,
            gst::GridSheetRefusal::no_grid_found, gst::GridSheetRefusal::too_few_ridge_points});
    }
    {   // the sheet is bent out of the table's plane in the middle of the picture
        gs::Scene s = SceneOf(c, 20);
        s.fold = true; s.fold_x_mm = 0; s.fold_deg = 12;
        const Run bent = Measure(s, c, settings);
        Print("fold", bent.outcome, gs::TruthOf(c.pose), gs::TruthOf(c.profile_pose), bent.seconds);
        expect("bent sheet", bent, {gst::GridSheetRefusal::residual_too_large, gst::GridSheetRefusal::sheet_not_flat, gst::GridSheetRefusal::methods_disagree,
            gst::GridSheetRefusal::grid_not_square});
        // The flatness check alone stops it, with room: the share of off-lattice cells is well above the line.
        Check(bent.outcome.evidence.off_lattice_cell_fraction > 1.5 * settings.acceptance.max_off_lattice_cell_fraction, "the bent sheet is far from flat");
        // With the other lines out of the way, the flatness check is what stops it.
        auto lenient = settings;
        lenient.acceptance.max_residual_rms_px = 1.4; lenient.acceptance.max_method_spread_deg = 5;
        lenient.acceptance.max_lattice_aspect_deviation = 1; lenient.acceptance.max_lattice_skew_deg = 45;
        expect("bent sheet, lenient residual, shape and spread", Measure(s, c, lenient), {gst::GridSheetRefusal::sheet_not_flat});
        const Run flat = Measure(SceneOf(c, 20), c, settings);
        Check(flat.outcome.tilt.has_value(), "the same picture without the bend is measured");
    }
    // The lines of the caller decide: every acceptance value, made impossible one at a time.
    {
        const gs::Scene scene = SceneOf(c, 37);
        const render::BgrImage image = gs::Renderer(scene).Render();
        const auto run = [&](const gst::GridSheetTiltSettings& s) { return gst::MeasureGridSheetTilt("CAM-T", gst::ViewOf(image), c.profile(), s); };
        const auto good = run(settings);
        Check(good.tilt.has_value(), "the picture of the acceptance checks is measured");
        const auto change = [&](auto edit) { auto s = settings; edit(s.acceptance); return run(s); };
        Check(change([](gst::GridSheetAcceptance& a) { a.min_ridge_points = 100000000; }).refusal == gst::GridSheetRefusal::too_few_ridge_points, "ridge point line");
        Check(change([](gst::GridSheetAcceptance& a) { a.min_lines_per_family = 100000; }).refusal == gst::GridSheetRefusal::too_few_lines, "line count line");
        Check(change([](gst::GridSheetAcceptance& a) { a.min_lattice_span = 100000; }).refusal == gst::GridSheetRefusal::span_too_small, "span line");
        Check(change([](gst::GridSheetAcceptance& a) { a.min_visible_fraction = 1.01; }).refusal == gst::GridSheetRefusal::view_too_small, "visible fraction line");
        Check(change([](gst::GridSheetAcceptance& a) { a.max_residual_rms_px = 1e-6; }).refusal == gst::GridSheetRefusal::residual_too_large, "residual line");
        Check(change([](gst::GridSheetAcceptance& a) { a.max_method_spread_deg = 1e-9; }).refusal == gst::GridSheetRefusal::methods_disagree, "method spread line");
        Check(change([](gst::GridSheetAcceptance& a) { a.max_lattice_aspect_deviation = 0; }).refusal == gst::GridSheetRefusal::grid_not_square, "lattice aspect line");
        Check(change([](gst::GridSheetAcceptance& a) { a.max_lattice_skew_deg = 0; }).refusal == gst::GridSheetRefusal::grid_not_square, "lattice skew line");
        auto unsettled = settings;
        unsettled.detection.fit_settle_px = 1e-300;
        const auto not_settled = run(unsettled);
        Check(good.evidence.fit_converged && good.evidence.fit_last_step_px <= settings.detection.fit_settle_px, "the settled fit is reported");
        Check(not_settled.evidence.fit_last_step_px == 0 || not_settled.refusal == gst::GridSheetRefusal::fit_failed,
            "a fit whose last step moved more than the settle line is refused");
        auto unjudged = settings;
        unjudged.detection.flat_cell_min_points = 100000000;
        Check(run(unjudged).refusal == gst::GridSheetRefusal::flatness_not_judged, "a flatness that cannot be judged is refused, not passed");
        Check(good.evidence.method_spread_deg > 0 && good.evidence.method_spread_deg < 0.2, "the spread is reported");
        Check(good.evidence.residual_rms_px > 0 && good.evidence.residual_rms_px < 1.0, "the residual is reported");
        Check(good.evidence.nodes > 100 && good.evidence.lines[0] >= 12 && good.evidence.lines[1] >= 12, "nodes and lines are counted");
        Check(!std::isnan(good.evidence.tilt_deg_fit) && !std::isnan(good.evidence.tilt_deg_nodes) && !std::isnan(good.evidence.tilt_deg_vanishing), "all three tilts are reported");
        // the evidence of a refusal is kept
        const auto refused = change([](gst::GridSheetAcceptance& a) { a.max_residual_rms_px = 1e-6; });
        Check(refused.evidence.used_points > 1000 && !refused.tilt.has_value(), "a refusal keeps its evidence");
        Check(gst::RefusalCode(gst::GridSheetRefusal::sheet_not_flat) == "sheet-not-flat", "refusal codes are words");
    }
    // programming errors are exceptions, not refusals
    {
        const render::BgrImage image = gs::Renderer(SceneOf(c, 0)).Render();
        const auto throws = [&](const gst::ImageView& view, const gst::GridSheetProfile& profile, const gst::GridSheetTiltSettings& s) {
            try { (void)gst::MeasureGridSheetTilt("X", view, profile, s); } catch (const std::invalid_argument&) { return true; }
            return false;
        };
        Check(throws(gst::ImageView{}, c.profile(), settings), "an empty picture is an invalid argument");
        auto bad = settings;
        bad.acceptance.min_lines_per_family = 0;
        Check(throws(gst::ViewOf(image), c.profile(), bad), "unset acceptance is an invalid argument");
        bad = settings;
        bad.acceptance.max_residual_rms_px = bad.detection.final_gate_px / std::sqrt(3.0);
        Check(throws(gst::ViewOf(image), c.profile(), bad), "a residual line the final gate cannot reach is an invalid argument");
        bad = settings;
        bad.acceptance.max_lattice_skew_deg = std::numeric_limits<double>::quiet_NaN();
        Check(throws(gst::ViewOf(image), c.profile(), bad), "a line that is not a number is an invalid argument");
        bad = settings;
        bad.detection.radii[3] = bad.detection.radii[2];
        Check(throws(gst::ViewOf(image), c.profile(), bad), "fit windows that do not grow are an invalid argument");
        // The profile's own size and its principal point must belong together.
        auto profile = c.profile();
        profile.picture_width = 4 * c.rig.width; profile.picture_height = 4 * c.rig.height;
        Check(throws(gst::ViewOf(image), profile, settings), "a profile size whose middle does not hold the principal point is an invalid argument");
        profile = c.profile();
        profile.picture_width = 0;
        Check(throws(gst::ViewOf(image), profile, settings), "a profile without a picture size is an invalid argument");
    }
    // Only the profile's own picture size is measured: a reduced picture with the full size profile is refused before
    // anything is detected (its pixel numbers would put the principal point and the focal length in the wrong place).
    {
        const Camera full = MakeCamera(1.0, 0, 180, 1.0, -1.0, 0);
        const Camera half = MakeCamera(0.5, 0, 180, 1.0, -1.0, 0);
        const render::BgrImage reduced = gs::Renderer(SceneOf(half, 37)).Render();
        const auto o = gst::MeasureGridSheetTilt("CAM-T", gst::ViewOf(reduced), full.profile(), TestSettings(0.5));
        Check(o.refusal == gst::GridSheetRefusal::picture_not_profile_size && !o.tilt.has_value(), "a reduced picture with the full size profile is refused");
        Check(o.evidence.ridge_points == 0, "nothing is detected in a picture of another size");
        const auto ok = gst::MeasureGridSheetTilt("CAM-T", gst::ViewOf(reduced), half.profile(), TestSettings(0.5));
        Check(ok.tilt.has_value(), "the same picture with the profile of its own size is measured");
    }
    // A rectangular grid is not the square sheet the decomposition assumes: refused, not measured with a wrong tilt.
    {
        gs::Scene s = SceneOf(c, 37);
        s.second_pitch_ratio = 1.5;
        const Run r = Measure(s, c, settings);
        Print("rectangular grid", r.outcome, gs::TruthOf(c.pose), gs::TruthOf(c.profile_pose), r.seconds);
        expect("rectangular grid", r, {gst::GridSheetRefusal::grid_not_square});
        const double aspect = r.outcome.evidence.lattice_aspect_ratio;
        CheckNear(aspect > 1 ? aspect : 1 / aspect, 1.5, 0.01, "the rectangle's step ratio is reported");
    }
}

// ---- two cameras, the same sheet --------------------------------------------------------------------------------------------------
void Pairs() {
    const double scale = 0.5;
    const auto settings = TestSettings(scale);
    struct Setup { std::string name; double mount_a, mount_b; };
    const Setup setups[] = {{"mounted alike", 180.0, 178.2}, {"mounted opposite", 180.0, 0.4}};
    for (const auto& setup : setups) {
        std::vector<gst::GridSheetPair> pairs;
        for (double mat : {0.0, 37.0, 90.0, 123.0}) {
            const Camera a = MakeCamera(scale, -200, setup.mount_a, 0.7, -1.1, 0, 0.4, -0.3);
            const Camera b = MakeCamera(scale, 200, setup.mount_b, -1.6, 2.2, 0, 0.2, 0.3);
            const Run ra = Measure(SceneOf(a, mat), a, settings, "CAM-A");
            const Run rb = Measure(SceneOf(b, mat), b, settings, "CAM-B");
            const std::string name = setup.name + " mat " + std::to_string(static_cast<int>(mat));
            Check(ra.outcome.tilt.has_value() && rb.outcome.tilt.has_value(), name + ": both are measured");
            if (!ra.outcome.tilt || !rb.outcome.tilt) continue;
            const auto pair = gst::CompareGridSheetTilts(ra.outcome, rb.outcome, settings.pair);
            Check(pair.pair.has_value(), name + ": the pair is compared");
            if (!pair.pair) continue;
            const gs::Truth ta = gs::TruthOf(a.pose), tb = gs::TruthOf(b.pose);
            const double dot = ta.axis_doc[0] * tb.axis_doc[0] + ta.axis_doc[1] * tb.axis_doc[1] + ta.axis_doc[2] * tb.axis_doc[2];
            const double truth_angle = std::acos(std::clamp(dot, -1.0, 1.0)) / gs::kDeg;
            const Point truth_difference{tb.lean_doc_deg.x - ta.lean_doc_deg.x, tb.lean_doc_deg.y - ta.lean_doc_deg.y};
            const auto& p = *pair.pair;
            std::cout << std::fixed << std::setprecision(4) << "pair " << name << ": axis angle " << p.optical_axis_angle_deg << " (truth " << truth_angle << "), difference ("
                      << p.lean_difference_drawing_deg.x << ", " << p.lean_difference_drawing_deg.y << ") (truth " << truth_difference.x << ", " << truth_difference.y
                      << "), quarter turns " << p.quarter_turns << ", mismatch " << p.rotation_mismatch_deg << '\n';
            CheckNear(p.optical_axis_angle_deg, truth_angle, 0.04, name + ": the angle between the optical axes");
            CheckNear(p.lean_difference_drawing_deg.x, truth_difference.x, 0.06, name + ": the difference (x)");
            CheckNear(p.lean_difference_drawing_deg.y, truth_difference.y, 0.06, name + ": the difference (y)");
            Check(std::abs(p.rotation_mismatch_deg) < 0.2, name + ": the numbering matches the calibration");
            CheckNear(pair.rotation_mismatch_deg, p.rotation_mismatch_deg, 0, name + ": the outcome carries the mismatch");
            Check(p.reference_camera == "CAM-A" && p.target_camera == "CAM-B", name + ": the camera names are carried");
            pairs.push_back(p);
        }
        // the answer does not depend on how the mat lies, nor on the numbering the stages happened to pick
        bool turned = false;
        for (std::size_t i = 1; i < pairs.size(); ++i) {
            CheckNear(pairs[i].optical_axis_angle_deg, pairs[0].optical_axis_angle_deg, 0.04, setup.name + ": the angle is the same for every mat");
            CheckNear(pairs[i].lean_difference_drawing_deg.x, pairs[0].lean_difference_drawing_deg.x, 0.06, setup.name + ": the difference x is the same for every mat");
            CheckNear(pairs[i].lean_difference_drawing_deg.y, pairs[0].lean_difference_drawing_deg.y, 0.06, setup.name + ": the difference y is the same for every mat");
            if (pairs[i].quarter_turns != pairs[0].quarter_turns) turned = true;
        }
        std::cout << "pair " << setup.name << ": the quarter turn differs between mats: " << (turned ? "yes" : "no") << '\n';
    }
    // The two cameras do not see the same sheet the same way: B's mat lies turned against A's. Each picture is measured
    // correctly against the sheet it sees, but their sheet frames differ by the turn, so the difference of the leans would
    // be wrong. The rotation mismatch shows the turn and refuses it.
    const Camera a = MakeCamera(scale, -200, 180, 0.7, -1.1);
    const Camera b = MakeCamera(scale, 200, 178.2, -1.6, 2.2);
    const Run ra = Measure(SceneOf(a, 37), a, settings, "CAM-A"), rb = Measure(SceneOf(b, 37), b, settings, "CAM-B");
    for (double turn : {1.5, -3.0, 20.0, 30.0, 40.0}) {
        const Run turned = Measure(SceneOf(b, 37 + turn), b, settings, "CAM-B");
        const auto pair = gst::CompareGridSheetTilts(ra.outcome, turned.outcome, settings.pair);
        const std::string name = "B's mat turned " + std::to_string(turn).substr(0, 5) + " degrees";
        std::cout << "pair " << name << ": " << gst::RefusalCode(pair.refusal) << ", mismatch " << pair.rotation_mismatch_deg << '\n';
        Check(turned.outcome.tilt.has_value(), name + ": B is measured");
        // the mismatch is the turn of the mat (the picture's axes turn the other way against the sheet), up to the small
        // calibrated difference of the test cameras
        CheckNear(std::abs(pair.rotation_mismatch_deg), std::abs(turn), 0.2, name + ": the mismatch is reported");
        const bool within = std::abs(turn) <= settings.pair.max_rotation_mismatch_deg;
        Check(within ? pair.pair.has_value() : pair.refusal == gst::GridSheetPairRefusal::turn_mismatch && !pair.pair.has_value(),
            name + (within ? ": compared" : ": refused as a turn mismatch"));
    }
    gst::GridSheetTiltOutcome none;
    const auto& ok = settings.pair;
    Check(gst::CompareGridSheetTilts(none, rb.outcome, ok).refusal == gst::GridSheetPairRefusal::reference_not_measured, "a reference that was not measured is named");
    Check(gst::CompareGridSheetTilts(ra.outcome, none, ok).refusal == gst::GridSheetPairRefusal::target_not_measured, "a target that was not measured is named");
    Check(!gst::CompareGridSheetTilts(none, none, ok).pair.has_value(), "no pair without both");
    Check(std::isnan(gst::CompareGridSheetTilts(none, rb.outcome, ok).rotation_mismatch_deg), "no mismatch without both");
    gst::GridSheetPairAcceptance impossible;
    impossible.max_rotation_mismatch_deg = 0;
    Check(gst::CompareGridSheetTilts(ra.outcome, rb.outcome, impossible).refusal == gst::GridSheetPairRefusal::turn_mismatch, "a mismatch above the line is refused");
    for (double line : {-1.0, 45.0, std::numeric_limits<double>::quiet_NaN()}) {
        gst::GridSheetPairAcceptance bad;
        bad.max_rotation_mismatch_deg = line;
        bool threw = false;
        try { (void)gst::CompareGridSheetTilts(ra.outcome, rb.outcome, bad); } catch (const std::invalid_argument&) { threw = true; }
        Check(threw, "a mismatch line outside [0, 45) is an invalid argument");
    }
    // The same camera pair with the roles swapped: the angle is symmetric, the difference changes sign.
    const auto forward = gst::CompareGridSheetTilts(ra.outcome, rb.outcome, ok), backward = gst::CompareGridSheetTilts(rb.outcome, ra.outcome, ok);
    Check(forward.pair && backward.pair, "both orders compare");
    if (forward.pair && backward.pair) CheckNear(forward.pair->optical_axis_angle_deg, backward.pair->optical_axis_angle_deg, 1e-9, "the angle between the axes is symmetric");
}

// ---- the result is only for reading ------------------------------------------------------------------------------------------------
void Isolation() {
    using gst::GridSheetPair;
    using gst::GridSheetTilt;
    using gst::GridSheetTiltOutcome;
    static_assert(!std::is_convertible_v<GridSheetTilt, cr::PoseParameters>);
    static_assert(!std::is_constructible_v<cr::PoseParameters, GridSheetTilt>);
    static_assert(!std::is_constructible_v<cr::PoseParameters, GridSheetTiltOutcome>);
    static_assert(!std::is_convertible_v<GridSheetTilt, cr::CameraOrientation>);
    static_assert(!std::is_constructible_v<cr::CameraOrientation, GridSheetTilt>);
    static_assert(!std::is_convertible_v<GridSheetPair, cr::RelativeOrientation>);
    static_assert(!std::is_constructible_v<cr::RelativeOrientation, GridSheetPair>);
    static_assert(!std::is_constructible_v<a0::m2::experimental::SurfacePlane, GridSheetTilt>);
    static_assert(!std::is_convertible_v<cr::CameraOrientation, GridSheetTilt>);
    static_assert(!std::is_convertible_v<GridSheetTilt, render::CameraProjection>);
    static_assert(!std::is_constructible_v<render::CameraProjection, GridSheetTilt>);
    static_assert(!std::is_convertible_v<GridSheetTilt, cr::RelativeOrientation>);
    static_assert(!std::is_constructible_v<cr::RelativeOrientation, GridSheetTilt>);
    static_assert(!std::is_convertible_v<GridSheetPair, cr::CameraOrientation>);
    static_assert(!std::is_constructible_v<cr::CameraOrientation, GridSheetPair>);
    static_assert(!std::is_constructible_v<cr::PoseParameters, GridSheetPair>);
    static_assert(!std::is_convertible_v<GridSheetTilt, a0::m2::experimental::SurfacePlane>);
    static_assert(!std::is_convertible_v<GridSheetTiltOutcome, render::CameraProjection>);
    static_assert(!std::is_convertible_v<gst::GridSheetPairOutcome, cr::RelativeOrientation>);
    Check(true, "the result types do not convert to a pose, an orientation or a surface");
}

// ---- the working draft settings themselves, on full size pictures ------------------------------------------------------------------
// The product's starting values (not the test's), on pictures of the profile's full size: a clean sheet is measured, a bent
// sheet is refused, and a sheet under a bundle of other lines or seen at a pitch outside the assumed range is either
// refused or measured without being pulled off.
std::vector<gs::Segment> LineBundle(const gs::Rig& rig, double degrees, double gap_min, double gap_max, std::uint64_t seed) {
    std::vector<gs::Segment> bundle;
    const double w = rig.width, h = rig.height, th = degrees * gs::kDeg;
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> gap(gap_min, gap_max);
    for (double s = -0.6 * w; s < 0.9 * w; s += gap(rng)) {
        const double x0 = 0.3 * w + s * std::cos(th + kPi / 2) - 0.8 * w * std::cos(th), y0 = 0.5 * h + s * std::sin(th + kPi / 2) - 0.8 * w * std::sin(th);
        bundle.push_back({x0, y0, x0 + 1.6 * w * std::cos(th), y0 + 1.6 * w * std::sin(th), 50, 1.0});
    }
    return bundle;
}

void WorkingDraftDefaults() {
    const auto settings = gst::WorkingDraftSettings();
    const Camera c = MakeCamera(1.0, 0, 180, -1.1, 1.6, 0.2);
    const gs::Truth truth = gs::TruthOf(c.pose), profile_truth = gs::TruthOf(c.profile_pose);
    {
        const Run run = Measure(SceneOf(c, 37), c, settings);
        Print("defaults clean", run.outcome, truth, profile_truth, run.seconds);
        Check(run.outcome.tilt.has_value(), "defaults: a clean full size sheet is measured");
        if (run.outcome.tilt) {
            const Error e = ErrorOf(*run.outcome.tilt, truth, profile_truth);
            CheckNear(e.tilt, 0, kTiltTolerance, "defaults clean tilt");
            CheckNear(e.lean_screen, 0, kLeanTolerance, "defaults clean lean");
            CheckEvidence(run.outcome, "defaults clean");
        }
    }
    {
        gs::Scene s = SceneOf(c, 37);
        s.fold = true; s.fold_x_mm = 0; s.fold_deg = 12;
        const Run run = Measure(s, c, settings);
        Print("defaults fold", run.outcome, truth, profile_truth, run.seconds);
        const auto r = run.outcome.refusal;
        Check(!run.outcome.tilt.has_value() && (r == gst::GridSheetRefusal::sheet_not_flat || r == gst::GridSheetRefusal::residual_too_large ||
            r == gst::GridSheetRefusal::methods_disagree || r == gst::GridSheetRefusal::grid_not_square), "defaults: a bent full size sheet is refused");
    }
    // A value under a disturbance is accepted only when it is not pulled off: tighter than the clean tolerance allows for
    // the disturbance, far below any tolerance a person would read the tilt to.
    constexpr double kDisturbed = 0.05;
    struct Disturbed { std::string name; gs::Scene scene; Camera camera; };
    std::vector<Disturbed> cases;
    {
        gs::Scene s = SceneOf(c, 37);
        s.extra = LineBundle(c.rig, 20, 12, 36, 21);
        cases.push_back({"dense bundle", s, c});
        s.extra = LineBundle(c.rig, 20, 25, 75, 22);
        cases.push_back({"wide bundle", s, c});
    }
    {
        // the camera over the sheet at a height where one pitch is about 150 pixels, above the assumed range
        const Camera near = MakeCamera(1.0, 0, 180, 1.4, -0.9, 0, 0.4, -0.3, 667);
        cases.push_back({"pitch above the range", SceneOf(near, 23), near});
    }
    for (const auto& d : cases) {
        const Run run = Measure(d.scene, d.camera, settings);
        const gs::Truth t = gs::TruthOf(d.camera.pose), pt = gs::TruthOf(d.camera.profile_pose);
        Print("defaults " + d.name, run.outcome, t, pt, run.seconds);
        if (run.outcome.tilt) {
            const Error e = ErrorOf(*run.outcome.tilt, t, pt);
            CheckNear(e.tilt, 0, kDisturbed, "defaults " + d.name + ": a value given is not pulled off (tilt)");
            CheckNear(e.lean_screen, 0, kDisturbed, "defaults " + d.name + ": a value given is not pulled off (lean)");
        } else {
            Check(run.outcome.refusal != gst::GridSheetRefusal::none, "defaults " + d.name + ": a refusal names its reason");
        }
    }
}

void Speed() {
    const double scale = 1.0;
    const auto settings = TestSettings(scale);
    const Camera c = MakeCamera(scale, 0, 180, 1.3, -2.4, 0.2);
    const gs::Scene scene = SceneOf(c, 37);
    const auto draw = std::chrono::steady_clock::now();
    const render::BgrImage image = gs::Renderer(scene).Render();
    const double draw_seconds = Seconds(draw);
    double best = 1e9;
    gst::GridSheetTiltOutcome outcome;
    for (int i = 0; i < 3; ++i) {
        const auto start = std::chrono::steady_clock::now();
        outcome = gst::MeasureGridSheetTilt("CAM-T", gst::ViewOf(image), c.profile(), settings);
        best = std::min(best, Seconds(start));
    }
    const gs::Truth truth = gs::TruthOf(c.pose), profile_truth = gs::TruthOf(c.profile_pose);
    Print("full size " + std::to_string(image.width) + "x" + std::to_string(image.height), outcome, truth, profile_truth, best);
    std::cout << "speed: drawing the picture took " << std::fixed << std::setprecision(1) << draw_seconds << " s; the best of three measurements took " << best << " s; ridge points "
              << outcome.evidence.ridge_points << ", used " << outcome.evidence.used_points << "\n";
    Check(outcome.tilt.has_value(), "the full size picture is measured");
    if (outcome.tilt) {
        const Error e = ErrorOf(*outcome.tilt, truth, profile_truth);
        CheckNear(e.tilt, 0, kTiltTolerance, "full size tilt");
        CheckNear(e.lean_screen, 0, kLeanTolerance, "full size lean");
    }
    Check(best < 60.0, "a full size picture is measured in under a minute");
    auto one_thread = settings;
    one_thread.detection.worker_threads = 1;
    const auto start = std::chrono::steady_clock::now();
    const auto single = gst::MeasureGridSheetTilt("CAM-T", gst::ViewOf(image), c.profile(), one_thread);
    std::cout << "speed: one worker thread " << Seconds(start) << " s\n";
    Check(single.tilt.has_value() && outcome.tilt.has_value() && single.tilt->tilt_deg == outcome.tilt->tilt_deg, "the thread count does not change the result");
}

// ---- the lattice map and the largest residual that a measured value carries ---------------------------------------------------------
// The returned map is the last fit's lattice-to-plane matrix: eight free numbers, as fitted. Its truth is the drawn sheet: a lattice
// point goes to the plane and through the lens to a picture position, where the drawn sheet has its own lattice coordinates.
det::Mat3 InverseOf(const det::Mat3& m) {
    const double c0 = m[4] * m[8] - m[5] * m[7], c1 = m[5] * m[6] - m[3] * m[8], c2 = m[3] * m[7] - m[4] * m[6];
    const double d = m[0] * c0 + m[1] * c1 + m[2] * c2;
    return {c0 / d, (m[2] * m[7] - m[1] * m[8]) / d, (m[1] * m[5] - m[2] * m[4]) / d,
            c1 / d, (m[0] * m[8] - m[2] * m[6]) / d, (m[2] * m[3] - m[0] * m[5]) / d,
            c2 / d, (m[1] * m[6] - m[0] * m[7]) / d, (m[0] * m[4] - m[1] * m[3]) / d};
}
// The drawn sheet's lattice coordinates at the picture position that the returned map gives the lattice point (i, j).
bool TrueLatticeAt(const gs::Renderer& drawn, const det::Lens& lens, const det::Mat3& map, double i, double j, double& lx, double& ly) {
    const double w = map[6] * i + map[7] * j + map[8];
    const double x = (map[0] * i + map[1] * j + map[2]) / w, y = (map[3] * i + map[4] * j + map[5]) / w;
    double xd = 0, yd = 0;
    det::DistortNormalized(lens, x, y, xd, yd);
    return drawn.LatticeAt(lens.fx * xd + lens.cx, lens.fy * yd + lens.cy, lx, ly);
}
struct Recomputed {
    bool ok{};
    det::Mat3 map{};
    double residual_max_px{};
};
// The stages in the order the measurement runs them, up to the last fit: what the returned values must be, bit for bit.
Recomputed RecomputeLastFit(const Camera& c, const render::BgrImage& image, const gst::GridSheetTiltSettings& settings) {
    const auto& d = settings.detection;
    const det::RidgeResult ridge = det::DetectRidges(gst::ViewOf(image), d);
    const det::Lens lens = det::LensOf(c.projection());
    const double focal = lens.MeanFocal();
    const det::PreparedPoints points = det::PreparePoints(ridge.points, lens, true);
    Recomputed r;
    const auto initial = det::FindInitialLattice(points, focal, d);
    if (!initial) return r;
    const det::LatticeFit fit = det::FitLattice(points, focal, *initial, d);
    r.ok = fit.ok && fit.converged;
    r.map = det::LatticeToPlane(fit.parameters);
    const det::Assignment a = det::AssignPoints(fit.parameters, points, focal, d.direction_match_deg, d.final_gate_px);
    for (std::size_t i = 0; i < points.size(); ++i)
        if (a.ok[i]) r.residual_max_px = std::max(r.residual_max_px, static_cast<double>(a.distance_px[i]));
    return r;
}
void PrintBits(int index, const gst::GridSheetTiltOutcome& o) {
    const auto& t = *o.tilt;
    std::printf("dump %d tilt %a lean_sheet %a %a lean_screen %a %a lean_drawing %a %a azimuth %a cal %a fit %a nodes %a vanish %a spread %a rms %a vis %a\n", index, t.tilt_deg,
        t.lean_sheet_deg.x, t.lean_sheet_deg.y, t.lean_screen_deg.x, t.lean_screen_deg.y, t.lean_drawing_deg.x, t.lean_drawing_deg.y, t.image_x_azimuth_deg,
        t.calibration_x_azimuth_deg, o.evidence.tilt_deg_fit, o.evidence.tilt_deg_nodes, o.evidence.tilt_deg_vanishing, o.evidence.method_spread_deg,
        o.evidence.residual_rms_px, o.evidence.visible_fraction);
}

void MappingReturned() {
    const double scale = 0.5;
    const auto settings = TestSettings(scale);
    // Poses, mat turns and image-axis mounts that put the fit's numbering of the lattice at several quarter turns against the drawn one.
    struct Item { double rx, ry, rz, mat, mount; };
    const Item items[] = {{1.2, -0.8, 0.2, 37, 180}, {-2.5, 1.5, -0.3, 123, 90}, {3.0, 0.0, 0.8, 10, 37}};
    int index = 0;
    for (const auto& it : items) {
        const std::string name = "mapping " + std::to_string(index);
        const int this_index = index++;
        const Camera c = MakeCamera(scale, 0, it.mount, it.rx, it.ry, it.rz);
        const gs::Scene scene = SceneOf(c, it.mat);
        const gs::Renderer drawn(scene);
        const render::BgrImage image = drawn.Render();
        const auto outcome = gst::MeasureGridSheetTilt("CAM-T", gst::ViewOf(image), c.profile(), settings);
        Check(outcome.tilt.has_value(), name + " is measured");
        if (!outcome.tilt) continue;
        const gst::GridSheetTilt& t = *outcome.tilt;
        PrintBits(this_index, outcome);

        // (1) what the map is: the last fit's matrix and the largest distance of a used point, bit for bit.
        const Recomputed again = RecomputeLastFit(c, image, settings);
        Check(again.ok, name + ": the stages run again settle");
        bool same_map = true;
        for (std::size_t k = 0; k < 9; ++k) same_map = same_map && t.lattice_to_plane[k] == again.map[k];
        Check(same_map, name + ": the returned map is the last fit's lattice-to-plane matrix");
        Check(t.residual_max_px == again.residual_max_px, name + ": the returned largest residual is the largest distance of a used point");
        Check(t.lattice_to_plane[8] == 1.0, name + ": the map is scaled so that its last number is one");
        Check(t.residual_max_px > outcome.evidence.residual_rms_px && t.residual_max_px <= settings.detection.final_gate_px,
            name + ": the largest residual lies between the rms and the final gate");

        // (2) returning the map does not change the tilt: the map gives the reported numbers back, bit for bit.
        const det::Mat3& map = t.lattice_to_plane;
        const det::OrientationNumbers back = det::OrientationOfLattice(c.projection(), map);
        Check(back.ok && back.tilt_deg == t.tilt_deg && back.lean_sheet_deg.x == t.lean_sheet_deg.x && back.lean_sheet_deg.y == t.lean_sheet_deg.y &&
            back.image_x_azimuth_deg == t.image_x_azimuth_deg, name + ": the map gives the reported tilt, lean and azimuth back bit for bit");

        // (3) where it points: the map takes lattice points to the true lattice points of the drawn sheet. The fit numbers the lattice by
        // its own: it differs from the drawn one by whole pitches and a quarter turn, which the first node and its neighbours tell.
        const det::Lens lens = det::LensOf(c.projection());
        const det::Mat3 inverse = InverseOf(map);
        std::vector<std::array<double, 2>> nodes;
        for (double fu : {0.2, 0.4, 0.6, 0.8}) {
            for (double fv : {0.25, 0.5, 0.75}) {
                double x = 0, y = 0;
                const double u = fu * c.rig.width, v = fv * c.rig.height;
                if (!det::UndistortNormalized(lens, (u - lens.cx) / lens.fx, (v - lens.cy) / lens.fy, x, y)) continue;
                const double w = inverse[6] * x + inverse[7] * y + inverse[8];
                nodes.push_back({std::round((inverse[0] * x + inverse[1] * y + inverse[2]) / w), std::round((inverse[3] * x + inverse[4] * y + inverse[5]) / w)});
            }
        }
        Check(nodes.size() == 12, name + ": lattice points near twelve picture positions are found");
        if (nodes.empty()) continue;
        const double i0 = nodes[0][0], j0 = nodes[0][1];
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        const bool found = TrueLatticeAt(drawn, lens, map, i0, j0, x0, y0) && TrueLatticeAt(drawn, lens, map, i0 + 1, j0, x1, y1) &&
            TrueLatticeAt(drawn, lens, map, i0, j0 + 1, x2, y2);
        Check(found, name + ": the true lattice is defined at the first nodes");
        if (!found) continue;
        const double qa = std::round(x1 - x0), qb = std::round(y1 - y0), qc = std::round(x2 - x0), qd = std::round(y2 - y0);
        Check(std::abs(qa * qd - qb * qc - 1.0) < 1e-9 && std::abs(qa) + std::abs(qb) == 1.0 && std::abs(qc) + std::abs(qd) == 1.0,
            name + ": the fit's numbering is a quarter turn of the drawn one, not a mirror image");
        const double sx = std::round(x0), sy = std::round(y0);
        double worst = 0;
        for (const auto& n : nodes) {
            double lx = 0, ly = 0;
            if (!TrueLatticeAt(drawn, lens, map, n[0], n[1], lx, ly)) { worst = 1e9; continue; }
            const double ex = sx + qa * (n[0] - i0) + qc * (n[1] - j0), ey = sy + qb * (n[0] - i0) + qd * (n[1] - j0);
            worst = std::max(worst, std::hypot(lx - ex, ly - ey));
        }
        std::cout << "  " << name << ": the largest distance of a returned map's lattice point from the true one is " << std::setprecision(4) << worst << " pitch\n";
        CheckNear(worst, 0, kMapTolerancePitch, name + ": the map takes lattice points to the true lattice points");
    }
    {   // a refused measurement carries no value, so no map and no largest residual
        const Camera c = MakeCamera(scale, 0, 180, 1.0, -1.0, 0);
        gs::Scene s = SceneOf(c, 0);
        s.region[0] = s.region[1] = 0; s.region[2] = s.region[3] = 0;
        const auto refused = Measure(s, c, settings).outcome;
        Check(!refused.tilt.has_value() && refused.refusal != gst::GridSheetRefusal::none, "a refused measurement has no map");
    }
}

// ---- a sheet of paper over the middle of the mat ------------------------------------------------------------------------------------
// The mat is measured from what is left around the paper. The line for the share of the picture that holds grid points is the
// working draft's own (the other lines are the test's), so a change of that line shows here. `hole` is the paper: the drawn sheet
// is missing there.
struct PaperRun {
    gst::GridSheetTiltOutcome outcome;
    Error error;
};
PaperRun MeasureUnderPaper(const Camera& c, double mat, double hole_width, double hole_height, const gst::GridSheetTiltSettings& settings,
    const std::string& name) {
    gs::Scene scene = SceneOf(c, mat);
    scene.hole[0] = 0.5 - hole_width / 2; scene.hole[2] = 0.5 + hole_width / 2;
    scene.hole[1] = 0.5 - hole_height / 2; scene.hole[3] = 0.5 + hole_height / 2;
    const Run run = Measure(scene, c, settings);
    const gs::Truth truth = gs::TruthOf(c.pose), profile_truth = gs::TruthOf(c.profile_pose);
    Print(name, run.outcome, truth, profile_truth, run.seconds);
    PaperRun r;
    r.outcome = run.outcome;
    if (run.outcome.tilt) r.error = ErrorOf(*run.outcome.tilt, truth, profile_truth);
    return r;
}

void VisibleFractionLine() {
    const double scale = 0.5;
    const double line = gst::WorkingDraftSettings().acceptance.min_visible_fraction;
    Check(line == 0.2, "the working line for the share of the picture that holds grid points is a fifth");
    auto settings = TestSettings(scale);
    settings.acceptance.min_visible_fraction = line;
    const Camera c = MakeCamera(scale, 0, 180, 1.2, -0.8, 0.2);
    // The paper's size against the share that is left: measured above the line, refused below it.
    struct Paper { double side; bool measured; double visible_low, visible_high; };
    const Paper papers[] = {{0.88, true, 0.26, 0.30}, {0.90, true, 0.22, 0.26}, {0.92, true, 0.21, 0.24}, {0.94, false, 0.17, 0.20}, {0.96, false, 0.12, 0.16}};
    for (const auto& p : papers) {
        const std::string name = "paper " + std::to_string(p.side).substr(0, 4);
        const PaperRun r = MeasureUnderPaper(c, 37, p.side, p.side, settings, name);
        const double visible = r.outcome.evidence.visible_fraction;
        Check(visible >= p.visible_low && visible <= p.visible_high, name + ": the share of the picture that holds grid points is the intended one");
        if (p.measured) {
            Check(r.outcome.tilt.has_value(), name + ": a share above the line is measured");
            if (r.outcome.tilt) {
                CheckNear(r.error.tilt, 0, kTiltTolerance, name + " tilt");
                CheckNear(r.error.lean_screen, 0, kLeanTolerance, name + " lean (screen)");
                Check(r.outcome.evidence.fit_converged, name + ": the fit settled");
                CheckNear(r.outcome.evidence.lattice_aspect_ratio, 1, 0.002, name + ": the lattice is square");
                Check(r.outcome.evidence.method_spread_deg < 0.2, name + " three methods agree");
            }
        } else {
            Check(!r.outcome.tilt.has_value() && r.outcome.refusal == gst::GridSheetRefusal::view_too_small, name + ": a share below the line is refused as view-too-small");
        }
    }
}

}   // namespace

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    const auto wanted = [&](const char* name) { return only.empty() || only == name; };
    if (wanted("lens")) LensAgainstProduct();
    if (wanted("ridge")) { RidgeAccuracy(); RidgeAlongImageAxes(); RidgeThreadsAndBands(); }
    if (wanted("jacobian")) LatticeJacobian();
    if (wanted("assign")) AssignmentDirectionGate();
    if (wanted("shape")) LatticeShapes();
    if (wanted("fit")) LatticeFitRobust();
    if (wanted("decomposition")) Decomposition();
    if (wanted("isolation")) Isolation();
    if (wanted("accuracy")) Accuracy();
    if (wanted("layouts")) Layouts();
    if (wanted("distractors")) Distractors();
    if (wanted("refusals")) Refusals();
    if (wanted("visible")) VisibleFractionLine();
    if (wanted("mapping")) MappingReturned();
    if (wanted("pairs")) Pairs();
    if (wanted("defaults")) WorkingDraftDefaults();
    if (wanted("speed")) Speed();
    std::cout << checks - failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
