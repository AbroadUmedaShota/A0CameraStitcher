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

enum class PcDirectObservation {
    CallbackRegistered,
    BaselineReady,
    CaptureCommandStarted,
    CaptureCommandAccepted,
    ForcedEnumerationSucceeded,
    ForcedEnumerationFailed,
    AddChildNotification,
    EnumeratedCandidate,
    DuplicateAddChildNotification,
    CaptureComplete,
    AddChildInCard,
    CandidateRemoved,
    IgnoredForeignEvent,
    SessionClosed,
};

enum class PcDirectTerminalSubreason {
    SdramNotEmpty,
    PreDispatchCandidate,
    CallbackWindowInvalid,
    CaptureCommandFailed,
    TransactionWatchdogExpired,
    CaptureCompleteMissing,
    SdramItemMissing,
    CardItemOnly,
    CandidateRemoved,
    AttributionFailed,
    ImageDownloadFailed,
    ReceivedExactlyOneItem,
};

enum class PcDirectSaveMediaValue {
    Card,
    Sdram,
    CardAndSdram,
    Unknown,
};

enum class PcDirectCommandResult {
    NoError,
    Pending,
    Error,
};

struct PcDirectTransportDiagnostics {
    bool measurement_started{};
    std::optional<bool> callback_registered;
    std::optional<bool> callback_active_before_capture;
    std::optional<bool> session_closed;
    std::optional<std::size_t> baseline_children_count;
    std::optional<std::size_t> raw_add_child_count;
    std::optional<std::size_t> raw_remove_child_count;
    std::optional<std::size_t> raw_capture_complete_count;
    std::optional<std::size_t> baseline_hit_count;
    std::vector<std::size_t> children_count_sequence;
    std::optional<std::size_t> children_count_transition_count;
    std::optional<bool> children_count_sequence_truncated;
    std::optional<PcDirectSaveMediaValue> save_media_original;
    std::optional<PcDirectSaveMediaValue> save_media_selected;
    std::optional<PcDirectSaveMediaValue> save_media_readback;
    std::optional<std::size_t> save_media_selection_set_count;
    std::optional<PcDirectCommandResult> capture_cap_start_immediate_result;
    std::optional<PcDirectCommandResult> capture_cap_start_completion_result;
    std::optional<std::int64_t> capture_cap_start_duration_ms;
    std::optional<std::size_t> capture_complete_count;
    std::optional<std::size_t> add_child_notification_count;
    std::optional<std::size_t> forced_enumeration_attempt_count;
    std::optional<std::size_t> forced_enumeration_success_count;
    std::optional<std::size_t> forced_enumeration_failure_count;
    std::optional<std::size_t> distinct_notified_candidate_count;
    std::optional<std::size_t> distinct_enumerated_candidate_count;
    std::optional<std::size_t> duplicate_candidate_notification_count;
    std::optional<std::size_t> removed_candidate_count;
    std::optional<std::size_t> add_child_in_card_count;
    std::optional<std::size_t> ignored_event_count;
    std::optional<PcDirectTerminalSubreason> terminal_subreason;
    std::vector<PcDirectObservation> observation_order;
};

[[nodiscard]] std::string_view ToString(PcDirectObservation observation) noexcept;
[[nodiscard]] std::string_view ToString(PcDirectTerminalSubreason subreason) noexcept;
[[nodiscard]] std::string_view ToString(PcDirectSaveMediaValue value) noexcept;
[[nodiscard]] std::string_view ToString(PcDirectCommandResult result) noexcept;
[[nodiscard]] std::string SerializePcDirectTransportDiagnostics(
    const PcDirectTransportDiagnostics& diagnostics);

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
    [[nodiscard]] virtual PcDirectTransportDiagnostics
        InspectPcDirectDiagnostics() const = 0;
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
    PcDirectTransportDiagnostics transport_diagnostics;
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
