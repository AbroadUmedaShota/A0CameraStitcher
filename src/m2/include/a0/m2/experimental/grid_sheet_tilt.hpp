#pragma once

// Tilt of a camera against a flat square-grid sheet (a cutting mat), from one picture of that sheet (#440 stage 2).
//
// What it measures: the angle between the camera's optical axis and the normal of the sheet, the direction the lens leans,
// and, for two cameras that see the same sheet, the angle between their optical axes. The sheet is the reference, not
// gravity. What it does not measure: any pass or fail, the lens distortion (the one of the given projection is used and
// never re-estimated), the height or the position of the camera, or the sheet's real pitch (the grid is only assumed to
// be square and flat; a lattice that does not come out square is refused).
//
// Input: one original picture at the full size the camera's rig profile was calibrated for, in stored pixel order. A
// picture of any other size (a live view frame, a reduced copy) is refused, not rescaled.
//
// Camera, file, clock and window free. The result types below are measurement values for a person to read. They are
// deliberately not convertible to a pose, a registration estimate, a confirmed position, a rig profile or a fixed
// composite condition, and no function of those APIs accepts them. Nothing here feeds a composite or a capture.
//
// No camera name is built in: a camera is a name and its profile (the intrinsics, the distortion and the document matrix
// of a rig profile, with the picture size that profile belongs to). The projection's matrix is used for two things only:
// the turn of the image axes at calibration (to name a direction in the drawing's words, and to choose between the four
// ways a grid can be numbered) and content_registration::OrientationOf, which defines tilt, direction and turn once for the
// whole code base.

#include "a0/m2/document_render.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace a0::m2::experimental::grid_sheet_tilt {

// A picture to measure: 8-bit, row-major. The memory belongs to the caller and is only read.
enum class PixelFormat {
    bgr8,    // three bytes per pixel, blue first. The line signal is the mean of green and red.
    luma8,   // one byte per pixel
};
struct ImageView {
    std::uint32_t width{};
    std::uint32_t height{};
    PixelFormat format{PixelFormat::bgr8};
    const std::uint8_t* pixels{};
    std::size_t stride_bytes{};   // 0 means tightly packed
};
[[nodiscard]] ImageView ViewOf(const render::BgrImage& image);

// A camera's rig profile as this code needs it: the projection, and the width and height in stored pixel order of the
// pictures that projection was calibrated on (the profile's sensor size). Only a picture of exactly this size is measured.
struct GridSheetProfile {
    render::CameraProjection projection;
    std::uint32_t picture_width{};
    std::uint32_t picture_height{};
};

// Working numbers for the detection and the fit. These are development values found on a prototype; they are not
// accepted optical thresholds and not precision claims. Pixel values are in the picture's own pixels and assume the full
// size picture of a profile (a grid pitch of roughly 50 to 120 pixels).
struct GridSheetDetectionSettings {
    // (a) ridge detector (bright thin line on a darker ground)
    double ridge_sigma_px{1.6};
    double ridge_noise_multiplier{6.0};     // threshold = this x the noise scale of the second derivative
    double ridge_minimum_strength{0.5};     // floor for that threshold (second derivative, grey levels per px^2)
    double ridge_axis_ratio{0.7};           // |weaker eigenvalue| must be below this x |stronger|
    std::uint32_t ridge_band_rows{128};     // rows per work band (memory only; the result does not depend on it)
    std::uint32_t worker_threads{0};        // 0: the hardware's, at most 4. The result does not depend on it
    std::uint32_t noise_bands{6};           // bands sampled to find the noise scale
    std::uint32_t noise_band_rows{128};     // rows of each of those bands
    // (b) first lattice
    double pitch_min_px{46.0};              // assumed range of the grid pitch in the picture
    double pitch_max_px{128.0};
    double window_half_size{0.075};         // in normalised camera units (pixels / focal length)
    std::uint32_t window_min_points{3000};  // ridge points a window needs to be scored
    std::uint32_t direction_min_points{1000};
    std::uint32_t lattice_min_points{500};  // points of each direction the chosen window needs to place the first lattice
    double window_min_score{0.15};          // periodicity of both line families below which no grid is reported
    double direction_gate_deg{8.0};
    double second_direction_window_deg{25.0};
    double pitch_coarse_divisor{30.0};      // the coarse pitch scan steps by the smallest pitch over this
    double pitch_fine_step{1e-5};           // fine pitch scan step, normalised units
    double pitch_fine_span{0.04};           // fine scan covers the coarse peak +- this fraction
    std::uint32_t window_scan_points{800};  // points of a window used to score its periodicity
    std::uint32_t init_scan_points{15000};  // points used to place the first lattice
    // (d) fit
    std::array<double, 6> radii{0.09, 0.14, 0.22, 0.33, 0.5, 2.0};              // half sizes of the growing fit window
    std::array<std::array<double, 4>, 6> gates_px{{{14, 8, 5, 3}, {8, 5, 3, 0}, {6, 4, 3, 0}, {5, 3.5, 3, 0}, {4, 3, 2.5, 0}, {3.5, 3, 2.5, 0}}};   // 0 ends a row
    double direction_match_deg{7.0};        // a point's line direction against the predicted one
    std::uint32_t fit_max_points{45000};    // a fit step uses at most about this many points
    std::uint32_t fit_iterations{6};        // steps per gate
    double fit_settle_px{0.05};             // the last step of the last gate may move a point's line by at most this: the fit settled
    std::uint32_t fit_min_inliers{200};
    double final_gate_px{2.5};              // distance to a lattice line for a point to count
    // lines, nodes, coverage, flatness
    std::uint32_t line_min_points{200};
    double node_max_gap_px{15.0};           // a node must lie this close to a detected point on both lines
    std::uint32_t node_min_count{20};
    std::uint32_t coverage_cell_px{128};
    std::uint32_t coverage_cell_min_points{10};
    std::uint32_t flat_cells_across{8};     // the picture is cut into this many cells across for the flatness check
    std::uint32_t flat_cell_min_points{150};  // points of one family a cell needs to be judged
    double flat_cell_coherence{0.6};        // resultant length of the phase of a cell's points above which it counts as a grid
};

// The lines that decide whether a value is shown. The caller owns them: no value here is an accepted optical limit, and a
// tilt that passes them is not called good.
struct GridSheetAcceptance {
    std::uint32_t min_ridge_points{};            // below this the picture holds no usable lines
    std::uint32_t min_lines_per_family{};        // lines with enough points in each of the two directions
    std::uint32_t min_lattice_span{};            // lattice lines from the first to the last fitted line, each direction
    double min_visible_fraction{};               // share of the picture that holds grid points (0..1)
    double max_lattice_aspect_deviation{};       // |ratio of the two lattice steps on the sheet - 1|: the grid is square
    double max_lattice_skew_deg{};               // the two lattice directions on the sheet are at right angles to within this
    double max_residual_rms_px{};                // rms distance of the used points to their lattice lines; below final_gate_px / sqrt(3)
    double max_method_spread_deg{};              // the three ways of getting the tilt may differ by this much (vector distance)
    double max_off_lattice_cell_fraction{};      // share of the judged cells that hold a coherent grid not on the fitted one
    double off_lattice_offset_px{};              // such a cell's offset from the fitted lattice that counts as "not on it"
};

// Two cameras that see the same sheet: each picture's numbering of the grid is fixed only up to a quarter turn, and is
// chosen against the turn the two calibrated projections have against each other. What is left of that turn is the
// rotation mismatch: near zero when both see the same sheet and neither camera turned about its axis since calibration.
struct GridSheetPairAcceptance {
    double max_rotation_mismatch_deg{};          // 0 <= value < 45
};

struct GridSheetTiltSettings {
    GridSheetDetectionSettings detection;
    GridSheetAcceptance acceptance;
    GridSheetPairAcceptance pair;
};
// The one place that holds starting values. They are working numbers for development, not precision lines: nothing in
// them says how large a tilt may be, and they decide only when this code gives up and says why.
[[nodiscard]] GridSheetTiltSettings WorkingDraftSettings();

enum class GridSheetRefusal {
    none,
    picture_not_profile_size,   // the picture is not the size of the profile's pictures (a reduced copy, a live frame)
    too_few_ridge_points,       // too few thin bright lines in the picture
    no_grid_found,              // no part of the picture shows two periodic line families (also: lines in one direction only)
    fit_failed,                 // the lattice could not be fitted, did not settle, or its geometry is degenerate
    too_few_lines,              // fewer lines per direction than the acceptance asks
    span_too_small,             // the lattice covers too few lines per direction
    view_too_small,             // the grid fills too little of the picture
    grid_not_square,            // the fitted lattice is not square on the sheet (a family locked to another divisor of the pitch)
    residual_too_large,         // the points do not lie on the fitted lattice closely enough
    flatness_not_judged,        // no part of the picture holds enough points to judge whether the sheet is flat
    sheet_not_flat,             // another part of the picture holds a grid that is not on the fitted lattice
    methods_disagree,           // lattice fit, line intersections and vanishing points give different tilts
};
[[nodiscard]] std::string_view RefusalCode(GridSheetRefusal refusal);

// What a measurement rests on. Filled as far as the run got, also when it refuses. NaN: not reached.
struct GridSheetEvidence {
    std::size_t ridge_points{};
    std::size_t used_points{};                       // points within the final gate of a lattice line
    double used_point_fraction{std::numeric_limits<double>::quiet_NaN()};   // used_points / ridge_points; no line is set on it here
    std::array<std::size_t, 2> lines{};              // lines with enough points, per family
    std::array<std::int32_t, 2> lattice_span{};      // lattice lines from the first to the last fitted line, per family
    std::array<double, 2> pitch_px{};                // pitch of the first lattice, per family
    bool fit_converged{};                            // the last step of the lattice fit moved no line by more than fit_settle_px
    double fit_last_step_px{std::numeric_limits<double>::quiet_NaN()};      // how far that step moved the lines (pixels)
    std::size_t nodes{};                             // line intersections used by the second method
    double residual_rms_px{std::numeric_limits<double>::quiet_NaN()};       // used points to their lines
    double node_residual_rms_px{std::numeric_limits<double>::quiet_NaN()};  // second method, intersections to the fitted lattice
    double visible_fraction{std::numeric_limits<double>::quiet_NaN()};      // share of the picture's cells that hold at least a few used points
    double lattice_aspect_ratio{std::numeric_limits<double>::quiet_NaN()};  // second lattice step over the first, on the sheet
    double lattice_skew_deg{std::numeric_limits<double>::quiet_NaN()};      // departure of the two lattice directions from a right angle
    double tilt_deg_fit{std::numeric_limits<double>::quiet_NaN()};            // lattice fit (the reported value)
    double tilt_deg_nodes{std::numeric_limits<double>::quiet_NaN()};          // line intersections
    double tilt_deg_vanishing{std::numeric_limits<double>::quiet_NaN()};      // vanishing points of the two families
    double method_spread_deg{std::numeric_limits<double>::quiet_NaN()};      // largest vector distance between the three
    // The tilt's standard deviation from the fit's own normal equations, as if every used point were independent. Points
    // along one line are not, so this is a lower bound of the uncertainty, not a precision claim.
    double tilt_formal_sigma_deg{std::numeric_limits<double>::quiet_NaN()};
    // The lean fitted to the left and right halves, and to the top and bottom halves, of the used points: the larger of
    // the two vector distances. It shows how much the answer depends on the part of the sheet it comes from.
    double half_split_spread_deg{std::numeric_limits<double>::quiet_NaN()};
    double off_lattice_cell_fraction{std::numeric_limits<double>::quiet_NaN()};   // see GridSheetAcceptance
    std::size_t judged_cells{};
};

// A measured tilt. Directions come in three frames:
//   sheet frame   the grid's own axes: x runs across the first family's lines (along the second family's), y across the
//                 second family's lines (along the first family's). The numbering is arbitrary up to a quarter turn.
//                 Used to compare two cameras.
//   screen frame  the picture's own axes: x to the right, y down, stored pixel order. Independent of how the sheet lies.
//   drawing frame the drawing's x right and y down, assuming the camera turned about its axis as at calibration (the
//                 turn of the given projection). It is the words an installer uses; it rests on that assumption.
// lean is where the lens points on the sheet, measured from the point under the camera: angle (degrees) times the unit
// direction. A tilt of zero has a zero lean.
struct GridSheetTilt {
    std::string camera;
    double tilt_deg{};                               // angle between the optical axis and the sheet's normal
    render::Point lean_screen_deg{};
    render::Point lean_drawing_deg{};
    render::Point lean_sheet_deg{};
    double image_x_azimuth_deg{};                    // the picture's x axis in the sheet frame, turned from the sheet's x toward its y, (-180, 180]
    double calibration_x_azimuth_deg{};              // the same for the given projection (turn at calibration)
};

struct GridSheetTiltOutcome {
    GridSheetRefusal refusal{GridSheetRefusal::fit_failed};
    std::optional<GridSheetTilt> tilt;               // set exactly when refusal is none
    GridSheetEvidence evidence;
};

// Measures one picture. The profile's intrinsics and distortion are used as they are. Throws std::invalid_argument for a
// programming error (an empty picture, a profile without a valid focal length, a profile whose principal point does not
// lie in the middle half of its own picture size, settings that are not set); what the picture shows is never an
// exception, it is a refusal. A picture whose size is not the profile's is refused.
[[nodiscard]] GridSheetTiltOutcome MeasureGridSheetTilt(std::string_view camera, const ImageView& picture,
    const GridSheetProfile& profile, const GridSheetTiltSettings& settings);

enum class GridSheetPairRefusal {
    none,
    reference_not_measured,
    target_not_measured,
    turn_mismatch,           // after the best quarter turn the two pictures are still turned against the calibration by more than the line
};
[[nodiscard]] std::string_view RefusalCode(GridSheetPairRefusal refusal);

// Each camera's numbering of the grid may differ from the other's by a multiple of 90 degrees; the multiple is the one
// that brings the turn of the target's image axes closest to the turn the two calibrated projections have against each
// other.
struct GridSheetPair {
    std::string reference_camera;
    std::string target_camera;
    double optical_axis_angle_deg{};                 // between the two optical axes (independent of how the sheet lies)
    render::Point lean_difference_drawing_deg{};     // target's lean less the reference's, in the reference's drawing frame
    double relative_rotation_deg{};                  // turn of the target's picture axes against the reference's, on the sheet
    int quarter_turns{};                             // 0..3, the target's numbering was turned this many quarter turns
    double rotation_mismatch_deg{};                  // relative_rotation_deg less the calibrated projections' own difference
};
struct GridSheetPairOutcome {
    GridSheetPairRefusal refusal{GridSheetPairRefusal::reference_not_measured};
    std::optional<GridSheetPair> pair;               // set exactly when refusal is none
    double rotation_mismatch_deg{std::numeric_limits<double>::quiet_NaN()};   // set whenever both were measured, also when refused
};
// The function cannot tell whether the two pictures show the same sheet; the caller has to know that. A large rotation
// mismatch is what a different sheet, a wrong numbering or a camera turned since calibration looks like, and is refused.
// Each result carries the turn of its camera's calibrated projection, which is what the numbering is chosen against.
[[nodiscard]] GridSheetPairOutcome CompareGridSheetTilts(const GridSheetTiltOutcome& reference, const GridSheetTiltOutcome& target,
    const GridSheetPairAcceptance& acceptance);

}   // namespace a0::m2::experimental::grid_sheet_tilt
