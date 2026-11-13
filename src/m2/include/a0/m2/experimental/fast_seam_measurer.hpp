#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/parallax_correction.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::experimental::fast_seam_measurer {
// The fast measurer for layer 2's points (#417, docs/design/fixed-composite-registration.md 11.31). It measures the displacement of
// camera B against camera A at the lattice of the seam band on the output strip itself (the smoothed luminance, then its gradient
// magnitude, built from the mapping the points are measured under), with a search of +-search_radius_px, Gauss-Newton for the
// fraction, a round trip and a second-peak check inside the window. It estimates and nothing else:
//   - it is never called by the gate, and seam_gate.* is not touched: what judges a capture is still the seam gate's measurer;
//   - it does not check the self-correlation around a point (the gate's `periodic` status); it labels a point whose own feature
//     repeats near it instead, and the selection below decides what to do with the label;
//   - its points feed layer 2 only when the capture's design says so (CaptureRegistrationDesign::layer_two_points), and then only
//     after the two stops of Select (a periodic point nothing can check is left out; a measurement whose points mostly ended at the
//     search edge is not used at all).
// Pure computation on the images and the mapping it is given: no camera, file or clock. Every point is measured on its own, so the
// result does not depend on the thread count (bit for bit).
//
// Reason codes of Selection::fallback_reason (kebab-case):
//   edge-share       the share of content points that ended at the search edge is over the line
//   no-points        no point is left for layer 2
//   measurer-failed  the measurement threw (set by the caller; std::bad_alloc is passed on)

// Design constants (*proposal*; none of them is a pass/fail line of the gate). Every field is required (ValidateDesign).
struct Design {
    // The measurer.
    std::uint32_t search_radius_px = 0;          // R: the integer search is +-R around the mapping's placement; a multiple of 4
    double content_level_dn = 0;                 // a pixel counts as content when the gradient magnitude of the luminance is at least this
    std::uint32_t content_samples = 0;           // a patch has content when either image has this many such pixels
    double one_directional_ratio = 0;            // the smaller eigenvalue below this times the larger: a line in one direction
    double second_peak_ratio = 0;                // a second local maximum of the correlation at or above this times the best: ambiguous
    double round_trip_px = 0;                    // the backward measurement must return within this
    double maximum_uncertainty_px = 0;           // a point whose uncertainty is above this is not valid
    double uncertainty_floor_px = 0;             // added to every point's uncertainty
    std::uint32_t maximum_iterations = 0;        // Gauss-Newton steps at most
    // The label of a repeating feature: the self-correlation of A's patch over +-reach px at a quarter of the scale.
    std::uint32_t periodic_reach_px = 0;         // a multiple of 4
    // The stops.
    double neighbour_line_px = 0;                // a periodic point differing from its non-periodic neighbours by more than this is left out
    std::uint32_t neighbour_radius_px = 0;       // the neighbours are the non-periodic points within this (Chebyshev) ...
    std::uint32_t neighbour_wide_radius_px = 0;  // ... or, when there is none, within this
    double edge_share_line = 0;                  // the share of content points that ended at the search edge, above which none is used
    // The strip.
    std::uint32_t strip_margin_px = 0;           // the strip reaches this far beyond the lattice on every side
};
// The first values (*proposal*). The measurer's are the values the desk experiment used; the edge share line 0.25 and the neighbour
// line 1 px were set from seven real pairs and a deliberate misplacement of layer 1 (*to be measured* on more pairs).
inline constexpr Design kDesignDraft{8, 8.0, 32, 0.1, 0.8, 0.05, 0.05, 0.01, 12, 32, 1.0, 64, 128, 0.25, 80};
// Throws std::invalid_argument for an unset or inconsistent value.
void ValidateDesign(const Design& design);

enum class Status {
    valid,
    no_content,        // neither image has content in the patch
    outside,           // the patch or its search window leaves the strip, or touches a pixel the mapping does not cover
    flat,              // the template is flat or nothing correlates
    ambiguous,         // a second peak inside the window
    search_boundary,   // the best match is at the edge of the search window
    window,            // a Gauss-Newton window left the strip, or the system is singular
    round_trip,        // the backward measurement does not return
    uncertain,         // the uncertainty is over the line
};
[[nodiscard]] std::string_view StatusCode(Status status);

// How a point was used by the selection.
enum class Disposition {
    not_used,                 // not valid, or gives no component along the baseline
    kept,                     // used
    dropped_by_neighbours,    // periodic, and its component along the baseline differs from its non-periodic neighbours
    dropped_unchecked,        // periodic, and it has no non-periodic neighbour to check it against
};

struct Point {
    std::uint32_t x{}, y{};              // the lattice position (output sample)
    Status status{Status::no_content};
    bool content{};
    bool one_directional{};
    render::Point normal{};              // unit, one-directional points only (x > 0, or x == 0 and y > 0)
    render::Point displacement{};        // B minus A, output px (valid points)
    double score{}, uncertainty_px{}, round_trip_px{};
    bool periodic{};                     // A's own feature repeats near the point (labelled for content points)
    bool usable{};                       // valid, and gives the component along the baseline
    double along_baseline{};             // the component's weight: 1 for two-directional points, n . e_b for the others
    double baseline_component_px{};      // z: the displacement's component along e_b (usable points)
    Disposition disposition{Disposition::not_used};
    [[nodiscard]] bool Valid() const {return status==Status::valid;}
};

struct Request {
    const render::BgrImage* camera_a{};
    const render::BgrImage* camera_b{};
    render::OutputToCamera map_a, map_b;       // the mapping the points are measured under (B placed by the pose and the plane)
    std::uint32_t output_height{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> lattice;   // the points (x, y), in the order the result keeps
    std::uint32_t lattice_spacing_px{};        // the lattice's spacing (the neighbours are found on it)
    std::uint32_t patch_px{};                  // the point patch of layer 2's design; a multiple of 4
    render::Point baseline{};                  // e_b, unit
    double minimum_baseline_component = 0;     // c_min: |n . e_b| a one-directional point needs
    unsigned thread_count = 1;                 // 1..256; the result does not depend on it
};
// Measures every lattice point and labels the repeating ones. Throws std::invalid_argument for a programming error; what the
// mapping throws is passed on.
[[nodiscard]] std::vector<Point> Measure(const Request& request, const Design& design);

struct Selection {
    std::vector<parallax_correction::ParallaxPoint> points;   // for layer 2, in the lattice's order; empty when not used
    std::uint32_t lattice_points{};    // the lattice
    std::uint32_t content_points{};    // points with content
    std::uint32_t edge_points{};       // content points that ended at the search edge
    double edge_share{};               // edge_points / content_points (0 without content)
    std::uint32_t valid_points{};
    std::uint32_t periodic_points{};   // content points labelled periodic
    std::uint32_t dropped_by_neighbours{}, dropped_unchecked{};
    std::uint32_t points_kept{};       // the points that would go to layer 2 (before the edge share and emptiness checks)
    bool use{};                        // the points go to layer 2
    std::string fallback_reason;       // not use: why (the codes above), else empty
};
// Applies the stops to measured points (it sets Point::disposition): a periodic point whose component along the baseline differs
// by more than neighbour_line_px from the median of the non-periodic usable points within the radius (the wide radius when there is
// none) is left out; one with no such neighbour is left out too (nothing can check it); the capture's points are not used at all
// when the share of content points that ended at the search edge is over edge_share_line. `points` are the result of Measure.
[[nodiscard]] Selection Select(std::vector<Point>& points, const Request& request, const Design& design);
}
