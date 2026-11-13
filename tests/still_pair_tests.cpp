#include "a0/phase0/experimental/still_pair.hpp"
#include "a0/phase0/experimental/still_pair_text.hpp"
#include "a0/phase0/experimental/pair_cycle_probe.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace a0::phase0::experimental;
namespace {
using Clock=std::chrono::steady_clock;
struct Scripted {
    // What each step does for each body; unset means success.
    std::array<bool,2> preflight_ready{true,true},preflight_throws{},prepare_ready{true,true},prepare_throws{},collect_ok{true,true},collect_throws{},close_throws{};
    std::array<bool,2> preflight_out_of_focus{},prepare_out_of_focus{};
    // #430: why a refused preflight stopped, whether its Close was confirmed, and a hook that runs
    // after each preflight step (to revoke admission or let the deadline pass).
    std::array<StillPreflightKind,2> preflight_kind{};std::array<std::optional<std::uint16_t>,2> preflight_code{};std::array<bool,2> preflight_unclosed{};
    std::function<void(int)> after_preflight;
    std::array<StillShutterState,2> shutter{StillShutterState::Sent,StillShutterState::Sent};
    std::array<bool,2> shutter_throws{};
    std::vector<std::string> calls,log;
    Clock::time_point origin=Clock::now();
    std::chrono::milliseconds gap{137};
};
VerifiedStillOriginal Original(int i){return VerifiedStillOriginal{true,false,i==0?"CAM-A/original.jpg":"CAM-B/original.jpg",100,std::string(64,i==0?'a':'b'),{}};}
VerifiedStillPair Run(Scripted& s,std::function<bool()> admission=[]{return true;},Clock::time_point deadline=Clock::now()+std::chrono::seconds(10)){
    StillPairSteps steps;
    const auto tag=[](const char* step,int i){return std::string(step)+std::to_string(i);};
    steps.preflight=[&](int i,Clock::time_point){
        s.calls.push_back(tag("preflight",i));if(s.preflight_throws[i])throw std::runtime_error("preflight refused");
        VerifiedStillPreflight status{s.preflight_ready[i],!s.preflight_unclosed[i],s.preflight_ready[i]?"":"preflight not ready",s.preflight_out_of_focus[i],s.preflight_kind[i],s.preflight_code[i]};
        if(s.after_preflight)s.after_preflight(i);
        return status;};
    steps.prepare=[&](int i,Clock::time_point){
        s.calls.push_back(tag("prepare",i));if(s.prepare_throws[i])throw std::runtime_error("open refused");
        return StillPairPrepared{s.prepare_ready[i],s.prepare_ready[i]?"":"recheck refused",s.prepare_out_of_focus[i]};};
    steps.shutter=[&](int i){
        s.calls.push_back(tag("shutter",i));if(s.shutter_throws[i])throw std::runtime_error("shutter threw");
        StillShutterOutcome outcome;outcome.state=s.shutter[i];
        if(outcome.state!=StillShutterState::NotSent)outcome.dispatched_at=s.origin+(i==0?std::chrono::milliseconds(0):s.gap);
        if(outcome.state!=StillShutterState::Sent)outcome.error=outcome.state==StillShutterState::Uncertain?"shutter unconfirmed":"shutter refused";
        return outcome;};
    steps.collect=[&](int i){
        s.calls.push_back(tag("collect",i));if(s.collect_throws[i])throw std::runtime_error("collect threw");
        auto original=Original(i);if(!s.collect_ok[i]){original.success=false;original.error="import failed";}return original;};
    steps.rescue=[&](int i){s.calls.push_back(tag("rescue",i));};
    steps.close=[&](int i,VerifiedStillOriginal& original){
        s.calls.push_back(tag("close",i));if(s.close_throws[i])throw std::runtime_error("close refused");original.closed=true;};
    steps.log=[&](const std::string& line){s.log.push_back(line);};
    return CaptureStillPairShutterFirstOnce(steps,admission,deadline);
}
using Calls=std::vector<std::string>;
bool Called(const Scripted& s,const std::string& call){for(const auto& c:s.calls)if(c==call)return true;return false;}
}
int main(){
    int checks=0,failures=0;auto check=[&](bool value){++checks;if(!value)++failures;};
    // Order: preflight both, open+recheck both, shutter A then B, import A then B,
    // close both. Both shutters precede either import.
    {Scripted s;const auto r=Run(s);
        check(r.complete && r.preflight_complete && r.error.empty() && r.shutter_dispatched[0] && r.shutter_dispatched[1]);
        check(s.calls==Calls{"preflight0","preflight1","prepare0","prepare1","shutter0","shutter1","collect0","collect1","close0","close1"});
        check(r.shutter_gap_ms==137 && s.log==std::vector<std::string>{"pair_shutters shutter_gap_ms=137"});
        check(r.originals[0].path=="CAM-A/original.jpg" && r.originals[1].path=="CAM-B/original.jpg" && r.originals[0].sha256!=r.originals[1].sha256);
        check(r.originals[0].closed && r.originals[1].closed);}
    // Preflight (open, read-only check, Close) refusal stops both bodies before any exposure.
    for(int refused:{0,1})for(int failure:{0,1,2}){
        Scripted s;if(failure==0)s.preflight_ready[refused]=false;if(failure==1)s.preflight_throws[refused]=true;
        if(failure==2){s.preflight_ready[refused]=false;s.preflight_out_of_focus[refused]=true;}
        const auto r=Run(s);
        check(!r.complete && !r.preflight_complete && !r.error.empty() && !r.shutter_dispatched[0] && !r.shutter_dispatched[1]);
        check(s.calls==(refused==0?Calls{"preflight0"}:Calls{"preflight0","preflight1"}));
        check(r.originals[0].path.empty() && r.originals[1].path.empty() && (failure!=2 || r.out_of_focus));
    }
    // A ready check whose Close is unconfirmed is also a refusal: no exposure.
    for(int refused:{0,1}){
        Scripted t;StillPairSteps steps;steps.preflight=[&](int i,Clock::time_point){return VerifiedStillPreflight{true,i!=refused,{},false};};
        steps.prepare=[&](int,Clock::time_point){t.calls.push_back("prepare");return StillPairPrepared{true,{},false};};
        const auto closed=CaptureStillPairShutterFirstOnce(steps,[]{return true;},Clock::now()+std::chrono::seconds(10));
        check(!closed.complete && t.calls.empty() && closed.error.find("readiness/close unconfirmed")!=std::string::npos);}
    // #430: A refused for the camera's own state with its Close confirmed: B is still checked and
    // closed, so one press reads what both bodies hold. No shutter or exposure recheck either way.
    for(auto kind:{StillPreflightKind::Event,StillPreflightKind::LiveViewOn,StillPreflightKind::Quality,StillPreflightKind::Size,StillPreflightKind::NotSettled,StillPreflightKind::NotReady,StillPreflightKind::OutOfFocus}){
        Scripted s;s.preflight_ready[0]=false;s.preflight_kind[0]=kind;if(kind==StillPreflightKind::Event)s.preflight_code[0]=0xc102;
        s.preflight_out_of_focus[0]=kind==StillPreflightKind::OutOfFocus;
        const auto r=Run(s);
        check(s.calls==Calls{"preflight0","preflight1"} && !r.complete && !r.preflight_complete && !r.shutter_dispatched[0] && !r.shutter_dispatched[1]);
        check(r.preflight_checked[0] && r.preflight_checked[1] && !r.preflight[0].ready && r.preflight[1].ready && r.preflight[1].closed);
        check(r.error=="CAM-A preflight: preflight not ready" && r.out_of_focus==(kind==StillPreflightKind::OutOfFocus) && !r.preflight[1].out_of_focus);
        check(r.originals[0].path.empty() && r.originals[1].path.empty());
        check(s.log.size()==1 && s.log[0].find("pair_preflight_result a=refused b=ready a_kind=")==0 && s.log[0].find("shutter_commands=0")!=std::string::npos);
    }
    {Scripted s;s.preflight_ready[0]=false;s.preflight_kind[0]=StillPreflightKind::Event;s.preflight_code[0]=0xc102;const auto r=Run(s);
        check(!r.complete && s.log==std::vector<std::string>{"pair_preflight_result a=refused b=ready a_kind=event b_kind=none a_code=0xc102 b_code=none shutter_commands=0"});}
    // Both refused: both reasons are in the error and the log, in A then B order.
    {Scripted s;s.preflight_ready={false,false};s.preflight_kind={StillPreflightKind::Event,StillPreflightKind::Event};s.preflight_code={0xc102,0x4005};
        const auto r=Run(s);
        check(s.calls==Calls{"preflight0","preflight1"} && !r.complete && r.preflight_checked[0] && r.preflight_checked[1]);
        check(r.error=="CAM-A preflight: preflight not ready; CAM-B preflight: preflight not ready");
        check(r.preflight[0].event_code==0xc102 && r.preflight[1].event_code==0x4005);
        check(s.log==std::vector<std::string>{"pair_preflight_result a=refused b=refused a_kind=event b_kind=event a_code=0xc102 b_code=0x4005 shutter_commands=0"});}
    // A refused for its own state, B's preflight throws: the pair still stops and names both.
    {Scripted s;s.preflight_ready[0]=false;s.preflight_kind[0]=StillPreflightKind::Event;s.preflight_throws[1]=true;const auto r=Run(s);
        check(s.calls==Calls{"preflight0","preflight1"} && !r.complete && r.error=="CAM-A preflight: preflight not ready; CAM-B preflight: preflight refused");
        check(r.preflight_checked[1] && r.preflight[1].kind==StillPreflightKind::Transport && !r.preflight[1].closed);}
    // A stops for anything but the camera's own state, or its Close is unconfirmed: B is never opened.
    for(auto kind:{StillPreflightKind::None,StillPreflightKind::Identity,StillPreflightKind::Transport,StillPreflightKind::Close,StillPreflightKind::DeadlineStop}){
        Scripted s;s.preflight_ready[0]=false;s.preflight_kind[0]=kind;const auto r=Run(s);
        check(s.calls==Calls{"preflight0"} && !r.complete && !r.preflight_checked[1] && r.error=="CAM-A preflight: preflight not ready");
        check(s.log.size()==1 && s.log[0].find("a=refused b=not_checked")!=std::string::npos);}
    {Scripted s;s.preflight_ready[0]=false;s.preflight_kind[0]=StillPreflightKind::Event;s.preflight_unclosed[0]=true;const auto r=Run(s);
        check(s.calls==Calls{"preflight0"} && !r.preflight_checked[1] && !r.complete);}
    // A stop request or the deadline right after A's refusal: B is not opened, A's refusal is the error.
    {Scripted s;bool active=true;s.preflight_ready[0]=false;s.preflight_kind[0]=StillPreflightKind::Event;s.after_preflight=[&](int i){if(i==0)active=false;};
        const auto r=Run(s,[&]{return active;});
        check(s.calls==Calls{"preflight0"} && !r.complete && !r.preflight_checked[1] && r.error=="CAM-A preflight: preflight not ready");}
    {Scripted s;s.preflight_ready[0]=false;s.preflight_kind[0]=StillPreflightKind::Event;
        s.after_preflight=[&](int i){if(i==0)std::this_thread::sleep_for(std::chrono::milliseconds(80));};
        const auto r=Run(s,[]{return true;},Clock::now()+std::chrono::milliseconds(40));
        check(s.calls==Calls{"preflight0"} && !r.complete && !r.preflight_checked[1] && r.error=="CAM-A preflight: preflight not ready");}
    // A ready and B refused is what it always was: same call order, same error, plus the new log line.
    {Scripted s;s.preflight_ready[1]=false;s.preflight_kind[1]=StillPreflightKind::Event;s.preflight_code[1]=0x4005;const auto r=Run(s);
        check(s.calls==Calls{"preflight0","preflight1"} && r.error=="CAM-B preflight: preflight not ready" && r.preflight_checked[0] && r.preflight_checked[1]);
        check(s.log==std::vector<std::string>{"pair_preflight_result a=ready b=refused a_kind=none b_kind=event a_code=none b_code=0x4005 shutter_commands=0"});}
    // Operator sentences (#430). Codes in brackets; nothing else from the notice.
    {const auto contains=[](const std::wstring& text,const wchar_t* part){return text.find(part)!=std::wstring::npos;};
        const auto both=[&](StillPreflightKind kind_a,std::uint16_t code_a,StillPreflightKind kind_b,std::uint16_t code_b){
            VerifiedStillPair pair;
            for(int i=0;i<2;++i){pair.preflight_checked[i]=true;pair.preflight[i].closed=true;}
            pair.preflight[0].kind=kind_a;pair.preflight[0].ready=kind_a==StillPreflightKind::None;if(code_a)pair.preflight[0].event_code=code_a;
            pair.preflight[1].kind=kind_b;pair.preflight[1].ready=kind_b==StillPreflightKind::None;if(code_b)pair.preflight[1].event_code=code_b;
            return DescribeStillPairPreflightRefusal(pair);};
        const auto leftover=both(StillPreflightKind::Event,0xc102,StillPreflightKind::Event,0xc102);
        check(contains(leftover,L"撮影前の点検で止めました。A・B どちらもシャッターは切っていません。") &&
            contains(leftover,L"A：前の撮影の『撮影終了』の知らせが本体に残っていました（0xC102）。") && contains(leftover,L"B：前の撮影の『撮影終了』の知らせが本体に残っていました（0xC102）。"));
        check(contains(leftover,L"この画面を閉じてアプリを起動し直すと、次は通る見込みです") && !contains(leftover,L"この画面を閉じて、担当者に知らせてください。"));
        const auto removed=both(StillPreflightKind::None,0,StillPreflightKind::Event,0x4005);
        check(contains(removed,L"A：点検は通りました。") && contains(removed,L"B：本体から『記録先が外れた』という知らせが届いていました（0x4005）。USB を抜き差しした後に出ることがあります。"));
        const auto image=both(StillPreflightKind::Event,0xc101,StillPreflightKind::Event,0xc102);
        check(contains(image,L"A：PC に取り込んでいない画像が本体にある、という知らせがありました（0xC101）。") && contains(image,L"この画面を閉じて、担当者に知らせてください。") && !contains(image,L"次は通る見込み"));
        const auto unknown=both(StillPreflightKind::Event,0x4fff,StillPreflightKind::NotSettled,0);
        check(contains(unknown,L"A：想定していない知らせがありました（0x4FFF）。") && contains(unknown,L"B：本体の状態が続けて変わったので止めました。"));
        const auto settings=both(StillPreflightKind::Quality,0,StillPreflightKind::Size,0);
        check(contains(settings,L"A：画質が JPEG Fine になっていません。本体の設定を直してください。") && contains(settings,L"B：画像サイズが L になっていません。本体の設定を直してください。") && contains(settings,L"本体の設定を直し"));
        const auto live=both(StillPreflightKind::LiveViewOn,0,StillPreflightKind::NotReady,0);
        check(contains(live,L"A：ライブビューが止まり切っていませんでした。") && contains(live,L"B：本体が 10 秒たっても準備中のままでした。"));
        const auto focus=both(StillPreflightKind::OutOfFocus,0,StillPreflightKind::None,0);
        check(contains(focus,L"A：ピントが合っていないと本体が答えました。") && contains(focus,L"AFで確認してください"));
        // Causes that call for different steps get the one general step, so the row stays within four lines.
        for(const auto& mixed:{both(StillPreflightKind::Event,0xc102,StillPreflightKind::Quality,0),both(StillPreflightKind::OutOfFocus,0,StillPreflightKind::Event,0xc102),
                both(StillPreflightKind::Size,0,StillPreflightKind::OutOfFocus,0)}){
            check(contains(mixed,L"この画面を閉じて、担当者に知らせてください。") && !contains(mixed,L"次は通る見込み") && !contains(mixed,L"本体の設定を直し、") && !contains(mixed,L"AFで確認してください"));
            check(contains(mixed,L"A：") && contains(mixed,L"B："));}
        // B not checked because A's connection could not be confirmed: said so, and escalated.
        VerifiedStillPair partial;partial.preflight_checked[0]=true;partial.preflight[0].kind=StillPreflightKind::Close;
        const auto skipped=DescribeStillPairPreflightRefusal(partial);
        check(contains(skipped,L"A：点検の後の接続の終了を確かめられませんでした。") && contains(skipped,L"B：点検していません（A の接続を確かめられなかったため）。") && contains(skipped,L"この画面を閉じて、担当者に知らせてください。"));
        // Not a preflight stop: no text, so the screen keeps its earlier sentence.
        VerifiedStillPair done;done.preflight_complete=true;done.preflight_checked={true,true};
        check(DescribeStillPairPreflightRefusal(done).empty() && DescribeStillPairPreflightRefusal(VerifiedStillPair{}).empty());
        VerifiedStillPair exposed;exposed.preflight_checked[0]=true;exposed.preflight[0].kind=StillPreflightKind::Event;exposed.shutter_dispatched[0]=true;
        check(DescribeStillPairPreflightRefusal(exposed).empty());
        // Late image notices (a body that read 0xC101 while waiting for its completion).
        VerifiedStillPair late;late.originals[1].late_object_event=true;
        const auto late_text=DescribeStillPairLateObjects(late);
        check(contains(late_text,L"B：撮影は完了できませんでしたが、本体に取り込んでいない画像が残っているかもしれません。") && !contains(late_text,L"A：") && contains(late_text,L"担当者に知らせてください。"));
        check(DescribeStillPairLateObjects(VerifiedStillPair{}).empty());
        const auto single_late=DescribeStillSingleLateObject();
        check(contains(single_late,L"本体に取り込んでいない画像が残っているかもしれません。") && contains(single_late,L"担当者に知らせてください。") && !contains(single_late,L"A：") && !contains(single_late,L"B："));}
    // Exposure recheck refusal: no shutter for either body; every opened body closes.
    for(int refused:{0,1})for(int failure:{0,1,2}){
        Scripted s;if(failure==0)s.prepare_ready[refused]=false;if(failure==1)s.prepare_throws[refused]=true;
        if(failure==2){s.prepare_ready[refused]=false;s.prepare_out_of_focus[refused]=true;}
        const auto r=Run(s);
        check(!r.complete && r.preflight_complete && !r.shutter_dispatched[0] && !r.shutter_dispatched[1] && !r.shutter_gap_ms && (failure!=2 || r.out_of_focus));
        check(!Called(s,"shutter0") && !Called(s,"shutter1") && !Called(s,"collect0") && !Called(s,"collect1"));
        check(Called(s,"close0") && Called(s,"close1")==(refused==1));
        check(!Called(s,"prepare1") || refused==1);}
    // A's shutter does not complete: B is never exposed, nothing is imported, and
    // an uncertain A (sent but unconfirmed, or thrown) gets one private-partial rescue.
    for(int mode:{0,1,2}){
        Scripted s;if(mode==0)s.shutter[0]=StillShutterState::NotSent;if(mode==1)s.shutter[0]=StillShutterState::Uncertain;if(mode==2)s.shutter_throws[0]=true;
        const auto r=Run(s);
        check(!r.complete && !Called(s,"shutter1") && !Called(s,"collect0") && !Called(s,"collect1") && !r.shutter_gap_ms);
        check(Called(s,"rescue0")==(mode!=0) && !Called(s,"rescue1") && Called(s,"close0") && Called(s,"close1"));
        check(r.shutter_dispatched[0]==(mode!=0) && !r.shutter_dispatched[1] && !r.originals[0].success && !r.originals[1].success && !r.originals[0].error.empty());}
    // B's shutter does not complete: A's original is kept, B is rescued only if its
    // shutter may have exposed, and no shutter is sent twice.
    for(int mode:{0,1,2}){
        Scripted s;if(mode==0)s.shutter[1]=StillShutterState::NotSent;if(mode==1)s.shutter[1]=StillShutterState::Uncertain;if(mode==2)s.shutter_throws[1]=true;
        const auto r=Run(s);
        check(!r.complete && r.originals[0].success && r.originals[0].closed && !r.originals[1].success && !Called(s,"collect1"));
        check(Called(s,"collect0") && Called(s,"rescue1")==(mode!=0) && !Called(s,"rescue0") && Called(s,"close0") && Called(s,"close1"));
        check(r.shutter_dispatched[1]==(mode!=0) && r.shutter_gap_ms.has_value()==(mode==1) && !r.error.empty());
        int shutters=0;for(const auto& c:s.calls)shutters+=c.rfind("shutter",0)==0;check(shutters==2);
        check(s.calls.back()=="close1" && s.log.size()==1);}
    // Import failure of either body: B has been exposed before A is read, so B is
    // still imported once; the pair fails with the first failing body's error and
    // the other body's saved original is kept.
    for(int failed_camera:{0,1})for(int failure:{0,1}){
        Scripted s;if(failure==0)s.collect_ok[failed_camera]=false;else s.collect_throws[failed_camera]=true;
        const auto r=Run(s);
        check(!r.complete && !r.error.empty() && Called(s,"collect0") && Called(s,"collect1") && Called(s,"close0") && Called(s,"close1"));
        check(!r.originals[failed_camera].success && r.originals[1-failed_camera].success && r.originals[1-failed_camera].path==Original(1-failed_camera).path);
        check(!Called(s,"rescue0") && !Called(s,"rescue1"));}
    // OutOfFocus reported by an import is carried to the result.
    {Scripted s;StillPairSteps steps;steps.preflight=[](int,Clock::time_point){return VerifiedStillPreflight{true,true,{},false};};
        steps.prepare=[](int,Clock::time_point){return StillPairPrepared{true,{},false};};
        steps.shutter=[](int){StillShutterOutcome o;o.state=StillShutterState::Sent;o.dispatched_at=Clock::now();return o;};
        steps.collect=[](int i){auto o=Original(i);if(i==0){o.success=false;o.out_of_focus=true;o.error="OutOfFocus";}return o;};
        steps.rescue=[](int){};steps.close=[](int,VerifiedStillOriginal& o){o.closed=true;};
        const auto r=CaptureStillPairShutterFirstOnce(steps,[]{return true;},Clock::now()+std::chrono::seconds(10));
        check(!r.complete && r.out_of_focus && r.error=="OutOfFocus" && r.originals[1].success);}
    // Invalid or unclosed originals never complete a pair.
    for(int mutation:{0,1,2,3,4})for(int camera:{0,1}){
        StillPairSteps steps;steps.preflight=[](int,Clock::time_point){return VerifiedStillPreflight{true,true,{},false};};
        steps.prepare=[](int,Clock::time_point){return StillPairPrepared{true,{},false};};
        steps.shutter=[](int){StillShutterOutcome o;o.state=StillShutterState::Sent;o.dispatched_at=Clock::now();return o;};
        steps.collect=[&](int i){auto o=Original(i);if(i==camera){if(mutation==0)o.success=false;if(mutation==2)o.sha256="";if(mutation==3)o.bytes=0;if(mutation==4)o.sha256=std::string(64,'Z');}return o;};
        steps.rescue=[](int){};steps.close=[&](int i,VerifiedStillOriginal& o){o.closed=!(mutation==1 && i==camera);};
        const auto r=CaptureStillPairShutterFirstOnce(steps,[]{return true;},Clock::now()+std::chrono::seconds(10));
        check(!r.complete && !r.error.empty());}
    // A close that throws fails that body; the other body still closes.
    for(int camera:{0,1}){Scripted s;s.close_throws[camera]=true;const auto r=Run(s);
        check(!r.complete && Called(s,"close0") && Called(s,"close1") && !r.originals[camera].success && r.originals[camera].error.find("session close failed")!=std::string::npos && r.originals[1-camera].success);}
    // Deadline or stop before the start: nothing is called.
    {Scripted s;const auto r=Run(s,[]{return true;},Clock::now()-std::chrono::seconds(1));check(!r.complete && s.calls.empty());}
    {Scripted s;const auto r=Run(s,[]{return false;});check(!r.complete && s.calls.empty());}
    // Admission revoked at each point: before any shutter nothing is exposed and
    // everything opened closes; after A's shutter B is not exposed but A is imported;
    // after both shutters both are imported but the pair is not complete.
    for(int revoked_after:{0,1,2,3,4}){
        Scripted s;bool active=true;StillPairSteps steps;
        steps.preflight=[&](int i,Clock::time_point){s.calls.push_back("preflight"+std::to_string(i));if(revoked_after==i)active=false;return VerifiedStillPreflight{true,true,{},false};};
        steps.prepare=[&](int i,Clock::time_point){s.calls.push_back("prepare"+std::to_string(i));if(revoked_after==2+i)active=false;return StillPairPrepared{true,{},false};};
        steps.shutter=[&](int i){s.calls.push_back("shutter"+std::to_string(i));if(i==0 && revoked_after==4)active=false;StillShutterOutcome o;o.state=StillShutterState::Sent;o.dispatched_at=Clock::now();return o;};
        steps.collect=[&](int i){s.calls.push_back("collect"+std::to_string(i));return Original(i);};
        steps.rescue=[](int){};steps.close=[&](int i,VerifiedStillOriginal& o){s.calls.push_back("close"+std::to_string(i));o.closed=true;};
        const auto r=CaptureStillPairShutterFirstOnce(steps,[&]{return active;},Clock::now()+std::chrono::seconds(10));
        check(!r.complete);
        if(revoked_after<=3){check(!Called(s,"shutter0") && !Called(s,"shutter1"));}
        if(revoked_after==4){check(Called(s,"shutter0") && !Called(s,"shutter1") && Called(s,"collect0") && !Called(s,"collect1"));}
        // Only bodies that were opened are closed; B is never opened if admission
        // ended before its turn.
        check(Called(s,"close0")==Called(s,"prepare0") && Called(s,"close1")==Called(s,"prepare1"));
        if(revoked_after==1)check(!Called(s,"prepare0"));
        if(revoked_after==2)check(Called(s,"prepare0") && !Called(s,"prepare1"));
        if(revoked_after>=3)check(Called(s,"prepare0") && Called(s,"prepare1"));}
    {Scripted s;bool active=true;StillPairSteps steps;
        steps.preflight=[](int,Clock::time_point){return VerifiedStillPreflight{true,true,{},false};};
        steps.prepare=[](int,Clock::time_point){return StillPairPrepared{true,{},false};};
        steps.shutter=[&](int i){if(i==1)active=false;StillShutterOutcome o;o.state=StillShutterState::Sent;o.dispatched_at=Clock::now();return o;};
        steps.collect=[&](int i){s.calls.push_back("collect"+std::to_string(i));return Original(i);};
        steps.rescue=[](int){};steps.close=[](int,VerifiedStillOriginal& o){o.closed=true;};
        const auto r=CaptureStillPairShutterFirstOnce(steps,[&]{return active;},Clock::now()+std::chrono::seconds(10));
        check(!r.complete && r.error=="pair completed after deadline/admission" && s.calls==Calls{"collect0","collect1"} && r.originals[0].success && r.originals[1].success);}
    PairCycleProbe probe;
    PairCycleObservation view{false,true,true,true,1,0,0,0,5};
    check(probe.Next(view)==PairCycleAction::Capture);
    check(probe.Next(view)==PairCycleAction::Wait); // Never send the same shutter twice.
    view.idle=false;check(probe.Next(view)==PairCycleAction::Wait);
    view.idle=true;view.sessions=2;view.reviews=1;view.saved=2;view.review_paints=5;
    view.live_ready=false;check(probe.Next(view)==PairCycleAction::Wait);
    view.live_ready=true;check(probe.Next(view)==PairCycleAction::ShowLive);
    check(probe.Next(view)==PairCycleAction::Wait); // Old C paints cannot prove resumed C.
    view.merge_paints=10;view.merge_ready=false;check(probe.Next(view)==PairCycleAction::Wait);
    view.merge_ready=true;check(probe.Next(view)==PairCycleAction::Wait);
    check(probe.Completed()==1);
    check(probe.Next(view)==PairCycleAction::Capture);
    check(probe.Next(view)==PairCycleAction::Wait);
    view.sessions=3;view.reviews=2;view.saved=4;
    check(probe.Next(view)==PairCycleAction::Wait); // Previous static review cannot count.
    view.review_paints=10;check(probe.Next(view)==PairCycleAction::ShowLive);
    check(probe.Next(view)==PairCycleAction::Wait);
    view.merge_paints=15;check(probe.Next(view)==PairCycleAction::Complete);
    check(probe.Completed()==2 && probe.Next(view)==PairCycleAction::Complete);
    for(int stage:{0,1,2}){
        PairCycleProbe failed;PairCycleObservation state{false,true,true,true,1,0,0,0,5};
        if(stage>0)check(failed.Next(state)==PairCycleAction::Capture);
        if(stage>1){state.sessions=2;state.reviews=1;state.saved=2;state.review_paints=5;check(failed.Next(state)==PairCycleAction::ShowLive);}
        state.failed=true;check(failed.Next(state)==PairCycleAction::Fail);
        state.failed=false;check(failed.Next(state)==PairCycleAction::Fail); // Failure is terminal.
    }
    PairCycleProbe unexpected;view.sessions=1;view.reviews=1;view.saved=2;
    check(unexpected.Next(view)==PairCycleAction::Fail);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
