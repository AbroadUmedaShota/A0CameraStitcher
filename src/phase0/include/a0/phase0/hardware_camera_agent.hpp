#pragma once

#include "a0/phase0/phase0.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace a0::phase0 {

inline constexpr std::string_view kHardwareCameraAgentSchemaVersion =
    "a0.camera-agent.hardware.v1";
inline constexpr std::string_view kHardwareCameraAgentLiveViewSchemaVersion =
    "a0.camera-agent.hardware.v2";
inline constexpr std::string_view kHardwareCameraAgentMarker = "Hardware";
inline constexpr std::string_view kDefaultHardwareCameraAgentPipeName =
    "A0CameraStitcher.CameraAgent.Hardware.v1";

enum class HardwareCameraAgentOperation {
    get_single_readiness,
    capture_single,
    live_view_probe,
    get_transaction_result,
    start_live_view,
    read_live_view_frame,
    live_view_heartbeat,
    stop_live_view,
    close_agent_session,
};

class HardwareCameraAgentProtocolError final : public std::runtime_error {
public:
    HardwareCameraAgentProtocolError(std::string code, std::string message);
    [[nodiscard]] const std::string& Code() const noexcept;

private:
    std::string code_;
};

struct HardwareCameraAgentRequest {
    std::string schema_version{std::string(kHardwareCameraAgentSchemaVersion)};
    std::string request_id;
    HardwareCameraAgentOperation operation{HardwareCameraAgentOperation::get_single_readiness};
    std::string transaction_id;
    std::string camera_alias;
    std::string expected_capture_profile_id;
    std::uint32_t expected_capture_profile_version{};
    std::string expected_capture_profile_sha256;
    std::string expected_capture_profile_expires_at_utc;
    bool exclusive_camera_control_confirmed{};
    bool dedicated_spool_scope_confirmed{};
    bool exact_object_delete_confirmed{};
    bool live_view_handoff_requested{};
    int live_view_frames{1};
    int live_view_interval_ms{100};
    std::string session_id;
};

struct SingleCameraBindingResolution {
    bool ready{};
    std::string camera_alias;
    std::size_t sdk_camera_count{};
    std::size_t wpd_camera_count{};
    bool sdk_identity_bound{};
    bool wpd_identity_bound{};
    bool sdk_alias_matches{};
    bool wpd_alias_matches{};
    std::optional<CameraInfo> sdk_camera;
    std::optional<CameraInfo> wpd_camera;
    std::string failure_category;
    std::string failure_detail;
};

struct SingleCameraIdentityV3 {
    std::string camera_alias;
    std::string wpd_stable_identity_sha256;
    std::string sdk_selection_policy{"exactly-one-current-session"};
};

// DualCamera deliberately uses a separate contract from SingleCamera
// identity-v3. Digests are anonymous projections supplied by a future
// documented same-body correlation provider; this layer never invents one.
enum class DualIdentityTransport {
    sdk,
    wpd,
};

struct DualIdentityBindingProof {
    std::string camera_alias;
    std::string provider_id;
    std::uint32_t provider_version{};
    std::string sdk_identity_sha256;
    std::string wpd_identity_sha256;
    std::string created_at_utc;
    std::string expires_at_utc;
    bool single_camera_connected_confirmed{};
    bool documented_correlation_confirmed{};
    std::string proof_payload_sha256;
};

struct DualIdentityCorrelationProvider {
    std::string provider_id;
    std::uint32_t provider_version{};
    bool documented_stable_per_body_correlation{};
};

[[nodiscard]] DualIdentityCorrelationProvider ParseDualIdentityCorrelationProvider(
    std::string_view json);
[[nodiscard]] DualIdentityCorrelationProvider LoadDualIdentityCorrelationProvider(
    const std::filesystem::path& path);

struct DualIdentityInventoryProjection {
    DualIdentityTransport transport{DualIdentityTransport::sdk};
    std::string model;
    std::string identity_sha256;
    std::string provider_id;
    std::uint32_t provider_version{};
};

struct DualIdentitySafetyState {
    bool inventory_read_only{true};
    bool capture_command_sent{};
    bool card_access_performed{};
    bool live_view_started{};
    bool camera_settings_changed{};
    bool camera_object_delete_attempted{};
    bool card_format_attempted{};
    bool vendor_operation_executed{};
    std::uint32_t automatic_retry_count{};
};

enum class DualIdentityBlockReason {
    identity_strategy_unresolved,
    provider_config_invalid,
    legacy_map_fallback_prohibited,
    proof_count_mismatch,
    proof_invalid,
    proof_tampered,
    proof_stale,
    confirmation_mismatch,
    provider_mismatch,
    camera_count_mismatch,
    missing_identity,
    duplicate_identity,
    identity_collision,
    mismatched_transport,
    unbound_identity,
    alias_cardinality_mismatch,
};

struct DualIdentityReady {
    std::size_t sdk_cam_a_count{};
    std::size_t sdk_cam_b_count{};
    std::size_t sdk_unbound_count{};
    std::size_t wpd_cam_a_count{};
    std::size_t wpd_cam_b_count{};
    std::size_t wpd_unbound_count{};
    DualIdentitySafetyState safety;
};

struct DualIdentityBlocked {
    DualIdentityBlockReason reason{DualIdentityBlockReason::identity_strategy_unresolved};
    DualIdentitySafetyState safety;
};

using DualIdentityResult = std::variant<DualIdentityReady, DualIdentityBlocked>;

struct ProductionDualIdentityPreflightRequest {
    std::optional<std::filesystem::path> provider_config_path;
    std::vector<std::filesystem::path> proof_paths;
    std::vector<DualIdentityInventoryProjection> sdk_inventory;
    std::vector<DualIdentityInventoryProjection> wpd_inventory;
    std::string observed_at_utc;
    bool legacy_map_fallback_requested{};
};

[[nodiscard]] std::string_view DualIdentityBlockReasonName(
    DualIdentityBlockReason reason) noexcept;

[[nodiscard]] std::string ComputeDualIdentityBindingProofPayloadSha256(
    const DualIdentityBindingProof& proof);
[[nodiscard]] std::string SerializeDualIdentityBindingProof(
    const DualIdentityBindingProof& proof);
[[nodiscard]] DualIdentityBindingProof ParseDualIdentityBindingProof(
    std::string_view json);
[[nodiscard]] DualIdentityBindingProof LoadDualIdentityBindingProof(
    const std::filesystem::path& path);
[[nodiscard]] DualIdentityResult VerifyDualIdentitySoftwareContract(
    const std::optional<DualIdentityCorrelationProvider>& provider,
    const std::vector<std::filesystem::path>& proof_paths,
    const std::vector<DualIdentityInventoryProjection>& sdk_inventory,
    const std::vector<DualIdentityInventoryProjection>& wpd_inventory,
    std::string_view observed_at_utc,
    bool legacy_map_fallback_requested);
[[nodiscard]] DualIdentityResult RunProductionDualIdentityPreflight(
    const ProductionDualIdentityPreflightRequest& request);

[[nodiscard]] SingleCameraIdentityV3 ParseSingleCameraIdentityV3(
    std::string_view json);
[[nodiscard]] SingleCameraIdentityV3 LoadSingleCameraIdentityV3(
    const std::filesystem::path& path);
[[nodiscard]] CameraInfo ResolveSingleCameraSdkStatusCamera(
    const SingleCameraIdentityV3& identity,
    std::string_view requested_alias,
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras);

class ISingleIdentityV3WpdEnumerator {
public:
    virtual ~ISingleIdentityV3WpdEnumerator() = default;
    [[nodiscard]] virtual std::vector<CameraInfo> Enumerate() = 0;
};

struct SingleIdentityV3SdkStatusExecution {
    CameraInfo camera;
    SdkCameraStatus status;
    SdkStatusProcessRouting routing;
    std::string sdk_version;
};

[[nodiscard]] SingleIdentityV3SdkStatusExecution ExecuteSingleIdentityV3SdkStatus(
    const std::filesystem::path& identity_path,
    std::string_view requested_alias,
    const std::function<std::unique_ptr<ISingleIdentityV3WpdEnumerator>()>&
        wpd_factory,
    const std::function<std::unique_ptr<ISdkStatusExecutor>()>& sdk_factory,
    std::chrono::seconds timeout);

struct ObservedCameraSetting {
    bool available{};
    std::string cap_type{"unsupported"};
    std::string probe_state{"not-advertised"};
    std::string value_type{"unsupported"};
    std::optional<std::uint32_t> current_value;
    std::optional<std::uint32_t> current_index;
    std::optional<std::string> current_label;
};

struct ObservedCameraSettings {
    ObservedCameraSetting file_type;
    ObservedCameraSetting compression_level;
    ObservedCameraSetting image_size;
    ObservedCameraSetting exposure_mode;
    ObservedCameraSetting shutter_speed;
    ObservedCameraSetting aperture;
    ObservedCameraSetting sensitivity;
    ObservedCameraSetting white_balance_mode;
    ObservedCameraSetting focus_mode;
};

struct SingleCameraReadinessResult {
    bool ready{};
    std::string camera_alias;
    std::size_t sdk_camera_count{};
    std::size_t wpd_camera_count{};
    bool sdk_identity_bound{};
    bool wpd_identity_bound{};
    bool sdk_alias_matches{};
    bool wpd_alias_matches{};
    bool sdk_status_probed{};
    bool spool_inspected{};
    std::size_t spool_payload_object_count{};
    bool spool_known_empty{};
    std::string firmware{"unknown"};
    std::string live_view_status{"unknown"};
    bool live_view_status_available{};
    bool capture_profile_approved{};
    std::string capture_profile_id;
    std::uint32_t capture_profile_version{};
    std::string capture_profile_sha256;
    std::string capture_profile_camera_alias;
    std::string profile_expires_at_utc;
    bool capture_profile_alias_matches{};
    bool settings_match_approved_profile{};
    ObservedCameraSettings observed_settings;
    bool read_only{};
    bool capture_command_sent{};
    bool camera_object_delete_attempted{};
    bool camera_settings_changed{};
    bool real_identifiers_included{};
    std::string failure_category;
    std::string failure_detail;
};

struct RetainedOriginalRecord {
    std::string camera_alias;
    std::filesystem::path path;
    std::size_t size{};
    std::string sha256;
};

struct PreviewJpegRecord {
    std::filesystem::path path;
    std::size_t size{};
    std::string sha256;
};

struct SingleCameraCaptureResult {
    bool succeeded{};
    std::string camera_alias;
    std::string run_id;
    std::string transaction_id;
    std::string capture_profile_id;
    std::uint32_t capture_profile_version{};
    std::string capture_profile_sha256;
    std::string capture_profile_camera_alias;
    std::string profile_expires_at_utc;
    std::string terminal_state{"Blocked"};
    std::string error_category;
    std::string error_detail;
    std::optional<RetainedOriginalRecord> retained_original;
    bool live_view_handoff_requested{};
    bool live_view_stopped_before_capture{};
    bool live_view_sdk_session_closed_before_capture{};
    bool live_view_resume_attempted{};
    bool live_view_resumed{};
    std::optional<PreviewJpegRecord> resumed_preview;
    bool spool_empty_before_capture{};
    bool camera_object_delete_attempted{};
    bool camera_object_delete_succeeded{};
    bool spool_empty_after_cleanup{};
    int automatic_retry_count{};
    int transaction_watchdog_seconds{180};
    bool real_identifiers_included{};
};

struct SingleCameraLiveViewProbeResult {
    bool succeeded{};
    std::string camera_alias;
    std::string run_id;
    int frames{};
    std::size_t last_frame_bytes{};
    std::string last_frame_sha256;
    long long duration_ms{};
    bool preview_persisted{};
    std::optional<PreviewJpegRecord> preview;
    bool preview_is_original{};
    bool preview_is_stitch_input{};
    bool live_view_stopped{};
    bool sdk_session_closed{};
    bool real_identifiers_included{};
    std::string error_category;
    std::string error_detail;
};

struct ContinuousLiveViewResult {
    bool succeeded{};
    std::string camera_alias{"CAM-A"};
    std::string session_id;
    std::string state{"Failed"};
    std::uint64_t frame_number{};
    std::size_t frame_size{};
    std::string frame_sha256;
    std::string frame_jpeg_base64;
    bool preview_is_original{};
    bool preview_is_stitch_input{};
    bool sdk_session_open{};
    bool live_view_running{};
    int heartbeat_timeout_seconds{20};
    int maximum_session_seconds{600};
    bool real_identifiers_included{};
    std::string error_category;
    std::string error_detail;
};

struct HardwareCameraAgentResponse {
    std::string schema_version{std::string(kHardwareCameraAgentSchemaVersion)};
    std::string request_id;
    bool success{};
    std::string result_code;
    std::optional<SingleCameraReadinessResult> readiness;
    std::optional<SingleCameraCaptureResult> capture;
    std::optional<SingleCameraLiveViewProbeResult> live_view;
    std::optional<ContinuousLiveViewResult> continuous_live_view;
    std::string rejection_code;
    std::string error_detail;
};

[[nodiscard]] HardwareCameraAgentRequest ParseHardwareCameraAgentRequest(
    std::string_view json);
[[nodiscard]] std::string SerializeHardwareCameraAgentResponse(
    const HardwareCameraAgentResponse& response);

[[nodiscard]] SingleCameraBindingResolution ResolveExactlyOneBoundCamera(
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras,
    const IdentityMap& sdk_identity_map,
    const IdentityMap& wpd_identity_map,
    std::string_view camera_alias);

[[nodiscard]] SingleCameraCaptureResult ExecuteBoundSingleCapture(
    const HardwareCameraAgentRequest& request,
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras,
    const IdentityMap& sdk_identity_map,
    const IdentityMap& wpd_identity_map,
    ICameraTransport& wpd_session,
    IPostCardObservationTransport& wpd,
    ICameraTransport& sdk_session,
    ICardCaptureTransport& sdk,
    EvidenceWriter& evidence,
    const SdkCameraStatus& pre_wpd_sdk_status,
    const std::function<SdkCameraStatus()>& shutter_session_status_probe,
    Timeouts timeouts = {},
    std::optional<std::chrono::steady_clock::time_point> transaction_deadline = std::nullopt,
    const std::function<void()>& before_camera_object_delete = {},
    const std::function<void(const SdkCameraStatus&)>&
        validate_shutter_session_status = {});

class IHardwareCameraAgentBackend {
public:
    virtual ~IHardwareCameraAgentBackend() = default;
    [[nodiscard]] virtual SingleCameraReadinessResult GetSingleReadiness(
        std::string_view camera_alias) = 0;
    [[nodiscard]] virtual SingleCameraCaptureResult CaptureSingle(
        const HardwareCameraAgentRequest& request) = 0;
    [[nodiscard]] virtual SingleCameraLiveViewProbeResult ProbeLiveView(
        const HardwareCameraAgentRequest& request) = 0;
    [[nodiscard]] virtual SingleCameraCaptureResult GetTransactionResult(
        std::string_view transaction_id) = 0;
    [[nodiscard]] virtual ContinuousLiveViewResult StartContinuousLiveView(
        const HardwareCameraAgentRequest& request);
    [[nodiscard]] virtual ContinuousLiveViewResult ReadContinuousLiveViewFrame(
        const HardwareCameraAgentRequest& request);
    [[nodiscard]] virtual ContinuousLiveViewResult HeartbeatContinuousLiveView(
        const HardwareCameraAgentRequest& request);
    [[nodiscard]] virtual ContinuousLiveViewResult StopContinuousLiveView(
        const HardwareCameraAgentRequest& request);
    [[nodiscard]] virtual ContinuousLiveViewResult CloseAgentSession(
        const HardwareCameraAgentRequest& request);
    virtual void OnAgentIdle() noexcept;
};

// Narrow production boundary for the SDK session that owns continuous Live
// View. The Nikon adapter is the default implementation; tests inject only
// this boundary and never load the licensed SDK or issue a camera command.
class IContinuousLiveViewSdkTransport {
public:
    virtual ~IContinuousLiveViewSdkTransport() = default;
    [[nodiscard]] virtual std::vector<CameraInfo> Enumerate() = 0;
    [[nodiscard]] virtual SdkCameraStatus ProbeSdkStatus(
        std::string_view stable_identity,
        std::chrono::seconds timeout) = 0;
    virtual void OpenLiveView(
        std::string_view stable_identity,
        std::chrono::seconds timeout) = 0;
    virtual void StartLiveView(std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::vector<unsigned char> ReadLiveViewFrame(
        std::chrono::seconds timeout) = 0;
    virtual void StopLiveView(std::chrono::seconds timeout) = 0;
    virtual void Close(std::chrono::seconds timeout) = 0;
};

struct ProductionHardwareCameraAgentConfig {
    std::filesystem::path artifacts_root;
    std::filesystem::path reports_root;
    std::filesystem::path sdk_identity_map;
    std::filesystem::path wpd_identity_map;
    // Product SingleCamera identity-v3. When configured, the WPD serial
    // digest is the persistent body identity and SDK selection is permitted
    // only by exact-one current-session cardinality. Legacy SDK/WPD maps are
    // never automatically migrated into this file.
    std::filesystem::path single_identity_v3;
    std::filesystem::path transaction_state_root;
    std::filesystem::path approved_capture_profile;
    Timeouts timeouts{};
    // Deterministic concurrency seam for contract tests. Production launchers
    // leave this empty.
    std::function<void()> after_initial_active_journal_read_for_testing;
    std::function<void()>
        after_transaction_reservation_directory_created_for_testing;
    // Continuous Live View contract-test seams. Production launchers leave
    // all three empty. Factory/resolver must be supplied together.
    std::function<std::unique_ptr<IContinuousLiveViewSdkTransport>()>
        continuous_live_view_sdk_factory_for_testing;
    std::function<std::string(
        std::string_view,
        const std::vector<CameraInfo>&)>
        continuous_live_view_identity_resolver_for_testing;
    std::function<std::chrono::steady_clock::time_point()>
        continuous_live_view_clock_for_testing;

    [[nodiscard]] static ProductionHardwareCameraAgentConfig Defaults();
};

class ProductionHardwareCameraAgentBackend final : public IHardwareCameraAgentBackend {
public:
    explicit ProductionHardwareCameraAgentBackend(ProductionHardwareCameraAgentConfig config);
    ~ProductionHardwareCameraAgentBackend() override;
    ProductionHardwareCameraAgentBackend(const ProductionHardwareCameraAgentBackend&) = delete;
    ProductionHardwareCameraAgentBackend& operator=(const ProductionHardwareCameraAgentBackend&) = delete;

    [[nodiscard]] SingleCameraReadinessResult GetSingleReadiness(
        std::string_view camera_alias) override;
    [[nodiscard]] SingleCameraCaptureResult CaptureSingle(
        const HardwareCameraAgentRequest& request) override;
    [[nodiscard]] SingleCameraLiveViewProbeResult ProbeLiveView(
        const HardwareCameraAgentRequest& request) override;
    [[nodiscard]] SingleCameraCaptureResult GetTransactionResult(
        std::string_view transaction_id) override;
    [[nodiscard]] ContinuousLiveViewResult StartContinuousLiveView(
        const HardwareCameraAgentRequest& request) override;
    [[nodiscard]] ContinuousLiveViewResult ReadContinuousLiveViewFrame(
        const HardwareCameraAgentRequest& request) override;
    [[nodiscard]] ContinuousLiveViewResult HeartbeatContinuousLiveView(
        const HardwareCameraAgentRequest& request) override;
    [[nodiscard]] ContinuousLiveViewResult StopContinuousLiveView(
        const HardwareCameraAgentRequest& request) override;
    [[nodiscard]] ContinuousLiveViewResult CloseAgentSession(
        const HardwareCameraAgentRequest& request) override;
    void OnAgentIdle() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class HardwareCameraAgentDispatcher final {
public:
    explicit HardwareCameraAgentDispatcher(IHardwareCameraAgentBackend& backend);
    [[nodiscard]] std::string Handle(std::string_view request_json) noexcept;
    [[nodiscard]] bool ShouldStop() const noexcept;
    void OnIdle() noexcept;

private:
    IHardwareCameraAgentBackend& backend_;
    bool should_stop_{};
};

// Serves one length-prefixed UTF-8 JSON request per local named-pipe
// connection. The loop is intentionally single-threaded: the production
// backend also takes the operator-session-wide camera-control lease for every
// real operation.
struct HardwareCameraAgentPipeFailureInjectionForTesting {
    bool fail_response_header_write{};
    bool fail_response_body_write{};
    bool fail_response_flush{};
};

[[nodiscard]] int RunHardwareCameraAgentNamedPipeServer(
    std::string_view pipe_name,
    HardwareCameraAgentDispatcher& dispatcher,
    bool serve_once = false,
    HardwareCameraAgentPipeFailureInjectionForTesting failure_injection = {});

} // namespace a0::phase0
