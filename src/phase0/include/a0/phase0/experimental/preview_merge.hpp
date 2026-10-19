#pragma once
#include "a0/m2/render.hpp"
#include <chrono>
#include <cstdlib>

namespace a0::phase0::experimental {
// Development review only: A is left, B is right. No learned calibration,
// profile approval or optical-quality claim is implied by this fixed shift.
inline a0::m2::render::PairRenderResult RenderPreviewMerge(
    const a0::m2::render::BgrImage& a,const a0::m2::render::BgrImage& b,
    unsigned overlap_pixels,int b_vertical_pixels){
    if(a.width!=b.width || a.height!=b.height || a.width<2 || !a.height ||
        a.width>640 || a.height>640 || !overlap_pixels || overlap_pixels>=a.width ||
        b_vertical_pixels<=-static_cast<int>(a.height) || b_vertical_pixels>=static_cast<int>(a.height))
        throw std::invalid_argument("invalid uncalibrated preview geometry");
    const auto trim=static_cast<std::uint32_t>(std::abs(b_vertical_pixels));
    a0::m2::render::PairRenderParameters parameters{
        {1,0,static_cast<double>(a.width-overlap_pixels),0,1,static_cast<double>(b_vertical_pixels),0,0,1},
        a0::m2::StitchLayout::camera_a_left_camera_b_right,{0,trim,0,trim}};
    return a0::m2::render::RenderPair(a,b,parameters);
}
// Same expiry rule for A, B and C. A rendered C must also match the current
// session and geometry revision; callers discard it on either transition.
inline bool PreviewPairFresh(std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point a,std::chrono::steady_clock::time_point b){
    return a.time_since_epoch().count()>0 && b.time_since_epoch().count()>0 &&
        now>=a && now>=b && now-a<std::chrono::seconds(2) && now-b<std::chrono::seconds(2);
}
}
