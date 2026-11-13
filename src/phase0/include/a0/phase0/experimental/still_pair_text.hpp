#pragma once
#include "a0/phase0/experimental/still_pair.hpp"
#include <cstdint>
#include <cwchar>
#include <string>

// Operator-facing sentences for a pair capture that stopped before any shutter (#430). They name,
// per body, what the camera answered and what to do next, in plain Japanese with the notice
// code in brackets. They carry no path, no device identifier and no notice parameter.
// Compile this header with /utf-8.
namespace a0::phase0::experimental {
namespace still_pair_text_detail {
inline std::wstring CodeText(std::uint16_t code){
    wchar_t text[16]{};std::swprintf(text,16,L"0x%04X",static_cast<unsigned>(code));return text;
}
inline std::wstring BodyName(int index){return index==0?L"A":L"B";}
inline bool IsNoticeEvent(const VerifiedStillPreflight& status){
    return status.kind==StillPreflightKind::Event && !(status.event_code && *status.event_code==0xc101);
}
inline bool IsImageEvent(const VerifiedStillPreflight& status){
    return status.kind==StillPreflightKind::Event && status.event_code && *status.event_code==0xc101;
}
inline std::wstring EventSentence(const VerifiedStillPreflight& status){
    if(!status.event_code)return L"想定していない知らせがありました。";
    const auto code=*status.event_code;
    if(code==0xc102 || code==0x400d)return L"前の撮影の『撮影終了』の知らせが本体に残っていました（"+CodeText(code)+L"）。";
    if(code==0x4005)return L"本体から『記録先が外れた』という知らせが届いていました（"+CodeText(code)+L"）。USB を抜き差しした後に出ることがあります。";
    if(code==0xc101)return L"PC に取り込んでいない画像が本体にある、という知らせがありました（"+CodeText(code)+L"）。";
    return L"想定していない知らせがありました（"+CodeText(code)+L"）。";
}
inline std::wstring NotCheckedReason(const VerifiedStillPair& pair,int index){
    const auto& other=pair.preflight[1-index];
    if(pair.preflight_checked[1-index] && (other.kind==StillPreflightKind::Identity || other.kind==StillPreflightKind::Transport || other.kind==StillPreflightKind::Close))
        return L"（"+BodyName(1-index)+L" の接続を確かめられなかったため）";
    return L"（制限時間または停止の要求のため）";
}
inline std::wstring BodyLine(const VerifiedStillPair& pair,int index){
    const auto name=BodyName(index)+L"：";
    if(!pair.preflight_checked[index])return name+L"点検していません"+NotCheckedReason(pair,index)+L"。";
    const auto& status=pair.preflight[index];
    if(status.ready && status.closed)return name+L"点検は通りました。";
    switch(status.kind){
    case StillPreflightKind::Event:return name+EventSentence(status);
    case StillPreflightKind::NotSettled:return name+L"本体の状態が続けて変わったので止めました。";
    case StillPreflightKind::Quality:return name+L"画質が JPEG Fine になっていません。本体の設定を直してください。";
    case StillPreflightKind::Size:return name+L"画像サイズが L になっていません。本体の設定を直してください。";
    case StillPreflightKind::LiveViewOn:return name+L"ライブビューが止まり切っていませんでした。";
    case StillPreflightKind::NotReady:return name+L"本体が 10 秒たっても準備中のままでした。";
    case StillPreflightKind::OutOfFocus:return name+L"ピントが合っていないと本体が答えました。";
    case StillPreflightKind::Identity:return name+L"登録済みのカメラとして確かめられませんでした。";
    case StillPreflightKind::Transport:return name+L"本体と通信できませんでした。";
    case StillPreflightKind::Close:return name+L"点検の後の接続の終了を確かめられませんでした。";
    case StillPreflightKind::DeadlineStop:return name+L"制限時間または停止の要求で点検を終えられませんでした。";
    default:return name+L"点検を終えられませんでした。";
    }
}
}
// The status text of a pair whose preflight stopped it: one line saying nothing was exposed, one
// line per body, and what to do next. Empty when the preflight did not stop the pair or no body
// was checked, so the caller keeps its existing text.
inline std::wstring DescribeStillPairPreflightRefusal(const VerifiedStillPair& pair){
    using namespace still_pair_text_detail;
    if(pair.complete || pair.preflight_complete || pair.shutter_dispatched[0] || pair.shutter_dispatched[1])return {};
    bool any_refused=false;
    for(int i=0;i<2;++i)if(pair.preflight_checked[i] && !(pair.preflight[i].ready && pair.preflight[i].closed))any_refused=true;
    if(!any_refused)return {};
    std::wstring text=L"撮影前の点検で止めました。A・B どちらもシャッターは切っていません。";
    bool escalate=false,notice=false,settings=false,focus=false;
    for(int i=0;i<2;++i){
        text+=L"\n"+BodyLine(pair,i);
        if(!pair.preflight_checked[i]){escalate=true;continue;}
        const auto& status=pair.preflight[i];
        if(status.ready && status.closed)continue;
        switch(status.kind){
        case StillPreflightKind::Event:if(IsImageEvent(status))escalate=true;else notice=true;break;
        case StillPreflightKind::NotSettled:notice=true;break;
        case StillPreflightKind::Quality:case StillPreflightKind::Size:case StillPreflightKind::LiveViewOn:case StillPreflightKind::NotReady:settings=true;break;
        case StillPreflightKind::OutOfFocus:focus=true;break;
        default:escalate=true;break;
        }
    }
    // One next step only: the status row holds four lines at the default window width. When the bodies call for different
    // steps, or one cannot be checked, the general one is said.
    const int steps=(notice?1:0)+(settings?1:0)+(focus?1:0);
    if(escalate || steps>1)text+=L"\nこの画面を閉じて、担当者に知らせてください。";
    else if(notice)text+=L"\n残っていた知らせは、この点検で読み取りました。この画面を閉じてアプリを起動し直すと、次は通る見込みです。また止まる時は、担当者に知らせてください。";
    else if(settings)text+=L"\n本体の設定を直し、この画面を閉じてアプリを起動し直してから、もう一度撮影してください。";
    else if(focus)text+=L"\nこの画面を閉じてライブを再起動し、AFで確認してください。";
    return text;
}
}
