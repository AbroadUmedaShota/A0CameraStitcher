#include "a0/m2/stitch_evaluation.hpp"
#include "a0/m2/stitch_metrics.hpp"
#include "a0/common/protocol_json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace evaluation = a0::m2::evaluation;
namespace json = a0::common::protocol_json;
using Value = json::JsonValue;
using Kind = json::JsonKind;
using namespace evaluation;
int checks = 0, failures = 0;
void Check(bool condition, const std::string& label) { ++checks; if (!condition) { ++failures; std::cerr << "FAIL " << label << '\n'; } }
void Near(double actual, double expected, const char* label, double tolerance = 1e-10) {
    Check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, label);
}
template<class F> void Rejected(F call, const char* label) {
    bool rejected = false; try { call(); } catch (const std::exception&) { rejected = true; } Check(rejected, label);
}
struct Failure { [[noreturn]] static void Fail(std::string_view, std::string_view) { throw std::runtime_error("test JSON parse failed"); } };
Value Parse(const std::string& text) { return json::BasicJsonParser<Failure>(text).Parse(); }
std::string Dump(const Value& value) {
    if (value.kind == Kind::null_value) return "null";
    if (value.kind == Kind::number) return value.string;
    if (value.kind == Kind::boolean) return value.boolean ? "true" : "false";
    if (value.kind == Kind::string) return "\"" + json::JsonEscape(value.string) + "\"";
    std::string result = value.kind == Kind::object ? "{" : "["; bool first = true;
    if (value.kind == Kind::object) for (const auto& [key, child] : value.object) {
        if (!first) result += ','; first = false; result += "\"" + json::JsonEscape(key) + "\":" + Dump(child);
    } else for (const auto& child : value.array) { if (!first) result += ','; first = false; result += Dump(child); }
    return result + (value.kind == Kind::object ? "}" : "]");
}
Value Number(const char* lex) { Value result; result.kind = Kind::number; result.string = lex; return result; }
std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary); if (!input) throw std::runtime_error("missing evaluator test fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
Point Expected(Point doc, const Raster& raster) {
    return {(doc.x - static_cast<double>(raster.left_um) / 1000.0) * raster.dpi / 25.4 - .5,
        (doc.y - static_cast<double>(raster.top_um) / 1000.0) * raster.dpi / 25.4 - .5};
}
Plan AnalyticPlan() {
    Plan p; p.raster = {0, 0, 25600, 25600, 254, 256, 256};
    p.detector = {.5, .3, 1.5, .25, 5, 100};
    p.minimum_fiducials = 9; p.minimum_dpi_pairs = 4; p.minimum_seam_pairs = 3; p.decimal_places = 4;
    for (unsigned row = 0; row < 3; ++row) for (unsigned col = 0; col < 3; ++col) {
        const Point document{(48.5 + 80 * col) / 10.0, (48.5 + 80 * row) / 10.0};
        p.fiducials.push_back({"p" + std::to_string(row) + std::to_string(col), document, Expected(document, p.raster), 20});
    }
    p.dpi_pairs = {{"p00", "p01"}, {"p01", "p02"}, {"p00", "p10"}, {"p10", "p20"}};
    p.seam_pairs = {{{10, 10, 8, 8}, {150, 10, 8, 8}}, {{10, 24, 8, 8}, {150, 24, 8, 8}}, {{10, 220, 8, 8}, {150, 220, 8, 8}}};
    return p;
}
Thresholds Limits() {
    Thresholds limits;
    for (std::size_t i = 0; i < limits.size(); ++i)
        limits[i] = {std::string(kMetricIds[i]), i == 3 || i == 4 ? "AtLeast" : "AtMost", i == 3 || i == 4 ? 1.0 : i == 7 ? .001 : .01};
    return limits;
}
struct Pixels {
    std::vector<std::uint8_t> bgr = std::vector<std::uint8_t>(256 * 256 * 3, 200);
    std::vector<std::uint8_t> mask = std::vector<std::uint8_t>(256 * 256, 1);
    ImageView View() const { return {256, 256, bgr}; }
    void Pixel(int x, int y, std::array<std::uint8_t, 3> value) {
        if (x < 0 || y < 0 || x >= 256 || y >= 256) return;
        const auto at = (static_cast<std::size_t>(y) * 256 + x) * 3;
        for (unsigned c = 0; c < 3; ++c) bgr[at + c] = value[c];
    }
    void Disk(int x, int y, int radius = 3, std::uint8_t code = 0) {
        // Analytic integer-grid disk: the observations do not call any source
        // renderer/generator, calibration or measurement implementation.
        for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx)
            if (dx * dx + dy * dy <= radius * radius) Pixel(x + dx, y + dy, {code, code, code});
    }
    void Patch(Rect rectangle, std::array<std::uint8_t, 3> value) {
        for (unsigned y = 0; y < rectangle.height; ++y) for (unsigned x = 0; x < rectangle.width; ++x)
            Pixel(rectangle.left + static_cast<int>(x), rectangle.top + static_cast<int>(y), value);
    }
};
Pixels Chart(int dx = 0, int dy = 0, bool skip_middle = false) {
    Pixels image;
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) {
        if (skip_middle && row == 1 && col == 1) continue; image.Disk(48 + col * 80 + dx, 48 + row * 80 + dy);
    }
    return image;
}
bool Diagnostic(const Evaluation& result, std::string_view code) {
    return std::find(result.diagnostic_codes.begin(), result.diagnostic_codes.end(), code) != result.diagnostic_codes.end();
}
void MetricNear(const Evaluation& result, unsigned index, double expected, const char* label, double tolerance = 1e-10) {
    const auto& metric = result.metrics[index];
    Check(metric.outcome == "Success" && metric.raw_value && metric.failure_code.empty(), std::string(label) + " outcome");
    if (metric.raw_value) Near(*metric.raw_value, expected, label, tolerance);
}
SourceHashes Hashes() { const std::string hash(64, 'a'); return {hash, hash, hash, hash, hash, hash}; }
void ValidateMetricPairs(const std::string& report) {
    const auto root = Parse(report); const auto& metrics = root.object.at("metrics").array;
    Check(metrics.size() == 8, "report contains eight metric pairs");
    for (const auto& metric : metrics) {
        const auto definition = Dump(metric.object.at("definition")), result = Dump(metric.object.at("result"));
        Check(a0::m2::ValidateStitchMetricDefinitionJson(definition).valid, "existing metric definition validator accepts independent definition");
        Check(a0::m2::ValidateStitchMetricResultJson(definition, result).valid, "existing runtime validator accepts emitted exact decimal rounding");
    }
}
void PureMeasurements() {
    auto plan = AnalyticPlan(); const auto limits = Limits(); const auto chart = Chart();
    const auto input_bytes = chart.bgr, input_mask = chart.mask;
    const auto baseline = Evaluate(chart.View(), chart.mask, plan, limits);
    Check(baseline.threshold_assessment == "thresholds-met" && baseline.diagnostic_codes.empty(), "pristine analytic chart meets explicit test limits");
    for (unsigned i = 0; i < 8; ++i) MetricNear(baseline, i, i == 3 || i == 4 ? 1 : 0, "pristine analytic metric");
    Check(chart.bgr == input_bytes && chart.mask == input_mask, "evaluation never changes image/mask bytes");
    for (const auto& observation : baseline.fiducials) {
        Check(observation.status == "found" && observation.observed_pixel.has_value(), "analytic disk independently found");
    }
    for (const auto& pair : baseline.dpi_pairs) if (pair.measured_dpi) Near(*pair.measured_dpi, 254, "analytic known millimeter distance yields 254 DPI");
    const auto report = SerializeReport(baseline, plan, limits, Hashes());
    Check(report == SerializeReport(Evaluate(chart.View(), chart.mask, plan, limits), plan, limits, Hashes()), "identical inputs yield identical report bytes");
    const auto parsed_report = Parse(report);
    Check(parsed_report.object.at("quality").string == "not-evaluated", "numerical limits never become quality approval");
    Check(parsed_report.object.at("measurementModel").object.at("coverageTrialValidity").string == "all-declared-trials", "report states coverage trial eligibility");
    ValidateMetricPairs(report);

    const auto shifted = Chart(3, 4); const auto shift = Evaluate(shifted.View(), shifted.mask, plan, limits);
    for (unsigned i = 0; i < 3; ++i) MetricNear(shift, i, 5, "analytic 3-4 displacement remains visible");
    MetricNear(shift, 7, 0, "common displacement cannot change local DPI");
    Check(shift.threshold_assessment == "thresholds-not-met" && Diagnostic(shift, "fiducial-position-out-of-range")
        && std::count(shift.diagnostic_codes.begin(), shift.diagnostic_codes.end(), "fiducial-position-out-of-range") == 1, "position failures retain Success metrics and deduplicate diagnostic");
    auto inclusive = limits; for (unsigned i = 0; i < 3; ++i) inclusive[i].value = *shift.metrics[i].raw_value;
    Check(Evaluate(shifted.View(), shifted.mask, plan, inclusive).threshold_assessment == "thresholds-met", "inclusive threshold comparison uses raw exact boundary");
    inclusive[2].value = std::nextafter(*shift.metrics[2].raw_value, -std::numeric_limits<double>::infinity());
    Check(Evaluate(shifted.View(), shifted.mask, plan, inclusive).threshold_assessment == "thresholds-not-met", "one binary64 quantum below raw threshold fails even when rounded display matches");

    Pixels varied; unsigned ordinal = 0;
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) varied.Disk(48 + col * 80 + ordinal++, 48 + row * 80);
    const auto order = Evaluate(varied.View(), varied.mask, plan, limits);
    MetricNear(order, 0, 4, "SortedMiddle median has independent 0..8 oracle");
    MetricNear(order, 1, 7.6, "LinearR7 p95 has independent 0..8 oracle");
    MetricNear(order, 2, 8, "maximum has independent 0..8 oracle");

    Pixels scaled;
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) scaled.Disk(48 + col * 84, 48 + row * 80);
    const auto dpi = Evaluate(scaled.View(), scaled.mask, plan, limits);
    MetricNear(dpi, 7, .05, "84 pixels over known 8 mm exposes 5 percent local DPI error");
    Check(Diagnostic(dpi, "local-dpi-out-of-range"), "local DPI exceedance has distinct diagnostic");

    auto seams = Chart();
    for (const auto& pair : plan.seam_pairs) { seams.Patch(pair.first, {80, 80, 80}); seams.Patch(pair.second, {96, 112, 144}); }
    const auto seam = Evaluate(seams.View(), seams.mask, plan, limits);
    MetricNear(seam, 5, (.2126 * 64 + .7152 * 32 + .0722 * 16) / 255.0, "seam luma uses normalized encoded Rec709 weights");
    MetricNear(seam, 6, 64.0 / 255, "seam RGB uses maximum absolute channel mean step");
    Check(Diagnostic(seam, "seam-luminance-out-of-range") && Diagnostic(seam, "seam-color-out-of-range"), "seam/exposure observations have separate diagnostics");
    ValidateMetricPairs(SerializeReport(seam, plan, limits, Hashes()));

    const auto missing = Chart(0, 0, true); const auto absent = Evaluate(missing.View(), missing.mask, plan, limits);
    MetricNear(absent, 3, 8.0 / 9, "missing fiducial remains in denominator");
    Check(absent.fiducials[4].status == "missing" && absent.metrics[0].outcome == "NoResult" && !absent.metrics[0].raw_value
        && absent.metrics[0].sample_count == 0 && absent.metrics[0].failure_code == "insufficient-valid-samples", "insufficient nonzero fiducials give explicit null NoResult");
    Check(absent.threshold_assessment == "incomplete-measurements" && Diagnostic(absent, "insufficient-valid-measurements"), "incomplete groups override numerical limit assessment");
    ValidateMetricPairs(SerializeReport(absent, plan, limits, Hashes()));

    auto invalid = Chart(); invalid.mask[48 * 256 + 40] = 0; // inside window but outside disk
    const auto invalid_fid = Evaluate(invalid.View(), invalid.mask, plan, limits);
    Check(invalid_fid.fiducials[0].status == "invalid-mask", "whole window validity required beyond component/center");
    MetricNear(invalid_fid, 3, 8.0 / 9, "invalid-mask fiducial remains in denominator");
    MetricNear(invalid_fid, 4, 65535.0 / 65536, "pixel coverage includes entire explicit output raster");
    Check(invalid_fid.metrics[7].outcome == "NoResult" && invalid_fid.dpi_pairs.size() == 4
        && !invalid_fid.dpi_pairs[0].measured_dpi && !invalid_fid.dpi_pairs[2].measured_dpi, "invalid fiducial cannot leak into DPI pair observations");
    auto elsewhere = Chart(); elsewhere.mask[0] = 0;
    const auto outside = Evaluate(elsewhere.View(), elsewhere.mask, plan, limits);
    MetricNear(outside, 3, 1, "invalid pixel outside trials does not remove detected fiducials");
    MetricNear(outside, 4, 65535.0 / 65536, "unselected pixel still reduces whole-raster coverage");

    auto patch_invalid = Chart(); patch_invalid.mask[10 * 256 + 10] = 0;
    const auto patch_result = Evaluate(patch_invalid.View(), patch_invalid.mask, plan, limits);
    Check(patch_result.metrics[5].outcome == "NoResult" && patch_result.metrics[6].outcome == "NoResult", "any invalid patch pixel excludes whole paired seam trial");
    auto patch_plan = plan; patch_plan.seam_pairs[0].first.left = -1;
    const auto patch_clip = Evaluate(chart.View(), chart.mask, patch_plan, limits);
    Check(patch_clip.metrics[5].outcome == "NoResult" && patch_clip.metrics[5].failure_code == "insufficient-valid-samples", "partial seam patch remains missing at group minimum");

    auto ambiguous = Chart(0, 0, true); ambiguous.Disk(125, 128, 1); ambiguous.Disk(131, 128, 1);
    const auto ambiguous_result = Evaluate(ambiguous.View(), ambiguous.mask, plan, limits);
    Check(ambiguous_result.fiducials[4].status == "ambiguous" && !ambiguous_result.fiducials[4].observed_pixel, "distance gap rejects equally near components");
    MetricNear(ambiguous_result, 3, 8.0 / 9, "ambiguous trial retained in coverage");
    auto partial_plan = plan; const Point corner{.05, .05};
    partial_plan.fiducials.push_back({"corner", corner, Expected(corner, plan.raster), 20});
    const auto partial = Evaluate(chart.View(), chart.mask, partial_plan, limits);
    Check(partial.fiducials.back().status == "partial-window", "clipped search window has explicit missing status");
    MetricNear(partial, 3, .9, "partial trial retained in denominator when other samples suffice");

    auto no_mask = Chart(); std::fill(no_mask.mask.begin(), no_mask.mask.end(), std::uint8_t{0});
    const auto zeros = Evaluate(no_mask.View(), no_mask.mask, plan, limits);
    MetricNear(zeros, 3, 0, "zero detected coverage is valid observation"); MetricNear(zeros, 4, 0, "zero valid pixel coverage is valid observation");
    Check(zeros.metrics[0].failure_code == "zero-valid-results" && zeros.metrics[7].failure_code == "zero-valid-results", "zero measurement groups have explicit failure codes");
    ValidateMetricPairs(SerializeReport(zeros, plan, limits, Hashes()));
    Pixels uniform; std::fill(uniform.bgr.begin(), uniform.bgr.end(), std::uint8_t{0});
    const auto dark = Evaluate(uniform.View(), uniform.mask, plan, limits);
    MetricNear(dark, 3, 0, "uniform dark crop cannot masquerade as centered fiducial");
    auto contrast_plan = plan; contrast_plan.detector.minimum_contrast = .1;
    Pixels contrast; std::fill(contrast.bgr.begin(), contrast.bgr.end(), std::uint8_t{150});
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) contrast.Disk(48 + col * 80, 48 + row * 80, 3, 120);
    MetricNear(Evaluate(contrast.View(), contrast.mask, contrast_plan, limits), 3, 1, "contrast is local background-minus-component rather than threshold-minus-luma");
    for (std::size_t p = 0; p < contrast.bgr.size(); ++p) if (contrast.bgr[p] == 150) contrast.bgr[p] = 130;
    MetricNear(Evaluate(contrast.View(), contrast.mask, contrast_plan, limits), 3, 0, "same component against low-contrast background is refused");

    auto forged = plan; forged.fiducials[0].expected_pixel.x += 1;
    Rejected([&] { ValidatePlan(forged); }, "hand-built Plan cannot inject aligned expected pixel");
    auto wrong_mask = chart.mask; wrong_mask[0] = 2;
    Rejected([&] { (void)Evaluate(chart.View(), wrong_mask, plan, limits); }, "mask values must be exactly binary");
    Rejected([&] { (void)Evaluate(chart.View(), std::span(chart.mask).first(chart.mask.size() - 1), plan, limits); }, "short mask cannot silently shrink coverage denominator");
    auto short_image = chart.View(); short_image.bgr = short_image.bgr.first(short_image.bgr.size() - 1);
    Rejected([&] { (void)Evaluate(short_image, chart.mask, plan, limits); }, "short pixel buffer refused before access");
    auto wide = plan; wide.fiducials[0].half_window_pixels = 513;
    Rejected([&] { ValidatePlan(wide); }, "window resource bound enforced");
    auto huge = plan; huge.raster.right_um = std::numeric_limits<std::int64_t>::max();
    Rejected([&] { ValidatePlan(huge); }, "raster arithmetic overflow cannot occur before rejection");
}

void RoundingVectors() {
    auto plan = AnalyticPlan(); auto limits = Limits();
    for (unsigned i = 0; i < 3; ++i) limits[i].value = 1e12;
    const auto chart = Chart();
    // These deliberately injected values test the report's numeric protocol,
    // not a claim that the analytic image measured these errors. Public v1
    // validators independently check raw decimal lexeme versus rounded value.
    const std::array<std::pair<double, const char*>, 6> vectors{{{1.125, "1.12"}, {1.375, "1.38"}, {2.675, "2.67"}, {1.005, "1"}, {0.125, "0.12"}, {0.375, "0.38"}}};
    for (const auto& [raw, expected] : vectors) {
        plan.decimal_places = 2; auto result = Evaluate(chart.View(), chart.mask, plan, limits); result.metrics[0].raw_value = raw;
        const auto report = SerializeReport(result, plan, limits, Hashes()); const auto root = Parse(report);
        Check(root.object.at("metrics").array[0].object.at("result").object.at("value").string == expected, "exact decimal half-even vector");
        ValidateMetricPairs(report);
    }
    for (unsigned places = 0; places <= 9; ++places) {
        plan.decimal_places = places; auto result = Evaluate(chart.View(), chart.mask, plan, limits); result.metrics[0].raw_value = 1.23456789;
        ValidateMetricPairs(SerializeReport(result, plan, limits, Hashes()));
    }
    plan.decimal_places = 0; auto boundary = Evaluate(chart.View(), chart.mask, plan, limits); boundary.metrics[0].raw_value = 549755813888.0;
    ValidateMetricPairs(SerializeReport(boundary, plan, limits, Hashes()));
    boundary.metrics[0].raw_value = 549755813889.0;
    Rejected([&] { (void)SerializeReport(boundary, plan, limits, Hashes()); }, "raw decimal scaled 2^39 plus one refused");
    plan.decimal_places = 9; auto precise = Evaluate(chart.View(), chart.mask, plan, limits); precise.metrics[0].raw_value = std::nextafter(549.755813888, std::numeric_limits<double>::infinity());
    Rejected([&] { (void)SerializeReport(precise, plan, limits, Hashes()); }, "decimalPlaces9 exact scaled overflow refused");
}

void JsonContracts(const std::filesystem::path& fixtures) {
    const auto truth_text = ReadText(fixtures / "ground-truth.json"), plan_text = ReadText(fixtures / "plan.json"), threshold_text = ReadText(fixtures / "thresholds.json");
    const auto parsed = ParsePlan(truth_text, plan_text); const auto thresholds = ParseThresholds(threshold_text);
    Check(parsed.fiducials.size() == 9, "shared analytic fixture selects nine declared references");
    for (std::size_t i = 0; i < parsed.fiducials.size(); ++i) {
        Near(parsed.fiducials[i].expected_pixel.x, 48.0 + 80.0 * static_cast<double>(i % 3), "DOC mm independently maps to known x pixel");
        Near(parsed.fiducials[i].expected_pixel.y, 48.0 + 80.0 * static_cast<double>(i / 3), "DOC mm independently maps to known y pixel");
    }
    auto truth = Parse(truth_text), plan = Parse(plan_text), checks_json = Parse(threshold_text);
    auto wrong_camera = truth;
    for (auto& reference : wrong_camera.object.at("reference_points").array) reference.object.at("image_px") = Number("999");
    const auto independent = ParsePlan(Dump(wrong_camera), plan_text);
    Check(independent.fiducials[0].expected_pixel.x == parsed.fiducials[0].expected_pixel.x, "raw camera annotation cannot become expected stitched position");
    auto unknown = plan; unknown.object["unexpected"] = Number("0");
    Rejected([&] { (void)ParsePlan(truth_text, Dump(unknown)); }, "unknown plan field refused");
    auto missing = plan; missing.object.erase("detector");
    Rejected([&] { (void)ParsePlan(truth_text, Dump(missing)); }, "missing detector block has no defaults");
    auto integer = plan; integer.object.at("raster").object.at("dpi") = Number("254.0");
    Rejected([&] { (void)ParsePlan(truth_text, Dump(integer)); }, "integer decimal lexeme refused");
    integer.object.at("raster").object.at("dpi") = Number("254e0");
    Rejected([&] { (void)ParsePlan(truth_text, Dump(integer)); }, "integer exponent lexeme refused");
    integer = plan; integer.object.at("rounding").object.at("decimalPlaces") = Number("10");
    Rejected([&] { (void)ParsePlan(truth_text, Dump(integer)); }, "rounding outside public v1 contract refused");
    const auto duplicated = std::string("{\"schemaVersion\":\"a0.stitch-evaluation-plan.v1\",") + plan_text.substr(plan_text.find('{') + 1);
    Rejected([&] { (void)ParsePlan(truth_text, duplicated); }, "duplicate field refused");
    auto duplicated_reference = truth; duplicated_reference.object.at("reference_points").array.push_back(duplicated_reference.object.at("reference_points").array.front());
    Rejected([&] { (void)ParsePlan(Dump(duplicated_reference), plan_text); }, "duplicate ground truth reference identity refused");
    auto convention = truth; convention.object.at("conventions").object.at("cameraPixel").string = "legacy-half-pixel";
    Rejected([&] { (void)ParsePlan(Dump(convention), plan_text); }, "wrong camera/document convention refused");
    auto absent_id = plan; absent_id.object.at("fiducials").array[0].object.at("id").string = "unknown-reference";
    Rejected([&] { (void)ParsePlan(truth_text, Dump(absent_id)); }, "selected absent reference refused");
    auto negative_doc = truth; negative_doc.object.at("reference_points").array[0].object.at("document_mm").array[0] = Number("-1e-400");
    Rejected([&] { (void)ParsePlan(Dump(negative_doc), plan_text); }, "negative-underflow document coordinate refused");
    auto repeated = checks_json; repeated.object.at("checks").array[1] = repeated.object.at("checks").array[0];
    Rejected([&] { (void)ParseThresholds(Dump(repeated)); }, "duplicate metric threshold refused");
    auto direction = checks_json; direction.object.at("checks").array[0].object.at("comparison").string = "AtLeast";
    Rejected([&] { (void)ParseThresholds(Dump(direction)); }, "metric comparison direction cannot change");
    auto negative = checks_json; negative.object.at("checks").array[0].object.at("value") = Number("-1e-400");
    Rejected([&] { (void)ParseThresholds(Dump(negative)); }, "negative underflow threshold cannot masquerade as zero");
    auto nonfinite = checks_json; nonfinite.object.at("checks").array[0].object.at("value") = Number("1e309");
    Rejected([&] { (void)ParseThresholds(Dump(nonfinite)); }, "overflowed threshold refused");
    auto tiny = checks_json; tiny.object.at("checks").array[0].object.at("value") = Number("1e-400");
    Check(ParseThresholds(Dump(tiny))[0].value == 0, "finite underflow threshold rounds to zero");
    for (const auto& bytes : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2)}) {
        auto bad = truth; bad.object.at("output_files").array.push_back(Value{});
        Value text; text.kind = Kind::string; text.string = bytes; bad.object.at("output_files").array.back() = text;
        Rejected([&] { (void)ParsePlan(Dump(bad), plan_text); }, "public core rejects invalid UTF8 even in unused annotation");
        const auto invalid_thresholds = std::string("{\"bad\":\"") + bytes + "\"}";
        Rejected([&] { (void)ParseThresholds(invalid_thresholds); }, "public thresholds reject invalid UTF8");
    }
    Check(thresholds[3].comparison == "AtLeast" && thresholds[0].comparison == "AtMost", "threshold parser canonicalizes all metric IDs");
}
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) { std::cerr << "fixture directory required\n"; return 2; }
    try { PureMeasurements(); RoundingVectors(); JsonContracts(argv[1]); }
    catch (const std::exception& exception) { Check(false, std::string("unexpected exception: ") + exception.what()); }
    std::cout << "independent evaluation checks=" << checks << " failures=" << failures << '\n'; return failures ? 1 : 0;
}
