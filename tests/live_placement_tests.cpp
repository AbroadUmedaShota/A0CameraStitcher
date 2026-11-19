// The machine that holds the one position of camera B that the live C is drawn with (#437 stage 1). No window, camera, file, clock or thread:
// the calls are made by the test, in an order that it chooses, and every answer is read back from the machine.
#include "a0/phase0/experimental/live_placement.hpp"
#include <cmath>
#include <iostream>
#include <optional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
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
    CHECK(machine.ApplyCapture(jpeg,"G1",kSha,0.5));
    const auto view=machine.View();
    CHECK(Same(view.mapping,jpeg) && !view.mapping.parallax);   // the nine numbers are the JPEG's, to the bit
    CHECK(view.serial==serial+1 && machine.WantedSerial()==wanted+1 && view.source==LivePlacementSource::after_capture);
    CHECK(view.activity==LivePlacementActivity::idle && !view.ask && view.outcome==LivePlacementOutcomeKind::none && !machine.BeginJob());
    CHECK(view.capture && view.capture->level=="G1" && view.capture->layer_two_dropped && Same(view.capture->before,live.mapping) &&
        view.capture->before_source==LivePlacementSource::auto_start && !view.capture->difference_px);
    CHECK(view.capture->layer_two_max_px && *view.capture->layer_two_max_px==0.5);   // the largest shift of what was dropped, as the caller gave it
    CHECK(machine.CaptureDifferencePending());
    machine.SetCaptureDifference(std::numeric_limits<double>::quiet_NaN());
    machine.SetCaptureDifference(-1.0);
    CHECK(machine.CaptureDifferencePending());
    machine.SetCaptureDifference(2.5);
    CHECK(!machine.CaptureDifferencePending() && machine.Capture()->difference_px==2.5);
    // A level without layer 2: nothing is dropped.
    auto plain=Placed(Pose{0.3,0,0,1.0,0,0});
    CHECK(plain.ApplyCapture(ex::RegisteredMapping{kBase,{},std::nullopt},"G4",kSha,0.5) && !plain.Capture()->layer_two_dropped && !plain.Capture()->layer_two_max_px &&
        Same(plain.Placement(),ex::RegisteredMapping{kBase,{},std::nullopt}));
    // A profile whose samples are all zero is no layer 2 either.
    ex::RegisteredMapping flat{kBase,{},std::nullopt};
    ex::ParallaxProfile zero;zero.q_px={0.0,0.0};
    flat.parallax=zero;
    auto zeros=Placed(Pose{0.3,0,0,1.0,0,0});
    CHECK(zeros.ApplyCapture(flat,"G2",kSha,0.5) && !zeros.Capture()->layer_two_dropped && !zeros.Capture()->layer_two_max_px);
    // A layer 2 with no usable number (none, zero, negative, not finite): dropped, and no number is kept.
    for(const std::optional<double> number:{std::optional<double>{},std::optional<double>{0.0},std::optional<double>{-0.5},std::optional<double>{std::numeric_limits<double>::infinity()},
        std::optional<double>{std::numeric_limits<double>::quiet_NaN()}}){
        auto without=Placed(Pose{0.3,0,0,1.0,0,0});
        CHECK(without.ApplyCapture(jpeg,"G1",kSha,number) && without.Capture()->layer_two_dropped && !without.Capture()->layer_two_max_px);
    }
}

// 7b. Layer 2 of the capture's record, for the sentence and the two log lines: the largest shift is taken from the record's summary only for a
// mapping that carries a layer 2 that moves something, and the log fields are written by one function.
void LayerTwoFields(){
    ex::ParallaxProfile moving;moving.q_px={0.0,0.5,0.0};
    ex::ParallaxProfile still;still.q_px={0.0,0.0};
    const ex::RegisteredMapping with{kBase,{},moving},zero{kBase,{},still},none{kBase,{},std::nullopt};
    CHECK(LivePlacementHasLayerTwo(with) && !LivePlacementHasLayerTwo(zero) && !LivePlacementHasLayerTwo(none));
    CHECK(LivePlacementLayerTwoMaxPx(with,0.5)==0.5 && !LivePlacementLayerTwoMaxPx(zero,0.5) && !LivePlacementLayerTwoMaxPx(none,0.5));
    CHECK(!LivePlacementLayerTwoMaxPx(with,0.0) && !LivePlacementLayerTwoMaxPx(with,-1.0) && !LivePlacementLayerTwoMaxPx(with,std::numeric_limits<double>::quiet_NaN()) &&
        !LivePlacementLayerTwoMaxPx(with,std::numeric_limits<double>::infinity()));
    CHECK(LivePlacementLayerTwoFields(true,0.5)=="layer_two_dropped=1 layer_two_max_px=0.5");
    CHECK(LivePlacementLayerTwoFields(true,std::nullopt)=="layer_two_dropped=1 layer_two_max_px=unknown");
    CHECK(LivePlacementLayerTwoFields(false,std::nullopt)=="layer_two_dropped=0 layer_two_max_px=0" && LivePlacementLayerTwoFields(false,0.5)=="layer_two_dropped=0 layer_two_max_px=0");
    CHECK(LivePlacementLayerTwoFields(true,0.1)=="layer_two_dropped=1 layer_two_max_px=0.10000000000000001");
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

// ---- Hand steps and the freeze of a capture (#437 stage 2) ----
// The check of a hand step, in the test's own arithmetic: the solved position is the base plus values of the counts (a displacement of d px
// along the seam is d / 16 mm of tx, and likewise ty; the rotation is added as it is), the envelope is |tx - the condition's tx| <= tx_limit_mm,
// and the cover fails when the result's rz exceeds cover_rz_limit_deg. Every call is recorded. The real checks (CheckHandStep) are in
// m2_confirmed_position_contracts; the machine is tested against a check whose answers are known.
struct Toy {
    double tx_limit_mm{6.0};
    double cover_rz_limit_deg{1e9};
    bool unsolvable{false};
    int calls{0};
    std::vector<std::pair<ex::RegisteredMapping,ex::HandCounts>> seen;
    std::optional<int> throw_kind;   // 0 std::runtime_error, 1 std::invalid_argument, 2 a value that is not an exception
    static ex::RegisteredMapping Solved(const ex::RegisteredMapping& base,const ex::HandCounts& total){
        const auto v=ex::HandValuesOf(total);
        ex::RegisteredMapping out=base;
        out.pose.tx_mm=base.pose.tx_mm+v.seam_dx_px/16.0;
        out.pose.ty_mm=base.pose.ty_mm+v.seam_dy_px/16.0;
        out.pose.rz_deg=base.pose.rz_deg+v.rotation_deg;
        return out;
    }
    LiveHandChecker Fn(){
        return [this](const ex::RegisteredMapping& base,const ex::HandCounts& total){
            ++calls;seen.emplace_back(base,total);
            if(throw_kind){
                if(*throw_kind==0)throw std::runtime_error("the check failed");
                if(*throw_kind==1)throw std::invalid_argument("not a request");
                throw 42;
            }
            LiveHandCheck out;
            if(unsolvable){out.status=LiveHandCheck::Status::refused_unsolvable;return out;}
            out.mapping=Solved(base,total);
            if(std::abs(out.mapping.pose.tx_mm-kBase.tx_mm)>tx_limit_mm){out.status=LiveHandCheck::Status::refused_envelope;out.exceeded={"position"};return out;}
            if(out.mapping.pose.rz_deg>cover_rz_limit_deg){out.status=LiveHandCheck::Status::refused_cover;out.seam_covered=false;out.output_covered=true;return out;}
            out.status=LiveHandCheck::Status::accepted;
            return out;
        };
    }
};
constexpr LiveHandContext kReady{true,true};
ex::HandCounts Counts(int dx,int dy=0,int rotation=0){return ex::HandCounts{dx,dy,rotation};}
// P by hand: a position that was placed by an estimate, then moved by `total` and drawn.
LivePlacementMachine ByHand(Toy& toy,const Pose& estimate,const ex::HandCounts& total,bool button=false){
    auto machine=Placed(estimate);
    if(button){
        machine.RequestEstimate(LiveEstimateAsk::button);
        const auto job=machine.BeginJob();
        machine.Accept(*job,Usable(estimate));
        machine.Committed();
    }
    const auto result=machine.HandStep(total,kReady,toy.Fn());
    CHECK(result.outcome==LiveHandOutcome::accepted);
    machine.Committed();
    return machine;
}
// Nothing changed: what the screen, the log and the capture read is what it was.
bool Unchanged(const LivePlacementMachine& machine,const LivePlacementView& before,std::uint64_t wanted){
    const auto view=machine.View();
    return Same(view.mapping,before.mapping) && view.serial==before.serial && view.source==before.source && machine.WantedSerial()==wanted &&
        view.hand_entered==before.hand_entered && view.activity==before.activity && view.ask==before.ask && view.outcome==before.outcome;
}

// 15. A hand press: the position that waits is the base plus the sum, solved once from the position before the first press; P is replaced
// after it has been drawn. The next press adds to the sum and solves from the same base, not from the position that the press before gave.
void HandStepBuildsOnTheBase(){
    Toy toy;
    auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2},ex::SurfacePlane{3.0,0.001,-0.002});
    const auto estimate=machine.Placement();
    const auto serial=machine.Serial(),wanted=machine.WantedSerial();
    auto result=machine.HandStep(Counts(3),kReady,toy.Fn());
    CHECK(result.outcome==LiveHandOutcome::accepted && result.total==Counts(3) && !result.returned_to_base && !result.became_condition_position);
    CHECK(toy.calls==1 && Same(toy.seen[0].first,estimate) && toy.seen[0].second==Counts(3));
    // It waits: P is still the estimate, what is wanted is the solved position (the surface is the estimate's), and nothing says "manual" yet.
    CHECK(machine.HasPending() && machine.Serial()==serial && machine.Source()==LivePlacementSource::auto_start && machine.WantedSerial()==wanted+1);
    CHECK(Same(machine.Wanted(),Toy::Solved(estimate,Counts(3))) && machine.Wanted().surface.h0_mm==3.0 && !machine.Wanted().parallax);
    CHECK(Same(machine.Placement(),estimate) && machine.View().hand_wanted==Counts(3) && !machine.View().hand_entered);
    machine.Committed();
    auto view=machine.View();
    CHECK(view.source==LivePlacementSource::manual && view.serial==serial+1 && view.base_source==LivePlacementSource::auto_start && view.hand_entered==Counts(3));
    CHECK(Same(view.mapping,Toy::Solved(estimate,Counts(3))) && view.hand_base && Same(*view.hand_base,estimate) && !view.hand_wanted && !view.hand_returned);
    CHECK(view.activity==LivePlacementActivity::idle && !view.beyond_envelope && view.counters.hand_accepted==1);
    // The next press: the check is given the same base and the sum of both.
    const auto afterwards=machine.HandStep(Counts(1,-2,4),kReady,toy.Fn());
    CHECK(afterwards.outcome==LiveHandOutcome::accepted && afterwards.total==Counts(4,-2,4));
    CHECK(toy.calls==2 && Same(toy.seen[1].first,estimate) && toy.seen[1].second==Counts(4,-2,4));
    CHECK(Same(machine.Wanted(),Toy::Solved(estimate,Counts(4,-2,4))));
    machine.Committed();
    CHECK(machine.View().hand_entered==Counts(4,-2,4) && machine.View().base_source==LivePlacementSource::auto_start && machine.Serial()==serial+2);
    // The base is also kept by the position that the estimate before the hand steps was made from: the surface and the source.
    CHECK(Same(*machine.View().hand_base,estimate));
}

// 16. The sum is the same whatever the order of the presses, and a press does not build on the one before: two machines with the same presses
// in other orders are at the same position to the bit, and the check never sees a base that was a hand result.
void HandSumDoesNotDependOnTheOrder(){
    const std::vector<ex::HandCounts> presses{Counts(3),Counts(0,1),Counts(0,0,4),Counts(-1),Counts(0,0,-1),Counts(1,-1,1)};
    ex::HandCounts total{};
    for(const auto& p:presses)total=ex::AddHandCounts(total,p);
    const auto run=[&](const std::vector<size_t>& order,Toy& toy){
        auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
        const auto estimate=machine.Placement();
        for(const auto index:order){
            CHECK(machine.HandStep(presses[index],kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);
            machine.Committed();
        }
        for(const auto& call:toy.seen)CHECK(Same(call.first,estimate));
        return machine;
    };
    Toy a,b,c;
    const auto m1=run({0,1,2,3,4,5},a),m2=run({5,4,3,2,1,0},b),m3=run({2,0,5,1,4,3},c);
    CHECK(m1.View().hand_entered==total && m2.View().hand_entered==total && m3.View().hand_entered==total);
    CHECK(Same(m1.Placement(),m2.Placement()) && Same(m1.Placement(),m3.Placement()) && Same(m1.Placement(),Toy::Solved(*m1.View().hand_base,total)));
    // The same presses in a different grouping (two presses in one) end at the same position.
    Toy d;
    auto grouped=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
    CHECK(grouped.HandStep(ex::AddHandCounts(presses[0],presses[1]),kReady,d.Fn()).outcome==LiveHandOutcome::accepted);
    CHECK(Same(grouped.Wanted(),Toy::Solved(grouped.Placement(),Counts(3,1))));
}

// 17. A press before the one before it was drawn is accepted and replaces the position that waits; a position that waited and is dropped
// (a bake that failed) takes its sum with it.
void HandStepBeforeTheDraw(){
    Toy toy;
    auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
    const auto estimate=machine.Placement();
    const auto wanted=machine.WantedSerial();
    CHECK(machine.HandStep(Counts(3),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);
    CHECK(machine.HandStep(Counts(1),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);
    CHECK(machine.WantedSerial()==wanted+2 && Same(machine.Wanted(),Toy::Solved(estimate,Counts(4))) && toy.seen[1].second==Counts(4) && Same(toy.seen[1].first,estimate));
    machine.Committed();
    CHECK(machine.View().hand_entered==Counts(4) && Same(machine.Placement(),Toy::Solved(estimate,Counts(4))));
    // The next press waits and is dropped (it could not be drawn): the sum is P's again, and the press after builds on P's sum.
    CHECK(machine.HandStep(Counts(1),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);
    const auto before=machine.View();
    const auto serial_wanted=machine.WantedSerial();
    machine.DrawFailed("bake-failed");
    CHECK(!machine.HasPending() && machine.WantedSerial()==serial_wanted+1 && Same(machine.Placement(),before.mapping) && machine.Serial()==before.serial);
    CHECK(machine.View().hand_entered==Counts(4) && !machine.View().hand_wanted && machine.Counters().hand_draw_failed==1 && machine.Counters().not_decided==0);
    CHECK(machine.View().outcome==LivePlacementOutcomeKind::none);   // a hand step that could not be drawn is not an estimate that decided nothing
    CHECK(machine.HandStep(Counts(1),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted && toy.seen.back().second==Counts(5));
}

// 18. A press is refused, and changes nothing, when Live is stopped, the screen was not drawn with P, a pair capture holds P, it adds nothing,
// the sum leaves the counts' range, the check refuses it, or something changed while the check ran.
void HandStepRefused(){
    Toy toy;
    auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
    auto refuses=[&](LivePlacementMachine& m,const ex::HandCounts& delta,const LiveHandContext& context,LiveHandOutcome outcome,const char* what){
        const auto before=m.View();const auto wanted=m.WantedSerial();const auto refused=before.counters.hand_refused;
        const auto result=m.HandStep(delta,context,toy.Fn());
        Check(result.outcome==outcome && Unchanged(m,before,wanted) && !m.HasPending() && m.View().counters.hand_refused==refused+1,what);
        return result;
    };
    const int calls_before=toy.calls;
    refuses(machine,Counts(3),LiveHandContext{false,true},LiveHandOutcome::refused_stopped,"stopped");
    refuses(machine,Counts(3),LiveHandContext{true,false},LiveHandOutcome::refused_not_drawn,"not drawn");
    refuses(machine,Counts(3),LiveHandContext{false,false},LiveHandOutcome::refused_stopped,"stopped, not drawn");
    refuses(machine,Counts(0),kReady,LiveHandOutcome::refused_zero,"zero");
    CHECK(toy.calls==calls_before);   // none of these reached the check
    LivePlacementMachine unconfigured;
    CHECK(unconfigured.HandStep(Counts(3),kReady,toy.Fn()).outcome==LiveHandOutcome::refused_not_configured);
    // The envelope: exactly at the limit is inside (4 px along the seam is 0.25 mm of tx), a step past it is refused, never rounded to the edge.
    Toy tight;tight.tx_limit_mm=1.0;
    auto edge=Started();
    {
        const auto job=edge.BeginJob();edge.Accept(*job,Usable(kBase));edge.Committed();   // P is the condition's position
    }
    CHECK(edge.HandStep(Counts(4),kReady,tight.Fn()).outcome==LiveHandOutcome::accepted);   // 16 px: tx +1.0 mm, the limit exactly
    edge.Committed();
    CHECK(edge.Placement().pose.tx_mm==kBase.tx_mm+1.0);
    {
        const auto before=edge.View();const auto wanted=edge.WantedSerial();
        const auto past=edge.HandStep(Counts(1),kReady,tight.Fn());
        CHECK(past.outcome==LiveHandOutcome::refused_envelope && past.exceeded==std::vector<std::string>{"position"} && Unchanged(edge,before,wanted) && !edge.HasPending());
    }
    // The cover check: which of the two failed is carried.
    Toy cover;cover.cover_rz_limit_deg=kBase.rz_deg+0.1;
    auto covered=Placed(kBase);
    const auto before=covered.View();const auto wanted=covered.WantedSerial();
    const auto no_cover=covered.HandStep(Counts(0,0,4),kReady,cover.Fn());   // +0.2 degrees
    CHECK(no_cover.outcome==LiveHandOutcome::refused_cover && !no_cover.seam_covered && no_cover.output_covered && Unchanged(covered,before,wanted));
    CHECK(covered.HandStep(Counts(0,0,2),kReady,cover.Fn()).outcome==LiveHandOutcome::accepted);   // +0.1 degrees is exactly the limit: covered
    // Unsolvable, and a check that throws: the reason is the refusal, and nothing is swallowed.
    Toy unsolvable;unsolvable.unsolvable=true;
    auto solving=Placed(kBase);
    const auto solving_before=solving.View();const auto solving_wanted=solving.WantedSerial();
    CHECK(solving.HandStep(Counts(3),kReady,unsolvable.Fn()).outcome==LiveHandOutcome::refused_unsolvable && Unchanged(solving,solving_before,solving_wanted));
    for(const int kind:{0,1,2}){
        Toy throws;throws.throw_kind=kind;
        auto thrown=Placed(kBase);
        const auto thrown_before=thrown.View();const auto thrown_wanted=thrown.WantedSerial();
        const auto result=thrown.HandStep(Counts(3),kReady,throws.Fn());
        CHECK(result.outcome==LiveHandOutcome::refused_error && Unchanged(thrown,thrown_before,thrown_wanted) && !thrown.HasPending());
        CHECK(kind==2?result.detail=="an exception of an unknown kind":kind==0?result.detail=="the check failed":result.detail=="not a request");
    }
    auto without=Placed(kBase);
    const auto no_check=without.HandStep(Counts(3),kReady,LiveHandChecker{});
    CHECK(no_check.outcome==LiveHandOutcome::refused_error && !no_check.detail.empty() && !without.HasPending());
    // A solved position that is not finite is not a position.
    auto bad=Placed(kBase);
    const auto bad_result=bad.HandStep(Counts(3),kReady,[](const ex::RegisteredMapping& base,const ex::HandCounts&){
        LiveHandCheck out;out.status=LiveHandCheck::Status::accepted;out.mapping=base;out.mapping.pose.tx_mm=std::numeric_limits<double>::infinity();return out;});
    CHECK(bad_result.outcome==LiveHandOutcome::refused_unsolvable && !bad.HasPending());
    // The counts' range: a sum that does not fit is refused (no wrap-around), and P stays at the sum it had.
    Toy wide;wide.tx_limit_mm=1e18;
    auto large=Placed(kBase);
    CHECK(large.HandStep(Counts(std::numeric_limits<std::int32_t>::max()),kReady,wide.Fn()).outcome==LiveHandOutcome::accepted);
    large.Committed();
    const auto large_before=large.View();const auto large_wanted=large.WantedSerial();
    CHECK(large.HandStep(Counts(1),kReady,wide.Fn()).outcome==LiveHandOutcome::refused_range && Unchanged(large,large_before,large_wanted));
    CHECK(large.HandStep(Counts(-1),kReady,wide.Fn()).outcome==LiveHandOutcome::accepted);
    // Something changed while the check ran: the proposal is stale and is refused.
    auto racing=Placed(kBase);
    const auto first=racing.PrepareHandStep(Counts(3),kReady),second=racing.PrepareHandStep(Counts(1),kReady);
    CHECK(first.outcome==LiveHandOutcome::accepted && second.outcome==LiveHandOutcome::accepted);
    CHECK(racing.CommitHandStep(second.proposal,EvaluateLiveHandCheck(toy.Fn(),second.proposal)).outcome==LiveHandOutcome::accepted);
    const auto late=racing.CommitHandStep(first.proposal,EvaluateLiveHandCheck(toy.Fn(),first.proposal));
    CHECK(late.outcome==LiveHandOutcome::refused_stale && Same(racing.Wanted(),Toy::Solved(ex::RegisteredMapping{kBase,{},std::nullopt},Counts(1))));
    racing.Committed();
    CHECK(racing.View().hand_entered==Counts(1));
    // A shot that began while the check ran: the same.
    auto capturing=Placed(kBase);
    const auto proposal=capturing.PrepareHandStep(Counts(3),kReady);
    CHECK(capturing.BeginCapture(true).admission==LiveCaptureAdmission::accepted);
    CHECK(capturing.CommitHandStep(proposal.proposal,EvaluateLiveHandCheck(toy.Fn(),proposal.proposal)).outcome==LiveHandOutcome::refused_capturing && !capturing.HasPending());
}

// 19. A press withdraws the estimate that was asked for, running or waiting to be drawn, and its result; nothing runs until the button is
// pressed again. This holds for an estimate that was left asked for by a session that ended (the one that "continues once").
void HandStepWithdrawsTheEstimate(){
    Toy toy;
    // Asked for and not begun.
    {
        auto machine=Started();
        CHECK(machine.Activity()==LivePlacementActivity::requested);
        const auto result=machine.HandStep(Counts(3),kReady,toy.Fn());
        CHECK(result.outcome==LiveHandOutcome::accepted && result.withdrew_estimate && machine.Activity()==LivePlacementActivity::idle && !machine.View().ask);
        CHECK(!machine.BeginJob() && machine.Counters().jobs==0);
        machine.Committed();
        CHECK(machine.Source()==LivePlacementSource::manual && machine.View().base_source==LivePlacementSource::fixed_condition && !machine.BeginJob());
        CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted && machine.BeginJob());   // the button asks again, once
    }
    // Running: its result is dropped.
    {
        auto machine=Started();
        const auto job=machine.BeginJob();
        const auto result=machine.HandStep(Counts(3),kReady,toy.Fn());
        CHECK(result.outcome==LiveHandOutcome::accepted && result.withdrew_estimate);
        const auto wanted=machine.WantedSerial();
        CHECK(machine.Accept(*job,Usable(Pose{0.9,0,0,0,0,0}))==LivePlacementMachine::AcceptKind::discarded_stale);
        CHECK(machine.WantedSerial()==wanted && Same(machine.Wanted(),Toy::Solved(ex::RegisteredMapping{kBase,{},std::nullopt},Counts(3))) && machine.Activity()==LivePlacementActivity::idle);
        machine.Committed();
        CHECK(machine.Source()==LivePlacementSource::manual && machine.Placement().pose.rx_deg==kBase.rx_deg && !machine.BeginJob() && machine.Counters().not_decided==0);
        CHECK(machine.View().outcome==LivePlacementOutcomeKind::none);
    }
    // Its result waits to be drawn: the hand position replaces it, and the estimate's position never becomes P.
    {
        auto machine=Started();
        const auto job=machine.BeginJob();
        machine.Accept(*job,Usable(Pose{0.9,0,0,0,0,0}));
        CHECK(machine.HasPending() && machine.Activity()==LivePlacementActivity::estimating);
        const auto result=machine.HandStep(Counts(0,3),kReady,toy.Fn());
        CHECK(result.outcome==LiveHandOutcome::accepted && result.withdrew_estimate && machine.Activity()==LivePlacementActivity::idle);
        CHECK(Same(machine.Wanted(),Toy::Solved(ex::RegisteredMapping{kBase,{},std::nullopt},Counts(0,3))));
        machine.Committed();
        CHECK(machine.Source()==LivePlacementSource::manual && machine.Placement().pose.rx_deg==kBase.rx_deg && machine.Counters().placed==0);
    }
    // An estimate that a session that ended left asked for: the hand press after the next session began withdraws it.
    {
        auto machine=Started();
        (void)machine.BeginJob();
        machine.BeginSession(kSha,kBase,true);
        CHECK(machine.Activity()==LivePlacementActivity::requested);
        CHECK(machine.HandStep(Counts(3),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);
        machine.Committed();
        CHECK(!machine.BeginJob() && machine.Counters().jobs==1 && machine.Source()==LivePlacementSource::manual);
    }
    // A press that is refused leaves the request alone.
    {
        auto machine=Started();
        CHECK(machine.HandStep(Counts(3),LiveHandContext{false,true},toy.Fn()).outcome==LiveHandOutcome::refused_stopped);
        CHECK(machine.Activity()==LivePlacementActivity::requested && machine.BeginJob());
    }
    // The estimate that the button asks for afterwards replaces a hand position (the button is the other way to change P), and the hand mark goes.
    {
        auto machine=ByHand(toy,Pose{0.3,-0.1,0.5,2.0,-1.0,0.2},Counts(3));
        CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
        const auto job=machine.BeginJob();
        machine.Accept(*job,Usable(Pose{0.31,-0.1,0.5,2.5,-1.0,0.2}));
        machine.Committed();
        const auto view=machine.View();
        CHECK(view.source==LivePlacementSource::auto_button && !view.hand_entered && !view.hand_base && !view.base_source && !view.hand_returned);
        // Hand steps go on from the new position, not from the old base.
        CHECK(machine.HandStep(Counts(1),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted && Same(toy.seen.back().first,view.mapping) && toy.seen.back().second==Counts(1));
    }
}

// 20. A sum that comes back to exactly 0 is not a hand position: P goes back to what it was before the first press, source included, whatever
// that was; no hand mark stays. It is not a step, so it is not checked against the envelope: a position outside the envelope that an estimate
// found comes back too.
void HandSumComesBackToZero(){
    Toy toy;
    // From the condition's own position.
    {
        auto machine=Started();
        { const auto job=machine.BeginJob();machine.Accept(*job,Usable(kBase));machine.Committed(); }
        auto from=Placed(kBase);
        const auto condition=from.Placement();
        const auto serial=from.Serial();
        CHECK(from.HandStep(Counts(3),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);from.Committed();
        CHECK(from.Source()==LivePlacementSource::manual && from.Serial()==serial+1);
        const auto calls=toy.calls;
        const auto back=from.HandStep(Counts(-3),kReady,toy.Fn());
        CHECK(back.outcome==LiveHandOutcome::accepted && back.returned_to_base && toy.calls==calls);   // not checked
        CHECK(Same(from.Wanted(),condition) && from.Placement().pose.tx_mm!=condition.pose.tx_mm);   // it waits to be drawn: P is still the hand position
        from.Committed();
        const auto view=from.View();
        CHECK(Same(view.mapping,condition) && view.source==LivePlacementSource::auto_start && !view.hand_entered && !view.hand_base && !view.base_source && view.hand_returned);
        CHECK(view.serial==serial+2 && !view.hand_wanted);
    }
    // From a condition position that nothing estimated: the source it had, fixed-condition.
    {
        auto machine=Started();
        const auto condition=machine.Placement();
        CHECK(machine.HandStep(Counts(3,-1,4),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);machine.Committed();
        CHECK(machine.HandStep(Counts(-3),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);machine.Committed();
        CHECK(machine.Source()==LivePlacementSource::manual && machine.View().hand_entered==Counts(0,-1,4));
        CHECK(machine.HandStep(Counts(0,1,-4),kReady,toy.Fn()).returned_to_base);machine.Committed();
        CHECK(machine.Source()==LivePlacementSource::fixed_condition && Same(machine.Placement(),condition) && !machine.View().hand_entered && machine.View().hand_returned);
    }
    // 0.2 degrees and 0.05 degrees mixed up and down come back to 0 whatever the order (counts, not floating point).
    {
        auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
        const auto estimate=machine.Placement();
        const std::vector<ex::HandCounts> presses{Counts(0,0,4),Counts(0,0,1),Counts(0,0,-4),Counts(0,0,-1)};
        for(size_t i=0;i<presses.size();++i){
            const auto result=machine.HandStep(presses[i],kReady,toy.Fn());
            CHECK(result.outcome==LiveHandOutcome::accepted && result.returned_to_base==(i==presses.size()-1));
            machine.Committed();
        }
        CHECK(machine.Source()==LivePlacementSource::auto_start && Same(machine.Placement(),estimate) && !machine.View().hand_entered);
        // In doubles 0.2+0.05-0.2-0.05 is not 0.
        CHECK(0.2+0.05-0.2-0.05!=0.0);
    }
    // An estimate that was outside the envelope (applied beyond it) and that the hand steps left and came back to: the position that the
    // estimate found, with what was said about its range.
    {
        Toy limit;limit.tx_limit_mm=6.0;
        auto machine=Started();
        const auto job=machine.BeginJob();
        auto outcome=Usable(Pose{kBase.rx_deg,kBase.ry_deg,kBase.rz_deg,kBase.tx_mm+9.0,kBase.ty_mm,kBase.tz_percent});
        outcome.beyond_envelope=true;outcome.exceeded={"position"};
        machine.Accept(*job,outcome);machine.Committed();
        const auto outside=machine.View();
        CHECK(outside.beyond_envelope && outside.source==LivePlacementSource::auto_start);
        // A press that leaves the position outside the envelope is refused; one that brings the result inside is accepted (-52 px is -3.25 mm).
        CHECK(machine.HandStep(Counts(-1),kReady,limit.Fn()).outcome==LiveHandOutcome::refused_envelope);
        CHECK(machine.HandStep(Counts(-13),kReady,limit.Fn()).outcome==LiveHandOutcome::accepted);
        machine.Committed();
        CHECK(machine.Source()==LivePlacementSource::manual && !machine.View().beyond_envelope && machine.Placement().pose.tx_mm==kBase.tx_mm+9.0-3.25);
        const auto calls=limit.calls;
        const auto back=machine.HandStep(Counts(13),kReady,limit.Fn());
        CHECK(back.outcome==LiveHandOutcome::accepted && back.returned_to_base && limit.calls==calls);
        machine.Committed();
        const auto view=machine.View();
        CHECK(Same(view.mapping,outside.mapping) && view.source==LivePlacementSource::auto_start && view.beyond_envelope && view.exceeded_components==outside.exceeded_components);
        CHECK(!view.hand_entered && view.hand_returned);
    }
    // A sum that came back to 0 before the position that waited was drawn: P was never moved, and nothing waits.
    {
        auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
        const auto before=machine.View();
        CHECK(machine.HandStep(Counts(3),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);
        const auto wanted=machine.WantedSerial();
        const auto back=machine.HandStep(Counts(-3),kReady,toy.Fn());
        CHECK(back.outcome==LiveHandOutcome::accepted && back.returned_to_base && !machine.HasPending() && machine.WantedSerial()==wanted+1);
        CHECK(Same(machine.Wanted(),before.mapping) && machine.Serial()==before.serial && machine.Source()==LivePlacementSource::auto_start && !machine.View().hand_entered);
    }
    // A press that gives the condition's own position, bit for bit, is not by hand either (the sum is not 0): the source is fixed-condition.
    {
        auto machine=Placed(Pose{kBase.rx_deg,kBase.ry_deg,kBase.rz_deg,kBase.tx_mm+0.25,kBase.ty_mm,kBase.tz_percent});
        const auto result=machine.HandStep(Counts(-1),kReady,toy.Fn());
        CHECK(result.outcome==LiveHandOutcome::accepted && result.became_condition_position && !result.returned_to_base);
        machine.Committed();
        const auto view=machine.View();
        CHECK(view.source==LivePlacementSource::fixed_condition && Same(view.mapping,ex::RegisteredMapping{kBase,{},std::nullopt}) && !view.hand_entered && !view.hand_base && !view.hand_returned);
        // The next press builds on this position, which is not by hand.
        CHECK(machine.HandStep(Counts(1),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted && toy.seen.back().second==Counts(1) && Same(toy.seen.back().first,view.mapping));
    }
}

// 21. The steps of the buttons are whole counts of the smallest step: 12 px is 3, 4 px is 1, 0.2 degrees is 4, 0.05 degrees is 1, and no other.
void HandStepSizes(){
    CHECK(ex::kHandCoarseSeamCounts==3 && ex::kHandFineSeamCounts==1 && ex::kHandCoarseRotationCounts==4 && ex::kHandFineRotationCounts==1);
    CHECK(ex::HandValuesOf(Counts(ex::kHandCoarseSeamCounts)).seam_dx_px==12.0 && ex::HandValuesOf(Counts(ex::kHandFineSeamCounts)).seam_dx_px==4.0);
    CHECK(ex::HandValuesOf(Counts(0,ex::kHandCoarseSeamCounts)).seam_dy_px==12.0 && ex::HandValuesOf(Counts(0,-ex::kHandFineSeamCounts)).seam_dy_px==-4.0);
    CHECK(ex::HandValuesOf(Counts(0,0,ex::kHandCoarseRotationCounts)).rotation_deg==0.2 && ex::HandValuesOf(Counts(0,0,ex::kHandFineRotationCounts)).rotation_deg==0.05);
    // What the check is given for each of the four steps, from the same base: the sum in counts, and the values made from it once.
    for(const auto& step:{Counts(3),Counts(1),Counts(0,0,4),Counts(0,0,1),Counts(-3),Counts(0,-1),Counts(0,0,-4)}){
        Toy toy;
        auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
        CHECK(machine.HandStep(step,kReady,toy.Fn()).outcome==LiveHandOutcome::accepted && toy.seen.size()==1 && toy.seen[0].second==step);
    }
    // The record's fields for a hand position: the values are written like the position.
    CHECK(LivePlacementManualFields(Counts(3,-1,4))=="manual_seam_dx_px=12 manual_seam_dy_px=-4 manual_rotation_deg=0.20000000000000001");
    CHECK(LivePlacementSourceCode(LivePlacementSource::manual)=="manual" && LiveHandOutcomeCode(LiveHandOutcome::refused_envelope)=="envelope");
}

// 22. A pair capture freezes P. What it is given is P as it is drawn, never the position that waits; the position that waited is dropped and
// WantedSerial moves, so that a bake in progress for it is dropped too; a running estimate's result is dropped and the ask stays; the button and
// a hand press are refused until the result is in.
void CaptureFreezesP(){
    Toy toy;
    // A hand position waits to be drawn.
    {
        auto machine=ByHand(toy,Pose{0.3,-0.1,0.5,2.0,-1.0,0.2},Counts(3));
        const auto drawn=machine.Placement();
        const auto serial=machine.Serial();
        CHECK(machine.HandStep(Counts(1),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);
        const auto wanted=machine.WantedSerial();
        const auto begin=machine.BeginCapture(true);
        CHECK(begin.admission==LiveCaptureAdmission::accepted && begin.dropped_pending && !begin.dropped_estimate && begin.grant);
        CHECK(Same(begin.grant->mapping,drawn) && begin.grant->serial==serial && begin.grant->source==LivePlacementSource::manual && begin.grant->base_source==LivePlacementSource::auto_start &&
            begin.grant->hand_entered==Counts(3) && begin.grant->condition_sha256==kSha);
        CHECK(!machine.HasPending() && machine.WantedSerial()==wanted+1 && Same(machine.Wanted(),drawn) && machine.Capturing());
        // A bake that was in progress for the position that waited ends after the request: it commits nothing.
        machine.Committed();
        CHECK(Same(machine.Placement(),drawn) && machine.Serial()==serial && machine.View().hand_entered==Counts(3));
        // Refused while frozen.
        const auto before=machine.View();const auto frozen_wanted=machine.WantedSerial();
        CHECK(machine.HandStep(Counts(1),kReady,toy.Fn()).outcome==LiveHandOutcome::refused_capturing && Unchanged(machine,before,frozen_wanted));
        CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::capturing && machine.Activity()==LivePlacementActivity::idle);
        CHECK(machine.BeginCapture(true).admission==LiveCaptureAdmission::already_capturing && machine.View().capturing);
    }
    // An estimate is running: its result is dropped, and the ask stays.
    {
        auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
        const auto drawn=machine.Placement();
        CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
        const auto job=machine.BeginJob();
        const auto serial=machine.Serial();
        const auto begin=machine.BeginCapture(true);
        CHECK(begin.admission==LiveCaptureAdmission::accepted && !begin.dropped_pending && begin.dropped_estimate && Same(begin.grant->mapping,drawn) && begin.grant->serial==serial);
        CHECK(begin.grant->source==LivePlacementSource::auto_start && !begin.grant->base_source && !begin.grant->hand_entered);
        CHECK(machine.Activity()==LivePlacementActivity::requested && machine.View().ask==LiveEstimateAsk::button);
        CHECK(machine.Accept(*job,Usable(Pose{0.9,0,0,0,0,0}))==LivePlacementMachine::AcceptKind::discarded_stale && !machine.HasPending());
        CHECK(machine.Activity()==LivePlacementActivity::requested && !machine.BeginJob());   // no job begins while P is held
        CHECK(Same(machine.Placement(),drawn) && machine.Serial()==serial);
        // The capture ended with no JPEG (it failed, or it was not a pair): P is free, and the ask that was kept runs once.
        machine.EndCapture();
        CHECK(!machine.Capturing());
        const auto again=machine.BeginJob();
        CHECK(again && again->full_reason==LiveFullReason::button && !machine.BeginJob());
    }
    // An estimate's result waits to be drawn: it is dropped, and the ask stays.
    {
        auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
        const auto drawn=machine.Placement();
        CHECK(machine.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
        const auto job=machine.BeginJob();
        machine.Accept(*job,Usable(Pose{0.9,0,0,0,0,0}));
        const auto wanted=machine.WantedSerial();
        const auto begin=machine.BeginCapture(true);
        CHECK(begin.dropped_pending && begin.dropped_estimate && Same(begin.grant->mapping,drawn) && machine.WantedSerial()==wanted+1 && !machine.HasPending());
        CHECK(machine.Activity()==LivePlacementActivity::requested);
        machine.Committed();
        CHECK(Same(machine.Placement(),drawn) && machine.Counters().placed==1);
    }
    // The screen was not drawn with P: nothing is admitted, and nothing changes.
    {
        auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
        const auto before=machine.View();const auto wanted=machine.WantedSerial();
        const auto begin=machine.BeginCapture(false);
        CHECK(begin.admission==LiveCaptureAdmission::not_drawn && !begin.grant && !machine.Capturing() && Unchanged(machine,before,wanted));
        LivePlacementMachine unconfigured;
        CHECK(unconfigured.BeginCapture(true).admission==LiveCaptureAdmission::not_configured);
    }
    // A session begins after the capture: it is free again (the machine is carried to the next session).
    {
        auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
        CHECK(machine.BeginCapture(true).admission==LiveCaptureAdmission::accepted);
        machine.BeginSession(kSha,kBase,true);
        CHECK(!machine.Capturing() && machine.HandStep(Counts(3),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);
    }
}

// 23. The result of a capture: `confirmed` leaves P as it is (the source and the serial too); a hand position stays by hand even when the JPEG
// fell to the condition's position; the other levels make P the JPEG's mapping as before, the serial moving only when P changes.
void CaptureResultKeepsP(){
    Toy toy;
    // confirmed, P by estimate (a position that was outside the envelope keeps what was said about it).
    {
        auto machine=Started();
        const auto job=machine.BeginJob();
        auto outcome=Usable(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});outcome.beyond_envelope=true;outcome.exceeded={"position"};
        machine.Accept(*job,outcome);machine.Committed();
        const auto drawn=machine.Placement();
        const auto serial=machine.Serial();
        const auto grant=machine.BeginCapture(true).grant;
        CHECK(machine.ApplyCapture(drawn,"confirmed",kSha,std::nullopt,grant->serial));
        const auto view=machine.View();
        CHECK(view.serial==serial && view.source==LivePlacementSource::auto_start && Same(view.mapping,drawn) && view.beyond_envelope && !view.capturing);
        CHECK(view.capture && view.capture->level=="confirmed" && view.capture->difference_px==0.0 && !machine.CaptureDifferencePending() && Same(view.capture->before,drawn));
        CHECK(view.activity==LivePlacementActivity::idle && !view.ask && !machine.BeginJob());
    }
    // confirmed with another mapping, or another serial: refused, and the capture is over all the same.
    {
        auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
        const auto before=machine.View();
        CHECK(machine.BeginCapture(true).admission==LiveCaptureAdmission::accepted);
        CHECK(!machine.ApplyCapture(ex::RegisteredMapping{Pose{0.31,-0.1,0.5,2.0,-1.0,0.2},{},std::nullopt},"confirmed",kSha) && !machine.Capturing());
        CHECK(Same(machine.Placement(),before.mapping) && machine.Serial()==before.serial && !machine.Capture());
        CHECK(!machine.ApplyCapture(before.mapping,"confirmed",kSha,std::nullopt,before.serial+1) && !machine.Capture());
        CHECK(!machine.ApplyCapture(before.mapping,"confirmed",kOtherSha) && machine.Serial()==before.serial);
        CHECK(machine.ApplyCapture(before.mapping,"confirmed",kSha,std::nullopt,before.serial) && machine.Serial()==before.serial);
    }
    // A hand position: confirmed, and G4 (the JPEG was made with the condition's own position because the hand position failed the cover check).
    for(const char* level:{"confirmed","G4"}){
        auto machine=ByHand(toy,Pose{0.3,-0.1,0.5,2.0,-1.0,0.2},Counts(3,1,4));
        const auto before=machine.View();
        const auto grant=machine.BeginCapture(true).grant;
        const ex::RegisteredMapping jpeg=std::string(level)=="G4"?ex::RegisteredMapping{kBase,{},std::nullopt}:before.mapping;
        CHECK(machine.ApplyCapture(jpeg,level,kSha,std::nullopt,grant->serial));
        const auto view=machine.View();
        CHECK(view.source==LivePlacementSource::manual && Same(view.mapping,before.mapping) && view.serial==before.serial && view.hand_entered==Counts(3,1,4) && view.base_source==before.base_source);
        CHECK(view.capture && view.capture->level==level && !machine.BeginJob() && view.activity==LivePlacementActivity::idle);
        // The position is by hand still: the next hand press builds on the same base.
        CHECK(machine.HandStep(Counts(1),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted && Same(toy.seen.back().first,*before.hand_base) && toy.seen.back().second==Counts(4,1,4));
    }
    // A hand position with a level that a hand position cannot have: refused, and P stays.
    {
        auto machine=ByHand(toy,Pose{0.3,-0.1,0.5,2.0,-1.0,0.2},Counts(3));
        const auto before=machine.View();
        for(const char* level:{"G1","G2","G3"})CHECK(!machine.ApplyCapture(ex::RegisteredMapping{kBase,{},std::nullopt},level,kSha) && machine.Source()==LivePlacementSource::manual && machine.Serial()==before.serial);
    }
    // Any other P: the JPEG's mapping, as before; the serial moves when P's nine numbers or its source change, and not otherwise.
    {
        auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
        const auto serial=machine.Serial();
        const ex::RegisteredMapping jpeg{Pose{0.31,-0.12,0.52,2.25,-1.125,0.21},{},std::nullopt};
        CHECK(machine.ApplyCapture(jpeg,"G1",kSha) && machine.Serial()==serial+1 && machine.Source()==LivePlacementSource::after_capture);
        CHECK(machine.ApplyCapture(jpeg,"G1",kSha) && machine.Serial()==serial+1);   // the same position from the same source: P did not change
        CHECK(machine.ApplyCapture(ex::RegisteredMapping{kBase,{},std::nullopt},"G4",kSha) && machine.Serial()==serial+2);
        // confirmed after a shot: P stays after-capture.
        const auto grant=machine.BeginCapture(true).grant;
        CHECK(machine.ApplyCapture(machine.Placement(),"confirmed",kSha,std::nullopt,grant->serial) && machine.Source()==LivePlacementSource::after_capture && machine.Serial()==serial+2);
    }
    // A pair capture, the result of which is applied, ends what a hand step left waiting too (the freeze took it, so nothing is left).
    {
        auto machine=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
        CHECK(machine.HandStep(Counts(3),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);
        (void)machine.BeginCapture(true);
        CHECK(machine.ApplyCapture(machine.Placement(),"confirmed",kSha) && !machine.HasPending());
    }
}

// 24. The condition changes: a hand-set P is dropped with the rest of P, and the screen is told; the same condition keeps it.
void ConditionChangeDropsTheHandPosition(){
    Toy toy;
    auto machine=ByHand(toy,Pose{0.3,-0.1,0.5,2.0,-1.0,0.2},Counts(3));
    const auto serial=machine.Serial();
    machine.BeginSession(kSha,kBase,true);
    CHECK(machine.Source()==LivePlacementSource::manual && machine.Serial()==serial && machine.View().hand_entered==Counts(3) && !machine.View().hand_discarded_by_condition);
    CHECK(machine.Activity()==LivePlacementActivity::idle && !machine.BeginJob());
    const Pose other{0.9,0,0,0,0,0};
    machine.BeginSession(kOtherSha,other,true);
    const auto view=machine.View();
    CHECK(view.source==LivePlacementSource::fixed_condition && view.serial>serial && view.mapping.pose.rx_deg==0.9 && !view.hand_entered && !view.hand_base && !view.base_source);
    CHECK(view.hand_discarded_by_condition && view.activity==LivePlacementActivity::requested && view.ask==LiveEstimateAsk::condition_changed);
    CHECK(!machine.HasPending());
    // A position that was not by hand: nothing is said about a hand position.
    auto plain=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
    plain.BeginSession(kOtherSha,other,true);
    CHECK(!plain.View().hand_discarded_by_condition);
    // A hand position that waited when the condition changed is dropped with the rest.
    auto waiting=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
    CHECK(waiting.HandStep(Counts(3),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);
    waiting.BeginSession(kOtherSha,other,true);
    CHECK(!waiting.HasPending() && waiting.Source()==LivePlacementSource::fixed_condition);
    // A hand position that waits when the same condition is begun again is kept (the new session draws it first).
    auto resumed=Placed(Pose{0.3,-0.1,0.5,2.0,-1.0,0.2});
    CHECK(resumed.HandStep(Counts(3),kReady,toy.Fn()).outcome==LiveHandOutcome::accepted);
    resumed.BeginSession(kSha,kBase,false);
    CHECK(resumed.HasPending() && resumed.View().hand_wanted==Counts(3));
}

// 25. P changes in four ways only: an estimate that was asked for, a hand press, a capture's result, a condition that changed. Calls that are
// none of these (refused presses, requests that are refused, jobs that were not asked for, a session of the same condition, the view) leave a
// hand position as it is.
void HandPositionDoesNotDrift(){
    Toy toy;
    auto machine=ByHand(toy,Pose{0.3,-0.1,0.5,2.0,-1.0,0.2},Counts(3,-1,4));
    const auto serial=machine.Serial(),wanted=machine.WantedSerial();
    const auto before=machine.View();
    for(int i=0;i<100000;++i){
        (void)machine.BeginJob();
        (void)machine.View();
        if(i%1000==0){
            machine.BeginSession(kSha,kBase,true);
            (void)machine.HandStep(Counts(1),LiveHandContext{false,true},toy.Fn());     // stopped
            (void)machine.HandStep(Counts(1),LiveHandContext{true,false},toy.Fn());     // not drawn
            (void)machine.HandStep(Counts(0),kReady,toy.Fn());                            // nothing
            (void)machine.BeginCapture(false);
            machine.Committed();
            machine.DrawFailed("none");
            machine.EndCapture();
        }
    }
    CHECK(machine.Serial()==serial && machine.WantedSerial()==wanted && Unchanged(machine,before,wanted) && machine.View().hand_base && Same(*machine.View().hand_base,*before.hand_base));
    CHECK(machine.Counters().jobs==before.counters.jobs && toy.calls==1);
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
        LayerTwoFields();
        BeyondEnvelope();
        HandStepBuildsOnTheBase();
        HandSumDoesNotDependOnTheOrder();
        HandStepBeforeTheDraw();
        HandStepRefused();
        HandStepWithdrawsTheEstimate();
        HandSumComesBackToZero();
        HandStepSizes();
        CaptureFreezesP();
        CaptureResultKeepsP();
        ConditionChangeDropsTheHandPosition();
        HandPositionDoesNotDrift();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
