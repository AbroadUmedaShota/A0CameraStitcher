#pragma once
#include "a0/m2/render.hpp"
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace a0::m2::experimental {
// Uncalibrated A|B translation estimator v2 (#364). Camera, file and clock
// free; deterministic. Inputs are the oriented frames (#361), A left, B right,
// equal size. Convention: A(x,y) shows the same point as B(x-dx,y-dy) with
// dx=width-overlap_pixels, dy=vertical_pixels. Rotation, scale, photometric
// correction and any optical acceptance are outside this estimator.
enum class OverlapEstimateStatus {estimated_not_judged,undetermined};
enum class OverlapUndeterminedReason {none,no_candidate,tied_candidates,search_boundary,refinement_boundary,reverse_mismatch};
constexpr std::string_view OverlapEstimateStatusCode(OverlapEstimateStatus v){
    return v==OverlapEstimateStatus::estimated_not_judged?"estimated-not-judged":"undetermined";
}
constexpr std::string_view OverlapUndeterminedReasonCode(OverlapUndeterminedReason v){
    switch(v){
    case OverlapUndeterminedReason::none:return "none";
    case OverlapUndeterminedReason::no_candidate:return "no-candidate";
    case OverlapUndeterminedReason::tied_candidates:return "tied-candidates";
    case OverlapUndeterminedReason::search_boundary:return "search-boundary";
    case OverlapUndeterminedReason::refinement_boundary:return "refinement-boundary";
    case OverlapUndeterminedReason::reverse_mismatch:return "reverse-mismatch";
    }
    return {};
}
// correlation: zero-mean normalized correlation of gradient magnitude over the
// shared rectangle. evidence: atanh(correlation)*sqrt(samples-3), the ranking
// key, so a small window's chance correlation does not outrank a large one.
// Both are diagnostics, never acceptance thresholds.
struct OverlapCandidate {unsigned overlap_pixels{};int vertical_pixels{};double correlation{},evidence{};std::uint64_t samples{};};
struct OverlapEstimate {
    OverlapEstimateStatus status{OverlapEstimateStatus::undetermined};
    OverlapUndeterminedReason reason{OverlapUndeterminedReason::no_candidate};
    std::vector<OverlapCandidate> candidates; // distinct full-resolution peaks, best first
    std::optional<OverlapCandidate> reverse;  // best of the swapped, 180-degree turned search
    unsigned levels{};                        // pyramid levels searched, 1 = full resolution only
    // Applicable position only when status is estimated_not_judged.
    const OverlapCandidate* Applicable() const {return status==OverlapEstimateStatus::estimated_not_judged?&candidates.front():nullptr;}
};
// Search range equals the manual C range: overlap 10..90% of the width and
// |vertical| <= min(100, height/4). Accepts 8..640 pixels per side; throws
// std::invalid_argument for other sizes, differing sizes or broken buffers.
// Without approved numeric thresholds, a position is returned only when every
// threshold-free check passes: a strict best, no search-range or refinement
// edge, and the swapped turned search returning the same position.
[[nodiscard]] OverlapEstimate EstimateOverlapTranslation(const render::BgrImage& a,const render::BgrImage& b);
}
