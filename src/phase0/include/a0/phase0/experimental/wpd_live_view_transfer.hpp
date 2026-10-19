#pragma once
#include "a0/phase0/experimental/wpd_live_view_codec.hpp"
#include <atomic>
#include <functional>
#include <optional>
#include <span>

namespace a0::phase0::experimental {
struct WpdLiveViewExchange {
    HRESULT status{E_FAIL};
    WpdValues results;
};
enum class WpdPayloadPurpose { LivePreview, StillImage };
struct WpdLiveViewTransferOptions {
    // Required. The owner checks its monotonic deadline, stop request, lease and
    // capture exclusion here. This does not interrupt a blocked synchronous API.
    std::function<bool()> may_continue;
    // Separately verifies ownership/session exclusion for cleanup even after a
    // frame deadline or stop request. Losing the lease must not authorize END.
    std::function<bool()> may_cleanup;
    // Optional read-only begin request for the narrowly allowlisted preview
    // status diagnostic. Concrete hardware adapters still validate every send.
    std::function<WpdValues()> begin_request;
    WpdPayloadPurpose purpose{WpdPayloadPurpose::LivePreview};
    // StillImage only. Uncommitted validated chunks may be appended to a
    // private diagnostic .partial. Never publish them before verified END.
    std::function<void(std::span<const unsigned char>)> on_chunk;
    std::uint64_t max_payload_bytes{WpdLiveViewMaxPayloadBytes};
    DWORD max_chunk_bytes{WpdLiveViewMaxChunkBytes};
    std::size_t max_read_commands{4096};
};
enum class WpdLiveViewTransferError {
    None, InvalidOptions, NotAdmitted, Interrupted, ChannelBusy, ChannelRetired,
    BeginRejected, ReadRejected, IncompletePayload, EmptyPayload,
    PayloadLimitExceeded, ReadLimitExceeded, EndRejected,
    CleanupNotAdmitted, DispatcherException, AdmissionException, InternalException,
};
struct WpdLiveViewTransferResult {
    WpdLiveViewTransferError error{WpdLiveViewTransferError::InternalException};
    std::optional<WpdLiveViewCommandResult> command_failure;
    std::optional<WpdLiveViewCommandResult> end_result;
    WpdLiveViewTransferError end_error{WpdLiveViewTransferError::None};
    bool begin_dispatched{};
    bool end_attempted{};
    bool end_confirmed{};
    bool channel_retired{};
    std::size_t read_commands{};
    std::size_t received_bytes{};
    // Opaque payload only after verified END and final admission. Not a JPEG,
    // physical camera identity, approved preview or saved product original.
    std::vector<unsigned char> payload;
    [[nodiscard]] bool Ok() const noexcept { return error == WpdLiveViewTransferError::None; }
};
class WpdLiveViewCommandChannel;
[[nodiscard]] WpdLiveViewTransferResult ReadWpdLiveViewPayload(
    WpdLiveViewCommandChannel& channel, const WpdLiveViewTransferOptions& options);

// One instance belongs to one camera. No device open, hardware lease acquisition
// or concrete IPortableDevice implementation exists in this development slice.
class WpdLiveViewCommandChannel {
public:
    virtual ~WpdLiveViewCommandChannel() = default;
    [[nodiscard]] bool Retired() const noexcept { return retired_.load(); }
protected:
    virtual WpdLiveViewExchange Dispatch(IPortableDeviceValues* request) = 0;
private:
    std::atomic_flag in_flight_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> retired_{false};
    friend WpdLiveViewTransferResult ReadWpdLiveViewPayload(
        WpdLiveViewCommandChannel&, const WpdLiveViewTransferOptions&);
};
} // namespace a0::phase0::experimental
