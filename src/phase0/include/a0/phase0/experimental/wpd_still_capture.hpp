#pragma once
#include "a0/phase0/experimental/wpd_live_view_transfer.hpp"
#include <PortableDevice.h>
#include <WpdMtpExtensions.h>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <exception>
#include <cstdio>
#include <string>
#include <vector>

namespace a0::phase0::experimental {
enum class WpdStillOperation { Ready, LiveStatus, ImageQuality, ImageSize, Events, CaptureNoAf, ObjectInfo, Object };
inline bool ValidStillHandle(DWORD handle){return handle!=0 && handle!=0xffffffff;}
inline WpdValues BuildWpdStillRequest(WpdStillOperation operation,DWORD handle=0){
    DWORD code{};bool read=true;std::vector<DWORD> arguments;
    switch(operation){
    case WpdStillOperation::Ready:code=0x90c8;read=false;break;
    case WpdStillOperation::LiveStatus:code=0x1015;arguments={0xd1a2};break;
    case WpdStillOperation::ImageQuality:code=0x1015;arguments={0x5004};break;
    case WpdStillOperation::ImageSize:code=0x1015;arguments={0x5003};break;
    case WpdStillOperation::Events:code=0x90c7;break;
    case WpdStillOperation::CaptureNoAf:code=0x90c0;read=false;arguments={0xffffffff};break;
    case WpdStillOperation::ObjectInfo:code=0x1008;arguments={handle};break;
    case WpdStillOperation::Object:code=0x1009;arguments={handle};break;
    default:throw std::invalid_argument("unsupported still operation");
    }
    if((code==0x1008 || code==0x1009) && !ValidStillHandle(handle))throw std::invalid_argument("invalid still handle");
    auto request=BuildWpdLiveViewOperation(read?WpdLiveViewOperation::Frame:WpdLiveViewOperation::Start);
    if(request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,code)!=S_OK)throw std::runtime_error("still opcode");
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> parameters;
    if(request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&parameters)!=S_OK)throw std::runtime_error("still params");
    for(auto argument:arguments){PROPVARIANT v{};v.vt=VT_UI4;v.ulVal=argument;if(parameters->Add(&v)!=S_OK)throw std::runtime_error("still parameter");}
    return request;
}
inline bool AllowedWpdStillRequest(IPortableDeviceValues* request,bool capture_armed,std::optional<DWORD> attributable_handle){
    if(!request)return false;
    GUID category{};DWORD id{},code{};
    if(request->GetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,&category)!=S_OK || request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,&id)!=S_OK)return false;
    PROPERTYKEY command{category,id};
    if(IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_READ_DATA) || IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER))return true;
    if(request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&code)!=S_OK)return false;
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;DWORD count{};
    if(request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&args)!=S_OK || !args || args->GetCount(&count)!=S_OK || count>1)return false;
    DWORD argument{};
    if(count){PROPVARIANT v{};const auto hr=args->GetAt(0,&v);const bool typed=hr==S_OK && v.vt==VT_UI4;argument=v.ulVal;PropVariantClear(&v);if(!typed)return false;}
    if(IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE))
        return (code==0x90c8 && count==0) || (code==0x90c0 && count==1 && argument==0xffffffff && capture_armed);
    if(!IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ))return false;
    return (code==0x90c7 && count==0) ||
        (code==0x1015 && count==1 && (argument==0xd1a2 || argument==0x5004 || argument==0x5003)) ||
        ((code==0x1008 || code==0x1009) && count==1 && attributable_handle && ValidStillHandle(*attributable_handle) && argument==*attributable_handle);
}
// Separate last-boundary gate for event diagnostics: no Ready, property,
// object, shutter, Live or lens operation can pass this channel.
inline bool AllowedWpdStillEventDiagnosticRequest(IPortableDeviceValues* request){
    if(!AllowedWpdStillRequest(request,false,{}))return false;
    GUID category{};DWORD id{},code{};
    if(request->GetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,&category)!=S_OK ||
        request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,&id)!=S_OK)return false;
    const PROPERTYKEY command{category,id};
    if(IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_READ_DATA) || IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER))return true;
    return request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&code)==S_OK && code==0x90c7;
}
inline std::uint32_t StillLittleEndian(std::span<const unsigned char> data,size_t offset,size_t size){
    if(size==0 || size>4 || offset>data.size() || size>data.size()-offset)throw std::runtime_error("truncated still metadata");
    std::uint32_t value{};for(size_t i=0;i<size;++i)value|=static_cast<std::uint32_t>(data[offset+i])<<(8*i);return value;
}
struct WpdStillEvent {std::uint16_t code{};DWORD parameter{};};
inline std::vector<WpdStillEvent> ParseWpdStillEvents(std::span<const unsigned char> data){
    const auto count=StillLittleEndian(data,0,2);
    if(count>1024 || data.size()!=2+6*static_cast<size_t>(count))throw std::runtime_error("invalid Nikon event payload");
    std::vector<WpdStillEvent> result;result.reserve(count);
    for(size_t i=0;i<count;++i)result.push_back({static_cast<std::uint16_t>(StillLittleEndian(data,2+6*i,2)),StillLittleEndian(data,4+6*i,4)});
    return result;
}
// A rejected event names its code and whether the shutter had been sent, so a
// completion left over from an earlier capture (read by the next preflight) is
// distinguishable from a second completion inside one capture (#381).
inline std::string WpdStillEventCodeText(std::uint16_t code){char text[8]{};std::snprintf(text,sizeof text,"0x%04x",static_cast<unsigned>(code));return text;}
class WpdStillEventRejected final:public std::runtime_error {
public:
    WpdStillEventRejected(const std::string& reason,std::uint16_t code,bool after_shutter):
        std::runtime_error(reason+" code="+WpdStillEventCodeText(code)+" phase="+(after_shutter?"after_shutter":"before_shutter")),
        code_(code),after_shutter_(after_shutter){}
    std::uint16_t Code()const{return code_;}
    bool AfterShutter()const{return after_shutter_;}
private:
    std::uint16_t code_{};bool after_shutter_{};
};
class WpdStillEventLedger final {
public:
    void Observe(std::span<const WpdStillEvent> events,bool after_shutter){
        for(const auto& event:events){
            auto reject=[&](const char* reason){throw WpdStillEventRejected(reason,event.code,after_shutter);};
            if(event.code==0x4006 || event.code==0xc104)continue; // property/obsolete event, no image ownership.
            if(event.code==0x4004 || event.code==0x4008 || event.code==0xc10c){
                // StoreAdded invalidates preflight; its parameter is not an
                // image handle. After shutter, any such change is terminal.
                if(after_shutter)reject("camera storage/metadata/live state changed during capture");
                refresh_required_=true;continue; // Never grants image ownership.
            }
            if(event.code==0xc101){
                if(!after_shutter)reject("pending SDRAM object");
                if(handle_)reject("ambiguous SDRAM object");
                if(!ValidStillHandle(event.parameter))reject("invalid SDRAM object handle");
                handle_=event.parameter;
            }else if(event.code==0xc102 || event.code==0x400d){
                // Before shutter it belongs to an earlier capture; after shutter
                // a second one is a duplicate. Both stay terminal.
                if(!after_shutter)reject("pending capture completion");
                if(completed_)reject("duplicate capture completion");
                completed_=true;completion_code_=event.code;
            }else if(event.code==0x4002)reject("unexpected card image event");
            else reject("unclassified capture event");
        }
    }
    std::optional<DWORD> Handle()const{return handle_;}
    bool Completed()const{return completed_;}
    std::uint16_t CompletionCode()const{return completion_code_;}
    bool ProfileRefreshRequired()const{return refresh_required_;}
private:
    std::optional<DWORD> handle_;bool completed_{},refresh_required_{};std::uint16_t completion_code_{};
};
struct WpdStillObjectInfo {std::uint32_t bytes{},width{},height{};};
inline WpdStillObjectInfo ParseWpdStillObjectInfo(std::span<const unsigned char> data){
    if(data.size()<52 || data.size()>4096 || StillLittleEndian(data,4,2)!=0x3801)throw std::runtime_error("still object is not JPEG metadata");
    WpdStillObjectInfo result{StillLittleEndian(data,8,4),StillLittleEndian(data,26,4),StillLittleEndian(data,30,4)};
    if(result.bytes==0 || result.bytes>WpdStillImageMaxPayloadBytes || result.width!=7360 || result.height!=4912)throw std::runtime_error("still object violates FX JPEG Fine L profile");
    return result;
}
inline std::wstring ParseWpdStillString(std::span<const unsigned char> data){
    if(data.empty() || data[0]==0 || data.size()!=1+2*static_cast<size_t>(data[0]) || data[data.size()-1]!=0 || data[data.size()-2]!=0)throw std::runtime_error("invalid PTP string");
    std::wstring value;for(size_t i=0;i+1<data[0];++i){auto c=StillLittleEndian(data,1+2*i,2);if(!c || c>127)throw std::runtime_error("non-ASCII image dimensions");value.push_back(static_cast<wchar_t>(c));}return value;
}
// The owner opens/closes a correctly bound device only after BOTH Live View
// sessions close. This channel cannot reopen, retry a shutter, or delete data.
class WpdStillCaptureChannel:public WpdLiveViewCommandChannel {
public:
    WpdStillCaptureChannel(std::function<bool()> may_continue,std::function<bool()> may_cleanup):
        may_continue_(std::move(may_continue)),may_cleanup_(std::move(may_cleanup)){
        if(!may_continue_ || !may_cleanup_)throw std::invalid_argument("missing still ownership admission");
    }
    bool CaptureAttempted()const{return capture_attempted_;}
    bool CaptureConfirmed()const{return capture_confirmed_;}
    // When the single shutter command was handed to the transport (#389). Unset
    // until then. It only feeds the A/B gap log, never an ordering decision.
    std::optional<std::chrono::steady_clock::time_point> ShutterDispatchedAt()const{return shutter_dispatched_at_;}
    WpdLiveViewCommandResult Ready(){
        auto request=BuildWpdStillRequest(WpdStillOperation::Ready);auto r=Dispatch(request.Get());
        return InspectWpdLiveViewCommand(r.status,r.results.Get(),true);
    }
    void ShutterOnce(){
        if(capture_attempted_)throw std::runtime_error("shutter is single-use");
        capture_armed_=true;
        auto request=BuildWpdStillRequest(WpdStillOperation::CaptureNoAf);auto r=Dispatch(request.Get());
        auto result=InspectWpdLiveViewCommand(r.status,r.results.Get(),true);
        capture_confirmed_=result.Ok();
        if(!capture_confirmed_)throw std::runtime_error("still shutter completion unconfirmed; not retried");
    }
    void AuthorizeObject(const WpdStillEventLedger& ledger){
        if(!capture_attempted_ || !ledger.Handle() || attributable_handle_)throw std::runtime_error("still object attribution not established");
        attributable_handle_=ledger.Handle();
    }
    std::vector<unsigned char> Read(WpdStillOperation operation,std::uint64_t cap,
        const std::function<void(std::span<const unsigned char>)>& partial_sink={}){
        if(operation==WpdStillOperation::CaptureNoAf || operation==WpdStillOperation::Ready)throw std::invalid_argument("not a read operation");
        WpdLiveViewTransferOptions options;options.may_continue=may_continue_;options.may_cleanup=may_cleanup_;
        options.purpose=WpdPayloadPurpose::StillImage;options.max_payload_bytes=cap;options.on_chunk=partial_sink;
        options.begin_request=[&]{return BuildWpdStillRequest(operation,attributable_handle_.value_or(0));};
        auto result=ReadWpdLiveViewPayload(*this,options);
        if(!result.Ok())throw std::runtime_error("still data transfer failed: "+std::to_string(static_cast<int>(result.error)));
        return std::move(result.payload);
    }
protected:
    virtual WpdLiveViewExchange Exchange(IPortableDeviceValues*)=0;
private:
    WpdLiveViewExchange Dispatch(IPortableDeviceValues* request) final {
        GUID category{};DWORD id{},code{};
        request->GetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,&category);request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,&id);
        request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&code);
        const PROPERTYKEY command{category,id};const bool cleanup=IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER);
        if(!may_cleanup_() || (!cleanup && !may_continue_()) || !AllowedWpdStillRequest(request,capture_armed_,attributable_handle_))throw std::runtime_error("still command not admitted");
        if(code==0x90c0){capture_armed_=false;capture_attempted_=true;shutter_dispatched_at_=std::chrono::steady_clock::now();}
        return Exchange(request);
    }
    std::function<bool()> may_continue_,may_cleanup_;
    bool capture_armed_{},capture_attempted_{},capture_confirmed_{};
    std::optional<DWORD> attributable_handle_;
    std::optional<std::chrono::steady_clock::time_point> shutter_dispatched_at_;
};
struct WpdStillPayload {std::vector<unsigned char> jpeg;WpdStillObjectInfo info;bool complete_event{};};
// #381: the completion event (0xC102/0x400D) can reach the camera's queue after
// the image has been read. A leftover is read by the next capture's preflight
// and stops it as "pending capture completion". Before closing, the capture
// therefore keeps polling the same session for it, inside the 180 s transaction
// deadline. This is observation, not a retry: nothing is resent and a missing
// completion does not fail an otherwise verified capture.
// Upper bound per camera. The SDRAM-object wait (15 s) already allows for a slow
// body; the completion follows the same exposure, so a third of that is ample
// and costs at most 2 x 5 s of the 180 s budget for a pair. No measured
// completion delay exists yet (an estimate): review it against `completion_wait_ms`
// from the field logs rather than treating 5 s as established.
inline constexpr std::chrono::milliseconds kWpdStillCompletionWaitMax{5000};
// Same cadence as the other event polls in this file (100 ms).
inline constexpr std::chrono::milliseconds kWpdStillCompletionPoll{100};
struct WpdStillCompletionWait {std::chrono::milliseconds max{kWpdStillCompletionWaitMax},poll{kWpdStillCompletionPoll};};
enum class WpdStillCompletionWaitResult {NotReached,AlreadySeen,Seen,TimedOut,Aborted};
inline const char* WpdStillCompletionWaitResultText(WpdStillCompletionWaitResult result){
    switch(result){
    case WpdStillCompletionWaitResult::AlreadySeen:return "already_seen";
    case WpdStillCompletionWaitResult::Seen:return "seen";
    case WpdStillCompletionWaitResult::TimedOut:return "timeout";
    case WpdStillCompletionWaitResult::Aborted:return "aborted";
    default:return "not_reached";
    }
}
// Filled on every exit of a capture, failures included, so each capture can log
// whether its completion event was read before the session closed. Completion is
// still not required for success (#381 observation).
struct WpdStillCaptureObservation {
    bool shutter_attempted{},complete_event{};std::uint16_t complete_event_code{};
    WpdStillCompletionWaitResult completion_wait{};long long completion_wait_ms{};
};
// The line that field counting relies on (#381): keep the three keys and their
// format exactly. Wait details go to DescribeWpdStillCompletionWait instead.
inline std::string DescribeWpdStillCaptureObservation(const WpdStillCaptureObservation& observation){
    return std::string("shutter_attempted=")+(observation.shutter_attempted?"1":"0")+
        " complete_event="+(observation.complete_event?"1":"0")+
        " complete_event_code="+(observation.complete_event?WpdStillEventCodeText(observation.complete_event_code):std::string("none"));
}
// The log fields for the completion wait (#381), written after the
// capture_completion line: `result=timeout` is a completion that was not seen
// before the session closed; `aborted` means the deadline or a stop ended the
// wait first; `already_seen` means no wait was needed.
inline std::string DescribeWpdStillCompletionWait(const WpdStillCaptureObservation& observation,const WpdStillCompletionWait& wait={}){
    return std::string("result=")+WpdStillCompletionWaitResultText(observation.completion_wait)+
        " waited_ms="+std::to_string(observation.completion_wait_ms)+
        " max_ms="+std::to_string(wait.max.count());
}
class WpdStillOutOfFocus final:public std::runtime_error {
public:WpdStillOutOfFocus():std::runtime_error("camera reports OutOfFocus; no automatic AF or shutter retry"){}
};
inline void WaitWpdStillReady(WpdStillCaptureChannel& channel,const std::function<bool()>& may_continue){
    if(!may_continue)throw std::invalid_argument("missing still admission");
        auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        for(;;){if(!may_continue())throw std::runtime_error("still transaction deadline/stop");const auto r=channel.Ready();if(r.Ok())return;
            if(r.error==WpdLiveViewResultError::MtpRejected && r.mtp_response==0xa002)throw WpdStillOutOfFocus();
            if(r.error!=WpdLiveViewResultError::MtpRejected || r.mtp_response!=0x2019 || std::chrono::steady_clock::now()>=until)throw std::runtime_error("still camera not ready");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));}
}
inline WpdStillEventLedger ReadWpdStillPreflight(WpdStillCaptureChannel& channel,const std::function<bool()>& may_continue){
    if(!may_continue)throw std::invalid_argument("missing still preflight admission");
    auto active=[&]{if(!may_continue())throw std::runtime_error("still transaction deadline/stop");};
    auto ready=[&]{WaitWpdStillReady(channel,may_continue);};
    auto validate_profile=[&]{
    auto live=channel.Read(WpdStillOperation::LiveStatus,16);
    if(live.size()!=1 || live[0]!=0)throw std::runtime_error("capture requires Live View off");
    auto quality=channel.Read(WpdStillOperation::ImageQuality,16);
    if((quality.size()!=1 && quality.size()!=2) || StillLittleEndian(quality,0,quality.size())!=2)throw std::runtime_error("capture requires JPEG Fine only; camera settings unchanged");
    if(ParseWpdStillString(channel.Read(WpdStillOperation::ImageSize,128))!=L"7360x4912")throw std::runtime_error("capture requires FX JPEG L; camera settings unchanged");
    };
    active();ready();validate_profile();
    auto drain_preflight=[&](WpdStillEventLedger& baseline){
        active();baseline.Observe(ParseWpdStillEvents(channel.Read(WpdStillOperation::Events,8192)),false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));active();
        baseline.Observe(ParseWpdStillEvents(channel.Read(WpdStillOperation::Events,8192)),false);
    };
    WpdStillEventLedger ledger;drain_preflight(ledger);
    if(ledger.ProfileRefreshRequired()){
        active();ready();validate_profile();
        // One read-only refresh, then a new bounded baseline. Never loop until
        // a changing camera looks stable or carry an old object into capture.
        WpdStillEventLedger settled;drain_preflight(settled);
        if(settled.ProfileRefreshRequired())throw std::runtime_error("camera state did not settle after capture profile refresh");
    }
    active();return ledger;
}
// One camera's capture, split into the steps a pair transaction interleaves
// (#389): Preflight, Shutter, then Collect (or RetainUncertainShutterCandidate
// when the shutter acknowledgement failed). The ledger, object handle and
// observation belong to this run and its channel only, so an event read from one
// body is never credited to the other even if both report the same handle.
class WpdStillCaptureRun {
public:
    WpdStillCaptureRun(WpdStillCaptureChannel& channel,std::function<bool()> may_continue,
        std::function<void(std::span<const unsigned char>)> partial_sink,
        WpdStillCaptureObservation* observation=nullptr,WpdStillCompletionWait wait={}):
        channel_(channel),may_continue_(std::move(may_continue)),partial_sink_(std::move(partial_sink)),observation_(observation),wait_(wait){
        if(!may_continue_ || !partial_sink_)throw std::invalid_argument("missing capture admission/partial sink");
        if(observation_)*observation_={};
    }
    WpdStillCaptureRun(const WpdStillCaptureRun&)=delete;
    WpdStillCaptureRun& operator=(const WpdStillCaptureRun&)=delete;
    // Profile and event baseline. No exposure happens before this passes.
    void Preflight(){ledger_=ReadWpdStillPreflight(channel_,may_continue_);preflighted_=true;Publish();}
    // Hands the single shutter to the transport. Throws if it was refused or
    // its acknowledgement is unconfirmed; the caller must never resend it.
    void Shutter(){
        if(!preflighted_)throw std::logic_error("shutter before preflight");
        Publisher publisher{*this};
        channel_.ShutterOnce();
    }
    // A failed acknowledgement can still follow a real exposure. Observe once
    // and retain an attributable candidate only as a private partial; never
    // resend the shutter or promote it to canonical original.jpg. Best effort:
    // the original uncertain shutter failure stays the terminal result.
    void RetainUncertainShutterCandidate()noexcept{
        if(!preflighted_ || !channel_.CaptureAttempted())return;
        Publisher publisher{*this};
        try{
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while(!ledger_.Handle()){
                Active();ledger_.Observe(ParseWpdStillEvents(channel_.Read(WpdStillOperation::Events,8192)),true);
                if(!ledger_.Handle()){
                    if(std::chrono::steady_clock::now()>=until)break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            if(ledger_.Handle()){
                channel_.AuthorizeObject(ledger_);
                const auto info=ParseWpdStillObjectInfo(channel_.Read(WpdStillOperation::ObjectInfo,4096));
                channel_.Read(WpdStillOperation::Object,info.bytes,partial_sink_);
            }
        }catch(...){}
    }
    // Waits for this body's image, reads it, then observes the completion event
    // (#381) before the caller closes the session.
    WpdStillPayload Collect(){
        if(!preflighted_ || !channel_.CaptureAttempted())throw std::logic_error("collect before shutter");
        if(!channel_.CaptureConfirmed())throw std::runtime_error("unconfirmed shutter cannot publish an original");
        Publisher publisher{*this};
        // A shutter acknowledgement alone does not prove exposure completion.
        // Observe its final readiness/focus result before waiting for an image.
        Ready();
        const auto event_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(!ledger_.Handle()){
            Active();ledger_.Observe(ParseWpdStillEvents(channel_.Read(WpdStillOperation::Events,8192)),true);
            if(!ledger_.Handle()){
                if(std::chrono::steady_clock::now()>=event_deadline)throw std::runtime_error("fresh SDRAM capture event timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        channel_.AuthorizeObject(ledger_);Ready();
        const auto info=ParseWpdStillObjectInfo(channel_.Read(WpdStillOperation::ObjectInfo,4096));
        auto bytes=channel_.Read(WpdStillOperation::Object,info.bytes,partial_sink_);
        if(bytes.size()!=info.bytes)throw std::runtime_error("still image byte count mismatch");
        for(int i=0;i<2;++i){
            Active();ledger_.Observe(ParseWpdStillEvents(channel_.Read(WpdStillOperation::Events,8192)),true);
            if(i==0)std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Active();if(!channel_.CaptureConfirmed())throw std::runtime_error("unconfirmed shutter cannot publish an original");
        AwaitCompletion();
        return {std::move(bytes),info,ledger_.Completed()};
    }
    bool ShutterAttempted()const{return channel_.CaptureAttempted();}
    std::optional<std::chrono::steady_clock::time_point> ShutterDispatchedAt()const{return channel_.ShutterDispatchedAt();}
private:
    struct Publisher {
        WpdStillCaptureRun& run;
        ~Publisher(){run.Publish();}
    };
    void Publish()noexcept{
        if(!observation_ || !preflighted_)return;
        observation_->shutter_attempted=channel_.CaptureAttempted();
        observation_->complete_event=ledger_.Completed();
        observation_->complete_event_code=ledger_.CompletionCode();
        observation_->completion_wait=wait_result_;observation_->completion_wait_ms=wait_ms_;
    }
    void Active()const{if(!may_continue_())throw std::runtime_error("still transaction deadline/stop");}
    void Ready(){WaitWpdStillReady(channel_,may_continue_);}
    // Polls this session's own event queue until its completion arrives, the
    // bound passes, or the transaction stops admitting I/O. An event that the
    // ledger rejects (duplicate completion, storage change) stays terminal. A
    // read that fails only because admission ended is an abort, not a failure:
    // the image is already complete and the caller's deadline check decides.
    void AwaitCompletion(){
        if(ledger_.Completed()){wait_result_=WpdStillCompletionWaitResult::AlreadySeen;wait_ms_=0;return;}
        const auto start=std::chrono::steady_clock::now();
        struct Elapsed {
            WpdStillCaptureRun& run;std::chrono::steady_clock::time_point start;
            ~Elapsed(){run.wait_ms_=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();}
        } elapsed{*this,start};
        wait_result_=WpdStillCompletionWaitResult::Aborted;
        for(;;){
            const auto waited=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start);
            if(!may_continue_())return;
            if(waited>=wait_.max){wait_result_=WpdStillCompletionWaitResult::TimedOut;return;}
            std::this_thread::sleep_for(std::min(wait_.poll,wait_.max-waited));
            if(!may_continue_())return;
            try{ledger_.Observe(ParseWpdStillEvents(channel_.Read(WpdStillOperation::Events,8192)),true);}
            catch(const WpdStillEventRejected&){throw;}
            catch(...){if(may_continue_())throw;return;}
            if(ledger_.Completed()){wait_result_=WpdStillCompletionWaitResult::Seen;return;}
        }
    }
    WpdStillCaptureChannel& channel_;
    std::function<bool()> may_continue_;
    std::function<void(std::span<const unsigned char>)> partial_sink_;
    WpdStillCaptureObservation* observation_{};
    WpdStillCompletionWait wait_;
    WpdStillEventLedger ledger_;
    bool preflighted_{};
    WpdStillCompletionWaitResult wait_result_{};long long wait_ms_{};
};
// Single-camera capture: preflight, one shutter, collect. Pair transactions use
// WpdStillCaptureRun directly to put both shutters before either import.
inline WpdStillPayload AcquireWpdStillPayload(WpdStillCaptureChannel& channel,
    const std::function<bool()>& may_continue,
    const std::function<void(std::span<const unsigned char>)>& partial_sink,
    WpdStillCaptureObservation* observation=nullptr,WpdStillCompletionWait wait={}){
    WpdStillCaptureRun run(channel,may_continue,partial_sink,observation,wait);
    run.Preflight();
    try{run.Shutter();}
    catch(...){
        const auto shutter_error=std::current_exception();
        run.RetainUncertainShutterCandidate();
        std::rethrow_exception(shutter_error);
    }
    return run.Collect();
}
}
