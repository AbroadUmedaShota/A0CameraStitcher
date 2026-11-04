#pragma once
#include "a0/m2/experimental/still_review.hpp"
#include "a0/phase0/experimental/fixed_composite_text.hpp"
#include <string>

namespace a0::phase0::experimental {
// Display text for the lens-aware fixed composite (#405 W4, report v7). Rounded for reading; the
// report keeps the full values. Never states a quality pass. The condition is shown as unapproved,
// and the profile as a calibrated draft, as they are.
inline std::wstring FixedLensScaleLine(const a0::m2::experimental::FixedScaleCorrectionV2& s){
    using S=a0::m2::experimental::FixedScaleStatus;
    const auto limit=L"上限 "+StillFixed(s.limit_percent,2)+L"%";
    std::wstring text;
    switch(s.status){
    case S::applied:text=L"倍率を ×"+StillFixed(s.applied_scale,5)+L" 補正しました（100% との差 "+StillFixed(*s.difference_percent,3)+L"%、"+limit+L"）";break;
    case S::exceeded:text=L"倍率は補正していません（100% との差 "+StillFixed(*s.difference_percent,3)+L"%、"+limit+L"）";break;
    case S::not_estimated:
    case S::seam_outside_overlap:text=L"倍率は補正していません（"+limit+L"）";break;
    }
    const auto notice=a0::m2::experimental::FixedScaleNotice(s.status);
    if(!notice.empty())text+=L" / "+WidenUtf8(notice);
    return text;
}
// One line for the capture status of the preview window.
inline std::wstring FixedCompositeLensLine(const a0::m2::experimental::SavedFixedCompositeLens& f){
    using C=a0::m2::experimental::FixedPolicyCheck;
    std::wstring text=L"固定の合成条件（校正の写像・未承認）で合成: 継ぎ目の列 "+std::to_wstring(f.seam_column)+L"（"+FixedSeamOriginText(f.seam_origin)+L"、桃色の縦線） / "+FixedLensScaleLine(f.scale);
    if(f.policy_limit.check!=C::same)text+=L" / "+FixedPolicyLine(f.policy_limit,f.scale.limit_percent);
    return text;
}
// Multi-line block (CRLF) for the inspector's side panel.
inline std::wstring FixedCompositeLensDetails(const a0::m2::experimental::SavedStillReview& r){
    const auto& f=*r.fixed_composite_lens;const auto& s=f.scale;const auto& g=f.raster.region_um;
    const auto mm=[](std::int64_t um){return StillFixed(static_cast<double>(um)/1000.0,1);};
    std::wstring text=L"合成（保存JPEG）: 固定の合成条件（校正の写像・未承認）\r\n条件 SHA-256: "+std::wstring(f.condition_sha256.begin(),f.condition_sha256.begin()+12)+L"…\r\n";
    text+=L"校正ファイル SHA-256: "+std::wstring(f.profile_file_sha256.begin(),f.profile_file_sha256.begin()+12)+L"…\r\n";
    if(f.profile_id)text+=L"校正の識別子: "+WidenUtf8(*f.profile_id)+L"\r\n";
    text+=L"出力: 文書の面 "+mm(g.left)+L"〜"+mm(g.right)+L" × "+mm(g.top)+L"〜"+mm(g.bottom)+L" mm、"+std::to_wstring(f.raster.dpi)+L" DPI、"+
        std::to_wstring(f.raster.width_pixels)+L" × "+std::to_wstring(f.raster.height_pixels)+L" px（撮影ごとに変わりません）\r\n";
    text+=L"継ぎ目の列: "+std::to_wstring(f.seam_column)+L"（"+FixedSeamOriginText(f.seam_origin)+L"）\r\n縦線は「継ぎ目を表示」で桃色になります\r\n\r\n";
    text+=L"倍率の補正\r\n"+FixedLensScaleLine(s)+L"\r\n";
    text+=L"補正の中心 P: 文書 "+StillFixed(s.center_mm.x,2)+L", "+StillFixed(s.center_mm.y,2)+L" mm（出力 "+StillFixed(s.center_output_pixels.x,1)+L", "+StillFixed(s.center_output_pixels.y,1)+L" px）\r\n";
    if(s.estimated_scale)
        text+=L"推定 ×"+StillFixed(*s.estimated_scale,5)+L"（使ったタイル "+std::to_wstring(s.used_tiles)+L"、行 "+std::to_wstring(s.first_used_row)+L"〜"+std::to_wstring(s.last_used_row)+
            L"、残差 RMS "+StillFixed(s.residual_output_pixels.rms,2)+L" px・最大 "+StillFixed(s.residual_output_pixels.maximum,2)+L" px）\r\n倍率で説明できなかったずれ（合成には使っていません） "+
            StillSigned(s.unexplained_shift_mm.x,2)+L", "+StillSigned(s.unexplained_shift_mm.y,2)+L" mm（出力 "+StillSigned(s.unexplained_shift_output_pixels.x,2)+L", "+StillSigned(s.unexplained_shift_output_pixels.y,2)+L" px）\r\n";
    text+=FixedPolicyLine(f.policy_limit,s.limit_percent)+L"\r\n\r\n";
    text+=L"倍率の推定に使った一致タイル "+std::to_wstring(f.tiles_agreed)+L"/"+std::to_wstring(f.tiles_matched)+L"/"+std::to_wstring(f.tiles_searched)+L"\r\n";
    return text+L"再Merge・位置/回転/倍率の調整は、この報告では使えません（校正の写像で描いています）\r\n\r\n";
}
}
