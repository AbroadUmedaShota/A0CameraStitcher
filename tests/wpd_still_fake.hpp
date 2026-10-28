#pragma once
// Synthetic WPD still channel shared by the still-capture and pair contracts.
// No camera, no device: it answers the allowlisted still operations from memory.
#include "a0/phase0/experimental/wpd_still_capture.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
namespace {
using namespace a0::phase0::experimental;
void Require(bool value){if(!value)throw std::runtime_error("synthetic still channel contract");}
void Put(std::vector<unsigned char>& data,size_t offset,std::uint32_t value,size_t size){for(size_t i=0;i<size;++i)data[offset+i]=static_cast<unsigned char>(value>>(8*i));}
WpdValues Reply(bool final=false){WpdValues result;Require(CoCreateInstance(CLSID_PortableDeviceValues,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&result))==S_OK);result->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT,S_OK);if(final)result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE,0x2001);return result;}
std::vector<unsigned char> EventsOf(const std::vector<WpdStillEvent>& events){std::vector<unsigned char> data(2+6*events.size());Put(data,0,static_cast<unsigned>(events.size()),2);size_t i=0;for(auto e:events){Put(data,2+6*i,e.code,2);Put(data,4+6*i,e.parameter,4);++i;}return data;}
std::vector<unsigned char> Events(std::initializer_list<WpdStillEvent> events){return EventsOf(std::vector<WpdStillEvent>(events));}
std::vector<unsigned char> Info(size_t bytes){std::vector<unsigned char> data(52);Put(data,4,0x3801,2);Put(data,8,static_cast<unsigned>(bytes),4);Put(data,26,7360,4);Put(data,30,4912,4);return data;}
std::vector<unsigned char> Text(std::wstring_view text){std::vector<unsigned char> data(1+2*(text.size()+1));data[0]=static_cast<unsigned char>(text.size()+1);for(size_t i=0;i<text.size();++i)Put(data,1+2*i,text[i],2);return data;}
class Fake final:public WpdStillCaptureChannel {
public:
    Fake():WpdStillCaptureChannel([&]{return admission?admission():admitted;},[&]{return owned;}){}
    bool admitted=true,owned=true,pending=false,ambiguous=false,bad_quality=false,reject_object_end=false,reject_shutter=false,metadata_before=false,metadata_after=false;
    bool out_of_focus_after_shutter=false,out_of_focus_before_shutter=false;
    DWORD preflight_event{},postflight_event{},preflight_companion{};
    int preflight_event_on_read{};bool repeat_preflight_event{},bad_quality_after_refresh{},late_pending{};
    // #381: the camera's event queue outlives one session. A late completion
    // is queued there and only a later session's preflight 0x90C7 reads it.
    std::vector<WpdStillEvent>* camera_queue{};bool completion_late{},duplicate_completion{},duplicate_sent{};
    int preflight_reads{};
    int shutters{},object_reads{},ends{},profile_reads{};bool event_sent{},metadata_sent{};DWORD current_operation{};size_t offset{};
    std::vector<unsigned char> jpeg{0xff,0xd8,7,8,9,10,0xff,0xd9},data;
    // #389: one trace shared by both synthetic bodies, "<tag>:<opcode>[(<arg>)]"
    // per operation, to check ordering and the sent command set.
    char tag{'-'};std::vector<std::string>* trace{};
    // Replaces `admitted` when set (models deadline/stop admission per body).
    std::function<bool()> admission;
    // Runs when the shutter command reaches the transport, before it answers.
    std::function<void()> on_shutter;
    DWORD handle{123};
    // Scripted batches for the post-shutter 0x90C7 reads, in order; empty after.
    std::vector<std::vector<WpdStillEvent>> post_script;bool use_script{};
    size_t post_reads{}; // Post-shutter event reads so far.
protected:
    WpdLiveViewExchange Exchange(IPortableDeviceValues* request) override {
        DWORD id{},code{};request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,&id);
        request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&code);
        DWORD parameter{};bool has_parameter=false;
        {Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;DWORD count{};
            if(request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&args)==S_OK && args && args->GetCount(&count)==S_OK && count){
                PROPVARIANT v{};if(args->GetAt(0,&v)==S_OK){parameter=v.ulVal;has_parameter=true;}PropVariantClear(&v);}}
        const bool operation=id==WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE.pid || id==WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ.pid;
        if(trace && operation){char text[32];std::snprintf(text,sizeof text,"%c:%04x",tag,static_cast<unsigned>(code));std::string entry=text;
            if(has_parameter){std::snprintf(text,sizeof text,"(%08x)",static_cast<unsigned>(parameter));entry+=text;}trace->push_back(entry);}
        if(id==WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE.pid){
            if(code==0x90c0){++shutters;if(on_shutter)on_shutter();}else Require(code==0x90c8);auto result=Reply(true);if(code==0x90c0 && reject_shutter)result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE,0xa004);
            if(code==0x90c8 && ((shutters && out_of_focus_after_shutter) || (!shutters && out_of_focus_before_shutter)))result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE,0xa002);return {S_OK,result};
        }
        if(id==WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ.pid){
            current_operation=code;offset=0;
            if(code==0x1015){if(parameter==0xd1a2){++profile_reads;data={0};}else if(parameter==0x5004)data={static_cast<unsigned char>((bad_quality || (bad_quality_after_refresh && profile_reads>1))?4:2)};else{Require(parameter==0x5003);data=Text(L"7360x4912");}}
            else if(code==0x90c7){
                const int preflight_read=preflight_reads;
                const size_t post_index=shutters?post_reads++:0;
                if(!shutters)++preflight_reads;
                if(shutters && use_script)data=post_index<post_script.size()?EventsOf(post_script[post_index]):Events({});
                else if(camera_queue && !camera_queue->empty() && !shutters){data=EventsOf(*camera_queue);camera_queue->clear();}
                else if((pending || (late_pending && profile_reads>1)) && !shutters)data=Events({{0xc101,handle}});
                else if(preflight_event && !shutters && (repeat_preflight_event || preflight_read==preflight_event_on_read)){
                    const auto event=static_cast<std::uint16_t>(preflight_event);
                    data=preflight_companion?Events({{event,0},{static_cast<std::uint16_t>(preflight_companion),handle}}):Events({{event,0}});
                }
                else if(metadata_before && !shutters && !metadata_sent){metadata_sent=true;data=Events({{0x4008,0},{0xc10c,0}});}
                else if(shutters && !event_sent && completion_late){event_sent=true;data=Events({{0xc101,handle}});if(camera_queue)camera_queue->push_back({0xc102,0});}
                else if(shutters && event_sent && duplicate_completion && !duplicate_sent){duplicate_sent=true;data=Events({{0x400d,0}});}
                else if(shutters && !event_sent && postflight_event){event_sent=true;data=Events({{static_cast<std::uint16_t>(postflight_event),0},{0xc101,handle}});}
                else if(shutters && !event_sent){event_sent=true;data=metadata_after?Events({{0x4008,0},{0xc101,handle}}):(ambiguous?Events({{0xc101,handle},{0xc101,handle+1}}):Events({{0xc101,handle},{0xc102,0}}));}
                else data=Events({});
            }else if(code==0x1008){Require(parameter==handle);data=Info(jpeg.size());}
            else {Require(code==0x1009 && parameter==handle);++object_reads;data=jpeg;}
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
