#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/registration_gate.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/experimental/registration_pose.hpp"
#include "a0/m2/seam_gate.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::experimental::parallax_correction {
// Layer 2 of the paper-surface correction (#417, docs/design/fixed-composite-registration.md 11.4, 11.5, 11.8): along the
// seam column, the part of the remaining B-minus-A displacement that lies along the baseline, q(y), estimated at every
// gate row by a weighted local polynomial whose points never share pixels with the gate points that judge it: with the split
// (ParallaxRows::verify_period_rows), the gate rows' verify rows; without it, that row's own (the hole).
//
// Pure computation on points the caller has measured: no image, no measurer call, no camera, file or clock, no random
// choice. The points are measured with B placed by the pose and the paper plane (layer 1) and without layer 2, so their
// displacement is the residual r of 11.4; the caller measures them in one call of seam_gate::MeasureSeam on the lattice of
// the design (PointsFromSeamMeasurement turns the result into points). The same inputs give the same result for any
// thread count. Inputs are not modified. Programming errors and non-finite inputs throw std::invalid_argument.
//
// Sign: r is "where B shows A's feature, minus where A shows it" (the gate's d). A paper point raised by h moves B's
// image toward -e_b by k h (11.2), so the lift relative to the paper plane is -q / k.
//
// The result types are what the capture path, the report and the texts share (docs/design/
// fixed-composite-registration-formats.md, 4.8); fields are only added to them. The request is this lane's own.
//
// Reason codes of ParallaxResult::reasons (kebab-case; the first is the primary reason):
//   not-implemented       the fixed value of the report before this lane (formats 4.8); not returned here
//   disabled              the condition has no parallax line (registration.parallax is null)
//   pose-untrusted        the pose and surface are not trusted, so layer 2 is not tried (set by the caller)
//   time-budget           dropped by the time budget (set by the caller, 11.25)
//   baseline-unmeasured   no point gives the component along the baseline (lines across the baseline only)
//   too-few-points        no gate row has an estimate of its own
//   beyond-lift-line      a row's lift is over the line and the switch says not to apply beyond it
//   strain-limit          the strain (pi/2) max|q| / L would be above s_max
// and of ParallaxRow::reason (rows that are filled):
//   no-points             no point near the row
//   baseline-unmeasured   points near the row, but none gives the component along the baseline
//   too-few-points        n_eff below n_min
//   one-sided             the points sit on one side of the hole (the prediction would be an extrapolation)
//   uncertain             sigma_q above sigma_max
//   ill-conditioned       the weighted system is singular

// Design constants (11.4, 11.8). None of them is a pass/fail line and none has a default in code; every field is required
// (ValidateParallaxDesign refuses an unset one). The values in use are recorded in the result and the report.
//
// Two changes from the list of 11.4. q is computed at every gate row directly (the measurement lane M1), so there is no
// sample spacing Delta y_e of its own: the gate's row step is the spacing. And the hole is not a size: a point is left out
// of a gate row's estimate when the output pixels its measurement read (its footprint, registration_gate::GatePatchOf)
// share a pixel with those any gate point of the row may read (the gate's footprint over Omega's columns), widened by
// the margin iota; the gate lane found the formula g_x >= P + omega + iota too small once the remeasurement and the
// feature smoothing are counted. The mapping joins the rows' q between the rows, so a gate point also reads the q of the
// rows next to its own: the hole of a row covers every gate row that reads a q the row's estimate enters (HoleNeighbourRows),
// whatever the row step and the footprint are.
struct ParallaxDesign {
    // The points (the caller measures them on this lattice; the estimate itself takes each point's own patch).
    double column_spacing_px = std::numeric_limits<double>::quiet_NaN();      // Delta x, and the row spacing of the lattice
    double column_radius_px = std::numeric_limits<double>::quiet_NaN();       // R: points within |x - x_s| <= R
    std::uint32_t point_patch_px = 0;                                         // the measurer's patch side for these points
    // The regression at a gate row (x_s, y): weights K(dx / sigma_x, dy / sigma_y) S / sigma^2.
    std::uint32_t polynomial_degree = 0;                                      // d_q: 1 or 2, in (dx / sigma_x, dy / sigma_y)
    double kernel_sigma_x_px = std::numeric_limits<double>::quiet_NaN();
    double kernel_sigma_y_px = std::numeric_limits<double>::quiet_NaN();
    double kernel_reach_sigmas = std::numeric_limits<double>::quiet_NaN();    // points beyond the hole + this many sigma are not used
    double kernel_floor = std::numeric_limits<double>::quiet_NaN();           // a kernel value below this counts as 0
    double hole_margin_px = std::numeric_limits<double>::quiet_NaN();         // iota: pixels kept free between the footprints
    double hole_edge_px = std::numeric_limits<double>::quiet_NaN();           // delta: S rises from 0 at the hole to 1 over this
    std::uint32_t hole_neighbour_rows = 0;                                    // the hole also covers the footprints of at least this many
                                                                              // gate rows on each side (0: none beyond the join's)
    std::uint32_t hole_join_reach_rows = 0;                                   // r: the mapping joins the rows' q, and a row's sample enters
                                                                              // q(y) for |y - y_row| < r row steps; the hole also covers
                                                                              // every gate row that reads such a q (HoleNeighbourRows).
                                                                              // 2 for the mapping's join; 0 leaves the join out (it leaks)
    double ridge_weight = std::numeric_limits<double>::quiet_NaN();           // times the sum of the weights, on every coefficient but the constant
    double tukey_constant = std::numeric_limits<double>::quiet_NaN();
    double scale_floor_px = std::numeric_limits<double>::quiet_NaN();         // tau_min: the robust scale never goes below this
    std::uint32_t reweighting_iterations = 0;                                 // Tukey reweightings after the first solve
    double sigma_floor_px = std::numeric_limits<double>::quiet_NaN();         // a point's uncertainty never counts as smaller than this
    double minimum_baseline_component = std::numeric_limits<double>::quiet_NaN();   // c_min: |n . e_b| a one-directional point needs
    // A row's estimate is its own when all of these hold (otherwise the row is filled from its neighbours).
    double minimum_effective_points = std::numeric_limits<double>::quiet_NaN();     // n_min, on n_eff = (sum w)^2 / sum w^2
    double maximum_sigma_q_px = std::numeric_limits<double>::quiet_NaN();           // sigma_max
    double minimum_enclosure_share = std::numeric_limits<double>::quiet_NaN();      // the weight on every side of the hole centre
    double row_blend_px = std::numeric_limits<double>::quiet_NaN();                 // delta_y: rows without an estimate (FillUnmeasuredRows)
    // The weight w of the mapping (S1 builds it) and its strain.
    double weight_flat_px = std::numeric_limits<double>::quiet_NaN();         // a: w = 1 for |x - x_s| <= a (at least Omega's half width)
    double weight_falloff_px = std::numeric_limits<double>::quiet_NaN();      // L: w falls to 0 over the next L
    double maximum_strain = std::numeric_limits<double>::quiet_NaN();         // s_max, against (pi/2) max|q| / L
    // Fold and gutter lines (11.8): the leave-one-out leftover c_i = z_i - q_(-i) of every point (the same regression at the
    // point, with the hole of its own patch), points with a large |c| voted into a Hough accumulator of fixed angle and
    // offset steps, the strongest cells taken one by one.
    double fold_minimum_length_px = std::numeric_limits<double>::quiet_NaN();       // l_min: the extent of a line's points along it
    double fold_exclusion_half_width_px = std::numeric_limits<double>::quiet_NaN(); // w_f: the band S5 leaves out of W around a line; also
                                                                                    // the band whose points a found line takes with it
    double fold_near_seam_px = std::numeric_limits<double>::quiet_NaN();            // D_fold
    double hough_angle_step_deg = std::numeric_limits<double>::quiet_NaN();         // the fixed angle step of the line search
    double hough_offset_step_px = std::numeric_limits<double>::quiet_NaN();         // the offset step; a line collects three steps
    double fold_residual_sigmas = std::numeric_limits<double>::quiet_NaN();         // |c| above this many robust scales of c ...
    double fold_minimum_residual_px = std::numeric_limits<double>::quiet_NaN();     // ... and above this makes a point a candidate
    std::uint32_t fold_minimum_points = 0;                                          // candidates a line needs (about l_min / Delta x times 3)
    std::uint32_t fold_maximum_lines = 0;                                           // lines reported at most
    // The prediction along a found line (C1 of 11.26). Along a fold the paper's shape hardly changes, so the points on the
    // parallel to the line through a gate row's centre, outside the row's hole, see the shape the row sees. At a gate row near a
    // line, the regression of 11.4 is solved again with the kernel and the polynomial in the coordinates along and across the
    // line (the hole is the same S of the same pixels).
    double fold_kernel_sigma_along_px = std::numeric_limits<double>::quiet_NaN();
    double fold_kernel_sigma_across_px = std::numeric_limits<double>::quiet_NaN();
    double fold_row_half_width_px = std::numeric_limits<double>::quiet_NaN();       // gate rows whose centre is this close to a line
                                                                                    // (across it) are tried along it
    double fold_minimum_change_px = std::numeric_limits<double>::quiet_NaN();       // a row whose prediction along the line is this
                                                                                    // close to 11.4's keeps 11.4's (bit for bit)
    double fold_maximum_rms_ratio = std::numeric_limits<double>::quiet_NaN();       // the prediction along the line is used only when
                                                                                    // its leftover RMS is at most this times 11.4's
    double fold_angle_refinement_deg = std::numeric_limits<double>::quiet_NaN();    // the regression is also tried this far either way
    std::uint32_t fold_angle_refinement_steps = 0;                                  // ... in this many steps each way (0: the line's angle only)
};
// The first values (*proposal*, not approved values; design 11.4 leaves them to the measurement lane M1). From M1 (a
// development analysis of real pairs, outside the repository): the lattice of 16 px, the band half width 192 px, the
// point patch 32 px, degree 2 with sigma 48 px (degree 1 with a narrow kernel failed on rows with points on one side
// only), Tukey 4.685 with the scale floor 0.1 px and 3 reweightings, the ridge 0.01, the hole edge 8 px, n_min 8 (6
// coefficients and a margin), c_min 0.5, the point floor 0.05 px. The margin iota is 0: the footprints already hold the
// feature smoothing (M1's 4 px stood for it). Set here on synthetic scenes only, without a measurement behind them:
// sigma_max, the enclosure share, delta_y, a, L, s_max, the kernel reach and floor, and the constants of the fold lines.
// The hole's rows are not a value: the join's reach r = 2 is the mapping's (S1's cubic Hermite, checked against it by the
// tests), the rows follow from it, the gate's footprint and the row step, and no row is added beyond them (0).
// C1 of 11.26, also on synthetic scenes only (tests/parallax_correction_tests.cpp, --explore): sigma along 96 px (the hole along
// a line is 69 to 120 px; 64 and 128 px gave the same leftover), sigma across 8 px (half the lattice: 12 and 16 px lost the step
// and a fold of 4 mm half width), the rows within 192 px of a line (11.4's error from a fold of 8 mm half width reaches about
// 190 px across it), the least change 0.1 px, the residual ratio 0.45 (the folds' worst 0.37; short waves tried along a line of
// their own direction 0.50 and more), and the angle tried 2.5 degrees either way in 2 steps (half the line search's step).
inline constexpr ParallaxDesign kParallaxDesignDraft{
    16.0, 192.0, 32,
    2, 48.0, 48.0, 3.0, 1e-4, 0.0, 8.0, 0, 2, 0.01, 4.685, 0.1, 3, 0.05, 0.5,
    8.0, 0.25, 0.05, 64.0,
    48.0, 192.0, 0.1,
    128.0, 96.0, 64.0, 5.0, 16.0, 4.0, 0.25, 24, 8,
    96.0, 8.0, 192.0, 0.1, 0.45, 2.5, 2};
// Throws std::invalid_argument for an unset or inconsistent value.
void ValidateParallaxDesign(const ParallaxDesign& design);
// The number of coefficients of the local polynomial (3 for degree 1, 6 for degree 2).
[[nodiscard]] std::size_t ParallaxCoefficientCount(const ParallaxDesign& design);

enum class ParallaxState {
    applied,       // layer 2 is part of the mapping
    not_applied,   // estimated or not, but not put into the mapping (reasons say why)
    disabled,      // the condition has no parallax line
};
[[nodiscard]] constexpr std::string_view ParallaxStateCode(ParallaxState state){
    return state==ParallaxState::applied?"applied":state==ParallaxState::not_applied?"not-applied":
        state==ParallaxState::disabled?"disabled":std::string_view{};
}
[[nodiscard]] constexpr std::optional<ParallaxState> ParseParallaxState(std::string_view text){
    for(const auto s:{ParallaxState::applied,ParallaxState::not_applied,ParallaxState::disabled})if(ParallaxStateCode(s)==text)return s;
    return std::nullopt;
}

enum class ParallaxRowState {
    measured,   // q is the row's own estimate
    filled,     // no estimate of its own; q comes from the neighbouring measured rows (FillUnmeasuredRows)
};
[[nodiscard]] constexpr std::string_view ParallaxRowStateCode(ParallaxRowState state){
    return state==ParallaxRowState::measured?"measured":state==ParallaxRowState::filled?"filled":std::string_view{};
}

// What the fold lines did to a gate row (C1 of 11.26).
enum class ParallaxRowFold {
    none,            // no found line near the row: the row is 11.4's alone
    predicted,       // q is the row's prediction along the line (the row's own estimate; state measured)
    agrees,          // the prediction along the line is within fold_minimum_change_px of 11.4's: the row keeps 11.4's
    unpredictable,   // a line is near the row, but its prediction along it is not used (fold_reason): the row keeps 11.4's,
                     // and what that misses is left for the gate to measure
};
[[nodiscard]] constexpr std::string_view ParallaxRowFoldCode(ParallaxRowFold fold){
    return fold==ParallaxRowFold::none?"none":fold==ParallaxRowFold::predicted?"predicted":fold==ParallaxRowFold::agrees?"agrees":
        fold==ParallaxRowFold::unpredictable?"unpredictable":std::string_view{};
}

// What a gate row is for when the rows are split (11.4, 11.5): a verify row is judged by the gate, and no point that shares a
// pixel with a verify row's gate points enters any estimate; a correct row is corrected, measured by the gate for reference
// only, and never judged. Without the split every row is a verify row with a hole of its own.
enum class ParallaxRowRole {verify, correct};
[[nodiscard]] constexpr std::string_view ParallaxRowRoleCode(ParallaxRowRole role){
    return role==ParallaxRowRole::verify?"verify":role==ParallaxRowRole::correct?"correct":std::string_view{};
}

// One gate row of q (11.4).
//
// Reason codes of ParallaxRow::fold_reason (unpredictable rows):
//   line-ends         the line's segment does not reach past the row's hole on both sides along it (a fold that ends near
//                     the seam column, or the points of the line end there)
//   in-hole           every point on the parallel near the row is in the row's hole (a fold along the seam column)
//   no-points, too-few-points, one-sided, uncertain, ill-conditioned
//                     the regression along the line (as ParallaxRow::reason)
//   not-explained     the surface along the line does not explain its points clearly better than 11.4's explains its own
//                     (the shape is not the same along the line: not a fold, or not only one)
struct ParallaxRow {
    double y{};                 // output row (sample coordinate) of the gate row
    double q{};                 // output px along e_b: what goes into the mapping when the result is applied
    double sigma_q{};           // output px, propagated from the points' uncertainties (measured rows; 0 otherwise)
    double n_eff{};             // effective number of points behind the estimate (measured rows; 0 otherwise)
    bool applied{};             // q is in the mapping (the result is applied)
    std::uint32_t points{};     // points with a nonzero weight (measured rows)
    double lift_mm{};           // -q / k: the paper's height over the paper plane this q stands for
    ParallaxRowState state{ParallaxRowState::filled};
    std::string reason;         // filled rows: why the row has no estimate of its own
    // C1 of 11.26 (fields added after S4).
    ParallaxRowFold fold{ParallaxRowFold::none};
    std::string fold_reason;                  // unpredictable rows: why (codes above)
    std::optional<std::uint32_t> fold_line;   // the line the row was tried along (index in ParallaxResult::folds)
    double fold_angle_deg{};                  // the angle of the regression along it (the line's, or one within the refinement)
    double q_isotropic{};                     // 11.4's estimate of the row (FillUnmeasuredRows included): q without C1
    // The split (fields added with it).
    ParallaxRowRole role{ParallaxRowRole::verify};   // IsVerifyRow of the request's rows
    bool hole{true};                          // the row's estimate kept a hole of its own: no split, or a row the seam column's
                                              // points do not enclose (the edge of the content, EstimateParallax)
};

enum class FoldLineKind {fold, gutter};
[[nodiscard]] constexpr std::string_view FoldLineKindCode(FoldLineKind kind){return kind==FoldLineKind::fold?"fold":kind==FoldLineKind::gutter?"gutter":std::string_view{};}
// A fold or gutter candidate found in the leftover (11.8). The line runs from `first` to `last` (the ends of its points
// along it). Every line is reported as a fold: telling a gutter apart needs more than the leftover along the seam (the
// layer 1 surface, the booklet), so `gutter` is left to the caller.
struct FoldLine {
    FoldLineKind kind{FoldLineKind::fold};
    double position_px{};            // the output row where the line crosses the seam column; the row of its end nearest the
                                     // seam column when it does not cross it
    double angle_deg{};              // the line's direction from +x toward +y, in [0, 180): 0 runs along the rows, 90 along
                                     // the columns (a multiple of the angle step)
    int step_sign{};                 // +1: the paper is higher along the line than its surroundings predict; -1 lower; 0 neither
    double step_px{};                // the median leftover c of the line's points, output px along e_b
    double step_mm{};                // the same as paper height: -step_px / k
    double distance_from_seam_px{};  // the smallest |x - x_s| over the line's segment; 0 when it crosses the seam column
    bool crosses_seam{};             // the segment has points on both sides of the seam column
    render::Point first{}, last{};   // the segment's ends (output px)
    double length_px{};
    std::uint32_t points{};          // the candidates that make the line
    bool near_seam{};                // distance_from_seam_px <= D_fold
    // A line within 45 degrees of the columns that is near the seam: the horizontal move of the drawing (output px,
    // positive to the right) after which the whole segment is D_fold from the seam column; the smaller of the two ways.
    // Empty otherwise (moving the drawing sideways does not take a line along the rows away from the seam).
    std::optional<double> shift_to_clear_px;
    // C1 of 11.26 (fields added after S4): the gate rows tried along this line, by what came of it.
    std::uint32_t rows_predicted{}, rows_agree{}, rows_unpredictable{};
};

// The lift against the condition's line (11.7, warning seam-lift-limit). Over measured rows only.
struct ParallaxLift {
    double limit_mm = std::numeric_limits<double>::quiet_NaN();   // the condition's maxLiftMm (NaN when disabled)
    double maximum_mm{};        // the largest |lift| of a measured row
    double at_row{};            // its output row
    bool exceeded{};            // a measured row's |lift| is over the line (equal is inside)
    std::uint32_t exceeded_rows{};
    double first_exceeded_row{}, last_exceeded_row{};   // output rows, when exceeded
};

// How far layer 2 moved B (11.4: reported, so a local change of the drawing's dimensions is visible).
struct ParallaxSummary {
    double max_shift_px{};           // max |q| over the rows (the shift in output px at the seam column)
    double max_shift_mm{};           // the same in mm of the output
    double at_row{};                 // the output row of the largest shift
    double max_strain{};             // (pi/2) max|q| / L, ParallaxMaximumStrain of the rows' profile
    std::uint32_t first_column{},last_column{};   // the columns where w > 0 (ParallaxSeamWeight), clipped to the output
    std::uint32_t points{};          // the points the regression used (a nonzero weight in at least one row)
    std::uint32_t points_input{};    // the points of the request
    std::uint32_t points_baseline{}; // the points that give the component along the baseline
    std::uint32_t rows_measured{}, rows_filled{};
    double hole_x_px{}, hole_y_px{}; // the hole for a point of the design's patch: g_x and g_y
    // The fold lines' leave-one-out leftover.
    std::uint32_t points_cross_validated{};   // points whose own prediction is measured (n_eff, enclosure, sigma)
    std::uint32_t fold_candidates{};          // points over the candidate threshold
    double fold_residual_scale_px{};          // the robust scale of c (never below tau_min)
    // C1 of 11.26 (fields added after S4): the gate rows by ParallaxRow::fold.
    std::uint32_t rows_fold_predicted{}, rows_fold_agree{}, rows_fold_unpredictable{};
    // The split (fields added with it).
    std::uint32_t points_excluded{};          // points of the request that share a pixel with a verify row's gate points (in no estimate)
    std::uint32_t rows_verify{}, rows_correct{};
    std::uint32_t rows_with_hole{};           // rows whose estimate kept a hole of its own (ParallaxRow::hole)
};

// e_b and k (11.2). C_A and C_B are the camera centres over the table (document mm) that the mapping's Pi(pi) is built
// with (content_registration::PoseModel::Centre: A calibrated, B moved by the estimated pose); b = C_B - C_A on the table;
// e_b = b / |b| in the output frame (the output and document axes point the same way); k = rho |b| / (mean camera height -
// paper height).
struct BaselineGeometry {
    render::Point direction{};        // e_b, unit
    double length_mm{};               // |b|
    double camera_a_height_mm{}, camera_b_height_mm{};
    double paper_height_mm{};         // the paper height k is taken at (layer 1's h0, 0 without layer 1)
    double pixels_per_mm{};           // rho
    double k_px_per_mm{};             // output px of displacement per mm of paper height
};
// Throws std::invalid_argument when a camera is not above the table or a value is not finite.
[[nodiscard]] BaselineGeometry MakeBaselineGeometry(const render::DocumentRenderParameters& calibrated,
    const content_registration::PoseParameters& pose, double paper_height_mm);

struct ParallaxResult {
    ParallaxState state{ParallaxState::not_applied};
    std::vector<std::string> reasons;               // not_applied / disabled: the first is the primary reason
    render::Point baseline{};                        // e_b
    double k_px_per_mm{};                            // output px of displacement per mm of paper height along e_b
    ParallaxDesign design{};                         // the constants this result was made with
    std::vector<ParallaxRow> rows;                   // one per gate row, in order
    ParallaxSummary summary;
    std::vector<FoldLine> folds;                     // the report's `folds`
    BaselineGeometry baseline_geometry;              // e_b and k with what they were made from
    std::uint32_t seam_column{}, first_row{}, row_step{};   // the sample grid of the rows (ToParallaxProfile)
    std::uint32_t verify_period_rows{}, verify_phase{};     // the split of the rows (ParallaxRows); period 0: no split
    ParallaxLift lift;
    [[nodiscard]] bool Applied() const {return state==ParallaxState::applied;}
};

// One measured point around the seam column. The fields are the seam gate's measurer's (seam_gate::SeamPoint).
struct ParallaxPoint {
    render::Point position{};       // output sample position of the patch centre (the gate's lattice convention)
    render::Point displacement{};   // B minus A, output px, with B placed without layer 2
    double uncertainty_px{};        // one sigma
    seam_gate::PointKind kind{seam_gate::PointKind::two_directional};
    render::Point normal{};         // unit normal of the lines (one-directional points only)
    registration_gate::GatePatch footprint{};   // the output pixels its measurement read (registration_gate::GatePatchOf)
};
// The counted points (valid, and not unconfirmed) of a measurement, each with its footprint (GatePatchOf with the
// measurement's design: the remeasurement variants of a point that was remeasured are counted).
[[nodiscard]] std::vector<ParallaxPoint> PointsFromSeamMeasurement(const seam_gate::SeamMeasurement& measurement);
// The footprint of a point at (x, y) measured with a patch of patch_px and not remeasured (GatePatchOf). Throws
// std::invalid_argument for a patch below 2.
[[nodiscard]] registration_gate::GatePatch PointFootprint(std::uint32_t x, std::uint32_t y, std::uint32_t patch_px);
// The footprint of a gate point at (0, 0) with the gate's design, relative to the point: remeasured true counts both
// remeasurement variants (the larger patch and the patch moved along the seam), the most a gate point can read. A gate
// point is remeasured when its value is over the line, which is not known before the gate measures; the strict choice is
// true. Throws what seam_gate::ValidateSeamGateDesign throws.
[[nodiscard]] registration_gate::GatePatch GateFootprint(const seam_gate::SeamGateDesign& gate_design, bool remeasured);

// Where q is computed: the gate's rows along the seam column. The gate's points of a row lie at that row, with x in
// [seam_column - omega_half_width_px, seam_column + omega_half_width_px], and each reads at most gate_footprint around itself.
// The mapping's samples are these rows (ToParallaxProfile), and the hole takes the gate to measure on them.
struct ParallaxRows {
    std::uint32_t output_width{}, output_height{};
    std::uint32_t seam_column{};          // x_s
    std::uint32_t omega_half_width_px{};  // omega
    registration_gate::GatePatch gate_footprint{};   // relative to a gate point (GateFootprint): [x0, x1) x [y0, y1)
    std::uint32_t first_row{};            // gate row i is first_row + i * row_step
    std::uint32_t row_step{};
    std::uint32_t row_count{};
    // The split (11.4, 11.5): gate row i is a verify row when verify_period_rows > 0 and i % verify_period_rows == verify_phase,
    // and a correct row otherwise. The roles follow from the lattice and these two design constants alone, before anything is
    // measured. With the split, a point whose footprint shares a pixel with a verify row's gate points (VerifyRowPixels, widened
    // by iota) enters no estimate at all (the rows' regressions, the fold candidates, the rows filled from their neighbours), and
    // a row's estimate has no hole of its own: it reads the seam column's points of the rows around it. A row that those points
    // do not enclose (no seam column point within the kernel's reach above and below it, and none on the row itself) keeps the
    // hole of the rule without the split. verify_period_rows 0: no split (every row is judged and has a hole of its own).
    std::uint32_t verify_period_rows{};
    std::uint32_t verify_phase{};
};
// Whether gate row `row` is a verify row (always, without the split). Throws std::invalid_argument when the phase is not
// below the period.
[[nodiscard]] bool IsVerifyRow(const ParallaxRows& rows, std::size_t row);
// The pixels any gate point of gate row `row` may read: every column of Omega, each with the gate's footprint. One rectangle.
[[nodiscard]] registration_gate::GatePatch VerifyRowPixels(const ParallaxRows& rows, std::size_t row);

// The gate rows above and below a gate row whose footprints the row's hole covers besides its own (11.5). The mapping joins
// the samples with a cubic Hermite (S1's ParallaxShiftAtRow): between two samples q(y) reads them and their tangents, and
// a tangent reads the samples on either side, so the sample of row j enters q(y) exactly for |y - y_j| < r row steps
// (r = hole_join_reach_rows = 2; at |y - y_j| = 2 row steps q(y) is the sample there). A gate point reads the rows
// [y + f.y0, y + f.y1) of its footprint f, so with s the row step the gate row j + m reads a q of row j's sample when
// m s + f.y0 < r s, and the gate row j - m when m s - (f.y1 - 1) < r s:
//     below = r - 1 + ceil(-f.y0 / s),   above = r - 1 + ceil((f.y1 - 1) / s)   (none for r = 0),
// each at least hole_neighbour_rows. With the gate's footprint with the remeasurement (-50 and 51 px) that is 3 rows for a
// row step of 32 px, 5 for 16 px and 8 for 8 px. The hole is clipped to the gate rows that exist (RowCentre).
struct HoleRows {
    std::uint32_t above{}, below{};
};
// Throws std::invalid_argument when the footprint does not contain its point or the row step is 0.
[[nodiscard]] HoleRows HoleNeighbourRows(const ParallaxRows& rows, const ParallaxDesign& design);

// What the estimate takes.
struct ParallaxRequest {
    std::vector<ParallaxPoint> points;
    ParallaxRows rows;
    BaselineGeometry baseline;
    ParallaxDesign design;
    // The condition's line (registration.parallax.maxLiftMm) and switch (registration.applyBeyondEnvelope). No line
    // means layer 2 is disabled and nothing is estimated. With a line, the switch is required.
    std::optional<double> max_lift_mm;
    std::optional<bool> apply_beyond_line;
    unsigned thread_count{1};          // 1..256
};

// Estimates layer 2 and the fold lines. Throws std::invalid_argument for a programming error or a non-finite input.
[[nodiscard]] ParallaxResult EstimateParallax(const ParallaxRequest& request);
// The result of a path that does not estimate (state disabled, or not_applied with pose-untrusted or time-budget). No
// rows. Throws std::invalid_argument for state applied or an empty reason.
[[nodiscard]] ParallaxResult UnestimatedParallax(ParallaxState state, std::string reason);
// The samples the mapping takes (registration_mapping.hpp): e_b, one q per gate row from first_row at row_step, a and L.
// With one sample on every gate row, the mapping's q(y) (ParallaxShiftAtRow) is each row's estimate exactly there.
// Throws std::invalid_argument when the result is not applied or the profile is not one the mapping accepts
// (ValidateParallaxProfile). The caller keeps a >= Omega's half width (EstimateParallax checks it on its request).
[[nodiscard]] ParallaxProfile ToParallaxProfile(const ParallaxResult& result);

// The weights one gate row's estimate gave to the points (the hole invariant is checked on these). point is the index in
// request.points; hole is the factor S (0 in the hole; 1 for every point of a row without a hole of its own); weight is the
// final weight, Tukey included. Points that give the component along the baseline and lie within the kernel's reach are
// listed, in the order the estimate uses them; with the split, the points excluded for the verify rows are never listed.
struct ParallaxPointWeight {
    std::size_t point{};
    double hole{};
    double weight{};
};
[[nodiscard]] std::vector<ParallaxPointWeight> ParallaxRowWeights(const ParallaxRequest& request, std::size_t row);

// One gate row's regression as EstimateParallax solves it: without an angle, the regression of 11.4; with one (as
// FoldLine::angle_deg), the regression along that direction (C1 of 11.26). weights lists the points within the kernel's
// reach that give the component along the baseline, with the hole's factor and the final weight (the hole invariant is
// checked on these). reason is empty when the estimate is the row's own (n_eff, enclosure and sigma_q within the design's
// lines), else one of ParallaxRow's reasons or in-hole (every point in reach is in the hole). rms is the points' leftover
// against the fitted surface, weighted by the kernel, the hole and the points' uncertainty (not by Tukey).
struct ParallaxRowFit {
    bool measured{};
    std::string reason;
    double q{}, sigma_q{}, n_eff{}, enclosure{}, rms{};
    std::uint32_t points{};
    std::vector<ParallaxPointWeight> weights;
};
[[nodiscard]] ParallaxRowFit ParallaxRowFitOf(const ParallaxRequest& request, std::size_t row, std::optional<double> angle_deg);

// Rows without their own estimate (11.4, "rows without content"): q is not left at 0 next to measured rows. A gap between
// two measured rows that is at most 2 delta_y long is joined linearly; otherwise each measured neighbour's q fades to 0
// over delta_y (1 - smoothstep), so a long white gap goes to 0 and no value is held beyond delta_y. Measured rows are not
// changed. Throws std::invalid_argument for sizes or a non-positive spacing.
void FillUnmeasuredRows(std::vector<double>& q, const std::vector<bool>& measured, double row_step_px, double row_blend_px);
}
