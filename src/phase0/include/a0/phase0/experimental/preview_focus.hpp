#pragma once
#include "a0/phase0/experimental/wpd_live_view_codec.hpp"
#include <PortableDevice.h>
#include <WpdMtpExtensions.h>
#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace a0::phase0::experimental {
enum class PreviewFocusAction { AutoFocus, Near, Far };
enum class PreviewFocusAdmission { Allow, NeedsAfSwitch, NeedsAfS, UnsupportedMode };
inline PreviewFocusAdmission AdmitPreviewFocusMode(PreviewFocusAction action,DWORD mode){
    if(action!=PreviewFocusAction::AutoFocus && action!=PreviewFocusAction::Near && action!=PreviewFocusAction::Far)
        return PreviewFocusAdmission::UnsupportedMode;
    if(mode==3 || mode==4)return PreviewFocusAdmission::NeedsAfSwitch;
    if(mode==0)return PreviewFocusAdmission::Allow;
    if(mode==2)return action==PreviewFocusAction::AutoFocus?PreviewFocusAdmission::Allow:PreviewFocusAdmission::NeedsAfS;
    return PreviewFocusAdmission::UnsupportedMode;
}
inline constexpr DWORD PreviewFineFocusSteps=10;
inline bool ValidPreviewFocusAction(PreviewFocusAction action){
    return action==PreviewFocusAction::AutoFocus || action==PreviewFocusAction::Near || action==PreviewFocusAction::Far;
}
// Numbers/directions from public MIT NikonBase at the fixed commit in NOTICE.
// AF-only is NOT the AF-and-capture operation. Fine drive has one fixed small step.
inline WpdValues BuildPreviewFocusRequest(PreviewFocusAction action){
    if(!ValidPreviewFocusAction(action))throw std::invalid_argument("invalid focus action");
    auto request=BuildWpdLiveViewOperation(WpdLiveViewOperation::Start);
    if(request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,
        action==PreviewFocusAction::AutoFocus?0x90c1:0x9204)!=S_OK)throw std::runtime_error("focus opcode");
    if(action!=PreviewFocusAction::AutoFocus){
        Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;
        if(request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&args)!=S_OK)throw std::runtime_error("focus args");
        PROPVARIANT value{};value.vt=VT_UI4;value.ulVal=action==PreviewFocusAction::Near?1:2;
        if(args->Add(&value)!=S_OK)throw std::runtime_error("focus direction");
        value.ulVal=PreviewFineFocusSteps;if(args->Add(&value)!=S_OK)throw std::runtime_error("focus step");
    }
    return request;
}
inline WpdValues BuildPreviewDeviceReadyRequest(){
    auto request=BuildWpdLiveViewOperation(WpdLiveViewOperation::Start);
    if(request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,0x90c8)!=S_OK)throw std::runtime_error("ready opcode");
    return request;
}
// One AF frame position (#426), in the coordinate space of the frame header. The size of that space
// travels with the position, so the worker can refuse it when the header changed after the click.
struct PreviewAfPointRequest {
    std::uint16_t x{},y{},width{},height{};
    [[nodiscard]] bool Plausible()const{return width>=1 && height>=1 && width<=0x7fff && height<=0x7fff && x<width && y<height;}
    bool operator==(const PreviewAfPointRequest&)const=default;
};
// Single pending action per camera. The worker is the sole command dispatcher.
// Closing is sticky, so a late completion cannot re-enable a retired session.
// An AF frame move shares the one slot with AF and fine drive: while any of them is pending or
// running, no other is accepted.
class PreviewFocusMailbox final {
public:
    bool TryQueue(PreviewFocusAction action,bool admitted){
        if(!admitted || !ValidPreviewFocusAction(action))return false;
        int expected=Idle;return state_.compare_exchange_strong(expected,static_cast<int>(action));
    }
    std::optional<PreviewFocusAction> Take(){
        int value=state_.load();
        if(value<0 || value>2 || !state_.compare_exchange_strong(value,Running))return {};
        return static_cast<PreviewFocusAction>(value);
    }
    bool TryQueueAfPoint(const PreviewAfPointRequest& request,bool admitted){
        if(!admitted || !request.Plausible())return false;
        int expected=Idle;if(!state_.compare_exchange_strong(expected,Staging))return false;
        point_=Pack(request);
        // Disable() may have run while the position was being stored: it stays disabled.
        expected=Staging;return state_.compare_exchange_strong(expected,AfPointQueued);
    }
    std::optional<PreviewAfPointRequest> TakeAfPoint(){
        int expected=AfPointQueued;if(!state_.compare_exchange_strong(expected,Running))return {};
        return Unpack(point_.load());
    }
    void Complete(){int expected=Running;state_.compare_exchange_strong(expected,Idle);}
    void Disable(){state_=Disabled;}
    bool Available()const{return state_==Idle;}
private:
    static constexpr int Idle=-1,Running=-2,Disabled=-3,Staging=-4,AfPointQueued=-5;
    static std::uint64_t Pack(const PreviewAfPointRequest& r){
        return static_cast<std::uint64_t>(r.x)|(static_cast<std::uint64_t>(r.y)<<16)|(static_cast<std::uint64_t>(r.width)<<32)|(static_cast<std::uint64_t>(r.height)<<48);
    }
    static PreviewAfPointRequest Unpack(std::uint64_t v){
        return {static_cast<std::uint16_t>(v),static_cast<std::uint16_t>(v>>16),static_cast<std::uint16_t>(v>>32),static_cast<std::uint16_t>(v>>48)};
    }
    std::atomic<int> state_{Idle};
    std::atomic<std::uint64_t> point_{0};
};
}
