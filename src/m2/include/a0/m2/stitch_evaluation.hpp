#pragma once

// Independent image observations. No renderer, profile, stitcher, generator,
// WIC or product metric implementation is part of this interface.
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::evaluation {

inline constexpr std::array<std::string_view, 8> kMetricIds{
    "fiducial-position-median", "fiducial-position-p95", "fiducial-position-max",
    "fiducial-detection-coverage", "valid-pixel-coverage", "seam-luma-step-max",
    "seam-rgb-step-max", "local-dpi-error-ratio-max"};

struct Point { double x, y; };
struct Rect { std::int32_t left, top; std::uint32_t width, height; };
struct Raster {
    std::int64_t left_um, top_um, right_um, bottom_um;
    std::uint32_t dpi, width, height;
};
struct Detector {
    double dark_threshold, minimum_contrast, maximum_aspect_ratio, ambiguity_gap_pixels;
    std::uint32_t minimum_component_pixels, maximum_component_pixels;
};
struct Fiducial { std::string id; Point document_mm, expected_pixel; std::uint32_t half_window_pixels; };
struct DpiPair { std::string first_id, second_id; };
struct SeamPair { Rect first, second; };
struct Plan {
    Raster raster;
    Detector detector;
    std::uint32_t minimum_fiducials, minimum_dpi_pairs, minimum_seam_pairs, decimal_places;
    std::vector<Fiducial> fiducials;
    std::vector<DpiPair> dpi_pairs;
    std::vector<SeamPair> seam_pairs;
};
struct Threshold { std::string metric_id; std::string comparison; double value; };
using Thresholds = std::array<Threshold, kMetricIds.size()>;
struct ImageView { std::uint32_t width, height; std::span<const std::uint8_t> bgr; };
struct Observation {
    std::string id, status;
    std::optional<Point> observed_pixel;
    std::optional<double> position_error_pixels;
};
struct DpiObservation { std::string first_id, second_id; std::optional<double> measured_dpi; };
struct Metric {
    std::string id, outcome, failure_code;
    std::optional<double> raw_value;
    std::uint64_t sample_count;
};
struct Evaluation {
    std::array<Metric, kMetricIds.size()> metrics;
    std::vector<Observation> fiducials;
    std::vector<DpiObservation> dpi_pairs;
    std::string threshold_assessment;
    std::vector<std::string> diagnostic_codes;
};
struct SourceHashes { std::string image, ground_truth, plan, thresholds, validity, mask; };

// JSON shapes are defined by the evaluator contracts, not by product types.
// Expected stitched centers derive from ground-truth document_mm and the
// explicit raster. Raw camera image_px is never an expected stitched center.
Plan ParsePlan(std::string_view ground_truth_json, std::string_view plan_json);
Thresholds ParseThresholds(std::string_view json);
void ValidatePlan(const Plan& plan);
void ValidateThresholds(const Thresholds& thresholds);

// Mask samples are exactly 0 or 1 and bind to the decoded image externally.
// Missing/ambiguous/clipped fiducials remain in the coverage denominator.
Evaluation Evaluate(const ImageView& image, std::span<const std::uint8_t> validity_mask,
    const Plan& plan, const Thresholds& thresholds);

// Each embedded definition/result follows the existing stitch-metric v1
// contracts. Successful threshold exceedances retain outcome=Success and
// failureCode=null; diagnostic codes are separate and quality stays unevaluated.
std::string SerializeReport(const Evaluation& result, const Plan& plan,
    const Thresholds& thresholds, const SourceHashes& hashes);

} // namespace a0::m2::evaluation
