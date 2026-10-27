#pragma once
#include "a0/m2/render.hpp"
#include <algorithm>
#include <cstdint>
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
// Recorded result. difference_*: |A - ratio*B| luminance (8-bit levels) on the
// seam pixel of each row. ratio_*: the smoothed A/B ratio applied at the seam.
// saturation_limited_pixels: B samples whose gain was lowered to stay <= 255.
struct SeamBlendRecord {
    SeamBlendSettings settings;
    std::vector<std::uint32_t> seam_x;
    SeamSummary summary;
    double difference_mean{},difference_maximum{},ratio_minimum{1},ratio_maximum{1};
    std::uint64_t saturation_limited_pixels{};
};
struct SeamBlendResult {
    render::BgrImage image;
    SeamBlendRecord record;
    std::vector<double> row_ratio; // per output row, the gain applied to B at the seam
};
// Throws std::invalid_argument for a top/bottom layout, the same input
// conditions as render::RenderPair, invalid settings, or an output row with no
// pixel covered by both A and B (the seam needs the overlap on every row).
[[nodiscard]] SeamBlendResult RenderPairSeamCut(const render::BgrImage& a,const render::BgrImage& b,
    const render::PairRenderParameters& parameters,const SeamBlendSettings& settings);

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
