#include "a0/m2/experimental/registration_probe.hpp"
#include "a0/m2/seam_gate.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace probe = a0::m2::experimental::registration_probe;
namespace gate = a0::m2::seam_gate;
namespace render = a0::m2::render;
using render::Point;

namespace {
int checks = 0, failures = 0;
void Check(bool value, const std::string& what) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL " << what << '\n'; }
}
void CheckNear(double value, double expected, double tolerance, const std::string& what) {
    ++checks;
    if (!(std::abs(value-expected) <= tolerance)) {
        ++failures;
        std::cerr << "FAIL " << what << ": " << value << " expected " << expected << " +/- " << tolerance << '\n';
    }
}
constexpr double kPi = std::numbers::pi;

struct Lcg {
    std::uint64_t state;
    double Uniform() { state = state*6364136223846793005ull+1442695040888963407ull; return static_cast<double>(state>>11)/9007199254740992.0; }
};

// ---- A synthetic rig: two cameras over a flat sheet, the lens and the document matrix of a profile -----------------
constexpr unsigned kWidth = 1200, kHeight = 800;
constexpr double kFocal = 1560;
const render::LensDistortion kLens{-0.035, 0.012, 0.0, 0.0005, -0.0003};

render::Matrix3 Mul(const render::Matrix3& a, const render::Matrix3& b) {
    render::Matrix3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[3*i+j] += a[3*i+k]*b[3*k+j];
    return r;
}
render::Matrix3 RotationX(double a) { return {1,0,0,0,std::cos(a),-std::sin(a),0,std::sin(a),std::cos(a)}; }
render::Matrix3 RotationY(double a) { return {std::cos(a),0,std::sin(a),0,1,0,-std::sin(a),0,std::cos(a)}; }
render::Matrix3 RotationZ(double a) { return {std::cos(a),-std::sin(a),0,std::sin(a),std::cos(a),0,0,0,1}; }

// A camera at `depth` mm looking at the sheet point (x0,y0), rolled and tilted by small angles (degrees).
render::CameraProjection MakeCamera(double x0, double y0, double depth, double roll, double tilt_x, double tilt_y) {
    const double d = kPi/180;
    const auto rotation = Mul(RotationZ(roll*d), Mul(RotationX(tilt_x*d), RotationY(tilt_y*d)));
    // Camera-frame point of sheet point (x,y): R (x-x0, y-y0, 0) + (0,0,depth).
    const double c0[3]{rotation[0], rotation[3], rotation[6]}, c1[3]{rotation[1], rotation[4], rotation[7]};
    double t[3]{-x0*c0[0]-y0*c1[0], -x0*c0[1]-y0*c1[1], -x0*c0[2]-y0*c1[2]+depth};
    const double cx = (kWidth-1)/2.0, cy = (kHeight-1)/2.0;
    const render::Matrix3 k{kFocal,0,cx,0,kFocal,cy,0,0,1};
    const render::Matrix3 columns{c0[0],c1[0],t[0],c0[1],c1[1],t[1],c0[2],c1[2],t[2]};
    auto h = Mul(k, columns);
    const double n = h[8];
    for (auto& v : h) v /= n;
    h[8] = 1;
    return render::CameraProjection(render::CameraIntrinsics(kFocal,kFocal,cx,cy), kLens, h);
}

// The sheet point that a camera shows at a stored pixel: the lens is undone by fixed-point iteration, then the
// sheet matrix. Independent of the code under test.
bool SheetPointOf(const render::CameraProjection& camera, double u, double v, Point& out) {
    const auto& i = camera.intrinsics;const auto& d = camera.distortion;
    const double xd = (u-i.cx_pixels)/i.fx_pixels, yd = (v-i.cy_pixels)/i.fy_pixels;
    double x = xd, y = yd;
    for (int it = 0; it < 60; ++it) {
        const double r2 = x*x+y*y, radial = 1+d.k1*r2+d.k2*r2*r2+d.k3*r2*r2*r2;
        const double mx = x*radial+2*d.p1*x*y+d.p2*(r2+2*x*x), my = y*radial+d.p1*(r2+2*y*y)+2*d.p2*x*y;
        x += xd-mx; y += yd-my;
    }
    const auto inverse = render::Invert(camera.document_to_image);
    return render::TryTransform(inverse, {i.fx_pixels*x+i.cx_pixels, i.fy_pixels*y+i.cy_pixels}, out);
}

// A sheet without a lattice or a repeat: a sum of plane waves, in DN.
struct Sheet {
    struct Wave {double kx, ky, phase, amplitude;};
    std::vector<Wave> waves;
    explicit Sheet(std::uint64_t seed) {
        Lcg r{seed};
        const int n = 40;
        for (int k = 0; k < n; ++k) {
            const double wavelength = 4.0*std::pow(10.0, r.Uniform());   // 4 .. 40 mm
            const double angle = 2*kPi*r.Uniform();
            waves.push_back({2*kPi/wavelength*std::cos(angle), 2*kPi/wavelength*std::sin(angle), 2*kPi*r.Uniform(), 36.0*std::sqrt(2.0/n)});
        }
    }
    double At(double x, double y) const {
        double s = 150;
        for (const auto& w : waves) s += w.amplitude*std::cos(w.kx*x+w.ky*y+w.phase);
        return s;
    }
};
render::BgrImage RenderRaw(const render::CameraProjection& camera, const Sheet& sheet) {
    render::BgrImage image{kWidth, kHeight, std::vector<std::uint8_t>(static_cast<std::size_t>(kWidth)*kHeight*3)};
    for (unsigned v = 0; v < kHeight; ++v) for (unsigned u = 0; u < kWidth; ++u) {
        Point p{};
        std::uint8_t value = 0;
        if (SheetPointOf(camera, u, v, p)) value = static_cast<std::uint8_t>(std::clamp(std::lround(sheet.At(p.x, p.y)), 0L, 255L));
        const std::size_t at = (static_cast<std::size_t>(v)*kWidth+u)*3;
        image.bgr[at] = image.bgr[at+1] = image.bgr[at+2] = value;
    }
    return image;
}

render::OutputRaster MakeRaster() {
    const std::int64_t left = 10000, top = 10000, right = 780000, bottom = 350000;
    const std::uint32_t dpi = 48;
    const auto pixels = [dpi](std::int64_t extent) {return static_cast<std::uint32_t>((extent*dpi+25399)/25400);};
    return render::OutputRaster(render::DocumentRegionUm(left, top, right, bottom), dpi, pixels(right-left), pixels(bottom-top));
}

// ---- Fields built from a model, for the fits that need no image ------------------------------------------------------
std::vector<probe::Observation> Lattice(const probe::PlaneFrame& frame, const render::OutputRaster& raster, unsigned step, unsigned seed) {
    Lcg r{seed};
    std::vector<probe::Observation> out;
    const unsigned x0 = raster.width_pixels/3, x1 = raster.width_pixels*2/3;
    for (unsigned y = 100; y+100 < raster.height_pixels; y += step) for (unsigned x = x0; x < x1; x += step) {
        probe::Observation o;
        o.x = x; o.y = y; o.sigma_px = 0.05;
        o.one_directional = r.Uniform() < 0.25;
        if (o.one_directional) {
            const double a = 2*kPi*r.Uniform();
            o.normal = {std::cos(a), std::sin(a)};
        }
        out.push_back(o);
    }
    (void)frame;
    return out;
}
void Fill(std::vector<probe::Observation>& observations, const std::function<Point(double,double)>& truth) {
    for (auto& o : observations) {
        o.d = truth(o.x, o.y);
        o.visible_px = o.one_directional ? std::abs(o.normal.x*o.d.x+o.normal.y*o.d.y) : std::hypot(o.d.x, o.d.y);
    }
}

void PoseAlgebra() {
    const auto camera_b = MakeCamera(520, 180, 700, -0.4, 0.2, -0.1);
    const probe::PoseModel pose(camera_b);
    // p = 0: the plane homography is the identity and the camera matrix is the calibrated one.
    const auto g0 = pose.Homography({});
    const render::Matrix3 identity{1,0,0,0,1,0,0,0,1};
    for (int i = 0; i < 9; ++i) CheckNear(g0[i], identity[i], 1e-12, "pose p=0 homography entry "+std::to_string(i));
    const auto h0 = pose.CameraHomography({});
    for (int i = 0; i < 9; ++i) CheckNear(h0[i], camera_b.document_to_image[i], 1e-9*std::max(1.0, std::abs(camera_b.document_to_image[i])),
        "pose p=0 camera matrix entry "+std::to_string(i));
    CheckNear(pose.PivotDepthMm(), 700, 5, "pivot depth is the camera distance");
    CheckNear(pose.PivotDocumentMm().x, 520, 2, "pivot x is the sheet point on the optical axis");
    CheckNear(pose.PivotDocumentMm().y, 180, 2, "pivot y is the sheet point on the optical axis");

    // G maps the point the moved camera shows at a pixel onto the point the calibrated camera shows there.
    const probe::PoseParameters p{0.6, -0.5, 0.5, 3.0, -2.0, 0.8};
    auto moved = camera_b;
    moved.document_to_image = pose.CameraHomography(p);
    const auto g = pose.Homography(p);
    double worst = 0;
    for (const double u : {50.0, 400.0, 600.0, 1100.0}) for (const double v : {40.0, 300.0, 700.0}) {
        Point calibrated{}, truth{};
        Check(SheetPointOf(camera_b, u, v, calibrated) && SheetPointOf(moved, u, v, truth), "sheet points exist");
        const auto mapped = render::Transform(g, truth);
        worst = std::max(worst, std::hypot(mapped.x-calibrated.x, mapped.y-calibrated.y));
    }
    CheckNear(worst, 0, 1e-6, "G(moved sheet point) = calibrated sheet point (mm)");
    // The pivot does not move under a pure tilt.
    const auto tilt = pose.Homography({0.7, -0.4, 0, 0, 0, 0});
    const auto fixed = render::Transform(tilt, pose.PivotDocumentMm());
    CheckNear(std::hypot(fixed.x-pose.PivotDocumentMm().x, fixed.y-pose.PivotDocumentMm().y), 0, 1e-6, "a tilt keeps the pivot in place (mm)");
    // A degenerate camera is refused.
    auto behind = camera_b;
    behind.document_to_image = {-camera_b.document_to_image[0], -camera_b.document_to_image[1], -camera_b.document_to_image[2],
        -camera_b.document_to_image[3], -camera_b.document_to_image[4], -camera_b.document_to_image[5], 0, 0, -1};
    bool refused = false;
    try { probe::PoseModel bad(behind); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "a plane behind the camera is refused");
}

void Statistics() {
    const auto s = probe::StatsOf({4, 1, 3, 2, 10, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20});
    Check(s.count == 20, "stats count");
    CheckNear(s.mean, 10.5, 1e-12, "stats mean");
    CheckNear(s.p95, 19, 1e-12, "stats nearest-rank p95");
    CheckNear(s.maximum, 20, 1e-12, "stats maximum");
    Check(probe::StatsOf({}).count == 0, "stats of nothing");

    // Residual along the seam column: lattice rows 16, 48, 80, 112 (step 32); the row at 144 is missing, then 176.
    std::vector<probe::Observation> o;
    std::vector<double> r;
    for (const double y : {16.0, 48.0, 80.0, 112.0, 176.0, 400.0}) {
        probe::Observation a; a.x = 100; a.y = y; o.push_back(a); r.push_back(y == 16.0 ? 4.0 : y == 48.0 ? 2.0 : y == 80.0 ? 2.0 : y == 112.0 ? 0.0 : y == 176.0 ? 1.0 : 9.0);
    }
    // A second, farther observation on a row that already has one does not replace the nearer one.
    probe::Observation far; far.x = 125; far.y = 16; o.push_back(far); r.push_back(50.0);
    // An observation more than one step away is not used.
    probe::Observation out_of_reach; out_of_reach.x = 300; out_of_reach.y = 240; o.push_back(out_of_reach); r.push_back(70.0);
    const auto series = probe::SeamColumnResidual(o, r, 96, 32, 500);
    CheckNear(series.residual[16], 4.0, 1e-12, "seam series at a lattice row");
    CheckNear(series.residual[32], 3.0, 1e-12, "seam series interpolates between lattice rows");
    CheckNear(series.residual[96], 1.0, 1e-12, "seam series interpolates");
    Check(std::isnan(series.residual[10]), "seam series does not extrapolate before the first row");
    CheckNear(series.residual[144], 0.5, 1e-9, "seam series bridges one missing lattice row");
    Check(std::isnan(series.residual[250]), "seam series leaves a gap uncovered");
    CheckNear(series.residual[400], 9.0, 1e-12, "an isolated lattice row covers its own row");
    Check(series.stats.maximum == 9.0, "seam series maximum over covered rows");
    Check(series.covered_rows > 100 && series.covered_rows < 200, "seam series counts only covered rows");

    probe::SeamColumnSeries shape_series;
    shape_series.residual.assign(100, 0.0);
    for (int y = 0; y < 100; ++y) shape_series.residual[y] = (y < 10 || y >= 90) ? 3.0 : 0.5;
    const auto shape = probe::ShapeOf(shape_series);
    Check(shape.end_share > 0.9, "the residual sits at the ends");
    CheckNear(shape.middle_third_mean, 0.5, 1e-12, "middle third of the shape");
}


probe::Observation Make(double x, double y, bool one, Point normal, Point d, double sigma = 0.05) {
    probe::Observation o;
    o.x = x; o.y = y; o.one_directional = one; o.normal = normal; o.d = d; o.sigma_px = sigma;
    o.visible_px = one ? std::abs(normal.x*d.x+normal.y*d.y) : std::hypot(d.x, d.y);
    return o;
}

void ObservationAssembly() {
    gate::SeamMeasurement m;
    gate::SeamPoint two;
    two.x = 100; two.y = 50; two.status = gate::PointStatus::valid; two.kind = gate::PointKind::two_directional;
    two.displacement = {1, 2}; two.uncertainty_px = 0.05;
    gate::SeamPoint one;
    one.x = 164; one.y = 50; one.status = gate::PointStatus::valid; one.kind = gate::PointKind::one_directional;
    one.normal = {0, 1}; one.displacement = {0.3, 4}; one.uncertainty_px = 0.04;
    gate::SeamPoint ambiguous = two;
    ambiguous.x = 228; ambiguous.status = gate::PointStatus::ambiguous;
    gate::SeamPoint unconfirmed = two;
    unconfirmed.x = 292; unconfirmed.confirmation = gate::Confirmation::unconfirmed;
    m.points = {two, one, ambiguous, unconfirmed};
    const auto plain = probe::ObservationsOf(m);
    Check(plain.size() == 2, "only counted points become observations");
    CheckNear(plain[0].visible_px, std::hypot(1.0, 2.0), 1e-12, "two-directional visible value");
    CheckNear(plain[1].visible_px, 4.0, 1e-12, "one-directional visible value is the component across the lines");
    // A prior is added back: the displacement is against the unshifted placement.
    const auto shifted = probe::ObservationsOf(m, {-34, 5});
    CheckNear(shifted[0].d.x, -33, 1e-12, "shift added to x");
    CheckNear(shifted[0].d.y, 7, 1e-12, "shift added to y");
    CheckNear(shifted[1].visible_px, 9.0, 1e-12, "visible value follows the shift (across the lines)");
}

void FieldDifference() {
    const std::vector<probe::Observation> before{
        Make(100, 50, false, {}, {1, 2}), Make(164, 50, true, {0, 1}, {0, 5}), Make(228, 50, true, {1, 0}, {3, 0}),
        Make(292, 50, false, {}, {4, 4}), Make(356, 50, false, {}, {9, 9}), Make(420, 50, true, {0, 1}, {0, 1})};
    const std::vector<probe::Observation> after{
        Make(100, 50, false, {}, {-2, 3}), Make(164, 50, false, {}, {0, 7}), Make(228, 50, true, {0, 1}, {0, 8}),
        Make(292, 50, true, {1, 0}, {-5, 0}), Make(500, 50, false, {}, {0, 0}), Make(420, 50, true, {0.0871557, 0.9961947}, {0.1, 1.1})};
    const auto d = probe::DifferenceObservations(before, after);
    // (356,50) and (500,50) are on one side only; (228,50) has lines in two different directions.
    Check(d.size() == 4, "difference keeps the joined points with compatible directions (" + std::to_string(d.size()) + ")");
    if (d.size() != 4) return;
    Check(!d[0].one_directional, "two-directional on both sides stays two-directional");
    CheckNear(d[0].d.x, -3, 1e-12, "difference x");
    CheckNear(d[0].d.y, 1, 1e-12, "difference y");
    Check(d[1].one_directional && d[1].normal.y == 1, "a one-directional side keeps its direction");
    CheckNear(d[1].visible_px, 2, 1e-12, "difference across the lines");
    Check(d[2].one_directional && d[2].normal.x == 1, "the one-directional side of the later measurement keeps its direction");
    CheckNear(d[2].visible_px, 9, 1e-12, "difference across the lines (later side)");
    Check(d[3].one_directional, "directions within five degrees agree");
    CheckNear(d[0].sigma_px, std::hypot(0.05, 0.05), 1e-12, "uncertainties add in quadrature");
}
void DesignIsValid() {
    for (const unsigned spacing : {32u, 64u, 96u}) {
        bool ok = true;
        try { (void)probe::FieldDesign(spacing, 128); } catch (const std::invalid_argument&) { ok = false; }
        Check(ok, "field design is valid for lattice " + std::to_string(spacing));
    }
    bool refused = false;
    try { (void)probe::FieldDesign(63, 128); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "an odd lattice is refused");
}

// The fits on fields that follow a model exactly: the model is recovered, p = 0 gives no displacement.
void FitsOnExactFields() {
    const auto raster = MakeRaster();
    const auto frame = probe::FrameOfRaster(raster);
    const auto camera_b = MakeCamera(520, 180, 700, -0.4, 0.2, -0.1);
    const probe::PoseModel pose(camera_b);
    const probe::FitContext context{frame, &pose};

    // p = 0: a field of zeros gives p = 0 and no residual.
    {
        auto obs = Lattice(frame, raster, 48, 1);
        Fill(obs, [](double, double) { return Point{0, 0}; });
        const auto fit = probe::FitModel(probe::ModelKind::pose6, obs, context);
        Check(fit.fitted && fit.converged && fit.pose.has_value(), "pose fit of a zero field converges");
        const auto& p = *fit.pose;
        for (const double v : {p.rx_deg, p.ry_deg, p.rz_deg, p.tx_mm, p.ty_mm, p.tz_percent}) CheckNear(v, 0, 1e-6, "zero field gives p = 0");
        double worst = 0;
        for (const auto& o : obs) worst = std::max(worst, probe::ResidualOf(o, fit));
        CheckNear(worst, 0, 1e-6, "zero field leaves no residual");
    }
    // Pose: a known pose, mixed two- and one-directional points.
    {
        const probe::PoseParameters truth{0.6, -0.5, 0.5, 3.0, -2.0, 0.8};
        auto obs = Lattice(frame, raster, 48, 2);
        Fill(obs, [&](double x, double y) { return probe::PoseDisplacement(pose, frame, truth, x, y); });
        const auto fit = probe::FitModel(probe::ModelKind::pose6, obs, context);
        Check(fit.fitted && fit.converged && fit.pose.has_value(), "pose fit converges");
        const auto& p = *fit.pose;
        CheckNear(p.rx_deg, truth.rx_deg, 1e-4, "pose rx recovered");
        CheckNear(p.ry_deg, truth.ry_deg, 1e-4, "pose ry recovered");
        CheckNear(p.rz_deg, truth.rz_deg, 1e-4, "pose rz recovered");
        CheckNear(p.tx_mm, truth.tx_mm, 1e-3, "pose tx recovered");
        CheckNear(p.ty_mm, truth.ty_mm, 1e-3, "pose ty recovered");
        CheckNear(p.tz_percent, truth.tz_percent, 1e-4, "pose tz recovered");
        double worst = 0;
        for (const auto& o : obs) worst = std::max(worst, probe::ResidualOf(o, fit));
        CheckNear(worst, 0, 1e-4, "pose fit leaves no residual on a pose field");
        Check(fit.pose_sigma.has_value(), "pose fit reports its uncertainty");
        // A smaller model cannot explain it: the translation-only fit leaves a residual of pixels.
        const auto t = probe::FitModel(probe::ModelKind::translation, obs, context);
        double translation_worst = 0;
        for (const auto& o : obs) translation_worst = std::max(translation_worst, probe::ResidualOf(o, t));
        Check(translation_worst > 1.0, "a translation cannot explain a pose field");
    }
    // Pose with gross outliers (a placement one period off): the robust fit ignores them.
    {
        const probe::PoseParameters truth{-0.3, 0.4, -0.7, -2.5, 1.5, -0.6};
        auto obs = Lattice(frame, raster, 48, 3);
        Fill(obs, [&](double x, double y) { return probe::PoseDisplacement(pose, frame, truth, x, y); });
        Lcg r{99};
        unsigned planted = 0;
        for (auto& o : obs) if (r.Uniform() < 0.12) {
            o.d.x += 18+10*r.Uniform(); o.d.y -= 12+10*r.Uniform();
            o.visible_px = std::hypot(o.d.x, o.d.y);
            ++planted;
        }
        Check(planted > 5, "outliers planted");
        const auto fit = probe::FitModel(probe::ModelKind::pose6, obs, context);
        Check(fit.pose.has_value(), "robust pose fit returns a pose");
        const auto& p = *fit.pose;
        CheckNear(p.rx_deg, truth.rx_deg, 0.02, "robust pose rx");
        CheckNear(p.rz_deg, truth.rz_deg, 0.02, "robust pose rz");
        CheckNear(p.tx_mm, truth.tx_mm, 0.1, "robust pose tx");
        CheckNear(p.ty_mm, truth.ty_mm, 0.1, "robust pose ty");
        CheckNear(p.tz_percent, truth.tz_percent, 0.02, "robust pose tz");
        Check(fit.inliers + planted <= obs.size()+planted/4+1 && fit.inliers < obs.size(), "the outliers are rejected");
    }
    // Translation, similarity, similarity with vertical scale, second-order surface: exact fields.
    {
        const double cx = [&] { double s = 0; auto o = Lattice(frame, raster, 48, 4); for (auto& a : o) s += a.x; return s/o.size(); }();
        auto obs = Lattice(frame, raster, 48, 4);
        double cy = 0;
        for (const auto& o : obs) cy += o.y;
        cy /= obs.size();
        Fill(obs, [](double, double) { return Point{3.5, -2.25}; });
        auto fit = probe::FitModel(probe::ModelKind::translation, obs, context);
        CheckNear(fit.parameters[0], 3.5, 1e-6, "translation x");
        CheckNear(fit.parameters[1], -2.25, 1e-6, "translation y");
        Fill(obs, [&](double x, double y) {
            const double a = 4e-3, b = -2e-3, u = x-cx, v = y-cy;
            return Point{1.5+a*u-b*v, -0.5+b*u+a*v};
        });
        fit = probe::FitModel(probe::ModelKind::similarity, obs, context);
        CheckNear(fit.parameters[0], 1.5, 1e-5, "similarity tx");
        CheckNear(fit.parameters[1], -0.5, 1e-5, "similarity ty");
        CheckNear(fit.parameters[2], 4e-3, 1e-8, "similarity scale");
        CheckNear(fit.parameters[3], -2e-3, 1e-8, "similarity rotation");
        Fill(obs, [&](double x, double y) {
            const double a = 4e-3, b = -2e-3, e = 3e-3, u = x-cx, v = y-cy;
            return Point{1.5+a*u-b*v, -0.5+b*u+a*v+e*v};
        });
        fit = probe::FitModel(probe::ModelKind::similarity_vertical_scale, obs, context);
        CheckNear(fit.parameters[4], 3e-3, 1e-8, "vertical scale");
        double worst = 0;
        for (const auto& o : obs) worst = std::max(worst, probe::ResidualOf(o, fit));
        CheckNear(worst, 0, 1e-5, "five-parameter fit leaves no residual");
        fit = probe::FitModel(probe::ModelKind::similarity, obs, context);
        double four = 0;
        for (const auto& o : obs) four = std::max(four, probe::ResidualOf(o, fit));
        Check(four > 0.5, "four parameters cannot explain a vertical scale");
        Fill(obs, [&](double x, double y) {
            const double u = (x-cx)/1000.0, v = (y-cy)/1000.0;
            return Point{2+3*u-1*v+4*u*u-2*u*v+1*v*v, -1+0.5*u+2*v-3*u*u+1*u*v+2*v*v};
        });
        fit = probe::FitModel(probe::ModelKind::polynomial2, obs, context);
        worst = 0;
        for (const auto& o : obs) worst = std::max(worst, probe::ResidualOf(o, fit));
        CheckNear(worst, 0, 1e-5, "second-order surface is recovered");
    }
    // Projective: a real homography of the sheet, small.
    {
        auto obs = Lattice(frame, raster, 48, 5);
        double cx = 0, cy = 0;
        for (const auto& o : obs) {cx += o.x; cy += o.y;}
        cx /= obs.size(); cy /= obs.size();
        const double xm = frame.left_mm+(cx+0.5)*frame.pitch_mm, ym = frame.top_mm+(cy+0.5)*frame.pitch_mm;
        Fill(obs, [&](double x, double y) {
            const double X = frame.left_mm+(x+0.5)*frame.pitch_mm, Y = frame.top_mm+(y+0.5)*frame.pitch_mm;
            const double u = (X-xm)/1000.0, v = (Y-ym)/1000.0;
            const double h[8]{0.004, -0.002, 0.005, 0.003, -0.005, -0.004, 0.006, -0.003};
            const double w = h[6]*u+h[7]*v+1;
            const double mu = ((1+h[0])*u+h[1]*v+h[2])/w, mv = (h[3]*u+(1+h[4])*v+h[5])/w;
            return Point{(mu-u)*1000.0/frame.pitch_mm, (mv-v)*1000.0/frame.pitch_mm};
        });
        const auto fit = probe::FitModel(probe::ModelKind::projective8, obs, context);
        double worst = 0;
        for (const auto& o : obs) worst = std::max(worst, probe::ResidualOf(o, fit));
        CheckNear(worst, 0, 1e-4, "projective field is recovered");
        CheckNear(fit.parameters[6], 0.006, 1e-5, "projective h6");
    }
    // Too few points: not fitted, and the residual is the field itself.
    {
        auto obs = Lattice(frame, raster, 48, 6);
        obs.resize(4);
        Fill(obs, [](double, double) { return Point{1, 1}; });
        const auto fit = probe::FitModel(probe::ModelKind::pose6, obs, context);
        Check(!fit.fitted, "four points cannot fit six parameters");
        const auto& first = obs[0];
        CheckNear(probe::ResidualOf(first, fit), first.visible_px, 1e-12, "an unfitted model predicts nothing");
    }
    // A pose fit without the pose model is refused.
    {
        auto obs = Lattice(frame, raster, 48, 7);
        Fill(obs, [](double, double) { return Point{0, 0}; });
        bool refused = false;
        try { (void)probe::FitModel(probe::ModelKind::pose6, obs, probe::FitContext{frame, nullptr}); } catch (const std::invalid_argument&) { refused = true; }
        Check(refused, "pose fit needs the pose model");
    }
}

// ---- A rendered pair with a known pose change of B ------------------------------------------------------------------
unsigned Threads() { return std::max(2u, std::min(8u, std::thread::hardware_concurrency())); }

struct Measured {
    std::vector<probe::Observation> observations;
    double zero_field_maximum{};
};
Measured MeasurePair(const render::BgrImage& a, const render::BgrImage& b, const render::DocumentRenderParameters& nominal, unsigned& seam_column) {
    auto geometry = gate::DocumentSeamGeometry(nominal);
    auto design = probe::FieldDesign(32, 64);
    design.global_search_radius_px = 128;
    design.global_minimum_anchors = 2;
    geometry.band = gate::OverlapBand(geometry, kWidth, kHeight, kWidth, kHeight, gate::RequiredBandInsetPx(design));
    // The seam column of a condition: the middle of the band's widest row.
    std::uint32_t first = geometry.width, last = 0;
    for (const auto& row : geometry.band) if (row.begin < row.end) {first = std::min(first, row.begin); last = std::max(last, row.end);}
    seam_column = (first+last)/2;
    const gate::SeamGateLimits limits{1.0, 0.2, "test"};
    const auto m = gate::MeasureSeam(a, b, geometry, design, limits, nullptr, Threads());
    return {probe::ObservationsOf(m), 0};
}

void RenderedPairs() {
    const auto started = std::chrono::steady_clock::now();
    const auto raster = MakeRaster();
    const auto frame = probe::FrameOfRaster(raster);
    const auto camera_a = MakeCamera(270, 180, 700, 0.3, -0.15, 0.1);
    const auto camera_b = MakeCamera(520, 180, 700, -0.4, 0.2, -0.1);
    const render::DocumentRenderParameters nominal(raster, camera_a, camera_b, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bilinear);
    const probe::PoseModel pose(camera_b);
    const probe::FitContext context{frame, &pose};
    const Sheet sheet(7);
    const auto image_a = RenderRaw(camera_a, sheet);

    struct Case {std::string name; probe::PoseParameters truth; double rotation_tolerance_deg, translation_tolerance_mm, height_tolerance_percent;};
    const Case cases[]{
        {"no change", {0, 0, 0, 0, 0, 0}, 0.03, 0.1, 0.03},
        {"mixed change", {0.6, -0.5, 0.5, 3.0, -2.0, 0.8}, 0.03, 0.1, 0.03},
        {"height and roll", {0, 0, -0.9, 0, 0, -0.9}, 0.03, 0.1, 0.03},
    };
    for (const auto& c : cases) {
        auto moved = camera_b;
        moved.document_to_image = pose.CameraHomography(c.truth);
        const auto image_b = RenderRaw(moved, sheet);
        unsigned seam_column = 0;
        const auto measured = MeasurePair(image_a, image_b, nominal, seam_column);
        const auto& obs = measured.observations;
        Check(obs.size() >= 100, c.name+": enough counted points ("+std::to_string(obs.size())+")");
        // What the gate measures agrees with what the pose model predicts for the true pose.
        double worst_prediction = 0, field_maximum = 0;
        for (const auto& o : obs) {
            const auto p = probe::PoseDisplacement(pose, frame, c.truth, o.x, o.y);
            const double dx = o.d.x-p.x, dy = o.d.y-p.y;
            worst_prediction = std::max(worst_prediction, o.one_directional ? std::abs(o.normal.x*dx+o.normal.y*dy) : std::hypot(dx, dy));
            field_maximum = std::max(field_maximum, o.visible_px);
        }
        CheckNear(worst_prediction, 0, 0.35, c.name+": the measured field follows the pose model (px)");
        if (c.name == "no change") CheckNear(field_maximum, 0, 0.35, c.name+": no displacement when p = 0 (px)");
        else Check(field_maximum > 2.0, c.name+": the change shows in the field (max "+std::to_string(field_maximum)+" px)");

        const auto fit = probe::FitModel(probe::ModelKind::pose6, obs, context);
        Check(fit.fitted && fit.pose.has_value(), c.name+": pose fit");
        if (!fit.pose) continue;
        const auto& p = *fit.pose;
        std::cerr << c.name << ": n " << obs.size() << " rx " << p.rx_deg << " ry " << p.ry_deg << " rz " << p.rz_deg << " tx " << p.tx_mm
                  << " ty " << p.ty_mm << " tz " << p.tz_percent << " field max " << field_maximum << " worst-vs-model " << worst_prediction << '\n';
        CheckNear(p.rx_deg, c.truth.rx_deg, c.rotation_tolerance_deg, c.name+": rx");
        CheckNear(p.ry_deg, c.truth.ry_deg, c.rotation_tolerance_deg, c.name+": ry");
        CheckNear(p.rz_deg, c.truth.rz_deg, c.rotation_tolerance_deg, c.name+": rz");
        CheckNear(p.tx_mm, c.truth.tx_mm, c.translation_tolerance_mm, c.name+": tx");
        CheckNear(p.ty_mm, c.truth.ty_mm, c.translation_tolerance_mm, c.name+": ty");
        CheckNear(p.tz_percent, c.truth.tz_percent, c.height_tolerance_percent, c.name+": tz");

        // The residual along the seam column after the pose fit is sub-pixel; before it, it is not (when B moved).
        std::vector<double> before, after;
        probe::ModelFit none = probe::FitModel(probe::ModelKind::none, obs, context);
        for (const auto& o : obs) {before.push_back(probe::ResidualOf(o, none)); after.push_back(probe::ResidualOf(o, fit));}
        const auto seam_after = probe::SeamColumnResidual(obs, after, seam_column, 32, raster.height_pixels);
        const auto seam_before = probe::SeamColumnResidual(obs, before, seam_column, 32, raster.height_pixels);
        Check(seam_after.covered_rows > 200, c.name+": the seam column is covered");
        CheckNear(seam_after.stats.maximum, 0, 0.35, c.name+": seam column residual after the pose fit (px)");
        if (c.name != "no change") Check(seam_before.stats.maximum > 1.5*std::max(seam_after.stats.maximum, 0.1), c.name+": the fit removes most of the seam residual");
    }
    std::cerr << "rendered pairs took " << std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() << " s\n";
}
}

// The helpers of the pipeline check: the inverse of a mapping, and the seam-column components split by kind of point.
constexpr std::uint32_t kInverseWidth = 400, kInverseHeight = 300;
void PipelineHelpers() {
    constexpr std::uint32_t w = kInverseWidth, h = kInverseHeight;
    const render::OutputToCamera map = [](Point o, Point& raw) {
        if (o.x < -20 || o.y < -20 || o.x > kInverseWidth+20 || o.y > kInverseHeight+20) return false;
        raw = {1.02*o.x+0.01*o.y+5+1e-5*o.x*o.x, -0.005*o.x+0.98*o.y-3+2e-6*o.x*o.y};
        return true;
    };
    const probe::OutputInverse inverse(map, w, h, 32);
    double worst = 0;
    bool all_found = true;
    for (const Point o : {Point{0, 0}, Point{12.25, 7.5}, Point{199.5, 150.75}, Point{398.9, 298.2}, Point{250.125, 33.0}}) {
        Point raw{};
        (void)map(o, raw);
        const auto found = inverse.Find(raw, 1e-9);
        all_found = all_found && found.has_value();
        if (found) worst = std::max(worst, std::hypot(found->x-o.x, found->y-o.y));
    }
    Check(all_found, "inverse: every inner point is found");
    CheckNear(worst, 0, 1e-6, "inverse: the found output position maps back (px)");
    Check(!inverse.Find({-500, -500}).has_value(), "inverse: a raw position outside the raster's image is not found");
    const probe::OutputInverse nothing([](Point, Point&) {return false;}, w, h, 16);
    Check(!nothing.Find({10, 10}).has_value(), "inverse: a mapping without samples finds nothing");
    bool threw = false;
    try {(void)probe::OutputInverse(map, w, h, 1);} catch (const std::invalid_argument&) {threw = true;}
    Check(threw, "inverse: a grid step below 2 is refused");

    gate::SeamMeasurement m;
    const auto point = [](std::uint32_t x, gate::PointKind kind, Point n, Point d, gate::PointStatus status = gate::PointStatus::valid,
        gate::Confirmation confirmation = gate::Confirmation::not_required) {
        gate::SeamPoint p;
        p.x = x; p.y = 10; p.kind = kind; p.normal = n; p.displacement = d; p.status = status; p.confirmation = confirmation;
        return p;
    };
    m.points = {
        point(100, gate::PointKind::two_directional, {}, {0.6, -0.1}),
        point(104, gate::PointKind::two_directional, {}, {-0.8, 0.3}),
        point(96, gate::PointKind::one_directional, {0, 1}, {5, 0.2}),        // across the baseline: says nothing of dx
        point(108, gate::PointKind::one_directional, {1, 0}, {0.4, 9}),       // with the baseline
        point(130, gate::PointKind::two_directional, {}, {7, 7}),             // outside the half width
        point(99, gate::PointKind::two_directional, {}, {3, 3}, gate::PointStatus::ambiguous),
        point(101, gate::PointKind::two_directional, {}, {3, 3}, gate::PointStatus::valid, gate::Confirmation::unconfirmed),
        point(102, gate::PointKind::two_directional, {}, {3, 3}, gate::PointStatus::outside_coverage),
    };
    const auto c = probe::SeamComponentsOf(m, 100, 8, {1, 0}, 0.5);
    Check(c.counted == 4 && c.not_counted == 2, "components: counted and not counted points (outside coverage and width left out)");
    Check(c.two_along.count == 2 && c.two_across.count == 2, "components: two-directional points give both components");
    CheckNear(c.two_along.maximum_abs, 0.8, 1e-12, "components: largest baseline component");
    CheckNear(c.two_across.rms, std::sqrt((0.01+0.09)/2), 1e-12, "components: rms across the baseline");
    Check(c.one_with_baseline.count == 1 && c.one_across_baseline.count == 1, "components: one-directional points split by their normal");
    CheckNear(c.one_with_baseline.maximum_abs, 0.4, 1e-12, "components: a line across x gives n.d only");
    CheckNear(c.one_across_baseline.maximum_abs, 0.2, 1e-12, "components: a line across y gives n.d only, never |d|");
    const auto lines_only = probe::SeamComponentsOf(m, 96, 0, {1, 0}, 0.5);
    Check(lines_only.two_along.count == 0 && lines_only.two_along.maximum_abs == 0 && lines_only.one_across_baseline.count == 1,
        "components: a component no point measured has count 0 (unmeasured)");
    threw = false;
    try {(void)probe::SeamComponentsOf(m, 100, 8, {1, 1}, 0.5);} catch (const std::invalid_argument&) {threw = true;}
    Check(threw, "components: a baseline that is not a unit vector is refused");
}

int main() {
    PipelineHelpers();
    PoseAlgebra();
    Statistics();
    ObservationAssembly();
    FieldDifference();
    DesignIsValid();
    FitsOnExactFields();
    RenderedPairs();
    std::cout << checks-failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
