#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/seam_gate.hpp"
#include "parallax_scene.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <numbers>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace cr = a0::m2::experimental::content_registration;
namespace m2x = a0::m2::experimental;
namespace gate = a0::m2::seam_gate;
namespace render = a0::m2::render;
namespace ps = parallax_scene;
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
bool SameBits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }
constexpr double kPi = std::numbers::pi;

struct Lcg {
    std::uint64_t state;
    double Uniform() { state = state*6364136223846793005ull+1442695040888963407ull; return static_cast<double>(state>>11)/9007199254740992.0; }
};

// ---- A synthetic rig: two cameras over a flat sheet, the lens and the document matrix of a profile ------------------
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

render::CameraProjection MakeCamera(double x0, double y0, double depth, double roll, double tilt_x, double tilt_y) {
    const double d = kPi/180;
    const auto rotation = Mul(RotationZ(roll*d), Mul(RotationX(tilt_x*d), RotationY(tilt_y*d)));
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

// The sheet point that a camera shows at a stored pixel. Independent of the code under test.
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

using Sheet = std::function<double(double, double)>;
// A sheet without a lattice or a repeat: a sum of plane waves, in DN.
Sheet WaveSheet(std::uint64_t seed) {
    struct Wave {double kx, ky, phase, amplitude;};
    std::vector<Wave> waves;
    Lcg r{seed};
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
// A pattern that repeats with the same period in both directions and holds nothing else.
Sheet PeriodicSheet(double period_mm) {
    return [period_mm](double x, double y) { return 150+45*std::cos(2*kPi*x/period_mm)+45*std::cos(2*kPi*y/period_mm); };
}
Sheet WhiteSheet() { return [](double, double) { return 255.0; }; }

render::BgrImage RenderRaw(const render::CameraProjection& camera, const Sheet& sheet) {
    render::BgrImage image{kWidth, kHeight, std::vector<std::uint8_t>(static_cast<std::size_t>(kWidth)*kHeight*3)};
    for (unsigned v = 0; v < kHeight; ++v) for (unsigned u = 0; u < kWidth; ++u) {
        Point p{};
        std::uint8_t value = 0;
        if (SheetPointOf(camera, u, v, p)) value = static_cast<std::uint8_t>(std::clamp(std::lround(sheet(p.x, p.y)), 0L, 255L));
        const std::size_t at = (static_cast<std::size_t>(v)*kWidth+u)*3;
        image.bgr[at] = image.bgr[at+1] = image.bgr[at+2] = value;
    }
    return image;
}

// B as a different exposure with sensor noise: gain, offset and a roughly Gaussian noise (sum of four uniforms), deterministic.
render::BgrImage Degrade(const render::BgrImage& image, double gain, double offset, double noise_sigma, std::uint64_t seed) {
    render::BgrImage out = image;
    Lcg r{seed};
    for (auto& v : out.bgr) {
        const double noise = (r.Uniform()+r.Uniform()+r.Uniform()+r.Uniform()-2.0)*std::sqrt(3.0)*noise_sigma;
        v = static_cast<std::uint8_t>(std::clamp(std::lround(gain*v+offset+noise), 0L, 255L));
    }
    return out;
}

render::OutputRaster MakeRaster() {
    const std::int64_t left = 10000, top = 10000, right = 780000, bottom = 350000;
    const std::uint32_t dpi = 48;
    const auto pixels = [dpi](std::int64_t extent) {return static_cast<std::uint32_t>((extent*dpi+25399)/25400);};
    return render::OutputRaster(render::DocumentRegionUm(left, top, right, bottom), dpi, pixels(right-left), pixels(bottom-top));
}

struct Rig {
    render::OutputRaster raster = MakeRaster();
    render::CameraProjection camera_a = MakeCamera(270, 180, 700, 0.3, -0.15, 0.1);
    render::CameraProjection camera_b = MakeCamera(520, 180, 700, -0.4, 0.2, -0.1);
    render::DocumentRenderParameters parameters{raster, camera_a, camera_b, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bilinear};
};

// ---- The pose mapping of the fixed composite ---------------------------------------------------------------------------
void PoseMapping() {
    const Rig rig;
    const cr::PoseModel model(rig.camera_b);
    const auto unmoved = render::DocumentOutputToCamera(rig.raster, rig.camera_b);
    const auto zero = m2x::PoseCorrectedDocumentOutputToCamera(rig.raster, rig.camera_b, model, {});
    const auto input_zero = m2x::PoseCorrectedDocumentMappedFixedSeamInput(rig.parameters, {});
    const auto input_plain = m2x::DocumentMappedFixedSeamInput(rig.parameters);
    bool same = true;
    for (double y = 0; y < rig.raster.height_pixels; y += 37.25) for (double x = 0; x < rig.raster.width_pixels; x += 41.5) {
        Point a{}, b{}, c{}, d{}, e{};
        const bool ha = unmoved({x, y}, a), hb = zero({x, y}, b), hc = input_zero.camera_b({x, y}, c), hd = input_plain.camera_b({x, y}, d);
        const bool he = input_zero.camera_a({x, y}, e);
        Point plain_a{};
        input_plain.camera_a({x, y}, plain_a);
        same = same && ha == hb && ha == hc && ha == hd && he && SameBits(a.x, b.x) && SameBits(a.y, b.y) && SameBits(a.x, c.x) && SameBits(a.y, c.y) &&
            SameBits(a.x, d.x) && SameBits(a.y, d.y) && SameBits(e.x, plain_a.x) && SameBits(e.y, plain_a.y);
    }
    Check(same, "the zero pose gives the calibrated mapping bit for bit, and A is untouched");
    Check(input_zero.width == input_plain.width && input_zero.height == input_plain.height, "the zero pose keeps the raster");

    // A moved pose: the mapping puts a feature of the moved camera back at its own document point.
    const cr::PoseParameters pose{0.4, -0.3, 0.9, 4.0, -3.0, 0.6};
    auto moved = rig.camera_b;
    moved.document_to_image = model.CameraHomography(pose);
    const auto corrected = m2x::PoseCorrectedDocumentOutputToCamera(rig.raster, rig.camera_b, model, pose);
    const auto frame = cr::FrameOfRaster(rig.raster);
    double worst = 0;
    unsigned used = 0;
    for (const double u : {200.0, 450.0, 600.0, 900.0, 1100.0}) for (const double v : {150.0, 400.0, 650.0}) {
        Point sheet{};
        if (!SheetPointOf(moved, u, v, sheet)) continue;
        const double x = (sheet.x-frame.left_mm)/frame.pitch_mm-0.5, y = (sheet.y-frame.top_mm)/frame.pitch_mm-0.5;
        Point raw{};
        if (!corrected({x, y}, raw)) continue;
        worst = std::max(worst, std::hypot(raw.x-u, raw.y-v));
        ++used;
    }
    Check(used >= 10, "pose mapping check points exist");
    CheckNear(worst, 0, 1e-3, "the corrected mapping follows the moved camera (raw px)");
    Point plain_raw{}, moved_raw{};
    unmoved({700, 300}, plain_raw); corrected({700, 300}, moved_raw);
    Check(std::hypot(plain_raw.x-moved_raw.x, plain_raw.y-moved_raw.y) > 1.0, "a moved pose changes the mapping");
    // The mapping of the pose and of its homography are the same function.
    const auto by_homography = m2x::HomographyCorrectedDocumentOutputToCamera(rig.raster, rig.camera_b, model.Homography(pose));
    Point h_raw{};
    by_homography({700, 300}, h_raw);
    Check(SameBits(h_raw.x, moved_raw.x) && SameBits(h_raw.y, moved_raw.y), "pose and homography entrances agree");
    bool refused = false;
    try { (void)m2x::PoseCorrectedDocumentOutputToCamera(rig.raster, rig.camera_b, model, {std::nan(""), 0, 0, 0, 0, 0}); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "a nonfinite pose is refused");
    // The scale path is unchanged: k = 1 is still the unscaled mapping.
    const auto scaled = m2x::ScaledDocumentMappedFixedSeamInput(rig.parameters, 1.0);
    Point s_raw{}, p_raw{};
    scaled.camera_b({700, 300}, s_raw); unmoved({700, 300}, p_raw);
    Check(SameBits(s_raw.x, p_raw.x) && SameBits(s_raw.y, p_raw.y), "k = 1 is still the calibrated mapping");
}

// ---- The estimator on rendered pairs -----------------------------------------------------------------------------------
unsigned Threads() { return std::max(2u, std::min(8u, std::thread::hardware_concurrency())); }

cr::RegistrationSettings MakeSettings(unsigned seam_column) {
    cr::RegistrationSettings s;
    s.geometry = {seam_column, 8, 4, 40, 400};
    s.envelope = {10.0, 2.0, 1.0, 1.0};
    s.limits.brown_lowe_alpha = 8; s.limits.brown_lowe_beta = 0.3; s.limits.minimum_unique_blocks = 2;
    s.limits.maximum_prediction_sigma_px = 1.0;
    s.limits.maximum_residual_rms_px = 0.5; s.limits.maximum_residual_p95_px = 1.0; s.limits.maximum_bin_bias_px = 0.5;
    s.limits.convergence_px = 0.02; s.limits.start_agreement_px = 0.3;
    s.design = cr::kRegistrationDesignDraft;
    // The test raster is 48 dpi (about a quarter of a real one), so lengths in pixels are smaller.
    s.design.coarse_block_rows = 256;
    s.design.coarse_maximum_radius_px = 256;
    s.design.fine_point_spacing_px = 32;
    s.design.fine_patch_px = 48;
    s.design.fine_search_radius_px = 16;
    s.design.second_start_search_radius_px = 16;   // no widening of the second start beyond the first radius, as before the first radius was cut
    s.design.fine_maximum_search_radius_px = 64;
    s.design.fine_global_search_radius_px = 36;
    return s;
}

unsigned SeamColumnOf(const Rig& rig, const render::BgrImage& a, const render::BgrImage& b) {
    gate::SeamGateGeometry g;
    g.width = rig.raster.width_pixels; g.height = rig.raster.height_pixels;
    g.camera_a = render::DocumentOutputToCamera(rig.raster, rig.camera_a);
    g.camera_b = render::DocumentOutputToCamera(rig.raster, rig.camera_b);
    const auto band = gate::OverlapBand(g, a.width, a.height, b.width, b.height, 0);
    std::uint32_t first = g.width, last = 0;
    for (const auto& row : band) if (row.begin < row.end) { first = std::min(first, row.begin); last = std::max(last, row.end); }
    return (first+last)/2;
}

// Largest displacement left at the seam column over every covered row when B is placed by `estimated` and really sits at `truth`.
double SeamResidualMaximum(const Rig& rig, const cr::PoseModel& model, const cr::PoseParameters& estimated, const cr::PoseParameters& truth,
    unsigned seam_column, unsigned& rows) {
    const auto frame = cr::FrameOfRaster(rig.raster);
    const auto map_a = render::DocumentOutputToCamera(rig.raster, rig.camera_a);
    const auto map_b = render::DocumentOutputToCamera(rig.raster, rig.camera_b);
    double worst = 0;
    rows = 0;
    for (unsigned y = 0; y < rig.raster.height_pixels; ++y) {
        Point ra{}, rb{};
        if (!map_a({double(seam_column), double(y)}, ra) || !map_b({double(seam_column), double(y)}, rb)) continue;
        if (!(ra.x >= 0 && ra.y >= 0 && ra.x < kWidth && ra.y < kHeight && rb.x >= 0 && rb.y >= 0 && rb.x < kWidth && rb.y < kHeight)) continue;
        const Point d = cr::ResidualDisplacement(model, frame, estimated, truth, seam_column, y);
        worst = std::max(worst, std::hypot(d.x, d.y));
        ++rows;
    }
    return worst;
}

std::string Describe(const cr::RegistrationResult& r) {
    std::string s = std::string(cr::RegistrationStateCode(r.state));
    for (const auto& reason : r.reasons) s += " [" + reason + "]";
    return s;
}

// FNV-1a over the bytes of the six-degree result (pose, covariance, checks, observations). Printed so that two builds
// of the estimator can be compared bit for bit on the same inputs (the six-degree path must not change).
std::uint64_t Fingerprint(const cr::RegistrationResult& r) {
    std::uint64_t h = 1469598103934665603ull;
    const auto bytes = [&](const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    };
    const auto number = [&](double v) { bytes(&v, sizeof v); };
    const auto state = static_cast<int>(r.state);
    bytes(&state, sizeof state);
    for (const auto& reason : r.reasons) bytes(reason.data(), reason.size());
    bytes(&r.pose, sizeof r.pose); bytes(&r.pose_sigma, sizeof r.pose_sigma); bytes(r.covariance.data(), sizeof r.covariance);
    const auto& d = r.diagnostics;
    for (const auto& b : d.coarse_blocks) { number(b.displacement.x); number(b.displacement.y); number(b.correlation); }
    if (d.coarse_pose) bytes(&*d.coarse_pose, sizeof *d.coarse_pose);
    bytes(&d.points, sizeof d.points); bytes(&d.inliers, sizeof d.inliers); bytes(&d.outer_iterations, sizeof d.outer_iterations);
    number(d.robust_scale_px); number(d.residual.rms); number(d.residual.p95); number(d.residual.maximum); number(d.maximum_bin_bias_px);
    number(d.maximum_prediction_sigma_px); number(d.last_change_px); number(d.second_start_difference_px);
    for (const double v : d.change_history_px) number(v);
    for (const auto& o : d.observations) { number(o.x); number(o.y); number(o.displacement.x); number(o.displacement.y); number(o.sigma_px); number(o.residual_px); number(o.weight); }
    return h;
}
void PrintFingerprint(const std::string& name, const cr::RegistrationResult& r) {
    std::cerr << "fingerprint " << name << ' ' << std::hex << Fingerprint(r) << std::dec << '\n';
}

void Estimate() {
    const auto started = std::chrono::steady_clock::now();
    const Rig rig;
    const cr::PoseModel model(rig.camera_b);
    const auto wave = WaveSheet(7);
    const auto image_a = RenderRaw(rig.camera_a, wave);
    const auto render_b = [&](const Sheet& sheet, const cr::PoseParameters& truth) {
        auto moved = rig.camera_b;
        moved.document_to_image = model.CameraHomography(truth);
        return RenderRaw(moved, sheet);
    };
    const unsigned seam = SeamColumnOf(rig, image_a, render_b(wave, {}));
    const auto settings = MakeSettings(seam);

    struct Case { std::string name; cr::PoseParameters truth; };
    const Case cases[]{
        {"no change", {0, 0, 0, 0, 0, 0}},
        {"translation and tilt", {0.3, -0.2, 0.0, 3.0, -2.0, 0.0}},
        {"mixed", {0.3, -0.2, 0.8, 3.0, -2.0, 0.5}},
        {"large mixed", {-0.7, 0.6, -1.6, -7.0, 5.0, -0.8}},
        {"roll", {0, 0, 1.8, 0, 0, 0}},
        {"height", {0, 0, 0, 0, 0, 0.9}},
    };
    for (const auto& c : cases) {
        const auto image_b = render_b(wave, c.truth);
        const auto begin = std::chrono::steady_clock::now();
        const auto r = cr::EstimateRegistration(image_a, image_b, rig.parameters, settings, Threads());
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        Check(r.state == cr::RegistrationState::verified, c.name + ": verified (" + Describe(r) + ")");
        PrintFingerprint(c.name, r);
        if (!r.has_estimate) continue;
        unsigned rows = 0;
        const double residual = SeamResidualMaximum(rig, model, r.pose, c.truth, seam, rows);
        std::cerr << c.name << ": " << Describe(r) << " seam residual max " << residual << " px over " << rows << " rows; pose rx " << r.pose.rx_deg
                  << " ry " << r.pose.ry_deg << " rz " << r.pose.rz_deg << " tx " << r.pose.tx_mm << " ty " << r.pose.ty_mm << " tz " << r.pose.tz_percent
                  << "; blocks " << r.diagnostics.unique_blocks << " points " << r.diagnostics.points << " inliers " << r.diagnostics.inliers
                  << " outer " << r.diagnostics.outer_iterations << " rms " << r.diagnostics.residual.rms << " p95 " << r.diagnostics.residual.p95
                  << " sigma_pred " << r.diagnostics.maximum_prediction_sigma_px << " second " << cr::SecondStartCode(r.diagnostics.second_start)
                  << " (" << r.diagnostics.second_start_difference_px << ") " << seconds << " s\n";
        Check(rows > 300, c.name + ": the seam column is covered");
        CheckNear(residual, 0, 0.1, c.name + ": residual at the seam column after the estimate (px)");
        Check(r.exceeded.empty(), c.name + ": inside the envelope");
        Check(r.diagnostics.coarse_pose.has_value(), c.name + ": the coarse stage gave a start");
    }

    // A different exposure and sensor noise on B (the registration works on edge energy, not on the brightness).
    {
        const cr::PoseParameters truth{0.3, -0.2, 0.8, 3.0, -2.0, 0.5};
        const auto image_b = Degrade(render_b(wave, truth), 1.15, 6.0, 2.5, 99);
        const auto r = cr::EstimateRegistration(image_a, image_b, rig.parameters, settings, Threads());
        Check(r.state == cr::RegistrationState::verified, "noise and gain: verified (" + Describe(r) + ")");
        PrintFingerprint("noise and gain", r);
        if (r.has_estimate) {
            unsigned rows = 0;
            const double residual = SeamResidualMaximum(rig, model, r.pose, truth, seam, rows);
            std::cerr << "noise and gain: " << Describe(r) << " seam residual max " << residual << " px; rms " << r.diagnostics.residual.rms
                      << " inliers " << r.diagnostics.inliers << "/" << r.diagnostics.points << '\n';
            CheckNear(residual, 0, 0.15, "noise and gain: residual at the seam column after the estimate (px)");
        }
    }

    // Determinism: the same inputs give the same bytes for any thread count.
    {
        const cr::PoseParameters truth{0.3, -0.2, 0.8, 3.0, -2.0, 0.5};
        const auto image_b = render_b(wave, truth);
        const auto one = cr::EstimateRegistration(image_a, image_b, rig.parameters, settings, 1);
        const auto four = cr::EstimateRegistration(image_a, image_b, rig.parameters, settings, 4);
        const auto again = cr::EstimateRegistration(image_a, image_b, rig.parameters, settings, 4);
        const auto same = [](const cr::RegistrationResult& a, const cr::RegistrationResult& b) {
            return a.state == b.state && a.reasons == b.reasons && std::memcmp(&a.pose, &b.pose, sizeof a.pose) == 0 &&
                std::memcmp(a.covariance.data(), b.covariance.data(), sizeof a.covariance) == 0 && a.diagnostics.points == b.diagnostics.points &&
                a.diagnostics.inliers == b.diagnostics.inliers && SameBits(a.diagnostics.residual.rms, b.diagnostics.residual.rms) &&
                SameBits(a.diagnostics.maximum_prediction_sigma_px, b.diagnostics.maximum_prediction_sigma_px);
        };
        Check(one.has_estimate, "determinism: an estimate exists");
        PrintFingerprint("determinism", one);
        Check(same(one, four), "one thread and four threads give the same bytes");
        Check(same(four, again), "two runs give the same bytes");
    }

    // A periodic pattern alone, and a white sheet: nothing to register on.
    {
        const auto periodic = PeriodicSheet(14.0);
        const auto a = RenderRaw(rig.camera_a, periodic);
        const auto b = render_b(periodic, {0.2, 0.0, 0.0, 2.0, 0.0, 0.0});
        const auto r = cr::EstimateRegistration(a, b, rig.parameters, settings, Threads());
        Check(r.state == cr::RegistrationState::undetermined, "a periodic pattern alone is undetermined (" + Describe(r) + ")");
        Check(!r.reasons.empty(), "a periodic pattern alone has a reason");
        std::cerr << "periodic: " << Describe(r) << " unique " << r.diagnostics.unique_blocks << '\n';
        PrintFingerprint("periodic", r);
        const auto white_a = RenderRaw(rig.camera_a, WhiteSheet()), white_b = RenderRaw(rig.camera_b, WhiteSheet());
        const auto w = cr::EstimateRegistration(white_a, white_b, rig.parameters, settings, Threads());
        Check(w.state == cr::RegistrationState::undetermined, "a white sheet is undetermined (" + Describe(w) + ")");
        PrintFingerprint("white", w);
        Check(!w.reasons.empty() && w.reasons.front() == "coarse-no-unique-blocks", "a white sheet has no unique block");
    }

    // Outside the envelope: not clamped, and the component is named.
    {
        struct Outside { std::string name; cr::PoseParameters truth; cr::EnvelopeComponent component; };
        const Outside outside[]{
            {"position 14 mm", {0, 0, 0, 14.0, 4.0, 0}, cr::EnvelopeComponent::position},
            {"height 1.8 %", {0, 0, 0, 0, 0, 1.8}, cr::EnvelopeComponent::height},
            {"roll 3.5 deg", {0, 0, 3.5, 0, 0, 0}, cr::EnvelopeComponent::rotation},
            {"tilt 2 deg", {1.6, -1.2, 0, 0, 0, 0}, cr::EnvelopeComponent::tilt},
        };
        for (const auto& o : outside) {
            const auto image_b = render_b(wave, o.truth);
            const auto r = cr::EstimateRegistration(image_a, image_b, rig.parameters, settings, Threads());
            std::cerr << o.name << ": " << Describe(r) << " pose rx " << r.pose.rx_deg << " ry " << r.pose.ry_deg << " rz " << r.pose.rz_deg << " tx "
                      << r.pose.tx_mm << " ty " << r.pose.ty_mm << " tz " << r.pose.tz_percent << '\n';
            Check(r.state == cr::RegistrationState::out_of_envelope, o.name + ": out of the envelope (" + Describe(r) + ")");
            PrintFingerprint(o.name, r);
            Check(std::find(r.exceeded.begin(), r.exceeded.end(), o.component) != r.exceeded.end(), o.name + ": the component is named");
            Check(r.components.size() == 4, o.name + ": all four components are reported");
            if (r.has_estimate) {
                // Not clamped: the estimate is the real pose, beyond the range.
                unsigned rows = 0;
                CheckNear(SeamResidualMaximum(rig, model, r.pose, o.truth, seam, rows), 0, 0.15, o.name + ": the estimate is not clamped (seam px)");
            }
        }
    }
    std::cerr << "estimator cases took " << std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() << " s\n";
}

void Settings() {
    auto settings = MakeSettings(700);
    bool refused = false;
    auto unset = settings;
    unset.envelope = {};
    try { cr::ValidateRegistrationSettings(unset); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "an unset envelope is refused");
    refused = false;
    unset = settings;
    unset.limits = {};
    try { cr::ValidateRegistrationSettings(unset); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "unset limits are refused");
    refused = false;
    unset = settings;
    unset.geometry.band_half_width_px = 10;
    try { cr::ValidateRegistrationSettings(unset); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "a band that does not reach past Omega and the guard is refused");
    bool ok = true;
    try { cr::ValidateRegistrationSettings(settings); } catch (const std::invalid_argument&) { ok = false; }
    Check(ok, "the test settings are valid");
    const cr::PoseModel model(Rig{}.camera_b);
    const auto display = cr::DisplayOf({0.3, -0.4, 1.5, 3.0, 4.0, 0.5}, model);
    CheckNear(display.position_mm, 5.0, 1e-12, "display position");
    CheckNear(display.tilt_deg, 0.5, 1e-12, "display tilt");
    CheckNear(display.rotation_deg, 1.5, 1e-12, "display rotation");
    CheckNear(display.height_mm, 0.5/100.0*model.PivotDepthMm(), 1e-9, "display height in mm");
}

// ---- The paper surface: eight components (design 11.3, the scenes of tests/parallax_scene.hpp) --------------------------
cr::RegistrationSettings SurfaceSettings(unsigned seam_column) {
    auto s = MakeSettings(seam_column);
    // Example ranges for the synthetic rig (not the owner's values).
    s.surface_envelope = cr::SurfaceEnvelope{10.0, 4.0};
    return s;
}

Point DisplacementOf(const render::Matrix3& m, const cr::PlaneFrame& f, double x, double y) {
    const double px = f.left_mm+(x+0.5)*f.pitch_mm, py = f.top_mm+(y+0.5)*f.pitch_mm;
    const double w = m[6]*px+m[7]*py+m[8];
    return {((m[0]*px+m[1]*py+m[2])/w-px)/f.pitch_mm, ((m[3]*px+m[4]*py+m[5])/w-py)/f.pitch_mm};
}

// The residual along the seam column between the scene's true parallax and the displacement the estimate places B by,
// split along the baseline e_b and across it (where the paper height makes no field, design 11.2).
struct SeamSplit { double across_rms{}, across_max{}, along_max{}, along_rms{}; unsigned rows{}; };
SeamSplit SplitAtSeam(const ps::Truth& truth, const cr::RegistrationResult& r, const cr::PlaneFrame& frame) {
    SeamSplit s;
    const Point across{-truth.e_b.y, truth.e_b.x};
    double across_sq = 0, along_sq = 0;
    for (unsigned y = 0; y < truth.rows.size(); ++y) {
        const auto& row = truth.rows[y];
        if (!row.covered) continue;
        const Point d = DisplacementOf(r.document_homography, frame, truth.seam.column, y);
        const double rx = row.dx_px-d.x, ry = row.dy_px-d.y;
        const double a = rx*across.x+ry*across.y, b = rx*truth.e_b.x+ry*truth.e_b.y;
        across_sq += a*a; along_sq += b*b;
        s.across_max = std::max(s.across_max, std::abs(a)); s.along_max = std::max(s.along_max, std::abs(b));
        ++s.rows;
    }
    if (s.rows) { s.across_rms = std::sqrt(across_sq/s.rows); s.along_rms = std::sqrt(along_sq/s.rows); }
    return s;
}

bool SameSurfaceBytes(const cr::RegistrationResult& a, const cr::RegistrationResult& b) {
    return a.state == b.state && a.reasons == b.reasons && std::memcmp(&a.pose, &b.pose, sizeof a.pose) == 0 &&
        std::memcmp(&a.surface.plane, &b.surface.plane, sizeof a.surface.plane) == 0 && a.joint_covariance == b.joint_covariance &&
        std::memcmp(a.document_homography.data(), b.document_homography.data(), sizeof a.document_homography) == 0 &&
        a.diagnostics.points == b.diagnostics.points && a.diagnostics.inliers == b.diagnostics.inliers &&
        SameBits(a.diagnostics.residual.rms, b.diagnostics.residual.rms) &&
        SameBits(a.diagnostics.information_condition_number, b.diagnostics.information_condition_number) &&
        a.diagnostics.coarse_vote.support == b.diagnostics.coarse_vote.support;
}

const ps::SceneSpec& SceneNamed(const std::vector<ps::SceneSpec>& catalog, const std::string& name) {
    for (const auto& s : catalog) if (s.name == name) return s;
    throw std::runtime_error("no scene " + name);
}

void Surface() {
    const auto started = std::chrono::steady_clock::now();
    const ps::Rig rig = ps::StandardRig();
    const ps::SeamPlacement seam = ps::PlaceSeam(rig);
    const auto catalog = ps::SceneCatalog(seam);
    const auto parameters = rig.Parameters();
    const auto frame = cr::FrameOfRaster(rig.raster);
    const auto settings = SurfaceSettings(seam.column);
    ps::SceneOptions options;
    options.truth.height_map = false;
    const double slope_of_degree = std::tan(kPi/180);

    const auto estimate = [&](const ps::Scene& scene, const cr::RegistrationSettings& s, unsigned threads) {
        const auto begin = std::chrono::steady_clock::now();
        auto r = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, s, threads);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        const auto split = SplitAtSeam(scene.truth, r, frame);
        const auto& p = r.surface.plane;
        const auto& d = r.diagnostics;
        std::cerr << scene.spec.name << ": " << Describe(r) << " surface " << cr::SurfaceStateCode(r.surface.state) << " h0 " << p.h0_mm << " gx " << p.slope_x
                  << " gy " << p.slope_y << " (truth h0 " << scene.truth.surface.plane.Height(r.surface.frame.reference_mm.x, r.surface.frame.reference_mm.y)
                  << " gx " << scene.truth.surface.plane.slope_x << " gy " << scene.truth.surface.plane.slope_y << "); pose rx " << r.pose.rx_deg << " ry "
                  << r.pose.ry_deg << " rz " << r.pose.rz_deg << " tx " << r.pose.tx_mm << " ty " << r.pose.ty_mm << " tz " << r.pose.tz_percent
                  << "; seam across rms " << split.across_rms << " max " << split.across_max << " along rms " << split.along_rms << " max " << split.along_max
                  << " over " << split.rows << " rows; cond " << d.information_condition_number << " corr " << d.maximum_surface_pose_correlation
                  << "; vote " << d.coarse_vote.support << "/" << d.coarse_vote.rival_support << (d.coarse_vote.decided ? " decided" : " open")
                  << " radius " << d.coarse_radius_x_px << "x" << d.coarse_radius_y_px << " points " << d.points << " outer " << d.outer_iterations
                  << " second " << cr::SecondStartCode(d.second_start) << "; " << seconds << " s\n";
        return r;
    };
    const auto truth_h0 = [](const ps::Scene& scene, const cr::RegistrationResult& r) {
        return scene.truth.surface.plane.Height(r.surface.frame.reference_mm.x, r.surface.frame.reference_mm.y);
    };

    // Planes inside the range, and B moved: verified, the residual across the baseline at the seam within 0.1 px rms.
    struct Case { std::string name; bool plane_known; };
    const Case cases[]{{"zero", true}, {"flat_high", true}, {"flat_tilted", true}, {"b_moved_baseline", false}, {"b_moved_orthogonal", true},
        {"b_moved_rotation", true}, {"b_moved_height", true}, {"b_moved_tilt", true}, {"tilt_with_b_rotation_scale", true}};
    for (const auto& c : cases) {
        const auto scene = ps::MakeScene(rig, seam, SceneNamed(catalog, c.name), options);
        const auto r = estimate(scene, settings, Threads());
        Check(r.state == cr::RegistrationState::verified, c.name + ": verified (" + Describe(r) + ")");
        Check(r.surface.state == cr::SurfaceState::estimated, c.name + ": the surface is estimated");
        if (!r.has_estimate) continue;
        const auto split = SplitAtSeam(scene.truth, r, frame);
        Check(split.rows > 300, c.name + ": the seam column is covered");
        CheckNear(split.across_rms, 0, 0.1, c.name + ": rms across the baseline at the seam column (px)");
        CheckNear(split.along_max, 0, 0.15, c.name + ": largest residual along the baseline at the seam column (px)");
        CheckNear(r.surface.plane.slope_x, scene.truth.surface.plane.slope_x, 0.1*slope_of_degree, c.name + ": g_x");
        CheckNear(r.surface.plane.slope_y, scene.truth.surface.plane.slope_y, 0.1*slope_of_degree, c.name + ": g_y");
        if (c.plane_known) CheckNear(r.surface.plane.h0_mm, truth_h0(scene, r), 0.3, c.name + ": h0 (mm)");
        // The B components come back as B's; the surface does not take them.
        const auto& t = scene.truth.b_pose;
        if (c.plane_known) {
            CheckNear(r.pose.rz_deg, t.rz_deg, 0.05, c.name + ": B's rotation");
            CheckNear(r.pose.tz_percent, t.tz_percent, 0.05, c.name + ": B's height");
        }
        // k of the true camera B; under the gauge, B moved along the baseline is read as h0 with the calibrated baseline.
        if (c.plane_known) CheckNear(r.surface.frame.parallax_px_per_mm, scene.truth.ParallaxPerMm(r.surface.plane.h0_mm), 0.002, c.name + ": k (px per mm)");
        Check(std::isfinite(r.diagnostics.information_condition_number) && r.diagnostics.information_condition_number >= 1, c.name + ": the condition number is reported");
        Check(r.components.size() == 4 && r.surface_components.size() == 2, c.name + ": four pose and two surface components are reported");
        Check(r.surface.estimate.state == cr::SurfaceState::estimated && SameBits(r.surface.estimate.plane.h0_mm, r.surface.plane.h0_mm) &&
            SameBits(r.surface.estimate.tilt_deg, r.surface.display.tilt_deg), c.name + ": the report's surface carries the estimate");
        // The drawing-frame values (addendum 11.24), against the scene's truth.
        Check(r.display.output_frame.has_value(), c.name + ": the output frame is filled");
        if (!r.display.output_frame) continue;
        const auto& of = *r.display.output_frame;
        const auto& t2 = scene.truth.b_pose;
        if (c.name == "b_moved_orthogonal") {
            // The plane moved by +ty in B's frame: the camera moved the other way over the drawing.
            CheckNear(of.camera_motion_mm.y, -t2.ty_mm, 0.15, c.name + ": the camera motion over the drawing (mm)");
        }
        if (c.name == "b_moved_rotation") {
            CheckNear(std::abs(of.rotation_clockwise_deg), t2.rz_deg, 0.05, c.name + ": the turn on the screen (deg)");
            // The orientation display agrees with the output frame: B's turn against A changed by the same amount.
            const auto calibrated = cr::RelativeOrientationOf(cr::OrientationOf(rig.camera_a), cr::OrientationOf(rig.camera_b));
            CheckNear(r.orientation.b_from_a.rotation_clockwise_deg-calibrated.rotation_clockwise_deg, of.rotation_clockwise_deg, 0.02,
                c.name + ": the orientation display follows the estimate (deg)");
        }
        if (c.name == "b_moved_tilt") {
            CheckNear(of.lens_tilt_deg, std::hypot(t2.rx_deg, t2.ry_deg), 0.05, c.name + ": the lens tilt (deg)");
            CheckNear(std::hypot(of.lens_direction.x, of.lens_direction.y), 1, 1e-9, c.name + ": the lens direction is a unit vector");
            // The orientation display (the optical axis through the principal point) leans the same way.
            const auto lean = cr::RelativeOrientationOf(cr::OrientationOf(rig.camera_b), r.orientation.camera_b);
            CheckNear(lean.tilt_deg, of.lens_tilt_deg, 0.02, c.name + ": the axis turned by the lens tilt (deg)");
            CheckNear(lean.tilt_direction.x*of.lens_direction.x+lean.tilt_direction.y*of.lens_direction.y, 1, 0.01,
                c.name + ": the axis leans the way the lens direction says");
        }
        if (c.name == "flat_tilted") {
            const double g = std::hypot(r.surface.plane.slope_x, r.surface.plane.slope_y);
            CheckNear(of.surface_high_side.x, r.surface.plane.slope_x/g, 1e-9, c.name + ": the high side of the paper");
        }
        if (c.name == "flat_high") {
            // The second reading of surface-height: B moved along the baseline by u, away from A for a high paper.
            const auto& e = scene.truth.e_b;
            const auto& m = of.baseline_equivalent_motion_mm;
            CheckNear((m.x*e.x+m.y*e.y)/std::hypot(m.x, m.y), 1, 1e-3, c.name + ": the equivalent motion is along the baseline, away from A");
            CheckNear(std::hypot(m.x, m.y), r.surface.display.baseline_equivalent_mm, 0.02*r.surface.display.baseline_equivalent_mm, c.name + ": its length is u");
        }
        if (c.name == "b_moved_baseline") {
            // B really moved along the baseline: under the gauge it is read as h0, and the second reading gives the motion back.
            const auto& m = of.baseline_equivalent_motion_mm;
            CheckNear(m.x, -t2.tx_mm, 0.2, c.name + ": the equivalent motion returns B's real motion (mm)");
        }
        // The gauge: the translation along the baseline stays at the base pose.
        CheckNear(r.pose.tx_mm*r.diagnostics.gauge_direction.y-r.pose.ty_mm*r.diagnostics.gauge_direction.x, 0, 1e-9, c.name + ": only the free translation moves");
    }

    // Surface tilt with B's rotation and scale: they come back apart.
    {
        const auto scene = ps::MakeScene(rig, seam, SceneNamed(catalog, "tilt_with_b_rotation_scale"), options);
        const auto r = estimate(scene, settings, Threads());
        CheckNear(r.display.tilt_deg, 0, 0.1, "surface tilt with B's rotation: B's tilt stays near 0 (deg)");
        Check(r.diagnostics.maximum_surface_pose_correlation > 0 && r.diagnostics.maximum_surface_pose_correlation < 1, "the surface-pose correlation is reported");
    }

    // A high plane beyond the range: estimated, not clamped, the surface height named.
    {
        const auto scene = ps::MakeScene(rig, seam, SceneNamed(catalog, "flat_high_outside"), options);
        const auto r = estimate(scene, settings, Threads());
        Check(r.state == cr::RegistrationState::out_of_envelope, "flat_high_outside: out of the envelope (" + Describe(r) + ")");
        Check(std::find(r.exceeded.begin(), r.exceeded.end(), cr::EnvelopeComponent::surface_height) != r.exceeded.end(), "flat_high_outside: the surface height is named");
        CheckNear(r.surface.plane.h0_mm, truth_h0(scene, r), 0.5, "flat_high_outside: h0 is not clamped (mm)");
        CheckNear(SplitAtSeam(scene.truth, r, frame).across_rms, 0, 0.1, "flat_high_outside: rms across the baseline (px)");
        const auto& shift = r.diagnostics.coarse_shift;
        Check(shift.determined, "flat_high_outside: the coarse uniform shift is determined (G3 candidate)");
        CheckNear(shift.surface.h0_mm, truth_h0(scene, r), 3.0, "flat_high_outside: the coarse shift carries h0 (mm)");
        const auto display = r.surface.display;
        CheckNear(display.baseline_equivalent_mm, r.surface.plane.h0_mm*r.surface.frame.baseline_length_mm/
            (0.5*(r.surface.frame.camera_a_height_mm+r.surface.frame.camera_b_height_mm)-r.surface.plane.h0_mm), 1e-9, "the baseline equivalent u");
        Check(display.scale_factor > 1 && display.scale_minimum <= display.scale_factor && display.scale_factor <= display.scale_maximum, "the scale of a high plane is above 1");
    }

    // Table cells and text in the band: the vote picks the right peak. Table cells alone: repeating-only.
    {
        const auto scene = ps::MakeScene(rig, seam, SceneNamed(catalog, "periodic_and_text"), options);
        const auto r = estimate(scene, settings, Threads());
        Check(r.diagnostics.coarse_vote.decided, "table and text: the vote decides");
        Check(r.has_estimate, "table and text: an estimate exists");
        if (r.has_estimate) {
            CheckNear(r.surface.plane.h0_mm, truth_h0(scene, r), 0.3, "table and text: h0 (mm)");
            CheckNear(SplitAtSeam(scene.truth, r, frame).across_rms, 0, 0.1, "table and text: rms across the baseline (px)");
        }
        auto spec = SceneNamed(catalog, "periodic_and_text");
        spec.name = "periodic_only";
        spec.pattern.kind = ps::PatternKind::periodic_grid;
        const auto periodic = ps::MakeScene(rig, seam, spec, options);
        const auto q = estimate(periodic, settings, Threads());
        Check(q.state == cr::RegistrationState::undetermined, "table cells alone: undetermined (" + Describe(q) + ")");
        Check(!q.reasons.empty() && q.reasons.front() == "repeating-only", "table cells alone: repeating-only");
    }

    // Little content: the band has texture in a narrow strip of rows only. The prior keeps the fit finite.
    {
        auto spec = SceneNamed(catalog, "flat_tilted");
        spec.name = "sparse_strip";
        const auto scene = ps::MakeScene(rig, seam, spec, options);
        auto a = scene.image_a, b = scene.image_b;
        // Keep a strip of raw rows around the middle; the rest is white.
        for (auto* image : {&a, &b}) for (unsigned v = 0; v < image->height; ++v) {
            if (v > image->height/2-150 && v < image->height/2+150) continue;
            std::fill(image->bgr.begin()+static_cast<std::ptrdiff_t>(v)*image->width*3, image->bgr.begin()+static_cast<std::ptrdiff_t>(v+1)*image->width*3, std::uint8_t{255});
        }
        const ps::Scene sparse{spec, a, b, scene.truth};
        bool threw = false;
        cr::RegistrationResult r;
        try { r = estimate(sparse, settings, Threads()); } catch (const std::exception&) { threw = true; }
        Check(!threw, "sparse content: no exception");
        const bool finite = std::isfinite(r.surface.plane.h0_mm) && std::isfinite(r.surface.plane.slope_x) && std::isfinite(r.surface.plane.slope_y) &&
            std::isfinite(r.pose.rx_deg) && std::isfinite(r.pose.tz_percent);
        Check(finite, "sparse content: the estimate stays finite");
        Check(r.has_estimate, "sparse content: an estimate exists (" + Describe(r) + ")");
        if (r.has_estimate) {
            Check(std::isfinite(r.diagnostics.information_condition_number), "sparse content: the condition number is reported");
            Check(std::abs(r.surface.plane.h0_mm) < 3*settings.surface_envelope->height_mm, "sparse content: h0 does not diverge");
            Check(r.surface.display.tilt_deg < 3*settings.surface_envelope->tilt_deg, "sparse content: the tilt does not diverge");
        }
    }

    // W never reaches Omega: every measured point of the fit, and its patch, is outside Omega (+ the guard for the point).
    {
        const auto scene = ps::MakeScene(rig, seam, SceneNamed(catalog, "flat_high"), options);
        const auto r = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, settings, Threads());
        const auto& g = settings.geometry;
        bool outside = !r.diagnostics.observations.empty() && !r.diagnostics.last_points.empty();
        for (const auto& o : r.diagnostics.observations) {
            const double distance = std::abs(o.x-static_cast<double>(g.seam_column));
            outside = outside && distance > g.exclusion_half_width_px() && distance-settings.design.fine_patch_px/2.0 >= g.omega_half_width_px();
        }
        for (const auto& p : r.diagnostics.last_points) {
            const double distance = std::abs(static_cast<double>(p.x)-static_cast<double>(g.seam_column));
            outside = outside && distance > g.exclusion_half_width_px();
        }
        Check(outside, "no point of W, nor its patch, is in Omega");
        Check(!r.diagnostics.last_placement_calibrated, "the last placement is reported for layer 2");
    }

    // Determinism in the surface mode: one and four threads, two runs.
    {
        const auto scene = ps::MakeScene(rig, seam, SceneNamed(catalog, "tilt_with_b_rotation_scale"), options);
        const auto one = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, settings, 1);
        const auto four = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, settings, 4);
        const auto again = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, settings, 4);
        Check(one.has_estimate, "surface determinism: an estimate exists");
        Check(SameSurfaceBytes(one, four), "surface: one thread and four threads give the same bytes");
        Check(SameSurfaceBytes(four, again), "surface: two runs give the same bytes");
    }

    // The caller's stop (addendum 11.25): undetermined, time-budget, the coarse stage kept.
    {
        const auto scene = ps::MakeScene(rig, seam, SceneNamed(catalog, "flat_high"), options);
        cr::RegistrationControl control;
        control.stop = []() { return true; };
        const auto r = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, settings, Threads(), control);
        Check(r.state == cr::RegistrationState::undetermined && !r.reasons.empty() && r.reasons.front() == "time-budget", "a stop gives time-budget (" + Describe(r) + ")");
        Check(!r.has_estimate, "a stop drops the fit");
        Check(r.diagnostics.coarse_pose.has_value() && r.diagnostics.coarse_shift.determined, "a stop keeps the coarse stage");
    }
    std::cerr << "surface cases took " << std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() << " s\n";
}

// ---- Repeating content, a break of the surface beside the seam, a surface beyond its range ------------------------------
// The lengths are examples for the synthetic rig (48 dpi, about 0.67 px of parallax per mm), not product values.

// A table: lines that repeat with one period in both directions, and in each cell a soft shade that does not repeat (a
// hash of the cell's index). The edge energy of the band repeats; what the lines enclose does not.
ps::Sheet ShadedTableSheet(double period_mm, double shade_dn) {
    return [period_mm, shade_dn](double x, double y) {
        const double u = x/period_mm, v = y/period_mm;
        const double fu = u-std::floor(u), fv = v-std::floor(v);
        const auto line = [period_mm](double f) { const double d = std::min(f, 1-f)*period_mm; return std::exp(-d*d/(2*0.8*0.8)); };
        std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(u)))*0x9E3779B97F4A7C15ull ^
            static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(v)))*0xC2B2AE3D27D4EB4Full;
        h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
        const double level = static_cast<double>(h & 0xFFFF)/65535.0*2-1;
        return 185.0+shade_dn*level*std::sin(kPi*fu)*std::sin(kPi*fv)-130.0*std::max(line(fu), line(fv));
    };
}

// The scene of a spec with another sheet in place of the spec's pattern (the generator's own steps).
ps::Scene SceneWithSheet(const ps::Rig& rig, const ps::SeamPlacement& seam, const ps::SceneSpec& spec, const ps::Sheet& sheet, const ps::SceneOptions& options) {
    ps::SceneModel model(rig, seam, spec);
    model.sheet = sheet;
    ps::Scene scene{spec, {}, {}, ps::MakeTruth(model, options.truth)};
    scene.image_a = ps::RenderRaw(model, ps::Camera::a);
    scene.image_b = ps::RenderRaw(model, ps::Camera::b);
    return scene;
}

std::string DescribeCoarse(const cr::RegistrationResult& r) {
    std::ostringstream s;
    const auto& d = r.diagnostics;
    s << "radius " << d.coarse_radius_x_px << "x" << d.coarse_radius_y_px << " vote anchors " << d.coarse_vote.anchor_support << "/" << d.coarse_vote.rival_anchor_support
      << (d.coarse_vote.decided ? " decided" : " open") << "; blocks";
    for (const auto& b : d.coarse_blocks) {
        s << " [" << cr::CoarseBlockStatusCode(b.status);
        if (b.compared_peaks) s << " n " << b.compared_peaks << " colour " << b.colour_score << " lead " << b.colour_margin;
        if (b.voted) s << " d " << b.displacement.x << "," << b.displacement.y;
        s << "]";
    }
    const auto& w = d.fit_window;
    s << "; window left " << w.left_px << "/" << w.left_available_px << (w.left_cut ? " cut" : "") << " right " << w.right_px << "/" << w.right_available_px
      << (w.right_cut ? " cut" : "") << " core " << w.core_points << " beyond " << w.refused_points;
    return s.str();
}

void Repeating() {
    const auto started = std::chrono::steady_clock::now();
    const ps::Rig rig = ps::StandardRig();
    const ps::SeamPlacement seam = ps::PlaceSeam(rig);
    const auto parameters = rig.Parameters();
    const auto frame = cr::FrameOfRaster(rig.raster);
    ps::SceneOptions options;
    options.truth.height_map = false;
    const double period_mm = 24.0;
    const double period_px = period_mm/frame.pitch_mm;
    const auto table = ShadedTableSheet(period_mm, 60.0);
    const auto count = [](const cr::RegistrationResult& r, cr::CoarseBlockStatus status) {
        unsigned n = 0;
        for (const auto& b : r.diagnostics.coarse_blocks) if (b.status == status) ++n;
        return n;
    };

    // The period is far below the search radius, so every block of the table has many equal peaks of the edge energy. The
    // shades pick the place: the estimate exists, is verified, and is the true one.
    {
        ps::SceneSpec spec;
        spec.name = "table_near";
        spec.field.plane = ps::PlaneOver(seam, 3.0, 0, 0);
        spec.b_pose = {0.1, -0.1, 0.3, 0, -1.5, 0.2};
        const auto scene = SceneWithSheet(rig, seam, spec, table, options);
        const auto settings = SurfaceSettings(seam.column);
        const auto r = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, settings, Threads());
        const auto split = SplitAtSeam(scene.truth, r, frame);
        std::cerr << spec.name << ": " << Describe(r) << " " << DescribeCoarse(r) << "; seam across rms " << split.across_rms << " along max " << split.along_max
                  << "; h0 " << r.surface.plane.h0_mm << " pose rz " << r.pose.rz_deg << " ty " << r.pose.ty_mm << '\n';
        Check(r.diagnostics.coarse_radius_x_px > 2*period_px && r.diagnostics.coarse_radius_y_px > 2*period_px, spec.name + ": the search radius holds several periods");
        Check(count(r, cr::CoarseBlockStatus::resolved) >= 2, spec.name + ": blocks whose edge energy repeats are resolved by what the cells hold");
        Check(r.diagnostics.coarse_vote.decided, spec.name + ": the vote decides");
        Check(r.state == cr::RegistrationState::verified, spec.name + ": verified (" + Describe(r) + ")");
        if (r.has_estimate) {
            CheckNear(split.across_rms, 0, 0.1, spec.name + ": rms across the baseline at the seam column (px)");
            CheckNear(split.along_max, 0, 0.15, spec.name + ": largest residual along the baseline at the seam column (px)");
            CheckNear(r.pose.rz_deg, spec.b_pose.rz_deg, 0.05, spec.name + ": B's rotation");
        }
    }

    // B really sits one period away across the baseline. The lines agree at the calibrated place too (the peak next to
    // the base is one period off the truth); the shades do not, and the estimate is the true place, not the near one.
    {
        ps::SceneSpec spec;
        spec.name = "table_one_period_away";
        const cr::PoseModel model(rig.camera_b);
        spec.b_pose = {0, 0, 0, 0, period_mm, 0};
        const auto scene = SceneWithSheet(rig, seam, spec, table, options);
        auto settings = SurfaceSettings(seam.column);
        settings.envelope.position_mm = 30.0;   // the true place is inside the range; the search holds both peaks
        const auto r = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, settings, Threads());
        const auto split = SplitAtSeam(scene.truth, r, frame);
        std::cerr << spec.name << ": " << Describe(r) << " " << DescribeCoarse(r) << "; seam across rms " << split.across_rms << " max " << split.across_max
                  << " along max " << split.along_max << "; pose tx " << r.pose.tx_mm << " ty " << r.pose.ty_mm << '\n';
        Check(r.has_estimate, spec.name + ": an estimate exists (" + Describe(r) + ")");
        if (r.has_estimate) {
            Check(r.state == cr::RegistrationState::verified, spec.name + ": verified (" + Describe(r) + ")");
            CheckNear(std::hypot(r.pose.tx_mm, r.pose.ty_mm), period_mm, 0.5, spec.name + ": B's translation is the true one, a period from the base (mm)");
            CheckNear(split.across_max, 0, 0.3, spec.name + ": the seam is placed at the true peak, not a period off (px)");
        }
    }

    // The same table with nothing in its cells: no peak can be told from the others, and the state says so.
    {
        ps::SceneSpec spec;
        spec.name = "table_empty_cells";
        spec.field.plane = ps::PlaneOver(seam, 3.0, 0, 0);
        const auto scene = SceneWithSheet(rig, seam, spec, ShadedTableSheet(period_mm, 0.0), options);
        const auto settings = SurfaceSettings(seam.column);
        const auto r = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, settings, Threads());
        std::cerr << spec.name << ": " << Describe(r) << " " << DescribeCoarse(r) << '\n';
        Check(r.state == cr::RegistrationState::undetermined && !r.has_estimate, spec.name + ": undetermined, no estimate (" + Describe(r) + ")");
        Check(!r.reasons.empty() && r.reasons.front() == "repeating-only", spec.name + ": repeating-only");
        Check(count(r, cr::CoarseBlockStatus::resolved) == 0, spec.name + ": no block is resolved");
    }

    // The same table with shades in the cells of one block's rows only: one anchor. The other blocks follow it (each has a
    // strong peak next to the place the anchor gives), but a block that repeats agrees with almost any place and is no
    // evidence of it. V2 counts the anchors, so one anchor with followers is not a verified estimate.
    {
        ps::SceneSpec spec;
        spec.name = "table_one_shaded_block";
        spec.field.plane = ps::PlaneOver(seam, 3.0, 0, 0);
        auto settings = SurfaceSettings(seam.column);
        settings.design.coarse_block_rows = 128;   // several blocks down the band
        // Where the blocks are: the coarse stage reports them for the empty table too.
        const auto empty_scene = SceneWithSheet(rig, seam, spec, ShadedTableSheet(period_mm, 0.0), options);
        const auto blocks = cr::EstimateRegistration(empty_scene.image_a, empty_scene.image_b, parameters, settings, Threads()).diagnostics.coarse_blocks;
        Check(blocks.size() >= 4, spec.name + ": the band holds several blocks");
        if (blocks.size() >= 4) {
            const auto& one = blocks[blocks.size()/2];
            // The shaded rows stay a period inside the block, so the neighbours' cells are empty.
            const double y_low = frame.top_mm+(one.y0+0.5)*frame.pitch_mm+period_mm, y_high = frame.top_mm+(one.y0+one.rows+0.5)*frame.pitch_mm-period_mm;
            const auto shaded = ShadedTableSheet(period_mm, 60.0), empty = ShadedTableSheet(period_mm, 0.0);
            const ps::Sheet sheet = [=](double x, double y) { return y >= y_low && y < y_high ? shaded(x, y) : empty(x, y); };
            const auto scene = SceneWithSheet(rig, seam, spec, sheet, options);
            const auto r = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, settings, Threads());
            std::cerr << spec.name << ": " << Describe(r) << " " << DescribeCoarse(r) << "; blocks used " << r.diagnostics.unique_blocks << " consistent "
                      << r.diagnostics.consistent_blocks << " anchors " << r.diagnostics.anchor_blocks << " consistent " << r.diagnostics.consistent_anchor_blocks << '\n';
            unsigned followers = 0;
            for (const auto& b : r.diagnostics.coarse_blocks) if (b.status == cr::CoarseBlockStatus::repeating && b.voted) ++followers;
            Check(count(r, cr::CoarseBlockStatus::resolved)+count(r, cr::CoarseBlockStatus::unique) == 1, spec.name + ": one block is an anchor");
            Check(followers >= 2, spec.name + ": the blocks that repeat follow it");
            Check(r.diagnostics.anchor_blocks == 1 && r.diagnostics.consistent_anchor_blocks <= 1, spec.name + ": one anchor is counted");
            Check(r.diagnostics.consistent_blocks >= settings.limits.minimum_unique_blocks && 2*r.diagnostics.consistent_blocks >= r.diagnostics.unique_blocks,
                spec.name + ": counted with the followers, the blocks reach the line of V2");
            Check(r.state == cr::RegistrationState::undetermined, spec.name + ": not verified on one anchor (" + Describe(r) + ")");
            // The only reason: every other check passes, so it is the count of anchors alone that keeps the estimate from being trusted.
            Check(r.reasons.size() == 1 && r.reasons.front() == "unique-blocks", spec.name + ": the one reason is unique-blocks");
            Check(r.has_estimate, spec.name + ": the fit still ran (the followers help the start)");
        }
    }
    std::cerr << "repeating cases took " << std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() << " s\n";
}

void Window() {
    const auto started = std::chrono::steady_clock::now();
    const ps::Rig rig = ps::StandardRig();
    const ps::SeamPlacement seam = ps::PlaceSeam(rig);
    const auto parameters = rig.Parameters();
    const auto frame = cr::FrameOfRaster(rig.raster);
    ps::SceneOptions options;
    options.truth.height_map = false;
    const double px_per_mm = 1.0/frame.pitch_mm;
    auto settings = SurfaceSettings(seam.column);
    // The synthetic raster is about a quarter of a real one: the core is the two columns next to the seam.
    settings.design.window_core_px = 48;
    const auto run = [&](const ps::Scene& scene, const cr::RegistrationSettings& s) {
        auto r = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, s, Threads());
        const auto split = SplitAtSeam(scene.truth, r, frame);
        std::cerr << scene.spec.name << ": " << Describe(r) << " " << DescribeCoarse(r) << "; seam across rms " << split.across_rms << " along max " << split.along_max
                  << "; h0 " << r.surface.plane.h0_mm << " gx " << r.surface.plane.slope_x << " gy " << r.surface.plane.slope_y << " cond "
                  << r.diagnostics.information_condition_number << " sigma_pred " << r.diagnostics.maximum_prediction_sigma_px << '\n';
        return r;
    };

    // Two planes that meet in a fold beside the seam (a gutter: the far page turns down). The fit takes the seam's page
    // only: the far page is cut off, the near side is not, and the seam is placed by the near page's plane.
    {
        ps::SceneSpec spec;
        spec.name = "two_planes";
        const double fold_mm = 45.0, step_mm = 4.0, turn = std::tan(7.0*kPi/180);
        spec.field.plane = ps::PlaneOver(seam, 6.0, 2.0, 0.5);
        // The far page as a stair of smooth steps (the generator has no surface with a kink): 7 degrees down from the fold on.
        for (int i = 0; i < 45; ++i) spec.field.steps.push_back({seam.x_mm+fold_mm+(i+0.5)*step_mm, seam.y_mid_mm, -90.0, -turn*step_mm, step_mm/2});
        const auto scene = ps::MakeScene(rig, seam, spec, options);
        const auto r = run(scene, settings);
        const auto split = SplitAtSeam(scene.truth, r, frame);
        const auto& w = r.diagnostics.fit_window;
        Check(w.grown, spec.name + ": the fit window is reported");
        // The first column past the fold is a few mm down the far page and may still agree; the ones after it may not.
        Check(w.right_cut && w.right_px < (fold_mm+20.0)*px_per_mm, spec.name + ": the far page is left out of the fit");
        Check(w.right_available_px > (fold_mm+35.0)*px_per_mm && w.refused_points >= 16, spec.name + ": points exist on the far page");
        Check(!w.left_cut && w.left_px == w.left_available_px, spec.name + ": the side without a fold is taken whole");
        Check(r.state == cr::RegistrationState::verified, spec.name + ": verified (" + Describe(r) + ")");
        if (r.has_estimate) {
            CheckNear(split.along_max, 0, 0.15, spec.name + ": largest residual along the baseline at the seam column (px)");
            CheckNear(split.across_rms, 0, 0.1, spec.name + ": rms across the baseline at the seam column (px)");
            CheckNear(r.surface.plane.slope_x, spec.field.plane.slope_x, std::tan(0.5*kPi/180), spec.name + ": the slope is the near page's");
        }
        // The same pair with all of W as the core (no growth): one plane through both pages.
        auto whole = settings;
        whole.design.window_core_px = whole.design.window_reach_px = 100000;
        auto whole_spec = scene;
        whole_spec.spec.name = "two_planes_whole_band";
        const auto q = run(whole_spec, whole);
        const auto whole_split = SplitAtSeam(scene.truth, q, frame);
        std::cerr << "two_planes: along max " << split.along_max << " px with the grown window, " << whole_split.along_max << " px with the whole band\n";
        // The scene has something to cut: one plane through both pages leaves more at the seam than the near page's plane.
        Check(q.has_estimate && whole_split.along_max > 0.3 && whole_split.along_max > 2*split.along_max, spec.name + ": the whole band as one plane is worse at the seam");
    }

    // One plane over all of W: nothing is cut.
    {
        const auto catalog = ps::SceneCatalog(seam);
        const auto scene = ps::MakeScene(rig, seam, SceneNamed(catalog, "flat_tilted"), options);
        const auto r = run(scene, settings);
        const auto& w = r.diagnostics.fit_window;
        Check(r.state == cr::RegistrationState::verified, "one plane: verified (" + Describe(r) + ")");
        Check(!w.left_cut && !w.right_cut && w.refused_points == 0, "one plane: every column is taken");
        // The reach of the fine stage: nothing is measured or fitted beyond it.
        auto near = settings;
        near.design.window_reach_px = 64;
        const auto q = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, near, Threads());
        const double limit = near.geometry.exclusion_half_width_px()+64.0;
        Check(q.state == cr::RegistrationState::verified, "short reach: verified (" + Describe(q) + ")");
        Check(q.diagnostics.fit_window.left_available_px <= limit && q.diagnostics.fit_window.right_available_px <= limit &&
            q.diagnostics.points < r.diagnostics.points, "short reach: no point beyond the reach");
        bool inside = !q.diagnostics.last_points.empty();
        for (const auto& p : q.diagnostics.last_points) inside = inside && std::abs(static_cast<double>(p.x)-static_cast<double>(near.geometry.seam_column)) <= limit;
        Check(inside, "short reach: the measurer was not asked beyond the reach");
    }

    // A surface tilted beyond its range, and B turned beyond its own: estimated as it is (not held back by the range or
    // by the prior), with the component named. Applying it with a warning is the capture path's part.
    {
        ps::SceneSpec spec;
        spec.name = "tilt_beyond_range";
        spec.field.plane = ps::PlaneOver(seam, 5.0, 6.0, 1.0);
        const auto scene = ps::MakeScene(rig, seam, spec, options);
        const auto r = run(scene, settings);
        const auto split = SplitAtSeam(scene.truth, r, frame);
        Check(r.state == cr::RegistrationState::out_of_envelope, spec.name + ": out of the envelope (" + Describe(r) + ")");
        Check(r.has_estimate, spec.name + ": the estimate is kept");
        Check(std::find(r.exceeded.begin(), r.exceeded.end(), cr::EnvelopeComponent::surface_tilt) != r.exceeded.end(), spec.name + ": the surface tilt is named");
        Check(r.surface.estimate.exceeded.tilt, spec.name + ": the report's surface carries the excess");
        CheckNear(r.surface.display.tilt_deg, std::atan(std::hypot(spec.field.plane.slope_x, spec.field.plane.slope_y))*180/kPi, 0.2, spec.name + ": the tilt is not clamped (deg)");
        CheckNear(split.along_max, 0, 0.15, spec.name + ": largest residual along the baseline at the seam column (px)");
        CheckNear(split.across_rms, 0, 0.1, spec.name + ": rms across the baseline at the seam column (px)");
    }
    {
        ps::SceneSpec spec;
        spec.name = "b_rotation_beyond_range";
        spec.b_pose = {0, 0, 3.0, 0, 0, 0};
        const auto scene = ps::MakeScene(rig, seam, spec, options);
        const auto r = run(scene, settings);
        Check(r.state == cr::RegistrationState::out_of_envelope, spec.name + ": out of the envelope (" + Describe(r) + ")");
        Check(std::find(r.exceeded.begin(), r.exceeded.end(), cr::EnvelopeComponent::rotation) != r.exceeded.end(), spec.name + ": the rotation is named");
        CheckNear(r.pose.rz_deg, 3.0, 0.05, spec.name + ": the prior does not hold the rotation back (deg)");
        CheckNear(SplitAtSeam(scene.truth, r, frame).across_rms, 0, 0.1, spec.name + ": rms across the baseline at the seam column (px)");
    }
    std::cerr << "window cases took " << std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() << " s\n";
}

// A sheet whose content near the seam is narrow bands of text beside a ruled table (a repeat of 22 px in the measurer's feature,
// which is longer than the legacy window of the periodicity check at the first search radius of 16 and shorter than twice it): the
// measurer judges every patch of the table periodic when its first radius is 16, and takes the few rows of text only. The coarse
// stage places B to within its consistency, so a radius of 8 finds the match, and the table gives the rows between the bands.
void Ruled() {
    const auto started = std::chrono::steady_clock::now();
    const ps::Rig rig = ps::StandardRig();
    const ps::SeamPlacement seam = ps::PlaceSeam(rig);
    const auto parameters = rig.Parameters();
    const auto frame = cr::FrameOfRaster(rig.raster);
    ps::SceneOptions options;
    options.truth.height_map = false;
    const auto legacy = SurfaceSettings(seam.column);   // first search radius 16, as the estimator's tests are set
    auto draft = legacy;
    // The first radius of the design draft (on this raster too), and the second start's window the same as its first.
    draft.design.fine_search_radius_px = cr::kRegistrationDesignDraft.fine_search_radius_px;
    draft.design.second_start_search_radius_px = cr::kRegistrationDesignDraft.fine_search_radius_px;
    const auto run = [&](const ps::Scene& scene, const cr::RegistrationSettings& s, const std::string& label) {
        auto r = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, s, Threads());
        const auto split = SplitAtSeam(scene.truth, r, frame);
        std::cerr << scene.spec.name << " " << label << ": " << Describe(r) << " points " << r.diagnostics.points << " h0 " << r.surface.plane.h0_mm
                  << " sigma_pred " << r.diagnostics.maximum_prediction_sigma_px << " second " << cr::SecondStartCode(r.diagnostics.second_start)
                  << "; seam across rms " << split.across_rms << " max " << split.across_max << '\n';
        return r;
    };

    ps::SceneSpec spec;
    spec.name = "ruled_beside_text";
    spec.pattern.kind = ps::PatternKind::banded_beside_ruled;
    spec.pattern.period_mm = 24.0;
    spec.pattern.split_mm = seam.x_mm;
    for (int k = -1; k <= 1; ++k) spec.pattern.bands.push_back({seam.y_mid_mm+110.0*k-10.0, seam.y_mid_mm+110.0*k+10.0, ps::PatternKind::two_directional, 0});
    spec.field.plane = ps::PlaneOver(seam, 3.0, 0, 0);
    const auto scene = ps::MakeScene(rig, seam, spec, options);
    const auto before = run(scene, legacy, "first radius 16");
    const auto after = run(scene, draft, "first radius of the draft");
    // The table is out of the legacy fit (a patch whose repeat is within twice the radius is periodic) and in the draft's.
    std::size_t periodic_before = 0, periodic_after = 0;
    for (const auto& p : before.diagnostics.last_points) periodic_before += p.status == a0::m2::seam_gate::PointStatus::periodic;
    for (const auto& p : after.diagnostics.last_points) periodic_after += p.status == a0::m2::seam_gate::PointStatus::periodic;
    Check(periodic_before >= 2*periodic_after && periodic_before > 20, "ruled: the table is judged periodic at the legacy radius and not at the draft's");
    Check(before.state == cr::RegistrationState::undetermined && !before.has_estimate && !before.reasons.empty() && before.reasons.front() == "too-few-points",
        "ruled: the legacy radius leaves too few points (" + Describe(before) + ")");
    Check(after.state == cr::RegistrationState::verified, "ruled: the draft's radius gives a verified estimate (" + Describe(after) + ")");
    Check(after.diagnostics.points >= 4*std::max<std::uint32_t>(before.diagnostics.points, 1), "ruled: the table's points are fitted");
    CheckNear(after.surface.plane.h0_mm, 3.0, 0.2, "ruled: h0 from the table and the text (mm)");
    CheckNear(SplitAtSeam(scene.truth, after, frame).across_rms, 0, 0.1, "ruled: rms across the baseline at the seam column (px)");
    CheckNear(after.diagnostics.maximum_prediction_sigma_px, 0, 0.3, "ruled: the prediction at the seam is certain (px)");
    // The same sheet, the same radius, one thread and four: the same bytes.
    const auto one = cr::EstimateRegistration(scene.image_a, scene.image_b, parameters, draft, 1);
    Check(SameSurfaceBytes(one, after), "ruled: one thread and several give the same bytes");

    // The second start widens as far as the design's second radius and no farther: a sheet whose paper is far above the calibration
    // plane (the calibrated pose is off by more than the first radius) is held against V6 only when the second start can see it.
    {
        ps::SceneSpec far = ps::SceneCatalog(seam).front();
        for (const auto& s : ps::SceneCatalog(seam)) if (s.name == "flat_high") far = s;
        far.field.plane = ps::PlaneOver(seam, 12.0, 0, 0);
        far.name = "flat_far";
        const auto far_scene = ps::MakeScene(rig, seam, far, options);
        auto narrow = draft;
        narrow.design.second_start_search_radius_px = 8;
        auto wide = draft;
        wide.design.second_start_search_radius_px = 24;
        const auto a = run(far_scene, narrow, "second start 8");
        const auto b = run(far_scene, wide, "second start 24");
        Check(a.diagnostics.second_start != cr::RegistrationDiagnostics::SecondStart::agreed,
            "ruled: a second start that stops at the first radius does not see the calibrated pose's displacement");
        Check(b.diagnostics.second_start == cr::RegistrationDiagnostics::SecondStart::agreed, "ruled: a second start that widens sees it and agrees (" + Describe(b) + ")");
    }
    std::cerr << "ruled cases took " << std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() << " s\n";
}

// How each camera faces the drawing (the owner's guide for setting the cameras). The synthetic rig's cameras are made
// with known rolls and tilts, so the display can be checked against them.
void Orientation() {
    const Rig rig;
    const auto a = cr::OrientationOf(rig.camera_a), b = cr::OrientationOf(rig.camera_b);
    std::cerr << "orientation: A tilt " << a.tilt_deg << " towards (" << a.lens_direction.x << ", " << a.lens_direction.y << ") turn " << a.rotation_clockwise_deg
              << "; B tilt " << b.tilt_deg << " turn " << b.rotation_clockwise_deg << '\n';
    CheckNear(a.height_mm, 700, 0.5, "A's height over the table (mm)");
    CheckNear(a.centre_mm.x, 270, 3, "A's centre over the table (mm)");
    CheckNear(a.tilt_deg, std::hypot(0.15, 0.1), 0.01, "A's tilt from the table normal (deg)");
    CheckNear(b.tilt_deg, std::hypot(0.2, 0.1), 0.01, "B's tilt from the table normal (deg)");
    CheckNear(std::abs(a.rotation_clockwise_deg), 0.3, 0.01, "A's turn against the drawing (deg)");
    CheckNear(std::abs(b.rotation_clockwise_deg), 0.4, 0.01, "B's turn against the drawing (deg)");
    const auto rel = cr::RelativeOrientationOf(a, b);
    CheckNear(std::abs(rel.rotation_clockwise_deg), 0.7, 0.01, "B's turn against A (deg)");
    Check(rel.tilt_deg > 0 && rel.tilt_deg < a.tilt_deg+b.tilt_deg+1e-9, "B's tilt against A lies between the difference and the sum");
    // A camera turned half a turn (the 180-degree mounting) reads about 180 degrees from the same camera unturned.
    const auto turned = cr::OrientationOf(MakeCamera(270, 180, 700, 180.3, -0.15, 0.1));
    CheckNear(std::abs(cr::RelativeOrientationOf(a, turned).rotation_clockwise_deg), 180, 0.01, "a half turn reads 180 degrees");
}

// The base pose (addendum 11.19): the start, the prior centre and the gauge; the envelope is applied to p-hat - p_b.
void BasePose() {
    const Rig rig;
    const cr::PoseModel model(rig.camera_b);
    const auto wave = WaveSheet(7);
    const auto image_a = RenderRaw(rig.camera_a, wave);
    const cr::PoseParameters base{0.2, -0.1, 0.6, 6.0, -4.0, 0.3};
    const cr::PoseParameters truth{0.3, -0.2, 0.9, 11.0, -6.0, 0.5};
    auto moved = rig.camera_b;
    moved.document_to_image = model.CameraHomography(truth);
    const auto image_b = RenderRaw(moved, wave);
    const unsigned seam = SeamColumnOf(rig, image_a, RenderRaw(rig.camera_b, wave));
    auto settings = MakeSettings(seam);
    settings.base_pose = base;
    const auto r = cr::EstimateRegistration(image_a, image_b, rig.parameters, settings, Threads());
    std::cerr << "base pose: " << Describe(r) << " pose tx " << r.pose.tx_mm << " ty " << r.pose.ty_mm << " rz " << r.pose.rz_deg << " display position "
              << r.display.position_mm << '\n';
    Check(r.state == cr::RegistrationState::verified, "base pose: verified (" + Describe(r) + ")");
    if (!r.has_estimate) return;
    unsigned rows = 0;
    CheckNear(SeamResidualMaximum(rig, model, r.pose, truth, seam, rows), 0, 0.1, "base pose: residual at the seam column (px)");
    CheckNear(r.display.position_mm, std::hypot(truth.tx_mm-base.tx_mm, truth.ty_mm-base.ty_mm), 0.3, "base pose: the position is measured from the base");
    CheckNear(r.display.rotation_deg, truth.rz_deg-base.rz_deg, 0.05, "base pose: the rotation is measured from the base");
    // Beyond the envelope from the calibration, inside it from the base: verified (the base was accepted).
    Check(r.exceeded.empty(), "base pose: inside the envelope around the base");
    bool refused = false;
    auto bad = settings;
    bad.base_pose.tx_mm = std::nan("");
    try { cr::ValidateRegistrationSettings(bad); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "a nonfinite base pose is refused");
    refused = false;
    bad = settings;
    bad.surface_envelope = cr::SurfaceEnvelope{};
    try { cr::ValidateRegistrationSettings(bad); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "an unset surface envelope is refused");
}
}

// With no argument every section runs (ctest). A section name runs that section only (development).
int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    const auto wanted = [&](const char* name) { return only.empty() || only == name; };
    if (wanted("mapping")) PoseMapping();
    if (wanted("settings")) Settings();
    if (wanted("estimate")) Estimate();
    if (wanted("orientation")) Orientation();
    if (wanted("base")) BasePose();
    if (wanted("surface")) Surface();
    if (wanted("repeating")) Repeating();
    if (wanted("window")) Window();
    if (wanted("ruled")) Ruled();
    std::cout << checks-failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
