#pragma once
#include "a0/phase0/experimental/wpd_live_view_transfer.hpp"
#include <array>
#include <stdexcept>
#include <PortableDevice.h>

namespace a0::phase0::experimental {
class PreviewStartInterrupted final:public std::runtime_error {
public:PreviewStartInterrupted():std::runtime_error("preview start interrupted by stop request"){}
};
// Only an explicit, successfully transported Nikon response says Live View
// ended. An empty payload or a ten-minute interval alone never proves it.
inline bool PreviewCameraEnded(const WpdLiveViewTransferResult& transfer){
    const auto ended=[](const std::optional<WpdLiveViewCommandResult>& command){
        return command && command->error==WpdLiveViewResultError::MtpRejected &&
            command->dispatch_status==S_OK && command->command_status==S_OK && command->mtp_response==0xa00b;
    };
    return !transfer.Ok() && (ended(transfer.command_failure) || (transfer.end_attempted && ended(transfer.end_result)));
}
inline bool PreviewSessionUnavailable(const WpdLiveViewTransferResult& transfer){
    const auto unavailable=[](const std::optional<WpdLiveViewCommandResult>& command){
        return command && ((command->error==WpdLiveViewResultError::DispatchFailed && command->dispatch_status==E_WPD_DEVICE_NOT_OPEN) ||
            (command->error==WpdLiveViewResultError::CommandFailed && command->dispatch_status==S_OK && command->command_status==E_WPD_DEVICE_NOT_OPEN));
    };
    return !transfer.Ok() && (unavailable(transfer.command_failure) || (transfer.end_attempted && unavailable(transfer.end_result)));
}
// A Windows device-unavailable result is distinct from a closed WPD session
// and from Nikon NotLiveView. It does not identify the physical cause.
inline bool PreviewDeviceUnavailable(const WpdLiveViewTransferResult& transfer){
    const auto unavailable=[](const std::optional<WpdLiveViewCommandResult>& command){
        const auto no_device=HRESULT_FROM_WIN32(ERROR_NO_SUCH_DEVICE);
        return command && ((command->error==WpdLiveViewResultError::DispatchFailed && command->dispatch_status==no_device) ||
            (command->error==WpdLiveViewResultError::CommandFailed && command->dispatch_status==S_OK && command->command_status==no_device));
    };
    return !transfer.Ok() && (unavailable(transfer.command_failure) || (transfer.end_attempted && unavailable(transfer.end_result)));
}
inline bool PreviewControlReady(WpdLiveViewOperation operation,DWORD response){
    return (operation==WpdLiveViewOperation::Start || operation==WpdLiveViewOperation::Stop) &&
        (response==0x2001 || (operation==WpdLiveViewOperation::Stop && response==0xa002));
}
struct PreviewClosedState {bool opened{},start_attempted{},stopped{},closed{};};
// Explicit operator restart only. Keep failed acquisition history separate
// from confirmed cleanup; never reuse a retired channel or an ambiguous Close.
inline bool PreviewRestartAllowed(bool joined,bool closing,bool cleanup_only,bool capture_blocked,
    const std::array<PreviewClosedState,2>& cameras){
    if(!joined || closing || cleanup_only || capture_blocked)return false;
    for(const auto& camera:cameras){
        if(!camera.opened){if(camera.start_attempted)return false;}
        else if(!camera.closed || (camera.start_attempted && !camera.stopped))return false;
    }
    return true;
}
}
