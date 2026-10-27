#pragma once
#include "a0/m2/experimental/still_review.hpp"
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace a0::phase0::experimental {
// Display text for the automatic still alignment (#365). Rounded for reading;
// reports keep the full values. Never states a quality pass.
inline std::wstring StillFixed(double value,int digits){
    char buffer[64]{};const auto r=std::to_chars(buffer,buffer+sizeof(buffer),value,std::chars_format::fixed,digits);
    if(r.ec!=std::errc{})throw std::invalid_argument("number formatting failed");return std::wstring(buffer,r.ptr);
}
inline std::wstring StillSigned(double value,int digits){return (value>=0?L"+":L"")+StillFixed(value,digits);}
// The estimated rotation turns B, positive clockwise on the merged screen, so
// returning B means that sense. The camera body's sense depends on mounting.
inline std::wstring StillRotationAdvice(double degrees){
    return std::wstring(L"Bを合成画面で見て")+(degrees>=0?L"時計回り":L"反時計回り")+L"に"+StillFixed(std::abs(degrees),2)+L"°戻す向きです";
}
inline const wchar_t* StillUndeterminedText(a0::m2::experimental::SimilarityUndeterminedReason reason){
    using R=a0::m2::experimental::SimilarityUndeterminedReason;
    switch(reason){
    case R::no_tiles:return L"重なりにタイルを置けません";
    case R::insufficient_tiles:return L"一致したタイルが足りません";
    case R::outside_image:return L"推定値が画像の外です";
    case R::tied_starts:return L"開始位置ごとの結果が同点で区別できません";
    case R::none:break;
    }
    return L"理由不明";
}
inline std::wstring StillAlignmentLine(const a0::m2::experimental::AutomaticStillAlignment& a){
    using D=a0::m2::experimental::StillAlignmentDecision;
    if(!a.similarity)return std::wstring(L"自動位置合わせ: 推定できません（")+StillUndeterminedText(a.reason)+L"）。ライブの位置で合成しました（撮影JPEGで未確認）";
    using S=a0::m2::experimental::StillScaleDecision;
    const auto& s=*a.similarity;
    const auto rotation=L"回転 "+StillSigned(s.rotation_degrees,2)+L"°",scale=L"倍率 "+StillFixed(s.scale*100,2)+L"%";
    std::wstring text;
    if(a.decision==D::corrected && a.scale_decision==S::corrected)
        text=L"自動位置合わせ: "+rotation+L" "+scale+L" を補正しました（上限 回転 "+StillFixed(a.policy->rotation_max_degrees,2)+L"° / 倍率 "+StillFixed(*a.policy->scale_max_percent,2)+L"%）";
    else if(a.decision==D::corrected){
        text=L"自動位置合わせ: "+rotation+L" を補正しました（上限 "+StillFixed(a.policy->rotation_max_degrees,2)+L"°）。";
        if(a.scale_decision!=S::scale_exceeded)text+=scale+L" は上限が未設定のため補正していません";
    }
    else if(a.decision==D::rotation_exceeded)text=L"警告: "+rotation+L" が上限 "+StillFixed(a.policy->rotation_max_degrees,2)+L"° を超えたため補正せず、平行移動だけで合成しました。"+StillRotationAdvice(s.rotation_degrees);
    else text=L"自動位置合わせ: "+rotation+L" "+scale+L"（方針ファイルが無いため補正せず、平行移動だけで合成）";
    if(a.scale_exceeded)text+=(a.decision==D::corrected?L"":L" / ")+std::wstring(L"警告: 倍率が上限を超えたため倍率は補正していません（")+scale+L"、100% との差 "+
        StillFixed(a0::m2::experimental::StillScaleDifferencePercent(s.scale),2)+L"%、上限 "+StillFixed(*a.policy->scale_max_percent,2)+L"%）";
    const auto residual=a0::m2::experimental::AppliedStillResidual(a);
    text+=L"（一致タイル "+std::to_wstring(a.tiles_agreed)+L"、残差 RMS "+StillFixed(residual.rms,1)+L" px）";
    if(a.residual_exceeded)text+=L" / 警告: 相似の後の残差 最大 "+StillFixed(a.similarity_residual.maximum,1)+L" px が警告値 "+StillFixed(*a.policy->residual_max_pixels,1)+L" px を超えています（台形かレンズの歪みの可能性）";
    return text;
}
// Multi-line block (CRLF) for the inspector's side panel.
inline std::wstring StillAlignmentDetails(const a0::m2::experimental::AutomaticStillAlignment& a){
    using D=a0::m2::experimental::StillAlignmentDecision;using S=a0::m2::experimental::StillScaleDecision;
    std::wstring text=L"自動位置合わせ（撮影JPEG・未校正）\r\n";
    if(a.decision==D::corrected)text+=a.scale_decision==S::corrected?L"補正: あり（回転と倍率）\r\n":
        a.scale_decision==S::scale_exceeded?L"補正: あり（回転だけ。倍率は上限超えのため 1）\r\n":L"補正: あり（回転だけ。倍率は上限未設定のため 1）\r\n";
    else text+=a.decision==D::rotation_exceeded?L"補正: なし（回転が上限超え）\r\n":
        a.decision==D::no_policy?L"補正: なし（方針ファイルなし）\r\n":L"補正: なし（推定できず・ライブの位置）\r\n";
    text+=L"一致タイル: "+std::to_wstring(a.tiles_agreed)+L" / 検証済み "+std::to_wstring(a.tiles_matched)+L" / "+std::to_wstring(a.tiles_searched)+L"\r\n";
    if(a.similarity){
        const auto& s=*a.similarity;const auto applied=a0::m2::experimental::AppliedStillAlignment(a);
        text+=L"推定: 回転 "+StillSigned(s.rotation_degrees,3)+L"°（正=時計回り） / 倍率 "+StillFixed(s.scale*100,3)+L"% / B中心 "+StillFixed(s.x,1)+L", "+StillFixed(s.y,1)+L"\r\n";
        text+=L"当てた値: 回転 "+StillSigned(applied.rotation_degrees,3)+L"° / 倍率 "+StillFixed(applied.scale*100,3)+L"% / B中心 "+std::to_wstring(applied.x)+L", "+std::to_wstring(applied.y)+L"\r\n";
        text+=L"残差（相似）: RMS "+StillFixed(a.similarity_residual.rms,1)+L" / 最大 "+StillFixed(a.similarity_residual.maximum,1)+L" px\r\n";
        text+=L"残差（回転と平行移動・倍率 1）: RMS "+StillFixed(a.rigid_residual.rms,1)+L" / 最大 "+StillFixed(a.rigid_residual.maximum,1)+L" px\r\n";
        text+=L"残差（平行移動のみ）: RMS "+StillFixed(a.translation_residual.rms,1)+L" / 最大 "+StillFixed(a.translation_residual.maximum,1)+L" px\r\n";
        text+=a.wide_search?L"開始位置: 撮影JPEGの広い探索\r\n":L"開始位置: ライブの位置\r\n";
    }else text+=std::wstring(L"理由: ")+StillUndeterminedText(a.reason)+L"\r\n";
    if(a.policy){
        text+=L"方針: 回転の上限 "+StillFixed(a.policy->rotation_max_degrees,2)+L"° / 倍率の上限 "+(a.policy->scale_max_percent?StillFixed(*a.policy->scale_max_percent,2)+L"%":std::wstring(L"未設定"))+
            L" / 残差の警告値 "+(a.policy->residual_max_pixels?StillFixed(*a.policy->residual_max_pixels,1)+L" px":std::wstring(L"未設定"))+L"\r\n";
    }else text+=L"方針: ファイルなし（表示のみ）\r\n";
    if(a.rotation_exceeded)text+=L"警告: "+StillRotationAdvice(a.similarity->rotation_degrees)+L"\r\n";
    if(a.scale_exceeded)text+=L"警告: 倍率が上限を超えたため倍率は補正していません。B の高さかピントが A と違う可能性があります\r\n";
    if(a.residual_exceeded)text+=L"警告: 相似の後の残差が警告値を超えています。台形（傾き）かレンズの歪みの可能性があります\r\n";
    return text+L"品質は未評価です。\r\n";
}
// One ASCII log line with the full values.
inline std::string StillAlignmentLog(const a0::m2::experimental::AutomaticStillAlignment& a,double milliseconds){
    namespace x=a0::m2::experimental;
    std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(9);
    o<<"pair_review_alignment status="<<x::OverlapEstimateStatusCode(a.status)<<" reason="<<x::SimilarityUndeterminedReasonCode(a.reason)<<" decision="<<x::StillAlignmentDecisionCode(a.decision)
        <<" scale_decision="<<x::StillScaleDecisionCode(a.scale_decision)
        <<" start="<<(a.wide_search?"still-wide-search":"live-preview")<<" tiles_agreed="<<a.tiles_agreed<<" tiles_matched="<<a.tiles_matched<<" tiles_searched="<<a.tiles_searched;
    if(a.similarity){const auto applied=x::AppliedStillAlignment(a);
        o<<" rotation_degrees="<<a.similarity->rotation_degrees<<" scale="<<a.similarity->scale<<" x="<<a.similarity->x<<" y="<<a.similarity->y
        <<" rigid_x="<<a.rigid->x<<" rigid_y="<<a.rigid->y<<" applied_rotation_degrees="<<applied.rotation_degrees<<" applied_scale="<<applied.scale<<" applied_x="<<applied.x<<" applied_y="<<applied.y
        <<" residual_rms="<<a.similarity_residual.rms<<" residual_max="<<a.similarity_residual.maximum<<" rigid_rms="<<a.rigid_residual.rms<<" rigid_max="<<a.rigid_residual.maximum
        <<" translation_only_rms="<<a.translation_residual.rms<<" translation_only_max="<<a.translation_residual.maximum;}
    o<<" policy="<<(a.policy?a.policy->sha256:std::string("none"));
    if(a.policy){o<<" scale_max_percent=";if(a.policy->scale_max_percent)o<<*a.policy->scale_max_percent;else o<<"none";}
    o<<" rotation_exceeded="<<a.rotation_exceeded<<" scale_exceeded="<<a.scale_exceeded<<" residual_exceeded="<<a.residual_exceeded<<" ms="<<milliseconds<<" quality_accepted=0";
    return o.str();
}
}
