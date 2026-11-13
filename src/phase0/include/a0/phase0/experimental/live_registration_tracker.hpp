#pragma once
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/render.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a0::phase0::experimental {
// The state machine that keeps the live C registered (#417, docs/design/fixed-composite-registration.md 4 "the live C",
// 11.9, addendum 11.21). It decides what to run next and what to draw with; it runs nothing itself.
//
// No thread, clock, window, file or random choice is in here. The caller passes the time (seconds on a clock of its own,
// only differences are read) and the frames; the estimate is made by a function object that the caller supplies (the
// estimator of the live C, a thin call of content_registration's reduced entrance). The same sequence of calls gives the
// same sequence of results.
//
// What it does, in the order of the design:
//  - a full estimate (every stage) is asked for at the start of a session, after a capture, when a change is detected and
//    when the condition is read again;
//  - between them a follow-up runs about every follow_interval_seconds: the fine stage only, from the estimate in force
//    (no coarse stage, no V6, no layer 2). It is taken when the estimator says it is usable and no change is detected;
//  - a change is detected when the follow-up is not usable (V1 or V3 failed), moved the seam column's displacement by more
//    than follow_change_limit_px, kept fewer matches than inlier_fraction_floor, or the brightness of A's band left the
//    value of the last full estimate by more than brightness_change_limit (the paper was replaced). A detected change drops
//    the follow-up's result and asks for a full estimate at once;
//  - a result carries the revision of the job; when something that the job was based on has changed since (a restart, a
//    capture, the end of the holding time) the result is discarded;
//  - an estimate that is taken does not necessarily replace the estimate in force. Two estimates of the same still scene differ
//    by the scatter of the measurement, and along the directions that the band at the seam does not constrain (the lean of B,
//    its height, the slope of the paper) that scatter is several pixels at the ends of C while it is a fraction of a pixel at
//    the seam. A guide that is drawn again for every estimate then trembles although nothing moved. So the estimate in force is
//    replaced only when the new one is further from it, along the seam, than the significance line
//    max(adopt_threshold_px, adopt_sigma_factor x sigma): a follow-up measures that distance itself (its change), a full
//    estimate is measured by a function the caller supplies. Below the line the estimate is taken as a confirmation: the holding
//    time and the follow-up interval restart, the mapping in force and what is drawn stay as they are;
//  - when no estimate can be made, the last verified estimate is held for hold_seconds. After that, and when there never was
//    one, C is drawn with the condition's fixed mapping (the base pose p_b, no surface: G4), and the state says why;
//  - the table of camera B is baked again only when the mapping that is wanted differs from the baked one by more than
//    bake_threshold_px (tau_bake); the difference is measured by a function the caller supplies.
//
// Every number that is a line or an interval is a member of LiveTrackerSettings with no default in code: they are design
// constants of the live view that are decided by measurement, and the caller passes them in.

inline constexpr std::string_view kLiveRegistrationDisabledReason="live-registration-disabled";

// fixed: C is drawn with the fixed mapping. tracking: the last attempt was usable and the estimate is current. holding: the
// last attempt was not usable and the last verified estimate is kept (for at most hold_seconds).
enum class LiveRegistrationState {fixed,tracking,holding};
[[nodiscard]] inline std::string_view LiveRegistrationStateCode(LiveRegistrationState state){
    switch(state){
    case LiveRegistrationState::fixed:return "fixed";
    case LiveRegistrationState::tracking:return "tracking";
    case LiveRegistrationState::holding:return "holding";
    }
    return {};
}

enum class LiveJobKind {full,follow};
[[nodiscard]] inline std::string_view LiveJobKindCode(LiveJobKind kind){return kind==LiveJobKind::full?"full":"follow";}

// Why a full estimate is wanted.
enum class LiveFullReason {session_start,after_capture,condition_reloaded,change_detected,hold_expired};
[[nodiscard]] inline std::string_view LiveFullReasonCode(LiveFullReason reason){
    switch(reason){
    case LiveFullReason::session_start:return "session-start";
    case LiveFullReason::after_capture:return "after-capture";
    case LiveFullReason::condition_reloaded:return "condition-reloaded";
    case LiveFullReason::change_detected:return "change-detected";
    case LiveFullReason::hold_expired:return "hold-expired";
    }
    return {};
}

// What the follow-up showed (a follow-up may show several).
enum class LiveChange {estimate_not_usable,change_over_limit,inliers_below_floor,brightness_changed,measurement_missing};
[[nodiscard]] inline std::string_view LiveChangeCode(LiveChange change){
    switch(change){
    case LiveChange::estimate_not_usable:return "estimate-not-usable";
    case LiveChange::change_over_limit:return "change-over-limit";
    case LiveChange::inliers_below_floor:return "inliers-below-floor";
    case LiveChange::brightness_changed:return "brightness-changed";
    case LiveChange::measurement_missing:return "measurement-missing";
    }
    return {};
}

// Lines and intervals. NaN until set, so an unset value is refused, not read. +infinity is a value for the three change lines
// and means "this detection is not made". Units: seconds; C pixels (the pixels of C's grid); a fraction; the brightness unit of
// LiveEstimateOutcome::band_brightness.
struct LiveTrackerSettings {
    double follow_interval_seconds=std::numeric_limits<double>::quiet_NaN();   // from the end of one job to the start of the next follow-up
    double hold_seconds=std::numeric_limits<double>::quiet_NaN();              // how long the last verified estimate is kept
    double full_retry_seconds=std::numeric_limits<double>::quiet_NaN();        // the pause before a full estimate is tried again after it failed
    double follow_change_limit_px=std::numeric_limits<double>::quiet_NaN();    // the follow-up's change of the seam column's displacement
    double inlier_fraction_floor=std::numeric_limits<double>::quiet_NaN();     // matches kept by the follow-up, 0..1
    double brightness_change_limit=std::numeric_limits<double>::quiet_NaN();   // |brightness - brightness of the last full estimate|
    double bake_threshold_px=std::numeric_limits<double>::quiet_NaN();         // tau_bake
    // The significance line of an estimate that would replace the estimate in force: max(adopt_threshold_px, adopt_sigma_factor x
    // the estimate's prediction sigma), measured along the seam in C pixels. Both 0 replaces the estimate in force for any
    // difference at all (every estimate that is taken is drawn).
    double adopt_threshold_px=std::numeric_limits<double>::quiet_NaN();
    double adopt_sigma_factor=std::numeric_limits<double>::quiet_NaN();
};
// Throws std::invalid_argument for an unset (NaN) or negative value, and for a fraction above 1. follow_interval_seconds and
// full_retry_seconds may be 0 (every call), hold_seconds too (no holding).
inline void ValidateLiveTrackerSettings(const LiveTrackerSettings& s){
    const auto require=[](double v,const char* what,bool allow_infinity){
        if(std::isnan(v) || v<0 || (!allow_infinity && std::isinf(v)))throw std::invalid_argument(std::string("live tracker: ")+what+" is unset or out of range");
    };
    require(s.follow_interval_seconds,"follow_interval_seconds",false);
    require(s.hold_seconds,"hold_seconds",false);
    require(s.full_retry_seconds,"full_retry_seconds",false);
    require(s.follow_change_limit_px,"follow_change_limit_px",true);
    require(s.inlier_fraction_floor,"inlier_fraction_floor",false);
    if(s.inlier_fraction_floor>1)throw std::invalid_argument("live tracker: inlier_fraction_floor is above 1");
    require(s.brightness_change_limit,"brightness_change_limit",true);
    require(s.bake_threshold_px,"bake_threshold_px",false);
    require(s.adopt_threshold_px,"adopt_threshold_px",false);
    require(s.adopt_sigma_factor,"adopt_sigma_factor",false);
}

// One job for the estimator.
struct LiveJob {
    std::uint64_t id{};
    std::uint64_t revision{};
    LiveJobKind kind{LiveJobKind::full};
    std::optional<LiveFullReason> full_reason;          // full only
    // follow: the estimate in force. full: the fixed mapping. The estimator starts the fine stage from the pose and the
    // surface of a follow-up, and a full estimate from the base pose.
    a0::m2::experimental::content_registration::PoseParameters start_pose;
    a0::m2::experimental::SurfacePlane start_surface;
    a0::m2::experimental::content_registration::PoseParameters base_pose;   // p_b
    // The caller's stop, set by whoever runs the job (the tracker leaves it empty). The estimator asks it between its steps and
    // ends early when it answers true, with an outcome that is not usable. It may be asked from more than one thread.
    std::function<bool()> stop;
};

// What the estimator returns. `usable` is the estimator's decision that C may be drawn with this estimate: for a full estimate
// that is a verified estimate, or an estimate outside the envelope that the condition applies anyway
// (applyBeyondEnvelope); for a follow-up it is V1 and V3 passed. There is no "within criteria" in C: the picture is a guide.
struct LiveEstimateOutcome {
    bool usable{false};
    a0::m2::experimental::content_registration::PoseParameters pose;   // p-hat, against the calibrated pose
    a0::m2::experimental::SurfacePlane surface;                        // pi-hat
    bool beyond_envelope{false};                                       // information for the display
    // Follow-up only. NaN when not measured; a follow-up that did not measure what a detection needs counts as a change.
    double change_px=std::numeric_limits<double>::quiet_NaN();         // change of the seam column's displacement from the start
    double inlier_fraction=std::numeric_limits<double>::quiet_NaN();
    // Brightness of A's band (either job). The value of a full estimate is the reference of the following follow-ups.
    double band_brightness=std::numeric_limits<double>::quiet_NaN();
    // The estimate's own uncertainty along the seam column (either job), in C pixels. NaN when not measured: the significance line
    // is then the threshold alone.
    double prediction_sigma_px=std::numeric_limits<double>::quiet_NaN();
    // Codes of the components (position, rotation, tilt, height, surface-height, surface-tilt) that are outside the condition's
    // range; set when beyond_envelope.
    std::vector<std::string> exceeded;
    std::vector<std::string> reasons;                                  // codes of why it is not usable
    std::string detail;                                                // for logs
};

enum class LiveAcceptKind {applied,kept_previous,discarded_stale};
struct LiveAcceptResult {
    LiveAcceptKind kind{LiveAcceptKind::discarded_stale};
    std::vector<LiveChange> changes;       // follow-up: what was detected
    bool full_requested{false};            // this result asked for a full estimate
    // applied only: whether the estimate replaced the one in force (false: taken as a confirmation, nothing changes on screen),
    // how far it was from the estimate in force along the seam (NaN when that was not measured) and the line it was judged by.
    bool replaced{false};
    double seam_difference_px=std::numeric_limits<double>::quiet_NaN();
    double adopt_line_px=std::numeric_limits<double>::quiet_NaN();
};

struct LiveTrackerCounters {
    std::uint64_t full_jobs{},follow_jobs{},applied{},kept_previous{},discarded_stale{},changes_detected{};
    std::uint64_t held{};   // applied estimates that were below the significance line and did not replace the estimate in force
};

// What C is drawn with now.
struct LiveRegistrationView {
    LiveRegistrationState state{LiveRegistrationState::fixed};
    a0::m2::experimental::RegisteredMapping mapping;   // pose and surface; never layer 2
    std::uint64_t serial{};                            // changes whenever `mapping` changes
    std::uint64_t revision{};
    bool full_pending{false};
    std::optional<LiveFullReason> full_reason;
    std::string fixed_reason;                          // state fixed: why ("not-estimated-yet", "estimate-failed", "hold-expired")
    std::vector<std::string> last_failure;             // codes of the last attempt that was not taken (reasons, then detections)
    std::optional<double> seconds_since_verified;      // tracking and holding
    bool beyond_envelope{false};                       // the estimate in force is outside the envelope
    std::vector<std::string> exceeded_components;      // which components of it (codes of LiveEstimateOutcome::exceeded)
    LiveTrackerCounters counters;
};

struct LiveRebakeDecision {
    bool rebake{false};
    double difference_px{0};
    a0::m2::experimental::RegisteredMapping wanted;
};
// The largest difference, in C pixels, between what camera B's table was baked with and what is wanted (measured on a sparse
// sample of C's B range by the caller).
using LiveMappingDifference=std::function<double(const a0::m2::experimental::RegisteredMapping& baked,const a0::m2::experimental::RegisteredMapping& wanted)>;

class LiveRegistrationTracker {
public:
    // base_pose is p_b of the condition. A new tracker needs a full estimate (session start) and draws the fixed mapping.
    LiveRegistrationTracker(const LiveTrackerSettings& settings,const a0::m2::experimental::content_registration::PoseParameters& base_pose)
        :settings_(settings),base_(base_pose){
        ValidateLiveTrackerSettings(settings_);
        RequireFinite(base_);
        fixed_.pose=base_;
        baked_=fixed_;
        drawn_=fixed_;
        revision_=1;
        need_full_=true;
        full_reason_=LiveFullReason::session_start;
        fixed_reason_="not-estimated-yet";
    }

    // Starts again for a new session or a condition that was read again (p_b may differ): the estimate in force is dropped, the
    // fixed mapping is drawn with the new base pose and a full estimate is wanted. The result of a job in flight is dropped.
    void Restart(double now,LiveFullReason reason,const a0::m2::experimental::content_registration::PoseParameters& base_pose){
        RequireFinite(base_pose);
        RequireTime(now);
        base_=base_pose;
        fixed_.pose=base_;
        has_estimate_=false;beyond_envelope_=false;exceeded_.clear();
        last_failure_.clear();
        fixed_reason_="not-estimated-yet";
        Invalidate();
        need_full_=true;full_reason_=reason;next_full_at_=-std::numeric_limits<double>::infinity();
        Refresh();
    }
    // A full estimate is wanted now (after a capture). The estimate in force stays on screen until the new one arrives; a job in
    // flight is dropped when it returns, because it was made on the scene before.
    void RequestFull(double now,LiveFullReason reason){
        RequireTime(now);
        Advance(now);
        Invalidate();
        need_full_=true;full_reason_=reason;next_full_at_=-std::numeric_limits<double>::infinity();
    }

    // The next job, or nullopt when nothing is due or one is in flight. The caller runs it and hands the outcome to Accept.
    [[nodiscard]] std::optional<LiveJob> BeginJob(double now){
        RequireTime(now);
        Advance(now);
        if(in_flight_)return std::nullopt;
        LiveJob job;
        job.base_pose=base_;
        if(need_full_){
            if(now<next_full_at_)return std::nullopt;
            job.kind=LiveJobKind::full;job.full_reason=full_reason_;
            job.start_pose=base_;job.start_surface={};
            ++counters_.full_jobs;
        }else if(has_estimate_){
            if(now<next_follow_at_)return std::nullopt;
            job.kind=LiveJobKind::follow;
            job.start_pose=estimate_.pose;job.start_surface=estimate_.surface;
            ++counters_.follow_jobs;
        }else return std::nullopt;
        job.id=++job_counter_;job.revision=revision_;
        in_flight_=true;in_flight_id_=job.id;
        return job;
    }

    // The outcome of a job. `now` is the time at which the job ended. `seam_difference` measures the distance, along the seam in C
    // pixels, between the estimate in force and the one of a full estimate (a follow-up carries its own, LiveEstimateOutcome::
    // change_px); without it a full estimate always replaces the estimate in force.
    LiveAcceptResult Accept(const LiveJob& job,const LiveEstimateOutcome& outcome,double now,const LiveMappingDifference& seam_difference={}){
        RequireTime(now);
        Advance(now);
        LiveAcceptResult result;
        const bool mine=in_flight_ && job.id==in_flight_id_;
        if(mine)in_flight_=false;
        if(!mine || job.revision!=revision_){
            ++counters_.discarded_stale;
            return result;
        }
        const bool finite=outcome.usable && Finite(outcome.pose) && Finite(outcome.surface);
        const bool usable=finite;
        if(job.kind==LiveJobKind::full){
            if(usable){
                const double distance=has_estimate_ && seam_difference ? SeamDistance(outcome,seam_difference) : std::numeric_limits<double>::quiet_NaN();
                Take(outcome,now,distance,result);
                if(std::isfinite(outcome.band_brightness))reference_brightness_=outcome.band_brightness;
                else reference_brightness_.reset();
                need_full_=false;full_reason_.reset();
                last_failure_.clear();
                result.kind=LiveAcceptKind::applied;
                ++counters_.applied;
            }else{
                last_ok_=false;
                need_full_=true;next_full_at_=now+settings_.full_retry_seconds;
                Fail(outcome,{});
                result.kind=LiveAcceptKind::kept_previous;
                ++counters_.kept_previous;
            }
            Refresh();
            return result;
        }
        // follow-up
        if(!usable)result.changes.push_back(LiveChange::estimate_not_usable);
        else{
            const auto missing=[](double v){return !std::isfinite(v);};
            if(missing(outcome.change_px) || missing(outcome.inlier_fraction))result.changes.push_back(LiveChange::measurement_missing);
            else{
                if(outcome.change_px>settings_.follow_change_limit_px)result.changes.push_back(LiveChange::change_over_limit);
                if(outcome.inlier_fraction<settings_.inlier_fraction_floor)result.changes.push_back(LiveChange::inliers_below_floor);
            }
            if(reference_brightness_ && std::isfinite(settings_.brightness_change_limit)){
                if(missing(outcome.band_brightness))result.changes.push_back(LiveChange::measurement_missing);
                else if(std::abs(outcome.band_brightness-*reference_brightness_)>settings_.brightness_change_limit)
                    result.changes.push_back(LiveChange::brightness_changed);
            }
        }
        if(result.changes.empty()){
            Take(outcome,now,outcome.change_px,result);
            last_failure_.clear();
            result.kind=LiveAcceptKind::applied;
            ++counters_.applied;
        }else{
            last_ok_=false;
            need_full_=true;full_reason_=LiveFullReason::change_detected;next_full_at_=now;   // at once
            Fail(outcome,result.changes);
            result.kind=LiveAcceptKind::kept_previous;result.full_requested=true;
            ++counters_.kept_previous;++counters_.changes_detected;
        }
        Refresh();
        return result;
    }

    // What to draw with now.
    [[nodiscard]] LiveRegistrationView View(double now){
        RequireTime(now);
        Advance(now);
        LiveRegistrationView view;
        view.mapping=drawn_;view.serial=serial_;view.revision=revision_;
        view.full_pending=need_full_;view.full_reason=need_full_?full_reason_:std::nullopt;
        view.last_failure=last_failure_;view.counters=counters_;
        if(has_estimate_){
            view.state=last_ok_?LiveRegistrationState::tracking:LiveRegistrationState::holding;
            view.seconds_since_verified=now-last_verified_;
            view.beyond_envelope=beyond_envelope_;
            view.exceeded_components=exceeded_;
        }else{
            view.state=LiveRegistrationState::fixed;view.fixed_reason=fixed_reason_;
        }
        return view;
    }

    // Whether camera B's table has to be baked again for the mapping that is wanted now. `difference` measures the distance
    // between two mappings in C pixels; it is called only when they differ, and a difference that is not finite counts as
    // over the threshold. After the caller has baked the wanted mapping it calls MarkBaked.
    [[nodiscard]] LiveRebakeDecision DecideRebake(double now,const LiveMappingDifference& difference){
        RequireTime(now);
        Advance(now);
        if(!difference)throw std::invalid_argument("live tracker: no mapping difference");
        LiveRebakeDecision decision;
        decision.wanted=drawn_;
        if(SameMapping(baked_,drawn_))return decision;
        decision.difference_px=difference(baked_,drawn_);
        decision.rebake=!std::isfinite(decision.difference_px) || decision.difference_px>settings_.bake_threshold_px;
        return decision;
    }
    void MarkBaked(const a0::m2::experimental::RegisteredMapping& mapping){
        if(mapping.parallax)throw std::invalid_argument("live tracker: layer 2 is not drawn by C");
        baked_=mapping;
    }

    [[nodiscard]] const LiveTrackerSettings& Settings() const {return settings_;}
    [[nodiscard]] const a0::m2::experimental::content_registration::PoseParameters& BasePose() const {return base_;}

private:
    using Pose=a0::m2::experimental::content_registration::PoseParameters;
    using Surface=a0::m2::experimental::SurfacePlane;
    static bool Finite(const Pose& p){
        for(const double v:{p.rx_deg,p.ry_deg,p.rz_deg,p.tx_mm,p.ty_mm,p.tz_percent})if(!std::isfinite(v))return false;
        return true;
    }
    static bool Finite(const Surface& s){return std::isfinite(s.h0_mm) && std::isfinite(s.slope_x) && std::isfinite(s.slope_y);}
    static void RequireFinite(const Pose& p){if(!Finite(p))throw std::invalid_argument("live tracker: the base pose must be finite");}
    static void RequireTime(double now){if(!std::isfinite(now))throw std::invalid_argument("live tracker: the time must be finite");}
    static bool SameMapping(const a0::m2::experimental::RegisteredMapping& a,const a0::m2::experimental::RegisteredMapping& b){
        return a.pose.rx_deg==b.pose.rx_deg && a.pose.ry_deg==b.pose.ry_deg && a.pose.rz_deg==b.pose.rz_deg &&
            a.pose.tx_mm==b.pose.tx_mm && a.pose.ty_mm==b.pose.ty_mm && a.pose.tz_percent==b.pose.tz_percent &&
            a.surface.h0_mm==b.surface.h0_mm && a.surface.slope_x==b.surface.slope_x && a.surface.slope_y==b.surface.slope_y;
    }
    // A result of a job that started before this point must not be taken. The job in flight keeps the worker busy until its
    // result comes back (and is dropped): the next job begins after that.
    void Invalidate(){++revision_;}
    // The holding time is over: drop the estimate and ask for a full one.
    void Advance(double now){
        if(has_estimate_ && now-last_verified_>settings_.hold_seconds){
            has_estimate_=false;beyond_envelope_=false;exceeded_.clear();
            fixed_reason_="hold-expired";
            Invalidate();
            need_full_=true;full_reason_=LiveFullReason::hold_expired;
            Refresh();
        }
    }
    // The distance, along the seam, between the estimate in force and the one of a full estimate; +infinity when the function cannot
    // tell (it throws), so an estimate that cannot be compared replaces the one in force.
    double SeamDistance(const LiveEstimateOutcome& outcome,const LiveMappingDifference& seam_difference) const {
        try{
            return seam_difference(estimate_,a0::m2::experimental::RegisteredMapping{outcome.pose,outcome.surface,std::nullopt});
        }catch(const std::exception&){return std::numeric_limits<double>::infinity();}
    }
    // The line that an estimate has to pass to replace the estimate in force: its own uncertainty counts when it is larger.
    double AdoptLine(const LiveEstimateOutcome& outcome) const {
        double line=settings_.adopt_threshold_px;
        if(std::isfinite(outcome.prediction_sigma_px))line=std::max(line,settings_.adopt_sigma_factor*outcome.prediction_sigma_px);
        return line;
    }
    // An estimate that was taken. It replaces the estimate in force when there is none, when its distance from it is not known, or
    // when the distance is over the significance line; otherwise it only confirms it. Either way the estimate in force was verified
    // now and the next follow-up is due after the interval.
    void Take(const LiveEstimateOutcome& outcome,double now,double distance_px,LiveAcceptResult& result){
        const double line=AdoptLine(outcome);
        const bool replace=!has_estimate_ || !(distance_px<=line);
        result.replaced=replace;result.seam_difference_px=distance_px;result.adopt_line_px=line;
        if(replace){
            estimate_.pose=outcome.pose;estimate_.surface=outcome.surface;estimate_.parallax.reset();
            beyond_envelope_=outcome.beyond_envelope;
            exceeded_=outcome.exceeded;
        }else ++counters_.held;
        has_estimate_=true;last_ok_=true;last_verified_=now;next_follow_at_=now+settings_.follow_interval_seconds;
    }
    void Fail(const LiveEstimateOutcome& outcome,const std::vector<LiveChange>& changes){
        last_failure_.clear();
        for(const auto& reason:outcome.reasons)last_failure_.push_back(reason);
        for(const auto change:changes)last_failure_.emplace_back(LiveChangeCode(change));
        if(!has_estimate_)fixed_reason_="estimate-failed";
    }
    // Recomputes what is drawn and counts a change of it.
    void Refresh(){
        const auto now_drawn=has_estimate_?estimate_:fixed_;
        if(!SameMapping(now_drawn,drawn_)){drawn_=now_drawn;++serial_;}
    }

    LiveTrackerSettings settings_;
    Pose base_;
    a0::m2::experimental::RegisteredMapping fixed_,estimate_,drawn_,baked_;
    bool has_estimate_{false},last_ok_{false},beyond_envelope_{false};
    double last_verified_{0};
    double next_follow_at_{0};
    double next_full_at_{-std::numeric_limits<double>::infinity()};
    bool need_full_{false};
    std::optional<LiveFullReason> full_reason_;
    std::optional<double> reference_brightness_;
    std::vector<std::string> exceeded_;
    std::string fixed_reason_;
    std::vector<std::string> last_failure_;
    std::uint64_t revision_{0},serial_{0},job_counter_{0},in_flight_id_{0};
    bool in_flight_{false};
    LiveTrackerCounters counters_;
};

// The estimator of the live C: runs one job on the two oriented, reduced live frames (the frames C is drawn from). The
// estimate is made on C's own grid, with the mapping of LiveViewRegisteredOutputToCamera, and never reaches the JPEG.
using LiveEstimator=std::function<LiveEstimateOutcome(const LiveJob&,const a0::m2::render::BgrImage& a,const a0::m2::render::BgrImage& b)>;

struct LiveStepResult {
    bool ran{false};
    std::optional<LiveJob> job;
    LiveAcceptResult accept;
};
// One synchronous step: begin a job if one is due, run the estimator and accept its outcome. The time is used for the start
// and the end alike; a worker that measures the run passes the two times to BeginJob and Accept itself. An exception of the
// estimator is an outcome that is not usable (reason "estimator-exception"), so a worker does not stop.
inline LiveStepResult RunLiveStep(LiveRegistrationTracker& tracker,double now,const a0::m2::render::BgrImage& a,
    const a0::m2::render::BgrImage& b,const LiveEstimator& estimator){
    if(!estimator)throw std::invalid_argument("live tracker: no estimator");
    LiveStepResult step;
    step.job=tracker.BeginJob(now);
    if(!step.job)return step;
    step.ran=true;
    LiveEstimateOutcome outcome;
    try{outcome=estimator(*step.job,a,b);}
    catch(const std::exception& error){
        outcome=LiveEstimateOutcome{};
        outcome.reasons.emplace_back("estimator-exception");
        outcome.detail=error.what();
    }
    step.accept=tracker.Accept(*step.job,outcome,now);
    return step;
}
}
