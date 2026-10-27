#include "a0/m2/image_patch_correction.hpp"
#include "image_patch_detail.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace a0::m2::correction {
namespace {
using namespace render;
using namespace patch_detail;
using Outcome = ImagePatchOutcome;
PatchSearchResult Search(const BgrImage& source, const BgrImage& target, const CameraProjection& source_camera,
    const CameraProjection& target_camera, Point seed, const std::array<Point,4>& domain, const ImagePatchPolicy& p,
    const PatchRefinementPolicy* refinement) {
    PatchSearchResult result{Outcome::SourcePatchUnavailable,{},{},{},{},{},0,0};
    std::vector<double> a, b;
    if (!Patch(source,source_camera,seed,domain,p,a)) return result;
    const double norm_a = Center(a), contrast_scale = std::sqrt(static_cast<double>(a.size()));
    result.source_intensity_stddev = norm_a/contrast_scale;
    if (*result.source_intensity_stddev < p.minimum_intensity_stddev) { result.outcome = Outcome::SourceTextureInsufficient; return result; }
    double best = -std::numeric_limits<double>::infinity(), second = best;
    const auto radius = static_cast<int>(p.search_radius_steps);
    int best_x = 0, best_y = 0;
    for (int y = -radius; y <= radius; ++y) for (int x = -radius; x <= radius; ++x) {
        const Point candidate{seed.x+x*p.search_step_mm,seed.y+y*p.search_step_mm};
        ++result.examined_candidates;
        // No clipped search: unavailable competitors could conceal a better peak.
        if (!Patch(target,target_camera,candidate,domain,p,b)) { result.outcome = Outcome::SearchWindowUnavailable; return result; }
        const double norm_b = Center(b);
        const double target_stddev = norm_b/contrast_scale;
        result.maximum_target_intensity_stddev = std::max(result.maximum_target_intensity_stddev.value_or(0),target_stddev);
        if (target_stddev < p.minimum_intensity_stddev) continue;
        ++result.eligible_candidates;
        const double score = NormalizedDot(a,norm_a,b,norm_b);
        if (!std::isfinite(score)) { result.outcome = Outcome::NumericalFailure; return result; }
        if (score > best) {
            second = best; best = score; best_x = x; best_y = y;
            result.best_document_mm = candidate;
        } else if (score > second) second = score;
    }
    if (result.eligible_candidates == 0) { result.outcome = Outcome::TargetTextureInsufficient; return result; }
    result.best_score = best;
    if (result.eligible_candidates > 1) result.second_score = second;
    if (best < p.minimum_correlation) result.outcome = Outcome::CorrelationInsufficient;
    else if (!result.second_score || best-second < p.minimum_best_second_gap) result.outcome = Outcome::Ambiguous;
    else if (std::abs(best_x) == radius || std::abs(best_y) == radius) result.outcome = Outcome::SearchBoundary;
    else result.outcome = Outcome::Matched;
    if (!refinement || result.outcome != Outcome::Matched) return result;
    Point center = *result.best_document_mm;
    double step = p.search_step_mm;
    const Point low{seed.x-radius*step,seed.y-radius*step}, high{seed.x+radius*step,seed.y+radius*step};
    for (std::uint32_t level = 0; level < refinement->levels; ++level) {
        step /= 2;
        PatchRefinementLevel detail{step,Outcome::NumericalFailure,{},{},{},{},0,0};
        double local_best = -std::numeric_limits<double>::infinity(), local_second = local_best;
        std::set<std::pair<double,double>> positions;
        bool complete = std::isfinite(step) && step > 0;
        for (int y = -1; y <= 1 && complete; ++y) for (int x = -1; x <= 1 && complete; ++x) {
            const Point candidate{center.x+x*step,center.y+y*step};
            // Never silently collapse an unrepresentable grid or leave the
            // original coarse window. A failed level is retained diagnostically.
            if (!std::isfinite(candidate.x) || !std::isfinite(candidate.y) ||
                candidate.x <= low.x || candidate.x >= high.x || candidate.y <= low.y || candidate.y >= high.y ||
                !positions.emplace(candidate.x,candidate.y).second) { complete = false; break; }
            ++detail.examined_candidates;
            if (!Patch(target,target_camera,candidate,domain,p,b)) { detail.outcome = Outcome::SearchWindowUnavailable; complete = false; break; }
            const double norm_b = Center(b), target_stddev = norm_b/contrast_scale;
            detail.maximum_target_intensity_stddev = std::max(detail.maximum_target_intensity_stddev.value_or(0),target_stddev);
            if (target_stddev < p.minimum_intensity_stddev) continue;
            ++detail.eligible_candidates;
            const double score = NormalizedDot(a,norm_a,b,norm_b);
            if (!std::isfinite(score)) { complete = false; break; }
            if (score > local_best) { local_second = local_best; local_best = score; detail.best_document_mm = candidate; }
            else if (score > local_second) local_second = score;
        }
        if (detail.best_document_mm) detail.best_score = local_best;
        if (std::isfinite(local_second)) detail.second_score = local_second;
        if (complete) {
            if (!detail.best_score) detail.outcome = Outcome::TargetTextureInsufficient;
            else if (local_best < p.minimum_correlation) detail.outcome = Outcome::CorrelationInsufficient;
            else if (!detail.second_score || !(local_best > local_second)) detail.outcome = Outcome::Ambiguous;
            else detail.outcome = Outcome::Matched;
        }
        result.refinement.push_back(detail);
        result.outcome = detail.outcome;
        if (detail.outcome != Outcome::Matched) return result;
        center = *detail.best_document_mm;
    }
    return result;
}
Point MatchedPosition(const PatchSearchResult& result) {
    return result.refinement.empty() ? *result.best_document_mm : *result.refinement.back().best_document_mm;
}

ImagePatchCorrectionResult Estimate(const BgrImage& image_a, const BgrImage& image_b,
    const CameraProjection& camera_a, const CameraProjection& camera_b, const OutputRaster& raster,
    std::span<const Point> seeds, const ImagePatchPolicy& policy, const RawInversePolicy& inverse,
    const DocumentCorrectionPolicy& correction, const PatchRefinementPolicy* refinement) {
    Validate(image_a); Validate(image_b); Validate(policy);
    const RawImageExtent extent_a{image_a.width,image_a.height}, extent_b{image_b.width,image_b.height};
    const auto count_rejection = EstimateRawPairDocumentCorrection(camera_a,camera_b,raster,extent_a,extent_b,{},inverse,correction);
    ImagePatchCorrectionResult result{ImagePatchCorrectionOutcome::InvalidSeeds,{}, {}};
    if (seeds.size() > kMaximumPatchSeeds) return result;
    const std::uint64_t side = 2*policy.patch_radius_samples+1, search = 2*policy.search_radius_steps+1;
    const std::uint64_t candidates = search*search+(refinement ? 9*refinement->levels : 0);
    if (seeds.size()*2*side*side*candidates > kMaximumPatchSampleEvaluations) {
        result.outcome = ImagePatchCorrectionOutcome::WorkLimitExceeded; return result;
    }
    if (seeds.size() < correction.minimum_points) {
        result.outcome = ImagePatchCorrectionOutcome::CorrectionRejected; result.correction = count_rejection; return result;
    }
    const auto domain = DocumentRasterCorners(raster);
    std::set<std::pair<double,double>> unique;
    std::vector<RawPixelCorrespondence> matches;
    bool complete = true;
    for (const auto seed : seeds) {
        ImagePatchPointResult point{seed,Outcome::InvalidSeed,{Outcome::InvalidSeed,{},{},{},{},{},0,0},{},{},{}};
        if (!InDomain(seed,domain) || !unique.emplace(seed.x,seed.y).second) {
            complete = false; result.points.push_back(point); continue;
        }
        point.forward = Search(image_a,image_b,camera_a,camera_b,seed,domain,policy,refinement);
        point.outcome = point.forward.outcome;
        if (point.outcome == Outcome::Matched) {
            const auto candidate = MatchedPosition(point.forward);
            point.reverse = Search(image_b,image_a,camera_b,camera_a,candidate,domain,policy,refinement);
            point.outcome = point.reverse->outcome;
            if (point.outcome == Outcome::Matched) {
                const auto returned = MatchedPosition(*point.reverse);
                point.reverse_error_mm = std::hypot(returned.x-seed.x,returned.y-seed.y);
                if (!std::isfinite(*point.reverse_error_mm)) point.outcome = Outcome::NumericalFailure;
                else if (*point.reverse_error_mm > policy.maximum_reverse_error_mm) point.outcome = Outcome::ReverseInconsistent;
                else {
                    Point a{},b{};
                    if (!TryProjectDocument(camera_a,seed,a) || !TryProjectDocument(camera_b,candidate,b)) point.outcome = Outcome::NumericalFailure;
                    else { point.correspondence = RawPixelCorrespondence{a,b}; matches.push_back(*point.correspondence); }
                }
            }
        }
        if (!point.correspondence) complete = false;
        result.points.push_back(point);
    }
    if (!complete) { result.outcome = ImagePatchCorrectionOutcome::MatchingFailed; return result; }
    result.correction = EstimateRawPairDocumentCorrection(camera_a,camera_b,raster,extent_a,extent_b,matches,inverse,correction);
    result.outcome = result.correction->outcome == RawPairCorrectionOutcome::Estimated ?
        ImagePatchCorrectionOutcome::Estimated : ImagePatchCorrectionOutcome::CorrectionRejected;
    return result;
}
} // namespace

ImagePatchCorrectionResult EstimateImagePatchCorrection(const BgrImage& a, const BgrImage& b,
    const CameraProjection& ca, const CameraProjection& cb, const OutputRaster& raster,
    std::span<const Point> seeds, const ImagePatchPolicy& policy, const RawInversePolicy& inverse,
    const DocumentCorrectionPolicy& correction) {
    return Estimate(a,b,ca,cb,raster,seeds,policy,inverse,correction,nullptr);
}
ImagePatchCorrectionResult EstimateImagePatchCorrection(const BgrImage& a, const BgrImage& b,
    const CameraProjection& ca, const CameraProjection& cb, const OutputRaster& raster,
    std::span<const Point> seeds, const ImagePatchPolicy& policy, const PatchRefinementPolicy& refinement,
    const RawInversePolicy& inverse, const DocumentCorrectionPolicy& correction) {
    Validate(refinement);
    return Estimate(a,b,ca,cb,raster,seeds,policy,inverse,correction,&refinement);
}
} // namespace a0::m2::correction
