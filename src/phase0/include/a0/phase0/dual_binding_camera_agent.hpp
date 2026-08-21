#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "a0/phase0/dual_identity_session_binding.hpp"

namespace a0::phase0 {

// Wire protocol for session-local operator binding (ADR-0025, GitHub Issue #61).
//
// This is the transport that makes the binding core in
// dual_identity_session_binding.hpp reachable from another process. The core
// decides what a valid binding is; this layer only carries requests to it,
// drives the SDK candidate/Live View lifecycle around it, and decides what is
// allowed back out.
//
// Two things it deliberately does not do:
//
//   - It does not touch `a0.camera-agent.hardware-dual.v2`. That protocol is
//     stable and ADR-0025 leaves it unchanged, so binding got its own schema,
//     its own marker and its own pipe rather than five more v2 operations. The
//     marker differs from v2's on purpose: a v2 request can then never be
//     accepted here by accident, and vice versa, without either dispatcher
//     having to know the other exists.
//
//   - It does not persist anything. No source object, no candidate ordinal, no
//     preview frame, no enumeration order. This is structural rather than
//     policed by a counter: the implementing translation unit depends on no
//     filesystem, registry or store type, so there is no call it could make to
//     write one. Live View frames exist only inside the single response that
//     carries them.
//
// Candidate ordinals do appear on the wire. They have to -- the operator has to
// be able to say "the one I am looking at now" -- but they are session-local
// selectors, not identity: they are meaningless once the session ends, and the
// evidence allowlist below excludes them.

inline constexpr std::string_view kDualBindingCameraAgentSchemaVersion =
    "a0.camera-agent.hardware-dual-binding.v1";
// Deliberately not "Hardware" (the v2 marker). See the note above.
inline constexpr std::string_view kDualBindingCameraAgentMarker = "HardwareBinding";
inline constexpr std::string_view kDefaultDualBindingCameraAgentPipeName =
    "A0CameraStitcher.CameraAgent.HardwareDualBinding.v1";

// A Live View frame is a transient preview, not an image the product keeps, so
// it is capped rather than streamed. 256 KiB of raw bytes becomes roughly
// 341 KB of base64, which leaves the 1 MiB pipe frame comfortable room for the
// envelope around it. A larger frame is a rejection, never a truncation: half a
// preview is worse than no preview, because the operator would be deciding
// which body they are looking at from an image that is not all there.
inline constexpr std::size_t kMaximumBindingLiveViewFrameBytes = 256U * 1024U;

enum class DualBindingCameraAgentOperation {
    begin_binding,
    start_candidate_live_view,
    get_candidate_live_view_frame,
    confirm_alias,
    complete_binding,
};

class DualBindingCameraAgentProtocolError final : public std::runtime_error {
public:
    DualBindingCameraAgentProtocolError(std::string code, std::string message);

    [[nodiscard]] const std::string& Code() const noexcept;

private:
    std::string code_;
};

struct DualBindingCameraAgentRequest {
    std::string request_id;
    DualBindingCameraAgentOperation operation{
        DualBindingCameraAgentOperation::begin_binding};
    // Empty for begin-binding, which is the operation that creates one.
    std::string session_id;
    std::size_t candidate_ordinal{};
    std::string camera_alias;
    std::string confirmed_at_utc;
};

[[nodiscard]] DualBindingCameraAgentRequest ParseDualBindingCameraAgentRequest(
    std::string_view json);

// The SDK candidate/Live View lifecycle the binding protocol drives.
//
// Only the fake implementation below exists today. The real Nikon SDK one is
// Issue #10 and is not part of this protocol's scope -- which is the point of
// having the seam: the whole binding flow, including every rejection path, is
// reachable and testable without a camera.
class DualBindingSdkAdapter {
public:
    virtual ~DualBindingSdkAdapter() = default;

    // Opens the SDK manager and enumerates the attached bodies, returning one
    // opaque source-object token each, in whatever order enumeration produced.
    //
    // That order is not identity and is not stable across calls. If it were,
    // there would be no reason for any of this to exist. Callers must treat the
    // returned tokens as values to carry, never to compare or interpret.
    [[nodiscard]] virtual std::vector<std::string> EnumerateCandidates() = 0;

    [[nodiscard]] virtual bool StartLiveView(std::size_t ordinal) = 0;
    [[nodiscard]] virtual bool StopLiveView(std::size_t ordinal) = 0;

    // Transient preview bytes for the running Live View. The protocol layer
    // hands these straight back in one response and keeps no copy.
    [[nodiscard]] virtual std::vector<std::uint8_t> ReadLiveViewFrame(
        std::size_t ordinal) = 0;

    // Closes this candidate's SDK session. The result is reported to the
    // binding core as `sdk_session_closed`; a false keeps the binding out of
    // Ready instead of being retried or ignored.
    [[nodiscard]] virtual bool CloseCandidateSession(std::size_t ordinal) = 0;

    // Any typed invalidation the adapter observed since the previous call, or
    // None. The protocol is request/response with no server push, so this is
    // polled at the start of every session-scoped operation and delivered as a
    // typed rejection in that operation's response. A caller therefore learns
    // about an agent restart, a USB reconnect or an SDK error at the first
    // moment it could possibly act on one, and never gets a success built on a
    // binding that stopped being trustworthy in between.
    [[nodiscard]] virtual DualIdentityInvalidationReason PollInvalidation() = 0;
};

// How the fake SDK should behave for one test. Everything here exists because
// some required rejection path needs it; there is no "make it work" knob.
struct DualBindingFakeSdkOptions {
    // 0, 1, 3 ... all have to be rejected by the core, so all have to be
    // producible here. Two is the only accepted count.
    std::size_t candidate_count{kDualIdentityRequiredCandidateCount};
    // Emits the same source-object token for every candidate, which the core
    // must refuse: two bodies that look identical are exactly the case where a
    // wrong alias assignment cannot be detected later.
    bool duplicate_source_object_tokens{};
    // Emits an empty token for every candidate, which the core must also refuse:
    // a candidate with no source object cannot be captured from later, and
    // discovering that at capture time would be far too late.
    bool empty_source_object_tokens{};
    bool fail_start_live_view{};
    bool fail_stop_live_view{};
    bool fail_close_candidate_session{};
    std::size_t live_view_frame_bytes{4096};
    // Already queued before the operation under test runs. Reported once by the
    // next PollInvalidation, then cleared.
    DualIdentityInvalidationReason pending_invalidation{
        DualIdentityInvalidationReason::None};
    // Queued by EnumerateCandidates itself, which is a different case: the event
    // happened *while* the candidates being offered were collected, so those
    // candidates may already be stale. Nothing queued beforehand can express
    // that, because begin-binding drains stale events first.
    DualIdentityInvalidationReason invalidation_during_enumeration{
        DualIdentityInvalidationReason::None};
};

// Fake SDK candidate/Live View lifecycle adapter.
//
// It is not a permissive stub: it enforces the same one-Live-View-at-a-time
// rule the protocol layer enforces, and records violations instead of tolerating
// them. If the protocol layer ever let two Live Views run at once, this class is
// what would notice.
class DualBindingFakeSdkAdapter final : public DualBindingSdkAdapter {
public:
    DualBindingFakeSdkAdapter() noexcept = default;
    explicit DualBindingFakeSdkAdapter(DualBindingFakeSdkOptions options) noexcept;

    [[nodiscard]] std::vector<std::string> EnumerateCandidates() override;
    [[nodiscard]] bool StartLiveView(std::size_t ordinal) override;
    [[nodiscard]] bool StopLiveView(std::size_t ordinal) override;
    [[nodiscard]] std::vector<std::uint8_t> ReadLiveViewFrame(
        std::size_t ordinal) override;
    [[nodiscard]] bool CloseCandidateSession(std::size_t ordinal) override;
    [[nodiscard]] DualIdentityInvalidationReason PollInvalidation() override;

    void RaiseInvalidation(DualIdentityInvalidationReason reason) noexcept;

    [[nodiscard]] std::size_t EnumerationCount() const noexcept;
    [[nodiscard]] std::size_t ActiveLiveViewCount() const noexcept;
    // Non-zero means the protocol layer allowed two Live Views to overlap.
    [[nodiscard]] std::size_t ConcurrentLiveViewViolationCount() const noexcept;
    [[nodiscard]] std::size_t ClosedCandidateSessionCount() const noexcept;

private:
    DualBindingFakeSdkOptions options_;
    std::vector<bool> live_view_active_;
    std::vector<bool> session_closed_;
    std::size_t enumeration_count_{};
    std::size_t concurrent_live_view_violations_{};
};

// Counters that can actually move.
//
// An earlier draft also carried persisted_frame_count / automatic_retry_count
// and asserted they stayed zero. Nothing in the implementation can increment
// them, so those assertions could not fail; they would have looked like coverage
// without being any. The properties they claimed are structural instead (see the
// "does not persist anything" note at the top of this header), and the retry
// count is zero because no code path retries.
struct DualBindingCameraAgentSafetyCounters {
    std::size_t binding_session_count{};
    std::size_t live_view_start_count{};
    std::size_t live_view_frame_count{};
    // Requests refused because they named a session this dispatcher is not
    // currently serving -- an old session after a restart, or a session that was
    // replaced. Non-zero here is the protocol working, not failing.
    std::size_t rejected_stale_session_count{};
};

// Serves the five binding operations over one pure in-memory boundary.
//
// Without an adapter every operation fails closed with SdkUnavailable: an agent
// that cannot see any candidates must not invent a session for the operator to
// confirm.
class DualBindingCameraAgentDispatcher final {
public:
    DualBindingCameraAgentDispatcher() noexcept = default;
    explicit DualBindingCameraAgentDispatcher(
        std::shared_ptr<DualBindingSdkAdapter> adapter) noexcept;

    [[nodiscard]] std::string Handle(std::string_view request_json) noexcept;

    [[nodiscard]] DualBindingCameraAgentSafetyCounters SafetyCounters()
        const noexcept;

    // The capture seam, for the process that owns this dispatcher. Mirrors
    // DualIdentitySessionBinding::BoundSourceObjectForCapture and is available
    // only while the binding is Ready. Nothing on the wire exposes this.
    [[nodiscard]] std::string BoundSourceObjectForCapture(
        std::string_view camera_alias);

    [[nodiscard]] DualIdentitySessionBindingState BindingState() const noexcept;

    // Named-pipe host lifecycle hooks, same shape as the Dual v2 dispatcher.
    // Binding has no idle-driven state and no operation that asks the host to
    // stop, so these stay a no-op and false; the host's own absolute lifetime
    // bound is what stops it.
    void OnIdle() noexcept;
    [[nodiscard]] bool ShouldStop() const noexcept;

private:
    [[nodiscard]] std::string HandleBeginBinding(
        const DualBindingCameraAgentRequest& request);
    [[nodiscard]] std::string HandleStartCandidateLiveView(
        const DualBindingCameraAgentRequest& request);
    [[nodiscard]] std::string HandleGetCandidateLiveViewFrame(
        const DualBindingCameraAgentRequest& request);
    [[nodiscard]] std::string HandleConfirmAlias(
        const DualBindingCameraAgentRequest& request);
    [[nodiscard]] std::string HandleCompleteBinding(
        const DualBindingCameraAgentRequest& request);

    void InvalidateSession(DualIdentityInvalidationReason reason) noexcept;
    [[nodiscard]] std::string InvalidatedRejection(std::string_view request_id) const;

    std::shared_ptr<DualBindingSdkAdapter> adapter_;
    DualIdentitySessionBinding binding_;
    std::string session_id_;
    // Empty session id plus this being set is impossible; both are cleared and
    // set together with the session.
    std::optional<std::size_t> active_live_view_ordinal_;
    // Candidates whose alias the operator already confirmed. The binding core
    // knows this too but exposes no accessor for it, and Live View has to be
    // refused for them: their SDK session is closed and reopening it would undo
    // the quiesce.
    std::vector<std::size_t> assigned_ordinals_;
    // Kept here rather than read back from the core, which exposes an
    // invalidation reason only through evidence and therefore only for a binding
    // that reached Ready. A session invalidated while candidates were still
    // being assigned has a reason worth reporting too.
    DualIdentityInvalidationReason invalidation_reason_{
        DualIdentityInvalidationReason::None};
    std::uint64_t session_counter_{};
    std::string session_id_seed_;
    DualBindingCameraAgentSafetyCounters safety_counters_;
};

// Failure-injection seam for named-pipe host contract tests only, identical in
// shape and meaning to the Dual v2 one.
struct DualBindingCameraAgentPipeFailureInjectionForTesting {
    bool fail_response_header_write{};
    bool fail_response_body_write{};
    bool fail_delivery_ack_wait{};
    bool fail_response_flush{};
};

// Serves the binding protocol over one dedicated local named pipe.
//
// Framing, current-logon access boundary, bounded delivery-ACK semantics and
// absolute-lifetime policy are not reimplemented here: this shares the exact
// loop that already serves the Single-camera and Dual v2 hosts (see
// hardware_camera_agent_pipe.cpp). A second copy of that loop is how the two
// hosts would eventually disagree about what an oversize frame or a missing ACK
// means, which is precisely the non-regression this protocol has to preserve.
[[nodiscard]] int RunDualBindingCameraAgentNamedPipeServer(
    std::string_view pipe_name,
    DualBindingCameraAgentDispatcher& dispatcher,
    bool serve_once = false,
    DualBindingCameraAgentPipeFailureInjectionForTesting failure_injection = {},
    std::optional<std::chrono::milliseconds> lifetime_budget_for_testing =
        std::nullopt);

} // namespace a0::phase0
