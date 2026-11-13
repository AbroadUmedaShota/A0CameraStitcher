// Contracts of the Live-open capture probe (#426 stage 4, ADR-0044 stage P) on a synthetic body: its dedicated
// allowlist, the channel, the event ledger and the whole procedure. No camera and no device. The existing Live View,
// still capture and settings gates are exercised only to show that they did not change.
#include "wpd_still_fake.hpp"
#include "a0/phase0/experimental/d810_preview.hpp"
#include "a0/phase0/experimental/wpd_live_capture_probe.hpp"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
using namespace a0::phase0::experimental;
namespace {
int checks{}, failures{};
void Check(bool value, const char* what) {
    ++checks;
    if (!value) { ++failures; std::cerr << "failed: " << what << '\n'; }
}
bool Contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

// A request of any shape, to probe the allowlist: `read` picks WITH_DATA_TO_READ, otherwise WITHOUT_DATA_PHASE.
WpdValues MakeRequest(bool read, DWORD code, const std::vector<DWORD>& arguments) {
    auto request = BuildWpdLiveViewOperation(read ? WpdLiveViewOperation::Frame : WpdLiveViewOperation::Start);
    Require(request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, code) == S_OK);
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> parameters;
    Require(request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, &parameters) == S_OK);
    for (auto argument : arguments) { PROPVARIANT v{}; v.vt = VT_UI4; v.ulVal = argument; Require(parameters->Add(&v) == S_OK); }
    return request;
}
WpdValues WithCommand(const PROPERTYKEY& key, DWORD code) {
    WpdValues request;
    Require(CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&request)) == S_OK);
    Require(request->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, key.fmtid) == S_OK);
    Require(request->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, key.pid) == S_OK);
    Require(request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, code) == S_OK);
    return request;
}
WpdLiveProbeGateState Lent() { WpdLiveProbeGateState state; state.lent = true; return state; }
WpdLiveProbeGateState Armed() { auto state = Lent(); state.shutter_armed = true; return state; }
bool Allows(IPortableDeviceValues* request, const WpdLiveProbeGateState& state) { return AllowedWpdLiveCaptureProbeRequest(request, state); }

// The synthetic body. Every behaviour the probe may meet is a field; counters say what reached it.
class ProbeFake final : public WpdLiveCaptureProbeChannel {
public:
    ProbeFake() : WpdLiveCaptureProbeChannel([this] { return lent; }, [this] { return admission ? admission() : true; }, [this] { return owned; }) {}
    bool lent = true, owned = true;
    std::function<bool()> admission;
    // The body.
    int live = 1, live_after = 1, quality = 2;
    std::wstring size = L"7360x4912";
    bool fail_live_read = false, bad_info = false, shutter_throws = false;
    int busy_before = 0, busy_after = 0;
    DWORD ready_before = 0x2001, ready_after = 0x2001, shutter_response = 0x2001;
    std::vector<std::vector<WpdStillEvent>> pre, post{std::vector<WpdStillEvent>{{0xc101, 0x0badf00d}, {0xc102, 0}}}; // batches for successive reads before / after the shutter (default: one object and its completion)
    DWORD handle = 0x0badf00d;
    std::vector<unsigned char> jpeg{0xff, 0xd8, 7, 8, 9, 10, 0xff, 0xd9};
    std::function<void()> on_shutter;
    std::function<void(size_t)> on_post_events; // after the n-th events read that follows the shutter
    // What reached the body.
    int shutters{}, object_reads{}, info_reads{}, ends{}, profile_reads{};
    std::vector<std::string> trace;
protected:
    WpdLiveViewExchange Exchange(IPortableDeviceValues* request) override {
        DWORD id{}, code{};
        request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, &id);
        request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, &code);
        DWORD parameter{};
        {
            Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;
            DWORD count{};
            if (request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, &args) == S_OK && args && args->GetCount(&count) == S_OK && count) {
                PROPVARIANT v{};
                if (args->GetAt(0, &v) == S_OK) parameter = v.ulVal;
                PropVariantClear(&v);
            }
        }
        const bool without = id == WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE.pid;
        const bool with_read = id == WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ.pid;
        if (without || with_read) { char text[32]; std::snprintf(text, sizeof text, "%04x", static_cast<unsigned>(code)); trace.push_back(text); }
        if (without) {
            if (code == 0x90c0) {
                ++shutters;
                if (on_shutter) on_shutter();
                if (shutter_throws) throw std::runtime_error("synthetic transport failure");
                auto result = Reply(true);
                result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, shutter_response);
                return {S_OK, result};
            }
            Require(code == 0x90c8);
            DWORD response = 0x2001;
            int& busy = shutters ? busy_after : busy_before;
            if (busy > 0) { --busy; response = 0x2019; } else response = shutters ? ready_after : ready_before;
            auto result = Reply(true);
            result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, response);
            return {S_OK, result};
        }
        if (with_read) {
            current_operation_ = code;
            offset_ = 0;
            if (code == 0x1015) {
                if (parameter == 0xd1a2) {
                    if (fail_live_read) throw std::runtime_error("synthetic transport failure");
                    ++profile_reads;
                    data_ = {static_cast<unsigned char>(shutters ? live_after : live)};
                } else if (parameter == 0x5004) data_ = {static_cast<unsigned char>(quality)};
                else { Require(parameter == 0x5003); data_ = Text(size); }
            } else if (code == 0x90c7) {
                auto& script = shutters ? post : pre;
                size_t& index = shutters ? post_index_ : pre_index_;
                data_ = index < script.size() ? EventsOf(script[index]) : Events({});
                ++index;
                if (shutters && on_post_events) on_post_events(post_index_);
            } else if (code == 0x1008) {
                Require(parameter == handle);
                ++info_reads;
                data_ = bad_info ? Info(0) : Info(jpeg.size());
            } else {
                Require(code == 0x1009 && parameter == handle);
                ++object_reads;
                data_ = jpeg;
            }
            auto result = Reply();
            result->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, L"synthetic-probe-context");
            result->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, data_.size());
            result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE, 3);
            return {S_OK, result};
        }
        if (id == WPD_COMMAND_MTP_EXT_READ_DATA.pid) {
            DWORD requested{};
            request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ, &requested);
            const auto count = std::min<size_t>(requested, data_.size() - offset_);
            auto result = Reply();
            result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, static_cast<DWORD>(count));
            result->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, data_.data() + offset_, static_cast<DWORD>(count));
            offset_ += count;
            return {S_OK, result};
        }
        Require(id == WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER.pid);
        ++ends;
        return {S_OK, Reply(true)};
    }
private:
    std::vector<unsigned char> data_;
    size_t offset_{}, pre_index_{}, post_index_{};
    DWORD current_operation_{};
};
int Count(const ProbeFake& f, const char* op) { return static_cast<int>(std::count(f.trace.begin(), f.trace.end(), op)); }
WpdLiveProbeOptions Fast() {
    WpdLiveProbeOptions o;
    o.ready_wait = std::chrono::milliseconds(100); o.ready_poll = std::chrono::milliseconds(1);
    o.preflight_gap = std::chrono::milliseconds(1);
    o.event_wait = std::chrono::milliseconds(50); o.event_poll = std::chrono::milliseconds(1);
    o.completion = WpdStillCompletionWait{std::chrono::milliseconds(20), std::chrono::milliseconds(1)};
    return o;
}
struct Run {
    WpdLiveProbeOutcome out;
    std::vector<std::string> logs;
    std::vector<unsigned char> partial;
    int saved{};
    std::string All() const { std::string text; for (const auto& line : logs) text += line + "\n"; return text; }
    int IndexOf(std::string_view part) const { for (size_t i = 0; i < logs.size(); ++i) if (Contains(logs[i], part)) return static_cast<int>(i); return -1; }
};
Run Go(ProbeFake& f, bool save_throws = false, bool save_false = false, const std::function<void(const std::string&)>& tap = {}) {
    Run run;
    WpdLiveProbeHooks hooks;
    hooks.partial_sink = [&](std::span<const unsigned char> bytes) { run.partial.insert(run.partial.end(), bytes.begin(), bytes.end()); };
    hooks.save_original = [&](WpdStillPayload&& payload) {
        ++run.saved;
        if (save_throws) throw std::runtime_error("synthetic persist failure");
        return !save_false && !payload.jpeg.empty();
    };
    hooks.log = [&](const std::string& line) { run.logs.push_back(line); if (tap) tap(line); };
    run.out = RunLiveCaptureProbe(f, "CAM-B", hooks, Fast());
    return run;
}
const std::vector<WpdStillEvent> kClean{{0xc101, 0x0badf00d}, {0xc102, 0}};
}
int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;

    // ---- 1..2. The shutter: armed, once, only 0xFFFFFFFF with one unsigned argument.
    {
        auto shutter = MakeRequest(false, 0x90c0, {0xffffffff});
        Check(!Allows(shutter.Get(), Lent()), "the shutter is refused before it is armed");
        Check(Allows(shutter.Get(), Armed()), "the shutter is admitted when armed");
        auto used = Armed(); used.shutter_used = true;
        Check(!Allows(shutter.Get(), used), "a second shutter is refused");
        used.shutter_armed = false;
        Check(!Allows(shutter.Get(), used), "a used shutter is not re-armed by clearing the arm flag");
        for (const DWORD argument : {0u, 0xfffffffeu, 1u, 0x7fffffffu}) {
            auto other = MakeRequest(false, 0x90c0, {argument});
            Check(!Allows(other.Get(), Armed()), "a different shutter argument is refused");
        }
        Check(!Allows(MakeRequest(false, 0x90c0, {}).Get(), Armed()), "a shutter with no argument is refused");
        Check(!Allows(MakeRequest(false, 0x90c0, {0xffffffff, 0}).Get(), Armed()), "a shutter with two arguments is refused");
        {
            auto typed = MakeRequest(false, 0x90c0, {});
            Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> parameters;
            typed->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, &parameters);
            PROPVARIANT v{}; v.vt = VT_I4; v.lVal = -1; parameters->Add(&v);
            Check(!Allows(typed.Get(), Armed()), "a shutter whose argument is not VT_UI4 is refused");
        }
        Check(!Allows(MakeRequest(true, 0x90c0, {0xffffffff}).Get(), Armed()), "the shutter under the data-read command is refused");
        Check(!Allows(nullptr, Armed()), "a null request is refused");
    }

    // ---- 3. Everything else that moves the lens, the Live state, the card or the settings.
    {
        const std::vector<DWORD> forbidden{0x9207, 0x90cb, 0x90c1, 0x9204, 0x9205, 0x9206, 0x90c2, 0x90c3, 0x100b, 0x1014, 0x1016, 0x9201, 0x9202, 0x9203, 0x100e, 0x100f};
        auto state = Armed(); state.attributed_handle = 123;
        for (const DWORD code : forbidden)
            for (const bool read : {false, true})
                for (const auto& arguments : {std::vector<DWORD>{}, std::vector<DWORD>{0}, std::vector<DWORD>{0xffffffff}, std::vector<DWORD>{123}, std::vector<DWORD>{1, 2}})
                    Check(!Allows(MakeRequest(read, code, arguments).Get(), state), "a forbidden operation is refused whatever its shape");
        Check(!Allows(WithCommand(WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE, 0x1016).Get(), state), "WITH_DATA_TO_WRITE is refused");
        Check(!Allows(WithCommand(WPD_COMMAND_MTP_EXT_WRITE_DATA, 0x1016).Get(), state), "WRITE_DATA is refused");
        Check(!Allows(WithCommand(WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE, 0x90c0).Get(), state), "a shutter under WITH_DATA_TO_WRITE is refused");
    }

    // ---- 4. Properties: three reads only.
    {
        for (const DWORD property : {0xd1a2u, 0x5004u, 0x5003u}) Check(Allows(MakeRequest(true, 0x1015, {property}).Get(), Lent()), "an allowed property read");
        for (const DWORD property : {0xd061u, 0xd1a4u, 0x500du, 0xd049u, 0xd161u, 0x5001u, 0xd1b2u, 0u, 0xffffffffu})
            Check(!Allows(MakeRequest(true, 0x1015, {property}).Get(), Lent()), "another property is refused");
        Check(!Allows(MakeRequest(true, 0x1015, {}).Get(), Lent()), "a property read without a code is refused");
        Check(!Allows(MakeRequest(false, 0x1015, {0xd1a2}).Get(), Lent()), "a property read under the no-data command is refused");
        Check(Allows(MakeRequest(false, 0x90c8, {}).Get(), Lent()) && Allows(MakeRequest(true, 0x90c7, {}).Get(), Lent()), "DeviceReady and CheckEvent are admitted");
        Check(!Allows(MakeRequest(false, 0x90c8, {0}).Get(), Lent()) && !Allows(MakeRequest(true, 0x90c7, {0}).Get(), Lent()), "DeviceReady and CheckEvent take no argument");
        Check(!Allows(MakeRequest(true, 0x90c8, {}).Get(), Lent()) && !Allows(MakeRequest(false, 0x90c7, {}).Get(), Lent()), "they are admitted under their own command only");
    }

    // ---- 5. Object info and data: the attributed handle only.
    {
        for (const DWORD code : {0x1008u, 0x1009u}) {
            auto request = MakeRequest(true, code, {123});
            Check(!Allows(request.Get(), Lent()), "an object read is refused before an object is attributed");
            auto state = Lent(); state.attributed_handle = 123;
            Check(Allows(request.Get(), state), "the attributed object is admitted");
            Check(!Allows(MakeRequest(true, code, {124}).Get(), state), "another handle is refused");
            Check(!Allows(MakeRequest(true, code, {0}).Get(), state) && !Allows(MakeRequest(true, code, {0xffffffff}).Get(), state), "an invalid handle is refused");
            state.attributed_handle = 0;
            Check(!Allows(MakeRequest(true, code, {0}).Get(), state), "an invalid attributed handle admits nothing");
            Check(!Allows(MakeRequest(false, code, {123}).Get(), []{ auto s = Lent(); s.attributed_handle = 123; return s; }()), "an object read under the no-data command is refused");
        }
    }

    // ---- 6. A session that is not lent admits nothing, DeviceReady and the data-transfer commands included.
    {
        WpdLiveProbeGateState not_lent; not_lent.shutter_armed = true; not_lent.attributed_handle = 123;
        Check(!Allows(MakeRequest(false, 0x90c8, {}).Get(), not_lent) && !Allows(MakeRequest(true, 0x90c7, {}).Get(), not_lent) &&
            !Allows(MakeRequest(true, 0x1015, {0xd1a2}).Get(), not_lent) && !Allows(MakeRequest(false, 0x90c0, {0xffffffff}).Get(), not_lent) &&
            !Allows(MakeRequest(true, 0x1008, {123}).Get(), not_lent) && !Allows(MakeRequest(true, 0x1009, {123}).Get(), not_lent), "nothing is admitted unless the session is lent");
        Check(!Allows(BuildWpdLiveViewRead(L"probe", 32).Get(), not_lent) && !Allows(BuildWpdLiveViewEndTransfer(L"probe").Get(), not_lent), "READ_DATA and END_DATA_TRANSFER need the lent session too");
        Check(Allows(BuildWpdLiveViewRead(L"probe", 32).Get(), Lent()) && Allows(BuildWpdLiveViewEndTransfer(L"probe").Get(), Lent()), "they pass for the lent session");
    }

    // ---- 7. The existing gates did not change.
    {
        // Live View gate: none of the probe's operations.
        Check(AllowedD810PreviewRequest(BuildWpdStillRequest(WpdStillOperation::Ready).Get()), "Live gate: DeviceReady is as before");
        Check(AllowedD810PreviewRequest(BuildWpdLiveViewOperation(WpdLiveViewOperation::Frame).Get()), "Live gate: a frame read is as before");
        Check(!AllowedD810PreviewRequest(BuildWpdStillRequest(WpdStillOperation::CaptureNoAf).Get()), "Live gate refuses the shutter");
        Check(!AllowedD810PreviewRequest(BuildWpdStillRequest(WpdStillOperation::Events).Get()), "Live gate refuses CheckEvent");
        Check(!AllowedD810PreviewRequest(BuildWpdStillRequest(WpdStillOperation::ObjectInfo, 123).Get()) && !AllowedD810PreviewRequest(BuildWpdStillRequest(WpdStillOperation::Object, 123).Get()), "Live gate refuses object reads");
        Check(!AllowedD810PreviewRequest(MakeRequest(false, 0x9207, {0}).Get()) && !AllowedD810PreviewRequest(MakeRequest(false, 0x9205, {1, 2}).Get()), "Live gate refuses 0x9207 and the AF frame");
        // Still gate: Live View control and the AF family stay outside, the shutter still needs its arm.
        auto still_shutter = BuildWpdStillRequest(WpdStillOperation::CaptureNoAf);
        Check(!AllowedWpdStillRequest(still_shutter.Get(), false, {}) && AllowedWpdStillRequest(still_shutter.Get(), true, {}), "still gate: the shutter needs its arm");
        for (const DWORD code : {0x9201u, 0x9202u, 0x9203u, 0x9207u, 0x90cbu, 0x90c1u, 0x9204u, 0x9205u}) {
            still_shutter->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, code);
            Check(!AllowedWpdStillRequest(still_shutter.Get(), true, 123), "still gate refuses Live View control and the AF family");
        }
        // The preflight of a still capture still wants Live View off.
        Fake body; body.live_on = true;
        bool refused = false;
        try { ReadWpdStillPreflight(body, [] { return true; }); } catch (const std::runtime_error& e) { refused = Contains(e.what(), "Live View off"); }
        Check(refused && body.shutters == 0, "the still preflight still stops on Live View on");
    }

    // ---- The ledger.
    {
        WpdLiveProbeEventLedger before;
        before.Observe(std::vector<WpdStillEvent>{{0x4006, 1}, {0xc104, 2}}, false);
        Check(!before.StopCode() && !before.RefreshRequired(), "ledger: property and preview-image events are read through");
        for (const std::uint16_t code : std::initializer_list<std::uint16_t>{0xc101, 0xc102, 0x400d, 0x4002, 0x4005, 0x4fff}) {
            WpdLiveProbeEventLedger one;
            one.Observe(std::vector<WpdStillEvent>{{code, 1}}, false);
            Check(one.StopCode() == code, "ledger: these codes end the probe before the shutter");
        }
        for (const std::uint16_t code : std::initializer_list<std::uint16_t>{0x4004, 0x4008, 0xc10c}) {
            WpdLiveProbeEventLedger one;
            one.Observe(std::vector<WpdStillEvent>{{code, 1}}, false);
            Check(!one.StopCode() && one.RefreshRequired(), "ledger: a state change asks for one re-check");
        }
        WpdLiveProbeEventLedger after;
        after.Observe(std::vector<WpdStillEvent>{{0xc101, 77}, {0x4006, 0}, {0xc102, 0}}, true);
        Check(after.Pristine() && after.Handle() == 77u && after.Completed() && after.CompletionCode() == 0xc102 && after.Unexpected() == 0, "ledger: one object and one completion is pristine");
        WpdLiveProbeEventLedger twice;
        twice.Observe(std::vector<WpdStillEvent>{{0xc101, 77}, {0xc101, 78}}, true);
        Check(twice.Ambiguous() && !twice.Handle() && !twice.ImportAllowed(), "ledger: two objects are ambiguous");
        WpdLiveProbeEventLedger card;
        card.Observe(std::vector<WpdStillEvent>{{0xc101, 77}, {0x4002, 5}}, true);
        Check(card.CardImageEvent() && !card.ImportAllowed(), "ledger: a card image event forbids the import");
        for (const std::uint16_t code : std::initializer_list<std::uint16_t>{0x4004, 0x4008, 0xc10c, 0x4005, 0x4fff}) {
            WpdLiveProbeEventLedger odd;
            odd.Observe(std::vector<WpdStillEvent>{{0xc101, 77}, {code, 0}}, true);
            Check(odd.ImportAllowed() && !odd.Pristine() && odd.Unexpected() == 1, "ledger: an odd event keeps the image out of original");
        }
        WpdLiveProbeEventLedger invalid;
        invalid.Observe(std::vector<WpdStillEvent>{{0xc101, 0xffffffff}}, true);
        Check(!invalid.Handle() && !invalid.ImportAllowed(), "ledger: an invalid handle is not an object");
        WpdLiveProbeEventLedger twice_done;
        twice_done.Observe(std::vector<WpdStillEvent>{{0xc101, 77}, {0xc102, 0}, {0x400d, 0}}, true);
        Check(!twice_done.Pristine() && twice_done.CompletionEvents() == 2, "ledger: two completions are not pristine");
        Check(DescribeLiveProbeEventCounts(after.Counts()) == "0x4006:1,0xc101:1,0xc102:1" && DescribeLiveProbeEventCounts({}) == "none", "ledger: counts are codes and numbers");
    }

    // ---- 14. The clean flow.
    {
        ProbeFake f;
        const auto run = Go(f);
        const auto& o = run.out;
        Check(o.result == WpdLiveProbeResult::Observed && o.shutter_sent && f.shutters == 1, "clean: observed, one shutter");
        Check(o.ready == 0x2001 && o.live == 1 && o.quality == 2 && o.size_ok && !o.refresh, "clean: the preflight values");
        Check(o.after_ready == 0x2001 && o.busy_polls == 0, "clean: ready after the shutter");
        Check(o.attributed && o.bytes == f.jpeg.size() && o.width == 7360 && o.height == 4912 && o.published == "original" && o.object_reason == "verified", "clean: one object imported and published as original");
        Check(run.saved == 1 && run.partial == f.jpeg && f.object_reads == 1 && f.info_reads == 1, "clean: saved once, the partial holds the bytes, read once");
        Check(o.completion.complete_event && o.completion.complete_event_code == 0xc102 && o.completion.completion_wait == WpdStillCompletionWaitResult::AlreadySeen, "clean: the completion was seen");
        Check(o.live_after == 1u && o.out_of_focus == false && !o.sdram_may_remain, "clean: Live read on after the shot, nothing left in the camera");
        Check(o.object_events == 1 && o.completion_events == 1 && o.unexpected == 0 && o.after_events == "0xc101:1,0xc102:1", "clean: event counts");
        Check(Count(f, "90c0") == 1 && f.ends >= 5, "clean: one 0x90c0 and every transfer ended");
        // The lines are in the planned order and carry codes and numbers only.
        const auto all = run.All();
        Check(run.IndexOf("live_capture_probe_preflight") >= 0 && run.IndexOf("live_capture_probe_preflight") < run.IndexOf("shutter_no_af_once=1 card_write_command=0 live_open=1") &&
            run.IndexOf("shutter_no_af_once=1") < run.IndexOf("shutter_result_error=0") && run.IndexOf("shutter_result_error=0") < run.IndexOf("live_capture_probe_after_shutter_ready=0x2001") &&
            run.IndexOf("live_capture_probe_after_shutter_ready") < run.IndexOf("live_capture_probe_events phase=after_shutter") && run.IndexOf("live_capture_probe_events") < run.IndexOf("live_capture_probe_object") &&
            run.IndexOf("live_capture_probe_object") < run.IndexOf("capture_completion shutter_attempted=1 complete_event=1 complete_event_code=0xc102") &&
            run.IndexOf("capture_completion") < run.IndexOf("completion_wait result=already_seen") && run.IndexOf("completion_wait") < run.IndexOf("live_capture_probe_live_after value=0x01"), "clean: the log lines appear in the planned order");
        Check(Contains(all, "CAM-B live_capture_probe_preflight ready=0x2001 live=1 quality=2 size_ok=1 refresh=0 events=none result=ready reason=none") &&
            Contains(all, "live_capture_probe_object attributed=1 bytes=8 width=7360 height=4912 published=original reason=verified"), "clean: the preflight and object lines");
        Check(!Contains(all, "badf00d") && !Contains(all, "195948557") && !Contains(all, "synthetic-probe-context"), "clean: no object handle or transfer context in the log");
        Check(Contains(DescribeLiveCaptureProbeResult(o.result, o.shutter_sent, 1234, o.stop_reason), "live_capture_probe_result=observed shutter_commands=1 resend=0 setting_writes=0 live_start_commands=0 elapsed_ms=1234"), "clean: the result line");
        // Only the allowed operations ever reached the body, and none of the forbidden ones.
        for (const auto& op : f.trace) Check(op == "90c8" || op == "1015" || op == "90c7" || op == "90c0" || op == "1008" || op == "1009", "clean: only allowed operations reached the body");
    }

    // ---- 8. The state before the shutter.
    {
        const auto refuse = [](ProbeFake& f, const char* reason, const char* what) {
            const auto run = Go(f);
            Check(run.out.result == WpdLiveProbeResult::StoppedBeforeShutter && f.shutters == 0 && !run.out.shutter_sent && run.out.stop_reason == reason && run.saved == 0, what);
            Check(run.IndexOf("result=refused reason=" + std::string(reason)) >= 0, "the preflight line names the reason");
            Check(run.IndexOf("shutter_no_af_once") < 0, "no shutter line when the shutter was not sent");
            return run;
        };
        for (const int live : {0, 2}) { ProbeFake f; f.live = live; const auto run = refuse(f, "live_not_on", "Live not on stops before the shutter"); Check(run.out.live == live, "the Live value is recorded"); }
        { ProbeFake f; f.fail_live_read = true; refuse(f, "profile_read_failed", "an unreadable Live state stops"); }
        { ProbeFake f; f.quality = 4; refuse(f, "quality_not_jpeg_fine", "not JPEG Fine stops"); }
        { ProbeFake f; f.size = L"5520x3680"; refuse(f, "size_not_fx_l", "not FX L stops"); }
        { ProbeFake f; f.busy_before = 5; const auto run = Go(f); Check(run.out.result == WpdLiveProbeResult::Observed && f.shutters == 1, "a few busy answers are waited out"); }
        { ProbeFake f; f.busy_before = 100000; refuse(f, "ready_timeout", "a camera that stays busy stops"); }
        { ProbeFake f; f.ready_before = 0xa002; refuse(f, "out_of_focus_before_shutter", "OutOfFocus before the shutter stops"); }
        { ProbeFake f; f.ready_before = 0xa004; refuse(f, "ready_rejected", "another refusal of DeviceReady stops"); }
        // Events waiting before the shutter.
        for (const std::uint16_t code : std::initializer_list<std::uint16_t>{0xc101, 0xc102, 0x400d, 0x4002, 0x4005, 0x4fff}) {
            ProbeFake f; f.pre = {{{code, 9}}};
            const auto run = refuse(f, ("event_" + WpdStillEventCodeText(code) + "_before_shutter").c_str(), "an image, completion, card image, store removal or unnamed event stops");
            Check(Contains(run.out.preflight_events, WpdStillEventCodeText(code) + ":1"), "the event code is recorded");
        }
        { ProbeFake f; f.pre = {{{0x4004, 0}}}; const auto run = Go(f); Check(run.out.refresh && run.out.result == WpdLiveProbeResult::Observed && f.shutters == 1 && f.profile_reads >= 2, "StoreAdded asks for exactly one re-check, then the shot goes ahead"); }
        { ProbeFake f; f.pre = {{{0xc10c, 0}}, {}, {{0x4008, 0}}}; const auto run = Go(f); Check(run.out.refresh && run.out.result == WpdLiveProbeResult::StoppedBeforeShutter && run.out.stop_reason == "state_did_not_settle" && f.shutters == 0, "a second change after the re-check stops"); }
        { ProbeFake f; f.pre = {{{0x4006, 0}, {0xc104, 0}}}; const auto run = Go(f); Check(run.out.result == WpdLiveProbeResult::Observed && Contains(run.out.preflight_events, "0x4006:1"), "property and preview-image events do not stop"); }
        { ProbeFake f; f.lent = false; const auto run = Go(f); Check(run.out.result == WpdLiveProbeResult::StoppedBeforeShutter && f.trace.empty() && run.out.stop_reason == "ready_failed", "a session that is not lent sees no I/O at all"); }
        { ProbeFake f; f.owned = false; const auto run = Go(f); Check(run.out.result == WpdLiveProbeResult::StoppedBeforeShutter && f.trace.empty(), "a closed session sees no I/O at all"); }
        // The borrowed session is taken back between the baseline and the shutter: the shutter is not admitted.
        { ProbeFake f; const auto run = Go(f, false, false, [&](const std::string& line) { if (Contains(line, "result=ready")) f.lent = false; });
            Check(f.shutters == 0 && !run.out.shutter_sent && run.out.stop_reason == "shutter_not_admitted" && Count(f, "90c0") == 0, "a stale lease cannot send the shutter"); }
    }

    // ---- 15. Deadline and stop.
    {
        { ProbeFake f; bool stop = true; f.admission = [&] { return !stop; }; const auto run = Go(f); Check(f.shutters == 0 && f.trace.empty() && run.out.stop_reason == "deadline_or_stop", "a stop before anything is sent sends nothing"); }
        { ProbeFake f; bool stop = false; f.admission = [&] { return !stop || f.ShutterAttempted(); };
            const auto run = Go(f, false, false, [&](const std::string& line) { if (Contains(line, "result=ready")) stop = true; }); Check(f.shutters == 0 && run.out.result == WpdLiveProbeResult::StoppedBeforeShutter && run.out.stop_reason == "deadline_or_stop", "a stop requested before the shutter keeps the shutter back"); }
        { ProbeFake f; bool stop = false; f.on_shutter = [&] { stop = true; }; f.admission = [&] { return !stop || f.ShutterAttempted(); };
            const auto run = Go(f); Check(f.shutters == 1 && run.saved == 1 && run.out.published == "original" && run.out.result == WpdLiveProbeResult::Observed, "a stop after the shutter still imports and saves the image"); }
        { ProbeFake f; bool expired = false; f.on_shutter = [&] { expired = true; }; f.admission = [&] { return !expired; };
            const auto run = Go(f); Check(f.shutters == 1 && f.object_reads == 0 && run.saved == 0 && run.out.result == WpdLiveProbeResult::StoppedAfterShutter && run.out.sdram_may_remain, "an expired deadline after the shutter starts no new I/O"); }
    }

    // ---- 10. The shutter's answer.
    {
        for (const DWORD response : {0xa00bu, 0xa004u, 0x2019u, 0x2002u}) {
            ProbeFake f; f.shutter_response = response;
            const auto run = Go(f);
            const auto& o = run.out;
            Check(f.shutters == 1 && Count(f, "90c0") == 1 && o.shutter_sent && o.result == WpdLiveProbeResult::StoppedAfterShutter && o.stop_reason == "shutter_refused_by_body", "a refused shutter is recorded and not resent");
            Check(f.object_reads == 0 && f.info_reads == 0 && run.saved == 0 && o.published == "none", "a refused shutter imports nothing");
            Check(o.shutter.error == WpdLiveViewResultError::MtpRejected && o.shutter.mtp_response == response && o.after_ready == response, "the response code is recorded");
            Check(run.IndexOf("shutter_result_error=" + std::to_string(static_cast<int>(WpdLiveViewResultError::MtpRejected))) >= 0 && run.IndexOf("mtp=0x") >= 0, "the response line names the code");
            Check(o.live_after == 1u, "Live is still read once after a refusal");
            Check(!o.sdram_may_remain, "a refusal leaves nothing in the camera");
        }
        {   // A transport failure: the exposure is unknown. The candidate is kept as a private partial only.
            ProbeFake f; f.shutter_throws = true;
            const auto run = Go(f);
            const auto& o = run.out;
            Check(f.shutters == 1 && o.shutter_sent && !o.shutter_result_known && o.stop_reason == "shutter_unconfirmed" && o.result == WpdLiveProbeResult::StoppedAfterShutter, "a transport failure is uncertain, not resent");
            Check(o.attributed && o.published == "partial_only" && run.saved == 0 && run.partial == f.jpeg && o.object_reason == "shutter_unconfirmed", "an uncertain shutter's image is a partial only");
            Check(run.IndexOf("shutter_result_error=none transport_exception=1") >= 0 && run.IndexOf("capture_completion") < 0, "the line says the transport failed");
        }
        {   // A transport failure with no image to find.
            ProbeFake f; f.shutter_throws = true; f.post.clear();
            const auto run = Go(f);
            Check(f.shutters == 1 && run.out.published == "none" && run.saved == 0 && run.out.sdram_may_remain && f.object_reads == 0, "an uncertain shutter with no event imports nothing");
        }
        {   // A second ShutterOnce on a used channel is refused whatever the first did.
            ProbeFake f; Go(f);
            bool second = false;
            try { f.ShutterOnce(); } catch (const std::runtime_error&) { second = true; }
            Check(second && f.shutters == 1, "the channel's shutter is single-use");
            bool reused = false;
            try { Go(f); } catch (const std::logic_error&) { reused = true; }
            Check(reused && Count(f, "90c0") == 1, "a used channel cannot start a second run, still one shutter on the wire");
        }
    }

    // ---- 11..13. After the shutter.
    {
        {   // OutOfFocus after the shutter: recorded, never resent, the image is a partial only.
            ProbeFake f; f.ready_after = 0xa002;
            const auto run = Go(f);
            const auto& o = run.out;
            Check(o.out_of_focus && o.after_ready == 0xa002 && f.shutters == 1, "OutOfFocus after the shutter is recorded, not resent");
            Check(o.attributed && o.published == "partial_only" && run.saved == 0 && o.object_reason == "after_shutter_not_ready" && o.result == WpdLiveProbeResult::Observed, "its image is a partial only");
        }
        {   // The camera stays busy after the shutter: waited, recorded, the image is a partial only.
            ProbeFake f; f.busy_after = 3;
            const auto run = Go(f);
            Check(run.out.busy_polls == 3 && run.out.after_ready == 0x2001 && run.saved == 1, "busy answers after the shutter are counted");
            ProbeFake g; g.busy_after = 100000;
            const auto timed_out = Go(g);
            Check(timed_out.out.after_ready == 0x2019 && timed_out.out.busy_polls > 0 && timed_out.out.published == "partial_only" && timed_out.saved == 0, "a camera that stays busy leaves a partial only");
        }
        {   // Two objects, a card image, no event at all.
            ProbeFake f; f.post = {{{0xc101, 0x0badf00d}, {0xc101, 0x0badf00e}}};
            auto run = Go(f);
            Check(f.info_reads == 0 && f.object_reads == 0 && !run.out.attributed && run.out.published == "none" && run.out.object_reason == "ambiguous_object_events" && run.saved == 0, "two objects: nothing is imported");
            ProbeFake g; g.post = {{{0xc101, 0x0badf00d}, {0x4002, 3}}};
            run = Go(g);
            Check(g.info_reads == 0 && g.object_reads == 0 && run.out.object_reason == "card_image_event" && run.saved == 0, "a card image event: nothing is imported");
            ProbeFake h; h.post.clear();
            run = Go(h);
            Check(h.info_reads == 0 && run.out.object_reason == "object_event_timeout" && run.out.published == "none" && run.out.sdram_may_remain && run.out.result == WpdLiveProbeResult::StoppedAfterShutter, "no object event within the bound: nothing is imported");
            Check(Count(h, "90c0") == 1, "no resend when the image never arrives");
        }
        {   // The object arrives after a few empty reads.
            ProbeFake f; f.post = {{}, {}, {}, kClean};
            const auto run = Go(f);
            Check(run.out.published == "original" && run.saved == 1, "an object that arrives late is still found inside the bound");
        }
        // 0xC10C, 0x4008, 0x4004, 0x4005 and an unnamed code after the shutter: recorded, the image is a partial only.
        for (const std::uint16_t code : std::initializer_list<std::uint16_t>{0xc10c, 0x4008, 0x4004, 0x4005, 0x4fff}) {
            ProbeFake f; f.post = {{{0xc101, 0x0badf00d}, {code, 0}, {0xc102, 0}}};
            const auto run = Go(f);
            Check(run.out.attributed && run.out.published == "partial_only" && run.saved == 0 && run.partial == f.jpeg && run.out.unexpected == 1 && run.out.object_reason == "unexpected_event", "an odd event after the shutter keeps the image out of original");
            Check(Contains(run.out.after_events, WpdStillEventCodeText(code) + ":1") && run.out.result == WpdLiveProbeResult::Observed && f.shutters == 1, "the odd code is recorded and nothing is resent");
        }
        {   // An odd event that arrives only after the image was read still keeps it out of original.
            ProbeFake f; f.post = {{{0xc101, 0x0badf00d}}, {{0xc10c, 0}}, {{0xc102, 0}}};
            const auto run = Go(f);
            Check(run.out.published == "partial_only" && run.saved == 0 && run.out.unexpected == 1 && run.out.completion.complete_event, "a late odd event is still seen");
        }
        {   // No completion arrives: observation times out, the image is still original.
            ProbeFake f; f.post = {{{0xc101, 0x0badf00d}}};
            const auto run = Go(f);
            Check(run.out.published == "original" && !run.out.completion.complete_event && run.out.completion.completion_wait == WpdStillCompletionWaitResult::TimedOut && run.saved == 1, "a missing completion is recorded and does not fail the capture");
            Check(run.IndexOf("capture_completion shutter_attempted=1 complete_event=0 complete_event_code=none") >= 0 && run.IndexOf("completion_wait result=timeout") >= 0, "the completion lines use the existing form");
        }
        {   // A completion that comes with the wait.
            ProbeFake f; f.post = {{{0xc101, 0x0badf00d}}, {}, {}, {{0x400d, 0}}};
            const auto run = Go(f);
            Check(run.out.completion.completion_wait == WpdStillCompletionWaitResult::Seen && run.out.completion.complete_event_code == 0x400d && run.saved == 1, "a completion that arrives during the wait is seen");
        }
        {   // A broken object description or byte count.
            ProbeFake f; f.bad_info = true;
            const auto run = Go(f);
            Check(run.out.object_reason == "object_info_invalid" && f.object_reads == 0 && run.saved == 0 && run.out.sdram_may_remain && run.out.result == WpdLiveProbeResult::StoppedAfterShutter, "a bad object description imports nothing");
        }
        {   // A save that fails or throws leaves a partial only and is not a success.
            ProbeFake f; const auto run = Go(f, true);
            Check(run.saved == 1 && run.out.published == "partial_only" && run.out.object_reason == "persist_failed", "a save that throws leaves a partial only");
            ProbeFake g; const auto again = Go(g, false, true);
            Check(again.saved == 1 && again.out.published == "partial_only" && again.out.object_reason == "persist_failed", "a save that reports failure leaves a partial only");
        }
        {   // Live View ended with the shot, or cannot be read.
            ProbeFake f; f.live_after = 0;
            const auto run = Go(f);
            Check(run.out.live_after == 0u && run.out.published == "original" && run.out.result == WpdLiveProbeResult::Observed && run.IndexOf("live_after value=0x00") >= 0, "Live off after the shot is recorded");
            ProbeFake g; g.post = {kClean};
            bool over = false; g.on_post_events = [&](size_t read) { if (read >= 3) over = true; }; g.admission = [&] { return !over; }; // the deadline runs out right after the import
            const auto expired = Go(g);
            Check(!expired.out.live_after && expired.IndexOf("live_after value=unreadable") >= 0 && expired.out.result == WpdLiveProbeResult::StoppedAfterShutter, "Live state not readable after the deadline is recorded as such");
        }
    }
    std::cout << "wpd_live_capture_probe_checks=" << checks << " failures=" << failures << '\n';
    CoUninitialize();
    return failures ? 1 : 0;
}
