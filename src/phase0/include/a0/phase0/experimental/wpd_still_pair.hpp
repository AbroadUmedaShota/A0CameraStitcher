#pragma once
#include "a0/phase0/experimental/still_pair.hpp"
#include "a0/phase0/experimental/wpd_still_capture.hpp"

namespace a0::phase0::experimental {
// Adapters between one WpdStillCaptureRun and the transaction steps in
// still_pair.hpp (#389). They never throw: a refusal becomes data so the pair
// transaction decides what to close and what to retain.

// Why a read-only preflight stopped, from the exception it threw (#430). Only the typed
// refusals count as the camera's own state; any other exception (a read that failed, a
// deadline, an alias mismatch the caller decides separately) is a transport-level stop.
struct WpdStillRefusal {StillPreflightKind kind{StillPreflightKind::Transport};std::optional<std::uint16_t> event_code;};
inline WpdStillRefusal ClassifyWpdStillRefusal(const std::exception& error){
    WpdStillRefusal refusal;
    if(const auto* event=dynamic_cast<const WpdStillEventRejected*>(&error)){refusal.kind=StillPreflightKind::Event;refusal.event_code=event->Code();}
    else if(dynamic_cast<const WpdStillOutOfFocus*>(&error))refusal.kind=StillPreflightKind::OutOfFocus;
    else if(const auto* state=dynamic_cast<const WpdStillStateRefused*>(&error)){
        switch(state->Reason()){
        case WpdStillStateRefusal::LiveViewOn:refusal.kind=StillPreflightKind::LiveViewOn;break;
        case WpdStillStateRefusal::Quality:refusal.kind=StillPreflightKind::Quality;break;
        case WpdStillStateRefusal::Size:refusal.kind=StillPreflightKind::Size;break;
        case WpdStillStateRefusal::NotSettled:refusal.kind=StillPreflightKind::NotSettled;break;
        case WpdStillStateRefusal::NotReady:refusal.kind=StillPreflightKind::NotReady;break;
        }
    }
    return refusal;
}
// The log line for one preflight read (#430): the number of 0x90C7 reads, the code and count of
// every notice they returned, whether the capture profile was refreshed, and how it ended.
// `stage` is "check" for the read-only preflight and "recheck" for the one before the exposure.
inline std::string DescribeWpdStillPreflightEvents(const char* stage,const WpdStillPreflightRecord& record,const WpdStillRefusal* refusal){
    return std::string("preflight_events stage=")+stage+" polls="+std::to_string(record.polls)+" codes="+record.codes.Describe()+
        " refresh="+(record.refresh?"1":"0")+" result="+(refusal?"refused":"ready")+
        " refused_code="+(refusal && refusal->event_code?WpdStillEventCodeText(*refusal->event_code):std::string("none"))+
        " kind="+(refusal?StillPreflightKindText(refusal->kind):"none");
}
// Recheck the capture state of a body that is about to be exposed. OutOfFocus is
// typed so the UI can tell it from a generic refusal.
inline StillPairPrepared PrepareWpdStillRun(WpdStillCaptureRun& run,WpdStillPreflightRecord* record=nullptr,WpdStillRefusal* refusal=nullptr){
    StillPairPrepared prepared;
    try{run.Preflight(record);prepared.ready=true;}
    catch(const WpdStillOutOfFocus& error){prepared.error=error.what();prepared.out_of_focus=true;if(refusal)*refusal=ClassifyWpdStillRefusal(error);}
    catch(const std::exception& error){prepared.error=error.what();if(refusal)*refusal=ClassifyWpdStillRefusal(error);}
    catch(...){prepared.error="unknown exposure preflight failure";}
    return prepared;
}
// Send the one shutter. `Uncertain` is reported only when the command reached
// the transport (CaptureAttempted); an admission refusal is NotSent.
inline StillShutterOutcome SendWpdStillShutter(WpdStillCaptureRun& run){
    StillShutterOutcome outcome;
    try{run.Shutter();outcome.state=StillShutterState::Sent;}
    catch(const std::exception& error){outcome.error=error.what();}
    catch(...){outcome.error="unknown shutter failure";}
    if(outcome.state!=StillShutterState::Sent && run.ShutterAttempted())outcome.state=StillShutterState::Uncertain;
    if(const auto at=run.ShutterDispatchedAt())outcome.dispatched_at=*at;
    return outcome;
}
}
