#pragma once
#include "a0/m2/experimental/confirmed_position.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/phase0/experimental/live_registration_tracker.hpp"
#include <cmath>
#include <cstdint>
#include <functional>
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
//  - a person presses a hand step (#437 stage 2: along or across the seam, or about the optical axis, by whole counts of the smallest step),
//  - the condition changes (P goes back to the condition's position and one estimate is asked for), or
//  - a capture ends with a JPEG (P becomes the mapping that the JPEG was made with, layer 2 left out; a hand-set P and a P that the JPEG
//    itself was made with stay as they are).
// Time and the content of the frames change nothing: no follow-up, no change detection, no holding time, no retry. An estimate that
// cannot decide a position leaves P as it is and says why.
//
// A hand step is the position before the first hand press (hand_base) plus the sum of the counts entered since, solved once by a function
// that the caller gives (confirmed_position.hpp's SolveHandStep behind the checks of confirmed_position_check.hpp; this header stays free
// of the condition's libraries). A press withdraws the estimate that was asked for or running, and its result. A pair capture freezes P:
// from BeginCapture until the result is in, nothing changes P, and what the capture is given is the P that is drawn, not the one that waits.
//
// No thread, clock, window, file or random choice is in here. A caller that shares the machine between threads guards it (the runtime does).
// The machine is a value: copying it carries P across the sessions of the window.

// Where P comes from. manual: a hand step on top of a position that was not manual (base_source says which).
enum class LivePlacementSource {fixed_condition,auto_start,auto_button,after_capture,manual};
[[nodiscard]] inline std::string_view LivePlacementSourceCode(LivePlacementSource source){
    switch(source){
    case LivePlacementSource::fixed_condition:return "fixed-condition";
    case LivePlacementSource::auto_start:return "auto-start";
    case LivePlacementSource::auto_button:return "auto-button";
    case LivePlacementSource::after_capture:return "after-capture";
    case LivePlacementSource::manual:return "manual";
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
enum class LiveAskResult {accepted,busy,disabled,capturing};

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
    std::uint64_t hand_accepted{},hand_refused{},hand_withdrawn_estimates{},hand_draw_failed{},captures_begun{};
};

// ---- Hand steps and the freeze of a capture (#437 stage 2) ----
// What a hand press came to. Everything but `accepted` changed nothing.
enum class LiveHandOutcome {
    accepted,
    refused_not_configured,   // no session has begun
    refused_capturing,        // a pair capture was requested and its result is not in
    refused_stopped,          // Live is stopped or stopping: nothing is baked, so a new position could not be drawn
    refused_not_drawn,        // the picture on the screen was not drawn with P (the bake of P is not done, or it failed)
    refused_zero,             // the press adds nothing
    refused_range,            // the sum leaves the range of the counts
    refused_envelope,         // the result's pose is outside the pose envelope
    refused_cover,            // the result's mapping fails the cheap cover check
    refused_unsolvable,       // the sum could not be turned into a pose
    refused_error,            // the check itself failed (detail has what it said)
    refused_stale,            // P, or the hand position that waits, changed while the check ran; press again
};
[[nodiscard]] inline std::string_view LiveHandOutcomeCode(LiveHandOutcome outcome){
    switch(outcome){
    case LiveHandOutcome::accepted:return "accepted";
    case LiveHandOutcome::refused_not_configured:return "not-configured";
    case LiveHandOutcome::refused_capturing:return "capturing";
    case LiveHandOutcome::refused_stopped:return "stopped";
    case LiveHandOutcome::refused_not_drawn:return "not-drawn";
    case LiveHandOutcome::refused_zero:return "zero";
    case LiveHandOutcome::refused_range:return "range";
    case LiveHandOutcome::refused_envelope:return "envelope";
    case LiveHandOutcome::refused_cover:return "cover";
    case LiveHandOutcome::refused_unsolvable:return "unsolvable";
    case LiveHandOutcome::refused_error:return "error";
    case LiveHandOutcome::refused_stale:return "stale";
    }
    return {};
}
// What the caller's check says of a hand step (the answer of CheckHandStep, in this header's own words).
struct LiveHandCheck {
    enum class Status {accepted,refused_envelope,refused_cover,refused_unsolvable,error};
    Status status{Status::error};
    a0::m2::experimental::RegisteredMapping mapping;     // the position the step gives (used when accepted)
    std::vector<std::string> exceeded;                   // refused_envelope: the components outside the envelope
    bool seam_covered{true},output_covered{true};        // refused_cover: which of the two failed
    std::string detail;                                  // error: what the check threw
};
// The check: the position before the first hand press (no layer 2) and the sum of the counts entered since (never 0: a sum of 0 is not
// checked, it goes back to hand_base). It solves the sum, measures the pose against the envelope and covers the result. It is called with
// no lock held (a cover check allocates a full-size frame), and it may throw.
using LiveHandChecker=std::function<LiveHandCheck(const a0::m2::experimental::RegisteredMapping& hand_base,const a0::m2::experimental::HandCounts& total)>;

// What the machine cannot know and the caller says: the window's Live is running, and the picture on the screen was drawn with a table
// baked for P (the caller compares the drawn plan's baked mapping with Placement()).
struct LiveHandContext {
    bool live_running{false};
    bool drawn_with_placement{false};
};
// What a hand press is made against; the commit refuses it when anything has changed since.
struct LiveHandProposal {
    a0::m2::experimental::RegisteredMapping base;        // the position before the first hand press
    LivePlacementSource base_source{LivePlacementSource::fixed_condition};
    a0::m2::experimental::HandCounts total;              // the sum of the counts, with this press
    bool zero{false};                                    // the sum is 0: the position goes back to `base`, nothing to check
    bool base_beyond_envelope{false};                    // what was said of `base` (an estimate that was applied beyond a range), kept to say it again when the position goes back
    std::vector<std::string> base_exceeded;
    // What the press was made against: the machine's revision, the counter of the hand position that waits (hand_serial) and P's serial.
    // An estimate's result that comes while the check runs is none of these: the press withdraws it.
    std::uint64_t revision{},hand_serial{},serial{};
};
struct LiveHandPrepared {
    LiveHandOutcome outcome{LiveHandOutcome::refused_not_configured};
    LiveHandProposal proposal;
};
struct LiveHandResult {
    LiveHandOutcome outcome{LiveHandOutcome::refused_not_configured};
    a0::m2::experimental::HandCounts total;              // the sum that was tried (accepted: the sum in force once it is drawn)
    a0::m2::experimental::RegisteredMapping mapping;     // accepted: the position that waits to be drawn (or P itself when nothing waits)
    bool returned_to_base{false};                        // accepted: the sum came back to 0, so the position goes back to what it was before the hand steps
    bool became_condition_position{false};               // accepted: the result is the condition's own position, so the hand mark is dropped
    bool withdrew_estimate{false};                       // accepted: an estimate that was asked for, running or waiting to be drawn was withdrawn
    std::vector<std::string> exceeded;                   // refused_envelope
    bool seam_covered{true},output_covered{true};        // refused_cover
    std::string detail;                                  // refused_error
};

// What a pair capture is given: P as it is drawn. Never the position that waits to be drawn.
struct LivePlacementGrant {
    a0::m2::experimental::RegisteredMapping mapping;
    std::uint64_t serial{};
    LivePlacementSource source{LivePlacementSource::fixed_condition};
    std::optional<LivePlacementSource> base_source;                      // manual only
    std::optional<a0::m2::experimental::HandCounts> hand_entered;        // manual only
    std::string condition_sha256;
};
enum class LiveCaptureAdmission {accepted,not_configured,already_capturing,not_drawn};
struct LiveCaptureBegin {
    LiveCaptureAdmission admission{LiveCaptureAdmission::not_configured};
    std::optional<LivePlacementGrant> grant;
    bool dropped_pending{false};     // a position that waited to be drawn (an estimate's or a hand step's) was dropped
    bool dropped_estimate{false};    // an estimate was running or its result waited: the result is dropped and the ask stays
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
    // Hand steps (#437 stage 2). base_source, hand_base and hand_entered are set while P is manual.
    std::optional<LivePlacementSource> base_source;
    std::optional<a0::m2::experimental::RegisteredMapping> hand_base;
    std::optional<a0::m2::experimental::HandCounts> hand_entered;       // the sum in P
    std::optional<a0::m2::experimental::HandCounts> hand_wanted;        // a hand position waits to be drawn: its sum (0: P goes back to hand_base)
    bool hand_returned{false};                // the last change of P went back to what it was before the hand steps
    bool hand_discarded_by_condition{false};  // the session began with another condition and a hand-set P was dropped
    bool capturing{false};                    // a pair capture was requested and its result is not in
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

// The two cameras of every position the logs write, as `key=value` fields (ADR-0045's reference and target camera, the names the report's
// confirmedPosition carries). The live log's position line and the capture's confirmed-position line both call this one function, so the
// cameras read the same in both; the nine numbers stay in LivePlacementFields (which a `before_` line also uses, and which therefore
// does not carry the cameras).
[[nodiscard]] inline std::string LivePlacementCameraFields(){
    return "reference_camera="+std::string(a0::m2::experimental::kConfirmedReferenceCamera)+" target_camera="+std::string(a0::m2::experimental::kConfirmedTargetCamera);
}

// The sum of the hand counts of a position that is by hand, as `key=value` fields for a log line: the displacement at the seam column's middle row
// (output px) and the rotation (degrees), written like the position. The live log and the capture's log both call it.
[[nodiscard]] inline std::string LivePlacementManualFields(const a0::m2::experimental::HandCounts& counts){
    const auto values=a0::m2::experimental::HandValuesOf(counts);
    return "manual_seam_dx_px="+LivePlacementNumber(values.seam_dx_px)+" manual_seam_dy_px="+LivePlacementNumber(values.seam_dy_px)+
        " manual_rotation_deg="+LivePlacementNumber(values.rotation_deg);
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

// The reasons the cover check gives to an estimate's position or a hand step's (codes for a log line and a state row).
inline constexpr std::string_view kLivePositionSeamOutsideOverlapReason="seam-outside-overlap";
inline constexpr std::string_view kLivePositionOutputUncoveredReason="output-uncovered";
inline constexpr std::string_view kLivePositionCheckFailedReason="position-check-failed";

// The level code of a capture that was made with P itself (the geometry level `confirmed`, a0::m2::experimental::GeometryLevelCode).
inline constexpr std::string_view kLiveCaptureConfirmedLevel="confirmed";

// The answer of the caller's check for a proposal, with whatever the check throws made into an answer (never swallowed: it is the reason of the
// refusal). A proposal whose sum is 0 is not checked: it goes back to its base.
[[nodiscard]] inline LiveHandCheck EvaluateLiveHandCheck(const LiveHandChecker& checker,const LiveHandProposal& proposal){
    LiveHandCheck out;
    if(proposal.zero){out.status=LiveHandCheck::Status::accepted;out.mapping=proposal.base;return out;}
    out.status=LiveHandCheck::Status::error;
    if(!checker){out.detail="no check of hand steps is connected";return out;}
    try{return checker(proposal.base,proposal.total);}
    catch(const std::exception& error){out.detail=error.what();}
    catch(...){out.detail="an exception of an unknown kind";}
    return out;
}

class LivePlacementMachine {
public:
    LivePlacementMachine()=default;

    // A session begins with the condition of `condition_sha256` (the sha of the file that was read) and its base pose p_b.
    //  - the first session, or a condition that differs from the one P belongs to: P is the condition's position (a hand-set P is dropped
    //    with it) and one estimate is asked for (start for the first session, condition-changed otherwise). A request that was waiting is replaced;
    //  - the same condition: P stays as it is, whatever it came from (a hand-set P too), and nothing is asked for (a restart of Live by hand,
    //    or after a shot). A request that was left (an estimate that was running when the last session ended, or one that a capture kept)
    //    is asked for again, once.
    // `live_enabled` is false when the condition has no registration.live: nothing is ever estimated, and P can still be set by a shot or by hand.
    // Throws std::invalid_argument for an empty sha or a base pose that is not finite.
    void BeginSession(const std::string& condition_sha256,const a0::m2::experimental::content_registration::PoseParameters& base_pose,bool live_enabled){
        if(condition_sha256.empty())throw std::invalid_argument("live placement: no condition");
        RequireFinite(base_pose);
        live_enabled_=live_enabled;
        capturing_=false;   // a session begins after the capture it follows (or after one that did not end with a JPEG)
        if(!configured_ || sha_!=condition_sha256){
            const bool first=!configured_;
            const bool hand_dropped=configured_ && source_==LivePlacementSource::manual;
            configured_=true;sha_=condition_sha256;base_=base_pose;
            ResetPlacement();
            hand_discarded_by_condition_=hand_dropped;
            if(live_enabled_)Ask(first?LiveEstimateAsk::start:LiveEstimateAsk::condition_changed);
            return;
        }
        // The same condition: the worker of the session before is gone, so a job that was running did not finish and is asked for again.
        base_=base_pose;
        in_flight_=false;
        if(activity_==LivePlacementActivity::estimating && !pending_)activity_=LivePlacementActivity::requested;
        if(!live_enabled_){
            activity_=LivePlacementActivity::idle;ask_.reset();
            if(pending_ && pending_->kind==PendingKind::estimate)pending_.reset();
        }
    }
    [[nodiscard]] bool Configured() const {return configured_;}
    [[nodiscard]] const std::string& ConditionSha256() const {return sha_;}

    // The button: one estimate is asked for. Refused while one is asked for or running, when the condition has no live estimate, and
    // from a pair capture's request until its result is in.
    LiveAskResult RequestEstimate(LiveEstimateAsk ask){
        ++counters_.asked;
        if(!configured_ || !live_enabled_){++counters_.refused;return LiveAskResult::disabled;}
        if(capturing_){++counters_.refused;return LiveAskResult::capturing;}
        if(activity_!=LivePlacementActivity::idle){++counters_.refused;return LiveAskResult::busy;}
        Ask(ask);
        return LiveAskResult::accepted;
    }

    // The next job, or nullopt when nothing is asked for or one is running (or a pair capture holds P). Always a full estimate from the
    // condition's base pose.
    [[nodiscard]] std::optional<LiveJob> BeginJob(){
        if(!configured_ || capturing_ || activity_!=LivePlacementActivity::requested || in_flight_)return std::nullopt;
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
    // Anything else leaves P as it is, records the reasons, and asks for nothing more. The result of a job that a hand press, a pair
    // capture or a new condition overtook is dropped.
    AcceptKind Accept(const LiveJob& job,const LiveEstimateOutcome& outcome){
        const bool mine=in_flight_ && job.id==in_flight_id_;
        if(mine)in_flight_=false;
        if(!mine || job.revision!=revision_ || capturing_){
            ++counters_.discarded_stale;
            if(mine && activity_==LivePlacementActivity::estimating){activity_=LivePlacementActivity::idle;ask_.reset();}
            return AcceptKind::discarded_stale;
        }
        if(outcome.usable && Finite(outcome.pose) && Finite(outcome.surface)){
            Pending pending;
            pending.kind=PendingKind::estimate;
            pending.mapping=a0::m2::experimental::RegisteredMapping{outcome.pose,outcome.surface,std::nullopt};
            pending.beyond_envelope=outcome.beyond_envelope;pending.exceeded=outcome.exceeded;
            pending.source=ask_==LiveEstimateAsk::button?LivePlacementSource::auto_button:LivePlacementSource::auto_start;
            pending_=std::move(pending);
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

    // The mapping that C has to be drawn with: the position that waits to be drawn (an estimate's or a hand step's), else P.
    [[nodiscard]] const a0::m2::experimental::RegisteredMapping& Wanted() const {return pending_?pending_->mapping:placement_;}
    [[nodiscard]] bool HasPending() const {return pending_.has_value();}
    // Changes whenever Wanted() does (a position waits, a position that waited is dropped, a shot, a new condition), not when a pending
    // position becomes P.
    [[nodiscard]] std::uint64_t WantedSerial() const {return wanted_serial_;}
    // The wanted mapping was drawn: a pending position becomes P.
    void Committed(){
        if(!pending_)return;
        Pending pending=std::move(*pending_);
        pending_.reset();
        placement_=pending.mapping;
        hand_discarded_by_condition_=false;
        if(pending.kind==PendingKind::estimate){
            source_=pending.source;
            ClearHand();hand_returned_=false;
            beyond_envelope_=pending.beyond_envelope;exceeded_=pending.exceeded;
            outcome_=LivePlacementOutcomeKind::placed;outcome_ask_=ask_;failure_.clear();
            activity_=LivePlacementActivity::idle;ask_.reset();
            ++counters_.placed;
        }else{
            // A hand step: the position is by hand, or it is what it was before the hand steps (the sum came back to 0, or the result is
            // the condition's own position), and then no hand mark stays.
            beyond_envelope_=false;exceeded_.clear();
            if(pending.kind==PendingKind::hand_manual){
                source_=LivePlacementSource::manual;
                base_source_=pending.base_source;hand_base_=pending.hand_base;hand_total_=pending.total;
                base_beyond_=pending.base_beyond_envelope;base_exceeded_=pending.base_exceeded;
                hand_returned_=false;
            }else{
                source_=pending.source;
                ClearHand();
                hand_returned_=pending.returned_to_base;
                // Back to what it was before the hand steps: what was said of that position (an estimate applied beyond a range) is said again.
                if(pending.returned_to_base){beyond_envelope_=pending.base_beyond_envelope;exceeded_=pending.base_exceeded;}
            }
        }
        ++serial_;capture_.reset();
    }
    // The wanted mapping could not be drawn: a pending position is dropped and P stays (and the sum of a hand step that waited is P's again).
    void DrawFailed(std::string reason){
        if(!pending_)return;
        const bool estimate=pending_->kind==PendingKind::estimate;
        pending_.reset();
        ++wanted_serial_;
        if(estimate)Fail({std::move(reason)});
        else{++counters_.hand_draw_failed;++hand_serial_;}   // a press that was made against the hand position that waited is stale
    }

    // ---- Hand steps (#437 stage 2) ----
    // A press of a hand button: `delta` is the counts of its step (12 px is 3 counts, 4 px is 1, 0.2 degrees is 4, 0.05 is 1; the sign is the
    // direction). The sum of the counts entered since the position before the first press (hand_base) is solved once from that position by the
    // caller's check; a press never builds on the position that the press before it gave. The result is a position that waits to be drawn
    // (Wanted()), and P becomes it when it has been drawn (Committed), as for an estimate.
    //  - It is accepted only while Live is running, the screen was drawn with P (the caller says both), no pair capture holds P, and the
    //    check accepts: the result's pose is inside the pose envelope, its mapping passes the cover check, and it can be solved. A
    //    refused press changes nothing and rounds nothing.
    //  - A press that comes before the one before it was drawn is accepted and replaces the position that waits: the sum is the sum of both.
    //  - An accepted press withdraws the estimate that was asked for (not begun, running, or waiting to be drawn): its result is dropped,
    //    and nothing runs until the button is pressed again. This holds for an estimate whose result came while the press was checked:
    //    the press still wins, and the result that waits to be drawn is dropped (only a change of P or of the hand position that waits
    //    makes the press stale).
    //  - A sum that comes back to exactly 0 is not a step: P goes back to what it was before the first press, source included, and no hand
    //    mark stays, whatever that position was (an estimate that was outside the envelope included). A result that is the condition's own
    //    position (bit for bit) is also not by hand: the source is fixed-condition and no hand mark stays.
    // The three calls are one: HandStep. They are apart so that a caller that guards the machine with a lock does not hold it while the check
    // runs (the check scans the full-size output). CommitHandStep refuses (refused_stale) a proposal whose P or hand position that waits changed.
    [[nodiscard]] LiveHandPrepared PrepareHandStep(const a0::m2::experimental::HandCounts& delta,const LiveHandContext& context){
        LiveHandPrepared prepared;
        const auto refuse=[&](LiveHandOutcome outcome){++counters_.hand_refused;prepared.outcome=outcome;return prepared;};
        if(!configured_)return refuse(LiveHandOutcome::refused_not_configured);
        if(capturing_)return refuse(LiveHandOutcome::refused_capturing);
        if(!context.live_running)return refuse(LiveHandOutcome::refused_stopped);
        if(!context.drawn_with_placement)return refuse(LiveHandOutcome::refused_not_drawn);
        if(delta.IsZero())return refuse(LiveHandOutcome::refused_zero);
        // What the press builds on: the position that waits when it is a hand step, else P (or P's own base when P is by hand).
        a0::m2::experimental::HandCounts before;
        auto& proposal=prepared.proposal;
        if(pending_ && pending_->kind!=PendingKind::estimate){
            proposal.base=*pending_->hand_base;proposal.base_source=*pending_->base_source;before=pending_->total;
            proposal.base_beyond_envelope=pending_->base_beyond_envelope;proposal.base_exceeded=pending_->base_exceeded;
        }else if(source_==LivePlacementSource::manual){
            proposal.base=*hand_base_;proposal.base_source=*base_source_;before=hand_total_;
            proposal.base_beyond_envelope=base_beyond_;proposal.base_exceeded=base_exceeded_;
        }else{
            proposal.base=a0::m2::experimental::RegisteredMapping{placement_.pose,placement_.surface,std::nullopt};proposal.base_source=source_;
            proposal.base_beyond_envelope=beyond_envelope_;proposal.base_exceeded=exceeded_;
        }
        try{proposal.total=a0::m2::experimental::AddHandCounts(before,delta);}
        catch(const std::out_of_range&){return refuse(LiveHandOutcome::refused_range);}
        proposal.zero=proposal.total.IsZero();
        proposal.revision=revision_;proposal.hand_serial=hand_serial_;proposal.serial=serial_;
        prepared.outcome=LiveHandOutcome::accepted;
        return prepared;
    }
    LiveHandResult CommitHandStep(const LiveHandProposal& proposal,const LiveHandCheck& check){
        LiveHandResult result;result.total=proposal.total;
        const auto refuse=[&](LiveHandOutcome outcome){++counters_.hand_refused;result.outcome=outcome;return result;};
        if(!configured_)return refuse(LiveHandOutcome::refused_not_configured);
        if(capturing_)return refuse(LiveHandOutcome::refused_capturing);
        // Stale when what the press built on changed: P (serial), the hand position that waits (hand_serial), or anything that moved the revision
        // (another press, a capture, a new condition). An estimate's result that came meanwhile and waits to be drawn is not: it is withdrawn below.
        if(proposal.revision!=revision_ || proposal.hand_serial!=hand_serial_ || proposal.serial!=serial_)return refuse(LiveHandOutcome::refused_stale);
        a0::m2::experimental::RegisteredMapping mapping{proposal.base.pose,proposal.base.surface,std::nullopt};
        if(!proposal.zero){
            switch(check.status){
            case LiveHandCheck::Status::accepted:break;
            case LiveHandCheck::Status::refused_envelope:result.exceeded=check.exceeded;return refuse(LiveHandOutcome::refused_envelope);
            case LiveHandCheck::Status::refused_cover:result.seam_covered=check.seam_covered;result.output_covered=check.output_covered;return refuse(LiveHandOutcome::refused_cover);
            case LiveHandCheck::Status::refused_unsolvable:return refuse(LiveHandOutcome::refused_unsolvable);
            case LiveHandCheck::Status::error:result.detail=check.detail;return refuse(LiveHandOutcome::refused_error);
            }
            if(!Finite(check.mapping.pose) || !Finite(check.mapping.surface))return refuse(LiveHandOutcome::refused_unsolvable);
            mapping=a0::m2::experimental::RegisteredMapping{check.mapping.pose,check.mapping.surface,std::nullopt};
        }
        // Accepted from here.
        result.outcome=LiveHandOutcome::accepted;result.mapping=mapping;
        result.withdrew_estimate=in_flight_ || activity_!=LivePlacementActivity::idle || (pending_ && pending_->kind==PendingKind::estimate);
        if(result.withdrew_estimate)++counters_.hand_withdrawn_estimates;
        ++counters_.hand_accepted;
        ++revision_;++wanted_serial_;++hand_serial_;       // a running estimate's result is stale; a position that waited is replaced
        activity_=LivePlacementActivity::idle;ask_.reset();
        outcome_=LivePlacementOutcomeKind::none;outcome_ask_.reset();failure_.clear();
        const bool from_hand=source_==LivePlacementSource::manual;
        Pending pending;
        pending.mapping=mapping;pending.hand_base=proposal.base;pending.base_source=proposal.base_source;pending.total=proposal.total;
        pending.base_beyond_envelope=proposal.base_beyond_envelope;pending.base_exceeded=proposal.base_exceeded;
        if(proposal.zero){
            result.returned_to_base=true;
            if(!from_hand){pending_.reset();return result;}   // P is what it was before the hand steps already: nothing waits
            pending.kind=PendingKind::hand_restore;pending.source=proposal.base_source;pending.returned_to_base=true;
        }else if(a0::m2::experimental::IsConditionPosition(mapping,base_)){   // the one test that the capture's entrance uses too
            result.became_condition_position=true;
            pending.kind=PendingKind::hand_restore;pending.source=LivePlacementSource::fixed_condition;
        }else{
            pending.kind=PendingKind::hand_manual;pending.source=LivePlacementSource::manual;
        }
        pending_=std::move(pending);
        return result;
    }
    // The three calls in one, for a caller that has no lock to spare.
    LiveHandResult HandStep(const a0::m2::experimental::HandCounts& delta,const LiveHandContext& context,const LiveHandChecker& checker){
        const auto prepared=PrepareHandStep(delta,context);
        if(prepared.outcome!=LiveHandOutcome::accepted){
            LiveHandResult refused;refused.outcome=prepared.outcome;refused.total=prepared.proposal.total;
            return refused;
        }
        return CommitHandStep(prepared.proposal,EvaluateLiveHandCheck(checker,prepared.proposal));
    }

    // ---- A pair capture (#437 stage 2) ----
    // The request of a pair capture: P is frozen until the capture's result is in (ApplyCapture, or EndCapture when there is none). What the
    // capture is given is P as it is drawn: Placement() and Serial(), never Wanted(). A position that waited to be drawn is dropped (and
    // WantedSerial() moves, so that a bake that is in progress for it is dropped too), the result of an estimate that is running is
    // dropped, and the estimate that was asked for stays asked for: it is gone when the JPEG's result is applied, and it runs once when Live
    // starts again if there is no JPEG. While frozen, a hand press and the button are refused, and no job begins.
    // `drawn_with_placement`: the picture on the screen was drawn with a table baked for P (the caller compares); without it the capture is
    // not admitted and nothing changes. A capture of one camera is not a pair capture and does not call this.
    LiveCaptureBegin BeginCapture(bool drawn_with_placement){
        LiveCaptureBegin begin;
        if(!configured_){begin.admission=LiveCaptureAdmission::not_configured;return begin;}
        if(capturing_){begin.admission=LiveCaptureAdmission::already_capturing;return begin;}
        if(!drawn_with_placement){begin.admission=LiveCaptureAdmission::not_drawn;return begin;}
        begin.admission=LiveCaptureAdmission::accepted;
        begin.dropped_pending=pending_.has_value();
        begin.dropped_estimate=in_flight_ || activity_==LivePlacementActivity::estimating;
        if(pending_){pending_.reset();++wanted_serial_;}
        ++revision_;
        if(activity_==LivePlacementActivity::estimating)activity_=LivePlacementActivity::requested;
        capturing_=true;++counters_.captures_begun;
        LivePlacementGrant grant;
        grant.mapping=placement_;grant.serial=serial_;grant.source=source_;grant.condition_sha256=sha_;
        if(source_==LivePlacementSource::manual){grant.base_source=base_source_;grant.hand_entered=hand_total_;}
        begin.grant=std::move(grant);
        return begin;
    }
    // The capture ended with no JPEG that applies: P is free again.
    void EndCapture(){capturing_=false;}
    [[nodiscard]] bool Capturing() const {return capturing_;}

    // A capture ended with a JPEG made at geometry level `level` with `mapping` (the mapping of that level; layer 2, when it has one, is left out
    // and noted). The capture is over, whatever the answer. A request that waited, a job that ran and a position that waited are dropped.
    //  - level "confirmed" (the JPEG was made with P): P stays, source and serial too; `mapping` must be P. The note says so.
    //  - P is by hand: it stays by hand, at any level the capture can have (confirmed, or G4 when the hand-set position failed the cover
    //    check and the JPEG was made with the condition's own position). Any other level is refused.
    //  - otherwise P becomes `mapping` (layer 2 left out), source after-capture. The serial moves only when P's nine numbers or its source change.
    // `condition_sha256` is the sha of the condition the JPEG was made with: another one than the session's is refused (false) and
    // changes nothing, because the mapping is B's position against another condition's base pose. `given_serial`, when the capture
    // recorded the position it was given, must be P's serial. `layer_two_max_px`: the largest shift of that layer 2
    // (LivePlacementLayerTwoMaxPx); kept in the note for the sentence and the log.
    bool ApplyCapture(const a0::m2::experimental::RegisteredMapping& mapping,const std::string& level,const std::string& condition_sha256,
        std::optional<double> layer_two_max_px=std::nullopt,std::optional<std::uint64_t> given_serial=std::nullopt){
        capturing_=false;
        if(!configured_ || condition_sha256!=sha_ || !Finite(mapping.pose) || !Finite(mapping.surface))return false;
        if(given_serial && *given_serial!=serial_)return false;
        const bool confirmed=level==kLiveCaptureConfirmedLevel;
        const bool by_hand=source_==LivePlacementSource::manual;
        if(confirmed){if(!LivePlacementSameMapping(mapping,placement_))return false;}
        else if(by_hand && level!="G4")return false;
        const bool stays=confirmed || by_hand;
        LivePlacementCaptureNote note;
        note.level=level;note.before=placement_;note.before_source=source_;
        note.layer_two_dropped=LivePlacementHasLayerTwo(mapping);
        if(note.layer_two_dropped && layer_two_max_px && std::isfinite(*layer_two_max_px) && *layer_two_max_px>0)note.layer_two_max_px=layer_two_max_px;
        pending_.reset();in_flight_=false;
        ++revision_;++wanted_serial_;
        activity_=LivePlacementActivity::idle;ask_.reset();
        if(stays){
            note.difference_px=0.0;   // P did not move
        }else{
            const a0::m2::experimental::RegisteredMapping next{mapping.pose,mapping.surface,std::nullopt};
            const bool changed=!LivePlacementSameMapping(placement_,next) || source_!=LivePlacementSource::after_capture;
            placement_=next;source_=LivePlacementSource::after_capture;
            ClearHand();hand_returned_=false;hand_discarded_by_condition_=false;
            beyond_envelope_=false;exceeded_.clear();
            if(changed)++serial_;
            outcome_=LivePlacementOutcomeKind::none;outcome_ask_.reset();failure_.clear();
        }
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
        view.base_source=base_source_;view.hand_base=hand_base_;
        if(source_==LivePlacementSource::manual)view.hand_entered=hand_total_;
        if(pending_ && pending_->kind!=PendingKind::estimate)view.hand_wanted=pending_->total;
        view.hand_returned=hand_returned_;view.hand_discarded_by_condition=hand_discarded_by_condition_;view.capturing=capturing_;
        return view;
    }

private:
    using Pose=a0::m2::experimental::content_registration::PoseParameters;
    using Surface=a0::m2::experimental::SurfacePlane;
    // estimate: an estimate's position. hand_manual: a hand step's position. hand_restore: a hand step whose position is not by hand (the
    // sum came back to 0, or the result is the condition's own position): P becomes it with the source `source` and no hand mark.
    enum class PendingKind {estimate,hand_manual,hand_restore};
    struct Pending {
        PendingKind kind{PendingKind::estimate};
        a0::m2::experimental::RegisteredMapping mapping;
        LivePlacementSource source{LivePlacementSource::auto_start};
        bool beyond_envelope{false};
        std::vector<std::string> exceeded;
        // hand steps: the position before the first press, where it came from, and the sum that `mapping` was solved for
        std::optional<a0::m2::experimental::RegisteredMapping> hand_base;
        std::optional<LivePlacementSource> base_source;
        a0::m2::experimental::HandCounts total;
        bool returned_to_base{false};
        bool base_beyond_envelope{false};
        std::vector<std::string> base_exceeded;
    };
    static bool Finite(const Pose& p){
        for(const double v:{p.rx_deg,p.ry_deg,p.rz_deg,p.tx_mm,p.ty_mm,p.tz_percent})if(!std::isfinite(v))return false;
        return true;
    }
    static bool Finite(const Surface& s){return std::isfinite(s.h0_mm) && std::isfinite(s.slope_x) && std::isfinite(s.slope_y);}
    static void RequireFinite(const Pose& p){if(!Finite(p))throw std::invalid_argument("live placement: the base pose must be finite");}
    void ClearHand(){base_source_.reset();hand_base_.reset();hand_total_={};base_beyond_=false;base_exceeded_.clear();}
    void ResetPlacement(){
        placement_=a0::m2::experimental::RegisteredMapping{base_,{},std::nullopt};
        source_=LivePlacementSource::fixed_condition;
        ClearHand();hand_returned_=false;hand_discarded_by_condition_=false;
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

    bool configured_{false},live_enabled_{false},capturing_{false};
    std::string sha_;
    Pose base_;
    a0::m2::experimental::RegisteredMapping placement_;
    LivePlacementSource source_{LivePlacementSource::fixed_condition};
    // P is by hand: the position before the first press, its source, and the sum of the counts entered since. All empty otherwise.
    std::optional<LivePlacementSource> base_source_;
    std::optional<a0::m2::experimental::RegisteredMapping> hand_base_;
    a0::m2::experimental::HandCounts hand_total_;
    bool base_beyond_{false};                // what was said of hand_base: an estimate that was applied beyond a range
    std::vector<std::string> base_exceeded_;
    bool hand_returned_{false},hand_discarded_by_condition_{false};
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
    // Moves when a hand position starts to wait, is replaced or is dropped without being drawn (a press, a bake that failed). The other ways a
    // hand position that waits goes (a capture, a capture's result, a new condition) move revision_, and its drawing moves serial_.
    std::uint64_t hand_serial_{0};
    bool in_flight_{false};
    LivePlacementCounters counters_;
};
}
