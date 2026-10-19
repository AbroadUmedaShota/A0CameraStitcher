#include "a0/phase0/experimental/d810_preview.hpp"
#include "a0/phase0/experimental/preview_end_restart_probe.hpp"
#include "a0/phase0/experimental/preview_focus.hpp"
#include "a0/phase0/experimental/preview_lifecycle.hpp"
#include <iostream>
#include <chrono>
using namespace a0::phase0::experimental;
int main() {
    if (FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED))) return 2;
    int failures=0,checks=0;
    auto check=[&](bool value){++checks; if(!value)++failures;};
    check(ContinuePreviewFrame(false,false,false));
    check(!ContinuePreviewFrame(true,false,false));
    check(ContinuePreviewFrame(true,true,false));
    check(!ContinuePreviewFrame(true,true,true));
    check(!ContinuePreviewFrame(false,true,true));
    for(auto op:{WpdLiveViewOperation::Start,WpdLiveViewOperation::Frame,WpdLiveViewOperation::Stop}) {
        auto request=BuildWpdLiveViewOperation(op); check(AllowedD810PreviewRequest(request.Get()));
        request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,0x9207);
        check(!AllowedD810PreviewRequest(request.Get()));
    }
    check(AllowedD810PreviewRequest(BuildWpdLiveViewRead(L"test",16).Get()));
    check(AllowedD810PreviewRequest(BuildWpdLiveViewEndTransfer(L"test").Get()));
    auto wrong=BuildWpdLiveViewOperation(WpdLiveViewOperation::Start);
    wrong->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE.pid);
    check(!AllowedD810PreviewRequest(wrong.Get()));
    check(!AllowedD810PreviewRequest(nullptr));
    for(DWORD mode:{0u,1u}){
        auto request=BuildWpdLiveViewOperation(WpdLiveViewOperation::Start);
        request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,0x90c2);
        Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;
        request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&args);
        PROPVARIANT value{};value.vt=VT_UI4;value.ulVal=mode;args->Add(&value);
        check(!AllowedD810PreviewRequest(request.Get()));
        check(!AdmitD810PreviewDispatch(request.Get(),true,false,false));
    }
    for(auto action:{PreviewFocusAction::AutoFocus,PreviewFocusAction::Near,PreviewFocusAction::Far}){
        auto request=BuildPreviewFocusRequest(action);check(AllowedD810PreviewRequest(request.Get()));
        DWORD code{};request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&code);
        check(code==(action==PreviewFocusAction::AutoFocus?0x90c1u:0x9204u));
        Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;
        request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&args);
        DWORD count{};args->GetCount(&count);check(count==(action==PreviewFocusAction::AutoFocus?0u:2u));
        if(count==2){
            PROPVARIANT v{};args->GetAt(0,&v);check(v.vt==VT_UI4 && v.ulVal==(action==PreviewFocusAction::Near?1u:2u));PropVariantClear(&v);
            args->GetAt(1,&v);check(v.vt==VT_UI4 && v.ulVal==10);PropVariantClear(&v);
            args->Clear();v.vt=VT_UI4;v.ulVal=1;args->Add(&v);v.ulVal=500;args->Add(&v);
            check(!AllowedD810PreviewRequest(request.Get()));
        }
    }
    check(AllowedD810PreviewRequest(BuildPreviewDeviceReadyRequest().Get()));
    for(DWORD forbidden:{0x100eu,0x90c0u,0x90c2u,0x90c7u,0x90cbu,0x9205u,0x9207u}){
        auto request=BuildPreviewFocusRequest(PreviewFocusAction::AutoFocus);
        request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,forbidden);
        check(!AllowedD810PreviewRequest(request.Get()));
    }
    for(DWORD bad_direction:{0u,3u}){
        auto request=BuildPreviewFocusRequest(PreviewFocusAction::Near);
        Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;
        request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&args);args->Clear();
        PROPVARIANT v{};v.vt=VT_UI4;v.ulVal=bad_direction;args->Add(&v);v.ulVal=10;args->Add(&v);
        check(!AllowedD810PreviewRequest(request.Get()));
    }
    bool invalid_action=false;try{BuildPreviewFocusRequest(static_cast<PreviewFocusAction>(10));}catch(...){invalid_action=true;}check(invalid_action);
    // Feature gap: AF-only and bounded focus drive are separate from capture.
    {
        auto af=BuildWpdLiveViewOperation(WpdLiveViewOperation::Start);
        af->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,0x90c1);
        check(AllowedD810PreviewRequest(af.Get()));
        for(DWORD direction:{1u,2u}) {
            auto mf=BuildWpdLiveViewOperation(WpdLiveViewOperation::Start);
            mf->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,0x9204);
            Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;
            mf->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&args);
            PROPVARIANT v{};v.vt=VT_UI4;v.ulVal=direction;args->Add(&v);v.ulVal=10;args->Add(&v);
            check(AllowedD810PreviewRequest(mf.Get()));
        }
    }
    for(auto property:{0xd1a2u,0xd1a4u,0xd061u})check(AllowedD810PreviewRequest(D810PreviewStatusRequest(property).Get()));
    check(D810PreviewPropertyBytes(0x5001)==1);
    check(D810PreviewPropertyBytes(0x500a)==2);
    check(D810PreviewPropertyBytes(0x500d)==4);
    check(D810PreviewPropertyBytes(0x5010)==0);
    for(auto property:{0x5001u,0x500au,0x500du})check(AllowedD810PreviewRequest(D810PreviewStatusRequest(property).Get()));
    // Extended fault diagnostics read established scalar properties only;
    // their identifiers must never admit a property write.
    for(auto property:{0xd1b2u,0xd1f0u,0xd1a6u,0xd10bu}){
        check(D810PreviewPropertyBytes(property)==1);
        try{
            auto request=D810PreviewStatusRequest(property);check(AllowedD810PreviewRequest(request.Get()));
            request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,0x1016);
            check(!AllowedD810PreviewRequest(request.Get()));
        }catch(...){check(false);}
    }
    check(D810PreviewPropertyBytes(0x500e)==2);
    check(AllowedD810PreviewRequest(D810PreviewStatusRequest(0x500e).Get()));
    for(auto action:{PreviewFocusAction::AutoFocus,PreviewFocusAction::Near,PreviewFocusAction::Far}){
        auto request=BuildPreviewFocusRequest(action);
        check(AdmitD810PreviewDispatch(request.Get(),true,false,false));
        check(!AdmitD810PreviewDispatch(request.Get(),true,true,false));
        check(!AdmitD810PreviewDispatch(request.Get(),true,false,true));
        check(!AdmitD810PreviewDispatch(request.Get(),false,false,false));
    }
    for(auto request:{BuildWpdLiveViewOperation(WpdLiveViewOperation::Stop),BuildWpdLiveViewEndTransfer(L"test")}){
        check(AdmitD810PreviewDispatch(request.Get(),true,true,true));
        check(!AdmitD810PreviewDispatch(request.Get(),false,true,true));
    }
    check(!AdmitD810PreviewDispatch(BuildWpdLiveViewOperation(WpdLiveViewOperation::Frame).Get(),true,true,false));
    check(!AdmitD810PreviewDispatch(BuildWpdLiveViewRead(L"test",16).Get(),true,false,true));
    auto altered=D810PreviewStatusRequest(0xd1a4);
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> parameters;
    altered->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&parameters);
    parameters->Clear();PROPVARIANT property{};property.vt=VT_UI4;property.ulVal=0x5010;parameters->Add(&property);
    check(!AllowedD810PreviewRequest(altered.Get()));
    property.ulVal=0xd1a4;parameters->Add(&property);check(!AllowedD810PreviewRequest(altered.Get()));
    parameters.Reset();altered.Reset();
    bool bad_property=false;try{D810PreviewStatusRequest(0x5010);}catch(...){bad_property=true;}check(bad_property);
    std::vector<unsigned char> data(390,0); data[384]=0xff; data[385]=0xd8;data[388]=0xff;data[389]=0xd9;
    check(D810PreviewJpeg(data).size()==6);
    // Public D810 ancestor driver reads signed big-endian degree words at
    // 52/56, not little-endian words or a guessed four-byte fixed-point value.
    data[52]=0;data[53]=12;data[54]=0x81;data[55]=0x23;
    data[56]=0xff;data[57]=0xf9;data[58]=0x45;data[59]=0x67;
    auto level=D810PreviewLevel(data);
    check(level && level->roll_degrees==12 && level->pitch_degrees==-7);
    auto zero=data;zero[53]=0;zero[56]=0;zero[57]=0;
    auto zero_level=D810PreviewLevel(zero);
    check(zero_level && zero_level->roll_degrees==0 && zero_level->pitch_degrees==0);
    auto edge=data;edge[52]=0xff;edge[53]=0x4c;edge[56]=0;edge[57]=180;
    auto edge_level=D810PreviewLevel(edge);
    check(edge_level && edge_level->roll_degrees==-180 && edge_level->pitch_degrees==180);
    auto wrapped=data;wrapped[52]=1;wrapped[53]=0x66;wrapped[56]=1;wrapped[57]=0x67;
    auto wrapped_level=D810PreviewLevel(wrapped);
    check(wrapped_level && wrapped_level->roll_degrees==-2 && wrapped_level->pitch_degrees==-1);
    wrapped[52]=0;wrapped[53]=181;wrapped[56]=0xff;wrapped[57]=0x4b;
    wrapped_level=D810PreviewLevel(wrapped);
    check(wrapped_level && wrapped_level->roll_degrees==-179 && wrapped_level->pitch_degrees==179);
    wrapped[52]=1;wrapped[53]=0x68;wrapped[56]=0xfe;wrapped[57]=0x98;
    wrapped_level=D810PreviewLevel(wrapped);
    check(wrapped_level && wrapped_level->roll_degrees==0 && wrapped_level->pitch_degrees==0);
    for(auto word:{0x0169u,0xfe97u,0x7fffu,0x8000u,0xffffu}){
        auto bad=data;bad[52]=static_cast<unsigned char>(word>>8);bad[53]=static_cast<unsigned char>(word);
        // -1 is a real signed value, not an assumed sensor sentinel.
        check(D810PreviewLevel(bad).has_value()==(word==0xffff));
        bad=data;bad[56]=static_cast<unsigned char>(word>>8);bad[57]=static_cast<unsigned char>(word);
        check(D810PreviewLevel(bad).has_value()==(word==0xffff));
    }
    check(!D810PreviewLevel(std::span<const unsigned char>(data.data(),57)));
    auto bad_level_envelope=data;bad_level_envelope[384]=0;
    check(!D810PreviewLevel(bad_level_envelope));
    using LevelClock=std::chrono::steady_clock;
    const auto timestamp=LevelClock::time_point{}+std::chrono::seconds(10);
    check(D810LevelVisible(level,true,timestamp,timestamp));
    check(D810LevelVisible(level,true,timestamp,timestamp+std::chrono::milliseconds(1999)));
    check(!D810LevelVisible(level,true,timestamp,timestamp+std::chrono::seconds(2)));
    check(!D810LevelVisible(level,true,timestamp,timestamp-std::chrono::milliseconds(1)));
    check(!D810LevelVisible(level,false,timestamp,timestamp));
    check(!D810LevelVisible({},true,timestamp,timestamp));
    WpdLiveViewTransferResult ended;
    ended.error=WpdLiveViewTransferError::BeginRejected;ended.end_attempted=true;
    ended.end_result=WpdLiveViewCommandResult{WpdLiveViewResultError::MtpRejected,S_OK,S_OK,0xa00b};
    check(PreviewCameraEnded(ended));
    ended.end_attempted=false;check(!PreviewCameraEnded(ended));ended.end_attempted=true;
    ended.error=WpdLiveViewTransferError::None;check(!PreviewCameraEnded(ended));ended.error=WpdLiveViewTransferError::BeginRejected;
    ended.end_result->dispatch_status=E_FAIL;check(!PreviewCameraEnded(ended));
    ended.end_result->dispatch_status=S_OK;ended.end_result->command_status=E_FAIL;check(!PreviewCameraEnded(ended));
    ended.end_result->command_status=S_OK;ended.end_result->error=WpdLiveViewResultError::InvalidTransferSize;check(!PreviewCameraEnded(ended));
    ended.end_result.reset();ended.command_failure=WpdLiveViewCommandResult{WpdLiveViewResultError::MtpRejected,S_OK,S_OK,0xa00b};
    check(PreviewCameraEnded(ended));ended.command_failure->mtp_response=0x2019;check(!PreviewCameraEnded(ended));
    WpdLiveViewTransferResult lost;lost.error=WpdLiveViewTransferError::BeginRejected;
    lost.command_failure=WpdLiveViewCommandResult{WpdLiveViewResultError::DispatchFailed,E_WPD_DEVICE_NOT_OPEN,E_PENDING,0};
    check(PreviewSessionUnavailable(lost));lost.command_failure->dispatch_status=E_FAIL;check(!PreviewSessionUnavailable(lost));
    lost.command_failure=WpdLiveViewCommandResult{WpdLiveViewResultError::CommandFailed,S_OK,E_WPD_DEVICE_NOT_OPEN,0};
    check(PreviewSessionUnavailable(lost));lost.error=WpdLiveViewTransferError::None;check(!PreviewSessionUnavailable(lost));
    lost.error=WpdLiveViewTransferError::BeginRejected;lost.end_result=lost.command_failure;lost.command_failure.reset();
    check(!PreviewSessionUnavailable(lost));lost.end_attempted=true;check(PreviewSessionUnavailable(lost));
    lost.end_result->error=WpdLiveViewResultError::MtpRejected;check(!PreviewSessionUnavailable(lost));
    // Observed WPD command HRESULT: Windows reports the device unavailable,
    // without proving physical USB removal, battery loss, or camera timeout.
    WpdLiveViewTransferResult missing;missing.error=WpdLiveViewTransferError::BeginRejected;
    const auto no_device=HRESULT_FROM_WIN32(ERROR_NO_SUCH_DEVICE);
    missing.command_failure=WpdLiveViewCommandResult{WpdLiveViewResultError::CommandFailed,S_OK,no_device,0};
    check(PreviewDeviceUnavailable(missing));check(!PreviewCameraEnded(missing));check(!PreviewSessionUnavailable(missing));
    missing.command_failure->command_status=E_FAIL;check(!PreviewDeviceUnavailable(missing));
    missing.command_failure=WpdLiveViewCommandResult{WpdLiveViewResultError::DispatchFailed,no_device,E_PENDING,0};
    check(PreviewDeviceUnavailable(missing));
    missing.command_failure->error=WpdLiveViewResultError::MtpRejected;check(!PreviewDeviceUnavailable(missing));
    missing.command_failure=WpdLiveViewCommandResult{WpdLiveViewResultError::CommandFailed,E_FAIL,no_device,0};
    check(!PreviewDeviceUnavailable(missing));
    missing.command_failure->dispatch_status=S_OK;missing.error=WpdLiveViewTransferError::None;
    check(!PreviewDeviceUnavailable(missing));missing.error=WpdLiveViewTransferError::BeginRejected;
    missing.end_result=missing.command_failure;missing.command_failure.reset();
    check(!PreviewDeviceUnavailable(missing));missing.end_attempted=true;check(PreviewDeviceUnavailable(missing));
    missing.end_result->command_status=E_WPD_DEVICE_NOT_OPEN;check(!PreviewDeviceUnavailable(missing));
    check(PreviewControlReady(WpdLiveViewOperation::Start,0x2001));check(PreviewControlReady(WpdLiveViewOperation::Stop,0x2001));
    check(!PreviewControlReady(WpdLiveViewOperation::Start,0xa002));check(PreviewControlReady(WpdLiveViewOperation::Stop,0xa002));
    for(auto response:{0x2019u,0xa00bu,0u}){check(!PreviewControlReady(WpdLiveViewOperation::Start,response));check(!PreviewControlReady(WpdLiveViewOperation::Stop,response));}
    check(!PreviewControlReady(WpdLiveViewOperation::Frame,0x2001));
    std::array<PreviewClosedState,2> closed{{{true,true,true,true},{true,true,true,true}}};
    check(PreviewRestartAllowed(true,false,false,false,closed));
    check(!PreviewRestartAllowed(false,false,false,false,closed));
    check(!PreviewRestartAllowed(true,true,false,false,closed));
    check(!PreviewRestartAllowed(true,false,true,false,closed));
    check(!PreviewRestartAllowed(true,false,false,true,closed));
    for(int index:{0,1}){
        auto unsafe=closed;unsafe[index].closed=false;check(!PreviewRestartAllowed(true,false,false,false,unsafe));
        unsafe=closed;unsafe[index].stopped=false;check(!PreviewRestartAllowed(true,false,false,false,unsafe));
        // Preflight refused before any Start still requires closing its Open.
        unsafe=closed;unsafe[index]={true,false,false,true};check(PreviewRestartAllowed(true,false,false,false,unsafe));
        unsafe[index]={false,false,false,false};check(PreviewRestartAllowed(true,false,false,false,unsafe));
        unsafe[index]={false,true,false,false};check(!PreviewRestartAllowed(true,false,false,false,unsafe));
    }
    for(auto index:{0u,384u,385u,388u,389u}) {
        auto bad=data; bad[index]=0;
        bool rejected=false;try{D810PreviewJpeg(bad);}catch(...){rejected=true;}
        check(index==0 ? !rejected : rejected);
    }
    bool rejected=false;try{D810PreviewJpeg(std::span<const unsigned char>{});}catch(...){rejected=true;}
    check(rejected);
    PreviewEndRestartProbe end_probe;
    PreviewEndRestartObservation end_view{};end_view.sessions=1;
    check(end_probe.Next(end_view)==PreviewEndRestartAction::Wait); // Elapsed time alone never restarts.
    end_view.camera_ended=true;check(end_probe.Next(end_view)==PreviewEndRestartAction::Wait);
    end_view.joined=true;check(end_probe.Next(end_view)==PreviewEndRestartAction::Wait);
    end_view.restart_allowed=true;end_view.merge_paints=20;
    check(end_probe.Next(end_view)==PreviewEndRestartAction::Restart);
    check(end_probe.Next(end_view)==PreviewEndRestartAction::Wait); // No duplicate Start.
    end_view.sessions=2;end_view.camera_ended=false;end_view.joined=false;end_view.restart_allowed=false;
    end_view.live_fresh=true;end_view.merge_fresh=true;check(end_probe.Next(end_view)==PreviewEndRestartAction::Wait);
    end_view.merge_paints=25;check(end_probe.Next(end_view)==PreviewEndRestartAction::Complete);
    check(end_probe.Completed() && end_probe.Next(end_view)==PreviewEndRestartAction::Complete);
    for(int stage:{0,1}){PreviewEndRestartProbe failed;PreviewEndRestartObservation state{};state.sessions=1;
        if(stage){state.camera_ended=true;state.joined=true;state.restart_allowed=true;check(failed.Next(state)==PreviewEndRestartAction::Restart);state.sessions=2;}
        state.unexpected_failure=true;check(failed.Next(state)==PreviewEndRestartAction::Fail);
        state.unexpected_failure=false;check(failed.Next(state)==PreviewEndRestartAction::Fail);}
    wrong.Reset();
    CoUninitialize(); std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
