#pragma once

// Seam quality gate: measurer (G1) and decision (G2) of #385/#386.
// Design: docs/M2_SEAM_QUALITY_GATE.md.
//
// The measurer never reads a composite. Its inputs are the two original
// images and the two output-to-camera mappings the composite is rendered
// with, so the result is the same for any blend method (seam-cut, feather).
// A and B are each placed on the output grid by their own mapping, without
// mixing and without brightness matching, and the local position difference
// is measured across the transition band in output pixels.
//
// Pure computation: no camera, file or clock dependency, no random choice.
// The same inputs give the same result for any thread count. Inputs are not
// modified. Programming errors (sizes, missing mappings, invalid constants)
// throw std::invalid_argument.

#include "a0/m2/document_render.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::seam_gate {

using render::BgrImage;
using render::OutputToCamera;
using render::Point;

// Output columns [begin, end) of one output row of the transition band
// (the range where the composite may switch from A to B). Empty when
// begin == end.
struct BandRow {
    std::uint32_t begin{};
    std::uint32_t end{};
};

// Layout is CAM-A left, CAM-B right. The mappings take an output sample
// position (x, y), possibly fractional, and return raw camera sample
// coordinates in the convention of render::RenderMappedPair. They are called
// concurrently and must be immutable. A true return with nonfinite
// coordinates throws.
struct SeamGateGeometry {
    std::uint32_t width{};
    std::uint32_t height{};
    OutputToCamera camera_a;
    OutputToCamera camera_b;
    std::vector<BandRow> band; // one entry per output row
};

// Mappings of render::RenderPair (uncalibrated similarity, #365). Band empty.
SeamGateGeometry PairSeamGeometry(std::uint32_t camera_a_width, std::uint32_t camera_a_height,
    std::uint32_t camera_b_width, std::uint32_t camera_b_height, const render::PairRenderParameters& parameters);
// Mappings of render::RenderDocumentPair (calibrated profile). Band empty.
SeamGateGeometry DocumentSeamGeometry(const render::DocumentRenderParameters& parameters);
// Overlap of the two coverages per row, shrunk by inset_px on every side
// (rows within inset_px of a row without overlap become empty).
std::vector<BandRow> OverlapBand(const SeamGateGeometry& geometry, std::uint32_t camera_a_width,
    std::uint32_t camera_a_height, std::uint32_t camera_b_width, std::uint32_t camera_b_height, std::uint32_t inset_px);

// Optional calibration prediction of the B-minus-A displacement at an output
// position, in output pixels. false: no prediction there.
// Product paths must not pass a prediction until the global placement is
// verified outside the band: a predicted blank block can pass a placement that
// is one lattice period off (docs/M2_SEAM_QUALITY_GATE.md, 16.10).
using DisplacementPrediction = std::function<bool(Point, Point&)>;

// Design constants of the measurer (docs/M2_SEAM_QUALITY_GATE.md, 3.3/3.4).
// None of them is a pass/fail line. Every field is required.
struct SeamGateDesign {
    std::uint32_t point_spacing_px;      // measurement lattice
    std::uint32_t patch_px;              // matching patch side
    std::uint32_t block_px;              // accounting block side
    std::uint32_t search_radius_px;      // first search radius
    std::uint32_t maximum_search_radius_px; // widened on a boundary hit, doubling
    std::uint32_t refinement_levels;     // grid halving after the integer search (#323)
    double content_gradient_dn;          // gradient magnitude that counts as ink
    std::uint32_t content_minimum_samples; // samples at that level for "content"
    double one_direction_ratio;          // structure tensor lambda2/lambda1 below: lines in one direction
    double secondary_peak_ratio;         // second peak / best at or above: ambiguous or periodic
    double round_trip_px;                // forward/reverse disagreement limit
    double uncertainty_floor_px;         // systematic floor added to every point
    double maximum_uncertainty_px;       // point uncertainty limit
    std::uint32_t remeasure_shift_px;    // variant 1: patch moved along the seam
    std::uint32_t remeasure_patch_px;    // variant 2: larger patch
    double agreement_factor;             // agreement: max(factor * u, floor)
    double agreement_floor_px;
    double block_neighbor_px;            // block value differing from every measured neighbour by more
    std::uint32_t strip_rows;            // rows sampled together (memory bound)
};
inline constexpr SeamGateDesign kSeamGateDesignCalibrated{
    32, 64, 128, 16, 256, 7, 8.0, 32, 0.1, 0.8, 0.05, 0.01, 0.05, 16, 96, 3.0, 0.1, 0.5, 256};
inline constexpr SeamGateDesign kSeamGateDesignUncalibrated{
    32, 64, 128, 48, 256, 7, 8.0, 32, 0.1, 0.8, 0.05, 0.01, 0.05, 16, 96, 3.0, 0.1, 0.5, 256};
// Throws std::invalid_argument for an inconsistent set (see the docs).
void ValidateSeamGateDesign(const SeamGateDesign& design);
// Band inset that keeps every search window of the first radius inside both
// coverages (horizontal and vertical).
std::uint32_t RequiredBandInsetPx(const SeamGateDesign& design);

// Owner's values (#385: limit 1 px, measurement uncertainty 0.2 px). No
// default here; the caller reads them from its policy or profile.
struct SeamGateLimits {
    // NaN until set, so an unset line is rejected instead of read.
    double limit_px = std::numeric_limits<double>::quiet_NaN();
    double guard_band_px = std::numeric_limits<double>::quiet_NaN();
    std::string source;
};

enum class PointStatus {
    outside_coverage,          // patch not covered by both cameras; not counted anywhere
    no_content,
    search_window_unavailable, // window or periodicity window leaves a coverage
    flat_correlation,
    ambiguous,                 // no strict maximum, or a second peak
    periodic,
    search_boundary,
    round_trip_inconsistent,
    uncertain,
    valid,
};
std::string_view PointStatusCode(PointStatus status);
enum class PointKind { two_directional, one_directional };
enum class Confirmation { not_required, confirmed, unconfirmed };
enum class Variant { shifted, enlarged, luminance };
std::string_view VariantCode(Variant variant);

struct Remeasurement {
    Variant variant{};
    PointStatus status{PointStatus::no_content};
    PointKind kind{};
    Point normal{};
    Point displacement{};
    double uncertainty_px{};
    bool agrees{};
};

struct SeamPoint {
    std::uint32_t x{}, y{};
    std::uint32_t block_column{}, block_row{};
    bool content{};
    PointStatus status{PointStatus::outside_coverage};
    PointKind kind{};
    Point normal{};        // unit normal of the lines (one-directional)
    Point displacement{};  // where B shows A's patch, minus the point (output px)
    double visible_px{};   // |d|, or |n.d| for lines in one direction
    double uncertainty_px{};
    double round_trip_px{};
    double correlation{};
    std::uint32_t search_radius_px{};
    std::vector<Remeasurement> remeasurements; // only for points over the line
    Confirmation confirmation{Confirmation::not_required};
    // Valid, and either under the line or confirmed by remeasurement.
    bool Counted() const { return status == PointStatus::valid && confirmation != Confirmation::unconfirmed; }
};

enum class BlockClass { outside, empty, measured, unmeasurable_content };
std::string_view BlockClassCode(BlockClass value);

struct SeamBlock {
    std::uint32_t column{}, row{};
    std::uint32_t x{}, y{}, width{}, height{};
    BlockClass block_class{BlockClass::outside};
    std::uint32_t points{}, content_points{}, valid_points{}, counted_points{};
    std::uint32_t confirmed_points{}, unconfirmed_points{};
    std::optional<double> maximum_px;      // over counted points
    std::optional<double> uncertainty_px;  // largest u of the counted points
    bool neighbor_inconsistent{};
    std::optional<double> predicted_px;    // empty block: |prediction| + mismatch
};

struct SeamMeasurement {
    SeamGateDesign design{};
    SeamGateLimits limits;
    std::uint32_t width{}, height{};
    std::uint32_t origin_x{}, origin_y{}; // block grid origin, output px
    std::uint32_t block_columns{}, block_rows{};
    std::vector<SeamPoint> points;
    std::vector<SeamBlock> blocks; // row-major
    bool prediction_supplied{};
    std::optional<double> prediction_mismatch_px; // largest |measured - predicted| over counted points
};

// Measures every lattice point of the band. Points whose visible value plus
// the guard band exceeds the limit are measured again three ways and count
// only when at least two variants agree. No point is dropped for its value.
SeamMeasurement MeasureSeam(const BgrImage& camera_a, const BgrImage& camera_b, const SeamGateGeometry& geometry,
    const SeamGateDesign& design, const SeamGateLimits& limits, const DisplacementPrediction& prediction,
    unsigned thread_count);

enum class SeamGateState { pass, fail, undetermined };
std::string_view SeamGateStateCode(SeamGateState state);

struct SeamGateFinding {
    std::string reason; // seam-misalignment, predicted-misalignment, unmeasurable-content, no-measurable-block, unpredicted-empty-block, ...
    std::string remedy; // instruction code
};
struct SeamGateLocation {
    std::uint32_t x{}, y{}, block_column{}, block_row{};
};
struct SeamGateResult {
    SeamGateState state{SeamGateState::undetermined};
    std::vector<SeamGateFinding> findings; // first is the primary reason
    SeamGateLimits limits;
    std::optional<double> maximum_px;   // gate statistic: measured and predicted
    std::optional<SeamGateLocation> maximum_at;
    bool maximum_is_prediction{};
    std::optional<double> measured_maximum_px, second_px, p95_px, mean_px, predicted_maximum_px;
    std::uint32_t counted_points{}, confirmed_points{}, unconfirmed_points{};
    std::uint32_t measured_blocks{}, empty_blocks{}, unmeasurable_blocks{}, unpredicted_empty_blocks{};
};

// pass when max + guard <= limit and nothing else fails (equal passes).
SeamGateResult DecideSeamGate(const SeamMeasurement& measurement, const SeamGateLimits& limits);
// For a path that could not measure (missing inputs, geometry or policy).
SeamGateResult UndeterminedSeamGate(std::string reason, const SeamGateLimits& limits);

inline constexpr std::string_view kSeamGateReportSchema = "a0.seam-gate.v1";
// Self-contained JSON object for the still review report and the evaluation
// report. measurement may be null for an undetermined result.
std::string SeamGateReportJson(const SeamGateResult& result, const SeamMeasurement* measurement);

} // namespace a0::m2::seam_gate
