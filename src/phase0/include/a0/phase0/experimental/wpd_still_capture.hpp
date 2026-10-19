#pragma once
#include "a0/phase0/experimental/wpd_live_view_transfer.hpp"
#include <PortableDevice.h>
#include <WpdMtpExtensions.h>
#include <optional>
#include <span>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <exception>

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
class WpdStillEventLedger final {
public:
    void Observe(std::span<const WpdStillEvent> events,bool after_shutter){
        for(const auto& event:events){
            if(event.code==0x4006 || event.code==0xc104)continue; // property/obsolete event, no image ownership.
            if(event.code==0x4004 || event.code==0x4008 || event.code==0xc10c){
                // StoreAdded invalidates preflight; its parameter is not an
                // image handle. After shutter, any such change is terminal.
                if(after_shutter)throw std::runtime_error("camera storage/metadata/live state changed during capture");
                refresh_required_=true;continue; // Never grants image ownership.
            }
            if(event.code==0xc101){
                if(!after_shutter || handle_ || !ValidStillHandle(event.parameter))throw std::runtime_error("pending or ambiguous SDRAM object");
                handle_=event.parameter;
            }else if(event.code==0xc102 || event.code==0x400d){
                if(!after_shutter || completed_)throw std::runtime_error("pending or duplicate capture completion");
                completed_=true;
            }else if(event.code==0x4002)throw std::runtime_error("unexpected card image event");
            else throw std::runtime_error("unclassified capture event code="+std::to_string(event.code)+" before_shutter="+std::to_string(!after_shutter));
        }
    }
    std::optional<DWORD> Handle()const{return handle_;}
    bool Completed()const{return completed_;}
    bool ProfileRefreshRequired()const{return refresh_required_;}
private:
    std::optional<DWORD> handle_;bool completed_{},refresh_required_{};
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
        if(code==0x90c0){capture_armed_=false;capture_attempted_=true;}
        return Exchange(request);
    }
    std::function<bool()> may_continue_,may_cleanup_;
    bool capture_armed_{},capture_attempted_{},capture_confirmed_{};
    std::optional<DWORD> attributable_handle_;
};
struct WpdStillPayload {std::vector<unsigned char> jpeg;WpdStillObjectInfo info;bool complete_event{};};
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
inline WpdStillPayload AcquireWpdStillPayload(WpdStillCaptureChannel& channel,
    const std::function<bool()>& may_continue,
    const std::function<void(std::span<const unsigned char>)>& partial_sink){
    if(!may_continue || !partial_sink)throw std::invalid_argument("missing capture admission/partial sink");
    auto active=[&]{if(!may_continue())throw std::runtime_error("still transaction deadline/stop");};
    auto ready=[&]{WaitWpdStillReady(channel,may_continue);};
    auto ledger=ReadWpdStillPreflight(channel,may_continue);
    try{channel.ShutterOnce();}
    catch(...){
        const auto shutter_error=std::current_exception();
        // A failed acknowledgement can still follow a real exposure. Observe
        // once and retain an attributable candidate only as a private partial;
        // never resend the shutter or promote it to canonical original.jpg.
        if(channel.CaptureAttempted())try{
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while(!ledger.Handle()){
                active();ledger.Observe(ParseWpdStillEvents(channel.Read(WpdStillOperation::Events,8192)),true);
                if(!ledger.Handle()){
                    if(std::chrono::steady_clock::now()>=until)break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            if(ledger.Handle()){
                channel.AuthorizeObject(ledger);
                const auto info=ParseWpdStillObjectInfo(channel.Read(WpdStillOperation::ObjectInfo,4096));
                channel.Read(WpdStillOperation::Object,info.bytes,partial_sink);
            }
        }catch(...){} // Original uncertain shutter remains terminal, partial retained.
        std::rethrow_exception(shutter_error);
    }
    // A shutter acknowledgement alone does not prove exposure completion.
    // Observe its final readiness/focus result before waiting for an image.
    ready();
    const auto event_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(!ledger.Handle()){
        active();ledger.Observe(ParseWpdStillEvents(channel.Read(WpdStillOperation::Events,8192)),true);
        if(!ledger.Handle()){
            if(std::chrono::steady_clock::now()>=event_deadline)throw std::runtime_error("fresh SDRAM capture event timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    channel.AuthorizeObject(ledger);ready();
    const auto info=ParseWpdStillObjectInfo(channel.Read(WpdStillOperation::ObjectInfo,4096));
    auto bytes=channel.Read(WpdStillOperation::Object,info.bytes,partial_sink);
    if(bytes.size()!=info.bytes)throw std::runtime_error("still image byte count mismatch");
    for(int i=0;i<2;++i){
        active();ledger.Observe(ParseWpdStillEvents(channel.Read(WpdStillOperation::Events,8192)),true);
        if(i==0)std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    active();if(!channel.CaptureConfirmed())throw std::runtime_error("unconfirmed shutter cannot publish an original");
    return {std::move(bytes),info,ledger.Completed()};
}
}
