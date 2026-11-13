// Contracts of layer 2 of the paper-surface correction (#417 S4; docs/design/fixed-composite-registration.md 11.4, 11.5,
// 11.10) and of its prediction along fold lines (C1 of 11.26). The points are made from the truth of the parallax scenes
// (tests/parallax_scene.hpp): no image is measured here. A point is the true B-minus-A displacement at a lattice position,
// minus the displacement of the scene's paper plane (layer 1 taken as exact), plus a deterministic pseudo-noise.
// ASSUMPTION: a point is the displacement at its centre, not averaged over its patch.
//
//   parallax_correction_tests                the contracts (ctest m2_parallax_correction_contracts)
//   parallax_correction_tests --only GROUP   one group of them (SceneTable, HoleInvariant, FalseLines, ..., Split)
//   parallax_correction_tests --timing       the time of the estimate per number of points and field (not a check)
//   parallax_correction_tests --sweep        the leftover per hole, band half width and lattice (not a check)
//   parallax_correction_tests --leaks [SCENE] the coupling of every gate row's pixels with the mapping's q there (not a check)
//   parallax_correction_tests --hash         a hash of the rows' bits per scene, to compare two builds (not a check)
#include "a0/m2/experimental/parallax_correction.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "parallax_scene.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace pc = a0::m2::experimental::parallax_correction;
namespace ps = parallax_scene;
namespace render = a0::m2::render;
namespace gate = a0::m2::seam_gate;
namespace cr = a0::m2::experimental::content_registration;
using render::Point;

int failures = 0;
void Check(bool ok, const std::string& label) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << label << "\n"; }
}
void CheckNear(double value, double expected, double tolerance, const std::string& label) {
    Check(std::abs(value-expected) <= tolerance, label+" ("+std::to_string(value)+" vs "+std::to_string(expected)+")");
}
template <class F> bool Throws(F f) {
    try { f(); } catch (const std::invalid_argument&) { return true; }
    return false;
}

// The target of design 11.10 for synthetic data: the gate's line minus its guard (#385: 1 px and 0.2 px).
constexpr double kTargetPx = 0.8;
// The gate of the tests: a patch of 64 px over Omega's half width of 8 px (the seam gate's calibrated patch, and the
// half width of the pose tests' geometry), at every 8th row.
constexpr std::uint32_t kGatePatch = 64, kOmega = 8, kGateRowStep = 8;
constexpr double kPointSigma = 0.05;     // the uncertainty a point reports
constexpr double kNoiseSpan = 0.10;      // uniform in [-0.05, 0.05]: sd 0.029 px

// A fixed pseudo-noise in [-0.5, 0.5) from integers (no random numbers).
double Noise(std::uint64_t a, std::uint64_t b, std::uint64_t salt) {
    std::uint64_t h = (a+0x9E3779B97F4A7C15ull)*0xBF58476D1CE4E5B9ull ^ (b+0x632BE59BD9B4E019ull)*0x94D049BB133111EBull ^ salt*0x2545F4914F6CDD1Dull;
    h ^= h >> 31; h *= 0xD6E8FEB86659FD93ull; h ^= h >> 32;
    return static_cast<double>(h >> 11)/9007199254740992.0-0.5;
}

// The scenes are evaluated on the output raster of the product's resolution (180 DPI, docs/DECISIONS.md), with the cameras and the
// region of ps::StandardRig, so that the design constants, which are output pixels, have the physical size they are meant to have
// (the generator's own raster is 48 DPI, where the same pixels are 3.75 times larger on the paper).
constexpr std::uint32_t kProductDpi = 180;
ps::Rig ProductScaleRig() {
    const auto standard = ps::StandardRig();
    const auto& region = standard.raster.region_um;
    const auto pixels = [](std::int64_t extent) {return static_cast<std::uint32_t>((extent*kProductDpi+25399)/25400);};
    return {render::OutputRaster(region, kProductDpi, pixels(region.right-region.left), pixels(region.bottom-region.top)), standard.camera_a, standard.camera_b};
}
// The seam column at the generator's seam (same table x), and the rows of it that both cameras cover.
ps::SeamPlacement PlaceSeamOn(const ps::Rig& rig, double x_mm) {
    const auto frame = cr::FrameOfRaster(rig.raster);
    ps::SeamPlacement seam;
    seam.column = static_cast<unsigned>(std::lround((x_mm-frame.left_mm)/frame.pitch_mm-0.5));
    seam.x_mm = frame.left_mm+(seam.column+0.5)*frame.pitch_mm;
    const auto map_a = render::DocumentOutputToCamera(rig.raster, rig.camera_a), map_b = render::DocumentOutputToCamera(rig.raster, rig.camera_b);
    bool any = false;
    for (unsigned y = 0; y < rig.raster.height_pixels; ++y) {
        Point ra{}, rb{};
        if (!(map_a({static_cast<double>(seam.column), static_cast<double>(y)}, ra) && map_b({static_cast<double>(seam.column), static_cast<double>(y)}, rb) &&
            ps::InsideFrame(ra) && ps::InsideFrame(rb))) continue;
        if (!any) { seam.first_row = y; any = true; }
        seam.last_row = y;
    }
    if (!any) throw std::runtime_error("the seam column is not covered");
    seam.y_mid_mm = frame.top_mm+((seam.first_row+seam.last_row)/2.0+0.5)*frame.pitch_mm;
    return seam;
}
struct Setup {
    ps::Rig rig = ProductScaleRig();
    ps::SeamPlacement seam = PlaceSeamOn(rig, ps::PlaceSeam(ps::StandardRig()).x_mm);
};
const Setup& TheSetup() {static const Setup s; return s;}

// Layer 1 is taken as exact: the paper plane is the generator's least-squares plane over the band of the generator's window
// (200 px at its 48 DPI, the same 106 mm here).
ps::TruthOptions LayerOneWindow() {
    ps::TruthOptions o;
    o.height_map = false;
    o.fit_band_half_width_px = 200*kProductDpi/48;
    o.fit_spacing_px = 16;
    return o;
}

struct BuildOptions {
    pc::ParallaxDesign design = pc::kParallaxDesignDraft;
    double noise_span = kNoiseSpan;
    std::optional<double> max_lift_mm = 1.0;
    std::optional<bool> apply_beyond_line = true;
    bool subtract_plane = true;
    unsigned thread_count = 4;   // the results do not depend on it (Determinism); 4 keeps the run short
    bool gate_remeasured = true;  // the gate's footprint counts the remeasurement variants (the strict hole)
    std::uint32_t gate_row_step = kGateRowStep;
    std::uint32_t verify_period_rows = 0, verify_phase = 0;   // the split of the gate rows (0: none)
    // A point is kept when this returns true (row, column of the output); default: all.
    std::function<bool(double, double)> keep;
    // The residual at an output position (x, y), changed for the points and the truth alike (a field the generator does not
    // make, such as a fold that ends); default: unchanged.
    std::function<Point(double, double, Point)> reshape;
    // Added to a point's measured displacement only (a measurement error; the truth is unchanged); default: none.
    std::function<Point(double, double)> measurement_error;
};

// The hole of the requests Build makes, from the footprints: the smallest |x - x_s| on a gate row and |y - y_r| on the
// seam column at which a point of the design's patch shares no pixel with the gate (what the result's summary reports).
double HoleX(const BuildOptions& o = {}) {
    const auto f = pc::GateFootprint(gate::kSeamGateDesignCalibrated, o.gate_remeasured);
    const auto p = pc::PointFootprint(0, 0, o.design.point_patch_px);
    return kOmega+static_cast<double>(f.x1-p.x0)+o.design.hole_margin_px;
}
double HoleY(const BuildOptions& o = {}) {
    pc::ParallaxRows g;
    g.gate_footprint = pc::GateFootprint(gate::kSeamGateDesignCalibrated, o.gate_remeasured);
    g.row_step = o.gate_row_step;
    const auto p = pc::PointFootprint(0, 0, o.design.point_patch_px);
    return static_cast<double>(g.gate_footprint.y1-p.y0)+o.design.hole_margin_px+static_cast<double>(pc::HoleNeighbourRows(g, o.design).below)*o.gate_row_step;
}

struct Built {
    ps::SceneSpec spec;
    pc::ParallaxRequest request;
    std::vector<double> q_true;       // per gate row: e_b . residual at the seam column
    std::vector<Point> residual_true; // per gate row
    std::vector<bool> covered;
    Point e_b;                        // the scene's own (S3)
    double k{};                       // the scene's own at the plane's height
};

Built Build(const ps::SceneSpec& spec, const BuildOptions& options = {}) {
    const auto& s = TheSetup();
    const ps::SceneModel model(s.rig, s.seam, spec);
    const auto truth = ps::MakeTruth(model, LayerOneWindow());
    const auto& plane = truth.surface.plane;
    const bool flat_plane = plane.h0_mm == 0 && plane.slope_x == 0 && plane.slope_y == 0;
    const auto pi = ps::PlaneHomographyPi(model.geometry_a, model.geometry_b_true, plane);
    const double pitch = model.frame.pitch_mm;
    // The residual after layer 1: the truth minus what the plane alone makes (11.3: G(p) Pi(pi) X - X).
    const auto residual = [&](double x, double y, bool& covered) {
        const auto d = ps::DisplacementAt(model, x, y);
        covered = d.covered;
        Point r{d.dx_px, d.dy_px};
        if (options.subtract_plane && !flat_plane) {
            const Point t = model.TableOf(x, y);
            const double w = pi[6]*t.x+pi[7]*t.y+pi[8];
            const Point moved{(pi[0]*t.x+pi[1]*t.y+pi[2])/w, (pi[3]*t.x+pi[4]*t.y+pi[5])/w};
            r.x -= (moved.x-t.x)/pitch;
            r.y -= (moved.y-t.y)/pitch;
        }
        return options.reshape ? options.reshape(x, y, r) : r;
    };

    Built b;
    b.spec = spec;
    b.e_b = truth.e_b;
    b.k = truth.ParallaxPerMm(plane.h0_mm);
    auto& r = b.request;
    r.design = options.design;
    r.max_lift_mm = options.max_lift_mm;
    r.apply_beyond_line = options.apply_beyond_line;
    r.thread_count = options.thread_count;
    r.baseline = pc::MakeBaselineGeometry(s.rig.Parameters(), spec.b_pose, plane.h0_mm);
    auto& g = r.rows;
    g.output_width = truth.width;
    g.output_height = truth.height;
    g.seam_column = s.seam.column;
    g.omega_half_width_px = kOmega;
    g.gate_footprint = pc::GateFootprint(gate::kSeamGateDesignCalibrated, options.gate_remeasured);
    g.first_row = s.seam.first_row+kGatePatch/2;
    g.row_step = options.gate_row_step;
    g.row_count = (s.seam.last_row-kGatePatch/2-g.first_row)/g.row_step+1;
    g.verify_period_rows = options.verify_period_rows;
    g.verify_phase = options.verify_phase;

    // The lattice of the design around the seam column.
    const double spacing = options.design.column_spacing_px, reach = options.design.column_radius_px;
    const double half = options.design.point_patch_px/2.0;
    const auto steps = static_cast<int>(std::floor(reach/spacing));
    const bool lines = spec.pattern.kind == ps::PatternKind::one_directional;
    const double nx = std::cos(spec.pattern.line_normal_deg*ps::kPi/180), ny = std::sin(spec.pattern.line_normal_deg*ps::kPi/180);
    for (double y = s.seam.first_row+half; y <= s.seam.last_row-half; y += spacing) {
        for (int j = -steps; j <= steps; ++j) {
            const double x = s.seam.column+j*spacing;
            if (options.keep && !options.keep(y, x)) continue;
            bool covered = false, corner = false, all = true;
            const auto d = residual(x, y, covered);
            for (const double cx : {x-half, x+half}) for (const double cy : {y-half, y+half}) { (void)residual(cx, cy, corner); all = all && corner; }
            if (!covered || !all) continue;
            pc::ParallaxPoint p;
            p.position = {x, y};
            const auto ix = static_cast<std::uint64_t>(x), iy = static_cast<std::uint64_t>(y);
            p.displacement = {d.x+options.noise_span*Noise(ix, iy, 1), d.y+options.noise_span*Noise(ix, iy, 2)};
            if (options.measurement_error) {
                const Point e = options.measurement_error(x, y);
                p.displacement.x += e.x;
                p.displacement.y += e.y;
            }
            p.uncertainty_px = kPointSigma;
            p.footprint = pc::PointFootprint(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), options.design.point_patch_px);
            if (lines) {
                p.kind = gate::PointKind::one_directional;
                Point n{nx, ny};
                if (n.x < 0 || (n.x == 0 && n.y < 0)) n = {-n.x, -n.y};   // the measurer's sign rule
                const double length = std::hypot(n.x, n.y);
                p.normal = {n.x/length, n.y/length};
            }
            r.points.push_back(p);
        }
    }
    b.q_true.resize(g.row_count);
    b.residual_true.resize(g.row_count);
    b.covered.resize(g.row_count);
    for (std::uint32_t i = 0; i < g.row_count; ++i) {
        bool covered = false;
        const auto d = residual(g.seam_column, g.first_row+static_cast<double>(i)*g.row_step, covered);
        b.residual_true[i] = d;
        b.q_true[i] = truth.e_b.x*d.x+truth.e_b.y*d.y;
        b.covered[i] = covered;
    }
    return b;
}

struct Outcome {
    double before{}, after{};
    std::size_t rows{};
};
// Along the baseline at the seam column, over the gate rows: before = |q_true|, after = |q_true - q applied|.
Outcome Evaluate(const Built& b, const pc::ParallaxResult& result, const std::function<bool(double)>& row_filter = {}) {
    Outcome o;
    for (std::size_t i = 0; i < b.q_true.size(); ++i) {
        if (!b.covered[i]) continue;
        const double y = b.request.rows.first_row+static_cast<double>(i)*b.request.rows.row_step;
        if (row_filter && !row_filter(y)) continue;
        const double applied = result.Applied() ? result.rows[i].q : 0.0;
        o.before = std::max(o.before, std::abs(b.q_true[i]));
        o.after = std::max(o.after, std::abs(b.q_true[i]-applied));
        ++o.rows;
    }
    return o;
}

const ps::SceneSpec& SceneNamed(const std::string& name) {
    static const auto catalog = ps::SceneCatalog(TheSetup().seam);
    for (const auto& s : catalog) if (s.name == name) return s;
    throw std::runtime_error("no scene "+name);
}

std::string RowReasons(const pc::ParallaxResult& r) {
    std::vector<std::pair<std::string, int>> counts;
    for (const auto& row : r.rows) {
        if (row.state == pc::ParallaxRowState::measured) continue;
        auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) {return c.first == row.reason;});
        if (it == counts.end()) counts.push_back({row.reason, 1}); else ++it->second;
    }
    std::string out;
    for (const auto& [reason, n] : counts) out += (out.empty() ? "" : ",")+reason+"="+std::to_string(n);
    return out.empty() ? "-" : out;
}
std::string Reasons(const pc::ParallaxResult& r) {
    std::string out;
    for (const auto& s : r.reasons) out += (out.empty() ? "" : ",")+s;
    return out.empty() ? "-" : out;
}

void PrintRow(const std::string& name, const Built& b, const pc::ParallaxResult& r, const Outcome& o, const std::string& verdict) {
    std::printf("| %-28s | %6zu | %6.3f | %6.3f | %-11s | %-20s | %3u/%3u | %-34s | %s |\n", name.c_str(), b.request.points.size(), o.before, o.after,
        std::string(pc::ParallaxStateCode(r.state)).c_str(), Reasons(r).c_str(), r.summary.rows_measured, r.summary.rows_filled, RowReasons(r).c_str(),
        verdict.c_str());
}

// ---- 11.10 and C1 of 11.26: the scenes of layer 2 -------------------------------------------------------------------------------
enum class Expect {
    corrected,    // after <= the target on every row
    left,         // before and after above the target: the excess stays for the gate to report
    split,        // the rows predicted along a fold line are corrected; the rows the result marks unpredictable keep the excess
};
const char* ExpectCode(Expect e) {return e == Expect::corrected ? "corrected" : e == Expect::left ? "left" : "split";}

// A scene with the long waves of wave_long added (a fold on a drawing that also undulates).
ps::SceneSpec WithLongWaves(ps::SceneSpec spec, const std::string& name) {
    spec.name = name;
    for (const auto& u : SceneNamed("wave_long").field.undulations) spec.field.undulations.push_back(u);
    return spec;
}

// The largest |q_true - q applied| over the covered rows, by what C1 did to the row.
struct FoldRows {
    double predicted{}, unpredictable{}, other{};
    std::uint32_t over_unmarked{};     // rows over the target that the result does not mark unpredictable
    std::uint32_t unpredictable_moved{};   // unpredictable rows whose q is not 11.4's
    std::string reasons;
};
FoldRows ByFold(const Built& b, const pc::ParallaxResult& r) {
    FoldRows f;
    for (std::size_t i = 0; i < b.q_true.size(); ++i) {
        if (!b.covered[i]) continue;
        const auto& row = r.rows[i];
        const double e = std::abs(b.q_true[i]-(r.Applied() ? row.q : 0.0));
        if (row.fold == pc::ParallaxRowFold::predicted) f.predicted = std::max(f.predicted, e);
        else if (row.fold == pc::ParallaxRowFold::unpredictable) {
            f.unpredictable = std::max(f.unpredictable, e);
            if (f.reasons.find(row.fold_reason) == std::string::npos) f.reasons += (f.reasons.empty() ? "" : ",")+row.fold_reason;
            if (row.state == pc::ParallaxRowState::measured && row.q != row.q_isotropic) ++f.unpredictable_moved;
        } else f.other = std::max(f.other, e);
        if (e > kTargetPx && row.fold != pc::ParallaxRowFold::unpredictable) ++f.over_unmarked;
    }
    return f;
}

void SceneTable() {
    const auto& s = TheSetup();
    const double pitch = cr::FrameOfRaster(s.rig.raster).pitch_mm;
    const pc::ParallaxDesign& d = pc::kParallaxDesignDraft;
    const double gx_mm = HoleX()*pitch;
    struct Case {
        std::string name;
        ps::SceneSpec spec;
        Expect expect;
    };
    std::vector<Case> cases{
        {"zero", SceneNamed("zero"), Expect::corrected},
        {"wave_long", SceneNamed("wave_long"), Expect::corrected},
        {"wave_long_dy_error", SceneNamed("wave_long_dy_error"), Expect::corrected},
        {"wave_short", SceneNamed("wave_short"), Expect::left},
        {"seam_bump", SceneNamed("seam_bump"), Expect::left},
        {"fold_horizontal", SceneNamed("fold_horizontal"), Expect::corrected},
        {"fold_diagonal", SceneNamed("fold_diagonal"), Expect::corrected},
        // Along the seam column: the rows with points along the line on both sides are predicted; the rows at the ends of the
        // column (the line's points on one side only) are left. The hole holds the gate rows the mapping's join couples (8 rows
        // each side with the gate rows of these tests, 8 px apart), so the line must reach farther past it: half g_x from the
        // seam column the ends keep a little over the target too.
        {"fold_vertical", SceneNamed("fold_vertical"), Expect::split},
        {"fold_vertical_half_gx", ps::FoldScene(s.seam, 90.0, gx_mm/2), Expect::split},
        {"fold_vertical_2gx", ps::FoldScene(s.seam, 90.0, 2*gx_mm), Expect::corrected},
        {"fold_vertical_band_edge", ps::FoldScene(s.seam, 90.0, (d.column_radius_px-d.column_spacing_px)*pitch), Expect::corrected},
        {"fold_diagonal_2gx", ps::FoldScene(s.seam, 35.0, 2*gx_mm), Expect::corrected},
        {"step_horizontal", SceneNamed("step_horizontal"), Expect::corrected},
        // Off the line search's angles (half a step off), narrower than the scenes' folds, and with the long waves.
        {"fold_2.5deg", ps::FoldScene(s.seam, 2.5, 0.0), Expect::corrected},
        {"fold_37.5deg", ps::FoldScene(s.seam, 37.5, 0.0), Expect::corrected},
        {"fold_87.5deg", ps::FoldScene(s.seam, 87.5, 0.0), Expect::split},
        {"fold_horizontal_narrow", ps::FoldScene(s.seam, 0.0, 0.0, 1.5, 4.0), Expect::corrected},
        {"fold_37.5deg_narrow", ps::FoldScene(s.seam, 37.5, 0.0, 1.5, 4.0), Expect::corrected},
        {"fold_horizontal+waves", WithLongWaves(SceneNamed("fold_horizontal"), "fold_horizontal+waves"), Expect::corrected},
        {"fold_diagonal+waves", WithLongWaves(SceneNamed("fold_diagonal"), "fold_diagonal+waves"), Expect::corrected},
        {"fold_vertical+waves", WithLongWaves(SceneNamed("fold_vertical"), "fold_vertical+waves"), Expect::split},
    };
    std::printf("| scene | points | before px | after px | 11.4 alone px | rows predicted/agree/unpredictable | predicted px | unpredictable px (why) | verdict |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|\n");
    for (const auto& c : cases) {
        const auto b = Build(c.spec);
        const auto r = pc::EstimateParallax(b.request);
        const auto o = Evaluate(b, r);
        double isotropic = 0;
        for (std::size_t i = 0; i < b.q_true.size(); ++i) if (b.covered[i]) isotropic = std::max(isotropic, std::abs(b.q_true[i]-r.rows[i].q_isotropic));
        const auto f = ByFold(b, r);
        const bool corrected = o.after <= kTargetPx;
        const bool left = o.before > kTargetPx && o.after > kTargetPx;
        const bool split = r.summary.rows_fold_predicted > 0 && f.predicted <= kTargetPx && r.summary.rows_fold_unpredictable > 0 && f.unpredictable > kTargetPx;
        const bool ok = c.expect == Expect::corrected ? corrected : c.expect == Expect::left ? left : split;
        std::printf("| %-26s | %5zu | %6.3f | %6.3f | %6.3f | %3u/%3u/%3u | %6.3f | %6.3f %s | %s%s |\n", c.name.c_str(), b.request.points.size(), o.before, o.after,
            isotropic, r.summary.rows_fold_predicted, r.summary.rows_fold_agree, r.summary.rows_fold_unpredictable, f.predicted, f.unpredictable,
            f.reasons.empty() ? "" : ("("+f.reasons+")").c_str(), ExpectCode(c.expect), ok ? "" : " MISMATCH");
        Check(r.Applied(), c.name+": layer 2 is applied");
        Check(ok, c.name+": the scene falls on its side of the table (corrected below the target, the excess left for the gate, or split by the rows' marks)");
        Check(o.rows > 20, c.name+": enough covered gate rows");
        // What is not reached is said: with a line found, every row over the target is marked unpredictable, and those rows keep
        // 11.4's estimate.
        if (!r.folds.empty()) Check(f.over_unmarked == 0, c.name+": every row over the target is marked unpredictable ("+std::to_string(f.over_unmarked)+" are not)");
        Check(f.unpredictable_moved == 0, c.name+": the unpredictable rows with an estimate of their own keep 11.4's");
    }
    // The step: rows far from the step line are corrected; the rows near it keep the excess.
    {
        const auto b = Build(SceneNamed("step_horizontal"));
        const auto r = pc::EstimateParallax(b.request);
        const double step_row = (s.seam.y_mid_mm-cr::FrameOfRaster(s.rig.raster).top_mm)/pitch-0.5;
        const double near = d.kernel_reach_sigmas*d.kernel_sigma_y_px+HoleY();
        const auto far = Evaluate(b, r, [&](double y) {return std::abs(y-step_row) > near;});
        std::printf("| %-28s | %6zu | %6.3f | %6.3f | (rows farther than %.0f px from the step) |\n", "step_horizontal far rows", b.request.points.size(),
            far.before, far.after, near);
        Check(far.rows > 10 && far.after <= kTargetPx, "step: rows away from the step line are corrected");
    }
    // A line across the baseline only: nothing measures the component along it.
    {
        const auto b = Build(SceneNamed("lines_only"));
        const auto r = pc::EstimateParallax(b.request);
        const auto o = Evaluate(b, r);
        PrintRow("lines_only", b, r, o, "state");
        Check(r.state == pc::ParallaxState::not_applied && !r.reasons.empty() && r.reasons.front() == "baseline-unmeasured",
            "lines only: not applied, baseline-unmeasured");
        bool rows_ok = r.summary.points_baseline == 0 && !r.rows.empty();
        for (const auto& row : r.rows) rows_ok = rows_ok && row.state == pc::ParallaxRowState::filled && row.reason == "baseline-unmeasured" && row.q == 0;
        Check(rows_ok, "lines only: every row is filled for baseline-unmeasured, with q 0");
    }
}

// The orthogonal error (dy) does not move q beyond its own share along e_b.
void OrthogonalError() {
    const auto clean = Build(SceneNamed("wave_long"));
    const auto shifted = Build(SceneNamed("wave_long_dy_error"));
    const auto a = pc::EstimateParallax(clean.request), b = pc::EstimateParallax(shifted.request);
    Check(a.rows.size() == b.rows.size(), "dy error: the same rows");
    double worst = 0, across = 0;
    for (std::size_t i = 0; i < a.rows.size() && i < b.rows.size(); ++i) {
        if (!clean.covered[i]) continue;
        const Point delta{shifted.residual_true[i].x-clean.residual_true[i].x, shifted.residual_true[i].y-clean.residual_true[i].y};
        const double along = shifted.e_b.x*delta.x+shifted.e_b.y*delta.y;
        across = std::max(across, std::abs(-shifted.e_b.y*delta.x+shifted.e_b.x*delta.y));
        worst = std::max(worst, std::abs((b.rows[i].q-a.rows[i].q)-along));
    }
    Check(across > 2.0, "dy error: the injected error is mostly across the baseline");
    Check(worst <= 0.05, "dy error: q moves only by the injected error's share along e_b ("+std::to_string(worst)+" px)");
}

// Zero displacements give q exactly 0 on every row (the mapping with q = 0 is the one without layer 2, 11.13). The generator's
// h = 0 truth is 0 up to the rounding of its round trip, so the points are set to 0 here.
void ZeroIsExact() {
    auto b = Build(SceneNamed("zero"));
    for (auto& p : b.request.points) p.displacement = {0, 0};
    const auto r = pc::EstimateParallax(b.request);
    bool zero = r.Applied() && !r.rows.empty();
    for (const auto& row : r.rows) zero = zero && row.q == 0.0 && row.lift_mm == 0.0;
    Check(zero, "h = 0: q is exactly 0 on every row");
    Check(r.summary.max_shift_px == 0 && r.summary.max_strain == 0, "h = 0: nothing moves");
}

// e_b and k against the scene's own geometry.
void Baseline() {
    const auto& s = TheSetup();
    for (const char* name : {"zero", "b_moved_baseline", "b_moved_tilt", "flat_high"}) {
        const auto& spec = SceneNamed(name);
        const ps::SceneModel model(s.rig, s.seam, spec);
        const auto truth = ps::MakeTruth(model, LayerOneWindow());
        const auto baseline = pc::MakeBaselineGeometry(s.rig.Parameters(), spec.b_pose, truth.surface.plane.h0_mm);
        CheckNear(baseline.direction.x, truth.e_b.x, 1e-9, std::string(name)+": e_b.x");
        CheckNear(baseline.direction.y, truth.e_b.y, 1e-9, std::string(name)+": e_b.y");
        CheckNear(baseline.length_mm, truth.baseline_length_mm, 1e-6, std::string(name)+": |b|");
        CheckNear(baseline.k_px_per_mm, truth.ParallaxPerMm(truth.surface.plane.h0_mm), 1e-9, std::string(name)+": k");
        CheckNear(baseline.pixels_per_mm, truth.rho_px_per_mm, 1e-12, std::string(name)+": rho");
    }
    Check(Throws([&] {(void)pc::MakeBaselineGeometry(s.rig.Parameters(), {}, std::nan(""));}), "baseline: a non-finite paper height is refused");
    Check(Throws([&] {(void)pc::MakeBaselineGeometry(s.rig.Parameters(), {}, 1e6);}), "baseline: paper above the cameras is refused");
}

// ---- The hole (11.5): a row's estimate never uses a point that shares pixels with that row's gate points ----------------------------
namespace rg = a0::m2::experimental::registration_gate;
// The pixels the gate's point at (x, y) reads, from the gate lane's own function (GatePatchOf), widened by margin on every side:
// on a gate point remeasured both ways (the most it reads) or on one not remeasured.
rg::GatePatch GatePixels(std::uint32_t x, std::uint32_t y, double margin, bool remeasured = true) {
    gate::SeamPoint p;
    p.x = x;
    p.y = y;
    if (remeasured) {
        gate::Remeasurement shifted, enlarged;
        shifted.variant = gate::Variant::shifted;
        enlarged.variant = gate::Variant::enlarged;
        p.remeasurements = {shifted, enlarged};
    }
    const auto patch = rg::GatePatchOf(p, gate::kSeamGateDesignCalibrated);
    const auto m = static_cast<std::int64_t>(std::ceil(margin));
    return {patch.x0-m, patch.y0-m, patch.x1+m, patch.y1+m};
}
// Whether a request's gate footprint is the one with the remeasurement (the requests here take one of the two).
bool RemeasuredFootprint(const pc::ParallaxRows& g) {
    for (const bool remeasured : {true, false}) {
        const auto f = GatePixels(1000, 1000, 0, remeasured);
        if (g.gate_footprint.x0 == f.x0-1000 && g.gate_footprint.y0 == f.y0-1000 && g.gate_footprint.x1 == f.x1-1000 && g.gate_footprint.y1 == f.y1-1000)
            return remeasured;
    }
    throw std::runtime_error("the request's gate footprint is neither of the gate lane's");
}
// Whether a point shares a pixel with any gate point of the row (every column of Omega), the gate's pixels widened by margin.
bool SharesWithRow(const pc::ParallaxRows& g, std::uint32_t row_y, const pc::ParallaxPoint& p, double margin) {
    const bool remeasured = RemeasuredFootprint(g);
    for (std::uint32_t x = g.seam_column-g.omega_half_width_px; x <= g.seam_column+g.omega_half_width_px; ++x)
        if (!rg::PatchesShareNoPixel(GatePixels(x, row_y, margin, remeasured), p.footprint)) return true;
    return false;
}

// The gate rows that read a q gate row j's estimate enters, from the mapping's own join (S1's ParallaxShiftAtRow), not from the
// lane's formula: on samples that rise at every row (so every tangent reads its neighbours), the pixel rows at which a change of
// sample j changes q(y); a gate row is coupled when a row its points read is one of them. Gate rows [first, last].
struct Coupled {
    std::size_t first{}, last{};
};
Coupled CoupledRows(const pc::ParallaxRows& g, std::size_t j) {
    namespace m2x = a0::m2::experimental;
    m2x::ParallaxProfile before;
    before.baseline = {1, 0};
    before.first_row_px = g.first_row;
    before.row_spacing_px = g.row_step;
    before.a_px = 8;
    before.l_px = 192;
    before.seam_column = g.seam_column;
    for (std::size_t i = 0; i < g.row_count; ++i) before.q_px.push_back(0.01*static_cast<double>(i*i)+0.1*static_cast<double>(i));
    auto after = before;
    after.q_px[j] += 0.05;   // the samples still rise
    const auto f = GatePixels(1000, 1000, 0, RemeasuredFootprint(g));
    Coupled c{g.row_count, 0};
    for (std::size_t i = 0; i < g.row_count; ++i) {
        const auto y = static_cast<std::int64_t>(g.first_row+i*g.row_step);
        bool coupled = false;
        for (auto r = y+f.y0-1000; r < y+f.y1-1000 && !coupled; ++r)
            coupled = m2x::ParallaxShiftAtRow(after, static_cast<double>(r)) != m2x::ParallaxShiftAtRow(before, static_cast<double>(r));
        if (!coupled) continue;
        c.first = std::min(c.first, i);
        c.last = std::max(c.last, i);
    }
    if (c.first > c.last) throw std::runtime_error("no gate row reads the sample");
    return c;
}
// The gate rows a row's hole must cover: the join's, and hole_neighbour_rows on each side.
Coupled RequiredRows(const pc::ParallaxRequest& request, std::size_t row) {
    const auto& g = request.rows;
    const std::size_t k = request.design.hole_neighbour_rows;
    auto c = CoupledRows(g, row);
    c.first = std::min(c.first, row > k ? row-k : 0);
    c.last = std::max(c.last, std::min<std::size_t>(g.row_count-1, row+k));
    return c;
}
// The pixels every gate point of a gate row may read (Omega's columns), widened by margin: one rectangle, the columns' patches
// overlapping.
rg::GatePatch RowPixels(const pc::ParallaxRows& g, std::size_t row, double margin) {
    const bool remeasured = RemeasuredFootprint(g);
    const auto y = static_cast<std::uint32_t>(g.first_row+row*g.row_step);
    const auto left = GatePixels(g.seam_column-g.omega_half_width_px, y, margin, remeasured), right = GatePixels(g.seam_column+g.omega_half_width_px, y, margin, remeasured);
    return {left.x0, left.y0, right.x1, right.y1};
}
bool SharesWithRows(const pc::ParallaxRows& g, const Coupled& rows, const pc::ParallaxPoint& p, double margin) {
    for (std::size_t i = rows.first; i <= rows.last; ++i) if (!rg::PatchesShareNoPixel(RowPixels(g, i, margin), p.footprint)) return true;
    return false;
}
struct HoleCount {
    std::size_t sharing{}, sharing_used{}, clear_outside{}, clear_with_hole_one{};
};
// A row's weights against the gate rows its hole must cover (RequiredRows): a point that shares a pixel with one of them has no
// weight and S = 0; a point clear of all of them by more than the hole's edge has S = 1 (the hole is not larger than that).
void CountHole(const pc::ParallaxRequest& request, std::size_t row, const std::vector<pc::ParallaxPointWeight>& weights, HoleCount& c) {
    const auto& g = request.rows;
    const auto required = RequiredRows(request, row);
    for (const auto& w : weights) {
        const auto& p = request.points[w.point];
        if (SharesWithRows(g, required, p, request.design.hole_margin_px)) {
            ++c.sharing;
            if (w.weight != 0 || w.hole != 0) ++c.sharing_used;
            continue;
        }
        if (!SharesWithRows(g, required, p, request.design.hole_margin_px+request.design.hole_edge_px)) {
            ++c.clear_outside;
            if (w.hole == 1) ++c.clear_with_hole_one;
        }
    }
}
void ReportHole(const HoleCount& c, const std::string& label) {
    Check(c.sharing > 0, label+": some points share pixels with a gate row the hole must cover (the check sees the hole)");
    Check(c.sharing_used == 0, label+": no point that shares a pixel with a gate row the join couples to the row has a weight ("+
        std::to_string(c.sharing_used)+" of "+std::to_string(c.sharing)+")");
    Check(c.clear_outside > 0 && c.clear_with_hole_one == c.clear_outside, label+": points clear of those gate rows by the hole's edge have S = 1 ("+
        std::to_string(c.clear_with_hole_one)+" of "+std::to_string(c.clear_outside)+")");
}
void CheckHoleInvariant(const pc::ParallaxRequest& request, const std::string& label) {
    HoleCount c;
    for (std::size_t row = 0; row < request.rows.row_count; row += 3) CountHole(request, row, pc::ParallaxRowWeights(request, row), c);
    ReportHole(c, label);
}
// The same on the rows predicted along a fold line (C1), with the regression along the line each used; and a predicted row's q
// is that regression's, bit for bit (so its weights are the ones checked).
void CheckHoleInvariantAlongLines(const pc::ParallaxRequest& request, const std::string& label) {
    const auto result = pc::EstimateParallax(request);
    HoleCount c;
    std::size_t predicted = 0, checked = 0;
    bool same = true;
    for (std::size_t row = 0; row < result.rows.size(); ++row) {
        const auto& r = result.rows[row];
        if (r.fold != pc::ParallaxRowFold::predicted || predicted++ % 3 != 0) continue;
        const auto fit = pc::ParallaxRowFitOf(request, row, r.fold_angle_deg);
        same = same && fit.measured && fit.q == r.q && fit.sigma_q == r.sigma_q && fit.n_eff == r.n_eff && r.state == pc::ParallaxRowState::measured;
        CountHole(request, row, fit.weights, c);
        ++checked;
    }
    Check(checked > 0, label+": rows are predicted along a line");
    Check(same, label+": a predicted row's q, sigma_q and n_eff are its regression along the line's, bit for bit");
    ReportHole(c, label);
}
void HoleInvariant() {
    const auto b = Build(SceneNamed("wave_long"));
    CheckHoleInvariant(b.request, "hole, design patch");
    // Points that were remeasured in the band's measurement (their footprint is the larger patch moved both ways), and a wider Omega.
    auto mixed = b.request;
    for (std::size_t i = 0; i < mixed.points.size(); i += 3) {
        const auto& p = mixed.points[i];
        mixed.points[i].footprint = GatePixels(static_cast<std::uint32_t>(p.position.x), static_cast<std::uint32_t>(p.position.y), 0);
    }
    mixed.rows.omega_half_width_px = 24;
    CheckHoleInvariant(mixed, "hole, remeasured points and a wider Omega");
    // With more rows on each side than the join's (hole_neighbour_rows), the hole covers them too.
    const std::uint32_t beyond = std::max(pc::HoleNeighbourRows(b.request.rows, b.request.design).above, pc::HoleNeighbourRows(b.request.rows, b.request.design).below)+2;
    auto neighbours = b.request;
    neighbours.design.hole_neighbour_rows = beyond;
    CheckHoleInvariant(neighbours, "hole, two rows beyond the join's");
    {
        const auto rows = pc::HoleNeighbourRows(neighbours.rows, neighbours.design);
        Check(rows.above == beyond && rows.below == beyond, "hole: hole_neighbour_rows beyond the join's sets the rows");
    }
    // The rows predicted along a fold line (C1): the same hole, on the regression along the line.
    for (const char* name : {"fold_horizontal", "fold_diagonal", "fold_vertical", "step_horizontal"})
        CheckHoleInvariantAlongLines(Build(SceneNamed(name)).request, std::string("hole along the line, ")+name);
    {
        auto along = Build(SceneNamed("fold_diagonal")).request;
        for (std::size_t i = 0; i < along.points.size(); i += 3) {
            const auto& p = along.points[i];
            along.points[i].footprint = GatePixels(static_cast<std::uint32_t>(p.position.x), static_cast<std::uint32_t>(p.position.y), 0);
        }
        along.rows.omega_half_width_px = 24;
        CheckHoleInvariantAlongLines(along, "hole along the line, remeasured points and a wider Omega");
        auto wide = Build(SceneNamed("fold_horizontal")).request;
        wide.design.hole_neighbour_rows = beyond;
        CheckHoleInvariantAlongLines(wide, "hole along the line, two rows beyond the join's");
    }

    // The hole's edge, exactly: a point at g_x has S = 0, at g_x + delta has S = 1 (a point of the gate row).
    const auto& d = b.request.design;
    const auto& g = b.request.rows;
    const double gx = HoleX();
    pc::ParallaxRequest edge = b.request;
    edge.points.clear();
    const auto y = g.first_row;
    for (const double dx : {gx-1, gx, gx+d.hole_edge_px/2, gx+d.hole_edge_px}) {
        pc::ParallaxPoint p;
        const auto x = static_cast<std::uint32_t>(g.seam_column+dx);
        p.position = {static_cast<double>(x), static_cast<double>(y)};
        p.uncertainty_px = kPointSigma;
        p.footprint = pc::PointFootprint(x, y, d.point_patch_px);
        edge.points.push_back(p);
    }
    const auto weights = pc::ParallaxRowWeights(edge, 0);
    std::vector<double> hole(4, -1);
    for (const auto& w : weights) hole[w.point] = w.hole;
    CheckNear(hole[0], 0, 0, "hole edge: inside the hole");
    CheckNear(hole[1], 0, 0, "hole edge: at g_x");
    CheckNear(hole[2], 0.5, 1e-12, "hole edge: half way through delta");
    CheckNear(hole[3], 1, 0, "hole edge: at g_x + delta");
    Check(SharesWithRow(g, y, edge.points[0], d.hole_margin_px) && !SharesWithRow(g, y, edge.points[1], d.hole_margin_px),
        "hole edge: g_x is where the gate lane's patches stop sharing pixels");
    const auto result = pc::EstimateParallax(b.request);
    CheckNear(result.summary.hole_x_px, gx, 0, "the reported hole is g_x");
    BuildOptions primary;
    primary.gate_remeasured = false;
    std::printf("\nhole for a point of the design's patch: g_x %.0f px, g_y %.0f px (gate footprint with the remeasurement); "
        "without the remeasurement g_x %.0f px, g_y %.0f px\n", result.summary.hole_x_px, result.summary.hole_y_px, HoleX(primary), HoleY(primary));
}

// A large error confined to the pixels the gate reads on a row does not reach that row's q (the gate stays independent), while
// the same error next to the hole does. The mapping joins the rows' q (S1's ParallaxShiftAtRow), so a gate patch, which spans
// rows, also sees its neighbours' q: that coupling is measured here (the largest change of the mapping's q over the rows the
// gate point reads), and the hole's rows of the join remove it.
double InterpolationLeak(const pc::ParallaxRequest& request, const a0::m2::experimental::ParallaxProfile& before, std::size_t row, double error_px,
    const Point& e_b) {
    const auto& g = request.rows;
    const auto y = static_cast<std::uint32_t>(g.first_row+row*g.row_step);
    auto spoiled = request;
    for (auto& p : spoiled.points) {
        if (!SharesWithRow(g, y, p, request.design.hole_margin_px)) continue;
        p.displacement.x += error_px*e_b.x;
        p.displacement.y += error_px*e_b.y;
    }
    const auto after = pc::ToParallaxProfile(pc::EstimateParallax(spoiled));
    const auto f = GatePixels(g.seam_column, y, 0, RemeasuredFootprint(g));
    double leak = 0;
    for (auto r = f.y0; r < f.y1; ++r)
        leak = std::max(leak, std::abs(a0::m2::experimental::ParallaxShiftAtRow(after, static_cast<double>(r))-
            a0::m2::experimental::ParallaxShiftAtRow(before, static_cast<double>(r))));
    return leak;
}
double InterpolationLeak(const pc::ParallaxRequest& request, std::size_t row, double error_px, const Point& e_b) {
    return InterpolationLeak(request, pc::ToParallaxProfile(pc::EstimateParallax(request)), row, error_px, e_b);
}
void GateIndependence() {
    const auto b = Build(SceneNamed("wave_long"));
    const auto base = pc::EstimateParallax(b.request);
    const auto& g = b.request.rows;
    const std::size_t row = g.row_count/2;
    const auto y = static_cast<std::uint32_t>(g.first_row+row*g.row_step);
    auto spoiled = b.request;
    std::size_t touched = 0;
    for (auto& p : spoiled.points) {
        if (!SharesWithRow(g, y, p, b.request.design.hole_margin_px)) continue;
        p.displacement.x += 3*b.e_b.x;
        p.displacement.y += 3*b.e_b.y;
        ++touched;
    }
    const auto r = pc::EstimateParallax(spoiled);
    Check(touched > 0, "independence: the error is placed on the gate's pixels of the row");
    Check(r.rows[row].q == base.rows[row].q, "independence: q of the row is unchanged, bit for bit");
    // Next to the hole (outside it), the same error moves q.
    auto next = b.request;
    const double gx = HoleX();
    for (auto& p : next.points) {
        if (std::abs(p.position.y-y) > b.request.design.column_spacing_px/2 || std::abs(p.position.x-g.seam_column) < gx+b.request.design.hole_edge_px ||
            std::abs(p.position.x-g.seam_column) > gx+3*b.request.design.column_spacing_px) continue;
        p.displacement.x += 0.3*b.e_b.x;
        p.displacement.y += 0.3*b.e_b.y;
    }
    Check(pc::EstimateParallax(next).rows[row].q != base.rows[row].q, "independence: points next to the hole do count");

    // The coupling through the mapping's join of the rows, with the gate rows of the gate's design (32 px apart): an error of 3 px on
    // one gate row's pixels, the largest change of the mapping's q over the rows that gate point reads. The design's hole has the
    // join's rows (HoleJoin has the row steps and footprints); the row's own footprint alone leaks.
    BuildOptions product;
    product.gate_row_step = gate::kSeamGateDesignCalibrated.point_spacing_px;
    const auto p0 = Build(SceneNamed("wave_long"), product);
    auto own = p0.request;
    own.design.hole_join_reach_rows = 0;
    const double leak_own = InterpolationLeak(own, own.rows.row_count/2, 3.0, p0.e_b);
    const double leak = InterpolationLeak(p0.request, p0.request.rows.row_count/2, 3.0, p0.e_b);
    const auto rows = pc::HoleNeighbourRows(p0.request.rows, p0.request.design);
    std::printf("coupling through the join of the rows (3 px on one gate row's pixels, gate rows every %u px): hole of the row's own footprint "
        "%.3f px; the design's hole (%u rows above, %u below) %.3f px\n", product.gate_row_step, leak_own, rows.above, rows.below, leak);
    Check(leak_own > 0, "independence: the row's own footprint alone lets the join carry the error (the check sees the coupling)");
    Check(leak == 0, "independence: with the join's rows in the hole, the rows a gate point reads see none of the error");
}

// ---- The hole's rows and the mapping's join (11.5) ----------------------------------------------------------------------------------------
// The join's reach: on samples that rise at every row, a change of one sample changes S1's q(y) exactly for |y - y_j| < r row
// steps, r = the design's hole_join_reach_rows (2).
void JoinReach() {
    namespace m2x = a0::m2::experimental;
    const std::uint32_t r = pc::kParallaxDesignDraft.hole_join_reach_rows;
    for (const std::uint32_t step : {8u, 16u, 32u}) {
        m2x::ParallaxProfile before;
        before.baseline = {1, 0};
        before.first_row_px = 40;
        before.row_spacing_px = step;
        before.a_px = 8;
        before.l_px = 192;
        before.seam_column = 100;
        for (std::size_t i = 0; i < 24; ++i) before.q_px.push_back(0.01*static_cast<double>(i*i)+0.1*static_cast<double>(i));
        const std::size_t j = 12;
        auto after = before;
        after.q_px[j] += 0.05;
        const auto yj = static_cast<long long>(before.first_row_px)+static_cast<long long>(j*step);
        const auto reach = static_cast<long long>(r)*step;
        long long first = 0, last = -1;
        bool exact = true;
        for (long long y = 0; y < static_cast<long long>(before.first_row_px)+24ll*step+64; ++y) {
            const bool changed = m2x::ParallaxShiftAtRow(after, static_cast<double>(y)) != m2x::ParallaxShiftAtRow(before, static_cast<double>(y));
            // Within the reach every row changes but the other samples' own rows (q passes through each sample).
            const long long d = y-yj;
            exact = exact && changed == (std::abs(d) < reach && (d == 0 || d % static_cast<long long>(step) != 0));
            if (!changed) continue;
            if (last < first) first = y;
            last = y;
        }
        std::printf("join of the rows every %u px: a change of the sample at row %lld changes q(y) on rows %lld..%lld (not on the other samples' rows)\n",
            step, yj, first, last);
        Check(exact && first == yj-reach+1 && last == yj+reach-1,
            "join: a sample enters q(y) exactly for |y - y_j| < r row steps (r = "+std::to_string(r)+", rows every "+std::to_string(step)+" px)");
    }
}

// The hole covers every gate row that reads a q the row's estimate enters, and no more, whatever the row step and the footprint:
// for gate rows every 8, 16 and 32 px and the gate's footprint with and without the remeasurement,
//   - the rows HoleNeighbourRows gives are the extent of the coupled rows found from S1's function (CoupledRows);
//   - no point with a weight shares a pixel with a coupled gate row, and points clear of them by the hole's edge have S = 1, on
//     11.4's rows and on the rows predicted along a fold line (C1);
//   - an error of 3 px or of 0.2 px (below Tukey's cut, so the regression would keep it) on one gate row's pixels changes no q of the
//     mapping over the rows that gate point reads, on a smooth field and at the row C1 moves most on a fold across the seam.
void HoleJoin() {
    std::printf("\n| gate footprint | row step px | hole rows above/below | g_y px | leak, own footprint px | one row fewer: points with a weight on a "
        "coupled row | leak px | fold_horizontal: C1 row, leak px |\n|---|---|---|---|---|---|---|---|\n");
    for (const bool remeasured : {true, false}) for (const std::uint32_t step : {8u, 16u, 32u}) {
        BuildOptions o;
        o.gate_row_step = step;
        o.gate_remeasured = remeasured;
        const std::string label = std::string(remeasured ? "with the remeasurement" : "primary only")+", rows every "+std::to_string(step)+" px";
        const auto b = Build(SceneNamed("wave_long"), o);
        const auto& g = b.request.rows;
        const std::size_t mid = g.row_count/2;
        const auto coupled = CoupledRows(g, mid);
        const auto rows = pc::HoleNeighbourRows(g, b.request.design);
        Check(rows.above == mid-coupled.first && rows.below == coupled.last-mid, label+": the hole's rows are the rows the join couples ("+
            std::to_string(mid-coupled.first)+" above, "+std::to_string(coupled.last-mid)+" below)");
        CheckHoleInvariant(b.request, "hole, "+label);
        const auto profile = pc::ToParallaxProfile(pc::EstimateParallax(b.request));
        const double leak = InterpolationLeak(b.request, profile, mid, 3.0, b.e_b);
        const double leak_small = InterpolationLeak(b.request, profile, mid, 0.2, b.e_b);
        auto own = b.request;
        own.design.hole_join_reach_rows = 0;
        const double leak_own = InterpolationLeak(own, mid, 3.0, b.e_b);
        // One row fewer on each side (reported: that the hole is the least is the S = 1 of CheckHoleInvariant): the points with a weight
        // on a coupled row's pixels over the rows around the middle. Whether the mapping then moves over one gate row's pixels depends on
        // whether a point of the lattice falls in the strip that frees.
        auto fewer = own;
        fewer.design.hole_neighbour_rows = std::min(rows.above, rows.below)-1;
        HoleCount fewer_count;
        for (std::size_t r = mid-std::min<std::size_t>(mid, 12); r <= std::min<std::size_t>(g.row_count-1, mid+12); ++r)
            CountHole(fewer, r, pc::ParallaxRowWeights(fewer, r), fewer_count);
        Check(leak == 0 && leak_small == 0, label+": the rows a gate point reads see none of an error on its pixels ("+std::to_string(leak)+" px)");
        // C1: the row the prediction along a fold line moves most.
        const auto f = Build(SceneNamed("fold_horizontal"), o);
        const auto base = pc::EstimateParallax(f.request);
        std::size_t row = base.rows.size();
        for (std::size_t i = 0; i < base.rows.size(); ++i)
            if (base.rows[i].fold == pc::ParallaxRowFold::predicted && (row == base.rows.size() ||
                std::abs(base.rows[i].q-base.rows[i].q_isotropic) > std::abs(base.rows[row].q-base.rows[row].q_isotropic))) row = i;
        Check(row < base.rows.size(), label+": fold_horizontal has a predicted row");
        double leak_c1 = -1;
        if (row < base.rows.size()) {
            CheckHoleInvariantAlongLines(f.request, "hole along the line, fold_horizontal, "+label);
            leak_c1 = InterpolationLeak(f.request, pc::ToParallaxProfile(base), row, 3.0, f.e_b);
            Check(leak_c1 == 0, label+": fold_horizontal, the rows a predicted row's gate point reads see none of an error on its pixels ("+
                std::to_string(leak_c1)+" px)");
        }
        std::printf("| %s | %u | %u/%u | %.0f | %.3f | %zu | %.3f | %zu, %.3f |\n", remeasured ? "with the remeasurement" : "primary only", step, rows.above,
            rows.below, HoleY(o), leak_own, fewer_count.sharing_used, leak, row, leak_c1);
        Check(fewer_count.sharing_used > 0, label+": with one row fewer on each side, points with a weight read a coupled row's pixels");
        std::fflush(stdout);
    }
}

// ---- Independence through the line search (11.5, C1 of 11.26) ----------------------------------------------------------------------------
// Which lines are found rests on the candidates, and an error on a gate row's pixels makes those points candidates (3 px is far over
// the threshold): a row whose hole holds candidates searches again without them, so an error confined to the pixels of the gate rows
// that read a row's q does not move the row's lines either. Gate rows every 32 px (the gate design's spacing); per scene, of the gate
// rows whose coupled samples (HoleNeighbourRows) include a row tried along a line, `three` with 3 px (the first and the last, where the
// error sits at a line's end, then for each found line the two whose centre is nearest it, then the second and the second last) and
// the first `small` of them with 0.2 px; the largest change of the mapping's q over the rows the gate point reads must be 0.
// (The rows the search over every candidate moved, gate rows every 32 px: book 0, 3, 9, 70, 73; fold_horizontal 40.)
struct LeakCheck {
    std::size_t rows{}, leaking{};
    double worst{};
    std::string at;
};
double SegmentDistance(const pc::FoldLine& line, Point c) {
    const double vx = line.last.x-line.first.x, vy = line.last.y-line.first.y, length2 = vx*vx+vy*vy;
    const double t = length2 > 0 ? std::clamp(((c.x-line.first.x)*vx+(c.y-line.first.y)*vy)/length2, 0.0, 1.0) : 0.0;
    return std::hypot(c.x-(line.first.x+t*vx), c.y-(line.first.y+t*vy));
}
std::vector<std::size_t> RowsNearLines(const pc::ParallaxRequest& request, const pc::ParallaxResult& base, std::size_t most) {
    const auto& g = request.rows;
    const auto hole = pc::HoleNeighbourRows(g, request.design);
    std::vector<std::size_t> near;
    for (std::size_t j = 0; j < g.row_count; ++j) {
        bool tried = false;
        for (std::size_t i = j > hole.above ? j-hole.above : 0; i <= std::min<std::size_t>(g.row_count-1, j+hole.below) && !tried; ++i)
            tried = base.rows[i].fold != pc::ParallaxRowFold::none;
        if (tried) near.push_back(j);
    }
    std::vector<std::size_t> picked;
    const auto add = [&](std::size_t j) {
        if (picked.size() < most && std::find(picked.begin(), picked.end(), j) == picked.end()) picked.push_back(j);
    };
    if (near.empty()) return picked;
    add(near.front());
    add(near.back());
    for (const auto& line : base.folds) {
        std::vector<std::pair<double, std::size_t>> by;
        for (const auto j : near) by.push_back({SegmentDistance(line, {static_cast<double>(g.seam_column), g.first_row+static_cast<double>(j)*g.row_step}), j});
        std::stable_sort(by.begin(), by.end(), [](const auto& a, const auto& b) {return a.first < b.first;});
        for (std::size_t k = 0; k < std::min<std::size_t>(2, by.size()); ++k) add(by[k].second);
    }
    if (near.size() > 1) { add(near[1]); add(near[near.size()-2]); }
    return picked;
}
LeakCheck CheckLeaks(const Built& b, const std::vector<std::size_t>& rows, double error) {
    const auto profile = pc::ToParallaxProfile(pc::EstimateParallax(b.request));
    LeakCheck c;
    for (const auto row : rows) {
        const double leak = InterpolationLeak(b.request, profile, row, error, b.e_b);
        ++c.rows;
        c.worst = std::max(c.worst, leak);
        if (leak != 0) { ++c.leaking; c.at += (c.at.empty() ? "" : ",")+std::to_string(row); }
    }
    return c;
}
void LineIndependence() {
    constexpr std::size_t three = 4, small = 1;
    BuildOptions o;
    o.gate_row_step = gate::kSeamGateDesignCalibrated.point_spacing_px;
    std::printf("\n| scene | lines | rows checked, 3 px | leaking rows, largest px | rows checked, 0.2 px | leaking rows, largest px |\n|---|---|---|---|---|---|\n");
    for (const char* name : {"book", "fold_horizontal", "step_horizontal", "fold_vertical"}) {
        const auto b = Build(SceneNamed(name), o);
        const auto base = pc::EstimateParallax(b.request);
        const auto rows = RowsNearLines(b.request, base, three);
        const auto big = CheckLeaks(b, rows, 3.0);
        const auto tiny = CheckLeaks(b, std::vector<std::size_t>(rows.begin(), rows.begin()+static_cast<std::ptrdiff_t>(std::min(small, rows.size()))), 0.2);
        std::printf("| %s | %zu | %zu | %zu %s, %.4f | %zu | %zu %s, %.4f |\n", name, base.folds.size(), big.rows, big.leaking, big.at.c_str(), big.worst, tiny.rows,
            tiny.leaking, tiny.at.c_str(), tiny.worst);
        std::fflush(stdout);
        Check(!rows.empty(), std::string(name)+": gate rows read a row tried along a line");
        Check(big.leaking == 0 && tiny.leaking == 0, std::string(name)+": an error on a gate row's pixels does not move the lines of the rows it reads ("+
            std::to_string(big.worst)+" px, "+std::to_string(tiny.worst)+" px)");
    }
}

// ---- The coupling over every gate row (--leaks; not a check) ----------------------------------------------------------------------------
// The hole holds the join; two other paths carry a row's estimate farther than the join: the rows without their own estimate take
// the q of measured rows up to 2 delta_y away (FillUnmeasuredRows), and which fold lines are found (and so which rows are predicted
// along them, at which angle) rests on the candidates (a row searches again without those in its hole). An error of 3 px and of
// 0.2 px on each gate row's pixels in turn, gate rows every 32 px; per scene, the largest change of the mapping's q over the rows that
// gate point reads, for the gate rows whose coupled samples are all measured without C1, those with a filled sample among them, and
// those with a row tried along a line among them; then the gate rows with a change.
// The scenes with a strip without points across the band (a white strip), so that gate rows are filled next to measured ones.
struct LeakScene {
    std::string name;
    ps::SceneSpec spec;
    BuildOptions options;
};
std::vector<LeakScene> LeakScenes() {
    BuildOptions o;
    o.gate_row_step = gate::kSeamGateDesignCalibrated.point_spacing_px;
    std::vector<LeakScene> scenes;
    for (const char* name : {"wave_long", "book", "fold_horizontal", "fold_vertical", "step_horizontal"}) scenes.push_back({name, SceneNamed(name), o});
    const double middle = (TheSetup().seam.first_row+TheSetup().seam.last_row)/2.0;
    for (const double strip : {96.0, 160.0, 224.0}) {
        auto white = o;
        white.keep = [=](double y, double) {return !(y >= middle && y < middle+strip);};
        scenes.push_back({"wave_long, white strip "+std::to_string(static_cast<int>(strip))+" px", SceneNamed("wave_long"), white});
    }
    return scenes;
}
int Leaks(const std::string& only) {
    namespace m2x = a0::m2::experimental;
    std::printf("| scene | error px | gate rows | measured, no C1: rows with a leak / largest px | a filled sample: rows / px | a row tried along a line: rows / px | rows with a leak |\n");
    std::printf("|---|---|---|---|---|---|---|\n");
    for (const auto& scene : LeakScenes()) {
        if (!only.empty() && scene.name.find(only) == std::string::npos) continue;
        const char* name = scene.name.c_str();
        const auto b = Build(scene.spec, scene.options);
        const auto& g = b.request.rows;
        const auto base = pc::EstimateParallax(b.request);
        const auto before = pc::ToParallaxProfile(base);
        const auto f = GatePixels(1000, 1000, 0, true);
        for (const double error : {3.0, 0.2}) {
            std::uint32_t n[3] = {0, 0, 0};
            double worst[3] = {0, 0, 0};
            std::string leaking;
            for (std::size_t row = 0; row < g.row_count; ++row) {
                const auto y = static_cast<std::uint32_t>(g.first_row+row*g.row_step);
                auto spoiled = b.request;
                for (auto& p : spoiled.points) {
                    if (!SharesWithRow(g, y, p, b.request.design.hole_margin_px)) continue;
                    p.displacement.x += error*b.e_b.x;
                    p.displacement.y += error*b.e_b.y;
                }
                const auto result = pc::EstimateParallax(spoiled);
                if (!result.Applied()) continue;
                const auto after = pc::ToParallaxProfile(result);
                double leak = 0;
                for (auto r = static_cast<std::int64_t>(y)+f.y0-1000; r < static_cast<std::int64_t>(y)+f.y1-1000; ++r)
                    leak = std::max(leak, std::abs(m2x::ParallaxShiftAtRow(after, static_cast<double>(r))-m2x::ParallaxShiftAtRow(before, static_cast<double>(r))));
                // The samples q(y) reads over those rows: the gate rows within 2 row steps of them.
                const auto low = static_cast<std::int64_t>(y)+f.y0-1000-2*static_cast<std::int64_t>(g.row_step), high = static_cast<std::int64_t>(y)+f.y1-1000+2*static_cast<std::int64_t>(g.row_step);
                bool filled = false, along = false;
                for (std::size_t i = 0; i < g.row_count; ++i) {
                    const auto yi = static_cast<std::int64_t>(g.first_row+i*g.row_step);
                    if (yi <= low || yi >= high) continue;
                    filled = filled || base.rows[i].state == pc::ParallaxRowState::filled || result.rows[i].state == pc::ParallaxRowState::filled;
                    along = along || base.rows[i].fold != pc::ParallaxRowFold::none || result.rows[i].fold != pc::ParallaxRowFold::none;
                }
                const int kind = along ? 2 : filled ? 1 : 0;
                if (leak > 0) {
                    ++n[kind];
                    char text[64];
                    std::snprintf(text, sizeof text, "%s%zu (%.4f)", leaking.empty() ? "" : ", ", row, leak);
                    leaking += text;
                }
                worst[kind] = std::max(worst[kind], leak);
            }
            std::printf("| %s | %.1f | %u | %u / %.4f | %u / %.4f | %u / %.4f | %s |\n", name, error, g.row_count, n[0], worst[0], n[1], worst[1], n[2], worst[2],
                leaking.empty() ? "-" : leaking.c_str());
            std::fflush(stdout);
        }
    }
    return 0;
}

// ---- The lift line and the switch (11.7, 11.18) ------------------------------------------------------------------------------------------
ps::SceneSpec WideBump(double height_mm) {
    const auto& s = TheSetup();
    ps::SceneSpec spec;
    spec.name = "wide_bump";
    spec.field.bumps.push_back({s.seam.x_mm, s.seam.y_mid_mm, height_mm, 40.0});
    return spec;
}
void LiftLine() {
    const auto spec = WideBump(2.0);
    BuildOptions options;
    const auto b = Build(spec, options);
    const auto r = pc::EstimateParallax(b.request);
    const auto o = Evaluate(b, r);
    PrintRow("wide_bump (2 mm, apply)", b, r, o, "lift");
    Check(r.Applied(), "lift over the line, switch true: applied");
    Check(r.lift.exceeded && r.lift.exceeded_rows > 0 && r.lift.maximum_mm > 1.0, "lift over the line: exceeded, with rows");
    CheckNear(r.lift.limit_mm, 1.0, 0, "lift: the line is the condition's");
    // The sign: a raised paper gives a positive lift, and lift = -q / k.
    std::size_t peak = 0;
    for (std::size_t i = 0; i < r.rows.size(); ++i) if (std::abs(r.rows[i].q) > std::abs(r.rows[peak].q)) peak = i;
    Check(r.rows[peak].q < 0 && r.rows[peak].lift_mm > 0, "lift: raised paper moves B toward -e_b and lifts positive");
    const double true_lift = -b.q_true[peak]/b.k;
    CheckNear(r.rows[peak].lift_mm, true_lift, 0.25*true_lift, "lift: the peak's lift is the truth's (relative to the plane)");
    CheckNear(r.rows[peak].lift_mm, -r.rows[peak].q/r.k_px_per_mm, 1e-12, "lift = -q / k");
    Check(r.lift.first_exceeded_row <= r.lift.at_row && r.lift.at_row <= r.lift.last_exceeded_row, "lift: the exceeded rows contain the maximum");

    auto off = b.request;
    off.apply_beyond_line = false;
    const auto n = pc::EstimateParallax(off);
    Check(n.state == pc::ParallaxState::not_applied && n.reasons == std::vector<std::string>{"beyond-lift-line"}, "lift over the line, switch false: not applied");
    bool same = n.rows.size() == r.rows.size();
    for (std::size_t i = 0; same && i < n.rows.size(); ++i) same = n.rows[i].q == r.rows[i].q && !n.rows[i].applied;
    Check(same, "switch false: the estimate is the same, only not applied");

    auto wide = b.request;
    wide.max_lift_mm = 5.0;
    const auto w = pc::EstimateParallax(wide);
    Check(w.Applied() && !w.lift.exceeded && w.lift.exceeded_rows == 0, "lift under the line: applied, not exceeded");
    auto exact = b.request;
    exact.max_lift_mm = r.lift.maximum_mm;
    Check(!pc::EstimateParallax(exact).lift.exceeded, "lift equal to the line is inside");

    // The booklet: the paper's bend at the seam is far over the line; the switch decides, and the lift is reported.
    {
        const auto book = Build(SceneNamed("book"));
        const auto applied = pc::EstimateParallax(book.request);
        PrintRow("book (apply)", book, applied, Evaluate(book, applied), "lift");
        Check(applied.Applied() && applied.lift.exceeded && applied.lift.maximum_mm > 1.0, "book: over the line, applied with the switch true");
        auto refused = book.request;
        refused.apply_beyond_line = false;
        const auto kept = pc::EstimateParallax(refused);
        Check(kept.state == pc::ParallaxState::not_applied && kept.reasons.front() == "beyond-lift-line", "book: not applied with the switch false");
    }
    // Strain: a short fall-off would stretch B by more than s_max.
    BuildOptions strained;
    strained.design.weight_falloff_px = 30;
    const auto t = pc::EstimateParallax(Build(spec, strained).request);
    Check(t.state == pc::ParallaxState::not_applied && t.reasons == std::vector<std::string>{"strain-limit"} && t.summary.max_strain > strained.design.maximum_strain,
        "strain over s_max: not applied (strain-limit)");
}

// ---- States and refusals ---------------------------------------------------------------------------------------------------------------------
void States() {
    const auto b = Build(SceneNamed("wave_long"));
    {
        // The samples the mapping takes: one q per gate row on the gate's grid, e_b, a and L.
        const auto r = pc::EstimateParallax(b.request);
        const auto profile = pc::ToParallaxProfile(r);
        bool same = profile.q_px.size() == r.rows.size();
        for (std::size_t i = 0; same && i < r.rows.size(); ++i) same = profile.q_px[i] == r.rows[i].q;
        Check(same && profile.first_row_px == b.request.rows.first_row && profile.row_spacing_px == b.request.rows.row_step &&
            profile.seam_column == b.request.rows.seam_column && profile.baseline.x == r.baseline.x && profile.baseline.y == r.baseline.y &&
            profile.a_px == b.request.design.weight_flat_px && profile.l_px == b.request.design.weight_falloff_px, "profile: the rows on the gate's grid");
        Check(r.baseline.x == b.request.baseline.direction.x && r.k_px_per_mm == b.request.baseline.k_px_per_mm, "result: e_b and k are the request's");
        // The mapping's interpolation (S1) passes through every gate row's q unchanged, so the hole of each row holds in the
        // mapping as estimated; and the strain is the mapping's own.
        a0::m2::experimental::ValidateParallaxProfile(profile);
        bool through = true;
        double between = 0;
        for (std::size_t i = 0; i < r.rows.size(); ++i) {
            through = through && a0::m2::experimental::ParallaxShiftAtRow(profile, r.rows[i].y) == r.rows[i].q;
            if (i+1 < r.rows.size()) {
                const double mid = a0::m2::experimental::ParallaxShiftAtRow(profile, r.rows[i].y+0.5*b.request.rows.row_step);
                const double low = std::min(r.rows[i].q, r.rows[i+1].q), high = std::max(r.rows[i].q, r.rows[i+1].q);
                between = std::max(between, std::max(low-mid, mid-high));
            }
        }
        Check(through, "profile: the mapping's q(y) is the estimate at every gate row, bit for bit");
        Check(between <= 0, "profile: between gate rows the mapping's q stays between the two rows' values");
        Check(a0::m2::experimental::ParallaxMaximumStrain(profile) == r.summary.max_strain, "profile: the strain is the mapping's own");
        Check(a0::m2::experimental::ParallaxSeamWeight(static_cast<double>(r.summary.last_column-b.request.rows.seam_column), profile.a_px, profile.l_px) > 0 &&
            a0::m2::experimental::ParallaxSeamWeight(static_cast<double>(r.summary.last_column-b.request.rows.seam_column)+1, profile.a_px, profile.l_px) == 0,
            "summary: the last moved column is the last with w > 0");
        auto refused = b.request;
        refused.apply_beyond_line = false;
        refused.max_lift_mm = 1e-3;
        Check(Throws([&] {(void)pc::ToParallaxProfile(pc::EstimateParallax(refused));}), "profile: a result that is not applied is refused");
    }
    {
        auto r = b.request;
        r.max_lift_mm.reset();
        const auto d = pc::EstimateParallax(r);
        Check(d.state == pc::ParallaxState::disabled && d.reasons == std::vector<std::string>{"disabled"} && d.rows.empty(),
            "no line: disabled, nothing estimated");
    }
    {
        const auto u = pc::UnestimatedParallax(pc::ParallaxState::not_applied, "pose-untrusted");
        Check(u.state == pc::ParallaxState::not_applied && u.reasons == std::vector<std::string>{"pose-untrusted"} && u.rows.empty(), "unestimated: not applied");
        Check(Throws([] {(void)pc::UnestimatedParallax(pc::ParallaxState::applied, "x");}), "unestimated: applied is refused");
        Check(Throws([] {(void)pc::UnestimatedParallax(pc::ParallaxState::disabled, "");}), "unestimated: an empty reason is refused");
    }
    {
        auto r = b.request;
        r.points.resize(5);
        const auto few = pc::EstimateParallax(r);
        Check(few.state == pc::ParallaxState::not_applied && few.reasons.front() == "too-few-points" && few.summary.rows_measured == 0,
            "five points: not applied, too-few-points");
    }
    {
        // Points on one side of the seam only: every row would be an extrapolation.
        auto r = b.request;
        r.points.erase(std::remove_if(r.points.begin(), r.points.end(), [&](const pc::ParallaxPoint& p) {return p.position.x < r.rows.seam_column+64;}), r.points.end());
        const auto one = pc::EstimateParallax(r);
        bool all = one.summary.rows_measured == 0;
        for (const auto& row : one.rows) all = all && row.reason == "one-sided";
        Check(all && one.reasons.front() == "too-few-points", "points on one side only: every row is one-sided");
        // The same with degree 1 and a narrow kernel (the measured failure).
        auto narrow = r;
        narrow.design.polynomial_degree = 1;
        narrow.design.kernel_sigma_x_px = narrow.design.kernel_sigma_y_px = 16;
        bool flagged = true;
        for (const auto& row : pc::EstimateParallax(narrow).rows) flagged = flagged && row.state == pc::ParallaxRowState::filled;
        Check(flagged, "points on one side only, degree 1 with a narrow kernel: no row is measured");
    }
    {
        // Points above a row only: the rows below the last point row are one-sided or empty, never measured by extrapolation.
        auto r = b.request;
        const double cut = r.rows.first_row+r.rows.row_step*(r.rows.row_count/2.0);
        r.points.erase(std::remove_if(r.points.begin(), r.points.end(), [&](const pc::ParallaxPoint& p) {return p.position.y > cut;}), r.points.end());
        const auto above = pc::EstimateParallax(r);
        bool none_far = true;
        for (const auto& row : above.rows) if (row.y > cut+kGatePatch) none_far = none_far && row.state == pc::ParallaxRowState::filled;
        Check(none_far, "points above only: rows below the points are not measured");
    }
}

void Refusals() {
    const auto b = Build(SceneNamed("wave_long"));
    const auto refuse = [&](const std::string& label, const std::function<void(pc::ParallaxRequest&)>& spoil) {
        auto r = b.request;
        spoil(r);
        Check(Throws([&] {(void)pc::EstimateParallax(r);}), "refused: "+label);
    };
    const double nan = std::nan(""), inf = std::numeric_limits<double>::infinity();
    refuse("a NaN displacement", [&](auto& r) {r.points[3].displacement.x = nan;});
    refuse("an infinite displacement", [&](auto& r) {r.points[3].displacement.y = inf;});
    refuse("a NaN position", [&](auto& r) {r.points[7].position.y = nan;});
    refuse("a NaN uncertainty", [&](auto& r) {r.points[7].uncertainty_px = nan;});
    refuse("a zero uncertainty", [&](auto& r) {r.points[7].uncertainty_px = 0;});
    refuse("a footprint without its point", [&](auto& r) {r.points[7].footprint = {0, 0, 1, 1};});
    refuse("a gate footprint without its point", [&](auto& r) {r.rows.gate_footprint = {1, 1, 2, 2};});
    refuse("a one-directional point without a unit normal", [&](auto& r) {r.points[2].kind = gate::PointKind::one_directional; r.points[2].normal = {nan, 0};});
    refuse("a NaN baseline", [&](auto& r) {r.baseline.direction.x = nan;});
    refuse("a baseline that is not a unit vector", [&](auto& r) {r.baseline.direction = {1, 1};});
    refuse("a NaN k", [&](auto& r) {r.baseline.k_px_per_mm = nan;});
    refuse("a NaN lift line", [&](auto& r) {r.max_lift_mm = nan;});
    refuse("a zero lift line", [&](auto& r) {r.max_lift_mm = 0.0;});
    refuse("a line without the switch", [&](auto& r) {r.apply_beyond_line.reset();});
    refuse("an unset design value", [&](auto& r) {r.design.maximum_sigma_q_px = nan;});
    refuse("degree 3", [&](auto& r) {r.design.polynomial_degree = 3;});
    refuse("n_min below the coefficients", [&](auto& r) {r.design.minimum_effective_points = 5;});
    refuse("a smaller than Omega", [&](auto& r) {r.rows.omega_half_width_px = static_cast<std::uint32_t>(r.design.weight_flat_px)+1;});
    refuse("a gate row outside the output", [&](auto& r) {r.rows.row_count = r.rows.output_height;});
    refuse("no gate rows", [&](auto& r) {r.rows.row_count = 0;});
    refuse("thread count 0", [&](auto& r) {r.thread_count = 0;});
    refuse("an unset kernel along a line", [&](auto& r) {r.design.fold_kernel_sigma_along_px = nan;});
    refuse("a zero kernel across a line", [&](auto& r) {r.design.fold_kernel_sigma_across_px = 0;});
    refuse("a negative band of the rows tried along a line", [&](auto& r) {r.design.fold_row_half_width_px = -1;});
    refuse("an unset least change", [&](auto& r) {r.design.fold_minimum_change_px = nan;});
    refuse("a zero residual ratio", [&](auto& r) {r.design.fold_maximum_rms_ratio = 0;});
    refuse("an angle refinement over 45 degrees", [&](auto& r) {r.design.fold_angle_refinement_deg = 50;});
    refuse("too many refinement steps", [&](auto& r) {r.design.fold_angle_refinement_steps = 17;});
    refuse("a join's reach over 16 rows", [&](auto& r) {r.design.hole_join_reach_rows = 17;});
    Check(Throws([&] {auto g = b.request.rows; g.row_step = 0; (void)pc::HoleNeighbourRows(g, b.request.design);}), "refused: the hole's rows for a row step of 0");
    Check(Throws([] {pc::ValidateParallaxDesign(pc::ParallaxDesign{});}), "refused: the empty design");
    pc::ValidateParallaxDesign(pc::kParallaxDesignDraft);
    Check(Throws([&] {(void)pc::ParallaxRowWeights(b.request, b.request.rows.row_count);}), "refused: a row outside the gate rows");
    Check(Throws([&] {(void)pc::ParallaxRowFitOf(b.request, b.request.rows.row_count, 0.0);}), "refused: a row outside the gate rows (along a line)");
    Check(Throws([&] {(void)pc::ParallaxRowFitOf(b.request, 0, nan);}), "refused: a NaN angle");
    // Without an angle the row's fit is the regression of 11.4 (its weights are ParallaxRowWeights').
    {
        const auto fit = pc::ParallaxRowFitOf(b.request, 5, std::nullopt);
        const auto weights = pc::ParallaxRowWeights(b.request, 5);
        bool same = fit.weights.size() == weights.size();
        for (std::size_t i = 0; same && i < weights.size(); ++i) same = fit.weights[i].point == weights[i].point && fit.weights[i].weight == weights[i].weight;
        Check(same && fit.measured && fit.q == pc::EstimateParallax(b.request).rows[5].q_isotropic, "row fit without an angle: 11.4's regression");
    }
}

// ---- Rows without their own estimate ------------------------------------------------------------------------------------------------------
void Fill() {
    const double step = 8, blend = 64;
    {
        // A short gap is joined linearly (not set to 0).
        std::vector<double> q{1, 1, 0, 0, 0, 3, 3};
        std::vector<bool> m{true, true, false, false, false, true, true};
        pc::FillUnmeasuredRows(q, m, step, blend);
        CheckNear(q[2], 1.5, 1e-15, "fill: short gap, first");
        CheckNear(q[3], 2.0, 1e-15, "fill: short gap, middle");
        CheckNear(q[4], 2.5, 1e-15, "fill: short gap, last");
        CheckNear(q[0], 1, 0, "fill: measured rows unchanged");
    }
    {
        // A long gap fades to 0 over delta_y from each side and holds nothing beyond it.
        const std::size_t n = 40;
        std::vector<double> q(n, 0.0);
        std::vector<bool> m(n, false);
        q[0] = 2; m[0] = true;
        q[n-1] = -1; m[n-1] = true;
        pc::FillUnmeasuredRows(q, m, step, blend);
        CheckNear(q[1], 2*(1-(1/8.0)*(1/8.0)*(3-2/8.0)), 1e-12, "fill: long gap, next to the measured row");
        CheckNear(q[8], 0, 0, "fill: long gap, delta_y away");
        CheckNear(q[20], 0, 0, "fill: long gap, middle");
        Check(q[n-2] < 0 && q[n-2] > -1, "fill: long gap, the other side fades from its own value");
        bool monotone = true;
        for (std::size_t i = 1; i < 8; ++i) monotone = monotone && q[i] <= q[i-1];
        Check(monotone, "fill: the fade is monotone");
    }
    {
        std::vector<double> q{5, 5};
        std::vector<bool> m{false, false};
        pc::FillUnmeasuredRows(q, m, step, blend);
        Check(q[0] == 0 && q[1] == 0, "fill: no measured row gives 0");
        Check(Throws([&] {std::vector<double> x{1}; std::vector<bool> y{true, false}; pc::FillUnmeasuredRows(x, y, step, blend);}), "fill: sizes differ");
        Check(Throws([&] {pc::FillUnmeasuredRows(q, m, 0, blend);}), "fill: zero spacing");
    }
}

// ---- The points from the gate's measurer -----------------------------------------------------------------------------------------------------
void FromMeasurement() {
    gate::SeamMeasurement m;
    m.design = gate::kSeamGateDesignCalibrated;
    gate::SeamPoint two;
    two.x = 100; two.y = 200; two.status = gate::PointStatus::valid; two.kind = gate::PointKind::two_directional;
    two.displacement = {0.5, -0.25}; two.uncertainty_px = 0.04;
    gate::SeamPoint one = two;
    one.x = 116; one.kind = gate::PointKind::one_directional; one.normal = {0.6, 0.8};
    gate::SeamPoint unconfirmed = two;
    unconfirmed.x = 132; unconfirmed.confirmation = gate::Confirmation::unconfirmed;
    gate::SeamPoint ambiguous = two;
    ambiguous.x = 148; ambiguous.status = gate::PointStatus::ambiguous;
    m.points = {two, one, unconfirmed, ambiguous};
    const auto points = pc::PointsFromSeamMeasurement(m);
    Check(points.size() == 2, "from the measurer: counted points only");
    if (points.size() == 2) {
        Check(points[0].position.x == 100 && points[0].position.y == 200 && points[0].displacement.x == 0.5 && points[0].displacement.y == -0.25 &&
            points[0].uncertainty_px == 0.04 && points[0].kind == gate::PointKind::two_directional && !rg::PatchesShareNoPixel(points[0].footprint, rg::GatePatchOf(two, m.design)) && points[0].footprint.x0 == rg::GatePatchOf(two, m.design).x0 && points[0].footprint.y1 == rg::GatePatchOf(two, m.design).y1,
            "from the measurer: the fields of a two-directional point");
        Check(points[1].kind == gate::PointKind::one_directional && points[1].normal.x == 0.6 && points[1].normal.y == 0.8, "from the measurer: the normal");
    }
}

// ---- Fold and gutter lines (11.8) -------------------------------------------------------------------------------------------------------
std::string FoldText(const pc::ParallaxResult& r) {
    std::string out;
    for (const auto& f : r.folds) {
        char text[200];
        std::snprintf(text, sizeof text, "%s%.0f deg, row %.0f, dist %.0f px%s, %+d %.2f mm, %u pts%s", out.empty() ? "" : "; ", f.angle_deg, f.position_px,
            f.distance_from_seam_px, f.crosses_seam ? " (crosses)" : "", f.step_sign, f.step_mm, f.points,
            f.shift_to_clear_px ? (", shift "+std::to_string(static_cast<int>(std::lround(*f.shift_to_clear_px)))+" px").c_str() : "");
        out += text;
    }
    return out.empty() ? "none" : out;
}
double AngleGap(double a, double b) {
    const double d = std::fmod(std::abs(a-b), 180.0);
    return std::min(d, 180-d);
}
void FoldLines() {
    const auto& s = TheSetup();
    const auto frame = cr::FrameOfRaster(s.rig.raster);
    const double pitch = frame.pitch_mm;
    const auto& d = pc::kParallaxDesignDraft;
    const double gx_px = HoleX();
    const double seam_row = (s.seam.y_mid_mm-frame.top_mm)/pitch-0.5;
    struct Case {
        std::string name;
        ps::SceneSpec spec;
        double angle_deg;
        double distance_px;       // from the seam column along x, through the middle row
        bool crosses;
    };
    const std::vector<Case> cases{
        {"fold_horizontal", SceneNamed("fold_horizontal"), 0, 0, true},
        {"fold_diagonal", SceneNamed("fold_diagonal"), 35, 0, true},
        {"fold_diagonal_2gx", ps::FoldScene(s.seam, 35.0, 2*gx_px*pitch), 35, 2*gx_px, true},
        {"fold_vertical", SceneNamed("fold_vertical"), 90, 0, false},
        {"fold_vertical_half_gx", ps::FoldScene(s.seam, 90.0, gx_px/2*pitch), 90, gx_px/2, false},
        {"fold_vertical_2gx", ps::FoldScene(s.seam, 90.0, 2*gx_px*pitch), 90, 2*gx_px, false},
        {"fold_vertical_band_edge", ps::FoldScene(s.seam, 90.0, (d.column_radius_px-d.column_spacing_px)*pitch), 90, d.column_radius_px-d.column_spacing_px, false},
    };
    std::printf("\n| scene | true line | lines found |\n|---|---|---|\n");
    for (const auto& c : cases) {
        const auto b = Build(c.spec);
        const auto r = pc::EstimateParallax(b.request);
        std::printf("| %s | %.0f deg, %.0f px from the seam | %s |\n", c.name.c_str(), c.angle_deg, c.distance_px, FoldText(r).c_str());
        // The ridge's line: at the fold's angle, the paper higher along it, nearest the fold.
        const double expected_row = seam_row-c.distance_px*std::tan(c.angle_deg*ps::kPi/180);
        const auto off = [&](const pc::FoldLine& f) {return c.crosses ? std::abs(f.position_px-expected_row) : std::abs(f.distance_from_seam_px-c.distance_px);};
        const pc::FoldLine* line = nullptr;
        for (const auto& f : r.folds)
            if (AngleGap(f.angle_deg, c.angle_deg) <= d.hough_angle_step_deg && f.step_sign == 1 && (!line || off(f) < off(*line))) line = &f;
        Check(line != nullptr, c.name+": a ridge line along the fold is found");
        if (!line) continue;
        Check(line->step_mm > 0, c.name+": the fold is a ridge (the paper higher along it)");
        Check(&r.folds.front() == line, c.name+": the ridge is the strongest line");
        CheckNear(line->step_mm, -line->step_px/r.k_px_per_mm, 1e-12, c.name+": step_mm = -step_px / k");
        Check(line->crosses_seam == c.crosses, c.name+": crosses the seam column as the fold does");
        Check(line->length_px >= d.fold_minimum_length_px && line->points >= d.fold_minimum_points, c.name+": the line is long enough");
        if (c.crosses) {
            CheckNear(line->distance_from_seam_px, 0, 0, c.name+": distance 0 when it crosses");
            CheckNear(line->position_px, expected_row, d.hough_offset_step_px+d.column_spacing_px, c.name+": the row where it crosses the seam column");
            Check(line->near_seam && !line->shift_to_clear_px, c.name+": near the seam, and no sideways move takes it away");
        } else {
            CheckNear(line->distance_from_seam_px, c.distance_px, d.column_spacing_px, c.name+": the distance from the seam column");
            const bool near = line->distance_from_seam_px <= d.fold_near_seam_px;
            Check(line->near_seam == near, c.name+": near the seam by D_fold");
            if (near) {
                Check(line->shift_to_clear_px.has_value(), c.name+": the move that clears it is given");
                if (line->shift_to_clear_px) {
                    const double moved = std::min(line->first.x, line->last.x)-s.seam.column+*line->shift_to_clear_px;
                    const double moved_far = std::max(line->first.x, line->last.x)-s.seam.column+*line->shift_to_clear_px;
                    Check(std::min(std::abs(moved), std::abs(moved_far)) >= d.fold_near_seam_px-1e-9 && moved*moved_far >= 0,
                        c.name+": after the move the line is D_fold from the seam column");
                }
            }
        }
    }
    // Smooth fields give no line.
    for (const char* name : {"zero", "wave_long"}) {
        const auto r = pc::EstimateParallax(Build(SceneNamed(name)).request);
        std::printf("| %s | none | %s |\n", name, FoldText(r).c_str());
        Check(r.folds.empty(), std::string(name)+": no line in a smooth field");
    }
    {
        const auto r = pc::EstimateParallax(Build(WideBump(2.0)).request);
        std::printf("| wide_bump | none | %s |\n", FoldText(r).c_str());
        Check(r.folds.empty(), "wide bump: no line");
    }
    // Lines across the baseline only: nothing to cross-validate.
    {
        const auto r = pc::EstimateParallax(Build(SceneNamed("lines_only")).request);
        Check(r.folds.empty() && r.summary.points_cross_validated == 0, "lines only: no leftover, no line");
    }
    // Reported only (no expectation in the design): the step, the short waves, the bump in the hole, the booklet.
    for (const char* name : {"step_horizontal", "wave_short", "seam_bump", "book"}) {
        const auto r = pc::EstimateParallax(Build(SceneNamed(name)).request);
        std::printf("| %s | (reported) | %s |\n", name, FoldText(r).c_str());
    }
}

// ---- C1 of 11.26: the rows near a fold line, predicted along it ------------------------------------------------------------------------------
// The rows' marks and the lines' counts agree, and the rows that do not take the prediction keep 11.4's estimate.
void FoldRowMarks() {
    for (const char* name : {"fold_horizontal", "fold_diagonal", "fold_vertical", "step_horizontal", "book"}) {
        const auto b = Build(SceneNamed(name));
        const auto r = pc::EstimateParallax(b.request);
        std::vector<std::uint32_t> predicted(r.folds.size(), 0), agree(r.folds.size(), 0), unpredictable(r.folds.size(), 0);
        std::uint32_t total[3] = {0, 0, 0};
        bool marks = true, kept = true, line_ok = true;
        for (const auto& row : r.rows) {
            if (row.fold == pc::ParallaxRowFold::none) {
                marks = marks && !row.fold_line && row.fold_reason.empty();
                kept = kept && row.q == row.q_isotropic;
                continue;
            }
            line_ok = line_ok && row.fold_line && *row.fold_line < r.folds.size();
            if (!row.fold_line || *row.fold_line >= r.folds.size()) continue;
            line_ok = line_ok && AngleGap(row.fold_angle_deg, r.folds[*row.fold_line].angle_deg) <= pc::kParallaxDesignDraft.fold_angle_refinement_deg+1e-9;
            if (row.fold == pc::ParallaxRowFold::predicted) {
                ++predicted[*row.fold_line]; ++total[0];
                marks = marks && row.fold_reason.empty() && row.state == pc::ParallaxRowState::measured &&
                    std::abs(row.q-row.q_isotropic) > pc::kParallaxDesignDraft.fold_minimum_change_px;
            } else if (row.fold == pc::ParallaxRowFold::agrees) {
                ++agree[*row.fold_line]; ++total[1];
                marks = marks && row.fold_reason.empty();
                if (row.state == pc::ParallaxRowState::measured) kept = kept && row.q == row.q_isotropic;
            } else {
                ++unpredictable[*row.fold_line]; ++total[2];
                marks = marks && !row.fold_reason.empty();
                if (row.state == pc::ParallaxRowState::measured) kept = kept && row.q == row.q_isotropic;
            }
        }
        bool counts = total[0] == r.summary.rows_fold_predicted && total[1] == r.summary.rows_fold_agree && total[2] == r.summary.rows_fold_unpredictable;
        for (std::size_t l = 0; l < r.folds.size(); ++l)
            counts = counts && r.folds[l].rows_predicted == predicted[l] && r.folds[l].rows_agree == agree[l] && r.folds[l].rows_unpredictable == unpredictable[l];
        Check(marks, std::string(name)+": each row's mark goes with its reason and state");
        Check(line_ok, std::string(name)+": a tried row names its line, at an angle within the refinement");
        Check(counts, std::string(name)+": the lines' and the summary's counts are the rows'");
        Check(kept, std::string(name)+": rows not predicted (with an estimate of their own) keep 11.4's estimate, bit for bit");
        if (std::string(name) != "book") Check(r.summary.rows_fold_predicted > 0, std::string(name)+": rows are predicted along the line");
    }
}

// An error confined to the pixels a predicted row's gate reads does not reach that row's q (as 11.4's rows, GateIndependence).
void GateIndependenceAlongLines() {
    for (const char* name : {"fold_horizontal", "fold_diagonal", "fold_vertical"}) {
        const auto b = Build(SceneNamed(name));
        const auto base = pc::EstimateParallax(b.request);
        std::size_t row = base.rows.size();
        for (std::size_t i = 0; i < base.rows.size(); ++i)
            if (base.rows[i].fold == pc::ParallaxRowFold::predicted && (row == base.rows.size() ||
                std::abs(base.rows[i].q-base.rows[i].q_isotropic) > std::abs(base.rows[row].q-base.rows[row].q_isotropic))) row = i;
        Check(row < base.rows.size(), std::string(name)+": a predicted row");
        if (row == base.rows.size()) continue;
        const auto& g = b.request.rows;
        const auto y = static_cast<std::uint32_t>(g.first_row+row*g.row_step);
        auto spoiled = b.request;
        std::size_t touched = 0;
        for (auto& p : spoiled.points) {
            if (!SharesWithRow(g, y, p, b.request.design.hole_margin_px)) continue;
            p.displacement.x += 3*b.e_b.x;
            p.displacement.y += 3*b.e_b.y;
            ++touched;
        }
        const auto r = pc::EstimateParallax(spoiled);
        std::printf("independence along the line, %s: row %zu (moved %.3f px by C1), %zu points of its gate's pixels spoiled by 3 px: %s, q %s\n", name, row,
            base.rows[row].q-base.rows[row].q_isotropic, touched, std::string(pc::ParallaxRowFoldCode(r.rows[row].fold)).c_str(),
            r.rows[row].q == base.rows[row].q ? "unchanged" : "changed");
        Check(touched > 0 && r.rows[row].fold == pc::ParallaxRowFold::predicted && r.rows[row].q == base.rows[row].q,
            std::string(name)+": the predicted row's q is unchanged, bit for bit, by an error on its gate's pixels");
    }
}

// The smooth fields: C1 changes no row (no line, or no row whose prediction along a line is used).
void SmoothFieldsUnchanged() {
    std::vector<std::pair<std::string, ps::SceneSpec>> scenes;
    for (const char* name : {"zero", "wave_long", "wave_long_dy_error", "book", "wave_short", "seam_bump", "lines_only"}) scenes.push_back({name, SceneNamed(name)});
    scenes.push_back({"wide_bump", WideBump(2.0)});
    for (const auto& [name, spec] : scenes) {
        const auto r = pc::EstimateParallax(Build(spec).request);
        bool same = true;
        for (const auto& row : r.rows) same = same && row.q == row.q_isotropic && row.fold != pc::ParallaxRowFold::predicted;
        std::printf("smooth field %s: lines %zu, rows tried along a line %u (agree %u, unpredictable %u), every row 11.4's: %s\n", name.c_str(), r.folds.size(),
            r.summary.rows_fold_predicted+r.summary.rows_fold_agree+r.summary.rows_fold_unpredictable, r.summary.rows_fold_agree, r.summary.rows_fold_unpredictable,
            same ? "yes" : "no");
        Check(same && r.summary.rows_fold_predicted == 0, name+": C1 changes no row");
    }
}

// Lines that are not a fold through the row, and the guards' switches: the rows keep 11.4's estimate.
double SmoothStep01(double t) {return t <= 0 ? 0 : t >= 1 ? 1 : t*t*(3-2*t);}
void FalseLines() {
    const auto& s = TheSetup();
    const auto frame = cr::FrameOfRaster(s.rig.raster);
    const double mid = (s.seam.y_mid_mm-frame.top_mm)/frame.pitch_mm-0.5;
    const auto unchanged = [](const pc::ParallaxResult& r) {
        bool same = true;
        for (const auto& row : r.rows) same = same && row.q == row.q_isotropic && row.fold != pc::ParallaxRowFold::predicted;
        return same;
    };
    const auto tried = [](const pc::ParallaxResult& r) {return r.summary.rows_fold_agree+r.summary.rows_fold_unpredictable+r.summary.rows_fold_predicted;};
    const auto reasons = [](const pc::ParallaxResult& r) {
        std::string out;
        for (const auto& row : r.rows) if (!row.fold_reason.empty() && out.find(row.fold_reason) == std::string::npos) out += (out.empty() ? "" : ",")+row.fold_reason;
        return out.empty() ? std::string("-") : out;
    };
    const Point e_b = Build(SceneNamed("zero")).e_b;
    // (a) A measurement error on one side of the seam column only: a streak of +2 px along e_b over the left half of the band, three
    // lattice rows tall (ASSUMPTION: a printed line that biases the measurer), on the long waves; the truth has no fold. Its line
    // ends before the seam column.
    {
        BuildOptions o;
        o.measurement_error = [&](double x, double y) {
            return x <= s.seam.column-24.0 && std::abs(y-mid) <= 24 ? Point{2*e_b.x, 2*e_b.y} : Point{0, 0};
        };
        const auto b = Build(SceneNamed("wave_long"), o);
        const auto r = pc::EstimateParallax(b.request);
        std::printf("false line, one-sided streak: %s (candidates %u, scale %.3f px); rows tried %u (unpredictable %u: %s)\n", FoldText(r).c_str(),
            r.summary.fold_candidates, r.summary.fold_residual_scale_px, tried(r), r.summary.rows_fold_unpredictable, reasons(r).c_str());
        Check(!r.folds.empty() && !r.folds.front().crosses_seam, "one-sided streak: a line is found, and it does not cross the seam column");
        Check(tried(r) > 0 && unchanged(r), "one-sided streak: the rows near it are tried and keep 11.4's estimate");
        bool ends = r.summary.rows_fold_unpredictable > 0;
        for (const auto& row : r.rows) if (row.fold == pc::ParallaxRowFold::unpredictable) ends = ends && row.fold_reason == "line-ends";
        Check(ends, "one-sided streak: the rows near it are unpredictable for line-ends");
    }
    // (b) A fold that crosses the seam column and ends just after it: fold_horizontal's residual faded to 0 over 32 px around
    // x_s - 40 (the points along it on the left of the hole see flat paper).
    {
        BuildOptions o;
        o.reshape = [&](double x, double, Point r) {
            const double t = SmoothStep01((x-(s.seam.column-56.0))/32.0);
            return Point{r.x*t, r.y*t};
        };
        const auto b = Build(SceneNamed("fold_horizontal"), o);
        const auto r = pc::EstimateParallax(b.request);
        const auto out = Evaluate(b, r);
        std::printf("fold that ends at the seam: %s (candidates %u, scale %.3f px); rows tried %u (unpredictable %u: %s); before %.3f after %.3f px\n",
            FoldText(r).c_str(), r.summary.fold_candidates, r.summary.fold_residual_scale_px, tried(r), r.summary.rows_fold_unpredictable, reasons(r).c_str(),
            out.before, out.after);
        for (std::size_t i = 0; i < r.rows.size(); ++i) {
            const auto& row = r.rows[i];
            if (row.fold == pc::ParallaxRowFold::none || row.q == row.q_isotropic) continue;
            std::printf("  row %zu (y %.0f): %s %s, angle %.1f, line %d, q - 11.4 %.3f px\n", i, row.y, std::string(pc::ParallaxRowFoldCode(row.fold)).c_str(),
                row.fold_reason.c_str(), row.fold_angle_deg, row.fold_line ? static_cast<int>(*row.fold_line) : -1, row.q-row.q_isotropic);
        }
        Check(!r.folds.empty() && tried(r) > 0 && unchanged(r), "fold that ends at the seam: tried, and every row keeps 11.4's estimate");
        bool ends = r.summary.rows_fold_unpredictable > 0;
        for (const auto& row : r.rows) if (row.fold == pc::ParallaxRowFold::unpredictable) ends = ends && row.fold_reason == "line-ends";
        Check(ends, "fold that ends at the seam: the rows near it are unpredictable for line-ends");
    }
    // (c) The guards' switches on a real fold: no ratio passes, or no change is large enough.
    {
        const auto b = Build(SceneNamed("fold_horizontal"));
        auto strict = b.request;
        strict.design.fold_maximum_rms_ratio = 1e-9;
        const auto r = pc::EstimateParallax(strict);
        bool explained = r.summary.rows_fold_unpredictable > 0;
        for (const auto& row : r.rows) if (row.fold == pc::ParallaxRowFold::unpredictable) explained = explained && row.fold_reason == "not-explained";
        Check(unchanged(r) && explained, "ratio 0: every row tried is not-explained and keeps 11.4's estimate");
        auto still = b.request;
        still.design.fold_minimum_change_px = 1e3;
        const auto a = pc::EstimateParallax(still);
        Check(unchanged(a) && a.summary.rows_fold_agree == tried(a) && tried(a) > 0, "a least change of 1000 px: every row tried agrees and keeps 11.4's estimate");
        auto none = b.request;
        none.design.fold_angle_refinement_steps = 0;
        const auto n = pc::EstimateParallax(none);
        bool angle = n.summary.rows_fold_predicted > 0;
        for (const auto& row : n.rows) if (row.fold_line) angle = angle && row.fold_angle_deg == n.folds[*row.fold_line].angle_deg;
        Check(angle, "no refinement: the rows use the line's own angle");
    }
    // (d) A fold on short waves: along the line the shape is not the same, so the rows the surface along the line does not explain
    // keep 11.4's estimate, and no row ends worse than 11.4's alone by more than the noise.
    {
        auto spec = SceneNamed("fold_horizontal");
        for (const auto& u : SceneNamed("wave_short").field.undulations) spec.field.undulations.push_back(u);
        const auto b = Build(spec);
        const auto r = pc::EstimateParallax(b.request);
        double worse = 0, iso = 0, now = 0;
        for (std::size_t i = 0; i < r.rows.size(); ++i) {
            if (!b.covered[i]) continue;
            const double e = std::abs(b.q_true[i]-r.rows[i].q), ei = std::abs(b.q_true[i]-r.rows[i].q_isotropic);
            worse = std::max(worse, e-ei);
            iso = std::max(iso, ei);
            now = std::max(now, e);
        }
        std::printf("fold on short waves: %s; predicted %u, agree %u, unpredictable %u (%s); 11.4 alone %.3f px, with C1 %.3f px, a row worse by at most %.3f px\n",
            FoldText(r).c_str(), r.summary.rows_fold_predicted, r.summary.rows_fold_agree, r.summary.rows_fold_unpredictable, reasons(r).c_str(), iso, now, worse);
        Check(now <= iso, "fold on short waves: the largest leftover is not larger than 11.4's alone");
    }
}

// The hole of the row's own footprint (the join left out, which leaks: GateIndependence) against the design's (the join's rows),
// with the gate's rows 32 px apart: how far layer 2 reaches with each. Reported for the designer; the folds across the seam stay
// corrected.
void WidenedHole() {
    const auto& s = TheSetup();
    const double gx_mm = HoleX()*cr::FrameOfRaster(s.rig.raster).pitch_mm;
    BuildOptions product;
    product.gate_row_step = gate::kSeamGateDesignCalibrated.point_spacing_px;
    const std::uint32_t draft_reach = product.design.hole_join_reach_rows;
    std::vector<std::pair<std::string, ps::SceneSpec>> scenes{
        {"wave_long", SceneNamed("wave_long")}, {"book", SceneNamed("book")}, {"fold_horizontal", SceneNamed("fold_horizontal")},
        {"fold_diagonal", SceneNamed("fold_diagonal")}, {"fold_diagonal_2gx", ps::FoldScene(s.seam, 35.0, 2*gx_mm)}, {"fold_vertical", SceneNamed("fold_vertical")},
        {"fold_vertical_2gx", ps::FoldScene(s.seam, 90.0, 2*gx_mm)}, {"fold_87.5deg", ps::FoldScene(s.seam, 87.5, 0.0)},
        {"fold_horizontal_narrow", ps::FoldScene(s.seam, 0.0, 0.0, 1.5, 4.0)}, {"step_horizontal", SceneNamed("step_horizontal")}};
    BuildOptions drafted = product;
    pc::ParallaxRows grid;
    grid.gate_footprint = pc::GateFootprint(gate::kSeamGateDesignCalibrated, drafted.gate_remeasured);
    grid.row_step = drafted.gate_row_step;
    const auto rows_reached = pc::HoleNeighbourRows(grid, drafted.design).below;
    std::printf("\nhole of the row's own footprint and the design's (gate rows every %u px): the largest leftover along e_b at the seam column, px\n", product.gate_row_step);
    std::printf("| scene | rows | own footprint: 11.4 alone | with C1 | predicted rows | unpredictable | the join's %u rows: 11.4 alone | with C1 | predicted rows | unpredictable |\n", rows_reached);
    std::printf("|---|---|---|---|---|---|---|---|---|---|\n");
    for (const auto& [name, spec] : scenes) {
        std::printf("| %s |", name.c_str());
        for (const std::uint32_t reach : {0u, draft_reach}) {
            BuildOptions o = product;
            o.design.hole_join_reach_rows = reach;
            const std::uint32_t neighbours = reach == 0 ? 0 : rows_reached;
            const auto b = Build(spec, o);
            const auto r = pc::EstimateParallax(b.request);
            double iso = 0;
            for (std::size_t i = 0; i < b.q_true.size(); ++i) if (b.covered[i]) iso = std::max(iso, std::abs(b.q_true[i]-r.rows[i].q_isotropic));
            const auto f = ByFold(b, r);
            const auto out = Evaluate(b, r);
            if (neighbours == 0) std::printf(" %zu |", r.rows.size());
            std::printf(" %.3f | %.3f | %.3f (%u) | %.3f (%u%s) |", iso, out.after, f.predicted, r.summary.rows_fold_predicted, f.unpredictable,
                r.summary.rows_fold_unpredictable, f.reasons.empty() ? "" : (", "+f.reasons).c_str());
            const bool across = name == "fold_horizontal" || name == "fold_diagonal" || name == "fold_diagonal_2gx" || name == "step_horizontal" || name == "wave_long";
            if (across) Check(out.after <= kTargetPx, name+": corrected with "+std::to_string(neighbours)+" neighbour rows in the hole");
            if (name == "fold_vertical") Check(f.predicted <= kTargetPx && r.summary.rows_fold_predicted > 0, name+": predicted rows corrected with "+
                std::to_string(neighbours)+" neighbour rows in the hole");
        }
        std::printf("\n");
        std::fflush(stdout);
    }
    // The leak through the join with the design's hole, on a fold across the seam (the row C1 moves most).
    const BuildOptions& o = product;
    const auto b = Build(SceneNamed("fold_horizontal"), o);
    const auto base = pc::EstimateParallax(b.request);
    std::size_t row = 0;
    for (std::size_t i = 0; i < base.rows.size(); ++i) if (std::abs(base.rows[i].q-base.rows[i].q_isotropic) > std::abs(base.rows[row].q-base.rows[row].q_isotropic)) row = i;
    const double leak = InterpolationLeak(b.request, row, 3.0, b.e_b);
    std::printf("coupling through the join with the design's hole (%u rows), fold_horizontal, at the row C1 moves most (%s): %.3f px\n", rows_reached,
        std::string(pc::ParallaxRowFoldCode(base.rows[row].fold)).c_str(), leak);
    Check(leak == 0, "fold_horizontal, the design's hole: the rows a predicted row's gate point reads see none of an error on its pixels");
}

// ---- Determinism -------------------------------------------------------------------------------------------------------------------------------
bool SameResult(const pc::ParallaxResult& a, const pc::ParallaxResult& b) {
    if (a.state != b.state || a.reasons != b.reasons || a.rows.size() != b.rows.size()) return false;
    for (std::size_t i = 0; i < a.rows.size(); ++i) {
        const auto& x = a.rows[i];
        const auto& y = b.rows[i];
        if (std::memcmp(&x.q, &y.q, sizeof(double)) != 0 || std::memcmp(&x.sigma_q, &y.sigma_q, sizeof(double)) != 0 ||
            std::memcmp(&x.n_eff, &y.n_eff, sizeof(double)) != 0 || std::memcmp(&x.lift_mm, &y.lift_mm, sizeof(double)) != 0 ||
            x.points != y.points || x.state != y.state || x.reason != y.reason || x.applied != y.applied) return false;
        if (std::memcmp(&x.q_isotropic, &y.q_isotropic, sizeof(double)) != 0 || std::memcmp(&x.fold_angle_deg, &y.fold_angle_deg, sizeof(double)) != 0 ||
            x.fold != y.fold || x.fold_reason != y.fold_reason || x.fold_line != y.fold_line) return false;
    }
    if (a.folds.size() != b.folds.size()) return false;
    for (std::size_t i = 0; i < a.folds.size(); ++i) {
        const auto& x = a.folds[i];
        const auto& y = b.folds[i];
        if (std::memcmp(&x.first, &y.first, sizeof(Point)) != 0 || std::memcmp(&x.last, &y.last, sizeof(Point)) != 0 ||
            std::memcmp(&x.step_px, &y.step_px, sizeof(double)) != 0 || x.angle_deg != y.angle_deg || x.points != y.points ||
            x.rows_predicted != y.rows_predicted || x.rows_agree != y.rows_agree || x.rows_unpredictable != y.rows_unpredictable) return false;
    }
    return std::memcmp(&a.lift.maximum_mm, &b.lift.maximum_mm, sizeof(double)) == 0 && a.summary.points == b.summary.points &&
        std::memcmp(&a.summary.max_shift_px, &b.summary.max_shift_px, sizeof(double)) == 0 &&
        std::memcmp(&a.summary.fold_residual_scale_px, &b.summary.fold_residual_scale_px, sizeof(double)) == 0;
}
void Determinism() {
    for (const char* name : {"wave_long", "fold_horizontal", "fold_vertical", "lines_only"}) {
        BuildOptions one, four;
        one.thread_count = 1;
        four.thread_count = 4;
        const auto b1 = Build(SceneNamed(name), one), b4 = Build(SceneNamed(name), four);
        const auto r1 = pc::EstimateParallax(b1.request), r1b = pc::EstimateParallax(b1.request), r4 = pc::EstimateParallax(b4.request);
        Check(SameResult(r1, r1b), std::string(name)+": two runs give the same bytes");
        Check(SameResult(r1, r4), std::string(name)+": 1 and 4 threads give the same bytes");
    }
}

// ---- The split: verify rows and correct rows (11.4, 11.5) ---------------------------------------------------------------------------------
// Gate rows every 32 px (the gate design's spacing), one verify row in six (the capture path's design). The verify rows are derived
// here from the period and the phase, and their pixels from the gate lane's own patches (RowPixels, GatePatchOf), not from the lane.
constexpr std::uint32_t kVerifyPeriod = 6;
BuildOptions SplitOptions(std::uint32_t phase = 0) {
    BuildOptions o;
    o.gate_row_step = gate::kSeamGateDesignCalibrated.point_spacing_px;
    o.verify_period_rows = kVerifyPeriod;
    o.verify_phase = phase;
    return o;
}
bool SharesWithVerifyRows(const pc::ParallaxRows& g, std::uint32_t period, std::uint32_t phase, const pc::ParallaxPoint& p, double margin) {
    for (std::size_t i = phase; i < g.row_count; i += period) if (!rg::PatchesShareNoPixel(RowPixels(g, i, margin), p.footprint)) return true;
    return false;
}
// The request's points that share a pixel with its verify rows' gate points get `error` px along e_b.
std::size_t SpoilVerifyPoints(pc::ParallaxRequest& r, std::uint32_t period, std::uint32_t phase, double error, const Point& e_b) {
    std::size_t touched = 0;
    const auto before = r.points;
    for (std::size_t i = 0; i < r.points.size(); ++i) {
        if (!SharesWithVerifyRows(r.rows, period, phase, before[i], r.design.hole_margin_px)) continue;
        r.points[i].displacement.x += error*e_b.x;
        r.points[i].displacement.y += error*e_b.y;
        ++touched;
    }
    return touched;
}

// The points: no point with a weight in any row's estimate (the regression of 11.4, and the one along a line of a predicted row)
// shares a pixel with a verify row, and the points left out are exactly those that do.
void SplitPoints() {
    for (const char* name : {"wave_long", "fold_horizontal", "fold_vertical", "book"}) for (const std::uint32_t phase : {0u, 3u}) {
        const auto b = Build(SceneNamed(name), SplitOptions(phase));
        const auto& r = b.request;
        const auto& g = r.rows;
        std::size_t sharing = 0;
        for (const auto& p : r.points) sharing += SharesWithVerifyRows(g, kVerifyPeriod, phase, p, r.design.hole_margin_px);
        const auto result = pc::EstimateParallax(r);
        std::size_t listed = 0, used = 0, along = 0, without_hole = 0;
        for (std::size_t row = 0; row < g.row_count; ++row) {
            std::vector<pc::ParallaxPointWeight> weights = pc::ParallaxRowWeights(r, row);
            if (result.rows[row].fold == pc::ParallaxRowFold::predicted) {
                const auto fit = pc::ParallaxRowFitOf(r, row, result.rows[row].fold_angle_deg);
                weights.insert(weights.end(), fit.weights.begin(), fit.weights.end());
                ++along;
            }
            for (const auto& w : weights) {
                ++listed;
                if (SharesWithVerifyRows(g, kVerifyPeriod, phase, r.points[w.point], r.design.hole_margin_px)) ++used;
            }
            without_hole += !result.rows[row].hole;
        }
        const std::string label = std::string("split, ")+name+", phase "+std::to_string(phase);
        Check(sharing > 0 && result.summary.points_excluded == sharing, label+": the points left out are those that share a pixel with a verify row ("+
            std::to_string(result.summary.points_excluded)+" of "+std::to_string(sharing)+")");
        Check(listed > 0 && used == 0, label+": no point in any row's estimate shares a pixel with a verify row ("+std::to_string(used)+" of "+std::to_string(listed)+")");
        Check(without_hole > 0, label+": rows are estimated without a hole of their own");
        if (std::string(name) != "wave_long" && std::string(name) != "book") Check(along > 0, label+": rows are predicted along a line, and checked");
    }
}

// An error on the points that share a pixel with a verify row (3 px and 0.2 px along e_b) leaves every row's q, the fold lines and
// the rows filled from their neighbours as they were, bit for bit. The same error is seen (the check can fail) when the estimate
// leaves out the points of other rows: without the split, or with the verify rows one row off.
void SplitErrors() {
    const auto& s = TheSetup();
    const auto frame = cr::FrameOfRaster(s.rig.raster);
    const double mid = (s.seam.y_mid_mm-frame.top_mm)/frame.pitch_mm-0.5;
    auto gap = SplitOptions();
    gap.keep = [mid](double y, double) {return std::abs(y-mid) > 160;};   // a white gap: rows filled from their neighbours
    std::vector<std::pair<std::string, Built>> cases;
    for (const char* name : {"wave_long", "fold_horizontal", "fold_diagonal", "step_horizontal", "fold_vertical", "book"})
        cases.emplace_back(name, Build(SceneNamed(name), SplitOptions()));
    cases.emplace_back("wave_long with a white gap", Build(SceneNamed("wave_long"), gap));
    for (const auto& [name, b] : cases) {
        const auto base = pc::EstimateParallax(b.request);
        for (const double error : {3.0, 0.2}) {
            auto spoiled = b.request;
            const auto touched = SpoilVerifyPoints(spoiled, kVerifyPeriod, 0, error, b.e_b);
            const auto r = pc::EstimateParallax(spoiled);
            Check(touched > 0 && SameResult(base, r), "split, "+name+": an error of "+std::to_string(error)+" px on the verify rows' points changes no bit");
        }
        if (name == "wave_long with a white gap") Check(base.summary.rows_filled > 0, "split, a white gap: rows are filled from their neighbours");
    }
    for (const char* name : {"wave_long", "fold_horizontal"}) {
        const auto b = Build(SceneNamed(name), SplitOptions());
        for (const char* mutation : {"no split", "one row off"}) {
            auto wrong = b.request;
            if (std::string(mutation) == "no split") wrong.rows.verify_period_rows = 0; else wrong.rows.verify_phase = 1;
            const auto base = pc::EstimateParallax(wrong);
            auto spoiled = wrong;
            (void)SpoilVerifyPoints(spoiled, kVerifyPeriod, 0, 3.0, b.e_b);
            Check(!SameResult(base, pc::EstimateParallax(spoiled)), std::string("split, ")+name+", "+mutation+": the error is seen (the check can fail)");
        }
    }
}

// The roles follow from the period, the phase and the lattice: not from a measured value. Without the split every row is a verify
// row with a hole. A phase at or above the period is refused.
void SplitRoles() {
    const auto b = Build(SceneNamed("wave_long"), SplitOptions(2));
    const auto& g = b.request.rows;
    const auto r = pc::EstimateParallax(b.request);
    auto moved = b.request;
    for (auto& p : moved.points) p.displacement.x += 5;
    const auto m = pc::EstimateParallax(moved);
    bool roles = r.rows.size() == g.row_count && m.rows.size() == g.row_count;
    std::uint32_t verify = 0;
    for (std::size_t i = 0; roles && i < g.row_count; ++i) {
        const bool v = i%kVerifyPeriod == 2;
        verify += v;
        roles = pc::IsVerifyRow(g, i) == v && (r.rows[i].role == pc::ParallaxRowRole::verify) == v && m.rows[i].role == r.rows[i].role;
    }
    Check(roles, "split roles: row i is a verify row exactly when i mod 6 is the phase, whatever the points' values");
    Check(r.summary.rows_verify == verify && r.summary.rows_correct == g.row_count-verify && r.verify_period_rows == kVerifyPeriod && r.verify_phase == 2,
        "split roles: the summary counts them, and the result records the period and the phase");
    const auto plain = pc::EstimateParallax(Build(SceneNamed("wave_long"), BuildOptions{}).request);
    bool all = plain.summary.rows_correct == 0 && plain.summary.points_excluded == 0 && plain.summary.rows_with_hole == plain.rows.size();
    for (const auto& row : plain.rows) all = all && row.role == pc::ParallaxRowRole::verify && row.hole;
    Check(all, "no split: every row is a verify row with a hole of its own, and no point is left out");
    auto bad = b.request;
    bad.rows.verify_phase = kVerifyPeriod;
    Check(Throws([&] {(void)pc::EstimateParallax(bad);}) && Throws([&] {(void)pc::IsVerifyRow(bad.rows, 0);}), "split roles: a phase at the period is refused");
    // A verify row's pixels are every column of Omega with the gate's footprint (the gate lane's patches).
    const auto v = pc::VerifyRowPixels(g, 2);
    const auto own = RowPixels(g, 2, 0);
    Check(v.x0 == own.x0 && v.y0 == own.y0 && v.x1 == own.x1 && v.y1 == own.y1, "split roles: VerifyRowPixels is the gate's patches over Omega's columns");
}

// The edge of the content (the design's rule, *unverified* on real pairs): a row the seam column's points do not enclose (none on
// the row, and none on one side within the kernel's reach) keeps the hole of the rule without the split; rows inside do not.
void SplitEdge() {
    const auto& s = TheSetup();
    const auto frame = cr::FrameOfRaster(s.rig.raster);
    const double cut = (s.seam.y_mid_mm-frame.top_mm)/frame.pitch_mm-0.5;
    auto o = SplitOptions();
    o.keep = [cut](double y, double) {return y <= cut;};
    const auto b = Build(SceneNamed("wave_long"), o);
    const auto r = pc::EstimateParallax(b.request);
    const double half = b.request.design.column_spacing_px/2, reach = b.request.design.kernel_reach_sigmas*b.request.design.kernel_sigma_y_px;
    bool below = true, inside = false;
    std::size_t edge_rows = 0;
    for (const auto& row : r.rows) {
        if (row.y >= cut+half) { below = below && row.hole; ++edge_rows; }
        if (row.y <= cut-2*reach && !row.hole) inside = true;
    }
    Check(edge_rows > 0 && below, "split edge: the rows past the last points along the seam keep a hole of their own");
    Check(inside, "split edge: rows inside the content are estimated without a hole");
    Check(r.summary.rows_with_hole >= edge_rows, "split edge: the summary counts the rows with a hole");
}

void SplitDeterminism() {
    for (const char* name : {"wave_long", "fold_horizontal"}) {
        auto one = SplitOptions(), four = SplitOptions();
        one.thread_count = 1;
        four.thread_count = 4;
        const auto r1 = pc::EstimateParallax(Build(SceneNamed(name), one).request), r4 = pc::EstimateParallax(Build(SceneNamed(name), four).request);
        Check(SameResult(r1, r4), std::string("split, ")+name+": 1 and 4 threads give the same bytes");
    }
}

// How far layer 2 reaches with the split against the rule without it (both with gate rows every 32 px): the largest leftover along
// e_b at the seam column over the verify rows (the rows the gate judges with the split) and over every row.
void SplitTable() {
    const auto& s = TheSetup();
    const double gx_mm = HoleX()*cr::FrameOfRaster(s.rig.raster).pitch_mm;
    std::vector<std::pair<std::string, ps::SceneSpec>> scenes{
        {"wave_long", SceneNamed("wave_long")}, {"book", SceneNamed("book")}, {"fold_horizontal", SceneNamed("fold_horizontal")},
        {"fold_diagonal", SceneNamed("fold_diagonal")}, {"fold_diagonal_2gx", ps::FoldScene(s.seam, 35.0, 2*gx_mm)}, {"fold_vertical", SceneNamed("fold_vertical")},
        {"fold_vertical_2gx", ps::FoldScene(s.seam, 90.0, 2*gx_mm)}, {"fold_horizontal_narrow", ps::FoldScene(s.seam, 0.0, 0.0, 1.5, 4.0)},
        {"step_horizontal", SceneNamed("step_horizontal")}};
    std::printf("\nthe split (one verify row in %u) against the hole of every row, gate rows every %u px: the largest leftover along e_b at the seam column, px\n",
        kVerifyPeriod, gate::kSeamGateDesignCalibrated.point_spacing_px);
    std::printf("| scene | rows | before | hole: verify rows | hole: every row | split: verify rows | split: every row | split: rows with a hole |\n|---|---|---|---|---|---|---|---|\n");
    for (const auto& [name, spec] : scenes) {
        auto hole = SplitOptions();
        hole.verify_period_rows = 0;
        const auto bh = Build(spec, hole), bs = Build(spec, SplitOptions());
        const auto rh = pc::EstimateParallax(bh.request), rs = pc::EstimateParallax(bs.request);
        const auto verify = [&](double y) {return static_cast<std::uint32_t>(std::lround((y-bs.request.rows.first_row)/bs.request.rows.row_step))%kVerifyPeriod == 0;};
        const auto h_v = Evaluate(bh, rh, verify), h_all = Evaluate(bh, rh), s_v = Evaluate(bs, rs, verify), s_all = Evaluate(bs, rs);
        std::printf("| %s | %zu | %.3f | %.3f | %.3f | %.3f | %.3f | %u |\n", name.c_str(), rs.rows.size(), s_all.before, h_v.after, h_all.after, s_v.after, s_all.after,
            rs.summary.rows_with_hole);
        std::fflush(stdout);
        const bool across = name == "fold_horizontal" || name == "fold_diagonal" || name == "fold_diagonal_2gx" || name == "step_horizontal" || name == "wave_long";
        if (across) Check(s_v.after <= kTargetPx, name+": the split corrects the verify rows of a field across the seam");
    }
}

// ---- Time per number of points (--timing; not a check) ----------------------------------------------------------------------------------
// A synthetic request on an output of A0's width at the product's resolution: points on the design's lattice within the band, gate
// rows at the gate design's spacing. The folds' leave-one-out pass is included (it is part of EstimateParallax).
// The field: smooth waves, plus a fold (a ridge of 3.6 px, 57 px half width) along the seam column (every gate row is tried along
// it, C1's most) or across it.
enum class TimingField {smooth, fold_along_seam, fold_across_seam};
const char* TimingFieldCode(TimingField f) {return f == TimingField::smooth ? "smooth" : f == TimingField::fold_along_seam ? "fold along the seam" : "fold across the seam";}
int Timing() {
    std::printf("| output rows | band half width px | lattice px | field | gate rows | points | threads | ms | lines | rows tried along a line |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|\n");
    struct Shape {double radius, spacing;};
    struct Run {std::uint32_t height; Shape shape; TimingField field;};
    std::vector<Run> runs;
    for (const Shape shape : {Shape{192, 16}, Shape{160, 16}, Shape{128, 16}, Shape{192, 32}}) runs.push_back({2000, shape, TimingField::smooth});
    for (const Shape shape : {Shape{192, 16}, Shape{160, 16}, Shape{128, 16}, Shape{192, 32}})
        for (const TimingField field : {TimingField::smooth, TimingField::fold_along_seam, TimingField::fold_across_seam}) runs.push_back({6400, shape, field});
    for (const auto& [height, shape, field] : runs) {
        pc::ParallaxRequest r;
        r.design = pc::kParallaxDesignDraft;
        r.design.column_radius_px = shape.radius;
        r.design.column_spacing_px = shape.spacing;
        r.max_lift_mm = 1.0;
        r.apply_beyond_line = true;
        r.baseline.direction = {1, 0};
        r.baseline.length_mm = 250;
        r.baseline.camera_a_height_mm = r.baseline.camera_b_height_mm = 700;
        r.baseline.pixels_per_mm = 8;
        r.baseline.k_px_per_mm = 8*250/700.0;
        r.rows.output_width = 9000;
        r.rows.output_height = height;
        r.rows.seam_column = 4500;
        r.rows.omega_half_width_px = kOmega;
        r.rows.gate_footprint = pc::GateFootprint(gate::kSeamGateDesignCalibrated, true);
        r.rows.first_row = 64;
        r.rows.row_step = gate::kSeamGateDesignCalibrated.point_spacing_px;
        r.rows.row_count = (height-128)/r.rows.row_step;
        const int steps = static_cast<int>(shape.radius/shape.spacing);
        for (double y = 16; y < height-16; y += shape.spacing) for (int j = -steps; j <= steps; ++j) {
            pc::ParallaxPoint p;
            const double x = 4500.0+shape.spacing*j;
            p.position = {x, y};
            double z = 0.4*std::sin(y/300.0)+0.2*std::cos(x/80.0);
            const auto ridge = [](double d) {const double t = d/57.0; return std::abs(t) < 1 ? -3.6*(1-t*t)*(1-t*t) : 0.0;};
            if (field == TimingField::fold_along_seam) z += ridge(x-4500.0);
            if (field == TimingField::fold_across_seam) z += ridge(y-height/2.0);
            p.displacement = {z+0.05*Noise(static_cast<std::uint64_t>(y), static_cast<std::uint64_t>(j+40), 3), 0};
            p.uncertainty_px = kPointSigma;
            p.footprint = pc::PointFootprint(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), 32);
            r.points.push_back(p);
        }
        for (const unsigned threads : {1u, 4u}) {
            r.thread_count = threads;
            const auto start = std::chrono::steady_clock::now();
            const auto result = pc::EstimateParallax(r);
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
            std::printf("| %u | %.0f | %.0f | %s | %u | %zu | %u | %.1f | %zu | %u |\n", height, shape.radius, shape.spacing, TimingFieldCode(field), r.rows.row_count,
                r.points.size(), threads, ms, result.folds.size(), result.summary.rows_fold_predicted+result.summary.rows_fold_agree+result.summary.rows_fold_unpredictable);
            std::fflush(stdout);
            if (!result.Applied()) std::printf("(not applied: %s)\n", Reasons(result).c_str());
        }
    }
    return 0;
}

// ---- How far layer 2 reaches with the hole, the band and the lattice (--sweep; not a check) ---------------------------------------
// The largest leftover along the baseline at the seam column (px) on the scenes that layer 2 is meant to correct, and how many rows
// have an estimate of their own, for the gate's footprint with and without the remeasurement, the join's reach in the hole (0: the
// row's own footprint, which leaks), the band's half width and the lattice. Gate rows every 32 px (the gate design's spacing).
int Sweep() {
    const auto& s = TheSetup();
    const double pitch = cr::FrameOfRaster(s.rig.raster).pitch_mm;
    struct Variant {const char* name; bool remeasured; std::uint32_t reach;};
    struct Shape {double radius, spacing;};
    std::printf("| gate footprint | join reach rows | g_x px | g_y px | band half width px | lattice px | points | wave_long | wide_bump | book | fold 2 g_x | step far rows | rows measured |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (const Variant v : {Variant{"primary only", false, 0}, Variant{"primary only", false, 2}, Variant{"with remeasurement", true, 0},
             Variant{"with remeasurement", true, 2}})
    for (const Shape shape : {Shape{128, 16}, Shape{160, 16}, Shape{192, 16}, Shape{224, 16}, Shape{192, 24}, Shape{192, 32}}) {
        BuildOptions o;
        o.gate_remeasured = v.remeasured;
        o.gate_row_step = gate::kSeamGateDesignCalibrated.point_spacing_px;
        o.design.hole_join_reach_rows = v.reach;
        o.design.column_radius_px = shape.radius;
        o.design.column_spacing_px = shape.spacing;
        const double gx = HoleX(o), gy = HoleY(o);
        std::size_t points = 0;
        std::uint32_t measured = 0, rows = 0;
        const auto after = [&](const ps::SceneSpec& spec, const std::function<bool(double)>& filter = {}) {
            const auto b = Build(spec, o);
            const auto r = pc::EstimateParallax(b.request);
            points = b.request.points.size();
            measured += r.summary.rows_measured;
            rows += static_cast<std::uint32_t>(r.rows.size());
            return Evaluate(b, r, filter).after;
        };
        const double step_row = (s.seam.y_mid_mm-cr::FrameOfRaster(s.rig.raster).top_mm)/pitch-0.5;
        const double near = o.design.kernel_reach_sigmas*o.design.kernel_sigma_y_px+gy;
        const double wave = after(SceneNamed("wave_long")), bump = after(WideBump(2.0)), book = after(SceneNamed("book"));
        const double fold = after(ps::FoldScene(s.seam, 90.0, 2*gx*pitch));
        const double step = after(SceneNamed("step_horizontal"), [&](double y) {return std::abs(y-step_row) > near;});
        std::printf("| %s | %u | %.0f | %.0f | %.0f | %.0f | %zu | %.3f | %.3f | %.3f | %.3f | %.3f | %u/%u |\n", v.name, v.reach, gx, gy, shape.radius,
            shape.spacing, points, wave, bump, book, fold, step, measured, rows);
        std::fflush(stdout);
    }
    return 0;
}

// ---- The rows' bits per scene (--hash; not a check) ---------------------------------------------------------------------------------
std::uint64_t Fnv(std::uint64_t h, const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
int Hash() {
    const auto& s = TheSetup();
    const double gx_mm = HoleX()*cr::FrameOfRaster(s.rig.raster).pitch_mm;
    std::vector<std::pair<std::string, ps::SceneSpec>> scenes;
    for (const char* name : {"zero", "wave_long", "wave_long_dy_error", "wave_short", "seam_bump", "fold_horizontal", "fold_diagonal", "fold_vertical",
             "step_horizontal", "lines_only", "book"})
        scenes.push_back({name, SceneNamed(name)});
    scenes.push_back({"fold_vertical_2gx", ps::FoldScene(s.seam, 90.0, 2*gx_mm)});
    scenes.push_back({"fold_diagonal_2gx", ps::FoldScene(s.seam, 35.0, 2*gx_mm)});
    scenes.push_back({"wide_bump", WideBump(2.0)});
    for (const auto& [name, spec] : scenes) {
        const auto r = pc::EstimateParallax(Build(spec).request);
        std::uint64_t h = 1469598103934665603ull;
        for (const auto& row : r.rows) {
            h = Fnv(h, &row.q, sizeof row.q);
            h = Fnv(h, &row.sigma_q, sizeof row.sigma_q);
            h = Fnv(h, &row.n_eff, sizeof row.n_eff);
            const int state = static_cast<int>(row.state);
            h = Fnv(h, &state, sizeof state);
        }
        std::printf("%-20s %016llx\n", name.c_str(), static_cast<unsigned long long>(h));
    }
    return 0;
}
}

int main(int argc, char** argv) {
    try {
        if (argc > 2 && std::string(argv[1]) == "--only") {
            const std::string group = argv[2];
            if (group == "SceneTable") SceneTable();
            else if (group == "HoleInvariant") HoleInvariant();
            else if (group == "GateIndependence") GateIndependence();
            else if (group == "FoldRowMarks") FoldRowMarks();
            else if (group == "GateIndependenceAlongLines") GateIndependenceAlongLines();
            else if (group == "SmoothFieldsUnchanged") SmoothFieldsUnchanged();
            else if (group == "FalseLines") FalseLines();
            else if (group == "WidenedHole") WidenedHole();
            else if (group == "JoinReach") JoinReach();
            else if (group == "HoleJoin") HoleJoin();
            else if (group == "LineIndependence") LineIndependence();
            else if (group == "Determinism") Determinism();
            else if (group == "Split") { SplitPoints(); SplitErrors(); SplitRoles(); SplitEdge(); SplitDeterminism(); SplitTable(); }
            else { std::cerr << "no group " << group << "\n"; return 2; }
            if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
            std::cout << "parallax correction contracts (" << group << "): all checks passed\n";
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--hash") return Hash();
        if (argc > 1 && std::string(argv[1]) == "--timing") return Timing();
        if (argc > 1 && std::string(argv[1]) == "--sweep") return Sweep();
        if (argc > 1 && std::string(argv[1]) == "--leaks") return Leaks(argc > 2 ? argv[2] : "");
        SceneTable();
        OrthogonalError();
        ZeroIsExact();
        Baseline();
        HoleInvariant();
        GateIndependence();
        JoinReach();
        HoleJoin();
        LineIndependence();
        LiftLine();
        States();
        Refusals();
        Fill();
        FromMeasurement();
        FoldLines();
        FoldRowMarks();
        GateIndependenceAlongLines();
        SmoothFieldsUnchanged();
        FalseLines();
        WidenedHole();
        Determinism();
        SplitPoints();
        SplitErrors();
        SplitRoles();
        SplitEdge();
        SplitDeterminism();
        SplitTable();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: unexpected exception: " << e.what() << "\n";
        return 1;
    }
    if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "parallax correction contracts: all checks passed\n";
    return 0;
}
