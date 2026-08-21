#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace a0::phase0 {

// Exactly-one WPD object recovery for a bound alias (ADR-0025, GitHub Issue #9).
//
// After the SDK fires the shutter, the resulting file is recovered through WPD.
// ADR-0025 is strict about what may happen when that recovery is not
// unambiguous: keep the PC originals already obtained, report FailedPartial, and
// do nothing else. In particular this layer must not
//
//   - look at another alias for the missing object. The whole point of the
//     session binding is that we cannot tell the two bodies apart by their
//     objects; searching the other alias would silently defeat it and could
//     attribute CAM-B's frame to CAM-A.
//   - delete anything. A wrong object is evidence about what went wrong.
//   - retry. Automatic retry count is fixed at zero across the product.
//
// So the only outcomes are "exactly one" and a typed failure. There is no
// "best match" and no "first of several".

enum class WpdAliasProofOutcome {
    ExactlyOne,
    // No object attributable to this alias. Nothing was captured, or the object
    // has not appeared yet -- either way this layer does not wait or poll.
    Missing,
    // More than one candidate object. Which one belongs to this shutter is not
    // decidable here, and guessing would mislabel a canonical original.
    Multiple,
    // Exactly one object, but it does not carry the expected alias attribution.
    Mismatch,
};

[[nodiscard]] std::string_view WpdAliasProofOutcomeName(
    WpdAliasProofOutcome outcome) noexcept;

// One object visible to WPD for the alias under test.
//
// `attributed_alias` is what the observation layer believes this object belongs
// to. It is compared, never trusted to be right: a mismatch is a reportable
// outcome, not something to correct.
struct WpdAliasObjectObservation {
    std::string object_id;
    std::string attributed_alias;
};

struct WpdAliasProofResult {
    WpdAliasProofOutcome outcome{WpdAliasProofOutcome::Missing};
    // Only populated for ExactlyOne. Deliberately empty otherwise so a caller
    // cannot accidentally proceed on a failed proof.
    std::string object_id;
    std::size_t observed_count{};
    std::string failure_detail;

    [[nodiscard]] bool Succeeded() const noexcept {
        return outcome == WpdAliasProofOutcome::ExactlyOne;
    }
};

// Proves that `observations` contains exactly one object for `camera_alias`.
//
// `observations` is the set already scoped to this alias by the caller. This
// function does not widen that scope: given an empty set it reports Missing
// rather than looking anywhere else.
[[nodiscard]] WpdAliasProofResult ProveExactlyOneWpdAliasObject(
    std::string_view camera_alias,
    const std::vector<WpdAliasObjectObservation>& observations);

} // namespace a0::phase0
