#include "a0/phase0/experimental/preview_shutdown.hpp"
#include <atomic>
#include <latch>
#include <thread>
#include <iostream>
#include <chrono>
#include <stdexcept>
using a0::phase0::experimental::PreviewShutdownStages;
using namespace a0::phase0::experimental;
int main(){
    int checks=0,failures=0;auto check=[&](bool value){++checks;if(!value)++failures;};
    for(bool startup_failed:{false,true}){
        PreviewShutdownStages stages;std::atomic<int> stopped{},closed{};
        std::latch a_draining{1},allow_b_drain{1},a_stopped{1},allow_b_stop{1};
        std::jthread a([&]{
            a_draining.count_down();stages.TransfersDrained();
            ++stopped;a_stopped.count_down();stages.StopsAttempted();++closed;
        });
        std::jthread b([&]{
            allow_b_drain.wait(); // B still has a transfer or is finishing failed startup.
            stages.TransfersDrained();allow_b_stop.wait();
            // A failed worker has no device to stop, but must still reach this stage.
            if(!startup_failed)++stopped;
            stages.StopsAttempted();++closed;
        });
        a_draining.wait();check(stopped==0 && closed==0);
        allow_b_drain.count_down();a_stopped.wait();check(stopped==1 && closed==0);
        allow_b_stop.count_down();a.join();b.join();
        check(stopped==(startup_failed?1:2));check(closed==2);
    }
    {PreviewShutdownStages stages;stages.MissingWorker();stages.TransfersDrained();stages.StopsAttempted();check(true);}
    check(AwaitPreviewPeerEnd(true,true,false,false,false));
    check(!AwaitPreviewPeerEnd(false,true,false,false,false)); // User stop / unrelated failure.
    check(!AwaitPreviewPeerEnd(true,false,false,false,false));
    check(!AwaitPreviewPeerEnd(true,true,true,false,false)); // Already ended itself.
    check(!AwaitPreviewPeerEnd(true,true,false,true,false));
    check(!AwaitPreviewPeerEnd(true,true,false,false,true));
    using Clock=std::chrono::steady_clock;using namespace std::chrono_literals;
    {
        Clock::time_point now{};const auto began=now;unsigned reads{},pauses{};
        const bool off=ObservePreviewPeerEnd(began+10s,[&](auto deadline){
            check(deadline==began+10s);return ++reads==3?0u:1u;
        },[&]{return now;},[&](auto delay){++pauses;now+=delay;});
        check(off && reads==3 && pauses==2 && now>began && now<began+2s);
    }
    {
        Clock::time_point now{};const auto deadline=now+350ms;unsigned reads{};
        const bool off=ObservePreviewPeerEnd(deadline,[&](auto limit){check(limit==deadline);++reads;return 1u;},
            [&]{return now;},[&](auto delay){check(delay>Clock::duration::zero() && now+delay<=deadline);now+=delay;});
        check(!off && reads>0 && now==deadline); // Never extend the original Stop deadline.
    }
    {
        Clock::time_point now{};unsigned reads{};
        check(!ObservePreviewPeerEnd(now,[&](auto){++reads;return 0u;},[&]{return now;},[&](auto){check(false);}));
        check(reads==0);
        check(!ObservePreviewPeerEnd(now+10s,[&](auto){now+=3s;return 0u;},[&]{return now;},[&](auto){check(false);}));
    }
    {
        Clock::time_point now{};bool rejected=false;
        try{ObservePreviewPeerEnd(now+10s,[](auto){return 2u;},[&]{return now;},[&](auto){check(false);});}
        catch(const std::runtime_error&){rejected=true;}check(rejected);
        rejected=false;
        try{ObservePreviewPeerEnd(now+10s,[](auto)->unsigned{throw std::runtime_error("read failed");},[&]{return now;},[&](auto){check(false);});}
        catch(const std::runtime_error&){rejected=true;}check(rejected);
    }
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
