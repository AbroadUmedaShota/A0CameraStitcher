#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/experimental/registration_pose.hpp"
#include "a0/m2/seam_gate.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::experimental::content_registration {
// Per-capture registration of camera B by the content of the overlap (#417, docs/design/fixed-composite-registration.md).
// Camera, file and clock free; no random choice. The same inputs give the same result for any thread count. Inputs
// are not modified. Programming errors (sizes, unset limits, invalid constants) throw std::invalid_argument.
//
// What is estimated: the six-degree pose of camera B (registration_pose.hpp) against its calibrated pose. The lens
// and camera A's mapping are the calibration's and never change. A is the reference; B's mapping that follows the
// estimated pose is fixed_composite_maps.hpp's PoseCorrectedDocumentOutputToCamera.
//
// Four stages:
//   1 coarse   A and B are placed on a coarse grid by the calibrated mappings; edge-energy features of column
//              blocks of the window W are matched over the whole range the envelope can move the band (times a
//              margin) by a masked zero-mean correlation. A block counts when its best peak is strict and no second
//              peak is close (a repeating pattern is not unique). The unique blocks give the starting pose.
//              Surface mode: a block whose edge energy repeats is compared again at each of its strong peaks by the
//              colour of its cells (what the repeating lines enclose: text, filled cells, the edge of the paper). When
//              one peak agrees and every other strong peak agrees clearly less, the block is `resolved` and counts
//              like a unique block. A block that the colour cannot tell apart stays `repeating`: it never decides a
//              hypothesis, it only follows one that a unique or a resolved block decided.
//   2 fine     The seam gate's point measurer (seam_gate::MeasureSeam) is run on a sparse lattice of W with B placed
//              by the current pose, so the displacement left is small.
//   3 fit      Weighted Gauss-Newton (Levenberg-Marquardt) with a Tukey reweighting on the points; the scale is the
//              median absolute residual. A weak Gaussian prior on the pose (a multiple of the envelope) pulls the
//              directions the band cannot see towards zero. Stages 2 and 3 are repeated with the new pose.
//              Surface mode: the paper is one plane only as far as the next fold or gutter, so the fit does not take
//              all of W at once. It starts from the columns next to Omega + guard (the core) and adds the next
//              columns outward, one side at a time, while they agree with the plane of the columns already taken:
//              a group of columns is refused, and that side ends there, when more than a quarter of its points are
//              outliers of the current fit, or when taking it would bias the residual of the core. What lies beyond the first refusal
//              on a side is never used (RegistrationDiagnostics::fit_window). The window is found on the first two
//              measurements and kept after that; a kept window is all that is measured, and the second start of V6
//              is fitted over the first's window. The window never reaches farther than window_reach_px from
//              Omega + guard: the plane is the paper next to the seam, and the measurer's work grows with the area.
//              The coarse stage still uses all of W.
//   4 check    V1 Brown and Lowe, V2 unique blocks, V3 prediction uncertainty along the seam column, V4 envelope,
//              V5 shape of the residual, V6 convergence from two starts.
//              Surface mode: V5 judges the residual across the baseline only. A paper that is not flat (a lift, a
//              wave) leaves a residual along the baseline and none across it (design 11.2); that part is the paper's own
//              shape, which layer 2 corrects and the gate measures, and it says nothing against B's pose or the plane.
//              It is reported (RegistrationDiagnostics::residual_along, residual_along_exceeded) and does not make the
//              estimate undetermined (design 11.7: an estimate is trusted on V1 to V3 and V6).
//              Surface mode: V2 counts the anchors (unique or resolved blocks) that agree with the fit. A repeating block
//              that follows the vote's winner is used for the start of the fit only: it has a strong peak next to almost
//              any place, so one anchor with any number of followers is not a verified estimate (reason unique-blocks).
//
// W is not the gate's range Omega: Omega is the seam column +- (half of the high-frequency mixing width + a margin)
// and is where the composite can show a seam. W is the overlap band of both coverages without Omega +- a guard, so a
// point used to estimate is never one the gate judges, and the closer to Omega a point is, the more it weighs.
//
// The paper surface (design 11.3, layer 1). When the settings carry a surface envelope, the paper is a plane
// z = h0 + g_x (x - x0) + g_y (y - y0) above the calibration plane (table mm, height toward the cameras, (x0, y0) the
// middle of the seam column), and B's document point is G(p) Pi(pi) X: Pi is the table-to-table
// homography from A's table point X, through the paper, to the table point that the moved camera B sees. Then eight
// components are estimated: B's rotation (3), its translation across the baseline, its height, and the surface
// (h0, g_x, g_y). The translation along the baseline makes the same field as h0 and is held at the base pose (the
// gauge); a uniform shift along the baseline is carried by h0. Without a surface envelope the estimate is the
// six-degree pose, byte for byte the same as before the surface was added.
//
// The base pose p_b (design addendum 11.19): the start of the estimate, the centre of the weak prior and the gauge
// value. The envelope is applied to p-hat - p_b. p_b = 0 is the calibrated pose.

// The range the pose may take for the estimate to be applied. NaN until set, so an unset range is rejected, not
// read. There is no default in code (the owner's values come from the caller's policy).
struct RegistrationEnvelope {
    double position_mm = std::numeric_limits<double>::quiet_NaN();     // |(tx, ty)|
    double rotation_deg = std::numeric_limits<double>::quiet_NaN();    // |rz|, about the optical axis
    double tilt_deg = std::numeric_limits<double>::quiet_NaN();        // |(rx, ry)|
    double height_percent = std::numeric_limits<double>::quiet_NaN();  // |tz|
};

// The range the paper surface may take (condition v3 registration.surfaceEnvelope) and the paper plane
// pi = (h0, g_x, g_y) are the values of registration_mapping.hpp. The envelope sets the coarse search radius and the
// surface checks; h0 is in mm at the plane origin, the slopes in mm per mm, all zero is the calibration plane (the
// mapping is then the pose's exactly). The plane origin is the middle of the seam column: the output sample
// (seam_column, (height_pixels - 1) / 2) (the convention of S1's SurfaceOriginMm).
using SurfaceEnvelope = ::a0::m2::experimental::SurfaceEnvelope;
using SurfacePlane = ::a0::m2::experimental::SurfacePlane;
using SurfaceState = ::a0::m2::experimental::SurfaceState;
using SurfaceEstimate = ::a0::m2::experimental::SurfaceEstimate;
using OutputFrameDisplay = ::a0::m2::experimental::OutputFrameDisplay;

// Acceptance values of the checks. None has a default. Units are output pixels unless noted.
struct RegistrationLimits {
    double brown_lowe_alpha = std::numeric_limits<double>::quiet_NaN();   // V1: inliers > alpha + beta * points
    double brown_lowe_beta = std::numeric_limits<double>::quiet_NaN();
    std::uint32_t minimum_unique_blocks = 0;                              // V2
    double maximum_prediction_sigma_px = std::numeric_limits<double>::quiet_NaN();   // V3: along the seam column, all rows
    double maximum_residual_rms_px = std::numeric_limits<double>::quiet_NaN();       // V5: inliers
    double maximum_residual_p95_px = std::numeric_limits<double>::quiet_NaN();
    double maximum_bin_bias_px = std::numeric_limits<double>::quiet_NaN();           // V5: mean residual of a bin of inliers
    // The outer loop stops when the pose displacement along the seam column changes by less. Surface mode: or by less
    // than the estimate's own one-sigma prediction uncertainty there (the quantity of V3, which V3 bounds): on paper
    // that is not flat every new measurement moves the fit by a fraction of that uncertainty, however long the loop runs.
    double convergence_px = std::numeric_limits<double>::quiet_NaN();
    double start_agreement_px = std::numeric_limits<double>::quiet_NaN();  // V6: two starts must end within this displacement of each other
};

// Where W, Omega and the seam are. Output pixels; the seam column is the condition's.
struct RegistrationGeometry {
    std::uint32_t seam_column{};
    std::uint32_t mixing_width_px{};     // the high-frequency mixing width of the blend; Omega's half is half of it, rounded up
    std::uint32_t omega_margin_px{};     // m: added to the half width
    std::uint32_t guard_px{};            // g: W starts this far from Omega
    std::uint32_t band_half_width_px{};  // the band is the seam column +- this, inside both coverages
    std::uint32_t omega_half_width_px() const { return (mixing_width_px+1)/2+omega_margin_px; }
    std::uint32_t exclusion_half_width_px() const { return omega_half_width_px()+guard_px; }
};

// Design constants (not acceptance values). Every field is required.
struct RegistrationDesign {
    // Stage 1.
    std::uint32_t coarse_step_px;            // grid spacing of the coarse features (a multiple of 4: the cell holds 4x4 samples)
    std::uint32_t coarse_block_rows;         // rows of a block
    double coarse_radius_scale;              // search radius = (largest displacement the envelope gives) x this + margin
    std::uint32_t coarse_radius_margin_px;
    std::uint32_t coarse_maximum_radius_px;  // cap of the radius
    double coarse_content_level;             // cell edge energy (DN per 2 px) that counts as ink
    std::uint32_t coarse_minimum_cells;      // content cells a block needs
    double coarse_minimum_overlap;           // share of the content cells that must be covered by B at an offset
    double coarse_minimum_correlation;       // best peak
    double coarse_secondary_ratio;           // second peak / best at or above: not unique (repeating)
    double coarse_consistency_px;            // unique blocks must agree with the pose they give to within this
    double coarse_sigma_px;                  // uncertainty of a block's displacement
    // Stage 2 (the gate's measurer on a sparse lattice of W).
    std::uint32_t fine_point_spacing_px;
    std::uint32_t fine_patch_px;
    std::uint32_t fine_search_radius_px;
    std::uint32_t fine_maximum_search_radius_px;
    std::uint32_t fine_global_search_radius_px;   // the measurer's wide check; its result is reported, not used
    // Stage 3.
    std::uint32_t maximum_outer_iterations;  // measure and fit again at the new pose
    std::uint32_t maximum_inner_iterations;
    std::uint32_t maximum_reweighting_iterations;
    double tukey_constant;
    double scale_floor_px;                   // the robust scale never goes below this
    double sigma_floor_px;                   // a point's uncertainty never counts as smaller than this
    double prior_sigma_factor;               // prior sigma of each component = factor x its envelope
    double proximity_length_px;              // uncertainty grows as sqrt(1 + distance from Omega + guard / this)
    std::uint32_t minimum_points;            // counted points a fit needs
    // Stage 4.
    std::uint32_t bin_minimum_points;        // a bin of inliers smaller than this is not checked for bias
    // The paper surface (used only with a surface envelope).
    std::uint32_t coarse_vote_peaks;         // K: strong peaks of a block that are reported (CoarseBlock::peaks), best first
    double surface_prior_sigma_factor;       // prior sigma of h0 and of each slope = factor x the surface envelope
    // Repeating content (surface mode).
    std::uint32_t coarse_alias_peaks;        // strong peaks of a block that are compared by colour and that may follow a hypothesis (at most)
    double coarse_alias_minimum_score;       // colour agreement (pooled zero-mean correlation of the cell colours) the chosen peak needs
    double coarse_alias_margin;              // and by how much it must exceed the agreement at every other strong peak
    // The fit window (surface mode).
    std::uint32_t window_core_px;            // the columns within this distance of Omega + guard are always fitted (the core)
    std::uint32_t window_group_points;       // counted points a group of columns needs to be judged
    double window_core_bias_px;              // a group is refused when taking it leaves a mean residual above this in a bin of the core
    double surface_pose_prior_sigma_factor;  // prior sigma of B's components in the surface mode = factor x the envelope
    std::uint32_t window_reach_px;           // the fine stage measures and fits no farther than this from Omega + guard
    // The largest radius the measurer's window widens to in the second start of V6 (capture only; a grid keeps its measurer's), within
    // fine_search_radius_px..fine_maximum_search_radius_px. That start is the calibrated pose, which the coarse stage did not place, so
    // its displacement is the whole of what the pose changes; V6 does not hold the estimate against a start that cannot see it.
    std::uint32_t second_start_search_radius_px;
};
// Design values of the first implementation (*proposal*, not approved values). See the design document.
// The lattice of the fine stage is one patch apart (the patches do not share pixels): on paper that is not flat the
// points scatter along the baseline, and with a wider lattice the few points of a narrow fit window let the fit move
// with every new measurement. The outer loop may take five measurements: a surface that slopes across the band is not
// seen by the coarse stage (its blocks span the band), and the first fits then start several pixels off at the seam.
//
// The first search radius is what the start can be off by, not what the pose can move the band by. The coarse stage places B
// to within its consistency (4 px), so a radius of twice that finds the match; a point that is farther off hits the edge of the
// window and the measurer widens it (doubling, up to the maximum). The measurer judges a patch periodic over twice the search
// radius around it, so the radius is also how far a repeating pattern (a ruler, a dotted rule, the rows of a table) can be before
// it is no longer taken for a place that cannot be told. A radius of 24 judged every patch of such a sheet periodic that has a
// repeat within 48 px; at 8 only the ones that repeat within 16 px are, and the patches whose repeat lies farther are matched
// over the window the start can be off in.
inline constexpr RegistrationDesign kRegistrationDesignDraft{
    8, 512, 2.0, 16, 1024, 4.0, 40, 0.5, 0.3, 0.8, 16.0, 4.0,
    64, 64, 8, 48, 52,
    5, 25, 40, 4.685, 0.25, 0.1, 4.0, 400.0, 12,
    3,
    4, 4.0,
    512, 0.5, 0.1,
    96, 8, 0.25, 1.0, 512,
    24};

struct RegistrationSettings {
    RegistrationGeometry geometry;
    RegistrationEnvelope envelope;
    RegistrationLimits limits;
    RegistrationDesign design{};
    // Layer 1 (design 11.3). Absent: the surface is not estimated (six components, the result of the pose-only
    // estimator byte for byte). Present: eight components.
    std::optional<SurfaceEnvelope> surface_envelope;
    // p_b (addendum 11.19): start, prior centre, gauge; the envelope is applied to the difference from it. Finite.
    PoseParameters base_pose{};
};
// Throws std::invalid_argument for an unset or inconsistent value.
void ValidateRegistrationSettings(const RegistrationSettings& settings);

enum class RegistrationState {
    verified,         // estimated, every check passed, inside the envelope: the pose may be applied
    undetermined,     // not estimated or not trusted: compose at the calibrated pose (p = 0) and leave it to the gate
    out_of_envelope,  // estimated and trusted, but a component is outside the range: do not apply
};
std::string_view RegistrationStateCode(RegistrationState state);

// position, rotation, tilt and height are of p-hat - p_b (RegistrationResult::components, report v8
// registration.components); surface_height (|h0|, mm) and surface_tilt (degrees) are the surface checks
// (RegistrationResult::surface_components, report v8 surface.exceeded).
enum class EnvelopeComponent { position, rotation, tilt, height, surface_height, surface_tilt };
std::string_view EnvelopeComponentCode(EnvelopeComponent component);
struct ComponentCheck {
    EnvelopeComponent component{};
    double value{};      // mm, degrees, degrees, percent, mm, degrees (magnitudes)
    double limit{};
    bool exceeded{};
};

// The estimated pose as a person reads it (in place of a level, #417): sizes in mm, degrees and percent.
struct PoseDisplay {
    double position_mm{};    // |(tx, ty)|
    double tx_mm{}, ty_mm{};
    double rotation_deg{};   // rz, signed, about the optical axis
    double tilt_deg{};       // |(rx, ry)|
    double rx_deg{}, ry_deg{};
    double height_percent{}; // tz, signed: positive means B is farther from the paper (higher) than at calibration
    double height_mm{};      // height_percent of the pivot depth
    // The same in the drawing's own frame (design addendum 11.24; report v8 registration.display.outputFrame). Filled by
    // the estimator for p-hat - p_b; DisplayOf leaves it empty. See OutputFrameOf.
    std::optional<OutputFrameDisplay> output_frame;
};
PoseDisplay DisplayOf(const content_registration::PoseParameters& pose, const PoseModel& model);

// The pose difference p - p_b and the surface in the drawing's frame (output frame: x right, y down, mm and degrees;
// addendum 11.24). Every direction is found by moving the model, not by a fixed sign:
//   camera_motion_mm        where B's centre went: minus the movement of the drawing at B's view centre under the
//                           translation (tx, ty) alone
//   lens_direction, lens_tilt_deg  where B's lens turned (unit vector, 0 when the tilt is 0), and |(rx, ry)|. The tilt
//                           turns about the pivot, so the view centre stays put (the addendum's "movement of the view
//                           centre" is 0); the lens leans opposite to the shift of B's centre under (rx, ry) alone
//   rotation_clockwise_deg  B's turn as seen on the screen (from x toward y), minus the turn of the drawing under rz alone
//   surface_high_side       unit (g_x, g_y), 0 for a level plane
//   baseline_equivalent_motion_mm  the motion of B that makes the same shift at the plane origin as h0 (the second
//                           reading of surface-height): minus the movement of the origin under Pi(h0, 0, 0)
// The mounting (rotated_180) is inside the calibrated projection, so it is not applied again.
[[nodiscard]] OutputFrameDisplay OutputFrameOf(const render::CameraProjection& camera_a, const render::CameraProjection& camera_b,
    const render::OutputRaster& raster, std::uint32_t seam_column, const PoseParameters& base, const PoseParameters& pose,
    const SurfacePlane& surface);

// How a camera faces the drawing (the owner's request: a guide to set the cameras before shooting). From the camera's
// projection (calibrated, or B moved by an estimate): its centre over the table, the tilt of its optical axis from the
// table's normal and the direction over the drawing that the lens faces, and the turn of its image x axis against the
// drawing's x (from x toward y, clockwise on the screen; wrapped to (-180, 180], the 180-degree mounting included).
struct CameraOrientation {
    render::Point centre_mm{};
    double height_mm{};
    double tilt_deg{};
    render::Point lens_direction{};     // unit, drawing frame; 0 when the axis is vertical
    double rotation_clockwise_deg{};
};
[[nodiscard]] CameraOrientation OrientationOf(const render::CameraProjection& camera);
// B against A: B's turn less A's (wrapped to (-180, 180]), and the angle between the two optical axes with the direction
// over the drawing that B's axis leans to from A's (unit, 0 when parallel).
struct RelativeOrientation {
    double rotation_clockwise_deg{};
    double tilt_deg{};
    render::Point tilt_direction{};
};
[[nodiscard]] RelativeOrientation RelativeOrientationOf(const CameraOrientation& a, const CameraOrientation& b);
struct OrientationDisplay {
    CameraOrientation camera_a;   // A at calibration
    CameraOrientation camera_b;   // B as estimated (B at calibration when there is no estimate)
    RelativeOrientation b_from_a;
};

enum class CoarseBlockStatus {
    too_few_cells,       // not enough ink in the block
    low_overlap,         // B covers too little of the ink at every offset
    flat,                // no usable peak
    low_correlation,
    search_boundary,     // the best peak is on the edge of the search window
    repeating,           // a second peak close to the best
    unique,
    resolved,            // surface mode: the edge energy repeats, the colour of the cells picks one strong peak
};
std::string_view CoarseBlockStatusCode(CoarseBlockStatus status);
// A strong peak of a block's correlation surface (surface mode): the best peak, or another local maximum within the
// secondary ratio of it.
struct CoarsePeak {
    render::Point displacement{};
    double correlation{};
    double colour_score{};         // the colour agreement at the peak (compared peaks of a repeating block; 0 otherwise)
};
struct CoarseBlock {
    std::uint32_t y0{}, rows{};
    CoarseBlockStatus status{CoarseBlockStatus::too_few_cells};
    render::Point at{};            // centroid of the ink cells (output px index)
    render::Point displacement{};  // where B shows A's ink, minus the position (unique blocks; voted blocks: the voted peak)
    double correlation{};
    bool consistent{};             // unique and agreeing with the pose they give
    std::vector<CoarsePeak> peaks; // surface mode only (empty otherwise): the first K strong peaks; a resolved block's one peak
    bool voted{};                  // surface mode: one of its strong peaks agrees with the winning hypothesis
    // Surface mode, a block whose edge energy repeats: how many strong peaks were compared by colour, the best colour
    // agreement and its lead over the next strong peak (resolved when the lead and the agreement reach the design's values).
    std::uint32_t compared_peaks{};
    double colour_score{}, colour_margin{};
};

// The vote of the coarse stage (surface mode, design 11.3 step 1). An anchor is a block with one place: unique, or
// resolved by colour. A hypothesis is a displacement that is affine in the row, d(y) = c + g (y - y_ref), made from the
// peak of one anchor (g = 0) or of two anchors (|g| within the largest change along the rows that B's and the surface's
// ranges can give, times the radius scale, plus a margin). A block agrees with it when one of its strong peaks is within
// coarse_consistency_px of it; the counts are less one for a hypothesis made from two peaks (their two blocks always
// agree with it, which is no evidence). Only anchors are evidence: a repeating block has a strong peak near almost any
// hypothesis, so it never makes or decides one and only follows the winner. The winner has the most anchors, then the
// most blocks, then the larger summed correlation, then comes first (candidates are taken in block order).
struct CoarseVote {
    bool performed{};
    std::uint32_t candidates{};       // reported peaks over all blocks
    std::uint32_t hypotheses{};
    double gradient_limit{};          // output px per row
    std::uint32_t support{};          // blocks that agree with the winner, as above
    std::uint32_t rival_support{};    // the same for the best hypothesis that differs from the winner by more than the tolerance at a block
    std::uint32_t anchor_support{};   // anchors that agree with the winner, as above
    // The best hypothesis that an anchor outside the winner's anchors agrees with: another explanation of the evidence.
    std::uint32_t rival_anchor_support{};
    std::uint32_t unique_supporters{};// voted blocks that are anchors
    bool decided{};                   // anchor_support > rival_anchor_support
    render::Point offset{};           // c of the winner (output px)
    render::Point gradient{};         // g of the winner (output px per row)
    double reference_row{};
};

// The uniform shift of the coarse stage (design addendum 11.18, the G3 candidate): the base pose with only the
// translation across the baseline and (surface mode) h0 changed, fitted to the voted or unique blocks. Pose-only mode:
// the two translations. `determined` when the blocks that agree number at least minimum_unique_blocks and all of them
// agree within coarse_consistency_px.
struct CoarseShift {
    bool determined{};
    content_registration::PoseParameters pose;
    SurfacePlane surface;
    std::uint32_t blocks{};
    double maximum_miss_px{};
    // The two values of capture_registration's UniformShift (surface mode): h0 and the translation across the baseline
    // (mm along diagnostics.gauge_direction). Pose-only mode: both 0, and `pose` carries the two translations.
    double surface_height_mm{};
    double orthogonal_translation_mm{};
};

// disabled: no surface envelope (layer 1 is not estimated); not_estimated: surface mode, but no fit ran; estimated: h0,
// g_x, g_y and their covariance are meaningful (whatever the registration state).
std::string_view SurfaceStateCode(SurfaceState state);

// The frame the surface is expressed in, and the baseline quantities of design 11.2 (B is the estimated, moved B).
struct SurfaceFrame {
    render::Point reference_mm{};      // (x0, y0), table mm
    render::Point reference_px{};      // the same as an output sample index
    render::Point camera_a_mm{}, camera_b_mm{};   // camera centres on the table
    double camera_a_height_mm{}, camera_b_height_mm{};
    render::Point baseline{};          // e_b, unit, output frame (x right, y down)
    double baseline_length_mm{};
    double parallax_px_per_mm{};       // k = rho |b| / (mean camera height - h0)
};
// The surface as a person reads it (design 11.3 and 11.7). The scale is the factor A's image of the paper grows by,
// H_A / (H_A - h), at the plane origin and its range over the output raster (reported only, never applied).
struct SurfaceDisplay {
    double height_mm{};                // h0
    double baseline_equivalent_mm{};   // u = h0 |b| / (H - h0): the same field as B moved along the baseline by u
    double tilt_deg{};                 // atan |(g_x, g_y)|
    double tilt_x_deg{}, tilt_y_deg{}; // atan g_x, atan g_y (signed)
    double scale_factor{1}, scale_minimum{1}, scale_maximum{1};
};
struct SurfaceResult {
    SurfaceState state{SurfaceState::disabled};
    SurfacePlane plane;                // pi-hat
    SurfacePlane sigma;                // one sigma of each component
    std::array<double, 9> covariance{};   // row-major, order h0 g_x g_y
    SurfaceFrame frame;
    SurfaceDisplay display;
    // The report's shape (registration_mapping.hpp, report v8 `surface`): the same values, with the exceeded flags of the
    // surface envelope. `applied` stays false here; the capture path sets it when the plane goes into the mapping.
    SurfaceEstimate estimate;
};

struct ResidualSummary {
    std::uint32_t count{};
    double mean{}, rms{}, p95{}, maximum{};
};

// One counted point of the last fit (development reports).
struct ObservationReport {
    double x{}, y{};
    bool one_directional{};
    render::Point normal{};
    render::Point displacement{};   // measured with B placed by the pose before the last fit
    double sigma_px{};              // as used by the fit (floor and proximity factor included)
    double residual_px{};           // against the fitted pose
    double weight{};                // Tukey weight, 0 for an outlier
};

// The columns of W that the last fit used (surface mode; `grown` false otherwise). Distances are from the seam column,
// output px. `*_available_px` is the farthest counted point of the side; `*_px` the farthest one the fit took; `*_cut`
// says that the side ended before its last column because the next columns did not agree with the plane of the ones
// taken (a fold, a gutter, the edge of the sheet).
struct FitWindow {
    bool grown{};
    double left_px{}, right_px{};
    double left_available_px{}, right_available_px{};
    bool left_cut{}, right_cut{};
    std::uint32_t core_points{};      // counted points of the core
    std::uint32_t refused_points{};   // counted points beyond the cuts
};

// Measured quantities behind the checks.
struct RegistrationDiagnostics {
    // Stage 1.
    double coarse_radius_x_px{}, coarse_radius_y_px{};
    std::vector<CoarseBlock> coarse_blocks;
    std::uint32_t unique_blocks{}, consistent_blocks{};
    // The anchors among them: the blocks with one place (unique, or resolved by colour). In surface mode unique_blocks and
    // consistent_blocks also count the repeating blocks that follow the vote's winner, which help the start of the fit but
    // are no evidence of the place; V2 is held against the anchors too. In pose-only mode every counted block is an anchor.
    std::uint32_t anchor_blocks{}, consistent_anchor_blocks{};
    std::optional<content_registration::PoseParameters> coarse_pose;
    // Stages 2 and 3, from the start that the result comes from.
    std::uint32_t outer_iterations{};
    std::uint32_t points{};         // counted points of the last fit (surface mode: inside the fit window)
    FitWindow fit_window;
    std::uint32_t inliers{};
    std::uint32_t gate_points{};    // points the measurer was asked about, over the last iteration
    std::uint32_t global_contradicted{};  // anchors of the measurer's wide check that disagree (reported only)
    double robust_scale_px{};
    ResidualSummary residual;       // inliers within the proximity length of Omega + guard (V5)
    // Surface mode: the same inliers split by the baseline. Two-directional points give both components; a
    // one-directional point counts across when its normal is mostly across the baseline, else along. V5 judges
    // residual_across and maximum_bin_bias_across_px; the along values are the paper's own shape (reported).
    ResidualSummary residual_across, residual_along;
    double maximum_bin_bias_across_px{}, maximum_bin_bias_along_px{};
    bool residual_along_exceeded{};   // the along values are above V5's limits (not a reason: left to layer 2 and the gate)
    std::vector<ObservationReport> observations;
    double maximum_bin_bias_px{};
    // Stage 4.
    double brown_lowe_threshold{};  // alpha + beta * points
    double maximum_prediction_sigma_px{};   // along the seam column, covered rows
    std::uint32_t seam_rows_checked{};
    bool converged{};
    double last_change_px{};        // change of the pose's displacement along the seam column in the last outer iteration
    std::vector<double> change_history_px;   // the same for every outer iteration
    enum class SecondStart { not_run, same_start, agreed, disagreed, out_of_reach } second_start{SecondStart::not_run};
    double second_start_difference_px{};
    // Surface mode.
    CoarseVote coarse_vote;
    CoarseShift coarse_shift;          // also in pose-only mode
    render::Point gauge_direction{};   // surface mode: unit (tx, ty) in mm of the free translation (across the baseline)
    // Conditioning of the last fit (both modes): the condition number of the information matrix (prior included)
    // after scaling it to a unit diagonal, and the largest |correlation| between a surface component and B's rotation
    // or height (surface mode; 0 otherwise).
    double information_condition_number{};
    double maximum_surface_pose_correlation{};
    // The last fine measurement, for layer 2 (S4) to reuse: B was placed by last_placement (document homography;
    // identity when last_placement_calibrated), and these are the measurer's points of both sides of W in measuring
    // order (left side first), counted or not. Empty when no fine measurement ran. Surface mode: once the fit window
    // is kept, only its columns are measured, so these are the points of the window.
    render::Matrix3 last_placement{1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool last_placement_calibrated{true};
    std::vector<seam_gate::SeamPoint> last_points;
};
std::string_view SecondStartCode(RegistrationDiagnostics::SecondStart value);

struct RegistrationResult {
    RegistrationState state{RegistrationState::undetermined};
    std::vector<std::string> reasons;          // undetermined: the first is the primary reason (codes below)
    std::vector<ComponentCheck> components;    // all four pose components, when a pose was estimated
    std::vector<ComponentCheck> surface_components;   // surface_height and surface_tilt, when the surface was estimated
    std::vector<EnvelopeComponent> exceeded;   // out_of_envelope (and verified: empty): which components (pose or surface) are outside
    bool has_estimate{};                       // pose and covariance are meaningful (also for undetermined when a fit ran)
    content_registration::PoseParameters pose;       // p-hat
    content_registration::PoseParameters pose_sigma; // one sigma of each component
    std::array<double, 36> covariance{};       // row-major, order rx ry rz tx ty tz (degrees, mm, percent)
    PoseDisplay display;                       // of p-hat - p_b, with output_frame when a pose was estimated
    SurfaceResult surface;
    OrientationDisplay orientation;            // A at calibration, B as estimated, B against A (always filled)
    // Surface mode: the joint covariance of the eight estimated components, row-major, order rx ry rz s tz h0 g_x g_y
    // (s: mm along diagnostics.gauge_direction). Empty in pose-only mode.
    std::vector<double> joint_covariance;
    // The document homography that places B by the estimate: G(p-hat) Pi(pi-hat), or G(p-hat) when pi-hat is zero or
    // the surface is disabled (registration_pose's RegisteredHomography with the plane origin SurfaceOriginMm). Meaningful
    // when has_estimate. The composite builds its mapping with fixed_composite_maps from the pose and the plane.
    render::Matrix3 document_homography{1, 0, 0, 0, 1, 0, 0, 0, 1};
    RegistrationDiagnostics diagnostics;
};
// Codes in RegistrationResult::reasons:
//   no-overlap, coarse-no-unique-blocks, coarse-inconsistent, too-few-points, fit-failed, brown-lowe (V1), unique-blocks (V2),
//   prediction-uncertain (V3), residual-shape (V5), not-converged, starts-disagree (V6),
//   repeating-only (surface mode: every block with content repeats and the colour of its cells does not tell its peaks
//                   apart; there is no anchor and no estimate),
//   coarse-ambiguous (surface mode: the anchors do not agree on one hypothesis),
//   time-budget (the caller's stop was asked for; the coarse stage's result is kept)

// The caller's control (design addendum 11.25). The estimator has no clock: `stop`, when set, is asked before each call
// of the measurer and between outer iterations; a true answer ends the fine stage and the fit, and the result is
// undetermined with the reason time-budget (the coarse diagnostics stay). It may be called from more than one thread.
struct RegistrationControl {
    std::function<bool()> stop;
};

// Estimates B's pose (and, with a surface envelope, the paper plane) from the two original images, with the
// calibrated mappings of `parameters` (the condition's raster, A left and B right). The mappings are the document
// render's (render::DocumentOutputToCamera). thread_count is for the measurer only (1..256); the result does not depend
// on it. Throws std::invalid_argument for a programming error.
[[nodiscard]] RegistrationResult EstimateRegistration(const render::BgrImage& camera_a, const render::BgrImage& camera_b,
    const render::DocumentRenderParameters& parameters, const RegistrationSettings& settings, unsigned thread_count);
[[nodiscard]] RegistrationResult EstimateRegistration(const render::BgrImage& camera_a, const render::BgrImage& camera_b,
    const render::DocumentRenderParameters& parameters, const RegistrationSettings& settings, unsigned thread_count,
    const RegistrationControl& control);

// ---- The estimate on a grid of the caller's (#417 LC: the live C; design addendum 11.21) -------------------------------
//
// The same stages on a grid that is not the document raster of a capture: the live C draws on its own grid (the
// condition's region at the pitch of the reduced live frames), with mappings that end in the pixels of those frames. The
// caller passes the mappings as they are (calibrated projection -> pose -> surface -> raw -> live frame); nothing is
// estimated on the capture's raster and scaled down. The capture path (EstimateRegistration) is not changed by this.
//
// What differs from a capture:
//   - every pixel value of the settings (geometry, limits, design) is in the grid's own pixels;
//   - the measurer's constants are given whole (GridRegistrationSettings::measurer): a reduced live frame is far
//     coarser and noisier than a capture, and the capture's constants scaled by the reduction fall below the
//     measurer's lower bounds;
//   - nothing is estimated from a band that is too dark (minimum_band_brightness): a live view at night is noise;
//   - two jobs. `full` runs every stage. `follow` runs the fine stage and the fit once from a state the caller gives
//     (the estimate in force): no coarse stage, one outer iteration, no second start (V6); it is judged on V1 and V3
//     only, and reports how far the seam column's displacement moved from the start and the share of points kept;
//   - the caller's stop is asked more often (before the coarse stage, before each measurement and before each fit),
//     so that closing the window does not wait for a job.
struct RegistrationGrid {
    std::uint32_t width{}, height{};
    PlaneFrame frame;                    // sample index to document mm: (left + (x + 0.5) pitch, top + (y + 0.5) pitch)
    render::OutputToCamera camera_a;     // A on the grid; called with sample indices (whole numbers)
    // B on the grid for a pose and a surface (layer 1). Called once per placement. May throw std::invalid_argument for
    // a state that cannot be placed (a surface that is not below both cameras); the job then ends as fit-failed.
    std::function<render::OutputToCamera(const PoseParameters&, const SurfacePlane&)> camera_b;
    const PoseModel* model_b{};          // B's calibrated projection (the model of the mapping above)
    CameraCentre centre_a{};             // A's calibrated centre
    render::Point surface_origin_mm{};   // (x0, y0) of the surface: SurfaceOriginMm of the condition's raster and seam column
};
struct GridRegistrationSettings {
    RegistrationSettings estimator;      // grid pixels; the design's fine_* values must be the measurer's
    seam_gate::SeamGateDesign measurer{};
    // Mean luminance (0..255) of A's and of B's part of the band below which nothing is estimated (reason too-dark).
    // NaN until set; 0 turns the check off.
    double minimum_band_brightness = std::numeric_limits<double>::quiet_NaN();
};
enum class GridJob { full, follow };
struct GridRegistrationRequest {
    GridJob job{GridJob::full};
    PoseParameters start_pose;           // follow: the state the fine stage starts from
    SurfacePlane start_surface;
};
struct GridRegistrationResult {
    // full: as EstimateRegistration. follow: verified or out_of_envelope when V1 and V3 pass; no coarse diagnostics.
    // Further reasons: too-dark; time-budget when the caller's stop was asked for (then `stopped`).
    RegistrationResult registration;
    bool stopped{};
    // Mean luminance of each camera over the band W, B placed by the base pose. NaN where nothing is covered.
    double band_brightness_a = std::numeric_limits<double>::quiet_NaN();
    double band_brightness_b = std::numeric_limits<double>::quiet_NaN();
    // follow, when a fit ran: the largest displacement along the seam column between the start and the fit (grid px), and
    // inliers / counted points.
    double change_px = std::numeric_limits<double>::quiet_NaN();
    double inlier_fraction = std::numeric_limits<double>::quiet_NaN();
};
// Throws std::invalid_argument for a programming error: what ValidateRegistrationSettings and the measurer's validation
// refuse, a guard that does not clear half the measurer's patch (W and Omega must not share a pixel), an unset
// brightness line, an empty grid, a missing mapping or model, a start that is not finite. thread_count as above.
[[nodiscard]] GridRegistrationResult EstimateRegistrationOnGrid(const render::BgrImage& camera_a, const render::BgrImage& camera_b,
    const RegistrationGrid& grid, const GridRegistrationSettings& settings, const GridRegistrationRequest& request, unsigned thread_count,
    const RegistrationControl& control);

// Design values of the estimate on the live C's grid (*proposal*: chosen on synthetic frames of the live view's
// coarseness, not on a rig). One grid pixel is about a millimetre of paper, eleven or more capture pixels.
//   coarse: cells of 4 px, blocks of 48 rows, a peak of at least 0.5 (noise in a dark frame reaches 0.3 by chance)
//   fine:   patches of 16 px (the measurer's smallest) one patch apart, first search radius 8 px. A lattice of 8 px was
//           tried first: it took a follow-up about a second on a busy four-core machine, twice its interval, for an
//           error at the seam that was already a hundredth of a pixel
//   fit:    the same robust fit; the window's core is the first two columns of the measurer
struct GridRegistrationDesign {
    RegistrationDesign estimator;
    seam_gate::SeamGateDesign measurer;
    double minimum_band_brightness;
};
inline constexpr GridRegistrationDesign kRegistrationDesignLiveDraft{
    {4, 48, 2.0, 4, 96, 4.0, 40, 0.5, 0.5, 0.8, 4.0, 2.0,
     16, 16, 8, 16, 20,
     5, 25, 40, 4.685, 0.15, 0.05, 4.0, 40.0, 12,
     3,
     4, 4.0,
     512, 0.5, 0.1,
     32, 8, 0.15, 1.0, 96,
     8},
    // The measurer: the calibrated gate's constants with the sizes of the grid, and with the lines that a reduced,
    // noisy frame cannot meet set wider (round trip 0.25 px, point uncertainty 0.3 px). The second measurement of a point
    // over the gate's line is never made here (the registration passes no line), so its patch and shift are the smallest.
    {16, 16, 32, 8, 16, 7, 8.0, 24, 0.1, 0.8, 0.25, 0.01, 0.3, 0, 16, 3.0, 0.1, 0.5, 256, 20, 1, 1, 2.0},
    16.0};
// The geometry of a capture's condition on a grid whose pixel is 1 / scale capture pixels (addendum 11.21): Omega's
// margin and the guard rounded up, the band's half width rounded down. The guard is widened to clear half the
// measurer's patch when the scaled one does not (W and Omega stay apart). mixing_width_px is the grid's own blend width.
[[nodiscard]] RegistrationGeometry ScaleRegistrationGeometry(std::uint32_t grid_seam_column, std::uint32_t grid_mixing_width_px,
    std::uint32_t omega_margin_px, std::uint32_t guard_px, std::uint32_t band_half_width_px, double scale, std::uint32_t measurer_patch_px);

// The surface as a person reads it, for a plane in a frame (the frame's camera heights and baseline).
[[nodiscard]] SurfaceDisplay DisplayOfSurface(const SurfacePlane& plane, const SurfaceFrame& frame);

// Displacement (B minus A, output px) that a pose placed with `placed` would still show if B is really at `truth`: the
// feature that truly sits at (x, y) is shown at G_placed^-1 G_truth (x, y). Zero where placed == truth. Used to judge
// an estimate against a known pose.
[[nodiscard]] render::Point ResidualDisplacement(const PoseModel& model, const PlaneFrame& frame,
    const content_registration::PoseParameters& placed, const content_registration::PoseParameters& truth, double x, double y);
}
