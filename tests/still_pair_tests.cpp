#include "a0/phase0/experimental/still_pair.hpp"
#include "a0/phase0/experimental/pair_cycle_probe.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace a0::phase0::experimental;
namespace {
using Clock=std::chrono::steady_clock;
struct Scripted {
    // What each step does for each body; unset means success.
    std::array<bool,2> preflight_ready{true,true},preflight_throws{},prepare_ready{true,true},prepare_throws{},collect_ok{true,true},collect_throws{},close_throws{};
    std::array<bool,2> preflight_out_of_focus{},prepare_out_of_focus{};
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
        VerifiedStillPreflight status{s.preflight_ready[i],true,s.preflight_ready[i]?"":"preflight not ready",s.preflight_out_of_focus[i]};return status;};
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
