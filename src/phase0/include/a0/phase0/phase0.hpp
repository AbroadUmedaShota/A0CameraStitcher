#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace a0::phase0 {

class TransportError final : public std::runtime_error {
public:
    TransportError(std::string category, std::string message);
    [[nodiscard]] const std::string& Category() const noexcept;

private:
    std::string category_;
};

struct Timeouts {
    std::chrono::seconds open{10};
    std::chrono::seconds image_event{15};
    std::chrono::seconds download{60};
    std::chrono::seconds close{10};
    std::chrono::seconds transaction_watchdog{180};
};

class OperatorGate final {
public:
    OperatorGate(
        std::filesystem::path artifacts_root,
        std::string safe_name,
        std::chrono::seconds timeout,
        std::string scenario = {},
        std::string stage = {});
    [[nodiscard]] static bool IsSafeName(std::string_view value) noexcept;
    [[nodiscard]] const std::filesystem::path& ReadyPath() const noexcept;
    [[nodiscard]] std::filesystem::path AwaitContinue(std::ostream& output);

private:
    std::filesystem::path gate_directory_;
    std::filesystem::path ready_path_;
    std::filesystem::path continue_path_;
    std::string safe_name_;
    std::string scenario_;
    std::string stage_;
    std::chrono::seconds timeout_;
    bool armed_{false};
};

struct CameraInfo {
    std::string model;
    std::string firmware;
    std::string shooting_mode;
    std::string stable_identity;
};

struct SdkCameraStatus {
    std::string firmware{"unknown"};
    std::string live_view_status{"unknown"};
    std::string live_view_selector{"unknown"};
    bool live_view_status_available{false};
    bool live_view_selector_available{false};
    std::optional<std::uint32_t> live_view_prohibit_mask;
    struct SettingCapability {
        bool available{false};
        std::string cap_type{"unsupported"};
        std::string probe_state{"not-advertised"};
        std::string value_type{"unsupported"};
        std::optional<std::uint32_t> current_value;
        std::optional<std::uint32_t> current_index;
        std::optional<std::string> current_label;
        std::vector<std::uint32_t> numeric_values;
        std::vector<std::string> string_values;
    };
    SettingCapability file_type;
    SettingCapability compression_level;
    SettingCapability image_size;
    SettingCapability exposure_mode;
    SettingCapability shutter_speed;
    SettingCapability aperture;
    SettingCapability sensitivity;
    SettingCapability wb_mode;
    SettingCapability focus_mode;
};

// Anonymous aggregate for a read-only inspection of the dedicated camera
// spool. Object IDs, names, extensions, dates, and device identifiers must
// never cross this boundary.
struct WpdSpoolStatusSummary {
    std::size_t payload_object_count{};
    bool read_only_observation{true};
    bool capture_command_sent{false};
    bool vendor_operation_executed{false};
    bool camera_settings_changed{false};
    bool camera_object_delete_attempted{false};
    int wpd_sessions_closed{};
    std::string terminal_state{"InProgress"};
    std::string failed_stage;
};

struct WpdStatusSummary {
    std::string target_validation_state;
    std::string command_options_hresult;
    std::string option_value_hresult;
    std::size_t functional_object_count{};
    std::size_t valid_object_id_count{};
    std::size_t compatible_target_count{};
    bool valid_object_ids_option_present{};
    bool selected_target{};
    std::string vendor_opcode_validation_state;
    std::string supported_commands_hresult;
    std::string vendor_opcode_query_send_hresult;
    std::string vendor_opcode_query_common_hresult;
    bool wpd_still_image_capture_command_advertised{};
    bool vendor_opcode_query_advertised{};
    bool read_only_command_sent{};
    bool vendor_opcode_collection_available{};
    std::size_t vendor_opcode_item_count{};
    std::size_t vendor_opcode_unique_count{};
    bool vendor_capture_9207_advertised{};
    bool standard_opcode_100e_advertisement_available{};
    std::string standard_opcode_100e_advertisement_state;
    std::string requested_access{"read-only"};
    bool read_only_access{true};
};

struct WpdCorrelationRunSummary {
    int sample_count{};
    int device_datetime_available_count{};
    int reopen_advance_count{};
    int reopen_equal_count{};
    int reopen_regress_count{};
    std::size_t jpeg_count{};
    std::size_t dated_jpeg_count{};
    std::size_t latest_date_less_than_device_count{};
    std::size_t latest_date_equal_device_count{};
    std::size_t latest_date_greater_than_device_count{};
    bool read_only_observation{true};
    bool capture_command_sent{false};
    bool vendor_operation_executed{false};
    bool camera_settings_changed{false};
    bool camera_object_delete_attempted{false};
    int wpd_sessions_closed{};
    std::string terminal_state{"InProgress"};
    std::string failed_stage;
    int failed_sample{};
};

// Deliberately anonymous: no WPD date value, object ID, object name, or
// device identifier crosses this transport boundary.
struct WpdCorrelationSample {
    bool device_datetime_available{};
    bool reopen_datetime_advanced{};
    bool reopen_datetime_equal{};
    bool reopen_datetime_regressed{};
    std::size_t jpeg_count{};
    std::size_t dated_jpeg_count{};
    std::size_t latest_date_less_than_device{};
    std::size_t latest_date_equal_device{};
    std::size_t latest_date_greater_than_device{};
    bool latest_jpeg_date_available{};
};

class ICorrelationObservationTransport {
public:
    virtual ~ICorrelationObservationTransport() = default;
    virtual void OpenReadOnlyObservation(std::string_view stable_identity, std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual WpdCorrelationSample ReadCorrelationSample() = 0;
    virtual void Close(std::chrono::seconds timeout) = 0;
};

struct ImageCandidate {
    std::string source_name;
    std::vector<unsigned char> bytes;
    bool attributable{true};
    // In-memory capability issued by the recovery transport for exactly one
    // object. It must never cross the evidence/report boundary.
    std::string cleanup_token;
};

class UncertainDispatchError final : public std::runtime_error {
public:
    UncertainDispatchError(std::string category, std::string message, std::vector<ImageCandidate> candidates);
    [[nodiscard]] const std::string& Category() const noexcept;
    [[nodiscard]] const std::vector<ImageCandidate>& Candidates() const noexcept;

private:
    std::string category_;
    std::vector<ImageCandidate> candidates_;
};

class ICameraTransport {
public:
    virtual ~ICameraTransport() = default;
    [[nodiscard]] virtual std::string SdkVersion() const = 0;
    [[nodiscard]] virtual std::vector<CameraInfo> Enumerate() = 0;
    virtual void Open(std::string_view stable_identity, std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::string Baseline(std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) = 0;
    virtual void Close(std::chrono::seconds timeout) = 0;
};

// Hybrid capture deliberately separates the one SDK card capture from the
// later WPD observation.  Neither operation is a WPD shutter command.
class ICardCaptureTransport {
public:
    virtual ~ICardCaptureTransport() = default;
    virtual void CaptureToCard(std::chrono::seconds image_event_timeout,
                               std::chrono::seconds transaction_timeout) = 0;
};

class IPostCardObservationTransport {
public:
    virtual ~IPostCardObservationTransport() = default;
    [[nodiscard]] virtual std::string BeginPostCardObservation(std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::vector<ImageCandidate> ObserveAndDownloadPostCardCapture(
        std::string_view token,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) = 0;
    virtual void DeleteRecoveredObject(
        std::string_view cleanup_token,
        std::chrono::seconds timeout) = 0;
    virtual void VerifyJpegSpoolEmpty(std::chrono::seconds timeout) = 0;
    virtual void AbandonPostCardObservation(std::string_view token) noexcept = 0;
};

class ILiveViewTransport {
public:
    virtual ~ILiveViewTransport() = default;
    virtual void OpenLiveView(std::string_view stable_identity, std::chrono::seconds timeout) = 0;
    virtual void StartLiveView(std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::vector<unsigned char> ReadLiveViewFrame(std::chrono::seconds timeout) = 0;
    virtual void StopLiveView(std::chrono::seconds timeout) = 0;
    virtual void Close(std::chrono::seconds timeout) = 0;
};

class IdentityMap {
public:
    explicit IdentityMap(std::filesystem::path path);
    [[nodiscard]] std::optional<std::string> FindAlias(std::string_view stable_identity) const;
    [[nodiscard]] std::string AssignNext(std::string_view stable_identity);
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;

private:
    void Load();
    void Save() const;
    std::filesystem::path path_;
    std::optional<std::string> cam_a_;
    std::optional<std::string> cam_b_;
};

struct FrameEvidence {
    bool success{false};
    std::string camera_alias;
    std::filesystem::path path;
    std::string sha256;
    std::uintmax_t bytes{0};
    std::string error_category;
    std::string error_detail;
};

struct TransactionResult {
    std::string run_id;
    std::string transaction_id;
    std::string terminal_state;
    std::vector<FrameEvidence> frames;
    std::string error_category;
    std::string error_detail;
    std::chrono::milliseconds duration{0};
    bool spool_empty_before_capture{false};
    bool camera_card_delete_attempted{false};
    bool camera_card_delete_succeeded{false};
    bool spool_empty_after_cleanup{false};
};

struct LiveViewProbeResult {
    std::vector<unsigned char> last_frame;
    std::chrono::milliseconds duration{0};
    int frames{0};
};

struct LiveViewHandoffResult {
    std::string terminal_state;
    std::string error_category;
    std::string error_detail;
    TransactionResult capture;
    LiveViewProbeResult before;
    LiveViewProbeResult after;
    bool wpd_capture_started{false};
    bool wpd_capture_complete{false};
    bool resume_attempted{false};
};

struct LiveViewHandoffRunSummary {
    int requested{0};
    int attempted{0};
    int completed{0};
    int failures{0};
    int spool_empty_before_count{0};
    int camera_card_delete_attempted_count{0};
    int camera_card_delete_succeeded_count{0};
    int spool_empty_after_count{0};
    std::string terminal_state{"InProgress"};
    std::string last_handoff_state;
    std::string last_error_category;
    std::string last_error_detail;
};

struct HybridCaptureRunSummary {
    int requested{0};
    int attempted{0};
    int completed{0};
    int failures{0};
    std::string terminal_state{"InProgress"};
    std::string last_state;
    bool pc_original_canonical{true};
    bool camera_card_transient{true};
    int spool_empty_before_count{0};
    int camera_card_delete_attempted_count{0};
    int camera_card_delete_succeeded_count{0};
    int spool_empty_after_count{0};
    bool automatic_retry{false};
    bool exclusive_camera_control_confirmed{false};
    bool dedicated_spool_scope_confirmed{false};
    bool exact_object_delete_confirmed{false};
};

struct HybridFaultRunSummary {
    std::string scenario;
    std::string gate_stage{"after_sdk_close_before_wpd_recovery"};
    std::string transaction_state;
    std::string error_category;
    std::string acceptance_state{"InProgress"};
    bool spool_empty_before_capture{false};
    bool pc_original_persisted{false};
    bool camera_object_delete_attempted{false};
    bool automatic_retry{false};
    bool recovery_requires_new_transaction{true};
};

[[nodiscard]] std::optional<std::string> ValidateHybridCaptureArguments(
    std::string_view command,
    int count,
    bool exclusive_camera_control_confirmed,
    bool dedicated_spool_scope_confirmed,
    bool exact_object_delete_confirmed) noexcept;

[[nodiscard]] std::filesystem::path PersistSdkStatusSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const CameraInfo& camera,
    const SdkCameraStatus& status);
[[nodiscard]] std::filesystem::path PersistWpdStatusSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const CameraInfo& camera,
    const WpdStatusSummary& status);
[[nodiscard]] std::filesystem::path PersistWpdSpoolStatusSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const WpdSpoolStatusSummary& status);
[[nodiscard]] std::filesystem::path PersistWpdCorrelationSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const WpdCorrelationRunSummary& status);
[[nodiscard]] std::filesystem::path PersistHybridFaultSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const HybridFaultRunSummary& status);
[[nodiscard]] std::optional<std::string> ValidateWpdCorrelationArguments(
    std::string_view command,
    bool samples_explicit,
    bool interval_explicit,
    int samples,
    int interval_ms) noexcept;
[[nodiscard]] WpdCorrelationRunSummary ExecuteWpdCorrelationSamples(
    ICorrelationObservationTransport& transport,
    std::string_view stable_identity,
    int samples,
    int interval_ms);

class EvidenceWriter {
public:
    EvidenceWriter(std::filesystem::path artifacts_root, std::string run_id, std::string sdk_version);
    [[nodiscard]] FrameEvidence PersistExactlyOne(
        std::string_view transaction_id,
        std::string_view camera_alias,
        const std::vector<ImageCandidate>& candidates,
        std::optional<std::chrono::steady_clock::time_point> transaction_deadline = std::nullopt,
        const std::function<void()>& before_atomic_rename = {});
    [[nodiscard]] FrameEvidence QuarantineUnconfirmed(
        std::string_view transaction_id,
        std::string_view camera_alias,
        const std::vector<ImageCandidate>& candidates,
        std::string_view command_detail);
    void RecordCamera(std::string_view camera_alias, std::string_view firmware);
    void RecordState(std::string_view transaction_id, std::string_view state, std::string_view camera_alias = {},
                     std::string_view error_detail = {});
    void RecordResult(const TransactionResult& result);
    [[nodiscard]] const std::string& RunId() const noexcept;
    [[nodiscard]] const std::filesystem::path& RunRoot() const noexcept;
    void GenerateRedactedReport(const std::filesystem::path& report_root) const;

private:
    void AppendEvent(std::string_view json_line);
    std::filesystem::path artifacts_root_;
    std::filesystem::path run_root_;
    std::string run_id_;
    std::string sdk_version_;
    std::size_t transaction_count_{0};
    std::size_t complete_count_{0};
    std::size_t failed_count_{0};
};

class CaptureCoordinator {
public:
    CaptureCoordinator(ICameraTransport& transport, EvidenceWriter& evidence, Timeouts timeouts = {});
    [[nodiscard]] TransactionResult CaptureSingle(std::string_view alias, std::string_view stable_identity);
    [[nodiscard]] TransactionResult CapturePair(std::string_view cam_a_identity, std::string_view cam_b_identity);

private:
    [[nodiscard]] FrameEvidence CaptureOne(
        std::string_view transaction_id,
        std::string_view alias,
        std::string_view stable_identity,
        std::string_view capture_state,
        std::string_view persist_state,
        std::optional<std::chrono::steady_clock::time_point> transaction_deadline = std::nullopt);
    [[nodiscard]] std::string NextTransactionId();
    ICameraTransport& transport_;
    EvidenceWriter& evidence_;
    Timeouts timeouts_;
    bool active_{false};
    unsigned long long sequence_{0};
};

[[nodiscard]] LiveViewProbeResult AcquireLiveViewFrames(
    ILiveViewTransport& transport,
    std::string_view stable_identity,
    int frames,
    int interval_ms,
    int duration_seconds = 0);
[[nodiscard]] LiveViewHandoffResult ExecuteLiveViewHandoffOnce(
    ILiveViewTransport& live_view_transport,
    std::string_view live_view_identity,
    const std::function<TransactionResult()>& capture_once,
    EvidenceWriter& evidence,
    std::string_view handoff_id,
    std::string_view camera_alias,
    int frames,
    int interval_ms,
    std::chrono::milliseconds resume_delay = std::chrono::milliseconds{1000});
[[nodiscard]] std::filesystem::path PersistLiveViewHandoffSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const LiveViewHandoffRunSummary& result);
[[nodiscard]] std::filesystem::path PersistHybridCaptureSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const HybridCaptureRunSummary& result);
[[nodiscard]] TransactionResult ExecuteHybridCaptureOnce(
    ICameraTransport& wpd_session,
    IPostCardObservationTransport& wpd,
    ICameraTransport& sdk_session,
    ICardCaptureTransport& sdk,
    EvidenceWriter& evidence,
    std::string_view camera_alias,
    std::string_view wpd_identity,
    std::string_view sdk_identity,
    Timeouts timeouts = {},
    const std::function<void()>& before_wpd_recovery = {},
    const std::function<void()>& before_pc_original_rename = {});

[[nodiscard]] std::string NewRunId();
[[nodiscard]] std::filesystem::path DefaultIdentityMapPath();
[[nodiscard]] bool IsValidJpeg(const std::vector<unsigned char>& bytes);
[[nodiscard]] std::vector<unsigned char> ExtractD810LiveViewJpeg(
    const std::vector<unsigned char>& frame_with_header);
[[nodiscard]] std::string Sha256Hex(const std::vector<unsigned char>& bytes);
[[nodiscard]] std::optional<std::vector<std::string>> ParsePackedStringLabels(
    const std::vector<unsigned char>& bytes,
    std::size_t maximum_labels,
    std::size_t maximum_bytes);

} // namespace a0::phase0
