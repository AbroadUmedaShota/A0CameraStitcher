#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/render.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::experimental {
// Seam-cut blend for the uncalibrated Merge (#368). Camera, file and clock
// free; deterministic. Inputs are the oriented A|B buffers and the same pair
// parameters as render::RenderPair (A left, B right). One seam column per
// output row is found by dynamic programming over the overlap band: the cost
// of a cell is the difference of A and B in log luminance (after removing the
// local mean of that difference, so a gain between the bodies does not pull
// the seam toward dark ink) plus the difference of their log-luminance
// gradient magnitudes. The path moves at most one column per row, stays
// edge_margin_pixels inside each row's overlap and has no random choice.
// Left of the seam only A is used, right of it only B; width_pixels around it
// are mixed linearly. The A/B luminance ratio is measured per row in a
// window around the seam, smoothed along rows, and multiplied into B, fully at
// the seam and fading linearly to 1 at the right output edge. One gain for
// the three channels keeps B's colour ratios; a gain that would push a
// channel past 255 is lowered for that pixel instead of clipping one channel.
// The linear feather (render::RenderPair) stays available unchanged.
enum class BlendMethod {linear_feather,seam_cut};
constexpr std::string_view BlendMethodCode(BlendMethod v){return v==BlendMethod::seam_cut?"seam-cut":"linear-feather";}
// Every field is required. Pixel and row counts are in output pixels.
struct SeamBlendSettings {
    unsigned width_pixels{},edge_margin_pixels{},brightness_half_width_pixels{},brightness_smoothing_rows{};
};
// Design constants, not approved values (docs/WPD_STILL_REVIEW.md, #368).
inline constexpr SeamBlendSettings kSeamBlendDesign{8,64,32,128};
// Validates 1..4096 for the width and window, 0..32768 for margin and rows.
inline void ValidateSeamBlendSettings(const SeamBlendSettings& s){
    if(!s.width_pixels || s.width_pixels>4096 || !s.brightness_half_width_pixels || s.brightness_half_width_pixels>4096 ||
        s.edge_margin_pixels>32768 || s.brightness_smoothing_rows>32768)throw std::invalid_argument("invalid seam blend settings");
}
// ---- Colour match of B to A (#415) ----
// The fixed-seam renders below can match B to A in colour as well as in brightness. A is the
// reference; B is multiplied, per channel, by
//   1. a global gain, a constant over the whole of B, measured from the paper pixels of the
//      overlap band (pixels that are not dark, flat and alike in both cameras; ink, edges and
//      features that only one camera sees are left out), and
//   2. a residual gain along the seam, measured per row in a window around the seam on the paper
//      pixels of B after the global gain, smoothed along rows, applied in full at the seam and
//      fading linearly to 1 at the right output edge (the fade of the earlier luminance ratio).
// So the right of B keeps the global gain instead of returning to B's own colour, and the seam
// carries the remaining local difference (vignetting, a difference in light across the page).
// luminance_ratio is the earlier single luminance ratio (#368, #398), kept to compare against and
// for the searched seam cut, which always uses it.
enum class ColorMatchMode {luminance_ratio,rgb_paper};
constexpr std::string_view ColorMatchModeCode(ColorMatchMode v){return v==ColorMatchMode::rgb_paper?"rgb-paper":"luminance-ratio";}
struct ColorMatch {
    ColorMatchMode mode{ColorMatchMode::rgb_paper};
    // The largest ratio applied to a channel of B: the gain at the seam, global times residual, is kept
    // in [1/limit, limit]. No value is chosen here (the owner decides it, #396 B5): absent means no limit,
    // as for the luminance ratio, which was never limited.
    std::optional<double> gain_limit;
};
inline constexpr ColorMatch kColorMatchLuminanceRatio{ColorMatchMode::luminance_ratio,std::nullopt};
// Throws std::invalid_argument for a limit that is not finite or is below 1.
inline void ValidateColorMatch(const ColorMatch& c){
    if(c.gain_limit && (!std::isfinite(*c.gain_limit) || !(*c.gain_limit>=1.0)))throw std::invalid_argument("invalid colour gain limit");
}
// How the paper pixels are chosen. Design constants, not approved values (#415). The thresholds are
// taken from the data of the band (quantiles and a median of that band); only the fractions below are
// stated. A pixel is a paper pixel when, in both cameras, it is
//  - not dark: its luminance is at least luminance_fraction times the luminance_quantile quantile of
//    the band in that camera (ink and shadows are far below it; a surface of any colour passes, since
//    the ratio of the two cameras per channel does not depend on the colour of the surface),
//  - flat: the gradient of its log luminance is at most the gradient_quantile quantile of the band's
//    gradients in that camera (the edge of a line, its antialiasing and a misregistered line are not),
//  - not clipped: no channel is at or above clipped_level in either camera (a clipped channel does not
//    show the ratio), and
//  - consistent: the A/B ratio of each channel is within a factor of ratio_tolerance of the median ratio
//    of the candidates (a feature that one camera sees and the other does not, a sticker, a hand, a
//    reflection, has a ratio far from the page's).
// minimum_cell_fraction: the global gain is measured only if the paper pixels are at least this
// fraction of the cells covered by both cameras; otherwise it stays 1 and the record says so.
// quantile_sample_cells: the quantiles and the median are taken from about this many cells, at a
// fixed stride, which bounds the memory of a full-size band.
struct PaperSelectionDesign {
    double luminance_quantile{},luminance_fraction{},gradient_quantile{},minimum_cell_fraction{},clipped_level{},ratio_tolerance{};
    unsigned quantile_sample_cells{};
};
inline constexpr PaperSelectionDesign kPaperSelectionDesign{0.9,0.4,0.5,1.0/64,254.5,1.25,1u<<20};

struct SeamSummary {std::uint32_t x_minimum{},x_maximum{};double x_mean{};};
// One column per output row; empty input is rejected.
inline SeamSummary SummarizeSeam(const std::vector<std::uint32_t>& x){
    if(x.empty())throw std::invalid_argument("empty seam");
    SeamSummary s{x.front(),x.front(),0};double sum=0;
    for(const auto v:x){s.x_minimum=std::min(s.x_minimum,v);s.x_maximum=std::max(s.x_maximum,v);sum+=v;}
    s.x_mean=sum/static_cast<double>(x.size());return s;
}
// Compact path: the first column, then one of '-', '0', '+' per later row.
inline std::string EncodeSeamSteps(const std::vector<std::uint32_t>& x){
    std::string steps;steps.reserve(x.size()?x.size()-1:0);
    for(size_t i=1;i<x.size();++i){
        if(x[i]+1==x[i-1])steps+='-';else if(x[i]==x[i-1])steps+='0';else if(x[i]==x[i-1]+1)steps+='+';
        else throw std::invalid_argument("seam moves more than one column per row");
    }
    return steps;
}
// Throws std::invalid_argument for an unknown step or a column outside width.
inline std::vector<std::uint32_t> ExpandSeamSteps(std::uint32_t start,std::string_view steps,std::uint32_t width){
    if(start>=width)throw std::invalid_argument("seam outside image");
    std::vector<std::uint32_t> x;x.reserve(steps.size()+1);x.push_back(start);
    for(const char c:steps){
        auto v=x.back();
        if(c=='-'){if(!v)throw std::invalid_argument("seam outside image");--v;}
        else if(c=='+'){if(v+1>=width)throw std::invalid_argument("seam outside image");++v;}
        else if(c!='0')throw std::invalid_argument("invalid seam step");
        x.push_back(v);
    }
    return x;
}
// The colour match of #415, per channel in B, G, R order. Default: the luminance ratio, nothing more.
// measured_global: A/B over the paper pixels (1 if global_measured is false). global_gain: the gain
// applied to B everywhere (measured_global kept within the limit). local_*: the range over rows of the
// residual gain at the seam, after the limit. paper_cells / covered_cells: the paper pixels used and
// the cells covered by both cameras in the band. local_limited_rows: rows whose residual hit the limit.
// luminance_fallback: the global gain could not be measured (global_measured is false), so the render
// used the luminance ratio, byte for byte as with kColorMatchLuminanceRatio, instead of leaving B
// without a ratio; the gains and the local range are then 1 and row_gain is empty.
struct ColorMatchRecord {
    ColorMatchMode mode{ColorMatchMode::luminance_ratio};
    bool gain_limit_set{};double gain_limit{};
    bool global_measured{},global_limited{},luminance_fallback{};
    std::uint64_t paper_cells{},covered_cells{},local_limited_rows{};
    std::array<double,3> measured_global{1,1,1},global_gain{1,1,1},local_minimum{1,1,1},local_maximum{1,1,1};
};
// Recorded result. difference_*: |A - gain*B| luminance (8-bit levels) on the
// seam pixel of each row. ratio_*: the smoothed A/B ratio applied at the seam; for the
// colour match, the luminance of B's paper after the per-channel gain at the seam over its own
// luminance, which is the same quantity.
// saturation_limited_pixels: B samples whose gain was lowered to stay <= 255 (for the colour match
// all three channel gains are lowered together, so the hue of the match is kept).
struct SeamBlendRecord {
    SeamBlendSettings settings;
    std::vector<std::uint32_t> seam_x;
    SeamSummary summary;
    double difference_mean{},difference_maximum{},ratio_minimum{1},ratio_maximum{1};
    std::uint64_t saturation_limited_pixels{};
    bool fixed_seam{}; // true for RenderPairFixedSeam: seam_x holds one column in every row
    ColorMatchRecord color; // the luminance ratio unless a fixed-seam render matched the colour
};
struct SeamBlendResult {
    render::BgrImage image;
    SeamBlendRecord record;
    std::vector<double> row_ratio; // per output row, the gain applied to B at the seam
    // Colour match only: per output row, the gain of each channel (B, G, R) applied to B at the seam,
    // global times residual. Empty for the luminance ratio.
    std::vector<std::array<double,3>> row_gain;
};
// Throws std::invalid_argument for a top/bottom layout, the same input
// conditions as render::RenderPair, invalid settings, or an output row with no
// pixel covered by both A and B (the seam needs the overlap on every row).
// The searched seam cut always uses the luminance ratio.
[[nodiscard]] SeamBlendResult RenderPairSeamCut(const render::BgrImage& a,const render::BgrImage& b,
    const render::PairRenderParameters& parameters,const SeamBlendSettings& settings);

// Vertical seam at a given column (#396, #398): the same blend as
// RenderPairSeamCut with the seam at output column seam_column in every row
// instead of a searched path. The caller passes the column (the column of the
// fixed composition condition of #399, which a person changes in #403); this
// function never moves it, so the seam does not depend on the image content. Camera, file and clock free;
// deterministic. The brightness ratio is measured and
// applied exactly as in RenderPairSeamCut (window sums of A and B around the
// seam per row, smoothed along rows, multiplied into B, fading to 1 at the
// right output edge), and the blend over width_pixels is the same. The record
// has fixed_seam set and seam_x equal to seam_column in every row.
// color (#415): the default matches B to A per channel (see ColorMatch above); pass
// kColorMatchLuminanceRatio for the luminance ratio described in this paragraph, which is then
// byte-for-byte the output of #398. With A and B the same image at an integer offset both modes
// give the same bytes. When the paper pixels of the band cannot be measured the colour match
// renders with the luminance ratio as well (record.color.luminance_fallback). Throws std::invalid_argument for an invalid color. The same holds for
// RenderMappedFixedSeam and RenderDocumentFixedSeam below.
// seam_column is an output pixel column (after the crop). The columns whose
// blend weight is strictly between 0 and 1, seam_column +/- (ceil(width_pixels/2)-1),
// must be covered by both A and B on every output row; otherwise this throws
// std::invalid_argument and renders nothing. edge_margin_pixels is not used: it
// keeps a searched path away from the rim of the overlap, whereas a column
// given by the caller is validated here against the overlap itself (the value
// is still copied into the record). Throws
// std::invalid_argument also for the conditions of RenderPairSeamCut.
[[nodiscard]] SeamBlendResult RenderPairFixedSeam(const render::BgrImage& a,const render::BgrImage& b,
    const render::PairRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings,const ColorMatch& color={});
// Whether RenderPairFixedSeam accepts seam_column for these inputs, decided on
// the same coverage test without rendering (it samples the overlap band only).
// Throws std::invalid_argument for the other input conditions of
// RenderPairFixedSeam (layout, buffers, settings, a row with no overlap), so
// false means only that the column and its half blend width do not fit.
[[nodiscard]] bool CanRenderFixedSeam(const render::BgrImage& a,const render::BgrImage& b,
    const render::PairRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings);

// ---- Vertical seam between two cameras given as output-to-camera mappings (#405) ----
// RenderPairFixedSeam places B with a 3x3 similarity, which cannot follow a lens
// distortion. The functions below run the same blend (the band, the brightness
// ratio of window sums, the mixing along the seam, one bilinear sampling of the raw
// pixels) on a pair of render::OutputToCamera mappings: output pixel (x,y), passed as
// its integer index, to the raw position of A and of B. The mappings of the document
// render (a lens model, a document plane and a raster in micrometres and DPI) and the
// pair mapping of render::RenderPair are both such pairs, and the seam gate measures
// with the same form (seam_gate.hpp), so a gate can be given the pair a render used.
// A mapping returns false where its camera has no sample; a non-finite position is
// an error (std::invalid_argument), never a reason to use the other camera.
// Mappings must be immutable and deterministic.
struct MappedFixedSeamInput {
    std::uint32_t width{},height{};                 // output raster, in pixels
    render::OutputToCamera camera_a,camera_b;       // A is the left camera, B the right one
};
// Per output row, the run [first,end) of columns where both cameras have a sample.
struct MappedOverlapRow {std::uint32_t first{},end{};};

// render::RenderPair's mapping as a pair: output pixel (x,y) is global (ox+x,oy+y), where
// (ox,oy) is the canvas origin plus the crop; A is sampled there and B through the inverse of
// camera_b_to_camera_a. RenderMappedFixedSeam on this pair equals RenderPairFixedSeam byte for
// byte (the same double arithmetic; RenderPairFixedSeam itself avoids the indirect call).
// Throws std::invalid_argument for the conditions of RenderPair (layout, transform, canvas).
[[nodiscard]] MappedFixedSeamInput PairMappedFixedSeamInput(std::uint32_t camera_a_width,std::uint32_t camera_a_height,
    std::uint32_t camera_b_width,std::uint32_t camera_b_height,const render::PairRenderParameters& parameters);
// render::RenderDocumentPair's mapping as a pair (render::DocumentOutputToCamera of each camera):
// the raw position follows the lens coefficients and the document plane, so a lens distortion
// is followed, not approximated. Only A left and B right and bilinear resampling are supported.
// Throws std::invalid_argument for another layout or kernel, or when a camera projection does not
// validate over the raster (render::ValidateCameraProjection).
[[nodiscard]] MappedFixedSeamInput DocumentMappedFixedSeamInput(const render::DocumentRenderParameters& parameters);

// Per output row, the run of columns covered by both cameras that contains the first covered
// column of a coarse scan (every 8th column, every column for a row the scan misses). Throws
// std::invalid_argument for the input conditions of RenderMappedFixedSeam or a row with no
// column covered by both.
[[nodiscard]] std::vector<MappedOverlapRow> MappedFixedSeamBand(const render::BgrImage& a,const render::BgrImage& b,
    const MappedFixedSeamInput& input);

// The same blend as RenderPairFixedSeam with the seam at output column seam_column in every
// row, for a mapped pair. seam_column +/- (ceil(width_pixels/2)-1) must be covered by both
// cameras on every output row; otherwise this throws std::invalid_argument and renders nothing.
// So does a row whose covered columns are not one run, an output pixel covered by neither camera,
// a missing mapping, an empty or too large output, a buffer that does not match its size, and
// invalid settings. edge_margin_pixels is not used (it is copied into the record), as in
// RenderPairFixedSeam. The record has fixed_seam set and seam_x equal to seam_column in every row.
// Camera, file and clock free; deterministic for deterministic mappings.
[[nodiscard]] SeamBlendResult RenderMappedFixedSeam(const render::BgrImage& a,const render::BgrImage& b,
    const MappedFixedSeamInput& input,std::uint32_t seam_column,const SeamBlendSettings& settings,const ColorMatch& color={});
// Whether RenderMappedFixedSeam accepts seam_column, decided on the same coverage test without
// rendering. Throws for the other input conditions, so false means only that the column and its
// half blend width do not fit.
[[nodiscard]] bool CanRenderMappedFixedSeam(const render::BgrImage& a,const render::BgrImage& b,
    const MappedFixedSeamInput& input,std::uint32_t seam_column,const SeamBlendSettings& settings);
// RenderMappedFixedSeam / CanRenderMappedFixedSeam on DocumentMappedFixedSeamInput(parameters).
// seam_column is a column of the document raster (parameters.raster).
[[nodiscard]] SeamBlendResult RenderDocumentFixedSeam(const render::BgrImage& a,const render::BgrImage& b,
    const render::DocumentRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings,const ColorMatch& color={});
[[nodiscard]] bool CanRenderDocumentFixedSeam(const render::BgrImage& a,const render::BgrImage& b,
    const render::DocumentRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings);

// A composition condition for a vertical seam: the pair mapping with its crop
// and the seam column, all in the pixels of the image size it was made for.
struct FixedSeamCondition {render::PairRenderParameters parameters;std::uint32_t seam_column{};};
// Rescales a condition made for capture_width x capture_height frames (A and B
// of that size) to view_width x view_height frames, assuming the view shows the
// same field as the capture scaled by sx = view_width/capture_width and
// sy = view_height/capture_height (a normalized-coordinate correspondence, not
// a verified optical one). Pure: no image is read.
// - The mapping is S*H*S^-1 with S = diag(sx,sy,1), so B lands exactly where
//   the capture puts it, scaled.
// - The crop window and the seam are scaled in global canvas coordinates and
//   rounded to whole view pixels. The seam is within half a view pixel of sx
//   times its capture position. The crop edges round inward and are clamped to
//   the scaled canvas, so each moves by less than 2 view pixels and the window
//   never reaches outside the scaled capture window.
// Throws std::invalid_argument for empty sizes, an invalid mapping or canvas, or a seam
// column outside the output of the capture condition or of the scaled one.
[[nodiscard]] FixedSeamCondition ScaleFixedSeamCondition(const FixedSeamCondition& capture,
    std::uint32_t capture_width,std::uint32_t capture_height,std::uint32_t view_width,std::uint32_t view_height);

// Development diagnostic, not a quality measure: the offset between A and the
// placed B around seam points, from the zero-mean correlation of luminance
// gradient magnitude. A square patch of A centered on the seam is searched in
// B within +/-search pixels. valid: a strict best away from the search edge on
// a patch with texture whose match in B, searched back in A, returns to it
// (round trip, no threshold). dx/dy: where B shows A's patch, B minus A, in output
// pixels; a line crossing the seam is offset by this.
struct SeamDisplacementProbe {std::uint32_t y{};int dx{},dy{};double correlation{};bool valid{};};
[[nodiscard]] std::vector<SeamDisplacementProbe> ProbeSeamDisplacement(const render::BgrImage& a,const render::BgrImage& b,
    const render::PairRenderParameters& parameters,const std::vector<std::uint32_t>& seam_x,unsigned patch,unsigned search,unsigned row_step);
}
