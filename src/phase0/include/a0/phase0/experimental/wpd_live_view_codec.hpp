#pragma once

// Development-only message codec. It cannot enumerate/open a device or send a
// command. Real transport/lease/lifetime integration is deliberately separate.
#include <Windows.h>
#include <PortableDeviceApi.h>
#include <PortableDeviceTypes.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace a0::phase0::experimental {
using WpdValues = Microsoft::WRL::ComPtr<IPortableDeviceValues>;

enum class WpdLiveViewOperation { Start, Frame, Stop };
inline constexpr DWORD WpdLiveViewMaxChunkBytes = 64 * 1024;
inline constexpr std::uint64_t WpdLiveViewMaxPayloadBytes = 4 * 1024 * 1024;
inline constexpr std::uint64_t WpdStillImageMaxPayloadBytes = 256ULL * 1024 * 1024;
inline constexpr std::size_t WpdLiveViewMaxContextCharacters = 4096;

[[nodiscard]] WpdValues BuildWpdLiveViewOperation(WpdLiveViewOperation operation);
[[nodiscard]] WpdValues BuildWpdLiveViewRead(std::wstring_view context, DWORD requested_bytes);
[[nodiscard]] WpdValues BuildWpdLiveViewEndTransfer(std::wstring_view context);

enum class WpdLiveViewResultError {
    None, DispatchFailed, MissingResults, MissingCommandStatus, CommandFailed,
    MissingMtpStatus, MtpRejected, InvalidContext,
    InvalidTransferSize, InvalidChunk,
};
struct WpdLiveViewCommandResult {
    WpdLiveViewResultError error{WpdLiveViewResultError::None};
    HRESULT dispatch_status{S_OK};
    HRESULT command_status{E_PENDING};
    DWORD mtp_response{};
    [[nodiscard]] bool Ok() const noexcept { return error == WpdLiveViewResultError::None; }
};
[[nodiscard]] WpdLiveViewCommandResult InspectWpdLiveViewCommand(
    HRESULT dispatch_status, IPortableDeviceValues* results, bool require_mtp_response);

struct WpdLiveViewTransferBegin {
    WpdLiveViewCommandResult result;
    // Retained when valid even if size/hint validation fails, so a future caller
    // can end an already-created transfer exactly once without retrying it.
    std::wstring context;
    bool total_size_known{};
    std::uint64_t total_bytes{};
    DWORD chunk_bytes{WpdLiveViewMaxChunkBytes};
};
[[nodiscard]] WpdLiveViewTransferBegin InspectWpdLiveViewTransferBegin(
    HRESULT dispatch_status, IPortableDeviceValues* results,
    std::uint64_t maximum_payload_bytes = WpdLiveViewMaxPayloadBytes);

struct WpdLiveViewReadChunk {
    WpdLiveViewCommandResult result;
    // Opaque transfer bytes, NOT a validated JPEG or a publishable frame. The
    // final END_DATA_TRANSFER MTP response must still be checked by the caller.
    std::vector<unsigned char> bytes;
};
[[nodiscard]] WpdLiveViewReadChunk InspectWpdLiveViewReadChunk(
    HRESULT dispatch_status, IPortableDeviceValues* results, DWORD requested_bytes);
} // namespace a0::phase0::experimental
