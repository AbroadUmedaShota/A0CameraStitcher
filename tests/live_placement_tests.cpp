// The machine that holds the one position of camera B that the live C is drawn with (#437 stage 1). No window, camera, file, clock or thread:
// the calls are made by the test, in an order that it chooses, and every answer is read back from the machine.
#include "a0/phase0/experimental/live_placement.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace a0::phase0::experimental;
namespace ex=a0::m2::experimental;
namespace cr=a0::m2::experimental::content_registration;
using Pose=cr::PoseParameters;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr)
template<class Call> bool ThrowsInvalid(Call call){
    try{call();}catch(const std::invalid_argument&){return true;}
    return false;
}
#define REJECT(expr) Check(ThrowsInvalid([&]{(void)(expr);}),"rejects: " #expr)

const std::string kSha(64,'a'),kOtherSha(64,'b');
const Pose kBase{0.1,-0.2,0.3,1.0,-2.0,0.4};

LiveEstimateOutcome Usable(const Pose& pose,const ex::SurfacePlane& surface={}){
    LiveEstimateOutcome o;
    o.usable=true;o.pose=pose;o.surface=surface;
    return o;
}
LiveEstimateOutcome Unusable(const char* reason){LiveEstimateOutcome o;o.reasons.emplace_back(reason);return o;}
// The nine numbers (the pose and the surface); layer 2 is never compared: the machine never keeps it.
bool Same(const ex::RegisteredMapping& a,const ex::RegisteredMapping& b){return LivePlacementSameMapping(a,b);}

LivePlacementMachine Started(bool live=true){
    LivePlacementMachine machine;
    machine.BeginSession(kSha,kBase,live);
    return machine;
}
// The machine as it is after the estimate of the first session has been asked for, run and drawn.
LivePlacementMachine Placed(const Pose& pose,const ex::SurfacePlane& surface={}){
    auto machine=Started();
    const auto job=machine.BeginJob();
    machine.Accept(*job,Usable(pose,surface));
    machine.Committed();
    return machine;
}

// 1. The first session: P is the condition's position, and one estimate is asked for.
void FirstSession(){
    LivePlacementMachine machine;
    CHECK(!machine.Configured());
    machine.BeginSession(kSha,kBase,true);
    CHECK(machine.Configured() && machine.ConditionSha256()==kSha);
    const auto view=machine.View();
    CHECK(view.source==LivePlacementSource::fixed_condition && Same(view.mapping,ex::RegisteredMapping{kBase,{},std::nullopt}) && !view.mapping.parallax);
    CHECK(view.activity==LivePlacementActivity::requested && view.ask==LiveEstimateAsk::start && view.outcome==LivePlacementOutcomeKind::none);
    CHECK(Same(machine.Wanted(),view.mapping) && !machine.HasPending());
    const auto job=machine.BeginJob();
    CHECK(job && job->kind==LiveJobKind::full && job->full_reason==LiveFullReason::session_start && job->base_pose.rx_deg==kBase.rx_deg &&
        job->start_pose.tz_percent==kBase.tz_percent && job->start_surface.IsZero());
    CHECK(machine.Activity()==LivePlacementActivity::estimating);
    CHECK(!machine.BeginJob());   // one job at a time
    // The same call twice is the same machine: a session with no condition to read refuses.
    REJECT(LivePlacementMachine().BeginSession(std::string(),kBase,true));
    REJECT(LivePlacementMachine().BeginSession(kSha,Pose{std::numeric_limits<double>::quiet_NaN(),0,0,0,0,0},true));
}

// 2. A usable estimate waits to be drawn; P is replaced when it has been drawn and not before.
void PlacedAfterTheDraw(){
    auto machine=Started();
    const auto job=machine.BeginJob();
    const Pose pose{0.3,-0.1,0.5,2.0,-1.0,0.2};
    const ex::SurfacePlane surface{4.0,0.001,-0.002};
    const auto serial=machine.Serial(),wanted=machine.WantedSerial();
    CHECK(machine.Accept(*job,Usable(pose,surface))==LivePlacementMachine::AcceptKind::pending_draw);
    CHECK(machine.HasPending() && machine.WantedSerial()==wanted+1 && Same(machine.Wanted(),ex::RegisteredMapping{pose,surface,std::nullopt}));
    // Not yet P: what the screen reads is still the condition's position, and the sentence still says that it is working.
    CHECK(machine.Serial()==serial && machine.Source()==LivePlacementSource::fixed_condition && Same(machine.Placement(),ex::RegisteredMapping{kBase,{},std::nullopt}));
    CHECK(machine.Activity()==LivePlacementActivity::estimating);
    machine.Committed();
    const auto view=machine.View();
    CHECK(!machine.HasPending() && view.serial==serial+1 && view.source==LivePlacementSource::auto_start && Same(view.mapping,ex::RegisteredMapping{pose,surface,std::nullopt}));
    CHECK(view.activity==LivePlacementActivity::idle && view.outcome==LivePlacementOutcomeKind::placed && view.outcome_ask==LiveEstimateAsk::start && view.last_failure.empty());
    CHECK(machine.WantedSerial()==wanted+1);   // a pending position that becomes P does not change what is wanted
    CHECK(view.counters.jobs==1 && view.counters.placed==1 && view.counters.not_decided==0);
    // A call with nothing pending changes nothing.
    machine.Committed();
    CHECK(machine.Serial()==serial+1);
}

// 3. An estimate that decides nothing leaves P as it is, says why, and asks for nothing more.
void NotDecided(){
    auto machine=Placed(Pose{0.3,0,0,1.0,0,0});
    const auto before=machine.View();
    CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    auto job=machine.BeginJob();
    CHECK(job && job->full_reason==LiveFullReason::button);
    CHECK(machine.Accept(*job,Unusable("too-dark"))==LivePlacementMachine::AcceptKind::not_decided);
    auto view=machine.View();
    CHECK(Same(view.mapping,before.mapping) && view.serial==before.serial && view.source==before.source);
    CHECK(view.activity==LivePlacementActivity::idle && view.outcome==LivePlacementOutcomeKind::not_decided && view.outcome_ask==LiveEstimateAsk::button);
    CHECK(view.last_failure==std::vector<std::string>{"too-dark"} && !machine.HasPending());
    CHECK(!machine.BeginJob());   // no retry
    // A pose that is not finite is not a position.
    CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    job=machine.BeginJob();
    auto bad=Usable(Pose{std::numeric_limits<double>::quiet_NaN(),0,0,0,0,0});
    CHECK(machine.Accept(*job,bad)==LivePlacementMachine::AcceptKind::not_decided && Same(machine.Placement(),before.mapping));
    bad=Usable(Pose{},ex::SurfacePlane{std::numeric_limits<double>::infinity(),0,0});
    CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    job=machine.BeginJob();
    CHECK(machine.Accept(*job,bad)==LivePlacementMachine::AcceptKind::not_decided && Same(machine.Placement(),before.mapping));
    // The next request clears what the last one said.
    CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    view=machine.View();
    CHECK(view.outcome==LivePlacementOutcomeKind::none && view.last_failure.empty() && view.activity==LivePlacementActivity::requested);
    CHECK(view.counters.not_decided==3);
}

// 4. The estimate is asked for twice in a session, and never because time passed: the button, and only while nothing is asked for.
void AskedOnlyByRequest(){
    auto machine=Started();
    CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::busy);   // the first is still waiting
    auto job=machine.BeginJob();
    CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::busy);   // and running
    machine.Accept(*job,Usable(Pose{0.2,0,0,1.0,0,0}));
    CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::busy);   // and waiting to be drawn
    machine.Committed();
    CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    job=machine.BeginJob();
    machine.Accept(*job,Usable(Pose{0.2,0,0,2.0,0,0}));
    machine.Committed();
    CHECK(machine.Source()==LivePlacementSource::auto_button && machine.View().outcome_ask==LiveEstimateAsk::button);
    // Nothing else makes a job: any number of calls with no request.
    for(int i=0;i<1000;++i)CHECK(!machine.BeginJob());
    CHECK(machine.Counters().jobs==2 && machine.Counters().placed==2);
    // A condition with no live estimate refuses the button and never asks.
    LivePlacementMachine disabled;
    disabled.BeginSession(kSha,kBase,false);
    CHECK(disabled.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::disabled && !disabled.BeginJob() && disabled.Activity()==LivePlacementActivity::idle);
    CHECK(disabled.Counters().refused==1 && disabled.Counters().jobs==0);
    LivePlacementMachine unconfigured;
    CHECK(unconfigured.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::disabled && !unconfigured.BeginJob());
}

// 5. A job that estimated nothing (no frames, the worker stopped) is asked for again, and is not an estimate that decided nothing.
void Released(){
    auto machine=Started();
    const auto job=machine.BeginJob();
    machine.Release(*job);
    CHECK(machine.Activity()==LivePlacementActivity::requested && machine.View().outcome==LivePlacementOutcomeKind::none && machine.Counters().released==1);
    const auto again=machine.BeginJob();
    CHECK(again && again->id!=job->id);
    machine.Release(*job);   // a job that is not the one running changes nothing
    CHECK(machine.Activity()==LivePlacementActivity::estimating);
    // The outcome of a job that is not the one running is dropped.
    CHECK(machine.Accept(*job,Usable(Pose{1,0,0,0,0,0}))==LivePlacementMachine::AcceptKind::discarded_stale && !machine.HasPending());
    CHECK(machine.Accept(*again,Usable(Pose{1,0,0,0,0,0}))==LivePlacementMachine::AcceptKind::pending_draw);
}

// 6. A position that cannot be drawn is dropped, and P stays.
void DrawFailed(){
    auto machine=Placed(Pose{0.3,0,0,1.0,0,0});
    const auto before=machine.View();
    CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    const auto job=machine.BeginJob();
    machine.Accept(*job,Usable(Pose{0,0,0,3000.0,0,0}));
    const auto wanted=machine.WantedSerial();
    machine.DrawFailed("bake-failed");
    const auto view=machine.View();
    CHECK(!machine.HasPending() && Same(view.mapping,before.mapping) && view.serial==before.serial && machine.WantedSerial()==wanted+1);
    CHECK(view.outcome==LivePlacementOutcomeKind::not_decided && view.last_failure==std::vector<std::string>{"bake-failed"} && view.activity==LivePlacementActivity::idle);
    machine.DrawFailed("bake-failed");   // nothing pending: nothing changes
    CHECK(machine.WantedSerial()==wanted+1 && machine.Counters().not_decided==1);
}

// 7. A shot: P becomes the JPEG's mapping bit for bit, layer 2 is left out and noted, and nothing is asked for.
void AfterCapture(){
    auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
    const auto live=machine.View();
    ex::RegisteredMapping jpeg{Pose{0.31,-0.12,0.52,2.25,-1.125,0.21},ex::SurfacePlane{3.5,0.0015,-0.0025},std::nullopt};
    ex::ParallaxProfile profile;
    profile.q_px={0.0,0.5,0.0};profile.row_spacing_px=10;profile.a_px=4;profile.l_px=8;
    jpeg.parallax=profile;
    const auto serial=machine.Serial(),wanted=machine.WantedSerial();
    CHECK(machine.ApplyCapture(jpeg,"G1",kSha));
    const auto view=machine.View();
    CHECK(Same(view.mapping,jpeg) && !view.mapping.parallax);   // the nine numbers are the JPEG's, to the bit
    CHECK(view.serial==serial+1 && machine.WantedSerial()==wanted+1 && view.source==LivePlacementSource::after_capture);
    CHECK(view.activity==LivePlacementActivity::idle && !view.ask && view.outcome==LivePlacementOutcomeKind::none && !machine.BeginJob());
    CHECK(view.capture && view.capture->level=="G1" && view.capture->layer_two_dropped && Same(view.capture->before,live.mapping) &&
        view.capture->before_source==LivePlacementSource::auto_start && !view.capture->difference_px);
    CHECK(machine.CaptureDifferencePending());
    machine.SetCaptureDifference(std::numeric_limits<double>::quiet_NaN());
    machine.SetCaptureDifference(-1.0);
    CHECK(machine.CaptureDifferencePending());
    machine.SetCaptureDifference(2.5);
    CHECK(!machine.CaptureDifferencePending() && machine.Capture()->difference_px==2.5);
    // A level without layer 2: nothing is dropped.
    auto plain=Placed(Pose{0.3,0,0,1.0,0,0});
    CHECK(plain.ApplyCapture(ex::RegisteredMapping{kBase,{},std::nullopt},"G4",kSha) && !plain.Capture()->layer_two_dropped && Same(plain.Placement(),ex::RegisteredMapping{kBase,{},std::nullopt}));
    // A profile whose samples are all zero is no layer 2 either.
    ex::RegisteredMapping flat{kBase,{},std::nullopt};
    ex::ParallaxProfile zero;zero.q_px={0.0,0.0};
    flat.parallax=zero;
    auto zeros=Placed(Pose{0.3,0,0,1.0,0,0});
    CHECK(zeros.ApplyCapture(flat,"G2",kSha) && !zeros.Capture()->layer_two_dropped);
}

// 8. A shot that is not for the session's condition, or whose mapping is not a position, changes nothing.
void CaptureRefused(){
    auto machine=Placed(Pose{0.3,0,0,1.0,0,0});
    const auto before=machine.View();
    CHECK(!machine.ApplyCapture(ex::RegisteredMapping{Pose{1,0,0,0,0,0},{},std::nullopt},"G1",kOtherSha));
    CHECK(!machine.ApplyCapture(ex::RegisteredMapping{Pose{std::numeric_limits<double>::quiet_NaN(),0,0,0,0,0},{},std::nullopt},"G1",kSha));
    CHECK(!machine.ApplyCapture(ex::RegisteredMapping{Pose{},ex::SurfacePlane{0,std::numeric_limits<double>::infinity(),0},std::nullopt},"G1",kSha));
    const auto after=machine.View();
    CHECK(Same(after.mapping,before.mapping) && after.serial==before.serial && after.source==before.source && !after.capture);
    LivePlacementMachine unconfigured;
    CHECK(!unconfigured.ApplyCapture(ex::RegisteredMapping{},"G4",kSha));
}

// 9. A shot ends what was asked for: a job in flight and a position that waits are dropped, and their result is not taken.
void CaptureEndsWhatWasAsked(){
    auto machine=Started();
    const auto job=machine.BeginJob();
    CHECK(machine.ApplyCapture(ex::RegisteredMapping{Pose{0.5,0,0,0,0,0},{},std::nullopt},"G2",kSha));
    CHECK(machine.Activity()==LivePlacementActivity::idle && machine.Source()==LivePlacementSource::after_capture);
    CHECK(machine.Accept(*job,Usable(Pose{9,0,0,0,0,0}))==LivePlacementMachine::AcceptKind::discarded_stale);
    CHECK(!machine.HasPending() && machine.Placement().pose.rx_deg==0.5);
    // A position that waits to be drawn when the shot ends is dropped.
    auto waiting=Started();
    const auto second=waiting.BeginJob();
    waiting.Accept(*second,Usable(Pose{0.7,0,0,0,0,0}));
    CHECK(waiting.HasPending());
    CHECK(waiting.ApplyCapture(ex::RegisteredMapping{Pose{0.5,0,0,0,0,0},{},std::nullopt},"G3",kSha));
    CHECK(!waiting.HasPending() && waiting.Wanted().pose.rx_deg==0.5);
    waiting.Committed();
    CHECK(waiting.Placement().pose.rx_deg==0.5 && waiting.Source()==LivePlacementSource::after_capture);
}

// 10. The next session: the same condition keeps P, whatever it came from; another condition goes back to the condition's position and
// asks once.
void NextSession(){
    // After an estimate.
    auto machine=Placed(Pose{0.3,0,0,1.0,0,0});
    auto serial=machine.Serial();
    machine.BeginSession(kSha,kBase,true);
    CHECK(machine.Serial()==serial && machine.Source()==LivePlacementSource::auto_start && machine.Activity()==LivePlacementActivity::idle && machine.Placement().pose.rx_deg==0.3);
    // After a shot.
    CHECK(machine.ApplyCapture(ex::RegisteredMapping{Pose{0.5,0,0,0,0,0},{},std::nullopt},"G1",kSha));
    serial=machine.Serial();
    machine.BeginSession(kSha,kBase,true);
    CHECK(machine.Serial()==serial && machine.Source()==LivePlacementSource::after_capture && machine.Activity()==LivePlacementActivity::idle && !machine.BeginJob());
    CHECK(machine.Capture() && machine.Capture()->level=="G1");
    // A session that began while an estimate waited keeps asking, once.
    auto waiting=Started();
    waiting.BeginSession(kSha,kBase,true);
    CHECK(waiting.Activity()==LivePlacementActivity::requested && waiting.BeginJob());
    // A job that was running when the session ended did not finish: it is asked for again.
    auto running=Started();
    (void)running.BeginJob();
    running.BeginSession(kSha,kBase,true);
    CHECK(running.Activity()==LivePlacementActivity::requested && running.BeginJob() && running.Counters().asked==0);
    // Another condition.
    machine.BeginSession(kOtherSha,Pose{0.9,0,0,0,0,0},true);
    const auto view=machine.View();
    CHECK(view.source==LivePlacementSource::fixed_condition && view.serial>serial && view.mapping.pose.rx_deg==0.9 && !view.capture && view.condition_sha256==kOtherSha);
    CHECK(view.activity==LivePlacementActivity::requested && view.ask==LiveEstimateAsk::condition_changed);
    const auto job=machine.BeginJob();
    CHECK(job && job->full_reason==LiveFullReason::condition_reloaded && job->base_pose.rx_deg==0.9);
    // A result of the job before the condition changed is not taken.
    auto stale=Started();
    const auto old_job=stale.BeginJob();
    stale.BeginSession(kOtherSha,kBase,true);
    CHECK(stale.Accept(*old_job,Usable(Pose{1,0,0,0,0,0}))==LivePlacementMachine::AcceptKind::discarded_stale && !stale.HasPending());
    // A condition with no live estimate asks for nothing, and a request that was waiting is dropped when the condition loses it.
    LivePlacementMachine off;
    off.BeginSession(kSha,kBase,false);
    CHECK(off.Activity()==LivePlacementActivity::idle && !off.BeginJob());
    auto lost=Started();
    lost.BeginSession(kSha,kBase,false);
    CHECK(lost.Activity()==LivePlacementActivity::idle && !lost.BeginJob());
}

// 11. The machine is a value: a copy carries P to the next session, and the two copies go their own ways.
void ValueSemantics(){
    auto machine=Placed(Pose{0.3,0,0,1.0,0,0});
    auto copy=machine;
    CHECK(copy.Serial()==machine.Serial() && Same(copy.Placement(),machine.Placement()));
    CHECK(copy.ApplyCapture(ex::RegisteredMapping{Pose{0.8,0,0,0,0,0},{},std::nullopt},"G2",kSha));
    CHECK(machine.Placement().pose.rx_deg==0.3 && copy.Placement().pose.rx_deg==0.8 && machine.Source()==LivePlacementSource::auto_start);
}

// 12. Nothing in the machine reads time or frames: the same calls give the same answers, and a long run of calls with nothing asked does not
// change P (the serial and what is wanted stay).
void NoDrift(){
    auto machine=Placed(Pose{0.3,0,0,1.0,0,0});
    const auto serial=machine.Serial(),wanted=machine.WantedSerial();
    const auto before=machine.View();
    for(int i=0;i<100000;++i){
        (void)machine.BeginJob();
        (void)machine.View();
        if(i%1000==0)machine.BeginSession(kSha,kBase,true);
    }
    const auto after=machine.View();
    CHECK(machine.Serial()==serial && machine.WantedSerial()==wanted && Same(after.mapping,before.mapping) && after.source==before.source);
    CHECK(after.counters.jobs==before.counters.jobs && after.counters.asked==before.counters.asked);
}

// 13. The log fields: one writer, 17 digits, the classic locale, no negative zero; a prefix goes in front of every key.
void Fields(){
    const ex::RegisteredMapping mapping{Pose{0.1,-0.0,0.30000000000000004,1.0/3.0,-2.5,1e-9},ex::SurfacePlane{3.0,0.001,-0.002},std::nullopt};
    const auto text=LivePlacementFields(mapping);
    CHECK(text=="rx_deg=0.10000000000000001 ry_deg=0 rz_deg=0.30000000000000004 tx_mm=0.33333333333333331 ty_mm=-2.5 tz_percent=1.0000000000000001e-09 "
        "h0_mm=3 slope_x=0.001 slope_y=-0.002");
    CHECK(LivePlacementFields(mapping,"before_").find("before_rx_deg=0.10000000000000001 before_ry_deg=0 ")==0 && LivePlacementFields(mapping,"before_").find(" before_slope_y=-0.002")!=std::string::npos);
    CHECK(LivePlacementFields(ex::RegisteredMapping{Pose{std::numeric_limits<double>::quiet_NaN(),0,0,0,0,0},{},std::nullopt}).find("rx_deg=nan ")==0);
    // Equal to the bit: -0 equals +0, NaN equals nothing.
    CHECK(LivePlacementSameMapping(ex::RegisteredMapping{Pose{0,-0.0,0,0,0,0},{},std::nullopt},ex::RegisteredMapping{}));
    CHECK(!LivePlacementSameMapping(ex::RegisteredMapping{Pose{std::numeric_limits<double>::quiet_NaN(),0,0,0,0,0},{},std::nullopt},
        ex::RegisteredMapping{Pose{std::numeric_limits<double>::quiet_NaN(),0,0,0,0,0},{},std::nullopt}));
    CHECK(!LivePlacementSameMapping(ex::RegisteredMapping{Pose{0,0,0,1e-12,0,0},{},std::nullopt},ex::RegisteredMapping{}));
    // The codes.
    CHECK(LivePlacementSourceCode(LivePlacementSource::fixed_condition)=="fixed-condition" && LivePlacementSourceCode(LivePlacementSource::auto_start)=="auto-start" &&
        LivePlacementSourceCode(LivePlacementSource::auto_button)=="auto-button" && LivePlacementSourceCode(LivePlacementSource::after_capture)=="after-capture");
    CHECK(LiveEstimateAskCode(LiveEstimateAsk::start)=="start" && LiveEstimateAskCode(LiveEstimateAsk::condition_changed)=="condition-changed" && LiveEstimateAskCode(LiveEstimateAsk::button)=="button");
    CHECK(LiveFullReasonCode(LiveFullReason::button)=="button");
}

// 14. What the estimate said about the range goes with the position that it made.
void BeyondEnvelope(){
    auto machine=Started();
    auto job=machine.BeginJob();
    auto outcome=Usable(Pose{0,0,2.6,1.0,0,0});
    outcome.beyond_envelope=true;outcome.exceeded={"rotation"};
    machine.Accept(*job,outcome);
    machine.Committed();
    auto view=machine.View();
    CHECK(view.beyond_envelope && view.exceeded_components==std::vector<std::string>{"rotation"});
    // A shot is not an estimate that was outside a range.
    CHECK(machine.ApplyCapture(ex::RegisteredMapping{Pose{0,0,2.6,1.0,0,0},{},std::nullopt},"G1",kSha));
    view=machine.View();
    CHECK(!view.beyond_envelope && view.exceeded_components.empty());
}
}

int main(){
    try{
        FirstSession();
        PlacedAfterTheDraw();
        NotDecided();
        AskedOnlyByRequest();
        Released();
        DrawFailed();
        AfterCapture();
        CaptureRefused();
        CaptureEndsWhatWasAsked();
        NextSession();
        ValueSemantics();
        NoDrift();
        Fields();
        BeyondEnvelope();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
