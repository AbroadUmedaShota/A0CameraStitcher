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
#include <utility>
#include <vector>

namespace a0::phase0 {

struct ProductionDualIdentityPreflightRequest;

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
    // 継続 Live View の1フレーム取得だけの予算。open(10s) と分離する理由は、
    // SDK が予算内に応答する通常ケースにおいてこの値が「停止ボタンが返るまでの
    // 上界」になるため。ただし SDK がこの値を無視して詰まる病的ケースの上界は
    // C# 側 LiveViewResponseTimeout(30s)+ConnectTimeout(10s) で決まり、この
    // 値を縮めても短縮されない(GitHub Issue #141 参照)。
    std::chrono::seconds live_view_frame{3};
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
    [[noreturn]] void AwaitProcessTermination(std::ostream& output);

private:
    void PublishReady(std::ostream& output, std::string_view instruction);
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

// Bounded, anonymous evidence for the MAID command boundary used by a
// read-only SDK status probe. Capability IDs and camera identifiers never
// cross this boundary.
struct SdkCommandTrace {
    std::size_t cap_get_count{};
    std::size_t cap_get_array_count{};
    std::size_t cap_set_count{};
    std::size_t control_plane_cap_set_count{};
    std::size_t photographic_setting_cap_set_count{};
    std::size_t storage_routing_cap_set_count{};
    std::size_t live_view_control_cap_set_count{};
    std::size_t unexpected_cap_set_count{};
    std::size_t cap_start_count{};
    std::size_t capture_start_count{};
    std::size_t non_capture_start_count{};
    std::size_t unknown_cap_start_count{};
    std::size_t live_view_start_count{};
    bool sdk_session_opened{false};
    bool sdk_session_closed{false};
};

// Vendor-neutral event categories keep the MAID entry-boundary recorder
// testable without loading Nikon binaries. NikonSdkTransport maps documented
// MAID command/capability IDs to these categories before calling the shared
// boundary below.
enum class SdkTraceCommand {
    capability_get,
    capability_get_array,
    capability_set,
    capability_start,
    other,
};

enum class SdkTraceCapability {
    none,
    capture_start,
    non_capture_start,
    unknown_start,
    photographic_setting,
    control_plane,
    storage_routing,
    live_view_control,
    other,
};

struct SdkTraceEvent {
    SdkTraceCommand command{SdkTraceCommand::other};
    SdkTraceCapability capability{SdkTraceCapability::none};
    bool live_view_on{false};
};

void RecordSdkTraceEvent(SdkCommandTrace& trace, const SdkTraceEvent& event) noexcept;

// This is the production MAID entry-boundary seam. It records before the
// injected entry call, so a fake entry can prove one invocation/one count and
// the real adapter cannot rely on RunCompleted for a second recording.
template <typename Entry>
decltype(auto) InvokeSdkTraceBoundary(
    SdkCommandTrace& trace,
    const SdkTraceEvent& event,
    Entry&& entry) {
    RecordSdkTraceEvent(trace, event);
    return std::invoke(std::forward<Entry>(entry));
}

// Returns nullopt only when the trace proves a completed, read-only status
// probe. The error value is a stable anonymous reason suitable for tests and
// diagnostics, not a vendor error detail.
[[nodiscard]] std::optional<std::string_view> ValidateSdkReadOnlyCommandTrace(
    const SdkCommandTrace& trace) noexcept;

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
    SdkCommandTrace command_trace;
};

// Process-level proof for the sdk-status CLI route. These counters are not
// MAID evidence and must never be merged into SdkCommandTrace. The narrow SDK
// executor excludes WPD and delete operations. SingleCamera identity-v3 may
// perform exactly one separate read-only WPD identity enumeration before the
// SDK probe; all other WPD/capture/delete route calls remain forbidden.
struct SdkStatusProcessRouting {
    bool sdk_status_executor_selected{false};
    bool single_identity_v3_selected{false};
    std::size_t sdk_enumeration_count{};
    std::size_t sdk_status_probe_count{};
    std::size_t wpd_identity_enumeration_count{};
    std::size_t wpd_call_count{};
    std::size_t capture_call_count{};
    std::size_t delete_call_count{};
};

[[nodiscard]] std::optional<std::string_view> ValidateSdkStatusProcessRouting(
    const SdkStatusProcessRouting& routing) noexcept;

// The sdk-status CLI may enumerate and read status only. Capture, Live View,
// WPD, and delete operations are deliberately absent from this interface.
class ISdkStatusExecutor {
public:
    virtual ~ISdkStatusExecutor() = default;
    [[nodiscard]] virtual std::string SdkVersion() const = 0;
    virtual void RequireExactlyOneD810ForSingleStatus() = 0;
    [[nodiscard]] virtual std::vector<CameraInfo> Enumerate() = 0;
    [[nodiscard]] virtual SdkCameraStatus ProbeSdkStatus(
        std::string_view stable_identity,
        std::chrono::seconds timeout) = 0;
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
    // Constructs a read-only snapshot from bindings already parsed and
    // validated by a stricter product boundary. It does not read or write the
    // backing file.
    IdentityMap(
        std::filesystem::path path,
        std::optional<std::string> cam_a,
        std::optional<std::string> cam_b);
    [[nodiscard]] std::optional<std::string> FindAlias(std::string_view stable_identity) const;
    [[nodiscard]] std::optional<std::string> FindIdentity(std::string_view alias) const;
    void ValidateBinding(std::string_view alias, std::string_view stable_identity) const;
    void Bind(std::string_view alias, std::string_view stable_identity);
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;

private:
    void Load();
    void Save() const;
    std::filesystem::path path_;
    std::optional<std::string> cam_a_;
    std::optional<std::string> cam_b_;
};

struct AnonymousInventoryEntry {
    std::string alias{"UNBOUND"};
    std::string model;
    std::string firmware;
    std::string shooting_mode;
};

struct AnonymousInventorySummary {
    std::vector<AnonymousInventoryEntry> cameras;
    std::size_t bound_camera_count{0};
    std::size_t unbound_camera_count{0};
};

[[nodiscard]] AnonymousInventorySummary SummarizeInventoryReadOnly(
    const IdentityMap& map,
    const std::vector<CameraInfo>& cameras);
[[nodiscard]] CameraInfo SelectSingleCameraForBinding(const std::vector<CameraInfo>& cameras);
struct CrossTransportBindingSelection {
    CameraInfo sdk_camera;
    CameraInfo wpd_camera;
};
[[nodiscard]] CrossTransportBindingSelection BindCrossTransportIdentity(
    IdentityMap& sdk_map,
    IdentityMap& wpd_map,
    std::string_view alias,
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras);

struct DualIdentityVerificationSummary {
    std::size_t sdk_camera_count{};
    std::size_t sdk_cam_a_count{};
    std::size_t sdk_cam_b_count{};
    std::size_t sdk_unbound_count{};
    std::size_t wpd_camera_count{};
    std::size_t wpd_cam_a_count{};
    std::size_t wpd_cam_b_count{};
    std::size_t wpd_unbound_count{};
    bool identity_maps_changed{};
    bool capture_command_sent{};
    bool live_view_started{};
    bool camera_settings_changed{};
    bool card_access_performed{};
    bool real_identifiers_included{};
    std::string terminal_state{"Blocked"};
    std::string failure_category;
};
[[nodiscard]] DualIdentityVerificationSummary VerifyDualIdentityBindings(
    const IdentityMap& sdk_map,
    const IdentityMap& wpd_map,
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras);
[[nodiscard]] DualIdentityVerificationSummary VerifyProductionDualIdentityPreflight(
    const ProductionDualIdentityPreflightRequest& request);
[[nodiscard]] std::filesystem::path PersistDualIdentityVerificationSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    const DualIdentityVerificationSummary& summary);

struct DualSpoolVerificationSummary {
    DualIdentityVerificationSummary identity;
    std::size_t cam_a_payload_object_count{};
    std::size_t cam_b_payload_object_count{};
    std::size_t wpd_sessions_closed{};
    bool read_only_observation{true};
    bool card_inspection_performed{};
    bool capture_command_sent{};
    bool camera_delete_attempted{};
    bool vendor_operation_executed{};
    bool automatic_retry{};
    bool real_identifiers_included{};
    std::string terminal_state{"Blocked"};
    std::string failure_category;
};
[[nodiscard]] DualSpoolVerificationSummary PrepareDualSpoolVerification(
    const DualIdentityVerificationSummary& identity);
void FinalizeDualSpoolVerification(
    DualSpoolVerificationSummary& summary,
    std::size_t cam_a_payload_object_count,
    std::size_t cam_b_payload_object_count);
[[nodiscard]] std::filesystem::path PersistDualSpoolVerificationSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    const DualSpoolVerificationSummary& summary);

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

struct HybridPairResult {
    std::string run_id;
    std::string pair_id;
    std::string terminal_state{"FailedPartial"};
    std::string error_category;
    std::string error_detail;
    TransactionResult cam_a;
    TransactionResult cam_b;
    bool cam_b_started{false};
    bool automatic_retry{false};
    std::chrono::milliseconds duration{0};
};

struct HybridPairRunSummary {
    int requested_pairs{0};
    int attempted_pairs{0};
    int completed_pairs{0};
    int failures{0};
    int cam_a_completed_count{0};
    int cam_b_completed_count{0};
    int attempted_camera_transactions{0};
    int completed_camera_transactions{0};
    int spool_empty_before_count{0};
    int camera_card_delete_attempted_count{0};
    int camera_card_delete_succeeded_count{0};
    int spool_empty_after_count{0};
    int duration_sample_count{0};
    std::int64_t pair_duration_p50_ms{0};
    std::int64_t pair_duration_p95_ms{0};
    std::int64_t pair_duration_max_ms{0};
    std::string terminal_state{"InProgress"};
    std::string last_pair_state;
    std::string last_error_category;
    std::string last_error_detail;
    bool automatic_retry{false};
    bool exclusive_camera_control_confirmed{false};
    bool dedicated_spool_scope_confirmed{false};
    bool dual_dedicated_spools_confirmed{false};
    bool exact_object_delete_confirmed{false};
    bool actual_shutter_synchronization_guaranteed{false};
    int pair_watchdog_seconds{180};
};

struct HybridPairRecoveryStatus {
    int pair_started_count{0};
    int pair_complete_count{0};
    int pair_failed_count{0};
    bool interrupted_pair_detected{false};
    std::string interrupted_stage;
    bool cam_a_complete_before_interruption{false};
    bool retain_completed_originals{true};
    bool automatic_retry_allowed{false};
    bool recovery_requires_new_transaction{false};
    bool event_sequence_consistent{true};
    std::string terminal_state{"NoPairEvents"};
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

struct HybridPairFaultRunSummary {
    std::string scenario;
    std::string fault_camera_alias;
    std::string gate_stage{"after_sdk_close_before_wpd_recovery"};
    std::string pair_state;
    std::string error_category;
    std::string acceptance_state{"InProgress"};
    bool cam_b_started{false};
    bool cam_a_original_persisted{false};
    bool cam_b_original_persisted{false};
    bool cam_a_delete_attempted{false};
    bool cam_b_delete_attempted{false};
    bool automatic_retry{false};
    bool recovery_requires_new_transaction{true};
    bool actual_shutter_synchronization_guaranteed{false};
};

[[nodiscard]] std::optional<std::string> ValidateHybridCaptureArguments(
    std::string_view command,
    int count,
    bool exclusive_camera_control_confirmed,
    bool dedicated_spool_scope_confirmed,
    bool exact_object_delete_confirmed,
    bool dual_dedicated_spools_confirmed = false) noexcept;

[[nodiscard]] std::filesystem::path PersistSdkStatusSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const CameraInfo& camera,
    const SdkCameraStatus& status,
    const SdkStatusProcessRouting& routing);
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
[[nodiscard]] std::filesystem::path PersistHybridPairFaultSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    const HybridPairFaultRunSummary& status);
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
[[nodiscard]] std::filesystem::path PersistHybridPairSummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    const HybridPairRunSummary& result);
[[nodiscard]] HybridPairRecoveryStatus AssessHybridPairRecoveryEventLog(
    const std::filesystem::path& event_log);
[[nodiscard]] std::filesystem::path PersistHybridPairRecoverySummary(
    const std::filesystem::path& artifacts_root,
    std::string_view run_id,
    const HybridPairRecoveryStatus& status);
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
    const std::function<void()>& before_pc_original_rename = {},
    std::optional<std::chrono::steady_clock::time_point> transaction_deadline = std::nullopt,
    const std::function<void(const FrameEvidence&)>& before_camera_object_delete = {},
    const std::function<void()>& before_sdk_capture = {});
[[nodiscard]] HybridPairResult ExecuteHybridCapturePair(
    ICameraTransport& wpd_session,
    IPostCardObservationTransport& wpd,
    ICameraTransport& sdk_session,
    ICardCaptureTransport& sdk,
    EvidenceWriter& evidence,
    std::string_view cam_a_wpd_identity,
    std::string_view cam_a_sdk_identity,
    std::string_view cam_b_wpd_identity,
    std::string_view cam_b_sdk_identity,
    Timeouts timeouts = {},
    const std::function<void()>& before_cam_b = {},
    const std::function<void()>& before_cam_a_wpd_recovery = {},
    const std::function<void()>& before_cam_b_wpd_recovery = {});
[[nodiscard]] HybridPairRunSummary ExecuteHybridPairRun(
    int requested_pairs,
    const std::function<HybridPairResult()>& capture_pair_once);

[[nodiscard]] std::string NewRunId();
[[nodiscard]] std::filesystem::path DefaultIdentityMapPath();
[[nodiscard]] std::filesystem::path DefaultSingleIdentityV3Path();
void PersistSingleIdentityV3(
    const std::filesystem::path& path,
    std::string_view alias,
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras);
[[nodiscard]] bool IsValidJpeg(const std::vector<unsigned char>& bytes);
[[nodiscard]] std::vector<unsigned char> ExtractD810LiveViewJpeg(
    const std::vector<unsigned char>& frame_with_header);
[[nodiscard]] std::string Sha256Hex(const std::vector<unsigned char>& bytes);
[[nodiscard]] std::optional<std::vector<std::string>> ParsePackedStringLabels(
    const std::vector<unsigned char>& bytes,
    std::size_t maximum_labels,
    std::size_t maximum_bytes);

} // namespace a0::phase0
