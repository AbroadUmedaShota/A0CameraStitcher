#pragma once
#include "a0/m2/image_patch_correction.hpp"

namespace a0::m2::correction {
inline constexpr std::uint32_t kMaximumSeedGridSide = 64;
// Explicit development policy, not approved optical/texture thresholds.
struct PatchSeedSelectionPolicy {
    std::uint32_t grid_columns, grid_rows, maximum_seeds;
    double minimum_spacing_mm, minimum_minor_gradient_eigenvalue, minimum_gradient_axis_ratio;
    PatchSeedSelectionPolicy(std::uint32_t columns, std::uint32_t rows, std::uint32_t maximum,
        double spacing, double minor, double ratio)
        : grid_columns(columns), grid_rows(rows), maximum_seeds(maximum), minimum_spacing_mm(spacing),
          minimum_minor_gradient_eigenvalue(minor), minimum_gradient_axis_ratio(ratio) {}
};
enum class PatchSeedOutcome {
    Selected, CoverageAUnavailable, CoverageBUnavailable, SourcePatchUnavailable,
    LowContrast, OneDimensionalTexture, WeakTexture, DuplicatePosition,
    NumericalFailure, SpacingExcluded, SelectionLimit
};
constexpr std::string_view PatchSeedOutcomeCode(PatchSeedOutcome value) {
    switch (value) {
    case PatchSeedOutcome::Selected: return "seed-selected";
    case PatchSeedOutcome::CoverageAUnavailable: return "seed-coverage-a-unavailable";
    case PatchSeedOutcome::CoverageBUnavailable: return "seed-coverage-b-unavailable";
    case PatchSeedOutcome::SourcePatchUnavailable: return "seed-source-patch-unavailable";
    case PatchSeedOutcome::LowContrast: return "seed-low-contrast";
    case PatchSeedOutcome::OneDimensionalTexture: return "seed-one-dimensional-texture";
    case PatchSeedOutcome::WeakTexture: return "seed-weak-texture";
    case PatchSeedOutcome::DuplicatePosition: return "seed-duplicate-position";
    case PatchSeedOutcome::NumericalFailure: return "seed-numerical-failure";
    case PatchSeedOutcome::SpacingExcluded: return "seed-spacing-excluded";
    case PatchSeedOutcome::SelectionLimit: return "seed-selection-limit";
    }
    return {};
}
struct PatchSeedTexture {
    double intensity_stddev, minor_gradient_eigenvalue, major_gradient_eigenvalue, gradient_axis_ratio;
};
struct PatchSeedCandidate {
    std::uint32_t column, row;
    render::Point document_mm;
    PatchSeedOutcome outcome;
    std::optional<PatchSeedTexture> texture;
};
enum class PatchSeedSelectionOutcome { Selected, InsufficientSeeds, GeometryRejected };
struct PatchSeedSelectionResult {
    PatchSeedSelectionOutcome outcome;
    DocumentCorrectionOutcome geometry_outcome;
    std::vector<PatchSeedCandidate> candidates; // row-major, every candidate retained
    std::vector<std::size_t> selected_indices; // descending texture score, stable index tie-break
};
// Grid centers on the full ceil raster. Uses A content and BOTH fixed image
// extents; B content, correlation and fit results never influence selection.
// Conservative continuous support enclosure; can reject valid boundary areas.
// Spacing and geometry are checked BEFORE matching. No usable warp is returned.
[[nodiscard]] PatchSeedSelectionResult SelectImagePatchSeeds(
    const render::BgrImage& a, const render::BgrImage& b,
    const render::CameraProjection& camera_a, const render::CameraProjection& camera_b,
    const render::OutputRaster& raster, const PatchSeedSelectionPolicy& selection_policy,
    const ImagePatchPolicy& matching_policy, const DocumentCorrectionPolicy& correction_policy);

enum class AutoImagePatchOutcome { Estimated, SelectionRejected, MatchingRejected };
struct AutoImagePatchCorrectionResult {
    AutoImagePatchOutcome outcome;
    PatchSeedSelectionResult selection;
    std::optional<ImagePatchCorrectionResult> correction;
};
// Selection is frozen before the ONE matching pass. No retry/reselection after
// any selected match fails. Existing image/raw/geometry gates remain mandatory.
[[nodiscard]] AutoImagePatchCorrectionResult EstimateAutoImagePatchCorrection(
    const render::BgrImage& a, const render::BgrImage& b,
    const render::CameraProjection& camera_a, const render::CameraProjection& camera_b,
    const render::OutputRaster& raster, const PatchSeedSelectionPolicy& selection_policy,
    const ImagePatchPolicy& matching_policy, const RawInversePolicy& inverse_policy,
    const DocumentCorrectionPolicy& correction_policy);
} // namespace a0::m2::correction
