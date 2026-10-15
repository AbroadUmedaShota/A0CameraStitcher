#pragma once

#include "a0/m2/raw_document_correction.hpp"

namespace a0::m2::correction {
inline constexpr std::size_t kMaximumPatchSeeds = 256;
inline constexpr std::uint64_t kMaximumPatchSampleEvaluations = 16'000'000;

// Development matching policy, no defaults and no approved optical claim.
// Samples/offsets are in NOMINAL document mm, not raw or output pixels.
struct ImagePatchPolicy {
    std::uint32_t patch_radius_samples, search_radius_steps;
    double patch_spacing_mm, search_step_mm;
    double minimum_intensity_stddev, minimum_correlation, minimum_best_second_gap;
    double maximum_reverse_error_mm;
    ImagePatchPolicy(std::uint32_t radius, double spacing, std::uint32_t search, double step,
        double contrast, double correlation, double separation, double reverse_error)
        : patch_radius_samples(radius), search_radius_steps(search), patch_spacing_mm(spacing), search_step_mm(step),
          minimum_intensity_stddev(contrast), minimum_correlation(correlation), minimum_best_second_gap(separation),
          maximum_reverse_error_mm(reverse_error) {}
};
enum class ImagePatchOutcome {
    Matched, InvalidSeed, SourcePatchUnavailable, SourceTextureInsufficient,
    SearchWindowUnavailable, TargetTextureInsufficient, CorrelationInsufficient,
    Ambiguous, SearchBoundary, NumericalFailure, ReverseInconsistent
};
constexpr std::string_view ImagePatchOutcomeCode(ImagePatchOutcome v) {
    switch (v) {
    case ImagePatchOutcome::Matched: return "patch-matched";
    case ImagePatchOutcome::InvalidSeed: return "invalid-patch-seed";
    case ImagePatchOutcome::SourcePatchUnavailable: return "source-patch-unavailable";
    case ImagePatchOutcome::SourceTextureInsufficient: return "source-texture-insufficient";
    case ImagePatchOutcome::SearchWindowUnavailable: return "search-window-unavailable";
    case ImagePatchOutcome::TargetTextureInsufficient: return "target-texture-insufficient";
    case ImagePatchOutcome::CorrelationInsufficient: return "correlation-insufficient";
    case ImagePatchOutcome::Ambiguous: return "ambiguous-patch";
    case ImagePatchOutcome::SearchBoundary: return "search-boundary";
    case ImagePatchOutcome::NumericalFailure: return "patch-numerical-failure";
    case ImagePatchOutcome::ReverseInconsistent: return "reverse-inconsistent";
    }
    return {};
}
struct PatchSearchResult {
    ImagePatchOutcome outcome;
    std::optional<render::Point> best_document_mm; // diagnostic even on rejection
    std::optional<double> best_score, second_score;
    std::optional<double> source_intensity_stddev, maximum_target_intensity_stddev;
    std::size_t examined_candidates, eligible_candidates;
};
struct ImagePatchPointResult {
    render::Point seed_document_mm;
    ImagePatchOutcome outcome;
    PatchSearchResult forward;
    std::optional<PatchSearchResult> reverse;
    std::optional<double> reverse_error_mm;
    // Usable only after both searches and reverse check pass.
    std::optional<RawPixelCorrespondence> correspondence;
};
enum class ImagePatchCorrectionOutcome { Estimated, InvalidSeeds, WorkLimitExceeded, MatchingFailed, CorrectionRejected };
struct ImagePatchCorrectionResult {
    ImagePatchCorrectionOutcome outcome;
    std::vector<ImagePatchPointResult> points;
    std::optional<RawPairDocumentCorrectionResult> correction;
};
// Given explicit document seeds, searches image CONTENT near fixed prediction.
// Mean BGR intensity, bilinear sampling, zero-mean normalized correlation.
// All requested seed matches are required; no pruning/RANSAC/subpixel fitting.
// This is not a general keypoint detector or product profile/setup/job gate.
[[nodiscard]] ImagePatchCorrectionResult EstimateImagePatchCorrection(
    const render::BgrImage& image_a, const render::BgrImage& image_b,
    const render::CameraProjection& camera_a, const render::CameraProjection& camera_b,
    const render::OutputRaster& raster, std::span<const render::Point> document_seeds_mm,
    const ImagePatchPolicy& matching_policy, const RawInversePolicy& inverse_policy,
    const DocumentCorrectionPolicy& correction_policy);
} // namespace a0::m2::correction
