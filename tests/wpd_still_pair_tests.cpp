// Pair transaction contracts (#389, #381): both shutters precede either import,
// and the original-handling rules match the one-body-at-a-time flow. Synthetic
// WPD channels only; no camera is touched.
#include "wpd_still_fake.hpp"
#include "a0/phase0/experimental/wpd_still_pair.hpp"
#include <array>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <thread>
using namespace a0::phase0::experimental;
namespace {
using Clock=std::chrono::steady_clock;
int checks{},failures{};
void Check(bool value){++checks;if(!value)++failures;}
bool Contains(const std::string& text,std::string_view part){return text.find(part)!=std::string::npos;}
// Test-sized completion wait. Production uses kWpdStillCompletionWaitMax.
const WpdStillCompletionWait kWait{std::chrono::milliseconds(60),std::chrono::milliseconds(2)};
const char* Name(int i){return i==0?"A":"B";}
const std::string kShutterA="A:90c0(ffffffff)",kShutterB="B:90c0(ffffffff)";
struct Lane {
    Fake fake;
    WpdStillCaptureObservation observation;
    std::vector<unsigned char> partial,saved;
    std::optional<WpdStillCaptureRun> run;
    bool persisted{},closed{};
    int collects{},rescues{};
};
// Two synthetic bodies and the step callbacks the real window supplies, kept as
// close to wpd_dual_preview_main.cpp as an in-memory persist allows.
struct Harness {
    std::array<Lane,2> lane;
    std::vector<std::string> trace,log,events;
    std::atomic<bool> stop{};
    bool deadline_passed{};
    std::array<bool,2> blocked{};
    Clock::time_point deadline=Clock::now()+std::chrono::seconds(60);
    std::function<void()> after_prepare_b;
    std::function<void(int)> close_hook;
    Harness(){
        lane[0].fake.jpeg={0xff,0xd8,1,1,1,1,0xff,0xd9};
        lane[1].fake.jpeg={0xff,0xd8,2,2,2,2,2,2,0xff,0xd9};
        for(int i=0;i<2;++i){
            auto& fake=lane[i].fake;
            fake.tag=*Name(i);fake.trace=&trace;
            // Same rule as StillLane::Admitted: nothing after the deadline or a
            // block; a dispatched shutter keeps its import after a stop request.
            fake.admission=[this,i]{return !deadline_passed && !blocked[i] && (lane[i].fake.shutters>0 || !stop);};
        }
    }
    VerifiedStillPair Run(){
        StillPairSteps steps;
        steps.preflight=[&](int i,Clock::time_point){events.push_back(std::string("preflight:")+Name(i));return VerifiedStillPreflight{true,true,{}};};
        steps.prepare=[&](int i,Clock::time_point){
            events.push_back(std::string("open:")+Name(i));auto& l=lane[i];
            l.run.emplace(l.fake,[&l]{return l.fake.admission();},[&l](std::span<const unsigned char> bytes){l.partial.insert(l.partial.end(),bytes.begin(),bytes.end());},&l.observation,kWait);
            const auto prepared=PrepareWpdStillRun(*l.run);
            if(i==1 && after_prepare_b)after_prepare_b();
            return prepared;
        };
        steps.shutter=[&](int i){
            auto outcome=SendWpdStillShutter(*lane[i].run);
            if(lane[i].fake.shutters==0)outcome.state=StillShutterState::NotSent; // As StillLane::SendShutter.
            return outcome;
        };
        steps.collect=[&](int i){
            auto& l=lane[i];++l.collects;VerifiedStillOriginal original;
            try{
                auto payload=l.run->Collect();l.saved=payload.jpeg;l.persisted=true;
                original={true,false,std::filesystem::path(std::string(Name(i))+"/original.jpg"),payload.jpeg.size(),std::string(64,i==0?'a':'b'),{}};
            }catch(const WpdStillOutOfFocus& error){original.error=error.what();original.out_of_focus=true;}
            catch(const std::exception& error){original.error=error.what();}
            return original;
        };
        steps.rescue=[&](int i){++lane[i].rescues;lane[i].run->RetainUncertainShutterCandidate();};
        steps.close=[&](int i,VerifiedStillOriginal& original){
            events.push_back(std::string("close:")+Name(i));
            if(close_hook)close_hook(i);
            lane[i].closed=true;original.closed=true;
        };
        steps.log=[&](const std::string& line){log.push_back(line);};
        return CaptureStillPairShutterFirstOnce(steps,[&]{return !stop.load();},deadline);
    }
};
size_t Index(const std::vector<std::string>& trace,const std::string& entry){
    for(size_t i=0;i<trace.size();++i)if(trace[i]==entry)return i;
    return std::string::npos;
}
int Count(const std::vector<std::string>& trace,const std::string& entry){int n=0;for(const auto& e:trace)n+=e==entry;return n;}
bool IsImageOperation(const std::string& entry){return Contains(entry,":1008(") || Contains(entry,":1009(");}
bool Has(const std::vector<std::string>& events,const std::string& entry){return Index(events,entry)!=std::string::npos;}
bool HasLog(const Harness& h,std::string_view part){for(const auto& line:h.log)if(Contains(line,part))return true;return false;}
// Every command sent over either body is one of the allowlisted still
// operations, with the fixed no-AF shutter argument; never 0x9207; each shutter
// at most once (no retry).
void CheckSent(const Harness& h){
    static const std::set<std::string> allowed{"90c8","90c7","90c0(ffffffff)","1015(0000d1a2)","1015(00005004)","1015(00005003)","1008(0000007b)","1009(0000007b)"};
    bool all_allowed=true;
    for(const auto& entry:h.trace)all_allowed=all_allowed && entry.size()>2 && entry[1]==':' && allowed.count(entry.substr(2))==1;
    Check(all_allowed);
    Check(Count(h.trace,kShutterA)<=1 && Count(h.trace,kShutterB)<=1);
    Check(h.lane[0].fake.shutters<=1 && h.lane[1].fake.shutters<=1);
    for(const auto& entry:h.trace)Check(!Contains(entry,"9207"));
}
void CheckClosedBoth(const Harness& h){Check(h.lane[0].closed && h.lane[1].closed);}
}
int main(){
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    {
        // Happy path: preflight A, B -> open A, B -> shutter A, B -> import A, B -> close A, B.
        {Harness h;h.lane[0].fake.on_shutter=[]{std::this_thread::sleep_for(std::chrono::milliseconds(30));};
            const auto r=h.Run();CheckSent(h);
            Check(r.complete && r.preflight_complete && r.shutter_dispatched[0] && r.shutter_dispatched[1] && r.error.empty());
            const auto a=Index(h.trace,kShutterA),b=Index(h.trace,kShutterB);
            Check(a!=std::string::npos && b==a+1); // Nothing between the two shutters.
            bool image_before_second_shutter=false;
            for(size_t i=0;i<=b;++i)image_before_second_shutter=image_before_second_shutter || IsImageOperation(h.trace[i]);
            Check(!image_before_second_shutter);
            bool image_after=false;for(size_t i=b+1;i<h.trace.size();++i)image_after=image_after || IsImageOperation(h.trace[i]);
            Check(image_after);
            Check(h.events==std::vector<std::string>{"preflight:A","preflight:B","open:A","open:B","close:A","close:B"});
            Check(h.lane[0].persisted && h.lane[1].persisted && h.lane[0].saved==h.lane[0].fake.jpeg && h.lane[1].saved==h.lane[1].fake.jpeg);
            Check(h.lane[0].saved!=h.lane[1].saved); // Same object handle on both bodies, still no swap.
            Check(h.lane[0].fake.shutters==1 && h.lane[1].fake.shutters==1);
            Check(r.shutter_gap_ms.has_value() && *r.shutter_gap_ms>=30 && *r.shutter_gap_ms<2000);
            Check(r.shutter_gap_ms && HasLog(h,"pair_shutters shutter_gap_ms="+std::to_string(*r.shutter_gap_ms)));
            Check(r.originals[0].success && r.originals[0].closed && r.originals[1].success && r.originals[1].closed);
            // Each body's own observation, both complete on time.
            Check(h.lane[0].observation.shutter_attempted && h.lane[0].observation.complete_event && h.lane[1].observation.complete_event);
            Check(DescribeWpdStillCaptureObservation(h.lane[1].observation)=="shutter_attempted=1 complete_event=1 complete_event_code=0xc102");}
        // Import failure: A only, B only, both. The failed body keeps its private
        // partial and no original; the other body's original is saved; nothing is
        // re-read or re-shot. B is imported even when A failed (already exposed).
        for(int mode=0;mode<3;++mode){
            Harness h;const bool a_fails=mode!=1,b_fails=mode!=0;
            h.lane[0].fake.reject_object_end=a_fails;h.lane[1].fake.reject_object_end=b_fails;
            const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && Contains(r.error,"still data transfer failed"));
            Check(r.shutter_dispatched[0] && r.shutter_dispatched[1] && h.lane[0].fake.shutters==1 && h.lane[1].fake.shutters==1);
            for(int i=0;i<2;++i){
                const bool fails=i==0?a_fails:b_fails;auto& l=h.lane[i];
                Check(l.collects==1 && l.fake.object_reads==1);
                if(fails)Check(!l.persisted && l.partial==l.fake.jpeg && !r.originals[i].success);
                else Check(l.persisted && l.saved==l.fake.jpeg && r.originals[i].success && l.partial==l.fake.jpeg);
            }
            const auto a=Index(h.trace,kShutterA),b=Index(h.trace,kShutterB);Check(b==a+1);
        }
        // Deadline already passed: nothing is opened or sent.
        {Harness h;h.deadline=Clock::now()-std::chrono::seconds(1);const auto r=h.Run();
            Check(!r.complete && r.error=="pair preflight deadline/admission" && h.trace.empty() && h.events.empty() && !r.shutter_dispatched[0] && !r.shutter_dispatched[1]);}
        // Deadline reached after both shutters: no import starts, both close, both
        // exposures stay in the cameras. Nothing is resent.
        {Harness h;h.lane[1].fake.on_shutter=[&]{h.deadline_passed=true;};
            const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && h.lane[0].fake.shutters==1 && h.lane[1].fake.shutters==1);
            Check(!h.lane[0].persisted && !h.lane[1].persisted && h.lane[0].fake.object_reads==0 && h.lane[1].fake.object_reads==0);
            Check(h.lane[0].partial.empty() && h.lane[1].partial.empty() && !r.originals[0].success && !r.originals[1].success);}
        // Deadline reached between the shutters: B is never exposed; A's import is
        // refused too (no transport after the deadline).
        {Harness h;h.deadline=Clock::now()+std::chrono::seconds(2);
            h.lane[0].fake.on_shutter=[&]{while(Clock::now()<h.deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));h.deadline_passed=true;};
            const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(r.shutter_dispatched[0] && !r.shutter_dispatched[1] && !r.shutter_gap_ms && h.lane[1].fake.shutters==0);
            Check(!r.complete && !h.lane[0].persisted && h.lane[0].fake.object_reads==0 && h.lane[1].collects==0);
            Check(HasLog(h,"pair_shutters shutter_gap_ms=none a_dispatched=1 b_dispatched=0"));}
        // Stop requested before the transaction: nothing happens.
        {Harness h;h.stop=true;const auto r=h.Run();
            Check(!r.complete && h.trace.empty() && r.error=="pair preflight deadline/admission");}
        // Stop between the exposure recheck and A's shutter: no shutter for either.
        {Harness h;h.after_prepare_b=[&]{h.stop=true;};const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && r.error=="pair shutter deadline/admission" && h.lane[0].fake.shutters==0 && h.lane[1].fake.shutters==0 && !r.shutter_dispatched[0] && !r.shutter_dispatched[1]);
            for(const auto& entry:h.trace)Check(!IsImageOperation(entry));}
        // Stop right after A's shutter: B is not exposed; A's dispatched exposure is
        // still imported and saved.
        {Harness h;h.lane[0].fake.on_shutter=[&]{h.stop=true;};const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && r.shutter_dispatched[0] && !r.shutter_dispatched[1] && h.lane[1].fake.shutters==0 && h.lane[1].collects==0);
            Check(h.lane[0].persisted && h.lane[0].saved==h.lane[0].fake.jpeg && r.originals[0].success);
            Check(Contains(r.error,"CAM-B shutter not sent"));}
        // Stop after both shutters: both dispatched exposures are imported and saved;
        // the transaction is still reported incomplete.
        {Harness h;h.lane[1].fake.on_shutter=[&]{h.stop=true;};const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(h.lane[0].persisted && h.lane[1].persisted && r.originals[0].success && r.originals[1].success);
            Check(!r.complete && r.error=="pair completed after deadline/admission");}
        // B's shutter acknowledgement fails: the command was sent once, never
        // resent. A's original is kept; B's candidate is retained as a private
        // partial only, after A's import.
        {Harness h;h.lane[1].fake.reject_shutter=true;const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && r.shutter_dispatched[0] && r.shutter_dispatched[1] && h.lane[1].fake.shutters==1 && r.shutter_gap_ms.has_value());
            Check(h.lane[0].persisted && r.originals[0].success && h.lane[0].saved==h.lane[0].fake.jpeg);
            Check(h.lane[1].rescues==1 && h.lane[1].collects==0 && !h.lane[1].persisted && !r.originals[1].success);
            Check(h.lane[1].partial==h.lane[1].fake.jpeg); // Candidate kept as partial, not an original.
            Check(Contains(r.error,"unconfirmed") && Contains(r.originals[1].error,"unconfirmed"));
            const auto a_object=Index(h.trace,"A:1009(0000007b)"),b_object=Index(h.trace,"B:1009(0000007b)");
            Check(a_object!=std::string::npos && b_object!=std::string::npos && a_object<b_object);}
        // B's shutter is refused before reaching the transport (admission): B was
        // never exposed, A is still imported and saved, no gap is claimed.
        {Harness h;h.lane[0].fake.on_shutter=[&]{h.blocked[1]=true;};const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && r.shutter_dispatched[0] && !r.shutter_dispatched[1] && !r.shutter_gap_ms && h.lane[1].fake.shutters==0);
            Check(h.lane[0].persisted && r.originals[0].success && h.lane[1].rescues==0 && h.lane[1].collects==0 && h.lane[1].partial.empty());
            Check(HasLog(h,"shutter_gap_ms=none"));}
        // A's shutter acknowledgement fails: B is never exposed.
        {Harness h;h.lane[0].fake.reject_shutter=true;const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && h.lane[0].fake.shutters==1 && h.lane[1].fake.shutters==0 && r.shutter_dispatched[0] && !r.shutter_dispatched[1] && !r.shutter_gap_ms);
            Check(h.lane[0].rescues==1 && !h.lane[0].persisted && h.lane[0].partial==h.lane[0].fake.jpeg && h.lane[1].collects==0 && h.lane[1].rescues==0);
            for(const auto& entry:h.trace)Check(entry[0]!='B' || !Contains(entry,"90c0")); // No B shutter.
            Check(Contains(r.error,"unconfirmed"));}
        // Exposure recheck refused for A: no shutter anywhere, B is never opened.
        {Harness h;h.lane[0].fake.preflight_event=0xc101;const auto r=h.Run();CheckSent(h);
            Check(!r.complete && r.preflight_complete && Contains(r.error,"CAM-A exposure preflight: pending SDRAM object code=0xc101 phase=before_shutter"));
            Check(!Has(h.events,"open:B") && Has(h.events,"close:A") && !Has(h.events,"close:B") && h.lane[0].fake.shutters==0);
            for(const auto& entry:h.trace)Check(entry[0]=='A');}
        // ... and for B: A is open but not exposed; both close.
        {Harness h;h.lane[1].fake.preflight_event=0x4005;const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && Contains(r.error,"CAM-B exposure preflight:") && h.lane[0].fake.shutters==0 && h.lane[1].fake.shutters==0 && !r.shutter_dispatched[0]);}
        // OutOfFocus before the shutter is typed through the pair result.
        {Harness h;h.lane[1].fake.out_of_focus_before_shutter=true;const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && r.out_of_focus && h.lane[0].fake.shutters==0 && h.lane[1].fake.shutters==0);}
        // OutOfFocus reported after A's shutter is only known once A is read back:
        // B has been exposed by then and is imported once; the pair fails typed.
        {Harness h;h.lane[0].fake.out_of_focus_after_shutter=true;const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && r.out_of_focus && !h.lane[0].persisted && h.lane[0].fake.object_reads==0);
            Check(h.lane[1].persisted && r.originals[1].success && h.lane[1].fake.shutters==1);}
        // A session that will not close fails that body and still closes the other.
        {Harness h;h.close_hook=[&](int i){if(i==0)throw std::runtime_error("close refused");};const auto r=h.Run();
            Check(!r.complete && h.lane[1].closed && !r.originals[0].success && Contains(r.originals[0].error,"session close failed"));}

        // #381 inside a pair. The completion belongs to the body whose queue it was
        // read from: distinct codes show nothing is swapped, even though A's
        // arrives only after B's shutter and B's came with its image.
        {Harness h;h.lane[0].fake.use_script=true;h.lane[0].fake.post_script={{{0xc101,123}},{},{},{},{{0x400d,0}}};
            const auto r=h.Run();CheckSent(h);
            Check(r.complete);
            Check(h.lane[0].observation.complete_event && h.lane[0].observation.complete_event_code==0x400d && h.lane[0].observation.completion_wait==WpdStillCompletionWaitResult::Seen);
            Check(h.lane[1].observation.complete_event && h.lane[1].observation.complete_event_code==0xc102 && h.lane[1].observation.completion_wait==WpdStillCompletionWaitResult::AlreadySeen);
            Check(DescribeWpdStillCaptureObservation(h.lane[0].observation)=="shutter_attempted=1 complete_event=1 complete_event_code=0x400d");
            Check(DescribeWpdStillCaptureObservation(h.lane[1].observation)=="shutter_attempted=1 complete_event=1 complete_event_code=0xc102");}
        // Only B ever reports a completion: A is not credited with it. A's wait
        // times out, the log says so, and both originals are still saved.
        {Harness h;h.lane[0].fake.use_script=true;h.lane[0].fake.post_script={{{0xc101,123}}};
            const auto r=h.Run();CheckSent(h);
            Check(r.complete && h.lane[0].persisted && h.lane[1].persisted);
            Check(!h.lane[0].observation.complete_event && h.lane[0].observation.completion_wait==WpdStillCompletionWaitResult::TimedOut && h.lane[0].observation.completion_wait_ms>=kWait.max.count());
            Check(Contains(DescribeWpdStillCompletionWait(h.lane[0].observation,kWait),"result=timeout"));
            Check(DescribeWpdStillCaptureObservation(h.lane[0].observation)=="shutter_attempted=1 complete_event=0 complete_event_code=none");
            Check(h.lane[1].observation.complete_event && h.lane[1].observation.complete_event_code==0xc102);}
        // A foreign completion in A's queue (a second one for the same exposure) is
        // rejected as a duplicate. A keeps only its partial; B is unaffected.
        {Harness h;h.lane[0].fake.use_script=true;h.lane[0].fake.post_script={{{0xc101,123},{0xc102,0}},{{0x400d,0}}};
            const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && Contains(r.error,"duplicate capture completion code=0x400d phase=after_shutter"));
            Check(!h.lane[0].persisted && h.lane[0].partial==h.lane[0].fake.jpeg && h.lane[0].fake.shutters==1);
            Check(h.lane[1].persisted && r.originals[1].success && h.lane[1].saved==h.lane[1].fake.jpeg);}
        // A foreign image event in B's queue is ambiguous: B fails with its partial.
        {Harness h;h.lane[1].fake.use_script=true;h.lane[1].fake.post_script={{{0xc101,123}},{{0xc101,200}}};
            const auto r=h.Run();CheckSent(h);CheckClosedBoth(h);
            Check(!r.complete && Contains(r.error,"ambiguous SDRAM object") && h.lane[0].persisted && r.originals[0].success);
            Check(!h.lane[1].persisted && h.lane[1].partial==h.lane[1].fake.jpeg);}
    }
    CoUninitialize();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
