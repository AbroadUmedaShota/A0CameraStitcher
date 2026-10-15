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
//
// Anchors are checked against the chart (Issue #311). The orientation of the
// anchors must not be mirrored, and the pattern printed in the cells between
// detected rings (the chart's own cell pattern rule, not a ground truth) must
// agree with the node names the anchors gave. A view that fails either check is
// rejected and no observation of it is written.

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
inline constexpr std::string_view kDetectorVersion = "1.1.0";
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

// The pattern printed in the middle of the cell whose top-left node is
// (column, row), by the in-house chart's rule: kind = (3 * column + 5 * row)
// mod 8 (0 and 6 empty, 1 slanted edge dark on light, 7 slanted edge light on
// dark, 2 colour patch, 5 grey patch, 3 vertical fine lines, 4 horizontal fine
// lines). This is the chart's design, the same definition the synthetic chart
// is generated from; it is not read from the generator.
enum class CellPattern {
    Blank,
    SlantedEdgeDarkOnLight,
    SlantedEdgeLightOnDark,
    ColorPatch,
    GrayPatch,
    FineLinesVertical,
    FineLinesHorizontal,
};
[[nodiscard]] CellPattern ExpectedCellPattern(std::uint32_t column, std::uint32_t row) noexcept;
[[nodiscard]] const char* CellPatternCode(CellPattern pattern) noexcept;
// Reflectance of the grey patch of cell (column, row), as a fraction of white:
// the chart's six levels in the order of (column + row) mod 6.
[[nodiscard]] double ExpectedGrayPatchLevel(std::uint32_t column, std::uint32_t row) noexcept;

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

    // Identity check of the anchors against the cell patterns (#311). Levels are
    // compared as a ratio to the white of the same cell, which assumes a linear
    // response; vignetting and exposure cancel in the ratio.
    double identity_level_tolerance{0.08};     // |measured ratio - expected ratio| for a flat patch
    double identity_line_spread{0.30};         // fine lines: (90th - 10th percentile) / white, at least
    double identity_line_share{0.75};          // fine lines: share of level crossings along one cell axis
    std::uint32_t identity_min_cells{8};       // cells that must be checked, else the view is not verifiable
    std::uint32_t identity_min_gray_cells{2};  // grey patches that must be checked, else not verifiable
    double identity_max_mismatch_fraction{0.10}; // reject when more than this share of the cells disagree ...
    std::uint32_t identity_min_mismatches{2};    // ... and at least this many do (applies to each of the two checks)
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

// A cell whose measured pattern disagrees with the one its node names expect.
struct CellMismatch {
    std::uint32_t column{};   // top-left node of the cell
    std::uint32_t row{};
    CellPattern expected{CellPattern::Blank};
    std::string check;        // "structure" or "gray-level"
    double center_ratio{};    // middle of the cell / white of the cell
    double ring_ratio{};      // band just outside the pattern / white of the cell
    double spread{};          // (90th - 10th percentile of the middle) / white
    double axis_share{};      // share of the gradient energy along the column direction
};

// Result of checking the anchors against the chart (#311).
struct IdentityCheck {
    // "not-run" (an earlier failure), "verified", "mismatch" or "unverifiable".
    std::string result{"not-run"};
    std::uint32_t anchor_triples{};         // non-degenerate anchor triples whose orientation was checked
    std::uint32_t mirrored_triples{};
    std::uint32_t cells_examined{};         // cells with four detected corners that could be measured
    std::uint32_t cells_skipped{};          // four detected corners, but the cell could not be measured
    std::uint32_t cells_mismatched{};       // pattern disagrees with the one the node names expect
    std::uint32_t gray_cells_examined{};    // expected grey patches that could be measured
    std::uint32_t gray_cells_mismatched{};  // grey level disagrees with the expected level
    std::vector<CellMismatch> mismatches;   // the first ones, for the report
};

struct ViewDetection {
    std::string failure;       // empty on success; a short code
    std::string failure_detail;
    double dark_threshold{};
    std::vector<RingCandidate> candidates;
    std::vector<AnchorResolution> anchors;
    IdentityCheck identity;
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
