#pragma once
// One-shot probe: the no-AF SDRAM capture (0x90C0 / 0xFFFFFFFF) sent while Live View stays open (#426 stage 4,
// ADR-0044 stage P). It observes how a D810 answers; it is not a capture acceptance.
//
// This header is part of the probe build only (option A0_BUILD_WPD_LIVE_CAPTURE_PROBE, default OFF). It has its
// own allowlist, channel, event ledger and procedure. The Live View, still capture and settings gates are not
// called from here and are not changed by it. WpdStillCaptureChannel assumes "a session opened after both Live
// Views closed"; that assumption stays true for it, which is why this is a separate channel.
#if !defined(A0_ENABLE_WPD_LIVE_CAPTURE_PROBE)
#error "wpd_live_capture_probe.hpp belongs to the probe build only (A0_BUILD_WPD_LIVE_CAPTURE_PROBE, ADR-0044)"
#endif
#include "a0/phase0/experimental/wpd_still_capture.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace a0::phase0::experimental {
// Values below are provisional (ADR-0044 "unconfirmed"): kept as names so a later decision changes one place.
inline constexpr std::chrono::seconds kLiveCaptureProbeDeadline{60};      // from the press
inline constexpr std::chrono::minutes kLiveCaptureProbeLiveAgeMax{5};     // from the camera's first Live frame
inline constexpr std::chrono::seconds kLiveCaptureProbeHoldWait{3};       // both workers must hold within this
// The one camera the probe may expose. A change is a reviewed commit; no file, environment variable or argument.
inline constexpr std::array<const char*, 1> kLiveCaptureProbeCameras{"CAM-B"};
static_assert(kLiveCaptureProbeCameras.size() == 1, "the probe exposes exactly one camera");
constexpr int LiveCaptureProbeCameraIndex() {
    const char* name = kLiveCaptureProbeCameras[0];
    const bool a = name[0] == 'C' && name[1] == 'A' && name[2] == 'M' && name[3] == '-' && name[4] == 'A' && name[5] == 0;
    const bool b = name[0] == 'C' && name[1] == 'A' && name[2] == 'M' && name[3] == '-' && name[4] == 'B' && name[5] == 0;
    return a ? 0 : b ? 1 : -1;
}
static_assert(LiveCaptureProbeCameraIndex() >= 0, "the probe camera must be CAM-A or CAM-B");

// What the allowlist needs to know about the borrowed session.
struct WpdLiveProbeGateState {
    bool lent{};          // the session is lent to this channel right now
    bool shutter_armed{}; // ShutterOnce is being called
    bool shutter_used{};  // the one shutter has been handed to the transport
    std::optional<DWORD> attributed_handle;
};
// Dedicated allowlist, independent of AllowedWpdStillRequest / AllowedD810PreviewRequest. Everything is refused
// unless the session is lent. Live View control and frame reads (0x9201..0x9203), AF (0x90C1), fine drive (0x9204),
// the AF frame (0x9205), 0x9206, 0x9207, 0x90CB, 0x90C2, 0x90C3, DeleteObject, every write and every second
// shutter are not in it.
inline bool AllowedWpdLiveCaptureProbeRequest(IPortableDeviceValues* request, const WpdLiveProbeGateState& state) {
    if (!request || !state.lent) return false;
    GUID category{};
    DWORD id{}, code{};
    if (request->GetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, &category) != S_OK ||
        request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, &id) != S_OK) return false;
    const PROPERTYKEY command{category, id};
    if (IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_READ_DATA) || IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER)) return true;
    const bool without_data = IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE);
    const bool with_read = IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ);
    if (!without_data && !with_read) return false; // WITH_DATA_TO_WRITE, WRITE_DATA and anything else
    if (request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, &code) != S_OK) return false;
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;
    DWORD count{};
    if (request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, &args) != S_OK || !args ||
        args->GetCount(&count) != S_OK || count > 1) return false;
    DWORD argument{};
    if (count) {
        PROPVARIANT v{};
        const auto hr = args->GetAt(0, &v);
        const bool typed = hr == S_OK && v.vt == VT_UI4;
        argument = v.ulVal;
        PropVariantClear(&v);
        if (!typed) return false;
    }
    if (without_data) {
        if (code == 0x90c8) return count == 0;
        return code == 0x90c0 && count == 1 && argument == 0xffffffff && state.shutter_armed && !state.shutter_used;
    }
    if (code == 0x90c7) return count == 0;
    if (code == 0x1015) return count == 1 && (argument == 0xd1a2 || argument == 0x5004 || argument == 0x5003);
    if (code == 0x1008 || code == 0x1009)
        return count == 1 && state.attributed_handle && ValidStillHandle(*state.attributed_handle) && argument == *state.attributed_handle;
    return false;
}

// The borrowed session. Pure virtual Exchange keeps the transport out of this header. A channel is used once:
// ShutterOnce is single-use and nothing here reopens, retries or deletes.
class WpdLiveCaptureProbeChannel : public WpdLiveViewCommandChannel {
public:
    WpdLiveCaptureProbeChannel(std::function<bool()> lent, std::function<bool()> may_continue, std::function<bool()> may_cleanup)
        : lent_(std::move(lent)), may_continue_(std::move(may_continue)), may_cleanup_(std::move(may_cleanup)) {
        if (!lent_ || !may_continue_ || !may_cleanup_) throw std::invalid_argument("missing probe ownership admission");
    }
    [[nodiscard]] bool Admitted() const { return may_continue_(); }
    [[nodiscard]] bool ShutterAttempted() const { return state_.shutter_used; }
    [[nodiscard]] bool ShutterConfirmed() const { return shutter_confirmed_; }
    [[nodiscard]] const std::optional<WpdLiveViewCommandResult>& ShutterResult() const { return shutter_result_; }
    // Runs just before the shutter reaches the transport, so a hang inside the call still leaves the log line.
    void SetShutterAnnouncement(std::function<void()> announce) { announce_ = std::move(announce); }
    WpdLiveViewCommandResult Ready() {
        auto request = BuildWpdStillRequest(WpdStillOperation::Ready);
        auto r = Dispatch(request.Get());
        return InspectWpdLiveViewCommand(r.status, r.results.Get(), true);
    }
    // The only place in the probe that builds the shutter request.
    void ShutterOnce() {
        if (state_.shutter_used || state_.shutter_armed) throw std::runtime_error("probe shutter is single-use");
        state_.shutter_armed = true;
        auto request = BuildWpdStillRequest(WpdStillOperation::CaptureNoAf);
        auto r = Dispatch(request.Get());
        shutter_result_ = InspectWpdLiveViewCommand(r.status, r.results.Get(), true);
        shutter_confirmed_ = shutter_result_->Ok();
        if (!shutter_confirmed_) throw std::runtime_error("probe shutter completion unconfirmed; not retried");
    }
    // Only the object whose event the ledger attributed.
    void AuthorizeObject(DWORD handle) {
        if (!state_.shutter_used || !ValidStillHandle(handle) || state_.attributed_handle) throw std::runtime_error("probe object attribution not established");
        state_.attributed_handle = handle;
    }
    std::vector<unsigned char> Read(WpdStillOperation operation, std::uint64_t cap,
        const std::function<void(std::span<const unsigned char>)>& partial_sink = {}) {
        if (operation == WpdStillOperation::CaptureNoAf || operation == WpdStillOperation::Ready) throw std::invalid_argument("not a read operation");
        WpdLiveViewTransferOptions options;
        options.may_continue = may_continue_;
        options.may_cleanup = may_cleanup_;
        options.purpose = WpdPayloadPurpose::StillImage;
        options.max_payload_bytes = cap;
        options.on_chunk = partial_sink;
        options.begin_request = [&] { return BuildWpdStillRequest(operation, state_.attributed_handle.value_or(0)); };
        auto result = ReadWpdLiveViewPayload(*this, options);
        if (!result.Ok()) throw std::runtime_error("probe data transfer failed: " + std::to_string(static_cast<int>(result.error)));
        return std::move(result.payload);
    }
protected:
    virtual WpdLiveViewExchange Exchange(IPortableDeviceValues*) = 0;
private:
    WpdLiveViewExchange Dispatch(IPortableDeviceValues* request) final {
        GUID category{};
        DWORD id{}, code{};
        request->GetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, &category);
        request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, &id);
        request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, &code);
        const PROPERTYKEY command{category, id};
        const bool cleanup = IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER);
        WpdLiveProbeGateState gate = state_;
        gate.lent = lent_();
        if (!may_cleanup_() || (!cleanup && !may_continue_()) || !AllowedWpdLiveCaptureProbeRequest(request, gate))
            throw std::runtime_error("probe command not admitted");
        const bool shutter = code == 0x90c0 && IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE);
        if (shutter) {
            state_.shutter_armed = false;
            state_.shutter_used = true;
            if (announce_) announce_();
        }
        return Exchange(request);
    }
    std::function<bool()> lent_, may_continue_, may_cleanup_;
    std::function<void()> announce_;
    WpdLiveProbeGateState state_;
    bool shutter_confirmed_{};
    std::optional<WpdLiveViewCommandResult> shutter_result_;
};

// Reads event meanings from the public sources named in third_party/PTP_PROTOCOL_REFERENCES.md. It records and
// classifies; it never throws, because the probe's purpose is to see what arrives. Before the shutter any image,
// completion, card-image, store-removed or unnamed code ends the probe (StopCode); a change of storage or state asks
// for one re-check (RefreshRequired). After the shutter only one object event and at most one completion are
// expected; anything else is counted as unexpected and keeps the image out of original.jpg.
class WpdLiveProbeEventLedger final {
public:
    void Observe(std::span<const WpdStillEvent> events, bool after_shutter) {
        for (const auto& event : events) {
            ++counts_[event.code];
            if (event.code == 0x4006 || event.code == 0xc104) continue; // property change / preview image: no ownership
            if (event.code == 0x4004 || event.code == 0x4008 || event.code == 0xc10c) {
                if (after_shutter) ++unexpected_; else refresh_required_ = true;
                continue;
            }
            if (event.code == 0xc101) {
                if (!after_shutter) { NoteStop(event.code); continue; }
                ++object_events_;
                if (object_events_ == 1) {
                    if (ValidStillHandle(event.parameter)) handle_ = event.parameter;
                    else { ++unexpected_; invalid_handle_ = true; }
                } else { ambiguous_ = true; handle_.reset(); ++unexpected_; }
                continue;
            }
            if (event.code == 0xc102 || event.code == 0x400d) {
                if (!after_shutter) { NoteStop(event.code); continue; }
                ++completion_events_;
                if (completion_events_ == 1) { completed_ = true; completion_code_ = event.code; }
                else ++unexpected_;
                continue;
            }
            if (!after_shutter) { NoteStop(event.code); continue; }
            ++unexpected_;
            if (event.code == 0x4002) card_image_ = true;
        }
    }
    [[nodiscard]] std::optional<std::uint16_t> StopCode() const { return stop_code_; }
    [[nodiscard]] bool RefreshRequired() const { return refresh_required_; }
    [[nodiscard]] std::optional<DWORD> Handle() const { return handle_; }
    [[nodiscard]] bool Ambiguous() const { return ambiguous_; }
    [[nodiscard]] bool CardImageEvent() const { return card_image_; }
    [[nodiscard]] unsigned ObjectEvents() const { return object_events_; }
    [[nodiscard]] unsigned CompletionEvents() const { return completion_events_; }
    [[nodiscard]] unsigned Unexpected() const { return unexpected_; }
    [[nodiscard]] bool Completed() const { return completed_; }
    [[nodiscard]] std::uint16_t CompletionCode() const { return completion_code_; }
    // One valid object event and no card-image event: the image may be read (into a partial).
    [[nodiscard]] bool ImportAllowed() const { return handle_.has_value() && !ambiguous_ && !card_image_; }
    // Import allowed and every event after the shutter was an expected kind: the image may become original.jpg.
    [[nodiscard]] bool Pristine() const { return ImportAllowed() && unexpected_ == 0; }
    [[nodiscard]] const std::map<std::uint16_t, unsigned>& Counts() const { return counts_; }
private:
    void NoteStop(std::uint16_t code) { if (!stop_code_) stop_code_ = code; }
    std::map<std::uint16_t, unsigned> counts_;
    std::optional<std::uint16_t> stop_code_;
    std::optional<DWORD> handle_;
    unsigned object_events_{}, completion_events_{}, unexpected_{};
    bool refresh_required_{}, ambiguous_{}, card_image_{}, invalid_handle_{}, completed_{};
    std::uint16_t completion_code_{};
};
// "0x4006:1,0xc101:1" (codes and counts only, never an event parameter), or "none".
inline std::string DescribeLiveProbeEventCounts(const std::map<std::uint16_t, unsigned>& counts) {
    if (counts.empty()) return "none";
    std::string text;
    for (const auto& [code, count] : counts) {
        if (!text.empty()) text += ',';
        text += WpdStillEventCodeText(code) + ":" + std::to_string(count);
    }
    return text;
}

enum class WpdLiveProbeResult { StoppedBeforeShutter, Observed, StoppedAfterShutter };
inline const char* WpdLiveProbeResultText(WpdLiveProbeResult result) {
    switch (result) {
    case WpdLiveProbeResult::Observed: return "observed";
    case WpdLiveProbeResult::StoppedAfterShutter: return "stopped_after_shutter";
    default: return "stopped_before_shutter";
    }
}
// Everything one run saw, as codes, counts and states. No handle, file name or device identifier.
struct WpdLiveProbeOutcome {
    WpdLiveProbeResult result{WpdLiveProbeResult::StoppedBeforeShutter};
    std::string stop_reason; // short fixed token, never exception text
    // before the shutter
    DWORD ready{};
    int live{-1}, quality{-1};
    bool size_ok{}, refresh{};
    std::string preflight_events{"none"};
    // the shutter
    bool shutter_sent{}, shutter_result_known{};
    WpdLiveViewCommandResult shutter;
    // after the shutter
    DWORD after_ready{};
    unsigned busy_polls{};
    long long ready_waited_ms{};
    bool out_of_focus{};
    std::string after_events{"none"};
    unsigned object_events{}, completion_events{}, unexpected{};
    bool attributed{};
    std::uint32_t bytes{}, width{}, height{};
    std::string published{"none"}, object_reason{"not_attributed"};
    WpdStillCaptureObservation completion;
    std::optional<unsigned> live_after;
    // "read", "unreadable" (tried, or no longer admitted) or "not_read" (an anomaly stopped the run before it).
    std::string live_after_state{"not_read"};
    // DeviceReady sent once just before the import of a clean run, so that a busy camera does not refuse the import and
    // leave the image in the camera. It is recorded and never used to decide anything. A run with an anomaly sends none.
    bool import_ready_attempted{};
    DWORD import_ready{};
    // The image may still be in the camera's memory (the import did not finish): Live View may be refused until a power cycle.
    bool sdram_may_remain{};
    // True only when the shutter was confirmed and nothing outside the expected sequence happened. The window asks for
    // its one frame after the shot only when this is true. `anomaly` names the first thing outside the sequence.
    bool sequence_clean{};
    std::string anomaly{"none"};
};
// What to report when RunLiveCaptureProbe was left by an exception: all that is known is whether the shutter was attempted.
// If it was, an exposure may exist and the image may still be in the camera.
inline WpdLiveProbeOutcome WpdLiveProbeOutcomeAfterException(bool shutter_attempted) {
    WpdLiveProbeOutcome out;
    out.stop_reason = "probe_exception";
    out.anomaly = "probe_exception";
    if (shutter_attempted) {
        out.shutter_sent = true;
        out.result = WpdLiveProbeResult::StoppedAfterShutter;
        out.sdram_may_remain = true;
    }
    return out;
}

struct WpdLiveProbeOptions {
    std::chrono::milliseconds ready_wait{10000}, ready_poll{50};
    std::chrono::milliseconds preflight_gap{100};
    std::chrono::milliseconds event_wait{15000}, event_poll{100};
    WpdStillCompletionWait completion{};
};
struct WpdLiveProbeHooks {
    std::function<void(std::span<const unsigned char>)> partial_sink; // private partial; required
    // Persists a verified image as original.jpg; throws or returns false when it was not saved.
    std::function<bool(WpdStillPayload&&)> save_original;
    std::function<void(const std::string&)> log;
    std::function<void(std::chrono::milliseconds)> sleep; // tests only; default is a real sleep
};

namespace live_probe_detail {
inline std::string Hex(unsigned long value, int width) {
    char text[16]{};
    std::snprintf(text, sizeof text, "0x%0*lx", width, value);
    return text;
}
struct Refusal { std::string reason; };
}

// Steps 3..9 of the probe, on a session that is already lent. The caller (the window) holds both Live workers first
// and returns them afterwards. The shutter is sent at most once; nothing is resent. Each stop is a fixed reason token.
inline WpdLiveProbeOutcome RunLiveCaptureProbe(WpdLiveCaptureProbeChannel& channel, const std::string& alias,
    const WpdLiveProbeHooks& hooks, const WpdLiveProbeOptions& options = {}) {
    using live_probe_detail::Hex;
    using live_probe_detail::Refusal;
    if (!hooks.partial_sink) throw std::invalid_argument("missing probe partial sink");
    if (channel.ShutterAttempted()) throw std::logic_error("a probe channel is used once"); // a new channel for every run
    WpdLiveProbeOutcome out;
    const auto log = [&](const std::string& line) { if (hooks.log) hooks.log(alias + " " + line); };
    const auto sleep = [&](std::chrono::milliseconds d) { if (hooks.sleep) hooks.sleep(d); else std::this_thread::sleep_for(d); };
    const auto now = [] { return std::chrono::steady_clock::now(); };
    const auto elapsed_ms = [&](std::chrono::steady_clock::time_point since) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now() - since).count();
    };
    const auto read_events = [&]() { return ParseWpdStillEvents(channel.Read(WpdStillOperation::Events, 8192)); };

    // ---- Before the shutter: nothing is exposed unless every check passes.
    std::map<std::uint16_t, unsigned> before_counts;
    const auto active = [&] { if (!channel.Admitted()) throw Refusal{"deadline_or_stop"}; };
    const auto wait_ready = [&] {
        const auto until = now() + options.ready_wait;
        for (;;) {
            active();
            WpdLiveViewCommandResult r;
            try { r = channel.Ready(); } catch (const std::exception&) { throw Refusal{"ready_failed"}; }
            if (r.Ok()) { out.ready = 0x2001; return; }
            if (r.error != WpdLiveViewResultError::MtpRejected) throw Refusal{"ready_failed"};
            out.ready = r.mtp_response;
            if (r.mtp_response == 0xa002) throw Refusal{"out_of_focus_before_shutter"};
            if (r.mtp_response != 0x2019) throw Refusal{"ready_rejected"};
            if (now() >= until) throw Refusal{"ready_timeout"};
            sleep(options.ready_poll);
        }
    };
    const auto check_profile = [&] {
        try {
            const auto live = channel.Read(WpdStillOperation::LiveStatus, 16);
            out.live = live.size() == 1 ? static_cast<int>(live[0]) : -1;
            const auto quality = channel.Read(WpdStillOperation::ImageQuality, 16);
            out.quality = (quality.size() == 1 || quality.size() == 2) ? static_cast<int>(StillLittleEndian(quality, 0, quality.size())) : -1;
            try { out.size_ok = ParseWpdStillString(channel.Read(WpdStillOperation::ImageSize, 128)) == L"7360x4912"; }
            catch (const std::runtime_error&) { out.size_ok = false; }
        } catch (const std::exception&) { throw Refusal{"profile_read_failed"}; }
        if (out.live != 1) throw Refusal{"live_not_on"};
        if (out.quality != 2) throw Refusal{"quality_not_jpeg_fine"};
        if (!out.size_ok) throw Refusal{"size_not_fx_l"};
    };
    const auto drain_baseline = [&](WpdLiveProbeEventLedger& ledger) {
        struct Merge {
            std::map<std::uint16_t, unsigned>& into; const WpdLiveProbeEventLedger& from;
            ~Merge() { for (const auto& [code, count] : from.Counts()) into[code] += count; }
        } merge{before_counts, ledger};
        for (int i = 0; i < 2; ++i) {
            if (i) sleep(options.preflight_gap);
            active();
            try { ledger.Observe(read_events(), false); } catch (const std::exception&) { throw Refusal{"event_read_failed"}; }
            if (const auto stop = ledger.StopCode()) throw Refusal{"event_" + WpdStillEventCodeText(*stop) + "_before_shutter"};
        }
    };
    bool preflight_ok = false;
    try {
        active();
        wait_ready();
        check_profile();
        WpdLiveProbeEventLedger baseline;
        drain_baseline(baseline);
        if (baseline.RefreshRequired()) {
            // One read-only re-check, then a new bounded baseline. Never loop until a changing camera looks stable.
            out.refresh = true;
            active();
            wait_ready();
            check_profile();
            WpdLiveProbeEventLedger settled;
            drain_baseline(settled);
            if (settled.RefreshRequired()) throw Refusal{"state_did_not_settle"};
        }
        active();
        preflight_ok = true;
    } catch (const Refusal& refusal) { out.stop_reason = refusal.reason; }
    out.preflight_events = DescribeLiveProbeEventCounts(before_counts);
    log("live_capture_probe_preflight ready=" + Hex(out.ready, 4) + " live=" + std::to_string(out.live) + " quality=" + std::to_string(out.quality) +
        " size_ok=" + (out.size_ok ? "1" : "0") + " refresh=" + (out.refresh ? "1" : "0") + " events=" + out.preflight_events +
        " result=" + (preflight_ok ? "ready" : "refused") + " reason=" + (preflight_ok ? std::string("none") : out.stop_reason));
    if (!preflight_ok) return out;

    // ---- The one shutter.
    if (!channel.Admitted()) { out.stop_reason = "deadline_or_stop"; return out; }
    struct AnnouncementGuard { WpdLiveCaptureProbeChannel& channel; ~AnnouncementGuard() { channel.SetShutterAnnouncement({}); } } announcement_guard{channel};
    channel.SetShutterAnnouncement([&] { log("shutter_no_af_once=1 card_write_command=0 live_open=1"); });
    try { channel.ShutterOnce(); } catch (const std::exception&) {}
    out.shutter_sent = channel.ShutterAttempted();
    if (!out.shutter_sent) { out.stop_reason = "shutter_not_admitted"; return out; }
    out.result = WpdLiveProbeResult::StoppedAfterShutter;
    if (const auto& result = channel.ShutterResult()) {
        out.shutter = *result;
        out.shutter_result_known = true;
        log("shutter_result_error=" + std::to_string(static_cast<int>(result->error)) + " dispatch=" + Hex(static_cast<unsigned long>(result->dispatch_status), 0) +
            " command=" + Hex(static_cast<unsigned long>(result->command_status), 0) + " mtp=" + Hex(result->mtp_response, 4));
    } else log("shutter_result_error=none transport_exception=1");
    const bool confirmed = channel.ShutterConfirmed();
    out.completion.shutter_attempted = true;
    // A refusal carries a response code and no exposure. Anything else that is not a confirmed answer (a transport
    // failure) is uncertain: an exposure may exist.
    const bool refused_by_body = !confirmed && out.shutter_result_known && out.shutter.error == WpdLiveViewResultError::MtpRejected;
    const bool uncertain = !confirmed && !refused_by_body;
    // The first thing outside the expected sequence. Once it is set the probe sends no new kind of command: it only
    // reads events inside the existing bound to find the one attributed object and imports that object once into a
    // private partial. It does not wait for the completion event, does not read 0xD1A2 again and asks for no frame.
    bool anomaly = false;
    const auto flag = [&](const std::string& why) { if (!anomaly) { anomaly = true; out.anomaly = why; } };
    if (!confirmed) {
        out.stop_reason = refused_by_body ? "shutter_refused_by_body" : "shutter_unconfirmed";
        flag(out.stop_reason);
        if (refused_by_body) out.after_ready = out.shutter.mtp_response;
    }
    if (refused_by_body) {
        // The body refused: nothing was exposed, so there is nothing to wait for, import or read back.
        log("live_capture_probe_live_after value=not_read reason=" + out.anomaly);
        log("live_capture_probe_anomaly reason=" + out.anomaly + " not_sent=event_read,object_read,live_state_read,frame_read");
        return out;
    }
    bool imported = false;
    bool skipped_completion = false, skipped_import_ready = false;

    // ---- After the shutter: ready state, events, the one object, completion.
    WpdLiveProbeEventLedger ledger;
    std::optional<WpdStillPayload> payload;
    if (confirmed) {
        const auto start = now();
        DWORD code = 0;
        try {
            for (;;) {
                if (!channel.Admitted()) break;
                const auto r = channel.Ready();
                if (r.Ok()) { code = 0x2001; break; }
                if (r.error != WpdLiveViewResultError::MtpRejected) break;
                code = r.mtp_response;
                if (code != 0x2019) break;
                ++out.busy_polls;
                if (std::chrono::steady_clock::now() - start >= options.ready_wait) break;
                sleep(options.ready_poll);
            }
        } catch (const std::exception&) {}
        out.after_ready = code;
        out.ready_waited_ms = elapsed_ms(start);
        out.out_of_focus = code == 0xa002;
        if (code != 0x2001) flag("after_shutter_not_ready");
        log("live_capture_probe_after_shutter_ready=" + Hex(code, 4) + " busy_polls=" + std::to_string(out.busy_polls) + " waited_ms=" + std::to_string(out.ready_waited_ms));
    }
    // An uncertain shutter is observed the same way but never promoted (and nothing after the image is read). The
    // event reads below are the only commands sent after an anomaly, and only until the one object is found.
    {
        const auto until = now() + options.event_wait;
        out.object_reason = "object_event_timeout";
        for (;;) {
            if (!channel.Admitted()) { out.object_reason = "deadline_or_stop"; break; }
            try { ledger.Observe(read_events(), true); } catch (const std::exception&) { out.object_reason = "event_read_failed"; break; }
            if (ledger.Ambiguous()) { out.object_reason = "ambiguous_object_events"; break; }
            if (ledger.CardImageEvent()) { out.object_reason = "card_image_event"; break; }
            if (ledger.Handle()) { out.object_reason = "attributed"; break; }
            if (now() >= until) break;
            sleep(options.event_poll);
        }
        if (out.object_reason != "attributed") flag(out.object_reason);
        if (ledger.Unexpected() > 0) flag("unexpected_event");
        if (ledger.ImportAllowed()) {
            out.attributed = true;
            try {
                channel.AuthorizeObject(*ledger.Handle());
                // Once, in a clean run only: after an anomaly the import adds no new kind of command.
                if (anomaly) skipped_import_ready = true;
                else if (channel.Admitted()) {
                    out.import_ready_attempted = true;
                    try {
                        const auto r = channel.Ready();
                        out.import_ready = r.Ok() ? 0x2001 : r.error == WpdLiveViewResultError::MtpRejected ? r.mtp_response : 0;
                    } catch (const std::exception&) {}
                    log("live_capture_probe_import_ready=" + Hex(out.import_ready, 4));
                }
                WpdStillObjectInfo info{};
                try { info = ParseWpdStillObjectInfo(channel.Read(WpdStillOperation::ObjectInfo, 4096)); }
                catch (const std::runtime_error&) { out.object_reason = "object_info_invalid"; throw; }
                out.bytes = info.bytes; out.width = info.width; out.height = info.height;
                auto bytes = channel.Read(WpdStillOperation::Object, info.bytes, hooks.partial_sink);
                if (bytes.size() != info.bytes) { out.object_reason = "import_size_mismatch"; throw std::runtime_error("size"); }
                payload = WpdStillPayload{std::move(bytes), info, false};
                imported = true;
                out.object_reason = "imported";
            } catch (const std::exception&) { if (out.object_reason == "attributed") out.object_reason = "import_failed"; flag(out.object_reason); }
        }
        // Events that arrive around the import and the completion event, observed inside the existing bound. Skipped
        // once an anomaly is flagged, and left at once when an unexpected event shows up during it.
        if (confirmed && ledger.ObjectEvents() > 0) {
            if (anomaly) {
                skipped_completion = true;
            } else {
                for (int i = 0; i < 2 && channel.Admitted(); ++i) {
                    if (i) sleep(options.event_poll);
                    try { ledger.Observe(read_events(), true); } catch (const std::exception&) { break; }
                    if (ledger.Unexpected() > 0) break;
                }
                out.completion.completion_wait = WpdStillCompletionWaitResult::Aborted;
                if (ledger.Unexpected() > 0) { flag("unexpected_event"); }
                else if (ledger.Completed()) { out.completion.completion_wait = WpdStillCompletionWaitResult::AlreadySeen; }
                else {
                    const auto start = now();
                    for (;;) {
                        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now() - start);
                        if (!channel.Admitted()) break;
                        if (waited >= options.completion.max) { out.completion.completion_wait = WpdStillCompletionWaitResult::TimedOut; break; }
                        sleep(std::min(options.completion.poll, options.completion.max - waited));
                        if (!channel.Admitted()) break;
                        try { ledger.Observe(read_events(), true); } catch (const std::exception&) { break; }
                        if (ledger.Unexpected() > 0) { flag("unexpected_event"); break; }
                        if (ledger.Completed()) { out.completion.completion_wait = WpdStillCompletionWaitResult::Seen; break; }
                    }
                    out.completion.completion_wait_ms = elapsed_ms(start);
                }
            }
        }
        out.completion.complete_event = ledger.Completed();
        out.completion.complete_event_code = ledger.CompletionCode();
        out.after_events = DescribeLiveProbeEventCounts(ledger.Counts());
        out.object_events = ledger.ObjectEvents();
        out.completion_events = ledger.CompletionEvents();
        out.unexpected = ledger.Unexpected();
        log("live_capture_probe_events phase=after_shutter codes=" + out.after_events + " object_events=" + std::to_string(out.object_events) +
            " completion_events=" + std::to_string(out.completion_events) + " unexpected=" + std::to_string(out.unexpected));
        // Publication: only a confirmed shutter, a ready camera and a clean event sequence make original.jpg.
        if (payload) {
            const bool clean = confirmed && !anomaly && ledger.Pristine() && out.after_ready == 0x2001;
            out.published = "partial_only";
            out.object_reason = clean ? "verified" : !confirmed ? "shutter_unconfirmed" : !ledger.Pristine() ? "unexpected_event" : "after_shutter_not_ready";
            if (clean && hooks.save_original) {
                try {
                    if (hooks.save_original(std::move(*payload))) out.published = "original";
                    else out.object_reason = "persist_failed";
                } catch (const std::exception&) { out.object_reason = "persist_failed"; }
            }
        }
        log("live_capture_probe_object attributed=" + std::string(out.attributed ? "1" : "0") + " bytes=" + std::to_string(out.bytes) + " width=" + std::to_string(out.width) +
            " height=" + std::to_string(out.height) + " published=" + out.published + " reason=" + out.object_reason);
        if (confirmed) {
            log("capture_completion " + DescribeWpdStillCaptureObservation(out.completion));
            log("completion_wait " + DescribeWpdStillCompletionWait(out.completion, options.completion));
        }
    }
    out.sdram_may_remain = (confirmed || uncertain) && !imported;
    // The state of Live View after the shot (read once), only when the sequence stayed clean. After an anomaly the
    // log says it was not read.
    if (!anomaly) {
        out.live_after_state = "unreadable";
        try {
            if (channel.Admitted()) {
                const auto live = channel.Read(WpdStillOperation::LiveStatus, 16);
                if (live.size() == 1) { out.live_after = live[0]; out.live_after_state = "read"; }
            }
        } catch (const std::exception&) {}
        log("live_capture_probe_live_after value=" + (out.live_after ? Hex(*out.live_after, 2) : std::string("unreadable")));
    } else {
        log("live_capture_probe_live_after value=not_read reason=" + out.anomaly);
        log("live_capture_probe_anomaly reason=" + out.anomaly + " not_sent=" + std::string(skipped_completion ? "completion_wait," : "") +
            (skipped_import_ready ? "import_ready," : "") + "live_state_read,frame_read");
    }
    out.sequence_clean = confirmed && !anomaly;
    if (out.sequence_clean && imported && out.live_after) out.result = WpdLiveProbeResult::Observed;
    if (out.result != WpdLiveProbeResult::Observed && out.stop_reason.empty()) out.stop_reason = out.object_reason;
    return out;
}
// The last line of a run. `elapsed_ms` counts from the press.
inline std::string DescribeLiveCaptureProbeResult(WpdLiveProbeResult result, bool shutter_sent, long long elapsed_ms, const std::string& reason) {
    return std::string("live_capture_probe_result=") + WpdLiveProbeResultText(result) + " shutter_commands=" + (shutter_sent ? "1" : "0") +
        " resend=0 setting_writes=0 live_start_commands=0 elapsed_ms=" + std::to_string(elapsed_ms) + " reason=" + (reason.empty() ? std::string("none") : reason);
}
// The same line with what the run did after the shot: whether the sequence stayed clean, the first anomaly, whether the
// Live state was read ("not_read" says it was not sent) and what happened to the one frame the window may ask for.
inline std::string DescribeLiveCaptureProbeResult(const WpdLiveProbeOutcome& outcome, long long elapsed_ms, const std::string& frame_result) {
    return DescribeLiveCaptureProbeResult(outcome.result, outcome.shutter_sent, elapsed_ms, outcome.stop_reason) +
        " sequence_clean=" + (outcome.sequence_clean ? "1" : "0") + " anomaly=" + outcome.anomaly +
        " live_after=" + outcome.live_after_state + " frame_after=" + frame_result;
}
inline std::string DescribeLiveCaptureProbeBuild() {
    return std::string("live_capture_probe_build=1 cameras=") + kLiveCaptureProbeCameras[0] + " shutter_commands=0 setting_writes=0";
}
}
