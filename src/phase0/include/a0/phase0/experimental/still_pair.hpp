#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace a0::phase0::experimental {
struct VerifiedStillOriginal {
    bool success{},closed{};
    std::filesystem::path path;
    std::uintmax_t bytes{};
    std::string sha256,error;
    bool out_of_focus{};
};
struct VerifiedStillPair {
    std::array<VerifiedStillOriginal,2> originals;bool complete{};std::string error;bool out_of_focus{},preflight_complete{};
    // Whether each body's shutter command was handed to the transport. Both false
    // means nothing was exposed.
    std::array<bool,2> shutter_dispatched{};
    // Dispatch of B minus dispatch of A, whole milliseconds. Set only when both
    // shutters were dispatched (#389).
    std::optional<long long> shutter_gap_ms;
};
struct VerifiedStillPreflight {bool ready{},closed{};std::string error;bool out_of_focus{};};
// A body opened for exposure whose capture state was rechecked. `ready` false
// means no shutter may be sent to either body.
struct StillPairPrepared {bool ready{};std::string error;bool out_of_focus{};};
// NotSent: the command never reached the transport. Sent: acknowledged. Uncertain:
// dispatched but the acknowledgement failed, so an exposure may exist.
enum class StillShutterState {NotSent,Sent,Uncertain};
struct StillShutterOutcome {
    StillShutterState state{StillShutterState::NotSent};
    std::chrono::steady_clock::time_point dispatched_at{};
    std::string error;bool out_of_focus{};
};
// The steps of one dual-body capture transaction (#389). Index 0 is CAM-A and 1
// is CAM-B, bound by the caller to registered aliases.
struct StillPairSteps {
    // Open the body, read-only preflight, Close (#345). No exposure.
    std::function<VerifiedStillPreflight(int,std::chrono::steady_clock::time_point)> preflight;
    // Open the body again for exposure and recheck its capture state. The session
    // stays open until `close`. Must tolerate being closed after a partial open.
    std::function<StillPairPrepared(int,std::chrono::steady_clock::time_point)> prepare;
    // Send the one shutter. Must not wait for the image.
    std::function<StillShutterOutcome(int)> shutter;
    // Import and persist the image of a body whose shutter was acknowledged.
    std::function<VerifiedStillOriginal(int)> collect;
    // After an uncertain shutter: retain an attributable candidate as a private
    // partial only. Never promote it to an original and never resend.
    std::function<void(int)> rescue;
    // Close the session opened by `prepare` and fold the close and deadline
    // result into the original. Called once for every body that was prepared.
    std::function<void(int,VerifiedStillOriginal&)> close;
    std::function<void(const std::string&)> log;
};
// One transaction under one deadline:
//   preflight+Close A, preflight+Close B          (no exposure; a refusal stops both)
//   open A, open B and recheck each               (still no exposure)
//   shutter A, shutter B                          (nothing slow between them)
//   import+save A, import+save B, close both
// B is exposed only after A's shutter was acknowledged. Once B has been exposed
// its image is imported even if A's import failed: that is one attempt at an
// existing exposure, not a retry, and the pair still fails. No shutter is ever
// resent, no old or other image is substituted, and nothing starts after the
// deadline. A stop request prevents a shutter that has not been dispatched; the
// import of a dispatched one is the lane's own admission and continues.
inline VerifiedStillPair CaptureStillPairShutterFirstOnce(const StillPairSteps& steps,const std::function<bool()>& admission,
    std::chrono::steady_clock::time_point deadline){
    using Clock=std::chrono::steady_clock;
    VerifiedStillPair result;
    const auto name=[](int i){return i==0?"CAM-A":"CAM-B";};
    const auto may_start=[&]{return admission() && Clock::now()<deadline;};
    const auto log=[&](const std::string& line){if(steps.log)steps.log(line);};
    for(int i=0;i<2;++i){
        if(!may_start()){result.error="pair preflight deadline/admission";return result;}
        VerifiedStillPreflight status;
        try{status=steps.preflight(i,deadline);}catch(const std::exception& error){result.error=error.what();return result;}
        if(!status.ready || !status.closed){
            result.out_of_focus=status.out_of_focus;
            result.error=std::string(name(i))+" preflight: "+(status.error.empty()?"readiness/close unconfirmed":status.error);
            return result;
        }
    }
    result.preflight_complete=true;
    std::array<bool,2> opened{};
    const auto close_all=[&]{
        for(int i=0;i<2;++i){
            if(!opened[i])continue;
            auto& original=result.originals[i];
            try{steps.close(i,original);}
            catch(const std::exception& error){
                original.success=false;original.closed=false;
                original.error+=std::string(original.error.empty()?"":"; ")+"session close failed: "+error.what();
            }
        }
    };
    for(int i=0;i<2;++i){
        if(!may_start()){result.error="pair shutter deadline/admission";close_all();return result;}
        opened[i]=true;
        StillPairPrepared prepared;
        try{prepared=steps.prepare(i,deadline);}catch(const std::exception& error){prepared.ready=false;prepared.error=error.what();}
        if(!prepared.ready){
            result.out_of_focus=prepared.out_of_focus;
            result.error=std::string(name(i))+" exposure preflight: "+(prepared.error.empty()?"not ready":prepared.error);
            close_all();return result;
        }
    }
    if(!may_start()){result.error="pair shutter deadline/admission";close_all();return result;}
    const auto send=[&](int i){
        StillShutterOutcome outcome;
        try{outcome=steps.shutter(i);}
        catch(const std::exception& error){outcome.state=StillShutterState::Uncertain;outcome.error=error.what();}
        result.shutter_dispatched[i]=outcome.state!=StillShutterState::NotSent;
        return outcome;
    };
    std::array<StillShutterOutcome,2> shutters;
    shutters[0]=send(0);
    if(shutters[0].state==StillShutterState::Sent){
        if(may_start())shutters[1]=send(1);
        else shutters[1].error="CAM-B shutter not sent: pair deadline/admission";
    }else shutters[1].error="CAM-B shutter not sent: CAM-A shutter did not complete";
    // A callback that threw has no dispatch time: the gap is then unknown, not zero.
    const bool timed=shutters[0].dispatched_at!=Clock::time_point{} && shutters[1].dispatched_at!=Clock::time_point{};
    if(result.shutter_dispatched[0] && result.shutter_dispatched[1] && timed){
        result.shutter_gap_ms=std::chrono::duration_cast<std::chrono::milliseconds>(shutters[1].dispatched_at-shutters[0].dispatched_at).count();
        log("pair_shutters shutter_gap_ms="+std::to_string(*result.shutter_gap_ms));
    }else log(std::string("pair_shutters shutter_gap_ms=none a_dispatched=")+(result.shutter_dispatched[0]?"1":"0")+" b_dispatched="+(result.shutter_dispatched[1]?"1":"0"));
    for(int i=0;i<2;++i){
        auto& original=result.originals[i];
        if(shutters[i].state==StillShutterState::Sent){
            try{original=steps.collect(i);}
            catch(const std::exception& error){original=VerifiedStillOriginal{};original.error=error.what();}
        }else{
            if(shutters[i].state==StillShutterState::Uncertain){try{steps.rescue(i);}catch(...){}}
            original.error=shutters[i].error.empty()?std::string(name(i))+" shutter not completed":shutters[i].error;
        }
        original.out_of_focus=original.out_of_focus || shutters[i].out_of_focus;
    }
    close_all();
    for(int i=0;i<2;++i){
        const auto& original=result.originals[i];
        if(!original.success || !original.closed || original.path.empty() || !original.bytes || original.sha256.size()!=64 ||
            original.sha256.find_first_not_of("0123456789abcdef")!=std::string::npos){
            result.out_of_focus=original.out_of_focus;
            result.error=original.error.empty()?"pair original not fully verified and closed":original.error;return result;
        }
    }
    if(!admission() || Clock::now()>=deadline){result.error="pair completed after deadline/admission";return result;}
    result.complete=true;return result;
}
}
