#include "a0/phase0/hardware_camera_agent.hpp"
#include "a0/phase0/hardware_process_lease.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
        return {{"private-name.jpg", {0xFF, 0xD8, 0x01, 0xFF, 0xD9}, true, "exact-object"}};
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
        const std::vector<unsigned char> jpeg{0xFF, 0xD8, 0x01, 0xFF, 0xD9};
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

std::string LiveViewV2Envelope(
    std::string_view operation,
    std::string_view payload) {
    return "{\"schemaVersion\":\"a0.camera-agent.hardware.v2\","
           "\"simulation\":false,\"marker\":\"Hardware\",\"requestId\":\"req-v2\","
           "\"operation\":\"" + std::string(operation) + "\",\"payload\":" +
        std::string(payload) + "}";
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
}

} // namespace

int main() {
    TestStrictProtocolAndTypedResponses();
    TestServeOnceRejectsPartialFrameWithoutDispatch();
    TestDurableJournalRecoveryContracts();
    TestExactlyOneBindingAndHybridExecutorReuse();
    TestProfileSnapshotAndStrictIdentityMapGates();
    TestFixedLocalPathPolicy();
    TestSingleIdentityV3Parser();
    TestContinuousLiveViewV2Protocol();
    if (failures != 0) {
        std::cerr << failures << " hardware Camera Agent test(s) failed\n";
        return 1;
    }
    std::cout << "hardware Camera Agent contracts passed\n";
    return 0;
}
