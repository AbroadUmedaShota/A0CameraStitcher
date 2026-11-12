// The state machine of the live C's registration (#417, docs/design/fixed-composite-registration.md 4 "the live C", addendum
// 11.21): start, follow-up, change detection, full estimate, stale results, no estimate, holding, return to the fixed mapping,
// and the decision to bake camera B's table again. The estimator is a scripted fake: no image is read, no clock, thread or
// file is used. The settings below are values of this test, not proposals.
#include "a0/phase0/experimental/live_registration_tracker.hpp"
#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace a0::phase0::experimental;
namespace ex=a0::m2::experimental;
namespace render=a0::m2::render;
using Pose=ex::content_registration::PoseParameters;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr)
template<class Call> bool Throws(Call call){
    try{call();}catch(const std::invalid_argument&){return true;}
    return false;
}
#define REJECT(expr) Check(Throws([&]{(void)(expr);}),"rejects: " #expr)
constexpr double kNaN=std::numeric_limits<double>::quiet_NaN();
constexpr double kInf=std::numeric_limits<double>::infinity();

LiveTrackerSettings Settings(){
    LiveTrackerSettings s;
    s.follow_interval_seconds=0.5;s.hold_seconds=3.0;s.full_retry_seconds=1.0;
    s.follow_change_limit_px=0.4;s.inlier_fraction_floor=0.6;s.brightness_change_limit=20;s.bake_threshold_px=0.5;
    s.adopt_threshold_px=0;s.adopt_sigma_factor=0;   // every estimate that is taken replaces the one in force; the policy has its own section
    return s;
}
const Pose kBase{0,0,0.25,1.5,-2,0};
const render::BgrImage kFrame{1,1,{0,0,0}};

LiveEstimateOutcome Full(double tx,double h0=0,double brightness=100){
    LiveEstimateOutcome o;o.usable=true;o.pose=kBase;o.pose.tx_mm=tx;o.surface.h0_mm=h0;o.band_brightness=brightness;return o;
}
LiveEstimateOutcome Follow(double tx,double change=0.1,double inliers=0.9,double brightness=100,double h0=0){
    LiveEstimateOutcome o=Full(tx,h0,brightness);o.change_px=change;o.inlier_fraction=inliers;return o;
}
LiveEstimateOutcome NotUsable(const char* reason){LiveEstimateOutcome o;o.reasons.emplace_back(reason);return o;}

// A fake that returns the scripted outcomes in order and remembers the jobs.
struct Fake {
    std::deque<LiveEstimateOutcome> script;
    std::vector<LiveJob> jobs;
    LiveEstimator Estimator(){
        return [this](const LiveJob& job,const render::BgrImage&,const render::BgrImage&){
            jobs.push_back(job);
            if(script.empty())throw std::runtime_error("no scripted outcome");
            auto o=script.front();script.pop_front();return o;
        };
    }
};
bool SamePose(const Pose& a,const Pose& b){
    return a.rx_deg==b.rx_deg && a.ry_deg==b.ry_deg && a.rz_deg==b.rz_deg && a.tx_mm==b.tx_mm && a.ty_mm==b.ty_mm && a.tz_percent==b.tz_percent;
}
bool IsFixed(const ex::RegisteredMapping& m){return SamePose(m.pose,kBase) && m.surface.IsZero() && !m.parallax;}

void Validation(){
    CHECK(!Throws([]{ValidateLiveTrackerSettings(Settings());}));
    // Each member is required: an unset (NaN) value is refused, as is a negative one.
    for(int i=0;i<9;++i){
        auto s=Settings();
        double* members[]={&s.follow_interval_seconds,&s.hold_seconds,&s.full_retry_seconds,&s.follow_change_limit_px,&s.inlier_fraction_floor,
            &s.brightness_change_limit,&s.bake_threshold_px,&s.adopt_threshold_px,&s.adopt_sigma_factor};
        *members[i]=kNaN;
        Check(Throws([&]{ValidateLiveTrackerSettings(s);}),"NaN member refused");
        *members[i]=-0.1;
        Check(Throws([&]{ValidateLiveTrackerSettings(s);}),"negative member refused");
    }
    LiveTrackerSettings unset;
    REJECT(ValidateLiveTrackerSettings(unset));
    REJECT(LiveRegistrationTracker(unset,kBase));
    auto s=Settings();s.inlier_fraction_floor=1.5;
    REJECT(ValidateLiveTrackerSettings(s));
    // +infinity means a detection that is not made; it is refused for an interval.
    s=Settings();s.follow_change_limit_px=kInf;s.brightness_change_limit=kInf;
    CHECK(!Throws([&]{ValidateLiveTrackerSettings(s);}));
    s=Settings();s.hold_seconds=kInf;
    REJECT(ValidateLiveTrackerSettings(s));
    // Zero is a value for an interval.
    s=Settings();s.follow_interval_seconds=0;s.hold_seconds=0;s.full_retry_seconds=0;
    CHECK(!Throws([&]{ValidateLiveTrackerSettings(s);}));
    Pose bad=kBase;bad.rx_deg=kNaN;
    REJECT(LiveRegistrationTracker(Settings(),bad));
    LiveRegistrationTracker t(Settings(),kBase);
    REJECT(t.View(kNaN));
    REJECT(t.BeginJob(kInf));
    REJECT(t.Restart(0,LiveFullReason::session_start,bad));
}

void StartAndFollow(){
    LiveRegistrationTracker t(Settings(),kBase);
    auto v=t.View(0);
    CHECK(v.state==LiveRegistrationState::fixed && IsFixed(v.mapping) && v.fixed_reason=="not-estimated-yet");
    CHECK(v.full_pending && v.full_reason==LiveFullReason::session_start && !v.seconds_since_verified);
    const auto serial0=v.serial;
    // A full estimate is the first job, from the base pose; none is due while it runs.
    const auto first=t.BeginJob(0);
    CHECK(first && first->kind==LiveJobKind::full && first->full_reason==LiveFullReason::session_start);
    CHECK(first && SamePose(first->start_pose,kBase) && first->start_surface.IsZero() && SamePose(first->base_pose,kBase));
    CHECK(!t.BeginJob(0.1) && !t.BeginJob(5));
    // A usable full estimate is drawn; the follow-up is due 0.5 s after the end of the job.
    const auto taken=t.Accept(*first,Full(1.25,0.75),0.2);
    CHECK(taken.kind==LiveAcceptKind::applied && !taken.full_requested && taken.changes.empty());
    v=t.View(0.2);
    CHECK(v.state==LiveRegistrationState::tracking && v.mapping.pose.tx_mm==1.25 && v.mapping.surface.h0_mm==0.75 && v.serial!=serial0);
    CHECK(!v.full_pending && v.fixed_reason.empty() && v.seconds_since_verified && *v.seconds_since_verified==0);
    CHECK(!t.BeginJob(0.69));
    const auto second=t.BeginJob(0.71);
    CHECK(second && second->kind==LiveJobKind::follow && !second->full_reason);
    CHECK(second && second->start_pose.tx_mm==1.25 && second->start_surface.h0_mm==0.75);
    // A follow-up that is usable and shows no change replaces the estimate and starts from there next time.
    const auto serial1=t.View(0.71).serial;
    CHECK(t.Accept(*second,Follow(1.5,0.3,0.7,110,0.5),0.8).kind==LiveAcceptKind::applied);
    v=t.View(0.8);
    CHECK(v.state==LiveRegistrationState::tracking && v.mapping.pose.tx_mm==1.5 && v.mapping.surface.h0_mm==0.5 && v.serial!=serial1);
    const auto third=t.BeginJob(1.31);
    CHECK(third && third->kind==LiveJobKind::follow && third->start_pose.tx_mm==1.5 && third->start_surface.h0_mm==0.5);
    // The same estimate again does not change what is drawn.
    const auto serial2=t.View(1.31).serial;
    CHECK(t.Accept(*third,Follow(1.5,0.0,0.9,100,0.5),1.4).kind==LiveAcceptKind::applied);
    CHECK(t.View(1.4).serial==serial2);
    const auto c=t.View(1.4).counters;
    CHECK(c.full_jobs==1 && c.follow_jobs==2 && c.applied==3 && c.kept_previous==0 && c.discarded_stale==0 && c.changes_detected==0);
}

// A full estimate is in force at time 0 (brightness 100), the follow-up is due at 0.5.
LiveRegistrationTracker Tracking(){
    LiveRegistrationTracker t(Settings(),kBase);
    const auto job=t.BeginJob(0);
    (void)t.Accept(*job,Full(1.0),0);
    return t;
}
void Detection(const char* label,const LiveEstimateOutcome& outcome,std::vector<LiveChange> expected,bool detected){
    auto t=Tracking();
    const auto job=t.BeginJob(0.5);
    Check(job && job->kind==LiveJobKind::follow,label);
    if(!job)return;
    const auto serial=t.View(0.5).serial;
    const auto result=t.Accept(*job,outcome,0.6);
    Check(result.changes==expected,label);
    const auto v=t.View(0.6);
    if(detected){
        Check(result.kind==LiveAcceptKind::kept_previous && result.full_requested,label);
        // The estimate in force stays on screen; a full estimate is wanted at once.
        Check(v.state==LiveRegistrationState::holding && v.mapping.pose.tx_mm==1.0 && v.serial==serial && v.full_pending,label);
        Check(v.full_reason==LiveFullReason::change_detected && !v.last_failure.empty(),label);
        const auto next=t.BeginJob(0.6);
        Check(next && next->kind==LiveJobKind::full && next->full_reason==LiveFullReason::change_detected,label);
        Check(next && SamePose(next->start_pose,kBase),label);
        Check(t.View(0.6).counters.changes_detected==1,label);
    }else{
        Check(result.kind==LiveAcceptKind::applied && !result.full_requested && v.state==LiveRegistrationState::tracking,label);
    }
}
void Detections(){
    using C=LiveChange;
    Detection("small change",Follow(1.1,0.2,0.8,105),{},false);
    // A value equal to its line is not a change (strictly over, strictly under).
    Detection("change at the line",Follow(1.1,0.4,0.8,100),{},false);
    Detection("inliers at the floor",Follow(1.1,0.1,0.6,100),{},false);
    Detection("brightness at the limit",Follow(1.1,0.1,0.9,120),{},false);
    Detection("brightness at the limit, darker",Follow(1.1,0.1,0.9,80),{},false);
    Detection("change over the line",Follow(1.1,0.41,0.9,100),{C::change_over_limit},true);
    Detection("inliers under the floor",Follow(1.1,0.1,0.59,100),{C::inliers_below_floor},true);
    Detection("brighter",Follow(1.1,0.1,0.9,120.5),{C::brightness_changed},true);
    Detection("darker",Follow(1.1,0.1,0.9,79.5),{C::brightness_changed},true);
    Detection("two at once",Follow(1.1,0.9,0.1,100),{C::change_over_limit,C::inliers_below_floor},true);
    Detection("not usable",NotUsable("brown-lowe"),{C::estimate_not_usable},true);
    Detection("change not measured",Follow(1.1,kNaN,0.9,100),{C::measurement_missing},true);
    Detection("inliers not measured",Follow(1.1,0.1,kNaN,100),{C::measurement_missing},true);
    Detection("brightness not measured",Follow(1.1,0.1,0.9,kNaN),{C::measurement_missing},true);
    // A usable outcome with a pose that is not finite is not usable.
    auto bad=Follow(1.1);bad.pose.rx_deg=kNaN;
    Detection("pose not finite",bad,{C::estimate_not_usable},true);
    bad=Follow(1.1);bad.surface.slope_x=kInf;
    Detection("surface not finite",bad,{C::estimate_not_usable},true);

    // The lines can be switched off with +infinity; the others keep working.
    {
        auto s=Settings();s.follow_change_limit_px=kInf;s.brightness_change_limit=kInf;
        LiveRegistrationTracker t(s,kBase);
        (void)t.Accept(*t.BeginJob(0),Full(1.0),0);
        const auto job=t.BeginJob(0.5);
        CHECK(job && t.Accept(*job,Follow(1.1,50,0.9,255),0.6).kind==LiveAcceptKind::applied);
        const auto job2=t.BeginJob(1.2);
        CHECK(job2 && t.Accept(*job2,Follow(1.1,50,0.3,255),1.3).changes==std::vector<LiveChange>{LiveChange::inliers_below_floor});
    }
    // Without a brightness reference (the full estimate did not measure it) the brightness is not judged.
    {
        LiveRegistrationTracker t(Settings(),kBase);
        (void)t.Accept(*t.BeginJob(0),Full(1.0,0,kNaN),0);
        const auto job=t.BeginJob(0.5);
        CHECK(job && t.Accept(*job,Follow(1.1,0.1,0.9,255),0.6).kind==LiveAcceptKind::applied);
    }
}

void FullAfterChange(){
    auto t=Tracking();
    const auto follow=t.BeginJob(0.5);
    (void)t.Accept(*follow,Follow(1.1,0.9,0.9,100),0.6);
    const auto full=t.BeginJob(0.6);
    CHECK(full && full->kind==LiveJobKind::full && !t.BeginJob(0.7));
    // The new full estimate has another brightness: it is the new reference, and tracking is back.
    const auto serial=t.View(0.7).serial;
    CHECK(t.Accept(*full,Full(2.0,0,160),0.9).kind==LiveAcceptKind::applied);
    auto v=t.View(0.9);
    CHECK(v.state==LiveRegistrationState::tracking && v.mapping.pose.tx_mm==2.0 && v.serial!=serial && !v.full_pending && v.last_failure.empty());
    const auto next=t.BeginJob(1.41);
    CHECK(next && next->kind==LiveJobKind::follow && next->start_pose.tx_mm==2.0);
    CHECK(next && t.Accept(*next,Follow(2.0,0.1,0.9,170),1.5).kind==LiveAcceptKind::applied);   // 10 from 160
    const auto last=t.BeginJob(2.1);
    CHECK(last && t.Accept(*last,Follow(2.0,0.1,0.9,100),2.2).changes==std::vector<LiveChange>{LiveChange::brightness_changed});   // 60 from 160
}

void StaleResults(){
    // A capture ends while a follow-up runs: the follow-up was made on the scene before and is dropped.
    {
        auto t=Tracking();
        const auto follow=t.BeginJob(0.5);
        const auto serial=t.View(0.5).serial;
        t.RequestFull(0.55,LiveFullReason::after_capture);
        CHECK(!t.BeginJob(0.56));   // the worker is busy until the old job returns
        const auto v=t.View(0.55);
        CHECK(v.full_pending && v.full_reason==LiveFullReason::after_capture && v.state==LiveRegistrationState::tracking && v.mapping.pose.tx_mm==1.0);
        const auto stale=t.Accept(*follow,Follow(9,0.1,0.9,100),0.6);
        CHECK(stale.kind==LiveAcceptKind::discarded_stale && stale.changes.empty() && !stale.full_requested);
        CHECK(t.View(0.6).mapping.pose.tx_mm==1.0 && t.View(0.6).serial==serial && t.View(0.6).counters.discarded_stale==1);
        // The full estimate that was asked for runs next, and the old estimate is replaced by it.
        const auto full=t.BeginJob(0.6);
        CHECK(full && full->kind==LiveJobKind::full && full->full_reason==LiveFullReason::after_capture);
        CHECK(t.Accept(*full,Full(3.0),0.8).kind==LiveAcceptKind::applied && t.View(0.8).mapping.pose.tx_mm==3.0);
        // The same job handed in twice: the second is not in flight any more.
        CHECK(t.Accept(*full,Full(4.0),0.9).kind==LiveAcceptKind::discarded_stale && t.View(0.9).mapping.pose.tx_mm==3.0);
    }
    // A restart (a session, a condition read again) drops the estimate and the job in flight.
    {
        auto t=Tracking();
        const auto follow=t.BeginJob(0.5);
        Pose other=kBase;other.tx_mm=7;
        t.Restart(0.55,LiveFullReason::condition_reloaded,other);
        CHECK(!t.BeginJob(0.56));
        auto v=t.View(0.55);
        CHECK(v.state==LiveRegistrationState::fixed && v.mapping.pose.tx_mm==7 && v.mapping.surface.IsZero() && v.fixed_reason=="not-estimated-yet");
        CHECK(v.full_pending && v.full_reason==LiveFullReason::condition_reloaded);
        CHECK(t.Accept(*follow,Follow(9),0.6).kind==LiveAcceptKind::discarded_stale);
        CHECK(t.View(0.6).state==LiveRegistrationState::fixed && t.View(0.6).mapping.pose.tx_mm==7);
        const auto full=t.BeginJob(0.6);
        CHECK(full && full->kind==LiveJobKind::full && full->base_pose.tx_mm==7 && full->start_pose.tx_mm==7);
        // The base pose is the new one for the whole session.
        CHECK(t.BasePose().tx_mm==7);
    }
    // The holding time ends while a follow-up runs.
    {
        auto t=Tracking();
        const auto follow=t.BeginJob(0.5);
        CHECK(t.View(3.0).state==LiveRegistrationState::tracking);   // exactly hold_seconds after the last verification
        CHECK(t.View(3.1).state==LiveRegistrationState::fixed);
        CHECK(t.Accept(*follow,Follow(9),3.2).kind==LiveAcceptKind::discarded_stale);
        CHECK(t.View(3.2).state==LiveRegistrationState::fixed && IsFixed(t.View(3.2).mapping));
    }
    // A job from before is also stale after a later job has begun.
    {
        auto t=Tracking();
        const auto old=t.BeginJob(0.5);
        t.RequestFull(0.6,LiveFullReason::after_capture);
        CHECK(!t.BeginJob(0.6));
        CHECK(t.Accept(*old,Follow(9),0.7).kind==LiveAcceptKind::discarded_stale);
        const auto fresh=t.BeginJob(0.7);
        CHECK(fresh && fresh->revision!=old->revision && fresh->id!=old->id);
        // The old job handed in again does not end the fresh job: it is still in flight.
        CHECK(t.Accept(*old,Follow(9),0.8).kind==LiveAcceptKind::discarded_stale);
        CHECK(!t.BeginJob(0.8));
        CHECK(t.Accept(*fresh,Full(2),0.9).kind==LiveAcceptKind::applied && t.View(0.9).mapping.pose.tx_mm==2);
    }
}

void NoEstimateHoldAndFixed(){
    // The first full estimate fails: the fixed mapping stays, the reason is shown and the next try is full_retry_seconds later.
    LiveRegistrationTracker t(Settings(),kBase);
    const auto first=t.BeginJob(0);
    const auto failed=t.Accept(*first,NotUsable("coarse-no-unique-blocks"),0.4);
    CHECK(failed.kind==LiveAcceptKind::kept_previous && !failed.full_requested);
    auto v=t.View(0.4);
    CHECK(v.state==LiveRegistrationState::fixed && IsFixed(v.mapping) && v.fixed_reason=="estimate-failed" && v.full_pending);
    CHECK(v.last_failure==std::vector<std::string>{"coarse-no-unique-blocks"});
    CHECK(!t.BeginJob(1.39));
    // It succeeds the second time; a follow-up follows.
    const auto second=t.BeginJob(1.4);
    CHECK(second && second->kind==LiveJobKind::full && second->full_reason==LiveFullReason::session_start);
    CHECK(t.Accept(*second,Full(1.0),1.6).kind==LiveAcceptKind::applied);
    CHECK(t.View(1.6).state==LiveRegistrationState::tracking);
    // The follow-up at 2.1 is not usable: the estimate is held, and the full estimate that is asked for fails twice.
    const auto follow=t.BeginJob(2.1);
    CHECK(follow && t.Accept(*follow,NotUsable("residual-shape"),2.2).full_requested);
    v=t.View(2.2);
    CHECK(v.state==LiveRegistrationState::holding && v.mapping.pose.tx_mm==1.0 && v.seconds_since_verified && std::abs(*v.seconds_since_verified-0.6)<1e-12);
    const auto retry1=t.BeginJob(2.2);
    CHECK(retry1 && retry1->kind==LiveJobKind::full);
    CHECK(t.Accept(*retry1,NotUsable("too-few-points"),2.5).kind==LiveAcceptKind::kept_previous);
    v=t.View(2.5);
    CHECK(v.state==LiveRegistrationState::holding && v.mapping.pose.tx_mm==1.0 && v.last_failure==std::vector<std::string>{"too-few-points"});
    CHECK(v.fixed_reason.empty());
    CHECK(!t.BeginJob(3.49));
    const auto retry2=t.BeginJob(3.5);
    CHECK(retry2 && t.Accept(*retry2,NotUsable("too-few-points"),3.6).kind==LiveAcceptKind::kept_previous);
    // The estimate was last verified at 1.6 (the full estimate); the follow-up at 2.2 was not taken. It is held until 4.6.
    CHECK(t.View(4.6).state==LiveRegistrationState::holding);
    const auto serial=t.View(4.6).serial;
    v=t.View(4.61);
    CHECK(v.state==LiveRegistrationState::fixed && IsFixed(v.mapping) && v.fixed_reason=="hold-expired" && v.serial!=serial);
    CHECK(v.full_pending && v.full_reason==LiveFullReason::hold_expired && !v.seconds_since_verified);
    // A full estimate that succeeds takes C back from the fixed mapping.
    const auto again=t.BeginJob(4.7);
    CHECK(again && again->kind==LiveJobKind::full && again->full_reason==LiveFullReason::hold_expired);
    CHECK(t.Accept(*again,Full(2.5),4.9).kind==LiveAcceptKind::applied);
    v=t.View(4.9);
    CHECK(v.state==LiveRegistrationState::tracking && v.mapping.pose.tx_mm==2.5 && !v.full_pending);
    // Nothing is due before the follow-up interval.
    CHECK(!t.BeginJob(5.3) && t.BeginJob(5.41));
}

void BeyondEnvelope(){
    LiveRegistrationTracker t(Settings(),kBase);
    auto outcome=Full(1.0);outcome.beyond_envelope=true;
    (void)t.Accept(*t.BeginJob(0),outcome,0);
    CHECK(t.View(0).beyond_envelope && t.View(0).state==LiveRegistrationState::tracking);
    const auto job=t.BeginJob(0.5);
    (void)t.Accept(*job,Follow(1.1),0.6);
    CHECK(!t.View(0.6).beyond_envelope);
}

void Rebake(){
    LiveRegistrationTracker t(Settings(),kBase);
    int calls=0;
    double answer=0;
    const LiveMappingDifference difference=[&](const ex::RegisteredMapping& baked,const ex::RegisteredMapping& wanted){
        ++calls;
        CHECK(IsFixed(baked) || baked.pose.tx_mm==1.0);
        CHECK(!baked.parallax && !wanted.parallax);
        return answer;
    };
    // Nothing differs while the fixed mapping is drawn and baked: the function is not called.
    auto d=t.DecideRebake(0,difference);
    CHECK(!d.rebake && d.difference_px==0 && calls==0 && IsFixed(d.wanted));
    (void)t.Accept(*t.BeginJob(0),Full(1.0),0);
    // The wanted mapping differs: the threshold decides. A difference equal to tau_bake is not over it.
    answer=0.49;
    d=t.DecideRebake(0.1,difference);
    CHECK(!d.rebake && d.difference_px==0.49 && calls==1 && d.wanted.pose.tx_mm==1.0);
    answer=0.5;
    CHECK(!t.DecideRebake(0.1,difference).rebake);
    answer=0.5000001;
    d=t.DecideRebake(0.1,difference);
    CHECK(d.rebake && d.wanted.pose.tx_mm==1.0);
    answer=kNaN;
    CHECK(t.DecideRebake(0.1,difference).rebake);
    answer=kInf;
    CHECK(t.DecideRebake(0.1,difference).rebake);
    // Once the caller has baked the wanted mapping, the next decision needs no measurement.
    t.MarkBaked(d.wanted);
    const int before=calls;
    d=t.DecideRebake(0.2,difference);
    CHECK(!d.rebake && calls==before);
    // A small step (below tau_bake) is not baked; the baked mapping stays the old one, and a second small step is measured
    // against the baked mapping, not the previous step, so the drift is bounded by tau_bake.
    const auto follow=t.BeginJob(0.7);
    (void)t.Accept(*follow,Follow(1.2),0.7);
    answer=0.3;
    CHECK(!t.DecideRebake(0.7,difference).rebake);
    // When the holding time ends the wanted mapping is the fixed one again.
    answer=0.8;
    d=t.DecideRebake(5,difference);
    CHECK(d.rebake && IsFixed(d.wanted));
    t.MarkBaked(d.wanted);
    CHECK(!t.DecideRebake(5.1,difference).rebake);
    // Layer 2 is never baked into C, and the difference function is required.
    ex::RegisteredMapping with_layer2;with_layer2.parallax=ex::ParallaxProfile{};
    REJECT(t.MarkBaked(with_layer2));
    REJECT(t.DecideRebake(5.2,LiveMappingDifference{}));
}

// ---- The estimate in force is replaced only by an estimate that is significantly further from it along the seam ------------------
// The settings of the policy: the line is 0.3 px, or 1 x the estimate's own sigma when that is larger; a follow-up beyond 0.4 px asks for
// a full estimate.
LiveTrackerSettings PolicySettings(){auto s=Settings();s.adopt_threshold_px=0.3;s.adopt_sigma_factor=1.0;return s;}
LiveEstimateOutcome WithSigma(LiveEstimateOutcome o,double sigma){o.prediction_sigma_px=sigma;return o;}
// The seam distance of a full estimate is measured by the function the caller gives: here, 0.5 px per mm of tx.
LiveMappingDifference HalfPixelPerMillimetre(int& calls){
    return [&calls](const ex::RegisteredMapping& in_force,const ex::RegisteredMapping& candidate){
        ++calls;return 0.5*std::abs(candidate.pose.tx_mm-in_force.pose.tx_mm);
    };
}
void AdoptionConfirms(){
    int calls=0;
    const auto seam=HalfPixelPerMillimetre(calls);
    LiveRegistrationTracker t(PolicySettings(),kBase);
    // With nothing in force the first estimate is taken, and the function is not asked.
    auto result=t.Accept(*t.BeginJob(0),Full(1.0),0,seam);
    CHECK(result.kind==LiveAcceptKind::applied && result.replaced && calls==0 && t.View(0).mapping.pose.tx_mm==1.0);
    const auto serial=t.View(0).serial;
    // A follow-up below the line confirms the estimate in force: nothing that is drawn changes (not even the serial), the time of the
    // last verification and the interval of the next follow-up start again, and the state stays tracking.
    auto job=t.BeginJob(0.5);
    result=t.Accept(*job,WithSigma(Follow(1.9,0.25),0.1),0.6,seam);
    auto v=t.View(0.6);
    CHECK(result.kind==LiveAcceptKind::applied && !result.replaced && result.seam_difference_px==0.25 && result.adopt_line_px==0.3);
    CHECK(v.state==LiveRegistrationState::tracking && v.mapping.pose.tx_mm==1.0 && v.serial==serial && *v.seconds_since_verified==0);
    CHECK(v.counters.held==1 && v.counters.applied==2 && !t.BeginJob(1.0));
    // The next follow-up starts from the estimate in force, not from the one that was measured and not taken.
    job=t.BeginJob(1.11);
    CHECK(job && job->kind==LiveJobKind::follow && job->start_pose.tx_mm==1.0);
    // The holding time counts from the confirmation: a run of confirmations keeps the estimate for ever.
    (void)t.Accept(*job,WithSigma(Follow(1.0,0.1),0.1),1.2,seam);
    job=t.BeginJob(3.9);
    CHECK(job && job->kind==LiveJobKind::follow);
    (void)t.Accept(*job,WithSigma(Follow(1.0,0.1),0.1),4.0,seam);
    CHECK(t.View(4.0).state==LiveRegistrationState::tracking && t.View(4.0).serial==serial);
}
void AdoptionFollowAndFull(){
    int calls=0;
    const auto seam=HalfPixelPerMillimetre(calls);
    LiveRegistrationTracker t(PolicySettings(),kBase);
    (void)t.Accept(*t.BeginJob(0),Full(1.0),0,seam);
    const auto serial=t.View(0).serial;
    // A follow-up over the line and under the limit replaces the estimate in force.
    auto job=t.BeginJob(0.5);
    CHECK(job && job->start_pose.tx_mm==1.0);
    auto result=t.Accept(*job,WithSigma(Follow(1.2,0.35),0.1),0.55,seam);
    CHECK(result.replaced && result.seam_difference_px==0.35 && t.View(0.55).mapping.pose.tx_mm==1.2 && t.View(0.55).serial!=serial);
    // A follow-up over the limit is a change: a full estimate is wanted, and the estimate in force stays.
    job=t.BeginJob(1.1);
    result=t.Accept(*job,WithSigma(Follow(5.0,0.6),0.1),1.15,seam);
    CHECK(result.kind==LiveAcceptKind::kept_previous && result.full_requested && t.View(1.15).mapping.pose.tx_mm==1.2);
    // The full estimate of a change that was only scatter is 0.1 px from the estimate in force: below the line, it confirms it.
    const auto serial_before=t.View(1.15).serial;
    job=t.BeginJob(1.15);
    CHECK(job && job->kind==LiveJobKind::full);
    result=t.Accept(*job,WithSigma(Full(1.4),0.1),1.3,seam);
    CHECK(result.kind==LiveAcceptKind::applied && !result.replaced && calls==1 && result.seam_difference_px>0.09 && result.seam_difference_px<0.11);
    CHECK(t.View(1.3).mapping.pose.tx_mm==1.2 && t.View(1.3).serial==serial_before && t.View(1.3).state==LiveRegistrationState::tracking);
    CHECK(!t.View(1.3).full_pending);
    // The full estimate of a real move is far from it: it replaces it.
    job=t.BeginJob(1.8);
    result=t.Accept(*job,WithSigma(Follow(9.0,0.7),0.1),1.85,seam);
    CHECK(result.full_requested);
    job=t.BeginJob(1.85);
    result=t.Accept(*job,WithSigma(Full(9.0),0.1),2.0,seam);
    CHECK(result.replaced && std::abs(result.seam_difference_px-3.9)<1e-9 && t.View(2.0).mapping.pose.tx_mm==9.0);
    // A full estimate that is uncertain (sigma 0.6) needs 0.6 px, more than the threshold.
    job=t.BeginJob(2.6);
    result=t.Accept(*job,WithSigma(Follow(9.0,0.7),0.1),2.65,seam);
    job=t.BeginJob(2.65);
    result=t.Accept(*job,WithSigma(Full(9.9),0.6),2.8,seam);   // 0.45 px
    CHECK(!result.replaced && result.adopt_line_px==0.6 && t.View(2.8).mapping.pose.tx_mm==9.0);
    // An estimate that cannot be compared (the function throws) replaces the one in force, as does a missing function.
    t.RequestFull(3.0,LiveFullReason::after_capture);
    job=t.BeginJob(3.0);
    result=t.Accept(*job,Full(9.1),3.1,[](const ex::RegisteredMapping&,const ex::RegisteredMapping&)->double{throw std::invalid_argument("x");});
    CHECK(result.replaced && t.View(3.1).mapping.pose.tx_mm==9.1);
    t.RequestFull(3.2,LiveFullReason::after_capture);
    job=t.BeginJob(3.2);
    result=t.Accept(*job,Full(9.15),3.3);
    CHECK(result.replaced && t.View(3.3).mapping.pose.tx_mm==9.15);
    // The surface is part of the estimate: it is kept together with the pose.
    t.RequestFull(3.4,LiveFullReason::after_capture);
    job=t.BeginJob(3.4);
    result=t.Accept(*job,Full(9.15,2.0),3.5,[](const ex::RegisteredMapping&,const ex::RegisteredMapping&){return 0.05;});
    CHECK(!result.replaced && t.View(3.5).mapping.surface.h0_mm==0.0);
}
void AdoptionExceeded(){
    // The components beyond the range are those of the estimate in force.
    int calls=0;
    const auto seam=HalfPixelPerMillimetre(calls);
    LiveRegistrationTracker u(PolicySettings(),kBase);
    auto first=Full(1.0);first.beyond_envelope=true;first.exceeded={"position","tilt"};
    (void)u.Accept(*u.BeginJob(0),first,0,seam);
    CHECK(u.View(0).beyond_envelope && u.View(0).exceeded_components==(std::vector<std::string>{"position","tilt"}));
    auto confirm=Follow(1.0,0.1);confirm.beyond_envelope=false;confirm.exceeded.clear();
    (void)u.Accept(*u.BeginJob(0.5),confirm,0.55,seam);
    CHECK(u.View(0.55).beyond_envelope && u.View(0.55).exceeded_components.size()==2);   // held: still the estimate in force
    auto moved=Follow(1.35,0.35);moved.beyond_envelope=true;moved.exceeded={"height"};
    (void)u.Accept(*u.BeginJob(1.1),moved,1.15,seam);
    CHECK(u.View(1.15).exceeded_components==(std::vector<std::string>{"height"}));
    u.Restart(2.0,LiveFullReason::condition_reloaded,kBase);
    CHECK(u.View(2.0).exceeded_components.empty());
}

void Step(){
    // RunLiveStep begins, runs the estimator and accepts; an exception is an outcome that is not usable.
    Fake fake;
    LiveRegistrationTracker t(Settings(),kBase);
    const auto estimator=fake.Estimator();
    auto step=RunLiveStep(t,0,kFrame,kFrame,estimator);
    CHECK(step.ran && step.job && step.accept.kind==LiveAcceptKind::kept_previous);
    CHECK(t.View(0).last_failure==std::vector<std::string>{"estimator-exception"});
    CHECK(fake.jobs.size()==1 && fake.jobs[0].kind==LiveJobKind::full);
    step=RunLiveStep(t,0.5,kFrame,kFrame,estimator);   // full_retry_seconds not over
    CHECK(!step.ran && !step.job && fake.jobs.size()==1);
    fake.script={Full(1.0),Follow(1.1)};
    step=RunLiveStep(t,1.0,kFrame,kFrame,estimator);
    CHECK(step.ran && step.accept.kind==LiveAcceptKind::applied && t.View(1.0).state==LiveRegistrationState::tracking);
    step=RunLiveStep(t,1.4,kFrame,kFrame,estimator);
    CHECK(!step.ran);
    step=RunLiveStep(t,1.5,kFrame,kFrame,estimator);
    CHECK(step.ran && step.job->kind==LiveJobKind::follow && step.accept.kind==LiveAcceptKind::applied && t.View(1.5).mapping.pose.tx_mm==1.1);
    REJECT(RunLiveStep(t,2.0,kFrame,kFrame,LiveEstimator{}));
}

// The same sequence of calls gives the same sequence of states: a trace of a long run is compared with the same run again.
std::string Trace(){
    Fake fake;
    LiveRegistrationTracker t(Settings(),kBase);
    const auto estimator=fake.Estimator();
    fake.script={NotUsable("fit-failed"),Full(1.0,0.5,100),Follow(1.1,0.1,0.9,101,0.5),Follow(1.2,0.2,0.9,99,0.5),Follow(1.2,0.9,0.9,99,0.5),
        Full(1.4,0.6,130),Follow(1.5,0.1,0.9,131,0.6),NotUsable("brown-lowe"),NotUsable("brown-lowe"),NotUsable("brown-lowe"),Full(2,0,90)};
    std::ostringstream out;
    out.precision(17);
    for(int i=0;i<80;++i){
        const double now=0.25*i;
        if(i==30)t.RequestFull(now,LiveFullReason::after_capture);
        const auto step=RunLiveStep(t,now,kFrame,kFrame,estimator);
        const auto v=t.View(now);
        out<<i<<' '<<step.ran<<' '<<(step.job?LiveJobKindCode(step.job->kind):"-")<<' '<<static_cast<int>(step.accept.kind)<<' '
           <<LiveRegistrationStateCode(v.state)<<' '<<v.mapping.pose.tx_mm<<' '<<v.mapping.surface.h0_mm<<' '<<v.serial<<' '<<v.revision<<' '<<v.fixed_reason<<'\n';
    }
    return out.str();
}
void Determinism(){
    const auto a=Trace(),b=Trace();
    CHECK(!a.empty() && a==b);
    // The run is not a constant: it went through every state.
    CHECK(a.find("tracking")!=std::string::npos && a.find("holding")!=std::string::npos && a.find("fixed")!=std::string::npos);
}
}

int main(){
    try{
        Validation();
        StartAndFollow();
        Detections();
        FullAfterChange();
        StaleResults();
        NoEstimateHoldAndFixed();
        BeyondEnvelope();
        AdoptionConfirms();
        AdoptionFollowAndFull();
        AdoptionExceeded();
        Rebake();
        Step();
        Determinism();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
