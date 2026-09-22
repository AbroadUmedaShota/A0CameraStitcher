#pragma once
#include "a0/phase0/phase0.hpp"
#include <chrono>
#include <cstddef>
#include <string>

namespace a0::phase0 {
struct SingleWorkerPreviewResult {
    std::string error;
    std::size_t frames{}, bytes{};
    bool start_attempted{}, stopped{}, closed{};
    bool SafeToRelease() const { return closed && (!start_attempted || stopped); }
    bool Passed() const { return error.empty() && frames == 3 && stopped && closed; }
};

// Shared real/fake lifecycle. No capture, WPD, retry, settings or identity lookup.
// The owner must keep the hardware lease if SafeToRelease() is false.
template<class Transport, class Observe, class CheckDeadline>
SingleWorkerPreviewResult RunSingleWorkerPreview(Transport& transport, Observe&& observe, CheckDeadline&& check) {
    using namespace std::chrono_literals;
    SingleWorkerPreviewResult result;
    bool stop_attempted = false, started = false;
    try {
        check(); observe("opening"); transport.OpenSingleWorkerLiveView(10s);
        check(); observe("starting"); result.start_attempted = true;
        transport.StartSingleWorkerLiveView(10s);
        started = true;
        for (unsigned frame = 0; frame < 3; ++frame) {
            check(); transport.ValidateSingleWorkerLiveView(2s);
            check(); observe("reading");
            const auto jpeg = transport.ReadLiveViewFrame(3s);
            if (jpeg.empty()) throw TransportError("worker_empty_frame", "empty live view JPEG");
            ++result.frames; result.bytes += jpeg.size();
        }
        observe("stopping"); stop_attempted = true;
        transport.StopLiveView(10s); result.stopped = true;
    } catch (const TransportError& e) { result.error = e.Category(); }
      catch (...) { result.error = "worker_unexpected_error"; }
    // Safe close is permitted past the operation deadline. Never repeat STOP.
    if (started && !stop_attempted) {
        try { observe("stopping"); transport.StopLiveView(10s); result.stopped = true; }
        catch (...) { if (result.error.empty()) result.error = "worker_stop_unconfirmed"; }
    }
    try {
        observe("closing"); transport.Close(10s);
        result.closed = transport.InspectDualSessionExitState().FullyEnded();
        if (!result.closed && result.error.empty()) result.error = "worker_close_unconfirmed";
    } catch (...) { if (result.error.empty()) result.error = "worker_close_failed"; }
    return result;
}
} // namespace a0::phase0
