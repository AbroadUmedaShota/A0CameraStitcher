#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/grid_sheet_tilt.hpp"
#include "grid_sheet_scene.hpp"
#include "grid_sheet_tilt_detail.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
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
};
Camera MakeCamera(double scale, double x_mm, double mount_deg, double rx, double ry, double rz = 0, double profile_rx = 0.4, double profile_ry = -0.3) {
    Camera c;
    c.rig = gs::MakeRig(scale);
    c.profile_pose = gs::MakePose(x_mm, 0, 1200, mount_deg, profile_rx, profile_ry);
    c.pose = gs::MakePose(x_mm, 0, 1200, mount_deg, rx, ry, rz);
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
    r.outcome = gst::MeasureGridSheetTilt(name, gst::ViewOf(image), camera.projection(), settings);
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
    line << std::setprecision(3) << " rms " << o.evidence.residual_rms_px << " pts " << o.evidence.used_points << " lines " << o.evidence.lines[0] << "/"
         << o.evidence.lines[1] << " vis " << o.evidence.visible_fraction << " spread " << o.evidence.method_spread_deg << " off " << o.evidence.off_lattice_cell_fraction
         << " (" << seconds << " s)";
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

void Accuracy() {
    const double scale = 0.5;
    const auto settings = TestSettings(scale);
    struct Item { double rx, ry, rz, mat; };
    const Item items[] = {
        {0, 0, 0, 35},          {0.5, 0, 0, 0},         {0, -1.0, 0.4, -52},    {0.7, 0.7, -0.5, 123},   {-1.4, 1.4, 0, 90},
        {3.0, 0, 0.8, 37},      {-2.1, 2.1, 0, 10},     {3.5, -3.5, 0.3, -52},  {0, 5.0, 0, 35},         {5.0, 0, -0.6, 160},
    };
    int index = 0;
    for (const auto& it : items) {
        const Camera c = MakeCamera(scale, 0, 180, it.rx, it.ry, it.rz);
        const Run run = Measure(SceneOf(c, it.mat), c, settings);
        const gs::Truth truth = gs::TruthOf(c.pose), profile_truth = gs::TruthOf(c.profile_pose);
        const std::string name = "accuracy " + std::to_string(index++) + " tilt " + std::to_string(truth.tilt_deg).substr(0, 5) + " mat " + std::to_string(static_cast<int>(it.mat));
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
        expect("bent sheet", bent, {gst::GridSheetRefusal::residual_too_large, gst::GridSheetRefusal::sheet_not_flat, gst::GridSheetRefusal::methods_disagree});
        // With the other lines out of the way, the flatness check is what stops it.
        auto lenient = settings;
        lenient.acceptance.max_residual_rms_px = 5; lenient.acceptance.max_method_spread_deg = 5;
        expect("bent sheet, lenient residual and spread", Measure(s, c, lenient), {gst::GridSheetRefusal::sheet_not_flat});
        const Run flat = Measure(SceneOf(c, 20), c, settings);
        Check(flat.outcome.tilt.has_value(), "the same picture without the bend is measured");
    }
    // The lines of the caller decide: every acceptance value, made impossible one at a time.
    {
        const gs::Scene scene = SceneOf(c, 37);
        const render::BgrImage image = gs::Renderer(scene).Render();
        const auto run = [&](const gst::GridSheetTiltSettings& s) { return gst::MeasureGridSheetTilt("CAM-T", gst::ViewOf(image), c.projection(), s); };
        const auto good = run(settings);
        Check(good.tilt.has_value(), "the picture of the acceptance checks is measured");
        const auto change = [&](auto edit) { auto s = settings; edit(s.acceptance); return run(s); };
        Check(change([](gst::GridSheetAcceptance& a) { a.min_ridge_points = 100000000; }).refusal == gst::GridSheetRefusal::too_few_ridge_points, "ridge point line");
        Check(change([](gst::GridSheetAcceptance& a) { a.min_lines_per_family = 100000; }).refusal == gst::GridSheetRefusal::too_few_lines, "line count line");
        Check(change([](gst::GridSheetAcceptance& a) { a.min_lattice_span = 100000; }).refusal == gst::GridSheetRefusal::span_too_small, "span line");
        Check(change([](gst::GridSheetAcceptance& a) { a.min_visible_fraction = 1.01; }).refusal == gst::GridSheetRefusal::view_too_small, "visible fraction line");
        Check(change([](gst::GridSheetAcceptance& a) { a.max_residual_rms_px = 1e-6; }).refusal == gst::GridSheetRefusal::residual_too_large, "residual line");
        Check(change([](gst::GridSheetAcceptance& a) { a.max_method_spread_deg = 1e-9; }).refusal == gst::GridSheetRefusal::methods_disagree, "method spread line");
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
        bool threw = false;
        try { (void)gst::MeasureGridSheetTilt("X", gst::ImageView{}, c.projection(), settings); } catch (const std::invalid_argument&) { threw = true; }
        Check(threw, "an empty picture is an invalid argument");
        threw = false;
        auto bad = settings;
        bad.acceptance.min_lines_per_family = 0;
        try { (void)gst::MeasureGridSheetTilt("X", gst::ViewOf(image), c.projection(), bad); } catch (const std::invalid_argument&) { threw = true; }
        Check(threw, "unset acceptance is an invalid argument");
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
            gst::GridSheetPairAcceptance acceptance;
            acceptance.min_quarter_turn_margin_deg = 20;
            const auto pair = gst::CompareGridSheetTilts(ra.outcome, rb.outcome, acceptance);
            Check(pair.pair.has_value(), name + ": the pair is compared");
            if (!pair.pair) continue;
            const gs::Truth ta = gs::TruthOf(a.pose), tb = gs::TruthOf(b.pose);
            const double dot = ta.axis_doc[0] * tb.axis_doc[0] + ta.axis_doc[1] * tb.axis_doc[1] + ta.axis_doc[2] * tb.axis_doc[2];
            const double truth_angle = std::acos(std::clamp(dot, -1.0, 1.0)) / gs::kDeg;
            const Point truth_difference{tb.lean_doc_deg.x - ta.lean_doc_deg.x, tb.lean_doc_deg.y - ta.lean_doc_deg.y};
            const auto& p = *pair.pair;
            std::cout << std::fixed << std::setprecision(4) << "pair " << name << ": axis angle " << p.optical_axis_angle_deg << " (truth " << truth_angle << "), difference ("
                      << p.lean_difference_drawing_deg.x << ", " << p.lean_difference_drawing_deg.y << ") (truth " << truth_difference.x << ", " << truth_difference.y
                      << "), quarter turns " << p.quarter_turns << ", mismatch " << p.rotation_mismatch_deg << ", margin " << p.quarter_turn_margin_deg << '\n';
            CheckNear(p.optical_axis_angle_deg, truth_angle, 0.04, name + ": the angle between the optical axes");
            CheckNear(p.lean_difference_drawing_deg.x, truth_difference.x, 0.06, name + ": the difference (x)");
            CheckNear(p.lean_difference_drawing_deg.y, truth_difference.y, 0.06, name + ": the difference (y)");
            Check(std::abs(p.rotation_mismatch_deg) < 10, name + ": the numbering matches the calibration");
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
    // A numbering that cannot be told apart, and results that are not measured.
    const Camera a = MakeCamera(scale, -200, 180, 0.7, -1.1);
    const Camera b = MakeCamera(scale, 200, 178.2, -1.6, 2.2);
    const Run ra = Measure(SceneOf(a, 37), a, settings, "CAM-A"), rb = Measure(SceneOf(b, 37), b, settings, "CAM-B");
    gst::GridSheetPairAcceptance impossible;
    impossible.min_quarter_turn_margin_deg = 400;
    Check(gst::CompareGridSheetTilts(ra.outcome, rb.outcome, impossible).refusal == gst::GridSheetPairRefusal::quarter_turn_ambiguous, "an ambiguous numbering is refused");
    gst::GridSheetTiltOutcome none;
    gst::GridSheetPairAcceptance ok;
    ok.min_quarter_turn_margin_deg = 20;
    Check(gst::CompareGridSheetTilts(none, rb.outcome, ok).refusal == gst::GridSheetPairRefusal::reference_not_measured, "a reference that was not measured is named");
    Check(gst::CompareGridSheetTilts(ra.outcome, none, ok).refusal == gst::GridSheetPairRefusal::target_not_measured, "a target that was not measured is named");
    Check(!gst::CompareGridSheetTilts(none, none, ok).pair.has_value(), "no pair without both");
    gst::GridSheetPairAcceptance negative;
    negative.min_quarter_turn_margin_deg = -1;
    bool threw = false;
    try { (void)gst::CompareGridSheetTilts(ra.outcome, rb.outcome, negative); } catch (const std::invalid_argument&) { threw = true; }
    Check(threw, "an unset margin is an invalid argument");
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
    Check(true, "the result types do not convert to a pose, an orientation or a surface");
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
        outcome = gst::MeasureGridSheetTilt("CAM-T", gst::ViewOf(image), c.projection(), settings);
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
    const auto single = gst::MeasureGridSheetTilt("CAM-T", gst::ViewOf(image), c.projection(), one_thread);
    std::cout << "speed: one worker thread " << Seconds(start) << " s\n";
    Check(single.tilt.has_value() && outcome.tilt.has_value() && single.tilt->tilt_deg == outcome.tilt->tilt_deg, "the thread count does not change the result");
}

}   // namespace

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    const auto wanted = [&](const char* name) { return only.empty() || only == name; };
    if (wanted("lens")) LensAgainstProduct();
    if (wanted("ridge")) { RidgeAccuracy(); RidgeThreadsAndBands(); }
    if (wanted("jacobian")) LatticeJacobian();
    if (wanted("decomposition")) Decomposition();
    if (wanted("isolation")) Isolation();
    if (wanted("accuracy")) Accuracy();
    if (wanted("layouts")) Layouts();
    if (wanted("distractors")) Distractors();
    if (wanted("refusals")) Refusals();
    if (wanted("pairs")) Pairs();
    if (wanted("speed")) Speed();
    std::cout << checks - failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
