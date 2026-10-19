#include "a0/phase0/experimental/wpd_live_view_transfer.hpp"
#include <algorithm>
#include <utility>
#include <stdexcept>

namespace a0::phase0::experimental {
WpdLiveViewTransferResult ReadWpdLiveViewPayload(
    WpdLiveViewCommandChannel& channel, const WpdLiveViewTransferOptions& options) {
    WpdLiveViewTransferResult result;
    result.channel_retired = channel.Retired();
    const bool still_image=options.purpose==WpdPayloadPurpose::StillImage;
    const auto ceiling=still_image?WpdStillImageMaxPayloadBytes:WpdLiveViewMaxPayloadBytes;
    if (!options.may_continue || !options.may_cleanup || options.max_payload_bytes == 0 ||
        (options.purpose!=WpdPayloadPurpose::LivePreview && !still_image) || (options.on_chunk && !still_image) ||
        options.max_payload_bytes > ceiling || options.max_chunk_bytes == 0 ||
        options.max_chunk_bytes > WpdLiveViewMaxChunkBytes || options.max_read_commands == 0 ||
        options.max_read_commands > 4096) {
        result.error = WpdLiveViewTransferError::InvalidOptions; return result;
    }
    if (channel.in_flight_.test_and_set()) {
        result.error = WpdLiveViewTransferError::ChannelBusy;
        result.channel_retired = channel.retired_.load();
        return result;
    }
    struct Release final {
        std::atomic_flag& flag;
        ~Release() { flag.clear(); }
    } release{channel.in_flight_};
    if (channel.retired_.load()) {
        result.error = WpdLiveViewTransferError::ChannelRetired;
        result.channel_retired = true;
        return result;
    }
    result.error = WpdLiveViewTransferError::None;
    std::wstring context;
    std::vector<unsigned char> pending;
    const auto admitted = [&]() {
        try {
            if (options.may_continue()) return true;
            result.error = result.begin_dispatched ? WpdLiveViewTransferError::Interrupted :
                WpdLiveViewTransferError::NotAdmitted;
        } catch (...) { result.error = WpdLiveViewTransferError::AdmissionException; }
        return false;
    };
    const auto dispatch = [&](IPortableDeviceValues* request, WpdLiveViewExchange& reply) {
        try { reply = channel.Dispatch(request); return true; }
        catch (...) { result.error = WpdLiveViewTransferError::DispatcherException; return false; }
    };
    const auto receive = [&] {
        if (!admitted()) return;
        const auto request = options.begin_request ? options.begin_request() :
            BuildWpdLiveViewOperation(WpdLiveViewOperation::Frame);
        if (!request) throw std::invalid_argument("missing transfer begin request");
        if (!admitted()) return;
        WpdLiveViewExchange reply;
        result.begin_dispatched = true; // Even a throwing dispatch is not retried.
        if (!dispatch(request.Get(), reply)) return;
        auto begin = InspectWpdLiveViewTransferBegin(reply.status, reply.results.Get(),ceiling);
        context = std::move(begin.context);
        if (!begin.result.Ok()) {
            result.error = WpdLiveViewTransferError::BeginRejected;
            result.command_failure = begin.result;
            return;
        }
        if (!admitted()) return;
        if (begin.total_size_known && begin.total_bytes > options.max_payload_bytes) {
            result.error = WpdLiveViewTransferError::PayloadLimitExceeded; return;
        }
        const auto chunk_limit = std::min(begin.chunk_bytes, options.max_chunk_bytes);
        bool complete{};
        while (!complete) {
            if (result.read_commands == options.max_read_commands) {
                result.error = WpdLiveViewTransferError::ReadLimitExceeded; return;
            }
            if (!admitted()) return;
            const auto capacity = options.max_payload_bytes - pending.size();
            // One sentinel byte can prove an unknown-size payload exceeds its
            // cap, or a zero-byte read can prove EOF at an exact cap boundary.
            const auto remaining = begin.total_size_known ? begin.total_bytes - pending.size() : capacity + 1;
            const auto requested = static_cast<DWORD>(std::min<std::uint64_t>(chunk_limit, remaining));
            const auto read = BuildWpdLiveViewRead(context, requested);
            if (!admitted()) return;
            ++result.read_commands;
            WpdLiveViewExchange read_reply;
            if (!dispatch(read.Get(), read_reply)) return;
            auto chunk = InspectWpdLiveViewReadChunk(read_reply.status, read_reply.results.Get(), requested);
            if (!chunk.result.Ok()) {
                result.error = WpdLiveViewTransferError::ReadRejected;
                result.command_failure = chunk.result;
                return;
            }
            result.received_bytes += chunk.bytes.size();
            if (!admitted()) return;
            if (chunk.bytes.size() > capacity) {
                result.error = WpdLiveViewTransferError::PayloadLimitExceeded; return;
            }
            if (begin.total_size_known && chunk.bytes.empty()) {
                result.error = WpdLiveViewTransferError::IncompletePayload; return;
            }
            pending.insert(pending.end(), chunk.bytes.begin(), chunk.bytes.end());
            if(options.on_chunk && !chunk.bytes.empty())options.on_chunk(chunk.bytes);
            complete = begin.total_size_known ? pending.size() == begin.total_bytes : chunk.bytes.size() < requested;
        }
        if (pending.empty()) result.error = WpdLiveViewTransferError::EmptyPayload;
    };
    try { receive(); }
    catch (...) { result.error = WpdLiveViewTransferError::InternalException; }

    // Cleanup has its own admission: deadline/stop can permit END, lease/session
    // loss cannot. It must never become an unguarded send after ownership loss.
    if (!context.empty()) {
        const auto cleanup_admitted = [&] {
            try {
                if (options.may_cleanup()) return true;
                result.end_error = WpdLiveViewTransferError::CleanupNotAdmitted;
            } catch (...) { result.end_error = WpdLiveViewTransferError::AdmissionException; }
            return false;
        };
        try {
            if (cleanup_admitted()) {
                const auto end = BuildWpdLiveViewEndTransfer(context);
                if (cleanup_admitted()) {
                    WpdLiveViewExchange reply;
                    bool sent{};
                    result.end_attempted = true;
                    try { reply = channel.Dispatch(end.Get()); sent = true; }
                    catch (...) { result.end_error = WpdLiveViewTransferError::DispatcherException; }
                    if (sent) {
                        result.end_result = InspectWpdLiveViewCommand(reply.status, reply.results.Get(), true);
                        result.end_confirmed = result.end_result->Ok();
                        if (!result.end_confirmed) result.end_error = WpdLiveViewTransferError::EndRejected;
                    }
                }
            }
        } catch (...) { result.end_error = WpdLiveViewTransferError::InternalException; }
        if (result.error == WpdLiveViewTransferError::None && !result.end_confirmed)
            result.error = result.end_error;
    }
    if (result.error == WpdLiveViewTransferError::None) {
        if (!result.end_confirmed) result.error = WpdLiveViewTransferError::EndRejected;
        else if (admitted()) result.payload = std::move(pending);
    }
    if (result.begin_dispatched && !result.Ok()) channel.retired_.store(true);
    result.channel_retired = channel.retired_.load();
    return result;
}
}
