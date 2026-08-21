#include "a0/phase0/wpd_alias_proof.hpp"

#include <string>

namespace a0::phase0 {

std::string_view WpdAliasProofOutcomeName(WpdAliasProofOutcome outcome) noexcept {
    switch (outcome) {
        case WpdAliasProofOutcome::ExactlyOne:
            return "ExactlyOne";
        case WpdAliasProofOutcome::Missing:
            return "Missing";
        case WpdAliasProofOutcome::Multiple:
            return "Multiple";
        case WpdAliasProofOutcome::Mismatch:
            return "Mismatch";
    }
    return "Missing";
}

WpdAliasProofResult ProveExactlyOneWpdAliasObject(
    std::string_view camera_alias,
    const std::vector<WpdAliasObjectObservation>& observations) {
    WpdAliasProofResult result;
    result.observed_count = observations.size();

    if (observations.empty()) {
        result.outcome = WpdAliasProofOutcome::Missing;
        result.failure_detail =
            "No object was observed for " + std::string(camera_alias) +
            "; the PC originals already obtained are kept and no other alias is searched.";
        return result;
    }

    if (observations.size() > 1) {
        // Deliberately not "pick the newest". Attributing the wrong frame to an
        // alias produces a canonical original that claims to be from a body it
        // did not come from, which is worse than failing the transaction.
        result.outcome = WpdAliasProofOutcome::Multiple;
        result.failure_detail =
            "Observed " + std::to_string(observations.size()) + " objects for " +
            std::string(camera_alias) +
            "; exactly one is required and no object is deleted or retried.";
        return result;
    }

    const auto& only = observations.front();
    if (only.attributed_alias != camera_alias) {
        result.outcome = WpdAliasProofOutcome::Mismatch;
        result.failure_detail =
            "The single observed object is attributed to '" + only.attributed_alias +
            "' but " + std::string(camera_alias) + " was expected.";
        return result;
    }

    if (only.object_id.empty()) {
        result.outcome = WpdAliasProofOutcome::Mismatch;
        result.failure_detail =
            "The single observed object for " + std::string(camera_alias) +
            " carries no object id.";
        return result;
    }

    result.outcome = WpdAliasProofOutcome::ExactlyOne;
    result.object_id = only.object_id;
    return result;
}

} // namespace a0::phase0
