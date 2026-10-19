#include "a0/phase0/experimental/still_pair.hpp"
#include "a0/phase0/experimental/pair_cycle_probe.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace a0::phase0::experimental;
int main(){
    int checks=0,failures=0;auto check=[&](bool value){++checks;if(!value)++failures;};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    auto original=[](int i){return VerifiedStillOriginal{true,true,i==0?"CAM-A/original.jpg":"CAM-B/original.jpg",100,std::string(64,i==0?'a':'b'),{}};};
    std::vector<int> calls;
    auto result=CaptureStillPairOnce([&](int i,auto d){calls.push_back(i);check(d==deadline);return original(i);},[]{return true;},deadline);
    check(result.complete && calls==std::vector<int>{0,1});check(result.originals[0].sha256!=result.originals[1].sha256);
    for(int failed_camera:{0,1})for(int failure:{0,1,2,3,4}){
        calls.clear();auto r=CaptureStillPairOnce([&](int i,auto){calls.push_back(i);auto o=original(i);if(i==failed_camera){
            if(failure==0)o.success=false;if(failure==1)o.closed=false;if(failure==2)o.sha256="";if(failure==3)o.bytes=0;if(failure==4)throw std::runtime_error("capture failed");}return o;
        },[]{return true;},deadline);
        check(!r.complete && !r.error.empty() && calls.size()==static_cast<size_t>(failed_camera+1));
        if(failed_camera==1)check(r.originals[0].path==original(0).path && r.originals[0].sha256==original(0).sha256);
    }
    calls.clear();auto expired=CaptureStillPairOnce([&](int i,auto){calls.push_back(i);return original(i);},[]{return true;},std::chrono::steady_clock::now()-std::chrono::seconds(1));check(!expired.complete && calls.empty());
    auto refused=CaptureStillPairOnce([&](int i,auto){calls.push_back(i);return original(i);},[]{return false;},deadline);check(!refused.complete && calls.empty());
    calls.clear();auto focus_failed=CaptureStillPairOnce([&](int i,auto){calls.push_back(i);auto o=original(i);o.success=false;o.out_of_focus=true;o.error="OutOfFocus";return o;},[]{return true;},deadline);
    check(!focus_failed.complete && focus_failed.out_of_focus && focus_failed.error=="OutOfFocus" && calls==std::vector<int>{0});
    bool admission=true;calls.clear();auto revoked=CaptureStillPairOnce([&](int i,auto){calls.push_back(i);admission=false;return original(i);},[&]{return admission;},deadline);check(!revoked.complete && calls==std::vector<int>{0} && revoked.originals[0].success);
    calls.clear();auto preflight=[](int,auto){return VerifiedStillPreflight{true,true,{}};};
    auto prepared=CapturePreflightedStillPairOnce([&](int i,auto d){check(d==deadline);calls.push_back(i-2);return preflight(i,d);},
        [&](int i,auto d){check(d==deadline);calls.push_back(i);return original(i);},[]{return true;},deadline);
    check(prepared.complete && prepared.preflight_complete && calls==std::vector<int>{-2,-1,0,1});
    for(int refused_camera:{0,1})for(int failure:{0,1,2,3}){
        calls.clear();auto outcome=CapturePreflightedStillPairOnce([&](int i,auto){calls.push_back(i-2);auto status=VerifiedStillPreflight{true,true,{}};
            if(i==refused_camera){if(failure==0)status.ready=false;if(failure==1)status.closed=false;
                if(failure==2)throw std::runtime_error("preflight refused");if(failure==3){status.ready=false;status.out_of_focus=true;status.error="OutOfFocus";}}
            return status;
        },[&](int i,auto){calls.push_back(i);return original(i);},[]{return true;},deadline);
        check(!outcome.complete && !outcome.preflight_complete && !outcome.error.empty() && calls== (refused_camera==0?std::vector<int>{-2}:std::vector<int>{-2,-1}));
        check(outcome.originals[0].path.empty() && outcome.originals[1].path.empty());
        if(failure==3)check(outcome.out_of_focus);
    }
    calls.clear();auto pre_expired=CapturePreflightedStillPairOnce([&](int i,auto){calls.push_back(i-2);return VerifiedStillPreflight{true,true,{}};},
        [&](int i,auto){calls.push_back(i);return original(i);},[]{return true;},std::chrono::steady_clock::now()-std::chrono::seconds(1));
    check(!pre_expired.complete && calls.empty());
    for(int revoked_after:{0,1}){bool active=true;calls.clear();auto r=CapturePreflightedStillPairOnce([&](int i,auto){calls.push_back(i-2);if(i==revoked_after)active=false;return VerifiedStillPreflight{true,true,{}};},
        [&](int i,auto){calls.push_back(i);return original(i);},[&]{return active;},deadline);
        check(!r.complete && calls==(revoked_after==0?std::vector<int>{-2}:std::vector<int>{-2,-1}));}
    PairCycleProbe probe;
    PairCycleObservation view{false,true,true,true,1,0,0,0,5};
    check(probe.Next(view)==PairCycleAction::Capture);
    check(probe.Next(view)==PairCycleAction::Wait); // Never send the same shutter twice.
    view.idle=false;check(probe.Next(view)==PairCycleAction::Wait);
    view.idle=true;view.sessions=2;view.reviews=1;view.saved=2;view.review_paints=5;
    view.live_ready=false;check(probe.Next(view)==PairCycleAction::Wait);
    view.live_ready=true;check(probe.Next(view)==PairCycleAction::ShowLive);
    check(probe.Next(view)==PairCycleAction::Wait); // Old C paints cannot prove resumed C.
    view.merge_paints=10;view.merge_ready=false;check(probe.Next(view)==PairCycleAction::Wait);
    view.merge_ready=true;check(probe.Next(view)==PairCycleAction::Wait);
    check(probe.Completed()==1);
    check(probe.Next(view)==PairCycleAction::Capture);
    check(probe.Next(view)==PairCycleAction::Wait);
    view.sessions=3;view.reviews=2;view.saved=4;
    check(probe.Next(view)==PairCycleAction::Wait); // Previous static review cannot count.
    view.review_paints=10;check(probe.Next(view)==PairCycleAction::ShowLive);
    check(probe.Next(view)==PairCycleAction::Wait);
    view.merge_paints=15;check(probe.Next(view)==PairCycleAction::Complete);
    check(probe.Completed()==2 && probe.Next(view)==PairCycleAction::Complete);
    for(int stage:{0,1,2}){
        PairCycleProbe failed;PairCycleObservation state{false,true,true,true,1,0,0,0,5};
        if(stage>0)check(failed.Next(state)==PairCycleAction::Capture);
        if(stage>1){state.sessions=2;state.reviews=1;state.saved=2;state.review_paints=5;check(failed.Next(state)==PairCycleAction::ShowLive);}
        state.failed=true;check(failed.Next(state)==PairCycleAction::Fail);
        state.failed=false;check(failed.Next(state)==PairCycleAction::Fail); // Failure is terminal.
    }
    PairCycleProbe unexpected;view.sessions=1;view.reviews=1;view.saved=2;
    check(unexpected.Next(view)==PairCycleAction::Fail);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
