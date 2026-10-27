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

// Uncalibrated still similarity (#365). Same oriented A|B inputs at any size
// within the render limits. prior_x/prior_y is the expected B-to-A translation
// in these pixels (the live C geometry scaled to the still). The overlap band
// is cut into a fixed grid of tiles; each A tile is found in B by the same
// gradient correlation with a pyramid search around the prior, then kept only
// when every threshold-free check passes: a strict best, no window or range
// edge, the reverse B-to-A search returning the same integer position, and a
// structure tensor with two non-zero eigenvalues (two-dimensional texture).
// Among kept tiles, the least median of squares over every tile pair picks
// the consensus; tiles within a robust spread of it (spread re-estimated
// after each refit until the set repeats) fit a similarity about
// B's image center by least squares, in the SimilarityReviewGeometry
// convention (x/y: B center relative to A, positive rotation clockwise on
// screen, scale as a factor). Besides the prior, the best candidates of the
// translation search above on a reduced copy are tried as starts, since a
// wrong start can still give a few consistent random tiles. The attempt with
// the most agreed tiles, then the smaller residual, wins; an exact tie between
// different fits is undetermined. No random sampling; equal inputs give equal
// results. Rotation and scale limits, residual warnings and quality are
// decided by the caller.
enum class SimilarityUndeterminedReason {none,no_tiles,insufficient_tiles,outside_image,tied_starts};
constexpr std::string_view SimilarityUndeterminedReasonCode(SimilarityUndeterminedReason v){
    switch(v){
    case SimilarityUndeterminedReason::none:return "none";
    case SimilarityUndeterminedReason::no_tiles:return "no-tiles";
    case SimilarityUndeterminedReason::insufficient_tiles:return "insufficient-tiles";
    case SimilarityUndeterminedReason::outside_image:return "outside-image";
    case SimilarityUndeterminedReason::tied_starts:return "tied-starts";
    }
    return {};
}
struct SimilarityFit {double x{},y{},rotation_degrees{},scale{1};};
struct FitResidual {double rms{},maximum{};};
// a: tile center in A, b: the matched point in B (pixel-corner coordinates).
struct SimilarityTile {double a_x{},a_y{},b_x{},b_y{};unsigned row{},column{};bool matched{},agreed{};};
struct OverlapSimilarityEstimate {
    OverlapEstimateStatus status{OverlapEstimateStatus::undetermined};
    SimilarityUndeterminedReason reason{SimilarityUndeterminedReason::no_tiles};
    bool wide_search{};                // the chosen start came from the wide search
    unsigned levels{},tile_side{},search_radius{},matched{},agreed{};
    std::vector<SimilarityTile> tiles; // every searched tile, row-major
    // matched: passed the per-tile checks; agreed: inside the consensus. The
    // fits below use the agreed tiles and are valid only when status is
    // estimated_not_judged. The translation-only fit is the comparison baseline.
    SimilarityFit similarity;
    double translation_x{},translation_y{};
    FitResidual similarity_residual,translation_residual;
};
// Throws std::invalid_argument for differing sizes, broken buffers, sizes
// outside the render limits or a prior outside the frame.
[[nodiscard]] OverlapSimilarityEstimate EstimateOverlapSimilarity(const render::BgrImage& a,const render::BgrImage& b,int prior_x,int prior_y);
}
