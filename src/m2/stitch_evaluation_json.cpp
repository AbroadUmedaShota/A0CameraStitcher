#include "a0/m2/stitch_evaluation.hpp"
#include "a0/common/protocol_json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace a0::m2::evaluation {
namespace {
namespace json = a0::common::protocol_json;
using Value = json::JsonValue;
using Kind = json::JsonKind;
[[noreturn]] void Reject() { throw std::invalid_argument("invalid independent evaluation JSON contract"); }
struct Failure { [[noreturn]] static void Fail(std::string_view, std::string_view) { Reject(); } };
void Utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first < 128) { ++i; continue; }
        const auto count = first >= 194 && first <= 223 ? 2U : first >= 224 && first <= 239 ? 3U : first >= 240 && first <= 244 ? 4U : 0U;
        if (!count || count > text.size() - i) Reject();
        for (unsigned n = 1; n < count; ++n) {
            const auto byte = static_cast<unsigned char>(text[i + n]);
            if (byte < 128 || byte > 191) Reject();
        }
        const auto second = static_cast<unsigned char>(text[i + 1]);
        if ((first == 224 && second < 160) || (first == 237 && second > 159)
            || (first == 240 && second < 144) || (first == 244 && second > 143)) Reject();
        i += count;
    }
}
Value Parse(std::string_view text) { Utf8(text); return json::BasicJsonParser<Failure>(text).Parse(); }
const Value& Typed(const Value& value, Kind kind) { if (value.kind != kind) Reject(); return value; }
const Value& At(const Value& object, std::string_view key) {
    Typed(object, Kind::object); const auto found = object.object.find(std::string(key));
    if (found == object.object.end()) Reject(); return found->second;
}
void Fields(const Value& object, std::initializer_list<std::string_view> names) {
    Typed(object, Kind::object); if (object.object.size() != names.size()) Reject();
    for (const auto name : names) (void)At(object, name);
}
const std::string& Text(const Value& value) { return Typed(value, Kind::string).string; }
void Literal(const Value& value, std::string_view expected) { if (Text(value) != expected) Reject(); }
std::uint64_t Unsigned(const Value& value, std::uint64_t maximum) {
    const auto& lex = Typed(value, Kind::number).string;
    if (lex.empty() || (lex.size() > 1 && lex[0] == '0')) Reject(); std::uint64_t result = 0;
    for (const auto c : lex) {
        if (c < '0' || c > '9') Reject(); const auto digit = static_cast<unsigned>(c - '0');
        if (result > maximum / 10 || (result == maximum / 10 && digit > maximum % 10)) Reject();
        result = result * 10 + digit;
    }
    return result;
}
std::uint32_t U32(const Value& value) { return static_cast<std::uint32_t>(Unsigned(value, std::numeric_limits<std::uint32_t>::max())); }
std::int32_t I32(const Value& value) {
    const auto& lex = Typed(value, Kind::number).string;
    if (!lex.empty() && lex[0] == '-') {
        Value magnitude; magnitude.kind = Kind::number; magnitude.string = lex.substr(1);
        const auto integer = Unsigned(magnitude, 2147483648ULL);
        if (!integer) Reject(); return static_cast<std::int32_t>(-static_cast<std::int64_t>(integer));
    }
    return static_cast<std::int32_t>(Unsigned(value, std::numeric_limits<std::int32_t>::max()));
}
double Number(const Value& value) {
    const auto& lex = Typed(value, Kind::number).string; double result{};
    const auto parsed = std::from_chars(lex.data(), lex.data() + lex.size(), result, std::chars_format::general);
    if (parsed.ptr != lex.data() + lex.size()) Reject();
    if (parsed.ec == std::errc::result_out_of_range) {
        // from_chars reports zero underflow as out_of_range too. A decimal
        // adjusted exponent <= -324 can only be that case, never overflow;
        // representable nonzero subnormals are returned normally.
        auto mantissa = std::string_view(lex); if (mantissa.starts_with('-')) mantissa.remove_prefix(1);
        const auto e = mantissa.find_first_of("eE"); std::int64_t exponent = 0;
        if (e != std::string_view::npos) {
            auto exp = mantissa.substr(e + 1); bool negative = false;
            if (exp.starts_with('+') || exp.starts_with('-')) { negative = exp[0] == '-'; exp.remove_prefix(1); }
            for (const auto c : exp) exponent = std::min<std::int64_t>(100000, exponent * 10 + c - '0');
            if (negative) exponent = -exponent; mantissa = mantissa.substr(0, e);
        }
        const auto point = mantissa.find('.'); const auto before = point == std::string_view::npos ? mantissa.size() : point;
        std::size_t digit = 0, first = std::string_view::npos;
        for (const auto c : mantissa) { if (c == '.') continue; if (first == std::string_view::npos && c != '0') first = digit; ++digit; }
        if (first != std::string_view::npos && exponent + static_cast<std::int64_t>(before) - static_cast<std::int64_t>(first) - 1 > -324) Reject();
        result = 0;
    } else if (parsed.ec != std::errc{}) Reject();
    if (!std::isfinite(result)) Reject(); return result == 0 ? 0 : result;
}
double Nonnegative(const Value& value) {
    const auto result = Number(value); const auto& lex = value.string;
    if (result < 0) Reject();
    if (!lex.empty() && lex[0] == '-') {
        const auto end = lex.find_first_of("eE");
        const auto mantissa = std::string_view(lex).substr(1, end == std::string::npos ? lex.size() - 1 : end - 1);
        if (std::any_of(mantissa.begin(), mantissa.end(), [](char c) { return c >= '1' && c <= '9'; })) Reject();
    }
    return result;
}
Point Vector(const Value& value) {
    Typed(value, Kind::array); if (value.array.size() != 2) Reject();
    return {Nonnegative(value.array[0]), Nonnegative(value.array[1])};
}
Rect Rectangle(const Value& value) {
    Fields(value, {"left", "top", "width", "height"});
    return {I32(At(value, "left")), I32(At(value, "top")), U32(At(value, "width")), U32(At(value, "height"))};
}
bool Id(std::string_view value) {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
std::string Identifier(const Value& value) { const auto& id = Text(value); if (!Id(id)) Reject(); return id; }
std::string Quote(std::string_view value) { return "\"" + json::JsonEscape(value) + "\""; }
std::string Numeric(double value) {
    if (!std::isfinite(value)) Reject(); if (value == 0) return "0";
    char buffer[128]; const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (written.ec != std::errc{}) Reject(); return {buffer, written.ptr};
}

// Rounds precisely the decimal lexeme emitted as rawValue. Multiplying its
// binary64 approximation by 10^dp can cross a tie and disagree with the public
// validator. Digit division gives the exact quotient/remainder instead.
std::string Rounded(std::string_view raw, std::uint32_t decimal_places) {
    if (raw.empty() || raw[0] == '-' || decimal_places > 9) Reject();
    const auto e = raw.find_first_of("eE"); auto mantissa = raw.substr(0, e); std::int64_t exponent = 0;
    if (e != std::string_view::npos) {
        auto exp = raw.substr(e + 1); bool negative = false;
        if (exp.starts_with('+') || exp.starts_with('-')) { negative = exp[0] == '-'; exp.remove_prefix(1); }
        for (const auto c : exp) { if (c < '0' || c > '9' || exponent > 1000) Reject(); exponent = exponent * 10 + c - '0'; }
        if (negative) exponent = -exponent;
    }
    std::string digits; std::int64_t fractional = 0; bool after = false;
    for (const auto c : mantissa) { if (c == '.') { if (after) Reject(); after = true; continue; }
        if (c < '0' || c > '9') Reject(); digits += c; if (after) ++fractional; }
    const auto nonzero = digits.find_first_not_of('0'); if (nonzero == std::string::npos) return "0";
    digits.erase(0, nonzero); const auto shift = exponent + decimal_places - fractional;
    std::string integral, remainder; bool leading_fractional_zero = false;
    if (shift >= 0) {
        if (digits.size() + static_cast<std::uint64_t>(shift) > 12) Reject();
        integral = digits; integral.append(static_cast<std::size_t>(shift), '0');
    } else {
        const auto keep = static_cast<std::int64_t>(digits.size()) + shift;
        if (keep > 0) { integral = digits.substr(0, static_cast<std::size_t>(keep)); remainder = digits.substr(static_cast<std::size_t>(keep)); }
        else { integral = "0"; remainder = digits; leading_fractional_zero = keep < 0; }
    }
    if (integral.size() > 12) Reject(); std::uint64_t quotient = 0;
    for (const auto c : integral) quotient = quotient * 10 + c - '0';
    const bool has_fraction = std::any_of(remainder.begin(), remainder.end(), [](char c) { return c != '0'; });
    constexpr std::uint64_t bound = 1ULL << 39;
    if (quotient > bound || (quotient == bound && has_fraction)) Reject();
    if (!leading_fractional_zero && !remainder.empty()) {
        const bool after_half_nonzero = std::any_of(remainder.begin() + 1, remainder.end(), [](char c) { return c != '0'; });
        if (remainder[0] > '5' || (remainder[0] == '5' && (after_half_nonzero || quotient % 2))) ++quotient;
    }
    if (!quotient) return "0"; auto value = std::to_string(quotient);
    if (decimal_places) {
        if (value.size() <= decimal_places) value.insert(0, decimal_places + 1 - value.size(), '0');
        value.insert(value.size() - decimal_places, 1, '.');
        while (value.back() == '0') value.pop_back(); if (value.back() == '.') value.pop_back();
    }
    return value;
}
std::string Rounding(std::uint32_t places) {
    return "{\"mode\":\"HalfToEven\",\"decimalPlaces\":" + std::to_string(places) + ",\"order\":\"AggregateThenRound\"}";
}
std::string Definition(std::size_t index, std::uint32_t places) {
    std::string aggregation;
    if (index == 0) aggregation = "{\"method\":\"Median\",\"percentile\":null,\"algorithm\":\"SortedMiddle\",\"percentileInterpolation\":null,\"medianEvenRule\":\"MeanOfMiddlePair\"}";
    else if (index == 1) aggregation = "{\"method\":\"Percentile\",\"percentile\":95,\"algorithm\":\"SortedOrderStatistic\",\"percentileInterpolation\":\"LinearR7\",\"medianEvenRule\":null}";
    else if (index == 3 || index == 4) aggregation = "{\"method\":\"Mean\",\"percentile\":null,\"algorithm\":\"PairwiseArithmeticMean\",\"percentileInterpolation\":null,\"medianEvenRule\":null}";
    else aggregation = "{\"method\":\"Maximum\",\"percentile\":null,\"algorithm\":\"SortedLast\",\"percentileInterpolation\":null,\"medianEvenRule\":null}";
    return "{\"schemaVersion\":\"a0.stitch-metric-definition.v1\",\"definitionVersion\":1,\"metricId\":" + Quote(kMetricIds[index])
        + ",\"domain\":\"Image\",\"unit\":" + Quote(index < 3 ? "pixels" : "ratio")
        + ",\"coordinateSystem\":{\"origin\":\"TopLeftPixelCenter\",\"xAxis\":\"Right\",\"yAxis\":\"Down\",\"reference\":\"StitchedOutput\"},"
          "\"mask\":{\"kind\":\"BinaryValidity\",\"source\":\"StitchedOutputValidity\",\"includedValue\":1,\"excludedValue\":0},"
          "\"sampling\":{\"method\":" + Quote(index == 4 ? "Nearest" : "Area") + ",\"outOfBounds\":\"Exclude\",\"invalidSample\":\"Exclude\"},"
          "\"aggregation\":" + aggregation + ",\"rounding\":" + Rounding(places)
        + ",\"boundary\":{\"imageEdge\":\"ExcludeIncompleteKernel\",\"maskEdge\":\"RequireAllSamplesValid\",\"intervalClosure\":\"LowerInclusiveUpperExclusive\"},"
          "\"confidence\":{\"method\":\"None\",\"level\":null},\"uncertainty\":{\"method\":\"None\",\"unitMode\":\"SameAsMetric\"},"
          "\"outcomePolicy\":{\"allowedFailureCodes\":{\"NoResult\":[\"insufficient-valid-samples\",\"zero-valid-results\"],\"NotApplicable\":[],"
          "\"Invalid\":[\"invalid-source-contract\",\"definition-mismatch\"]},\"noResultRule\":\"ZeroValidSamples\"}}";
}
std::string MetricResult(const Metric& metric, std::uint32_t places) {
    if (metric.outcome != "Success" && metric.outcome != "NoResult") Reject();
    std::string raw = "null", value = "null", failure = "null";
    if (metric.outcome == "Success") {
        if (!metric.raw_value || *metric.raw_value < 0 || !metric.sample_count
            || metric.sample_count > 9007199254740991ULL || !metric.failure_code.empty()) Reject();
        raw = Numeric(*metric.raw_value); value = Rounded(raw, places);
    } else {
        if (metric.raw_value || metric.sample_count || (metric.failure_code != "insufficient-valid-samples" && metric.failure_code != "zero-valid-results")) Reject();
        failure = Quote(metric.failure_code);
    }
    return "{\"schemaVersion\":\"a0.stitch-metric-result.v1\",\"definitionVersion\":1,\"metricId\":" + Quote(metric.id)
        + ",\"metricDomain\":\"Image\",\"outcome\":" + Quote(metric.outcome) + ",\"rawValue\":" + raw + ",\"value\":" + value
        + ",\"sampleCount\":" + std::to_string(metric.sample_count) + ",\"rounding\":" + Rounding(places)
        + ",\"confidence\":{\"method\":\"None\",\"level\":null,\"lower\":null,\"upper\":null},\"uncertainty\":{\"method\":\"None\",\"value\":null},\"failureCode\":" + failure + "}";
}
std::string Pixel(Point point) { return "{\"x\":" + Numeric(point.x) + ",\"y\":" + Numeric(point.y) + "}"; }
void Hash(std::string_view value) {
    if (value.size() != 64 || !std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) Reject();
}
} // namespace

Plan ParsePlan(std::string_view ground_truth_json, std::string_view plan_json) {
    const auto truth = Parse(ground_truth_json), root = Parse(plan_json);
    Fields(truth, {"schema", "generator_version", "seed", "conventions", "spec", "output_files", "reference_points", "patterns"});
    Literal(At(truth, "schema"), "a0.m2.synthetic-pair-ground-truth/2");
    Literal(At(truth, "generator_version"), "a0.m2.synthetic-pair-generator/2");
    (void)Unsigned(At(truth, "seed"), std::numeric_limits<std::uint64_t>::max());
    const auto& conventions = At(truth, "conventions"); Fields(conventions, {"documentPlane", "cameraPixel", "matrix", "exifOrientation"});
    Literal(At(conventions, "documentPlane"), "mm-origin-sheet-top-left-x-right-y-down");
    Literal(At(conventions, "cameraPixel"), "stored-order-sample-at-integer-index");
    Literal(At(conventions, "matrix"), "row-major-column-vector"); Literal(At(conventions, "exifOrientation"), "ignored");
    const auto& spec = Typed(At(truth, "spec"), Kind::object); Literal(At(spec, "schema"), "a0.m2.synthetic-pair-spec/2");
    const auto& chart = Typed(At(spec, "chart"), Kind::object);
    const auto chart_width = Number(At(chart, "width_mm")), chart_height = Number(At(chart, "height_mm"));
    if (!(chart_width > 0) || !(chart_height > 0)) Reject();
    Typed(At(truth, "output_files"), Kind::array); Typed(At(truth, "patterns"), Kind::array);
    std::map<std::string, Point> references;
    const auto& reference_points = Typed(At(truth, "reference_points"), Kind::array).array;
    if (reference_points.empty()) Reject();
    for (const auto& reference : reference_points) {
        Fields(reference, {"id", "document_mm", "image_px"});
        const auto id = Identifier(At(reference, "id")); const auto document = Vector(At(reference, "document_mm"));
        if (document.x < 0 || document.y < 0 || document.x >= chart_width || document.y >= chart_height
            || !references.emplace(id, document).second) Reject();
        // image_px is intentionally neither converted nor projected. It is
        // camera-domain annotation, never a stitched-output expectation.
    }
    Fields(root, {"schemaVersion", "raster", "detector", "minimumSamples", "rounding", "fiducials", "dpiPairs", "seamPairs"});
    Literal(At(root, "schemaVersion"), "a0.stitch-evaluation-plan.v1");
    Plan plan;
    const auto& raster = At(root, "raster"); Fields(raster, {"regionUm", "dpi", "widthPixels", "heightPixels"});
    const auto& region = At(raster, "regionUm"); Fields(region, {"left", "top", "right", "bottom"});
    const auto um = [&](std::string_view key) { return static_cast<std::int64_t>(Unsigned(At(region, key), std::numeric_limits<std::int64_t>::max())); };
    plan.raster = {um("left"), um("top"), um("right"), um("bottom"), U32(At(raster, "dpi")), U32(At(raster, "widthPixels")), U32(At(raster, "heightPixels"))};
    if (static_cast<double>(plan.raster.right_um) / 1000.0 > chart_width || static_cast<double>(plan.raster.bottom_um) / 1000.0 > chart_height) Reject();
    const auto& detector = At(root, "detector"); Fields(detector, {"darkThreshold", "minimumContrast", "maximumAspectRatio", "ambiguityGapPixels", "minimumComponentPixels", "maximumComponentPixels"});
    plan.detector = {Nonnegative(At(detector, "darkThreshold")), Nonnegative(At(detector, "minimumContrast")), Nonnegative(At(detector, "maximumAspectRatio")),
        Nonnegative(At(detector, "ambiguityGapPixels")), U32(At(detector, "minimumComponentPixels")), U32(At(detector, "maximumComponentPixels"))};
    const auto& samples = At(root, "minimumSamples"); Fields(samples, {"fiducials", "dpiPairs", "seamPairs"});
    plan.minimum_fiducials = U32(At(samples, "fiducials")); plan.minimum_dpi_pairs = U32(At(samples, "dpiPairs")); plan.minimum_seam_pairs = U32(At(samples, "seamPairs"));
    const auto& rounding = At(root, "rounding"); Fields(rounding, {"decimalPlaces"}); plan.decimal_places = U32(At(rounding, "decimalPlaces"));
    for (const auto& entry : Typed(At(root, "fiducials"), Kind::array).array) {
        Fields(entry, {"id", "halfWindowPixels"}); const auto id = Identifier(At(entry, "id"));
        const auto known = references.find(id); if (known == references.end()) Reject(); const auto doc = known->second;
        const Point expected{(doc.x - static_cast<double>(plan.raster.left_um) / 1000.0) * plan.raster.dpi / 25.4 - 0.5,
            (doc.y - static_cast<double>(plan.raster.top_um) / 1000.0) * plan.raster.dpi / 25.4 - 0.5};
        plan.fiducials.push_back({id, doc, expected, U32(At(entry, "halfWindowPixels"))});
    }
    for (const auto& entry : Typed(At(root, "dpiPairs"), Kind::array).array) {
        Fields(entry, {"firstId", "secondId"}); plan.dpi_pairs.push_back({Identifier(At(entry, "firstId")), Identifier(At(entry, "secondId"))});
    }
    for (const auto& entry : Typed(At(root, "seamPairs"), Kind::array).array) {
        Fields(entry, {"first", "second"}); plan.seam_pairs.push_back({Rectangle(At(entry, "first")), Rectangle(At(entry, "second"))});
    }
    ValidatePlan(plan); return plan;
}
Thresholds ParseThresholds(std::string_view text) {
    const auto root = Parse(text); Fields(root, {"schemaVersion", "checks"}); Literal(At(root, "schemaVersion"), "a0.stitch-evaluation-thresholds.v1");
    const auto& checks = Typed(At(root, "checks"), Kind::array).array; if (checks.size() != kMetricIds.size()) Reject();
    Thresholds thresholds; std::set<std::string> seen;
    for (const auto& check : checks) {
        Fields(check, {"metricId", "comparison", "value"}); const auto id = Identifier(At(check, "metricId"));
        const auto known = std::find(kMetricIds.begin(), kMetricIds.end(), id); if (known == kMetricIds.end() || !seen.insert(id).second) Reject();
        thresholds[static_cast<std::size_t>(known - kMetricIds.begin())] = {id, Text(At(check, "comparison")), Nonnegative(At(check, "value"))};
    }
    ValidateThresholds(thresholds); return thresholds;
}

std::string SerializeReport(const Evaluation& result, const Plan& plan, const Thresholds& thresholds, const SourceHashes& hashes) {
    ValidatePlan(plan); ValidateThresholds(thresholds);
    for (const auto* hash : {&hashes.image, &hashes.ground_truth, &hashes.plan, &hashes.thresholds, &hashes.validity, &hashes.mask}) Hash(*hash);
    if (result.fiducials.size() != plan.fiducials.size() || result.dpi_pairs.size() != plan.dpi_pairs.size()) Reject();
    std::string output = "{\"schemaVersion\":\"a0.stitch-evaluation-report.v1\",\"quality\":\"not-evaluated\",\"thresholdAssessment\":" + Quote(result.threshold_assessment)
        + ",\"measurementModel\":{\"coordinates\":\"document-mm-to-output-sample-center-v1\",\"photometry\":\"rec709-weighted-decoded-rgb-code-values\","
          "\"validity\":\"externally-declared-image-bound-mask\",\"coverageDomain\":\"whole-explicit-output-raster\",\"coverageTrialValidity\":\"all-declared-trials\","
          "\"seamSelection\":\"preannotated-same-flat-content\"},\"sourceHashes\":{\"image\":" + Quote(hashes.image) + ",\"groundTruth\":" + Quote(hashes.ground_truth)
        + ",\"plan\":" + Quote(hashes.plan) + ",\"thresholds\":" + Quote(hashes.thresholds) + ",\"validity\":" + Quote(hashes.validity) + ",\"mask\":" + Quote(hashes.mask)
        + "},\"image\":{\"widthPixels\":" + std::to_string(plan.raster.width) + ",\"heightPixels\":" + std::to_string(plan.raster.height) + "},\"metrics\":[";
    bool incomplete = false, not_met = false;
    for (std::size_t i = 0; i < kMetricIds.size(); ++i) {
        const auto& metric = result.metrics[i]; if (metric.id != kMetricIds[i]) Reject(); if (i) output += ',';
        std::string met = "null";
        if (metric.raw_value) { const bool accepted = thresholds[i].comparison == "AtLeast" ? *metric.raw_value >= thresholds[i].value : *metric.raw_value <= thresholds[i].value;
            met = accepted ? "true" : "false"; if (!accepted) not_met = true; }
        else incomplete = true;
        output += "{\"definition\":" + Definition(i, plan.decimal_places) + ",\"result\":" + MetricResult(metric, plan.decimal_places)
            + ",\"threshold\":{\"comparison\":" + Quote(thresholds[i].comparison) + ",\"value\":" + Numeric(thresholds[i].value) + ",\"met\":" + met + "}}";
    }
    const auto assessment = incomplete ? "incomplete-measurements" : not_met ? "thresholds-not-met" : "thresholds-met";
    if (result.threshold_assessment != assessment) Reject();
    output += "],\"fiducials\":[";
    for (std::size_t i = 0; i < result.fiducials.size(); ++i) {
        const auto& observation = result.fiducials[i]; if (observation.id != plan.fiducials[i].id) Reject(); if (i) output += ',';
        const bool found = observation.status == "found";
        if (!found && observation.status != "missing" && observation.status != "ambiguous" && observation.status != "partial-window" && observation.status != "invalid-mask") Reject();
        if (found != observation.observed_pixel.has_value() || found != observation.position_error_pixels.has_value()) Reject();
        if (found && (!std::isfinite(*observation.position_error_pixels) || *observation.position_error_pixels < 0)) Reject();
        output += "{\"id\":" + Quote(observation.id) + ",\"status\":" + Quote(observation.status) + ",\"expectedPixel\":" + Pixel(plan.fiducials[i].expected_pixel)
            + ",\"observedPixel\":" + (found ? Pixel(*observation.observed_pixel) : "null") + ",\"positionErrorPixels\":" + (found ? Numeric(*observation.position_error_pixels) : "null") + "}";
    }
    output += "],\"dpiPairs\":[";
    for (std::size_t i = 0; i < result.dpi_pairs.size(); ++i) {
        const auto& pair = result.dpi_pairs[i]; if (pair.first_id != plan.dpi_pairs[i].first_id || pair.second_id != plan.dpi_pairs[i].second_id) Reject(); if (i) output += ',';
        if (pair.measured_dpi && *pair.measured_dpi < 0) Reject();
        output += "{\"firstId\":" + Quote(pair.first_id) + ",\"secondId\":" + Quote(pair.second_id) + ",\"measuredDpi\":" + (pair.measured_dpi ? Numeric(*pair.measured_dpi) : "null") + "}";
    }
    output += "],\"diagnosticCodes\":["; std::set<std::string> diagnostics;
    constexpr std::array<std::string_view, 7> allowed{"fiducial-position-out-of-range", "fiducial-coverage-below-minimum", "pixel-coverage-below-minimum",
        "seam-luminance-out-of-range", "seam-color-out-of-range", "local-dpi-out-of-range", "insufficient-valid-measurements"};
    for (std::size_t i = 0; i < result.diagnostic_codes.size(); ++i) {
        const auto& diagnostic = result.diagnostic_codes[i];
        if (std::find(allowed.begin(), allowed.end(), diagnostic) == allowed.end() || !diagnostics.insert(diagnostic).second) Reject();
        if (i) output += ','; output += Quote(diagnostic);
    }
    output += "]}\n"; return output;
}
} // namespace a0::m2::evaluation
