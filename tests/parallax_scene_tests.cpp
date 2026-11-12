// Tests of the parallax scene generator (tests/parallax_scene.hpp; #417 S3, design 11.2, 11.3, 11.10).
//
// What is checked is the generator: that its images and its truth follow the physics of the design, and that the
// checks bite (a flipped sign or a doubled k is caught). Nothing here estimates anything: no seam gate, estimator or
// fixed-composite code is linked. Camera and file free.
#include "parallax_scene.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ps = parallax_scene;
namespace cr = a0::m2::experimental::content_registration;
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
constexpr double kPi = ps::kPi;

struct Context {
    ps::Rig standard = ps::StandardRig();
    ps::Rig ideal = ps::IdealRig();
    ps::SeamPlacement seam = ps::PlaceSeam(standard);
    ps::SeamPlacement ideal_seam = ps::PlaceSeam(ideal);
    std::vector<ps::SceneSpec> catalog = ps::SceneCatalog(seam);

    [[nodiscard]] const ps::SceneSpec& Spec(const std::string& name) const {
        for (const auto& s : catalog) if (s.name == name) return s;
        throw std::runtime_error("no scene "+name);
    }
};

// ---- A copy of the pose tests' own renderer, as the reference for the h = 0 scenes ------------------------------------------------------
// (tests/content_registration_tests.cpp: SheetPointOf, WaveSheet, RenderRaw; copied, not shared, so that the generator is compared
// with what those tests render and a change there shows up here.)
struct RefLcg {
    std::uint64_t state;
    double Uniform() { state = state*6364136223846793005ull+1442695040888963407ull; return static_cast<double>(state>>11)/9007199254740992.0; }
};
bool RefSheetPointOf(const render::CameraProjection& camera, double u, double v, Point& out) {
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
using RefSheet = std::function<double(double, double)>;
RefSheet RefWaveSheet(std::uint64_t seed) {
    struct Wave {double kx, ky, phase, amplitude;};
    std::vector<Wave> waves;
    RefLcg r{seed};
    const int n = 40;
    for (int k = 0; k < n; ++k) {
        const double wavelength = 4.0*std::pow(10.0, r.Uniform());   // 4 .. 40 mm
        const double angle = 2*kPi*r.Uniform();
        waves.push_back({2*kPi/wavelength*std::cos(angle), 2*kPi/wavelength*std::sin(angle), 2*kPi*r.Uniform(), 36.0*std::sqrt(2.0/n)});
    }
    return [waves](double x, double y) {
        double s = 150;
        for (const auto& w : waves) s += w.amplitude*std::cos(w.kx*x+w.ky*y+w.phase);
        return s;
    };
}
render::BgrImage RefRenderRaw(const render::CameraProjection& camera, const RefSheet& sheet) {
    const unsigned kWidth = ps::kRawWidth, kHeight = ps::kRawHeight;
    render::BgrImage image{kWidth, kHeight, std::vector<std::uint8_t>(static_cast<std::size_t>(kWidth)*kHeight*3)};
    for (unsigned v = 0; v < kHeight; ++v) for (unsigned u = 0; u < kWidth; ++u) {
        Point p{};
        std::uint8_t value = 0;
        if (RefSheetPointOf(camera, u, v, p)) value = static_cast<std::uint8_t>(std::clamp(std::lround(sheet(p.x, p.y)), 0L, 255L));
        const std::size_t at = (static_cast<std::size_t>(v)*kWidth+u)*3;
        image.bgr[at] = image.bgr[at+1] = image.bgr[at+2] = value;
    }
    return image;
}
bool SameImage(const render::BgrImage& a, const render::BgrImage& b) {
    return a.width == b.width && a.height == b.height && a.bgr.size() == b.bgr.size() && a.bgr.size() > 0 &&
        std::memcmp(a.bgr.data(), b.bgr.data(), a.bgr.size()) == 0;
}
bool SameBytes(const std::vector<double>& a, const std::vector<double>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()*sizeof(double)) == 0);
}

// Gray value at a raw position, integer positions being the samples (as the generator renders them).
bool Bilinear(const render::BgrImage& image, double x, double y, double& value) {
    if (!(x >= 0 && y >= 0 && x < image.width-1.0 && y < image.height-1.0)) return false;
    const auto x0 = static_cast<unsigned>(x), y0 = static_cast<unsigned>(y);
    const double fx = x-x0, fy = y-y0;
    const auto at = [&](unsigned u, unsigned v) {return static_cast<double>(image.bgr[(static_cast<std::size_t>(v)*image.width+u)*3]);};
    value = (1-fy)*((1-fx)*at(x0, y0)+fx*at(x0+1, y0))+fy*((1-fx)*at(x0, y0+1)+fx*at(x0+1, y0+1));
    return true;
}

// ---- Geometry of the cameras -----------------------------------------------------------------------------------------------------------
void CameraGeometryTests(const Context& c) {
    struct Case {double x0, y0, depth, roll, tilt_x, tilt_y;};
    const Case cases[]{{270, 180, 700, 0.3, -0.15, 0.1}, {520, 180, 700, -0.4, 0.2, -0.1}, {100, 50, 650, 0, 3.0, -2.0}};
    for (const auto& k : cases) {
        const auto camera = ps::MakeCamera(k.x0, k.y0, k.depth, k.roll, k.tilt_x, k.tilt_y);
        const auto g = ps::GeometryOf(camera);
        // Third row of Rz Rx Ry is (-cos(a) sin(b), sin(a), cos(a) cos(b)) for a = tilt_x, b = tilt_y; the centre is
        // (x0, y0) - depth * (that row's first two entries), the height depth * (its third).
        const double a = k.tilt_x*kPi/180, b = k.tilt_y*kPi/180;
        CheckNear(g.center_mm.x, k.x0+k.depth*std::cos(a)*std::sin(b), 1e-9, "camera centre x");
        CheckNear(g.center_mm.y, k.y0-k.depth*std::sin(a), 1e-9, "camera centre y");
        CheckNear(g.height_mm, k.depth*std::cos(a)*std::cos(b), 1e-9, "camera height");

        // The rule T = C + (P - C) H / (H - h) against a plain pinhole (no lens): a paper point (x, y) at height h toward
        // the camera is at camera-frame depth along the optical axis, and its pixel must be the pixel of T.
        const render::CameraProjection pinhole(camera.intrinsics, render::LensDistortion(0, 0, 0, 0, 0), camera.document_to_image);
        const double d = kPi/180;
        const auto rotation = ps::Mul(ps::RotationZ(k.roll*d), ps::Mul(ps::RotationX(k.tilt_x*d), ps::RotationY(k.tilt_y*d)));
        for (const double h : {0.0, 5.0, 30.0}) for (const Point p : {Point{300, 150}, Point{450, 260}, Point{120, 40}}) {
            const double v[3]{p.x-k.x0, p.y-k.y0, -h};   // table frame, the height toward the camera is -w
            double cam[3]{};
            for (int r = 0; r < 3; ++r) cam[r] = rotation[3*r]*v[0]+rotation[3*r+1]*v[1]+rotation[3*r+2]*v[2];
            cam[2] += k.depth;
            const double u = ps::kFocal*cam[0]/cam[2]+camera.intrinsics.cx_pixels, w = ps::kFocal*cam[1]/cam[2]+camera.intrinsics.cy_pixels;
            ps::Surface s{p, h};
            Point raw{};
            const bool ok = render::TryProjectDocument(pinhole, ps::TablePointOfSurface(g, s), raw);
            Check(ok, "pinhole projection exists");
            CheckNear(raw.x, u, 1e-6, "the ray rule against a pinhole, u (px)");
            CheckNear(raw.y, w, 1e-6, "the ray rule against a pinhole, v (px)");
        }
    }
    // A pose that raises the plane in B's frame by tz percent of the pivot depth lifts the camera by about that much.
    const cr::PoseModel model(c.standard.camera_b);
    const double base = ps::GeometryOf(c.standard.camera_b).height_mm;
    const double raised = ps::GeometryOf(ps::MovedCamera(c.standard.camera_b, {0, 0, 0, 0, 0, 0.8})).height_mm;
    CheckNear(raised-base, 0.008*model.PivotDepthMm(), 0.2, "tz of the pose moves camera B's height by tz percent of the depth");
}

// ---- The surface solver --------------------------------------------------------------------------------------------------------------------
void Solver(const Context& c) {
    const auto geometry = ps::GeometryOf(c.standard.camera_a);
    // h = 0 returns the table point bit for bit.
    const ps::HeightField zero;
    bool exact = true;
    for (const Point t : {Point{123.456, 78.9}, Point{400.25, 300.125}, Point{-5, 1e3}}) {
        const auto s = ps::SolveSurface(zero, geometry, t);
        exact = exact && std::memcmp(&s.xy_mm.x, &t.x, sizeof(double)) == 0 && std::memcmp(&s.xy_mm.y, &t.y, sizeof(double)) == 0 && s.height_mm == 0;
    }
    Check(exact, "h = 0: the surface point is the table point, bit for bit");

    // On every scene field the fixed point satisfies P = T - (T - C) h(P) / H.
    double worst = 0;
    for (const auto& spec : c.catalog) {
        for (const Point t : {Point{300, 100}, Point{395, 180}, Point{480, 280}, Point{260, 330}}) {
            const auto s = ps::SolveSurface(spec.field, geometry, t);
            const double h = spec.field.Height(s.xy_mm.x, s.xy_mm.y);
            const double rx = s.xy_mm.x-(t.x-(t.x-geometry.center_mm.x)*h/geometry.height_mm), ry = s.xy_mm.y-(t.y-(t.y-geometry.center_mm.y)*h/geometry.height_mm);
            worst = std::max(worst, std::hypot(rx, ry));
            CheckNear(s.height_mm, h, 0, spec.name+": the solution carries the height at its own point");
        }
    }
    CheckNear(worst, 0, 1e-9, "the fixed point settles on every scene field (mm)");

    // A field too steep to be a single-valued surface does not settle and is refused, not returned.
    ps::HeightField steep;
    steep.steps.push_back({395, 330, 0, 30.0, 0.3});   // the ray moves along y by (T_y - C_y) h(P) / H, so the slope in y matters far from C_y
    bool refused = false;
    try { (void)ps::SolveSurface(steep, geometry, {395.2, 330.1}); } catch (const std::runtime_error&) { refused = true; }
    Check(refused, "a field too steep for the single-valued surface is refused");
    ps::HeightField reaching;
    reaching.plane.h0_mm = 400.0;
    refused = false;
    try { (void)ps::SolveSurface(reaching, geometry, {395, 180}); } catch (const std::runtime_error&) { refused = true; }
    Check(refused, "a paper that reaches the camera is refused");
}

// ---- h = 0 against the pose tests' own images and the pose model ------------------------------------------------------------------------
void ZeroField(const Context& c) {
    const cr::PoseModel model(c.standard.camera_b);
    const auto frame = cr::FrameOfRaster(c.standard.raster);
    const auto reference_a = RefRenderRaw(c.standard.camera_a, RefWaveSheet(7));
    const cr::PoseParameters poses[]{{0, 0, 0, 0, 0, 0}, {0.3, -0.2, 0.8, 3.0, -2.0, 0.5}};
    for (const auto& pose : poses) {
        auto moved = c.standard.camera_b;
        moved.document_to_image = model.CameraHomography(pose);
        const auto reference_b = RefRenderRaw(moved, RefWaveSheet(7));
        ps::SceneSpec spec = c.Spec("zero");
        spec.b_pose = pose;
        const auto scene = ps::MakeScene(c.standard, c.seam, spec);
        const std::string label = pose.IsZero() ? "zero pose" : "moved pose";
        Check(SameImage(scene.image_a, reference_a), label+": image A is the pose tests' image byte for byte");
        Check(SameImage(scene.image_b, reference_b), label+": image B is the pose tests' image byte for byte");
        // The truth is the pose displacement G(X) - X of the pose model.
        double worst = 0, largest = 0;
        unsigned rows = 0;
        for (unsigned y = 0; y < scene.truth.rows.size(); ++y) {
            const auto& r = scene.truth.rows[y];
            if (!r.covered) continue;
            const Point expected = cr::PoseDisplacement(model, frame, pose, c.seam.column, y);
            worst = std::max(worst, std::hypot(r.dx_px-expected.x, r.dy_px-expected.y));
            largest = std::max(largest, std::hypot(r.dx_px, r.dy_px));
            CheckNear(r.height_mm, 0, 0, label+": the height is 0");
            ++rows;
        }
        Check(rows > 300, label+": the seam column is covered");
        CheckNear(worst, 0, 1e-6, label+": the truth equals the pose displacement of the pose model (px)");
        if (pose.IsZero()) CheckNear(largest, 0, 1e-6, "zero pose: no displacement");
        else Check(largest > 2.0, "moved pose: the displacement is not vacuous");
        std::cerr << label << ": truth vs pose model " << worst << " px, largest displacement " << largest << " px over " << rows << " rows\n";
    }
}

// ---- Closed forms ----------------------------------------------------------------------------------------------------------------------------
// The design's table-level formula for the displacement of a paper point P (height h) seen by A at X, with camera B really at the
// scene's pose: X_B = G(p) (C_B + (P - C_B) H_B / (H_B - h)); the displacement is (X_B - X) rho. Independent of the lens, the
// pixel inversion and the projection chain of the generator.
Point ExactDisplacement(const ps::SceneModel& m, const cr::PoseModel& pose_model, const ps::Displacement& d, Point table) {
    const auto& b = m.geometry_b_true;
    const double scale = b.height_mm/(b.height_mm-d.height_mm);
    Point t{b.center_mm.x+(d.surface_mm.x-b.center_mm.x)*scale, b.center_mm.y+(d.surface_mm.y-b.center_mm.y)*scale};
    if (!m.spec.b_pose.IsZero()) {
        Point moved{};
        if (!render::TryTransform(pose_model.Homography(m.spec.b_pose), t, moved)) throw std::runtime_error("homography");
        t = moved;
    }
    return {(t.x-table.x)/m.frame.pitch_mm, (t.y-table.y)/m.frame.pitch_mm};
}

void LawOnEveryScene(const Context& c) {
    const cr::PoseModel pose_model(c.standard.camera_b);
    double worst_overall = 0;
    for (const auto& spec : c.catalog) {
        const ps::SceneModel model(c.standard, c.seam, spec);
        double worst = 0, largest = 0;
        unsigned used = 0;
        for (unsigned y = c.seam.first_row; y <= c.seam.last_row; y += 3) {
            const auto d = ps::DisplacementAt(model, c.seam.column, y);
            if (!d.covered) continue;
            const Point expected = ExactDisplacement(model, pose_model, d, model.TableOf(c.seam.column, y));
            worst = std::max(worst, std::hypot(d.dx_px-expected.x, d.dy_px-expected.y));
            largest = std::max(largest, std::hypot(d.dx_px, d.dy_px));
            ++used;
        }
        Check(used > 100, spec.name+": rows used");
        CheckNear(worst, 0, 1e-6, spec.name+": the truth equals the table-level formula (px)");
        if (spec.name != "zero") Check(largest > 0.05, spec.name+": the parallax is not vacuous (max "+std::to_string(largest)+" px)");
        worst_overall = std::max(worst_overall, worst);
    }
    std::cerr << "table-level formula vs generator over " << c.catalog.size() << " scenes: worst " << worst_overall << " px\n";
}

void PlaneProjection(const Context& c) {
    const cr::PoseModel pose_model(c.standard.camera_b);
    for (const char* name : {"flat_high", "flat_high_outside", "flat_tilted", "tilt_with_b_rotation_scale"}) {
        const auto& spec = c.Spec(name);
        const ps::SceneModel model(c.standard, c.seam, spec);
        const auto pi = ps::PlaneHomographyPi(model.geometry_a, model.geometry_b_true, spec.field.plane);
        render::Matrix3 g{1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (!spec.b_pose.IsZero()) g = pose_model.Homography(spec.b_pose);
        const auto total = ps::Mul(g, pi);
        double worst = 0, largest = 0;
        unsigned used = 0;
        for (const double fx : {-300.0, -150.0, 0.0, 150.0, 300.0}) for (unsigned y = c.seam.first_row; y <= c.seam.last_row; y += 40) {
            const double x = c.seam.column+fx;
            const auto d = ps::DisplacementAt(model, x, y);
            if (!d.covered) continue;
            const Point table = model.TableOf(x, y);
            Point placed{};
            if (!render::TryTransform(total, table, placed)) continue;
            worst = std::max(worst, std::hypot(d.dx_px-(placed.x-table.x)/model.frame.pitch_mm, d.dy_px-(placed.y-table.y)/model.frame.pitch_mm));
            largest = std::max(largest, std::hypot(d.dx_px, d.dy_px));
            ++used;
        }
        Check(used >= 20, std::string(name)+": points used for Pi");
        CheckNear(worst, 0, 1e-5, std::string(name)+": the truth equals G(p) Pi(pi) X - X (px)");
        std::cerr << name << ": truth vs G Pi " << worst << " px (largest " << largest << " px, " << used << " points)\n";
    }
    // With a level plane (no tilt) Pi is a pure translation, equal to -b h / (H - h), as design 11.3 says.
    for (const double h : {1.0, 4.0, 30.0}) {
        ps::Plane plane{h, 0, 0, c.ideal_seam.x_mm, c.ideal_seam.y_mid_mm};
        const auto a = ps::GeometryOf(c.ideal.camera_a), b = ps::GeometryOf(c.ideal.camera_b);
        const auto pi = ps::PlaneHomographyPi(a, b, plane);
        const double tx = -(b.center_mm.x-a.center_mm.x)*h/(a.height_mm-h), ty = -(b.center_mm.y-a.center_mm.y)*h/(a.height_mm-h);
        const render::Matrix3 expected{1, 0, tx, 0, 1, ty, 0, 0, 1};
        double worst = 0;
        for (int k = 0; k < 9; ++k) worst = std::max(worst, std::abs(pi[k]/pi[8]-expected[k]));
        CheckNear(worst, 0, 1e-12, "level plane: Pi is a translation by -b h / (H - h), h = "+std::to_string(h));
    }
}

// ---- The law of 11.2 on the level rig, and on the standard rig -------------------------------------------------------------------------------------
// Worst deviation (px) of the seam rows of a truth from "-k(h) h along e_b", with the sign and k scaled by the given factors.
double WorstDeviationFromKLaw(const ps::Truth& t, double factor) {
    double worst = 0;
    for (const auto& r : t.rows) {
        if (!r.covered) continue;
        const double parallax = factor*t.ParallaxPerMm(r.height_mm)*r.height_mm;
        worst = std::max(worst, std::hypot(r.dx_px+parallax*t.e_b.x, r.dy_px+parallax*t.e_b.y));
    }
    return worst;
}
// The relative size of the deviation that the checks allow: the law is exact for level cameras of equal height and holds to a fraction of
// a percent on the standard rig (design 11.2: the height difference and the y component of the baseline are small terms).
double LawTolerance(const ps::Truth& t) {
    double largest = 0;
    for (const auto& r : t.rows) if (r.covered) largest = std::max(largest, std::hypot(r.dx_px, r.dy_px));
    return 0.005*largest+1e-3;
}

void ParallaxLaw(const Context& c) {
    // Level cameras at the same height: exactly -b h / (H - h), along x, nothing across.
    for (const double h : {0.5, 1.0, 4.0, 10.0, 30.0}) {
        ps::SceneSpec spec;
        spec.name = "level";
        spec.field.plane = {h, 0, 0, c.ideal_seam.x_mm, c.ideal_seam.y_mid_mm};
        const ps::SceneModel model(c.ideal, c.ideal_seam, spec);
        const auto truth = ps::MakeTruth(model, {false});
        const auto a = model.geometry_a, b = model.geometry_b_true;
        const double rho = model.Rho();
        const double expected = -rho*(b.center_mm.x-a.center_mm.x)*h/(a.height_mm-h);
        double worst_x = 0, worst_y = 0;
        unsigned used = 0;
        for (const auto& r : truth.rows) {
            if (!r.covered) continue;
            worst_x = std::max(worst_x, std::abs(r.dx_px-expected));
            worst_y = std::max(worst_y, std::abs(r.dy_px));
            ++used;
        }
        Check(used > 300, "level rig: rows");
        CheckNear(worst_x, 0, 1e-6, "level rig, h = "+std::to_string(h)+": dx = -rho b h / (H - h) (px)");
        CheckNear(worst_y, 0, 1e-6, "level rig, h = "+std::to_string(h)+": dy = 0 (px)");
        CheckNear(truth.e_b.x, 1, 1e-12, "level rig: e_b is along x");
        CheckNear(WorstDeviationFromKLaw(truth, 1.0), 0, 1e-6, "level rig, h = "+std::to_string(h)+": parallax per mm equals k = rho |b| / (H - h)");
        Check(expected < 0, "a raised paper moves B's image toward A (toward -e_b)");
    }
    // Parallax per mm of height from two heights: the slope between them is k at the mean height of the interval (level rig).
    {
        const auto at = [&](double h) {
            ps::SceneSpec spec;
            spec.field.plane = {h, 0, 0, c.ideal_seam.x_mm, c.ideal_seam.y_mid_mm};
            const auto truth = ps::MakeTruth(ps::SceneModel(c.ideal, c.ideal_seam, spec), {false});
            return truth.rows[(c.ideal_seam.first_row+c.ideal_seam.last_row)/2];
        };
        const auto r1 = at(1.0), r2 = at(2.0);
        const auto truth = ps::MakeTruth(ps::SceneModel(c.ideal, c.ideal_seam, ps::SceneSpec{}), {false});
        // d(h) = -rho b h / (H - h); its derivative at h is -rho b H / (H - h)^2 = -k(h) H / (H - h), within 0.1 % of -k near 1.5 mm.
        const double slope = (r2.dx_px-r1.dx_px)/1.0;
        CheckNear(-slope, truth.ParallaxPerMm(1.5), 0.005*truth.ParallaxPerMm(1.5), "parallax per mm of height is k");
    }
    // The standard rig: along e_b to a fraction of a percent, the part across is the small term of 11.2.
    for (const char* name : {"flat_high", "flat_high_outside", "flat_tilted"}) {
        const auto& spec = c.Spec(name);
        const ps::SceneModel model(c.standard, c.seam, spec);
        const auto truth = ps::MakeTruth(model, {false});
        const double tolerance = LawTolerance(truth);
        const double deviation = WorstDeviationFromKLaw(truth, 1.0);
        CheckNear(deviation, 0, tolerance, std::string(name)+": the standard rig follows k(h) h along e_b");
        // Across e_b: bounded by rho h |H_B - H_A| |P - C| / ((H_A - h)(H_B - h)) (design 11.2), plus the lens-free rounding.
        double worst_across = 0, bound = 0;
        for (unsigned y = c.seam.first_row; y <= c.seam.last_row; y += 5) {
            const auto d = ps::DisplacementAt(model, c.seam.column, y);
            if (!d.covered) continue;
            const double across = std::abs(-d.dx_px*truth.e_b.y+d.dy_px*truth.e_b.x);
            const double dh = std::abs(model.geometry_b_true.height_mm-model.geometry_a.height_mm);
            const double reach = std::hypot(d.surface_mm.x-model.geometry_b_true.center_mm.x, d.surface_mm.y-model.geometry_b_true.center_mm.y);
            const double allowed = model.Rho()*d.height_mm*dh*reach/((model.geometry_a.height_mm-d.height_mm)*(model.geometry_b_true.height_mm-d.height_mm));
            worst_across = std::max(worst_across, across-allowed);
            bound = std::max(bound, allowed);
        }
        CheckNear(std::max(worst_across, 0.0), 0, 1e-6, std::string(name)+": the part across e_b is within the 11.2 bound");
        std::cerr << name << ": across-e_b bound " << bound << " px; k(0) = " << truth.ParallaxPerMm(0) << " px/mm; e_b = (" << truth.e_b.x << ", " << truth.e_b.y << ")\n";
    }
    // Tilt of the paper (11.2 table): a tilt along y makes dx grow with y, along x makes dx grow with x; dy is not made.
    {
        const auto& spec = c.Spec("flat_tilted");
        const ps::SceneModel model(c.standard, c.seam, spec);
        const auto truth = ps::MakeTruth(model, {false});
        const unsigned y1 = c.seam.first_row+20, y2 = c.seam.last_row-20;
        const double k = truth.ParallaxPerMm(spec.field.plane.h0_mm);
        const double slope_rows = (truth.rows[y2].dx_px-truth.rows[y1].dx_px)/((y2-y1)*model.frame.pitch_mm);   // px per mm of y
        CheckNear(slope_rows, -k*spec.field.plane.slope_y, 0.03*std::abs(k*spec.field.plane.slope_y), "tilt along y: dx changes with y by -k g_y");
        const auto left = ps::DisplacementAt(model, c.seam.column-150.0, (y1+y2)/2.0), right = ps::DisplacementAt(model, c.seam.column+150.0, (y1+y2)/2.0);
        const double slope_columns = (right.dx_px-left.dx_px)/(300.0*model.frame.pitch_mm);
        CheckNear(slope_columns, -k*spec.field.plane.slope_x, 0.03*std::abs(k*spec.field.plane.slope_x), "tilt along x: dx changes with x by -k g_x");
        double along_fraction = 0;
        for (unsigned y = y1; y <= y2; y += 10) along_fraction = std::max(along_fraction, std::abs(truth.rows[y].dy_px)-std::abs(truth.e_b.y/truth.e_b.x*truth.rows[y].dx_px));
        CheckNear(std::max(along_fraction, 0.0), 0, 0.02, "tilt: no dy beyond the baseline's y component");
    }
}

// The same checks must fail on a generator that is wrong; the faults are applied to a copy of the data.
void ChecksBite(const Context& c) {
    const auto& spec = c.Spec("flat_high_outside");
    const ps::SceneModel model(c.standard, c.seam, spec);
    auto truth = ps::MakeTruth(model, {false});
    const double tolerance = LawTolerance(truth);
    Check(WorstDeviationFromKLaw(truth, 1.0) <= tolerance, "baseline: the k law holds on the unbroken truth");
    // A flipped sign: every row moves the wrong way.
    auto flipped = truth;
    for (auto& r : flipped.rows) { r.dx_px = -r.dx_px; r.dy_px = -r.dy_px; }
    Check(WorstDeviationFromKLaw(flipped, 1.0) > 20*tolerance, "a flipped sign of the parallax is caught by the k law");
    // k doubled.
    auto doubled = truth;
    for (auto& r : doubled.rows) { r.dx_px *= 2; r.dy_px *= 2; }
    Check(WorstDeviationFromKLaw(doubled, 1.0) > 20*tolerance, "a doubled k is caught by the k law");
    // The law with the wrong factors on the true data is caught too.
    Check(WorstDeviationFromKLaw(truth, -1.0) > 20*tolerance, "the k law with the opposite sign does not hold");
    Check(WorstDeviationFromKLaw(truth, 2.0) > 20*tolerance, "the k law with k doubled does not hold");
    // The exact formula (table-level) catches a faulty direction as well.
    const cr::PoseModel pose_model(c.standard.camera_b);
    double worst_sign = 0, worst_double = 0;
    for (unsigned y = c.seam.first_row; y <= c.seam.last_row; y += 20) {
        auto d = ps::DisplacementAt(model, c.seam.column, y);
        if (!d.covered) continue;
        const Point expected = ExactDisplacement(model, pose_model, d, model.TableOf(c.seam.column, y));
        worst_sign = std::max(worst_sign, std::hypot(-d.dx_px-expected.x, -d.dy_px-expected.y));
        worst_double = std::max(worst_double, std::hypot(2*d.dx_px-expected.x, 2*d.dy_px-expected.y));
    }
    Check(worst_sign > 1.0, "the table-level formula catches a flipped sign");
    Check(worst_double > 1.0, "the table-level formula catches a doubled k");
}

// ---- Folds and steps -------------------------------------------------------------------------------------------------------------------------------------
struct Profile {
    std::vector<double> dx;   // per output row, NaN where the seam row is not covered
    double pitch_mm{};
    unsigned first_row{}, last_row{};
    double peak_px{};
    unsigned peak_row{};
};
Profile ProfileOf(const ps::Truth& t, double pitch_mm) {
    Profile p;
    p.pitch_mm = pitch_mm; p.first_row = t.seam.first_row; p.last_row = t.seam.last_row;
    p.dx.assign(t.rows.size(), std::nan(""));
    for (unsigned y = 0; y < t.rows.size(); ++y) if (t.rows[y].covered) {
        p.dx[y] = t.rows[y].dx_px;
        if (std::abs(t.rows[y].dx_px) > std::abs(p.peak_px)) { p.peak_px = t.rows[y].dx_px; p.peak_row = y; }
    }
    return p;
}
unsigned CountAboveHalf(const Profile& p) {
    unsigned n = 0;
    for (const double v : p.dx) if (std::isfinite(v) && std::abs(v) >= 0.5*std::abs(p.peak_px)) ++n;
    return n;
}

void Folds(const Context& c) {
    const double pitch = ps::SceneModel(c.standard, c.seam, ps::SceneSpec{}).frame.pitch_mm;
    const double mid_row = (c.seam.first_row+c.seam.last_row)/2.0;
    const double amplitude = 1.5, half_width = 8.0;
    // FWHM of (1 - t^2)^2 is 2 * 0.5412 half widths.
    const double fwhm_mm = 2*std::sqrt(1-std::sqrt(0.5))*half_width;
    const auto truth_of = [&](const ps::SceneSpec& spec) {
        return ps::MakeTruth(ps::SceneModel(c.standard, c.seam, spec), {false});
    };
    // Horizontal fold: a bump of the parallax in dx, centred on the row where the fold crosses the seam.
    {
        const auto truth = truth_of(c.Spec("fold_horizontal"));
        const auto p = ProfileOf(truth, pitch);
        const double k = truth.ParallaxPerMm(amplitude/2);
        CheckNear(p.peak_row, mid_row, 2.0, "horizontal fold: the peak is at the row of the fold");
        CheckNear(-p.peak_px, k*amplitude, 0.05*k*amplitude, "horizontal fold: the peak is k times the height of the fold");
        CheckNear(CountAboveHalf(p), fwhm_mm/pitch, 2.5, "horizontal fold: the width of the step in rows");
        double outside = 0;
        for (unsigned y = p.first_row; y <= p.last_row; ++y) if (std::abs(y-mid_row)*pitch > half_width+2*pitch && std::isfinite(p.dx[y])) outside = std::max(outside, std::abs(p.dx[y]));
        CheckNear(outside, 0, 1e-3, "horizontal fold: no parallax away from the fold");
    }
    // Diagonal fold: still centred on the crossing, wider along the seam by 1 / cos(angle).
    {
        const auto truth = truth_of(c.Spec("fold_diagonal"));
        const auto p = ProfileOf(truth, pitch);
        const double k = truth.ParallaxPerMm(amplitude/2);
        CheckNear(p.peak_row, mid_row, 2.0, "diagonal fold: the peak is at the crossing");
        CheckNear(-p.peak_px, k*amplitude, 0.05*k*amplitude, "diagonal fold: the peak is k times the height of the fold");
        CheckNear(CountAboveHalf(p), fwhm_mm/std::cos(35*kPi/180)/pitch, 3.5, "diagonal fold: the width along the seam is 1/cos(angle) wider (angle from horizontal)");
    }
    // Vertical folds at several distances from the seam column (0, 5, g_x / 2, 2 g_x): the parallax at the seam column does not depend on the row
    // and follows the profile of the fold at that distance.
    for (const double distance : {0.0, 5.0, ps::kAssumedHoleHalfWidthMm/2, 2*ps::kAssumedHoleHalfWidthMm}) {
        const auto spec = ps::FoldScene(c.seam, 90.0, distance, amplitude, half_width);
        const ps::SceneModel model(c.standard, c.seam, spec);
        const auto truth = ps::MakeTruth(model, {false});
        const auto p = ProfileOf(truth, pitch);
        double lowest = 1e9, highest = -1e9;
        for (unsigned y = p.first_row; y <= p.last_row; ++y) if (std::isfinite(p.dx[y])) { lowest = std::min(lowest, p.dx[y]); highest = std::max(highest, p.dx[y]); }
        // The expected value uses the paper point that A shows at the seam column (it is shifted from the column by the parallax of A itself).
        const auto d = ps::DisplacementAt(model, c.seam.column, mid_row);
        const double t = (d.surface_mm.x-(c.seam.x_mm+distance))/half_width;
        const double profile = std::abs(t) < 1 ? (1-t*t)*(1-t*t) : 0.0;
        const double expected = -truth.ParallaxPerMm(amplitude*profile/2)*amplitude*profile;
        const std::string label = "vertical fold at "+std::to_string(static_cast<int>(distance))+" mm: ";
        CheckNear(highest-lowest, 0, 0.01*std::abs(expected)+1e-3, label+"the same parallax on every row");
        CheckNear(0.5*(highest+lowest), expected, 0.03*std::abs(expected)+1e-3, label+"the parallax at the seam follows the fold's profile");
        if (distance >= half_width+1) CheckNear(std::max(std::abs(lowest), std::abs(highest)), 0, 1e-3, label+"a fold outside its own width does not reach the seam");
        std::cerr << label << "dx " << 0.5*(highest+lowest) << " px (expected " << expected << ")\n";
    }
    // Step: the parallax changes by k times the step height across the ramp, centred on the step.
    {
        const auto spec = c.Spec("step_horizontal");
        const auto truth = truth_of(spec);
        const auto& step = spec.field.steps.front();
        const double k = truth.ParallaxPerMm(step.height_mm/2);
        const double before = truth.rows[c.seam.first_row+20].dx_px, after = truth.rows[c.seam.last_row-20].dx_px;
        CheckNear(before, 0, 1e-3, "step: no parallax on the level side");
        CheckNear(after, -k*step.height_mm, 0.03*k*step.height_mm, "step: k times the step height on the raised side");
        // The row where the parallax crosses its half value is the row of the step.
        unsigned crossing = 0;
        for (unsigned y = c.seam.first_row+20; y < c.seam.last_row-20; ++y) if (truth.rows[y].dx_px > 0.5*after && truth.rows[y+1].dx_px <= 0.5*after) crossing = y;
        CheckNear(crossing, mid_row, 2.0, "step: the half-way row is the row of the step");
    }
}

// ---- Catalog ------------------------------------------------------------------------------------------------------------------------------------------------
void Catalog(const Context& c) {
    std::set<std::string> names;
    std::set<int> rows;
    for (const auto& s : c.catalog) {
        Check(names.insert(s.name).second, s.name+": the name is unique");
        rows.insert(s.design_row);
        Check(!s.expectation.empty(), s.name+": the expectation is written down");
    }
    for (int row = 1; row <= 12; ++row) Check(rows.count(row) == 1, "a scene for row "+std::to_string(row)+" of the table of 11.10");
    Check(c.catalog.size() == 21, "the catalog has the 21 named scenes");
    for (const auto& name : {"zero", "flat_high", "flat_high_outside", "flat_tilted", "b_moved_baseline", "b_moved_orthogonal", "b_moved_rotation",
                             "b_moved_height", "b_moved_tilt", "tilt_with_b_rotation_scale", "book", "periodic_and_text", "wave_long", "wave_short",
                             "seam_bump", "fold_horizontal", "fold_diagonal", "fold_vertical", "step_horizontal", "wave_long_dy_error", "lines_only"})
        Check(names.count(name) == 1, std::string("scene ")+name+" exists");

    // The truth of each scene: covered seam rows, finite values, the plane, the pose.
    for (const auto& spec : c.catalog) {
        const ps::SceneModel model(c.standard, c.seam, spec);
        const auto truth = ps::MakeTruth(model, {false});
        unsigned covered = 0;
        bool finite = true;
        for (const auto& r : truth.rows) if (r.covered) { ++covered; finite = finite && std::isfinite(r.dx_px) && std::isfinite(r.dy_px) && std::isfinite(r.height_mm); }
        Check(covered > 300 && finite, spec.name+": the seam rows are covered and finite");
        Check(truth.surface.exact == spec.field.IsPlanar(), spec.name+": the plane is exact exactly for planar fields");
        Check(std::memcmp(&truth.b_pose, &spec.b_pose, sizeof truth.b_pose) == 0, spec.name+": the true pose of B is reported");
        Check(std::isfinite(truth.surface.plane.h0_mm) && std::isfinite(truth.surface.plane.slope_x) && std::isfinite(truth.surface.plane.slope_y), spec.name+": the plane is finite");
    }
    // Plane truth of the planar scenes is the spec re-centred at the seam; a tilt gives the slopes.
    {
        const auto& spec = c.Spec("flat_tilted");
        const auto truth = ps::MakeTruth(ps::SceneModel(c.standard, c.seam, spec), {false});
        CheckNear(truth.surface.plane.h0_mm, 6.0, 1e-9, "flat_tilted: h0 at the seam");
        CheckNear(truth.surface.plane.slope_x, std::tan(1.5*kPi/180), 1e-12, "flat_tilted: g_x");
        CheckNear(truth.surface.plane.slope_y, std::tan(-1.0*kPi/180), 1e-12, "flat_tilted: g_y");
        CheckNear(truth.surface.plane.x0_mm, c.seam.x_mm, 1e-12, "flat_tilted: x0 is the seam column");
    }
    // The least-squares plane of the booklet is a plane that the field does follow: its residual is below the gutter's own rise.
    {
        const auto& spec = c.Spec("book");
        const ps::SceneModel model(c.standard, c.seam, spec);
        const auto truth = ps::MakeTruth(model, {false});
        double worst = 0;
        for (unsigned y = c.seam.first_row; y <= c.seam.last_row; y += 16) for (const double fx : {-150.0, -50.0, 0.0, 50.0, 150.0}) {
            const auto d = ps::DisplacementAt(model, c.seam.column+fx, y);
            if (!d.covered) continue;
            worst = std::max(worst, std::abs(d.height_mm-truth.surface.plane.Height(d.surface_mm.x, d.surface_mm.y)));
        }
        Check(worst < spec.field.gutters.front().page_rise_mm, "book: the fitted plane stays within the gutter's rise of the field (worst "+std::to_string(worst)+" mm)");
    }
    // Per-pixel height map: the closed form for a plane. h = (a + g.X) / (1 + g.(X - C) / H) for the paper A sees at X.
    for (const char* name : {"zero", "flat_tilted"}) {
        const auto& spec = c.Spec(name);
        const ps::SceneModel model(c.standard, c.seam, spec);
        const auto truth = ps::MakeTruth(model, {true});
        Check(truth.height_map_mm.size() == static_cast<std::size_t>(truth.width)*truth.height, std::string(name)+": a height for every output px");
        const auto& p = spec.field.plane;
        const double intercept = p.h0_mm-p.slope_x*p.x0_mm-p.slope_y*p.y0_mm;
        double worst = 0;
        for (unsigned y = 0; y < truth.height; y += 7) for (unsigned x = 0; x < truth.width; x += 7) {
            const Point t = model.TableOf(x, y);
            const auto& a = model.geometry_a;
            const double expected = (intercept+p.slope_x*t.x+p.slope_y*t.y)/(1+(p.slope_x*(t.x-a.center_mm.x)+p.slope_y*(t.y-a.center_mm.y))/a.height_mm);
            worst = std::max(worst, std::abs(truth.height_map_mm[static_cast<std::size_t>(y)*truth.width+x]-expected));
        }
        CheckNear(worst, 0, 1e-9, std::string(name)+": the height map equals the closed form for a plane (mm)");
    }
}

// ---- Determinism -------------------------------------------------------------------------------------------------------------------------------------------
void Determinism(const Context& c) {
    const auto& spec = c.Spec("wave_long");
    const auto one = ps::MakeScene(c.standard, c.seam, spec);
    const auto two = ps::MakeScene(c.standard, c.seam, spec);
    Check(SameImage(one.image_a, two.image_a) && SameImage(one.image_b, two.image_b), "the same scene gives the same bytes");
    Check(SameBytes(one.truth.height_map_mm, two.truth.height_map_mm), "the same scene gives the same height map");
    // The number of threads does not change the bytes.
    {
        const ps::SceneModel model(c.standard, c.seam, spec);
        Check(SameImage(ps::RenderRaw(model, ps::Camera::a, 1), one.image_a), "one thread and four threads give the same bytes");
    }
    bool same_rows = one.truth.rows.size() == two.truth.rows.size();
    for (std::size_t i = 0; same_rows && i < one.truth.rows.size(); ++i) {
        const auto &a = one.truth.rows[i], &b = two.truth.rows[i];
        same_rows = a.covered == b.covered && std::memcmp(&a.height_mm, &b.height_mm, sizeof(double)) == 0 &&
            std::memcmp(&a.dx_px, &b.dx_px, sizeof(double)) == 0 && std::memcmp(&a.dy_px, &b.dy_px, sizeof(double)) == 0;
    }
    Check(same_rows, "the same scene gives the same truth bytes");
    // The catalog is built from fixed seeds: building it again gives the same fields.
    const auto again = ps::SceneCatalog(c.seam);
    bool same_fields = again.size() == c.catalog.size();
    for (std::size_t i = 0; same_fields && i < again.size(); ++i)
        for (const Point p : {Point{300, 120}, Point{410, 200}, Point{470, 310}}) {
            const double a = again[i].field.Height(p.x, p.y), b = c.catalog[i].field.Height(p.x, p.y);
            same_fields = same_fields && std::memcmp(&a, &b, sizeof a) == 0 && again[i].name == c.catalog[i].name;
        }
    Check(same_fields, "the catalog is the same on every build");
    auto other = spec;
    other.pattern.seed = 8;
    const auto different = ps::MakeScene(c.standard, c.seam, other, {true, {false}});
    Check(!SameImage(one.image_a, different.image_a), "another seed gives another pattern");
    Check(!SameImage(one.image_a, one.image_b), "A and B are not the same image");
    // Each pattern kind renders and is not constant, except the plain one.
    for (const auto kind : {ps::PatternKind::two_directional, ps::PatternKind::one_directional, ps::PatternKind::periodic_grid, ps::PatternKind::plain, ps::PatternKind::table_and_text}) {
        const auto sheet = ps::MakeSheet(ps::PatternSpec{kind});
        double lo = 1e9, hi = -1e9;
        for (double y = 20; y < 340; y += 7.3) for (double x = 20; x < 760; x += 9.1) { lo = std::min(lo, sheet(x, y)); hi = std::max(hi, sheet(x, y)); }
        if (kind == ps::PatternKind::plain) Check(lo == hi, "plain pattern is constant");
        else Check(hi-lo > 40, "a textured pattern varies (kind "+std::to_string(static_cast<int>(kind))+")");
    }
    // One-directional lines do not vary along the lines (normal 90 deg = horizontal lines: constant along x).
    {
        const auto sheet = ps::MakeSheet(ps::PatternSpec{ps::PatternKind::one_directional});
        double worst = 0;
        for (double y = 20; y < 340; y += 13.7) for (double x = 30; x < 700; x += 53.3) worst = std::max(worst, std::abs(sheet(x, y)-sheet(x+97.0, y)));
        CheckNear(worst, 0, 1e-9, "one-directional pattern is constant along its lines");
    }
}

// ---- Images against the projective model, and the shift that the images show ---------------------------------------------------------------------------------------
// Mean absolute difference (DN) between an image and the pattern at paper points, using the forward projection (the table point of the paper
// point's ray, then the document-to-image matrix and the lens of the product's TryProjectDocument). `height_factor` scales the height the
// paper is assumed to have: 1 is the truth, -1 and 2 are the faults.
double MeanPatternError(const ps::SceneModel& m, ps::Camera which, const render::BgrImage& image, double height_factor, unsigned& used) {
    const bool is_a = which == ps::Camera::a;
    const auto& camera = is_a ? m.rig.camera_a : m.camera_b_true;
    const auto& geometry = is_a ? m.geometry_a : m.geometry_b_true;
    double total = 0;
    used = 0;
    for (double y = 60; y <= 300; y += 13) for (double x = 280; x <= 500; x += 11) {
        const double h = height_factor*m.spec.field.Height(x, y);
        const Point t{geometry.center_mm.x+(x-geometry.center_mm.x)*geometry.height_mm/(geometry.height_mm-h),
                      geometry.center_mm.y+(y-geometry.center_mm.y)*geometry.height_mm/(geometry.height_mm-h)};
        Point raw{};
        double value{};
        if (!render::TryProjectDocument(camera, t, raw) || !Bilinear(image, raw.x, raw.y, value)) continue;
        total += std::abs(value-m.sheet(x, y));
        ++used;
    }
    return used ? total/used : 1e9;
}

void ImagesFollowTheModel(const Context& c) {
    for (const char* name : {"flat_high_outside", "book", "b_moved_baseline"}) {
        const auto& spec = c.Spec(name);
        const ps::SceneModel model(c.standard, c.seam, spec);
        const auto scene = ps::MakeScene(c.standard, c.seam, spec, {true, {false}});
        for (const auto which : {ps::Camera::a, ps::Camera::b}) {
            const auto& image = which == ps::Camera::a ? scene.image_a : scene.image_b;
            unsigned used = 0, used_wrong = 0;
            const double error = MeanPatternError(model, which, image, 1.0, used);
            const double flipped = MeanPatternError(model, which, image, -1.0, used_wrong);
            const double doubled = MeanPatternError(model, which, image, 2.0, used_wrong);
            const std::string label = std::string(name)+(which == ps::Camera::a ? " A" : " B");
            Check(used > 200, label+": points used");
            CheckNear(error, 0, 4.0, label+": the image shows the pattern at the paper point on each ray (mean DN error)");
            if (std::string(name) != "b_moved_baseline") {   // a level paper has no height to get wrong
                Check(flipped > 4*error+8, label+": a flipped height would be caught (error "+std::to_string(flipped)+" DN vs "+std::to_string(error)+")");
                Check(doubled > 4*error+8, label+": a doubled height would be caught (error "+std::to_string(doubled)+" DN)");
            }
            std::cerr << label << ": mean pattern error " << error << " DN (flipped height " << flipped << ", doubled " << doubled << ")\n";
        }
    }
}

struct Shift {double x{}, y{};};
// The shift (B minus A, output px) that the two images show around the seam column and the given row: A and B are placed with the calibrated
// mappings and the integer shift with the smallest squared difference is refined by a parabola. Only the images are used.
Shift ShiftFromImages(const ps::Rig& rig, const render::BgrImage& image_a, const render::BgrImage& image_b, unsigned column, unsigned row) {
    const auto map_a = render::DocumentOutputToCamera(rig.raster, rig.camera_a);
    const auto map_b = render::DocumentOutputToCamera(rig.raster, rig.camera_b);
    constexpr int kHalfX = 48, kHalfY = 40, kRangeX = 40, kRangeY = 8;
    const int width = 2*(kHalfX+kRangeX)+1, height = 2*(kHalfY+kRangeY)+1;
    const int x0 = static_cast<int>(column)-kHalfX-kRangeX, y0 = static_cast<int>(row)-kHalfY-kRangeY;
    std::vector<double> a(static_cast<std::size_t>(width)*height, std::nan("")), b(a.size(), std::nan(""));
    for (int j = 0; j < height; ++j) for (int i = 0; i < width; ++i) {
        Point ra{}, rb{};
        double v{};
        if (map_a({double(x0+i), double(y0+j)}, ra) && Bilinear(image_a, ra.x, ra.y, v)) a[static_cast<std::size_t>(j)*width+i] = v;
        if (map_b({double(x0+i), double(y0+j)}, rb) && Bilinear(image_b, rb.x, rb.y, v)) b[static_cast<std::size_t>(j)*width+i] = v;
    }
    const auto ssd = [&](int sx, int sy) {
        double s = 0;
        for (int j = kRangeY; j < height-kRangeY; ++j) for (int i = kRangeX; i < width-kRangeX; ++i) {
            const double p = a[static_cast<std::size_t>(j)*width+i], q = b[static_cast<std::size_t>(j+sy)*width+i+sx];
            if (!std::isfinite(p) || !std::isfinite(q)) return 1e30;
            s += (p-q)*(p-q);
        }
        return s;
    };
    double best = 1e300;
    int best_x = 0, best_y = 0;
    for (int sy = -kRangeY; sy <= kRangeY; ++sy) for (int sx = -kRangeX; sx <= kRangeX; ++sx) {
        const double s = ssd(sx, sy);
        if (s < best) { best = s; best_x = sx; best_y = sy; }
    }
    const auto refine = [](double lo, double mid, double hi) {
        const double denominator = lo-2*mid+hi;
        return std::abs(denominator) > 1e-12 && mid < 1e29 && lo < 1e29 && hi < 1e29 ? 0.5*(lo-hi)/denominator : 0.0;
    };
    Shift s;
    s.x = best_x+refine(ssd(best_x-1, best_y), best, ssd(best_x+1, best_y));
    s.y = best_y+refine(ssd(best_x, best_y-1), best, ssd(best_x, best_y+1));
    return s;
}

void ShiftOfImages(const Context& c) {
    const unsigned row = (c.seam.first_row+c.seam.last_row)/2;
    for (const char* name : {"flat_high_outside", "b_moved_baseline", "b_moved_orthogonal"}) {
        const auto scene = ps::MakeScene(c.standard, c.seam, c.Spec(name));
        const auto shift = ShiftFromImages(c.standard, scene.image_a, scene.image_b, c.seam.column, row);
        const auto& truth = scene.truth.rows[row];
        CheckNear(shift.x, truth.dx_px, 0.4, std::string(name)+": the images show the truth dx (px)");
        CheckNear(shift.y, truth.dy_px, 0.4, std::string(name)+": the images show the truth dy (px)");
        std::cerr << name << ": shift from images (" << shift.x << ", " << shift.y << ") px; truth (" << truth.dx_px << ", " << truth.dy_px << ") px\n";
        // The same comparison against a faulty truth must fail: opposite sign, and k doubled.
        const bool sign_caught = std::hypot(shift.x+truth.dx_px, shift.y+truth.dy_px) > 1.0;
        const bool double_caught = std::hypot(shift.x-2*truth.dx_px, shift.y-2*truth.dy_px) > 1.0;
        Check(sign_caught, std::string(name)+": a flipped sign of the truth is caught by the images");
        Check(double_caught, std::string(name)+": a doubled truth is caught by the images");
    }
}
} // namespace

int main() {
    const auto started = std::chrono::steady_clock::now();
    const Context context;
    std::cerr << "seam column " << context.seam.column << " (x " << context.seam.x_mm << " mm), covered rows " << context.seam.first_row << ".." << context.seam.last_row << '\n';
    const auto section = [&](const char* name, const std::function<void(const Context&)>& run) {
        const auto begin = std::chrono::steady_clock::now();
        run(context);
        std::cerr << "[" << name << " " << std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count() << " s]\n";
    };
    section("geometry", CameraGeometryTests);
    section("solver", Solver);
    section("zero field", ZeroField);
    section("law on every scene", LawOnEveryScene);
    section("plane projection", PlaneProjection);
    section("parallax law", ParallaxLaw);
    section("checks bite", ChecksBite);
    section("folds", Folds);
    section("catalog", Catalog);
    section("determinism", Determinism);
    section("images follow the model", ImagesFollowTheModel);
    section("shift of images", ShiftOfImages);
    std::cerr << "parallax scene tests took " << std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() << " s\n";
    std::cout << checks-failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
