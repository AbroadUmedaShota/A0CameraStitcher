#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "a0/phase0/dual_identity_session_binding.hpp"

namespace a0::phase0 {

// Wire protocol `a0.camera-agent.hardware-dual-bound-live-view.v1` (Issue #291).
//
// A Live View that an operator can open while a binding is Ready, without
// reopening candidate selection. The binding protocol deliberately refuses a
// Live View for a candidate that was already assigned, because that would undo
// the quiesce a completed binding rests on (see dual_binding_camera_agent.cpp).
// This schema is the one sanctioned way back in: it reopens an assigned
// candidate under the retained Module, and it is held to the same rules.
//
//   - At most one SDK source / Live View is open at any time. The adapter is
//     the authority on that, not this class: "closed" is read back from the
//     adapter, never inferred from a flag kept here.
//   - Nothing opened here survives into a capture transaction. activate-capture
//     is refused while anything is open, and the capture host's own preflight
//     (WPD is never opened with a source or Live View open) is unchanged.
//   - Any SDK failure or invalidation event invalidates the binding. It is
//     never retried and never reopened automatically.
//   - A frame exists only inside the response that carries it. It is not
//     logged, journaled, or written to a file.
//   - It does not extend the Agent's lifetime. A host that stops hearing from
//     its client stops and closes the Live View on its own (OnIdle).
//
// The schema is the same for every host that serves it; only the `marker`
// differs (the binding pipe is `HardwareBinding`, the capture pipe is
// `Hardware`). Today only the binding host serves it.

inline constexpr std::string_view kDualBoundLiveViewSchemaVersion =
    "a0.camera-agent.hardware-dual-bound-live-view.v1";

// A capture is admitted only while the host has at least this much lifetime
// left (watchdog 180 s + reply/ack 3 s + cleanup reserve 2 s). The same value
// is enforced independently by dual_hardware_camera_agent.cpp
// (kDualMinimumHostAdmissionMilliseconds); this copy only computes the number
// reported back to the client. dual_bound_live_view_tests.cpp asserts the two
// values are equal.
inline constexpr std::uint64_t kBoundLiveViewCaptureAdmissionReserveMilliseconds =
    185'000;
// Provisional value, to be replaced by hardware measurement: a Live View is not
// started when the host has less lifetime than this.
inline constexpr std::uint64_t kBoundLiveViewMinimumHostRemainingMilliseconds = 20'000;
// Provisional value, to be replaced by hardware measurement: with no request in
// this schema for this long, the host stops and closes the Live View itself. The
// pipe loop calls OnIdle at least every 5 seconds, so the worst case is roughly
// this plus 5 seconds.
inline constexpr std::uint64_t kBoundLiveViewIdleStopMilliseconds = 10'000;

enum class DualBoundLiveViewOperation {
    start,
    get_frame,
    stop,
};

class DualBoundLiveViewProtocolError final : public std::runtime_error {
public:
    DualBoundLiveViewProtocolError(std::string code, std::string message);

    [[nodiscard]] const std::string& Code() const noexcept;

private:
    std::string code_;
};

struct DualBoundLiveViewRequest {
    std::string request_id;
    DualBoundLiveViewOperation operation{DualBoundLiveViewOperation::stop};
    std::string session_id;
    // Empty for stop.
    std::string camera_alias;
};

// `expected_marker` is the marker of the host doing the parsing. A request that
// carries the other host's marker is refused, in both directions, without either
// host having to know the other exists.
[[nodiscard]] DualBoundLiveViewRequest ParseDualBoundLiveViewRequest(
    std::string_view json, std::string_view expected_marker);

// What the controller needs to know about, and do to, the binding it serves.
// Implemented by the host dispatcher that owns the binding.
class DualBoundLiveViewBindingPort {
public:
    virtual ~DualBoundLiveViewBindingPort() = default;

    // Empty when no session is being served.
    [[nodiscard]] virtual std::string BoundLiveViewSessionId() const = 0;
    [[nodiscard]] virtual DualIdentitySessionBindingState
        BoundLiveViewBindingState() const noexcept = 0;
    [[nodiscard]] virtual DualIdentityInvalidationReason
        BoundLiveViewInvalidationReason() const noexcept = 0;
    // Session-local selector for the SDK adapter. Throws
    // DualIdentitySessionBindingError unless the binding is Ready and knows the
    // alias. Never leaves the process.
    [[nodiscard]] virtual std::size_t BoundLiveViewCandidateOrdinal(
        std::string_view camera_alias) = 0;
    virtual void InvalidateBoundLiveViewBinding(
        DualIdentityInvalidationReason reason) noexcept = 0;
};

enum class DualBoundLiveViewState {
    // Nothing of ours is open.
    Closed,
    // One alias has its source and Live View open.
    Live,
    // The binding was invalidated. Nothing is served again until a new binding.
    Terminal,
};

struct DualBoundLiveViewOptions {
    // Milliseconds left in the host's shared lifetime. Required: a controller
    // that cannot tell how long the host has left cannot honor the lifetime
    // rules, so construction without one is refused.
    std::function<std::uint64_t()> host_remaining_milliseconds;
    // Monotonic milliseconds, for the idle timeout. Empty uses steady_clock.
    std::function<std::uint64_t()> monotonic_milliseconds;
};

// Counters that can actually move (see the same note in
// dual_binding_camera_agent.hpp). There is deliberately no frame-persistence or
// retry counter: nothing here can write a frame or retry, so such a counter
// would be zero by construction.
struct DualBoundLiveViewCounters {
    std::size_t start_count{};
    std::size_t frame_count{};
    // stop-bound-live-view requests that actually closed something.
    std::size_t stop_count{};
    // Closes performed by OnIdle because the client went quiet.
    std::size_t idle_stop_count{};
};

class DualBindingSdkAdapter;

class DualBoundLiveViewController final {
public:
    DualBoundLiveViewController(
        DualBindingSdkAdapter& adapter, DualBoundLiveViewOptions options);

    // True when `request_json` names this schema. Anything unparsable is false,
    // so it falls through to the host's own protocol and its own refusals.
    [[nodiscard]] static bool IsBoundLiveViewRequest(
        std::string_view request_json) noexcept;
    // The response for a host that was started without a controller.
    [[nodiscard]] static std::string UnavailableResponse(
        std::string_view request_json, std::string_view marker) noexcept;

    // Serves one request. Checks run in this order and the first failing one
    // answers. Only the invalidation poll in 3 and the SDK calls in 8 touch the
    // adapter (the open-state reads in 6 are not SDK calls):
    //   1. schema, marker, fields, operation    DualBoundLiveViewProtocolRequired,
    //                                           UnexpectedField, InvalidSessionId,
    //                                           UnknownCameraAlias, UnsupportedOperation
    //   2. session id                           SessionMismatch
    //   3. invalidation event, or binding Invalid   BindingInvalidated
    //   4. binding not Ready                    BindingNotReady
    //   5. start: host lifetime                 HostLifetimeInsufficient
    //   6. start: anything already open         BoundLiveViewAlreadyOpen
    //      frame: alias is not the Live one     BoundLiveViewNotActive
    //   7. frame: empty / over the size cap     LiveViewFrameUnavailable /
    //                                           LiveViewFrameTooLarge (Live stays open)
    //   8. an SDK failure while starting, reading, stopping or closing invalidates
    //      the binding                          BindingInvalidated
    // A start the SDK refuses without any error event, with nothing left open, is
    // LiveViewStartFailed and leaves the binding Ready.
    [[nodiscard]] std::string Handle(
        std::string_view request_json,
        std::string_view marker,
        DualBoundLiveViewBindingPort& port) noexcept;

    // Stops and closes the Live View when the client has gone quiet for
    // kBoundLiveViewIdleStopMilliseconds. A close that cannot be confirmed
    // invalidates the binding.
    void OnIdle(DualBoundLiveViewBindingPort& port) noexcept;

    // Closed in our own bookkeeping AND closed according to the adapter. Used by
    // activate-capture; this is deliberately not just `State() == Closed`.
    [[nodiscard]] bool IsFullyClosed() const noexcept;

    // A new binding session began, or the old one ended cleanly.
    void NotifyBindingReset() noexcept;
    // The binding stopped being trustworthy.
    void NotifyBindingInvalidated() noexcept;

    [[nodiscard]] DualBoundLiveViewState State() const noexcept;
    [[nodiscard]] DualBoundLiveViewCounters Counters() const noexcept;

private:
    struct CloseOutcome;

    [[nodiscard]] std::string HandleStart(
        const DualBoundLiveViewRequest& request,
        std::string_view marker,
        DualBoundLiveViewBindingPort& port);
    [[nodiscard]] std::string HandleGetFrame(
        const DualBoundLiveViewRequest& request,
        std::string_view marker,
        DualBoundLiveViewBindingPort& port);
    [[nodiscard]] std::string HandleStop(
        const DualBoundLiveViewRequest& request,
        std::string_view marker,
        DualBoundLiveViewBindingPort& port);

    [[nodiscard]] CloseOutcome StopAndClose() noexcept;
    // Invalidates the binding and answers with the facts read back from the
    // adapter. `stop_attempt` carries the booleans of a close that was just
    // attempted, when there was one.
    [[nodiscard]] std::string FailClosed(
        std::string_view marker,
        std::string_view request_id,
        DualBoundLiveViewBindingPort& port,
        DualIdentityInvalidationReason reason,
        const CloseOutcome* stop_attempt = nullptr) noexcept;
    [[nodiscard]] std::string InvalidatedResponse(
        std::string_view marker,
        std::string_view request_id,
        const DualBoundLiveViewBindingPort& port,
        const CloseOutcome* stop_attempt = nullptr) const noexcept;

    [[nodiscard]] std::uint64_t HostRemainingMilliseconds() const noexcept;
    [[nodiscard]] std::uint64_t CaptureAdmissionRemainingMilliseconds() const noexcept;
    [[nodiscard]] std::uint64_t NowMilliseconds() const noexcept;

    DualBindingSdkAdapter& adapter_;
    DualBoundLiveViewOptions options_;
    DualBoundLiveViewState state_{DualBoundLiveViewState::Closed};
    std::string active_alias_;
    std::size_t active_ordinal_{};
    std::uint64_t last_request_milliseconds_{};
    DualBoundLiveViewCounters counters_;
};

} // namespace a0::phase0
