#pragma once
// Test-only generator of paper-height scenes for the parallax correction (#417 S3;
// docs/design/fixed-composite-registration.md, section "Parallax correction", 11.2, 11.3, 11.10).
//
// Given a height field h(x, y) of the paper above the calibration plane, it builds the raw images of camera A
// and camera B, and the truth that the images were made from. Nothing here is a product value: the rig numbers
// are the synthetic ones of tests/content_registration_tests.cpp, and the lengths and heights of the scenes are
// examples for a synthetic rig.
//
// Isolation (as for tests/seam_gate_scene.hpp): no seam gate, estimator or fixed-composite header is used. The
// only product pieces are the document render types (a0_m2_render) and the pose model of camera B
// (content_registration::PoseModel, a0_m2_registration_pose), which turns a pose p into the moved camera exactly
// as the pose tests do. The images are computed from the surface, not resampled from another image.
//
// Physics (design 11.2). The calibration plane is the table, with table mm coordinates (x, y) and a height
// toward the cameras. A camera with its centre at (C, H) above the table sees a paper point (P, h), with the
// paper h above the table, along the ray that meets the table at
//
//     T = C + (P - C) * H / (H - h)            (xy components)
//
// and the calibrated mapping (the document-to-image matrix and the lens) places that table point T. So a raw
// pixel whose calibrated table point is T shows the paper point P = T - (T - C) * h(P) / H, solved by a fixed-point
// iteration that is deterministic (a fixed stopping rule, no random numbers). The pattern is attached to the
// paper's (x, y): the paper is not stretched when it lifts (ASSUMPTION). Only smooth, single-valued fields are
// supported: no occlusion, no hidden surface. A field steep enough that the iteration does not settle throws.
//
// Output side. An output pixel (x, y) is the table point X of the output raster (the arithmetic of
// render::DocumentOutputToCamera). Camera A is the reference and is placed by its calibrated mapping, so A shows
// the paper point P_A(X) at X. Camera B really sits at the calibrated pose moved by the pose p of the scene
// (the moved camera of the pose tests: the calibrated intrinsics and lens, and the document-to-image matrix
// PoseModel::CameraHomography(p)), but the stitcher places it by the calibrated mapping. The truth displacement
// at X is therefore "B minus A", in output px: where the calibrated mapping places the B pixel that sees P_A(X),
// minus X. For h = 0 it is the pose displacement G(X) - X of PoseModel::Homography(p).
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/registration_pose.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace parallax_scene {
namespace render = a0::m2::render;
namespace cr = a0::m2::experimental::content_registration;
using render::BgrImage;
using render::Matrix3;
using render::Point;

inline constexpr double kPi = std::numbers::pi;
// The synthetic rig of tests/content_registration_tests.cpp: 1200 x 800 raw frames, a pinhole of focal length
// 1560 px and a mild lens.
inline constexpr unsigned kRawWidth = 1200, kRawHeight = 800;
inline constexpr double kFocal = 1560;
// ASSUMPTION: half width g_x of the hole of the layer 2 regression (design 11.4) is a design constant that M1 fixes.
// The scenes use this example length, on the scale of a few centimetres, to place folds.
inline constexpr double kAssumedHoleHalfWidthMm = 20.0;
// ASSUMPTION: an illustrative envelope for "inside" and "outside" scenes. The real envelope is the owner's value.
inline constexpr double kExampleEnvelopeHeightMm = 10.0;

struct Lcg {
    std::uint64_t state;
    double Uniform() { state = state*6364136223846793005ull+1442695040888963407ull; return static_cast<double>(state>>11)/9007199254740992.0; }
};

// ---- The rig ----------------------------------------------------------------------------------------------------------
inline Matrix3 Mul(const Matrix3& a, const Matrix3& b) {
    Matrix3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[3*i+j] += a[3*i+k]*b[3*k+j];
    return r;
}
inline Matrix3 RotationX(double a) { return {1,0,0,0,std::cos(a),-std::sin(a),0,std::sin(a),std::cos(a)}; }
inline Matrix3 RotationY(double a) { return {std::cos(a),0,std::sin(a),0,1,0,-std::sin(a),0,std::cos(a)}; }
inline Matrix3 RotationZ(double a) { return {std::cos(a),-std::sin(a),0,std::sin(a),std::cos(a),0,0,0,1}; }

inline const render::LensDistortion kLens{-0.035, 0.012, 0.0, 0.0005, -0.0003};

// A camera looking at the table from the height `depth` over the table point (x0, y0), rolled and tilted by the
// given angles (degrees). Same construction as MakeCamera of tests/content_registration_tests.cpp.
inline render::CameraProjection MakeCamera(double x0, double y0, double depth, double roll, double tilt_x, double tilt_y) {
    const double d = kPi/180;
    const auto rotation = Mul(RotationZ(roll*d), Mul(RotationX(tilt_x*d), RotationY(tilt_y*d)));
    const double c0[3]{rotation[0], rotation[3], rotation[6]}, c1[3]{rotation[1], rotation[4], rotation[7]};
    double t[3]{-x0*c0[0]-y0*c1[0], -x0*c0[1]-y0*c1[1], -x0*c0[2]-y0*c1[2]+depth};
    const double cx = (kRawWidth-1)/2.0, cy = (kRawHeight-1)/2.0;
    const render::Matrix3 k{kFocal,0,cx,0,kFocal,cy,0,0,1};
    const render::Matrix3 columns{c0[0],c1[0],t[0],c0[1],c1[1],t[1],c0[2],c1[2],t[2]};
    auto h = Mul(k, columns);
    const double n = h[8];
    for (auto& v : h) v /= n;
    h[8] = 1;
    return render::CameraProjection(render::CameraIntrinsics(kFocal,kFocal,cx,cy), kLens, h);
}

inline render::OutputRaster StandardRaster() {
    const std::int64_t left = 10000, top = 10000, right = 780000, bottom = 350000;
    const std::uint32_t dpi = 48;
    const auto pixels = [dpi](std::int64_t extent) {return static_cast<std::uint32_t>((extent*dpi+25399)/25400);};
    return render::OutputRaster(render::DocumentRegionUm(left, top, right, bottom), dpi, pixels(right-left), pixels(bottom-top));
}

struct Rig {
    render::OutputRaster raster;
    render::CameraProjection camera_a, camera_b;
    [[nodiscard]] render::DocumentRenderParameters Parameters() const {
        return render::DocumentRenderParameters(raster, camera_a, camera_b, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bilinear);
    }
};
// The numbers of the Rig of tests/content_registration_tests.cpp: two cameras 250 mm apart, 700 mm over the table,
// with small rolls and tilts.
inline Rig StandardRig() {
    return Rig{StandardRaster(), MakeCamera(270, 180, 700, 0.3, -0.15, 0.1), MakeCamera(520, 180, 700, -0.4, 0.2, -0.1)};
}
// The same baseline with both cameras level at the same height: the case where the design's closed form
// -b h / (H - h) holds exactly.
inline Rig IdealRig() {
    return Rig{StandardRaster(), MakeCamera(270, 180, 700, 0, 0, 0), MakeCamera(520, 180, 700, 0, 0, 0)};
}

// ---- Camera centre in the table frame -----------------------------------------------------------------------------------
struct CameraGeometry {
    Point center_mm;     // the camera centre projected on the table
    double height_mm{};  // its height above the table
};
// Decomposes the document-to-image matrix with the camera's own intrinsics (the columns are made orthonormal).
inline CameraGeometry GeometryOf(const render::CameraProjection& camera) {
    const auto& i = camera.intrinsics;
    const auto& h = camera.document_to_image;
    std::array<double,9> raw{};
    for (int j = 0; j < 3; ++j) {
        raw[j] = (h[j]-i.cx_pixels*h[6+j])/i.fx_pixels;
        raw[3+j] = (h[3+j]-i.cy_pixels*h[6+j])/i.fy_pixels;
        raw[6+j] = h[6+j];
    }
    std::array<double,3> c0{raw[0],raw[3],raw[6]}, c1{raw[1],raw[4],raw[7]}, c2{raw[2],raw[5],raw[8]};
    const auto dot = [](const std::array<double,3>& a, const std::array<double,3>& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const double n0 = std::sqrt(dot(c0,c0)), n1 = std::sqrt(dot(c1,c1));
    if (!(n0 > 0) || !(n1 > 0)) throw std::invalid_argument("parallax scene: degenerate document matrix");
    const double lambda = 2.0/(n0+n1);
    std::array<double,3> r1{c0[0]/n0,c0[1]/n0,c0[2]/n0};
    const double along = dot(c1,r1);
    std::array<double,3> r2{c1[0]-along*r1[0],c1[1]-along*r1[1],c1[2]-along*r1[2]};
    const double n2 = std::sqrt(dot(r2,r2));
    for (auto& v : r2) v /= n2;
    const std::array<double,3> r3{r1[1]*r2[2]-r1[2]*r2[1], r1[2]*r2[0]-r1[0]*r2[2], r1[0]*r2[1]-r1[1]*r2[0]};
    std::array<double,3> t{lambda*c2[0],lambda*c2[1],lambda*c2[2]};
    if (dot(r3,t) < 0) { for (auto& v : t) v = -v; }   // the camera is above the table
    const double height = dot(r3,t);
    if (!(height > 0)) throw std::invalid_argument("parallax scene: the camera is not above the table");
    return {{-dot(r1,t), -dot(r2,t)}, height};
}

// ---- Height fields ---------------------------------------------------------------------------------------------------------
// A plane z = h0 + slope_x (x - x0) + slope_y (y - y0), table mm. All zero is exactly 0 everywhere.
struct Plane {
    double h0_mm{}, slope_x{}, slope_y{}, x0_mm{}, y0_mm{};
    [[nodiscard]] double Height(double x, double y) const { return h0_mm + slope_x*(x-x0_mm) + slope_y*(y-y0_mm); }
};
// A smooth long wave: a cos(2 pi (x cos(phi) + y sin(phi)) / wavelength + phase).
struct Undulation { double amplitude_mm{}, wavelength_mm{100}, angle_deg{}, phase_rad{}; };
// A line (through (x_mm, y_mm), direction angle_deg from +x toward +y): a crease. A ridge when amplitude > 0, a
// valley when < 0 (relative to the rest of the field). Profile amplitude (1 - (d/w)^2)^2 inside |d| < w, else 0.
struct Crease { double x_mm{}, y_mm{}, angle_deg{}, amplitude_mm{}, half_width_mm{6}; };
// A step across a line: 0 on one side, height_mm on the other, joined by a smooth ramp of half width ramp_mm
// (a true discontinuity hides surface from a camera and is not supported). The positive side is the side that
// the normal (-sin(angle), cos(angle)) points to.
struct Step { double x_mm{}, y_mm{}, angle_deg{}, height_mm{}, ramp_half_width_mm{3}; };
// The gutter of a booklet along a line: the paper is lower at the line and rises to page_rise_mm on both sides,
// h = page_rise * (1 - exp(-(d / curl)^2)).
struct Gutter { double x_mm{}, y_mm{}, angle_deg{90}, page_rise_mm{}, curl_mm{25}; };
// A round bump: height * exp(-r^2 / (2 sigma^2)).
struct Bump { double x_mm{}, y_mm{}, height_mm{}, sigma_mm{4}; };

struct HeightField {
    Plane plane;
    std::vector<Undulation> undulations;
    std::vector<Crease> creases;
    std::vector<Step> steps;
    std::vector<Gutter> gutters;
    std::vector<Bump> bumps;

    [[nodiscard]] bool IsPlanar() const { return undulations.empty() && creases.empty() && steps.empty() && gutters.empty() && bumps.empty(); }
    [[nodiscard]] double Height(double x, double y) const {
        double h = plane.Height(x, y);
        for (const auto& u : undulations) {
            const double a = u.angle_deg*kPi/180;
            h += u.amplitude_mm*std::cos(2*kPi*(x*std::cos(a)+y*std::sin(a))/u.wavelength_mm+u.phase_rad);
        }
        for (const auto& c : creases) {
            const double d = SignedDistance(x, y, c.x_mm, c.y_mm, c.angle_deg);
            const double t = d/c.half_width_mm;
            if (std::abs(t) < 1) { const double v = 1-t*t; h += c.amplitude_mm*v*v; }
        }
        for (const auto& s : steps) {
            const double d = SignedDistance(x, y, s.x_mm, s.y_mm, s.angle_deg);
            const double t = std::clamp((d+s.ramp_half_width_mm)/(2*s.ramp_half_width_mm), 0.0, 1.0);
            h += s.height_mm*t*t*t*(t*(6*t-15)+10);
        }
        for (const auto& g : gutters) {
            const double d = SignedDistance(x, y, g.x_mm, g.y_mm, g.angle_deg)/g.curl_mm;
            h += g.page_rise_mm*(1-std::exp(-d*d));
        }
        for (const auto& b : bumps) {
            const double dx = x-b.x_mm, dy = y-b.y_mm;
            h += b.height_mm*std::exp(-(dx*dx+dy*dy)/(2*b.sigma_mm*b.sigma_mm));
        }
        return h;
    }
    // Signed distance (mm) of (x, y) from the line through (px, py) with direction angle_deg from +x toward +y.
    static double SignedDistance(double x, double y, double px, double py, double angle_deg) {
        const double a = angle_deg*kPi/180;
        return -(x-px)*std::sin(a)+(y-py)*std::cos(a);
    }
};

// Deterministic long waves from a seed: wavelength between the two bounds, any direction.
inline std::vector<Undulation> Undulations(std::uint64_t seed, int count, double rms_amplitude_mm, double min_wavelength_mm, double max_wavelength_mm) {
    Lcg r{seed};
    std::vector<Undulation> out;
    for (int k = 0; k < count; ++k) {
        Undulation u;
        u.wavelength_mm = min_wavelength_mm+(max_wavelength_mm-min_wavelength_mm)*r.Uniform();
        u.angle_deg = 360*r.Uniform();
        u.phase_rad = 2*kPi*r.Uniform();
        u.amplitude_mm = rms_amplitude_mm*std::sqrt(2.0/count);
        out.push_back(u);
    }
    return out;
}

// ---- Patterns on the paper -----------------------------------------------------------------------------------------------------
using Sheet = std::function<double(double, double)>;   // DN at paper (x, y) mm

enum class PatternKind {
    two_directional,   // a sum of plane waves: texture in every direction, no repeat
    one_directional,   // waves along one direction only: lines (no information along the lines)
    periodic_grid,     // the same period in both directions
    plain,             // white
    table_and_text,    // ASSUMPTION: bands of periodic table cells alternating with bands of non-periodic text-like texture
    banded,            // white paper with the bands of PatternSpec::bands (a drawing that is mostly blank at the seam)
    banded_beside_ruled, // the bands of PatternSpec::bands where x < split_mm, a periodic grid of period_mm where x >= split_mm (a ruler or a table of fine rows)
};
// A band of a banded sheet: paper y in [first_mm, last_mm) shows the pattern of `kind` (not banded), white elsewhere. With
// edge_mm > 0 the pattern fades to white over edge_mm on both sides (a raised cosine), so the band's edge is not a line.
struct PatternBand { double first_mm{}, last_mm{}; PatternKind kind{PatternKind::two_directional}; double edge_mm{0}; };
struct PatternSpec {
    PatternKind kind{PatternKind::two_directional};
    std::uint64_t seed{7};
    int wave_count{40};
    double period_mm{14.0};         // periodic_grid, and the cells of table_and_text
    double line_normal_deg{90.0};   // one_directional: the direction the pattern varies in (90 = horizontal lines)
    double band_mm{30.0};           // table_and_text: height of one band
    std::vector<PatternBand> bands; // banded, banded_beside_ruled
    double split_mm{0};             // banded_beside_ruled: the table x where the grid begins
};

inline Sheet WaveSheet(std::uint64_t seed, int count) {
    // Same draws and arithmetic as WaveSheet(seed) of tests/content_registration_tests.cpp when count == 40.
    struct Wave {double kx, ky, phase, amplitude;};
    std::vector<Wave> waves;
    Lcg r{seed};
    for (int k = 0; k < count; ++k) {
        const double wavelength = 4.0*std::pow(10.0, r.Uniform());   // 4 .. 40 mm
        const double angle = 2*kPi*r.Uniform();
        waves.push_back({2*kPi/wavelength*std::cos(angle), 2*kPi/wavelength*std::sin(angle), 2*kPi*r.Uniform(), 36.0*std::sqrt(2.0/count)});
    }
    return [waves](double x, double y) {
        double s = 150;
        for (const auto& w : waves) s += w.amplitude*std::cos(w.kx*x+w.ky*y+w.phase);
        return s;
    };
}
inline Sheet LineSheet(std::uint64_t seed, int count, double normal_deg) {
    struct Wave {double k, phase, amplitude;};
    std::vector<Wave> waves;
    Lcg r{seed};
    for (int k = 0; k < count; ++k) {
        const double wavelength = 4.0*std::pow(10.0, r.Uniform());
        waves.push_back({2*kPi/wavelength, 2*kPi*r.Uniform(), 36.0*std::sqrt(2.0/count)});
    }
    const double cx = std::cos(normal_deg*kPi/180), cy = std::sin(normal_deg*kPi/180);
    return [waves, cx, cy](double x, double y) {
        double s = 150;
        const double along = x*cx+y*cy;
        for (const auto& w : waves) s += w.amplitude*std::cos(w.k*along+w.phase);
        return s;
    };
}
inline Sheet PeriodicSheet(double period_mm) {
    return [period_mm](double x, double y) { return 150+45*std::cos(2*kPi*x/period_mm)+45*std::cos(2*kPi*y/period_mm); };
}
inline Sheet MakeSheet(const PatternSpec& p) {
    switch (p.kind) {
    case PatternKind::two_directional: return WaveSheet(p.seed, p.wave_count);
    case PatternKind::one_directional: return LineSheet(p.seed, p.wave_count, p.line_normal_deg);
    case PatternKind::periodic_grid: return PeriodicSheet(p.period_mm);
    case PatternKind::plain: return [](double, double) { return 255.0; };
    case PatternKind::table_and_text: {
        const auto cells = PeriodicSheet(p.period_mm);
        const auto text = WaveSheet(p.seed, p.wave_count);
        const double band = p.band_mm;
        return [cells, text, band](double x, double y) {
            const auto index = static_cast<long long>(std::floor(y/band));
            return (index%2 == 0) ? cells(x, y) : text(x, y);
        };
    }
    case PatternKind::banded: {
        std::vector<std::pair<PatternBand, Sheet>> bands;
        for (const auto& b : p.bands) {
            if (b.kind == PatternKind::banded || b.kind == PatternKind::banded_beside_ruled || !(b.first_mm < b.last_mm) || !(b.edge_mm >= 0)) throw std::invalid_argument("parallax scene: a band is not a pattern");
            PatternSpec inner = p;
            inner.kind = b.kind; inner.bands.clear();
            bands.push_back({b, MakeSheet(inner)});
        }
        return [bands](double x, double y) {
            for (const auto& [band, sheet] : bands) {
                const double outside = y < band.first_mm ? band.first_mm-y : y >= band.last_mm ? y-band.last_mm : 0.0;
                if (outside == 0) return sheet(x, y);
                if (outside < band.edge_mm) return 255.0+0.5*(1+std::cos(kPi*outside/band.edge_mm))*(sheet(x, y)-255.0);
            }
            return 255.0;
        };
    }
    case PatternKind::banded_beside_ruled: {
        PatternSpec left = p;
        left.kind = PatternKind::banded;
        const auto bands = MakeSheet(left);
        const auto ruled = PeriodicSheet(p.period_mm);
        const double split = p.split_mm;
        return [bands, ruled, split](double x, double y) { return x < split ? bands(x, y) : ruled(x, y); };
    }
    }
    throw std::invalid_argument("parallax scene: unknown pattern");
}

// ---- Raw pixel to table point -------------------------------------------------------------------------------------------------------
// The table point that a camera shows at a raw pixel (the lens inverted by a fixed number of iterations, then the
// inverse of the document-to-image matrix). Same arithmetic as SheetPointOf of tests/content_registration_tests.cpp.
class PixelToTable {
public:
    explicit PixelToTable(const render::CameraProjection& camera) : camera_(camera), inverse_(render::Invert(camera.document_to_image)) {}
    bool operator()(double u, double v, Point& out) const {
        const auto& i = camera_.intrinsics;const auto& d = camera_.distortion;
        const double xd = (u-i.cx_pixels)/i.fx_pixels, yd = (v-i.cy_pixels)/i.fy_pixels;
        double x = xd, y = yd;
        for (int it = 0; it < 60; ++it) {
            const double r2 = x*x+y*y, radial = 1+d.k1*r2+d.k2*r2*r2+d.k3*r2*r2*r2;
            const double mx = x*radial+2*d.p1*x*y+d.p2*(r2+2*x*x), my = y*radial+d.p1*(r2+2*y*y)+2*d.p2*x*y;
            x += xd-mx; y += yd-my;
        }
        return render::TryTransform(inverse_, {i.fx_pixels*x+i.cx_pixels, i.fy_pixels*y+i.cy_pixels}, out);
    }
private:
    render::CameraProjection camera_;
    Matrix3 inverse_;
};

[[nodiscard]] inline bool InsideFrame(Point raw) { return raw.x >= 0 && raw.y >= 0 && raw.x < kRawWidth && raw.y < kRawHeight; }

// ---- The paper point a camera sees ------------------------------------------------------------------------------------------------------
struct Surface {
    Point xy_mm;
    double height_mm{};
};
// P = T - (T - C) h(P) / H by fixed-point iteration from P = T (design 11.2: the paper point whose ray meets the
// table at T). A fixed stopping rule (step below 1e-12 mm, at most 200 iterations); throws std::runtime_error
// when it does not settle (a field too steep for a single-valued surface) or the surface reaches the camera.
// h == 0 returns T bit for bit.
inline Surface SolveSurface(const HeightField& field, const CameraGeometry& camera, Point table_mm) {
    constexpr int kMaximumIterations = 200;
    constexpr double kStopMm = 1e-12;
    Point p = table_mm;
    double h = field.Height(p.x, p.y);
    bool settled = false;
    for (int n = 0; n < kMaximumIterations; ++n) {
        const double scale = h/camera.height_mm;
        const Point next{table_mm.x-(table_mm.x-camera.center_mm.x)*scale, table_mm.y-(table_mm.y-camera.center_mm.y)*scale};
        const double step = std::hypot(next.x-p.x, next.y-p.y);
        p = next;
        h = field.Height(p.x, p.y);
        if (!std::isfinite(step) || !std::isfinite(h)) break;
        if (step <= kStopMm) { settled = true; break; }
    }
    if (!settled) throw std::runtime_error("parallax scene: the surface is not single-valued here (field too steep)");
    if (!(h < 0.5*camera.height_mm)) throw std::runtime_error("parallax scene: the paper reaches the camera");
    return {p, h};
}
// The table point where the ray of a camera through the paper point meets the table.
[[nodiscard]] inline Point TablePointOfSurface(const CameraGeometry& camera, const Surface& s) {
    const double denominator = camera.height_mm-s.height_mm;
    if (!(denominator > 0)) throw std::runtime_error("parallax scene: the paper reaches the camera");
    const double scale = camera.height_mm/denominator;
    return {camera.center_mm.x+(s.xy_mm.x-camera.center_mm.x)*scale, camera.center_mm.y+(s.xy_mm.y-camera.center_mm.y)*scale};
}

// ---- Scene description ----------------------------------------------------------------------------------------------------------------
struct SceneSpec {
    std::string name;
    int design_row{};            // the row of the table of design 11.10 that the scene stands for (1-based, 0 = none)
    std::string expectation;     // what the design expects of an estimator on this scene (plain text)
    PatternSpec pattern;
    HeightField field;
    cr::PoseParameters b_pose{}; // the real pose of camera B (the calibrated pose moved by p)
};

// Where the seam column is (an output column) and which rows of it both cameras cover.
struct SeamPlacement {
    unsigned column{};
    double x_mm{};              // table x of the seam column
    unsigned first_row{}, last_row{};
    double y_mid_mm{};          // table y of the middle of the covered rows
};
// The middle of the columns that both cameras cover (A and B at the calibrated pose), as the pose tests choose it.
inline SeamPlacement PlaceSeam(const Rig& rig) {
    const auto map_a = render::DocumentOutputToCamera(rig.raster, rig.camera_a);
    const auto map_b = render::DocumentOutputToCamera(rig.raster, rig.camera_b);
    const auto covered = [&](double x, double y) {
        Point ra{}, rb{};
        return map_a({x, y}, ra) && map_b({x, y}, rb) && InsideFrame(ra) && InsideFrame(rb);
    };
    std::uint32_t first = rig.raster.width_pixels, last = 0;
    for (unsigned y = 0; y < rig.raster.height_pixels; ++y) {
        std::uint32_t begin = 0, end = 0;
        bool found = false;
        for (unsigned x = 0; x < rig.raster.width_pixels; ++x) {
            if (!covered(x, y)) continue;
            if (!found) { begin = x; found = true; }
            end = x+1;
        }
        if (found) { first = std::min(first, begin); last = std::max(last, end); }
    }
    if (first >= last) throw std::runtime_error("parallax scene: the cameras do not overlap");
    SeamPlacement seam;
    seam.column = (first+last)/2;
    const auto frame = cr::FrameOfRaster(rig.raster);
    seam.x_mm = frame.left_mm+(seam.column+0.5)*frame.pitch_mm;
    bool any = false;
    for (unsigned y = 0; y < rig.raster.height_pixels; ++y) {
        if (!covered(seam.column, y)) continue;
        if (!any) { seam.first_row = y; any = true; }
        seam.last_row = y;
    }
    if (!any) throw std::runtime_error("parallax scene: the seam column is not covered");
    seam.y_mid_mm = frame.top_mm+((seam.first_row+seam.last_row)/2.0+0.5)*frame.pitch_mm;
    return seam;
}

// The moved camera of the pose tests: the calibrated camera with the document-to-image matrix of the pose (also
// for the zero pose, as the pose tests render it).
[[nodiscard]] inline render::CameraProjection MovedCamera(const render::CameraProjection& camera, const cr::PoseParameters& pose) {
    const cr::PoseModel model(camera);
    auto moved = camera;
    moved.document_to_image = model.CameraHomography(pose);
    return moved;
}

struct SceneModel {
    Rig rig;
    SeamPlacement seam;
    SceneSpec spec;
    cr::PlaneFrame frame;
    render::CameraProjection camera_b_true;
    CameraGeometry geometry_a, geometry_b_calibrated, geometry_b_true;
    PixelToTable a_to_table, b_calibrated_to_table, b_true_to_table;
    Sheet sheet;

    SceneModel(const Rig& r, const SeamPlacement& s, SceneSpec sp)
        : rig(r), seam(s), spec(std::move(sp)), frame(cr::FrameOfRaster(r.raster)), camera_b_true(MovedCamera(r.camera_b, spec.b_pose)),
          geometry_a(GeometryOf(r.camera_a)), geometry_b_calibrated(GeometryOf(r.camera_b)), geometry_b_true(GeometryOf(camera_b_true)),
          a_to_table(r.camera_a), b_calibrated_to_table(r.camera_b), b_true_to_table(camera_b_true), sheet(MakeSheet(spec.pattern)) {}

    // Table mm of an output sample index (render::DocumentOutputToCamera's arithmetic).
    [[nodiscard]] Point TableOf(double x, double y) const { return {frame.left_mm+(x+0.5)*frame.pitch_mm, frame.top_mm+(y+0.5)*frame.pitch_mm}; }
    [[nodiscard]] double Rho() const { return 1.0/frame.pitch_mm; }   // output px per mm
};

// ---- Truth ------------------------------------------------------------------------------------------------------------------------------
struct Displacement {
    bool covered{};            // A and B (the moved camera) both have the point inside their frames
    double dx_px{}, dy_px{};   // B minus A, output px
    double height_mm{};        // the paper height at the point A shows at the output sample
    Point surface_mm;          // that paper point (x, y)
};
inline Displacement DisplacementAt(const SceneModel& m, double x_px, double y_px) {
    Displacement d;
    const Point table = m.TableOf(x_px, y_px);
    const auto surface = SolveSurface(m.spec.field, m.geometry_a, table);
    d.height_mm = surface.height_mm;
    d.surface_mm = surface.xy_mm;
    Point raw_a{}, raw_b{}, placed{};
    const bool a_ok = render::TryProjectDocument(m.rig.camera_a, table, raw_a) && InsideFrame(raw_a);
    const bool b_projected = render::TryProjectDocument(m.camera_b_true, TablePointOfSurface(m.geometry_b_true, surface), raw_b);
    const bool placed_ok = b_projected && m.b_calibrated_to_table(raw_b.x, raw_b.y, placed);
    d.covered = a_ok && b_projected && InsideFrame(raw_b) && placed_ok;
    if (placed_ok) { d.dx_px = (placed.x-table.x)/m.frame.pitch_mm; d.dy_px = (placed.y-table.y)/m.frame.pitch_mm; }
    return d;
}

struct SeamRowTruth {
    bool covered{};
    double height_mm{};
    double dx_px{}, dy_px{};   // true parallax at (seam column, row), B minus A, output px
};
struct PlaneTruth {
    Plane plane;               // z = h0 + g_x (x - x0) + g_y (y - y0) with (x0, y0) = the seam column, middle row
    bool exact{};              // true for a planar field; otherwise the least-squares plane over the band
};
struct Truth {
    unsigned width{}, height{};        // output raster
    SeamPlacement seam;
    std::vector<double> height_map_mm; // width * height, row-major: the paper height at the point A shows at each output px (empty if not asked for)
    std::vector<SeamRowTruth> rows;    // one per output row, the seam column
    PlaneTruth surface;                // the true paper plane (design 11.3)
    cr::PoseParameters b_pose;         // the true pose of camera B
    // 11.2: rho (px/mm), b = C_B - C_A (xy, mm, the moved camera B), e_b = b/|b|, heights of the cameras.
    double rho_px_per_mm{};
    Point baseline_mm;
    double baseline_length_mm{};
    Point e_b;
    double height_a_mm{}, height_b_mm{};
    // k = rho |b| / (mean camera height - mean paper height): px of parallax per mm of paper height (design 11.2).
    [[nodiscard]] double ParallaxPerMm(double mean_paper_height_mm) const {
        return rho_px_per_mm*baseline_length_mm/(0.5*(height_a_mm+height_b_mm)-mean_paper_height_mm);
    }
};

struct TruthOptions {
    bool height_map{true};
    unsigned fit_band_half_width_px{200};   // ASSUMPTION: the columns around the seam that the plane of a non-planar field is fitted over
    unsigned fit_spacing_px{8};
};

inline PlaneTruth FitPlane(const SceneModel& m, const TruthOptions& options) {
    const auto& field = m.spec.field;
    const Point origin{m.seam.x_mm, m.seam.y_mid_mm};
    PlaneTruth truth;
    truth.plane.x0_mm = origin.x; truth.plane.y0_mm = origin.y;
    if (field.IsPlanar()) {
        const auto& p = field.plane;
        truth.plane.h0_mm = p.Height(origin.x, origin.y);
        truth.plane.slope_x = p.slope_x; truth.plane.slope_y = p.slope_y;
        truth.exact = true;
        return truth;
    }
    // Normal equations of h = a + gx u + gy v over the paper points that A shows in the band.
    double s[3][4]{};
    const unsigned x0 = m.seam.column > options.fit_band_half_width_px ? m.seam.column-options.fit_band_half_width_px : 0;
    const unsigned x1 = std::min(m.rig.raster.width_pixels-1, m.seam.column+options.fit_band_half_width_px);
    for (unsigned y = m.seam.first_row; y <= m.seam.last_row; y += options.fit_spacing_px) for (unsigned x = x0; x <= x1; x += options.fit_spacing_px) {
        const auto d = DisplacementAt(m, x, y);
        if (!d.covered) continue;
        const double row[4]{1, d.surface_mm.x-origin.x, d.surface_mm.y-origin.y, d.height_mm};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) s[i][j] += row[i]*row[j];
    }
    for (int c = 0; c < 3; ++c) {
        int pivot = c;
        for (int r = c+1; r < 3; ++r) if (std::abs(s[r][c]) > std::abs(s[pivot][c])) pivot = r;
        if (!(std::abs(s[pivot][c]) > 0)) throw std::runtime_error("parallax scene: the plane fit has no points");
        for (int j = 0; j < 4; ++j) std::swap(s[c][j], s[pivot][j]);
        for (int r = 0; r < 3; ++r) {
            if (r == c) continue;
            const double f = s[r][c]/s[c][c];
            for (int j = c; j < 4; ++j) s[r][j] -= f*s[c][j];
        }
    }
    truth.plane.h0_mm = s[0][3]/s[0][0];
    truth.plane.slope_x = s[1][3]/s[1][1];
    truth.plane.slope_y = s[2][3]/s[2][2];
    return truth;
}

inline Truth MakeTruth(const SceneModel& m, const TruthOptions& options = {}) {
    Truth t;
    t.width = m.rig.raster.width_pixels; t.height = m.rig.raster.height_pixels;
    t.seam = m.seam;
    t.b_pose = m.spec.b_pose;
    t.rho_px_per_mm = m.Rho();
    t.baseline_mm = {m.geometry_b_true.center_mm.x-m.geometry_a.center_mm.x, m.geometry_b_true.center_mm.y-m.geometry_a.center_mm.y};
    t.baseline_length_mm = std::hypot(t.baseline_mm.x, t.baseline_mm.y);
    t.e_b = {t.baseline_mm.x/t.baseline_length_mm, t.baseline_mm.y/t.baseline_length_mm};
    t.height_a_mm = m.geometry_a.height_mm; t.height_b_mm = m.geometry_b_true.height_mm;
    t.rows.resize(t.height);
    for (unsigned y = 0; y < t.height; ++y) {
        const auto d = DisplacementAt(m, m.seam.column, y);
        t.rows[y] = {d.covered && y >= m.seam.first_row && y <= m.seam.last_row, d.height_mm, d.dx_px, d.dy_px};
    }
    if (options.height_map) {
        t.height_map_mm.resize(static_cast<std::size_t>(t.width)*t.height);
        for (unsigned y = 0; y < t.height; ++y) for (unsigned x = 0; x < t.width; ++x)
            t.height_map_mm[static_cast<std::size_t>(y)*t.width+x] = SolveSurface(m.spec.field, m.geometry_a, m.TableOf(x, y)).height_mm;
    }
    t.surface = FitPlane(m, options);
    return t;
}

// ---- Images ---------------------------------------------------------------------------------------------------------------------------------
enum class Camera { a, b };
// The raw frame of a camera: each pixel shows the pattern at the paper point on its ray. A pixel with no table
// point is 0 (as the pose tests render it). Pixels are independent, so rows are dealt to `threads` threads; the
// bytes do not depend on the number of threads.
inline BgrImage RenderRaw(const SceneModel& m, Camera which, unsigned threads = 4) {
    const bool is_a = which == Camera::a;
    const auto& to_table = is_a ? m.a_to_table : m.b_true_to_table;
    const auto& geometry = is_a ? m.geometry_a : m.geometry_b_true;
    BgrImage image{kRawWidth, kRawHeight, std::vector<std::uint8_t>(static_cast<std::size_t>(kRawWidth)*kRawHeight*3)};
    threads = std::max(1u, std::min(threads, 8u));
    std::vector<std::exception_ptr> errors(threads);
    const auto rows = [&](unsigned first) {
        try {
            for (unsigned v = first; v < kRawHeight; v += threads) for (unsigned u = 0; u < kRawWidth; ++u) {
                Point table{};
                std::uint8_t value = 0;
                if (to_table(u, v, table)) {
                    const auto surface = SolveSurface(m.spec.field, geometry, table);
                    value = static_cast<std::uint8_t>(std::clamp(std::lround(m.sheet(surface.xy_mm.x, surface.xy_mm.y)), 0L, 255L));
                }
                const std::size_t at = (static_cast<std::size_t>(v)*kRawWidth+u)*3;
                image.bgr[at] = image.bgr[at+1] = image.bgr[at+2] = value;
            }
        } catch (...) { errors[first] = std::current_exception(); }
    };
    {
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(rows, t);
        rows(0);
        for (auto& th : pool) th.join();
    }
    for (const auto& e : errors) if (e) std::rethrow_exception(e);
    return image;
}

struct Scene {
    SceneSpec spec;
    BgrImage image_a, image_b;
    Truth truth;
};
struct SceneOptions {
    bool images{true};
    TruthOptions truth;
};
inline Scene MakeScene(const Rig& rig, const SeamPlacement& seam, const SceneSpec& spec, const SceneOptions& options = {}) {
    const SceneModel model(rig, seam, spec);
    Scene scene{spec, {}, {}, MakeTruth(model, options.truth)};
    if (options.images) {
        scene.image_a = RenderRaw(model, Camera::a);
        scene.image_b = RenderRaw(model, Camera::b);
    }
    return scene;
}

// ---- Closed forms for the tests (independent of the iteration above) ----------------------------------------------------------------------------
// The matrix that sends the (x, y) of a point of the plane z = a + gx x + gy y to the table point that the ray of a camera
// through it meets (homogeneous, row-major).
[[nodiscard]] inline Matrix3 CentralProjectionOfPlane(const CameraGeometry& c, double a, double gx, double gy) {
    const double h = c.height_mm;
    return {h-c.center_mm.x*gx, -c.center_mm.x*gy, -c.center_mm.x*a,
            -c.center_mm.y*gx, h-c.center_mm.y*gy, -c.center_mm.y*a,
            -gx, -gy, h-a};
}
// Design 11.3: Pi(pi), the table-to-table homography from the output point X (A's table point) to the table point B's
// ray meets after A's ray through X hit the plane: Central(C_B) * Central(C_A)^-1. `plane` uses its own (x0, y0).
[[nodiscard]] inline Matrix3 PlaneHomographyPi(const CameraGeometry& a, const CameraGeometry& b, const Plane& plane) {
    const double intercept = plane.h0_mm-plane.slope_x*plane.x0_mm-plane.slope_y*plane.y0_mm;
    const auto from_a = CentralProjectionOfPlane(a, intercept, plane.slope_x, plane.slope_y);
    const auto from_b = CentralProjectionOfPlane(b, intercept, plane.slope_x, plane.slope_y);
    return Mul(from_b, render::Invert(from_a));
}

// ---- The fields of the table of design 11.10 ----------------------------------------------------------------------------------------------------
// Lengths and heights are examples for the synthetic rig (a baseline of about 250 mm at about 700 mm, k about 0.67 px/mm), not
// product or measured values. A plane through the seam column, middle row.
[[nodiscard]] inline Plane PlaneOver(const SeamPlacement& s, double h0_mm, double tilt_x_deg, double tilt_y_deg) {
    return {h0_mm, std::tan(tilt_x_deg*kPi/180), std::tan(tilt_y_deg*kPi/180), s.x_mm, s.y_mid_mm};
}
// A fold through (seam x + distance_mm, middle row) in the given direction (0 = horizontal, 90 = vertical).
// The distance is measured along x from the seam column, so for a vertical fold it is the distance to the seam column.
[[nodiscard]] inline SceneSpec FoldScene(const SeamPlacement& s, double angle_deg, double distance_mm, double amplitude_mm = 1.5, double half_width_mm = 8.0) {
    SceneSpec spec;
    const char* orientation = angle_deg == 0 ? "horizontal" : angle_deg == 90 ? "vertical" : "diagonal";
    spec.name = std::string("fold_")+orientation+(distance_mm == 0 ? "" : "_"+std::to_string(static_cast<int>(distance_mm))+"mm");
    spec.design_row = 10;
    spec.expectation = "A fold (a ridge) is detected with its line and its distance to the seam; the excess is not missed; "
        "a fold near the seam raises fold-near-seam.";
    spec.field.creases.push_back({s.x_mm+distance_mm, s.y_mid_mm, angle_deg, amplitude_mm, half_width_mm});
    return spec;
}
[[nodiscard]] inline SceneSpec StepScene(const SeamPlacement& s, double angle_deg, double height_mm, double ramp_half_width_mm = 3.0) {
    SceneSpec spec;
    spec.name = "step_horizontal";
    spec.design_row = 10;
    spec.expectation = "A level difference across a line (a smooth ramp): the correction follows it where the ramp is wider than the hole, "
        "the gate reports the rest.";
    spec.field.steps.push_back({s.x_mm, s.y_mid_mm, angle_deg, height_mm, ramp_half_width_mm});
    return spec;
}

// The named scenes. design_row is the row of the table of design 11.10 (1-based): the table has 16 rows, of which the rows
// 1-12 describe a height field; row 13 (a narrow Omega) uses any of these, and rows 14-16 (determinism, the hole invariant,
// the quantity before the correction) are checks that run on them. A row that lists variants gives one scene per variant.
// Every scene is B at the calibrated pose except the ones named b_moved_* and the two that say so.
[[nodiscard]] inline std::vector<SceneSpec> SceneCatalog(const SeamPlacement& s) {
    std::vector<SceneSpec> out;
    const auto add = [&](std::string name, int row, std::string expectation, PatternSpec pattern, HeightField field, cr::PoseParameters pose) {
        SceneSpec spec;
        spec.name = std::move(name); spec.design_row = row; spec.expectation = std::move(expectation);
        spec.pattern = pattern; spec.field = std::move(field); spec.b_pose = pose;
        out.push_back(std::move(spec));
    };
    const PatternSpec waves{};
    const HeightField flat{};
    const auto plane = [&](double h0, double tx, double ty) { HeightField f; f.plane = PlaneOver(s, h0, tx, ty); return f; };

    add("zero", 1, "pi is about 0 and q is about 0; the composite equals the one without correction byte for byte.", waves, flat, {});
    add("flat_high", 2, "Returned as h0; the seam residual is below the target; surface-height is warned with both readings.",
        waves, plane(4.0, 0, 0), {});
    add("flat_high_outside", 2, "Returned as h0 beyond the example envelope; applied or only warned as the switch of 11.7 says.",
        waves, plane(3.0*kExampleEnvelopeHeightMm, 0, 0), {});
    add("flat_tilted", 3, "g_x and g_y are returned; B's tilt and rotation stay near 0; camera-tilt is not raised.",
        waves, plane(6.0, 1.5, -1.0), {});
    add("b_moved_baseline", 4, "Returned as a pose component; the translation along the baseline shows as h0 and warns with both readings.",
        waves, flat, {0, 0, 0, 3.0, 0, 0});
    add("b_moved_orthogonal", 4, "Returned as a pose component; the surface tilt stays near 0.", waves, flat, {0, 0, 0, 0, 3.0, 0});
    add("b_moved_rotation", 4, "Returned as a pose component; the surface tilt stays near 0.", waves, flat, {0, 0, 1.0, 0, 0, 0});
    add("b_moved_height", 4, "Returned as a pose component; the surface tilt stays near 0.", waves, flat, {0, 0, 0, 0, 0, 0.8});
    add("b_moved_tilt", 4, "Returned as a pose component; the surface tilt stays near 0.", waves, flat, {0.3, -0.2, 0, 0, 0, 0});
    add("tilt_with_b_rotation_scale", 5, "The surface tilt and B's rotation and scale come back separately (report the condition number).",
        waves, plane(5.0, 2.0, 1.0), {0, 0, 0.8, 0, 0, 0.5});
    {
        HeightField book;
        book.plane = PlaneOver(s, 20.0, -3.0, 1.5);
        book.gutters.push_back({s.x_mm+40.0, s.y_mid_mm, 90.0, 8.0, 25.0});
        add("book", 6, "Layer 1 fits the plane, the gutter band is left out of W, layer 2 follows the bend at the seam; "
            "the maximum is not reported smaller than it is.", waves, book, {});
    }
    {
        PatternSpec mixed;
        mixed.kind = PatternKind::table_and_text;
        add("periodic_and_text", 7, "The vote picks the right peak; with periodic cells alone the state is undetermined (repeating-only).",
            mixed, plane(3.0, 0, 0), {});
    }
    {
        HeightField f;
        f.undulations = Undulations(11, 3, 0.6, 80.0, 200.0);
        add("wave_long", 8, "Amplitude within the line, wavelength well above the hole: the quantity after the gate is below the target.", waves, f, {});
    }
    {
        HeightField f;
        f.undulations = Undulations(13, 3, 0.6, 8.0, 16.0);
        add("wave_short", 9, "Not predictable; the gate reports the excess correctly.", waves, f, {});
    }
    {
        HeightField f;
        f.bumps.push_back({s.x_mm, s.y_mid_mm, 1.5, 4.0});
        add("seam_bump", 9, "A bump inside the hole only: not predictable; the gate reports the excess correctly.", waves, f, {});
    }
    out.push_back(FoldScene(s, 0.0, 0.0));
    out.push_back(FoldScene(s, 35.0, 0.0));
    out.push_back(FoldScene(s, 90.0, 0.0));
    out.push_back(StepScene(s, 0.0, 1.5));
    {
        HeightField f;
        f.undulations = Undulations(11, 3, 0.6, 80.0, 200.0);
        add("wave_long_dy_error", 11, "An error orthogonal to the baseline (dy) is added: q does not change; the gate reports an excess that is not along the baseline.",
            waves, f, {0, 0, 0, 0, 2.0, 0});
    }
    {
        PatternSpec lines;
        lines.kind = PatternKind::one_directional;
        add("lines_only", 12, "Only horizontal lines at the seam: seam-dx-unmeasured is raised and the maximum says one-direction points only.",
            lines, plane(4.0, 0, 0), {});
    }
    return out;
}

// ---- The scenes of the gate's decision on what could not be measured (design 11.30) ----------------------------------------------
// Not in SceneCatalog (whose scenes stand for the rows of 11.10). B at the calibrated pose and the paper flat on the table: the true
// displacement at the seam is 0, so the value never fails and what decides is what could and could not be measured. Paper y in mm.
//   gate_mostly_blank      white paper with one band of texture across the seam: most rows of Omega show nothing in either camera
//   gate_periodic_bands    the same white paper with one band of texture and two bands of a fine grid, which the gate cannot measure
// The bands fade to white at their edges, so that the measured rows hold texture and not the band's edge as a line.
[[nodiscard]] inline std::vector<SceneSpec> GateScenes(const SeamPlacement& s) {
    std::vector<SceneSpec> out;
    const auto banded = [&](std::string name, std::string expectation, std::vector<PatternBand> bands) {
        SceneSpec spec;
        spec.name = std::move(name); spec.expectation = std::move(expectation);
        spec.pattern.kind = PatternKind::banded; spec.pattern.bands = std::move(bands); spec.pattern.period_mm = 6.0;
        out.push_back(std::move(spec));
    };
    const double y = s.y_mid_mm;
    banded("gate_mostly_blank", "Blank rows are reported and are no reason; the measured rows pass; with no trusted estimate the anchors, "
        "which cannot fit their window, leave the placement unverified.",
        {{y-30, y+30, PatternKind::two_directional, 0}});
    banded("gate_periodic_bands", "Content the gate cannot measure (a fine grid) on most of the rows with content, over the share line: not a pass.",
        {{y-80, y-60, PatternKind::two_directional, 8}, {y-20, y+10, PatternKind::periodic_grid, 8}, {y+40, y+70, PatternKind::periodic_grid, 8}});
    return out;
}
// A dark spot in one raw frame only (a mark one camera sees and the other does not): content in one camera at the seam.
inline void PaintSpot(BgrImage& image, Point raw_center, double radius_px, std::uint8_t value) {
    const int x0 = static_cast<int>(std::floor(raw_center.x-radius_px)), x1 = static_cast<int>(std::ceil(raw_center.x+radius_px));
    const int y0 = static_cast<int>(std::floor(raw_center.y-radius_px)), y1 = static_cast<int>(std::ceil(raw_center.y+radius_px));
    for (int y = std::max(0, y0); y <= std::min(static_cast<int>(image.height)-1, y1); ++y)
        for (int x = std::max(0, x0); x <= std::min(static_cast<int>(image.width)-1, x1); ++x) {
            if (std::hypot(x-raw_center.x, y-raw_center.y) > radius_px) continue;
            const std::size_t at = (static_cast<std::size_t>(y)*image.width+static_cast<std::size_t>(x))*3;
            image.bgr[at] = image.bgr[at+1] = image.bgr[at+2] = value;
        }
}

} // namespace parallax_scene
