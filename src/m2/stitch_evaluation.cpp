#include "a0/m2/stitch_evaluation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace a0::m2::evaluation {
namespace {
constexpr std::uint64_t maximum_pixels = 200'000'000;
constexpr std::uint64_t roi_budget = 16'777'216;
constexpr std::uint32_t maximum_items = 4096, maximum_half_window = 512;
[[noreturn]] void Reject() { throw std::invalid_argument("invalid independent evaluation contract"); }
bool Finite(double value) { return std::isfinite(value); }
bool Identifier(std::string_view id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
Point Expected(const Point document, const Raster& raster) {
    return {(document.x - static_cast<double>(raster.left_um) / 1000.0) * raster.dpi / 25.4 - 0.5,
        (document.y - static_cast<double>(raster.top_um) / 1000.0) * raster.dpi / 25.4 - 0.5};
}
void Budget(std::uint64_t amount, std::uint64_t& remaining) { if (amount > remaining) Reject(); remaining -= amount; }
using RectangleKey = std::tuple<std::int32_t, std::int32_t, std::uint32_t, std::uint32_t>;
RectangleKey Key(const Rect& rect) { return {rect.left, rect.top, rect.width, rect.height}; }
bool InBounds(const Rect& rect, const ImageView& image) {
    return rect.left >= 0 && rect.top >= 0 && rect.width && rect.height
        && static_cast<std::uint64_t>(rect.left) + rect.width <= image.width
        && static_cast<std::uint64_t>(rect.top) + rect.height <= image.height;
}
bool AllValid(const Rect& rect, const ImageView& image, std::span<const std::uint8_t> mask) {
    if (!InBounds(rect, image)) return false;
    for (std::uint32_t y = 0; y < rect.height; ++y)
        for (std::uint32_t x = 0; x < rect.width; ++x)
            if (!mask[(static_cast<std::size_t>(rect.top) + y) * image.width + static_cast<std::size_t>(rect.left) + x]) return false;
    return true;
}
double Luma(const ImageView& image, std::size_t index) {
    const auto p = index * 3;
    return (0.2126 * image.bgr[p + 2] + 0.7152 * image.bgr[p + 1] + 0.0722 * image.bgr[p]) / 255.0;
}

Observation Detect(const ImageView& image, std::span<const std::uint8_t> mask, const Fiducial& fiducial, const Detector& detector) {
    Observation observation{fiducial.id, "missing", std::nullopt, std::nullopt};
    const auto cx = static_cast<std::int64_t>(std::floor(fiducial.expected_pixel.x + 0.5));
    const auto cy = static_cast<std::int64_t>(std::floor(fiducial.expected_pixel.y + 0.5));
    const auto half = static_cast<std::int64_t>(fiducial.half_window_pixels);
    const auto side = fiducial.half_window_pixels * 2 + 1;
    const Rect window{static_cast<std::int32_t>(cx - half), static_cast<std::int32_t>(cy - half), side, side};
    if (!InBounds(window, image)) { observation.status = "partial-window"; return observation; }
    if (!AllValid(window, image, mask)) { observation.status = "invalid-mask"; return observation; }
    const auto area = static_cast<std::size_t>(side) * side;
    std::vector<double> luma(area);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x)
            luma[static_cast<std::size_t>(y) * side + x] = Luma(image,
                (static_cast<std::size_t>(window.top) + y) * image.width + static_cast<std::size_t>(window.left) + x);
    std::vector<std::uint8_t> visited(area);
    std::vector<std::size_t> queue;
    struct Candidate { Point point; double distance; };
    std::vector<Candidate> candidates;
    for (std::size_t start = 0; start < area; ++start) {
        if (visited[start] || !(luma[start] < detector.dark_threshold)) continue;
        queue.clear(); queue.push_back(start); visited[start] = 1;
        std::uint32_t min_x = side, min_y = side, max_x = 0, max_y = 0;
        double sum_weight = 0, weighted_x = 0, weighted_y = 0, component_luma = 0;
        for (std::size_t next = 0; next < queue.size(); ++next) {
            const auto index = queue[next]; const auto x = static_cast<std::uint32_t>(index % side), y = static_cast<std::uint32_t>(index / side);
            min_x = std::min(min_x, x); max_x = std::max(max_x, x); min_y = std::min(min_y, y); max_y = std::max(max_y, y);
            const auto weight = detector.dark_threshold - luma[index];
            component_luma += luma[index];
            sum_weight += weight; weighted_x += weight * (window.left + static_cast<double>(x));
            weighted_y += weight * (window.top + static_cast<double>(y));
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const auto nx = static_cast<std::int64_t>(x) + dx, ny = static_cast<std::int64_t>(y) + dy;
                if (nx < 0 || ny < 0 || nx >= side || ny >= side) continue;
                const auto neighbour = static_cast<std::size_t>(ny) * side + static_cast<std::size_t>(nx);
                if (!visited[neighbour] && luma[neighbour] < detector.dark_threshold) { visited[neighbour] = 1; queue.push_back(neighbour); }
            }
        }
        const auto count = queue.size();
        const auto width = static_cast<double>(max_x - min_x + 1), height = static_cast<double>(max_y - min_y + 1);
        const auto aspect = std::max(width / height, height / width);
        // A component touching the search boundary may be a clipped feature,
        // and a uniformly dark crop is not a fiducial. Its surrounding one-
        // pixel rectangular border must also fit inside this valid window.
        if (!min_x || !min_y || max_x + 1 >= side || max_y + 1 >= side) continue;
        double background_luma = 0; std::uint64_t background_count = 0;
        for (auto y = min_y - 1; y <= max_y + 1; ++y) for (auto x = min_x - 1; x <= max_x + 1; ++x) {
            if (x != min_x - 1 && x != max_x + 1 && y != min_y - 1 && y != max_y + 1) continue;
            background_luma += luma[static_cast<std::size_t>(y) * side + x]; ++background_count;
        }
        const auto contrast = background_luma / background_count - component_luma / count;
        if (count < detector.minimum_component_pixels || count > detector.maximum_component_pixels
            || aspect > detector.maximum_aspect_ratio || contrast < detector.minimum_contrast || !(sum_weight > 0)) continue;
        const Point center{weighted_x / sum_weight, weighted_y / sum_weight};
        candidates.push_back({center, std::hypot(center.x - fiducial.expected_pixel.x, center.y - fiducial.expected_pixel.y)});
    }
    if (candidates.empty()) return observation;
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    if (candidates.size() > 1 && candidates[1].distance - candidates[0].distance <= detector.ambiguity_gap_pixels) {
        observation.status = "ambiguous"; return observation;
    }
    observation.status = "found"; observation.observed_pixel = candidates[0].point; observation.position_error_pixels = candidates[0].distance;
    return observation;
}
std::array<double, 3> PatchMean(const ImageView& image, const Rect& patch) {
    std::array<std::uint64_t, 3> sums{};
    for (std::uint32_t y = 0; y < patch.height; ++y) for (std::uint32_t x = 0; x < patch.width; ++x) {
        const auto p = ((static_cast<std::size_t>(patch.top) + y) * image.width + static_cast<std::size_t>(patch.left) + x) * 3;
        for (unsigned c = 0; c < 3; ++c) sums[c] += image.bgr[p + c];
    }
    const auto denominator = static_cast<double>(static_cast<std::uint64_t>(patch.width) * patch.height) * 255.0;
    return {sums[0] / denominator, sums[1] / denominator, sums[2] / denominator};
}
void Diagnostic(Evaluation& result, const std::string& code) {
    if (std::find(result.diagnostic_codes.begin(), result.diagnostic_codes.end(), code) == result.diagnostic_codes.end()) result.diagnostic_codes.push_back(code);
}
Metric Aggregate(std::size_t index, std::vector<double> values, std::uint32_t minimum) {
    Metric metric{std::string(kMetricIds[index]), "NoResult", values.empty() ? "zero-valid-results" : "insufficient-valid-samples", std::nullopt, 0};
    if (values.size() < minimum) return metric;
    std::sort(values.begin(), values.end()); double value{};
    if (index == 0) {
        const auto middle = values.size() / 2;
        value = values.size() % 2 ? values[middle] : values[middle - 1] + (values[middle] - values[middle - 1]) * 0.5;
    } else if (index == 1) {
        const auto rank = 0.95 * static_cast<double>(values.size() - 1);
        const auto low = static_cast<std::size_t>(std::floor(rank)); const auto high = static_cast<std::size_t>(std::ceil(rank));
        value = values[low] + (values[high] - values[low]) * (rank - low);
    } else value = values.back();
    if (!Finite(value) || value < 0) Reject();
    metric.outcome = "Success"; metric.failure_code.clear(); metric.raw_value = value; metric.sample_count = values.size(); return metric;
}
} // namespace

void ValidatePlan(const Plan& plan) {
    const auto& r = plan.raster;
    if (r.left_um < 0 || r.top_um < 0 || r.right_um <= r.left_um || r.bottom_um <= r.top_um
        || !r.dpi || r.dpi > 65535 || !r.width || !r.height || r.width > 32768 || r.height > 32768
        || static_cast<std::uint64_t>(r.width) * r.height > maximum_pixels) Reject();
    const auto dimension = [&](std::uint64_t span) {
        if (span > (32768ULL * 25400ULL) / r.dpi) Reject();
        return (span * r.dpi + 25399ULL) / 25400ULL;
    };
    if (dimension(static_cast<std::uint64_t>(r.right_um - r.left_um)) != r.width
        || dimension(static_cast<std::uint64_t>(r.bottom_um - r.top_um)) != r.height) Reject();
    const auto& d = plan.detector;
    if (!Finite(d.dark_threshold) || d.dark_threshold < 0 || d.dark_threshold > 1
        || !Finite(d.minimum_contrast) || d.minimum_contrast < 0 || d.minimum_contrast > 1
        || !Finite(d.maximum_aspect_ratio) || d.maximum_aspect_ratio < 1
        || !Finite(d.ambiguity_gap_pixels) || d.ambiguity_gap_pixels < 0
        || !d.minimum_component_pixels || d.maximum_component_pixels < d.minimum_component_pixels
        || d.maximum_component_pixels > (maximum_half_window * 2 + 1) * (maximum_half_window * 2 + 1)) Reject();
    if (plan.fiducials.empty() || plan.fiducials.size() > maximum_items || plan.dpi_pairs.size() > maximum_items
        || plan.seam_pairs.size() > maximum_items || !plan.minimum_fiducials || plan.minimum_fiducials > maximum_items
        || !plan.minimum_dpi_pairs || plan.minimum_dpi_pairs > maximum_items || !plan.minimum_seam_pairs
        || plan.minimum_seam_pairs > maximum_items || plan.decimal_places > 9) Reject();
    std::uint64_t remaining = roi_budget; std::map<std::string, Point> points;
    for (const auto& fiducial : plan.fiducials) {
        const auto& doc = fiducial.document_mm;
        if (!Identifier(fiducial.id) || !points.emplace(fiducial.id, doc).second
            || !Finite(doc.x) || !Finite(doc.y) || doc.x < static_cast<double>(r.left_um) / 1000.0
            || doc.x >= static_cast<double>(r.right_um) / 1000.0 || doc.y < static_cast<double>(r.top_um) / 1000.0
            || doc.y >= static_cast<double>(r.bottom_um) / 1000.0 || !fiducial.half_window_pixels
            || fiducial.half_window_pixels > maximum_half_window) Reject();
        const auto expected = Expected(doc, r);
        if (!Finite(fiducial.expected_pixel.x) || !Finite(fiducial.expected_pixel.y)
            || fiducial.expected_pixel.x != expected.x || fiducial.expected_pixel.y != expected.y) Reject();
        const auto side = static_cast<std::uint64_t>(fiducial.half_window_pixels) * 2 + 1; Budget(side * side, remaining);
    }
    std::set<std::pair<std::string, std::string>> pairs;
    for (const auto& pair : plan.dpi_pairs) {
        if (!points.contains(pair.first_id) || !points.contains(pair.second_id) || pair.first_id == pair.second_id) Reject();
        const auto key = std::minmax(pair.first_id, pair.second_id);
        if (!pairs.emplace(key.first, key.second).second) Reject();
        const auto a = points.at(pair.first_id), b = points.at(pair.second_id);
        if (!(std::hypot(a.x - b.x, a.y - b.y) > 0)) Reject();
    }
    std::set<std::pair<RectangleKey, RectangleKey>> patches;
    for (const auto& pair : plan.seam_pairs) {
        auto first = Key(pair.first), second = Key(pair.second);
        if (first == second) Reject(); if (second < first) std::swap(first, second);
        if (!patches.emplace(first, second).second) Reject();
        for (const auto& rect : {pair.first, pair.second}) {
            if (!rect.width || !rect.height || rect.width > 32768 || rect.height > 32768) Reject();
            Budget(static_cast<std::uint64_t>(rect.width) * rect.height, remaining);
        }
    }
}
void ValidateThresholds(const Thresholds& thresholds) {
    for (std::size_t i = 0; i < thresholds.size(); ++i) {
        if (thresholds[i].metric_id != kMetricIds[i] || !Finite(thresholds[i].value) || thresholds[i].value < 0
            || thresholds[i].comparison != (i == 3 || i == 4 ? "AtLeast" : "AtMost")
            || ((i == 3 || i == 4) && thresholds[i].value > 1)) Reject();
    }
}

Evaluation Evaluate(const ImageView& image, std::span<const std::uint8_t> validity_mask,
    const Plan& plan, const Thresholds& thresholds) {
    ValidatePlan(plan); ValidateThresholds(thresholds);
    const auto pixels = static_cast<std::uint64_t>(image.width) * image.height;
    if (image.width != plan.raster.width || image.height != plan.raster.height || image.bgr.size() != pixels * 3
        || validity_mask.size() != pixels || std::any_of(validity_mask.begin(), validity_mask.end(), [](auto v) { return v > 1; })) Reject();
    Evaluation result; std::vector<double> position_errors; std::map<std::string, std::size_t> ids;
    for (const auto& fiducial : plan.fiducials) {
        ids.emplace(fiducial.id, result.fiducials.size());
        auto observation = Detect(image, validity_mask, fiducial, plan.detector);
        if (observation.position_error_pixels) position_errors.push_back(*observation.position_error_pixels);
        result.fiducials.push_back(std::move(observation));
    }
    for (std::size_t i = 0; i < 3; ++i) result.metrics[i] = Aggregate(i, position_errors, plan.minimum_fiducials);
    result.metrics[3] = {std::string(kMetricIds[3]), "Success", {}, static_cast<double>(position_errors.size()) / plan.fiducials.size(), plan.fiducials.size()};
    const auto valid = std::count(validity_mask.begin(), validity_mask.end(), static_cast<std::uint8_t>(1));
    result.metrics[4] = {std::string(kMetricIds[4]), "Success", {}, static_cast<double>(valid) / static_cast<double>(pixels), pixels};
    std::vector<double> luma_steps, rgb_steps;
    for (const auto& pair : plan.seam_pairs) {
        if (!AllValid(pair.first, image, validity_mask) || !AllValid(pair.second, image, validity_mask)) continue;
        const auto first = PatchMean(image, pair.first), second = PatchMean(image, pair.second);
        const auto luminance = [](const std::array<double, 3>& p) { return 0.2126 * p[2] + 0.7152 * p[1] + 0.0722 * p[0]; };
        luma_steps.push_back(std::abs(luminance(first) - luminance(second)));
        rgb_steps.push_back(std::max({std::abs(first[0] - second[0]), std::abs(first[1] - second[1]), std::abs(first[2] - second[2])}));
    }
    result.metrics[5] = Aggregate(5, luma_steps, plan.minimum_seam_pairs); result.metrics[6] = Aggregate(6, rgb_steps, plan.minimum_seam_pairs);
    std::vector<double> dpi_errors;
    for (const auto& pair : plan.dpi_pairs) {
        const auto a = ids.at(pair.first_id), b = ids.at(pair.second_id);
        DpiObservation observation{pair.first_id, pair.second_id, std::nullopt};
        if (result.fiducials[a].observed_pixel && result.fiducials[b].observed_pixel) {
            const auto first = *result.fiducials[a].observed_pixel, second = *result.fiducials[b].observed_pixel;
            const auto doc_a = plan.fiducials[a].document_mm, doc_b = plan.fiducials[b].document_mm;
            const auto dpi = 25.4 * std::hypot(first.x - second.x, first.y - second.y) / std::hypot(doc_a.x - doc_b.x, doc_a.y - doc_b.y);
            if (!Finite(dpi)) Reject(); observation.measured_dpi = dpi;
            dpi_errors.push_back(std::abs(dpi / plan.raster.dpi - 1));
        }
        result.dpi_pairs.push_back(std::move(observation));
    }
    result.metrics[7] = Aggregate(7, dpi_errors, plan.minimum_dpi_pairs);
    constexpr std::array<std::string_view, 8> diagnostic{
        "fiducial-position-out-of-range", "fiducial-position-out-of-range", "fiducial-position-out-of-range",
        "fiducial-coverage-below-minimum", "pixel-coverage-below-minimum", "seam-luminance-out-of-range",
        "seam-color-out-of-range", "local-dpi-out-of-range"};
    bool incomplete = false, exceeded = false;
    for (std::size_t i = 0; i < result.metrics.size(); ++i) {
        const auto& metric = result.metrics[i];
        if (!metric.raw_value) { incomplete = true; continue; }
        const bool met = thresholds[i].comparison == "AtLeast" ? *metric.raw_value >= thresholds[i].value : *metric.raw_value <= thresholds[i].value;
        if (!met) { exceeded = true; Diagnostic(result, std::string(diagnostic[i])); }
    }
    if (incomplete) Diagnostic(result, "insufficient-valid-measurements");
    result.threshold_assessment = incomplete ? "incomplete-measurements" : exceeded ? "thresholds-not-met" : "thresholds-met";
    return result;
}
} // namespace a0::m2::evaluation
