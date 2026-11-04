#pragma once
#include "a0/m2/experimental/fixed_scale_correction.hpp"
#include "a0/phase0/experimental/still_alignment_text.hpp"
#include <Windows.h>
#include <string>
#include <string_view>

namespace a0::phase0::experimental {
// Display text for a fixed composite (#400). Rounded for reading; the report
// keeps the full values. Never states a quality pass. The condition is shown as
// unapproved, as its record is.
inline std::wstring WidenUtf8(std::string_view text){
    if(text.empty())return {};
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(size<=0)throw std::invalid_argument("text is not UTF-8");
    std::wstring wide(static_cast<size_t>(size),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),wide.data(),size);
    return wide;
}
inline std::wstring FixedSeamOriginText(std::string_view origin){
    return origin=="operator"?L"人が決めた値":L"帯の中央の仮の値";
}
inline std::wstring FixedScaleLine(const a0::m2::experimental::FixedScaleCorrection& s){
    using S=a0::m2::experimental::FixedScaleStatus;
    const auto limit=L"上限 "+StillFixed(s.limit_percent,2)+L"%";
    std::wstring text;
    switch(s.status){
    case S::applied:text=L"倍率を ×"+StillFixed(s.applied_scale,5)+L" 補正しました（100% との差 "+StillFixed(*s.difference_percent,3)+L"%、"+limit+L"）";break;
    case S::exceeded:text=L"倍率は補正していません（100% との差 "+StillFixed(*s.difference_percent,3)+L"%、"+limit+L"）";break;
    case S::not_estimated:
    case S::seam_outside_overlap:
    case S::output_uncovered:text=L"倍率は補正していません（"+limit+L"）";break;
    }
    const auto notice=a0::m2::experimental::FixedScaleNotice(s.status);
    if(!notice.empty())text+=L" / "+WidenUtf8(notice);
    return text;
}
inline std::wstring FixedPolicyLine(const a0::m2::experimental::FixedPolicyLimit& p,double condition_percent){
    using C=a0::m2::experimental::FixedPolicyCheck;
    if(p.check==C::same)return L"方針ファイルの倍率の上限: 固定の合成条件と同じ（"+StillFixed(condition_percent,2)+L"%）";
    const auto notice=WidenUtf8(a0::m2::experimental::FixedPolicyNotice(p.check));
    if(p.check==C::differs)return notice+L"（方針ファイル "+StillFixed(*p.policy_percent,2)+L"% / 固定の合成条件 "+StillFixed(condition_percent,2)+L"%。合成は固定の合成条件の値で行いました）";
    return notice+L"（固定の合成条件 "+StillFixed(condition_percent,2)+L"%）";
}
// The inspector's fields and residuals are computed for the fixed mapping, so an applied scale is not in them.
inline std::wstring FixedScaleInputNote(const a0::m2::experimental::FixedScaleCorrection& s){
    return L"倍率の補正 ×"+StillFixed(s.applied_scale,5)+L" は入力欄に含まれません（入力欄と「調整値での残差」は固定の写像で計算します）";
}
// One line for the capture status of the preview window.
inline std::wstring FixedCompositeLine(const a0::m2::experimental::SavedFixedComposite& f){
    using C=a0::m2::experimental::FixedPolicyCheck;
    std::wstring text=L"固定の合成条件（未承認）で合成: 継ぎ目の列 "+std::to_wstring(f.seam_column)+L"（"+FixedSeamOriginText(f.seam_origin)+L"、桃色の縦線） / "+FixedScaleLine(f.scale);
    if(f.policy_limit.check!=C::same)text+=L" / "+FixedPolicyLine(f.policy_limit,f.scale.limit_percent);
    return text;
}
// Multi-line block (CRLF) for the inspector's side panel.
inline std::wstring FixedCompositeDetails(const a0::m2::experimental::SavedStillReview& r){
    const auto& f=*r.fixed_composite;
    std::wstring text=L"合成（保存JPEG）: 固定の合成条件（未承認）\r\n条件 SHA-256: "+std::wstring(f.condition_sha256.begin(),f.condition_sha256.begin()+12)+L"…\r\n";
    text+=L"継ぎ目の列: "+std::to_wstring(f.seam_column)+L"（"+FixedSeamOriginText(f.seam_origin)+L"）\r\n縦線は「継ぎ目を表示」で桃色になります\r\n\r\n";
    text+=L"倍率の補正\r\n"+FixedScaleLine(f.scale)+L"\r\n";
    if(f.scale.estimated_scale)text+=L"推定 ×"+StillFixed(*f.scale.estimated_scale,5)+L"（使ったタイル "+std::to_wstring(f.scale.used_tiles)+L"、残差 RMS "+StillFixed(f.scale.residual.rms,2)+L" px）\r\n倍率で説明できなかったずれ（Pを原点・合成には使っていません） "+StillSigned(f.scale.unexplained_shift.x,2)+L", "+StillSigned(f.scale.unexplained_shift.y,2)+L" px\r\n";
    if(f.scale.applied_scale!=1)text+=FixedScaleInputNote(f.scale)+L"\r\n";
    text+=FixedPolicyLine(f.policy_limit,f.scale.limit_percent)+L"\r\n\r\n";
    const auto& d=f.diagnostic;
    text+=L"診断（合成の位置には使っていません）\r\n";
    const auto delta=a0::m2::experimental::FixedDiagnosticDelta(d,r.translation_x,r.translation_y,r.rotation_degrees,r.scale);
    if(!delta)text+=std::wstring(L"推定できません（")+StillUndeterminedText(d.reason)+L"）\r\n";
    else text+=L"固定の位置からの動き\r\n平行移動 "+StillSigned(delta->x,1)+L", "+StillSigned(delta->y,1)+L" px\r\n回転 "+StillSigned(delta->rotation_degrees,3)+L"°\r\n倍率 ×"+StillFixed(delta->scale_ratio,5)+
        L"\r\n一致タイル "+std::to_wstring(d.tiles_agreed)+L"/"+std::to_wstring(d.tiles_matched)+L"/"+std::to_wstring(d.tiles_searched)+L"、残差 RMS "+StillFixed(d.similarity_residual.rms,2)+L" px\r\n";
    return text+L"\r\n";
}
}
