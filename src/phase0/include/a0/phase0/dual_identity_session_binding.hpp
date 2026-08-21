#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace a0::phase0 {

// Session-local operator binding for DualCamera (ADR-0025, GitHub Issue #9).
//
// HG-0003B was closed by a relaxation, not by finding a documented per-body SDK
// identity: no such identity exists, so this layer deliberately does NOT build
// one. The operator looks at one candidate's Live View at a time and assigns it
// to CAM-A or CAM-B. That assignment lives only in this object, in the memory of
// the one Agent process that produced the candidates, and dies with it.
//
// The three things that would be tempting to persist -- the candidate's source
// object, its ordinal, and the enumeration order that produced it -- are exactly
// the things that are not stable across a reconnect. Persisting any of them
// would silently re-create the problem HG-0003B blocked: the app would believe
// it knows which physical body it is talking to when it does not. Same for the
// USB port. This header therefore exposes no accessor that returns any of them
// as evidence; the source object is reachable only through the capture seam,
// and only while the binding is Ready.
//
// Out of scope here (kept deliberately separate):
//   - the binding wire protocol `a0.camera-agent.hardware-dual-binding.v1`
//     and its fake SDK transport (follow-up Issue #61)
//   - the WPF confirmation UI (follow-up Issue #62)
//   - the real capture backend (follow-up Issue #10)
//   - the stable `a0.camera-agent.hardware-dual.v2` protocol, which ADR-0025
//     explicitly leaves unchanged
//   - `DualIdentityBindingProof` / `DualIdentityCorrelationProvider` in
//     hardware_camera_agent.hpp, which model the older
//     documented-correlation-provider approach this decision replaced. Nothing
//     here reads or produces those, and neither may be read as evidence for the
//     other.

inline constexpr std::string_view kDualIdentitySessionBindingProviderId =
    "a0.dual-identity.session-binding";
inline constexpr std::uint32_t kDualIdentitySessionBindingProviderVersion = 1;

// Exactly two candidates, always. Not "at least two" and not "the first two of
// however many were found": three connected bodies means the operator cannot
// know which two the app picked, so that is a rejection, not a selection.
inline constexpr std::size_t kDualIdentityRequiredCandidateCount = 2;

inline constexpr std::string_view kDualIdentityCameraAliasA = "CAM-A";
inline constexpr std::string_view kDualIdentityCameraAliasB = "CAM-B";

enum class DualIdentitySessionBindingState {
    // No binding session has begun, or the previous one was invalidated.
    None,
    // Exactly two candidates are held and awaiting operator assignment.
    CollectingCandidates,
    // Both aliases are assigned; waiting for proof that every candidate's Live
    // View is stopped and the SDK session is fully closed.
    AwaitingQuiesce,
    // Assignments are complete and quiesced. Capture may use the bound source
    // objects without re-enumerating.
    Ready,
    // Something happened that makes the assignment untrustworthy. Capture is
    // refused until a fresh binding session completes.
    Invalid,
};

// Why a Ready binding stopped being trustworthy. ADR-0025 lists these as
// immediate invalidation triggers: every one of them can change which physical
// body a previously captured source object refers to, and none of them is
// detectable after the fact from the object alone.
enum class DualIdentityInvalidationReason {
    None,
    AgentRestart,
    UsbReconnect,
    CameraCountChanged,
    TopologyChanged,
    SdkManagerRecreated,
    SdkError,
};

[[nodiscard]] std::string_view DualIdentityInvalidationReasonName(
    DualIdentityInvalidationReason reason) noexcept;

class DualIdentitySessionBindingError final : public std::runtime_error {
public:
    DualIdentitySessionBindingError(std::string code, std::string message);

    [[nodiscard]] const std::string& Code() const noexcept;

private:
    std::string code_;
};

// One SDK candidate offered to the operator during a binding session.
//
// `ordinal` exists only so the caller can say which candidate it means inside
// this session. It is not identity, it is not stable across sessions, and it
// never leaves this process as evidence.
struct DualIdentityCandidate {
    std::size_t ordinal{};
    // Opaque handle to the SDK source object. Treated as a value this layer
    // stores and hands back; never parsed, never compared for identity, never
    // published.
    std::string source_object_token;
};

// Everything this layer is allowed to publish about a completed binding.
// ADR-0025 limits public evidence to alias, provider, version, confirmedAt and
// invalidationReason -- so that is the entire struct. Adding a field here is a
// decision change, not an implementation detail: preview frames, raw
// identifiers, serials, source objects and candidate ordinals are all excluded
// on purpose.
struct DualIdentitySessionBindingEvidence {
    std::string camera_alias;
    std::string provider_id;
    std::uint32_t provider_version{};
    std::string confirmed_at_utc;
    std::string invalidation_reason;
};

// The one counter here that can actually move.
//
// An earlier draft also carried camera_command_count / card_access_count /
// delete_count / automatic_retry_count / enumeration_count and asserted they
// stayed zero. Nothing in this translation unit could ever increment them, so
// those assertions could not fail and only looked like coverage. The property
// they claimed to check is structural instead: this header and its .cpp depend
// on no camera, card, WPD or SDK type, so there is no call they could make. A
// test that wants to guard that should check the dependency, not a counter that
// is zero by construction.
struct DualIdentitySessionBindingSafetyCounters {
    // Counts every time capture asked for a bound source object. Capture has no
    // other way to obtain one, so a non-zero value here with no enumeration
    // anywhere is what "capture reuses the bound object" means in practice.
    std::size_t source_object_reuse_count{};
};

class DualIdentitySessionBinding final {
public:
    DualIdentitySessionBinding() noexcept = default;

    [[nodiscard]] DualIdentitySessionBindingState State() const noexcept;

    // Starts a session over exactly two candidates.
    //
    // The previous session is discarded first, before validation, so a refused
    // BeginBinding cannot leave an older Ready binding in place. Validating
    // first would mean that plugging in a third body and failing to re-bind
    // leaves capture still using source objects from before the change --
    // exactly the stale-attribution case this whole design exists to prevent.
    void BeginBinding(const std::vector<DualIdentityCandidate>& candidates);

    // Assigns one candidate to one alias, exactly once each way. Re-using a
    // candidate or an alias is refused rather than overwritten -- an operator
    // who assigned the same body twice has misread the Live View, and silently
    // taking the second answer would hide that.
    void ConfirmAlias(std::size_t candidate_ordinal, std::string_view camera_alias);

    // Records that a candidate's Live View is stopped and its SDK session is
    // fully closed. Both must be observed for every candidate before Ready.
    //
    // Only accepted once that candidate has an alias. The operator's flow is
    // "view this body's Live View, assign it, stop it", so a quiesce report for
    // an unassigned candidate would describe a Live View the operator has not
    // finished with, and that stale flag could later satisfy CompleteBinding
    // while a Live View is open again.
    void ConfirmCandidateQuiesced(
        std::size_t candidate_ordinal,
        bool live_view_stopped,
        bool sdk_session_closed);

    // Promotes to Ready. Refused unless both aliases are assigned and every
    // candidate is quiesced, so there is no path where capture starts while a
    // Live View is still open.
    void CompleteBinding(std::string_view confirmed_at_utc);

    // Immediate, one-way transition out of Ready. Safe to call repeatedly; the
    // first reason is the one kept, because that is the one that explains why
    // the binding stopped being trustworthy.
    void Invalidate(DualIdentityInvalidationReason reason) noexcept;

    [[nodiscard]] bool IsReady() const noexcept;

    // The capture seam. Returns the bound source object for an alias without
    // re-enumerating, and only while Ready. Callers cannot reach a source
    // object any other way, which is what makes "capture never re-enumerates"
    // checkable rather than a convention.
    //
    // Returned by value on purpose: a reference would point into the candidate
    // vector, which BeginBinding clears, so a caller that held it across an
    // invalidate-and-rebind would read freed memory.
    [[nodiscard]] std::string BoundSourceObjectForCapture(std::string_view camera_alias);

    // Only a completed binding (Ready, or a Ready one later invalidated) has
    // evidence to publish. A half-assigned session would otherwise emit records
    // shaped exactly like a confirmed binding, differing only by an empty
    // confirmedAt, which a consumer could easily read as confirmed.
    [[nodiscard]] std::vector<DualIdentitySessionBindingEvidence> PublishableEvidence()
        const;

    [[nodiscard]] DualIdentitySessionBindingSafetyCounters SafetyCounters()
        const noexcept;

private:
    struct Assignment {
        std::size_t ordinal{};
        std::string source_object_token;
        std::string camera_alias;
        bool live_view_stopped{};
        bool sdk_session_closed{};
    };

    [[nodiscard]] Assignment* FindByOrdinal(std::size_t ordinal) noexcept;
    [[nodiscard]] const Assignment* FindByAlias(std::string_view alias) const noexcept;
    [[nodiscard]] bool EveryCandidateQuiesced() const noexcept;

    DualIdentitySessionBindingState state_{DualIdentitySessionBindingState::None};
    DualIdentityInvalidationReason invalidation_reason_{
        DualIdentityInvalidationReason::None};
    std::vector<Assignment> candidates_;
    std::string confirmed_at_utc_;
    DualIdentitySessionBindingSafetyCounters safety_counters_;
};

} // namespace a0::phase0
