#include "a0/phase0/dual_hardware_capture_backend.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
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

void TestReadOnlySourceWaitReturnsNormalizedPublishedIds() {
    const auto started = std::chrono::steady_clock::time_point{};
    auto current = started;
    std::size_t pump_count = 0;
    std::size_t read_count = 0;
    std::size_t wait_count = 0;

    const auto ids = WaitForNikonSdkReadOnlySourceIds(
        started + 5s,
        [&] { ++pump_count; },
        [&] {
            ++read_count;
            return read_count == 1
                ? std::vector<std::uint32_t>{}
                : std::vector<std::uint32_t>{20, 10, 20};
        },
        [&] { ++wait_count; current += 50ms; },
        [&] { return current; });

    Check(ids == std::vector<std::uint32_t>{10, 20} &&
              pump_count == 2 && read_count == 2 && wait_count == 1,
        "read-only source wait must normalize published ids and stop immediately on success");
}

void TestReadOnlySourceWaitRejectsDeadlineReachedDuringCallbacks() {
    const auto started = std::chrono::steady_clock::time_point{};
    for (const auto elapsed : {50ms, 75ms}) {
        for (const bool expire_during_pump : {false, true}) {
            auto current = started;
            std::size_t pump_count = 0;
            std::size_t read_count = 0;
            std::size_t wait_count = 0;
            bool timeout_typed = false;
            try {
                (void)WaitForNikonSdkReadOnlySourceIds(
                    started + 50ms,
                    [&] {
                        ++pump_count;
                        if (expire_during_pump) current = started + elapsed;
                    },
                    [&] {
                        ++read_count;
                        current = started + elapsed;
                        return std::vector<std::uint32_t>{10, 20};
                    },
                    [&] { ++wait_count; },
                    [&] { return current; });
            } catch (const TransportError& error) {
                timeout_typed =
                    error.Category() == "dual_read_only_source_wait_timeout";
            }
            Check(timeout_typed && pump_count == 1 &&
                      read_count == (expire_during_pump ? 0u : 1u) && wait_count == 0,
                "expired source callbacks must not publish late ids or start another operation");
        }
    }
}

void TestReadOnlySourceWaitHasDeterministicTimeoutAndFailureCategories() {
    const auto started = std::chrono::steady_clock::time_point{};
    auto current = started;
    std::size_t pump_count = 0;
    std::size_t wait_count = 0;
    bool timeout_typed = false;
    try {
        (void)WaitForNikonSdkReadOnlySourceIds(
            started + 150ms,
            [&] { ++pump_count; },
            [] { return std::vector<std::uint32_t>{}; },
            [&] { ++wait_count; current += 50ms; },
            [&] { return current; });
    } catch (const TransportError& error) {
        timeout_typed =
            error.Category() == "dual_read_only_source_wait_timeout";
    }
    Check(timeout_typed && pump_count == 3 && wait_count == 3,
        "empty source publication must stop at the injected deadline with no hidden retry");

    current = started;
    wait_count = 0;
    timeout_typed = false;
    try {
        (void)WaitForNikonSdkReadOnlySourceIds(
            started + 50ms,
            [] {},
            [&] {
                current = started + 50ms;
                return std::vector<std::uint32_t>{};
            },
            [&] { ++wait_count; },
            [&] { return current; });
    } catch (const TransportError& error) {
        timeout_typed =
            error.Category() == "dual_read_only_source_wait_timeout";
    }
    Check(timeout_typed && wait_count == 0,
        "source reads that reach the deadline must not schedule another wait");

    std::size_t read_count = 0;
    bool failure_typed = false;
    try {
        (void)WaitForNikonSdkReadOnlySourceIds(
            started + 5s,
            [] {},
            [&]() -> std::vector<std::uint32_t> {
                ++read_count;
                throw std::runtime_error("injected-sensitive-source-read-detail");
            },
            [] {},
            [started] { return started; });
    } catch (const TransportError& error) {
        failure_typed =
            error.Category() == "dual_read_only_source_wait_failed" &&
            std::string{error.what()}.find("injected-sensitive") ==
                std::string::npos;
    }
    Check(failure_typed && read_count == 1,
        "source wait exceptions must be normalized without leaking details or retrying");

    std::size_t wait_failure_count = 0;
    failure_typed = false;
    try {
        (void)WaitForNikonSdkReadOnlySourceIds(
            started + 5s,
            [] {},
            [] { return std::vector<std::uint32_t>{}; },
            [&] {
                ++wait_failure_count;
                throw std::runtime_error("injected-sensitive-wait-detail");
            },
            [started] { return started; });
    } catch (const TransportError& error) {
        failure_typed =
            error.Category() == "dual_read_only_source_wait_failed" &&
            std::string{error.what()}.find("injected-sensitive") ==
                std::string::npos;
    }
    Check(failure_typed && wait_failure_count == 1,
        "source wait scheduling exceptions must be normalized without leaking details or retrying");
}

class RecordingInventorySource {
public:
    std::size_t fail_close_number{};
    std::size_t unknown_close_number{};
    std::size_t open_count{};
    std::size_t inspect_count{};
    std::size_t close_count{};
    std::size_t concurrent_open_count{};
    bool source_open{};
    bool close_unconfirmed{};
    std::uint32_t current_source{};
    std::vector<std::string> events;

    void Open(std::uint32_t source_id) {
        if (source_open) {
            ++concurrent_open_count;
            throw std::runtime_error("inventory source overlap");
        }
        source_open = true;
        current_source = source_id;
        ++open_count;
        events.emplace_back("open:" + std::to_string(source_id));
    }

    bool InspectIsD810() {
        if (!source_open) throw std::runtime_error("inventory inspect without open source");
        ++inspect_count;
        events.emplace_back("inspect:" + std::to_string(current_source));
        return current_source != 30;
    }

    bool CloseOnce() {
        if (!source_open) throw std::runtime_error("inventory close without open source");
        ++close_count;
        events.emplace_back("close:" + std::to_string(current_source));
        if (unknown_close_number != 0 && close_count == unknown_close_number) {
            throw std::runtime_error("inventory close completion unknown");
        }
        if (fail_close_number != 0 && close_count == fail_close_number) {
            return false;
        }
        source_open = false;
        return true;
    }
};

void TestInventoryCloseFailureStopsBeforeOpeningNextSource() {
    RecordingInventorySource source;
    source.fail_close_number = 1;
    bool typed_failure = false;
    std::size_t successful_returns = 0;

    try {
        (void)InspectNikonD810InventorySources(
            {10, 20},
            [&](std::uint32_t id) { source.Open(id); },
            [&] { return source.InspectIsD810(); },
            [&] { return source.CloseOnce(); },
            source.close_unconfirmed);
        ++successful_returns;
    } catch (const TransportError& error) {
        typed_failure = error.Category() == "inventory_close_failed";
    }

    Check(typed_failure,
        "an unconfirmed inventory source close must return a typed failure");
    Check(source.open_count == 1 && source.inspect_count == 1 &&
              source.close_count == 1 && source.concurrent_open_count == 0,
        "close failure must stop before opening, inspecting, or closing a second source");
    Check(source.source_open && source.close_unconfirmed && successful_returns == 0,
        "unconfirmed source state must not be rewritten as closed or publish candidates");
    Check(source.events == std::vector<std::string>{
              "open:10", "inspect:10", "close:10"},
        "inventory close failure must have no retry or later-source operation");
}

void TestInventoryUnknownCloseCompletionStopsWithoutRetry() {
    RecordingInventorySource source;
    source.unknown_close_number = 1;
    bool typed_failure = false;

    try {
        (void)InspectNikonD810InventorySources(
            {10, 20},
            [&](std::uint32_t id) { source.Open(id); },
            [&] { return source.InspectIsD810(); },
            [&] { return source.CloseOnce(); },
            source.close_unconfirmed);
    } catch (const TransportError& error) {
        typed_failure = error.Category() == "inventory_close_failed";
    }

    Check(typed_failure && source.source_open && source.close_unconfirmed,
        "unknown close completion must remain open-state uncertain and typed failed");
    Check(source.open_count == 1 && source.close_count == 1 &&
              source.concurrent_open_count == 0,
        "unknown close completion must not retry close or open the next source");
}

void TestInventoryWalkRemainsSequentialWhenEveryCloseIsConfirmed() {
    RecordingInventorySource source;

    const auto d810_ids = InspectNikonD810InventorySources(
        {10, 20},
        [&](std::uint32_t id) { source.Open(id); },
        [&] { return source.InspectIsD810(); },
        [&] { return source.CloseOnce(); },
        source.close_unconfirmed);

    Check(d810_ids == std::vector<std::uint32_t>{10, 20},
        "both inspected D810 ids may be returned only after checked close");
    Check(source.open_count == 2 && source.inspect_count == 2 &&
              source.close_count == 2 && source.concurrent_open_count == 0 &&
              !source.source_open && !source.close_unconfirmed,
        "normal two-camera inventory must preserve one open/read/checked-close sequence per source");
    Check(source.events == std::vector<std::string>{
              "open:10", "inspect:10", "close:10",
              "open:20", "inspect:20", "close:20"},
        "normal inventory must never open the next source before checked close");
}

class RecordingDualSessionTransport final : public INikonDualSessionTransport {
public:
    std::vector<std::string> tokens{"session-token-a", "session-token-b"};
    DualIdentityInvalidationReason invalidation{DualIdentityInvalidationReason::None};
    bool fail_close{};
    bool fail_capture{};
    bool fail_read_only_start{};
    bool fail_read_only_with_non_transport_exception{};
    bool read_only_close_unconfirmed{};
    bool read_only_close_throws{};
    RecordingInventorySource read_only_inventory;
    std::string read_only_failure_category;
    bool fail_end_after_cleanup{};
    bool fail_end_before_cleanup{};
    std::size_t read_only_d810_count{2};
    bool process_claimed{};
    bool module_active{};
    bool source_open{};
    bool live_view_active{};
    bool capture_source{};
    std::size_t begin_count{};
    std::size_t read_only_begin_count{};
    std::size_t source_open_count{};
    std::size_t source_close_count{};
    std::size_t end_count{};
    std::size_t capture_count{};
    std::size_t status_probe_count{};
    std::size_t invalidation_poll_count{};
    std::size_t concurrent_source_violation_count{};
    std::chrono::seconds last_read_only_timeout{};
    std::vector<std::string> opened_tokens;

    std::size_t BeginDualReadOnlyProbe(std::chrono::seconds timeout) override {
        ++read_only_begin_count;
        last_read_only_timeout = timeout;
        if (fail_read_only_start) {
            throw TransportError(
                "sdk_load_failed", "injected-sensitive-start-detail");
        }
        process_claimed = true;
        module_active = true;
        if (fail_read_only_with_non_transport_exception) {
            process_claimed = false;
            module_active = false;
            throw std::runtime_error("injected-sensitive-non-transport-detail");
        }
        if (read_only_failure_category == "inventory_close_failed") {
            read_only_inventory.fail_close_number = read_only_close_throws ? 0 : 1;
            read_only_inventory.unknown_close_number = read_only_close_throws ? 1 : 0;
            try {
                return InspectNikonD810InventorySources(
                    {10, 20},
                    [&](std::uint32_t id) { read_only_inventory.Open(id); },
                    [&] { return read_only_inventory.InspectIsD810(); },
                    [&] { return read_only_inventory.CloseOnce(); },
                    read_only_close_unconfirmed).size();
            } catch (...) {
                // Model rollback/unload dropping all SDK-shaped handles. The
                // shared production inventory seam must preserve uncertainty.
                process_claimed = false;
                module_active = false;
                source_open = false;
                read_only_inventory.source_open = false;
                throw;
            }
        }
        if (!read_only_failure_category.empty()) {
            process_claimed = false;
            module_active = false;
            throw TransportError(
                read_only_failure_category,
                "injected-sensitive-read-only-detail");
        }
        return read_only_d810_count;
    }

    std::vector<std::string> BeginDualSession(std::chrono::seconds) override {
        ++begin_count;
        process_claimed = true;
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

    SdkCameraStatus ProbeOpenCaptureSessionStatus(std::chrono::seconds) override {
        if (!source_open || !capture_source) {
            throw std::runtime_error("capture source is not open for status");
        }
        ++status_probe_count;
        SdkCameraStatus status;
        status.live_view_status_available = true;
        status.live_view_status = "off";
        status.file_type.available = true;
        status.file_type.current_label = "JPEG";
        status.compression_level.available = true;
        status.compression_level.current_label = "Fine";
        status.image_size.available = true;
        status.image_size.current_label = "L";
        return status;
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
        ++invalidation_poll_count;
        return invalidation;
    }

    void EndDualSession(std::chrono::seconds) override {
        ++end_count;
        if (fail_end_before_cleanup) {
            throw TransportError(
                "close_failed", "injected-sensitive-unconfirmed-detail");
        }
        source_open = false;
        live_view_active = false;
        capture_source = false;
        module_active = false;
        process_claimed = false;
        if (fail_end_after_cleanup) {
            throw TransportError(
                "close_failed", "injected-sensitive-cleanup-detail");
        }
    }

    void AbandonDualSessionNoSdkCalls() noexcept override {
        // Deliberately do not touch any SDK-shaped state or count EndSession.
        abandoned = true;
    }

    ExitState InspectDualSessionExitState() const noexcept override {
        return {process_claimed, module_active,
            source_open || read_only_close_unconfirmed};
    }

    bool abandoned{};

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

constexpr std::string_view kPreflightCamA = "fixture-preflight-wpd-a";
constexpr std::string_view kPreflightCamB = "fixture-preflight-wpd-b";

CameraInfo PreflightCamera(std::string_view identity) {
    return {"D810", "fixture-firmware", "photo", std::string(identity)};
}

IdentityMap PreflightMap() {
    return IdentityMap(
        "fixture-preflight-map.json",
        std::string(kPreflightCamA),
        std::string(kPreflightCamB));
}

class RecordingPairPreflightWpdTransport final
    : public IWpdDualReadOnlyProbeTransport {
public:
    std::vector<std::vector<CameraInfo>> inventories{
        {PreflightCamera(kPreflightCamA), PreflightCamera(kPreflightCamB)},
        {PreflightCamera(kPreflightCamB), PreflightCamera(kPreflightCamA)},
        {PreflightCamera(kPreflightCamA), PreflightCamera(kPreflightCamB)},
    };
    std::function<void(std::size_t)> on_access;
    std::size_t inventory_index{};
    std::size_t access_count{};
    std::size_t cam_a_payload_count{};
    std::size_t cam_b_payload_count{};
    bool cleanup_leaves_session_open{};
    bool session_open{};

    std::vector<CameraInfo> EnumerateForDualReadOnlyProbe() override {
        ObserveAccess();
        if (inventory_index >= inventories.size()) {
            throw std::runtime_error("unexpected extra preflight inventory");
        }
        return inventories[inventory_index++];
    }

    std::size_t InspectDualReadOnlySpoolPayloadCount(
        std::string_view stable_identity,
        std::chrono::seconds) override {
        ObserveAccess();
        session_open = cleanup_leaves_session_open;
        if (stable_identity == kPreflightCamA) return cam_a_payload_count;
        if (stable_identity == kPreflightCamB) return cam_b_payload_count;
        throw std::runtime_error("unexpected preflight WPD identity");
    }

    void CloseDualReadOnlyProbeSession(std::chrono::seconds) override {
        ObserveAccess();
        if (!cleanup_leaves_session_open) session_open = false;
    }

    bool DualReadOnlyProbeSessionOpen() const noexcept override {
        return session_open;
    }

private:
    void ObserveAccess() {
        ++access_count;
        if (on_access) on_access(access_count);
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
    const auto status_a = adapter.ProbeOpenCaptureSessionStatus(5s);
    Check(status_a.live_view_status == "off",
        "bound capture status must be read from the same open source session");
    adapter.CaptureToCard(5s, 10s);
    adapter.CloseBoundCapture(5s);
    adapter.OpenBoundCapture(tokens[1], 5s);
    (void)adapter.ProbeOpenCaptureSessionStatus(5s);
    adapter.CaptureToCard(5s, 10s);
    adapter.CloseBoundCapture(5s);

    Check(transport->begin_count == 1,
        "bound capture must never re-enumerate SDK candidates");
    Check(transport->capture_count == 2,
        "CAM-A and CAM-B must each capture exactly once");
    Check(transport->status_probe_count == 2,
        "each bound source must be revalidated exactly once before capture");
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

void TestAbandonedSessionNeverCallsSdkDuringDestruction() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();
    {
        NikonDualBindingSdkAdapter adapter(transport);
        (void)adapter.EnumerateCandidates();
        Check(ResolveDualCaptureWpdCleanup(adapter, false, false) ==
                  DualIdentityInvalidationReason::SdkError,
            "a failed checked WPD Close must invalidate even after local handle release");
    }
    Check(transport->abandoned && transport->end_count == 0,
        "abandoned WPD boundary must not invoke SDK EndSession in adapter destruction");
    Check(transport->capture_count == 0 && transport->invalidation_poll_count == 0,
        "abandoned WPD boundary must not shutter, retry, or poll SDK");
}

void TestExplicitBindingCancellationEndsLiveViewAndManager() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();
    NikonDualBindingSdkAdapter adapter(transport);
    (void)adapter.EnumerateCandidates();
    Check(adapter.StartLiveView(0),
        "binding cancellation test requires an active candidate Live View");

    Check(adapter.EndBindingSession(5s),
        "explicit binding cancellation must confirm full SDK cleanup");
    Check(transport->end_count == 1 && !transport->live_view_active &&
          !transport->source_open && !transport->module_active &&
          !transport->process_claimed,
        "one cancellation call ends Live View, source, module, and process claim");
}

void TestFailedBindingCancellationIsNotRetriedByDestructor() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();
    transport->fail_end_before_cleanup = true;
    {
        NikonDualBindingSdkAdapter adapter(transport);
        (void)adapter.EnumerateCandidates();
        Check(!adapter.EndBindingSession(5s),
            "unconfirmed binding cleanup must be reported as failure");
        Check(transport->end_count == 1,
            "the explicit cancellation performs exactly one end attempt");
    }
    Check(transport->end_count == 1,
        "adapter destruction must not hide a failed cancellation with an implicit retry");
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

void TestPairPreflightEnforcesTheRetainedModuleOnlyBoundary() {
    {
        auto sdk = std::make_shared<RecordingDualSessionTransport>();
        NikonDualBindingSdkAdapter adapter(sdk);
        Check(adapter.EnumerateCandidates().size() == 2,
            "preflight pass requires two session-local candidates");
        RecordingPairPreflightWpdTransport wpd;
        bool module_only_at_every_wpd_access = true;
        bool sdk_poll_count_stable_during_wpd = true;
        wpd.on_access = [&](std::size_t) {
            module_only_at_every_wpd_access =
                module_only_at_every_wpd_access &&
                adapter.InspectRetainedModuleState().ReadyForReadOnlyWpd();
            sdk_poll_count_stable_during_wpd =
                sdk_poll_count_stable_during_wpd &&
                sdk->invalidation_poll_count == 1;
        };

        const auto result = RunDualBoundPairPreflight(
            adapter, wpd, PreflightMap(), 5s);
        Check(result.ready &&
              result.error == DualBoundPairPreflightError::None &&
              result.wpd_result.terminal_state ==
                  DualWpdReadOnlyProbeTerminalState::Pass &&
              result.sdk_state_before_wpd.ReadyForReadOnlyWpd() &&
              result.sdk_state_after_wpd.ReadyForReadOnlyWpd(),
            "retained Module with zero sources and Live View off must pass");
        Check(module_only_at_every_wpd_access &&
              sdk_poll_count_stable_during_wpd && wpd.access_count == 5,
            "every WPD read must observe Module-only state and zero SDK calls");
        Check(sdk->begin_count == 1 && sdk->source_open_count == 0 &&
              sdk->capture_count == 0 && sdk->status_probe_count == 0 &&
              sdk->invalidation_poll_count == 2 && sdk->end_count == 0,
            "preflight must not open a source, inspect settings, capture, retry, or end a valid binding");
        adapter.EndSession(5s);
        Check(sdk->end_count == 1 && !sdk->module_active,
            "terminal cleanup must end the retained Module exactly once");
    }

    {
        auto sdk = std::make_shared<RecordingDualSessionTransport>();
        NikonDualBindingSdkAdapter adapter(sdk);
        (void)adapter.EnumerateCandidates();
        Check(adapter.StartLiveView(0),
            "unsafe preflight fixture requires an active Live View source");
        RecordingPairPreflightWpdTransport wpd;
        const auto result = RunDualBoundPairPreflight(
            adapter, wpd, PreflightMap(), 5s);
        Check(!result.ready &&
              result.error ==
                  DualBoundPairPreflightError::SdkBoundaryUnsafeBeforeWpd &&
              wpd.access_count == 0 && sdk->capture_count == 0,
            "an open source or Live View must block before the first WPD read");
        adapter.EndSession(5s);
    }

    {
        auto sdk = std::make_shared<RecordingDualSessionTransport>();
        NikonDualBindingSdkAdapter adapter(sdk);
        (void)adapter.EnumerateCandidates();
        sdk->module_active = false;
        RecordingPairPreflightWpdTransport wpd;
        const auto result = RunDualBoundPairPreflight(
            adapter, wpd, PreflightMap(), 5s);
        Check(!result.ready &&
              result.error ==
                  DualBoundPairPreflightError::SdkBoundaryUnsafeBeforeWpd &&
              wpd.access_count == 0,
            "a missing retained Module must never be treated as coexistence");
        adapter.EndSession(5s);
    }

    {
        auto sdk = std::make_shared<RecordingDualSessionTransport>();
        NikonDualBindingSdkAdapter adapter(sdk);
        (void)adapter.EnumerateCandidates();
        RecordingPairPreflightWpdTransport wpd;
        wpd.cam_b_payload_count = 1;
        const auto result = RunDualBoundPairPreflight(
            adapter, wpd, PreflightMap(), 5s);
        Check(!result.ready &&
              result.error == DualBoundPairPreflightError::WpdProbeBlocked &&
              result.wpd_result.error == DualWpdReadOnlyProbeError::SpoolNotEmpty &&
              sdk->capture_count == 0 && sdk->invalidation_poll_count == 2,
            "either nonempty card must block both shutters without retry");
        adapter.EndSession(5s);
    }

    {
        auto sdk = std::make_shared<RecordingDualSessionTransport>();
        NikonDualBindingSdkAdapter adapter(sdk);
        (void)adapter.EnumerateCandidates();
        sdk->invalidation = DualIdentityInvalidationReason::UsbReconnect;
        RecordingPairPreflightWpdTransport wpd;
        const auto result = RunDualBoundPairPreflight(
            adapter, wpd, PreflightMap(), 5s);
        Check(!result.ready &&
              result.error ==
                  DualBoundPairPreflightError::SdkInvalidatedBeforeWpd &&
              result.invalidation_reason ==
                  DualIdentityInvalidationReason::UsbReconnect &&
              wpd.access_count == 0 && sdk->end_count == 1 &&
              !sdk->module_active,
            "invalidation before WPD must unload once and prevent all WPD access");
    }

    {
        auto sdk = std::make_shared<RecordingDualSessionTransport>();
        NikonDualBindingSdkAdapter adapter(sdk);
        (void)adapter.EnumerateCandidates();
        RecordingPairPreflightWpdTransport wpd;
        wpd.on_access = [&](std::size_t access_count) {
            if (access_count == 5) {
                sdk->invalidation =
                    DualIdentityInvalidationReason::TopologyChanged;
            }
        };
        const auto result = RunDualBoundPairPreflight(
            adapter, wpd, PreflightMap(), 5s);
        Check(!result.ready &&
              result.error ==
                  DualBoundPairPreflightError::SdkInvalidatedAfterWpd &&
              result.invalidation_reason ==
                  DualIdentityInvalidationReason::TopologyChanged &&
              sdk->end_count == 1 && !sdk->module_active &&
              sdk->capture_count == 0,
            "invalidation observed after WPD must unload once before capture");
    }

    {
        auto sdk = std::make_shared<RecordingDualSessionTransport>();
        {
            NikonDualBindingSdkAdapter adapter(sdk);
            (void)adapter.EnumerateCandidates();
            RecordingPairPreflightWpdTransport wpd;
            wpd.cleanup_leaves_session_open = true;
            const auto result = RunDualBoundPairPreflight(
                adapter, wpd, PreflightMap(), 5s);
            Check(!result.ready &&
                  result.error == DualBoundPairPreflightError::WpdProbeBlocked &&
                  result.wpd_result.cleanup == DualWpdReadOnlyProbeCleanup::Unconfirmed &&
                  sdk->invalidation_poll_count == 1 && sdk->end_count == 0,
                "unconfirmed WPD cleanup must issue no SDK call while WPD may be open");

            const auto outcome = ResolveDualBoundPairPreflight(adapter, result);
            Check(outcome.state ==
                      DualHardwarePairPreflightState::CleanupUnconfirmed &&
                  outcome.binding_invalidation_reason ==
                      DualIdentityInvalidationReason::SdkError &&
                  outcome.requires_rebinding &&
                  outcome.host_terminal_after_reservation_close,
                "production preflight resolution must type unconfirmed WPD cleanup as terminal");
            Check(sdk->abandoned && sdk->end_count == 0 &&
                  sdk->capture_count == 0 &&
                  sdk->invalidation_poll_count == 1,
                "production preflight resolution must abandon without SDK end, shutter, retry, or poll");
        }
        Check(sdk->end_count == 0,
            "adapter destruction after unconfirmed WPD cleanup must not hide an SDK End call");
    }
}

void TestReadOnlyProbeEndsSessionWithoutPublishingBindingTokens() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();

    const auto result = RunDualSdkReadOnlyProbe(*transport, 5s);
    Check(result.sdk_d810_count == 2 && result.exit_state.FullyEnded() &&
          result.error == DualSdkReadOnlyProbeError::None &&
          result.cleanup == DualSdkReadOnlyProbeCleanup::Ended &&
          result.terminal_state == DualSdkReadOnlyProbeTerminalState::Pass,
        "Dual SDK read-only probe must report exactly two inspected candidates and end the session");
    Check(transport->read_only_begin_count == 1 && transport->begin_count == 0 &&
          transport->end_count == 1 &&
          transport->last_read_only_timeout == 5s &&
          !transport->module_active && transport->source_open_count == 0,
        "the transport-level fake must receive the deadline without generating tokens or retaining a source");
    Check(transport->capture_count == 0 && transport->status_probe_count == 0,
        "Dual SDK read-only probe must not inspect settings or send a capture command");

    const auto json = SerializeDualSdkReadOnlyProbeResult(result);
    Check(json ==
        "{\"operation\":\"read-only-sdk-probe\",\"sdkD810Count\":2,"
        "\"sdkSessionEnded\":true,\"sdkProcessClaimRetainedAtExit\":false,"
        "\"sdkModuleRetainedAtExit\":false,"
        "\"sdkSourceOpenAtExit\":false,\"candidateTokensPublished\":false,"
        "\"liveViewStarted\":false,\"captureCommandSent\":false,"
        "\"cameraSettingsChanged\":false,\"wpdAccessed\":false,"
        "\"errorCategory\":\"none\",\"cleanupState\":\"ended\","
        "\"terminalState\":\"Pass\"}",
        "Dual SDK read-only probe output must expose only the safe fixed schema");
    for (const auto& token : transport->tokens) {
        Check(json.find(token) == std::string::npos,
            "Dual SDK read-only probe output must not publish candidate tokens");
    }
}

void TestReadOnlyProbeBlocksZeroOneAndThreeCandidatesWithoutTokens() {
    for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
        auto transport = std::make_shared<RecordingDualSessionTransport>();
        transport->read_only_d810_count = count;

        const auto result = RunDualSdkReadOnlyProbe(*transport, 5s);
        Check(result.sdk_d810_count == count && result.exit_state.FullyEnded() &&
              result.error == DualSdkReadOnlyProbeError::CameraCountMismatch &&
              result.cleanup == DualSdkReadOnlyProbeCleanup::Ended &&
              result.terminal_state == DualSdkReadOnlyProbeTerminalState::Blocked,
            "every non-two Dual SDK count must return a typed Blocked result");
        Check(transport->read_only_begin_count == 1 && transport->begin_count == 0 &&
              transport->end_count == 1 && !transport->module_active &&
              transport->source_open_count == 0 && transport->capture_count == 0,
            "count mismatch must close once without token generation or camera commands");
        const auto json = SerializeDualSdkReadOnlyProbeResult(result);
        Check(json.find("\"errorCategory\":\"cameraCountMismatch\"") !=
                  std::string::npos &&
              json.find("\"terminalState\":\"Blocked\"") != std::string::npos,
            "count mismatch must keep the fixed anonymous JSON schema");
    }
}

void TestReadOnlyProbePublishesAnonymousFailureStages() {
    struct FailureCase {
        const char* transport_category;
        DualSdkReadOnlyProbeError expected;
        const char* public_category;
    };
    constexpr FailureCase cases[] = {
        {"dual_read_only_inventory_failed",
            DualSdkReadOnlyProbeError::SdkInventoryFailed,
            "sdkInventoryFailed"},
        {"dual_read_only_source_wait_timeout",
            DualSdkReadOnlyProbeError::SdkSourceWaitTimeout,
            "sdkSourceWaitTimeout"},
        {"dual_read_only_source_wait_failed",
            DualSdkReadOnlyProbeError::SdkSourceWaitFailed,
            "sdkSourceWaitFailed"},
        {"dual_read_only_metadata_projection_failed",
            DualSdkReadOnlyProbeError::SdkMetadataProjectionFailed,
            "sdkMetadataProjectionFailed"},
        {"inventory_close_failed",
            DualSdkReadOnlyProbeError::SdkInventorySourceCloseFailed,
            "sdkInventorySourceCloseFailed"},
    };

    {
        auto transport = std::make_shared<RecordingDualSessionTransport>();
        transport->fail_read_only_start = true;

        const auto result = RunDualSdkReadOnlyProbe(*transport, 5s);
        Check(!result.sdk_d810_count.has_value() &&
              result.exit_state.FullyEnded() &&
              result.error == DualSdkReadOnlyProbeError::SdkStartFailed &&
              result.cleanup == DualSdkReadOnlyProbeCleanup::Ended &&
              result.terminal_state == DualSdkReadOnlyProbeTerminalState::Blocked,
            "SDK startup failure must remain distinct after confirmed cleanup");
        Check(transport->read_only_begin_count == 1 && transport->end_count == 0 &&
              transport->begin_count == 0 && transport->source_open_count == 0 &&
              transport->capture_count == 0 &&
              transport->last_read_only_timeout == 5s,
            "failed SDK startup must remain deadline-bound and must not retry or bind");
        const auto json = SerializeDualSdkReadOnlyProbeResult(result);
        Check(json.find("\"errorCategory\":\"sdkStartFailed\"") !=
                  std::string::npos &&
              json.find("injected-sensitive") == std::string::npos &&
              json.find("\"sdkD810Count\":null") != std::string::npos &&
              json.find("\"terminalState\":\"Blocked\"") != std::string::npos,
            "SDK startup details must never cross the anonymous JSON boundary");
    }

    for (const auto& failure : cases) {
        auto transport = std::make_shared<RecordingDualSessionTransport>();
        transport->read_only_failure_category = failure.transport_category;

        const auto result = RunDualSdkReadOnlyProbe(*transport, 5s);
        const bool close_unconfirmed =
            failure.expected ==
                DualSdkReadOnlyProbeError::SdkInventorySourceCloseFailed;
        Check(!result.sdk_d810_count.has_value() &&
              result.exit_state.FullyEnded() == !close_unconfirmed &&
              result.error == failure.expected &&
              result.cleanup == (close_unconfirmed
                  ? DualSdkReadOnlyProbeCleanup::Unconfirmed
                  : DualSdkReadOnlyProbeCleanup::Ended) &&
              result.terminal_state == DualSdkReadOnlyProbeTerminalState::Blocked,
            "each SDK inventory stage must retain its fixed typed category after cleanup");
        Check(transport->read_only_begin_count == 1 &&
              transport->end_count == (close_unconfirmed ? 1u : 0u) &&
              transport->begin_count == 0 && transport->source_open_count == 0 &&
              transport->capture_count == 0 &&
              transport->last_read_only_timeout == 5s,
            "each inventory failure must remain deadline-bound without retry, binding, or camera commands");
        const auto json = SerializeDualSdkReadOnlyProbeResult(result);
        Check(json.find(std::string{"\"errorCategory\":\""} +
                  failure.public_category + "\"") != std::string::npos &&
              json.find(failure.transport_category) == std::string::npos &&
              json.find("injected-sensitive") == std::string::npos &&
              json.find("\"sdkD810Count\":null") != std::string::npos &&
              json.find(close_unconfirmed
                  ? "\"cleanupState\":\"unconfirmed\""
                  : "\"cleanupState\":\"ended\"") != std::string::npos &&
              json.find(close_unconfirmed
                  ? "\"sdkSessionEnded\":false"
                  : "\"sdkSessionEnded\":true") != std::string::npos &&
              json.find("\"terminalState\":\"Blocked\"") != std::string::npos,
            "inventory diagnostics must expose only fixed public categories and cleanup facts");
    }

    {
        auto transport = std::make_shared<RecordingDualSessionTransport>();
        transport->fail_read_only_with_non_transport_exception = true;
        const auto result = RunDualSdkReadOnlyProbe(*transport, 5s);
        const auto json = SerializeDualSdkReadOnlyProbeResult(result);
        Check(result.error == DualSdkReadOnlyProbeError::SdkOperationFailed &&
              result.cleanup == DualSdkReadOnlyProbeCleanup::Ended &&
              result.exit_state.FullyEnded() &&
              transport->read_only_begin_count == 1 &&
              transport->last_read_only_timeout == 5s &&
              json.find("\"errorCategory\":\"sdkOperationFailed\"") !=
                  std::string::npos &&
              json.find("injected-sensitive") == std::string::npos,
            "unexpected SDK exceptions must remain anonymous and cleanup-confirmed without retry");
    }
}

void TestReadOnlyProbePreservesSharedInventoryUncertaintyAfterCleanup() {
    for (const bool close_throws : {false, true}) {
        RecordingDualSessionTransport transport;
        transport.read_only_failure_category = "inventory_close_failed";
        transport.read_only_close_throws = close_throws;
        const auto result = RunDualSdkReadOnlyProbe(transport, 5s);
        const auto json = SerializeDualSdkReadOnlyProbeResult(result);
        Check(transport.read_only_inventory.open_count == 1 &&
                  transport.read_only_inventory.close_count == 1 &&
                  transport.read_only_begin_count == 1 && transport.end_count == 1,
            "shared inventory close failure must stop after one source and one checked close");
        Check(!transport.process_claimed && !transport.module_active &&
                  !transport.source_open && !transport.read_only_inventory.source_open &&
                  transport.read_only_close_unconfirmed && !result.exit_state.FullyEnded() &&
                  result.error == DualSdkReadOnlyProbeError::SdkInventorySourceCloseFailed &&
                  result.cleanup == DualSdkReadOnlyProbeCleanup::Unconfirmed &&
                  result.terminal_state == DualSdkReadOnlyProbeTerminalState::Blocked &&
                  json.find("\"sdkSessionEnded\":false") != std::string::npos,
            "dropping SDK handles must not erase shared inventory close uncertainty");

        bool repeated_walk_blocked = false;
        try {
            (void)InspectNikonD810InventorySources(
                {10, 20},
                [&](std::uint32_t id) { transport.read_only_inventory.Open(id); },
                [&] { return transport.read_only_inventory.InspectIsD810(); },
                [&] { return transport.read_only_inventory.CloseOnce(); },
                transport.read_only_close_unconfirmed);
        } catch (const TransportError& error) {
            repeated_walk_blocked = error.Category() == "inventory_close_failed";
        }
        Check(repeated_walk_blocked && transport.read_only_inventory.open_count == 1 &&
                  transport.read_only_inventory.close_count == 1,
            "a sticky unconfirmed inventory must reject any later walk without retry");
    }
}

void TestReadOnlyProbeBlocksCleanupErrorAfterConfirmedEnd() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();
    transport->fail_end_after_cleanup = true;

    const auto result = RunDualSdkReadOnlyProbe(*transport, 5s);
    Check(result.sdk_d810_count == 2 && result.exit_state.FullyEnded() &&
          result.error == DualSdkReadOnlyProbeError::None &&
          result.cleanup == DualSdkReadOnlyProbeCleanup::EndedAfterError &&
          result.terminal_state == DualSdkReadOnlyProbeTerminalState::Blocked,
        "cleanup error must block even when module release is confirmed");
    Check(transport->read_only_begin_count == 1 && transport->end_count == 1 &&
          transport->begin_count == 0 && transport->capture_count == 0,
        "cleanup failure must not retry the probe or issue a camera command");
    const auto json = SerializeDualSdkReadOnlyProbeResult(result);
    Check(json.find("injected-sensitive-cleanup-detail") == std::string::npos &&
          json.find("\"cleanupState\":\"endedAfterError\"") != std::string::npos &&
          json.find("\"terminalState\":\"Blocked\"") != std::string::npos,
        "confirmed cleanup error must use only the fixed anonymous schema");
}

void TestReadOnlyProbeBlocksWhenCleanupCannotBeConfirmed() {
    auto transport = std::make_shared<RecordingDualSessionTransport>();
    transport->fail_end_before_cleanup = true;

    const auto result = RunDualSdkReadOnlyProbe(*transport, 5s);
    Check(result.sdk_d810_count == 2 && !result.exit_state.FullyEnded() &&
          result.exit_state.process_claim_retained &&
          result.exit_state.module_retained &&
          result.cleanup == DualSdkReadOnlyProbeCleanup::Unconfirmed &&
          result.terminal_state == DualSdkReadOnlyProbeTerminalState::Blocked,
        "unconfirmed SDK cleanup must remain visible and Blocked");
    Check(transport->read_only_begin_count == 1 && transport->end_count == 1 &&
          transport->begin_count == 0 && transport->capture_count == 0,
        "unconfirmed cleanup must not trigger an automatic retry");
    const auto json = SerializeDualSdkReadOnlyProbeResult(result);
    Check(json.find("injected-sensitive-unconfirmed-detail") == std::string::npos &&
          json.find("\"sdkSessionEnded\":false") != std::string::npos &&
          json.find("\"cleanupState\":\"unconfirmed\"") != std::string::npos &&
          json.find("\"terminalState\":\"Blocked\"") != std::string::npos,
        "unconfirmed cleanup output must remain anonymous and fixed-schema");
}

} // namespace

int main() {
    try {
        TestReadOnlySourceWaitReturnsNormalizedPublishedIds();
        TestReadOnlySourceWaitRejectsDeadlineReachedDuringCallbacks();
        TestReadOnlySourceWaitHasDeterministicTimeoutAndFailureCategories();
        TestInventoryCloseFailureStopsBeforeOpeningNextSource();
        TestInventoryUnknownCloseCompletionStopsWithoutRetry();
        TestInventoryWalkRemainsSequentialWhenEveryCloseIsConfirmed();
        TestSequentialBindingAndBoundCaptureReuseOneModule();
        TestSecondLiveViewCannotOverlapFirst();
        TestInvalidationRevokesEveryCandidateWithoutRetry();
        TestCloseFailureEndsManagerAndInvalidatesSession();
        TestDestructorClosesRetainedManager();
        TestAbandonedSessionNeverCallsSdkDuringDestruction();
        TestExplicitBindingCancellationEndsLiveViewAndManager();
        TestFailedBindingCancellationIsNotRetriedByDestructor();
        TestCaptureFailureInvalidatesAndEndsSessionWithoutRetry();
        TestPairPreflightEnforcesTheRetainedModuleOnlyBoundary();
        TestReadOnlyProbeEndsSessionWithoutPublishingBindingTokens();
        TestReadOnlyProbeBlocksZeroOneAndThreeCandidatesWithoutTokens();
        TestReadOnlyProbePublishesAnonymousFailureStages();
        TestReadOnlyProbePreservesSharedInventoryUncertaintyAfterCleanup();
        TestReadOnlyProbeBlocksCleanupErrorAfterConfirmedEnd();
        TestReadOnlyProbeBlocksWhenCleanupCannotBeConfirmed();
        std::cout << "Nikon Dual session adapter contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Nikon Dual session adapter contract failed: "
                  << error.what() << '\n';
        return 1;
    }
}
