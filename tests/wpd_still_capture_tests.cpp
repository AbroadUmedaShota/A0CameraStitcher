#include "a0/phase0/experimental/wpd_still_capture.hpp"
#include <algorithm>
#include <iostream>
using namespace a0::phase0::experimental;
namespace {
int checks{},failures{};
void Check(bool value){++checks;if(!value)++failures;}
template<class F>void Reject(F f){bool rejected=false;try{f();}catch(...){rejected=true;}Check(rejected);}
void Require(bool value){if(!value)throw std::runtime_error("synthetic still channel contract");}
void Put(std::vector<unsigned char>& data,size_t offset,std::uint32_t value,size_t size){for(size_t i=0;i<size;++i)data[offset+i]=static_cast<unsigned char>(value>>(8*i));}
WpdValues Reply(bool final=false){WpdValues result;Require(CoCreateInstance(CLSID_PortableDeviceValues,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&result))==S_OK);result->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT,S_OK);if(final)result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE,0x2001);return result;}
std::vector<unsigned char> Events(std::initializer_list<WpdStillEvent> events){std::vector<unsigned char> data(2+6*events.size());Put(data,0,static_cast<unsigned>(events.size()),2);size_t i=0;for(auto e:events){Put(data,2+6*i,e.code,2);Put(data,4+6*i,e.parameter,4);++i;}return data;}
std::vector<unsigned char> Info(size_t bytes){std::vector<unsigned char> data(52);Put(data,4,0x3801,2);Put(data,8,static_cast<unsigned>(bytes),4);Put(data,26,7360,4);Put(data,30,4912,4);return data;}
std::vector<unsigned char> Text(std::wstring_view text){std::vector<unsigned char> data(1+2*(text.size()+1));data[0]=static_cast<unsigned char>(text.size()+1);for(size_t i=0;i<text.size();++i)Put(data,1+2*i,text[i],2);return data;}
class Fake final:public WpdStillCaptureChannel {
public:
    Fake():WpdStillCaptureChannel([&]{return admitted;},[&]{return owned;}){}
    bool admitted=true,owned=true,pending=false,ambiguous=false,bad_quality=false,reject_object_end=false,reject_shutter=false,metadata_before=false,metadata_after=false;
    bool out_of_focus_after_shutter=false,out_of_focus_before_shutter=false;
    DWORD preflight_event{},postflight_event{},preflight_companion{};
    int preflight_event_on_read{};bool repeat_preflight_event{},bad_quality_after_refresh{},late_pending{};
    int preflight_reads{};
    int shutters{},object_reads{},ends{},profile_reads{};bool event_sent{},metadata_sent{};DWORD current_operation{};size_t offset{};
    std::vector<unsigned char> jpeg{0xff,0xd8,7,8,9,10,0xff,0xd9},data;
protected:
    WpdLiveViewExchange Exchange(IPortableDeviceValues* request) override {
        DWORD id{},code{};request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,&id);
        request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&code);
        if(id==WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE.pid){
            if(code==0x90c0)++shutters;else Require(code==0x90c8);auto result=Reply(true);if(code==0x90c0 && reject_shutter)result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE,0xa004);
            if(code==0x90c8 && ((shutters && out_of_focus_after_shutter) || (!shutters && out_of_focus_before_shutter)))result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE,0xa002);return {S_OK,result};
        }
        if(id==WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ.pid){
            current_operation=code;offset=0;
            Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&args);
            PROPVARIANT v{};DWORD count{};args->GetCount(&count);if(count)args->GetAt(0,&v);DWORD parameter=v.ulVal;PropVariantClear(&v);
            if(code==0x1015){if(parameter==0xd1a2){++profile_reads;data={0};}else if(parameter==0x5004)data={static_cast<unsigned char>((bad_quality || (bad_quality_after_refresh && profile_reads>1))?4:2)};else{Require(parameter==0x5003);data=Text(L"7360x4912");}}
            else if(code==0x90c7){
                const int preflight_read=preflight_reads;
                if(!shutters)++preflight_reads;
                if((pending || (late_pending && profile_reads>1)) && !shutters)data=Events({{0xc101,123}});
                else if(preflight_event && !shutters && (repeat_preflight_event || preflight_read==preflight_event_on_read)){
                    const auto event=static_cast<std::uint16_t>(preflight_event);
                    data=preflight_companion?Events({{event,0},{static_cast<std::uint16_t>(preflight_companion),123}}):Events({{event,0}});
                }
                else if(metadata_before && !shutters && !metadata_sent){metadata_sent=true;data=Events({{0x4008,0},{0xc10c,0}});}
                else if(shutters && !event_sent && postflight_event){event_sent=true;data=Events({{static_cast<std::uint16_t>(postflight_event),0},{0xc101,123}});}
                else if(shutters && !event_sent){event_sent=true;data=metadata_after?Events({{0x4008,0},{0xc101,123}}):(ambiguous?Events({{0xc101,123},{0xc101,124}}):Events({{0xc101,123},{0xc102,0}}));}
                else data=Events({});
            }else if(code==0x1008){Require(parameter==123);data=Info(jpeg.size());}
            else {Require(code==0x1009 && parameter==123);++object_reads;data=jpeg;}
            auto result=Reply();result->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT,L"synthetic-still-context");
            result->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE,data.size());result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE,3);
            return {S_OK,result};
        }
        if(id==WPD_COMMAND_MTP_EXT_READ_DATA.pid){
            DWORD requested{};request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ,&requested);
            auto count=std::min<size_t>(requested,data.size()-offset);auto result=Reply();result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ,static_cast<DWORD>(count));
            result->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA,data.data()+offset,static_cast<DWORD>(count));offset+=count;return {S_OK,result};
        }
        Require(id==WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER.pid);++ends;auto result=Reply(true);
        if(reject_object_end && current_operation==0x1009)result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE,0x2019);
        return {S_OK,result};
    }
};
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
            Reject([&]{ReadWpdStillPreflight(f,[]{return true;});});Check(f.shutters==0 && f.object_reads==0);}
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
        for(DWORD event:{0x4004u,0x4005u,0x400bu,0x4fffu}){Fake f;f.postflight_event=event;
            Reject([&]{AcquireWpdStillPayload(f,[]{return true;},[](auto){});});Check(f.shutters==1 && f.object_reads==0);}
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
