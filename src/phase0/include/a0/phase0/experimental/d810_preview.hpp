#pragma once
#include <span>
#include <stdexcept>
#include <optional>
#include <chrono>
#include "a0/phase0/experimental/wpd_live_view_codec.hpp"
#include <PortableDevice.h>
#include <WpdMtpExtensions.h>

namespace a0::phase0::experimental {
// Graceful stop refuses a new frame but drains one already sent, within its
// original deadline. Aborting a healthy WPD data phase can leave the camera
// unable to accept DeviceReady/Stop. This never extends the I/O deadline.
inline bool ContinuePreviewFrame(bool stop_requested,bool frame_dispatched,bool expired){
    return !expired && (!stop_requested || frame_dispatched);
}
inline size_t D810PreviewPropertyBytes(DWORD property){
    switch(property){
    case 0xd1a2:case 0xd061:case 0x5001:return 1; // Live, Live AF, battery.
    case 0xd1b2:case 0xd1f0:return 1; // Read-only diagnostic: display error, application mode.
    case 0xd1a6:case 0xd10b:return 1; // Live selector, recording media (read only here).
    case 0x500e:return 2; // Standard exposure program (read only here).
    case 0x500a:return 2; // Standard focus mode.
    case 0xd1a4:case 0x500d:return 4; // Prohibition, exposure time.
    default:return 0;
    }
}
inline WpdValues D810PreviewStatusRequest(DWORD property) {
    if(!D810PreviewPropertyBytes(property))throw std::invalid_argument("unsupported diagnostic property");
    auto request=BuildWpdLiveViewOperation(WpdLiveViewOperation::Frame);
    if(request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,0x1015)!=S_OK)throw std::runtime_error("diagnostic opcode");
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> parameters;
    if(CoCreateInstance(CLSID_PortableDevicePropVariantCollection,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&parameters))!=S_OK)throw std::runtime_error("diagnostic params");
    PROPVARIANT value{};value.vt=VT_UI4;value.ulVal=property;
    if(parameters->Add(&value)!=S_OK || request->SetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,parameters.Get())!=S_OK)throw std::runtime_error("diagnostic parameter");
    return request;
}
// Public digiCamControl NikonD600Base (D810 ancestor), fixed commit in NOTICE:
// 384-byte metadata prefix. Never search arbitrary offsets for an apparent JPEG.
inline std::span<const unsigned char> D810PreviewJpeg(std::span<const unsigned char> payload) {
    if (payload.size() < 388 || payload.size() > WpdLiveViewMaxPayloadBytes ||
        payload[384] != 0xff || payload[385] != 0xd8 ||
        payload[payload.size()-2] != 0xff || payload.back() != 0xd9)
        throw std::runtime_error("invalid D810 preview JPEG envelope");
    return payload.subspan(384);
}
// Public digiCamControl NikonD600Base + BaseMTPCamera.ToInt16, fixed
// commit in NOTICE. Only the signed integer words are established by that
// implementation; do not guess fractional bytes or a sensor-validity flag.
struct D810LevelSample {int roll_degrees{},pitch_degrees{};};
inline std::optional<D810LevelSample> D810PreviewLevel(std::span<const unsigned char> payload){
    try{D810PreviewJpeg(payload);}catch(const std::runtime_error&){return {};}
    const auto signed_word=[&](size_t offset){
        const int word=(static_cast<int>(payload[offset])<<8)|payload[offset+1];
        return word>=32768?word-65536:word;
    };
    D810LevelSample sample{signed_word(52),signed_word(56)};
    // Public driver uses degrees in RotateTransform. Real D810s also send
    // 358/359 near horizontal. Accept one full turn, then show its equivalent
    // signed angle. This is not a sensor-validity or optical quality test.
    if(sample.roll_degrees < -360 || sample.roll_degrees > 360 ||
        sample.pitch_degrees < -360 || sample.pitch_degrees > 360)return {};
    const auto signed_angle=[](int degrees){return degrees>180?degrees-360:degrees < -180?degrees+360:degrees;};
    sample.roll_degrees=signed_angle(sample.roll_degrees);sample.pitch_degrees=signed_angle(sample.pitch_degrees);
    return sample;
}
inline bool D810LevelVisible(const std::optional<D810LevelSample>& sample,bool active,
    std::chrono::steady_clock::time_point received,std::chrono::steady_clock::time_point now){
    return active && sample.has_value() && now>=received && now-received<std::chrono::seconds(2);
}
// Last-boundary allowlist. This adapter can never dispatch capture/property writes.
inline bool AllowedD810PreviewRequest(IPortableDeviceValues* request) {
    if (!request) return false;
    GUID category{}; DWORD id{};
    if (request->GetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, &category) != S_OK ||
        request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, &id) != S_OK) return false;
    PROPERTYKEY command{category,id};
    if (IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_READ_DATA) ||
        IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER)) return true;
    DWORD opcode{};
    if (request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&opcode) != S_OK) return false;
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> parameters;
    DWORD count{};
    if (request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&parameters) != S_OK ||
        !parameters || parameters->GetCount(&count) != S_OK) return false;
    if(opcode==0x1015 && count==1 && IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ)){
        PROPVARIANT value{};const auto hr=parameters->GetAt(0,&value);
        bool allowed=hr==S_OK && value.vt==VT_UI4 && D810PreviewPropertyBytes(value.ulVal)!=0;
        PropVariantClear(&value);return allowed;
    }
    if(opcode==0x9204 && count==2 && IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE)){
        PROPVARIANT direction{},steps{};
        const auto first=parameters->GetAt(0,&direction),second=parameters->GetAt(1,&steps);
        const bool allowed=first==S_OK && second==S_OK && direction.vt==VT_UI4 && steps.vt==VT_UI4 &&
            (direction.ulVal==1 || direction.ulVal==2) && steps.ulVal==10;
        PropVariantClear(&direction);PropVariantClear(&steps);return allowed;
    }
    if(count!=0)return false;
    return (IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE) &&
        (opcode==0x9201 || opcode==0x9202 || opcode==0x90c1 || opcode==0x90c8)) ||
        (IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ) && opcode==0x9203);
}
// Session ownership is required even for cleanup. END/Stop remain permitted
// after a frame deadline so a timed-out transfer can still be closed safely.
inline bool AdmitD810PreviewDispatch(IPortableDeviceValues* request,bool owned,bool stop_requested,bool expired){
    if(!owned || !AllowedD810PreviewRequest(request))return false;
    GUID category{};DWORD id{},opcode{};
    request->GetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,&category);
    request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,&id);
    request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&opcode);
    const PROPERTYKEY command{category,id};
    if(IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER) ||
        (IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE) && opcode==0x9202))return true;
    if(expired)return false;
    return !(stop_requested && (opcode==0x9201 || opcode==0x9203 || opcode==0x90c1 || opcode==0x9204));
}
}
