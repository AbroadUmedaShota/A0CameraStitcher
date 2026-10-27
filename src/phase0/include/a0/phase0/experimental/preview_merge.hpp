#pragma once
#include "a0/m2/render.hpp"
#include "a0/m2/experimental/mount_orientation.hpp"
#include "a0/m2/experimental/overlap_estimation.hpp"
#include <chrono>
#include <cstdlib>
#include <string>

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
// C heading. A/B panes stay as received; only C inputs follow the mounting.
inline std::wstring PreviewMergeTitle(a0::m2::experimental::MountOrientation orientation,unsigned overlap_pixels,int b_vertical_pixels){
    using a0::m2::experimental::MountOrientation;
    (void)a0::m2::experimental::MountOrientationName(orientation);
    return std::wstring(L"C 仮Merge（未校正） 取付の向き: ")+(orientation==MountOrientation::rotated_180?L"180°回転（A/Bを回して合成）":L"通常")+
        L" / A:左 + B:右 / 重なり "+std::to_wstring(overlap_pixels)+L"px / B上下 "+std::to_wstring(b_vertical_pixels)+L"px";
}
// Operator wording for each undetermined reason of the v2 estimator (#364).
inline const wchar_t* OverlapUndeterminedLabel(a0::m2::experimental::OverlapUndeterminedReason reason){
    using R=a0::m2::experimental::OverlapUndeterminedReason;
    switch(reason){
    case R::no_candidate:return L"比べられる模様がありません";
    case R::tied_candidates:return L"同じ強さの候補が複数あります";
    case R::search_boundary:return L"候補が探索範囲の端にあります";
    case R::refinement_boundary:return L"細かい段で候補の位置が定まりません";
    case R::reverse_mismatch:return L"A/Bを入れ替えた照合で同じ位置に戻りません";
    case R::none:break;
    }
    throw std::invalid_argument("no undetermined reason");
}
// Same expiry rule for A, B and C. A rendered C must also match the current
// session and geometry revision; callers discard it on either transition.
inline bool PreviewPairFresh(std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point a,std::chrono::steady_clock::time_point b){
    return a.time_since_epoch().count()>0 && b.time_since_epoch().count()>0 &&
        now>=a && now>=b && now-a<std::chrono::seconds(2) && now-b<std::chrono::seconds(2);
}
}
