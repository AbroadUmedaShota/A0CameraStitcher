// Contracts of the gate for the registered composite (#417, docs/design/fixed-composite-registration.md 11.5, 11.6, 11.22).
// Synthetic pairs of tests/seam_gate_scene.hpp (no camera, no file, no clock in the checks). The pair mapping
// (PairMappedFixedSeamInput) stands for the composite's mapping: the gate only sees an output-to-camera pair.
#include "a0/m2/experimental/registration_gate.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include "parallax_scene.hpp"
#include "seam_gate_scene.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numbers>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace rg = a0::m2::experimental::registration_gate;
namespace gate = a0::m2::seam_gate;
namespace render = a0::m2::render;
namespace scene = seam_gate_scene;
using a0::m2::experimental::MappedFixedSeamInput;
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
// The owner's line of #385 (1 px, guard 0.2 px); a test value here, the product reads it from condition v3.
const gate::SeamGateLimits kLimits{1.0, 0.2, "test"};
unsigned Threads() { return std::max(2u, std::min(4u, std::thread::hardware_concurrency())); }
// As in seam_gate_tests.cpp: the production constants with a wide radius of 128 px and 2 anchors, for scenes smaller than the
// production wide window.
gate::SeamGateDesign Reduced(gate::SeamGateDesign d) { d.global_search_radius_px = 128; d.global_minimum_anchors = 2; return d; }
const gate::SeamGateDesign kDesign = Reduced(gate::kSeamGateDesignCalibrated);
// Test values of this part's design constants (not proposals for the product).
rg::GateDesign TestDesign() {
    rg::GateDesign d;
    d.minimum_baseline_component = 0.5; d.lift_ratio = 3; d.lift_share = 0.9;
    d.wide_search_radius_px = 40; d.wide_search_half_width_px = 64; d.wide_search_block_rows = 96;
    d.wide_search_minimum_correlation = 0.5; d.wide_search_secondary_ratio = 0.8; d.wide_search_consistency_px = 1.5;
    d.wide_search_minimum_blocks = 2; d.wide_search_trigger_share = 0.5;
    d.maximum_unmeasured_content_share = 0.25;
    return d;
}

// Scene: A at the world origin, B translated by kTx and showing world point G - delta(G) at the position G where the pair
// mapping places it, so a feature of A at p appears in placed B at p + delta.
constexpr unsigned kW = 1100, kH = 640;
constexpr double kTx = 640;
constexpr std::uint32_t kSeam = 870;      // the overlap is [640, 1100); the measurable part [726, 1014)
using Field = std::function<Point(double, double)>;
Field Constant(double x, double y) { return [x, y](double, double) { return Point{x, y}; }; }
struct Views { BgrImage a, b; };
Views MakeViews(const std::vector<scene::Stroke>& s, const Field& delta, double bound) {
    Views v;
    v.a = scene::Render(kW, kH, 0, 0, 0, s, [](int i, int j, double& x, double& y) { x = i; y = j; }, 11, 1.0);
    v.b = scene::Render(kW, kH, kTx, 0, bound, s, [&](int i, int j, double& x, double& y) {
        const double gx = i+kTx, gy = j; const auto d = delta(gx, gy); x = gx-d.x; y = gy-d.y; }, 23, 1.0);
    return v;
}
MappedFixedSeamInput Mapped() {
    return a0::m2::experimental::PairMappedFixedSeamInput(kW, kH, kW, kH,
        {{1, 0, kTx, 0, 1, 0, 0, 0, 1}, a0::m2::StitchLayout::camera_a_left_camera_b_right, {}});
}
// A cosine step: 1 for |u| <= inner, 0 for |u| >= outer.
double Step(double u, double inner, double outer) {
    u = std::abs(u);
    if (u <= inner) return 1;
    if (u >= outer) return 0;
    return 0.5*(1+std::cos(std::numbers::pi*(u-inner)/(outer-inner)));
}

rg::RegistrationGateRequest Request(const Views& v) {
    rg::RegistrationGateRequest r;
    r.camera_a = &v.a; r.camera_b = &v.b; r.mapped = Mapped();
    r.layout.seam_column = kSeam; r.layout.mixing_width_px = 8; r.layout.omega_margin_px = 28; r.layout.guard_px = 0;
    r.layout.band_half_width_px = 128;   // Omega is the seam +- 32
    r.blend = a0::m2::experimental::kSeamBlendDesign;
    r.gate_design = kDesign; r.gate_limits = kLimits; r.design = TestDesign();
    r.baseline = {1, 0}; r.thread_count = Threads();
    r.measure_whole_band_for_anchors = true;
    return r;
}

std::vector<scene::Stroke> Content(std::uint64_t seed) {
    std::vector<scene::Stroke> s;
    scene::Drawing(s, {500, 0, 1300, double(kH)}, seed, 2500);
    scene::Text(s, {500, 200, 1300, 420}, seed+1);
    return s;
}
gate::DisplacementPrediction Predict(Point d) { return [d](Point, Point& p) { p = d; return true; }; }

// ---- A fingerprint of a result, for the byte comparison of two runs. -------------------------------------------------
std::string Hex(double v) { std::uint64_t u; std::memcpy(&u, &v, 8); std::ostringstream s; s << std::hex << u; return s.str(); }
std::string Hex(const std::optional<double>& v) { return v ? Hex(*v) : "null"; }
std::string Text(const rg::GateMaxima& m) {
    return Hex(m.two_directional_px)+","+Hex(m.one_directional_px)+","+
        (m.one_directional_normal ? Hex(m.one_directional_normal->x)+"/"+Hex(m.one_directional_normal->y) : "null")+","+
        (m.one_directional_component ? std::string(rg::OneDirectionalComponentCode(*m.one_directional_component)) : "null");
}
std::string Text(const std::vector<rg::RowRange>& rows) {
    std::string s;
    for (const auto& r : rows) s += std::to_string(r.first)+"-"+std::to_string(r.last)+";";
    return s;
}
std::string Text(const rg::GateRows& r) {
    return Hex(r.dx_measured_share)+","+std::to_string(r.rows_with_content)+","+std::to_string(r.rows_dx_measured)+","+
        Text(r.unmeasured)+"|"+Text(r.outside_measurable)+"|"+std::to_string(r.rows_blank)+","+std::to_string(r.rows_measured)+","+
        std::to_string(r.rows_content_unmeasured)+","+std::to_string(r.rows_one_sided)+","+std::to_string(r.rows_unconfirmed)+","+
        Hex(r.content_unmeasured_share)+"|"+Text(r.blank)+"|"+Text(r.content_unmeasured);
}
std::string Fingerprint(const rg::RegistrationGateResult& r) {
    std::string s = std::string(rg::GateStateCode(r.state))+" "+std::string(rg::GateCauseCode(r.cause))+" "+r.remedy+"\n";
    for (const auto& f : r.gate_findings) s += f.reason+":"+f.remedy+" ";
    s += "\nrows "+Text(r.rows)+"\nmax "+Text(r.maxima)+"\nbefore "+Hex(r.before.maximum_px)+" "+Text(r.before.maxima)+
        "\nafter "+Hex(r.after.maximum_px)+" "+Text(r.after.maxima)+"\n";
    if (r.anchor_columns) s += "anchors "+std::to_string(r.anchor_columns->first)+"-"+std::to_string(r.anchor_columns->second)+"\n";
    if (r.lift) s += "lift "+std::to_string(r.lift->points_exceeding)+" "+Hex(r.lift->baseline_share)+" "+Hex(r.lift->sign_agreement_share)+" "+
        Hex(r.lift->height_estimate_mm)+"\n";
    if (r.wide_search) s += "wide "+std::to_string(r.wide_search->blocks)+" "+std::to_string(r.wide_search->unique_blocks)+" "+
        std::to_string(r.wide_search->agreeing_blocks)+" "+Hex(r.wide_search->offset_px)+"\n";
    if (r.seam_gate) s += gate::SeamGateReportJson(*r.seam_gate, r.measurement ? &*r.measurement : nullptr)+"\n";
    if (r.band) s += "band "+gate::SeamGateReportJson(r.band->result, nullptr)+" "+Text(r.band->maxima)+" "+Text(r.band->rows)+"\n";
    return s;
}
std::string Summary(const rg::RegistrationGateResult& r) {
    std::ostringstream s;
    s << "state=" << rg::GateStateCode(r.state) << " cause=" << rg::GateCauseCode(r.cause) << " seam-gate=";
    if (r.seam_gate) { s << gate::SeamGateStateCode(r.seam_gate->state) << "["; for (const auto& f : r.seam_gate->findings) s << f.reason << " "; s << "]"; }
    else s << "none";
    s << " own=["; for (const auto& f : r.gate_findings) s << f.reason << " "; s << "]";
    s << " two=" << r.maxima.two_directional_px.value_or(-1) << " one=" << r.maxima.one_directional_px.value_or(-1);
    if (r.maxima.one_directional_component) s << "(" << rg::OneDirectionalComponentCode(*r.maxima.one_directional_component) << ")";
    s << " rows " << r.rows.rows_dx_measured << "/" << r.rows.rows_with_content << " unmeasured=" << Text(r.rows.unmeasured)
      << " outside=" << Text(r.rows.outside_measurable);
    if (r.seam_gate) s << " anchors verified=" << r.seam_gate->global_verified << "/" << r.seam_gate->global_evaluated;
    return s.str();
}
void Report(const char* name, const rg::RegistrationGateResult& r) { std::cout << name << ": " << Summary(r) << '\n'; }

// ---- Checks ------------------------------------------------------------------------------------------------------------

void Validation() {
    const auto v = MakeViews({}, Constant(0, 0), 0);
    const auto ok = Request(v);
    rg::ValidateRegistrationGateRequest(ok);
    auto r = ok; r.camera_b = nullptr;
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "missing image");
    r = ok; r.mapped.camera_b = {};
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "missing mapping");
    r = ok; r.baseline = {1, 0.01};
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "baseline not a unit vector");
    r = ok; r.design.minimum_baseline_component = std::numeric_limits<double>::quiet_NaN();
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "unset c_min");
    r = ok; r.design.lift_share = 1.5;
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "lift share over 1");
    r = ok; r.design.wide_search_radius_px = 0;
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "unset wide radius");
    r = ok; r.gate_limits.limit_px = std::numeric_limits<double>::quiet_NaN();
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "unset line");
    r = ok; r.correction = Predict({0, 0});
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "a correction without a parallax profile");
    r = ok; r.registered.parallax = a0::m2::experimental::ParallaxProfile{};
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "a parallax profile without its correction");
    r = ok; r.layout.band_half_width_px = 20;
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "band narrower than Omega");
    r = ok; r.band_measure_half_width_px = 20;
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "measured band narrower than Omega");
    r = ok; r.band_measure_half_width_px = 32;
    rg::ValidateRegistrationGateRequest(r);   // exactly Omega's half width is allowed
    r = ok; r.thread_count = 0;
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "zero threads");
    r = ok; r.layout.seam_column = 5000;
    Rejects([&] { rg::ValidateRegistrationGateRequest(r); }, "seam outside the output");
}

void Layout() {
    const auto v = MakeViews({}, Constant(0, 0), 0);
    const auto r = Request(v);
    const auto layout = rg::MakeRegistrationGateLayout(r);
    const std::uint32_t inset = gate::RequiredBandInsetPx(kDesign);
    Check(layout.omega_half_width_px == 32, "Omega's half width is half the mixing width, rounded up, plus the margin");
    bool omega_ok = true, band_ok = true, transition_ok = true;
    std::uint32_t rows = 0;
    for (std::uint32_t y = 0; y < layout.omega.height; ++y) {
        const auto o = layout.omega.band[y], b = layout.band.band[y], t = layout.omega.transition[y];
        if (o.begin == o.end) { transition_ok = transition_ok && t.begin == t.end; continue; }
        ++rows;
        omega_ok = omega_ok && o.begin == kSeam-32 && o.end == kSeam+33;
        band_ok = band_ok && b.begin == kSeam-128 && b.end == kSeam+129;
        transition_ok = transition_ok && t.begin == kSeam-4 && t.end == kSeam+5;
    }
    Check(omega_ok && band_ok && rows == kH-2*inset, "Omega and the band are the seam +- their half widths on every measurable row");
    Check(transition_ok, "the transition is the fixed seam's on the measurable rows and empty elsewhere");
    Check(layout.outside_measurable.size() == 2 && layout.outside_measurable[0].first == 0 &&
        layout.outside_measurable[0].last == inset-1 && layout.outside_measurable[1].first == kH-inset &&
        layout.outside_measurable[1].last == kH-1, "the rows the measurer cannot reach are reported");
}

void AlignedPasses() {
    const Point delta{0.3, -0.1};
    const auto v = MakeViews(Content(7), Constant(delta.x, delta.y), 2);
    auto r = Request(v); r.prediction = Predict(delta);
    const auto result = rg::MeasureRegistrationGate(r);
    Report("aligned", result);
    Check(result.state == rg::GateState::pass && result.cause == rg::GateCause::none && result.remedy == "none",
        "an aligned pair passes");
    Check(result.rows.rows_with_content > 5 && result.rows.dx_measured_share == 1 && result.rows.unmeasured.empty(),
        "every row of Omega with content measured the baseline component");
    Check(result.maxima.two_directional_px && std::abs(*result.maxima.two_directional_px-std::hypot(0.3, 0.1)) <= 0.06,
        "the two-directional maximum is the known shift");
    Check(result.before.maximum_px == result.after.maximum_px, "without layer 2, before equals after");
    bool inside = true;
    for (const auto& p : result.measurement->points) inside = inside && p.x >= kSeam-32 && p.x <= kSeam+32;
    Check(inside && !result.measurement->points.empty(), "the decision read Omega's points only");
}

void OmegaShiftFails() {
    // 2 px along the baseline in Omega, nothing beyond 120 px from the seam.
    const Field along = [](double x, double) { return Point{2*Step(x-kSeam, 60, 120), 0}; };
    auto v = MakeViews(Content(17), along, 3);
    auto r = Request(v);
    const auto lift = rg::MeasureRegistrationGate(r);
    Report("2 px along the baseline in Omega", lift);
    Check(lift.state == rg::GateState::fail && lift.maxima.two_directional_px && *lift.maxima.two_directional_px > 1.8,
        "2 px in Omega fails with the value");
    Check(lift.cause == rg::GateCause::lift && lift.lift && lift.lift->baseline_share >= 0.9 && lift.remedy == "shift-drawing-horizontal",
        "a displacement along the baseline is a lift, and the remedy moves the drawing");
    Check(!lift.lift->height_estimate_mm, "no height without k");
    r.baseline_px_per_mm = 0.5;
    const auto lift_k = rg::MeasureRegistrationGate(r);
    Check(lift_k.lift && lift_k.lift->height_estimate_mm && std::abs(*lift_k.lift->height_estimate_mm-4) <= 0.2, "the height is the baseline component over k");

    // 1.5 px across the baseline, uniform: a similarity explains it.
    v = MakeViews(Content(17), Constant(0, 1.5), 3);
    r = Request(v);
    const auto pose = rg::MeasureRegistrationGate(r);
    Report("1.5 px across the baseline", pose);
    Check(pose.state == rg::GateState::fail && pose.cause == rg::GateCause::pose && pose.remedy == "retake-then-recalibrate" && !pose.lift,
        "a uniform displacement across the baseline is a pose");

    // Across the baseline and varying along the seam faster than a similarity can follow.
    const Field wave = [](double, double y) { return Point{0, 1.6*std::sin(2*std::numbers::pi*y/220)}; };
    v = MakeViews(Content(17), wave, 3);
    r = Request(v);
    const auto other = rg::MeasureRegistrationGate(r);
    Report("a wave across the baseline", other);
    Check(other.state == rg::GateState::fail && other.cause == rg::GateCause::displacement && other.remedy == "retake",
        "a displacement neither a lift nor a pose is a displacement");
}

void OutsideOmegaOnly() {
    // 2 px along the baseline only farther than 100 px from the seam (the band, not Omega).
    const Field outside = [](double x, double) { return Point{2*(1-Step(x-kSeam, 70, 100)), 0}; };
    const auto v = MakeViews(Content(27), outside, 3);
    auto r = Request(v); r.prediction = Predict({0, 0});
    const auto result = rg::MeasureRegistrationGate(r);
    Report("2 px outside Omega only", result);
    Check(result.state == rg::GateState::pass && result.maxima.two_directional_px && *result.maxima.two_directional_px <= 0.3,
        "a displacement outside Omega does not enter the decision");
    Check(result.band && result.band->maxima.two_directional_px && *result.band->maxima.two_directional_px >= 1.8 &&
        result.band->result.state == gate::SeamGateState::fail, "the band diagnostic shows the displacement outside Omega");
}

// The shape behind the under-report: Omega holds only horizontal lines, so the gate's points are one-directional with a
// normal across the baseline, and a left-right displacement of 4 px is invisible to them.
std::vector<scene::Stroke> HorizontalAtSeam(std::uint64_t seed) {
    std::vector<scene::Stroke> s;
    scene::Drawing(s, {500, 0, kSeam-100.0, double(kH)}, seed, 2500);
    scene::Drawing(s, {kSeam+100.0, 0, 1300, double(kH)}, seed+1, 2500);
    scene::HorizontalLines(s, {kSeam-100.0, 0, kSeam+100.0, double(kH)}, seed+2);
    return s;
}
void HorizontalLinesOnly() {
    const auto v = MakeViews(HorizontalAtSeam(37), Constant(4, 0), 5);
    const auto r = Request(v);
    const auto result = rg::MeasureRegistrationGate(r);
    Report("horizontal lines at the seam, 4 px left-right", result);
    Check(result.state != rg::GateState::pass, "Omega with horizontal lines only does not pass");
    Check(!result.rows.unmeasured.empty() && result.rows.dx_measured_share < 1, "the rows without a baseline component are reported unmeasured");
    Check(!result.maxima.two_directional_px && result.maxima.one_directional_px && *result.maxima.one_directional_px < 0.3 &&
        result.maxima.one_directional_component == rg::OneDirectionalComponent::across_baseline &&
        result.maxima.one_directional_normal && std::abs(result.maxima.one_directional_normal->y) > 0.99,
        "the maximum is one-directional, across the baseline, and not a two-directional value");
    bool has = false;
    for (const auto& f : result.gate_findings) has = has || (f.reason == "dx-unmeasured" && f.remedy == "review-visually");
    Check(has, "the finding dx-unmeasured is given");
    // The anchors come from the band, where the drawing is: the seam gate alone passes this pair (the under-report), this part
    // does not, and only because of the unmeasured rows.
    Check(result.seam_gate && result.seam_gate->state == gate::SeamGateState::pass, "with the band's anchors the seam gate alone passes");
    Check(result.state == rg::GateState::undetermined && result.gate_findings.size() == 1, "the only reason not to pass is dx-unmeasured");
    Check(result.band && result.band->maxima.two_directional_px && std::abs(*result.band->maxima.two_directional_px-4) <= 0.1,
        "the band diagnostic sees the 4 px the seam's lines cannot");
}

// 11.10 "the anchors when Omega is narrow": Omega alone holds no two-directional point here, the band does.
void AnchorsFromTheBand() {
    const auto v = MakeViews(HorizontalAtSeam(37), Constant(0.3, 0), 2);
    auto r = Request(v);
    r.measure_whole_band_for_anchors = false;
    const auto omega_only = rg::MeasureRegistrationGate(r);
    r.measure_whole_band_for_anchors = true;
    const auto band = rg::MeasureRegistrationGate(r);
    Report("anchors from Omega only", omega_only);
    Report("anchors from the band", band);
    Check(omega_only.seam_gate->global_verified < kDesign.global_minimum_anchors && !omega_only.band &&
        omega_only.anchor_columns && omega_only.anchor_columns->first == kSeam-32 && omega_only.anchor_columns->second == kSeam+32,
        "from Omega alone the placement is not verified, and the range says so");
    Check(band.seam_gate->global_verified >= kDesign.global_minimum_anchors && band.anchor_columns &&
        band.anchor_columns->first == kSeam-128 && band.anchor_columns->second == kSeam+128, "from the band it is, and the range is the band");
    bool anchors_outside_omega = !band.measurement->global.anchors.empty();
    for (const auto& a : band.measurement->global.anchors) anchors_outside_omega = anchors_outside_omega && (a.x < kSeam-32 || a.x > kSeam+32);
    Check(anchors_outside_omega, "the anchors lie outside Omega; the decided points do not");
    bool inside = true;
    for (const auto& p : band.measurement->points) inside = inside && p.x >= kSeam-32 && p.x <= kSeam+32;
    Check(inside, "the decision still reads Omega's points only");
}

void WideSearchDiagnostic() {
    const auto v = MakeViews(Content(87), Constant(30, 0), 31);
    auto r = Request(v);
    r.wide_search = true; r.output_pixel_mm = 0.25;
    const auto result = rg::MeasureRegistrationGate(r);
    Report("30 px along the baseline, wide search", result);
    const auto& w = result.wide_search;
    if (w) std::cout << "  wide: blocks=" << w->blocks << " unique=" << w->unique_blocks << " agreeing=" << w->agreeing_blocks
                     << " offset=" << w->offset_px.value_or(-1) << " px\n";
    Check(w && w->offset_px && std::abs(*w->offset_px-30) <= 0.5 && w->agreeing_blocks >= 2 && w->offset_mm &&
        std::abs(*w->offset_mm-*w->offset_px*0.25) == 0 && w->radius_px == 40, "the wide search finds 30 px along the baseline");
    Check(result.after.maximum_px == result.seam_gate->maximum_px, "the wide search is not part of the maximum");
    // Horizontal lines give no offset along the baseline: the search does not invent one.
    const auto lines = MakeViews(HorizontalAtSeam(37), Constant(30, 0), 31);
    auto rl = Request(lines);
    rl.wide_search = true; rl.design.wide_search_half_width_px = 48;
    const auto lr = rg::MeasureRegistrationGate(rl);
    if (lr.wide_search) std::cout << "  lines: blocks=" << lr.wide_search->blocks << " unique=" << lr.wide_search->unique_blocks << '\n';
    Check(lr.wide_search && !lr.wide_search->offset_px && lr.wide_search->blocks > 0 && lr.wide_search->unique_blocks == 0,
        "horizontal lines at the seam give no wide offset");
    // Not asked and Omega clear: no search.
    auto rn = Request(v); rn.design.wide_search_trigger_share = 1;
    Check(!rg::MeasureRegistrationGate(rn).wide_search, "without the request or the trigger the search does not run");
}

// A constructed Omega measurement on the layout of the test pair: the seam gate's decision is known, and this part's is checked
// against it. Points at the seam column on the lattice rows of Omega.
struct Constructed { rg::RegistrationGateRequest request; rg::RegistrationGateLayout layout; gate::SeamMeasurement m; Views v; };
Constructed Construct(const std::function<gate::SeamPoint(std::uint32_t)>& point_at) {
    Constructed c;
    c.v = MakeViews({}, Constant(0, 0), 0);
    c.request = Request(c.v);
    c.request.camera_a = &c.v.a; c.request.camera_b = &c.v.b;
    c.layout = rg::MakeRegistrationGateLayout(c.request);
    auto& m = c.m;
    m.design = kDesign; m.limits = kLimits; m.width = c.layout.omega.width; m.height = c.layout.omega.height;
    m.containment = gate::CheckTransitionBandContained(c.layout.omega.band, c.layout.omega.transition, m.width);
    m.global.performed = true; m.global.verified = kDesign.global_minimum_anchors; m.global.evaluated = kDesign.global_minimum_anchors;
    const std::uint32_t inset = gate::RequiredBandInsetPx(kDesign);
    m.block_columns = 1; m.block_rows = 1;
    gate::SeamBlock b; b.block_class = gate::BlockClass::measured;
    for (std::uint32_t y = inset+16; y+inset < kH; y += 32) {
        auto p = point_at(y);
        p.x = kSeam; p.y = y; p.content = true; p.status = gate::PointStatus::valid;
        p.visible_px = p.kind == gate::PointKind::one_directional ? std::abs(p.normal.x*p.displacement.x+p.normal.y*p.displacement.y) :
            std::hypot(p.displacement.x, p.displacement.y);
        p.confirmation = p.visible_px+kLimits.guard_band_px > kLimits.limit_px ? gate::Confirmation::confirmed : gate::Confirmation::not_required;
        m.points.push_back(p);
        ++b.points; ++b.content_points; ++b.valid_points; ++b.counted_points;
    }
    m.blocks.push_back(b);
    return c;
}
rg::RegistrationGateResult Decide(const Constructed& c) {
    auto r = c.request;   // the images of this copy (the request was made before the struct moved)
    r.camera_a = &c.v.a; r.camera_b = &c.v.b;
    return rg::DecideRegistrationGate(r, c.layout, {c.m, std::nullopt});
}

void ConstructedDecisions() {
    // The under-report: horizontal lines everywhere in Omega, the seam gate passes on them.
    const auto lines = Construct([](std::uint32_t) {
        gate::SeamPoint p; p.kind = gate::PointKind::one_directional; p.normal = {0, 1}; p.displacement = {40, 0.1}; return p; });
    const auto lr = Decide(lines);
    Check(lr.seam_gate && lr.seam_gate->state == gate::SeamGateState::pass && *lr.seam_gate->maximum_px == 0.1,
        "constructed: the seam gate alone passes horizontal lines with a maximum of 0.1 px");
    Check(lr.state == rg::GateState::undetermined, "constructed: this part does not pass them");
    Check(!lr.maxima.two_directional_px && lr.maxima.one_directional_px == 0.1 &&
        lr.maxima.one_directional_component == rg::OneDirectionalComponent::across_baseline,
        "constructed: the 0.1 px is a one-directional value across the baseline, not a maximum of the displacement");
    Check(lr.rows.rows_with_content == lines.m.points.size() && lr.rows.rows_dx_measured == 0 && lr.rows.unmeasured.size() == 1 &&
        lr.rows.unmeasured[0].first == lines.m.points.front().y-16 && lr.rows.unmeasured[0].last == lines.m.points.back().y+15,
        "constructed: every lattice row is unmeasured, as one range of output rows");

    // A vertical line (normal along the baseline) measures the baseline component.
    const auto vertical = Construct([](std::uint32_t) {
        gate::SeamPoint p; p.kind = gate::PointKind::one_directional; p.normal = {1, 0}; p.displacement = {0.1, 3}; return p; });
    const auto vr = Decide(vertical);
    Check(vr.state == rg::GateState::pass && vr.rows.dx_measured_share == 1 &&
        vr.maxima.one_directional_component == rg::OneDirectionalComponent::with_baseline, "constructed: vertical lines measure dx");

    // 11.30: a row that measured lines along the baseline only is information while other rows measured the baseline component:
    // it is reported, and the pass stands.
    const auto one_row = Construct([](std::uint32_t y) {
        gate::SeamPoint p; p.displacement = {0.1, 0};
        if (y == 86+16+32*3) { p.kind = gate::PointKind::one_directional; p.normal = {0, 1}; }
        return p; });
    const auto orr = Decide(one_row);
    Check(orr.state == rg::GateState::pass && orr.gate_findings.empty() && orr.rows.unmeasured.size() == 1 &&
        orr.rows.unmeasured[0].first == 86+16+32*3-16 && orr.rows.unmeasured[0].last == 86+16+32*3+15 &&
        orr.rows.rows_measured == orr.rows.rows_with_content && orr.rows.rows_dx_measured+1 == orr.rows.rows_measured,
        "constructed: one row of lines along the baseline is reported and does not lower the pass");
    Check(orr.placement == rg::PlacementBasis::anchors, "constructed: the anchors verified the placement");

    // Causes from constructed displacements.
    const auto lift = Decide(Construct([](std::uint32_t) { gate::SeamPoint p; p.displacement = {-2, 0.2}; return p; }));
    Check(lift.state == rg::GateState::fail && lift.cause == rg::GateCause::lift && lift.lift && lift.lift->baseline_share == 1 &&
        lift.lift->sign_agreement_share == 1, "constructed: a uniform baseline displacement is a lift");
    const auto mixed = Decide(Construct([](std::uint32_t y) { gate::SeamPoint p; p.displacement = {(y/32)%2 ? 2.0 : -2.0, 0}; return p; }));
    Check(mixed.cause != rg::GateCause::lift, "constructed: baseline displacements of both signs are not a lift");
    const auto pose = Decide(Construct([](std::uint32_t y) { gate::SeamPoint p; p.displacement = {0.2, 1.2+0.002*y}; return p; }));
    Check(pose.cause == rg::GateCause::pose, "constructed: a displacement linear along the seam is a pose");
    const auto single = Decide(Construct([](std::uint32_t y) { gate::SeamPoint p; p.displacement = {0, y == 230 ? 2.0 : 0.0}; return p; }));
    Check(single.state == rg::GateState::fail && single.cause == rg::GateCause::displacement, "constructed: a single step is a displacement");

    // Unmeasurable content (a block whose value differs from every measured neighbour), and a placement that is not verified.
    auto blocked = Construct([](std::uint32_t) { gate::SeamPoint p; p.displacement = {0.1, 0}; return p; });
    blocked.m.blocks[0].block_class = gate::BlockClass::unmeasurable_content; blocked.m.blocks[0].neighbor_inconsistent = true;
    const auto br = Decide(blocked);
    Check(br.state == rg::GateState::fail && br.cause == rg::GateCause::unmeasurable && br.remedy == "shift-drawing-horizontal",
        "constructed: unmeasurable content fails with its cause");
    auto unverified = Construct([](std::uint32_t) { gate::SeamPoint p; p.displacement = {0.1, 0}; return p; });
    unverified.m.global.verified = 0;
    const auto ur = Decide(unverified);
    Check(ur.state == rg::GateState::undetermined && ur.cause == rg::GateCause::none && !ur.gate_findings.empty() &&
        ur.gate_findings.back().reason == "placement-unverified" && ur.placement == rg::PlacementBasis::none,
        "constructed: an unverified placement is undetermined, not a pass");
    // 11.30: the same anchors with B placed by a trusted estimate: the estimate verifies the placement.
    unverified.request.placement_by_estimate = rg::PlacementByEstimate{6, 6};
    const auto ue = Decide(unverified);
    Check(ue.state == rg::GateState::pass && ue.gate_findings.empty() && ue.placement == rg::PlacementBasis::estimate &&
        ue.seam_gate && ue.seam_gate->state == gate::SeamGateState::fail, "constructed: a trusted estimate verifies the placement the anchors could not");
    // A contradicted anchor is not set aside by the estimate.
    auto contradicted = unverified;
    contradicted.m.global.contradicted = 1;
    const auto cr = Decide(contradicted);
    Check(cr.state == rg::GateState::fail && cr.placement == rg::PlacementBasis::none, "constructed: a contradicted anchor fails with a trusted estimate too");

    // 11.30: blank rows (no content in either camera) are reported and are no reason; a block of them is the seam gate's
    // unpredicted-empty-block.
    auto white = Construct([](std::uint32_t) { gate::SeamPoint p; p.displacement = {0.1, 0}; return p; });
    {
        auto& m = white.m;
        m.blocks.clear(); m.block_rows = static_cast<std::uint32_t>(m.points.size());
        for (std::uint32_t i = 0; i < m.points.size(); ++i) {
            auto& p = m.points[i];
            p.block_row = i;
            gate::SeamBlock b; b.row = i; b.points = 1;
            if (i%4 == 0) { b.block_class = gate::BlockClass::measured; b.content_points = b.valid_points = b.counted_points = 1; }
            else { p.content = false; p.status = gate::PointStatus::no_content; b.block_class = gate::BlockClass::empty; }
            m.blocks.push_back(b);
        }
    }
    const auto wr = Decide(white);
    Check(wr.seam_gate && wr.seam_gate->state == gate::SeamGateState::fail && wr.seam_gate->unpredicted_empty_blocks > 0,
        "constructed: the seam gate fails a sheet that is mostly blank without a prediction");
    Check(wr.state == rg::GateState::pass && wr.gate_findings.empty() && wr.rows.rows_blank > wr.rows.rows_with_content &&
        wr.rows.rows_blank+wr.rows.rows_with_content == white.m.points.size() && !wr.rows.blank.empty() && wr.rows.rows_content_unmeasured == 0,
        "constructed: this part passes it and reports the blank rows");

    // 11.30: content that nothing measured. Over the share line it fails; at the line it passes and is reported; unconfirmed
    // (#385 Q1) never passes.
    const auto with_unmeasured = [](std::uint32_t every) {
        auto c = Construct([](std::uint32_t) { gate::SeamPoint p; p.displacement = {0.1, 0}; return p; });
        for (std::size_t i = 0; i < c.m.points.size(); ++i) if (i%every == 0) c.m.points[i].status = gate::PointStatus::periodic;
        return c;
    };
    const auto fifth = with_unmeasured(5), third = with_unmeasured(3);
    const auto qr = Decide(fifth), tr = Decide(third);
    std::cout << "  unmeasured share " << qr.rows.content_unmeasured_share << " and " << tr.rows.content_unmeasured_share << '\n';
    Check(qr.rows.content_unmeasured_share > 0 && qr.rows.content_unmeasured_share <= 0.25 && qr.state == rg::GateState::pass &&
        qr.rows.rows_content_unmeasured > 0 && !qr.rows.content_unmeasured.empty(), "constructed: content unmeasured on at most a quarter of the rows passes and is reported");
    Check(tr.rows.content_unmeasured_share > 0.25 && tr.state == rg::GateState::fail && tr.cause == rg::GateCause::unmeasurable &&
        tr.gate_findings.size() == 1 && tr.gate_findings[0].reason == "content-unmeasured", "constructed: over the share line it fails");
    auto unconfirmed_point = Construct([](std::uint32_t) { gate::SeamPoint p; p.displacement = {0.1, 0}; return p; });
    unconfirmed_point.m.points[5].confirmation = gate::Confirmation::unconfirmed;
    const auto uc = Decide(unconfirmed_point);
    Check(uc.state == rg::GateState::fail && uc.cause == rg::GateCause::unmeasurable && uc.rows.rows_unconfirmed == 1 &&
        uc.gate_findings.size() == 1 && uc.gate_findings[0].reason == "unconfirmed-content", "constructed: one unconfirmed point never passes");
    // Nothing measured: a blank Omega does not pass (D1 of #385).
    auto empty = Construct([](std::uint32_t) { gate::SeamPoint p; return p; });
    for (auto& p : empty.m.points) { p.content = false; p.status = gate::PointStatus::no_content; }
    empty.m.blocks[0].block_class = gate::BlockClass::empty;
    const auto er = Decide(empty);
    Check(er.state == rg::GateState::fail && er.cause == rg::GateCause::unmeasurable && er.rows.rows_with_content == 0 &&
        er.gate_findings.size() == 1 && er.gate_findings[0].reason == "nothing-measured", "constructed: a blank Omega does not pass");
    // A block the seam gate left unmeasurable only because its counted points (one above the other in Omega) do not agree: its
    // rows are measured, and their values are in the maximum. A block with an unconfirmed point is refused (#385 Q1).
    auto disagree = Construct([](std::uint32_t y) { gate::SeamPoint p; p.displacement = {(y/32)%2 ? 0.1 : 0.5, 0}; return p; });
    disagree.m.blocks[0].block_class = gate::BlockClass::unmeasurable_content;
    const auto dr = Decide(disagree);
    Check(dr.state == rg::GateState::pass && dr.rows.rows_measured == dr.rows.rows_with_content && dr.maxima.two_directional_px == 0.5,
        "constructed: points of a block that do not agree are measured rows");
    auto refused = disagree;
    refused.m.blocks[0].unconfirmed_points = 1;
    const auto rr = Decide(refused);
    Check(rr.rows.rows_measured == 0 && rr.rows.rows_content_unmeasured == rr.rows.rows_with_content, "constructed: a refused block measures no row");

    // The decision reads Omega's points only: a point outside Omega is a programming error.
    auto stray = Construct([](std::uint32_t) { gate::SeamPoint p; return p; });
    stray.m.points[0].x = kSeam+40;
    Rejects([&] { (void)Decide(stray); }, "constructed: a point outside Omega is rejected");
    auto limits = Construct([](std::uint32_t) { gate::SeamPoint p; return p; });
    limits.m.limits.limit_px = 2;
    Rejects([&] { (void)Decide(limits); }, "constructed: a measurement with other limits is rejected");

    // The trigger of the wide search: Omega's content points mostly ambiguous (the blank images give it no block to search).
    auto unclear = Construct([](std::uint32_t) { gate::SeamPoint p; p.displacement = {0.1, 0}; return p; });
    for (std::size_t i = 0; i+3 < unclear.m.points.size(); ++i) unclear.m.points[i].status = gate::PointStatus::ambiguous;
    const auto ar = Decide(unclear);
    Check(ar.wide_search && ar.wide_search->blocks == 0 && !ar.wide_search->offset_px, "constructed: many ambiguous points run the wide search");
    Check(!Decide(Construct([](std::uint32_t) { gate::SeamPoint p; return p; })).wide_search, "constructed: clear points do not");
}

void TransitionOutside() {
    const auto v = MakeViews(Content(47), Constant(0.2, 0), 2);
    auto r = Request(v); r.prediction = Predict({0.2, 0});
    r.blend.width_pixels = 100;   // the composite mixes over 100 px; Omega is 65 px wide
    const auto wide = rg::MeasureRegistrationGate(r);
    Report("blend wider than Omega", wide);
    bool has = false;
    for (const auto& f : wide.gate_findings) has = has || (f.reason == "transition-outside-band" && f.remedy == "contact-technician");
    Check(wide.state == rg::GateState::undetermined && has, "a transition outside Omega is undetermined");
    r = Request(v); r.prediction = Predict({0.2, 0});
    r.layout.seam_column = 728;   // the measurable overlap starts at 726: Omega is clipped and the transition leaves it
    const auto edge = rg::MeasureRegistrationGate(r);
    Report("seam at the edge of the measurable overlap", edge);
    Check(edge.state == rg::GateState::undetermined, "a transition outside the measurable overlap is undetermined");
}

void LatticeOneOff() {
    std::vector<scene::Stroke> s;
    scene::Lattice(s, {0, 0, 2000, double(kH)}, 96, kSeam+10, 300);
    const auto v = MakeViews(s, Constant(96, 0), 98);
    const auto r = Request(v);
    const auto result = rg::MeasureRegistrationGate(r);
    Report("lattice one period off", result);
    Check(result.state != rg::GateState::pass, "a mapping one lattice period off does not pass");
}

// Layer 2 by hand: B shows a displacement q(y) w(x) along the baseline; the corrected mapping samples B where the displacement
// puts A's feature, and the correction passed to the gate is that displacement.
void BeforeAndAfter() {
    const auto q = [](double y) { return 1.5+0.5*std::sin(2*std::numbers::pi*y/600); };
    // Flat over every patch of Omega (|u| <= 32 + 34), falling to 0 beyond.
    const Field field = [q](double x, double y) { return Point{q(y)*Step(x-kSeam, 80, 160), 0}; };
    const auto v = MakeViews(Content(57), field, 3);
    auto r = Request(v); r.prediction = Predict({0, 0});
    const auto plain = rg::MeasureRegistrationGate(r);   // measured without the correction: the "before" measured again
    auto corrected = r;
    const auto b = r.mapped.camera_b;
    corrected.mapped.camera_b = [b, field](Point o, Point& raw) { const auto d = field(o.x, o.y); return b({o.x+d.x, o.y+d.y}, raw); };
    corrected.correction = [field](Point o, Point& s) { s = field(o.x, o.y); return true; };
    a0::m2::experimental::ParallaxProfile profile;
    profile.baseline = {1, 0}; profile.a_px = 80; profile.l_px = 80; profile.seam_column = kSeam;
    corrected.registered.parallax = profile;
    const auto after = rg::MeasureRegistrationGate(corrected);
    Report("layer 2 by hand, after", after);
    Report("layer 2 by hand, measured without it", plain);
    Check(after.state == rg::GateState::pass && after.after.maxima.two_directional_px && *after.after.maxima.two_directional_px <= 0.3,
        "after the correction Omega passes");
    Check(after.before.maxima.two_directional_px && plain.maxima.two_directional_px &&
        std::abs(*after.before.maxima.two_directional_px-*plain.maxima.two_directional_px) <= 0.1,
        "before (after plus the correction) matches the maximum measured without the correction");
    // Point by point on the common lattice.
    double worst = 0; std::size_t compared = 0;
    const auto& pa = after.measurement->points; const auto& pp = plain.measurement->points;
    for (std::size_t i = 0; i < pa.size() && i < pp.size(); ++i) {
        if (!pa[i].Counted() || !pp[i].Counted() || pa[i].x != pp[i].x || pa[i].y != pp[i].y) continue;
        if (pa[i].kind != gate::PointKind::two_directional || pp[i].kind != gate::PointKind::two_directional) continue;
        const auto s = field(pa[i].x, pa[i].y);
        worst = std::max(worst, std::hypot(pa[i].displacement.x+s.x-pp[i].displacement.x, pa[i].displacement.y+s.y-pp[i].displacement.y));
        ++compared;
    }
    std::cout << "  before vs measured again: " << compared << " points, largest difference " << worst << " px\n";
    Check(compared > 10 && worst <= 0.1, "before matches the measurement without the correction point by point (0.1 px)");
}

// ---- 11.30 on the paper scenes of tests/parallax_scene.hpp -------------------------------------------------------------------
// The generator's rig and raster (48 DPI) with the production measurer: its wide anchor window (768 px) cannot fit the overlap of
// this rig, so every anchor is indeterminate (window-unavailable), as on the real pairs. B at the calibrated pose, the paper flat.
namespace ps = parallax_scene;
struct PaperScene { ps::Rig rig; ps::SeamPlacement seam; ps::Scene scene; };
PaperScene MakePaper(const std::string& name) {
    PaperScene p{ps::StandardRig(), {}, {}};
    p.seam = ps::PlaceSeam(p.rig);
    for (const auto& spec : ps::GateScenes(p.seam)) if (spec.name == name) p.scene = ps::MakeScene(p.rig, p.seam, spec, {true, {false}});
    if (p.scene.spec.name != name) throw std::runtime_error("no gate scene "+name);
    return p;
}
rg::RegistrationGateRequest PaperRequest(const PaperScene& p, const BgrImage& a, const BgrImage& b) {
    rg::RegistrationGateRequest r;
    r.camera_a = &a; r.camera_b = &b;
    r.mapped = a0::m2::experimental::RegisteredDocumentMappedFixedSeamInput(p.rig.Parameters(), r.registered, p.seam.column);
    r.layout.seam_column = p.seam.column; r.layout.mixing_width_px = 8; r.layout.omega_margin_px = 8; r.layout.guard_px = 0;
    r.layout.band_half_width_px = 128;
    r.blend = a0::m2::experimental::kSeamBlendDesign;
    r.gate_design = gate::kSeamGateDesignCalibrated; r.gate_limits = kLimits; r.design = TestDesign();
    // At the generator's 48 DPI the measurer's 64 px patch is 34 mm: the two lattice rows over each edge of a band, where the patch
    // holds the edge beside blank paper, are periodic for the measurer's own check, and a band that leaves most rows blank has no
    // more than about as many rows inside it. A test line of one half here keeps the blank rows and the anchors the subject; the
    // line itself is checked on the constructed measurements.
    r.design.maximum_unmeasured_content_share = 0.5;
    const double n = std::hypot(p.scene.truth.e_b.x, p.scene.truth.e_b.y);
    r.baseline = {p.scene.truth.e_b.x/n, p.scene.truth.e_b.y/n}; r.thread_count = Threads();
    return r;
}
bool HasOwn(const rg::RegistrationGateResult& r, const std::string& reason) {
    for (const auto& f : r.gate_findings) if (f.reason == reason) return true;
    return false;
}
void PaperDecisions() {
    // A sheet that is mostly blank at the seam, the values within the line, the anchors indeterminate.
    const auto blank = MakePaper("gate_mostly_blank");
    auto r = PaperRequest(blank, blank.scene.image_a, blank.scene.image_b);
    const auto none = rg::MeasureRegistrationGate(r);
    Report("mostly blank, no trusted estimate", none);
    r.placement_by_estimate = rg::PlacementByEstimate{4, 4};
    const auto trusted = rg::MeasureRegistrationGate(r);
    Report("mostly blank, B placed by a trusted estimate", trusted);
    std::cout << "  rows: blank " << trusted.rows.rows_blank << ", measured " << trusted.rows.rows_measured << " (dx " << trusted.rows.rows_dx_measured
              << "), unmeasured " << trusted.rows.rows_content_unmeasured << ", anchors verified " << (trusted.seam_gate ? trusted.seam_gate->global_verified : 0)
              << " of " << (trusted.seam_gate ? trusted.seam_gate->global_evaluated : 0) << '\n';
    Check(trusted.seam_gate && trusted.seam_gate->state == gate::SeamGateState::fail && trusted.seam_gate->unpredicted_empty_blocks > 0 &&
        trusted.seam_gate->global_contradicted == 0 && trusted.seam_gate->global_verified < gate::kSeamGateDesignCalibrated.global_minimum_anchors,
        "paper: the seam gate fails a mostly blank sheet without a prediction, and its anchors verify nothing");
    Check(trusted.rows.rows_blank > trusted.rows.rows_with_content && trusted.rows.rows_measured > 0 && !trusted.rows.blank.empty(),
        "paper: most rows of Omega are blank, and they are reported");
    Check(trusted.maxima.two_directional_px && *trusted.maxima.two_directional_px+kLimits.guard_band_px <= kLimits.limit_px,
        "paper: the measured rows are within the line");
    Check(trusted.state == rg::GateState::pass && trusted.gate_findings.empty() && trusted.placement == rg::PlacementBasis::estimate,
        "paper: with B placed by a trusted estimate the gate passes; blank rows and indeterminate anchors are no reason");
    Check(none.state == rg::GateState::undetermined && none.gate_findings.size() == 1 && none.gate_findings[0].reason == "placement-unverified" &&
        none.placement == rg::PlacementBasis::none, "paper: without one the placement is unverified, and that is the only reason");

    // A mark in one camera only, in a blank stretch of the seam: content that one camera shows and the other does not.
    const gate::SeamPoint* mark = nullptr;
    {
        const auto& pts = trusted.measurement->points;
        std::size_t best = 0, best_length = 0;
        for (std::size_t i = 0; i < pts.size();) {
            std::size_t j = i;
            while (j < pts.size() && !pts[j].content) ++j;
            if (j-i > best_length) { best = i; best_length = j-i; }
            i = j == i ? i+1 : j;
        }
        if (best_length >= 3) mark = &pts[best+best_length/2];
    }
    Check(mark != nullptr, "paper: the blank sheet has a blank stretch of three rows");
    if (mark) {
        Point raw{};
        auto a = blank.scene.image_a;
        Check(r.mapped.camera_a({static_cast<double>(mark->x), static_cast<double>(mark->y)}, raw), "paper: the mark's point is in A");
        ps::PaintSpot(a, raw, 8, 20);
        auto rs = PaperRequest(blank, a, blank.scene.image_b); rs.placement_by_estimate = rg::PlacementByEstimate{4, 4};
        const auto one = rg::MeasureRegistrationGate(rs);
        Report("a mark in A only", one);
        Check(one.state == rg::GateState::fail && one.cause == rg::GateCause::unmeasurable && one.rows.rows_one_sided >= 1 && HasOwn(one, "one-sided-content"),
            "paper: content in one camera only is never a pass");
        // The same mark in both cameras is no one-sided content.
        auto b = blank.scene.image_b;
        Point raw_b{};
        Check(rs.mapped.camera_b({static_cast<double>(mark->x), static_cast<double>(mark->y)}, raw_b), "paper: the mark's point is in B");
        ps::PaintSpot(b, raw_b, 8, 20);
        auto rb = PaperRequest(blank, a, b); rb.placement_by_estimate = rg::PlacementByEstimate{4, 4};
        const auto both = rg::MeasureRegistrationGate(rb);
        Report("the mark in both", both);
        Check(both.rows.rows_one_sided == 0 && !HasOwn(both, "one-sided-content"), "paper: a mark both cameras show is not one-sided");
    }

    // Content the gate cannot measure (a fine grid) on more than a quarter of the rows with content.
    const auto grid = MakePaper("gate_periodic_bands");
    auto rg_ = PaperRequest(grid, grid.scene.image_a, grid.scene.image_b); rg_.placement_by_estimate = rg::PlacementByEstimate{4, 4};
    const auto periodic = rg::MeasureRegistrationGate(rg_);
    Report("periodic bands", periodic);
    std::cout << "  unmeasured " << periodic.rows.rows_content_unmeasured << " of " << periodic.rows.rows_with_content << " rows with content (share "
              << periodic.rows.content_unmeasured_share << ")\n";
    Check(periodic.rows.content_unmeasured_share > rg_.design.maximum_unmeasured_content_share && periodic.state == rg::GateState::fail &&
        periodic.cause == rg::GateCause::unmeasurable && HasOwn(periodic, "content-unmeasured") && !periodic.rows.content_unmeasured.empty(),
        "paper: content left unmeasured on more than the share line is not a pass");
}

// The band the gate measures is its own value, the smaller of it and the layout's band; Omega and the decision on Omega do not move.
void BandMeasureWidth() {
    const auto v = MakeViews(Content(71), Constant(0.35, -0.1), 2);
    auto wide = Request(v); wide.prediction = Predict({0.35, -0.1});
    auto narrow = wide; narrow.band_measure_half_width_px = 64;
    auto wider = wide; wider.band_measure_half_width_px = 512;   // never widens the layout's band
    const auto l_wide = rg::MakeRegistrationGateLayout(wide), l_narrow = rg::MakeRegistrationGateLayout(narrow), l_wider = rg::MakeRegistrationGateLayout(wider);
    bool narrow_ok = true, wider_ok = true, omega_ok = true, rows = false;
    for (std::uint32_t y = 0; y < l_wide.band.height; ++y) {
        const auto w = l_wide.band.band[y], n = l_narrow.band.band[y], x = l_wider.band.band[y];
        omega_ok = omega_ok && l_narrow.omega.band[y].begin == l_wide.omega.band[y].begin && l_narrow.omega.band[y].end == l_wide.omega.band[y].end &&
            l_narrow.omega.transition[y].begin == l_wide.omega.transition[y].begin && l_narrow.omega.transition[y].end == l_wide.omega.transition[y].end;
        wider_ok = wider_ok && x.begin == w.begin && x.end == w.end;
        if (w.begin == w.end) { narrow_ok = narrow_ok && n.begin == n.end; continue; }
        rows = true;
        narrow_ok = narrow_ok && n.begin == kSeam-64 && n.end == kSeam+65;
    }
    Check(rows && narrow_ok, "the measured band is the seam +- its own half width on every measurable row");
    Check(wider_ok, "a measured band wider than the layout's band is the layout's");
    Check(omega_ok && l_narrow.omega_half_width_px == l_wide.omega_half_width_px && Text(l_narrow.outside_measurable) == Text(l_wide.outside_measurable),
        "Omega, its transition and the rows outside the measurer's reach do not depend on the measured band");
    const auto a = rg::MeasureRegistrationGate(wide), b = rg::MeasureRegistrationGate(narrow);
    Report("band measured wide", a);
    Report("band measured narrow", b);
    Check(a.band && b.band && b.band->result.counted_points < a.band->result.counted_points && b.anchor_columns &&
        b.anchor_columns->first == kSeam-64 && b.anchor_columns->second == kSeam+64, "the band diagnostic and the anchors' range are the measured band's");
    // What the decision reads is Omega's points: the same state, rows and maxima whatever the measured band is (the anchors may differ).
    Check(a.state == b.state && a.cause == b.cause && Text(a.rows) == Text(b.rows) && Text(a.maxima) == Text(b.maxima) &&
        Hex(a.after.maximum_px) == Hex(b.after.maximum_px) && a.measurement->points.size() == b.measurement->points.size(),
        "the decision on Omega is the same with a narrower measured band");
    narrow.thread_count = 1;
    auto four = narrow; four.thread_count = 4;
    Check(Fingerprint(rg::MeasureRegistrationGate(narrow)) == Fingerprint(rg::MeasureRegistrationGate(four)), "the narrow band gives the same bytes on 1 and 4 threads");
}

std::string PointsText(const gate::SeamMeasurement& m) {
    std::string s;
    for (const auto& p : m.points)
        s += std::to_string(p.x)+","+std::to_string(p.y)+","+std::string(gate::PointStatusCode(p.status))+","+Hex(p.displacement.x)+","+Hex(p.displacement.y)+","+
            Hex(p.uncertainty_px)+","+Hex(p.correlation)+","+std::to_string(static_cast<int>(p.confirmation))+";";
    return s;
}
// The global check of Omega is the work the decision drops when the band decides it: it runs on one anchor, nothing else changes.
void OmegaGlobalLeast() {
    const auto v = MakeViews(Content(73), Constant(0.4, 0.2), 2);
    auto r = Request(v); r.prediction = Predict({0.4, 0.2});
    const auto layout = rg::MakeRegistrationGateLayout(r);
    const auto full = gate::MeasureSeam(*r.camera_a, *r.camera_b, layout.omega, r.gate_design, r.gate_limits, r.prediction, r.thread_count);
    const auto least = rg::MeasureGateOmega(r, layout, true);
    const auto kept = rg::MeasureGateOmega(r, layout, false);
    Check(full.global.evaluated > 1 && least.global.evaluated == 1, "Omega's global check runs on one anchor when the band decides it, and on all of them otherwise");
    Check(PointsText(least) == PointsText(full) && !least.points.empty(), "Omega's points are the same with the least global check");
    Check(least.blocks.size() == full.blocks.size() && least.design.global_maximum_evaluations == r.gate_design.global_maximum_evaluations &&
        least.design.global_minimum_anchors == r.gate_design.global_minimum_anchors, "the measurement records the request's design, not the reduced one");
    Check(PointsText(kept) == PointsText(full) && kept.global.evaluated == full.global.evaluated && kept.global.verified == full.global.verified,
        "without the band, Omega is measured as before");
    // The decision: Omega measured either way, with the band's check, is one result byte for byte.
    const auto decide = [&](const gate::SeamMeasurement& omega) {
        rg::RegistrationGateMeasurements m;
        m.omega = omega;
        m.band = gate::MeasureSeam(*r.camera_a, *r.camera_b, layout.band, r.gate_design, r.gate_limits, r.prediction, r.thread_count);
        return Fingerprint(rg::DecideRegistrationGate(r, layout, std::move(m)));
    };
    Check(decide(least) == decide(full), "the decision does not depend on how many anchors Omega's own check ran");
    // The band path of the one entry measures Omega the same way.
    r.measure_whole_band_for_anchors = true;
    const auto entry = Fingerprint(rg::MeasureRegistrationGate(r));
    Check(entry == decide(full), "the entry that measures both gives the decision of the full measurements");
    // Without the band, Omega's own check decides and stays whole.
    r.measure_whole_band_for_anchors = false;
    const auto solo = rg::MeasureRegistrationGate(r);
    Check(solo.measurement->global.evaluated == full.global.evaluated, "without the band, the decision reads Omega's whole global check");
    // Threads do not matter.
    auto one = r; one.thread_count = 1;
    auto four = r; four.thread_count = 4;
    const auto omega1 = rg::MeasureGateOmega(one, rg::MakeRegistrationGateLayout(one), true), omega4 = rg::MeasureGateOmega(four, rg::MakeRegistrationGateLayout(four), true);
    Check(PointsText(omega1) == PointsText(omega4) && omega1.global.evaluated == omega4.global.evaluated, "the least global check does not depend on the thread count");
}

void Determinism() {
    const auto v = MakeViews(Content(67), Constant(0.4, 0.2), 2);
    auto r = Request(v); r.prediction = Predict({0.4, 0.2});
    r.wide_search = true;
    r.thread_count = 1;
    const auto one = Fingerprint(rg::MeasureRegistrationGate(r));
    r.thread_count = 4;
    const auto four = Fingerprint(rg::MeasureRegistrationGate(r));
    const auto again = Fingerprint(rg::MeasureRegistrationGate(r));
    Check(one == four && four == again, "threads 1 and 4, and a second run, give the same bytes");
}

// The pixels of the gate's points and of the points a row's correction may use (11.5): with the hole of layer 2 sized from
// the patches this part reports, no correction point reads a pixel that a gate point reads.
void PatchesShareNoPixel() {
    const auto v = MakeViews(Content(77), Constant(1.5, 0), 3);   // over the line: points are remeasured, the patches widen
    const auto r = Request(v);
    const auto result = rg::MeasureRegistrationGate(r);
    const auto& m = *result.measurement;
    const auto patches = rg::RegistrationGatePatches(m);
    std::size_t remeasured = 0;
    for (const auto& p : m.points) remeasured += !p.remeasurements.empty();
    Check(patches.size() > 10 && remeasured > 5, "Omega's points and their remeasurements are present");
    // The widest patch a point can read: every variant (a correction point measured by the same measurer).
    gate::SeamPoint widest; widest.remeasurements = {{gate::Variant::shifted}, {gate::Variant::enlarged}};
    const auto w = rg::GatePatchOf(widest, kDesign);
    const std::int64_t half_x = -w.x0, half_y = -w.y0;   // the point is at the origin
    Check(w.x1 == half_x+1 && w.y1 == half_y+1, "a patch is symmetric about its point");
    const std::int64_t omega = 32, iota = 32;   // iota: the support of the interpolation of q between rows (a test value)
    const std::int64_t g_x = omega+2*half_x+1, g_y = 2*half_y+1+iota;
    bool disjoint = true; std::size_t pairs = 0;
    for (std::size_t i = 0; i < m.points.size(); ++i) {
        const auto& gp = m.points[i];
        if (gp.status == gate::PointStatus::outside_coverage) continue;
        const auto g = rg::GatePatchOf(gp, kDesign);
        // The rows whose q the gate point uses: within iota of its row.
        for (std::int64_t ye = gp.y-iota; ye <= gp.y+iota; ye += 16)
            for (std::int64_t dy = -320; dy <= 320; dy += 16) for (std::int64_t dx = -256; dx <= 256; dx += 8) {
                if (std::abs(dx) < g_x && std::abs(dy) < g_y) continue;   // inside the hole of row ye
                if (ye+dy < 0) continue;
                gate::SeamPoint c = widest; c.x = static_cast<std::uint32_t>(kSeam+dx); c.y = static_cast<std::uint32_t>(ye+dy);
                disjoint = disjoint && rg::PatchesShareNoPixel(g, rg::GatePatchOf(c, kDesign));
                ++pairs;
            }
    }
    Check(disjoint && pairs > 1000, "with the hole sized from the reported patches, no pixel is shared");
    // The design's hole without the remeasured patch and the smoothing is not enough for a remeasured point.
    const std::int64_t design_g_x = kDesign.patch_px+omega;
    gate::SeamPoint gp = widest; gp.x = kSeam+static_cast<std::uint32_t>(omega); gp.y = 300;
    gate::SeamPoint cp = widest; cp.x = static_cast<std::uint32_t>(kSeam+omega+design_g_x-omega); cp.y = 300;
    Check(!rg::PatchesShareNoPixel(rg::GatePatchOf(gp, kDesign), rg::GatePatchOf(cp, kDesign)),
        "the function finds the shared pixels of the hole g_x = P + omega for remeasured points");
}

int Timing(unsigned threads) {
    // A pair of the still size, the band and Omega of the tests scaled to it (no product value).
    const unsigned w = 7360, h = 4912; const double tx = 3760;
    std::vector<scene::Stroke> s;
    scene::Drawing(s, {tx-760, 0, w+640.0, double(h)}, 91, 4000);
    scene::Text(s, {tx-760, 0.3*h, w+640.0, 0.53*h}, 92);
    scene::HorizontalLines(s, {tx-760, 0.73*h, w+640.0, 0.88*h}, 93);
    const auto t0 = std::chrono::steady_clock::now();
    Views v;
    v.a = scene::Render(w, h, 0, 0, 0, s, [](int i, int j, double& x, double& y) { x = i; y = j; }, 11, 1.0);
    v.b = scene::Render(w, h, tx, 0, 1, s, [&](int i, int j, double& x, double& y) { x = i+tx-0.3; y = j; }, 23, 1.0);
    rg::RegistrationGateRequest r;
    r.camera_a = &v.a; r.camera_b = &v.b;
    r.mapped = a0::m2::experimental::PairMappedFixedSeamInput(w, h, w, h, {{1, 0, tx, 0, 1, 0, 0, 0, 1}, a0::m2::StitchLayout::camera_a_left_camera_b_right, {}});
    r.layout.seam_column = static_cast<std::uint32_t>((tx+w)/2); r.layout.mixing_width_px = 8; r.layout.omega_margin_px = 28;
    r.layout.band_half_width_px = 512;
    r.blend = a0::m2::experimental::kSeamBlendDesign;
    r.gate_design = gate::kSeamGateDesignCalibrated; r.gate_limits = kLimits; r.design = TestDesign();
    r.baseline = {1, 0}; r.thread_count = threads; r.prediction = Predict({0.3, 0});
    const auto t1 = std::chrono::steady_clock::now();
    for (const bool band : {false, true}) {
        r.measure_whole_band_for_anchors = band;
        const auto t2 = std::chrono::steady_clock::now();
        const auto result = rg::MeasureRegistrationGate(r);
        const auto t3 = std::chrono::steady_clock::now();
        std::cout << "timing threads=" << threads << " band=" << band << " omega points=" << result.measurement->points.size()
                  << " " << Summary(result) << " scene=" << std::chrono::duration<double>(t1-t0).count()
                  << " s gate=" << std::chrono::duration<double>(t3-t2).count() << " s\n";
    }
    return 0;
}
} // namespace

// The overlap scan on threads equals seam_gate::OverlapBand for every thread count: rows, and an exception's type and message.
bool SameBand(const std::vector<gate::BandRow>& a, const std::vector<gate::BandRow>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t y = 0; y < a.size(); ++y) if (a[y].begin != b[y].begin || a[y].end != b[y].end) return false;
    return true;
}
std::string ThrownBy(const std::function<void()>& call) {
    try { call(); } catch (const std::invalid_argument& e) { return std::string("invalid_argument:")+e.what(); }
    catch (const std::exception& e) { return std::string("exception:")+e.what(); }
    return "none";
}
void OverlapOnThreads() {
    const std::vector<unsigned> counts{1, 2, 3, Threads(), 17};
    const std::vector<std::uint32_t> insets{0, 5, gate::RequiredBandInsetPx(kDesign)};
    const auto compare = [&](const gate::SeamGateGeometry& g, std::uint32_t aw, std::uint32_t ah, std::uint32_t bw, std::uint32_t bh, const std::string& name) {
        for (const auto inset : insets) {
            const auto expected = gate::OverlapBand(g, aw, ah, bw, bh, inset);
            std::size_t filled = 0;
            for (const auto& row : expected) filled += row.begin < row.end;
            Check(filled > 0, name+": the scene has an overlap (inset "+std::to_string(inset)+")");
            for (const auto t : counts)
                Check(SameBand(rg::OverlapBandOnThreads(g, aw, ah, bw, bh, inset, t), expected),
                    name+": the overlap on "+std::to_string(t)+" threads equals OverlapBand (inset "+std::to_string(inset)+")");
        }
    };
    // The test pair: B translated by kTx.
    {
        const auto m = Mapped();
        gate::SeamGateGeometry g; g.width = m.width; g.height = m.height; g.camera_a = m.camera_a; g.camera_b = m.camera_b;
        compare(g, kW, kH, kW, kH, "translated pair");
    }
    // A rotated, scaled A and a B whose edge waves with the row, with rows that neither camera maps (empty rows inside the band).
    gate::SeamGateGeometry g; g.width = 900; g.height = 700;
    g.camera_a = [](Point o, Point& raw) {
        const double c = std::cos(0.03), s = std::sin(0.03);
        raw = {1.01*(c*o.x-s*o.y)+4, 1.01*(s*o.x+c*o.y)-7}; return true;
    };
    g.camera_b = [](Point o, Point& raw) {
        if (static_cast<int>(o.y) % 300 == 13) return false;   // farther apart than twice the largest inset, so rows remain
        raw = {o.x-500+12*std::sin(o.y/40), o.y+3*std::cos(o.x/90)}; return true;
    };
    compare(g, 880, 690, 820, 700, "rotated and waving pair");
    // A mapping that fails (a position that is not finite) on some rows: the exception of the lowest such row, as with one thread.
    auto bad = g;
    bad.camera_b = [](Point o, Point& raw) {
        const int y = static_cast<int>(o.y);
        if (y == 523 || (y >= 211 && y < 215)) { raw = {std::numeric_limits<double>::quiet_NaN(), o.y}; return true; }
        raw = {o.x-500, o.y}; return true;
    };
    const auto expected = ThrownBy([&] { (void)gate::OverlapBand(bad, 880, 690, 820, 700, 0); });
    Check(expected != "none", "the failing mapping makes OverlapBand throw");
    for (const auto t : counts)
        Check(ThrownBy([&] { (void)rg::OverlapBandOnThreads(bad, 880, 690, 820, 700, 0, t); }) == expected,
            "the overlap on "+std::to_string(t)+" threads throws OverlapBand's exception");
    auto missing = g; missing.camera_a = {};
    Check(ThrownBy([&] { (void)rg::OverlapBandOnThreads(missing, 880, 690, 820, 700, 0, 2); }) ==
        ThrownBy([&] { (void)gate::OverlapBand(missing, 880, 690, 820, 700, 0); }), "a missing mapping is refused as OverlapBand refuses it");
    Rejects([&] { (void)rg::OverlapBandOnThreads(g, 880, 690, 820, 700, 0, 0); }, "zero threads for the overlap");
    Rejects([&] { (void)rg::OverlapBandOnThreads(g, 880, 690, 820, 700, 0, 257); }, "257 threads for the overlap");
    // The layout of the gate does not depend on the request's thread count.
    const auto v = MakeViews({}, Constant(0, 0), 0);
    auto one = Request(v); one.thread_count = 1;
    auto many = one; many.thread_count = Threads()+3;
    const auto a = rg::MakeRegistrationGateLayout(one), b = rg::MakeRegistrationGateLayout(many);
    Check(SameBand(a.omega.band, b.omega.band) && SameBand(a.band.band, b.band.band) && SameBand(a.omega.transition, b.omega.transition) &&
        SameBand(a.band.transition, b.band.transition) && Text(a.outside_measurable) == Text(b.outside_measurable),
        "the gate's layout is the same on one thread and on several");
}

// ---- The split of Omega's rows (11.4, 11.5, 11.30) ----------------------------------------------------------------------
// Omega's lattice rows as layer 2's gate rows (the first band row + 16, every 32 px), one verify row in six. The decision reads the
// verify rows only; the correct rows are measured apart for reference and never change the decision.
void SplitRows() {
    const Point delta{0.3, -0.1};
    const auto v = MakeViews(Content(7), Constant(delta.x, delta.y), 2);
    auto r = Request(v); r.prediction = Predict(delta);
    const std::uint32_t s = kDesign.point_spacing_px;
    auto layout = rg::MakeRegistrationGateLayout(r);
    std::uint32_t first = 0, last = 0;
    for (std::uint32_t y = 0; y < layout.omega.height; ++y) if (layout.omega.band[y].begin < layout.omega.band[y].end) { if (!last) first = y; last = y+1; }
    const std::uint32_t first_row = first+s/2, rows = (last-first)/s;
    // The rows past the last whole lattice row are not layer 2's (AlignGateOmegaToRows leaves them out); here by hand.
    for (std::uint32_t y = first+rows*s; y < layout.omega.height; ++y) { layout.omega.band[y] = {}; layout.omega.transition[y] = {}; }
    const auto whole = layout;
    const auto moved = rg::SplitGateOmega(layout, first_row, s, rows, 6, 1, s);
    bool parts = layout.corrected_omega.has_value();
    std::uint32_t corrected_rows = 0;
    for (std::uint32_t y = 0; parts && y < layout.omega.height; ++y) {
        const auto& w = whole.omega.band[y];
        if (w.begin >= w.end) { parts = layout.omega.band[y].begin >= layout.omega.band[y].end && layout.corrected_omega->band[y].begin >= layout.corrected_omega->band[y].end; continue; }
        const bool verify = ((y-first)/s)%6 == 1;
        const auto& kept = verify ? layout.omega.band[y] : layout.corrected_omega->band[y];
        const auto& other = verify ? layout.corrected_omega->band[y] : layout.omega.band[y];
        parts = kept.begin == w.begin && kept.end == w.end && other.begin >= other.end;
        corrected_rows += !verify;
    }
    Check(parts && moved == corrected_rows && !layout.corrected.empty() && layout.corrected.front().first == first,
        "the split: the verify rows stay in Omega, the correct rows move to their own band, whole lattice rows each");
    Rejects([&] { auto again = layout; (void)rg::SplitGateOmega(again, first_row, s, rows, 6, 1, s); }, "a layout split twice");
    Rejects([&] { auto bad = whole; (void)rg::SplitGateOmega(bad, first_row, s, rows, 6, 6, s); }, "a phase at the period");
    Rejects([&] { auto bad = whole; (void)rg::SplitGateOmega(bad, first_row, s/2, rows, 6, 1, s); }, "rows off the gate's lattice");

    rg::RegistrationGateMeasurements m;
    m.omega = gate::MeasureSeam(v.a, v.b, layout.omega, r.gate_design, r.gate_limits, r.prediction, r.thread_count);
    m.band = gate::MeasureSeam(v.a, v.b, layout.band, r.gate_design, r.gate_limits, r.prediction, r.thread_count);
    m.corrected = gate::MeasureSeam(v.a, v.b, *layout.corrected_omega, r.gate_design, r.gate_limits, r.prediction, r.thread_count);
    bool on_verify = !m.omega.points.empty() && !m.corrected->points.empty();
    for (const auto& p : m.omega.points) on_verify = on_verify && ((p.y-first_row)/s)%6 == 1 && (p.y-first_row)%s == 0;
    for (const auto& p : m.corrected->points) on_verify = on_verify && ((p.y-first_row)/s)%6 != 1 && (p.y-first_row)%s == 0;
    Check(on_verify, "the split: the measurer keeps layer 2's rows in both parts (verify rows in Omega, correct rows apart)");
    auto copy = m;
    const auto result = rg::DecideRegistrationGate(r, layout, std::move(copy));
    Report("split, aligned", result);
    Check(result.state == rg::GateState::pass && result.corrected && result.corrected_measurement && result.rows.rows_corrected > 0 &&
        Text(result.rows.corrected) == Text(layout.corrected), "the split: the verify rows decide, the correct rows are kept as a reference");
    Check(result.corrected->after.maxima.two_directional_px && std::abs(*result.corrected->after.maxima.two_directional_px-std::hypot(0.3, 0.1)) <= 0.06 &&
        result.corrected->rows_measured > 0, "the split: the correct rows' reference measures the known shift");
    // The correct rows never decide: their values far over the line leave every part of the decision as it was.
    auto wild = m;
    for (auto& p : wild.corrected->points) if (p.Counted()) { p.displacement = {5, 0}; p.visible_px = 5; }
    const auto decided = rg::DecideRegistrationGate(r, layout, std::move(wild));
    Check(Fingerprint(decided) == Fingerprint(result) && decided.corrected->after.maxima.two_directional_px &&
        *decided.corrected->after.maxima.two_directional_px == 5, "the split: values of the correct rows over the line change the reference only");
    // The correct rows' measurement goes with a split layout, and only with one.
    auto missing = m; missing.corrected.reset();
    Rejects([&] { (void)rg::DecideRegistrationGate(r, layout, std::move(missing)); }, "a split layout without the correct rows' measurement");
    auto extra = m;
    Rejects([&] { (void)rg::DecideRegistrationGate(r, whole, std::move(extra)); }, "the correct rows' measurement on a layout that is not split");
}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--timing") return Timing(argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : Threads());
        const std::string only = argc > 2 && std::string(argv[1]) == "--only" ? argv[2] : "";
        const auto run = [&](const char* name, void (*test)()) { if (only.empty() || only == name) test(); };
        run("Validation", Validation);
        run("Layout", Layout);
        run("OverlapOnThreads", OverlapOnThreads);
        run("ConstructedDecisions", ConstructedDecisions);
        run("AlignedPasses", AlignedPasses);
        run("OmegaShiftFails", OmegaShiftFails);
        run("OutsideOmegaOnly", OutsideOmegaOnly);
        run("HorizontalLinesOnly", HorizontalLinesOnly);
        run("AnchorsFromTheBand", AnchorsFromTheBand);
        run("WideSearchDiagnostic", WideSearchDiagnostic);
        run("TransitionOutside", TransitionOutside);
        run("LatticeOneOff", LatticeOneOff);
        run("BeforeAndAfter", BeforeAndAfter);
        run("PaperDecisions", PaperDecisions);
        run("BandMeasureWidth", BandMeasureWidth);
        run("OmegaGlobalLeast", OmegaGlobalLeast);
        run("Determinism", Determinism);
        run("PatchesShareNoPixel", PatchesShareNoPixel);
        run("SplitRows", SplitRows);
    } catch (const std::exception& e) {
        std::cerr << "FAIL exception: " << e.what() << '\n';
        return 1;
    }
    std::cout << checks-failures << "/" << checks << " checks passed\n";
    return failures ? 1 : 0;
}
