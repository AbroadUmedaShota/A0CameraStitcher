#include "a0/phase0/dual_identity_session_binding.hpp"
#include "a0/phase0/wpd_alias_proof.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace a0::phase0;

int failures = 0;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Action>
void CheckBindingError(
    std::string_view expected_code,
    Action&& action,
    std::string_view message) {
    try {
        action();
        Check(false, message);
    } catch (const DualIdentitySessionBindingError& error) {
        Check(error.Code() == expected_code, message);
    } catch (...) {
        Check(false, message);
    }
}

std::vector<DualIdentityCandidate> TwoCandidates() {
    return {DualIdentityCandidate{0, "sdk-source-0"},
            DualIdentityCandidate{1, "sdk-source-1"}};
}

// Drives a binding all the way to Ready so the tests that care about what
// happens *after* Ready do not each repeat the ceremony.
DualIdentitySessionBinding ReadyBinding() {
    DualIdentitySessionBinding binding;
    binding.BeginBinding(TwoCandidates());
    binding.ConfirmAlias(0, kDualIdentityCameraAliasA);
    binding.ConfirmCandidateQuiesced(0, true, true);
    binding.ConfirmAlias(1, kDualIdentityCameraAliasB);
    binding.ConfirmCandidateQuiesced(1, true, true);
    binding.CompleteBinding("2026-08-21T00:00:00Z");
    return binding;
}

void CandidateCountMustBeExactlyTwo() {
    DualIdentitySessionBinding binding;

    CheckBindingError(
        "CandidateCountNotTwo",
        [&] { binding.BeginBinding({}); },
        "Zero candidates must be refused.");
    CheckBindingError(
        "CandidateCountNotTwo",
        [&] { binding.BeginBinding({DualIdentityCandidate{0, "sdk-source-0"}}); },
        "One candidate must be refused.");
    CheckBindingError(
        "CandidateCountNotTwo",
        [&] {
            binding.BeginBinding(
                {DualIdentityCandidate{0, "sdk-source-0"},
                 DualIdentityCandidate{1, "sdk-source-1"},
                 DualIdentityCandidate{2, "sdk-source-2"}});
        },
        "Three candidates must be refused rather than silently narrowed to two.");

    Check(
        binding.State() == DualIdentitySessionBindingState::None,
        "A refused BeginBinding must leave the binding unstarted.");

    binding.BeginBinding(TwoCandidates());
    Check(
        binding.State() == DualIdentitySessionBindingState::CollectingCandidates,
        "Exactly two candidates must open a binding session.");
}

void DuplicateCandidateAndAliasAssignmentsAreRefused() {
    DualIdentitySessionBinding binding;
    binding.BeginBinding(TwoCandidates());
    binding.ConfirmAlias(0, kDualIdentityCameraAliasA);

    CheckBindingError(
        "CandidateAlreadyAssigned",
        [&] { binding.ConfirmAlias(0, kDualIdentityCameraAliasB); },
        "Assigning the same candidate twice must be refused, not overwritten.");
    CheckBindingError(
        "AliasAlreadyAssigned",
        [&] { binding.ConfirmAlias(1, kDualIdentityCameraAliasA); },
        "Assigning the same alias twice must be refused.");
    CheckBindingError(
        "UnknownCandidateOrdinal",
        [&] { binding.ConfirmAlias(7, kDualIdentityCameraAliasB); },
        "An ordinal outside this session must be refused.");
    CheckBindingError(
        "UnknownCameraAlias",
        [&] { binding.ConfirmAlias(1, "CAM-C"); },
        "Only CAM-A and CAM-B may be assigned.");
}

void IncompleteAliasAssignmentCannotComplete() {
    DualIdentitySessionBinding binding;
    binding.BeginBinding(TwoCandidates());
    binding.ConfirmAlias(0, kDualIdentityCameraAliasA);
    binding.ConfirmCandidateQuiesced(0, true, true);
    // Candidate 1 is deliberately left unassigned: quiesce cannot be recorded
    // for it, and the binding must not complete without CAM-B.

    CheckBindingError(
        "AliasAssignmentIncomplete",
        [&] { binding.CompleteBinding("2026-08-21T00:00:00Z"); },
        "A binding missing CAM-B must not complete.");
    Check(!binding.IsReady(), "An incomplete binding must not be Ready.");
}

void LiveViewStopAndSdkCloseAreRequiredBeforeReady() {
    // Live View still open on one candidate.
    DualIdentitySessionBinding open_live_view;
    open_live_view.BeginBinding(TwoCandidates());
    open_live_view.ConfirmAlias(0, kDualIdentityCameraAliasA);
    open_live_view.ConfirmAlias(1, kDualIdentityCameraAliasB);
    open_live_view.ConfirmCandidateQuiesced(0, true, true);
    open_live_view.ConfirmCandidateQuiesced(1, false, true);
    CheckBindingError(
        "CandidateNotQuiesced",
        [&] { open_live_view.CompleteBinding("2026-08-21T00:00:00Z"); },
        "An unstopped Live View must block Ready.");

    // SDK session not fully closed on one candidate.
    DualIdentitySessionBinding open_sdk_session;
    open_sdk_session.BeginBinding(TwoCandidates());
    open_sdk_session.ConfirmAlias(0, kDualIdentityCameraAliasA);
    open_sdk_session.ConfirmAlias(1, kDualIdentityCameraAliasB);
    open_sdk_session.ConfirmCandidateQuiesced(0, true, false);
    open_sdk_session.ConfirmCandidateQuiesced(1, true, true);
    CheckBindingError(
        "CandidateNotQuiesced",
        [&] { open_sdk_session.CompleteBinding("2026-08-21T00:00:00Z"); },
        "An unclosed SDK session must block Ready.");

    // Never confirmed at all.
    DualIdentitySessionBinding never_confirmed;
    never_confirmed.BeginBinding(TwoCandidates());
    never_confirmed.ConfirmAlias(0, kDualIdentityCameraAliasA);
    never_confirmed.ConfirmAlias(1, kDualIdentityCameraAliasB);
    CheckBindingError(
        "CandidateNotQuiesced",
        [&] { never_confirmed.CompleteBinding("2026-08-21T00:00:00Z"); },
        "Absent quiesce confirmation must block Ready, not be assumed.");
}

void CompletedBindingBecomesReady() {
    auto binding = ReadyBinding();
    Check(binding.IsReady(), "A fully assigned and quiesced binding must be Ready.");
    Check(
        binding.State() == DualIdentitySessionBindingState::Ready,
        "State must report Ready.");
}

void EveryInvalidationTriggerLeavesReady() {
    const DualIdentityInvalidationReason reasons[] = {
        DualIdentityInvalidationReason::AgentRestart,
        DualIdentityInvalidationReason::UsbReconnect,
        DualIdentityInvalidationReason::CameraCountChanged,
        DualIdentityInvalidationReason::TopologyChanged,
        DualIdentityInvalidationReason::SdkManagerRecreated,
        DualIdentityInvalidationReason::SdkError,
    };

    for (const auto reason : reasons) {
        auto binding = ReadyBinding();
        binding.Invalidate(reason);
        Check(
            binding.State() == DualIdentitySessionBindingState::Invalid,
            "Every ADR-0025 invalidation trigger must leave Ready immediately.");
        Check(!binding.IsReady(), "An invalidated binding must not report Ready.");

        const auto evidence = binding.PublishableEvidence();
        Check(evidence.size() == 2, "Both aliases must still be described after invalidation.");
        if (!evidence.empty()) {
            Check(
                evidence.front().invalidation_reason ==
                    DualIdentityInvalidationReasonName(reason),
                "The published invalidation reason must name the trigger.");
        }
    }
}

void FirstInvalidationReasonIsKept() {
    auto binding = ReadyBinding();
    binding.Invalidate(DualIdentityInvalidationReason::UsbReconnect);
    binding.Invalidate(DualIdentityInvalidationReason::SdkError);

    const auto evidence = binding.PublishableEvidence();
    Check(!evidence.empty(), "Evidence must survive invalidation.");
    if (!evidence.empty()) {
        Check(
            evidence.front().invalidation_reason == "UsbReconnect",
            "The first reason explains the loss of trust and must be the one kept.");
    }
}

void CaptureSeamRequiresReadyAndNeverReEnumerates() {
    DualIdentitySessionBinding collecting;
    collecting.BeginBinding(TwoCandidates());
    CheckBindingError(
        "BindingNotReady",
        [&] { (void)collecting.BoundSourceObjectForCapture(kDualIdentityCameraAliasA); },
        "Capture must not reach a source object before Ready.");

    auto binding = ReadyBinding();
    Check(
        binding.BoundSourceObjectForCapture(kDualIdentityCameraAliasA) == "sdk-source-0",
        "CAM-A must resolve to the source object the operator assigned to it.");
    Check(
        binding.BoundSourceObjectForCapture(kDualIdentityCameraAliasB) == "sdk-source-1",
        "CAM-B must resolve to the source object the operator assigned to it.");

    const auto counters = binding.SafetyCounters();
    Check(
        counters.source_object_reuse_count == 2,
        "Both capture lookups must be counted as reuse of the bound object.");

    binding.Invalidate(DualIdentityInvalidationReason::AgentRestart);
    CheckBindingError(
        "BindingNotReady",
        [&] { (void)binding.BoundSourceObjectForCapture(kDualIdentityCameraAliasA); },
        "An invalidated binding must refuse the capture seam until re-binding.");
}

void PublishedEvidenceIsLimitedToTheFiveAllowedFields() {
    auto binding = ReadyBinding();
    const auto evidence = binding.PublishableEvidence();
    Check(evidence.size() == 2, "Both assigned aliases must be published.");

    for (const auto& item : evidence) {
        Check(
            item.camera_alias == kDualIdentityCameraAliasA ||
                item.camera_alias == kDualIdentityCameraAliasB,
            "Only CAM-A and CAM-B may appear as aliases.");
        Check(
            item.provider_id == kDualIdentitySessionBindingProviderId,
            "The provider id must identify the session-binding provider.");
        Check(
            item.provider_version == kDualIdentitySessionBindingProviderVersion,
            "The provider version must be published.");
        Check(
            item.confirmed_at_utc == "2026-08-21T00:00:00Z",
            "confirmedAt must be the timestamp the binding was completed with.");
        Check(
            item.invalidation_reason.empty(),
            "A Ready binding has no invalidation reason.");

        // The whole point of ADR-0025: the source object and the candidate
        // ordinal must not be reachable through published evidence. If a field
        // is ever added that carries them, this scan is what should fail.
        Check(
            item.camera_alias.find("sdk-source") == std::string::npos &&
                item.provider_id.find("sdk-source") == std::string::npos &&
                item.confirmed_at_utc.find("sdk-source") == std::string::npos,
            "No source object token may leak into published evidence.");
    }
}

void BeginningAgainDiscardsThePreviousSession() {
    DualIdentitySessionBinding binding;
    binding.BeginBinding(TwoCandidates());
    binding.ConfirmAlias(0, kDualIdentityCameraAliasA);

    binding.BeginBinding(
        {DualIdentityCandidate{5, "sdk-source-5"}, DualIdentityCandidate{6, "sdk-source-6"}});
    Check(
        binding.State() == DualIdentitySessionBindingState::CollectingCandidates,
        "A new session must start clean.");
    Check(
        binding.PublishableEvidence().empty(),
        "A half-finished assignment must not survive into a new session.");

    // The old ordinals must be gone, not merged with the new ones.
    CheckBindingError(
        "UnknownCandidateOrdinal",
        [&] { binding.ConfirmAlias(0, kDualIdentityCameraAliasA); },
        "Ordinals from the discarded session must not resolve.");
}

// A failed BeginBinding must not leave the previous binding usable. This is the
// case that matters in the field: a third body gets plugged in, the re-bind is
// refused, and capture must not keep using source objects from before that.
void RefusedRebindDoesNotLeaveTheOldBindingUsable() {
    auto binding = ReadyBinding();
    Check(binding.IsReady(), "Precondition: the binding starts Ready.");

    CheckBindingError(
        "CandidateCountNotTwo",
        [&] {
            binding.BeginBinding(
                {DualIdentityCandidate{0, "sdk-source-0"},
                 DualIdentityCandidate{1, "sdk-source-1"},
                 DualIdentityCandidate{2, "sdk-source-2"}});
        },
        "A three-candidate re-bind must be refused.");

    Check(
        binding.State() == DualIdentitySessionBindingState::None,
        "A refused re-bind must discard the previous session, not keep it Ready.");
    Check(!binding.IsReady(), "A refused re-bind must not leave the binding Ready.");
    CheckBindingError(
        "BindingNotReady",
        [&] { (void)binding.BoundSourceObjectForCapture(kDualIdentityCameraAliasA); },
        "Capture must not keep using source objects from before a refused re-bind.");
    Check(
        binding.PublishableEvidence().empty(),
        "A discarded session must publish nothing.");
}

// Two candidates naming one source object is one body offered twice. Accepting
// it binds both aliases to the same camera and still completes.
void TwoCandidatesSharingOneSourceObjectAreRefused() {
    DualIdentitySessionBinding binding;
    CheckBindingError(
        "DuplicateCandidateSourceObject",
        [&] {
            binding.BeginBinding(
                {DualIdentityCandidate{0, "sdk-source-same"},
                 DualIdentityCandidate{1, "sdk-source-same"}});
        },
        "Candidates sharing one SDK source object must be refused.");
    Check(
        binding.State() == DualIdentitySessionBindingState::None,
        "A refused candidate set must not open a session.");
}

// Quiesce may only be reported for a candidate the operator has finished with.
// Otherwise a flag recorded before assignment can later satisfy CompleteBinding
// while that Live View is open again.
void QuiesceRequiresTheCandidateToBeAssignedFirst() {
    DualIdentitySessionBinding binding;
    binding.BeginBinding(TwoCandidates());

    CheckBindingError(
        "CandidateNotAssigned",
        [&] { binding.ConfirmCandidateQuiesced(0, true, true); },
        "Quiesce must not be recorded before the candidate has an alias.");

    binding.ConfirmAlias(0, kDualIdentityCameraAliasA);
    binding.ConfirmCandidateQuiesced(0, true, true);
    binding.ConfirmAlias(1, kDualIdentityCameraAliasB);
    CheckBindingError(
        "CandidateNotQuiesced",
        [&] { binding.CompleteBinding("2026-08-21T00:00:00Z"); },
        "The second candidate still has no quiesce confirmation.");

    binding.ConfirmCandidateQuiesced(1, true, true);
    binding.CompleteBinding("2026-08-21T00:00:00Z");
    Check(binding.IsReady(), "The per-candidate operator flow must still reach Ready.");
}

// The confirmation UI (#62) shows these codes. "Assignment incomplete" would be
// actively wrong for a binding a USB reconnect invalidated.
void CompleteBindingReportsTheActualReasonPerState() {
    DualIdentitySessionBinding not_started;
    CheckBindingError(
        "BindingNotStarted",
        [&] { not_started.CompleteBinding("2026-08-21T00:00:00Z"); },
        "Completing with no session must say so.");

    auto invalidated = ReadyBinding();
    invalidated.Invalidate(DualIdentityInvalidationReason::UsbReconnect);
    CheckBindingError(
        "BindingInvalidated",
        [&] { invalidated.CompleteBinding("2026-08-21T00:00:00Z"); },
        "An invalidated binding must not be reported as an unfinished assignment.");

    auto already = ReadyBinding();
    CheckBindingError(
        "BindingAlreadyComplete",
        [&] { already.CompleteBinding("2026-08-21T00:00:00Z"); },
        "Completing twice must say the session is already complete.");
}

// A half-assigned session must publish nothing: its records would otherwise look
// exactly like a confirmed binding apart from an empty confirmedAt.
void UnfinishedSessionsPublishNoEvidence() {
    DualIdentitySessionBinding binding;
    binding.BeginBinding(TwoCandidates());
    Check(
        binding.PublishableEvidence().empty(),
        "A session with no assignment must publish nothing.");

    binding.ConfirmAlias(0, kDualIdentityCameraAliasA);
    Check(
        binding.PublishableEvidence().empty(),
        "A half-assigned session must publish nothing.");

    binding.ConfirmCandidateQuiesced(0, true, true);
    binding.ConfirmAlias(1, kDualIdentityCameraAliasB);
    binding.ConfirmCandidateQuiesced(1, true, true);
    Check(
        binding.PublishableEvidence().empty(),
        "An assigned-but-not-completed session must still publish nothing.");

    binding.CompleteBinding("2026-08-21T00:00:00Z");
    Check(
        binding.PublishableEvidence().size() == 2,
        "Only a completed binding publishes evidence.");
}

// The bound token must survive a rebind of the object it came from. Returning a
// reference into the candidate vector would leave the caller holding freed
// memory once BeginBinding clears it.
void BoundSourceObjectSurvivesARebind() {
    auto binding = ReadyBinding();
    const auto captured = binding.BoundSourceObjectForCapture(kDualIdentityCameraAliasA);
    binding.Invalidate(DualIdentityInvalidationReason::UsbReconnect);
    binding.BeginBinding(
        {DualIdentityCandidate{9, "sdk-source-9"}, DualIdentityCandidate{10, "sdk-source-10"}});

    Check(
        captured == "sdk-source-0",
        "A token handed to capture must stay valid after the session it came from is gone.");
}

void WpdAliasProofRequiresExactlyOneObject() {
    Check(
        ProveExactlyOneWpdAliasObject(kDualIdentityCameraAliasA, {}).outcome ==
            WpdAliasProofOutcome::Missing,
        "No observed object must report Missing.");

    const auto multiple = ProveExactlyOneWpdAliasObject(
        kDualIdentityCameraAliasA,
        {WpdAliasObjectObservation{"object-1", "CAM-A"},
         WpdAliasObjectObservation{"object-2", "CAM-A"}});
    Check(
        multiple.outcome == WpdAliasProofOutcome::Multiple,
        "Two observed objects must report Multiple rather than picking one.");
    Check(
        multiple.object_id.empty(),
        "A failed proof must not hand back an object id a caller could proceed on.");
    Check(multiple.observed_count == 2, "The observed count must be reported.");

    const auto mismatch = ProveExactlyOneWpdAliasObject(
        kDualIdentityCameraAliasA, {WpdAliasObjectObservation{"object-1", "CAM-B"}});
    Check(
        mismatch.outcome == WpdAliasProofOutcome::Mismatch,
        "An object attributed to the other alias must report Mismatch, not be accepted.");
    Check(
        mismatch.object_id.empty(),
        "A mismatched proof must not hand back the other alias's object.");

    const auto missing_id = ProveExactlyOneWpdAliasObject(
        kDualIdentityCameraAliasA, {WpdAliasObjectObservation{"", "CAM-A"}});
    Check(
        missing_id.outcome == WpdAliasProofOutcome::Mismatch,
        "A single object without an id must not be treated as a successful recovery.");

    const auto unattributed = ProveExactlyOneWpdAliasObject(
        kDualIdentityCameraAliasA, {WpdAliasObjectObservation{"object-1", ""}});
    Check(
        unattributed.outcome == WpdAliasProofOutcome::Mismatch,
        "An object nobody attributed must not be adopted as this alias's original.");

    const auto no_alias = ProveExactlyOneWpdAliasObject(
        "", {WpdAliasObjectObservation{"object-1", ""}});
    Check(
        no_alias.outcome == WpdAliasProofOutcome::Mismatch,
        "An empty alias must not match an unattributed object and succeed.");
    Check(
        no_alias.object_id.empty(),
        "A proof with no target alias must not hand back an object id.");

    const auto exactly_one = ProveExactlyOneWpdAliasObject(
        kDualIdentityCameraAliasA, {WpdAliasObjectObservation{"object-1", "CAM-A"}});
    Check(exactly_one.Succeeded(), "Exactly one attributed object must succeed.");
    Check(
        exactly_one.object_id == "object-1",
        "The recovered object id must be returned on success.");
}

void WpdAliasProofNeverWidensItsSearch() {
    // The caller scopes the observation set to one alias. Handing this function
    // the other alias's object must not produce a success: that is precisely the
    // cross-attribution ADR-0025 forbids.
    const auto result = ProveExactlyOneWpdAliasObject(
        kDualIdentityCameraAliasB, {WpdAliasObjectObservation{"object-1", "CAM-A"}});
    Check(
        result.outcome == WpdAliasProofOutcome::Mismatch,
        "CAM-B must not recover CAM-A's object even when it is the only one present.");
    Check(
        !result.failure_detail.empty(),
        "A failed proof must explain itself so the operator sees why the transaction stopped.");
}

} // namespace

int main() {
    CandidateCountMustBeExactlyTwo();
    DuplicateCandidateAndAliasAssignmentsAreRefused();
    IncompleteAliasAssignmentCannotComplete();
    LiveViewStopAndSdkCloseAreRequiredBeforeReady();
    CompletedBindingBecomesReady();
    EveryInvalidationTriggerLeavesReady();
    FirstInvalidationReasonIsKept();
    CaptureSeamRequiresReadyAndNeverReEnumerates();
    PublishedEvidenceIsLimitedToTheFiveAllowedFields();
    BeginningAgainDiscardsThePreviousSession();
    RefusedRebindDoesNotLeaveTheOldBindingUsable();
    TwoCandidatesSharingOneSourceObjectAreRefused();
    QuiesceRequiresTheCandidateToBeAssignedFirst();
    CompleteBindingReportsTheActualReasonPerState();
    UnfinishedSessionsPublishNoEvidence();
    BoundSourceObjectSurvivesARebind();
    WpdAliasProofRequiresExactlyOneObject();
    WpdAliasProofNeverWidensItsSearch();

    if (failures != 0) {
        std::cerr << "dual identity session binding tests failed: " << failures << '\n';
        return 1;
    }

    std::cout << "dual identity session binding tests passed\n";
    return 0;
}
