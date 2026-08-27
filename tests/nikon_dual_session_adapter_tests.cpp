#include "a0/phase0/nikon_sdk_transport.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace a0::phase0;
using namespace std::chrono_literals;

namespace {

void Check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class RecordingDualSessionTransport final : public INikonDualSessionTransport {
public:
    std::vector<std::string> tokens{"session-token-a", "session-token-b"};
    DualIdentityInvalidationReason invalidation{DualIdentityInvalidationReason::None};
    bool fail_close{};
    bool fail_capture{};
    bool module_active{};
    bool source_open{};
    bool live_view_active{};
    bool capture_source{};
    std::size_t begin_count{};
    std::size_t source_open_count{};
    std::size_t source_close_count{};
    std::size_t end_count{};
    std::size_t capture_count{};
    std::size_t concurrent_source_violation_count{};
    std::vector<std::string> opened_tokens;

    std::vector<std::string> BeginDualSession(std::chrono::seconds) override {
        ++begin_count;
        module_active = true;
        return tokens;
    }

    void OpenDualCandidateLiveView(
        std::string_view token, std::chrono::seconds) override {
        Open(token, false);
    }

    void OpenDualBoundCapture(
        std::string_view token, std::chrono::seconds) override {
        Open(token, true);
    }

    void StartLiveView(std::chrono::seconds) override {
        if (!source_open || capture_source) throw std::runtime_error("wrong source mode");
        live_view_active = true;
    }

    std::vector<unsigned char> ReadLiveViewFrame(std::chrono::seconds) override {
        if (!live_view_active) throw std::runtime_error("live view is not active");
        return {0xFF, 0xD8, 0xFF, 0xD9};
    }

    void StopLiveView(std::chrono::seconds) override {
        if (!live_view_active) throw std::runtime_error("live view is not active");
        live_view_active = false;
    }

    void CaptureToCard(std::chrono::seconds, std::chrono::seconds) override {
        if (!source_open || !capture_source) throw std::runtime_error("capture source is not open");
        if (fail_capture) throw std::runtime_error("injected capture failure");
        ++capture_count;
    }

    void CloseDualSourceKeepingModule(std::chrono::seconds) override {
        if (fail_close) throw std::runtime_error("injected source close failure");
        if (!source_open || live_view_active) throw std::runtime_error("unsafe source close");
        source_open = false;
        capture_source = false;
        ++source_close_count;
    }

    DualIdentityInvalidationReason PollDualInvalidation() override {
        return invalidation;
    }

    void EndDualSession(std::chrono::seconds) override {
        source_open = false;
        live_view_active = false;
        capture_source = false;
        module_active = false;
        ++end_count;
    }

private:
    void Open(std::string_view token, bool capture) {
        if (!module_active) throw std::runtime_error("module is not active");
        if (source_open) {
            ++concurrent_source_violation_count;
            throw std::runtime_error("two sources were opened concurrently");
        }
        if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) {
            throw std::runtime_error("unknown token");
        }
        source_open = true;
        capture_source = capture;
        ++source_open_count;
        opened_tokens.emplace_back(token);
    }
};

void TestSequentialBindingAndBoundCaptureReuseOneModule() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();
    NikonDualBindingSdkAdapter adapter(transport);

    const auto tokens = adapter.EnumerateCandidates();
    Check(tokens.size() == 2 && transport->begin_count == 1,
        "one manager enumeration must produce exactly two opaque candidates");

    Check(adapter.StartLiveView(0), "CAM-A candidate Live View must start");
    Check(adapter.ReadLiveViewFrame(0).size() == 4,
        "active candidate must return one transient frame");
    Check(adapter.StopLiveView(0), "CAM-A candidate Live View must stop");
    Check(adapter.CloseCandidateSession(0), "CAM-A candidate source must close");
    Check(transport->module_active && !transport->source_open,
        "candidate close must retain only the manager module");

    Check(adapter.StartLiveView(1), "CAM-B candidate Live View must start");
    Check(adapter.StopLiveView(1), "CAM-B candidate Live View must stop");
    Check(adapter.CloseCandidateSession(1), "CAM-B candidate source must close");

    adapter.OpenBoundCapture(tokens[0], 5s);
    adapter.CaptureToCard(5s, 10s);
    adapter.CloseBoundCapture(5s);
    adapter.OpenBoundCapture(tokens[1], 5s);
    adapter.CaptureToCard(5s, 10s);
    adapter.CloseBoundCapture(5s);

    Check(transport->begin_count == 1,
        "bound capture must never re-enumerate SDK candidates");
    Check(transport->capture_count == 2,
        "CAM-A and CAM-B must each capture exactly once");
    Check(transport->concurrent_source_violation_count == 0,
        "candidate and capture source sessions must never overlap");
    Check(transport->module_active && !transport->source_open,
        "manager module must remain available after each source close");

    adapter.EndSession(5s);
    Check(!transport->module_active && transport->end_count == 1,
        "explicit session end must close the retained manager module");
}

void TestSecondLiveViewCannotOverlapFirst() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();
    NikonDualBindingSdkAdapter adapter(transport);
    (void)adapter.EnumerateCandidates();
    Check(adapter.StartLiveView(0), "first candidate Live View must start");
    Check(!adapter.StartLiveView(1),
        "second candidate Live View must be refused while the first is open");
    Check(transport->source_open_count == 1 &&
          transport->concurrent_source_violation_count == 0,
        "overlap refusal must occur before a second source-open call");
}

void TestInvalidationRevokesEveryCandidateWithoutRetry() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();
    NikonDualBindingSdkAdapter adapter(transport);
    const auto tokens = adapter.EnumerateCandidates();
    transport->invalidation = DualIdentityInvalidationReason::UsbReconnect;
    Check(adapter.PollInvalidation() == DualIdentityInvalidationReason::UsbReconnect,
        "typed transport invalidation must cross the adapter boundary");
    bool rejected = false;
    try {
        adapter.OpenBoundCapture(tokens[0], 5s);
    } catch (const TransportError& error) {
        rejected = error.Category() == "candidate_unavailable";
    }
    Check(rejected, "capture must reject every token after invalidation");
    Check(transport->begin_count == 1 && transport->source_open_count == 0,
        "invalidation must not re-enumerate, retry, or open a camera source");
    Check(!transport->module_active && transport->end_count == 1,
        "invalidation must close the retained manager module");
}

void TestDestructorClosesRetainedManager() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();
    {
        NikonDualBindingSdkAdapter adapter(transport);
        (void)adapter.EnumerateCandidates();
        Check(transport->module_active, "test precondition requires an open manager");
    }
    Check(!transport->module_active && transport->end_count == 1,
        "adapter destruction must close a retained manager module");
}

void TestCaptureFailureInvalidatesAndEndsSessionWithoutRetry() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();
    NikonDualBindingSdkAdapter adapter(transport);
    const auto tokens = adapter.EnumerateCandidates();
    adapter.OpenBoundCapture(tokens[0], 5s);
    transport->fail_capture = true;
    bool failed = false;
    try {
        adapter.CaptureToCard(5s, 10s);
    } catch (...) {
        failed = true;
    }
    Check(failed, "capture failure must cross the capture boundary");
    Check(!transport->module_active && transport->end_count == 1,
        "capture failure must tear down the retained manager session");
    Check(adapter.PollInvalidation() == DualIdentityInvalidationReason::SdkError,
        "capture failure must invalidate the operator binding");
    Check(transport->capture_count == 0 && transport->begin_count == 1,
        "capture failure must not retry or re-enumerate");
}

void TestCloseFailureEndsManagerAndInvalidatesSession() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();
    NikonDualBindingSdkAdapter adapter(transport);
    (void)adapter.EnumerateCandidates();
    Check(adapter.StartLiveView(0), "candidate Live View must start");
    Check(adapter.StopLiveView(0), "candidate Live View must stop before close");
    transport->fail_close = true;
    Check(!adapter.CloseCandidateSession(0),
        "source close failure must be reported to the binding dispatcher");
    Check(!transport->module_active && transport->end_count == 1,
        "source close failure must tear down the manager instead of continuing");
    Check(adapter.PollInvalidation() == DualIdentityInvalidationReason::SdkError,
        "source close failure must invalidate the binding as an SDK error");
    Check(transport->begin_count == 1 && transport->capture_count == 0,
        "close failure must not retry or issue a capture command");
}

} // namespace

int main() {
    try {
        TestSequentialBindingAndBoundCaptureReuseOneModule();
        TestSecondLiveViewCannotOverlapFirst();
        TestInvalidationRevokesEveryCandidateWithoutRetry();
        TestCloseFailureEndsManagerAndInvalidatesSession();
        TestDestructorClosesRetainedManager();
        TestCaptureFailureInvalidatesAndEndsSessionWithoutRetry();
        std::cout << "Nikon Dual session adapter contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Nikon Dual session adapter contract failed: "
                  << error.what() << '\n';
        return 1;
    }
}
