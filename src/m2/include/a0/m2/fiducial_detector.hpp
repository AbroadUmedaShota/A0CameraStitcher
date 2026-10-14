#pragma once

// Development-only ring fiducial detector (Issue #310, #277 stage 3b).
//
// Finds the ring fiducials of the in-house chart in a JPEG and writes an
// observation session (docs/schemas/calibration-observation-session.v1.schema.json).
// This library links OpenCV and exists only for the development executable
// and its tests. It is not part of the product, and the pure calibration
// libraries must not depend on it (CMakeLists.txt checks that at configure time).
//
// Nothing here receives a ground truth. The point identity of a ring comes from
// operator anchors: the operator names the lattice node (column, row) of at
// least three rings and gives a rough pixel position and a search radius for
// each. The anchors are recorded in the detection report. Every other ring is
// identified by growing the lattice from the anchors.

#include <opencv2/core.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::fiducial {

inline constexpr std::string_view kDetectorToolId = "a0-fiducial-detector";
inline constexpr std::string_view kDetectorVersion = "1.0.0";
inline constexpr std::string_view kJobSchemaVersion = "a0.fiducial-detector-job/1";
inline constexpr std::string_view kReportSchemaVersion = "a0.fiducial-detector-report/1";
inline constexpr std::uint64_t kMaximumImageFileBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumJobBytes = 1ULL << 20;
inline constexpr std::uint32_t kMaximumLatticeIndex = 999; // the point id has three digits per index

// The printed lattice of ring centres. Node (column, row) sits at
// origin + pitch * (column, row) millimetres on the chart; its point id is
// "F-<column>-<row>" with three digits each (the id the synthetic chart uses).
struct ChartDefinition {
    std::uint32_t columns{};  // nodes are (columns + 1) x (rows + 1)
    std::uint32_t rows{};
    double origin_x_mm{};
    double origin_y_mm{};
    double pitch_mm{};

    [[nodiscard]] std::size_t NodeCount() const noexcept {
        return static_cast<std::size_t>(columns + 1U) * static_cast<std::size_t>(rows + 1U);
    }
};

// Throws std::invalid_argument for a chart that cannot be indexed.
void ValidateChart(const ChartDefinition& chart);
[[nodiscard]] std::string PointId(std::uint32_t column, std::uint32_t row);

// Detector settings. They are recorded in the report; none of them is a
// product threshold.
struct DetectorParams {
    // Pixels darker than low + dark_fraction * (high - low) are ink, with low and
    // high the 1st and 99th percentile of the image.
    double dark_fraction{0.5};
    double blur_sigma{1.0};
    double min_hole_radius_px{4.0};
    double max_hole_radius_px{600.0};
    double min_axis_ratio{0.4};          // minor / major axis of the white ring
    double ellipse_area_tolerance{0.15}; // |contour area / ellipse area - 1|
    double min_dot_area_ratio{0.07};     // dot area / white ring area (design value 0.174; contour polygons make it smaller)
    double max_dot_area_ratio{0.30};
    double concentric_tolerance{0.15};   // distance of the dot centre, in white ring radii
    double min_ring_dark_fraction{0.85}; // share of the surrounding black ring that is dark
    int refine_iterations{3};
    double accept_fraction{0.3};         // match radius, in local lattice spacings
};

// A ring found in the image, centre in stored-order pixels (an integer index is
// a pixel centre).
struct RingCandidate {
    double x{};
    double y{};
    double hole_radius_px{}; // equivalent radius of the white ring
};

struct LatticeAnchor {
    std::uint32_t column{};
    std::uint32_t row{};
    double x_px{};      // rough position given by the operator
    double y_px{};
    double radius_px{}; // search radius around it
};

enum class NodeState { Detected, Missing, Ambiguous };

struct NodeObservation {
    std::uint32_t column{};
    std::uint32_t row{};
    NodeState state{NodeState::Missing};
    double x_px{};      // meaningful only when Detected
    double y_px{};
};

struct AnchorResolution {
    LatticeAnchor anchor;
    std::optional<RingCandidate> ring; // the ring the anchor was bound to
};

struct ViewDetection {
    std::string failure;       // empty on success; a short code
    std::string failure_detail;
    double dark_threshold{};
    std::vector<RingCandidate> candidates;
    std::vector<AnchorResolution> anchors;
    std::vector<NodeObservation> nodes; // raster order (row, then column)
    std::uint32_t unmatched_candidates{};
};

// All rings (a white ring with a concentric dark dot, inside a dark ring, fully
// inside the image) with sub-pixel centres. `gray` is CV_8UC1.
[[nodiscard]] std::vector<RingCandidate> FindRingCandidates(
    const cv::Mat& gray, const DetectorParams& params, double* dark_threshold = nullptr);

// Identifies the rings from the anchors. On failure `failure` is set and
// `nodes` is empty.
[[nodiscard]] ViewDetection DetectView(const cv::Mat& gray, const ChartDefinition& chart,
    std::span<const LatticeAnchor> anchors, const DetectorParams& params);

// Job file -> session and report.
// Throws std::invalid_argument for a malformed job and std::runtime_error for
// an unreadable image. A view whose anchors do not resolve is not an exception:
// it is recorded in the report and `session_json` stays empty.
struct JobResult {
    std::string session_json;  // empty when any view failed
    std::string report_json;
    bool ok{};
};
[[nodiscard]] JobResult RunJob(std::string_view job_json, const std::filesystem::path& image_base_directory,
    const DetectorParams& params = {});

} // namespace a0::m2::fiducial
