#pragma once
#include "a0/m2/experimental/confirmed_position.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/phase0/experimental/live_registration_tracker.hpp"
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a0::phase0::experimental {
// The position of camera B that the live C is drawn with (#437 stage 1; docs/design/fixed-composite-registration.md 11.32). It replaces
// the tracker (live_registration_tracker.hpp) as the thing that decides what C is drawn with: C is drawn with ONE confirmed position P,
// and P changes only when
//  - an estimate that was asked for ends with a position that can be drawn (an estimate is asked for twice: when a session begins with a
//    condition the window has not seen, and when the button is pressed),
//  - the condition changes (P goes back to the condition's position and one estimate is asked for), or
//  - a capture ends with a JPEG (P becomes the mapping that the JPEG was made with, layer 2 left out).
// Time and the content of the frames change nothing: no follow-up, no change detection, no holding time, no retry. An estimate that
// cannot decide a position leaves P as it is and says why.
//
// No thread, clock, window, file or random choice is in here. A caller that shares the machine between threads guards it (the runtime does).
// The machine is a value: copying it carries P across the sessions of the window.

// Where P comes from.
enum class LivePlacementSource {fixed_condition,auto_start,auto_button,after_capture};
[[nodiscard]] inline std::string_view LivePlacementSourceCode(LivePlacementSource source){
    switch(source){
    case LivePlacementSource::fixed_condition:return "fixed-condition";
    case LivePlacementSource::auto_start:return "auto-start";
    case LivePlacementSource::auto_button:return "auto-button";
    case LivePlacementSource::after_capture:return "after-capture";
    }
    return {};
}

// Why an estimate is asked for.
enum class LiveEstimateAsk {start,condition_changed,button};
[[nodiscard]] inline std::string_view LiveEstimateAskCode(LiveEstimateAsk ask){
    switch(ask){
    case LiveEstimateAsk::start:return "start";
    case LiveEstimateAsk::condition_changed:return "condition-changed";
    case LiveEstimateAsk::button:return "button";
    }
    return {};
}

// idle: nothing is asked for. requested: an estimate is asked for and has not begun (it waits for a pair of frames). estimating: a job
// is running, or its position waits to be drawn.
enum class LivePlacementActivity {idle,requested,estimating};

// The answer to RequestEstimate.
enum class LiveAskResult {accepted,busy,disabled};

// What the last estimate that was asked for came to. none: no estimate was asked for since the session began (or P was replaced since).
enum class LivePlacementOutcomeKind {none,placed,not_decided};

// What is known about the shot that made P (after_capture only). `level` is the geometry level of the JPEG ("G1" to "G4"); `before` is P as
// the live C drew it when the shutter was pressed; the difference is how far P moved from `before` along the seam, in pixels of the JPEG's raster
// (empty until it was measured on a plan: the machine cannot measure it). It is how far the live C moved, not a difference between the live C
// and the JPEG: after the shot the live C is drawn with the JPEG's own position. What stays different is layer 2, which the live C does not
// draw: `layer_two_max_px` is its largest shift in output pixels (the record's ParallaxSummary::max_shift_px), empty when layer 2 is
// absent or the record gave no usable number.
struct LivePlacementCaptureNote {
    std::string level;
    a0::m2::experimental::RegisteredMapping before;
    LivePlacementSource before_source{LivePlacementSource::fixed_condition};
    bool layer_two_dropped{false};       // the JPEG was made with layer 2, which the live C does not draw
    std::optional<double> layer_two_max_px;
    std::optional<double> difference_px;
};

struct LivePlacementCounters {
    std::uint64_t asked{},refused{},jobs{},placed{},not_decided{},released{},discarded_stale{};
};

// What the screen and the log read.
struct LivePlacementView {
    LivePlacementActivity activity{LivePlacementActivity::idle};
    std::optional<LiveEstimateAsk> ask;                  // requested or estimating
    a0::m2::experimental::RegisteredMapping mapping;     // P: pose and surface; never layer 2
    LivePlacementSource source{LivePlacementSource::fixed_condition};
    std::uint64_t serial{};                              // changes whenever P is replaced
    std::string condition_sha256;
    bool beyond_envelope{false};                         // the estimate that made P is outside the envelope
    std::vector<std::string> exceeded_components;        // which components of it
    LivePlacementOutcomeKind outcome{LivePlacementOutcomeKind::none};
    std::optional<LiveEstimateAsk> outcome_ask;          // what the last outcome answered
    std::vector<std::string> last_failure;               // outcome not_decided: the reasons
    std::optional<LivePlacementCaptureNote> capture;
    bool live_enabled{false};
    LivePlacementCounters counters;
};

// The pose and the surface of a mapping as `key=value` fields for a log line (ASCII, the classic locale, 17 significant digits, no negative
// zero). The one function that writes P: the live log and the capture's log both call it, so the two lines can be compared as text.
// `prefix` goes in front of every key (a line that carries two positions).
[[nodiscard]] inline std::string LivePlacementNumber(double value){
    if(!std::isfinite(value))return std::string("nan");
    return a0::m2::experimental::FormatRecordNumber(value);
}
[[nodiscard]] inline std::string LivePlacementFields(const a0::m2::experimental::RegisteredMapping& mapping,std::string_view prefix={}){
    const auto number=[](double value){return LivePlacementNumber(value);};
    const auto& p=mapping.pose;const auto& s=mapping.surface;
    const std::string k(prefix);
    return k+"rx_deg="+number(p.rx_deg)+" "+k+"ry_deg="+number(p.ry_deg)+" "+k+"rz_deg="+number(p.rz_deg)+" "+k+"tx_mm="+number(p.tx_mm)+" "+k+"ty_mm="+number(p.ty_mm)+
        " "+k+"tz_percent="+number(p.tz_percent)+" "+k+"h0_mm="+number(s.h0_mm)+" "+k+"slope_x="+number(s.slope_x)+" "+k+"slope_y="+number(s.slope_y);
}

// Whether a mapping carries a layer 2 that moves something (a profile with a q sample that is not 0).
[[nodiscard]] inline bool LivePlacementHasLayerTwo(const a0::m2::experimental::RegisteredMapping& mapping){
    return mapping.parallax.has_value() && !mapping.parallax->IsZero();
}
// Layer 2's largest shift (output px) as the record of the shot has it, for the mapping that the JPEG was made with. Empty when the mapping has no
// layer 2 that moves something, or the number is not usable (not finite or not positive): nothing is estimated here.
[[nodiscard]] inline std::optional<double> LivePlacementLayerTwoMaxPx(const a0::m2::experimental::RegisteredMapping& mapping,double summary_max_shift_px){
    if(!LivePlacementHasLayerTwo(mapping) || !std::isfinite(summary_max_shift_px) || !(summary_max_shift_px>0))return std::nullopt;
    return summary_max_shift_px;
}
// Layer 2 of a mapping as `key=value` fields for a log line: `layer_two_dropped=0|1 layer_two_max_px=<number>|unknown` (0 when there is none).
// The live log and the capture's log both call it.
[[nodiscard]] inline std::string LivePlacementLayerTwoFields(bool layer_two,const std::optional<double>& max_shift_px){
    if(!layer_two)return "layer_two_dropped=0 layer_two_max_px=0";
    return "layer_two_dropped=1 layer_two_max_px="+(max_shift_px?LivePlacementNumber(*max_shift_px):std::string("unknown"));
}

// Equal to the bit (-0 and +0 count as equal; NaN is never equal): P and the mapping that C was baked with are compared by this.
[[nodiscard]] inline bool LivePlacementSameMapping(const a0::m2::experimental::RegisteredMapping& a,const a0::m2::experimental::RegisteredMapping& b){
    return a.pose.rx_deg==b.pose.rx_deg && a.pose.ry_deg==b.pose.ry_deg && a.pose.rz_deg==b.pose.rz_deg &&
        a.pose.tx_mm==b.pose.tx_mm && a.pose.ty_mm==b.pose.ty_mm && a.pose.tz_percent==b.pose.tz_percent &&
        a.surface.h0_mm==b.surface.h0_mm && a.surface.slope_x==b.surface.slope_x && a.surface.slope_y==b.surface.slope_y;
}

class LivePlacementMachine {
public:
    LivePlacementMachine()=default;

    // A session begins with the condition of `condition_sha256` (the sha of the file that was read) and its base pose p_b.
    //  - the first session, or a condition that differs from the one P belongs to: P is the condition's position and one estimate is
    //    asked for (start for the first session, condition-changed otherwise). A request that was waiting is replaced;
    //  - the same condition: P stays as it is, whatever it came from, and nothing is asked for (a restart of Live by hand, or after a shot).
    // `live_enabled` is false when the condition has no registration.live: nothing is ever estimated, and P can still be set by a shot.
    // Throws std::invalid_argument for an empty sha or a base pose that is not finite.
    void BeginSession(const std::string& condition_sha256,const a0::m2::experimental::content_registration::PoseParameters& base_pose,bool live_enabled){
        if(condition_sha256.empty())throw std::invalid_argument("live placement: no condition");
        RequireFinite(base_pose);
        live_enabled_=live_enabled;
        if(!configured_ || sha_!=condition_sha256){
            const bool first=!configured_;
            configured_=true;sha_=condition_sha256;base_=base_pose;
            ResetPlacement();
            if(live_enabled_)Ask(first?LiveEstimateAsk::start:LiveEstimateAsk::condition_changed);
            return;
        }
        // The same condition: a job that was running when the last session ended did not finish, so it is asked for again; nothing else changes.
        base_=base_pose;
        if(activity_==LivePlacementActivity::estimating && !pending_){activity_=LivePlacementActivity::requested;in_flight_=false;}
        if(!live_enabled_){activity_=LivePlacementActivity::idle;ask_.reset();pending_.reset();in_flight_=false;}
    }
    [[nodiscard]] bool Configured() const {return configured_;}
    [[nodiscard]] const std::string& ConditionSha256() const {return sha_;}

    // The button: one estimate is asked for. Refused while one is asked for or running, and when the condition has no live estimate.
    LiveAskResult RequestEstimate(LiveEstimateAsk ask){
        ++counters_.asked;
        if(!configured_ || !live_enabled_){++counters_.refused;return LiveAskResult::disabled;}
        if(activity_!=LivePlacementActivity::idle){++counters_.refused;return LiveAskResult::busy;}
        Ask(ask);
        return LiveAskResult::accepted;
    }

    // The next job, or nullopt when nothing is asked for or one is running. Always a full estimate from the condition's base pose.
    [[nodiscard]] std::optional<LiveJob> BeginJob(){
        if(!configured_ || activity_!=LivePlacementActivity::requested || in_flight_)return std::nullopt;
        LiveJob job;
        job.kind=LiveJobKind::full;
        job.full_reason=ask_==LiveEstimateAsk::button?LiveFullReason::button:ask_==LiveEstimateAsk::condition_changed?LiveFullReason::condition_reloaded:LiveFullReason::session_start;
        job.base_pose=base_;job.start_pose=base_;job.start_surface={};
        job.id=++job_counter_;job.revision=revision_;
        in_flight_=true;in_flight_id_=job.id;
        activity_=LivePlacementActivity::estimating;
        ++counters_.jobs;
        return job;
    }

    enum class AcceptKind {pending_draw,not_decided,discarded_stale};
    // The outcome of a job. A usable one becomes the position that is wanted: P is replaced when it has been drawn (Committed), not before.
    // Anything else leaves P as it is, records the reasons, and asks for nothing more.
    AcceptKind Accept(const LiveJob& job,const LiveEstimateOutcome& outcome){
        const bool mine=in_flight_ && job.id==in_flight_id_;
        if(mine)in_flight_=false;
        if(!mine || job.revision!=revision_){
            ++counters_.discarded_stale;
            if(mine && activity_==LivePlacementActivity::estimating){activity_=LivePlacementActivity::idle;ask_.reset();}
            return AcceptKind::discarded_stale;
        }
        if(outcome.usable && Finite(outcome.pose) && Finite(outcome.surface)){
            pending_=Pending{a0::m2::experimental::RegisteredMapping{outcome.pose,outcome.surface,std::nullopt},outcome.beyond_envelope,outcome.exceeded};
            ++wanted_serial_;
            return AcceptKind::pending_draw;   // activity stays estimating until the position is drawn
        }
        Fail(outcome.reasons);
        return AcceptKind::not_decided;
    }
    // A job that did not estimate anything (no frames, the worker was stopped): it is asked for again, and nothing is recorded.
    void Release(const LiveJob& job){
        if(!in_flight_ || job.id!=in_flight_id_)return;
        in_flight_=false;
        if(job.revision==revision_){activity_=LivePlacementActivity::requested;++counters_.released;}
        else if(activity_==LivePlacementActivity::estimating){activity_=LivePlacementActivity::idle;ask_.reset();}
    }

    // The mapping that C has to be drawn with: the position that waits to be drawn, else P.
    [[nodiscard]] const a0::m2::experimental::RegisteredMapping& Wanted() const {return pending_?pending_->mapping:placement_;}
    [[nodiscard]] bool HasPending() const {return pending_.has_value();}
    // Changes whenever Wanted() does (a position waits, a shot, a new condition), not when a pending position becomes P.
    [[nodiscard]] std::uint64_t WantedSerial() const {return wanted_serial_;}
    // The wanted mapping was drawn: a pending position becomes P.
    void Committed(){
        if(!pending_)return;
        placement_=pending_->mapping;
        source_=ask_==LiveEstimateAsk::button?LivePlacementSource::auto_button:LivePlacementSource::auto_start;
        beyond_envelope_=pending_->beyond_envelope;exceeded_=pending_->exceeded;
        pending_.reset();
        ++serial_;capture_.reset();
        outcome_=LivePlacementOutcomeKind::placed;outcome_ask_=ask_;failure_.clear();
        activity_=LivePlacementActivity::idle;ask_.reset();
        ++counters_.placed;
    }
    // The wanted mapping could not be drawn: a pending position is dropped and P stays.
    void DrawFailed(std::string reason){
        if(!pending_)return;
        pending_.reset();
        ++wanted_serial_;
        Fail({std::move(reason)});
    }

    // A capture ended with a JPEG made with `mapping` (the mapping of its geometry level; layer 2, when it has one, is left out and noted).
    // P becomes that mapping and nothing is asked for. A request that waited, a job that ran and a position that waited are dropped.
    // `condition_sha256` is the sha of the condition the JPEG was made with: another one than the session's is refused (false) and
    // changes nothing, because the mapping is B's position against another condition's base pose.
    // `layer_two_max_px`: the largest shift of that layer 2 (LivePlacementLayerTwoMaxPx); kept in the note for the sentence and the log.
    bool ApplyCapture(const a0::m2::experimental::RegisteredMapping& mapping,const std::string& level,const std::string& condition_sha256,
        std::optional<double> layer_two_max_px=std::nullopt){
        if(!configured_ || condition_sha256!=sha_ || !Finite(mapping.pose) || !Finite(mapping.surface))return false;
        LivePlacementCaptureNote note;
        note.level=level;note.before=placement_;note.before_source=source_;
        note.layer_two_dropped=LivePlacementHasLayerTwo(mapping);
        if(note.layer_two_dropped && layer_two_max_px && std::isfinite(*layer_two_max_px) && *layer_two_max_px>0)note.layer_two_max_px=layer_two_max_px;
        placement_=a0::m2::experimental::RegisteredMapping{mapping.pose,mapping.surface,std::nullopt};
        source_=LivePlacementSource::after_capture;
        beyond_envelope_=false;exceeded_.clear();
        pending_.reset();in_flight_=false;
        ++revision_;++wanted_serial_;++serial_;
        activity_=LivePlacementActivity::idle;ask_.reset();
        outcome_=LivePlacementOutcomeKind::none;outcome_ask_.reset();failure_.clear();
        capture_=std::move(note);
        return true;
    }
    // How far P moved from before the shot to the mapping of the JPEG along the seam (pixels of the JPEG's raster). Not finite: not measured.
    void SetCaptureDifference(double pixels){
        if(capture_ && std::isfinite(pixels) && pixels>=0)capture_->difference_px=pixels;
    }
    [[nodiscard]] bool CaptureDifferencePending() const {return capture_ && !capture_->difference_px;}
    [[nodiscard]] const std::optional<LivePlacementCaptureNote>& Capture() const {return capture_;}

    [[nodiscard]] const a0::m2::experimental::RegisteredMapping& Placement() const {return placement_;}
    [[nodiscard]] LivePlacementSource Source() const {return source_;}
    [[nodiscard]] std::uint64_t Serial() const {return serial_;}
    [[nodiscard]] LivePlacementActivity Activity() const {return activity_;}
    [[nodiscard]] const LivePlacementCounters& Counters() const {return counters_;}
    [[nodiscard]] const a0::m2::experimental::content_registration::PoseParameters& BasePose() const {return base_;}

    [[nodiscard]] LivePlacementView View() const {
        LivePlacementView view;
        view.activity=activity_;view.ask=ask_;view.mapping=placement_;view.source=source_;view.serial=serial_;view.condition_sha256=sha_;
        view.beyond_envelope=beyond_envelope_;view.exceeded_components=exceeded_;
        view.outcome=outcome_;view.outcome_ask=outcome_ask_;view.last_failure=failure_;view.capture=capture_;
        view.live_enabled=live_enabled_;view.counters=counters_;
        return view;
    }

private:
    using Pose=a0::m2::experimental::content_registration::PoseParameters;
    using Surface=a0::m2::experimental::SurfacePlane;
    struct Pending {
        a0::m2::experimental::RegisteredMapping mapping;
        bool beyond_envelope{false};
        std::vector<std::string> exceeded;
    };
    static bool Finite(const Pose& p){
        for(const double v:{p.rx_deg,p.ry_deg,p.rz_deg,p.tx_mm,p.ty_mm,p.tz_percent})if(!std::isfinite(v))return false;
        return true;
    }
    static bool Finite(const Surface& s){return std::isfinite(s.h0_mm) && std::isfinite(s.slope_x) && std::isfinite(s.slope_y);}
    static void RequireFinite(const Pose& p){if(!Finite(p))throw std::invalid_argument("live placement: the base pose must be finite");}
    void ResetPlacement(){
        placement_=a0::m2::experimental::RegisteredMapping{base_,{},std::nullopt};
        source_=LivePlacementSource::fixed_condition;
        beyond_envelope_=false;exceeded_.clear();
        pending_.reset();in_flight_=false;capture_.reset();
        outcome_=LivePlacementOutcomeKind::none;outcome_ask_.reset();failure_.clear();
        activity_=LivePlacementActivity::idle;ask_.reset();
        ++revision_;++wanted_serial_;++serial_;
    }
    void Ask(LiveEstimateAsk ask){
        ask_=ask;activity_=LivePlacementActivity::requested;
        outcome_=LivePlacementOutcomeKind::none;outcome_ask_.reset();failure_.clear();
    }
    void Fail(std::vector<std::string> reasons){
        outcome_=LivePlacementOutcomeKind::not_decided;outcome_ask_=ask_;failure_=std::move(reasons);
        activity_=LivePlacementActivity::idle;ask_.reset();
        ++counters_.not_decided;
    }

    bool configured_{false},live_enabled_{false};
    std::string sha_;
    Pose base_;
    a0::m2::experimental::RegisteredMapping placement_;
    LivePlacementSource source_{LivePlacementSource::fixed_condition};
    bool beyond_envelope_{false};
    std::vector<std::string> exceeded_;
    std::optional<Pending> pending_;
    std::optional<LivePlacementCaptureNote> capture_;
    LivePlacementActivity activity_{LivePlacementActivity::idle};
    std::optional<LiveEstimateAsk> ask_;
    LivePlacementOutcomeKind outcome_{LivePlacementOutcomeKind::none};
    std::optional<LiveEstimateAsk> outcome_ask_;
    std::vector<std::string> failure_;
    std::uint64_t revision_{0},serial_{0},wanted_serial_{0},job_counter_{0},in_flight_id_{0};
    bool in_flight_{false};
    LivePlacementCounters counters_;
};
}
