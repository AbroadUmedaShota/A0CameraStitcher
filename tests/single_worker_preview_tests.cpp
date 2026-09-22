#include "a0/phase0/single_worker_source.hpp"
#include "a0/phase0/single_worker_preview.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include <iostream>
#include <vector>
using namespace a0::phase0;
namespace {
int failures{};
void Check(bool value, const char* message) { if (!value) { ++failures; std::cerr << message << '\n'; } }
struct Fake {
    std::string fail;
    std::string category{"injected"};
    SdkLoadStage load_stage{SdkLoadStage::none};
    unsigned opens{}, starts{}, reads{}, stops{}, closes{};
    bool ended{};
    void Fault(const char* phase) {
        if (fail == phase) throw TransportError(category, phase, load_stage);
    }
    void OpenSingleWorkerLiveView(std::chrono::seconds) { ++opens; Fault("open"); }
    void StartSingleWorkerLiveView(std::chrono::seconds) { ++starts; Fault("start"); }
    void ValidateSingleWorkerLiveView(std::chrono::seconds) { Fault("topology"); }
    std::vector<unsigned char> ReadLiveViewFrame(std::chrono::seconds) { ++reads; Fault("frame"); return {1,2}; }
    void StopLiveView(std::chrono::seconds) { ++stops; Fault("stop"); }
    void Close(std::chrono::seconds) { ++closes; Fault("close"); ended = fail != "retained"; }
    INikonDualSessionTransport::ExitState InspectDualSessionExitState() const {
        return {!ended || fail == "claim", !ended || fail == "module", !ended || fail == "source"};
    }
};
}
int main() {
    for (const auto& ids : {std::vector<std::uint32_t>{}, {71,83}, {71,71}}) {
        unsigned calls{};
        try { OpenOnlyWorkerSource(ids, [&](auto) { ++calls; }); Check(false, "bad inventory accepted"); }
        catch (const TransportError& e) { Check(e.Category() == "worker_requires_single_source", "wrong rejection"); }
        Check(calls == 0, "source Open before exact-one selection");
    }
    unsigned calls{};
    try { OpenOnlyWorkerSource({71}, [&](auto id) { ++calls; Check(id == 71, "wrong source"); throw TransportError("injected", "Open"); }); }
    catch (const TransportError&) {}
    Check(calls == 1, "Open must not retry");
    const auto observe = [](std::string_view) {}; const auto check = [] {};
    Fake normal; auto result = RunSingleWorkerPreview(normal, observe, check);
    Check(result.Passed() && result.SafeToRelease() && normal.opens == 1 && normal.starts == 1 &&
        normal.reads == 3 && normal.stops == 1 && normal.closes == 1, "normal lifecycle");
    Check(result.source_closed && result.module_closed && result.process_claim_released,
          "checked close reports all three independent exit states");
    Fake typed_open;
    typed_open.fail = "open";
    typed_open.category = "sdk_load_failed";
    typed_open.load_stage = SdkLoadStage::module_library;
    const auto typed_result = RunSingleWorkerPreview(typed_open, observe, check);
    Check(typed_result.error == "sdk_load_failed" &&
              typed_result.sdk_load_stage == SdkLoadStage::module_library &&
              SdkLoadStageToken(typed_result.sdk_load_stage) == "module_library",
          "typed SDK load stage is preserved without changing the error category");
    Check(typed_open.opens == 1 && typed_open.starts == 0 && typed_open.reads == 0 &&
              typed_open.stops == 0 && typed_open.closes == 1 && typed_result.SafeToRelease() &&
              typed_result.source_closed && typed_result.module_closed && typed_result.process_claim_released,
          "typed SDK load stage preserves no-start, no-retry, and checked close behavior");
    Check(result.sdk_load_stage == SdkLoadStage::none &&
              SdkLoadStageToken(SdkLoadStage::none).empty() &&
              SdkLoadStageToken(static_cast<SdkLoadStage>(999)).empty(),
          "absent and unknown stages must not invent diagnostic text");
    for (const auto* failure : {"open", "start", "topology", "frame", "stop", "close", "retained"}) {
        Fake f; f.fail = failure; auto r = RunSingleWorkerPreview(f, observe, check);
        Check(!r.Passed() && f.opens == 1 && f.closes == 1 && f.reads <= 3 && f.stops <= 1, "failure must not retry");
        if (f.fail == "stop" || f.fail == "close" || f.fail == "retained")
            Check(!r.SafeToRelease(), "uncertain stop/close requires lease quarantine");
        if (f.fail == "open") Check(f.starts == 0 && f.reads == 0 && f.stops == 0, "open failure must not start");
        if (f.fail == "start") Check(f.stops == 0 && !r.SafeToRelease(), "failed start must not stop a foreign live view");
        if (f.fail == "frame" || f.fail == "topology") Check(f.stops == 1 && r.SafeToRelease(), "failed preview still closes once");
    }
    Fake deadline;
    for (const auto* retained : {"source", "module", "claim"}) {
        Fake f; f.fail = retained;
        const auto r = RunSingleWorkerPreview(f, observe, check);
        Check(!r.Passed() && !r.SafeToRelease() && !r.closed, "any retained SDK state quarantines");
        Check(r.source_closed == (f.fail != "source") && r.module_closed == (f.fail != "module") &&
              r.process_claim_released == (f.fail != "claim"), "receipt preserves independent state");
    }
    Fake failed_close; failed_close.fail = "close";
    const auto failed_receipt = RunSingleWorkerPreview(failed_close, observe, check);
    Check(!failed_receipt.source_closed && !failed_receipt.module_closed &&
          !failed_receipt.process_claim_released, "failed close emits no successful close flags");
    auto expired = RunSingleWorkerPreview(deadline, observe, [] { throw TransportError("deadline", "expired"); });
    Check(deadline.opens == 0 && deadline.reads == 0 && deadline.closes == 1 && !expired.Passed(), "expired run cannot start");
    std::cout << "{\"mode\":\"simulation\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
