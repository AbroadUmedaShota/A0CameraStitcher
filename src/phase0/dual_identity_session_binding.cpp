#include "a0/phase0/dual_identity_session_binding.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace a0::phase0 {
namespace {

[[nodiscard]] bool IsKnownAlias(std::string_view alias) noexcept {
    return alias == kDualIdentityCameraAliasA || alias == kDualIdentityCameraAliasB;
}

} // namespace

DualIdentitySessionBindingError::DualIdentitySessionBindingError(
    std::string code,
    std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

const std::string& DualIdentitySessionBindingError::Code() const noexcept {
    return code_;
}

std::string_view DualIdentityInvalidationReasonName(
    DualIdentityInvalidationReason reason) noexcept {
    switch (reason) {
        case DualIdentityInvalidationReason::None:
            return "";
        case DualIdentityInvalidationReason::AgentRestart:
            return "AgentRestart";
        case DualIdentityInvalidationReason::UsbReconnect:
            return "UsbReconnect";
        case DualIdentityInvalidationReason::CameraCountChanged:
            return "CameraCountChanged";
        case DualIdentityInvalidationReason::TopologyChanged:
            return "TopologyChanged";
        case DualIdentityInvalidationReason::SdkManagerRecreated:
            return "SdkManagerRecreated";
        case DualIdentityInvalidationReason::SdkError:
            return "SdkError";
    }
    return "";
}

DualIdentitySessionBindingState DualIdentitySessionBinding::State() const noexcept {
    return state_;
}

void DualIdentitySessionBinding::BeginBinding(
    const std::vector<DualIdentityCandidate>& candidates) {
    if (candidates.size() != kDualIdentityRequiredCandidateCount) {
        // Refused before anything is stored. Three connected bodies is not a
        // situation where the app may quietly pick two.
        throw DualIdentitySessionBindingError(
            "CandidateCountNotTwo",
            "A binding session requires exactly two SDK candidates.");
    }

    if (candidates[0].ordinal == candidates[1].ordinal) {
        throw DualIdentitySessionBindingError(
            "DuplicateCandidateOrdinal",
            "Candidate ordinals must be distinct within a binding session.");
    }

    for (const auto& candidate : candidates) {
        if (candidate.source_object_token.empty()) {
            throw DualIdentitySessionBindingError(
                "CandidateSourceObjectMissing",
                "Every candidate must carry a source object token.");
        }
    }

    // Any partially assigned previous session is discarded rather than merged.
    candidates_.clear();
    confirmed_at_utc_.clear();
    invalidation_reason_ = DualIdentityInvalidationReason::None;
    for (const auto& candidate : candidates) {
        candidates_.push_back(
            Assignment{candidate.ordinal, candidate.source_object_token, {}, false, false});
    }

    state_ = DualIdentitySessionBindingState::CollectingCandidates;
}

void DualIdentitySessionBinding::ConfirmAlias(
    std::size_t candidate_ordinal,
    std::string_view camera_alias) {
    if (state_ != DualIdentitySessionBindingState::CollectingCandidates &&
        state_ != DualIdentitySessionBindingState::AwaitingQuiesce) {
        throw DualIdentitySessionBindingError(
            "BindingNotCollecting",
            "Alias confirmation requires an active binding session.");
    }

    if (!IsKnownAlias(camera_alias)) {
        throw DualIdentitySessionBindingError(
            "UnknownCameraAlias",
            "Only CAM-A and CAM-B may be assigned.");
    }

    auto* candidate = FindByOrdinal(candidate_ordinal);
    if (candidate == nullptr) {
        throw DualIdentitySessionBindingError(
            "UnknownCandidateOrdinal",
            "The candidate ordinal does not belong to this binding session.");
    }

    if (!candidate->camera_alias.empty()) {
        // The operator already answered for this body. A second answer means
        // one of the two answers is wrong, and we cannot tell which.
        throw DualIdentitySessionBindingError(
            "CandidateAlreadyAssigned",
            "Each candidate may be assigned exactly once.");
    }

    if (FindByAlias(camera_alias) != nullptr) {
        throw DualIdentitySessionBindingError(
            "AliasAlreadyAssigned",
            "Each alias may be assigned exactly once.");
    }

    candidate->camera_alias = std::string(camera_alias);

    const bool every_alias_assigned = std::all_of(
        candidates_.begin(),
        candidates_.end(),
        [](const Assignment& assignment) { return !assignment.camera_alias.empty(); });
    state_ = every_alias_assigned
        ? DualIdentitySessionBindingState::AwaitingQuiesce
        : DualIdentitySessionBindingState::CollectingCandidates;
}

void DualIdentitySessionBinding::ConfirmCandidateQuiesced(
    std::size_t candidate_ordinal,
    bool live_view_stopped,
    bool sdk_session_closed) {
    if (state_ != DualIdentitySessionBindingState::CollectingCandidates &&
        state_ != DualIdentitySessionBindingState::AwaitingQuiesce) {
        throw DualIdentitySessionBindingError(
            "BindingNotCollecting",
            "Quiesce confirmation requires an active binding session.");
    }

    auto* candidate = FindByOrdinal(candidate_ordinal);
    if (candidate == nullptr) {
        throw DualIdentitySessionBindingError(
            "UnknownCandidateOrdinal",
            "The candidate ordinal does not belong to this binding session.");
    }

    // Recorded as observed, including "not stopped". CompleteBinding is what
    // refuses; this call must not quietly upgrade a failed stop into a success.
    candidate->live_view_stopped = live_view_stopped;
    candidate->sdk_session_closed = sdk_session_closed;
}

void DualIdentitySessionBinding::CompleteBinding(std::string_view confirmed_at_utc) {
    if (state_ != DualIdentitySessionBindingState::AwaitingQuiesce) {
        throw DualIdentitySessionBindingError(
            "AliasAssignmentIncomplete",
            "Both CAM-A and CAM-B must be assigned before completing a binding.");
    }

    if (!EveryCandidateQuiesced()) {
        // The one ordering rule that matters: no Ready binding while a Live
        // View may still be open, because capture would then race it.
        throw DualIdentitySessionBindingError(
            "CandidateNotQuiesced",
            "Every candidate must have a confirmed Live View stop and SDK session close.");
    }

    if (confirmed_at_utc.empty()) {
        throw DualIdentitySessionBindingError(
            "ConfirmedAtMissing",
            "A completed binding must carry the confirmation timestamp it publishes.");
    }

    confirmed_at_utc_ = std::string(confirmed_at_utc);
    state_ = DualIdentitySessionBindingState::Ready;
}

void DualIdentitySessionBinding::Invalidate(
    DualIdentityInvalidationReason reason) noexcept {
    if (reason == DualIdentityInvalidationReason::None) {
        return;
    }

    if (state_ == DualIdentitySessionBindingState::Invalid) {
        // Keep the first reason: it is the one that explains the loss of trust.
        return;
    }

    state_ = DualIdentitySessionBindingState::Invalid;
    invalidation_reason_ = reason;
}

bool DualIdentitySessionBinding::IsReady() const noexcept {
    return state_ == DualIdentitySessionBindingState::Ready;
}

const std::string& DualIdentitySessionBinding::BoundSourceObjectForCapture(
    std::string_view camera_alias) {
    if (state_ != DualIdentitySessionBindingState::Ready) {
        throw DualIdentitySessionBindingError(
            "BindingNotReady",
            "Capture may only use a source object from a Ready binding.");
    }

    const auto* assignment = FindByAlias(camera_alias);
    if (assignment == nullptr) {
        throw DualIdentitySessionBindingError(
            "UnknownCameraAlias",
            "The alias has no bound source object in this session.");
    }

    ++safety_counters_.source_object_reuse_count;
    return assignment->source_object_token;
}

std::vector<DualIdentitySessionBindingEvidence>
DualIdentitySessionBinding::PublishableEvidence() const {
    std::vector<DualIdentitySessionBindingEvidence> evidence;
    for (const auto& assignment : candidates_) {
        if (assignment.camera_alias.empty()) {
            continue;
        }

        evidence.push_back(DualIdentitySessionBindingEvidence{
            assignment.camera_alias,
            std::string(kDualIdentitySessionBindingProviderId),
            kDualIdentitySessionBindingProviderVersion,
            confirmed_at_utc_,
            std::string(DualIdentityInvalidationReasonName(invalidation_reason_))});
    }

    return evidence;
}

DualIdentitySessionBindingSafetyCounters
DualIdentitySessionBinding::SafetyCounters() const noexcept {
    return safety_counters_;
}

DualIdentitySessionBinding::Assignment* DualIdentitySessionBinding::FindByOrdinal(
    std::size_t ordinal) noexcept {
    const auto found = std::find_if(
        candidates_.begin(),
        candidates_.end(),
        [ordinal](const Assignment& assignment) { return assignment.ordinal == ordinal; });
    return found == candidates_.end() ? nullptr : &*found;
}

const DualIdentitySessionBinding::Assignment* DualIdentitySessionBinding::FindByAlias(
    std::string_view alias) const noexcept {
    const auto found = std::find_if(
        candidates_.begin(),
        candidates_.end(),
        [alias](const Assignment& assignment) { return assignment.camera_alias == alias; });
    return found == candidates_.end() ? nullptr : &*found;
}

bool DualIdentitySessionBinding::EveryCandidateQuiesced() const noexcept {
    return std::all_of(
        candidates_.begin(),
        candidates_.end(),
        [](const Assignment& assignment) {
            return assignment.live_view_stopped && assignment.sdk_session_closed;
        });
}

} // namespace a0::phase0
