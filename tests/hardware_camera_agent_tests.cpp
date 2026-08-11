#include "a0/phase0/hardware_camera_agent.hpp"
#include "a0/phase0/hardware_process_lease.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using namespace a0::phase0;

namespace {

int failures = 0;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::vector<unsigned char> FullSizeOriginalJpeg(
    std::uint16_t width = 7360U,
    std::uint16_t height = 4912U) {
    return {
        0xFFU, 0xD8U,
        0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U,
        static_cast<unsigned char>(height >> 8U),
        static_cast<unsigned char>(height & 0xFFU),
        static_cast<unsigned char>(width >> 8U),
        static_cast<unsigned char>(width & 0xFFU),
        0x03U, 0x01U, 0x11U, 0x00U, 0x02U, 0x11U, 0x00U,
        0x03U, 0x11U, 0x00U,
        0xFFU, 0xD9U,
    };
}

std::string Envelope(std::string_view operation, std::string_view payload) {
    return "{\"schemaVersion\":\"a0.camera-agent.hardware.v1\","
           "\"simulation\":false,\"marker\":\"Hardware\",\"requestId\":\"req-1\","
           "\"operation\":\"" + std::string(operation) + "\",\"payload\":" +
        std::string(payload) + "}";
}

void WriteText(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.flush();
    if (!output) throw std::runtime_error("test text write failed");
}

std::string ProfileJson(
    std::string_view profile_id = "approved-test-profile",
    std::uint32_t version = 1,
    std::string_view alias = "CAM-A",
    std::string_view expires_at = "2099-12-31T23:59:59Z") {
    return "{\"schemaVersion\":\"a0.camera-agent.capture-profile.v1\","
           "\"profileId\":\"" + std::string(profile_id) +
        "\",\"profileVersion\":" + std::to_string(version) +
        ",\"selectedAlias\":\"" + std::string(alias) +
        "\",\"cameraMode\":\"SingleCamera\",\"approved\":true,"
        "\"approvedBy\":\"test-operator\","
        "\"approvalReference\":\"test-approval-record\","
        "\"approvedAtUtc\":\"2020-01-01T00:00:00Z\","
        "\"expiresAtUtc\":\"" + std::string(expires_at) +
        "\",\"expectedSettings\":{\"fileType\":{\"available\":false}}}";
}

std::string Sha256OfText(std::string_view value) {
    return Sha256Hex(std::vector<unsigned char>(value.begin(), value.end()));
}

SdkCameraStatus ConfirmedLiveViewOffStatus() {
    SdkCameraStatus status;
    status.live_view_status_available = true;
    status.live_view_status = "off";
    return status;
}

std::function<SdkCameraStatus()> ConfirmedLiveViewOffProbe() {
    return [] { return ConfirmedLiveViewOffStatus(); };
}

std::string CaptureEnvelope(std::string_view transaction_id, bool live_view = false) {
    return Envelope(
        "capture-single",
        "{\"transactionId\":\"" + std::string(transaction_id) +
            "\",\"cameraAlias\":\"CAM-A\",\"expectedCaptureProfileId\":\"approved-test-profile\","
            "\"expectedCaptureProfileVersion\":1,\"expectedCaptureProfileSha256\":\"" +
            std::string(64, 'c') +
            "\",\"expectedCaptureProfileExpiresAtUtc\":\"2099-12-31T23:59:59Z\","
            "\"exclusiveCameraControlConfirmed\":true,"
            "\"dedicatedSpoolScopeConfirmed\":true,\"exactObjectDeleteConfirmed\":true,"
            "\"liveViewHandoffRequested\":" + (live_view ? "true" : "false") + "}");
}

class FakeBackend final : public IHardwareCameraAgentBackend {
public:
    SingleCameraReadinessResult GetSingleReadiness(std::string_view alias) override {
        ++readiness_calls;
        SingleCameraReadinessResult result;
        result.ready = true;
        result.camera_alias = std::string(alias);
        result.sdk_camera_count = 1;
        result.wpd_camera_count = 1;
        result.sdk_identity_bound = true;
        result.wpd_identity_bound = true;
        result.sdk_alias_matches = true;
        result.wpd_alias_matches = true;
        result.sdk_status_probed = true;
        result.live_view_status_available = true;
        result.live_view_status = "off";
        result.spool_inspected = true;
        result.spool_known_empty = true;
        result.capture_profile_approved = true;
        result.capture_profile_id = "approved-test-profile";
        result.capture_profile_version = 1;
        result.capture_profile_sha256 = std::string(64, 'c');
        result.capture_profile_camera_alias = std::string(alias);
        result.profile_expires_at_utc = "2099-12-31T23:59:59Z";
        result.capture_profile_alias_matches = true;
        result.settings_match_approved_profile = true;
        result.read_only = true;
        if (return_malformed_readiness) {
            result.sdk_camera_count = 2;
            result.read_only = false;
        }
        if (return_ready_with_live_view_on) {
            result.live_view_status = "on";
        }
        return result;
    }

    SingleCameraCaptureResult CaptureSingle(const HardwareCameraAgentRequest& request) override {
        ++capture_calls;
        SingleCameraCaptureResult result;
        result.succeeded = true;
        result.camera_alias = request.camera_alias;
        result.run_id = "run-1700000000000-1";
        result.transaction_id = request.transaction_id;
        result.capture_profile_id = "approved-test-profile";
        result.capture_profile_version = 1;
        result.capture_profile_sha256 = std::string(64, 'c');
        result.capture_profile_camera_alias = request.camera_alias;
        result.profile_expires_at_utc = "2099-12-31T23:59:59Z";
        result.terminal_state = "Complete";
        result.retained_original = RetainedOriginalRecord{
            request.camera_alias,
            fs::path(L"C:\\agent-test\\original.jpg"),
            5,
            std::string(64, 'a'),
        };
        result.spool_empty_before_capture = true;
        result.camera_object_delete_attempted = true;
        result.camera_object_delete_succeeded = true;
        result.spool_empty_after_cleanup = true;
        if (return_malformed_capture) {
            result.retained_original.reset();
            result.spool_empty_before_capture = false;
            result.camera_object_delete_attempted = false;
            result.camera_object_delete_succeeded = false;
            result.spool_empty_after_cleanup = false;
        }
        if (return_mismatched_capture_profile) {
            result.capture_profile_id = "different-approved-profile";
        }
        if (return_mismatched_capture_alias) {
            result.camera_alias = "CAM-B";
            result.capture_profile_camera_alias = "CAM-B";
            result.retained_original->camera_alias = "CAM-B";
        }
        if (return_mismatched_handoff) {
            result.live_view_handoff_requested = true;
            result.live_view_stopped_before_capture = true;
            result.live_view_sdk_session_closed_before_capture = true;
            result.live_view_resume_attempted = true;
            result.live_view_resumed = true;
            result.resumed_preview = PreviewJpegRecord{
                fs::path(L"C:\\agent-test\\post-capture-preview.jpg"),
                5,
                std::string(64, 'e'),
            };
        }
        return result;
    }

    SingleCameraLiveViewProbeResult ProbeLiveView(
        const HardwareCameraAgentRequest& request) override {
        ++live_view_calls;
        SingleCameraLiveViewProbeResult result;
        result.succeeded = true;
        result.camera_alias = request.camera_alias;
        result.run_id = "run-1700000000000-2";
        result.frames = request.live_view_frames;
        result.last_frame_bytes = 5;
        result.last_frame_sha256 = std::string(64, 'b');
        result.preview_persisted = true;
        result.preview = PreviewJpegRecord{
            fs::path(L"C:\\agent-test\\preview.jpg"), 5, std::string(64, 'b')};
        result.live_view_stopped = true;
        result.sdk_session_closed = true;
        if (return_malformed_live_view) {
            result.preview_is_original = true;
            result.sdk_session_closed = false;
        }
        return result;
    }

    SingleCameraCaptureResult GetTransactionResult(std::string_view transaction_id) override {
        ++transaction_calls;
        SingleCameraCaptureResult result;
        result.transaction_id = std::string(transaction_id);
        result.camera_alias = "CAM-A";
        result.run_id = "run-1700000000000-1";
        result.terminal_state = "FailedPartial";
        result.error_category = "process_interrupted";
        result.error_detail = "capture is never retried";
        return result;
    }

    ContinuousLiveViewResult StartContinuousLiveView(
        const HardwareCameraAgentRequest& request) override {
        ++continuous_start_calls;
        continuous_session = request.session_id;
        ContinuousLiveViewResult result;
        result.succeeded = true;
        result.session_id = request.session_id;
        result.state = "Started";
        result.sdk_session_open = true;
        result.live_view_running = true;
        return result;
    }

    ContinuousLiveViewResult ReadContinuousLiveViewFrame(
        const HardwareCameraAgentRequest& request) override {
        ++continuous_frame_calls;
        ContinuousLiveViewResult result;
        result.succeeded = true;
        result.session_id = request.session_id;
        result.state = "Frame";
        result.frame_number = 1;
        result.frame_size = 4;
        result.frame_sha256 = std::string(64, 'd');
        result.frame_jpeg_base64 = "/9j/2Q==";
        result.sdk_session_open = true;
        result.live_view_running = true;
        return result;
    }

    ContinuousLiveViewResult HeartbeatContinuousLiveView(
        const HardwareCameraAgentRequest& request) override {
        ContinuousLiveViewResult result;
        result.succeeded = true;
        result.session_id = request.session_id;
        result.state = "Heartbeat";
        result.frame_number = 1;
        result.sdk_session_open = true;
        result.live_view_running = true;
        return result;
    }

    ContinuousLiveViewResult StopContinuousLiveView(
        const HardwareCameraAgentRequest& request) override {
        ++continuous_stop_calls;
        ContinuousLiveViewResult result;
        result.succeeded = true;
        result.session_id = request.session_id;
        result.state = "Stopped";
        result.frame_number = 1;
        return result;
    }

    ContinuousLiveViewResult CloseAgentSession(
        const HardwareCameraAgentRequest& request) override {
        ++continuous_close_calls;
        ContinuousLiveViewResult result;
        result.succeeded = true;
        result.session_id = request.session_id;
        result.state = "Closed";
        result.frame_number = 1;
        return result;
    }

    int readiness_calls{};
    int capture_calls{};
    int live_view_calls{};
    int transaction_calls{};
    int continuous_start_calls{};
    int continuous_frame_calls{};
    int continuous_stop_calls{};
    int continuous_close_calls{};
    std::string continuous_session;
    bool return_malformed_capture{};
    bool return_mismatched_capture_profile{};
    bool return_mismatched_capture_alias{};
    bool return_mismatched_handoff{};
    bool return_malformed_readiness{};
    bool return_ready_with_live_view_on{};
    bool return_malformed_live_view{};
};

std::string JsonEscapeForTest(std::string_view value) {
    std::string output;
    for (const char character : value) {
        if (character == '\\' || character == '"') output.push_back('\\');
        output.push_back(character);
    }
    return output;
}

std::string JournalJson(
    std::string_view transaction_id,
    std::string_view run_id,
    std::string_view terminal_state,
    std::string_view error_category = {},
    bool succeeded = false,
    std::string_view original_path = {},
    std::size_t original_size = 0,
    std::string_view original_sha256 = {},
    bool complete_flags = false,
    std::string_view profile_expires_at = "2099-12-31T23:59:59Z") {
    const bool original_present = !original_path.empty();
    const bool profile_present = terminal_state == "Complete";
    return
        "{\"schemaVersion\":\"a0.camera-agent.transaction.v1\","
        "\"cameraMode\":\"SingleCamera\",\"requiredCameraAlias\":\"CAM-A\","
        "\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"cameraAlias\":\"CAM-A\",\"runId\":\"" +
        std::string(run_id) + "\",\"succeeded\":" + (succeeded ? "true" : "false") +
        ",\"captureProfileId\":\"" + (profile_present ? "approved-test-profile" : "") +
        "\",\"captureProfileVersion\":" + (profile_present ? "1" : "0") +
        ",\"captureProfileSha256\":\"" +
        (profile_present ? std::string(64, 'c') : std::string()) +
        "\",\"captureProfileCameraAlias\":\"" + (profile_present ? "CAM-A" : "") +
        "\",\"profileExpiresAtUtc\":\"" +
        (profile_present ? std::string(profile_expires_at) : std::string()) +
        "\",\"liveViewHandoffRequested\":false,"
        "\"liveViewStoppedBeforeCapture\":false,"
        "\"liveViewSdkSessionClosedBeforeCapture\":false,"
        "\"postCaptureLiveViewProbeAttempted\":false,"
        "\"postCaptureLiveViewProbeSucceeded\":false,"
        "\"postCapturePreviewPresent\":false,"
        "\"postCapturePreviewPath\":\"\",\"postCapturePreviewSizeBytes\":0,"
        "\"postCapturePreviewSha256\":\"\"" +
        ",\"terminalState\":\"" + std::string(terminal_state) +
        "\",\"errorCategory\":\"" + std::string(error_category) +
        "\",\"errorDetail\":\"" + (error_category.empty() ? "" : "test failure") +
        "\",\"originalPresent\":" + (original_present ? "true" : "false") +
        ",\"originalCameraAlias\":\"" + (original_present ? "CAM-A" : "") +
        "\",\"originalPath\":\"" + JsonEscapeForTest(original_path) +
        "\",\"originalSizeBytes\":" + std::to_string(original_size) +
        ",\"originalSha256\":\"" + std::string(original_sha256) +
        "\",\"spoolEmptyBeforeCapture\":" + (complete_flags ? "true" : "false") +
        ",\"cameraObjectDeleteAttempted\":" + (complete_flags ? "true" : "false") +
        ",\"cameraObjectDeleteSucceeded\":" + (complete_flags ? "true" : "false") +
        ",\"spoolEmptyAfterCleanup\":" + (complete_flags ? "true" : "false") +
        ",\"automaticRetryCount\":0,\"transactionWatchdogSeconds\":180,"
        "\"realIdentifiersIncluded\":false}";
}

void ReplaceJournal(
    const fs::path& transaction_root,
    std::string_view transaction_id,
    const std::string& json) {
    const fs::path directory = transaction_root / std::string(transaction_id);
    fs::create_directories(directory);
    const fs::path final = directory / "transaction.json";
    const fs::path partial = directory / ("transaction-" + NewRunId() + ".partial");
    {
        std::ofstream output(partial, std::ios::binary);
        output << json;
        output.flush();
        if (!output) throw std::runtime_error("test journal write failed");
    }
    if (!MoveFileExW(
            partial.c_str(), final.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("test journal atomic replace failed");
    }
}

class HybridWpdFake final : public ICameraTransport, public IPostCardObservationTransport {
public:
    std::string SdkVersion() const override { return "wpd-fake"; }
    std::vector<CameraInfo> Enumerate() override {
        return {{"Nikon D810", "fw", "M", "wpd-one"}};
    }
    void Open(std::string_view, std::chrono::seconds) override {
        ++opens;
        if (active_sessions != 0) throw TransportError("session_overlap", "fake overlap");
        ++active_sessions;
        open = true;
    }
    std::string Baseline(std::chrono::seconds timeout) override {
        return BeginPostCardObservation(timeout);
    }
    std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view,
        std::chrono::seconds,
        std::chrono::seconds,
        std::chrono::seconds) override {
        throw TransportError("unexpected_wpd_shutter", "WPD shutter must not be used");
    }
    void Close(std::chrono::seconds) override {
        if (open) --active_sessions;
        open = false;
        ++closes;
    }
    std::string BeginPostCardObservation(std::chrono::seconds) override {
        if (!open) throw TransportError("session_not_open", "WPD baseline without open");
        return "observation-token";
    }
    std::vector<ImageCandidate> ObserveAndDownloadPostCardCapture(
        std::string_view token,
        std::chrono::seconds,
        std::chrono::seconds,
        std::chrono::seconds) override {
        if (!open || token != "observation-token") {
            throw TransportError("baseline_mismatch", "fake observation mismatch");
        }
        if (!observed_candidates.empty()) return observed_candidates;
        return {{"private-name.jpg", FullSizeOriginalJpeg(), true, "exact-object"}};
    }
    void DeleteRecoveredObject(std::string_view token, std::chrono::seconds) override {
        if (token != "exact-object") throw TransportError("delete_scope", "wrong object");
        ++delete_attempts;
    }
    void VerifyJpegSpoolEmpty(std::chrono::seconds) override { ++empty_after_checks; }
    void AbandonPostCardObservation(std::string_view) noexcept override {}

    static inline int active_sessions{};
    bool open{};
    int opens{};
    int closes{};
    int delete_attempts{};
    int empty_after_checks{};
    std::vector<ImageCandidate> observed_candidates;
};

class HybridSdkFake final : public ICameraTransport, public ICardCaptureTransport {
public:
    std::string SdkVersion() const override { return "sdk-fake"; }
    std::vector<CameraInfo> Enumerate() override {
        return {{"Nikon D810", "fw", "M", "sdk-one"}};
    }
    void Open(std::string_view, std::chrono::seconds) override {
        ++opens;
        if (HybridWpdFake::active_sessions != 0) {
            throw TransportError("session_overlap", "SDK/WPD overlap");
        }
        ++HybridWpdFake::active_sessions;
        open = true;
    }
    std::string Baseline(std::chrono::seconds) override { return {}; }
    std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view,
        std::chrono::seconds,
        std::chrono::seconds,
        std::chrono::seconds) override {
        throw TransportError("unexpected_download", "SDK download must not be used");
    }
    void Close(std::chrono::seconds) override {
        if (open) --HybridWpdFake::active_sessions;
        open = false;
        ++closes;
    }
    void CaptureToCard(std::chrono::seconds, std::chrono::seconds) override {
        if (!open) throw TransportError("session_not_open", "SDK capture without open");
        ++captures;
    }

    bool open{};
    int opens{};
    int closes{};
    int captures{};
};

void TestStrictProtocolAndTypedResponses() {
    FakeBackend backend;
    HardwareCameraAgentDispatcher dispatcher(backend);
    const std::string readiness = dispatcher.Handle(Envelope(
        "get-single-readiness", "{\"cameraAlias\":\"CAM-A\"}"));
    Check(readiness.find("\"resultCode\":\"SingleReady\"") != std::string::npos,
        "readiness should be typed as SingleReady");
    Check(readiness.find("\"readOnly\":true") != std::string::npos,
        "readiness should explicitly remain read-only");

    const std::string transaction_id = "0123456789abcdef0123456789abcdef";
    const std::string capture = dispatcher.Handle(Envelope(
        "capture-single",
        "{\"transactionId\":\"" + transaction_id +
            "\",\"cameraAlias\":\"CAM-A\",\"expectedCaptureProfileId\":\"approved-test-profile\","
            "\"expectedCaptureProfileVersion\":1,\"expectedCaptureProfileSha256\":\"" +
            std::string(64, 'c') +
            "\",\"expectedCaptureProfileExpiresAtUtc\":\"2099-12-31T23:59:59Z\","
            "\"exclusiveCameraControlConfirmed\":true,"
            "\"dedicatedSpoolScopeConfirmed\":true,\"exactObjectDeleteConfirmed\":true,"
            "\"liveViewHandoffRequested\":false}"));
    Check(backend.capture_calls == 1, "valid capture should reach the backend exactly once");
    Check(capture.find("\"transactionId\":\"" + transaction_id + "\"") != std::string::npos,
        "capture response should preserve the client transaction ID");
    Check(capture.find("\"retainedOriginal\":{") != std::string::npos &&
          capture.find("\"sizeBytes\":5") != std::string::npos &&
          capture.find(std::string(64, 'a')) != std::string::npos,
        "capture response should type retained path, size, and SHA-256");

    const std::string unsafe = dispatcher.Handle(Envelope(
        "capture-single",
        "{\"transactionId\":\"" + transaction_id +
            "\",\"cameraAlias\":\"CAM-A\",\"expectedCaptureProfileId\":\"approved-test-profile\","
            "\"expectedCaptureProfileVersion\":1,\"expectedCaptureProfileSha256\":\"" +
            std::string(64, 'c') +
            "\",\"expectedCaptureProfileExpiresAtUtc\":\"2099-12-31T23:59:59Z\","
            "\"exclusiveCameraControlConfirmed\":true,"
            "\"dedicatedSpoolScopeConfirmed\":false,\"exactObjectDeleteConfirmed\":true,"
            "\"liveViewHandoffRequested\":false}"));
    Check(backend.capture_calls == 1 &&
          unsafe.find("\"rejectionCode\":\"SafetyConfirmationRequired\"") != std::string::npos,
        "a missing capture confirmation must reject before backend access");

    std::string missing_profile_expiry = CaptureEnvelope(transaction_id);
    const std::string expiry_field =
        "\"expectedCaptureProfileExpiresAtUtc\":\"2099-12-31T23:59:59Z\",";
    const auto expiry_position = missing_profile_expiry.find(expiry_field);
    Check(expiry_position != std::string::npos,
        "capture test envelope must contain the expected profile expiry");
    missing_profile_expiry.erase(expiry_position, expiry_field.size());
    const std::string missing_expiry_response =
        dispatcher.Handle(missing_profile_expiry);
    Check(backend.capture_calls == 1 &&
          missing_expiry_response.find("\"rejectionCode\":\"UnexpectedField\"") !=
              std::string::npos,
        "capture must reject a missing readiness-approved profile expiry before backend access");

    std::string simulated = Envelope("get-single-readiness", "{\"cameraAlias\":\"CAM-A\"}");
    const auto marker = simulated.find("\"simulation\":false");
    simulated.replace(marker, std::string("\"simulation\":false").size(), "\"simulation\":true");
    const std::string simulation_rejected = dispatcher.Handle(simulated);
    Check(backend.readiness_calls == 1 &&
          simulation_rejected.find("HardwareMarkerRequired") != std::string::npos,
        "simulation envelopes must never enter the hardware backend");

    const std::string unknown = dispatcher.Handle(Envelope(
        "get-single-readiness", "{\"cameraAlias\":\"CAM-A\",\"extra\":true}"));
    Check(backend.readiness_calls == 1 && unknown.find("UnexpectedField") != std::string::npos,
        "unknown fields must reject before hardware access");

    const std::string live = dispatcher.Handle(Envelope(
        "live-view-probe",
        "{\"cameraAlias\":\"CAM-A\",\"exclusiveCameraControlConfirmed\":true,"
        "\"liveViewFrames\":2,\"liveViewIntervalMs\":0}"));
    Check(live.find("\"previewPersisted\":true") != std::string::npos &&
          live.find("\"previewIsOriginal\":false") != std::string::npos &&
          live.find("\"previewIsStitchInput\":false") != std::string::npos,
        "Live View preview must be typed and excluded from original/stitch input roles");

    const std::string queried = dispatcher.Handle(Envelope(
        "get-transaction-result", "{\"transactionId\":\"" + transaction_id + "\"}"));
    Check(backend.transaction_calls == 1 && queried.find("process_interrupted") != std::string::npos,
        "durable transaction query should return typed terminal failure without capture");

    FakeBackend malformed_backend;
    malformed_backend.return_malformed_capture = true;
    HardwareCameraAgentDispatcher malformed_dispatcher(malformed_backend);
    const std::string malformed = malformed_dispatcher.Handle(Envelope(
        "capture-single",
        "{\"transactionId\":\"" + transaction_id +
            "\",\"cameraAlias\":\"CAM-A\",\"expectedCaptureProfileId\":\"approved-test-profile\","
            "\"expectedCaptureProfileVersion\":1,\"expectedCaptureProfileSha256\":\"" +
            std::string(64, 'c') +
            "\",\"expectedCaptureProfileExpiresAtUtc\":\"2099-12-31T23:59:59Z\","
            "\"exclusiveCameraControlConfirmed\":true,"
            "\"dedicatedSpoolScopeConfirmed\":true,\"exactObjectDeleteConfirmed\":true,"
            "\"liveViewHandoffRequested\":false}"));
    Check(malformed.find("\"resultCode\":\"InvalidBackendResult\"") != std::string::npos &&
          malformed.find("\"success\":false") != std::string::npos,
        "dispatcher must reject a fake Complete success without original/cleanup invariants");

    FakeBackend malformed_readiness_backend;
    malformed_readiness_backend.return_malformed_readiness = true;
    HardwareCameraAgentDispatcher malformed_readiness_dispatcher(
        malformed_readiness_backend);
    const std::string malformed_readiness = malformed_readiness_dispatcher.Handle(Envelope(
        "get-single-readiness", "{\"cameraAlias\":\"CAM-A\"}"));
    Check(malformed_readiness.find("\"resultCode\":\"InvalidBackendResult\"") !=
              std::string::npos,
        "dispatcher must reject readiness that is not a one-camera read-only observation");

    FakeBackend live_view_on_readiness_backend;
    live_view_on_readiness_backend.return_ready_with_live_view_on = true;
    HardwareCameraAgentDispatcher live_view_on_readiness_dispatcher(
        live_view_on_readiness_backend);
    const std::string live_view_on_readiness =
        live_view_on_readiness_dispatcher.Handle(Envelope(
            "get-single-readiness", "{\"cameraAlias\":\"CAM-A\"}"));
    Check(live_view_on_readiness.find("\"resultCode\":\"InvalidBackendResult\"") !=
              std::string::npos,
        "dispatcher must reject a forged Ready response unless Live View is confirmed OFF");

    FakeBackend malformed_live_backend;
    malformed_live_backend.return_malformed_live_view = true;
    HardwareCameraAgentDispatcher malformed_live_dispatcher(malformed_live_backend);
    const std::string malformed_live = malformed_live_dispatcher.Handle(Envelope(
        "live-view-probe",
        "{\"cameraAlias\":\"CAM-A\",\"exclusiveCameraControlConfirmed\":true,"
        "\"liveViewFrames\":1,\"liveViewIntervalMs\":0}"));
    Check(malformed_live.find("\"resultCode\":\"InvalidBackendResult\"") !=
              std::string::npos,
        "dispatcher must reject a forged Live View success or preview role");

    FakeBackend mismatched_profile_backend;
    mismatched_profile_backend.return_mismatched_capture_profile = true;
    HardwareCameraAgentDispatcher mismatched_profile_dispatcher(
        mismatched_profile_backend);
    Check(mismatched_profile_dispatcher.Handle(CaptureEnvelope(transaction_id)).find(
              "\"resultCode\":\"InvalidBackendResult\"") != std::string::npos,
        "CaptureComplete must correlate to the request's approved profile snapshot");

    FakeBackend mismatched_alias_backend;
    mismatched_alias_backend.return_mismatched_capture_alias = true;
    HardwareCameraAgentDispatcher mismatched_alias_dispatcher(mismatched_alias_backend);
    Check(mismatched_alias_dispatcher.Handle(CaptureEnvelope(transaction_id)).find(
              "\"resultCode\":\"InvalidBackendResult\"") != std::string::npos,
        "CaptureComplete must correlate to the requested camera alias");

    FakeBackend mismatched_handoff_backend;
    mismatched_handoff_backend.return_mismatched_handoff = true;
    HardwareCameraAgentDispatcher mismatched_handoff_dispatcher(
        mismatched_handoff_backend);
    Check(mismatched_handoff_dispatcher.Handle(CaptureEnvelope(transaction_id)).find(
              "\"resultCode\":\"InvalidBackendResult\"") != std::string::npos,
        "CaptureComplete must correlate to the requested Live View handoff snapshot");
}

void TestServeOnceRejectsPartialFrameWithoutDispatch() {
    FakeBackend backend;
    HardwareCameraAgentDispatcher dispatcher(backend);
    const std::string pipe_name =
        "A0CameraStitcher.CameraAgent.Hardware.v1.test-" + NewRunId();
    const std::wstring full_pipe_name =
        L"\\\\.\\pipe\\" + std::wstring(pipe_name.begin(), pipe_name.end());

    auto server = std::async(std::launch::async, [&] {
        return RunHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
    });

    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto connect_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < connect_deadline) {
        pipe = CreateFileW(
            full_pipe_name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    Check(pipe != INVALID_HANDLE_VALUE,
        "serve-once pipe should accept its same-session test client");
    if (pipe != INVALID_HANDLE_VALUE) {
        const std::array<unsigned char, 2> partial_header{0x20U, 0x00U};
        DWORD written = 0;
        Check(WriteFile(
                  pipe,
                  partial_header.data(),
                  static_cast<DWORD>(partial_header.size()),
                  &written,
                  nullptr) != FALSE &&
              written == partial_header.size(),
            "partial-frame test client should write only half of the length header");
        CloseHandle(pipe);
    }

    const auto completion = server.wait_for(std::chrono::seconds(8));
    Check(completion == std::future_status::ready,
        "serve-once must terminate after a partial-frame disconnect");
    const int server_exit = server.get();
    Check(server_exit == 2,
        "serve-once must report a pre-dispatch partial frame as transport failure");
    Check(backend.readiness_calls == 0 && backend.capture_calls == 0 &&
          backend.live_view_calls == 0 && backend.transaction_calls == 0,
        "partial request framing must never dispatch any hardware operation");
}

void TestDurableJournalRecoveryContracts() {
    const fs::path root = fs::temp_directory_path() / ("a0-agent-journal-test-" + NewRunId());
    try {
        ProductionHardwareCameraAgentConfig config;
        config.artifacts_root = root / "artifacts";
        config.reports_root = root / "reports";
        config.sdk_identity_map = root / "sdk-map.json";
        config.wpd_identity_map = root / "wpd-map.json";
        config.transaction_state_root = root / "transactions";
        std::promise<void> initial_read_promise;
        auto initial_read = initial_read_promise.get_future();
        std::promise<void> continue_query_promise;
        auto continue_query = continue_query_promise.get_future();
        config.after_initial_active_journal_read_for_testing = [&] {
            initial_read_promise.set_value();
            continue_query.wait();
        };
        ProductionHardwareCameraAgentBackend backend(config);

        const std::string race_id = "11111111111111111111111111111111";
        const std::string race_run = "run-1700000000000-11";
        ReplaceJournal(
            config.transaction_state_root,
            race_id,
            JournalJson(race_id, race_run, "InProgress"));

        std::promise<void> lease_acquired_promise;
        auto lease_acquired = lease_acquired_promise.get_future();
        std::promise<void> release_lease_promise;
        auto release_lease = release_lease_promise.get_future();
        std::thread capture_owner([&] {
            HardwareProcessLease lease(
                "A0CameraStitcher.CameraAgent.Transaction.v1." + race_id);
            lease_acquired_promise.set_value();
            release_lease.wait();
        });
        lease_acquired.wait();
        auto query = std::async(std::launch::async, [&] {
            return backend.GetTransactionResult(race_id);
        });
        initial_read.wait();
        ReplaceJournal(
            config.transaction_state_root,
            race_id,
            JournalJson(race_id, race_run, "FailedPartial", "capture_failed"));
        release_lease_promise.set_value();
        continue_query_promise.set_value();
        capture_owner.join();
        const auto raced_result = query.get();
        Check(raced_result.terminal_state == "FailedPartial" &&
              raced_result.error_category == "capture_failed",
            "get-result must reread after lease acquisition and preserve the newer terminal journal");
        const auto reread_result = backend.GetTransactionResult(race_id);
        Check(reread_result.error_category == "capture_failed",
            "terminal journal must never regress to process_interrupted");

        ProductionHardwareCameraAgentConfig no_hook_config = config;
        no_hook_config.after_initial_active_journal_read_for_testing = {};
        ProductionHardwareCameraAgentBackend no_hook_backend(no_hook_config);
        const std::string reserved_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        ReplaceJournal(
            config.transaction_state_root,
            reserved_id,
            JournalJson(reserved_id, "run-1700000000000-12", "Reserved"));
        std::promise<void> reserved_owner_ready_promise;
        auto reserved_owner_ready = reserved_owner_ready_promise.get_future();
        std::promise<void> release_reserved_owner_promise;
        auto release_reserved_owner = release_reserved_owner_promise.get_future();
        std::thread reserved_owner([&] {
            HardwareProcessLease lease(
                "A0CameraStitcher.CameraAgent.Transaction.v1." + reserved_id);
            reserved_owner_ready_promise.set_value();
            release_reserved_owner.wait();
        });
        reserved_owner_ready.wait();
        const auto active_reserved = no_hook_backend.GetTransactionResult(reserved_id);
        Check(active_reserved.terminal_state == "Reserved" &&
              active_reserved.error_category == "transaction_reserved",
            "active Reserved transaction must not be downgraded before camera lease acquisition");
        release_reserved_owner_promise.set_value();
        reserved_owner.join();
        const auto abandoned_reserved = no_hook_backend.GetTransactionResult(reserved_id);
        Check(abandoned_reserved.terminal_state == "FailedPartial" &&
              abandoned_reserved.error_category == "transaction_abandoned_before_camera",
            "orphaned Reserved transaction must become terminal without camera recovery or retry");

        const std::string incomplete_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        fs::create_directories(config.transaction_state_root / incomplete_id);
        const auto incomplete = no_hook_backend.GetTransactionResult(incomplete_id);
        Check(incomplete.terminal_state == "FailedPartial" &&
              incomplete.error_category == "transaction_reservation_incomplete",
            "reserved directory without initial journal must remain a typed no-retry failure; actual=" +
                incomplete.terminal_state + "/" + incomplete.error_category);

        const fs::path reservation_race_root = root / "reservation-race";
        ProductionHardwareCameraAgentConfig reservation_race_config;
        reservation_race_config.artifacts_root =
            reservation_race_root / "artifacts";
        reservation_race_config.reports_root = reservation_race_root / "reports";
        reservation_race_config.sdk_identity_map =
            reservation_race_root / "sdk-map.json";
        reservation_race_config.wpd_identity_map =
            reservation_race_root / "wpd-map.json";
        reservation_race_config.transaction_state_root =
            reservation_race_root / "transactions";
        reservation_race_config.approved_capture_profile =
            reservation_race_root / "approved-profile.json";
        const std::string reservation_profile = ProfileJson();
        WriteText(
            reservation_race_config.approved_capture_profile,
            reservation_profile);
        WriteText(reservation_race_config.sdk_identity_map, "{malformed-map");
        WriteText(
            reservation_race_config.wpd_identity_map,
            "{\"CAM-A\":\"wpd-one\",\"CAM-B\":null}");
        std::promise<void> reservation_directory_created_promise;
        auto reservation_directory_created =
            reservation_directory_created_promise.get_future();
        std::promise<void> allow_reservation_journal_promise;
        auto allow_reservation_journal =
            allow_reservation_journal_promise.get_future();
        reservation_race_config
            .after_transaction_reservation_directory_created_for_testing = [&] {
                reservation_directory_created_promise.set_value();
                allow_reservation_journal.wait();
            };
        ProductionHardwareCameraAgentBackend reservation_capture_backend(
            reservation_race_config);
        ProductionHardwareCameraAgentConfig reservation_query_config =
            reservation_race_config;
        reservation_query_config
            .after_transaction_reservation_directory_created_for_testing = {};
        ProductionHardwareCameraAgentBackend reservation_query_backend(
            reservation_query_config);
        const std::string reservation_race_id =
            "bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbc";
        HardwareCameraAgentRequest reservation_request;
        reservation_request.operation =
            HardwareCameraAgentOperation::capture_single;
        reservation_request.transaction_id = reservation_race_id;
        reservation_request.camera_alias = "CAM-A";
        reservation_request.expected_capture_profile_id =
            "approved-test-profile";
        reservation_request.expected_capture_profile_version = 1;
        reservation_request.expected_capture_profile_sha256 =
            Sha256OfText(reservation_profile);
        reservation_request.expected_capture_profile_expires_at_utc =
            "2099-12-31T23:59:59Z";
        reservation_request.exclusive_camera_control_confirmed = true;
        reservation_request.dedicated_spool_scope_confirmed = true;
        reservation_request.exact_object_delete_confirmed = true;
        auto reservation_capture = std::async(std::launch::async, [&] {
            return reservation_capture_backend.CaptureSingle(reservation_request);
        });
        reservation_directory_created.wait();
        HardwareCameraAgentDispatcher reservation_query_dispatcher(
            reservation_query_backend);
        const std::string active_reservation_response =
            reservation_query_dispatcher.Handle(Envelope(
                "get-transaction-result",
                "{\"transactionId\":\"" + reservation_race_id + "\"}"));
        Check(active_reservation_response.find(
                  "\"resultCode\":\"TransactionReserved\"") !=
                  std::string::npos &&
              active_reservation_response.find(
                  "\"terminalState\":\"Reserved\"") !=
                  std::string::npos &&
              active_reservation_response.find(
                  "transaction_reservation_incomplete") == std::string::npos,
            "live owner between reservation-directory creation and initial journal commit must remain TransactionReserved, never false terminal; response=" +
                active_reservation_response);
        allow_reservation_journal_promise.set_value();
        const auto reservation_capture_result = reservation_capture.get();
        Check(!reservation_capture_result.succeeded &&
              reservation_capture_result.error_category == "identity_map_invalid",
            "deterministic reservation-race owner should finish with its later real terminal failure");
        const auto reservation_terminal =
            reservation_query_backend.GetTransactionResult(reservation_race_id);
        Check(reservation_terminal.terminal_state == "FailedPartial" &&
              reservation_terminal.error_category == "identity_map_invalid",
            "post-owner query must return the committed terminal journal rather than reservation-incomplete");

        const std::string forged_success_id = "22222222222222222222222222222222";
        ReplaceJournal(
            config.transaction_state_root,
            forged_success_id,
            JournalJson(forged_success_id, "run-1700000000000-22", "Complete", {}, true));
        const auto forged_success = backend.GetTransactionResult(forged_success_id);
        Check(forged_success.error_category == "transaction_journal_invalid" &&
              !forged_success.succeeded,
            "Complete/succeeded journal without original and cleanup proof must fail closed");

        const std::string oversized_journal_id =
            "23232323232323232323232323232323";
        WriteText(
            config.transaction_state_root / oversized_journal_id / "transaction.json",
            std::string(64U * 1024U + 1U, 'x'));
        const auto oversized_journal =
            backend.GetTransactionResult(oversized_journal_id);
        Check(!oversized_journal.succeeded &&
              oversized_journal.terminal_state == "FailedPartial" &&
              oversized_journal.error_category == "transaction_journal_invalid",
            "oversized durable journal must fail closed before unbounded allocation or parse");

        const std::string traversal_id = "33333333333333333333333333333333";
        ReplaceJournal(
            config.transaction_state_root,
            traversal_id,
            JournalJson(traversal_id, "run-1700000000000-1/../../outside", "InProgress"));
        const auto traversal = backend.GetTransactionResult(traversal_id);
        Check(traversal.error_category == "transaction_journal_invalid",
            "journal runId traversal must reject before artifact recovery");

        const fs::path outside = root / "outside.jpg";
        const std::vector<unsigned char> jpeg = FullSizeOriginalJpeg();
        {
            std::ofstream output(outside, std::ios::binary);
            output.write(reinterpret_cast<const char*>(jpeg.data()),
                static_cast<std::streamsize>(jpeg.size()));
        }
        const std::string outside_id = "44444444444444444444444444444444";
        ReplaceJournal(
            config.transaction_state_root,
            outside_id,
            JournalJson(
                outside_id,
                "run-1700000000000-44",
                "Complete",
                {},
                true,
                outside.string(),
                jpeg.size(),
                Sha256Hex(jpeg),
                true));
        const auto outside_result = backend.GetTransactionResult(outside_id);
        Check(outside_result.error_category == "transaction_original_invalid" &&
              !outside_result.retained_original,
            "terminal original outside artifacts/runId must be dropped and fail closed");

        const std::string historical_id = "55555555555555555555555555555555";
        const std::string historical_run = "run-1700000000000-55";
        const fs::path historical_original = config.artifacts_root / historical_run /
            historical_id / "CAM-A" / "original.jpg";
        fs::create_directories(historical_original.parent_path());
        {
            std::ofstream output(historical_original, std::ios::binary);
            output.write(
                reinterpret_cast<const char*>(jpeg.data()),
                static_cast<std::streamsize>(jpeg.size()));
        }
        ReplaceJournal(
            config.transaction_state_root,
            historical_id,
            JournalJson(
                historical_id,
                historical_run,
                "Complete",
                {},
                true,
                historical_original.string(),
                jpeg.size(),
                Sha256Hex(jpeg),
                true,
                "2025-12-31T23:59:59Z"));
        const auto historical = backend.GetTransactionResult(historical_id);
        Check(historical.succeeded && historical.terminal_state == "Complete" &&
              historical.profile_expires_at_utc == "2025-12-31T23:59:59Z",
            "a profile expiring after shutter must not invalidate a durable historical Complete result");

        ProductionHardwareCameraAgentConfig isolated_recovery_config = config;
        isolated_recovery_config.approved_capture_profile =
            root / "malformed-current-profile.json";
        WriteText(
            isolated_recovery_config.approved_capture_profile,
            "{malformed-current-profile");
        isolated_recovery_config.sdk_identity_map =
            fs::path(L"\\\\untrusted-server\\share\\sdk-map.json");
        isolated_recovery_config.wpd_identity_map =
            fs::path(L"\\\\?\\C:\\device-map.json");
        ProductionHardwareCameraAgentBackend isolated_recovery_backend(
            isolated_recovery_config);
        const auto isolated_historical =
            isolated_recovery_backend.GetTransactionResult(historical_id);
        Check(isolated_historical.succeeded &&
              isolated_historical.terminal_state == "Complete" &&
              isolated_historical.retained_original,
            "historical get-result must use its durable snapshot even when current profile is malformed and current identity-map paths are nonlocal");

        const std::string recovered_failed_id = "56565656565656565656565656565656";
        const std::string recovered_failed_run = "run-1700000000000-56";
        const fs::path recovered_failed_original = config.artifacts_root /
            recovered_failed_run / recovered_failed_id / "CAM-A" / "original.jpg";
        fs::create_directories(recovered_failed_original.parent_path());
        {
            std::ofstream output(recovered_failed_original, std::ios::binary);
            output.write(
                reinterpret_cast<const char*>(jpeg.data()),
                static_cast<std::streamsize>(jpeg.size()));
        }
        ReplaceJournal(
            config.transaction_state_root,
            recovered_failed_id,
            JournalJson(
                recovered_failed_id,
                recovered_failed_run,
                "FailedPartial",
                "transaction_watchdog"));
        const auto recovered_failed =
            backend.GetTransactionResult(recovered_failed_id);
        Check(!recovered_failed.succeeded &&
              recovered_failed.terminal_state == "FailedPartial" &&
              recovered_failed.retained_original &&
              recovered_failed.retained_original->sha256 == Sha256Hex(jpeg),
            "terminal FailedPartial query must rediscover a canonical original omitted before journal commit");
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: durable journal test threw: " << error.what() << '\n';
    }
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

void TestExactlyOneBindingAndHybridExecutorReuse() {
    const fs::path root = fs::temp_directory_path() / ("a0-agent-test-" + NewRunId());
    try {
        IdentityMap sdk_map(root / "sdk-map.json");
        IdentityMap wpd_map(root / "wpd-map.json");
        sdk_map.Bind("CAM-A", "sdk-one");
        wpd_map.Bind("CAM-A", "wpd-one");
        const std::vector<CameraInfo> sdk_cameras{{"Nikon D810", "fw", "M", "sdk-one"}};
        const std::vector<CameraInfo> wpd_cameras{{"Nikon D810", "fw", "M", "wpd-one"}};
        const auto binding = ResolveExactlyOneBoundCamera(
            sdk_cameras, wpd_cameras, sdk_map, wpd_map, "CAM-A");
        Check(binding.ready, "one SDK and WPD projection with matching aliases should bind");
        const auto too_many = ResolveExactlyOneBoundCamera(
            {sdk_cameras.front(), {"Nikon D810", "fw", "M", "sdk-two"}},
            wpd_cameras, sdk_map, wpd_map, "CAM-A");
        Check(!too_many.ready && too_many.failure_category == "camera_count_mismatch",
            "two SDK bodies must fail the single-camera boundary closed");

        HardwareCameraAgentRequest request;
        request.operation = HardwareCameraAgentOperation::capture_single;
        request.transaction_id = "fedcba9876543210fedcba9876543210";
        request.camera_alias = "CAM-A";
        request.exclusive_camera_control_confirmed = true;
        request.dedicated_spool_scope_confirmed = true;
        request.exact_object_delete_confirmed = true;
        HybridWpdFake wpd;
        HybridSdkFake sdk;
        EvidenceWriter evidence(root / "artifacts", "run-executor", "fake-combined");
        const auto result = ExecuteBoundSingleCapture(
            request, sdk_cameras, wpd_cameras, sdk_map, wpd_map,
            wpd, wpd, sdk, sdk, evidence, ConfirmedLiveViewOffStatus(),
            ConfirmedLiveViewOffProbe());
        Check(result.succeeded && result.terminal_state == "Complete",
            "agent adapter should reuse the hybrid executor for one-camera capture");
        Check(result.transaction_id == request.transaction_id,
            "public transaction ID must remain the client idempotency key");
        Check(sdk.captures == 1 && wpd.delete_attempts == 1 && wpd.empty_after_checks == 1,
            "executor should issue one shutter, exact-object delete, and final empty check");
        Check(result.retained_original && fs::is_regular_file(result.retained_original->path),
            "successful capture should return the verified canonical PC original");
        Check(result.automatic_retry_count == 0,
            "agent capture must report zero automatic retries");

        HybridWpdFake wrong_dimensions_wpd;
        wrong_dimensions_wpd.observed_candidates = {{
            "wrong-size.jpg", FullSizeOriginalJpeg(1U, 1U), true, "wrong-size-object"}};
        HybridSdkFake wrong_dimensions_sdk;
        EvidenceWriter wrong_dimensions_evidence(
            root / "artifacts", "run-wrong-dimensions", "fake-combined");
        const auto wrong_dimensions = ExecuteBoundSingleCapture(
            request, sdk_cameras, wpd_cameras, sdk_map, wpd_map,
            wrong_dimensions_wpd, wrong_dimensions_wpd,
            wrong_dimensions_sdk, wrong_dimensions_sdk,
            wrong_dimensions_evidence, ConfirmedLiveViewOffStatus(),
            ConfirmedLiveViewOffProbe());
        Check(!wrong_dimensions.succeeded &&
              wrong_dimensions.error_category == "pc_original_verification_failed" &&
              wrong_dimensions.retained_original &&
              wrong_dimensions_wpd.delete_attempts == 0 &&
              wrong_dimensions_wpd.empty_after_checks == 0,
            "a non-7360x4912 SingleCamera JPEG must remain FailedPartial and must not authorize camera deletion");

        HybridWpdFake locked_original_wpd;
        HybridSdkFake locked_original_sdk;
        EvidenceWriter locked_original_evidence(
            root / "artifacts", "run-original-delete-lock", "fake-combined");
        std::optional<fs::path> locked_original_path;
        std::optional<fs::path> renamed_original_path;
        bool write_open_blocked = false;
        bool rename_blocked = false;
        const auto locked_original_result = ExecuteBoundSingleCapture(
            request, sdk_cameras, wpd_cameras, sdk_map, wpd_map,
            locked_original_wpd, locked_original_wpd,
            locked_original_sdk, locked_original_sdk,
            locked_original_evidence, ConfirmedLiveViewOffStatus(),
            ConfirmedLiveViewOffProbe(), Timeouts{}, std::nullopt, [&] {
                for (const auto& entry : fs::recursive_directory_iterator(
                         locked_original_evidence.RunRoot())) {
                    if (entry.path().filename() == "original.jpg") {
                        locked_original_path = entry.path();
                        break;
                    }
                }
                if (!locked_original_path) return;
                renamed_original_path =
                    locked_original_path->parent_path() / "tampered.jpg";
                const HANDLE writer = CreateFileW(
                    locked_original_path->c_str(),
                    GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);
                if (writer == INVALID_HANDLE_VALUE) {
                    const DWORD error = GetLastError();
                    write_open_blocked = error == ERROR_SHARING_VIOLATION ||
                        error == ERROR_ACCESS_DENIED;
                } else {
                    const std::array<unsigned char, 4> corrupt{
                        0x00U, 0x01U, 0x02U, 0x03U};
                    DWORD written = 0;
                    (void)WriteFile(
                        writer, corrupt.data(),
                        static_cast<DWORD>(corrupt.size()), &written, nullptr);
                    CloseHandle(writer);
                }
                if (!MoveFileExW(
                        locked_original_path->c_str(),
                        renamed_original_path->c_str(),
                        MOVEFILE_WRITE_THROUGH)) {
                    const DWORD error = GetLastError();
                    rename_blocked = error == ERROR_SHARING_VIOLATION ||
                        error == ERROR_ACCESS_DENIED;
                }
            });
        Check(locked_original_result.succeeded && write_open_blocked &&
              rename_blocked && locked_original_wpd.delete_attempts == 1 &&
              locked_original_path &&
              fs::is_regular_file(*locked_original_path) &&
              renamed_original_path && !fs::exists(*renamed_original_path),
            "verified canonical original must deny write/delete replacement until exact camera-object delete completes; state=" +
                locked_original_result.terminal_state + "/" +
                locked_original_result.error_category + ",writeBlocked=" +
                (write_open_blocked ? "true" : "false") + ",renameBlocked=" +
                (rename_blocked ? "true" : "false") + ",deletes=" +
                std::to_string(locked_original_wpd.delete_attempts));

        HybridWpdFake watchdog_wpd;
        HybridSdkFake watchdog_sdk;
        EvidenceWriter watchdog_evidence(root / "artifacts", "run-watchdog", "fake-combined");
        Timeouts zero_watchdog;
        zero_watchdog.transaction_watchdog = std::chrono::seconds::zero();
        const auto watchdog = ExecuteBoundSingleCapture(
            request, sdk_cameras, wpd_cameras, sdk_map, wpd_map,
            watchdog_wpd, watchdog_wpd, watchdog_sdk, watchdog_sdk,
            watchdog_evidence, ConfirmedLiveViewOffStatus(),
            ConfirmedLiveViewOffProbe(), zero_watchdog);
        Check(!watchdog.succeeded && watchdog.error_category == "transaction_watchdog" &&
              watchdog_sdk.captures == 0 && watchdog_wpd.opens == 0,
            "expired watchdog must prevent camera open and shutter without retry");

        HybridWpdFake preflight_wpd;
        HybridSdkFake preflight_sdk;
        EvidenceWriter preflight_evidence(
            root / "artifacts", "run-preflight-watchdog", "fake-combined");
        const auto preflight_watchdog = ExecuteBoundSingleCapture(
            request, sdk_cameras, wpd_cameras, sdk_map, wpd_map,
            preflight_wpd, preflight_wpd, preflight_sdk, preflight_sdk,
            preflight_evidence, ConfirmedLiveViewOffStatus(),
            ConfirmedLiveViewOffProbe(), Timeouts{},
            std::chrono::steady_clock::now());
        Check(!preflight_watchdog.succeeded &&
              preflight_watchdog.error_category == "transaction_watchdog" &&
              preflight_sdk.captures == 0 && preflight_wpd.opens == 0,
            "absolute agent deadline must include preflight and block the next transport/shutter");

        SdkCameraStatus live_view_on = ConfirmedLiveViewOffStatus();
        live_view_on.live_view_status = "on";
        HybridWpdFake live_view_on_wpd;
        HybridSdkFake live_view_on_sdk;
        EvidenceWriter live_view_on_evidence(
            root / "artifacts", "run-live-view-on-preflight", "fake-combined");
        const auto live_view_on_result = ExecuteBoundSingleCapture(
            request, sdk_cameras, wpd_cameras, sdk_map, wpd_map,
            live_view_on_wpd, live_view_on_wpd,
            live_view_on_sdk, live_view_on_sdk,
            live_view_on_evidence, live_view_on,
            ConfirmedLiveViewOffProbe());
        Check(!live_view_on_result.succeeded &&
              live_view_on_result.error_category == "live_view_not_off" &&
              live_view_on_wpd.opens == 0 && live_view_on_sdk.opens == 0 &&
              live_view_on_sdk.captures == 0,
            "Live View ON must block before WPD, SDK capture-session open, and shutter");

        SdkCameraStatus live_view_unavailable;
        HybridWpdFake live_view_unavailable_wpd;
        HybridSdkFake live_view_unavailable_sdk;
        EvidenceWriter live_view_unavailable_evidence(
            root / "artifacts", "run-live-view-unavailable-preflight", "fake-combined");
        const auto live_view_unavailable_result = ExecuteBoundSingleCapture(
            request, sdk_cameras, wpd_cameras, sdk_map, wpd_map,
            live_view_unavailable_wpd, live_view_unavailable_wpd,
            live_view_unavailable_sdk, live_view_unavailable_sdk,
            live_view_unavailable_evidence, live_view_unavailable,
            ConfirmedLiveViewOffProbe());
        Check(!live_view_unavailable_result.succeeded &&
              live_view_unavailable_result.error_category ==
                  "live_view_status_unavailable" &&
              live_view_unavailable_wpd.opens == 0 &&
              live_view_unavailable_sdk.opens == 0 &&
              live_view_unavailable_sdk.captures == 0,
            "unavailable Live View status must block before WPD, SDK capture-session open, and shutter");

        HybridWpdFake changed_settings_wpd;
        HybridSdkFake changed_settings_sdk;
        EvidenceWriter changed_settings_evidence(
            root / "artifacts", "run-shutter-profile-gate", "fake-combined");
        bool same_session_gate_called = false;
        const auto changed_settings = ExecuteBoundSingleCapture(
            request, sdk_cameras, wpd_cameras, sdk_map, wpd_map,
            changed_settings_wpd, changed_settings_wpd,
            changed_settings_sdk, changed_settings_sdk,
            changed_settings_evidence, ConfirmedLiveViewOffStatus(),
            ConfirmedLiveViewOffProbe(), Timeouts{}, std::nullopt, {},
            [&](const SdkCameraStatus&) {
                same_session_gate_called = true;
                Check(changed_settings_sdk.open,
                    "profile gate must run inside the already-open SDK capture session");
                throw TransportError(
                    "capture_settings_mismatch",
                    "deterministic setting change immediately before shutter");
            });
        Check(same_session_gate_called && !changed_settings.succeeded &&
              changed_settings.error_category == "capture_settings_mismatch" &&
              changed_settings_sdk.captures == 0 && changed_settings_sdk.closes == 1,
            "same-session profile mismatch must close SDK and send zero shutter commands");

        HybridWpdFake final_live_view_on_wpd;
        HybridSdkFake final_live_view_on_sdk;
        EvidenceWriter final_live_view_on_evidence(
            root / "artifacts", "run-final-live-view-on", "fake-combined");
        const auto final_live_view_on = ExecuteBoundSingleCapture(
            request, sdk_cameras, wpd_cameras, sdk_map, wpd_map,
            final_live_view_on_wpd, final_live_view_on_wpd,
            final_live_view_on_sdk, final_live_view_on_sdk,
            final_live_view_on_evidence, ConfirmedLiveViewOffStatus(),
            [] {
                SdkCameraStatus status = ConfirmedLiveViewOffStatus();
                status.live_view_status = "on";
                return status;
            });
        Check(!final_live_view_on.succeeded &&
              final_live_view_on.error_category == "live_view_not_off" &&
              final_live_view_on_sdk.captures == 0 &&
              final_live_view_on_sdk.closes == 1 &&
              final_live_view_on_wpd.delete_attempts == 0,
            "Live View ON in the open SDK capture session must close safely with zero shutter and delete");

        HybridWpdFake final_live_view_unavailable_wpd;
        HybridSdkFake final_live_view_unavailable_sdk;
        EvidenceWriter final_live_view_unavailable_evidence(
            root / "artifacts", "run-final-live-view-unavailable", "fake-combined");
        const auto final_live_view_unavailable = ExecuteBoundSingleCapture(
            request, sdk_cameras, wpd_cameras, sdk_map, wpd_map,
            final_live_view_unavailable_wpd, final_live_view_unavailable_wpd,
            final_live_view_unavailable_sdk, final_live_view_unavailable_sdk,
            final_live_view_unavailable_evidence, ConfirmedLiveViewOffStatus(),
            [] { return SdkCameraStatus{}; });
        Check(!final_live_view_unavailable.succeeded &&
              final_live_view_unavailable.error_category ==
                  "live_view_status_unavailable" &&
              final_live_view_unavailable_sdk.captures == 0 &&
              final_live_view_unavailable_sdk.closes == 1 &&
              final_live_view_unavailable_wpd.delete_attempts == 0,
            "unavailable Live View status in the open SDK capture session must close safely with zero shutter and delete");

        const fs::path escaped_quarantine = root / "escaped-quarantine";
        fs::create_directories(escaped_quarantine);
        EvidenceWriter reparse_evidence(
            root / "artifacts", "run-quarantine-reparse", "fake-combined");
        const fs::path quarantine_link = reparse_evidence.RunRoot() / "quarantine";
        const bool symlink_created = CreateSymbolicLinkW(
            quarantine_link.c_str(),
            escaped_quarantine.c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2U) != FALSE;
        if (symlink_created) {
            HybridWpdFake reparse_wpd;
            reparse_wpd.observed_candidates = {
                {"one.jpg", {0xFF, 0xD8, 0x01, 0xFF, 0xD9}, true, "one"},
                {"two.jpg", {0xFF, 0xD8, 0x02, 0xFF, 0xD9}, true, "two"},
            };
            HybridSdkFake reparse_sdk;
            const auto reparse_result = ExecuteBoundSingleCapture(
                request, sdk_cameras, wpd_cameras, sdk_map, wpd_map,
                reparse_wpd, reparse_wpd, reparse_sdk, reparse_sdk,
                reparse_evidence, ConfirmedLiveViewOffStatus(),
                ConfirmedLiveViewOffProbe());
            Check(!reparse_result.succeeded && reparse_wpd.delete_attempts == 0 &&
                  fs::is_empty(escaped_quarantine),
                "reparse-point quarantine must cause zero escaped bytes and zero camera-object delete");
        }
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: agent executor test threw: " << error.what() << '\n';
    }
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

void TestProfileSnapshotAndStrictIdentityMapGates() {
    const fs::path root =
        fs::temp_directory_path() / ("a0-agent-profile-test-" + NewRunId());
    try {
        const std::string profile_json = ProfileJson();
        const std::string profile_sha256 = Sha256OfText(profile_json);
        ProductionHardwareCameraAgentConfig config;
        config.artifacts_root = root / "artifacts";
        config.reports_root = root / "reports";
        config.sdk_identity_map = root / "sdk-map.json";
        config.wpd_identity_map = root / "wpd-map.json";
        config.transaction_state_root = root / "transactions";
        config.approved_capture_profile = root / "approved-profile.json";
        WriteText(config.approved_capture_profile, profile_json);

        HardwareCameraAgentRequest request;
        request.operation = HardwareCameraAgentOperation::capture_single;
        request.transaction_id = "66666666666666666666666666666666";
        request.camera_alias = "CAM-A";
        request.expected_capture_profile_id = "approved-test-profile";
        request.expected_capture_profile_version = 1;
        request.expected_capture_profile_sha256 = std::string(64, 'd');
        request.expected_capture_profile_expires_at_utc = "2099-12-31T23:59:59Z";
        request.exclusive_camera_control_confirmed = true;
        request.dedicated_spool_scope_confirmed = true;
        request.exact_object_delete_confirmed = true;

        ProductionHardwareCameraAgentBackend snapshot_backend(config);
        const auto mismatch = snapshot_backend.CaptureSingle(request);
        Check(!mismatch.succeeded && mismatch.terminal_state == "Blocked" &&
              mismatch.error_category == "capture_profile_snapshot_mismatch" &&
              !fs::exists(config.artifacts_root / mismatch.run_id),
            "readiness profile snapshot mismatch must become durable Blocked before camera or artifact access");

        request.transaction_id = "67676767676767676767676767676767";
        request.expected_capture_profile_sha256 = profile_sha256;
        request.expected_capture_profile_expires_at_utc = "2098-12-31T23:59:59Z";
        const auto expiry_mismatch = snapshot_backend.CaptureSingle(request);
        Check(!expiry_mismatch.succeeded &&
              expiry_mismatch.terminal_state == "Blocked" &&
              expiry_mismatch.error_category == "capture_profile_snapshot_mismatch" &&
              !fs::exists(config.artifacts_root / expiry_mismatch.run_id),
            "readiness profile expiry mismatch must become durable Blocked before camera or artifact access");

        const fs::path expired_root = root / "expired";
        ProductionHardwareCameraAgentConfig expired_config = config;
        expired_config.artifacts_root = expired_root / "artifacts";
        expired_config.reports_root = expired_root / "reports";
        expired_config.sdk_identity_map = expired_root / "sdk-map.json";
        expired_config.wpd_identity_map = expired_root / "wpd-map.json";
        expired_config.transaction_state_root = expired_root / "transactions";
        expired_config.approved_capture_profile = expired_root / "approved-profile.json";
        const std::string expired_json =
            ProfileJson("approved-expired-profile", 7, "CAM-A", "2025-12-31T23:59:59Z");
        WriteText(expired_config.approved_capture_profile, expired_json);
        ProductionHardwareCameraAgentBackend expired_backend(expired_config);
        request.transaction_id = "77777777777777777777777777777777";
        request.expected_capture_profile_id = "approved-expired-profile";
        request.expected_capture_profile_version = 7;
        request.expected_capture_profile_sha256 = Sha256OfText(expired_json);
        request.expected_capture_profile_expires_at_utc = "2025-12-31T23:59:59Z";
        const auto expired = expired_backend.CaptureSingle(request);
        Check(!expired.succeeded && expired.terminal_state == "Blocked" &&
              expired.error_category == "capture_profile_expired",
            "expired approved profile must block before identity-map or camera access");

        request.expected_capture_profile_id = "approved-test-profile";
        request.expected_capture_profile_version = 1;
        request.expected_capture_profile_sha256 = profile_sha256;
        request.expected_capture_profile_expires_at_utc = "2099-12-31T23:59:59Z";
        WriteText(config.wpd_identity_map, "{\"CAM-A\":\"wpd-one\",\"CAM-B\":null}");
        const std::vector<std::string> malformed_maps{
            "{\"CAM-A\":\"sdk-one\",\"CAM-A\":\"sdk-two\",\"CAM-B\":null}",
            "{\"CAM-A\":\"same\",\"CAM-B\":\"same\"}",
            "{\"CAM-A\":\"sdk-one\",\"CAM-B\":null} trailing",
            "{\"CAM-A\":\"sdk-one\",\"CAM-B\":null,\"CAM-C\":\"sdk-three\"}",
        };
        const std::vector<std::string> transaction_ids{
            "88888888888888888888888888888888",
            "99999999999999999999999999999999",
            "abababababababababababababababab",
            "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
        };
        for (std::size_t index = 0; index < malformed_maps.size(); ++index) {
            WriteText(config.sdk_identity_map, malformed_maps[index]);
            request.transaction_id = transaction_ids[index];
            const auto invalid_map = snapshot_backend.CaptureSingle(request);
            Check(!invalid_map.succeeded &&
                  invalid_map.error_category == "identity_map_invalid",
                "malformed, duplicate, trailing, or non-unique product identity map must fail before camera access");
        }

        ProductionHardwareCameraAgentConfig directory_profile_config = config;
        directory_profile_config.approved_capture_profile = root / "profile-directory";
        fs::create_directories(directory_profile_config.approved_capture_profile);
        ProductionHardwareCameraAgentBackend directory_profile_backend(
            directory_profile_config);
        const auto directory_profile_readiness =
            directory_profile_backend.GetSingleReadiness("CAM-A");
        Check(!directory_profile_readiness.ready &&
              directory_profile_readiness.failure_category ==
                  "capture_profile_invalid",
            "approved capture profile path must lazily reject a directory before camera access");

        ProductionHardwareCameraAgentConfig oversized_profile_config = config;
        oversized_profile_config.approved_capture_profile = root / "oversized-profile.json";
        WriteText(
            oversized_profile_config.approved_capture_profile,
            std::string(64U * 1024U + 1U, 'x'));
        ProductionHardwareCameraAgentBackend oversized_profile_backend(
            oversized_profile_config);
        request.transaction_id = "dededededededededededededededede";
        const auto oversized_profile =
            oversized_profile_backend.CaptureSingle(request);
        Check(!oversized_profile.succeeded &&
              oversized_profile.terminal_state == "Blocked" &&
              oversized_profile.error_category == "capture_profile_invalid" &&
              !fs::exists(
                  oversized_profile_config.artifacts_root /
                  oversized_profile.run_id),
            "oversized approved profile must become durable Blocked before camera or artifact access");
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: profile/identity-map gate test threw: " << error.what() << '\n';
    }
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

void TestFixedLocalPathPolicy() {
    const fs::path root =
        fs::temp_directory_path() / ("a0-agent-path-policy-test-" + NewRunId());
    try {
        ProductionHardwareCameraAgentConfig base;
        base.artifacts_root = root / "artifacts";
        base.reports_root = root / "reports";
        base.sdk_identity_map = root / "sdk-map.json";
        base.wpd_identity_map = root / "wpd-map.json";
        base.transaction_state_root = root / "transactions";
        base.approved_capture_profile = root / "approved-profile.json";
        const std::string approved_profile_json = ProfileJson();
        WriteText(base.approved_capture_profile, approved_profile_json);

        const auto constructor_rejects = [](ProductionHardwareCameraAgentConfig config) {
            try {
                ProductionHardwareCameraAgentBackend backend(std::move(config));
                (void)backend;
                return false;
            } catch (const std::invalid_argument&) {
                return true;
            }
        };

        ProductionHardwareCameraAgentConfig unc_root = base;
        unc_root.artifacts_root =
            fs::path(L"\\\\untrusted-server\\share\\artifacts");
        Check(constructor_rejects(unc_root),
            "UNC artifact root must be rejected before any operation");

        ProductionHardwareCameraAgentConfig device_root = base;
        device_root.reports_root = fs::path(L"\\\\?\\C:\\agent-reports");
        Check(constructor_rejects(device_root),
            "Windows device-path report root must be rejected before any operation");

        ProductionHardwareCameraAgentConfig ads_root = base;
        ads_root.transaction_state_root = fs::path(
            (root / "transactions").native() + std::wstring(L":journal-stream"));
        Check(constructor_rejects(ads_root),
            "ADS transaction-state root must be rejected before any operation");

        ProductionHardwareCameraAgentConfig relative_root = base;
        relative_root.artifacts_root = fs::path(L"relative-artifacts");
        Check(constructor_rejects(relative_root),
            "relative durable root must be rejected before any operation");

        std::optional<wchar_t> non_fixed_drive;
        for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
            const std::wstring drive_root{letter, L':', L'\\'};
            if (GetDriveTypeW(drive_root.c_str()) != DRIVE_FIXED) {
                non_fixed_drive = letter;
                break;
            }
        }
        Check(non_fixed_drive.has_value(),
            "path-policy test requires at least one non-fixed or unavailable drive letter");
        if (non_fixed_drive) {
            const std::wstring drive_root{*non_fixed_drive, L':', L'\\'};
            ProductionHardwareCameraAgentConfig non_fixed_root = base;
            non_fixed_root.reports_root =
                fs::path(drive_root) / "A0CameraStitcher" / "reports";
            Check(constructor_rejects(non_fixed_root),
                "mapped, removable, optical, RAM, and unavailable drives must not host durable roots");
        }

        const std::wstring device_profile =
            L"\\\\?\\" + (root / "approved-profile.json").native();
        std::vector<fs::path> invalid_profile_paths{
            fs::path(L"\\\\untrusted-server\\share\\approved-profile.json"),
            fs::path(device_profile),
            fs::path(
                (root / "approved-profile.json").native() +
                std::wstring(L":policy-stream")),
            fs::path(L"relative-approved-profile.json"),
        };
        if (non_fixed_drive) {
            const std::wstring drive_root{*non_fixed_drive, L':', L'\\'};
            invalid_profile_paths.emplace_back(
                fs::path(drive_root) / "approved-profile.json");
        }
        for (const fs::path& invalid_path : invalid_profile_paths) {
            ProductionHardwareCameraAgentConfig profile_config = base;
            profile_config.approved_capture_profile = invalid_path;
            ProductionHardwareCameraAgentBackend backend(profile_config);
            const auto readiness = backend.GetSingleReadiness("CAM-A");
            Check(!readiness.ready &&
                  readiness.failure_category == "capture_profile_invalid",
                "nonlocal, device, ADS, relative, or non-fixed profile path must lazily fail before identity-map or camera access");
        }

        const std::wstring device_map =
            L"\\\\?\\" + (root / "sdk-map.json").native();
        std::vector<fs::path> invalid_identity_paths{
            fs::path(L"\\\\untrusted-server\\share\\sdk-map.json"),
            fs::path(device_map),
            fs::path(
                (root / "sdk-map.json").native() +
                std::wstring(L":identity-stream")),
            fs::path(L"relative-sdk-map.json"),
        };
        if (non_fixed_drive) {
            const std::wstring drive_root{*non_fixed_drive, L':', L'\\'};
            invalid_identity_paths.emplace_back(
                fs::path(drive_root) / "sdk-map.json");
        }
        HardwareCameraAgentRequest live_view_request;
        live_view_request.operation = HardwareCameraAgentOperation::live_view_probe;
        live_view_request.camera_alias = "CAM-A";
        live_view_request.exclusive_camera_control_confirmed = true;
        live_view_request.live_view_frames = 1;
        live_view_request.live_view_interval_ms = 0;
        const std::vector<std::string> capture_transaction_ids{
            "10101010101010101010101010101010",
            "20202020202020202020202020202020",
            "30303030303030303030303030303030",
            "40404040404040404040404040404040",
            "50505050505050505050505050505050",
        };
        for (std::size_t index = 0; index < invalid_identity_paths.size(); ++index) {
            const fs::path& invalid_path = invalid_identity_paths[index];
            ProductionHardwareCameraAgentConfig identity_config = base;
            identity_config.sdk_identity_map = invalid_path;
            ProductionHardwareCameraAgentBackend backend(identity_config);
            const auto readiness = backend.GetSingleReadiness("CAM-A");
            Check(!readiness.ready &&
                  readiness.failure_category == "identity_map_invalid",
                "readiness must reject a nonlocal, device, ADS, relative, or non-fixed identity map before camera access");
            const auto probe = backend.ProbeLiveView(live_view_request);
            Check(!probe.succeeded &&
                  probe.error_category == "identity_map_invalid" &&
                  !fs::exists(identity_config.artifacts_root / probe.run_id),
                "nonlocal, device, ADS, relative, or non-fixed identity-map path must fail before artifact or camera access");

            HardwareCameraAgentRequest capture_request;
            capture_request.operation =
                HardwareCameraAgentOperation::capture_single;
            capture_request.transaction_id = capture_transaction_ids[index];
            capture_request.camera_alias = "CAM-A";
            capture_request.expected_capture_profile_id =
                "approved-test-profile";
            capture_request.expected_capture_profile_version = 1;
            capture_request.expected_capture_profile_sha256 =
                Sha256OfText(approved_profile_json);
            capture_request.expected_capture_profile_expires_at_utc =
                "2099-12-31T23:59:59Z";
            capture_request.exclusive_camera_control_confirmed = true;
            capture_request.dedicated_spool_scope_confirmed = true;
            capture_request.exact_object_delete_confirmed = true;
            const auto capture = backend.CaptureSingle(capture_request);
            Check(!capture.succeeded &&
                  capture.error_category == "identity_map_invalid" &&
                  !fs::exists(identity_config.artifacts_root / capture.run_id),
                "capture must durably fail a nonlocal, device, ADS, relative, or non-fixed identity map before artifact or camera access");
        }

        WriteText(
            base.sdk_identity_map,
            "{\"CAM-A\":\"same-map-camera\",\"CAM-B\":null}");
        ProductionHardwareCameraAgentConfig same_map_config = base;
        same_map_config.wpd_identity_map = same_map_config.sdk_identity_map;
        ProductionHardwareCameraAgentBackend same_map_backend(same_map_config);
        const auto same_map_readiness =
            same_map_backend.GetSingleReadiness("CAM-A");
        Check(!same_map_readiness.ready &&
              same_map_readiness.failure_category == "identity_map_invalid",
            "SDK and WPD identity maps must remain distinct even though validation is operation-lazy");
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: fixed-local path policy test threw: "
                  << error.what() << '\n';
    }
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

bool WriteAll(HANDLE pipe, const void* source, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(source);
    std::size_t offset = 0;
    while (offset < size) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            size - offset, static_cast<std::size_t>(64U * 1024U)));
        if (!WriteFile(pipe, bytes + offset, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool ReadAll(HANDLE pipe, void* destination, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(destination);
    std::size_t offset = 0;
    while (offset < size) {
        DWORD read = 0;
        if (!ReadFile(
                pipe,
                bytes + offset,
                static_cast<DWORD>(size - offset),
                &read,
                nullptr) || read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

void TestNamedPipeMaximumFrameBoundary() {
    constexpr std::uint32_t maximum = 1024U * 1024U;
    for (const std::uint32_t length : {maximum - 1U, maximum, maximum + 1U}) {
        FakeBackend backend;
        HardwareCameraAgentDispatcher dispatcher(backend);
        const std::string pipe_name =
            "A0CameraStitcher.CameraAgent.Hardware.v1.boundary-" + NewRunId();
        const std::wstring full_pipe_name =
            L"\\\\.\\pipe\\" + std::wstring(pipe_name.begin(), pipe_name.end());
        auto server = std::async(std::launch::async, [&] {
            return RunHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
        });

        HANDLE pipe = INVALID_HANDLE_VALUE;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            pipe = CreateFileW(
                full_pipe_name.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(pipe != INVALID_HANDLE_VALUE,
            "named-pipe frame boundary client must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const std::array<unsigned char, 4> header{
                static_cast<unsigned char>(length & 0xFFU),
                static_cast<unsigned char>((length >> 8U) & 0xFFU),
                static_cast<unsigned char>((length >> 16U) & 0xFFU),
                static_cast<unsigned char>((length >> 24U) & 0xFFU),
            };
            Check(WriteAll(pipe, header.data(), header.size()),
                "named-pipe frame boundary header must be writable");
            if (length <= maximum) {
                const std::string body(length, ' ');
                Check(WriteAll(pipe, body.data(), body.size()),
                    "limit-1 and limit named-pipe request bodies must be accepted");
                std::array<unsigned char, 4> response_header{};
                Check(ReadAll(pipe, response_header.data(), response_header.size()),
                    "limit-1 and limit named-pipe frames must receive a rejection envelope");
                const std::uint32_t response_length =
                    static_cast<std::uint32_t>(response_header[0]) |
                    (static_cast<std::uint32_t>(response_header[1]) << 8U) |
                    (static_cast<std::uint32_t>(response_header[2]) << 16U) |
                    (static_cast<std::uint32_t>(response_header[3]) << 24U);
                Check(response_length > 0 && response_length <= maximum,
                    "named-pipe response header must describe a bounded body");
                Check(server.wait_for(std::chrono::milliseconds(50)) ==
                        std::future_status::timeout,
                    "server must not disconnect while the response body remains unread");
                std::string response(response_length, '\0');
                Check(ReadAll(pipe, response.data(), response.size()),
                    "limit-1 and limit named-pipe frames must receive a complete response body");
            }
            CloseHandle(pipe);
        }
        Check(server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
            "named-pipe frame boundary server must terminate");
        const int exit_code = server.get();
        Check(exit_code == (length <= maximum ? 0 : 2),
            "named-pipe frame limit must accept limit-1/limit and reject limit+1");
        Check(backend.readiness_calls == 0 && backend.capture_calls == 0 &&
              backend.live_view_calls == 0 && backend.transaction_calls == 0,
            "oversized or parser-rejected pipe frames must not dispatch hardware operations");
    }
}

void TestNamedPipeDeliveryFailuresExitNonzeroWithoutRedispatch() {
    const std::array<HardwareCameraAgentPipeFailureInjectionForTesting, 3>
        failures_to_inject{{
        {.fail_response_header_write = true},
        {.fail_response_body_write = true},
        {.fail_response_flush = true},
    }};
    for (std::size_t index = 0; index < failures_to_inject.size(); ++index) {
        FakeBackend backend;
        HardwareCameraAgentDispatcher dispatcher(backend);
        const std::string pipe_name =
            "A0CameraStitcher.CameraAgent.Hardware.v1.delivery-" + NewRunId();
        const std::wstring full_pipe_name =
            L"\\\\.\\pipe\\" + std::wstring(pipe_name.begin(), pipe_name.end());
        auto server = std::async(std::launch::async, [&] {
            return RunHardwareCameraAgentNamedPipeServer(
                pipe_name, dispatcher, false, failures_to_inject[index]);
        });

        HANDLE pipe = INVALID_HANDLE_VALUE;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            pipe = CreateFileW(
                full_pipe_name.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(pipe != INVALID_HANDLE_VALUE,
            "delivery-failure injection client must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const std::string request = Envelope(
                "get-single-readiness", "{\"cameraAlias\":\"CAM-A\"}");
            const std::uint32_t request_length =
                static_cast<std::uint32_t>(request.size());
            const std::array<unsigned char, 4> header{
                static_cast<unsigned char>(request_length & 0xFFU),
                static_cast<unsigned char>((request_length >> 8U) & 0xFFU),
                static_cast<unsigned char>((request_length >> 16U) & 0xFFU),
                static_cast<unsigned char>((request_length >> 24U) & 0xFFU),
            };
            Check(WriteAll(pipe, header.data(), header.size()) &&
                    WriteAll(pipe, request.data(), request.size()),
                "delivery-failure request must be writable");

            std::array<unsigned char, 4> response_header{};
            const bool header_received =
                ReadAll(pipe, response_header.data(), response_header.size());
            if (failures_to_inject[index].fail_response_header_write) {
                Check(!header_received,
                    "injected header failure must not deliver a response header");
            } else {
                Check(header_received,
                    "body and flush failure injection must deliver the response header");
                if (header_received) {
                    const std::uint32_t response_length =
                        static_cast<std::uint32_t>(response_header[0]) |
                        (static_cast<std::uint32_t>(response_header[1]) << 8U) |
                        (static_cast<std::uint32_t>(response_header[2]) << 16U) |
                        (static_cast<std::uint32_t>(response_header[3]) << 24U);
                    std::string response(response_length, '\0');
                    const bool body_received =
                        ReadAll(pipe, response.data(), response.size());
                    if (failures_to_inject[index].fail_response_body_write) {
                        Check(!body_received,
                            "injected body failure must truncate the response body");
                    }
                }
            }
            CloseHandle(pipe);
        }

        Check(server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
            "delivery-failure server must terminate");
        Check(server.get() == 3,
            "dispatch-complete delivery failure must return the typed nonzero exit code");
        Check(backend.readiness_calls == 1,
            "delivery failure must not retry or redispatch readiness");
    }
}

std::string LiveViewV2Envelope(
    std::string_view operation,
    std::string_view payload) {
    return "{\"schemaVersion\":\"a0.camera-agent.hardware.v2\","
           "\"simulation\":false,\"marker\":\"Hardware\",\"requestId\":\"req-v2\","
           "\"operation\":\"" + std::string(operation) + "\",\"payload\":" +
        std::string(payload) + "}";
}

struct FakeContinuousLiveViewSdkState {
    std::vector<unsigned char> frame{0xFFU, 0xD8U, 0xFFU, 0xD9U};
    std::function<void()> before_read;
    std::atomic<int> enumerate_calls{};
    std::atomic<int> probe_calls{};
    std::atomic<int> open_calls{};
    std::atomic<int> start_calls{};
    std::atomic<int> read_calls{};
    std::atomic<int> stop_calls{};
    std::atomic<int> close_calls{};
    bool fail_stop{};
    bool fail_close{};
};

class FakeContinuousLiveViewSdkTransport final
    : public IContinuousLiveViewSdkTransport {
public:
    explicit FakeContinuousLiveViewSdkTransport(
        std::shared_ptr<FakeContinuousLiveViewSdkState> state)
        : state_(std::move(state)) {}

    std::vector<CameraInfo> Enumerate() override {
        ++state_->enumerate_calls;
        return {{"Nikon D810", "test", "test", "fake-sdk-identity"}};
    }

    SdkCameraStatus ProbeSdkStatus(
        std::string_view,
        std::chrono::seconds) override {
        ++state_->probe_calls;
        return ConfirmedLiveViewOffStatus();
    }

    void OpenLiveView(std::string_view, std::chrono::seconds) override {
        ++state_->open_calls;
    }

    void StartLiveView(std::chrono::seconds) override {
        ++state_->start_calls;
    }

    std::vector<unsigned char> ReadLiveViewFrame(
        std::chrono::seconds) override {
        ++state_->read_calls;
        if (state_->before_read) state_->before_read();
        return state_->frame;
    }

    void StopLiveView(std::chrono::seconds) override {
        ++state_->stop_calls;
        if (state_->fail_stop) throw std::runtime_error("injected stop failure");
    }

    void Close(std::chrono::seconds) override {
        ++state_->close_calls;
        if (state_->fail_close) throw std::runtime_error("injected close failure");
    }

private:
    std::shared_ptr<FakeContinuousLiveViewSdkState> state_;
};

ProductionHardwareCameraAgentConfig ContinuousLiveViewTestConfig(
    const fs::path& root,
    const std::shared_ptr<FakeContinuousLiveViewSdkState>& state,
    std::function<std::chrono::steady_clock::time_point()> clock = {}) {
    ProductionHardwareCameraAgentConfig config;
    config.artifacts_root = root / "artifacts";
    config.reports_root = root / "reports";
    config.transaction_state_root = root / "transactions";
    config.continuous_live_view_sdk_factory_for_testing = [state] {
        return std::make_unique<FakeContinuousLiveViewSdkTransport>(state);
    };
    config.continuous_live_view_identity_resolver_for_testing = [](
        std::string_view alias,
        const std::vector<CameraInfo>& cameras) {
        if (alias != "CAM-A" || cameras.size() != 1) {
            throw TransportError(
                "single_identity_not_ready", "injected identity must remain exact-one CAM-A");
        }
        return cameras.front().stable_identity;
    };
    config.continuous_live_view_clock_for_testing = std::move(clock);
    return config;
}

HardwareCameraAgentRequest ContinuousRequest(
    HardwareCameraAgentOperation operation,
    std::string session_id) {
    HardwareCameraAgentRequest request;
    request.schema_version = std::string(kHardwareCameraAgentLiveViewSchemaVersion);
    request.operation = operation;
    request.camera_alias = "CAM-A";
    request.session_id = std::move(session_id);
    request.exclusive_camera_control_confirmed = true;
    return request;
}

std::string DualIdentityProofJson(
    std::string_view alias,
    char sdk_digest,
    char wpd_digest,
    std::string_view provider_id = "documented-test-provider",
    std::uint32_t provider_version = 1,
    bool single_camera_confirmed = true,
    bool documented_correlation_confirmed = true,
    std::string_view expires_at_utc = "2026-09-01T00:00:00Z",
    std::string_view created_at_utc = "2026-08-01T00:00:00Z") {
    DualIdentityBindingProof proof;
    proof.camera_alias = std::string(alias);
    proof.provider_id = std::string(provider_id);
    proof.provider_version = provider_version;
    proof.sdk_identity_sha256 = std::string(64, sdk_digest);
    proof.wpd_identity_sha256 = std::string(64, wpd_digest);
    proof.created_at_utc = std::string(created_at_utc);
    proof.expires_at_utc = std::string(expires_at_utc);
    proof.single_camera_connected_confirmed = single_camera_confirmed;
    proof.documented_correlation_confirmed = documented_correlation_confirmed;
    proof.proof_payload_sha256 =
        ComputeDualIdentityBindingProofPayloadSha256(proof);
    return SerializeDualIdentityBindingProof(proof);
}

void CheckDualIdentitySafetyZero(
    const DualIdentitySafetyState& safety,
    std::string_view context) {
    Check(safety.inventory_read_only &&
              !safety.capture_command_sent &&
              !safety.card_access_performed &&
              !safety.live_view_started &&
              !safety.camera_settings_changed &&
              !safety.camera_object_delete_attempted &&
              !safety.card_format_attempted &&
              !safety.vendor_operation_executed &&
              safety.automatic_retry_count == 0,
        std::string(context));
}

void TestDualIdentityProofContractIsStrictAndFailClosed() {
    const fs::path root = fs::temp_directory_path() /
        ("a0-dual-identity-proof-test-" + NewRunId());
    const fs::path cam_a_path = root / "cam-a.json";
    const fs::path cam_b_path = root / "cam-b.json";
    WriteText(cam_a_path, DualIdentityProofJson("CAM-A", 'a', 'c'));
    WriteText(cam_b_path, DualIdentityProofJson("CAM-B", 'b', 'd'));

    const DualIdentityCorrelationProvider provider{
        "documented-test-provider", 1, true};
    const std::vector<DualIdentityInventoryProjection> sdk{
        {DualIdentityTransport::sdk, "Nikon D810", std::string(64, 'a'),
            "documented-test-provider", 1},
        {DualIdentityTransport::sdk, "Nikon D810", std::string(64, 'b'),
            "documented-test-provider", 1}};
    const std::vector<DualIdentityInventoryProjection> wpd{
        {DualIdentityTransport::wpd, "Nikon D810", std::string(64, 'c'),
            "documented-test-provider", 1},
        {DualIdentityTransport::wpd, "Nikon D810", std::string(64, 'd'),
            "documented-test-provider", 1}};

    const auto ready = VerifyDualIdentitySoftwareContract(
        provider, {cam_a_path, cam_b_path}, sdk, wpd,
        "2026-08-11T00:00:00Z", false);
    Check(std::holds_alternative<DualIdentityReady>(ready),
        "documented anonymous CAM-A/B proofs and exact read-only inventories should produce typed readiness");
    const auto& ready_value = std::get<DualIdentityReady>(ready);
    Check(ready_value.safety.inventory_read_only &&
              ready_value.sdk_cam_a_count == 1 && ready_value.sdk_cam_b_count == 1 &&
              ready_value.wpd_cam_a_count == 1 && ready_value.wpd_cam_b_count == 1 &&
              ready_value.sdk_unbound_count == 0 && ready_value.wpd_unbound_count == 0 &&
              !ready_value.safety.capture_command_sent &&
              !ready_value.safety.card_access_performed &&
              !ready_value.safety.live_view_started &&
              !ready_value.safety.camera_settings_changed &&
              !ready_value.safety.camera_object_delete_attempted &&
              !ready_value.safety.card_format_attempted &&
              !ready_value.safety.vendor_operation_executed &&
              ready_value.safety.automatic_retry_count == 0,
        "dual identity readiness must remain read-only with every camera mutation counter at zero");

    const auto block_reason = [](const DualIdentityResult& result) {
        Check(std::holds_alternative<DualIdentityBlocked>(result),
            "negative dual identity case must return typed Block");
        const auto& blocked = std::get<DualIdentityBlocked>(result);
        CheckDualIdentitySafetyZero(blocked.safety,
            "every typed dual identity Block must stop with read-only inventory and zero mutations/retries");
        return blocked.reason;
    };
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              std::nullopt, {cam_a_path, cam_b_path}, sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::identity_strategy_unresolved,
        "missing documented provider must keep DualCamera identity unresolved");
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, cam_b_path}, sdk, wpd,
              "2026-08-11T00:00:00Z", true)) ==
              DualIdentityBlockReason::legacy_map_fallback_prohibited,
        "legacy identity maps must never be accepted as a fallback");
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path}, sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::proof_count_mismatch,
        "zero or missing alias proof must block");

    WriteText(root / "collision.json",
        DualIdentityProofJson("CAM-B", 'a', 'd'));
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, root / "collision.json"}, sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::identity_collision,
        "same anonymous projection bound to both aliases must block as a collision");

    WriteText(root / "duplicate-alias.json",
        DualIdentityProofJson("CAM-A", 'b', 'd'));
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, root / "duplicate-alias.json"}, sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::alias_cardinality_mismatch,
        "CAM-A/B must each occur exactly once in the proof set");

    WriteText(root / "provider-mismatch.json",
        DualIdentityProofJson("CAM-B", 'b', 'd', "different-provider"));
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, root / "provider-mismatch.json"}, sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::provider_mismatch,
        "proof provider and version must match the documented provider seam");

    WriteText(root / "stale.json",
        DualIdentityProofJson("CAM-B", 'b', 'd',
            "documented-test-provider", 1, true, true,
            "2026-08-10T00:00:00Z"));
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, root / "stale.json"}, sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::proof_stale,
        "expired proof must block before any camera control");

    WriteText(root / "future.json",
        DualIdentityProofJson("CAM-B", 'b', 'd',
            "documented-test-provider", 1, true, true,
            "2026-09-01T00:00:00Z", "2026-08-12T00:00:00Z"));
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, root / "future.json"}, sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::proof_stale,
        "a proof created after the inventory observation must block");

    WriteText(root / "unconfirmed.json",
        DualIdentityProofJson("CAM-B", 'b', 'd',
            "documented-test-provider", 1, false));
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, root / "unconfirmed.json"}, sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::confirmation_mismatch,
        "one-body-at-a-time operator confirmation is mandatory for each alias");

    auto duplicate_sdk = sdk;
    duplicate_sdk.back().identity_sha256 = duplicate_sdk.front().identity_sha256;
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, cam_b_path}, duplicate_sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::duplicate_identity,
        "duplicate current SDK projections must block");

    auto three_sdk = sdk;
    three_sdk.push_back(sdk.front());
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, cam_b_path}, {}, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::camera_count_mismatch &&
          block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, cam_b_path}, three_sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::camera_count_mismatch,
        "zero or more than two current projections must block exact dual cardinality");

    auto missing_identity = sdk;
    missing_identity.back().identity_sha256.clear();
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, cam_b_path}, missing_identity, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::missing_identity,
        "missing or noncanonical anonymous current identity must block");

    auto unbound_sdk = sdk;
    unbound_sdk.back().identity_sha256 = std::string(64, 'e');
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, cam_b_path}, unbound_sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::unbound_identity,
        "a current projection absent from both proofs must not be auto-bound");

    auto wrong_transport = wpd;
    wrong_transport.front().identity_sha256 = std::string(64, 'a');
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, cam_b_path}, sdk, wrong_transport,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::mismatched_transport,
        "an SDK digest presented by WPD must block rather than auto-assign an alias");

    std::string tampered = DualIdentityProofJson("CAM-B", 'b', 'd');
    tampered.replace(tampered.find(std::string(64, 'd')), 64, std::string(64, 'e'));
    WriteText(root / "tampered.json", tampered);
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, root / "tampered.json"}, sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::proof_tampered,
        "proof payload tampering must return a typed Block");

    std::string malformed = DualIdentityProofJson("CAM-B", 'b', 'd');
    malformed.insert(malformed.find("\"cameraMode\""),
        "\"cameraMode\":\"DualCamera\",");
    WriteText(root / "malformed.json", malformed);
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, root / "malformed.json"}, sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::proof_invalid,
        "duplicate or structurally invalid proof fields must fail strict parsing");

    std::string wrong_version = DualIdentityProofJson("CAM-B", 'b', 'd');
    wrong_version.replace(
        wrong_version.find("dual-identity-binding-proof.v1"),
        std::string("dual-identity-binding-proof.v1").size(),
        "dual-identity-binding-proof.v2");
    WriteText(root / "wrong-version.json", wrong_version);
    Check(block_reason(VerifyDualIdentitySoftwareContract(
              provider, {cam_a_path, root / "wrong-version.json"}, sdk, wpd,
              "2026-08-11T00:00:00Z", false)) ==
              DualIdentityBlockReason::proof_invalid,
        "unsupported proof schema/version must fail closed");

    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

void TestDualIdentityDirectNegativeMatrix() {
    const fs::path root = fs::temp_directory_path() /
        ("a0-dual-identity-negative-matrix-" + NewRunId());
    const fs::path cam_a_path = root / "cam-a.json";
    const fs::path cam_b_path = root / "cam-b.json";
    const std::string cam_a_json = DualIdentityProofJson("CAM-A", 'a', 'c');
    const std::string cam_b_json = DualIdentityProofJson("CAM-B", 'b', 'd');
    WriteText(cam_a_path, cam_a_json);
    WriteText(cam_b_path, cam_b_json);

    const auto remove_line = [](std::string body, std::string_view field) {
        const auto field_position = body.find(field);
        const auto line_start = body.rfind('\n', field_position);
        const auto line_end = body.find('\n', field_position);
        if (field_position == std::string::npos ||
            line_start == std::string::npos || line_end == std::string::npos) {
            throw std::runtime_error("test proof field was not found");
        }
        body.erase(line_start + 1, line_end - line_start);
        return body;
    };
    const auto replace_once = [](std::string body,
                                 std::string_view from,
                                 std::string_view to) {
        const auto position = body.find(from);
        if (position == std::string::npos) {
            throw std::runtime_error("test proof token was not found");
        }
        body.replace(position, from.size(), to);
        return body;
    };

    std::string unknown_field = cam_a_json;
    unknown_field.insert(unknown_field.find('{') + 1,
        "\n  \"unknownField\": true,");
    std::string invalid_utf8 = cam_a_json;
    std::string invalid_provider{"bad-"};
    invalid_provider.push_back(static_cast<char>(0xC3));
    invalid_provider.push_back(static_cast<char>(0x28));
    invalid_utf8 = replace_once(
        std::move(invalid_utf8), "documented-test-provider", invalid_provider);
    const std::vector<std::pair<std::string, std::string>> parse_cases{
        {"unknown-field", std::move(unknown_field)},
        {"missing-field", remove_line(cam_a_json, "\"providerVersion\"")},
        {"wrong-type", replace_once(
            cam_a_json, "\"providerVersion\": 1", "\"providerVersion\": \"1\"")},
        {"invalid-utf8", std::move(invalid_utf8)},
    };
    for (const auto& [name, body] : parse_cases) {
        DualIdentitySafetyState safety;
        bool rejected = false;
        try {
            (void)ParseDualIdentityBindingProof(body);
        } catch (const std::exception&) {
            rejected = true;
        }
        Check(rejected,
            "direct dual identity parser negative must reject " + name);
        CheckDualIdentitySafetyZero(safety,
            "direct parser rejection must leave every safety counter at zero");
    }

    const fs::path empty_path = root / "empty.json";
    const fs::path oversized_path = root / "oversized.json";
    WriteText(empty_path, "");
    WriteText(oversized_path, std::string(16U * 1024U + 1U, ' '));
    std::vector<std::pair<std::string, fs::path>> load_cases{
        {"zero-byte", empty_path},
        {"oversized", oversized_path},
        {"relative", fs::path("relative-dual-proof.json")},
        {"unc", fs::path(L"\\\\invalid-server\\invalid-share\\proof.json")},
        {"device", fs::path(L"\\\\?\\C:\\invalid-proof.json")},
    };
    const DWORD drive_mask = GetLogicalDrives();
    for (wchar_t drive = L'Z'; drive >= L'D'; --drive) {
        const DWORD bit = 1UL << static_cast<DWORD>(drive - L'A');
        if ((drive_mask & bit) == 0) {
            std::wstring unavailable_drive;
            unavailable_drive.push_back(drive);
            unavailable_drive.append(L":\\dual-proof.json");
            load_cases.emplace_back(
                "non-fixed-or-unavailable-drive", fs::path(unavailable_drive));
            break;
        }
    }
    for (const auto& [name, path] : load_cases) {
        DualIdentitySafetyState safety;
        bool rejected = false;
        try {
            (void)LoadDualIdentityBindingProof(path);
        } catch (const std::exception&) {
            rejected = true;
        }
        Check(rejected,
            "direct dual identity loader negative must reject " + name);
        CheckDualIdentitySafetyZero(safety,
            "direct loader rejection must leave every safety counter at zero");
    }

    const fs::path reparse_path = root / "cam-a-link.json";
    const bool reparse_created = CreateSymbolicLinkW(
        reparse_path.c_str(), cam_a_path.c_str(), 0x2U) != FALSE;
    if (reparse_created) {
        DualIdentitySafetyState safety;
        bool rejected = false;
        try {
            (void)LoadDualIdentityBindingProof(reparse_path);
        } catch (const std::exception&) {
            rejected = true;
        }
        Check(rejected,
            "public dual identity loader must reject a proof file reparse point");
        CheckDualIdentitySafetyZero(safety,
            "reparse rejection must leave every safety counter at zero");
        std::error_code remove_link_error;
        fs::remove(reparse_path, remove_link_error);
        Check(!remove_link_error,
            "test proof reparse point must be removed without following it");
        std::cout << "Dual identity proof reparse negative: exercised\n";
    } else {
        std::cout << "Dual identity proof reparse negative: unavailable in this Windows session\n";
    }

    const DualIdentityCorrelationProvider provider{
        "documented-test-provider", 1, true};
    const std::vector<DualIdentityInventoryProjection> sdk{
        {DualIdentityTransport::sdk, "Nikon D810", std::string(64, 'a'),
            "documented-test-provider", 1},
        {DualIdentityTransport::sdk, "Nikon D810", std::string(64, 'b'),
            "documented-test-provider", 1}};
    const std::vector<DualIdentityInventoryProjection> wpd{
        {DualIdentityTransport::wpd, "Nikon D810", std::string(64, 'c'),
            "documented-test-provider", 1},
        {DualIdentityTransport::wpd, "Nikon D810", std::string(64, 'd'),
            "documented-test-provider", 1}};
    const auto expect_block = [](const DualIdentityResult& result,
                                 DualIdentityBlockReason expected,
                                 std::string_view context) {
        Check(std::holds_alternative<DualIdentityBlocked>(result),
            std::string(context));
        if (!std::holds_alternative<DualIdentityBlocked>(result)) return;
        const auto& blocked = std::get<DualIdentityBlocked>(result);
        Check(blocked.reason == expected, std::string(context));
        CheckDualIdentitySafetyZero(blocked.safety,
            "direct typed Block must keep every safety counter at zero");
    };

    WriteText(root / "correlation-unconfirmed.json",
        DualIdentityProofJson("CAM-B", 'b', 'd',
            "documented-test-provider", 1, true, false));
    auto one_sdk = sdk;
    one_sdk.pop_back();
    auto wrong_model_sdk = sdk;
    wrong_model_sdk.back().model = "Nikon Other";
    auto wrong_inventory_provider_version = sdk;
    wrong_inventory_provider_version.back().provider_version = 2;
    const std::vector<std::tuple<
        std::string, DualIdentityResult, DualIdentityBlockReason>> contract_cases{
        {"same-proof-path",
            VerifyDualIdentitySoftwareContract(
                provider, {cam_a_path, cam_a_path}, sdk, wpd,
                "2026-08-11T00:00:00Z", false),
            DualIdentityBlockReason::alias_cardinality_mismatch},
        {"provider-version-downgrade",
            VerifyDualIdentitySoftwareContract(
                DualIdentityCorrelationProvider{
                    "documented-test-provider", 2, true},
                {cam_a_path, cam_b_path}, sdk, wpd,
                "2026-08-11T00:00:00Z", false),
            DualIdentityBlockReason::provider_mismatch},
        {"inventory-provider-version-mismatch",
            VerifyDualIdentitySoftwareContract(
                provider, {cam_a_path, cam_b_path},
                wrong_inventory_provider_version, wpd,
                "2026-08-11T00:00:00Z", false),
            DualIdentityBlockReason::provider_mismatch},
        {"one-camera-inventory",
            VerifyDualIdentitySoftwareContract(
                provider, {cam_a_path, cam_b_path}, one_sdk, wpd,
                "2026-08-11T00:00:00Z", false),
            DualIdentityBlockReason::camera_count_mismatch},
        {"model-mismatch",
            VerifyDualIdentitySoftwareContract(
                provider, {cam_a_path, cam_b_path}, wrong_model_sdk, wpd,
                "2026-08-11T00:00:00Z", false),
            DualIdentityBlockReason::missing_identity},
        {"documented-correlation-unconfirmed",
            VerifyDualIdentitySoftwareContract(
                provider, {cam_a_path, root / "correlation-unconfirmed.json"},
                sdk, wpd, "2026-08-11T00:00:00Z", false),
            DualIdentityBlockReason::confirmation_mismatch},
    };
    for (const auto& [name, result, expected] : contract_cases) {
        expect_block(result, expected,
            "direct dual identity contract negative must block " + name);
    }

    const auto ready = VerifyDualIdentitySoftwareContract(
        provider, {cam_a_path, cam_b_path}, sdk, wpd,
        "2026-08-11T00:00:00Z", false);
    Check(std::holds_alternative<DualIdentityReady>(ready),
        "direct negative matrix control must retain the typed Ready path");
    if (std::holds_alternative<DualIdentityReady>(ready)) {
        CheckDualIdentitySafetyZero(
            std::get<DualIdentityReady>(ready).safety,
            "typed Ready must explicitly retain inventory_read_only and zero mutation counters");
    }

    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

void TestProductionDualIdentityPreflightReachesSoftwareReady() {
    const fs::path root = fs::temp_directory_path() /
        ("a0-production-dual-preflight-test-" + NewRunId());
    const fs::path provider_path = root / "provider.json";
    const fs::path cam_a_path = root / "cam-a.json";
    const fs::path cam_b_path = root / "cam-b.json";
    WriteText(provider_path,
        "{\"schemaVersion\":\"a0.camera-agent.dual-correlation-provider.v1\","
        "\"providerId\":\"documented-test-provider\",\"providerVersion\":1,"
        "\"documentedStablePerBodyCorrelation\":true}");
    WriteText(cam_a_path, DualIdentityProofJson("CAM-A", 'a', 'c'));
    WriteText(cam_b_path, DualIdentityProofJson("CAM-B", 'b', 'd'));

    ProductionDualIdentityPreflightRequest request;
    request.provider_config_path = provider_path;
    request.proof_paths = {cam_a_path, cam_b_path};
    request.sdk_inventory = {
        {DualIdentityTransport::sdk, "Nikon D810", std::string(64, 'a'),
            "documented-test-provider", 1},
        {DualIdentityTransport::sdk, "Nikon D810", std::string(64, 'b'),
            "documented-test-provider", 1}};
    request.wpd_inventory = {
        {DualIdentityTransport::wpd, "Nikon D810", std::string(64, 'c'),
            "documented-test-provider", 1},
        {DualIdentityTransport::wpd, "Nikon D810", std::string(64, 'd'),
            "documented-test-provider", 1}};
    request.observed_at_utc = "2026-08-11T00:00:00Z";

    const auto typed_result = RunProductionDualIdentityPreflight(request);
    Check(std::holds_alternative<DualIdentityReady>(typed_result),
        "typed production preflight must accept approved anonymous software inputs");
    if (std::holds_alternative<DualIdentityReady>(typed_result)) {
        CheckDualIdentitySafetyZero(std::get<DualIdentityReady>(typed_result).safety,
            "software-only production preflight Ready must retain zero side effects");
    }
    const auto public_caller = VerifyProductionDualIdentityPreflight(request);
    Check(public_caller.terminal_state == "Ready" &&
              public_caller.sdk_camera_count == 2 &&
              public_caller.wpd_camera_count == 2 &&
              public_caller.sdk_cam_a_count == 1 &&
              public_caller.sdk_cam_b_count == 1 &&
              public_caller.wpd_cam_a_count == 1 &&
              public_caller.wpd_cam_b_count == 1 &&
              !public_caller.capture_command_sent &&
              !public_caller.live_view_started &&
              !public_caller.camera_settings_changed &&
              !public_caller.card_access_performed,
        "the public summary caller used by all three CLIs must reach software-only Ready");
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

void TestProductionDualIdentityPreflightBlocksInvalidInputsWithoutSideEffects() {
    const fs::path root = fs::temp_directory_path() /
        ("a0-production-dual-preflight-negative-test-" + NewRunId());
    const fs::path provider_path = root / "provider.json";
    const fs::path mismatch_provider_path = root / "provider-v2.json";
    const fs::path malformed_provider_path = root / "malformed-provider.json";
    const fs::path cam_a_path = root / "cam-a.json";
    const fs::path cam_b_path = root / "cam-b.json";
    const fs::path expired_path = root / "expired.json";
    const fs::path unknown_alias_path = root / "unknown-alias.json";
    const std::string provider_json =
        "{\"schemaVersion\":\"a0.camera-agent.dual-correlation-provider.v1\","
        "\"providerId\":\"documented-test-provider\",\"providerVersion\":1,"
        "\"documentedStablePerBodyCorrelation\":true}";
    WriteText(provider_path, provider_json);
    WriteText(mismatch_provider_path,
        "{\"schemaVersion\":\"a0.camera-agent.dual-correlation-provider.v1\","
        "\"providerId\":\"documented-test-provider\",\"providerVersion\":2,"
        "\"documentedStablePerBodyCorrelation\":true}");
    WriteText(malformed_provider_path, provider_json + " trailing");
    WriteText(cam_a_path, DualIdentityProofJson("CAM-A", 'a', 'c'));
    WriteText(cam_b_path, DualIdentityProofJson("CAM-B", 'b', 'd'));
    WriteText(expired_path, DualIdentityProofJson(
        "CAM-B", 'b', 'd', "documented-test-provider", 1, true, true,
        "2026-08-10T00:00:00Z"));
    std::string unknown_alias = DualIdentityProofJson("CAM-B", 'b', 'd');
    unknown_alias.replace(unknown_alias.find("CAM-B"), 5, "CAM-X");
    WriteText(unknown_alias_path, unknown_alias);

    ProductionDualIdentityPreflightRequest valid;
    valid.provider_config_path = provider_path;
    valid.proof_paths = {cam_a_path, cam_b_path};
    valid.sdk_inventory = {
        {DualIdentityTransport::sdk, "Nikon D810", std::string(64, 'a'),
            "documented-test-provider", 1},
        {DualIdentityTransport::sdk, "Nikon D810", std::string(64, 'b'),
            "documented-test-provider", 1}};
    valid.wpd_inventory = {
        {DualIdentityTransport::wpd, "Nikon D810", std::string(64, 'c'),
            "documented-test-provider", 1},
        {DualIdentityTransport::wpd, "Nikon D810", std::string(64, 'd'),
            "documented-test-provider", 1}};
    valid.observed_at_utc = "2026-08-11T00:00:00Z";

    const auto expect_block = [](const ProductionDualIdentityPreflightRequest& request,
                                 DualIdentityBlockReason expected,
                                 std::string_view context) {
        const auto result = RunProductionDualIdentityPreflight(request);
        Check(std::holds_alternative<DualIdentityBlocked>(result),
            std::string(context));
        if (!std::holds_alternative<DualIdentityBlocked>(result)) return;
        const auto& blocked = std::get<DualIdentityBlocked>(result);
        Check(blocked.reason == expected, std::string(context));
        CheckDualIdentitySafetyZero(blocked.safety,
            "production preflight Block must keep SDK/WPD/card/capture/delete/retry side effects zero");
        const auto public_caller = VerifyProductionDualIdentityPreflight(request);
        Check(public_caller.terminal_state == "Blocked" &&
                  public_caller.failure_category ==
                      DualIdentityBlockReasonName(expected) &&
                  !public_caller.identity_maps_changed &&
                  !public_caller.capture_command_sent &&
                  !public_caller.live_view_started &&
                  !public_caller.camera_settings_changed &&
                  !public_caller.card_access_performed &&
                  !public_caller.real_identifiers_included,
            "the public summary caller used by all three CLIs must preserve typed Block and zero side effects");
    };

    auto request = valid;
    request.proof_paths[1] = expired_path;
    expect_block(request, DualIdentityBlockReason::proof_stale,
        "expired proof must block the production caller");
    request = valid;
    request.provider_config_path = mismatch_provider_path;
    expect_block(request, DualIdentityBlockReason::provider_mismatch,
        "provider version mismatch must block the production caller");
    request = valid;
    request.provider_config_path = root / "missing-provider.json";
    expect_block(request, DualIdentityBlockReason::provider_config_invalid,
        "missing provider config must block the production caller");
    request.provider_config_path = malformed_provider_path;
    expect_block(request, DualIdentityBlockReason::provider_config_invalid,
        "malformed provider config must block the production caller");
    request = valid;
    request.proof_paths[1] = root / "missing-proof.json";
    expect_block(request, DualIdentityBlockReason::proof_invalid,
        "missing proof must block the production caller");
    request = valid;
    request.proof_paths.pop_back();
    expect_block(request, DualIdentityBlockReason::proof_count_mismatch,
        "missing CAM-B proof input must block the production caller");
    request = valid;
    request.proof_paths[1] = unknown_alias_path;
    expect_block(request, DualIdentityBlockReason::proof_invalid,
        "unknown proof alias must block the production caller");
    request = valid;
    request.sdk_inventory[1].identity_sha256 =
        request.sdk_inventory[0].identity_sha256;
    expect_block(request, DualIdentityBlockReason::duplicate_identity,
        "duplicate anonymous inventory digest must block the production caller");
    request = valid;
    request.sdk_inventory[0].identity_sha256 = std::string(64, 'c');
    request.sdk_inventory[1].identity_sha256 = std::string(64, 'd');
    request.wpd_inventory[0].identity_sha256 = std::string(64, 'a');
    request.wpd_inventory[1].identity_sha256 = std::string(64, 'b');
    expect_block(request, DualIdentityBlockReason::mismatched_transport,
        "cross-transport inventory reversal must block without order fallback");
    request = {};
    request.observed_at_utc = "2026-08-11T00:00:00Z";
    expect_block(request, DualIdentityBlockReason::identity_strategy_unresolved,
        "default production caller must remain identity_strategy_unresolved");
    request = valid;
    request.legacy_map_fallback_requested = true;
    expect_block(request, DualIdentityBlockReason::legacy_map_fallback_prohibited,
        "legacy map fallback must remain prohibited in the production caller");

    for (const auto& invalid_path : {
             fs::path("relative-provider.json"),
             fs::path(L"\\\\invalid-server\\invalid-share\\provider.json"),
             fs::path(L"\\\\?\\C:\\invalid-provider.json")}) {
        bool rejected = false;
        try {
            (void)LoadDualIdentityCorrelationProvider(invalid_path);
        } catch (const std::exception&) {
            rejected = true;
        }
        Check(rejected,
            "provider config loader must reject non-fixed-local input paths");
    }

    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

void TestSingleIdentityV3Parser() {
    const std::string digest(64, 'b');
    const std::string json =
        "{\"schemaVersion\":\"a0.camera-agent.single-identity.v3\","
        "\"cameraMode\":\"SingleCamera\",\"selectedAlias\":\"CAM-A\","
        "\"wpdStableIdentitySha256\":\"" + digest +
        "\",\"sdkSelectionPolicy\":\"exactly-one-current-session\"}";
    const auto parsed = ParseSingleCameraIdentityV3(json);
    Check(parsed.camera_alias == "CAM-A" &&
              parsed.wpd_stable_identity_sha256 == digest &&
              parsed.sdk_selection_policy == "exactly-one-current-session",
        "identity-v3 parser must preserve the WPD authority and exact-one SDK policy");
    for (const auto& invalid : {
             json + " trailing",
             std::string(json).replace(json.find("CAM-A"), 5, "CAM-B"),
             std::string(json).replace(json.find(digest), digest.size(), std::string(64, 'G')),
             std::string(json).replace(
                 json.find("exactly-one-current-session"),
                 std::string("exactly-one-current-session").size(),
                 "enumeration-order")}) {
        bool rejected = false;
        try {
            (void)ParseSingleCameraIdentityV3(invalid);
        } catch (const std::exception&) {
            rejected = true;
        }
        Check(rejected, "identity-v3 must reject trailing, CAM-B, malformed digest, and unsafe SDK policy values");
    }
}

void TestSingleIdentityV3SdkStatusResolution() {
    const fs::path root = fs::temp_directory_path() /
        ("a0-single-sdk-status-identity-test-" + NewRunId());
    const fs::path identity_path = root / "single-identity-v3.json";
    const fs::path legacy_map_path = root / "camera-map.json";
    const CameraInfo sdk_camera{
        "Nikon D810", "unknown", "S", "ephemeral-sdk-current-session"};
    const CameraInfo wpd_camera{
        "Nikon D810", "1.14", "S", std::string(64, 'a')};
    try {
        PersistSingleIdentityV3(
            identity_path, "CAM-A", {sdk_camera}, {wpd_camera});
        const auto identity = LoadSingleCameraIdentityV3(identity_path);
        const auto selected = ResolveSingleCameraSdkStatusCamera(
            identity, "CAM-A", {sdk_camera}, {wpd_camera});
        Check(selected.stable_identity == sdk_camera.stable_identity,
            "SingleCamera sdk-status must select the exact-one current SDK projection from identity-v3");

        const auto rejected = [&](const SingleCameraIdentityV3& candidate,
                                  std::string_view alias,
                                  const std::vector<CameraInfo>& sdk,
                                  const std::vector<CameraInfo>& wpd) {
            try {
                (void)ResolveSingleCameraSdkStatusCamera(
                    candidate, alias, sdk, wpd);
                return false;
            } catch (const std::exception&) {
                return true;
            }
        };
        CameraInfo mismatched_wpd = wpd_camera;
        mismatched_wpd.stable_identity = std::string(64, 'b');
        Check(rejected(identity, "CAM-A", {}, {wpd_camera}) &&
              rejected(identity, "CAM-A", {sdk_camera, sdk_camera}, {wpd_camera}) &&
              rejected(identity, "CAM-A", {sdk_camera}, {}) &&
              rejected(identity, "CAM-A", {sdk_camera}, {wpd_camera, wpd_camera}) &&
              rejected(identity, "CAM-A", {sdk_camera}, {mismatched_wpd}) &&
              rejected(identity, "CAM-B", {sdk_camera}, {wpd_camera}),
            "SingleCamera sdk-status must reject missing, multiple, digest-mismatched, or non-CAM-A identity before status probe");

        WriteText(root / "malformed.json", "{not-json}");
        bool malformed_rejected = false;
        try {
            (void)LoadSingleCameraIdentityV3(root / "malformed.json");
        } catch (const std::exception&) {
            malformed_rejected = true;
        }
        IdentityMap legacy_map(legacy_map_path);
        legacy_map.Bind("CAM-A", sdk_camera.stable_identity);
        bool legacy_only_rejected = false;
        try {
            (void)LoadSingleCameraIdentityV3(root / "missing-v3.json");
        } catch (const std::exception&) {
            legacy_only_rejected = true;
        }
        Check(malformed_rejected && legacy_only_rejected &&
              legacy_map.FindAlias(sdk_camera.stable_identity) == "CAM-A",
            "malformed or missing identity-v3 must fail even when a legacy CAM-A map exists");
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: SingleCamera sdk-status identity contract threw: "
                  << error.what() << '\n';
    }
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

void TestSingleIdentityV3SdkStatusRoutingStopsBeforeSdkOnWpdFailure() {
    struct FakeWpd final : ISingleIdentityV3WpdEnumerator {
        FakeWpd(std::vector<CameraInfo> source, std::vector<std::string>* trace)
            : cameras(std::move(source)), calls(trace) {}
        std::vector<CameraInfo> cameras;
        std::vector<std::string>* calls{};
        int enumerate_count{};
        std::vector<CameraInfo> Enumerate() override {
            ++enumerate_count;
            calls->push_back("wpd-enumerate");
            return cameras;
        }
    };
    struct SdkCalls {
        int factory_count{};
        int require_count{};
        int enumerate_count{};
        int probe_count{};
    };
    struct FakeSdk final : ISdkStatusExecutor {
        FakeSdk(std::vector<CameraInfo> source,
                std::vector<std::string>* trace,
                SdkCalls* counters)
            : cameras(std::move(source)), calls(trace), counts(counters) {}
        std::vector<CameraInfo> cameras;
        std::vector<std::string>* calls{};
        SdkCalls* counts{};
        std::string SdkVersion() const override { return "fake-read-only"; }
        void RequireExactlyOneD810ForSingleStatus() override {
            ++counts->require_count;
            calls->push_back("sdk-require-exactly-one");
        }
        std::vector<CameraInfo> Enumerate() override {
            ++counts->enumerate_count;
            calls->push_back("sdk-enumerate");
            return cameras;
        }
        SdkCameraStatus ProbeSdkStatus(
            std::string_view,
            std::chrono::seconds) override {
            ++counts->probe_count;
            calls->push_back("sdk-probe");
            SdkCameraStatus result;
            result.command_trace.sdk_session_opened = true;
            result.command_trace.sdk_session_closed = true;
            return result;
        }
    };

    const fs::path root = fs::temp_directory_path() /
        ("a0-single-sdk-status-routing-test-" + NewRunId());
    const fs::path identity_path = root / "single-identity-v3.json";
    const fs::path malformed_path = root / "malformed.json";
    const CameraInfo sdk_camera{
        "Nikon D810", "unknown", "S", "synthetic-current-sdk-projection"};
    const CameraInfo matching_wpd{
        "Nikon D810", "unknown", "S", std::string(64, 'a')};
    PersistSingleIdentityV3(
        identity_path, "CAM-A", {sdk_camera}, {matching_wpd});
    WriteText(malformed_path, "{not-json}");
    const auto run = [&](const fs::path& selected_identity_path,
                         std::vector<CameraInfo> wpd_cameras,
                         bool expect_success,
                         int expected_wpd_enumerations) {
        std::vector<std::string> calls;
        int wpd_factory_count = 0;
        const auto wpd_factory = [&]() ->
            std::unique_ptr<ISingleIdentityV3WpdEnumerator> {
            ++wpd_factory_count;
            return std::make_unique<FakeWpd>(
                std::move(wpd_cameras), &calls);
        };
        SdkCalls sdk_calls;
        const auto sdk_factory = [&]() -> std::unique_ptr<ISdkStatusExecutor> {
            ++sdk_calls.factory_count;
            return std::make_unique<FakeSdk>(
                std::vector<CameraInfo>{sdk_camera}, &calls, &sdk_calls);
        };
        bool succeeded = false;
        try {
            const auto result = ExecuteSingleIdentityV3SdkStatus(
                selected_identity_path, "CAM-A", wpd_factory, sdk_factory,
                std::chrono::seconds(1));
            succeeded = result.camera.stable_identity == sdk_camera.stable_identity;
        } catch (const std::exception&) {
        }
        if (expect_success) {
            Check(succeeded && wpd_factory_count == 1 &&
                      expected_wpd_enumerations == 1 &&
                      sdk_calls.factory_count == 1 &&
                      sdk_calls.require_count == 1 &&
                      sdk_calls.enumerate_count == 1 &&
                      sdk_calls.probe_count == 1 &&
                      calls == std::vector<std::string>({
                          "wpd-enumerate", "sdk-require-exactly-one",
                          "sdk-enumerate", "sdk-probe"}),
                "production SingleCamera status routing must validate WPD before SDK enumerate and probe");
        } else {
            Check(!succeeded &&
                      wpd_factory_count == expected_wpd_enumerations &&
                      sdk_calls.factory_count == 0 &&
                      sdk_calls.require_count == 0 &&
                      sdk_calls.enumerate_count == 0 &&
                      sdk_calls.probe_count == 0 &&
                      calls == (expected_wpd_enumerations == 0
                          ? std::vector<std::string>{}
                          : std::vector<std::string>{"wpd-enumerate"}),
                "identity-file or WPD rejection must leave every SDK status call at zero");
        }
    };

    run(identity_path, {matching_wpd}, true, 1);
    run(root / "missing.json", {matching_wpd}, false, 0);
    run(malformed_path, {matching_wpd}, false, 0);
    run(identity_path, {}, false, 1);
    run(identity_path, {matching_wpd, matching_wpd}, false, 1);
    auto wrong_model_wpd = matching_wpd;
    wrong_model_wpd.model = "Not a D810";
    run(identity_path, {wrong_model_wpd}, false, 1);
    auto mismatched_wpd = matching_wpd;
    mismatched_wpd.stable_identity = std::string(64, 'b');
    run(identity_path, {mismatched_wpd}, false, 1);
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

void TestContinuousLiveViewV2Protocol() {
    FakeBackend backend;
    HardwareCameraAgentDispatcher dispatcher(backend);
    const std::string session(32, 'a');
    const auto start = dispatcher.Handle(LiveViewV2Envelope(
        "start-live-view",
        "{\"cameraAlias\":\"CAM-A\",\"sessionId\":\"" + session +
            "\",\"exclusiveCameraControlConfirmed\":true}"));
    Check(start.find("\"schemaVersion\":\"a0.camera-agent.hardware.v2\"") != std::string::npos &&
          start.find("\"resultCode\":\"Started\"") != std::string::npos &&
          backend.continuous_start_calls == 1,
        "hardware v2 must start one owned continuous Live View session");

    const auto frame = dispatcher.Handle(LiveViewV2Envelope(
        "read-live-view-frame",
        "{\"sessionId\":\"" + session + "\"}"));
    Check(frame.find("\"state\":\"Frame\"") != std::string::npos &&
          frame.find("\"frameJpegBase64\":\"/9j/2Q==\"") != std::string::npos &&
          frame.find("\"previewIsOriginal\":false") != std::string::npos &&
          backend.continuous_frame_calls == 1,
        "hardware v2 frame must remain a non-original bounded payload");

    const auto stop = dispatcher.Handle(LiveViewV2Envelope(
        "stop-live-view",
        "{\"sessionId\":\"" + session + "\"}"));
    Check(stop.find("\"resultCode\":\"Stopped\"") != std::string::npos &&
          backend.continuous_stop_calls == 1 && !dispatcher.ShouldStop(),
        "stopping Live View must keep the agent available for same-process capture");

    const auto close = dispatcher.Handle(LiveViewV2Envelope(
        "close-agent-session",
        "{\"sessionId\":\"" + session + "\"}"));
    Check(close.find("\"resultCode\":\"Closed\"") != std::string::npos &&
          backend.continuous_close_calls == 1 && dispatcher.ShouldStop(),
        "close-agent-session must request bounded process exit");

    FakeBackend rejected_backend;
    HardwareCameraAgentDispatcher rejected_dispatcher(rejected_backend);
    const auto rejected = rejected_dispatcher.Handle(LiveViewV2Envelope(
        "capture-single", "{}"));
    Check(rejected.find("\"schemaVersion\":\"a0.camera-agent.hardware.v2\"") != std::string::npos &&
          rejected.find("UnsupportedOperation") != std::string::npos &&
          rejected_backend.capture_calls == 0,
        "hardware v2 must not reinterpret v1 capture operations");

    const std::vector<std::string> malformed_v2_requests{
        "{ \"schemaVersion\" : \"a0.camera-agent.hardware.v2\","
        "\"simulation\":false,\"marker\":\"Hardware\",\"requestId\":\"spaced-v2\","
        "\"operation\":\"capture-single\",\"payload\":{} }",
        "{\"payload\":{\"sessionId\":\"" + session + "\",\"unknown\":true},"
        "\"operation\":\"read-live-view-frame\",\"requestId\":\"reordered-v2\","
        "\"marker\":\"Hardware\",\"simulation\":false,"
        "\"schemaVersion\":\"a0.camera-agent.hardware.v2\"}",
        "{\"payload\":{\"sessionId\":\"" + session + "\",\"sessionId\":\"" +
        session + "\"},\"operation\":\"read-live-view-frame\","
        "\"requestId\":\"duplicate-v2\",\"marker\":\"Hardware\","
        "\"simulation\":false,\"schemaVersion\":\"a0.camera-agent.hardware.v2\"}",
    };
    for (const auto& malformed_v2 : malformed_v2_requests) {
        const auto rejection = rejected_dispatcher.Handle(malformed_v2);
        Check(rejection.find(
                  "\"schemaVersion\":\"a0.camera-agent.hardware.v2\"") !=
                  std::string::npos &&
              rejection.find("\"success\":false") != std::string::npos,
            "parsed v2 schema must survive whitespace, field order, and payload rejection");
    }
}

void TestProductionContinuousLiveViewContracts() {
    const fs::path root = fs::temp_directory_path() /
        ("a0-agent-continuous-live-view-test-" + NewRunId());
    const std::string owner_session(32, 'a');
    const std::string other_session(32, 'b');
    try {
        {
            auto state = std::make_shared<FakeContinuousLiveViewSdkState>();
            state->frame.assign(512U * 1024U, 0x5AU);
            state->frame[0] = 0xFFU;
            state->frame[1] = 0xD8U;
            state->frame[state->frame.size() - 2] = 0xFFU;
            state->frame.back() = 0xD9U;
            ProductionHardwareCameraAgentBackend backend(
                ContinuousLiveViewTestConfig(root / "frame-boundary", state));
            const auto started = backend.StartContinuousLiveView(ContinuousRequest(
                HardwareCameraAgentOperation::start_live_view, owner_session));
            const auto frame = backend.ReadContinuousLiveViewFrame(ContinuousRequest(
                HardwareCameraAgentOperation::read_live_view_frame, owner_session));
            Check(started.succeeded && frame.succeeded &&
                  frame.frame_size == 512U * 1024U,
                "production backend must accept the exact 512 KiB JPEG boundary");
            Check(backend.StopContinuousLiveView(ContinuousRequest(
                      HardwareCameraAgentOperation::stop_live_view,
                      owner_session)).succeeded,
                "exact-boundary session must stop cleanly");

            state->frame.push_back(0x00U);
            state->frame[state->frame.size() - 3] = 0x5AU;
            state->frame[state->frame.size() - 2] = 0xFFU;
            state->frame.back() = 0xD9U;
            Check(backend.StartContinuousLiveView(ContinuousRequest(
                      HardwareCameraAgentOperation::start_live_view,
                      owner_session)).succeeded,
                "oversize-frame contract setup must start");
            const auto oversized = backend.ReadContinuousLiveViewFrame(ContinuousRequest(
                HardwareCameraAgentOperation::read_live_view_frame, owner_session));
            Check(!oversized.succeeded &&
                  oversized.error_category == "continuous_live_view_frame_failed" &&
                  state->stop_calls >= 2 && state->close_calls >= 2,
                "production backend must reject 512 KiB + 1 and close the SDK session");
        }

        {
            auto state = std::make_shared<FakeContinuousLiveViewSdkState>();
            ProductionHardwareCameraAgentBackend backend(
                ContinuousLiveViewTestConfig(root / "ownership", state));
            Check(backend.StartContinuousLiveView(ContinuousRequest(
                      HardwareCameraAgentOperation::start_live_view,
                      owner_session)).succeeded,
                "ownership contract setup must start");
            const auto takeover = backend.ReadContinuousLiveViewFrame(ContinuousRequest(
                HardwareCameraAgentOperation::read_live_view_frame, other_session));
            const auto double_start = backend.StartContinuousLiveView(ContinuousRequest(
                HardwareCameraAgentOperation::start_live_view, other_session));
            Check(!takeover.succeeded &&
                  takeover.error_category == "live_view_session_not_found" &&
                  !double_start.succeeded &&
                  double_start.error_category == "live_view_session_active" &&
                  state->read_calls == 0,
                "another session must not take over or double-start active Live View");
            Check(backend.StopContinuousLiveView(ContinuousRequest(
                      HardwareCameraAgentOperation::stop_live_view,
                      owner_session)).succeeded,
                "the owning session must retain stop authority");
        }

        {
            auto state = std::make_shared<FakeContinuousLiveViewSdkState>();
            auto now = std::chrono::steady_clock::time_point{};
            ProductionHardwareCameraAgentBackend backend(
                ContinuousLiveViewTestConfig(
                    root / "heartbeat-timeout", state, [&] { return now; }));
            Check(backend.StartContinuousLiveView(ContinuousRequest(
                      HardwareCameraAgentOperation::start_live_view,
                      owner_session)).succeeded,
                "heartbeat timeout contract setup must start");
            now += std::chrono::seconds(20);
            const auto expired = backend.HeartbeatContinuousLiveView(ContinuousRequest(
                HardwareCameraAgentOperation::live_view_heartbeat, owner_session));
            Check(!expired.succeeded &&
                  expired.error_category == "live_view_session_expired" &&
                  state->stop_calls == 1 && state->close_calls == 1,
                "the injected clock must deterministically expire and close an idle session");
        }

        {
            auto state = std::make_shared<FakeContinuousLiveViewSdkState>();
            std::promise<void> read_entered_promise;
            auto read_entered = read_entered_promise.get_future();
            std::promise<void> release_read_promise;
            auto release_read = release_read_promise.get_future().share();
            state->before_read = [&] {
                read_entered_promise.set_value();
                release_read.wait();
            };
            ProductionHardwareCameraAgentBackend backend(
                ContinuousLiveViewTestConfig(root / "backpressure", state));
            Check(backend.StartContinuousLiveView(ContinuousRequest(
                      HardwareCameraAgentOperation::start_live_view,
                      owner_session)).succeeded,
                "backpressure contract setup must start");
            auto frame = std::async(std::launch::async, [&] {
                return backend.ReadContinuousLiveViewFrame(ContinuousRequest(
                    HardwareCameraAgentOperation::read_live_view_frame,
                    owner_session));
            });
            read_entered.wait();
            auto heartbeat = std::async(std::launch::async, [&] {
                return backend.HeartbeatContinuousLiveView(ContinuousRequest(
                    HardwareCameraAgentOperation::live_view_heartbeat,
                    owner_session));
            });
            Check(heartbeat.wait_for(std::chrono::milliseconds(100)) ==
                      std::future_status::timeout,
                "a second Live View command must observe backend backpressure");
            release_read_promise.set_value();
            Check(frame.get().succeeded && heartbeat.get().succeeded,
                "serialized frame and heartbeat commands must both complete after release");
            Check(backend.StopContinuousLiveView(ContinuousRequest(
                      HardwareCameraAgentOperation::stop_live_view,
                      owner_session)).succeeded,
                "backpressure session must stop cleanly");
        }

        {
            auto state = std::make_shared<FakeContinuousLiveViewSdkState>();
            state->fail_stop = true;
            state->fail_close = true;
            ProductionHardwareCameraAgentBackend backend(
                ContinuousLiveViewTestConfig(root / "close-failure", state));
            Check(backend.StartContinuousLiveView(ContinuousRequest(
                      HardwareCameraAgentOperation::start_live_view,
                      owner_session)).succeeded,
                "stop/close failure contract setup must start");
            const auto closed = backend.CloseAgentSession(ContinuousRequest(
                HardwareCameraAgentOperation::close_agent_session, owner_session));
            Check(!closed.succeeded &&
                  closed.error_category == "continuous_live_view_close_failed" &&
                  state->stop_calls == 1 && state->close_calls == 1,
                "client close must surface both injected SDK cleanup failures");

            HardwareCameraAgentRequest capture;
            capture.operation = HardwareCameraAgentOperation::capture_single;
            capture.transaction_id = "cccccccccccccccccccccccccccccccc";
            capture.camera_alias = "CAM-A";
            capture.expected_capture_profile_id = "test-profile";
            capture.expected_capture_profile_version = 1;
            capture.expected_capture_profile_sha256 = std::string(64, 'c');
            capture.expected_capture_profile_expires_at_utc =
                "2099-12-31T23:59:59Z";
            capture.exclusive_camera_control_confirmed = true;
            capture.dedicated_spool_scope_confirmed = true;
            capture.exact_object_delete_confirmed = true;
            const auto blocked = backend.CaptureSingle(capture);
            Check(!blocked.succeeded &&
                  blocked.error_category == "continuous_live_view_active" &&
                  !fs::exists(root / "close-failure" / "transactions" /
                      capture.transaction_id),
                "residual Live View after client close failure must block capture before reservation");
        }
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: production continuous Live View contract threw: "
                  << error.what() << '\n';
    }
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

} // namespace

int main() {
    TestStrictProtocolAndTypedResponses();
    TestServeOnceRejectsPartialFrameWithoutDispatch();
    TestNamedPipeMaximumFrameBoundary();
    TestNamedPipeDeliveryFailuresExitNonzeroWithoutRedispatch();
    TestDurableJournalRecoveryContracts();
    TestExactlyOneBindingAndHybridExecutorReuse();
    TestProfileSnapshotAndStrictIdentityMapGates();
    TestFixedLocalPathPolicy();
    TestDualIdentityProofContractIsStrictAndFailClosed();
    TestDualIdentityDirectNegativeMatrix();
    TestProductionDualIdentityPreflightReachesSoftwareReady();
    TestProductionDualIdentityPreflightBlocksInvalidInputsWithoutSideEffects();
    TestSingleIdentityV3Parser();
    TestSingleIdentityV3SdkStatusResolution();
    TestSingleIdentityV3SdkStatusRoutingStopsBeforeSdkOnWpdFailure();
    TestContinuousLiveViewV2Protocol();
    TestProductionContinuousLiveViewContracts();
    if (failures != 0) {
        std::cerr << failures << " hardware Camera Agent test(s) failed\n";
        return 1;
    }
    std::cout << "hardware Camera Agent contracts passed\n";
    return 0;
}
