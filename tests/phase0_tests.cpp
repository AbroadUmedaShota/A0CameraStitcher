#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <portabledevice.h>

#include "a0/phase0/fake_camera_transport.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/phase0/wpd_transport.hpp"
#include <atomic>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <thread>

namespace fs = std::filesystem;
using namespace a0::phase0;

namespace {

template <typename T>
concept ExposesCaptureToCard = requires(T& executor) {
    executor.CaptureToCard(std::chrono::seconds(1), std::chrono::seconds(1));
};

template <typename T>
concept ExposesRecoveredObjectDelete = requires(T& executor) {
    executor.DeleteRecoveredObject(std::string_view{}, std::chrono::seconds(1));
};

template <typename T>
concept ExposesLiveViewOpen = requires(T& executor) {
    executor.OpenLiveView(std::string_view{}, std::chrono::seconds(1));
};

template <typename T>
concept ExposesLiveViewStart = requires(T& executor) {
    executor.StartLiveView(std::chrono::seconds(1));
};

template <typename T>
concept ExposesLiveViewRead = requires(T& executor) {
    executor.ReadLiveViewFrame(std::chrono::seconds(1));
};

template <typename T>
concept ExposesLiveViewStop = requires(T& executor) {
    executor.StopLiveView(std::chrono::seconds(1));
};

static_assert(std::is_base_of_v<ISdkStatusExecutor, NikonSdkStatusExecutor>);
static_assert(!ExposesCaptureToCard<NikonSdkStatusExecutor>);
static_assert(!ExposesRecoveredObjectDelete<NikonSdkStatusExecutor>);
static_assert(!ExposesLiveViewOpen<NikonSdkStatusExecutor>);
static_assert(!ExposesLiveViewStart<NikonSdkStatusExecutor>);
static_assert(!ExposesLiveViewRead<NikonSdkStatusExecutor>);
static_assert(!ExposesLiveViewStop<NikonSdkStatusExecutor>);

int failures = 0;

enum class FakeLiveViewFailure {
    none,
    stop_failure,
    close_failure,
    second_open_failure
};

class FakeLiveViewTransport final : public ILiveViewTransport {
public:
    explicit FakeLiveViewTransport(FakeLiveViewFailure failure = FakeLiveViewFailure::none)
        : failure_(failure) {}

    void OpenLiveView(std::string_view stable_identity, std::chrono::seconds) override {
        ++open_attempts_;
        if (stable_identity.empty()) throw TransportError("open_failed", "fake live view identity is empty");
        if (failure_ == FakeLiveViewFailure::second_open_failure && open_attempts_ == 2) {
            throw TransportError("live_view_resume_open_failed", "fake second live view open failure");
        }
        if (open_) throw TransportError("session_busy", "fake live view session overlap");
        open_ = true;
    }

    void StartLiveView(std::chrono::seconds) override {
        ++start_attempts_;
        if (!open_) throw TransportError("session_not_open", "fake live view start without session");
        started_ = true;
    }

    std::vector<unsigned char> ReadLiveViewFrame(std::chrono::seconds) override {
        ++read_attempts_;
        if (!started_) throw TransportError("live_view_not_started", "fake frame without live view");
        return {0xFF, 0xD8, 0x01, static_cast<unsigned char>(read_attempts_ & 0xFF), 0xFF, 0xD9};
    }

    void StopLiveView(std::chrono::seconds) override {
        ++stop_attempts_;
        if (!started_) throw TransportError("live_view_not_started", "fake stop without live view");
        if (failure_ == FakeLiveViewFailure::stop_failure && stop_attempts_ == 1) {
            throw TransportError("live_view_stop_failed", "fake live view stop failure");
        }
        started_ = false;
    }

    void Close(std::chrono::seconds) override {
        ++close_attempts_;
        if (!open_) throw TransportError("session_not_open", "fake live view close without session");
        open_ = false;
        started_ = false;
        if (failure_ == FakeLiveViewFailure::close_failure && close_attempts_ == 1) {
            throw TransportError("close_failed", "fake live view close failure");
        }
    }

    [[nodiscard]] int OpenAttempts() const noexcept { return open_attempts_; }
    [[nodiscard]] int StartAttempts() const noexcept { return start_attempts_; }
    [[nodiscard]] int ReadAttempts() const noexcept { return read_attempts_; }
    [[nodiscard]] int StopAttempts() const noexcept { return stop_attempts_; }
    [[nodiscard]] int CloseAttempts() const noexcept { return close_attempts_; }

private:
    FakeLiveViewFailure failure_;
    bool open_{false};
    bool started_{false};
    int open_attempts_{0};
    int start_attempts_{0};
    int read_attempts_{0};
    int stop_attempts_{0};
    int close_attempts_{0};
};

class DetailedFailureTransport final : public ICameraTransport {
public:
    explicit DetailedFailureTransport(std::string detail) : detail_(std::move(detail)) {}

    [[nodiscard]] std::string SdkVersion() const override { return "detailed-fake"; }
    [[nodiscard]] std::vector<CameraInfo> Enumerate() override { return delegate_.Enumerate(); }
    void Open(std::string_view identity, std::chrono::seconds timeout) override { delegate_.Open(identity, timeout); }
    [[nodiscard]] std::string Baseline(std::chrono::seconds timeout) override { return delegate_.Baseline(timeout); }
    [[nodiscard]] std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view, std::chrono::seconds, std::chrono::seconds, std::chrono::seconds) override {
        throw TransportError("capture_command_failed", detail_);
    }
    void Close(std::chrono::seconds timeout) override { delegate_.Close(timeout); }

private:
    FakeCameraTransport delegate_;
    std::string detail_;
};

class UncertainDispatchTransport final : public ICameraTransport {
public:
    UncertainDispatchTransport(std::string category, std::string detail, std::vector<ImageCandidate> candidates)
        : category_(std::move(category)), detail_(std::move(detail)), candidates_(std::move(candidates)) {}

    [[nodiscard]] std::string SdkVersion() const override { return "uncertain-dispatch-fake"; }
    [[nodiscard]] std::vector<CameraInfo> Enumerate() override {
        return {{"Nikon D810", "fake-fw-a", "M", "private-a"}, {"Nikon D810", "fake-fw-b", "M", "private-b"}};
    }
    void Open(std::string_view, std::chrono::seconds) override { ++open_attempts_; open_ = true; }
    [[nodiscard]] std::string Baseline(std::chrono::seconds) override {
        if (!open_) throw TransportError("session_not_open", "fake baseline without session");
        return "uncertain-baseline";
    }
    [[nodiscard]] std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view, std::chrono::seconds, std::chrono::seconds, std::chrono::seconds) override {
        ++capture_attempts_;
        throw UncertainDispatchError(category_, detail_, candidates_);
    }
    void Close(std::chrono::seconds) override { open_ = false; }
    [[nodiscard]] int CaptureAttempts() const noexcept { return capture_attempts_; }
    [[nodiscard]] int OpenAttempts() const noexcept { return open_attempts_; }

private:
    std::string category_;
    std::string detail_;
    std::vector<ImageCandidate> candidates_;
    bool open_{false};
    int open_attempts_{0};
    int capture_attempts_{0};
};

class HybridWpdFake final : public ICameraTransport, public IPostCardObservationTransport {
public:
    [[nodiscard]] std::string SdkVersion() const override { return "hybrid-wpd-fake"; }
    [[nodiscard]] std::vector<CameraInfo> Enumerate() override { return {{"Nikon D810", "fw", "M", "wpd-a"}}; }
    void Open(std::string_view identity, std::chrono::seconds) override {
        ++opens;
        order += "Wopen;";
        if ((fail_second_open && opens == 2) || (fail_open_number > 0 && opens == fail_open_number)) {
            throw TransportError("open_failed", "configured WPD reopen failure");
        }
        open = true;
        current_identity = std::string(identity);
    }
    [[nodiscard]] std::string Baseline(std::chrono::seconds) override { return BeginPostCardObservation({}); }
    [[nodiscard]] std::vector<ImageCandidate> CaptureAndDownload(std::string_view, std::chrono::seconds, std::chrono::seconds, std::chrono::seconds) override {
        ++capture_commands; throw TransportError("unexpected_wpd_capture", "WPD shutter must not be used");
    }
    void Close(std::chrono::seconds) override { ++closes; open = false; order += "Wclose;"; }
    [[nodiscard]] std::string BeginPostCardObservation(std::chrono::seconds) override {
        if (!open) throw TransportError("session_not_open", "baseline without WPD open");
        ++spool_empty_before_checks;
        if (!spool_empty_before) {
            throw TransportError("spool_not_empty", "configured spool contains a non-JPEG payload before capture");
        }
        token_live = true; baseline_identity = current_identity; order += "Wbaseline;"; return "opaque-token";
    }
    [[nodiscard]] std::vector<ImageCandidate> ObserveAndDownloadPostCardCapture(
        std::string_view token, std::chrono::seconds, std::chrono::seconds, std::chrono::seconds) override {
        if (!open || !token_live || token != "opaque-token") throw TransportError("baseline_mismatch", "invalid observation token");
        if (current_identity != baseline_identity) {
            token_live = false;
            throw TransportError("identity_mismatch", "configured WPD identity mismatch");
        }
        token_live = false; ++observes; order += "Wobserve;";
        if (!observe_error_category.empty()) {
            throw TransportError(observe_error_category, "configured WPD observation failure");
        }
        return candidates;
    }
    void DeleteRecoveredObject(std::string_view cleanup_token, std::chrono::seconds) override {
        ++delete_attempts;
        order += "Wdelete;";
        if (cleanup_token != expected_cleanup_token || cleanup_token.empty()) {
            throw TransportError("cleanup_token_invalid", "configured cleanup capability does not name the recovered object");
        }
        if (delete_fails) throw TransportError("camera_object_delete_failed", "configured exact-object delete failure");
        ++delete_successes;
    }
    void VerifyJpegSpoolEmpty(std::chrono::seconds) override {
        ++spool_empty_after_checks;
        order += "Wverify-empty;";
        if (!spool_empty_after) {
            throw TransportError("spool_not_empty_after_cleanup", "configured spool retains a payload after cleanup");
        }
    }
    void AbandonPostCardObservation(std::string_view) noexcept override {
        if (token_live) ++abandons;
        token_live = false;
    }
    bool open{false}; bool token_live{false}; int opens{}; int closes{}; int observes{}; int capture_commands{}; int abandons{};
    int spool_empty_before_checks{}; int spool_empty_after_checks{}; int delete_attempts{}; int delete_successes{}; std::string order;
    bool spool_empty_before{true}; bool spool_empty_after{true}; bool delete_fails{false}; bool fail_second_open{false};
    int fail_open_number{};
    std::string expected_cleanup_token{"cleanup-capability"};
    std::vector<ImageCandidate> candidates{{"private-object-id.jpg", {0xFF, 0xD8, 0x01, 0xFF, 0xD9}, true, "cleanup-capability"}};
    std::string observe_error_category;
    std::string current_identity;
    std::string baseline_identity;
};

class HybridSdkFake final : public ICameraTransport, public ICardCaptureTransport {
public:
    explicit HybridSdkFake(bool close_fails = false, bool capture_fails = false,
        std::chrono::milliseconds capture_delay = std::chrono::milliseconds::zero())
        : close_fails_(close_fails), capture_fails_(capture_fails), capture_delay_(capture_delay) {}
    [[nodiscard]] std::string SdkVersion() const override { return "hybrid-sdk-fake"; }
    [[nodiscard]] std::vector<CameraInfo> Enumerate() override { return {{"Nikon D810", "fw", "M", "sdk-a"}}; }
    void Open(std::string_view, std::chrono::seconds) override { ++opens; open = true; order += "Sopen;"; }
    [[nodiscard]] std::string Baseline(std::chrono::seconds) override { return {}; }
    [[nodiscard]] std::vector<ImageCandidate> CaptureAndDownload(std::string_view, std::chrono::seconds, std::chrono::seconds, std::chrono::seconds) override { return {}; }
    void Close(std::chrono::seconds) override {
        ++closes;
        open = false;
        order += "Sclose;";
        if ((close_fails_ && closes == 1) || (fail_close_number > 0 && closes == fail_close_number)) {
            throw TransportError("close_failed", "SDK close failed");
        }
    }
    void CaptureToCard(std::chrono::seconds, std::chrono::seconds) override {
        if (!open) throw TransportError("session_not_open", "SDK capture without open");
        ++captures; order += "Scapture;";
        if (capture_delay_ > std::chrono::milliseconds::zero()) std::this_thread::sleep_for(capture_delay_);
        if (capture_fails_ || (fail_capture_number > 0 && captures == fail_capture_number)) {
            throw TransportError("image_event_timeout", "configured SDK card-capture failure");
        }
    }
    bool open{false}; int opens{}; int closes{}; int captures{}; int fail_capture_number{}; int fail_close_number{};
    std::string order;
private:
    bool close_fails_{};
    bool capture_fails_{};
    std::chrono::milliseconds capture_delay_{};
};

class CorrelationFake final : public ICorrelationObservationTransport {
public:
    enum class Failure { none, open, read, close };
    explicit CorrelationFake(Failure failure = Failure::none) : failure_(failure) {}
    void OpenReadOnlyObservation(std::string_view, std::chrono::seconds) override {
        ++opens;
        sequence += "open;";
        if (failure_ == Failure::open) throw TransportError("open_failed", "fake open failure");
        open = true;
    }
    [[nodiscard]] WpdCorrelationSample ReadCorrelationSample() override {
        ++reads;
        sequence += "read;";
        if (failure_ == Failure::read) throw TransportError("read_failed", "fake read failure");
        return {true, reads > 1, false, false, 2, 2, 1, 0, 0, true};
    }
    void Close(std::chrono::seconds) override {
        ++closes;
        sequence += "close;";
        open = false;
        if (failure_ == Failure::close) throw TransportError("close_failed", "fake close failure");
    }
    Failure failure_;
    bool open{};
    int opens{};
    int reads{};
    int closes{};
    std::string sequence;
};

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string ReadAll(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

fs::path NewTestRoot(const std::string& name) {
    const auto id = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto root = fs::temp_directory_path() / ("a0-phase0-test-" + name + '-' + std::to_string(id));
    fs::create_directories(root);
    return root;
}

void TestHashAndJpeg() {
    Check(Sha256Hex({'a', 'b', 'c'}) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 must match the independent abc vector");
    Check(IsValidJpeg({0xFF, 0xD8, 0x00, 0xFF, 0xD9}), "JPEG markers should be accepted");
    Check(!IsValidJpeg({0xFF, 0xD8, 0x00}), "truncated JPEG should be rejected");
}

void TestLiveViewFrameExtraction() {
    std::vector<unsigned char> frame(384, 0x42);
    const std::vector<unsigned char> jpeg{0xFF, 0xD8, 0x01, 0x02, 0xFF, 0xD9};
    frame.insert(frame.end(), jpeg.begin(), jpeg.end());
    Check(ExtractD810LiveViewJpeg(frame) == jpeg, "D810 live view header should be removed");
    frame.insert(frame.end(), {0x00, 0x00});
    Check(ExtractD810LiveViewJpeg(frame) == jpeg, "D810 live view padding should be excluded from the JPEG");

    bool short_frame_rejected = false;
    try {
        (void)ExtractD810LiveViewJpeg(std::vector<unsigned char>(384, 0));
    } catch (const TransportError& error) {
        short_frame_rejected = error.Category() == "live_view_invalid_frame";
    }
    Check(short_frame_rejected, "short D810 live view frames should be rejected");

    std::vector<unsigned char> invalid(384, 0);
    invalid.insert(invalid.end(), {0x00, 0x01, 0x02, 0x03});
    bool invalid_jpeg_rejected = false;
    try {
        (void)ExtractD810LiveViewJpeg(invalid);
    } catch (const TransportError& error) {
        invalid_jpeg_rejected = error.Category() == "live_view_invalid_frame";
    }
    Check(invalid_jpeg_rejected, "non-JPEG live view payloads should be rejected");

    std::vector<unsigned char> unexpected_offset(384, 0);
    unexpected_offset.push_back(0x00);
    unexpected_offset.insert(unexpected_offset.end(), jpeg.begin(), jpeg.end());
    bool unexpected_offset_rejected = false;
    try {
        (void)ExtractD810LiveViewJpeg(unexpected_offset);
    } catch (const TransportError& error) {
        unexpected_offset_rejected = error.Category() == "live_view_invalid_frame";
    }
    Check(unexpected_offset_rejected, "a D810 live view JPEG at an unexpected offset should be rejected");

    std::vector<unsigned char> missing_eoi(384, 0);
    missing_eoi.insert(missing_eoi.end(), {0xFF, 0xD8, 0x01, 0x02});
    bool missing_eoi_rejected = false;
    try {
        (void)ExtractD810LiveViewJpeg(missing_eoi);
    } catch (const TransportError& error) {
        missing_eoi_rejected = error.Category() == "live_view_invalid_frame";
    }
    Check(missing_eoi_rejected, "a D810 live view JPEG without EOI should be rejected");
}

void TestLiveViewHandoffSummaryReplacement() {
    const auto root = NewTestRoot("live-view-handoff-summary");
    LiveViewHandoffRunSummary summary;
    summary.requested = 10;
    summary.attempted = 1;
    const auto path = PersistLiveViewHandoffSummary(root, "run-summary", "CAM-A", summary);
    Check(fs::exists(path), "handoff summary should be created at the run root");
    summary.attempted = 6;
    summary.completed = 5;
    summary.failures = 1;
    summary.terminal_state = "FailedPartial";
    summary.last_handoff_state = "CaptureFailedResumeSkipped";
    summary.last_error_category = "image_event_timeout";
    summary.last_error_detail = "WPD timeout \"diagnostic\"";
    (void)PersistLiveViewHandoffSummary(root, "run-summary", "CAM-A", summary);
    const auto body = ReadAll(path);
    Check(body.find("\"requestedHandoffs\": 10") != std::string::npos &&
              body.find("\"attemptedHandoffs\": 6") != std::string::npos &&
              body.find("\"completeHandoffs\": 5") != std::string::npos,
        "replacement handoff summary should retain requested, attempted, and completed counts");
    Check(body.find("\"terminalState\": \"FailedPartial\"") != std::string::npos &&
              body.find("\"lastErrorCategory\": \"image_event_timeout\"") != std::string::npos,
        "handoff summary should persist the terminal state and failure category");
    Check(body.find("\"lastErrorDetail\": \"WPD timeout \\\"diagnostic\\\"\"") != std::string::npos,
        "handoff summary should persist the controlled failure detail with JSON escaping");
    Check(body.find("\"previewFramesPersisted\": false") != std::string::npos,
        "handoff summary should state that preview frames were not persisted");
    Check(!fs::exists(path.string() + ".partial"), "successful summary replacement should leave no partial file");
    EvidenceWriter evidence(root, "run-summary", "test");
    evidence.GenerateRedactedReport(root / "reports");
    Check(fs::exists(root / "reports" / "run-summary" / "handoff-summary.json"),
        "handoff-only evidence should be exportable as an anonymous report");
    Check(fs::exists(root / "reports" / "run-summary" / "report.md"),
        "handoff-only report should include its manifest");
    fs::remove_all(root);
}

void TestStandaloneLiveViewSummaryReport() {
    const auto root = NewTestRoot("live-view-summary-report");
    EvidenceWriter evidence(root / "artifacts", "run-live-view", "test");
    {
        std::ofstream summary(evidence.RunRoot() / "live-view-summary.json", std::ios::trunc);
        summary << "{\"schemaVersion\":\"phase0.live-view-summary.v1\",\"cameraAlias\":\"CAM-A\"}\n";
    }
    evidence.GenerateRedactedReport(root / "reports");
    Check(fs::exists(root / "reports" / "run-live-view" / "live-view-summary.json"),
        "standalone Live View summary should be exportable as an anonymous report");
    Check(!fs::exists(root / "reports" / "run-live-view" / "transaction-events.jsonl"),
        "standalone Live View report should not invent transaction events");
    fs::remove_all(root);
}

void TestSdkReadOnlyCommandTraceRejectsMutatingBoundaries() {
    SdkCommandTrace trace;
    trace.cap_get_count = 9;
    trace.cap_get_array_count = 8;
    trace.cap_set_count = 3;
    trace.control_plane_cap_set_count = 3;
    trace.sdk_session_opened = true;
    trace.sdk_session_closed = true;
    Check(!ValidateSdkReadOnlyCommandTrace(trace),
        "read-only SDK status trace should accept capability reads and control-plane CapSet only");

    const auto expect_rejected = [&](SdkCommandTrace invalid, std::string_view reason) {
        const auto failure = ValidateSdkReadOnlyCommandTrace(invalid);
        Check(failure && *failure == reason,
            std::string("read-only SDK status trace should reject ") + std::string(reason));
    };

    auto photographic_write = trace;
    photographic_write.photographic_setting_cap_set_count = 1;
    ++photographic_write.cap_set_count;
    expect_rejected(photographic_write, "photographic_setting_cap_set");

    auto capture = trace;
    capture.cap_start_count = 1;
    capture.capture_start_count = 1;
    expect_rejected(capture, "capture_started");

    auto non_capture_start = trace;
    non_capture_start.cap_start_count = 1;
    non_capture_start.non_capture_start_count = 1;
    expect_rejected(non_capture_start, "non_capture_cap_start");

    auto unknown_start = trace;
    unknown_start.cap_start_count = 1;
    unknown_start.unknown_cap_start_count = 1;
    expect_rejected(unknown_start, "unknown_cap_start");

    auto start_count_inconsistent = trace;
    start_count_inconsistent.cap_start_count = 2;
    start_count_inconsistent.capture_start_count = 1;
    expect_rejected(start_count_inconsistent, "cap_start_count_inconsistent");

    auto live_view = trace;
    live_view.live_view_start_count = 1;
    expect_rejected(live_view, "live_view_started");

    auto storage_routing = trace;
    storage_routing.storage_routing_cap_set_count = 1;
    ++storage_routing.cap_set_count;
    expect_rejected(storage_routing, "storage_routing_cap_set");

    auto live_view_control = trace;
    live_view_control.live_view_control_cap_set_count = 1;
    ++live_view_control.cap_set_count;
    expect_rejected(live_view_control, "live_view_control_cap_set");

    auto unknown_write = trace;
    unknown_write.unexpected_cap_set_count = 1;
    ++unknown_write.cap_set_count;
    expect_rejected(unknown_write, "unexpected_cap_set");

    auto count_inconsistent = trace;
    count_inconsistent.cap_set_count = 4;
    expect_rejected(count_inconsistent, "trace_count_inconsistent");

    auto no_reads = trace;
    no_reads.cap_get_count = 0;
    no_reads.cap_get_array_count = 0;
    expect_rejected(no_reads, "no_capability_reads");

    auto not_opened = trace;
    not_opened.sdk_session_opened = false;
    expect_rejected(not_opened, "session_not_opened");

    auto not_closed = trace;
    not_closed.sdk_session_closed = false;
    expect_rejected(not_closed, "session_not_closed");
}

void TestSdkTraceBoundaryRecordsBeforeFakeMaidEntryExactlyOnce() {
    SdkCommandTrace trace;
    int entry_calls = 0;
    const auto fake_entry = [&entry_calls] {
        ++entry_calls;
        return 37;
    };

    const int first = InvokeSdkTraceBoundary(
        trace,
        {SdkTraceCommand::capability_start, SdkTraceCapability::unknown_start, false},
        fake_entry);
    const int second = InvokeSdkTraceBoundary(
        trace,
        {SdkTraceCommand::capability_start, SdkTraceCapability::non_capture_start, false},
        fake_entry);
    const int third = InvokeSdkTraceBoundary(
        trace,
        {SdkTraceCommand::capability_set, SdkTraceCapability::storage_routing, false},
        fake_entry);
    const int fourth = InvokeSdkTraceBoundary(
        trace,
        {SdkTraceCommand::capability_set, SdkTraceCapability::live_view_control, true},
        fake_entry);

    Check(first == 37 && second == 37 && third == 37 && fourth == 37 && entry_calls == 4,
        "MAID trace seam should invoke the fake entry exactly once per boundary call");
    Check(trace.cap_start_count == 2 && trace.unknown_cap_start_count == 1 &&
              trace.non_capture_start_count == 1 && trace.capture_start_count == 0,
        "MAID trace seam should classify every fake CapStart exactly once");
    Check(trace.cap_set_count == 2 && trace.storage_routing_cap_set_count == 1 &&
              trace.live_view_control_cap_set_count == 1 && trace.live_view_start_count == 1,
        "MAID trace seam should classify storage and Live View CapSet at the production boundary");
}

void TestSdkStatusProcessRoutingIsSeparateAndNarrow() {
    SdkStatusProcessRouting routing;
    routing.sdk_status_executor_selected = true;
    routing.sdk_enumeration_count = 1;
    routing.sdk_status_probe_count = 1;
    Check(!ValidateSdkStatusProcessRouting(routing),
        "sdk-status process routing should allow one SDK enumeration and one read-only status probe only");

    auto single_v3 = routing;
    single_v3.single_identity_v3_selected = true;
    single_v3.wpd_identity_enumeration_count = 1;
    Check(!ValidateSdkStatusProcessRouting(single_v3),
        "SingleCamera sdk-status should allow one read-only WPD identity enumeration before the SDK probe");

    const auto expect_rejected = [&](SdkStatusProcessRouting invalid, std::string_view reason) {
        const auto failure = ValidateSdkStatusProcessRouting(invalid);
        Check(failure && *failure == reason,
            std::string("sdk-status process routing should reject ") + std::string(reason));
    };

    auto no_executor = routing;
    no_executor.sdk_status_executor_selected = false;
    expect_rejected(no_executor, "sdk_status_executor_not_selected");

    auto enumeration_count = routing;
    enumeration_count.sdk_enumeration_count = 2;
    expect_rejected(enumeration_count, "sdk_enumeration_count_inconsistent");

    auto probe_count = routing;
    probe_count.sdk_status_probe_count = 2;
    expect_rejected(probe_count, "sdk_status_probe_count_inconsistent");

    auto unmarked_identity_enumeration = routing;
    unmarked_identity_enumeration.wpd_identity_enumeration_count = 1;
    expect_rejected(unmarked_identity_enumeration, "single_identity_v3_routing_inconsistent");

    auto missing_identity_enumeration = routing;
    missing_identity_enumeration.single_identity_v3_selected = true;
    expect_rejected(missing_identity_enumeration, "single_identity_v3_routing_inconsistent");

    auto repeated_identity_enumeration = routing;
    repeated_identity_enumeration.single_identity_v3_selected = true;
    repeated_identity_enumeration.wpd_identity_enumeration_count = 2;
    expect_rejected(repeated_identity_enumeration, "wpd_identity_enumeration_count_inconsistent");

    auto wpd_call = routing;
    wpd_call.wpd_call_count = 1;
    expect_rejected(wpd_call, "wpd_call_routed");

    auto capture_call = routing;
    capture_call.capture_call_count = 1;
    expect_rejected(capture_call, "capture_call_routed");

    auto delete_call = routing;
    delete_call.delete_call_count = 1;
    expect_rejected(delete_call, "delete_call_routed");
}

void TestSdkStatusSummaryIsReadOnlyAndRedacted() {
    const auto root = NewTestRoot("sdk-status-summary");
    const CameraInfo camera{"Nikon D810", "1.14", "S", "private-camera-identity"};
    SdkCameraStatus status;
    status.firmware = "1.14";
    status.live_view_status = "off";
    status.live_view_selector = "photo";
    status.live_view_status_available = true;
    status.live_view_selector_available = true;
    status.live_view_prohibit_mask = 0;
    status.file_type = {true, "enum", "available", "unsigned", 42, 1, std::nullopt, {41, 42}, {}};
    status.compression_level = {true, "unsigned", "available", "unsigned", 9, std::nullopt, std::nullopt, {}, {}};
    status.exposure_mode = {false, "enum", "get-not-supported", "unsupported", std::nullopt, std::nullopt, std::nullopt, {}, {}};
    std::string escaped_label{"q\"\\"};
    escaped_label.push_back('\x01');
    escaped_label.push_back('\b');
    escaped_label.push_back('\f');
    escaped_label.push_back('\n');
    escaped_label.push_back(static_cast<char>(0x80));
    status.shutter_speed = {true, "enum", "available", "packed-string", std::nullopt, 1, escaped_label, {}, {"1/125", escaped_label}};
    status.aperture = {true, "enum", "available", "string", std::nullopt, 0, "f/2.8", {}, {"f/2.8", "f/4.0"}};
    status.sensitivity = {false, "enum", "invalid-shape", "unsupported", std::nullopt, std::nullopt, std::nullopt, {}, {}};
    status.wb_mode = {false, "unsigned", "read-error", "unsupported", std::nullopt, std::nullopt, std::nullopt, {}, {}};
    status.focus_mode = {false, "enum", "get-array-not-supported", "unsupported", std::nullopt, std::nullopt, std::nullopt, {}, {}};
    status.command_trace.cap_get_count = 9;
    status.command_trace.cap_get_array_count = 8;
    status.command_trace.cap_set_count = 3;
    status.command_trace.control_plane_cap_set_count = 3;
    status.command_trace.sdk_session_opened = true;
    status.command_trace.sdk_session_closed = true;
    SdkStatusProcessRouting routing;
    routing.sdk_status_executor_selected = true;
    routing.single_identity_v3_selected = true;
    routing.sdk_enumeration_count = 1;
    routing.sdk_status_probe_count = 1;
    routing.wpd_identity_enumeration_count = 1;

    const auto path = PersistSdkStatusSummary(
        root / "artifacts", "run-sdk-status", "CAM-A", camera, status, routing);
    const auto body = ReadAll(path);
    Check(body.find("\"schemaVersion\": \"phase0.sdk-status-summary.v5\"") != std::string::npos &&
               body.find("\"liveViewStatus\": \"off\"") != std::string::npos &&
               body.find("\"liveViewSelector\": \"photo\"") != std::string::npos &&
               body.find("\"liveViewProhibitMask\": 0") != std::string::npos,
        "SDK status summary should retain read-only Live View readiness values");
    Check(body.find("\"processRoutingProof\": {\n") != std::string::npos &&
              body.find("\"singleIdentityV3Selected\": true") != std::string::npos &&
              body.find("\"wpdIdentityEnumerationCount\": 1") != std::string::npos &&
              body.find("\"wpdCallCount\": 0") != std::string::npos &&
              body.find("\"deleteCallCount\": 0") != std::string::npos,
        "SDK status summary should keep process routing proof separate from the MAID trace");
    Check(body.find("\"fileType\": {\"available\": true, \"capType\": \"enum\", \"probeState\": \"available\", \"valueType\": \"unsigned\", \"currentValue\": 42, \"currentIndex\": 1, \"currentLabel\": null, \"numericValues\": [41, 42], \"stringValues\": []}") != std::string::npos &&
              body.find("\"compressionLevel\": {\"available\": true, \"capType\": \"unsigned\", \"probeState\": \"available\", \"valueType\": \"unsigned\", \"currentValue\": 9, \"currentIndex\": null, \"currentLabel\": null, \"numericValues\": [], \"stringValues\": []}") != std::string::npos &&
              body.find("\"imageSize\": {\"available\": false, \"capType\": \"unsupported\", \"probeState\": \"not-advertised\", \"valueType\": \"unsupported\", \"currentValue\": null, \"currentIndex\": null, \"currentLabel\": null, \"numericValues\": [], \"stringValues\": []}") != std::string::npos,
        "SDK status summary should serialize opaque capability values and fail closed for unavailable settings");
    Check(body.find("\"valueType\": \"packed-string\"") != std::string::npos &&
              body.find("\"valueType\": \"string\"") != std::string::npos &&
              body.find("q\\\"\\\\\\u0001\\u0008\\u000C\\n\\u0080") != std::string::npos,
        "SDK status summary should serialize SDK string labels without inferred meanings and with byte-safe JSON escaping");
    Check(body.find("\"probeState\": \"get-not-supported\"") != std::string::npos &&
              body.find("\"probeState\": \"get-array-not-supported\"") != std::string::npos &&
              body.find("\"probeState\": \"invalid-shape\"") != std::string::npos &&
              body.find("\"probeState\": \"read-error\"") != std::string::npos,
        "SDK status summary should distinguish anonymous fail-closed probe states");
    SdkCameraStatus unsupported_status;
    unsupported_status.aperture = {false, "unsupported", "unsupported-type", "unsupported", std::nullopt, std::nullopt, std::nullopt, {}, {}};
    unsupported_status.command_trace = status.command_trace;
    const auto unsupported_path = PersistSdkStatusSummary(
        root / "artifacts", "run-sdk-status-unsupported", "CAM-A", camera, unsupported_status, routing);
    Check(ReadAll(unsupported_path).find("\"probeState\": \"unsupported-type\"") != std::string::npos,
        "SDK status summary should preserve unsupported capability types without a payload");
    Check(body.find("\"cameraSettingReadOnlyProbe\": true") != std::string::npos &&
              body.find("\"cameraSettingWriteAttempted\": false") != std::string::npos &&
              body.find("\"photographicSettingCapSetCount\": 0") != std::string::npos &&
              body.find("\"captureStartCount\": 0") != std::string::npos &&
              body.find("\"liveViewStartCount\": 0") != std::string::npos &&
              body.find("\"readOnlyContractValid\": true") != std::string::npos &&
              body.find("\"sdkControlPlaneCallbackRegistrationMayUseCapSet\": true") != std::string::npos &&
              body.find("\"cameraSettingsChanged\": false") != std::string::npos &&
              body.find("\"liveViewStarted\": false") != std::string::npos &&
              body.find("\"sdkSessionClosed\": true") != std::string::npos,
        "SDK status summary should distinguish read-only setting access from SDK control-plane setup");
    Check(body.find(camera.stable_identity) == std::string::npos,
        "SDK status summary must not contain the real camera identity");

    bool overwrite_rejected = false;
    try {
        (void)PersistSdkStatusSummary(root / "artifacts", "run-sdk-status", "CAM-A", camera, status, routing);
    } catch (const std::runtime_error&) {
        overwrite_rejected = true;
    }
    Check(overwrite_rejected, "SDK status summary must not overwrite existing evidence");

    const auto expect_invalid_status = [&](std::string_view name, SdkCameraStatus invalid_status) {
        const auto invalid_root = root / "invalid" / std::string(name);
        bool invalid_rejected = false;
        try {
            (void)PersistSdkStatusSummary(invalid_root, "run-invalid", "CAM-A", camera, invalid_status, routing);
        } catch (const std::runtime_error&) {
            invalid_rejected = true;
        }
        Check(invalid_rejected && !fs::exists(invalid_root / "run-invalid" / "sdk-status-summary.json") &&
                  !fs::exists(invalid_root / "run-invalid" / "sdk-status-summary.json.partial"),
            std::string("SDK status summary must reject ") + std::string(name) + " before writing JSON");
    };
    SdkCameraStatus invalid_status;
    invalid_status.file_type = {true, "enum", "available", "unsigned", 1256, 256, std::nullopt, {}, {}};
    for (std::uint32_t value = 1000; value <= 1256; ++value) {
        invalid_status.file_type.numeric_values.push_back(value);
    }
    expect_invalid_status("too-many-enum-values", invalid_status);
    invalid_status.file_type = {true, "enum", "available", "unsigned", 42, std::nullopt, std::nullopt, {42}, {}};
    expect_invalid_status("missing-enum-index", invalid_status);
    invalid_status.file_type = {true, "enum", "available", "unsigned", 42, 1, std::nullopt, {42}, {}};
    expect_invalid_status("out-of-range-enum-index", invalid_status);
    invalid_status.file_type = {true, "enum", "available", "unsigned", 42, 0, std::nullopt, {41}, {}};
    expect_invalid_status("mismatched-enum-value", invalid_status);
    invalid_status.file_type = {false, "enum", "get-not-supported", "unsupported", 42, std::nullopt, std::nullopt, {}, {}};
    expect_invalid_status("unavailable-value-payload", invalid_status);
    invalid_status.file_type = {true, "enum", "available", "packed-string", std::nullopt, 1, "B", {}, {"A", "C"}};
    expect_invalid_status("packed-string-index-mismatch", invalid_status);
    invalid_status.file_type = {true, "enum", "available", "string", std::nullopt, 0, "A", {}, {}};
    expect_invalid_status("string-empty-labels", invalid_status);
    invalid_status.file_type = {true, "enum", "available", "packed-string", std::nullopt, 0, "A", {}, {"A"}};
    for (int value = 1; value <= 256; ++value) invalid_status.file_type.string_values.push_back("label-" + std::to_string(value));
    expect_invalid_status("too-many-string-labels", invalid_status);

    EvidenceWriter evidence(root / "artifacts", "run-sdk-status", "test");
    evidence.GenerateRedactedReport(root / "reports");
    const auto report_copy = root / "reports" / "run-sdk-status" / "sdk-status-summary.json";
    Check(fs::exists(report_copy), "SDK status summary should be exportable as an anonymous report");
    Check(ReadAll(report_copy).find(camera.stable_identity) == std::string::npos,
        "exported SDK status report must remain identity-redacted");
    fs::remove_all(root);
}

void TestPackedStringLabelParserBounds() {
    const auto valid = ParsePackedStringLabels({'A', 0, 'B', 0}, 2, 4);
    Check(valid && *valid == std::vector<std::string>({"A", "B"}),
        "packed SDK labels should require and retain NUL-terminated values");
    Check(!ParsePackedStringLabels({'A', 0, 'B', 0}, 1, 4) &&
              !ParsePackedStringLabels({'A', 'B'}, 2, 4) &&
              !ParsePackedStringLabels({'A', 0, 0}, 2, 3),
        "packed SDK labels should reject excessive, unterminated, and empty entries");
    std::vector<unsigned char> oversized(65U * 1024U, 'A');
    oversized.back() = 0;
    Check(!ParsePackedStringLabels(oversized, 256, 64U * 1024U),
        "packed SDK labels should reject input beyond the 64 KiB DoS bound");
}

void TestWpdVendorOpcodeDiagnosticsAreFailClosedAndRedacted() {
    WpdVendorOpcodeDiagnostic query_not_advertised;
    query_not_advertised.validation_state = "vendor_opcode_query_not_advertised";
    Check(query_not_advertised.query_send_hresult == "not_sent" &&
              query_not_advertised.query_common_hresult == "not_read" &&
              !query_not_advertised.read_only_command_sent,
        "an unadvertised vendor query must not present unperformed send or result reads as S_OK");
    WpdVendorOpcodeDiagnostic supported_commands_failed;
    supported_commands_failed.validation_state = "supported_commands_query_failed";
    supported_commands_failed.supported_commands_hresult = "0x80004005";
    Check(supported_commands_failed.query_send_hresult == "not_sent" &&
              supported_commands_failed.query_common_hresult == "not_read" &&
              !supported_commands_failed.vendor_opcode_query_advertised,
        "a supported-command query failure must remain distinct from a successful vendor opcode query");

    const auto present = SummarizeWpdVendorOpcodes({0x9207U, 0x9208U, 0x9207U});
    Check(present.available && !present.malformed && present.item_count == 3 && present.unique_count == 2 &&
              present.vendor_capture_9207_advertised,
        "vendor opcode aggregation should count duplicates without exposing individual values");
    const auto absent = SummarizeWpdVendorOpcodes({0x9208U});
    Check(absent.available && !absent.vendor_capture_9207_advertised,
        "available vendor opcode data should distinguish an absent 0x9207 from an unavailable query");
    const auto malformed = SummarizeWpdVendorOpcodes({0x9207U, std::nullopt});
    Check(!malformed.available && malformed.malformed && !malformed.vendor_capture_9207_advertised,
        "malformed vendor opcode data must fail closed rather than claim an advertised capture opcode");

    const auto root = NewTestRoot("wpd-status-summary");
    const CameraInfo camera{"Nikon D810", "1.14", "S", "private-camera-identity"};
    WpdStatusSummary status;
    status.target_validation_state = "selected_without_valid_object_ids_option";
    status.command_options_hresult = "0x00000000";
    status.option_value_hresult = "0x00000000";
    status.functional_object_count = 1;
    status.compatible_target_count = 1;
    status.selected_target = true;
    status.vendor_opcode_validation_state = "vendor_opcode_query_complete";
    status.supported_commands_hresult = "0x00000000";
    status.vendor_opcode_query_send_hresult = "0x00000000";
    status.vendor_opcode_query_common_hresult = "0x00000000";
    status.wpd_still_image_capture_command_advertised = true;
    status.vendor_opcode_query_advertised = true;
    status.read_only_command_sent = true;
    status.vendor_opcode_collection_available = true;
    status.vendor_opcode_item_count = present.item_count;
    status.vendor_opcode_unique_count = present.unique_count;
    status.vendor_capture_9207_advertised = true;
    status.standard_opcode_100e_advertisement_state = "unavailable_via_wpd_extension_api";

    const auto path = PersistWpdStatusSummary(root / "artifacts", "run-wpd-status", "CAM-A", camera, status);
    const auto body = ReadAll(path);
    Check(body.find("\"vendorCapture9207Advertised\": true") != std::string::npos &&
              body.find("\"standardOpcode100eAdvertisementAvailable\": false") != std::string::npos &&
              body.find("\"standardOpcode100eAdvertisementState\": \"unavailable_via_wpd_extension_api\"") != std::string::npos &&
              body.find("\"wpdRequestedAccess\": \"read-only\"") != std::string::npos &&
              body.find("\"readOnlyAccess\": true") != std::string::npos &&
              body.find("\"nonMutatingProbe\": true") != std::string::npos &&
              body.find("\"readOnlyCommandSent\": true") != std::string::npos &&
              body.find("\"captureCommandSent\": false") != std::string::npos &&
              body.find("\"vendorOperationExecuted\": false") != std::string::npos,
        "WPD summary should record the read-only command boundary and unavailable standard-opcode evidence");
    Check(body.find(camera.stable_identity) == std::string::npos,
        "WPD summary must not contain real camera identity");
    bool overwrite_rejected = false;
    try {
        (void)PersistWpdStatusSummary(root / "artifacts", "run-wpd-status", "CAM-A", camera, status);
    } catch (const std::runtime_error&) {
        overwrite_rejected = true;
    }
    Check(overwrite_rejected, "WPD summary must not overwrite existing evidence");
    EvidenceWriter evidence(root / "artifacts", "run-wpd-status", "test");
    evidence.GenerateRedactedReport(root / "reports");
    Check(fs::exists(root / "reports" / "run-wpd-status" / "wpd-status-summary.json"),
        "WPD summary should be included in the redacted report");

    WpdStatusSummary failed = status;
    failed.vendor_opcode_validation_state = "vendor_opcode_query_send_failed";
    failed.read_only_command_sent = true;
    failed.vendor_opcode_collection_available = false;
    failed.vendor_capture_9207_advertised = false;
    failed.requested_access = "read-write";
    failed.read_only_access = false;
    const auto failed_path = PersistWpdStatusSummary(root / "artifacts", "run-wpd-status-failed", "CAM-A", camera, failed);
    const auto failed_body = ReadAll(failed_path);
    Check(failed_body.find("\"vendorOpcodeCollectionAvailable\": false") != std::string::npos &&
              failed_body.find("\"wpdRequestedAccess\": \"read-write\"") != std::string::npos &&
              failed_body.find("\"readOnlyAccess\": false") != std::string::npos &&
              failed_body.find("\"nonMutatingProbe\": true") != std::string::npos,
        "query failure must remain distinct from an available list without 0x9207");
    fs::remove_all(root);
}

void TestRedactedReportPublishesAtomically() {
    const auto root = NewTestRoot("redacted-report-atomic");
    EvidenceWriter evidence(root / "artifacts", "run-atomic", "test");
    {
        std::ofstream summary(evidence.RunRoot() / "summary.json", std::ios::trunc);
        summary << "{\"schemaVersion\":\"phase0.summary.v1\"}\n";
    }
    const auto reports = root / "reports";
    const auto final_directory = reports / "run-atomic";
    const auto partial_directory = reports / "run-atomic.partial";
    fs::create_directories(partial_directory);
    bool partial_collision_rejected = false;
    try {
        evidence.GenerateRedactedReport(reports);
    } catch (const std::runtime_error&) {
        partial_collision_rejected = true;
    }
    Check(partial_collision_rejected && !fs::exists(final_directory),
        "an existing partial report directory must block publication without exposing a final report");
    fs::remove_all(partial_directory);
    evidence.GenerateRedactedReport(reports);
    Check(fs::exists(final_directory / "summary.json") && fs::exists(final_directory / "report.md") &&
              !fs::exists(partial_directory),
        "redacted reports must become visible only after complete staging and directory rename");
    bool overwrite_rejected = false;
    try {
        evidence.GenerateRedactedReport(reports);
    } catch (const std::runtime_error&) {
        overwrite_rejected = true;
    }
    Check(overwrite_rejected, "atomic redacted report publication must retain overwrite rejection");
    fs::remove_all(root);
}

void TestWpdCorrelationSummaryIsAnonymousAndReportable() {
    const auto root = NewTestRoot("wpd-correlation-summary");
    WpdCorrelationRunSummary summary;
    summary.sample_count = 3;
    summary.device_datetime_available_count = 3;
    summary.reopen_advance_count = 2;
    summary.jpeg_count = 12;
    summary.dated_jpeg_count = 10;
    summary.latest_date_less_than_device_count = 3;
    summary.latest_date_equal_device_count = 1;
    summary.wpd_sessions_closed = 3;
    summary.terminal_state = "Complete";
    const auto path = PersistWpdCorrelationSummary(root / "artifacts", "run-wpd-correlation", "CAM-A", summary);
    const auto body = ReadAll(path);
    Check(body.find("\"sampleCount\": 3") != std::string::npos &&
              body.find("\"reopenAdvanceCount\": 2") != std::string::npos &&
              body.find("\"datedJpegCount\": 10") != std::string::npos &&
              body.find("\"readOnlyObservation\": true") != std::string::npos &&
              body.find("\"captureCommandSent\": false") != std::string::npos &&
              body.find("\"vendorOperationExecuted\": false") != std::string::npos &&
              body.find("\"cameraObjectDeleteAttempted\": false") != std::string::npos,
        "correlation summary should retain only aggregate read-only observation evidence");
    Check(body.find("object-id") == std::string::npos && body.find("private-camera") == std::string::npos &&
              body.find("rawDeviceDatetime") == std::string::npos && body.find("rawObjectDateCreated") == std::string::npos,
        "correlation summary must not expose raw WPD dates, object IDs, or device identities");
    EvidenceWriter evidence(root / "artifacts", "run-wpd-correlation", "test");
    evidence.GenerateRedactedReport(root / "reports");
    Check(fs::exists(root / "reports" / "run-wpd-correlation" / "wpd-correlation-summary.json"),
        "redacted report should include the anonymous correlation summary");
    fs::remove_all(root);
}

void TestWpdCorrelationFailureEvidenceAndArgumentContract() {
    Check(!ValidateWpdCorrelationArguments("wpd-correlation-status", true, true, 3, 1500),
        "bounded correlation arguments should be accepted");
    Check(ValidateWpdCorrelationArguments("capture-single", true, false, 3, 1500).has_value() &&
              ValidateWpdCorrelationArguments("wpd-correlation-status", true, false, 11, 0).has_value() &&
              ValidateWpdCorrelationArguments("wpd-correlation-status", false, true, 1, 60001).has_value(),
        "correlation-only options and diagnostic duration bounds must be enforced");

    CorrelationFake success;
    const auto complete = ExecuteWpdCorrelationSamples(success, "private-id", 2, 0);
    Check(complete.terminal_state == "Complete" && complete.sample_count == 2 && complete.wpd_sessions_closed == 2 &&
              success.sequence == "open;read;close;open;read;close;",
        "correlation success must fully close every read-only WPD session before reopening");

    CorrelationFake read_failure(CorrelationFake::Failure::read);
    const auto failed = ExecuteWpdCorrelationSamples(read_failure, "private-id", 2, 0);
    Check(failed.terminal_state == "Failed" && failed.failed_stage == "read" && failed.wpd_sessions_closed == 1 &&
              read_failure.opens == 1 && read_failure.reads == 1 && read_failure.closes == 1,
        "read failure must close exactly once and persist fail-closed evidence");

    const auto root = NewTestRoot("wpd-correlation-close-failure");
    CorrelationFake close_failure(CorrelationFake::Failure::close);
    const auto close_failed = ExecuteWpdCorrelationSamples(close_failure, "private-id", 1, 0);
    const auto path = PersistWpdCorrelationSummary(root / "artifacts", "run-wpd-correlation-close-failure", "CAM-A", close_failed);
    const auto body = ReadAll(path);
    Check(close_failed.terminal_state == "Failed" && close_failed.failed_stage == "close" &&
              close_failed.wpd_sessions_closed == 0 && close_failure.closes == 1 &&
              body.find("\"terminalState\": \"Failed\"") != std::string::npos &&
              body.find("\"failedStage\": \"close\"") != std::string::npos &&
              body.find("private-id") == std::string::npos,
        "close failure must be anonymous, fail closed, and never trigger a second close");
    fs::remove_all(root);
}

void TestSuccessfulLiveViewHandoff() {
    const auto root = NewTestRoot("live-view-handoff-success");
    FakeLiveViewTransport live_view;
    HybridWpdFake wpd;
    HybridSdkFake sdk;
    EvidenceWriter evidence(root / "artifacts", "run-live-view-handoff-success", sdk.SdkVersion());
    const auto result = ExecuteLiveViewHandoffOnce(
        live_view, "sdk-private-a",
        [&] { return ExecuteHybridCaptureOnce(wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a"); },
        evidence, "handoff-1", "CAM-A", 1, 0, std::chrono::milliseconds::zero());
    Check(result.terminal_state == "Complete", "live view handoff should complete");
    Check(result.wpd_capture_started && result.wpd_capture_complete && result.resume_attempted,
        "successful handoff should capture and resume");
    Check(live_view.OpenAttempts() == 2 && live_view.StopAttempts() == 2 && live_view.CloseAttempts() == 2,
        "successful handoff should run exactly one preview before and after capture");
    Check(wpd.opens == 2 && wpd.observes == 1 && wpd.delete_attempts == 1 &&
              sdk.opens == 1 && sdk.captures == 1,
        "successful handoff should execute one cleanup-confirmed hybrid capture without retry");
    std::size_t jpeg_count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(evidence.RunRoot())) {
        if (entry.is_regular_file() && entry.path().extension() == ".jpg") ++jpeg_count;
    }
    Check(jpeg_count == 1, "only the WPD original, not preview frames, should be persisted");
    fs::remove_all(root);
}

void TestWpdCommandTargetPolicyIsReported() {
    WpdTransport default_policy;
    WpdTransport functional(WpdCommandTargetPolicy::functional);
    WpdTransport omit(WpdCommandTargetPolicy::omit);
    Check(default_policy.SdkVersion() == "windows-wpd-1;access=read-write;qos=impersonation;command-target=functional",
        "the default WPD command target policy must remain functional");
    Check(functional.SdkVersion() == "windows-wpd-1;access=read-write;qos=impersonation;command-target=functional",
        "functional WPD command target policy must be recorded in SDK version evidence");
    Check(omit.SdkVersion() == "windows-wpd-1;access=read-write;qos=impersonation;command-target=omit",
        "omit WPD command target policy must be recorded in SDK version evidence");
}

void TestWpdSpoolCountsEveryPayloadType() {
    Check(!WpdContentTypeCountsAsSpoolPayload(WPD_CONTENT_TYPE_FOLDER) &&
              !WpdContentTypeCountsAsSpoolPayload(WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT),
        "WPD folders and functional nodes are structural and must not make an empty card fail");
    Check(WpdContentTypeCountsAsSpoolPayload(WPD_CONTENT_TYPE_IMAGE) &&
              WpdContentTypeCountsAsSpoolPayload(WPD_CONTENT_TYPE_VIDEO) &&
              WpdContentTypeCountsAsSpoolPayload(WPD_CONTENT_TYPE_GENERIC_FILE),
        "JPEG, NEF-like image objects, video, and generic sidecar files must all make the spool non-empty");
}

void TestWpdSpoolStatusSummaryIsAnonymousAndReportable() {
    const auto root = NewTestRoot("wpd-spool-status-summary");
    WpdSpoolStatusSummary status;
    status.payload_object_count = 90;
    status.wpd_sessions_closed = 1;
    status.terminal_state = "Complete";
    const auto path = PersistWpdSpoolStatusSummary(
        root / "artifacts", "run-wpd-spool-status", "CAM-A", status);
    const auto body = ReadAll(path);
    Check(body.find("\"payloadObjectCount\": 90") != std::string::npos &&
              body.find("\"spoolState\": \"NON_EMPTY\"") != std::string::npos &&
              body.find("\"readOnlyObservation\": true") != std::string::npos &&
              body.find("\"captureCommandSent\": false") != std::string::npos &&
              body.find("\"cameraObjectDeleteAttempted\": false") != std::string::npos &&
              body.find("\"vendorOperationExecuted\": false") != std::string::npos &&
              body.find("\"objectIdentifiersIncluded\": false") != std::string::npos &&
              body.find("\"objectNamesIncluded\": false") != std::string::npos,
        "spool status summary must retain only aggregate non-mutating evidence");
    Check(body.find("private-camera") == std::string::npos && body.find("object-id") == std::string::npos,
        "spool status summary must not expose object or camera identities");
    EvidenceWriter evidence(root / "artifacts", "run-wpd-spool-status", "test");
    evidence.GenerateRedactedReport(root / "reports");
    Check(fs::exists(root / "reports" / "run-wpd-spool-status" / "wpd-spool-status-summary.json"),
        "spool status summary should be included in the redacted report");
    fs::remove_all(root);
}

void TestOperatorGateReadyThenContinue() {
    const auto root = NewTestRoot("operator-gate-ready");
    OperatorGate gate(
        root / "artifacts", "fault_A-1", std::chrono::seconds(2),
        "usb-disconnect", "after_sdk_close_before_wpd_recovery");
    std::ostringstream output;
    std::atomic<bool> marker_created{false};
    std::exception_ptr marker_error;
    std::thread marker([&] {
        try {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!fs::exists(gate.ReadyPath()) && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!fs::exists(gate.ReadyPath())) throw std::runtime_error("operator gate did not publish ready artifact");
            std::ofstream continue_marker(gate.ReadyPath().parent_path() / "continue", std::ios::out);
            if (!continue_marker) throw std::runtime_error("cannot create operator gate continue marker");
            continue_marker << "continue\n";
            marker_created = static_cast<bool>(continue_marker);
        } catch (...) {
            marker_error = std::current_exception();
        }
    });
    fs::path marker_path;
    try {
        marker_path = gate.AwaitContinue(output);
    } catch (const std::exception& error) {
        Check(false, "operator gate ready/continue failed: " + std::string(error.what()));
    }
    marker.join();
    Check(marker_error == nullptr && marker_created, "operator gate test worker should create its marker without exception");
    Check(marker_path.filename() == "continue" && fs::is_regular_file(marker_path),
        "operator gate should continue only after its marker appears");
    const auto ready = ReadAll(gate.ReadyPath());
    Check(ready.find("fault_A-1") != std::string::npos &&
              ready.find("\"scenario\": \"usb-disconnect\"") != std::string::npos &&
              ready.find("\"stage\": \"after_sdk_close_before_wpd_recovery\"") != std::string::npos &&
              ready.find("identity") == std::string::npos,
        "operator gate ready artifact should identify the gate without camera identities");
    Check(output.str().find("OPERATOR GATE READY") != std::string::npos &&
              output.str().find(marker_path.string()) != std::string::npos,
        "operator gate should announce and flush the continue marker path");
    fs::remove_all(root);
}

void TestOperatorGateTimeoutRefusesOverwrite() {
    const auto root = NewTestRoot("operator-gate-timeout");
    const auto root_artifacts = root / "artifacts";
    OperatorGate timeout_gate(root_artifacts, "timeout", std::chrono::seconds(1));
    std::ostringstream output;
    try {
        static_cast<void>(timeout_gate.AwaitContinue(output));
        Check(false, "operator gate without marker should time out");
    } catch (const TransportError& error) {
        Check(error.Category() == "operator_gate_timeout", "operator gate timeout should retain typed category");
    }
    const auto ready = timeout_gate.ReadyPath();
    const auto before = ReadAll(ready);
    Check(fs::exists(ready.parent_path()) && !fs::exists(ready.parent_path() / "continue"),
        "operator gate timeout should retain artifacts without creating a marker");
    try {
        OperatorGate duplicate(root_artifacts, "timeout", std::chrono::seconds(1));
        static_cast<void>(duplicate.AwaitContinue(output));
        Check(false, "existing operator gate directory must not be overwritten");
    } catch (const TransportError& error) {
        Check(error.Category() == "operator_gate_exists", "existing operator gate should be rejected");
    }
    Check(ReadAll(ready) == before, "duplicate operator gate must not overwrite ready artifact");
    fs::remove_all(root);
}

void TestLiveViewStopFailureBlocksWpdWithoutRetry() {
    const auto root = NewTestRoot("live-view-stop-failure");
    FakeLiveViewTransport live_view(FakeLiveViewFailure::stop_failure);
    bool capture_called = false;
    EvidenceWriter evidence(root / "artifacts", "run-live-view-stop-failure", "hybrid-sdk-fake");
    const auto result = ExecuteLiveViewHandoffOnce(
        live_view, "sdk-private-a", [&] {
            capture_called = true;
            return TransactionResult{};
        }, evidence, "handoff-1", "CAM-A", 1, 0, std::chrono::milliseconds::zero());
    Check(result.terminal_state == "FailedBeforeCapture", "stop failure should end before WPD capture");
    Check(result.error_category == "live_view_stop_failed", "stop failure category should be retained");
    Check(live_view.StopAttempts() == 1 && live_view.CloseAttempts() == 1,
        "failed live view stop should not be retried and should get one close attempt");
    Check(!capture_called, "hybrid capture must not start after live view stop failure");
    fs::remove_all(root);
}

void TestLiveViewCloseFailureBlocksWpdWithoutRetry() {
    const auto root = NewTestRoot("live-view-close-failure");
    FakeLiveViewTransport live_view(FakeLiveViewFailure::close_failure);
    bool capture_called = false;
    EvidenceWriter evidence(root / "artifacts", "run-live-view-close-failure", "hybrid-sdk-fake");
    const auto result = ExecuteLiveViewHandoffOnce(
        live_view, "sdk-private-a", [&] {
            capture_called = true;
            return TransactionResult{};
        }, evidence, "handoff-1", "CAM-A", 1, 0, std::chrono::milliseconds::zero());
    Check(result.terminal_state == "FailedBeforeCapture", "close failure should end before WPD capture");
    Check(result.error_category == "close_failed", "close failure category should be retained");
    Check(live_view.StopAttempts() == 1 && live_view.CloseAttempts() == 1,
        "failed live view close should not be retried");
    Check(!capture_called, "hybrid capture must not start after SDK live view close failure");
    fs::remove_all(root);
}

void TestWpdFailureSkipsLiveViewResume() {
    const auto root = NewTestRoot("wpd-failure-skips-resume");
    FakeLiveViewTransport live_view;
    HybridWpdFake wpd;
    HybridSdkFake sdk(false, true);
    EvidenceWriter evidence(root / "artifacts", "run-wpd-failure-skips-resume", sdk.SdkVersion());
    const auto result = ExecuteLiveViewHandoffOnce(
        live_view, "sdk-private-a",
        [&] { return ExecuteHybridCaptureOnce(wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a"); },
        evidence, "handoff-1", "CAM-A", 1, 0, std::chrono::milliseconds::zero());
    Check(result.terminal_state == "CaptureFailedResumeSkipped", "WPD failure should skip resume");
    Check(result.capture.terminal_state == "FailedPartial", "WPD failure should retain its failed transaction");
    Check(result.error_detail == "configured SDK card-capture failure" && result.capture.error_detail == result.error_detail,
        "hybrid transport detail should propagate through the handoff result");
    Check(!result.resume_attempted && live_view.OpenAttempts() == 1,
        "live view must not reopen after WPD failure");
    Check(wpd.opens == 1 && wpd.observes == 0 && sdk.captures == 1 && wpd.delete_attempts == 0,
        "hybrid failure must not auto-retry or delete");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("LiveViewResumeSkippedAfterCaptureFailure") != std::string::npos,
        "evidence should record that resume was skipped");
    fs::remove_all(root);
}

void TestResumeFailureRetainsWpdOriginal() {
    const auto root = NewTestRoot("resume-failure-retains-original");
    FakeLiveViewTransport live_view(FakeLiveViewFailure::second_open_failure);
    HybridWpdFake wpd;
    HybridSdkFake sdk;
    EvidenceWriter evidence(root / "artifacts", "run-resume-failure-retains-original", sdk.SdkVersion());
    const auto result = ExecuteLiveViewHandoffOnce(
        live_view, "sdk-private-a",
        [&] { return ExecuteHybridCaptureOnce(wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a"); },
        evidence, "handoff-1", "CAM-A", 1, 0, std::chrono::milliseconds::zero());
    Check(result.terminal_state == "CaptureCompleteLiveViewResumeFailed",
        "resume failure should be distinct from capture failure");
    Check(result.error_category == "live_view_resume_open_failed", "resume failure category should be retained");
    Check(result.capture.terminal_state == "Complete" && result.wpd_capture_complete,
        "WPD capture should remain complete when only resume fails");
    Check(result.resume_attempted && live_view.OpenAttempts() == 2,
        "resume should be attempted exactly once");
    Check(sdk.captures == 1 && wpd.delete_attempts == 1, "resume failure must not repeat the hybrid capture or cleanup");
    Check(result.capture.frames.size() == 1 && result.capture.frames.front().success &&
              fs::exists(result.capture.frames.front().path),
        "the saved WPD original must survive live view resume failure");
    fs::remove_all(root);
}

void TestIdentityMap() {
    const auto root = NewTestRoot("identity");
    IdentityMap map(root / "camera-map.json");
    std::string private_a{"private-a"};
    private_a.push_back('\x01');
    private_a.push_back(static_cast<char>(0x80));
    private_a += "-\\-\"";
    map.Bind("CAM-A", private_a);
    map.Bind("CAM-B", "private-b");
    IdentityMap reloaded(root / "camera-map.json");
    Check(reloaded.FindAlias(private_a) == "CAM-A",
        "CAM-A should survive reload after byte-safe JSON escaping");
    Check(reloaded.FindAlias("private-b") == "CAM-B", "CAM-B should survive reload");

    IdentityMap explicit_map(root / "explicit-camera-map.json");
    explicit_map.Bind("CAM-B", "explicit-private-b");
    explicit_map.Bind("CAM-B", "explicit-private-b");
    explicit_map.Bind("CAM-A", "explicit-private-a");
    IdentityMap explicit_reloaded(root / "explicit-camera-map.json");
    Check(explicit_reloaded.FindAlias("explicit-private-a") == "CAM-A",
        "explicit CAM-A binding should survive reload");
    Check(explicit_reloaded.FindAlias("explicit-private-b") == "CAM-B",
        "explicit CAM-B binding should survive reload");

    const auto rejected = [&](std::string_view alias, std::string_view identity) {
        try {
            explicit_reloaded.Bind(alias, identity);
            return false;
        } catch (const std::runtime_error&) {
            return true;
        }
    };
    Check(rejected("CAM-A", "replacement-private-a"), "binding must not replace an existing alias");
    Check(rejected("CAM-B", "explicit-private-a"), "one identity must not bind to both aliases");
    Check(rejected("CAM-C", "private-c"), "binding must reject an unsupported alias");
    Check(rejected("CAM-A", ""), "binding must reject an empty identity");

    const auto cameras = FakeCameraTransport().Enumerate();
    const auto unbound_map_path = root / "read-only-inventory-map.json";
    IdentityMap unbound_map(unbound_map_path);
    const auto unbound_inventory = SummarizeInventoryReadOnly(unbound_map, cameras);
    Check(unbound_inventory.cameras.size() == 2 && unbound_inventory.bound_camera_count == 0 &&
              unbound_inventory.unbound_camera_count == 2,
        "read-only inventory should report both unknown cameras as unbound");
    Check(unbound_inventory.cameras[0].alias == "UNBOUND" &&
              unbound_inventory.cameras[1].alias == "UNBOUND",
        "read-only inventory must not infer aliases from enumeration order");
    Check(!fs::exists(unbound_map_path), "read-only inventory must not create an identity map");

    IdentityMap bound_inventory_map(root / "bound-inventory-map.json");
    bound_inventory_map.Bind("CAM-B", cameras[1].stable_identity);
    bound_inventory_map.Bind("CAM-A", cameras[0].stable_identity);
    const auto bound_inventory = SummarizeInventoryReadOnly(bound_inventory_map, cameras);
    Check(bound_inventory.bound_camera_count == 2 && bound_inventory.unbound_camera_count == 0,
        "read-only inventory should recognize both explicit bindings");
    Check(bound_inventory.cameras[0].alias == "CAM-A" && bound_inventory.cameras[1].alias == "CAM-B",
        "read-only inventory should restore explicit aliases independently of registration order");
    auto reversed_cameras = cameras;
    std::reverse(reversed_cameras.begin(), reversed_cameras.end());
    const auto reversed_inventory = SummarizeInventoryReadOnly(bound_inventory_map, reversed_cameras);
    Check(reversed_inventory.bound_camera_count == 2 && reversed_inventory.unbound_camera_count == 0 &&
              reversed_inventory.cameras[0].alias == "CAM-B" &&
              reversed_inventory.cameras[1].alias == "CAM-A",
        "explicit aliases must follow stable identities after the camera enumeration order reverses");

    Check(SelectSingleCameraForBinding({cameras[0]}).stable_identity == cameras[0].stable_identity,
        "one enumerated camera should be selectable for explicit binding");
    const auto selection_rejected = [](const std::vector<CameraInfo>& candidates) {
        try {
            (void)SelectSingleCameraForBinding(candidates);
            return false;
        } catch (const std::runtime_error&) {
            return true;
        }
    };
    Check(selection_rejected({}), "binding must reject zero enumerated cameras");
    Check(selection_rejected(cameras), "binding must reject multiple enumerated cameras");
    fs::remove_all(root);
}

void TestProcessTerminationGateNeverContinues() {
    const auto root = NewTestRoot("process-termination-gate");
    OperatorGate gate(
        root / "artifacts", "pair_boundary_exit", std::chrono::seconds(2),
        "app-exit", "after-CAM-A-before-CAM-B");
    std::ostringstream output;
    std::thread prohibited_marker([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!fs::exists(gate.ReadyPath()) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (fs::exists(gate.ReadyPath())) {
            std::ofstream marker(gate.ReadyPath().parent_path() / "continue", std::ios::out);
            marker << "prohibited\n";
        }
    });
    try {
        gate.AwaitProcessTermination(output);
    } catch (const TransportError& error) {
        Check(error.Category() == "operator_interruption_continue_marker",
            "a process-termination gate must reject a continue marker instead of resuming");
    }
    prohibited_marker.join();
    Check(output.str().find("terminate this Phase 0 process now") != std::string::npos &&
              output.str().find("CAM-B must not start") != std::string::npos,
        "a process-termination gate must publish an explicit non-resume instruction");
    const auto ready = ReadAll(gate.ReadyPath());
    Check(ready.find("\"scenario\": \"app-exit\"") != std::string::npos &&
              ready.find("\"stage\": \"after-CAM-A-before-CAM-B\"") != std::string::npos,
        "the interruption ready artifact must identify the pair boundary without camera identities");
    fs::remove_all(root);
}

void TestNikonSdkStableIdentityUsesDocumentedSourceStrings() {
    const auto first = DeriveNikonSdkStableIdentity("D810-body-a", "usb-interface-a");
    const auto repeated = DeriveNikonSdkStableIdentity("D810-body-a", "usb-interface-a");
    Check(first == repeated && first.size() == 64,
        "documented MAID source strings should derive a deterministic private digest");
    Check(first != DeriveNikonSdkStableIdentity("D810-body-b", "usb-interface-a") &&
              first != DeriveNikonSdkStableIdentity("D810-body-a", "usb-interface-b"),
        "both source Name and Interface must participate in SDK identity v2");
    Check(DeriveNikonSdkStableIdentity("a", "bc") !=
              DeriveNikonSdkStableIdentity("ab", "c"),
        "length framing must prevent source identity concatenation ambiguity");

    const auto rejected = [](std::string_view name, std::string_view interface_name) {
        try {
            (void)DeriveNikonSdkStableIdentity(name, interface_name);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    Check(rejected("", "interface") && rejected("name", ""),
        "missing SDK source identity components must fail closed");
    Check(rejected(std::string(256, 'n'), "interface") &&
              rejected("name", std::string(256, 'i')),
        "oversized SDK source identity components must fail closed");
    Check(rejected(std::string("name\0tail", 9), "interface") &&
              rejected("name", std::string("interface\0tail", 14)),
        "embedded NUL bytes in SDK source identity components must fail closed");
}

void TestWpdStableIdentityUsesCameraSerialNotPnpPath() {
    const auto first = DeriveWpdStableIdentity("camera-body-a");
    Check(first == DeriveWpdStableIdentity("camera-body-a") && first.size() == 64,
        "WPD camera serial should derive a deterministic private digest");
    Check(first != DeriveWpdStableIdentity("camera-body-b"),
        "different WPD camera serials must derive different identities");
    const auto rejected = [](std::string_view serial) {
        try {
            (void)DeriveWpdStableIdentity(serial);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    Check(rejected("") && rejected(std::string(513, 's')) &&
              rejected(std::string("serial\0tail", 11)),
        "missing, oversized, or embedded-NUL WPD serials must fail closed");
}

void TestSingleIdentityV3PersistsOnlyWpdAuthority() {
    const auto root = NewTestRoot("single-identity-v3");
    const auto path = root / "single-identity-v3.json";
    const std::string wpd_digest(64, 'a');
    const std::vector<CameraInfo> sdk{{"Nikon D810", "unknown", "S", "sdk-session-projection"}};
    const std::vector<CameraInfo> wpd{{"Nikon D810", "1.14", "S", wpd_digest}};
    PersistSingleIdentityV3(path, "CAM-A", sdk, wpd);
    const auto body = ReadAll(path);
    Check(body.find("a0.camera-agent.single-identity.v3") != std::string::npos &&
              body.find("exactly-one-current-session") != std::string::npos &&
              body.find(wpd_digest) != std::string::npos &&
              body.find("sdk-session-projection") == std::string::npos,
        "identity-v3 must persist only WPD authority and the exact-one SDK policy");
    bool duplicate_rejected = false;
    try {
        PersistSingleIdentityV3(path, "CAM-A", sdk, wpd);
    } catch (const std::runtime_error&) {
        duplicate_rejected = true;
    }
    Check(duplicate_rejected,
        "identity-v3 must never overwrite an existing binding without explicit invalidation");
    bool extra_camera_rejected = false;
    try {
        PersistSingleIdentityV3(
            root / "extra.json", "CAM-A",
            {sdk.front(), sdk.front()}, wpd);
    } catch (const std::runtime_error&) {
        extra_camera_rejected = true;
    }
    Check(extra_camera_rejected && !fs::exists(root / "extra.json"),
        "identity-v3 must reject any inventory other than exactly one SDK and one WPD D810");
    fs::remove_all(root);
}

void TestCrossTransportBindingPrevalidatesBothMaps() {
    const CameraInfo sdk_camera{"Nikon D810", "unknown", "S", "sdk-body-a"};
    const CameraInfo wpd_camera{"Nikon D810", "1.14", "S", "wpd-body-a"};
    {
        const auto root = NewTestRoot("cross-transport-binding-success");
        IdentityMap sdk_map(root / "sdk.json");
        IdentityMap wpd_map(root / "wpd.json");
        const auto selected = BindCrossTransportIdentity(
            sdk_map, wpd_map, "CAM-A", {sdk_camera}, {wpd_camera});
        Check(selected.sdk_camera.stable_identity == sdk_camera.stable_identity &&
                  selected.wpd_camera.stable_identity == wpd_camera.stable_identity &&
                  sdk_map.FindAlias(sdk_camera.stable_identity) == "CAM-A" &&
                  wpd_map.FindAlias(wpd_camera.stable_identity) == "CAM-A",
            "cross-transport binding should register both one-body projections to one explicit alias");
        fs::remove_all(root);
    }
    {
        const auto root = NewTestRoot("cross-transport-binding-prevalidation");
        IdentityMap sdk_map(root / "sdk.json");
        IdentityMap wpd_map(root / "wpd.json");
        wpd_map.Bind("CAM-A", "different-wpd-body");
        bool rejected = false;
        try {
            (void)BindCrossTransportIdentity(
                sdk_map, wpd_map, "CAM-A", {sdk_camera}, {wpd_camera});
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        Check(rejected && !fs::exists(root / "sdk.json") &&
                  !sdk_map.FindAlias(sdk_camera.stable_identity),
            "a WPD conflict must be detected before the SDK map is changed");
        fs::remove_all(root);
    }
    {
        const auto root = NewTestRoot("cross-transport-binding-count");
        IdentityMap sdk_map(root / "sdk.json");
        IdentityMap wpd_map(root / "wpd.json");
        bool rejected = false;
        try {
            (void)BindCrossTransportIdentity(
                sdk_map, wpd_map, "CAM-A", {}, {wpd_camera});
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        Check(rejected && !fs::exists(root / "sdk.json") && !fs::exists(root / "wpd.json"),
            "cross-transport binding must reject anything other than one SDK and one WPD projection");
        fs::remove_all(root);
    }
}

void TestDualIdentityVerificationRequiresExactAliasCardinality() {
    const auto root = NewTestRoot("dual-identity-verification");
    IdentityMap sdk_map(root / "sdk.json");
    IdentityMap wpd_map(root / "wpd.json");
    sdk_map.Bind("CAM-A", "sdk-a");
    sdk_map.Bind("CAM-B", "sdk-b");
    wpd_map.Bind("CAM-A", "wpd-a");
    wpd_map.Bind("CAM-B", "wpd-b");
    const std::vector<CameraInfo> sdk_cameras{
        {"Nikon D810", "unknown", "S", "sdk-a"},
        {"Nikon D810", "unknown", "S", "sdk-b"}};
    const std::vector<CameraInfo> wpd_cameras{
        {"Nikon D810", "1.14", "S", "wpd-a"},
        {"Nikon D810", "1.11", "S", "wpd-b"}};

    const auto ready = VerifyDualIdentityBindings(
        sdk_map, wpd_map, sdk_cameras, wpd_cameras);
    Check(ready.terminal_state == "Blocked" &&
              ready.failure_category == "identity_strategy_unresolved" &&
              ready.sdk_cam_a_count == 1 && ready.sdk_cam_b_count == 1 &&
              ready.wpd_cam_a_count == 1 && ready.wpd_cam_b_count == 1 &&
              ready.sdk_unbound_count == 0 && ready.wpd_unbound_count == 0 &&
              !ready.identity_maps_changed && !ready.capture_command_sent &&
              !ready.live_view_started && !ready.camera_settings_changed &&
              !ready.card_access_performed && !ready.real_identifiers_included,
        "legacy dual maps may count exact aliases but must not claim same-body readiness");

    const auto count_failure = VerifyDualIdentityBindings(
        sdk_map, wpd_map, {sdk_cameras.front()}, wpd_cameras);
    Check(count_failure.terminal_state == "Blocked" &&
              count_failure.failure_category == "camera_count_mismatch",
        "dual identity verification must reject a transport count other than two");

    auto unbound_wpd = wpd_cameras;
    unbound_wpd.back().stable_identity = "wpd-unbound";
    const auto unbound = VerifyDualIdentityBindings(
        sdk_map, wpd_map, sdk_cameras, unbound_wpd);
    Check(unbound.terminal_state == "Blocked" &&
              unbound.failure_category == "unbound_identity" && unbound.wpd_unbound_count == 1,
        "dual identity verification must reject an unbound body before camera control");

    const auto path = PersistDualIdentityVerificationSummary(
        root / "artifacts", "run-dual-identity", ready);
    const auto body = ReadAll(path);
    Check(body.find("\"terminalState\": \"Blocked\"") != std::string::npos &&
              body.find("\"failureCategory\": \"identity_strategy_unresolved\"") != std::string::npos &&
              body.find("\"sdkCamACount\": 1") != std::string::npos &&
              body.find("\"wpdCamBCount\": 1") != std::string::npos &&
              body.find("sdk-a") == std::string::npos && body.find("wpd-a") == std::string::npos &&
              body.find("serial") == std::string::npos,
        "dual identity evidence must retain only anonymous counts and safety state");
    EvidenceWriter evidence(root / "artifacts", "run-dual-identity", "test");
    evidence.GenerateRedactedReport(root / "reports");
    Check(fs::exists(root / "reports" / "run-dual-identity" /
              "dual-identity-verification-summary.json"),
        "redacted report should include dual identity verification evidence");
    fs::remove_all(root);
}

void TestDualSpoolVerificationRequiresIdentityAndBothEmptyCards() {
    const auto root = NewTestRoot("dual-spool-verification");
    DualIdentityVerificationSummary identity;
    identity.sdk_camera_count = 2;
    identity.sdk_cam_a_count = 1;
    identity.sdk_cam_b_count = 1;
    identity.wpd_camera_count = 2;
    identity.wpd_cam_a_count = 1;
    identity.wpd_cam_b_count = 1;
    identity.terminal_state = "Ready";

    auto ready = PrepareDualSpoolVerification(identity);
    Check(ready.terminal_state == "ReadyForInspection" && ready.failure_category.empty(),
        "dual spool verification should inspect only after exact dual identity readiness");
    FinalizeDualSpoolVerification(ready, 0, 0);
    Check(ready.terminal_state == "Ready" && ready.failure_category.empty() &&
              ready.wpd_sessions_closed == 2 && ready.cam_a_payload_object_count == 0 &&
              ready.cam_b_payload_object_count == 0 && ready.read_only_observation &&
              ready.card_inspection_performed &&
              !ready.capture_command_sent && !ready.camera_delete_attempted &&
              !ready.vendor_operation_executed && !ready.automatic_retry &&
              !ready.real_identifiers_included,
        "both dedicated spools must contain zero payload objects before pair capture");

    auto non_empty = PrepareDualSpoolVerification(identity);
    FinalizeDualSpoolVerification(non_empty, 0, 1);
    Check(non_empty.terminal_state == "Blocked" &&
              non_empty.failure_category == "spool_not_empty" &&
              non_empty.cam_b_payload_object_count == 1,
        "one non-empty camera spool must block the whole dual capture lane without deletion");

    auto identity_blocked = identity;
    identity_blocked.terminal_state = "Blocked";
    identity_blocked.failure_category = "camera_count_mismatch";
    const auto not_inspected = PrepareDualSpoolVerification(identity_blocked);
    Check(not_inspected.terminal_state == "Blocked" &&
              not_inspected.failure_category == "dual_identity_not_ready" &&
              not_inspected.wpd_sessions_closed == 0 && !not_inspected.card_inspection_performed,
        "dual spool verification must perform no card inspection before identity readiness");

    const auto path = PersistDualSpoolVerificationSummary(
        root / "artifacts", "run-dual-spools", non_empty);
    const auto body = ReadAll(path);
    Check(body.find("\"camAPayloadObjectCount\": 0") != std::string::npos &&
              body.find("\"camBPayloadObjectCount\": 1") != std::string::npos &&
              body.find("\"cameraDeleteAttempted\": false") != std::string::npos &&
              body.find("\"cardInspectionPerformed\": true") != std::string::npos &&
              body.find("\"automaticRetry\": false") != std::string::npos &&
              body.find("private-") == std::string::npos,
        "dual spool evidence should be aggregate-only and disclose no private identity");
    EvidenceWriter evidence(root / "artifacts", "run-dual-spools", "test");
    evidence.GenerateRedactedReport(root / "reports");
    Check(fs::exists(root / "reports" / "run-dual-spools" /
              "dual-spool-verification-summary.json"),
        "redacted report should include dual spool verification evidence");
    fs::remove_all(root);
}

void TestSuccessfulPairAndRedaction() {
    const auto root = NewTestRoot("pair");
    FakeCameraTransport transport;
    EvidenceWriter evidence(root / "artifacts", "run-pair", transport.SdkVersion());
    evidence.RecordCamera("CAM-A", "fake-fw-a");
    evidence.RecordCamera("CAM-B", "fake-fw-b");
    CaptureCoordinator coordinator(transport, evidence);
    const auto cameras = transport.Enumerate();
    const auto result = coordinator.CapturePair(cameras[0].stable_identity, cameras[1].stable_identity);
    Check(result.terminal_state == "Complete", "pair should complete");
    Check(result.frames.size() == 2 && result.frames[0].success && result.frames[1].success, "both originals should persist");
    Check(transport.OpenSessions() == 0, "session should be closed");
    Check(transport.OpenAttempts() == 2 && transport.CaptureAttempts() == 2, "CAM-A and CAM-B should each run once without retry");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("private-a") == std::string::npos && events.find("private-b") == std::string::npos, "event log must redact stable identities");
    Check(events.find("CaptureA") < events.find("CaptureB"), "CAM-A must precede CAM-B");
    Check(fs::exists(evidence.RunRoot() / result.transaction_id / "CAM-A" / "original.jpg"), "CAM-A original should exist");
    Check(fs::exists(evidence.RunRoot() / result.transaction_id / "CAM-B" / "original.jpg"), "CAM-B original should exist");
    evidence.GenerateRedactedReport(root / "reports");
    const auto report = ReadAll(root / "reports" / "run-pair" / "report.md");
    const auto redacted_events = ReadAll(root / "reports" / "run-pair" / "transaction-events.jsonl");
    Check(report.find("private-") == std::string::npos, "report must not expose stable identity");
    Check(redacted_events.find(result.transaction_id) != std::string::npos, "redacted events should include transaction ID");
    Check(redacted_events.find("CAM-A") != std::string::npos && redacted_events.find("fake-fw-a") != std::string::npos,
        "redacted events should include alias and firmware");
    Check(redacted_events.find(result.frames[0].sha256) != std::string::npos,
        "redacted events should include original hash");
    Check(redacted_events.find("private-") == std::string::npos, "redacted events must not expose stable identity");
    fs::remove_all(root);
}

void TestAmbiguousCandidateFailsAndQuarantines() {
    const auto root = NewTestRoot("ambiguous");
    FakeCameraTransport transport(FakeFailureMode::ambiguous);
    EvidenceWriter evidence(root / "artifacts", "run-ambiguous", transport.SdkVersion());
    CaptureCoordinator coordinator(transport, evidence);
    const auto camera = transport.Enumerate().front();
    const auto result = coordinator.CaptureSingle("CAM-A", camera.stable_identity);
    Check(result.terminal_state == "FailedPartial", "ambiguity should fail the transaction");
    Check(result.error_category == "ambiguous_candidates", "ambiguity should be categorized");
    Check(transport.CaptureAttempts() == 1, "ambiguity must not auto-retry");
    const auto quarantine = root / "artifacts" / "run-ambiguous" / "quarantine" / result.transaction_id / "CAM-A";
    Check(fs::exists(quarantine), "ambiguous candidates should be quarantined");
    Check(std::distance(fs::directory_iterator(quarantine), fs::directory_iterator{}) == 2, "both ambiguous candidates should be retained");
    fs::remove_all(root);
}

void TestTransportFailureDoesNotRetry() {
    const auto root = NewTestRoot("failure");
    FakeCameraTransport transport(FakeFailureMode::capture_failure);
    EvidenceWriter evidence(root / "artifacts", "run-failure", transport.SdkVersion());
    CaptureCoordinator coordinator(transport, evidence);
    const auto camera = transport.Enumerate().front();
    const auto result = coordinator.CaptureSingle("CAM-A", camera.stable_identity);
    Check(result.terminal_state == "FailedPartial", "transport error should fail the transaction");
    Check(result.error_category == "capture_command_failed", "typed transport category should be retained");
    Check(result.error_detail == "fake capture failure", "transport error detail should be retained");
    Check(transport.OpenAttempts() == 1 && transport.CaptureAttempts() == 1, "transport failure must not retry");
    Check(transport.OpenSessions() == 0, "transport session should close after failure");
    fs::remove_all(root);
}

void TestTransportErrorDetailEvidence() {
    const auto root = NewTestRoot("transport-error-detail");
    const std::string raw_detail = "failed \"quoted\"\n" + std::string(600, 'x') + "\t";
    DetailedFailureTransport transport(raw_detail);
    EvidenceWriter evidence(root / "artifacts", "run-transport-error-detail", transport.SdkVersion());
    CaptureCoordinator coordinator(transport, evidence);
    const auto result = coordinator.CaptureSingle("CAM-A", transport.Enumerate().front().stable_identity);
    Check(result.error_detail.size() == 512, "transport detail should be limited to 512 characters");
    Check(result.error_detail.find('\n') == std::string::npos && result.error_detail.find('\t') == std::string::npos,
        "transport detail should remove control characters");
    Check(result.frames.front().error_detail == result.error_detail,
        "frame transport detail should propagate to its transaction");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("\"errorDetail\":\"failed \\\"quoted\\\"") != std::string::npos,
        "event JSON should escape the controlled transport detail");
    Check(events.find("private-a") == std::string::npos,
        "transport error evidence must not include the stable identity");
    fs::remove_all(root);
}

void TestUncertainDispatchQuarantinesExactlyOneAndStopsPair() {
    const auto root = NewTestRoot("uncertain-one");
    const std::vector<unsigned char> jpeg{0xFF, 0xD8, 0x11, 0x22, 0xFF, 0xD9};
    const std::string detail = "WPD capture command uncertain after SendCommand HRESULT=0x00000000; command HRESULT=0x80004005";
    UncertainDispatchTransport transport("uncertain_dispatch", detail, {{"new.jpg", jpeg, true}});
    EvidenceWriter evidence(root / "artifacts", "run-uncertain-one", transport.SdkVersion());
    CaptureCoordinator coordinator(transport, evidence);
    const auto cameras = transport.Enumerate();
    const auto result = coordinator.CapturePair(cameras[0].stable_identity, cameras[1].stable_identity);
    const auto quarantine = root / "artifacts" / "run-uncertain-one" / "quarantine" / result.transaction_id / "CAM-A";
    Check(result.terminal_state == "FailedPartial", "uncertain dispatch must never complete a pair");
    Check(result.frames.size() == 1, "CAM-B must not start after uncertain CAM-A dispatch");
    Check(transport.OpenAttempts() == 1 && transport.CaptureAttempts() == 1, "uncertain dispatch must not resend the command");
    Check(!result.frames.front().success && result.error_category == "uncertain_dispatch", "uncertain result must remain failed");
    Check(result.error_detail == detail, "uncertain result must retain command diagnostics");
    Check(result.frames.front().bytes == jpeg.size() && result.frames.front().sha256 == Sha256Hex(jpeg),
        "single uncertain candidate must retain byte and SHA evidence");
    Check(fs::exists(quarantine / "0_new_jpg.bin"), "single uncertain candidate should be quarantined");
    Check(!fs::exists(evidence.RunRoot() / result.transaction_id / "CAM-A" / "original.jpg"),
        "uncertain candidate must never become an original");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("UnconfirmedCandidateQuarantined") != std::string::npos &&
        events.find(result.frames.front().sha256) != std::string::npos && events.find("\"bytes\":6") != std::string::npos,
        "quarantine event must include bytes and SHA evidence");
    fs::remove_all(root);
}

void TestUncertainDispatchAmbiguousAndZeroCandidates() {
    const auto root = NewTestRoot("uncertain-ambiguous");
    const std::string detail = "SendCommand HRESULT=0x00000000; command HRESULT=0x80004005";
    UncertainDispatchTransport ambiguous("uncertain_dispatch_ambiguous", detail,
        {{"one.jpg", {0xFF, 0xD8, 0x01, 0xFF, 0xD9}, true}, {"two.jpg", {0xFF, 0xD8, 0x02, 0xFF, 0xD9}, true}});
    EvidenceWriter ambiguous_evidence(root / "artifacts", "run-uncertain-ambiguous", ambiguous.SdkVersion());
    CaptureCoordinator ambiguous_coordinator(ambiguous, ambiguous_evidence);
    const auto ambiguous_result = ambiguous_coordinator.CaptureSingle("CAM-A", ambiguous.Enumerate().front().stable_identity);
    const auto quarantine = root / "artifacts" / "run-uncertain-ambiguous" / "quarantine" / ambiguous_result.transaction_id / "CAM-A";
    Check(ambiguous_result.terminal_state == "FailedPartial" && ambiguous_result.error_category == "uncertain_dispatch_ambiguous",
        "multiple uncertain candidates must be failed as ambiguous");
    Check(ambiguous.CaptureAttempts() == 1 && fs::exists(quarantine), "ambiguous uncertain candidates must be quarantined without retry");
    Check(std::distance(fs::directory_iterator(quarantine), fs::directory_iterator{}) == 2,
        "all ambiguous uncertain candidates must be preserved");

    UncertainDispatchTransport none("uncertain_dispatch_no_object", detail, {});
    EvidenceWriter none_evidence(root / "artifacts", "run-uncertain-none", none.SdkVersion());
    CaptureCoordinator none_coordinator(none, none_evidence);
    const auto none_result = none_coordinator.CaptureSingle("CAM-A", none.Enumerate().front().stable_identity);
    Check(none_result.terminal_state == "FailedPartial" && none_result.error_category == "uncertain_dispatch_no_object",
        "zero uncertain candidates must be a typed no-object failure");
    Check(none.CaptureAttempts() == 1 && none_result.error_detail == detail,
        "zero uncertain candidates must retain command diagnostic without retry");
    fs::remove_all(root);
}

void TestUncertainDispatchObservationFailureRetainsCommandDiagnostic() {
    const auto root = NewTestRoot("uncertain-observation-error");
    const std::string detail = "SendCommand HRESULT=0x00000000; command HRESULT=0x80004005; post-baseline observation failed: download_failed";
    UncertainDispatchTransport transport("uncertain_dispatch_observation_failed", detail, {});
    EvidenceWriter evidence(root / "artifacts", "run-uncertain-observation-error", transport.SdkVersion());
    CaptureCoordinator coordinator(transport, evidence);
    const auto result = coordinator.CaptureSingle("CAM-A", transport.Enumerate().front().stable_identity);
    Check(result.terminal_state == "FailedPartial" && result.error_category == "uncertain_dispatch_observation_failed",
        "uncertain observation failure must remain failed");
    Check(result.error_detail == detail && transport.CaptureAttempts() == 1,
        "uncertain observation failure must retain command diagnostic without retry");
    fs::remove_all(root);
}

void TestLateCandidateFailsAndQuarantines() {
    const auto root = NewTestRoot("late");
    FakeCameraTransport transport(FakeFailureMode::late_candidate);
    EvidenceWriter evidence(root / "artifacts", "run-late", transport.SdkVersion());
    CaptureCoordinator coordinator(transport, evidence);
    const auto camera = transport.Enumerate().front();
    const auto result = coordinator.CaptureSingle("CAM-A", camera.stable_identity);
    Check(result.terminal_state == "FailedPartial", "late candidate should fail the transaction");
    Check(result.error_category == "late_candidate", "late candidate should retain its category");
    Check(transport.CaptureAttempts() == 1, "late candidate must not auto-retry");
    const auto quarantine = root / "artifacts" / "run-late" / "quarantine" / result.transaction_id / "CAM-A";
    Check(fs::exists(quarantine), "late candidate should be quarantined");
    Check(!fs::exists(root / "artifacts" / "run-late" / result.transaction_id / "CAM-A" / "original.jpg"),
        "late candidate must not become an original");
    fs::remove_all(root);
}

void TestPairWatchdogStopsBeforeOpen() {
    const auto root = NewTestRoot("watchdog");
    FakeCameraTransport transport;
    EvidenceWriter evidence(root / "artifacts", "run-watchdog", transport.SdkVersion());
    Timeouts timeouts;
    timeouts.transaction_watchdog = std::chrono::seconds::zero();
    CaptureCoordinator coordinator(transport, evidence, timeouts);
    const auto cameras = transport.Enumerate();
    const auto result = coordinator.CapturePair(cameras[0].stable_identity, cameras[1].stable_identity);
    Check(result.terminal_state == "FailedPartial", "expired pair watchdog should fail the transaction");
    Check(result.error_category == "transaction_watchdog", "watchdog failure should be categorized");
    Check(transport.OpenAttempts() == 0 && transport.CaptureAttempts() == 0,
        "expired watchdog must stop before opening CAM-A");
    fs::remove_all(root);
}

void TestCloseFailureRetainsOriginalAndStopsPair() {
    const auto root = NewTestRoot("close-failure");
    FakeCameraTransport transport(FakeFailureMode::close_failure);
    EvidenceWriter evidence(root / "artifacts", "run-close-failure", transport.SdkVersion());
    CaptureCoordinator coordinator(transport, evidence);
    const auto cameras = transport.Enumerate();
    const auto result = coordinator.CapturePair(cameras[0].stable_identity, cameras[1].stable_identity);
    Check(result.terminal_state == "FailedPartial", "close failure should fail the transaction");
    Check(result.error_category == "close_failed", "close failure category should be retained");
    Check(result.frames.size() == 1, "CAM-B must not start after CAM-A close failure");
    Check(result.frames.front().success, "a persisted original remains an acquired frame");
    Check(fs::exists(result.frames.front().path), "persisted original must survive close failure");
    Check(transport.OpenAttempts() == 1 && transport.CaptureAttempts() == 1, "close failure must not retry");
    Check(transport.OpenSessions() == 0, "fake close failure should still release its session");
    fs::remove_all(root);
}

void TestHybridCaptureOrdersOneCardCaptureAndNoWpdShutter() {
    const auto root = NewTestRoot("hybrid-success");
    HybridWpdFake wpd;
    HybridSdkFake sdk;
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-success", sdk.SdkVersion());
    const auto result = ExecuteHybridCaptureOnce(wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a");
    Check(result.terminal_state == "Complete" && result.frames.size() == 1 && result.frames.front().success,
        "hybrid capture should persist exactly one canonical PC original");
    Check(wpd.opens == 2 && wpd.closes == 2 && wpd.observes == 1 && wpd.capture_commands == 0,
        "WPD must baseline then reopen only to observe, never to shutter");
    Check(sdk.opens == 1 && sdk.closes == 1 && sdk.captures == 1,
        "SDK must open, capture to card, and close exactly once");
    Check(result.spool_empty_before_capture && result.camera_card_delete_attempted &&
              result.camera_card_delete_succeeded && result.spool_empty_after_cleanup &&
              wpd.spool_empty_before_checks == 1 && wpd.delete_attempts == 1 &&
              wpd.delete_successes == 1 && wpd.spool_empty_after_checks == 1,
        "a successful hybrid capture must verify the empty spool, delete exactly the recovered object once, and verify empty again");
    Check(fs::exists(evidence.RunRoot() / result.transaction_id / "CAM-A" / "original.jpg"),
        "hybrid output must use the canonical original path");
    HybridCaptureRunSummary summary;
    summary.requested = 1; summary.attempted = 1; summary.completed = 1; summary.terminal_state = "Complete"; summary.last_state = result.terminal_state;
    summary.exclusive_camera_control_confirmed = true;
    summary.dedicated_spool_scope_confirmed = true;
    summary.exact_object_delete_confirmed = true;
    summary.spool_empty_before_count = 1;
    summary.camera_card_delete_attempted_count = 1;
    summary.camera_card_delete_succeeded_count = 1;
    summary.spool_empty_after_count = 1;
    (void)PersistHybridCaptureSummary(root / "artifacts", evidence.RunId(), "CAM-A", summary);
    evidence.GenerateRedactedReport(root / "reports");
    const auto report_summary = ReadAll(root / "reports" / evidence.RunId() / "hybrid-capture-summary.json");
    Check(report_summary.find("\"pc_original_canonical\": true") != std::string::npos &&
              report_summary.find("\"cameraCardDeleteAttemptedCount\": 1") != std::string::npos &&
              report_summary.find("\"cameraCardDeleteSucceededCount\": 1") != std::string::npos &&
              report_summary.find("\"spoolEmptyAfterCount\": 1") != std::string::npos &&
              report_summary.find("\"automatic_retry\": false") != std::string::npos &&
              report_summary.find("\"exclusive_camera_control_confirmed\": true") != std::string::npos &&
              report_summary.find("\"dedicatedSpoolScopeConfirmed\": true") != std::string::npos &&
              report_summary.find("\"exactObjectDeleteConfirmed\": true") != std::string::npos &&
              report_summary.find("\"cleanupObjectIdIncluded\": false") != std::string::npos,
        "hybrid redacted summary must record the approved exact-object cleanup safeguards without object identifiers");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("cleanup-capability") == std::string::npos &&
              report_summary.find("cleanup-capability") == std::string::npos &&
              events.find("private-object-id") == std::string::npos &&
              report_summary.find("private-object-id") == std::string::npos,
        "cleanup capabilities and object IDs must not cross evidence or report boundaries");
    fs::remove_all(root);
}

void TestHybridPairRunsCamAThenCamBWithoutOverlapOrRetry() {
    const auto root = NewTestRoot("hybrid-pair-success");
    HybridWpdFake wpd;
    HybridSdkFake sdk;
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-success", sdk.SdkVersion());
    const auto pair = ExecuteHybridCapturePair(
        wpd, wpd, sdk, sdk, evidence, "wpd-a", "sdk-a", "wpd-b", "sdk-b");

    Check(pair.terminal_state == "Complete" && pair.cam_a.terminal_state == "Complete" &&
              pair.cam_b_started && pair.cam_b.terminal_state == "Complete",
        "hybrid pair should complete CAM-A before CAM-B");
    Check(pair.cam_a.transaction_id != pair.cam_b.transaction_id,
        "hybrid pair camera transactions must have unique durable IDs");
    Check(wpd.opens == 4 && wpd.closes == 4 && wpd.observes == 2 &&
              wpd.capture_commands == 0 && wpd.delete_attempts == 2 && wpd.delete_successes == 2,
        "hybrid pair should run two sequential WPD baseline/recovery lifecycles without WPD shutter");
    Check(sdk.opens == 2 && sdk.closes == 2 && sdk.captures == 2 && !sdk.open && !wpd.open,
        "hybrid pair should run exactly two closed SDK captures without overlap or retry");
    Check(fs::exists(evidence.RunRoot() / pair.cam_a.transaction_id / "CAM-A" / "original.jpg") &&
              fs::exists(evidence.RunRoot() / pair.cam_b.transaction_id / "CAM-B" / "original.jpg"),
        "hybrid pair should preserve one canonical PC original for each camera");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    const auto cam_a_complete = events.find("HybridPairCamAComplete");
    const auto cam_b_start = events.find("HybridPairCamBStarting");
    Check(cam_a_complete != std::string::npos && cam_b_start != std::string::npos &&
              cam_a_complete < cam_b_start,
        "hybrid pair evidence must order CAM-A completion before CAM-B start");

    HybridPairRunSummary summary;
    summary.requested_pairs = 1;
    summary.attempted_pairs = 1;
    summary.completed_pairs = 1;
    summary.cam_a_completed_count = 1;
    summary.cam_b_completed_count = 1;
    summary.attempted_camera_transactions = 2;
    summary.completed_camera_transactions = 2;
    summary.spool_empty_before_count = 2;
    summary.camera_card_delete_attempted_count = 2;
    summary.camera_card_delete_succeeded_count = 2;
    summary.spool_empty_after_count = 2;
    summary.duration_sample_count = 1;
    summary.pair_duration_p50_ms = 1250;
    summary.pair_duration_p95_ms = 1250;
    summary.pair_duration_max_ms = 1250;
    summary.terminal_state = "Complete";
    summary.last_pair_state = pair.terminal_state;
    summary.exclusive_camera_control_confirmed = true;
    summary.dedicated_spool_scope_confirmed = true;
    summary.dual_dedicated_spools_confirmed = true;
    summary.exact_object_delete_confirmed = true;
    const auto summary_path = PersistHybridPairSummary(root / "artifacts", evidence.RunId(), summary);
    evidence.GenerateRedactedReport(root / "reports");
    const auto body = ReadAll(summary_path);
    Check(body.find("\"schemaVersion\": \"phase0.hybrid-pair-summary.v2\"") != std::string::npos &&
              body.find("\"captureOrder\": \"CAM-A-then-CAM-B\"") != std::string::npos &&
              body.find("\"pairWatchdogSeconds\": 180") != std::string::npos &&
              body.find("\"durationSampleCount\": 1") != std::string::npos &&
              body.find("\"pairDurationP50Ms\": 1250") != std::string::npos &&
              body.find("\"pairDurationP95Ms\": 1250") != std::string::npos &&
              body.find("\"pairDurationMaxMs\": 1250") != std::string::npos &&
              body.find("\"timingUsedForPhase0PassFail\": false") != std::string::npos &&
              body.find("\"automaticRetry\": false") != std::string::npos &&
              body.find("\"dualDedicatedSpoolsConfirmed\": true") != std::string::npos &&
              body.find("\"cameraSessionOverlapAllowed\": false") != std::string::npos &&
              body.find("\"actualShutterSynchronizationGuaranteed\": false") != std::string::npos &&
              body.find("\"realIdentifiersIncluded\": false") != std::string::npos,
        "hybrid pair summary must publish ordering, watchdog, exclusivity, retry, sync, and redaction contracts");
    Check(fs::exists(root / "reports" / evidence.RunId() / "hybrid-pair-summary.json"),
        "hybrid pair summary should be included in the redacted report");
    Check(body.find("wpd-a") == std::string::npos && body.find("sdk-a") == std::string::npos &&
              body.find("wpd-b") == std::string::npos && body.find("sdk-b") == std::string::npos,
        "hybrid pair summary must exclude transport identities");
    const auto recovery = AssessHybridPairRecoveryEventLog(evidence.RunRoot() / "events.jsonl");
    Check(recovery.pair_started_count == 1 && recovery.pair_complete_count == 1 &&
              recovery.pair_failed_count == 0 && !recovery.interrupted_pair_detected &&
              !recovery.recovery_requires_new_transaction && recovery.event_sequence_consistent &&
              recovery.terminal_state == "Terminal",
        "a terminal successful pair should require no recovery transaction");
    fs::remove_all(root);
}

void TestHybridPairRecoveryDetectsInterruptionAfterCamA() {
    const auto root = NewTestRoot("hybrid-pair-recovery-after-a");
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-recovery-after-a", "fake");
    evidence.RecordState("hybrid-pair-interrupted", "HybridPairStarted", "CAM-A");
    evidence.RecordState("tx-cam-a", "HybridPcOriginalVerified", "CAM-A");
    evidence.RecordState("tx-cam-a", "HybridSpoolEmptyAfter", "CAM-A");
    evidence.RecordState("hybrid-pair-interrupted", "HybridPairCamAComplete", "CAM-A");

    const auto recovery = AssessHybridPairRecoveryEventLog(evidence.RunRoot() / "events.jsonl");
    Check(recovery.pair_started_count == 1 && recovery.pair_complete_count == 0 &&
              recovery.pair_failed_count == 0 && recovery.interrupted_pair_detected &&
              recovery.interrupted_stage == "after-CAM-A-before-CAM-B" &&
              recovery.cam_a_complete_before_interruption && recovery.retain_completed_originals &&
              !recovery.automatic_retry_allowed && recovery.recovery_requires_new_transaction &&
              recovery.event_sequence_consistent && recovery.terminal_state == "Interrupted",
        "recovery analysis should diagnose an app stop after CAM-A and require a new transaction without retry");

    const auto path = PersistHybridPairRecoverySummary(root / "artifacts", evidence.RunId(), recovery);
    evidence.GenerateRedactedReport(root / "reports");
    const auto body = ReadAll(path);
    Check(body.find("\"interruptedPairDetected\": true") != std::string::npos &&
              body.find("\"interruptedStage\": \"after-CAM-A-before-CAM-B\"") != std::string::npos &&
              body.find("\"camACompleteBeforeInterruption\": true") != std::string::npos &&
              body.find("\"retainCompletedOriginals\": true") != std::string::npos &&
              body.find("\"automaticRetryAllowed\": false") != std::string::npos &&
              body.find("\"recoveryRequiresNewTransaction\": true") != std::string::npos &&
              body.find("\"realIdentifiersIncluded\": false") != std::string::npos,
        "recovery summary should be anonymous and state retention, no-retry, and new-transaction requirements");
    Check(fs::exists(root / "reports" / evidence.RunId() / "hybrid-pair-recovery-summary.json"),
        "redacted reports should include the hybrid pair recovery summary");
    fs::remove_all(root);
}

void TestHybridPairRecoveryClassifiesActiveStagesAndInvalidEvidence() {
    {
        const auto root = NewTestRoot("hybrid-pair-recovery-cam-a-active");
        EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-recovery-cam-a-active", "fake");
        evidence.RecordState("hybrid-pair-active-a", "HybridPairStarted", "CAM-A");
        const auto recovery = AssessHybridPairRecoveryEventLog(evidence.RunRoot() / "events.jsonl");
        Check(recovery.interrupted_pair_detected && recovery.interrupted_stage == "CAM-A-active" &&
                  !recovery.cam_a_complete_before_interruption && recovery.recovery_requires_new_transaction &&
                  recovery.event_sequence_consistent && recovery.terminal_state == "Interrupted",
            "recovery analysis should distinguish interruption during CAM-A from a completed CAM-A");
        fs::remove_all(root);
    }
    {
        const auto root = NewTestRoot("hybrid-pair-recovery-cam-b-active");
        EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-recovery-cam-b-active", "fake");
        evidence.RecordState("hybrid-pair-active-b", "HybridPairStarted", "CAM-A");
        evidence.RecordState("hybrid-pair-active-b", "HybridPairCamAComplete", "CAM-A");
        evidence.RecordState("hybrid-pair-active-b", "HybridPairCamBStarting", "CAM-B");
        const auto recovery = AssessHybridPairRecoveryEventLog(evidence.RunRoot() / "events.jsonl");
        Check(recovery.interrupted_pair_detected && recovery.interrupted_stage == "CAM-B-active" &&
                  recovery.cam_a_complete_before_interruption && recovery.recovery_requires_new_transaction &&
                  recovery.event_sequence_consistent && recovery.terminal_state == "Interrupted",
            "recovery analysis should retain CAM-A completion when interruption occurs during CAM-B");
        fs::remove_all(root);
    }
    {
        const auto root = NewTestRoot("hybrid-pair-recovery-failed-terminal");
        EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-recovery-failed-terminal", "fake");
        evidence.RecordState("hybrid-pair-failed", "HybridPairStarted", "CAM-A");
        evidence.RecordState("hybrid-pair-failed", "HybridPairFailed", "CAM-A");
        const auto recovery = AssessHybridPairRecoveryEventLog(evidence.RunRoot() / "events.jsonl");
        Check(!recovery.interrupted_pair_detected && recovery.pair_failed_count == 1 &&
                  recovery.recovery_requires_new_transaction && recovery.event_sequence_consistent &&
                  recovery.terminal_state == "Terminal",
            "a terminal failed pair should still require a distinct new transaction");
        fs::remove_all(root);
    }
    {
        const auto root = NewTestRoot("hybrid-pair-recovery-invalid-sequence");
        EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-recovery-invalid-sequence", "fake");
        evidence.RecordState("hybrid-pair-first", "HybridPairStarted", "CAM-A");
        evidence.RecordState("hybrid-pair-second", "HybridPairStarted", "CAM-A");
        const auto recovery = AssessHybridPairRecoveryEventLog(evidence.RunRoot() / "events.jsonl");
        Check(!recovery.event_sequence_consistent && recovery.recovery_requires_new_transaction &&
                  recovery.terminal_state == "EvidenceInvalid",
            "overlapping pair starts must fail closed as invalid recovery evidence");
        fs::remove_all(root);
    }
}

void TestHybridPairStopsBeforeCamBAfterCamAFailure() {
    const auto root = NewTestRoot("hybrid-pair-a-failure");
    HybridWpdFake wpd;
    wpd.spool_empty_before = false;
    HybridSdkFake sdk;
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-a-failure", sdk.SdkVersion());
    const auto pair = ExecuteHybridCapturePair(
        wpd, wpd, sdk, sdk, evidence, "wpd-a", "sdk-a", "wpd-b", "sdk-b");

    Check(pair.terminal_state == "FailedPartial" && pair.cam_a.terminal_state == "FailedPartial" &&
              !pair.cam_b_started && pair.cam_b.transaction_id.empty(),
        "a CAM-A failure must stop the hybrid pair before CAM-B");
    Check(wpd.opens == 1 && wpd.closes == 1 && wpd.observes == 0 &&
              wpd.delete_attempts == 0 && sdk.opens == 0 && sdk.captures == 0,
        "a CAM-A empty-spool failure must not start SDK capture, cleanup, CAM-B, or retry");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("HybridPairCamBStarting") == std::string::npos,
        "CAM-B start evidence must be absent after CAM-A failure");
    fs::remove_all(root);
}

void TestHybridPairRetainsCamAWhenCamBFailsWithoutRetry() {
    const auto root = NewTestRoot("hybrid-pair-b-failure");
    HybridWpdFake wpd;
    HybridSdkFake sdk;
    sdk.fail_capture_number = 2;
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-b-failure", sdk.SdkVersion());
    const auto pair = ExecuteHybridCapturePair(
        wpd, wpd, sdk, sdk, evidence, "wpd-a", "sdk-a", "wpd-b", "sdk-b");

    Check(pair.terminal_state == "FailedPartial" && pair.cam_a.terminal_state == "Complete" &&
              pair.cam_b_started && pair.cam_b.terminal_state == "FailedPartial" &&
              pair.error_category == "image_event_timeout",
        "a CAM-B SDK failure should leave the pair FailedPartial after a complete CAM-A");
    Check(fs::exists(evidence.RunRoot() / pair.cam_a.transaction_id / "CAM-A" / "original.jpg"),
        "a CAM-B failure must retain CAM-A's verified canonical PC original");
    Check(sdk.captures == 2 && sdk.opens == 2 && sdk.closes == 2 &&
              wpd.delete_attempts == 1 && wpd.delete_successes == 1,
        "a CAM-B failure must not retry either shutter or delete beyond CAM-A's completed cleanup");
    Check(wpd.spool_empty_before_checks == 2 && wpd.observes == 1 && wpd.abandons == 1,
        "a CAM-B capture failure should abandon only its observation and never begin B recovery");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("HybridPairCamAComplete") != std::string::npos &&
              events.find("HybridPairCamBStarting") != std::string::npos &&
              events.find("HybridPairComplete") == std::string::npos,
        "CAM-B failure evidence must preserve A completion without a false pair success state");
    fs::remove_all(root);
}

void TestHybridPairCamASdkCloseFailureStopsBeforeCamB() {
    const auto root = NewTestRoot("hybrid-pair-a-sdk-close-failure");
    HybridWpdFake wpd;
    HybridSdkFake sdk;
    sdk.fail_close_number = 1;
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-a-sdk-close-failure", sdk.SdkVersion());
    const auto pair = ExecuteHybridCapturePair(
        wpd, wpd, sdk, sdk, evidence, "wpd-a", "sdk-a", "wpd-b", "sdk-b");

    Check(pair.terminal_state == "FailedPartial" && pair.cam_a.terminal_state == "FailedPartial" &&
              pair.error_category == "close_failed" && !pair.cam_b_started && pair.cam_b.transaction_id.empty(),
        "a CAM-A SDK close failure must stop the pair before every CAM-B operation");
    Check(sdk.opens == 1 && sdk.captures == 1 && sdk.closes == 1 && !sdk.open &&
              wpd.opens == 1 && wpd.closes == 1 && wpd.observes == 0 && wpd.delete_attempts == 0,
        "a CAM-A SDK close failure must not open WPD recovery, CAM-B, cleanup, or retry");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("HybridPairCamBStarting") == std::string::npos &&
              events.find("HybridWpdObserveOpen") == std::string::npos,
        "CAM-A SDK close failure evidence must contain no WPD recovery or CAM-B start state");
    fs::remove_all(root);
}

void TestHybridPairCamBWpdRecoveryFailureRetainsCamAWithoutRetry() {
    const auto root = NewTestRoot("hybrid-pair-b-wpd-recovery-failure");
    HybridWpdFake wpd;
    // A baseline/recovery are opens 1/2; B baseline/recovery are opens 3/4.
    wpd.fail_open_number = 4;
    HybridSdkFake sdk;
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-b-wpd-recovery-failure", sdk.SdkVersion());
    const auto pair = ExecuteHybridCapturePair(
        wpd, wpd, sdk, sdk, evidence, "wpd-a", "sdk-a", "wpd-b", "sdk-b");

    Check(pair.terminal_state == "FailedPartial" && pair.cam_a.terminal_state == "Complete" &&
              pair.cam_b_started && pair.cam_b.terminal_state == "FailedPartial" &&
              pair.error_category == "open_failed",
        "a CAM-B WPD recovery failure should fail the pair after retaining completed CAM-A");
    Check(fs::exists(evidence.RunRoot() / pair.cam_a.transaction_id / "CAM-A" / "original.jpg") &&
              !fs::exists(evidence.RunRoot() / pair.cam_b.transaction_id / "CAM-B" / "original.jpg"),
        "CAM-B recovery failure must retain CAM-A's canonical original without inventing a CAM-B original");
    Check(sdk.opens == 2 && sdk.captures == 2 && sdk.closes == 2 &&
              wpd.opens == 4 && wpd.closes == 3 && wpd.observes == 1 &&
              wpd.delete_attempts == 1 && wpd.delete_successes == 1,
        "CAM-B WPD recovery failure must perform no retry or cleanup beyond completed CAM-A");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("HybridPairCamAComplete") != std::string::npos &&
              events.find("HybridPairCamBStarting") != std::string::npos &&
              events.find("HybridPairComplete") == std::string::npos,
        "CAM-B recovery failure evidence must retain A completion without false pair completion");
    fs::remove_all(root);
}

void TestHybridPairFaultHooksRunAfterEachSdkCloseWithoutRetry() {
    {
        const auto root = NewTestRoot("hybrid-pair-cam-a-fault-hook");
        HybridWpdFake wpd;
        HybridSdkFake sdk;
        EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-cam-a-fault-hook", sdk.SdkVersion());
        bool cam_a_gate_called = false;
        const auto pair = ExecuteHybridCapturePair(
            wpd, wpd, sdk, sdk, evidence, "wpd-a", "sdk-a", "wpd-b", "sdk-b", {}, {}, [&] {
                cam_a_gate_called = true;
                Check(!sdk.open && sdk.captures == 1 && sdk.closes == 1 && !wpd.open,
                    "CAM-A fault hook must run after its SDK close and before WPD recovery");
                throw TransportError("injected_disconnect", "synthetic CAM-A disconnect");
            });
        Check(cam_a_gate_called && pair.terminal_state == "FailedPartial" &&
                  pair.error_category == "injected_disconnect" && !pair.cam_b_started &&
                  sdk.captures == 1 && wpd.observes == 0 && wpd.delete_attempts == 0,
            "a CAM-A active fault must stop before recovery, cleanup, CAM-B, or retry");
        const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
        Check(events.find("HybridOperatorGateBeforeWpdRecovery") != std::string::npos &&
                  events.find("HybridPairCamBStarting") == std::string::npos,
            "CAM-A fault evidence must contain the gate but no CAM-B start");
        fs::remove_all(root);
    }
    {
        const auto root = NewTestRoot("hybrid-pair-cam-b-fault-hook");
        HybridWpdFake wpd;
        HybridSdkFake sdk;
        EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-cam-b-fault-hook", sdk.SdkVersion());
        bool cam_b_gate_called = false;
        const auto pair = ExecuteHybridCapturePair(
            wpd, wpd, sdk, sdk, evidence, "wpd-a", "sdk-a", "wpd-b", "sdk-b", {}, {}, {}, [&] {
                cam_b_gate_called = true;
                Check(!sdk.open && sdk.captures == 2 && sdk.closes == 2 && !wpd.open,
                    "CAM-B fault hook must run after its SDK close and before WPD recovery");
                throw TransportError("injected_power_off", "synthetic CAM-B power off");
            });
        Check(cam_b_gate_called && pair.terminal_state == "FailedPartial" &&
                  pair.cam_a.terminal_state == "Complete" && pair.cam_b_started &&
                  pair.error_category == "injected_power_off" &&
                  wpd.observes == 1 && wpd.delete_attempts == 1 && sdk.captures == 2,
            "a CAM-B active fault must retain completed CAM-A without B cleanup or retry");
        Check(fs::exists(evidence.RunRoot() / pair.cam_a.transaction_id / "CAM-A" / "original.jpg") &&
                  !fs::exists(evidence.RunRoot() / pair.cam_b.transaction_id / "CAM-B" / "original.jpg"),
            "a CAM-B active fault must retain only CAM-A's verified canonical original");
        HybridPairFaultRunSummary summary;
        summary.scenario = "power-off";
        summary.fault_camera_alias = "CAM-B";
        summary.pair_state = pair.terminal_state;
        summary.error_category = pair.error_category;
        summary.acceptance_state = "Pass";
        summary.cam_b_started = pair.cam_b_started;
        summary.cam_a_original_persisted = true;
        const auto path = PersistHybridPairFaultSummary(
            root / "artifacts", evidence.RunId(), summary);
        const auto body = ReadAll(path);
        Check(body.find("\"schemaVersion\": \"phase0.hybrid-pair-fault-summary.v1\"") != std::string::npos &&
                  body.find("\"faultCameraAlias\": \"CAM-B\"") != std::string::npos &&
                  body.find("\"camAOriginalPersisted\": true") != std::string::npos &&
                  body.find("\"camBOriginalPersisted\": false") != std::string::npos &&
                  body.find("\"automaticRetry\": false") != std::string::npos &&
                  body.find("\"actualShutterSynchronizationGuaranteed\": false") != std::string::npos &&
                  body.find("sdk-a") == std::string::npos && body.find("wpd-b") == std::string::npos,
            "pair fault evidence must record retention and no-retry policy without transport identities");
        evidence.GenerateRedactedReport(root / "reports");
        Check(fs::exists(root / "reports" / evidence.RunId() / "hybrid-pair-fault-summary.json"),
            "redacted reports should include pair fault evidence");
        fs::remove_all(root);
    }
}

void TestHybridPairSharedWatchdogStopsBeforeCamB() {
    const auto root = NewTestRoot("hybrid-pair-shared-watchdog");
    HybridWpdFake wpd;
    HybridSdkFake sdk;
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-shared-watchdog", sdk.SdkVersion());
    Timeouts timeouts;
    timeouts.transaction_watchdog = std::chrono::seconds(2);
    const auto pair = ExecuteHybridCapturePair(
        wpd, wpd, sdk, sdk, evidence, "wpd-a", "sdk-a", "wpd-b", "sdk-b", timeouts, [] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        });

    Check(pair.cam_a.terminal_state == "Complete" && pair.terminal_state == "FailedPartial" &&
              pair.error_category == "transaction_watchdog" && !pair.cam_b_started,
        "the shared pair watchdog should retain completed CAM-A and stop before CAM-B");
    Check(sdk.captures == 1 && wpd.delete_successes == 1 && wpd.spool_empty_after_checks == 1,
        "the shared watchdog should not undo CAM-A's verified original and exact cleanup");
    Check(sdk.opens == 1 && wpd.opens == 2,
        "the shared watchdog must prevent every CAM-B transport open");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("HybridPairCamBStarting") == std::string::npos,
        "an expired shared watchdog must emit no CAM-B start state");
    fs::remove_all(root);
}

void TestHybridPairBoundaryCallbackFailureNeverStartsCamB() {
    const auto root = NewTestRoot("hybrid-pair-boundary-failure");
    HybridWpdFake wpd;
    HybridSdkFake sdk;
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-pair-boundary-failure", sdk.SdkVersion());
    const auto pair = ExecuteHybridCapturePair(
        wpd, wpd, sdk, sdk, evidence, "wpd-a", "sdk-a", "wpd-b", "sdk-b", {}, [] {
            throw TransportError("operator_interruption_continue_marker", "synthetic prohibited resume");
        });
    Check(pair.terminal_state == "FailedPartial" && pair.cam_a.terminal_state == "Complete" &&
              pair.error_category == "pair_boundary_exception" && !pair.cam_b_started,
        "a pair-boundary gate failure must retain CAM-A and stop before every CAM-B operation");
    Check(sdk.captures == 1 && wpd.delete_successes == 1 &&
              fs::exists(evidence.RunRoot() / pair.cam_a.transaction_id / "CAM-A" / "original.jpg"),
        "a pair-boundary failure must retain CAM-A's verified original and exact cleanup without retry");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("HybridPairCamAComplete") != std::string::npos &&
              events.find("HybridPairCamBStarting") == std::string::npos,
        "pair-boundary failure evidence must contain A completion and no B start");
    fs::remove_all(root);
}

HybridPairResult SyntheticHybridPairResult(
    int pair_number,
    bool cam_b_complete = true) {
    HybridPairResult pair;
    pair.pair_id = "pair-" + std::to_string(pair_number);
    pair.duration = std::chrono::milliseconds(pair_number);
    pair.cam_b_started = true;
    pair.cam_a.transaction_id = pair.pair_id + "-cam-a";
    pair.cam_a.terminal_state = "Complete";
    pair.cam_a.spool_empty_before_capture = true;
    pair.cam_a.camera_card_delete_attempted = true;
    pair.cam_a.camera_card_delete_succeeded = true;
    pair.cam_a.spool_empty_after_cleanup = true;
    pair.cam_b.transaction_id = pair.pair_id + "-cam-b";
    pair.cam_b.terminal_state = cam_b_complete ? "Complete" : "FailedPartial";
    pair.cam_b.spool_empty_before_capture = true;
    if (cam_b_complete) {
        pair.cam_b.camera_card_delete_attempted = true;
        pair.cam_b.camera_card_delete_succeeded = true;
        pair.cam_b.spool_empty_after_cleanup = true;
        pair.terminal_state = "Complete";
    } else {
        pair.terminal_state = "FailedPartial";
        pair.error_category = "image_event_timeout";
        pair.error_detail = "synthetic CAM-B failure";
    }
    return pair;
}

void TestHybridPairRunAggregatesOneHundredSuccessfulPairs() {
    int callbacks = 0;
    const auto summary = ExecuteHybridPairRun(100, [&] {
        return SyntheticHybridPairResult(++callbacks);
    });

    Check(callbacks == 100 && summary.requested_pairs == 100 &&
              summary.attempted_pairs == 100 && summary.completed_pairs == 100 &&
              summary.failures == 0 && summary.terminal_state == "Complete",
        "hybrid pair run should execute and aggregate exactly 100 successful pairs");
    Check(summary.attempted_camera_transactions == 200 &&
              summary.completed_camera_transactions == 200 &&
              summary.cam_a_completed_count == 100 && summary.cam_b_completed_count == 100,
        "100 successful pairs should aggregate 200 completed sequential camera transactions");
    Check(summary.spool_empty_before_count == 200 &&
              summary.camera_card_delete_attempted_count == 200 &&
              summary.camera_card_delete_succeeded_count == 200 &&
              summary.spool_empty_after_count == 200,
        "100 successful pairs should aggregate every exact-object spool safeguard");
    Check(summary.duration_sample_count == 100 && summary.pair_duration_p50_ms == 50 &&
              summary.pair_duration_p95_ms == 95 && summary.pair_duration_max_ms == 100,
        "100 successful pairs should publish deterministic nearest-rank p50, p95, and max timing statistics");
}

void TestHybridPairRunStopsAtFirstFailureAndRetainsCounts() {
    int callbacks = 0;
    const auto summary = ExecuteHybridPairRun(100, [&] {
        const int pair_number = ++callbacks;
        return SyntheticHybridPairResult(pair_number, pair_number != 6);
    });

    Check(callbacks == 6 && summary.requested_pairs == 100 &&
              summary.attempted_pairs == 6 && summary.completed_pairs == 5 &&
              summary.failures == 1 && summary.terminal_state == "FailedPartial",
        "hybrid pair run must stop immediately at the first failed pair without retry");
    Check(summary.attempted_camera_transactions == 12 &&
              summary.completed_camera_transactions == 11 &&
              summary.cam_a_completed_count == 6 && summary.cam_b_completed_count == 5,
        "a sixth-pair CAM-B failure should retain six CAM-A and five CAM-B completions");
    Check(summary.last_pair_state == "FailedPartial" &&
              summary.last_error_category == "image_event_timeout" &&
              summary.last_error_detail == "synthetic CAM-B failure",
        "hybrid pair run must retain the first failure category and detail");
    Check(summary.duration_sample_count == 6 && summary.pair_duration_p50_ms == 3 &&
              summary.pair_duration_p95_ms == 6 && summary.pair_duration_max_ms == 6,
        "a failed run should publish timing statistics for attempted pairs only");
}

void TestHybridFaultGateRunsAfterSdkCloseAndFailsWithoutDeleteOrRetry() {
    const auto root = NewTestRoot("hybrid-fault-gate");
    HybridWpdFake wpd;
    wpd.fail_second_open = true;
    HybridSdkFake sdk;
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-fault-gate", sdk.SdkVersion());
    bool gate_called = false;
    const auto result = ExecuteHybridCaptureOnce(
        wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a", {}, [&] {
            gate_called = true;
            Check(!sdk.open && sdk.captures == 1 && sdk.closes == 1 && !wpd.open,
                "fault gate must run only after SDK capture/close and before WPD recovery open");
        });
    Check(gate_called && result.terminal_state == "FailedPartial" && result.error_category == "open_failed" &&
              result.spool_empty_before_capture && !result.camera_card_delete_attempted,
        "WPD reopen failure after the fault gate must terminate the transaction without cleanup");
    Check(wpd.opens == 2 && wpd.closes == 1 && wpd.observes == 0 && wpd.delete_attempts == 0 &&
              wpd.abandons == 1 && sdk.opens == 1 && sdk.captures == 1 && sdk.closes == 1,
        "hybrid fault path must make one capture attempt, no retry, and no delete");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("HybridOperatorGateBeforeWpdRecovery") != std::string::npos &&
              events.find("HybridOperatorGateContinued") != std::string::npos,
        "fault evidence must record both sides of the operator gate");

    HybridFaultRunSummary summary;
    summary.scenario = "usb-disconnect";
    summary.transaction_state = result.terminal_state;
    summary.error_category = result.error_category;
    summary.acceptance_state = "Pass";
    summary.spool_empty_before_capture = result.spool_empty_before_capture;
    const auto summary_path = PersistHybridFaultSummary(
        root / "artifacts", evidence.RunId(), "CAM-A", summary);
    const auto body = ReadAll(summary_path);
    Check(body.find("\"scenario\": \"usb-disconnect\"") != std::string::npos &&
              body.find("\"acceptanceState\": \"Pass\"") != std::string::npos &&
              body.find("\"cameraObjectDeleteAttempted\": false") != std::string::npos &&
              body.find("\"automaticRetry\": false") != std::string::npos &&
              body.find("\"recoveryRequiresNewTransaction\": true") != std::string::npos,
        "hybrid fault summary must record the expected fail-closed contract anonymously");
    evidence.GenerateRedactedReport(root / "reports");
    Check(fs::exists(root / "reports" / evidence.RunId() / "hybrid-fault-summary.json"),
        "hybrid fault summary must be included in the redacted report");
    fs::remove_all(root);
}

void TestHybridWatchdogStopsBeforeHardwareOpen() {
    const auto root = NewTestRoot("hybrid-watchdog");
    HybridWpdFake wpd;
    HybridSdkFake sdk;
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-watchdog", sdk.SdkVersion());
    Timeouts timeouts;
    timeouts.transaction_watchdog = std::chrono::seconds::zero();

    const auto result = ExecuteHybridCaptureOnce(
        wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a", timeouts);

    Check(result.terminal_state == "FailedPartial" && result.error_category == "transaction_watchdog",
        "expired hybrid watchdog must fail the transaction");
    Check(wpd.opens == 0 && wpd.capture_commands == 0 && wpd.delete_attempts == 0 &&
              sdk.opens == 0 && sdk.captures == 0,
        "expired hybrid watchdog must stop before opening either transport");
    fs::remove_all(root);
}

void TestHybridWatchdogClosesSdkAndStopsBeforeWpdRecovery() {
    const auto root = NewTestRoot("hybrid-watchdog-overrun");
    HybridWpdFake wpd;
    HybridSdkFake sdk(false, false, std::chrono::milliseconds(2100));
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-watchdog-overrun", sdk.SdkVersion());
    Timeouts timeouts;
    timeouts.transaction_watchdog = std::chrono::seconds(2);

    const auto result = ExecuteHybridCaptureOnce(
        wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a", timeouts);

    Check(result.terminal_state == "FailedPartial" && result.error_category == "transaction_watchdog",
        "an SDK capture that returns after the watchdog must fail the transaction");
    Check(sdk.opens == 1 && sdk.captures == 1 && sdk.closes == 1 && !sdk.open,
        "an overdue SDK capture must still close the SDK session exactly once");
    Check(wpd.opens == 1 && wpd.closes == 1 && wpd.observes == 0 &&
              wpd.delete_attempts == 0 && wpd.abandons == 1,
        "an overdue SDK capture must not begin WPD recovery, persistence, or card deletion");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("HybridWpdObserveOpen") == std::string::npos &&
              events.find("HybridPersistPcOriginal") == std::string::npos,
        "overdue capture evidence must contain no success-side recovery or persistence state");
    fs::remove_all(root);
}

void TestHybridWatchdogStopsCanonicalRenameAndDelete() {
    const auto root = NewTestRoot("hybrid-persist-watchdog");
    HybridWpdFake wpd;
    HybridSdkFake sdk;
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-persist-watchdog", sdk.SdkVersion());
    Timeouts timeouts;
    timeouts.transaction_watchdog = std::chrono::seconds(2);

    const auto result = ExecuteHybridCaptureOnce(
        wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a", timeouts, {}, [] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        });

    const auto canonical = evidence.RunRoot() / result.transaction_id / "CAM-A" / "original.jpg";
    const auto partial = evidence.RunRoot() / result.transaction_id / "CAM-A" / "original.jpg.partial";
    Check(result.terminal_state == "FailedPartial" && result.error_category == "transaction_watchdog" &&
              result.frames.size() == 1 && result.frames.front().path == partial,
        "a deadline that expires during PC persistence must fail with the retained diagnostic partial");
    Check(!fs::exists(canonical) && fs::exists(partial) && wpd.delete_attempts == 0 &&
              wpd.spool_empty_after_checks == 0,
        "a deadline before rename must not create a canonical original, delete the card object, or continue cleanup");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("\"state\":\"Persisted\"") == std::string::npos &&
              events.find("\"state\":\"Complete\"") == std::string::npos,
        "a deadline before rename must not emit persisted or complete success states");
    fs::remove_all(root);
}

void TestHybridSdkCloseFailureBlocksWpdReopenAndRetry() {
    const auto root = NewTestRoot("hybrid-sdk-close-failure");
    HybridWpdFake wpd;
    HybridSdkFake sdk(true);
    EvidenceWriter evidence(root / "artifacts", "run-hybrid-sdk-close-failure", sdk.SdkVersion());
    const auto result = ExecuteHybridCaptureOnce(wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a");
    Check(result.terminal_state == "FailedPartial" && result.error_category == "close_failed",
        "SDK close failure must be terminal");
    Check(wpd.opens == 1 && wpd.observes == 0 && sdk.captures == 1 && sdk.opens == 1,
        "SDK close failure must block WPD reopen and any retry");
    Check(wpd.abandons == 1 && !wpd.token_live,
        "the WPD baseline token must be consumed when handoff fails");
    fs::remove_all(root);
}

void TestHybridSpoolAndCleanupFailuresRetainPcOriginal() {
    {
        const auto root = NewTestRoot("hybrid-spool-not-empty");
        HybridWpdFake wpd;
        wpd.spool_empty_before = false;
        HybridSdkFake sdk;
        EvidenceWriter evidence(root / "artifacts", "run-hybrid-spool-not-empty", sdk.SdkVersion());
        const auto result = ExecuteHybridCaptureOnce(wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a");
        Check(result.error_category == "spool_not_empty" && !result.spool_empty_before_capture &&
                  wpd.spool_empty_before_checks == 1 && sdk.captures == 0 && wpd.delete_attempts == 0,
            "a non-empty spool before capture must block SDK capture and deletion");
        fs::remove_all(root);
    }
    const auto run_retention_case = [](std::string_view name, bool delete_fails, bool spool_empty_after,
                                       std::string_view expected_category) {
        const auto root = NewTestRoot(std::string(name));
        HybridWpdFake wpd;
        wpd.delete_fails = delete_fails;
        wpd.spool_empty_after = spool_empty_after;
        HybridSdkFake sdk;
        EvidenceWriter evidence(root / "artifacts", "run-" + std::string(name), sdk.SdkVersion());
        const auto result = ExecuteHybridCaptureOnce(wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a");
        Check(result.terminal_state == "FailedPartial" && result.error_category == expected_category &&
                  result.frames.size() == 1 && result.frames.front().success && fs::exists(result.frames.front().path),
            std::string(name) + " must retain the verified PC original after cleanup failure");
        Check(wpd.delete_attempts == 1 && wpd.delete_successes == (delete_fails ? 0 : 1) &&
                  wpd.spool_empty_after_checks == (delete_fails ? 0 : 1),
            std::string(name) + " must make no cleanup retry");
        fs::remove_all(root);
    };
    run_retention_case("hybrid-delete-failure", true, true, "camera_object_delete_failed");
    run_retention_case("hybrid-empty-after-failure", false, false, "spool_not_empty_after_cleanup");
}

void TestHybridInvalidCandidatesAndMissingTokenNeverDelete() {
    const auto run_case = [](std::string_view name, std::vector<ImageCandidate> candidates,
                             std::string_view expected_category, bool expect_pc_original) {
        const auto root = NewTestRoot(std::string(name));
        HybridWpdFake wpd;
        wpd.candidates = std::move(candidates);
        HybridSdkFake sdk;
        EvidenceWriter evidence(root / "artifacts", "run-" + std::string(name), sdk.SdkVersion());
        const auto result = ExecuteHybridCaptureOnce(wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a");
        const bool original_exists = fs::exists(
            evidence.RunRoot() / result.transaction_id / "CAM-A" / "original.jpg");
        Check(result.terminal_state == "FailedPartial" && result.error_category == expected_category &&
                  wpd.delete_attempts == 0 && wpd.delete_successes == 0 &&
                  original_exists == expect_pc_original,
            std::string(name) + " must fail before delete and preserve only a verified PC original");
        fs::remove_all(root);
    };
    run_case("hybrid-invalid-jpeg",
        {{"broken.jpg", {0xFF, 0xD8, 0x01}, true, "cleanup-capability"}}, "invalid_jpeg", false);
    run_case("hybrid-missing-cleanup-token",
        {{"card.jpg", {0xFF, 0xD8, 0x01, 0xFF, 0xD9}, true, ""}}, "cleanup_token_missing", true);
}

void TestHybridCaptureArgumentConfirmations() {
    Check(ValidateHybridCaptureArguments("hybrid-capture-single", 1, true, true, true) == std::nullopt &&
              ValidateHybridCaptureArguments("hybrid-capture-single", 10, true, true, true) == std::nullopt &&
              ValidateHybridCaptureArguments("hybrid-capture-pair", 1, true, true, true, true) == std::nullopt &&
              ValidateHybridCaptureArguments("hybrid-capture-pair", 10, true, true, true, true) == std::nullopt &&
              ValidateHybridCaptureArguments("hybrid-capture-pair", 100, true, true, true, true) == std::nullopt &&
              ValidateHybridCaptureArguments("live-view-handoff", 10, true, true, true) == std::nullopt &&
              ValidateHybridCaptureArguments("hybrid-fault-single", 1, true, true, true) == std::nullopt &&
              ValidateHybridCaptureArguments("hybrid-fault-pair", 1, true, true, true, true) == std::nullopt &&
              ValidateHybridCaptureArguments("hybrid-interrupt-pair", 1, true, true, true, true) == std::nullopt,
        "approved hybrid commands must accept their permitted counts and all three confirmations");
    Check(ValidateHybridCaptureArguments("hybrid-capture-single", 2, true, true, true).has_value() &&
              ValidateHybridCaptureArguments("hybrid-capture-pair", 2, true, true, true, true).has_value() &&
              ValidateHybridCaptureArguments("hybrid-capture-pair", 1, true, true, true, false).has_value() &&
              ValidateHybridCaptureArguments("hybrid-capture-single", 1, true, true, true, true).has_value() &&
              ValidateHybridCaptureArguments("live-view-handoff", 1, true, true, true).has_value() &&
              ValidateHybridCaptureArguments("hybrid-fault-single", 2, true, true, true).has_value() &&
              ValidateHybridCaptureArguments("hybrid-fault-pair", 2, true, true, true, true).has_value() &&
              ValidateHybridCaptureArguments("hybrid-fault-pair", 1, true, true, true, false).has_value() &&
              ValidateHybridCaptureArguments("hybrid-interrupt-pair", 2, true, true, true, true).has_value() &&
              ValidateHybridCaptureArguments("hybrid-interrupt-pair", 1, true, true, true, false).has_value() &&
              ValidateHybridCaptureArguments("hybrid-capture-single", 1, false, true, true).has_value() &&
              ValidateHybridCaptureArguments("hybrid-capture-single", 1, true, false, true).has_value() &&
              ValidateHybridCaptureArguments("hybrid-capture-single", 1, true, true, false).has_value(),
        "hybrid argument validation must reject unsupported counts and each missing safety confirmation");
}

void TestWpdObjectDateCorrelationFailsClosed() {
    Check(WpdObjectDateIsAttributable(100.0, 100.0),
        "an object created at the cutoff may be attributable only after the cutoff clock advanced");
    Check(WpdObjectDateIsAttributable(101.0, 100.0),
        "an object created after the post-baseline device time may be attributable");
    Check(!WpdObjectDateIsAttributable(99.0, 100.0),
        "an object created before the post-baseline device time must be late");
    Check(!WpdObjectDateIsAttributable(std::nullopt, 100.0) &&
              !WpdObjectDateIsAttributable(101.0, std::nullopt) &&
              !WpdObjectDateIsAttributable(std::numeric_limits<double>::quiet_NaN(), 100.0),
        "missing or malformed WPD dates must fail closed");
    Check(WpdDeviceClockAdvanced(100.0, 101.0) &&
              !WpdDeviceClockAdvanced(100.0, 100.0) &&
              !WpdDeviceClockAdvanced(101.0, 100.0) &&
              !WpdDeviceClockAdvanced(std::nullopt, 101.0),
        "the baseline cutoff must be a strict device-clock tick after the initial quiet snapshot");
}

void TestHybridZeroMultipleAndLateCandidatesFailWithoutRetry() {
    const auto run_case = [](std::string_view name,
                             std::vector<ImageCandidate> candidates,
                             std::string_view expected_category,
                             bool expect_quarantine) {
        const auto root = NewTestRoot(std::string(name));
        HybridWpdFake wpd;
        wpd.candidates = std::move(candidates);
        HybridSdkFake sdk;
        EvidenceWriter evidence(root / "artifacts", "run-" + std::string(name), sdk.SdkVersion());
        const auto result = ExecuteHybridCaptureOnce(
            wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a");
        Check(result.terminal_state == "FailedPartial" && result.frames.size() == 1 &&
                  result.frames.front().error_category == expected_category,
            std::string(name) + " must fail with the expected attribution category");
        Check(wpd.opens == 2 && wpd.closes == 2 && wpd.observes == 1 &&
                  wpd.capture_commands == 0 && wpd.delete_attempts == 0 && sdk.opens == 1 && sdk.closes == 1 && sdk.captures == 1,
            std::string(name) + " must execute one hybrid attempt without WPD shutter or retry");
        Check(!fs::exists(evidence.RunRoot() / result.transaction_id / "CAM-A" / "original.jpg"),
            std::string(name) + " must not create a canonical original");
        const auto quarantine = root / "artifacts" / evidence.RunId() / "quarantine" /
            result.transaction_id / "CAM-A";
        Check(fs::exists(quarantine) == expect_quarantine,
            std::string(name) + " quarantine presence must match the available candidates");
        fs::remove_all(root);
    };

    run_case("hybrid-zero", {}, "no_candidate", false);
    run_case("hybrid-multiple",
        {{"first.jpg", {0xFF, 0xD8, 0x01, 0xFF, 0xD9}, true},
         {"second.jpg", {0xFF, 0xD8, 0x02, 0xFF, 0xD9}, true}},
        "ambiguous_candidates", true);
    run_case("hybrid-late",
        {{"late.jpg", {0xFF, 0xD8, 0x03, 0xFF, 0xD9}, false}},
        "late_candidate", true);
}

void TestHybridCaptureFailureAndObservationTokenAreFailClosed() {
    {
        const auto root = NewTestRoot("hybrid-sdk-capture-failure");
        HybridWpdFake wpd;
        HybridSdkFake sdk(false, true);
        EvidenceWriter evidence(root / "artifacts", "run-hybrid-sdk-capture-failure", sdk.SdkVersion());
        const auto result = ExecuteHybridCaptureOnce(
            wpd, wpd, sdk, sdk, evidence, "CAM-A", "wpd-a", "sdk-a");
        Check(result.terminal_state == "FailedPartial" && result.error_category == "image_event_timeout",
            "SDK card-capture failure must be terminal");
        Check(wpd.opens == 1 && wpd.observes == 0 && wpd.abandons == 1 &&
                  sdk.opens == 1 && sdk.captures == 1 && sdk.closes == 1,
            "SDK card-capture failure must close SDK, consume the baseline, and block WPD reopen");
        fs::remove_all(root);
    }
    {
        HybridWpdFake wpd;
        wpd.Open("wpd-a", {});
        const auto token = wpd.BeginPostCardObservation({});
        wpd.Close({});
        wpd.Open("wpd-a", {});
        (void)wpd.ObserveAndDownloadPostCardCapture(token, {}, {}, {});
        bool replay_rejected = false;
        try {
            (void)wpd.ObserveAndDownloadPostCardCapture(token, {}, {}, {});
        } catch (const TransportError& error) {
            replay_rejected = error.Category() == "baseline_mismatch";
        }
        Check(replay_rejected, "a post-card observation token must be single-use");
        wpd.Close({});
    }
    {
        HybridWpdFake wpd;
        wpd.Open("wpd-a", {});
        const auto token = wpd.BeginPostCardObservation({});
        wpd.Close({});
        wpd.Open("wpd-b", {});
        bool mismatch_rejected = false;
        try {
            (void)wpd.ObserveAndDownloadPostCardCapture(token, {}, {}, {});
        } catch (const TransportError& error) {
            mismatch_rejected = error.Category() == "identity_mismatch";
        }
        Check(mismatch_rejected && !wpd.token_live,
            "a post-card observation token must reject a different WPD identity and be consumed");
        wpd.Close({});
    }
}

} // namespace

int main() {
    try {
        TestHashAndJpeg();
        TestLiveViewFrameExtraction();
        TestLiveViewHandoffSummaryReplacement();
        TestStandaloneLiveViewSummaryReport();
        TestSdkReadOnlyCommandTraceRejectsMutatingBoundaries();
        TestSdkTraceBoundaryRecordsBeforeFakeMaidEntryExactlyOnce();
        TestSdkStatusProcessRoutingIsSeparateAndNarrow();
        TestSdkStatusSummaryIsReadOnlyAndRedacted();
        TestPackedStringLabelParserBounds();
        TestWpdVendorOpcodeDiagnosticsAreFailClosedAndRedacted();
        TestRedactedReportPublishesAtomically();
        TestWpdCorrelationSummaryIsAnonymousAndReportable();
        TestWpdCorrelationFailureEvidenceAndArgumentContract();
        TestSuccessfulLiveViewHandoff();
        TestWpdCommandTargetPolicyIsReported();
        TestWpdSpoolCountsEveryPayloadType();
        TestWpdSpoolStatusSummaryIsAnonymousAndReportable();
        TestOperatorGateReadyThenContinue();
        TestOperatorGateTimeoutRefusesOverwrite();
        TestProcessTerminationGateNeverContinues();
        TestLiveViewStopFailureBlocksWpdWithoutRetry();
        TestLiveViewCloseFailureBlocksWpdWithoutRetry();
        TestWpdFailureSkipsLiveViewResume();
        TestResumeFailureRetainsWpdOriginal();
        TestIdentityMap();
        TestNikonSdkStableIdentityUsesDocumentedSourceStrings();
        TestWpdStableIdentityUsesCameraSerialNotPnpPath();
        TestSingleIdentityV3PersistsOnlyWpdAuthority();
        TestCrossTransportBindingPrevalidatesBothMaps();
        TestDualIdentityVerificationRequiresExactAliasCardinality();
        TestDualSpoolVerificationRequiresIdentityAndBothEmptyCards();
        TestSuccessfulPairAndRedaction();
        TestAmbiguousCandidateFailsAndQuarantines();
        TestTransportFailureDoesNotRetry();
        TestTransportErrorDetailEvidence();
        TestUncertainDispatchQuarantinesExactlyOneAndStopsPair();
        TestUncertainDispatchAmbiguousAndZeroCandidates();
        TestUncertainDispatchObservationFailureRetainsCommandDiagnostic();
        TestLateCandidateFailsAndQuarantines();
        TestPairWatchdogStopsBeforeOpen();
        TestCloseFailureRetainsOriginalAndStopsPair();
        TestHybridCaptureOrdersOneCardCaptureAndNoWpdShutter();
        TestHybridPairRunsCamAThenCamBWithoutOverlapOrRetry();
        TestHybridPairRecoveryDetectsInterruptionAfterCamA();
        TestHybridPairRecoveryClassifiesActiveStagesAndInvalidEvidence();
        TestHybridPairStopsBeforeCamBAfterCamAFailure();
        TestHybridPairRetainsCamAWhenCamBFailsWithoutRetry();
        TestHybridPairCamASdkCloseFailureStopsBeforeCamB();
        TestHybridPairCamBWpdRecoveryFailureRetainsCamAWithoutRetry();
        TestHybridPairFaultHooksRunAfterEachSdkCloseWithoutRetry();
        TestHybridPairSharedWatchdogStopsBeforeCamB();
        TestHybridPairBoundaryCallbackFailureNeverStartsCamB();
        TestHybridPairRunAggregatesOneHundredSuccessfulPairs();
        TestHybridPairRunStopsAtFirstFailureAndRetainsCounts();
        TestHybridFaultGateRunsAfterSdkCloseAndFailsWithoutDeleteOrRetry();
        TestHybridWatchdogStopsBeforeHardwareOpen();
        TestHybridWatchdogClosesSdkAndStopsBeforeWpdRecovery();
        TestHybridWatchdogStopsCanonicalRenameAndDelete();
        TestHybridSdkCloseFailureBlocksWpdReopenAndRetry();
        TestHybridSpoolAndCleanupFailuresRetainPcOriginal();
        TestHybridInvalidCandidatesAndMissingTokenNeverDelete();
        TestHybridCaptureArgumentConfirmations();
        TestWpdObjectDateCorrelationFailsClosed();
        TestHybridZeroMultipleAndLateCandidatesFailWithoutRetry();
        TestHybridCaptureFailureAndObservationTokenAreFailClosed();
    } catch (const std::exception& error) {
        std::cerr << "UNEXPECTED: " << error.what() << '\n';
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All Phase 0 contract tests passed\n";
    return 0;
}
