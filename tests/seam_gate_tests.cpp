#include "a0/m2/seam_gate.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include "seam_gate_scene.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace gate = a0::m2::seam_gate;
namespace render = a0::m2::render;
namespace scene = seam_gate_scene;
using render::BgrImage;
using render::Point;

namespace {
int checks = 0, failures = 0;
void Check(bool value, const std::string& what) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL " << what << '\n'; }
}
template <class F> void Rejects(F call, const std::string& what) {
    bool threw = false;
    try { call(); } catch (const std::invalid_argument&) { threw = true; }
    Check(threw, what);
}
// Owner's decision #385: limit 1 px, measurement uncertainty 0.2 px.
const gate::SeamGateLimits kLimits{1.0, 0.2, "#385"};
unsigned Threads() { return std::max(2u, std::min(8u, std::thread::hardware_concurrency())); }
// The scenes of the local-measurement tests are smaller than the production
// wide window of the global placement check (768 px must fit in camera B), so
// they use the production constants with a wide radius of 128 px and 2 anchors.
// The production constants run on production-size scenes in GlobalPlacement*
// and Timing.
gate::SeamGateDesign Reduced(gate::SeamGateDesign d) { d.global_search_radius_px = 128; d.global_minimum_anchors = 2; return d; }
const gate::SeamGateDesign kCalibrated = Reduced(gate::kSeamGateDesignCalibrated);
const gate::SeamGateDesign kUncalibrated = Reduced(gate::kSeamGateDesignUncalibrated);

using Field = std::function<Point(double, double)>;
Field Constant(double x, double y) { return [x, y](double, double) { return Point{x, y}; }; }
struct Views { BgrImage a, b; double tx{}; unsigned w{}, h{}; };
// A sits at the world origin; B is translated by tx and shows the world
// point G - delta(G) at the position G where the pair mapping places it, so
// a feature of A at p appears in placed B at p + delta.
Views MakeViews(unsigned w, unsigned h, double tx, const std::vector<scene::Stroke>& a_strokes,
    const std::vector<scene::Stroke>& b_strokes, const Field& delta, double bound, double noise) {
    Views v{{}, {}, tx, w, h};
    v.a = scene::Render(w, h, 0, 0, 0, a_strokes, [](int i, int j, double& x, double& y) { x = i; y = j; }, 11, noise);
    v.b = scene::Render(w, h, tx, 0, bound, b_strokes, [&](int i, int j, double& x, double& y) {
        const double gx = i+tx, gy = j; const auto d = delta(gx, gy); x = gx-d.x; y = gy-d.y; }, 23, noise);
    return v;
}
render::PairRenderParameters PairParameters(double tx) {
    return {{1, 0, tx, 0, 1, 0, 0, 0, 1}, a0::m2::StitchLayout::camera_a_left_camera_b_right, {}};
}
gate::SeamGateGeometry Geometry(const Views& v, std::uint32_t inset) {
    auto g = gate::PairSeamGeometry(v.w, v.h, v.w, v.h, PairParameters(v.tx));
    g.band = gate::OverlapBand(g, v.w, v.h, v.w, v.h, inset);
    g.transition = g.band; // the composite mixes only inside the measured band
    return g;
}
struct Run { gate::SeamMeasurement m; gate::SeamGateResult r; };
Run Measure(const Views& v, const gate::SeamGateDesign& design, const gate::DisplacementPrediction& prediction,
    unsigned threads = Threads(), std::uint32_t inset = 0) {
    const auto g = Geometry(v, inset ? inset : gate::RequiredBandInsetPx(design));
    auto m = gate::MeasureSeam(v.a, v.b, g, design, kLimits, prediction, threads);
    auto r = gate::DecideSeamGate(m, kLimits);
    return {std::move(m), std::move(r)};
}
bool HasReason(const gate::SeamGateResult& r, const std::string& reason) {
    for (const auto& f : r.findings) if (f.reason == reason) return true;
    return false;
}
std::string Reasons(const gate::SeamGateResult& r) {
    std::string s;
    for (const auto& f : r.findings) s += f.reason+" ";
    return s;
}
// Error of every counted point against the known constant field.
struct Accuracy { std::size_t two{}, one{}; double maximum{}, sum_x{}, sum_y{}, sum_sq{}; std::size_t n{}; };
Accuracy Errors(const gate::SeamMeasurement& m, Point delta) {
    Accuracy a;
    for (const auto& p : m.points) {
        if (!p.Counted()) continue;
        const Point e{p.displacement.x-delta.x, p.displacement.y-delta.y};
        double err;
        if (p.kind == gate::PointKind::one_directional) { ++a.one; err = std::abs(p.normal.x*e.x+p.normal.y*e.y); }
        else { ++a.two; err = std::hypot(e.x, e.y); a.sum_x += e.x; a.sum_y += e.y; a.sum_sq += e.x*e.x+e.y*e.y; ++a.n; }
        a.maximum = std::max(a.maximum, err);
    }
    return a;
}
void Report(const char* name, const Run& run) {
    std::size_t counts[10]{};
    for (const auto& p : run.m.points) ++counts[static_cast<int>(p.status)];
    std::cout << name << ": state=" << gate::SeamGateStateCode(run.r.state) << " reasons=" << Reasons(run.r)
              << " max=" << run.r.measured_maximum_px.value_or(-1) << " counted=" << run.r.counted_points
              << " confirmed=" << run.r.confirmed_points << " unconfirmed=" << run.r.unconfirmed_points
              << " global verified/contradicted/evaluated=" << run.r.global_verified << "/" << run.r.global_contradicted << "/" << run.r.global_evaluated
              << " blocks measured/empty/unmeasurable=" << run.r.measured_blocks << "/" << run.r.empty_blocks << "/"
              << run.r.unmeasurable_blocks << " points";
    for (int s = 0; s < 10; ++s) if (counts[s]) std::cout << ' ' << gate::PointStatusCode(static_cast<gate::PointStatus>(s)) << '=' << counts[s];
    std::cout << '\n';
    for (const auto& b : run.m.blocks) if (b.block_class == gate::BlockClass::unmeasurable_content)
        std::cout << "  unmeasurable block at (" << b.x << "," << b.y << ") points=" << b.points << " content=" << b.content_points
                  << " counted=" << b.counted_points << " unconfirmed=" << b.unconfirmed_points
                  << (b.neighbor_inconsistent ? " neighbour-inconsistent" : "") << '\n';
}

// Content in three row bands: drawing, text, lines in one direction.
std::vector<scene::Stroke> MixedContent(double x0, double x1, double h, std::uint64_t seed) {
    std::vector<scene::Stroke> s;
    scene::Drawing(s, {x0, 0, x1, h/3}, seed, 3500);
    scene::Text(s, {x0, h/3, x1, 2*h/3}, seed+1);
    scene::HorizontalLines(s, {x0, 2*h/3, x1, h}, seed+2);
    return s;
}

void Validation() {
    gate::ValidateSeamGateDesign(gate::kSeamGateDesignCalibrated);
    gate::ValidateSeamGateDesign(gate::kSeamGateDesignUncalibrated);
    gate::ValidateSeamGateDesign(kCalibrated);
    gate::ValidateSeamGateDesign(kUncalibrated);
    auto bad = kCalibrated; bad.patch_px = 60;
    Rejects([&] { gate::ValidateSeamGateDesign(bad); }, "patch not a multiple of 8");
    bad = kCalibrated; bad.search_radius_px = 18;
    Rejects([&] { gate::ValidateSeamGateDesign(bad); }, "radius not a multiple of 4");
    bad = kCalibrated; bad.block_px = 100;
    Rejects([&] { gate::ValidateSeamGateDesign(bad); }, "block not a multiple of the spacing");
    Rejects([] { (void)gate::PairSeamGeometry(64, 64, 64, 64, {{1, 0, 0, 0, 1, 40, 0, 0, 1}, a0::m2::StitchLayout::camera_a_top_camera_b_bottom, {}}); },
        "top/bottom layout");
    const Views v = MakeViews(128, 96, 64, {}, {}, Constant(0, 0), 0, 0);
    auto g = Geometry(v, 4);
    Rejects([&] { (void)gate::MeasureSeam(v.a, v.b, g, kCalibrated, kLimits, {}, 0); }, "zero threads");
    Rejects([&] { (void)gate::MeasureSeam(v.a, v.b, g, kCalibrated, {1.0, 1.0, "x"}, {}, 1); }, "guard band not below the limit");
    auto short_band = g; short_band.band.pop_back();
    Rejects([&] { (void)gate::MeasureSeam(v.a, v.b, short_band, kCalibrated, kLimits, {}, 1); }, "band rows differ from output rows");
    auto no_map = g; no_map.camera_b = {};
    Rejects([&] { (void)gate::MeasureSeam(v.a, v.b, no_map, kCalibrated, kLimits, {}, 1); }, "missing mapping");
    auto truncated = v.a; truncated.bgr.pop_back();
    Rejects([&] { (void)gate::MeasureSeam(truncated, v.b, g, kCalibrated, kLimits, {}, 1); }, "short buffer");
    // Empty band: nothing measured, so no measured block.
    auto empty = g; for (auto& row : empty.band) row = {};
    for (auto& row : empty.transition) row = {};
    const auto m = gate::MeasureSeam(v.a, v.b, empty, kCalibrated, kLimits, {}, 1);
    const auto r = gate::DecideSeamGate(m, kLimits);
    Check(r.state == gate::SeamGateState::fail && HasReason(r, "no-measurable-block"), "empty band is no measurable block");
    Check(gate::DecideSeamGate(m, {1.0, -0.1, "x"}).state == gate::SeamGateState::undetermined, "invalid limits are undetermined");
    Check(gate::DecideSeamGate(m, {1.5, 0.2, "x"}).state == gate::SeamGateState::undetermined, "limits other than the measurement's");
    const auto u = gate::UndeterminedSeamGate("policy-missing", kLimits);
    Check(u.state == gate::SeamGateState::undetermined && u.findings.at(0).reason == "policy-missing", "undetermined result");
    const auto json = gate::SeamGateReportJson(u, nullptr);
    Check(json.find("\"state\":\"undetermined\"") != std::string::npos && json.find("\"measurement\":null") != std::string::npos,
        "undetermined report");
}

// The gate's pair mapping is the arithmetic RenderPair samples with.
void PairMappingMatchesRenderer() {
    Views v{{}, {}, 150.25, 256, 128};
    v.a = scene::Render(256, 128, 0, 0, 0, {}, [](int i, int j, double& x, double& y) { x = i; y = j; }, 1, 0);
    v.b = v.a;
    for (std::size_t k = 0; k < v.a.bgr.size(); k += 3) {
        const auto i = (k/3)%256, j = (k/3)/256;
        v.a.bgr[k] = v.a.bgr[k+1] = v.a.bgr[k+2] = static_cast<std::uint8_t>((i*7+j*3)%251);
        v.b.bgr[k] = v.b.bgr[k+1] = v.b.bgr[k+2] = static_cast<std::uint8_t>((i*5+j*11)%241);
    }
    const render::PairRenderParameters p{{0.999, -0.02, 150.25, 0.02, 0.999, 3.5, 0, 0, 1}, a0::m2::StitchLayout::camera_a_left_camera_b_right, {2, 10, 4, 12}};
    const auto out = render::RenderPair(v.a, v.b, p).image;
    const auto g = gate::PairSeamGeometry(256, 128, 256, 128, p);
    Check(g.width == out.width && g.height == out.height, "pair geometry has the renderer's output size");
    bool same = true; int compared = 0;
    for (std::uint32_t y = 0; y < out.height; y += 7) for (std::uint32_t x = 0; x < out.width; x += 5) {
        Point ra{}, rb{};
        std::array<double, 3> pa{}, pb{};
        const bool has_a = g.camera_a({double(x), double(y)}, ra) && render::SampleBilinear(v.a, ra.x, ra.y, pa);
        const bool has_b = g.camera_b({double(x), double(y)}, rb) && render::SampleBilinear(v.b, rb.x, rb.y, pb);
        if (has_a == has_b) continue; // overlap: blended
        const auto expected = std::lround(has_a ? pa[0] : pb[0]);
        same = same && out.bgr[(static_cast<std::size_t>(y)*out.width+x)*3] == expected;
        ++compared;
    }
    Check(same && compared > 100, "single-camera pixels equal the renderer through the gate mapping");
}

void AccuracyOnKnownShifts() {
    const unsigned w = 1400, h = 900; const double tx = 800;
    const auto strokes = MixedContent(650, 1450, h, 101);
    for (const Point delta : {Point{0.3, -0.2}, Point{0.62, 0.41}, Point{-0.47, 0.18}, Point{0.05, 0.71}}) {
        const auto v = MakeViews(w, h, tx, strokes, strokes, Constant(delta.x, delta.y), 2, 1.0);
        const auto run = Measure(v, kCalibrated, [delta](Point, Point& p) { p = delta; return true; });
        Report("accuracy", run);
        const auto a = Errors(run.m, delta);
        const double bx = a.n ? a.sum_x/a.n : 0, by = a.n ? a.sum_y/a.n : 0, rms = a.n ? std::sqrt(a.sum_sq/a.n) : 0;
        std::cout << "  delta=(" << delta.x << "," << delta.y << ") two=" << a.two << " one=" << a.one << " max error=" << a.maximum
                  << " bias=(" << bx << "," << by << ") rms=" << rms << '\n';
        Check(a.maximum <= 0.05, "every counted point within 0.05 px of the known shift");
        Check(a.two >= 60 && a.one >= 15, "drawing and text give two-directional points, ruled lines one-directional points");
        Check(run.r.state == gate::SeamGateState::pass, "a shift whose maximum plus 0.2 stays under 1 px passes");
    }
}

void DecisionBoundary() {
    // Constructed records: the rule is max + 0.2 <= 1, equality passes.
    const auto record = [](double value) {
        gate::SeamMeasurement m;
        m.design = kCalibrated; m.limits = kLimits; m.block_columns = 1; m.block_rows = 1;
        m.containment.supplied = true; m.global.verified = kCalibrated.global_minimum_anchors;
        for (int k = 0; k < 2; ++k) {
            gate::SeamPoint p; p.status = gate::PointStatus::valid; p.content = true; p.x = 16+32*k; p.y = 16;
            p.visible_px = k ? value : 0.5;
            p.confirmation = value+kLimits.guard_band_px > kLimits.limit_px && k ? gate::Confirmation::confirmed : gate::Confirmation::not_required;
            m.points.push_back(p);
        }
        gate::SeamBlock b; b.block_class = gate::BlockClass::measured; b.points = 2; b.counted_points = 2; m.blocks.push_back(b);
        return m;
    };
    Check(gate::DecideSeamGate(record(0.8), kLimits).state == gate::SeamGateState::pass, "maximum 0.8 passes (0.8 + 0.2 = 1)");
    const auto over = gate::DecideSeamGate(record(std::nextafter(0.8, 1.0)), kLimits);
    Check(over.state == gate::SeamGateState::fail && over.findings.at(0).reason == "seam-misalignment" &&
        over.findings.at(0).remedy == "retake-then-recalibrate", "the next value above 0.8 fails");
    Check(gate::DecideSeamGate(record(0.79), kLimits).state == gate::SeamGateState::pass, "0.79 passes");
    const auto r = gate::DecideSeamGate(record(0.9), kLimits);
    Check(r.maximum_at && r.maximum_at->x == 48 && *r.measured_maximum_px == 0.9 && *r.second_px == 0.5, "maximum and its location");
    // An unconfirmed exceedance is not in the maximum.
    auto unconfirmed = record(3.0); unconfirmed.points[1].confirmation = gate::Confirmation::unconfirmed;
    const auto ru = gate::DecideSeamGate(unconfirmed, kLimits);
    Check(ru.state == gate::SeamGateState::pass && ru.unconfirmed_points == 1 && *ru.measured_maximum_px == 0.5, "unconfirmed point is not in the maximum");

    // End to end around the line.
    const unsigned w = 1100, h = 640; const double tx = 640;
    std::vector<scene::Stroke> s; scene::Drawing(s, {500, 0, 1200, double(h)}, 7, 2500); scene::Text(s, {500, 200, 1200, 420}, 8);
    const auto inside = Measure(MakeViews(w, h, tx, s, s, Constant(0.72, 0), 2, 1.0), kCalibrated,
        [](Point, Point& p) { p = {0.72, 0}; return true; });
    Report("0.72", inside);
    Check(inside.r.state == gate::SeamGateState::pass, "0.72 px everywhere passes");
    const auto outside = Measure(MakeViews(w, h, tx, s, s, Constant(0.88, 0), 2, 1.0), kCalibrated,
        [](Point, Point& p) { p = {0.88, 0}; return true; });
    Report("0.88", outside);
    Check(outside.r.state == gate::SeamGateState::fail && outside.r.findings.at(0).reason == "seam-misalignment" &&
        outside.r.confirmed_points > 0 && std::abs(*outside.r.measured_maximum_px-0.88) <= 0.05, "0.88 px everywhere fails on confirmed points");
}

// #385 Q1 owner decision. One decoy: in B a small mark is missing and a copy sits 5 px to the right.
// The base patch sees only the mark; the shifted and the enlarged patches
// also see marks that are correct in both views.
void UnconfirmedPointIsUnmeasurable() {
    const unsigned w = 1100, h = 640; const double tx = 640;
    const auto design = kCalibrated;
    const auto inset = gate::RequiredBandInsetPx(design);
    std::vector<scene::Stroke> base;
    scene::Drawing(base, {500, 0, 1200, double(h)}, 17, 2500);
    // Lattice point: band origin + 16 + 32k. Clear a square around it.
    auto g0 = Geometry(MakeViews(w, h, tx, {}, {}, Constant(0, 0), 0, 0), inset);
    std::uint32_t ox = w, oy = h; for (std::uint32_t y = 0; y < h; ++y) if (g0.band[y].begin < g0.band[y].end) { oy = std::min(oy, y); ox = std::min(ox, g0.band[y].begin); }
    const double cx = ox+16+32*3, cy = oy+16+32*6;
    std::vector<scene::Stroke> a;
    for (const auto& k : base) if (std::abs(k.cx-cx) > 70+k.half_length || std::abs(k.cy-cy) > 70+k.half_length) a.push_back(k);
    const auto cross = [](double x, double y, double arm, std::vector<scene::Stroke>& out) {
        out.push_back(scene::Segment(x-arm, y, x+arm, y, 3, 170)); out.push_back(scene::Segment(x, y-arm, x, y+arm, 3, 170));
        out.push_back(scene::Segment(x-arm, y-arm, x-arm+4, y-arm, 3, 170));
    };
    for (const Point d : {Point{0, 45}, Point{0, -45}, Point{45, 0}, Point{-45, 0}}) cross(cx+d.x, cy+d.y, 5, a);
    auto b = a;
    const auto mark = [](double x, double y) { return scene::Segment(x-2.5, y, x+2.5, y, 5, 150); };
    a.push_back(mark(cx, cy)); b.push_back(mark(cx+5, cy));
    const Point delta{0.35, -0.1};
    const auto v = MakeViews(w, h, tx, a, b, Constant(delta.x, delta.y), 2, 1.0);
    const auto run = Measure(v, design, [delta](Point, Point& p) { p = delta; return true; });
    Report("false match", run);
    const gate::SeamPoint* decoy = nullptr;
    for (const auto& p : run.m.points) if (p.x == cx && p.y == cy) decoy = &p;
    if (decoy) std::cout << "  decoy " << gate::PointStatusCode(decoy->status) << " d=(" << decoy->displacement.x << "," << decoy->displacement.y
                         << ") u=" << decoy->uncertainty_px << " rt=" << decoy->round_trip_px << '\n';
    Check(decoy && decoy->status == gate::PointStatus::valid && decoy->visible_px > 4 &&
        decoy->confirmation == gate::Confirmation::unconfirmed, "the decoy point is valid by its own checks but not confirmed");
    if (decoy) for (const auto& r : decoy->remeasurements)
        std::cout << "  decoy variant " << gate::VariantCode(r.variant) << " " << gate::PointStatusCode(r.status) << " d=(" << r.displacement.x << ","
                  << r.displacement.y << ") agrees=" << r.agrees << '\n';
    // #385 Q1 (owner decision, #385): the block holding a point
    // over the line that remeasurement left unconfirmed is content that cannot
    // be measured, so the gate does not pass on the rest of the block.
    bool block_unmeasurable = false;
    if (decoy) for (const auto& blk : run.m.blocks)
        if (blk.column == decoy->block_column && blk.row == decoy->block_row)
            block_unmeasurable = blk.block_class == gate::BlockClass::unmeasurable_content && blk.unconfirmed_points >= 1 && blk.counted_points >= 1;
    Check(block_unmeasurable, "the decoy's block still has counted points but is unmeasurable content");
    Check(run.r.state == gate::SeamGateState::fail && HasReason(run.r, "unmeasurable-content") && run.r.unconfirmed_blocks >= 1 &&
        !HasReason(run.r, "seam-misalignment"), "an unconfirmed point makes the block unmeasurable and fails the gate");
    for (const auto& f : run.r.findings) if (f.reason == "unmeasurable-content") Check(f.remedy == "shift-paper-and-retake", "remedy: move the paper");
    // The same B without the decoy passes: the decoy alone decides.
    auto clean = b; clean.back() = mark(cx, cy);
    const auto reference = Measure(MakeViews(w, h, tx, a, clean, Constant(delta.x, delta.y), 2, 1.0), design,
        [delta](Point, Point& p) { p = delta; return true; });
    Report("false match, scene without the decoy", reference);
    Check(reference.r.state == gate::SeamGateState::pass && reference.r.unconfirmed_blocks == 0, "the scene without the decoy passes");
}

// A real local displacement: one region of B moved by 2 px.
void LocalDisplacementFails() {
    const unsigned w = 1100, h = 640; const double tx = 640;
    std::vector<scene::Stroke> s; scene::Drawing(s, {500, 0, 1200, double(h)}, 27, 2000); scene::Text(s, {500, 0, 1200, double(h)}, 28);
    const double bx = 800, by = 330;
    const Field bump = [bx, by](double x, double y) {
        const auto taper = [](double d) { return d < 50 ? 1.0 : d > 80 ? 0.0 : 0.5+0.5*std::cos((d-50)/30*3.14159265358979323846); };
        return Point{0.25+2*taper(std::abs(x-bx))*taper(std::abs(y-by)), 0.1};
    };
    const auto run = Measure(MakeViews(w, h, tx, s, s, bump, 3, 1.0), kCalibrated,
        [](Point, Point& p) { p = {0.25, 0.1}; return true; });
    Report("local 2 px", run);
    Check(run.r.state == gate::SeamGateState::fail && run.r.findings.at(0).reason == "seam-misalignment", "a local 2 px displacement fails");
    Check(run.r.maximum_at && std::abs(double(run.r.maximum_at->x)-bx) <= 64 && std::abs(double(run.r.maximum_at->y)-by) <= 64 &&
        std::abs(*run.r.measured_maximum_px-std::hypot(2.25, 0.1)) <= 0.1, "maximum located on the displaced region");
    Check(run.r.confirmed_points >= 4, "the displaced region is confirmed by remeasurement");
}

void UnmeasurableAndEmpty() {
    const unsigned w = 1100, h = 640; const double tx = 640;
    const auto design = kCalibrated;
    const gate::DisplacementPrediction exact = [](Point, Point& p) { p = {0.3, 0}; return true; };
    // Fine hatching over one area: content that cannot be measured.
    std::vector<scene::Stroke> s; scene::Drawing(s, {500, 0, 1200, double(h)}, 37, 2500);
    std::vector<scene::Stroke> hatched;
    for (const auto& k : s) if (!(k.cx > 700 && k.cx < 900 && k.cy > 200 && k.cy < 420)) hatched.push_back(k);
    scene::Hatching(hatched, {720, 230, 880, 400}, 6);
    const auto periodic = Measure(MakeViews(w, h, tx, hatched, hatched, Constant(0.3, 0), 2, 1.0), design, exact);
    Report("hatching", periodic);
    Check(periodic.r.state == gate::SeamGateState::fail && HasReason(periodic.r, "unmeasurable-content"), "periodic hatching is unmeasurable content");
    for (const auto& f : periodic.r.findings) if (f.reason == "unmeasurable-content") Check(f.remedy == "shift-paper-and-retake", "remedy: move the paper");
    bool no_valid_in_hatch = true;
    for (const auto& p : periodic.m.points)
        if (p.x > 740 && p.x < 860 && p.y > 250 && p.y < 380 && p.status == gate::PointStatus::valid) no_valid_in_hatch = false;
    Check(no_valid_in_hatch, "no point inside the hatching is valid");

    // Blank band: nothing measured.
    const auto blank = MakeViews(w, h, tx, {}, {}, Constant(0.3, 0), 2, 1.0);
    const auto none = Measure(blank, design, exact);
    Check(none.r.state == gate::SeamGateState::fail && none.r.findings.at(0).reason == "no-measurable-block" &&
        none.r.measured_blocks == 0 && none.r.empty_blocks > 0, "no measured block fails");
    const auto none_unpredicted = Measure(blank, design, {});
    Check(HasReason(none_unpredicted.r, "unpredicted-empty-block"), "blank without prediction is recorded as such");

    // Content in a strip of the band, blank to its right. (The anchors of the
    // global check need the wide window inside camera B, so the pair is larger
    // than the others here and the blank part is the far side of B.)
    const unsigned w2 = 1400, h2 = 900; const double tx2 = 800;
    std::vector<scene::Stroke> half; scene::Drawing(half, {960, 0, 1100, double(h2)}, 47, 1500); scene::Text(half, {960, 0, 1100, double(h2)}, 48);
    const auto v = MakeViews(w2, h2, tx2, half, half, Constant(0.3, 0), 2, 1.0);
    const auto predicted = Measure(v, design, exact);
    Report("half blank", predicted);
    Check(predicted.r.state == gate::SeamGateState::pass && predicted.r.empty_blocks > 0 && predicted.r.predicted_maximum_px &&
        *predicted.r.predicted_maximum_px <= 0.3+0.06, "blank blocks take the prediction plus the mismatch");
    const auto off = Measure(v, design, [](Point, Point& p) { p = {1.1, 0}; return true; });
    Check(off.r.state == gate::SeamGateState::fail && HasReason(off.r, "predicted-misalignment"), "a prediction over the line fails");
    const auto missing = Measure(v, design, {});
    Check(missing.r.state == gate::SeamGateState::fail && HasReason(missing.r, "unpredicted-empty-block") &&
        !HasReason(missing.r, "seam-misalignment"), "blank blocks without prediction are not counted as within the limit");
    // A prediction available only on part of the band.
    const auto partial = Measure(v, design, [](Point q, Point& p) { p = {0.3, 0}; return q.y < 320; });
    Check(partial.r.unpredicted_empty_blocks > 0 && partial.r.unpredicted_empty_blocks < partial.r.empty_blocks, "partial prediction");
}

void BlendIndependenceAndDeterminism() {
    const unsigned w = 1100, h = 640; const double tx = 640;
    std::vector<scene::Stroke> s; scene::Drawing(s, {500, 0, 1200, double(h)}, 57, 2500);
    const auto v = MakeViews(w, h, tx, s, s, Constant(0.4, 0.2), 2, 1.0);
    const auto a_before = v.a.bgr, b_before = v.b.bgr;
    const gate::DisplacementPrediction exact = [](Point, Point& p) { p = {0.4, 0.2}; return true; };
    // The two blend methods give different composites; the gate reads neither.
    const auto feather = render::RenderPair(v.a, v.b, PairParameters(tx)).image;
    const auto seam = a0::m2::experimental::RenderPairSeamCut(v.a, v.b, PairParameters(tx), a0::m2::experimental::kSeamBlendDesign).image;
    Check(feather.bgr != seam.bgr, "seam-cut and feather composites differ");
    const auto after_feather = Measure(v, kCalibrated, exact, 3);
    const auto after_seam = Measure(v, kCalibrated, exact, 1);
    const auto j1 = gate::SeamGateReportJson(after_feather.r, &after_feather.m), j2 = gate::SeamGateReportJson(after_seam.r, &after_seam.m);
    Check(j1 == j2, "same decision and report for seam-cut and feather, for 3 and 1 threads");
    Check(v.a.bgr == a_before && v.b.bgr == b_before, "inputs unchanged");
    Check(j1.find("\"schema\":\"a0.seam-gate.v1\"") == 1 && j1.find("\"source\":\"#385\"") != std::string::npos &&
        j1.find("\"limitPx\":1,\"guardBandPx\":0.2") != std::string::npos, "report carries the schema and the owner's limits");
}

void LargeShifts() {
    const unsigned w = 1100, h = 640; const double tx = 640;
    std::vector<scene::Stroke> s; scene::Drawing(s, {440, 0, 1260, double(h)}, 67, 2500); scene::Text(s, {440, 0, 1260, double(h)}, 68);
    // Uncalibrated radius: a residual of about 10 px.
    const Point big{10.2, -3.1};
    const auto run = Measure(MakeViews(w, h, tx, s, s, Constant(big.x, big.y), 12, 1.0), kUncalibrated,
        [big](Point, Point& p) { p = big; return true; });
    Report("uncalibrated 10 px", run);
    Check(run.r.state == gate::SeamGateState::fail && std::abs(*run.r.measured_maximum_px-std::hypot(big.x, big.y)) <= 0.05,
        "a 10 px residual is measured and fails");
    Check(Errors(run.m, big).maximum <= 0.05, "large shift measured within 0.05 px");
    // Calibrated radius with a residual beyond it: the search widens.
    const Point wide{22.0, 1.0};
    const auto widened = Measure(MakeViews(w, h, tx, s, s, Constant(wide.x, wide.y), 24, 1.0), kCalibrated,
        [wide](Point, Point& p) { p = wide; return true; }, Threads(), gate::RequiredBandInsetPx(kUncalibrated));
    Report("widened", widened);
    std::uint32_t widest = 0; for (const auto& p : widened.m.points) widest = std::max(widest, p.search_radius_px);
    Check(widest >= 32 && widened.r.state == gate::SeamGateState::fail && Errors(widened.m, wide).maximum <= 0.05 &&
        widened.r.counted_points > 20, "the search widens past the first radius");
}

void OneDirection() {
    const unsigned w = 1100, h = 640; const double tx = 640;
    std::vector<scene::Stroke> s; scene::HorizontalLines(s, {400, 0, 1300, double(h)}, 77);
    const Point delta{3.0, 0.4}; // along the lines: invisible
    const auto run = Measure(MakeViews(w, h, tx, s, s, Constant(delta.x, delta.y), 4, 1.0), kCalibrated,
        [delta](Point, Point& p) { p = delta; return true; });
    Report("one direction", run);
    const auto a = Errors(run.m, delta);
    Check(a.one > 20 && a.two == 0 && a.maximum <= 0.05, "lines in one direction: normal component within 0.05 px");
    Check(std::abs(*run.r.measured_maximum_px-0.4) <= 0.05, "visible value is the component across the lines");
}

// Full size by default; a divisor gives a smaller pair for quick checks.
// A placement one grid period off (#365 can lock onto a table grid when the
// overlap is narrow): the grid on the table lines up again, the drawing on
// the paper does not. The gate must not pass. Periods in still pixels: 24
// and 64, and the live-view periods 24, 40 and 64 at 640 px width scaled
// to the 7360 px still (276, 460, 736).
void GridDecoyPlacement() {
    const unsigned w = 1500, h = 640; const double tx = 760;
    for (const double period : {24.0, 64.0, 276.0, 460.0, 736.0}) {
        std::vector<scene::Stroke> s;
        // A sheet with a drawing in the upper part of the band, the table grid below it.
        const double grid_top = 334;
        scene::Drawing(s, {1000, 120, 1260, grid_top-4}, 97, 1500); scene::Text(s, {1000, 140, 1260, grid_top-10}, 98);
        for (double x = 0; x < 2600; x += period) s.push_back(scene::Segment(x, grid_top, x, h+40.0, 3, 110));
        for (double y = grid_top+std::fmod(period/3, period); y < h+40; y += period) s.push_back(scene::Segment(400, y, 2000, y, 3, 110));
        // Placed B shows the content one period to the right: the grid coincides.
        const Point off{period, 0};
        const auto run = Measure(MakeViews(w, h, tx, s, s, Constant(off.x, off.y), period+2, 1.0), kUncalibrated,
            [](Point, Point& p) { p = {0, 0}; return true; });
        double grid_max = 0; std::size_t grid_points = 0;
        for (const auto& p : run.m.points) if (p.Counted() && p.y > grid_top+40) { grid_max = std::max(grid_max, p.visible_px); ++grid_points; }
        std::cout << "grid period " << period << ": grid-only counted points=" << grid_points << " their max=" << grid_max << '\n';
        Report("  grid decoy", run);
        Check(run.r.state == gate::SeamGateState::fail &&
            (HasReason(run.r, "seam-misalignment") || HasReason(run.r, "unmeasurable-content")),
            "a placement one grid period off fails on the drawing (period "+std::to_string(static_cast<int>(period))+")");
    }
    // Known gap (docs 16.10): with only blank paper and the grid in the band,
    // the grid lines up and nothing in the band shows the wrong placement.
    // Without a calibration prediction the blank blocks keep it from passing.
    std::vector<scene::Stroke> grid;
    for (double x = 0; x < 2600; x += 276) grid.push_back(scene::Segment(x, 334, x, h+40.0, 3, 110));
    for (double y = 426; y < h+40; y += 276) grid.push_back(scene::Segment(400, y, 2000, y, 3, 110));
    const auto blank_paper = Measure(MakeViews(w, h, tx, grid, grid, Constant(276, 0), 278, 1.0), kUncalibrated, {});
    Report("  grid only, no prediction", blank_paper);
    Check(blank_paper.r.state == gate::SeamGateState::fail && HasReason(blank_paper.r, "unpredicted-empty-block"),
        "grid and blank paper without a prediction does not pass");
}

// ---- #392: global placement and the transition band -------------------------

// Rows [y0,y1) of the band, columns [x0,x1); the transition is the clipped band.
gate::SeamGateGeometry Clipped(gate::SeamGateGeometry g, std::uint32_t x0, std::uint32_t x1, std::uint32_t y0, std::uint32_t y1) {
    for (std::uint32_t y = 0; y < g.height; ++y) {
        auto& row = g.band[y];
        if (y < y0 || y >= y1) { row = {}; continue; }
        row.begin = std::max(row.begin, x0); row.end = std::min(row.end, x1);
        if (row.begin >= row.end) row = {};
    }
    g.transition = g.band;
    return g;
}
Run MeasureWith(const Views& v, const gate::SeamGateGeometry& g, const gate::SeamGateDesign& design,
    const gate::DisplacementPrediction& prediction, unsigned threads = Threads()) {
    auto m = gate::MeasureSeam(v.a, v.b, g, design, kLimits, prediction, threads);
    auto r = gate::DecideSeamGate(m, kLimits);
    return {std::move(m), std::move(r)};
}
void ReportGlobal(const std::string& name, const Run& run) {
    Report(name.c_str(), run);
    std::size_t counts[6]{};
    for (const auto& a : run.m.global.anchors) ++counts[static_cast<int>(a.status)];
    std::cout << "  global: radius=" << run.m.global.radius_px << " candidates=" << run.m.global.candidates << " evaluated=" << run.m.global.evaluated;
    for (int s = 0; s < 6; ++s) if (counts[s]) std::cout << ' ' << gate::GlobalAnchorStatusCode(static_cast<gate::GlobalAnchorStatus>(s)) << '=' << counts[s];
    std::cout << '\n';
}
bool OnlyGlobalReasons(const gate::SeamGateResult& r) {
    if (r.findings.empty()) return false;
    for (const auto& f : r.findings) if (f.reason.rfind("global-placement", 0) != 0) return false;
    return true;
}

// A pair large enough for the production wide window: camera B has to cover
// 768 px (plus the patch) around an anchor, so anchors lie right of kBigTx+803
// and between 803 and h-803. The band is clipped to the anchor area.
constexpr unsigned kBigW = 2700, kBigH = 2000;
constexpr double kBigTx = 1250;
gate::SeamGateGeometry BigGeometry(const Views& v, const gate::SeamGateDesign& design) {
    auto g = gate::PairSeamGeometry(v.w, v.h, v.w, v.h, PairParameters(v.tx));
    g.band = gate::OverlapBand(g, v.w, v.h, v.w, v.h, gate::RequiredBandInsetPx(design));
    return Clipped(g, 2100, 2550, 820, 1180);
}
// Table lattice with a crossing at (2330, 1000), plus optionally a drawing
// that is not periodic.
std::vector<scene::Stroke> LatticeScene(double period, bool drawing) {
    std::vector<scene::Stroke> s;
    scene::Lattice(s, {0, 0, 4300, double(kBigH)}, period, 2330, 1000);
    if (drawing) scene::Drawing(s, {1900, 700, 2800, 1300}, 301, 2500);
    return s;
}
const gate::DisplacementPrediction kZeroPrediction = [](Point, Point& p) { p = {0, 0}; return true; };

// A band of a table lattice only (or blank paper): B is placed one period off,
// so the lattice lines up and every local check sees an aligned pair. With a
// calibration prediction the blank blocks are filled in as well. Neither the
// calibrated search radius nor the uncalibrated one (periodicity checked to
// twice the radius) can see the shift; the gate must not pass.
void GlobalPlacementLatticeOnly() {
    for (const double period : {96.0, 128.0, 276.0, 460.0, 736.0}) {
        const std::string tag = " (period "+std::to_string(static_cast<int>(period))+")";
        const auto s = LatticeScene(period, false);
        const auto v = MakeViews(kBigW, kBigH, kBigTx, s, s, Constant(period, 0), period+2, 1.0);
        const auto cal = MeasureWith(v, BigGeometry(v, gate::kSeamGateDesignCalibrated), gate::kSeamGateDesignCalibrated, kZeroPrediction);
        ReportGlobal("lattice only, calibrated with prediction"+tag, cal);
        Check(cal.r.state == gate::SeamGateState::fail && HasReason(cal.r, "global-placement-unverified"),
            "calibrated, with a prediction: a placement one lattice period off does not pass"+tag);
        Check(cal.r.measured_blocks > 0 && OnlyGlobalReasons(cal.r),
            "the local checks alone would have passed; only the global check stops it"+tag);
        Check(cal.r.global_verified == 0 && cal.r.global_contradicted == 0, "no anchor verifies a repeating lattice"+tag);
        bool window_ok = true;
        for (const auto& a : cal.m.global.anchors) if (a.status == gate::GlobalAnchorStatus::window_unavailable) window_ok = false;
        Check(window_ok, "the anchors were rejected by the repetition, not for lack of a wide window"+tag);
        const auto uncal = MeasureWith(v, BigGeometry(v, gate::kSeamGateDesignUncalibrated), gate::kSeamGateDesignUncalibrated, {});
        ReportGlobal("lattice only, uncalibrated, no prediction"+tag, uncal);
        Check(uncal.r.state == gate::SeamGateState::fail && HasReason(uncal.r, "global-placement-unverified"),
            "uncalibrated radius, no prediction: a placement one lattice period off does not pass"+tag);
    }
    // Blank paper has nothing to verify with, with or without a prediction.
    const std::vector<scene::Stroke> none;
    const auto blank = MakeViews(kBigW, kBigH, kBigTx, none, none, Constant(0.3, 0), 2, 1.0);
    const auto b = MeasureWith(blank, BigGeometry(blank, gate::kSeamGateDesignCalibrated), gate::kSeamGateDesignCalibrated,
        [](Point, Point& p) { p = {0.3, 0}; return true; });
    Check(b.r.state == gate::SeamGateState::fail && HasReason(b.r, "global-placement-unverified"), "blank paper with a prediction does not pass");
}

// Lattice plus a drawing that is not periodic. Placed right the gate passes
// with the drawing's blocks verified; placed one lattice period off the
// drawing shows it and the gate fails.
void GlobalPlacementWithContent() {
    for (const double period : {276.0, 460.0}) {
        const std::string tag = " (period "+std::to_string(static_cast<int>(period))+")";
        const auto s = LatticeScene(period, true);
        const Point delta{0.3, -0.1};
        const gate::DisplacementPrediction exact = [delta](Point, Point& p) { p = delta; return true; };
        const auto v = MakeViews(kBigW, kBigH, kBigTx, s, s, Constant(delta.x, delta.y), 2, 1.0);
        for (const auto* design : {&gate::kSeamGateDesignCalibrated, &gate::kSeamGateDesignUncalibrated}) {
            const std::string name = design == &gate::kSeamGateDesignCalibrated ? "calibrated" : "uncalibrated";
            const auto run = MeasureWith(v, BigGeometry(v, *design), *design, exact);
            ReportGlobal("content, "+name+tag, run);
            Check(run.r.state == gate::SeamGateState::pass && run.r.global_verified >= design->global_minimum_anchors &&
                run.r.global_contradicted == 0, name+": a right placement with content passes"+tag);
        }
        // One lattice period off: the lattice coincides, the drawing does not.
        const auto off = MakeViews(kBigW, kBigH, kBigTx, s, s, Constant(period, 0), period+2, 1.0);
        for (const auto* design : {&gate::kSeamGateDesignCalibrated, &gate::kSeamGateDesignUncalibrated}) {
            const std::string name = design == &gate::kSeamGateDesignCalibrated ? "calibrated" : "uncalibrated";
            const auto run = MeasureWith(off, BigGeometry(off, *design), *design, kZeroPrediction);
            ReportGlobal("content one period off, "+name+tag, run);
            Check(run.r.state == gate::SeamGateState::fail, name+": content one period off does not pass"+tag);
            Check(HasReason(run.r, "global-placement-mismatch") || HasReason(run.r, "global-placement-unverified") ||
                HasReason(run.r, "seam-misalignment") || HasReason(run.r, "unmeasurable-content"), "a reason is given"+tag);
        }
    }
}

void TransitionBandContainment() {
    using gate::BandRow;
    // Unit level.
    std::vector<BandRow> band(6), tr(6);
    band[1] = {100, 200}; band[2] = {100, 200}; band[3] = {100, 200};
    tr[1] = {100, 200}; tr[2] = {120, 180}; tr[3] = {101, 199};
    auto c = gate::CheckTransitionBandContained(band, tr, 400);
    Check(c.supplied && c.Contained() && c.rows_outside == 0, "a transition inside the band is contained");
    tr[2] = {96, 200};  // 4 px left
    tr[3] = {100, 207}; // 7 px right
    tr[4] = {10, 20};   // a row without band
    c = gate::CheckTransitionBandContained(band, tr, 400);
    Check(!c.Contained() && c.rows_outside == 3 && c.pixels_outside == 4+7+10 && c.first_y == 2 && c.first_x == 96,
        "pixels outside are counted, the first one located");
    tr[2] = {120, 180}; tr[4] = {};
    c = gate::CheckTransitionBandContained(band, tr, 400);
    Check(c.rows_outside == 1 && c.first_y == 3 && c.first_x == 200, "first outside on the right side");
    tr[3] = {};
    Check(gate::CheckTransitionBandContained(band, tr, 400).Contained(), "empty transition rows are contained anywhere");
    Rejects([&] { (void)gate::CheckTransitionBandContained(band, std::vector<BandRow>(5), 400); }, "rows differ");
    tr[0] = {10, 401};
    Rejects([&] { (void)gate::CheckTransitionBandContained(band, tr, 400); }, "transition row outside the output");
    // Seam-cut transition: |x - seam| < width/2 around the seam, whole pixels.
    const auto cut = gate::SeamCutTransitionBand({50, 0, 99}, 8, 100);
    Check(cut.size() == 3 && cut[0].begin == 46 && cut[0].end == 55 && cut[1].begin == 0 && cut[1].end == 5 &&
        cut[2].begin == 95 && cut[2].end == 100, "seam-cut transition band, clipped to the output");
    Rejects([] { (void)gate::SeamCutTransitionBand({100}, 8, 100); }, "seam column outside the output");
    Rejects([] { (void)gate::SeamCutTransitionBand({5}, 0, 100); }, "zero blend width");

    // Through the measurer and the decision.
    const unsigned w = 1400, h = 900; const double tx = 800;
    const auto strokes = MixedContent(650, 1450, h, 101);
    const Point delta{0.3, -0.2};
    const auto v = MakeViews(w, h, tx, strokes, strokes, Constant(delta.x, delta.y), 2, 1.0);
    const gate::DisplacementPrediction exact = [delta](Point, Point& p) { p = delta; return true; };
    const auto g = Clipped(Geometry(v, gate::RequiredBandInsetPx(kCalibrated)), 1000, 1330, 200, 640);
    const auto inside = MeasureWith(v, g, kCalibrated, exact);
    ReportGlobal("transition inside the band", inside);
    Check(inside.r.state == gate::SeamGateState::pass && inside.r.transition_pixels_outside == 0 && inside.m.containment.supplied,
        "a transition inside the band passes");
    auto wide = g;
    for (std::uint32_t y = 300; y < 400; ++y) wide.transition[y].end += 24; // the blend reaches 24 px past the measured band
    const auto outside = MeasureWith(v, wide, kCalibrated, exact);
    ReportGlobal("transition 24 px past the band", outside);
    Check(outside.r.state == gate::SeamGateState::fail && HasReason(outside.r, "transition-outside-band") &&
        outside.r.transition_rows_outside == 100 && outside.r.transition_pixels_outside == 100*24 &&
        outside.m.containment.first_y == 300 && outside.m.containment.first_x == g.band[300].end,
        "a transition outside the band fails, with the amount and the place");
    for (const auto& f : outside.r.findings) if (f.reason == "transition-outside-band") Check(f.remedy == "check-gate-band", "remedy: check the band");
    Check(!HasReason(outside.r, "seam-misalignment"), "the measured values themselves are fine");
    // The path did not say where the composite mixes.
    auto unsaid = g; unsaid.transition.clear();
    const auto missing = MeasureWith(v, unsaid, kCalibrated, exact);
    Check(missing.r.state == gate::SeamGateState::undetermined && missing.r.findings.at(0).reason == "transition-band-not-supplied" &&
        !missing.m.containment.supplied, "no transition given: undetermined, not a pass");
    Rejects([&] { auto bad = g; bad.transition.pop_back(); (void)gate::MeasureSeam(v.a, v.b, bad, kCalibrated, kLimits, exact, 1); },
        "transition rows differ from output rows");
    // The report carries the checks.
    const auto json = gate::SeamGateReportJson(outside.r, &outside.m);
    Check(json.find("\"transitionPixelsOutside\":2400") != std::string::npos &&
        json.find("\"transition\":{\"supplied\":true,\"rowsOutside\":100") != std::string::npos &&
        json.find("\"globalAnchors\":[") != std::string::npos && json.find("\"placement\":{\"globalRadiusPx\":128") != std::string::npos &&
        json.find("transition-outside-band") != std::string::npos, "report carries placement and transition");
}

// A weaker lookalike of the content sits exactly where the local search
// looks (B shows each stroke again, half as dark, at the position the
// placement predicts), while the real match is 300 px away. The local checks
// accept the lookalike; the wide search finds the single strong peak
// elsewhere, so the anchor is contradicted and the gate fails.
void GlobalPlacementContradicted() {
    std::vector<scene::Stroke> a;
    scene::Drawing(a, {1900, 700, 2800, 1300}, 311, 2500);
    auto b = a;
    for (auto s : a) { s.cx -= 300; s.ink *= 0.5; b.push_back(s); }
    const auto v = MakeViews(kBigW, kBigH, kBigTx, a, b, Constant(300, 0), 302, 1.0);
    const auto run = MeasureWith(v, BigGeometry(v, gate::kSeamGateDesignCalibrated), gate::kSeamGateDesignCalibrated, kZeroPrediction);
    ReportGlobal("contradicted by a stronger match 300 px away", run);
    Check(run.r.state == gate::SeamGateState::fail && HasReason(run.r, "global-placement-mismatch") && run.r.global_contradicted > 0,
        "an anchor whose wide peak is elsewhere fails the gate as a placement mismatch");
    for (const auto& f : run.r.findings) if (f.reason == "global-placement-mismatch") Check(f.remedy == "retake-then-recalibrate", "remedy: retake, then recalibrate");
    bool located = false;
    for (const auto& anchor : run.m.global.anchors)
        if (anchor.status == gate::GlobalAnchorStatus::contradicted && std::abs(anchor.wide.x-300) <= 2 && std::abs(anchor.wide.y) <= 2) located = true;
    Check(located, "the wide peak is reported at the real position");
}

gate::SeamMeasurement HandRecord() {
    gate::SeamMeasurement m;
    m.design = kCalibrated; m.limits = kLimits; m.block_columns = 1; m.block_rows = 1;
    m.containment.supplied = true; m.global.verified = kCalibrated.global_minimum_anchors;
    for (int k = 0; k < 2; ++k) {
        gate::SeamPoint p; p.status = gate::PointStatus::valid; p.content = true; p.x = 16+32*k; p.y = 16; p.visible_px = 0.4;
        m.points.push_back(p);
    }
    gate::SeamBlock b; b.block_class = gate::BlockClass::measured; b.points = 2; b.counted_points = 2; m.blocks.push_back(b);
    return m;
}
void GlobalDecisionRules() {
    auto m = HandRecord();
    Check(gate::DecideSeamGate(m, kLimits).state == gate::SeamGateState::pass, "a record with enough verified anchors and a contained transition passes");
    m.global.verified = kCalibrated.global_minimum_anchors-1;
    auto r = gate::DecideSeamGate(m, kLimits);
    Check(r.state == gate::SeamGateState::fail && r.findings.size() == 1 && r.findings[0].reason == "global-placement-unverified" &&
        r.findings[0].remedy == "place-distinctive-content-and-retake", "one verified anchor too few: unverified");
    m.global.verified = 0; m.global.contradicted = 1;
    r = gate::DecideSeamGate(m, kLimits);
    Check(r.state == gate::SeamGateState::fail && r.findings.size() == 1 && r.findings[0].reason == "global-placement-mismatch",
        "a contradicted anchor is a mismatch, not only unverified");
    m.global.verified = 100; // verified anchors do not outvote a contradiction
    Check(gate::DecideSeamGate(m, kLimits).state == gate::SeamGateState::fail, "a contradiction fails whatever the verified count");
    m = HandRecord(); m.containment.pixels_outside = 5; m.containment.rows_outside = 1;
    r = gate::DecideSeamGate(m, kLimits);
    Check(r.state == gate::SeamGateState::fail && r.findings.size() == 1 && r.findings[0].reason == "transition-outside-band", "transition outside the band fails");
    m = HandRecord(); m.containment.supplied = false;
    r = gate::DecideSeamGate(m, kLimits);
    Check(r.state == gate::SeamGateState::undetermined && r.findings.at(0).reason == "transition-band-not-supplied", "transition not supplied is undetermined");
    // A placement that is fine and a seam that is not: the primary reason stays the seam.
    m = HandRecord(); m.points[1].visible_px = 0.9; m.points[1].confirmation = gate::Confirmation::confirmed; m.global.verified = 0;
    r = gate::DecideSeamGate(m, kLimits);
    Check(r.findings.size() == 2 && r.findings[0].reason == "seam-misalignment" && r.findings[1].reason == "global-placement-unverified",
        "the seam misalignment stays the primary reason");
}

void GlobalDesignValidation() {
    auto d = gate::kSeamGateDesignCalibrated; d.global_search_radius_px = 770;
    Rejects([&] { gate::ValidateSeamGateDesign(d); }, "global radius not a multiple of 4");
    d = gate::kSeamGateDesignUncalibrated; d.global_search_radius_px = 96;
    Rejects([&] { gate::ValidateSeamGateDesign(d); }, "global radius not past the periodicity check of the local search");
    d = gate::kSeamGateDesignCalibrated; d.global_search_radius_px = 1028;
    Rejects([&] { gate::ValidateSeamGateDesign(d); }, "global radius over the memory bound");
    d = gate::kSeamGateDesignCalibrated; d.global_minimum_anchors = 0;
    Rejects([&] { gate::ValidateSeamGateDesign(d); }, "no anchor required");
    d = gate::kSeamGateDesignCalibrated; d.global_maximum_evaluations = 3;
    Rejects([&] { gate::ValidateSeamGateDesign(d); }, "fewer evaluations than anchors required");
    d = gate::kSeamGateDesignCalibrated; d.global_tolerance_px = 0;
    Rejects([&] { gate::ValidateSeamGateDesign(d); }, "zero tolerance");
}

int Timing(unsigned divisor, unsigned threads) {
    const unsigned w = 7360/divisor, h = 4912/divisor; const double tx = 3760.0/divisor;
    std::vector<scene::Stroke> s;
    scene::Drawing(s, {tx-760, 0, w+640.0, double(h)}, 91, 4000);
    scene::Text(s, {tx-760, 0.3*h, w+640.0, 0.53*h}, 92);
    scene::HorizontalLines(s, {tx-760, 0.73*h, w+640.0, 0.88*h}, 93);
bool first_design = true;
    for (const auto& c : {std::pair{gate::kSeamGateDesignCalibrated, Point{0.3, 0.2}}, std::pair{gate::kSeamGateDesignUncalibrated, Point{6.0, -2.0}}}) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto v = MakeViews(w, h, tx, s, s, Constant(c.second.x, c.second.y), 8, 1.0);
        const auto g = Geometry(v, gate::RequiredBandInsetPx(c.first));
        const auto t1 = std::chrono::steady_clock::now();
        const auto m = gate::MeasureSeam(v.a, v.b, g, c.first, kLimits, [&](Point, Point& p) { p = c.second; return true; }, threads);
        const auto t2 = std::chrono::steady_clock::now();
        const auto r = gate::DecideSeamGate(m, kLimits);
        // One-directional error uses the estimated normal, so a shift along
        // the lines (6 px in the second case) adds normal error times 6 px.
        double two = 0, one = 0;
        for (const auto& p : m.points) if (p.Counted()) {
            const Point e{p.displacement.x-c.second.x, p.displacement.y-c.second.y};
            if (p.kind == gate::PointKind::one_directional) one = std::max(one, std::abs(p.normal.x*e.x+p.normal.y*e.y));
            else two = std::max(two, std::hypot(e.x, e.y));
        }
        std::size_t band = 0; for (const auto& row : g.band) band = std::max<std::size_t>(band, row.end-row.begin);
        std::size_t remeasured = 0; for (const auto& p : m.points) remeasured += !p.remeasurements.empty();
        std::cout << "timing radius=" << c.first.search_radius_px << " band width=" << band << " points=" << m.points.size()
                  << " remeasured=" << remeasured << " threads=" << threads << " state=" << gate::SeamGateStateCode(r.state)
                  << " max=" << r.measured_maximum_px.value_or(-1) << " max error two/one=" << two << "/" << one << " reasons=" << Reasons(r)
                  << " scene+band=" << std::chrono::duration<double>(t1-t0).count() << " s measure=" << std::chrono::duration<double>(t2-t1).count() << " s\n";
        std::cout << "  global placement: candidates=" << m.global.candidates << " evaluated=" << m.global.evaluated << " verified=" << m.global.verified
                  << " contradicted=" << m.global.contradicted << " indeterminate=" << m.global.indeterminate << '\n';
        if (first_design) { // the cost of the global stage: the same measurement with one anchor
            auto one_anchor = c.first; one_anchor.global_maximum_evaluations = 1; one_anchor.global_minimum_anchors = 1;
            const auto t3 = std::chrono::steady_clock::now();
            (void)gate::MeasureSeam(v.a, v.b, g, one_anchor, kLimits, [&](Point, Point& p) { p = c.second; return true; }, threads);
            std::cout << "  same measurement with one anchor: " << std::chrono::duration<double>(std::chrono::steady_clock::now()-t3).count() << " s\n";
        }
        first_design = false;
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--timing")
            return Timing(argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 1, argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : Threads());
        // --only NAME runs one check group (development aid); without it all run.
        const std::string only = argc > 2 && std::string(argv[1]) == "--only" ? argv[2] : "";
        const auto run = [&](const char* name, void (*test)()) { if (only.empty() || only == name) test(); };
        run("Validation", Validation);
        run("GlobalDesignValidation", GlobalDesignValidation);
        run("PairMappingMatchesRenderer", PairMappingMatchesRenderer);
        run("DecisionBoundary", DecisionBoundary);
        run("AccuracyOnKnownShifts", AccuracyOnKnownShifts);
        run("UnconfirmedPointIsUnmeasurable", UnconfirmedPointIsUnmeasurable);
        run("LocalDisplacementFails", LocalDisplacementFails);
        run("UnmeasurableAndEmpty", UnmeasurableAndEmpty);
        run("BlendIndependenceAndDeterminism", BlendIndependenceAndDeterminism);
        run("LargeShifts", LargeShifts);
        run("OneDirection", OneDirection);
        run("GridDecoyPlacement", GridDecoyPlacement);
        run("TransitionBandContainment", TransitionBandContainment);
        run("GlobalPlacementLatticeOnly", GlobalPlacementLatticeOnly);
        run("GlobalPlacementWithContent", GlobalPlacementWithContent);
        run("GlobalPlacementContradicted", GlobalPlacementContradicted);
        run("GlobalDecisionRules", GlobalDecisionRules);
    } catch (const std::exception& e) {
        std::cerr << "FAIL exception: " << e.what() << '\n';
        return 1;
    }
    std::cout << checks-failures << "/" << checks << " checks passed\n";
    return failures ? 1 : 0;
}
