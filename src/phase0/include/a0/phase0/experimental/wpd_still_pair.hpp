#pragma once
#include "a0/phase0/experimental/still_pair.hpp"
#include "a0/phase0/experimental/wpd_still_capture.hpp"

namespace a0::phase0::experimental {
// Adapters between one WpdStillCaptureRun and the transaction steps in
// still_pair.hpp (#389). They never throw: a refusal becomes data so the pair
// transaction decides what to close and what to retain.

// Recheck the capture state of a body that is about to be exposed. OutOfFocus is
// typed so the UI can tell it from a generic refusal.
inline StillPairPrepared PrepareWpdStillRun(WpdStillCaptureRun& run){
    StillPairPrepared prepared;
    try{run.Preflight();prepared.ready=true;}
    catch(const WpdStillOutOfFocus& error){prepared.error=error.what();prepared.out_of_focus=true;}
    catch(const std::exception& error){prepared.error=error.what();}
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
