#pragma once
#include "a0/m2/experimental/fixed_composite_condition.hpp"
#include "a0/m2/experimental/overlap_estimation.hpp"
#include "a0/m2/experimental/still_review.hpp"
#include <cmath>
#include <cstdint>
#include <string_view>

namespace a0::m2::experimental {
// Per-capture scale correction of a fixed composite (#400, ADR-0037 decision 6,
// owner decisions of 2026-11-04, #396: rotation is tied to the fixed value and only
// the scale changes per capture; the center of the correction is B's image
// center moved into A by the fixed mapping (the later answer of the same day,
// replacing the seam-column point of B7; a column of the output is fixed by the
// output window, so the seam does not move whatever B is scaled about); when the
// scale cannot be estimated,
// nothing is corrected and the fixed mapping is composed as it is, leaving the
// judgment to the gate;
// owner answer B7 of #396). Camera, file and clock free; deterministic.
//
// Model. The fixed condition places B by the mapping H (B to A). A capture may
// differ from that by one thing only: B appears larger or smaller than the
// condition says, because the focus was set again or the camera height
// differs. That is written as one factor k applied to B's placement about a
// point P of A's frame:  x_A = P + k (H(b) - P).
//   - P is B's image center, H(w/2, h/2). B scales about its own center, as a
//     focus or height change does, so a pure scale leaves no shift behind.
//   - Rotation and the translation of H stay fixed. Only the translation that
//     scaling about P implies (zero at P) changes. The seam column is a column
//     of the output window, whose left edge is kept, so it does not move.
// Estimation. The similarity estimator of #365 already matches tiles of A in B
// (EstimateOverlapSimilarity). Its own fit frees rotation and translation, so
// its scale cannot be used as is: a rotation of a few hundredths of a degree
// would leak into it. Instead the agreed tiles it returns are reused and k is
// refitted with the rotation tied to H. The translation is free inside the
// estimate only, t = k q + d with q = H(b) - P and t = a - P:
//   k = sum((q - qm) . (t - tm)) / sum(|q - qm|^2),  d = tm - k qm,
// so a shift that is not a scale (shake, one pattern period) does not leak into
// k when the tiles sit on one side of P. Only k is applied; d is recorded as
// the shift the scale could not explain. The center P is decided in one place,
// FixedScaleCenter: the center of B's image moved into the output by the
// fixed mapping (owner's decision, #396). The estimator's
// own similarity fit stays a diagnostic. A tile is used when it is in the
// estimator's consensus and its match lies within the estimator's search
// window of where H puts it; a match a whole pattern period away (a repeated
// pattern) cannot be explained by a scale of a few percent and is left out.
// At least three tiles over two rows are needed, the estimator's own minimum.
// No threshold of this module is an approved value: the only number that
// decides is the limit of the condition record (>= limit is inside).

// P: B's image center, (w/2, h/2) in B's pixels (the "image-center" anchor of the
// mapping), moved by the fixed mapping into A's pixels. Owner decision of
// 2026-11-04, #396: it replaces the seam-column point of the earlier B7 answer.
// Every use of the center (the fit, the scaled geometry, the report's `center`
// and its check) goes through this function.
[[nodiscard]] render::Point FixedScaleCenter(int x,int y,double rotation_degrees,double scale,unsigned width,unsigned height);
[[nodiscard]] inline render::Point FixedScaleCenter(const FixedCompositeCondition& c){
    return FixedScaleCenter(c.mapping.translation_x,c.mapping.translation_y,c.mapping.rotation_degrees,c.mapping.scale,c.source_width,c.source_height);
}

// The render parameters of the fixed mapping with k applied about
// FixedScaleCenter. k == 1 returns the fixed mapping unchanged. Otherwise the
// crop is derived again for the scaled B (the covered rectangle, as
// SimilarityReviewGeometry does for an integer placement), keeping the left
// edge of the output window, so seam_column keeps its meaning. Throws
// std::invalid_argument for an invalid mapping, a non-positive or non-finite k,
// or when the scaled B leaves no covered window.
[[nodiscard]] render::PairRenderParameters FixedCompositeScaledGeometry(int x,int y,double rotation_degrees,double scale,
    unsigned width,unsigned height,double applied_scale);
[[nodiscard]] inline render::PairRenderParameters FixedCompositeScaledGeometry(const FixedCompositeCondition& c,double applied_scale){
    ValidateFixedCompositeCondition(c);
    return FixedCompositeScaledGeometry(c.mapping.translation_x,c.mapping.translation_y,c.mapping.rotation_degrees,c.mapping.scale,
        c.source_width,c.source_height,applied_scale);
}
// What a saved report rendered: the fixed mapping of a v6 report with its
// applied scale, the report's own placement otherwise.
[[nodiscard]] inline render::PairRenderParameters SavedStillReviewRenderParameters(const SavedStillReview& r){
    // A v7 report has no similarity to rebuild: its mapping is the profile's (#405 W4).
    if(r.fixed_composite_lens)throw std::invalid_argument("a lens-aware (v7) report has no similarity placement");
    // Nor a registered (v8) one (#417).
    if(r.fixed_composite_registration)throw std::invalid_argument("a registered (v8) report has no similarity placement");
    if(r.fixed_composite)
        return FixedCompositeScaledGeometry(r.translation_x,r.translation_y,r.rotation_degrees,r.scale,r.source_width,r.source_height,
            r.fixed_composite->scale.applied_scale);
    return SimilarityReviewGeometry(r.translation_x,r.translation_y,r.rotation_degrees,r.scale,r.source_width,r.source_height);
}

// Fits k from an estimate and decides against the condition's limit. Pure.
// status is applied or exceeded when a fit exists, not_estimated otherwise;
// seam_outside_overlap is set by the caller, which alone can render.
[[nodiscard]] FixedScaleCorrection DecideFixedScale(const FixedCompositeCondition& condition,const OverlapSimilarityEstimate& estimate);
[[nodiscard]] FixedSimilarityDiagnostic MakeFixedSimilarityDiagnostic(const OverlapSimilarityEstimate& estimate);

// Compares the condition's limit with the merge policy file's (#400). The
// condition's value is always the one that decides; this only classifies.
// policy_percent: the policy's scaleAutoCorrectionMaxPercent when it has one,
// else pass the reason it is unavailable as `unavailable`.
[[nodiscard]] inline FixedPolicyLimit ComparePolicyLimit(double condition_percent,const std::optional<double>& policy_percent,FixedPolicyCheck unavailable){
    if(!policy_percent)return {unavailable,std::nullopt};
    return {*policy_percent==condition_percent?FixedPolicyCheck::same:FixedPolicyCheck::differs,policy_percent};
}
// UTF-8 Japanese notice for the report and the screen; empty for same.
[[nodiscard]] std::string_view FixedPolicyNotice(FixedPolicyCheck check);

// Notice recorded in the report and shown on screen (UTF-8, Japanese). Empty
// when the correction was applied.
[[nodiscard]] std::string_view FixedScaleNotice(FixedScaleStatus status);
// Operator text for a capture that could not be composed. UTF-8, Japanese.
[[nodiscard]] std::string_view FixedCompositeSkipText(FixedCompositeSkip skip);
}
