#pragma once
#include "a0/m2/experimental/fixed_composite_condition.hpp"
#include "a0/m2/experimental/mount_orientation.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include "a0/m2/render.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace a0::phase0::experimental {
// Live C drawn with the fixed composite condition (#401, parent #396;
// docs/design/fixed-composite.md sections 3.4 and 7). Development only,
// display only: the picture is never saved and feeds no capture, composite,
// estimate or condition (ADR-0036 decision 1, item 5).
//
// The condition (#399) is made for full-size captures. The live view frame is
// assumed to show the whole capture scaled by sx = view_width/capture_width
// and sy = view_height/capture_height (the normalized-coordinate
// correspondence of #334). That correspondence is NOT verified (#309), and
// everything here inherits that assumption. ScaleFixedSeamCondition (#398)
// turns the capture condition into the same mapping S*H*S^-1 for the view.
//
// Scale is not corrected: the live view does not know the per-capture scale
// before the shutter (ADR-0037), so C and the JPEG differ only by the scale
// within the limit and by the resolution.
//
// Blend width and brightness window. kSeamBlendDesign (8 px blend, 32 px
// half-window, 128 rows of smoothing) is stated in full-size capture pixels.
// For C each size is scaled by the same sx (columns) or sy (rows) as the view
// and rounded to whole view pixels, with a minimum of 1 (0 for the unused edge
// margin). The same ground length is therefore blended and measured, and a 8 px
// blend that is under one view pixel becomes the hard switch of one pixel that
// it already is at that resolution. A separate set of constants for C would
// make C look different from the JPEG by construction and add a second
// unapproved value, so none is introduced.
//
// A column that fits the full-size capture can fail the fit test at view scale
// only by rounding: the scaled seam and the scaled band edges are each rounded
// to whole view pixels (ScaleFixedSeamCondition documents under 2 view pixels
// for the crop edges), and the half blend width of the capture is well under
// one view pixel. PlanFixedPreview therefore tries the scaled seam column and
// then the neighbours up to kFixedPreviewSeamFitPixels away, nearest first,
// and uses the first that CanRenderFixedSeam accepts. The shift is recorded in
// the plan and shown on screen. If none fits, C is not drawn and the reason is
// shown. The full-size JPEG path is the authority for the condition itself: it
// refuses a column that does not fit at full size (seam-outside-overlap).

// Largest view side C accepts. The same bound as RenderPreviewMerge and the
// reduction of the live frames (a resource bound, not an optical value).
inline constexpr unsigned kFixedPreviewMaximumDimension=640;
// How far PlanFixedPreview may move the scaled seam column to make it fit.
inline constexpr unsigned kFixedPreviewSeamFitPixels=2;

// Design constants stated for full-size captures, scaled to a view.
inline a0::m2::experimental::SeamBlendSettings ScaleSeamBlendSettings(
    const a0::m2::experimental::SeamBlendSettings& design,double sx,double sy){
    if(!std::isfinite(sx) || !std::isfinite(sy) || !(sx>0) || !(sy>0))throw std::invalid_argument("invalid blend scale");
    const auto scaled=[](unsigned value,double factor,long long minimum){
        const auto r=std::llround(static_cast<double>(value)*factor);
        if(r>0xffffffffLL)throw std::invalid_argument("blend setting out of range");
        return static_cast<unsigned>(std::max(minimum,r));
    };
    a0::m2::experimental::SeamBlendSettings s{scaled(design.width_pixels,sx,1),scaled(design.edge_margin_pixels,sx,0),
        scaled(design.brightness_half_width_pixels,sx,1),scaled(design.brightness_smoothing_rows,sy,1)};
    a0::m2::experimental::ValidateSeamBlendSettings(s);
    return s;
}

// Everything C needs besides the two frames. Pure data; no pixel is read.
struct FixedPreviewPlan {
    // Empty: C can be drawn. Otherwise why not (the codes of the JPEG path).
    std::optional<a0::m2::experimental::FixedCompositeSkip> skip;
    std::string detail;
    unsigned view_width{},view_height{};
    double scale_x{},scale_y{};
    // The mapping and seam column in view pixels, as RenderPairFixedSeam takes them.
    a0::m2::experimental::FixedSeamCondition view;
    a0::m2::experimental::SeamBlendSettings settings{};
    std::uint32_t output_width{},output_height{};
    // From the condition record, in full-size capture pixels.
    std::uint32_t capture_seam_column{};
    a0::m2::experimental::FixedSeamOrigin seam_origin{a0::m2::experimental::FixedSeamOrigin::band_center_provisional};
    // ScaleFixedSeamCondition's seam column, and the signed distance the fit moved it.
    std::uint32_t scaled_seam_column{};
    int seam_shift{};
};

// Scales the condition to a view of view_width x view_height (the oriented,
// reduced live frames, equal for A and B). `orientation` is the mount
// orientation of the running C; a condition made for the other one is refused.
// Never throws: a refusal is in plan.skip.
inline FixedPreviewPlan PlanFixedPreview(const a0::m2::experimental::FixedCompositeCondition& condition,
    a0::m2::experimental::MountOrientation orientation,unsigned view_width,unsigned view_height){
    using a0::m2::experimental::FixedCompositeSkip;
    FixedPreviewPlan plan;
    plan.view_width=view_width;plan.view_height=view_height;
    plan.capture_seam_column=condition.seam.column;plan.seam_origin=condition.seam.origin;
    const auto refuse=[&](FixedCompositeSkip skip,std::string detail){plan.skip=skip;plan.detail=std::move(detail);return plan;};
    if(condition.orientation!=orientation)
        return refuse(FixedCompositeSkip::orientation_differs,std::string("condition ")+a0::m2::experimental::MountOrientationName(condition.orientation)+", live "+a0::m2::experimental::MountOrientationName(orientation));
    if(!view_width || !view_height || view_width>kFixedPreviewMaximumDimension || view_height>kFixedPreviewMaximumDimension)
        return refuse(FixedCompositeSkip::size_differs,"view "+std::to_string(view_width)+"x"+std::to_string(view_height));
    a0::m2::experimental::FixedSeamCondition capture;
    try{capture={a0::m2::experimental::FixedCompositeRenderParameters(condition),condition.seam.column};}
    catch(const std::invalid_argument& error){return refuse(FixedCompositeSkip::condition_invalid,error.what());}
    a0::m2::experimental::FixedSeamCondition scaled;
    try{scaled=a0::m2::experimental::ScaleFixedSeamCondition(capture,condition.source_width,condition.source_height,view_width,view_height);}
    catch(const std::invalid_argument& error){
        // The seam alone is the cause when the same mapping scales with the seam on the first column
        // (the rounded seam fell off the scaled output); otherwise the size cannot be scaled to.
        try{(void)a0::m2::experimental::ScaleFixedSeamCondition({capture.parameters,0},condition.source_width,condition.source_height,view_width,view_height);}
        catch(const std::invalid_argument&){return refuse(FixedCompositeSkip::size_differs,error.what());}
        return refuse(FixedCompositeSkip::seam_outside_overlap,error.what());
    }
    plan.scale_x=static_cast<double>(view_width)/condition.source_width;plan.scale_y=static_cast<double>(view_height)/condition.source_height;
    try{plan.settings=ScaleSeamBlendSettings(a0::m2::experimental::kSeamBlendDesign,plan.scale_x,plan.scale_y);}
    catch(const std::invalid_argument& error){return refuse(FixedCompositeSkip::size_differs,error.what());}
    const auto canvas=a0::m2::render::PlanPairCanvas(view_width,view_height,view_width,view_height,scaled.parameters);
    plan.output_width=canvas.output_width;plan.output_height=canvas.output_height;
    plan.scaled_seam_column=scaled.seam_column;
    // Coverage depends on the geometry only, so blank frames decide the fit.
    const a0::m2::render::BgrImage blank{view_width,view_height,std::vector<std::uint8_t>(static_cast<size_t>(view_width)*view_height*3)};
    std::vector<int> offsets{0};
    for(int distance=1;distance<=static_cast<int>(kFixedPreviewSeamFitPixels);++distance){offsets.push_back(-distance);offsets.push_back(distance);}
    for(const int offset:offsets){
        const long long column=static_cast<long long>(scaled.seam_column)+offset;
        if(column<0 || column>=static_cast<long long>(plan.output_width))continue;
        bool fits=false;
        try{fits=a0::m2::experimental::CanRenderFixedSeam(blank,blank,scaled.parameters,static_cast<std::uint32_t>(column),plan.settings);}
        catch(const std::invalid_argument&){fits=false;} // a row without A and B: nothing fits
        if(fits){
            plan.view={scaled.parameters,static_cast<std::uint32_t>(column)};plan.seam_shift=offset;
            return plan;
        }
    }
    return refuse(FixedCompositeSkip::seam_outside_overlap,"scaled seam column "+std::to_string(scaled.seam_column)+" of "+std::to_string(plan.output_width));
}

// One frame pair of C. A and B are the oriented, reduced live frames of the
// size the plan was made for. The brightness ratio is measured here, in every
// call, from these two frames; the mapping and the seam column never move.
inline a0::m2::experimental::SeamBlendResult RenderFixedPreview(const FixedPreviewPlan& plan,
    const a0::m2::render::BgrImage& a,const a0::m2::render::BgrImage& b){
    if(plan.skip)throw std::invalid_argument("fixed composite preview has no plan");
    if(a.width!=plan.view_width || a.height!=plan.view_height || b.width!=plan.view_width || b.height!=plan.view_height)
        throw std::invalid_argument("frames differ from the planned view size");
    return a0::m2::experimental::RenderPairFixedSeam(a,b,plan.view.parameters,plan.view.seam_column,plan.settings);
}

// The condition file beside the settings, read when a preview session starts.
// An absent file is the uncalibrated C; any other failure is a damaged one.
struct FixedPreviewSource {
    std::optional<a0::m2::experimental::LoadedFixedCompositeCondition> loaded;
    a0::m2::experimental::FixedCompositeSkip reason{a0::m2::experimental::FixedCompositeSkip::condition_absent};
    std::string detail;
};
template<class Hash>
FixedPreviewSource LoadFixedPreviewSource(const std::filesystem::path& file,Hash hash){
    FixedPreviewSource source;
    try{source.loaded=a0::m2::experimental::LoadFixedCompositeCondition(file,hash);}
    catch(const std::exception& error){source.reason=a0::m2::experimental::FixedCompositeSkip::condition_invalid;source.detail=error.what();}
    return source;
}

// uncalibrated: no condition exists, the earlier translation-only C (manual and
// automatic alignment) stays. fixed_composite: drawn with the condition.
// unavailable: a condition exists but C cannot be drawn with it; C is not drawn
// and the reason is shown (ADR-0036 decision 1, item 8).
enum class PreviewCMode {uncalibrated,fixed_composite,unavailable};
struct FixedPreviewChoice {
    PreviewCMode mode{PreviewCMode::uncalibrated};
    FixedPreviewPlan plan; // fixed_composite: ready; unavailable: plan.skip is the reason (also when the file was damaged)
};
inline FixedPreviewChoice ChoosePreviewC(const FixedPreviewSource& source,a0::m2::experimental::MountOrientation orientation,
    unsigned view_width,unsigned view_height){
    FixedPreviewChoice choice;
    choice.plan.view_width=view_width;choice.plan.view_height=view_height;
    if(!source.loaded){
        if(source.reason==a0::m2::experimental::FixedCompositeSkip::condition_absent)return choice;
        choice.mode=PreviewCMode::unavailable;choice.plan.skip=source.reason;choice.plan.detail=source.detail;
        return choice;
    }
    choice.plan=PlanFixedPreview(source.loaded->condition,orientation,view_width,view_height);
    choice.mode=choice.plan.skip?PreviewCMode::unavailable:PreviewCMode::fixed_composite;
    return choice;
}

inline const wchar_t* FixedPreviewSeamOriginText(a0::m2::experimental::FixedSeamOrigin origin){
    return origin==a0::m2::experimental::FixedSeamOrigin::operator_set?L"人が決めた値":L"帯の中央の仮の値";
}
// Shown beside the uncalibrated C when there is no condition.
inline constexpr const wchar_t* kUncalibratedPreviewNotice=L"固定の合成条件がありません（未校正の仮の C）";
inline std::wstring FixedPreviewTitle(a0::m2::experimental::MountOrientation orientation,const FixedPreviewPlan& plan){
    (void)a0::m2::experimental::MountOrientationName(orientation);
    return std::wstring(L"C 固定の合成条件（未校正・未承認・表示専用） 取付の向き: ")+
        (orientation==a0::m2::experimental::MountOrientation::rotated_180?L"180°回転":L"通常")+
        L" / 継ぎ目の列 "+std::to_wstring(plan.capture_seam_column)+L"（撮影画像の画素・"+FixedPreviewSeamOriginText(plan.seam_origin)+L"）";
}
// The standing note under the title; the unverified correspondence is always named.
inline std::wstring FixedPreviewNote(const FixedPreviewPlan& plan){
    std::wstring text=L"ライブビューと撮影画像の対応は ※未検証（#309） / 倍率は補正しません / 保存しません";
    if(plan.seam_shift)text+=L" / 継ぎ目をCの画素に合わせて "+std::to_wstring(std::abs(plan.seam_shift))+L"px "+(plan.seam_shift<0?L"左":L"右")+L"へ寄せています";
    return text;
}
// A position request (manual or automatic) while C is drawn with the condition.
inline std::wstring FixedPreviewPositionRefusal(){
    return L"固定の合成条件で描いているため、位置合わせはCの位置を変えません（ライブビューと撮影画像の対応は ※未検証 #309）";
}
// Why C is not drawn although a condition file exists.
inline std::wstring FixedPreviewUnavailableText(a0::m2::experimental::FixedCompositeSkip skip){
    using S=a0::m2::experimental::FixedCompositeSkip;
    switch(skip){
    case S::condition_absent:return kUncalibratedPreviewNotice;
    case S::condition_invalid:return L"固定の合成条件を読めません（内容が正しくありません）。Cを描きません。設置の時に作り直してください";
    case S::orientation_differs:return L"固定の合成条件の取り付けの向きが、この画面の向きと合いません。Cを描きません";
    case S::size_differs:return L"固定の合成条件を、ライブビューの大きさへ縮められません。Cを描きません";
    case S::seam_outside_overlap:return L"固定の合成条件の継ぎ目の列が、縮めたCの重なりの外にあります。Cを描きません";
    }
    throw std::invalid_argument("unknown fixed composite skip");
}
}
