#pragma once

#include "a0/phase0/dual_binding_camera_agent.hpp"
#include "a0/phase0/pc_direct_capture.hpp"
#include "a0/phase0/phase0.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace a0::phase0 {

// SDK-independent source-publication wait seam used by the real read-only
// inventory path. The injected clock and wait operation keep timeout behavior
// deterministic in software-only tests without loading the licensed SDK.
[[nodiscard]] std::vector<std::uint32_t> WaitForNikonSdkReadOnlySourceIds(
    std::chrono::steady_clock::time_point deadline,
    const std::function<void()>& pump_callbacks,
    const std::function<std::vector<std::uint32_t>()>& read_source_ids,
    const std::function<void()>& wait_iteration,
    const std::function<std::chrono::steady_clock::time_point()>& now);

// SDK-independent control-flow seam for the real D810 inventory walk. Every
// successful source open is followed by exactly one checked close before the
// next source can be opened or a matching id can be published. The caller owns
// the sticky close-uncertain state and must retain it even after SDK unload.
[[nodiscard]] std::vector<std::uint32_t> InspectNikonD810InventorySources(
    const std::vector<std::uint32_t>& source_ids,
    const std::function<void(std::uint32_t)>& open_source,
    const std::function<bool()>& inspect_current_source_is_d810,
    const std::function<bool()>& close_current_source_once,
    bool& close_unconfirmed);

class INikonDualSessionTransport {
public:
    virtual ~INikonDualSessionTransport() = default;
    // SDK-only Dual preflight. This counts D810 source objects without
    // deriving persistent identities or generating candidate tokens. The
    // caller must end the retained module session before any WPD access.
    [[nodiscard]] virtual std::size_t BeginDualReadOnlyProbe(
        std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::vector<std::string> BeginDualSession(
        std::chrono::seconds timeout) = 0;
    virtual void OpenDualCandidateLiveView(
        std::string_view candidate_token,
        std::chrono::seconds timeout) = 0;
    virtual void OpenDualBoundCapture(
        std::string_view candidate_token,
        std::chrono::seconds timeout) = 0;
    // Explicit experimental PC-direct path. This remains separate from the
    // accepted card-spool path so existing empty-card behavior cannot change
    // implicitly. The concrete transport snapshots SaveMedia, selects SDRAM,
    // verifies it, and restores the original value on checked source close.
    virtual void OpenDualBoundPcDirectCapture(
        std::string_view candidate_token,
        std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual SdkCameraStatus ProbeOpenCaptureSessionStatus(
        std::chrono::seconds timeout) = 0;
    virtual void StartLiveView(std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::vector<unsigned char> ReadLiveViewFrame(
        std::chrono::seconds timeout) = 0;
    virtual void StopLiveView(std::chrono::seconds timeout) = 0;
    virtual void CaptureToCard(
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds transaction_timeout) = 0;
    [[nodiscard]] virtual std::string BeginPcDirectBaseline(
        std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::vector<ImageCandidate> CaptureAndDownloadToPc(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) = 0;
    virtual void CloseDualSourceKeepingModule(
        std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual DualIdentityInvalidationReason PollDualInvalidation() = 0;
    virtual void EndDualSession(std::chrono::seconds timeout) = 0;
    // Last-resort process isolation for an unconfirmed WPD cleanup boundary.
    // This must not call the SDK: the owner is intentionally retained until
    // process exit rather than risking a cross-transport API call.
    virtual void AbandonDualSessionNoSdkCalls() noexcept = 0;
    struct ExitState {
        bool process_claim_retained{};
        bool module_retained{};
        bool source_open{};

        [[nodiscard]] bool FullyEnded() const noexcept {
            return !process_claim_retained && !module_retained && !source_open;
        }
    };
    [[nodiscard]] virtual ExitState InspectDualSessionExitState() const noexcept = 0;
};

// Anonymous, in-process proof of the ADR-0028 boundary. The retained manager
// Module and its process claim may remain active only while both SDK source
// count and Live View are zero. Candidate tokens never cross this boundary.
struct NikonDualRetainedModuleState {
    bool process_claim_retained{};
    bool module_retained{};
    std::size_t open_source_count{};
    bool live_view_active{};
    std::size_t active_candidate_count{};

    [[nodiscard]] bool ReadyForReadOnlyWpd() const noexcept {
        return process_claim_retained && module_retained &&
            open_source_count == 0 && !live_view_active &&
            active_candidate_count == kDualIdentityRequiredCandidateCount;
    }
};

// Derives the private SDK-side identity from documented, source-level MAID
// strings. The returned digest is local-only and must never be committed.
// Source object IDs are deliberately excluded because they are ephemeral.
[[nodiscard]] std::string DeriveNikonSdkStableIdentity(
    std::string_view source_name,
    std::string_view source_interface);

struct NikonSaveMediaSelection {
    std::uint32_t original_value{};
    std::uint32_t selected_value{};
};

// Testable command-order seam used by the concrete SDK transport. Unknown
// original state stops before Set; selection failure performs one checked
// restore; callers use RestoreNikonSaveMediaOnce once more only after a
// successful selection/session.
[[nodiscard]] NikonSaveMediaSelection SelectNikonSaveMediaForPcDirect(
    std::uint32_t desired_value,
    const std::function<std::uint32_t()>& get_current,
    const std::function<void(std::uint32_t)>& set_value);
void RestoreNikonSaveMediaOnce(
    std::uint32_t original_value,
    const std::function<std::uint32_t()>& get_current,
    const std::function<void(std::uint32_t)>& set_value);

// Small SDK-independent model of the source callback window used by a
// SaveMedia=Card capture. It keeps callback/pump/session ordering testable
// without loading licensed Nikon binaries or issuing a camera command.
enum class NikonCardCaptureEvent {
    capture_complete,
    add_child_in_card,
    other,
};

struct NikonCardCaptureEventSnapshot {
    bool callback_registered{false};
    bool capture_command_started{false};
    bool capture_command_accepted{false};
    bool event_pump_started{false};
    bool event_pump_stopped{false};
    bool session_closed{false};
    bool session_closed_while_pumping{false};
    std::size_t capture_complete_events{};
    std::size_t add_child_in_card_events{};
    std::size_t ignored_events{};
};

class NikonCardCaptureEventWindow final {
public:
    void ResetForSession() noexcept;
    void CallbackRegistered() noexcept;
    [[nodiscard]] bool BeginCaptureCommand() noexcept;
    [[nodiscard]] bool BeginEventPump() noexcept;
    void CaptureCommandAccepted() noexcept;
    void Observe(NikonCardCaptureEvent event) noexcept;
    void EndEventPump() noexcept;
    void SessionClosed() noexcept;
    [[nodiscard]] bool CaptureCompleted() const noexcept;
    [[nodiscard]] NikonCardCaptureEventSnapshot Snapshot() const noexcept;

private:
    NikonCardCaptureEventSnapshot snapshot_{};
};

enum class NikonPcDirectEvent {
    capture_complete,
    add_child_in_card,
    other,
};

struct NikonPcDirectEventSnapshot {
    bool measurement_started{};
    bool callback_registered{};
    bool callback_active_before_capture{};
    bool baseline_ready{};
    bool capture_command_started{};
    bool capture_command_accepted{};
    bool session_closed{};
    std::size_t pre_dispatch_candidate_count{};
    std::size_t candidate_notification_count{};
    std::size_t distinct_notified_candidate_count{};
    std::size_t distinct_enumerated_candidate_count{};
    std::size_t duplicate_candidate_notification_count{};
    std::size_t capture_complete_count{};
    std::size_t removed_candidate_count{};
    std::size_t add_child_in_card_count{};
    std::size_t ignored_event_count{};
    std::size_t forced_enumeration_attempt_count{};
    std::size_t forced_enumeration_success_count{};
    std::size_t forced_enumeration_failure_count{};
    std::optional<PcDirectTerminalSubreason> terminal_subreason;
    std::vector<PcDirectObservation> observation_order;
};

// SDK-independent PC-direct callback/reconciliation window. A candidate is
// attributable only after command acceptance when the distinct AddChild and
// Children sets contain the same single Item and at least one post-acceptance
// forced enumeration succeeded. Repeated AddChild for that same Item is kept
// as diagnostics. Pre-dispatch/pre-acceptance, different/removed/card items,
// enumeration failure, or multiple CaptureComplete events fail closed.
class NikonPcDirectEventWindow final {
public:
    void ResetForSession() noexcept;
    void CallbackRegistered() noexcept;
    void BeginBaseline() noexcept;
    [[nodiscard]] bool BeginCaptureCommand() noexcept;
    void CaptureCommandAccepted() noexcept;
    void ObserveCandidate(
        std::uint32_t candidate_id,
        bool callback_notification) noexcept;
    void ObserveRemovedCandidate(std::uint32_t candidate_id) noexcept;
    void Observe(NikonPcDirectEvent event) noexcept;
    void RecordForcedEnumeration(bool succeeded) noexcept;
    void RecordTerminalSubreason(
        PcDirectTerminalSubreason subreason) noexcept;
    void SessionClosed() noexcept;
    [[nodiscard]] bool CanAttributeExactlyOne() const noexcept;
    [[nodiscard]] bool CanTerminateWithStableCandidate(
        std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::time_point stable_since,
        std::chrono::steady_clock::time_point event_deadline,
        std::chrono::milliseconds settle_duration) const noexcept;
    [[nodiscard]] NikonPcDirectEventSnapshot Snapshot() const;

private:
    NikonPcDirectEventSnapshot snapshot_{};
    std::set<std::uint32_t> pre_dispatch_candidate_ids_;
    std::set<std::uint32_t> notified_candidate_ids_;
    std::set<std::uint32_t> enumerated_candidate_ids_;
    std::set<std::uint32_t> removed_candidate_ids_;
    void RecordObservation(PcDirectObservation observation) noexcept;
};

class NikonSdkTransport final : public ICameraTransport, public ILiveViewTransport,
                                 public ICardCaptureTransport,
                                 public INikonDualSessionTransport,
                                 public IPcDirectCaptureTransport {
public:
    NikonSdkTransport();
    ~NikonSdkTransport() override;
    NikonSdkTransport(const NikonSdkTransport&) = delete;
    NikonSdkTransport& operator=(const NikonSdkTransport&) = delete;
    [[nodiscard]] std::string SdkVersion() const override;
    [[nodiscard]] std::vector<CameraInfo> Enumerate() override;
    [[nodiscard]] SdkCameraStatus ProbeSdkStatus(
        std::string_view stable_identity,
        std::chrono::seconds timeout);
    // Reads settings from the already-open capture session. Camera Agent uses
    // this immediately before the shutter command so the approved profile is
    // checked in the same SDK session that performs the capture.
    [[nodiscard]] SdkCameraStatus ProbeOpenCaptureSessionStatus(
        std::chrono::seconds timeout) override;
    // Product Camera Agent only: every subsequent SDK source open rejects
    // unless the open-time inventory contains exactly one D810. Legacy pair
    // experiments leave this disabled.
    void RequireExactlyOneD810ForProductAgent();
    void Open(std::string_view stable_identity, std::chrono::seconds timeout) override;
    // Experimental, explicit one-shot route. Unlike Open(), this requests
    // SaveMedia=SDRAM so the documented SDK Item/Data callback path can return
    // the JPEG directly to the PC. The original SaveMedia value is restored
    // and reread by Close(); callers must treat a close failure as terminal.
    void OpenPcDirect(
        std::string_view stable_identity,
        std::chrono::seconds timeout) override;
    [[nodiscard]] std::string Baseline(std::chrono::seconds timeout) override;
    [[nodiscard]] std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) override;
    void CaptureToCard(std::chrono::seconds image_event_timeout,
                       std::chrono::seconds transaction_timeout) override;
    void OpenLiveView(std::string_view stable_identity, std::chrono::seconds timeout) override;
    void StartLiveView(std::chrono::seconds timeout) override;
    [[nodiscard]] std::vector<unsigned char> ReadLiveViewFrame(std::chrono::seconds timeout) override;
    void StopLiveView(std::chrono::seconds timeout) override;
    void Close(std::chrono::seconds timeout) override;
    void ClosePcDirect(std::chrono::seconds timeout) override;
    [[nodiscard]] PcDirectTransportDiagnostics
        InspectPcDirectDiagnostics() const override;

    // ADR-0025 DualCamera session boundary. The module object stays open for
    // the lifetime of one operator binding, while at most one candidate source
    // object is open at any time. Candidate tokens are opaque, memory-only and
    // valid only until EndDualSession or an invalidation.
    [[nodiscard]] std::size_t BeginDualReadOnlyProbe(
        std::chrono::seconds timeout) override;
    [[nodiscard]] std::vector<std::string> BeginDualSession(
        std::chrono::seconds timeout) override;
    void OpenDualCandidateLiveView(
        std::string_view candidate_token,
        std::chrono::seconds timeout) override;
    void OpenDualBoundCapture(
        std::string_view candidate_token,
        std::chrono::seconds timeout) override;
    void OpenDualBoundPcDirectCapture(
        std::string_view candidate_token,
        std::chrono::seconds timeout) override;
    [[nodiscard]] std::string BeginPcDirectBaseline(
        std::chrono::seconds timeout) override;
    [[nodiscard]] std::vector<ImageCandidate> CaptureAndDownloadToPc(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) override;
    void CloseDualSourceKeepingModule(std::chrono::seconds timeout) override;
    [[nodiscard]] DualIdentityInvalidationReason PollDualInvalidation() override;
    void EndDualSession(std::chrono::seconds timeout) override;
    void AbandonDualSessionNoSdkCalls() noexcept override;
    [[nodiscard]] INikonDualSessionTransport::ExitState
        InspectDualSessionExitState() const noexcept override;
    [[nodiscard]] static bool LicensedAdapterAvailable() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Production implementation of the ADR-0025 binding port. It deliberately
// owns one NikonSdkTransport so the private candidate tokens never cross a
// process or get persisted. Capture uses the same owner through
// BoundSourceObjectForCapture; this class only implements the binding and Live
// View lifecycle.
class NikonDualBindingSdkAdapter final : public DualBindingSdkAdapter {
public:
    NikonDualBindingSdkAdapter();
    explicit NikonDualBindingSdkAdapter(
        std::shared_ptr<INikonDualSessionTransport> transport);
    ~NikonDualBindingSdkAdapter() override;
    NikonDualBindingSdkAdapter(const NikonDualBindingSdkAdapter&) = delete;
    NikonDualBindingSdkAdapter& operator=(const NikonDualBindingSdkAdapter&) = delete;

    [[nodiscard]] std::vector<std::string> EnumerateCandidates() override;
    [[nodiscard]] bool StartLiveView(std::size_t ordinal) override;
    [[nodiscard]] bool StopLiveView(std::size_t ordinal) override;
    [[nodiscard]] std::vector<std::uint8_t> ReadLiveViewFrame(
        std::size_t ordinal) override;
    [[nodiscard]] bool CloseCandidateSession(std::size_t ordinal) override;
    [[nodiscard]] bool EndBindingSession(
        std::chrono::seconds timeout) noexcept override;
    [[nodiscard]] DualIdentityInvalidationReason PollInvalidation() override;

    // Capture-side seam. The token comes only from a Ready
    // DualIdentitySessionBinding and is never logged or persisted.
    void OpenBoundCapture(
        std::string_view candidate_token,
        std::chrono::seconds timeout);
    void OpenBoundPcDirectCapture(
        std::string_view candidate_token,
        std::chrono::seconds timeout);
    [[nodiscard]] SdkCameraStatus ProbeOpenCaptureSessionStatus(
        std::chrono::seconds timeout);
    void CaptureToCard(
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds transaction_timeout);
    [[nodiscard]] std::string BeginPcDirectBaseline(
        std::chrono::seconds timeout);
    [[nodiscard]] std::vector<ImageCandidate> CaptureAndDownloadToPc(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout);
    void CloseBoundCapture(std::chrono::seconds timeout);
    void EndSession(std::chrono::seconds timeout);
    void AbandonSessionNoSdkCalls() noexcept;
    [[nodiscard]] NikonDualRetainedModuleState
        InspectRetainedModuleState() const noexcept;

private:
    [[nodiscard]] std::string CandidateToken(std::size_t ordinal) const;
    void FailAndInvalidate() noexcept;

    std::shared_ptr<INikonDualSessionTransport> transport_;
    std::vector<std::string> candidate_tokens_;
    std::optional<std::size_t> open_live_view_ordinal_;
    DualIdentityInvalidationReason pending_invalidation_{
        DualIdentityInvalidationReason::None};
    bool explicit_end_attempted_{};
    bool abandoned_{};
};

enum class DualSdkReadOnlyProbeError {
    None,
    CameraCountMismatch,
    SdkStartFailed,
    SdkInventoryFailed,
    SdkSourceWaitTimeout,
    SdkSourceWaitFailed,
    SdkMetadataProjectionFailed,
    SdkInventorySourceCloseFailed,
    SdkOperationFailed,
    HostSetupFailed,
};

enum class DualSdkReadOnlyProbeCleanup {
    Ended,
    EndedAfterError,
    Unconfirmed,
};

enum class DualSdkReadOnlyProbeTerminalState {
    Pass,
    Blocked,
};

struct DualSdkReadOnlyProbeResult {
    std::optional<std::size_t> sdk_d810_count;
    INikonDualSessionTransport::ExitState exit_state;
    DualSdkReadOnlyProbeError error{DualSdkReadOnlyProbeError::HostSetupFailed};
    DualSdkReadOnlyProbeCleanup cleanup{DualSdkReadOnlyProbeCleanup::Ended};
    DualSdkReadOnlyProbeTerminalState terminal_state{
        DualSdkReadOnlyProbeTerminalState::Blocked};
};

// DualCamera readiness must use the session-local candidate path rather than
// generic stable-identity inventory: two identical D810 bodies can
// legitimately report the same MAID Name/Interface pair. The candidate tokens
// are not generated, and the SDK session is fully ended before this result is
// returned. All transport exceptions are normalized into fixed anonymous
// result values; no SDK error text crosses the CLI boundary.
[[nodiscard]] DualSdkReadOnlyProbeResult RunDualSdkReadOnlyProbe(
    INikonDualSessionTransport& transport,
    std::chrono::seconds timeout);
[[nodiscard]] std::string SerializeDualSdkReadOnlyProbeResult(
    const DualSdkReadOnlyProbeResult& result);

class NikonSdkStatusExecutor final : public ISdkStatusExecutor {
public:
    NikonSdkStatusExecutor();
    ~NikonSdkStatusExecutor() override;
    NikonSdkStatusExecutor(const NikonSdkStatusExecutor&) = delete;
    NikonSdkStatusExecutor& operator=(const NikonSdkStatusExecutor&) = delete;
    [[nodiscard]] std::string SdkVersion() const override;
    [[nodiscard]] std::vector<CameraInfo> Enumerate() override;
    // SingleCamera identity-v3 only: require the status probe's SDK open-time
    // inventory to remain exactly one D810. This does not add capture, Live
    // View, WPD, or delete capabilities to the status-only executor.
    void RequireExactlyOneD810ForSingleStatus() override;
    [[nodiscard]] SdkCameraStatus ProbeSdkStatus(
        std::string_view stable_identity,
        std::chrono::seconds timeout) override;

private:
    NikonSdkTransport transport_;
};

} // namespace a0::phase0
