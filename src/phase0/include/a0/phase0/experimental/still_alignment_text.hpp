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
    const auto& s=*a.similarity;
    const auto values=L"回転 "+StillSigned(s.rotation_degrees,2)+L"° 倍率 "+StillFixed(s.scale*100,2)+L"%";
    std::wstring text;
    if(a.decision==D::corrected)text=L"自動位置合わせ: "+values+L" を補正しました（上限 "+StillFixed(a.policy->rotation_max_degrees,2)+L"°）";
    else if(a.decision==D::rotation_exceeded)text=L"警告: 回転 "+StillSigned(s.rotation_degrees,2)+L"° が上限 "+StillFixed(a.policy->rotation_max_degrees,2)+L"° を超えたため補正せず、平行移動だけで合成しました。"+StillRotationAdvice(s.rotation_degrees);
    else text=L"自動位置合わせ: "+values+L"（方針ファイルが無いため補正せず、平行移動だけで合成）";
    if(a.residual_exceeded)text+=L" / 警告: 相似の後の残差 最大 "+StillFixed(a.similarity_residual.maximum,1)+L" px が警告値 "+StillFixed(*a.policy->residual_max_pixels,1)+L" px を超えています（台形かレンズの歪みの可能性）";
    return text;
}
// Multi-line block (CRLF) for the inspector's side panel.
inline std::wstring StillAlignmentDetails(const a0::m2::experimental::AutomaticStillAlignment& a){
    using D=a0::m2::experimental::StillAlignmentDecision;
    std::wstring text=L"自動位置合わせ（撮影JPEG・未校正）\r\n";
    text+=a.decision==D::corrected?L"補正: あり（相似）\r\n":a.decision==D::rotation_exceeded?L"補正: なし（回転が上限超え）\r\n":
        a.decision==D::no_policy?L"補正: なし（方針ファイルなし）\r\n":L"補正: なし（推定できず・ライブの位置）\r\n";
    text+=L"一致タイル: "+std::to_wstring(a.tiles_agreed)+L" / 検証済み "+std::to_wstring(a.tiles_matched)+L" / "+std::to_wstring(a.tiles_searched)+L"\r\n";
    if(a.similarity){
        const auto& s=*a.similarity;
        text+=L"回転: "+StillSigned(s.rotation_degrees,3)+L"°（正=時計回り）\r\n倍率: "+StillFixed(s.scale*100,3)+L"%\r\nB中心: "+StillFixed(s.x,1)+L", "+StillFixed(s.y,1)+L"\r\n";
        text+=L"残差（相似）: RMS "+StillFixed(a.similarity_residual.rms,1)+L" / 最大 "+StillFixed(a.similarity_residual.maximum,1)+L" px\r\n";
        text+=L"残差（平行移動のみ）: RMS "+StillFixed(a.translation_residual.rms,1)+L" / 最大 "+StillFixed(a.translation_residual.maximum,1)+L" px\r\n";
        text+=a.wide_search?L"開始位置: 撮影JPEGの広い探索\r\n":L"開始位置: ライブの位置\r\n";
    }else text+=std::wstring(L"理由: ")+StillUndeterminedText(a.reason)+L"\r\n";
    if(a.policy){
        text+=L"方針: 回転の上限 "+StillFixed(a.policy->rotation_max_degrees,2)+L"° / 残差の警告値 "+(a.policy->residual_max_pixels?StillFixed(*a.policy->residual_max_pixels,1)+L" px":std::wstring(L"未設定"))+L"\r\n";
    }else text+=L"方針: ファイルなし（表示のみ）\r\n";
    if(a.rotation_exceeded)text+=L"警告: "+StillRotationAdvice(a.similarity->rotation_degrees)+L"\r\n";
    if(a.residual_exceeded)text+=L"警告: 相似の後の残差が警告値を超えています。台形（傾き）かレンズの歪みの可能性があります\r\n";
    return text+L"品質は未評価です。\r\n";
}
// One ASCII log line with the full values.
inline std::string StillAlignmentLog(const a0::m2::experimental::AutomaticStillAlignment& a,double milliseconds){
    namespace x=a0::m2::experimental;
    std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(9);
    o<<"pair_review_alignment status="<<x::OverlapEstimateStatusCode(a.status)<<" reason="<<x::SimilarityUndeterminedReasonCode(a.reason)<<" decision="<<x::StillAlignmentDecisionCode(a.decision)
        <<" start="<<(a.wide_search?"still-wide-search":"live-preview")<<" tiles_agreed="<<a.tiles_agreed<<" tiles_matched="<<a.tiles_matched<<" tiles_searched="<<a.tiles_searched;
    if(a.similarity)o<<" rotation_degrees="<<a.similarity->rotation_degrees<<" scale="<<a.similarity->scale<<" x="<<a.similarity->x<<" y="<<a.similarity->y
        <<" residual_rms="<<a.similarity_residual.rms<<" residual_max="<<a.similarity_residual.maximum<<" translation_only_rms="<<a.translation_residual.rms<<" translation_only_max="<<a.translation_residual.maximum;
    o<<" policy="<<(a.policy?a.policy->sha256:std::string("none"))<<" rotation_exceeded="<<a.rotation_exceeded<<" residual_exceeded="<<a.residual_exceeded<<" ms="<<milliseconds<<" quality_accepted=0";
    return o.str();
}
}
