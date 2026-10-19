#pragma once
#include <barrier>
#include <algorithm>
#include <chrono>
#include <stdexcept>
namespace a0::phase0::experimental {
// A healthy peer may be crossing its own body timeout just after the first
// body reports NotLiveView. Do not apply this observation to user stops,
// unrelated failures, unstarted/ended cameras or retired data channels.
inline bool AwaitPreviewPeerEnd(bool peer_ended,bool started,bool ended,bool failed,bool retired){
    return peer_ended && started && !ended && !failed && !retired;
}
template<class ReadStatus,class Now,class Pause>
bool ObservePreviewPeerEnd(std::chrono::steady_clock::time_point control_deadline,
    ReadStatus read_status,Now now,Pause pause){
    using Clock=std::chrono::steady_clock;
    const auto grace=(std::min)(control_deadline,now()+std::chrono::seconds(2));
    while(now()<grace){
        // Keep any in-flight scalar transfer inside the ORIGINAL Stop deadline.
        // The grace bounds scheduling new reads; it never cancels a healthy
        // data phase at a shorter artificial deadline.
        const auto live=read_status(control_deadline);
        if(live>1)throw std::runtime_error("unknown peer Live View state");
        if(now()>=grace)return false;
        if(live==0)return true;
        const auto remaining=grace-now();
        if(remaining>Clock::duration::zero())pause((std::min)(remaining,Clock::duration(std::chrono::milliseconds(50))));
    }
    return false;
}
// Both workers must participate, including startup/COM failures. Never hold an
// I/O or camera mutex at either barrier. No Close while the other Stop is pending.
class PreviewShutdownStages final {
public:
    void TransfersDrained(){drained_.arrive_and_wait();}
    void StopsAttempted(){stopped_.arrive_and_wait();}
    // Called only if the second OS thread cannot be created at all.
    void MissingWorker(){drained_.arrive_and_drop();stopped_.arrive_and_drop();}
private:
    std::barrier<> drained_{2};
    std::barrier<> stopped_{2};
};
}
