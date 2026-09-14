#pragma once

#include "a0/phase0/phase0.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a0::phase0 {

class IPcDirectCaptureTransport {
public:
    virtual ~IPcDirectCaptureTransport() = default;
    virtual void OpenPcDirect(
        std::string_view stable_identity,
        std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual SdkCameraStatus ProbeOpenCaptureSessionStatus(
        std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::string BeginPcDirectBaseline(
        std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::vector<ImageCandidate> CaptureAndDownloadToPc(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) = 0;
    virtual void ClosePcDirect(std::chrono::seconds timeout) = 0;
};

struct DecodedJpegInfo {
    std::uint32_t width{};
    std::uint32_t height{};
};

// Uses Windows Imaging Component and reads every decoded pixel. Marker-only
// JPEG validation is intentionally insufficient for PC-direct originals.
[[nodiscard]] DecodedJpegInfo FullyDecodeJpeg(
    const std::vector<unsigned char>& bytes,
    std::uint32_t expected_width,
    std::uint32_t expected_height);

struct PcDirectCaptureRequest {
    std::string transaction_id;
    std::string camera_alias;
    std::string stable_identity;
    std::uint32_t expected_width{7360};
    std::uint32_t expected_height{4912};
    Timeouts timeouts{};
};

struct PcDirectCaptureResult {
    TransactionResult transaction;
    std::size_t candidate_count{};
    bool capture_attempted{};
    bool downloaded_jpeg_fully_decoded{};
    bool persisted_jpeg_fully_decoded{};
    bool save_media_restore_attempted{};
    bool save_media_restore_confirmed{};
    bool card_fallback_attempted{};
    int automatic_retry_count{};
};

// Returns only a report-safe path below the run root. Absolute or escaping
// paths are omitted from redacted evidence.
[[nodiscard]] std::optional<std::string> PcDirectReportRelativePath(
    const std::filesystem::path& run_root,
    const std::filesystem::path& original_path);

// Executes exactly one capture attempt in one fixed transaction. The function
// never invokes WPD, never retries, and never falls back to card capture.
[[nodiscard]] PcDirectCaptureResult ExecutePcDirectCaptureOnce(
    IPcDirectCaptureTransport& transport,
    EvidenceWriter& evidence,
    const PcDirectCaptureRequest& request,
    const std::function<void()>& after_sdk_close_validation = {});

} // namespace a0::phase0
