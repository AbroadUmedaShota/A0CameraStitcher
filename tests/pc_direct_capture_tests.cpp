#include "a0/phase0/pc_direct_capture.hpp"

#include <Windows.h>
#include <wincodec.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

void CheckHr(HRESULT result, std::string_view operation) {
    if (FAILED(result)) {
        throw std::runtime_error(
            std::string(operation) + " failed");
    }
}

fs::path NewRoot(std::string_view suffix) {
    const auto root = fs::temp_directory_path() /
        ("a0-pc-direct-" + std::string(suffix) + "-" + NewRunId());
    fs::create_directories(root);
    return root;
}

void WriteSolidJpeg(
    const fs::path& path,
    std::uint32_t width,
    std::uint32_t height) {
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    try {
        CheckHr(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)), "create factory");
        CheckHr(factory->CreateStream(&stream), "create stream");
        CheckHr(stream->InitializeFromFilename(
            path.c_str(), GENERIC_WRITE), "open output");
        CheckHr(factory->CreateEncoder(
            GUID_ContainerFormatJpeg, nullptr, &encoder),
            "create encoder");
        CheckHr(encoder->Initialize(
            stream, WICBitmapEncoderNoCache), "initialize encoder");
        CheckHr(encoder->CreateNewFrame(
            &frame, &properties), "create frame");
        CheckHr(frame->Initialize(properties), "initialize frame");
        CheckHr(frame->SetSize(width, height), "set dimensions");
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        CheckHr(frame->SetPixelFormat(&format), "set pixel format");
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(width) * height * 3U, 0x55U);
        CheckHr(frame->WritePixels(
            height, width * 3U, static_cast<UINT>(pixels.size()),
            pixels.data()), "write pixels");
        CheckHr(frame->Commit(), "commit frame");
        CheckHr(encoder->Commit(), "commit encoder");
    } catch (...) {
        if (properties) properties->Release();
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        throw;
    }
    properties->Release();
    frame->Release();
    encoder->Release();
    stream->Release();
    factory->Release();
}

std::vector<unsigned char> ReadBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

SdkCameraStatus GoodStatus() {
    SdkCameraStatus status;
    status.live_view_status_available = true;
    status.live_view_status = "off";
    status.file_type.available = false;
    status.file_type.probe_state = "not-advertised";
    status.compression_level.available = true;
    status.compression_level.current_label = "JPEG Fine";
    status.image_size.available = true;
    status.image_size.current_label = "L (7360 x 4912)";
    return status;
}

class RecordingPcDirectTransport final : public IPcDirectCaptureTransport {
public:
    void OpenPcDirect(
        std::string_view stable_identity,
        std::chrono::seconds) override {
        ++open_count;
        if (stable_identity != expected_identity) {
            throw TransportError(
                "wrong_camera", "unexpected current-session camera");
        }
        if (open_failure) throw *open_failure;
        open = true;
    }

    SdkCameraStatus ProbeOpenCaptureSessionStatus(
        std::chrono::seconds) override {
        if (!open) throw TransportError("session_not_open", "not open");
        return status;
    }

    std::string BeginPcDirectBaseline(
        std::chrono::seconds) override {
        if (!open) throw TransportError("session_not_open", "not open");
        ++baseline_count;
        return "fixed-session-baseline";
    }

    std::vector<ImageCandidate> CaptureAndDownloadToPc(
        std::string_view baseline,
        std::chrono::seconds,
        std::chrono::seconds,
        std::chrono::seconds) override {
        if (!open || baseline != "fixed-session-baseline") {
            throw TransportError(
                "session_mismatch", "baseline is from another session");
        }
        ++capture_count;
        if (capture_failure) throw *capture_failure;
        return candidates;
    }

    void ClosePcDirect(std::chrono::seconds) override {
        ++close_count;
        open = false;
        if (restore_failure) throw *restore_failure;
    }

    std::string expected_identity{"session-camera"};
    SdkCameraStatus status{GoodStatus()};
    std::vector<ImageCandidate> candidates;
    std::optional<TransportError> open_failure;
    std::optional<TransportError> capture_failure;
    std::optional<TransportError> restore_failure;
    int open_count{};
    int baseline_count{};
    int capture_count{};
    int close_count{};
    bool open{};
};

PcDirectCaptureRequest Request() {
    PcDirectCaptureRequest request;
    request.transaction_id = "tx-fixed";
    request.camera_alias = "CAM-A";
    request.stable_identity = "session-camera";
    request.expected_width = 16;
    request.expected_height = 8;
    return request;
}

PcDirectCaptureResult Run(
    const fs::path& root,
    RecordingPcDirectTransport& transport,
    const PcDirectCaptureRequest& request = Request()) {
    EvidenceWriter evidence(root / "artifacts", "run-fixed", "test");
    return ExecutePcDirectCaptureOnce(transport, evidence, request);
}

void CheckSingleAttemptSafety(
    const RecordingPcDirectTransport& transport,
    const PcDirectCaptureResult& result) {
    Check(transport.open_count == 1, "PC-direct source must open once");
    Check(transport.capture_count <= 1, "capture must never retry");
    Check(transport.close_count <= 1, "restore/close must never retry");
    Check(result.automatic_retry_count == 0,
        "reported automatic retry count must remain zero");
    Check(!result.card_fallback_attempted,
        "card capture fallback must remain disabled");
}

void TestApprovedD810ProfileAndNegativeVariantsStopBeforeCapture() {
    const auto root = NewRoot("profile-gate");
    const auto fixture = root / "fixture.jpg";
    WriteSolidJpeg(fixture, 16, 8);
    {
        RecordingPcDirectTransport transport;
        transport.candidates = {{"sdk-item.jpg", ReadBytes(fixture), true}};
        const auto result = Run(root / "known-d810", transport);
        Check(result.transaction.terminal_state == "Complete",
            "D810 FileType not-advertised with exact JPEG Fine and L must pass");
    }
    const auto expect_rejected = [&](SdkCameraStatus status,
                                      std::string_view suffix,
                                      std::string_view category) {
        RecordingPcDirectTransport transport;
        transport.status = std::move(status);
        const auto result = Run(root / std::string(suffix), transport);
        Check(result.transaction.terminal_state == "FailedPartial" &&
                result.transaction.error_category == category &&
                transport.capture_count == 0 &&
                result.save_media_restore_confirmed,
            "invalid PC-direct profile must stop before shutter and restore SaveMedia");
    };
    {
        auto status = GoodStatus();
        status.file_type.available = true;
        status.file_type.probe_state = "available";
        status.file_type.current_label = "RAW + JPEG Fine";
        expect_rejected(
            std::move(status), "raw-plus-jpeg",
            "pc_direct_jpeg_fine_not_confirmed");
    }
    {
        auto status = GoodStatus();
        status.compression_level.current_label = "JPEG Normal";
        expect_rejected(
            std::move(status), "jpeg-normal",
            "pc_direct_jpeg_fine_not_confirmed");
    }
    {
        auto status = GoodStatus();
        status.image_size.current_label = "M(5520*3680)";
        expect_rejected(
            std::move(status), "medium-size",
            "pc_direct_image_size_l_not_confirmed");
    }
    {
        auto status = GoodStatus();
        status.compression_level.available = false;
        status.compression_level.current_label.reset();
        expect_rejected(
            std::move(status), "missing-compression",
            "pc_direct_jpeg_fine_not_confirmed");
    }
    fs::remove_all(root);
}

void TestNormalCaptureFullyDecodesPersistsAndRestores() {
    const auto root = NewRoot("normal");
    const auto fixture = root / "fixture.jpg";
    WriteSolidJpeg(fixture, 16, 8);
    RecordingPcDirectTransport transport;
    transport.candidates = {{"sdk-item.jpg", ReadBytes(fixture), true}};
    EvidenceWriter evidence(root / "artifacts", "run-fixed", "test");
    const auto result = ExecutePcDirectCaptureOnce(
        transport, evidence, Request());
    Check(result.transaction.terminal_state == "Complete",
        "normal PC-direct capture must complete");
    Check(result.downloaded_jpeg_fully_decoded &&
            result.persisted_jpeg_fully_decoded,
        "downloaded and reread JPEGs must both be fully decoded");
    Check(result.save_media_restore_attempted &&
            result.save_media_restore_confirmed,
        "SaveMedia restoration must be attempted and confirmed");
    Check(result.transaction.frames.size() == 1 &&
            result.transaction.frames.front().success &&
            fs::is_regular_file(result.transaction.frames.front().path),
        "verified original.jpg must remain available");
    CheckSingleAttemptSafety(transport, result);
    std::ofstream(
        evidence.RunRoot() / "pc-direct-summary.json",
        std::ios::binary) << "{\"cardUnchanged\":true}\n";
    evidence.GenerateRedactedReport(root / "reports");
    Check(fs::is_regular_file(
              root / "reports" / "run-fixed" /
                  "pc-direct-summary.json"),
        "redacted report must include the anonymous PC-direct summary");
    fs::remove_all(root);
}

void TestRedactedReportPathCannotExposeAbsoluteLocation() {
    const auto root = NewRoot("redacted-report-path");
    const auto run_root = root / "artifacts" / "run-fixed";
    const auto inside = run_root / "pc-direct-1" / "CAM-A" /
        "original.jpg";
    const auto outside = root.parent_path() /
        "sentinel-private-absolute-location" / "original.jpg";
    const auto relative = PcDirectReportRelativePath(run_root, inside);
    const auto rejected = PcDirectReportRelativePath(run_root, outside);
    Check(relative ==
              "pc-direct-1/CAM-A/original.jpg" && !rejected,
        "redacted PC-direct evidence must retain only a relative path below the run root");
    fs::remove_all(root);
}

void TestNoNotificationFailsWithoutRetry() {
    const auto root = NewRoot("no-notification");
    RecordingPcDirectTransport transport;
    const auto result = Run(root, transport);
    Check(result.transaction.terminal_state == "FailedPartial" &&
            result.transaction.error_category == "no_candidate",
        "no SDK item notification must fail as no_candidate");
    Check(result.save_media_restore_confirmed,
        "no-candidate failure must still restore SaveMedia");
    CheckSingleAttemptSafety(transport, result);
    fs::remove_all(root);
}

void TestDuplicateAndDelayedCandidatesRemainUnattributed() {
    const auto root = NewRoot("candidate-boundaries");
    const auto fixture = root / "fixture.jpg";
    WriteSolidJpeg(fixture, 16, 8);
    const auto jpeg = ReadBytes(fixture);
    {
        RecordingPcDirectTransport transport;
        transport.candidates = {
            {"first.jpg", jpeg, true}, {"duplicate.jpg", jpeg, true}};
        const auto result = Run(root / "duplicate", transport);
        Check(result.transaction.error_category == "ambiguous_candidates",
            "duplicate candidates must remain ambiguous");
        CheckSingleAttemptSafety(transport, result);
    }
    {
        RecordingPcDirectTransport transport;
        transport.candidates = {{"delayed.jpg", jpeg, false}};
        const auto result = Run(root / "delayed", transport);
        Check(result.transaction.error_category == "late_candidate",
            "delayed candidate must not be auto-adopted");
        CheckSingleAttemptSafety(transport, result);
    }
    fs::remove_all(root);
}

void TestDisconnectAndOtherSessionFailClosed() {
    const auto root = NewRoot("session-failures");
    {
        RecordingPcDirectTransport transport;
        transport.capture_failure.emplace(
            "device_disconnected", "camera disconnected");
        const auto result = Run(root / "disconnect", transport);
        Check(result.transaction.error_category == "device_disconnected",
            "disconnect must retain its controlled category");
        Check(result.save_media_restore_attempted,
            "disconnect must attempt restoration once");
        CheckSingleAttemptSafety(transport, result);
    }
    {
        RecordingPcDirectTransport transport;
        transport.expected_identity = "other-session-camera";
        const auto result = Run(root / "other-session", transport);
        Check(result.transaction.error_category == "wrong_camera",
            "another camera/session must fail before capture");
        Check(transport.capture_count == 0 && transport.close_count == 0,
            "failed source selection must not capture or close an unopened source");
        CheckSingleAttemptSafety(transport, result);
    }
    fs::remove_all(root);
}

void TestIncompleteJpegAndSaveFailureRetainFailure() {
    const auto root = NewRoot("persistence-failures");
    {
        RecordingPcDirectTransport transport;
        transport.candidates = {
            {"incomplete.jpg", {0xFFU, 0xD8U, 0xFFU, 0xD9U}, true}};
        const auto result = Run(root / "incomplete", transport);
        Check(result.transaction.error_category == "invalid_jpeg_decode",
            "marker-only incomplete JPEG must fail full decode");
        Check(!result.downloaded_jpeg_fully_decoded,
            "incomplete JPEG cannot be reported as decoded");
        CheckSingleAttemptSafety(transport, result);
    }
    {
        const auto save_root = root / "save";
        const auto fixture = root / "fixture.jpg";
        WriteSolidJpeg(fixture, 16, 8);
        RecordingPcDirectTransport transport;
        transport.candidates = {{"sdk-item.jpg", ReadBytes(fixture), true}};
        EvidenceWriter evidence(
            save_root / "artifacts", "run-fixed", "test");
        const auto existing = evidence.RunRoot() /
            "tx-fixed" / "CAM-A" / "original.jpg";
        fs::create_directories(existing.parent_path());
        std::ofstream(existing, std::ios::binary) << "existing";
        const auto result = ExecutePcDirectCaptureOnce(
            transport, evidence, Request());
        Check(result.transaction.terminal_state == "FailedPartial",
            "non-overwrite save collision must fail");
        Check(ReadBytes(existing) ==
                std::vector<unsigned char>({'e','x','i','s','t','i','n','g'}),
            "save failure must not overwrite the existing file");
        Check(result.save_media_restore_confirmed,
            "save failure must still restore SaveMedia");
        CheckSingleAttemptSafety(transport, result);
    }
    fs::remove_all(root);
}

void TestRestoreFailureOverridesSuccessAndRetainsOriginal() {
    const auto root = NewRoot("restore-failure");
    const auto fixture = root / "fixture.jpg";
    WriteSolidJpeg(fixture, 16, 8);
    RecordingPcDirectTransport transport;
    transport.candidates = {{"sdk-item.jpg", ReadBytes(fixture), true}};
    transport.restore_failure.emplace(
        "save_media_restore_failed", "restore failed");
    const auto result = Run(root, transport);
    Check(result.transaction.terminal_state == "FailedPartial" &&
            result.transaction.error_category == "save_media_restore_failed",
        "restore failure must prevent a Complete result");
    Check(result.save_media_restore_attempted &&
            !result.save_media_restore_confirmed,
        "restore failure must remain explicit");
    Check(result.transaction.frames.size() == 1 &&
            result.transaction.frames.front().success &&
            fs::is_regular_file(result.transaction.frames.front().path),
        "verified PC original must remain after restore failure");
    CheckSingleAttemptSafety(transport, result);
    fs::remove_all(root);
}

void TestPostCloseCardValidationRunsAfterRestoreAndFailsClosed() {
    const auto root = NewRoot("post-close-validation");
    const auto fixture = root / "fixture.jpg";
    WriteSolidJpeg(fixture, 16, 8);
    {
        RecordingPcDirectTransport transport;
        transport.candidates = {{"sdk-item.jpg", ReadBytes(fixture), true}};
        EvidenceWriter evidence(
            root / "success-artifacts", "run-fixed", "test");
        bool validation_called = false;
        const auto result = ExecutePcDirectCaptureOnce(
            transport,
            evidence,
            Request(),
            [&] {
                validation_called = true;
                Check(!transport.open && transport.close_count == 1,
                    "card validation must run only after SDK restore/close");
            });
        Check(validation_called &&
                result.transaction.terminal_state == "Complete",
            "successful post-close card validation must permit completion");
        CheckSingleAttemptSafety(transport, result);
    }
    {
        RecordingPcDirectTransport transport;
        transport.candidates = {{"sdk-item.jpg", ReadBytes(fixture), true}};
        EvidenceWriter evidence(
            root / "changed-artifacts", "run-fixed", "test");
        const auto result = ExecutePcDirectCaptureOnce(
            transport,
            evidence,
            Request(),
            [] {
                throw TransportError(
                    "camera_payload_changed",
                    "anonymous card fingerprint changed");
            });
        Check(result.transaction.terminal_state == "FailedPartial" &&
                result.transaction.error_category ==
                    "camera_payload_changed",
            "changed card fingerprint must fail after one capture without retry");
        Check(result.save_media_restore_confirmed,
            "card mismatch must be observed only after confirmed restore");
        Check(result.transaction.frames.size() == 1 &&
                result.transaction.frames.front().success,
            "verified PC original must remain when card invariance fails");
        CheckSingleAttemptSafety(transport, result);
    }
    fs::remove_all(root);
}

} // namespace

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        std::cerr << "FAIL: COM initialization failed\n";
        return 1;
    }
    try {
        TestNormalCaptureFullyDecodesPersistsAndRestores();
        TestRedactedReportPathCannotExposeAbsoluteLocation();
        TestApprovedD810ProfileAndNegativeVariantsStopBeforeCapture();
        TestNoNotificationFailsWithoutRetry();
        TestDuplicateAndDelayedCandidatesRemainUnattributed();
        TestDisconnectAndOtherSessionFailClosed();
        TestIncompleteJpegAndSaveFailureRetainFailure();
        TestRestoreFailureOverridesSuccessAndRetainsOriginal();
        TestPostCloseCardValidationRunsAfterRestoreAndFailsClosed();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (failures != 0) return 1;
    std::cout << "PC-direct capture contracts passed\n";
    return 0;
}
