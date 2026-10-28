#include "wpd_still_fake.hpp"
#include <algorithm>
#include <iostream>
using namespace a0::phase0::experimental;
namespace {
int checks{},failures{};
void Check(bool value){++checks;if(!value)++failures;}
template<class F>void Reject(F f){bool rejected=false;try{f();}catch(...){rejected=true;}Check(rejected);}
// Requires the typed event rejection with this code/phase; returns its text.
template<class F>std::string RejectedEvent(F f,std::uint16_t code,bool after_shutter){
    std::string what;bool typed=false;
    try{f();}catch(const WpdStillEventRejected& e){typed=e.Code()==code && e.AfterShutter()==after_shutter;what=e.what();}catch(...){}
    Check(typed);return what;
}
// Test-sized completion wait. Production uses kWpdStillCompletionWaitMax.
const WpdStillCompletionWait kShortWait{std::chrono::milliseconds(60),std::chrono::milliseconds(2)};
bool Contains(const std::string& text,std::string_view part){return text.find(part)!=std::string::npos;}
}
int main(){
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    {
        for(auto op:{WpdStillOperation::Ready,WpdStillOperation::LiveStatus,WpdStillOperation::ImageQuality,WpdStillOperation::ImageSize,WpdStillOperation::Events})Check(AllowedWpdStillRequest(BuildWpdStillRequest(op).Get(),false,{}));
        auto shutter=BuildWpdStillRequest(WpdStillOperation::CaptureNoAf);Check(AllowedWpdStillRequest(shutter.Get(),true,{}));Check(!AllowedWpdStillRequest(shutter.Get(),false,{}));
        Check(AllowedWpdStillEventDiagnosticRequest(BuildWpdStillRequest(WpdStillOperation::Events).Get()));
        for(auto op:{WpdStillOperation::Ready,WpdStillOperation::LiveStatus,WpdStillOperation::ImageQuality,WpdStillOperation::ImageSize,WpdStillOperation::CaptureNoAf})
            Check(!AllowedWpdStillEventDiagnosticRequest(BuildWpdStillRequest(op).Get()));
        for(auto op:{WpdStillOperation::Object,WpdStillOperation::ObjectInfo})Check(!AllowedWpdStillEventDiagnosticRequest(BuildWpdStillRequest(op,123).Get()));
        Check(!AllowedWpdStillEventDiagnosticRequest(nullptr));
        Check(AllowedWpdStillEventDiagnosticRequest(BuildWpdLiveViewRead(L"diagnostic",32).Get()));
        Check(AllowedWpdStillEventDiagnosticRequest(BuildWpdLiveViewEndTransfer(L"diagnostic").Get()));
        for(DWORD forbidden:{0x9207u,0x90cbu,0x100eu,0x1016u,0x100bu,0x100fu,0x9201u}){shutter->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,forbidden);Check(!AllowedWpdStillRequest(shutter.Get(),true,{}));}
        for(auto op:{WpdStillOperation::Object,WpdStillOperation::ObjectInfo}){auto request=BuildWpdStillRequest(op,123);Check(!AllowedWpdStillRequest(request.Get(),false,{}));Check(AllowedWpdStillRequest(request.Get(),false,123));Check(!AllowedWpdStillRequest(request.Get(),false,124));}
        Reject([]{BuildWpdStillRequest(WpdStillOperation::Object,0);});Reject([]{BuildWpdStillRequest(WpdStillOperation::Object,0xffffffff);});
        Check(ParseWpdStillEvents(Events({{0x4006,1}})).size()==1);Reject([]{ParseWpdStillEvents({});});Reject([]{ParseWpdStillEvents(std::vector<unsigned char>{2,0});});
        WpdStillEventLedger l;l.Observe(ParseWpdStillEvents(Events({{0x4006,1}})),false);Check(!l.Handle());
        Reject([&]{l.Observe(ParseWpdStillEvents(Events({{0xc101,123}})),false);});
        WpdStillEventLedger ledger;ledger.Observe(ParseWpdStillEvents(Events({{0xc101,123},{0xc102,0}})),true);Check(ledger.Handle()==123 && ledger.Completed());
        Reject([&]{ledger.Observe(ParseWpdStillEvents(Events({{0xc101,124}})),true);});
        Check(ParseWpdStillObjectInfo(Info(100)).bytes==100);auto bad=Info(100);Put(bad,4,0x3800,2);Reject([&]{ParseWpdStillObjectInfo(bad);});
        bad=Info(100);Put(bad,26,640,4);Reject([&]{ParseWpdStillObjectInfo(bad);});Check(ParseWpdStillString(Text(L"7360x4912"))==L"7360x4912");
        {Fake f;std::vector<unsigned char> partial;auto r=AcquireWpdStillPayload(f,[]{return true;},[&](auto chunk){partial.insert(partial.end(),chunk.begin(),chunk.end());});Check(f.shutters==1 && f.object_reads==1 && r.jpeg==f.jpeg && partial==f.jpeg && r.complete_event);Reject([&]{f.ShutterOnce();});Check(f.shutters==1);}
        {Fake f;f.pending=true;Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){});});Check(f.shutters==0);}
        {Fake f;auto baseline=ReadWpdStillPreflight(f,[]{return true;});
            Check(f.shutters==0 && f.object_reads==0 && f.profile_reads==1 && f.preflight_reads==2 && !baseline.Handle() && !baseline.Completed());
            auto captured=AcquireWpdStillPayload(f,[]{return true;},[](auto){});Check(captured.jpeg==f.jpeg && f.profile_reads==2 && f.shutters==1);}
        for(DWORD event:{0xc101u,0xc102u,0x4005u,0x400bu,0x4fffu}){Fake f;f.preflight_event=event;
            const auto what=RejectedEvent([&]{ReadWpdStillPreflight(f,[]{return true;});},static_cast<std::uint16_t>(event),false);
            Check(Contains(what,"code="+WpdStillEventCodeText(static_cast<std::uint16_t>(event))+" phase=before_shutter"));
            Check(f.shutters==0 && f.object_reads==0);}
        // StoreRemoved before shutter names its code (#381).
        {Fake f;f.preflight_event=0x4005;
            Check(RejectedEvent([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){});},0x4005,false)=="unclassified capture event code=0x4005 phase=before_shutter");}
        {Fake f;f.preflight_event=0xc101;
            Check(RejectedEvent([&]{ReadWpdStillPreflight(f,[]{return true;});},0xc101,false)=="pending SDRAM object code=0xc101 phase=before_shutter");}
        {Fake f;f.bad_quality=true;Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){});});Check(f.shutters==0);}
        for(bool before:{false,true}){Fake f;f.out_of_focus_after_shutter=!before;f.out_of_focus_before_shutter=before;
            bool typed=false;try{AcquireWpdStillPayload(f,[]{return true;},[](auto){});}catch(const WpdStillOutOfFocus&){typed=true;}
            Check(typed && f.shutters==(before?0:1) && f.object_reads==0 && !f.event_sent);}
        {Fake f;f.metadata_before=true;auto r=AcquireWpdStillPayload(f,[]{return true;},[](auto){});Check(f.shutters==1 && f.profile_reads==2 && r.jpeg==f.jpeg);}
        {Fake f;f.metadata_after=true;Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){});});Check(f.shutters==1 && f.object_reads==0);}
        // StoreAdded is a preflight invalidation, never a new captured object.
        for(int poll:{0,1}){Fake f;f.preflight_event=0x4004;f.preflight_event_on_read=poll;bool ok=false;
            try{auto r=AcquireWpdStillPayload(f,[]{return true;},[](auto){});ok=r.jpeg==f.jpeg && r.complete_event;}catch(const std::exception&){}
            Check(ok && f.shutters==1 && f.profile_reads==2 && f.preflight_reads==4 && f.object_reads==1);}
        {Fake f;f.preflight_event=0x4004;f.bad_quality_after_refresh=true;
            Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){});});Check(f.profile_reads==2 && f.shutters==0 && f.object_reads==0);}
        {Fake f;f.preflight_event=0x4004;f.late_pending=true;
            Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){});});Check(f.profile_reads==2 && f.shutters==0 && f.object_reads==0);}
        {Fake f;f.preflight_event=0x4004;f.repeat_preflight_event=true;
            Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){});});Check(f.profile_reads==2 && f.preflight_reads<=4 && f.shutters==0 && f.object_reads==0);}
        for(DWORD companion:{0xc101u,0x4002u,0x4005u,0x400bu,0x4fffu}){Fake f;f.preflight_event=0x4004;f.preflight_companion=companion;
            Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){});});Check(f.shutters==0 && f.object_reads==0);}
        for(DWORD event:{0x4004u,0x4005u,0x400bu,0x4fffu}){Fake f;f.postflight_event=event;WpdStillCaptureObservation observed;
            const auto what=RejectedEvent([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){},&observed);},static_cast<std::uint16_t>(event),true);
            Check(Contains(what,"code="+WpdStillEventCodeText(static_cast<std::uint16_t>(event))+" phase=after_shutter"));
            Check(f.shutters==1 && f.object_reads==0 && observed.shutter_attempted && !observed.complete_event);}
        // #381: a completion that arrives only after the session's wait ends
        // stays queued in the camera. The next capture's preflight reads it and
        // stops before its shutter, naming the event as pending (not duplicate).
        // Reading it consumed the queue, so the capture after that passes: the
        // field pattern "first press fails, second press passes". The wait is
        // shortened here; production waits kWpdStillCompletionWaitMax.
        {std::vector<WpdStillEvent> camera_queue;
            Fake first;first.completion_late=true;first.camera_queue=&camera_queue;WpdStillCaptureObservation observed{true,true,0x400d};
            auto r=AcquireWpdStillPayload(first,[]{return true;},[](auto){},&observed,kShortWait);
            Check(observed.completion_wait==WpdStillCompletionWaitResult::TimedOut && observed.completion_wait_ms>=kShortWait.max.count());
            Check(r.jpeg==first.jpeg && !r.complete_event && first.shutters==1 && first.object_reads==1 && camera_queue.size()==1);
            Check(observed.shutter_attempted && !observed.complete_event && observed.complete_event_code==0);
            Check(DescribeWpdStillCaptureObservation(observed)=="shutter_attempted=1 complete_event=0 complete_event_code=none");
            Fake second;second.camera_queue=&camera_queue;WpdStillCaptureObservation rejected{true,true,0xc102};
            const auto what=RejectedEvent([&]{AcquireWpdStillPayload(second,[]{return true;},[](auto){},&rejected);},0xc102,false);
            Check(what=="pending capture completion code=0xc102 phase=before_shutter" && !Contains(what,"duplicate"));
            Check(second.shutters==0 && second.object_reads==0 && camera_queue.empty());
            Check(DescribeWpdStillCaptureObservation(rejected)=="shutter_attempted=0 complete_event=0 complete_event_code=none");
            Fake third;third.camera_queue=&camera_queue;WpdStillCaptureObservation passed;
            auto again=AcquireWpdStillPayload(third,[]{return true;},[](auto){},&passed);
            Check(again.jpeg==third.jpeg && again.complete_event && third.shutters==1 && passed.complete_event && passed.complete_event_code==0xc102);
            Check(DescribeWpdStillCaptureObservation(passed)=="shutter_attempted=1 complete_event=1 complete_event_code=0xc102");}
        // #381 wait: the completion is observed inside the same session, before it
        // closes, whenever it arrives within the bound.
        {Fake f;WpdStillCaptureObservation observed;
            auto r=AcquireWpdStillPayload(f,[]{return true;},[](auto){},&observed,kShortWait);
            Check(r.complete_event && observed.completion_wait==WpdStillCompletionWaitResult::AlreadySeen && observed.completion_wait_ms==0);
            Check(DescribeWpdStillCompletionWait(observed,kShortWait)=="result=already_seen waited_ms=0 max_ms=60");}
        // Completion after the image, on a later poll (0x400d is also accepted).
        for(std::uint16_t code:{0xc102,0x400d}){
            Fake f;f.use_script=true;f.post_script={{{0xc101,123}},{},{},{},{{code,0}}};WpdStillCaptureObservation observed;
            auto r=AcquireWpdStillPayload(f,[]{return true;},[](auto){},&observed,kShortWait);
            Check(r.jpeg==f.jpeg && r.complete_event && observed.complete_event && observed.complete_event_code==code);
            Check(observed.completion_wait==WpdStillCompletionWaitResult::Seen && observed.completion_wait_ms>0 && observed.completion_wait_ms<kShortWait.max.count()+40);
            Check(f.shutters==1 && f.object_reads==1);
            Check(DescribeWpdStillCaptureObservation(observed)=="shutter_attempted=1 complete_event=1 complete_event_code="+WpdStillEventCodeText(code));
            Check(Contains(DescribeWpdStillCompletionWait(observed,kShortWait),"result=seen waited_ms="));}
        // Never delivered: the capture still succeeds, the wait is bounded, and
        // the log says so. No shutter or read of the object is repeated.
        {Fake f;f.use_script=true;f.post_script={{{0xc101,123}}};WpdStillCaptureObservation observed;
            const auto started=std::chrono::steady_clock::now();
            auto r=AcquireWpdStillPayload(f,[]{return true;},[](auto){},&observed,kShortWait);
            const auto took=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started);
            Check(r.jpeg==f.jpeg && !r.complete_event && !observed.complete_event && observed.shutter_attempted);
            Check(observed.completion_wait==WpdStillCompletionWaitResult::TimedOut && observed.completion_wait_ms>=kShortWait.max.count());
            Check(took<std::chrono::milliseconds(3000)); // Bounded: never the 5 s production bound times anything.
            Check(Contains(DescribeWpdStillCompletionWait(observed,kShortWait),"result=timeout waited_ms="));
            Check(f.shutters==1 && f.object_reads==1);}
        // Production bound stays inside the 180 s transaction with room for a pair.
        Check(kWpdStillCompletionWaitMax==std::chrono::milliseconds(5000) && 2*kWpdStillCompletionWaitMax<std::chrono::seconds(180)/10);
        // Admission ending during the wait aborts it without failing the capture;
        // the caller's deadline check decides the transaction.
        {Fake f;f.use_script=true;f.post_script={{{0xc101,123}}};WpdStillCaptureObservation observed;
            auto r=AcquireWpdStillPayload(f,[&]{return f.post_reads<=3;},[](auto){},&observed,kShortWait);
            Check(r.jpeg==f.jpeg && !r.complete_event && observed.completion_wait==WpdStillCompletionWaitResult::Aborted);
            Check(Contains(DescribeWpdStillCompletionWait(observed,kShortWait),"result=aborted"));}
        // A second completion while waiting is not accepted silently. The first
        // one ends the wait, so this arrives as a duplicate on the trailing poll.
        {Fake f;f.use_script=true;f.post_script={{{0xc101,123},{0xc102,0}},{{0x400d,0}}};WpdStillCaptureObservation observed;size_t partial=0;
            const auto what=RejectedEvent([&]{AcquireWpdStillPayload(f,[]{return true;},[&](auto c){partial+=c.size();},&observed,kShortWait);},0x400d,true);
            Check(what=="duplicate capture completion code=0x400d phase=after_shutter" && partial==f.jpeg.size() && observed.complete_event);}
        // A rejected event during the wait itself (storage changed) stays terminal
        // and keeps the partial; the shutter is not resent.
        {Fake f;f.use_script=true;f.post_script={{{0xc101,123}},{},{},{{0x4005,0}}};WpdStillCaptureObservation observed;size_t partial=0;
            const auto what=RejectedEvent([&]{AcquireWpdStillPayload(f,[]{return true;},[&](auto c){partial+=c.size();},&observed,kShortWait);},0x4005,true);
            Check(Contains(what,"phase=after_shutter") && partial==f.jpeg.size() && f.shutters==1 && !observed.complete_event && observed.completion_wait==WpdStillCompletionWaitResult::Aborted);}
        // A second completion inside one capture is a duplicate, after shutter.
        {Fake f;f.duplicate_completion=true;WpdStillCaptureObservation observed;size_t partial=0;
            const auto what=RejectedEvent([&]{AcquireWpdStillPayload(f,[]{return true;},[&](auto c){partial+=c.size();},&observed);},0x400d,true);
            Check(what=="duplicate capture completion code=0x400d phase=after_shutter" && !Contains(what,"pending"));
            Check(f.shutters==1 && f.object_reads==1 && partial==f.jpeg.size() && observed.complete_event && observed.complete_event_code==0xc102);
            Check(DescribeWpdStillCaptureObservation(observed)=="shutter_attempted=1 complete_event=1 complete_event_code=0xc102");}
        {Fake f;f.preflight_event=0x4004;
            Reject([&]{AcquireWpdStillPayload(f,[&]{return f.profile_reads<2;},[](auto){});});Check(f.profile_reads==2 && f.shutters==0 && f.object_reads==0);}
        {Fake f;f.ambiguous=true;Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){});});Check(f.shutters==1 && f.object_reads==0);}
        {Fake f;f.reject_object_end=true;size_t partial=0;Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[&](auto c){partial+=c.size();});});Check(f.shutters==1 && partial==f.jpeg.size());}
        {Fake f;f.reject_shutter=true;size_t partial=0;Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[&](auto c){partial+=c.size();});});Check(f.shutters==1 && !f.CaptureConfirmed() && f.object_reads==1 && partial==f.jpeg.size());}
        {Fake f;f.admitted=false;Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){});});Check(f.shutters==0);}
        {Fake f;f.owned=false;Reject([&]{f.ShutterOnce();});Check(f.shutters==0);}
    }
    CoUninitialize();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
