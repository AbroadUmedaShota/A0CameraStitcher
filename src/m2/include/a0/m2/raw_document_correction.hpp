#pragma once

#include "a0/m2/document_correction.hpp"

namespace a0::m2::correction {

// Stored JPEG order, before any EXIF/display rotation. Same corner-coordinate
// model as the renderer: sample (i,j) has center (i+.5,j+.5).
// No images are read here; matching/detector adapters must supply this model.
struct RawImageExtent {
    std::uint32_t width, height;
    RawImageExtent(std::uint32_t w, std::uint32_t h) : width(w), height(h) {}
};
struct RawInversePolicy {
    std::uint32_t maximum_newton_updates, maximum_backtracking_steps;
    double maximum_round_trip_error_pixels;
    RawInversePolicy(std::uint32_t updates, std::uint32_t backtracks, double error)
        : maximum_newton_updates(updates), maximum_backtracking_steps(backtracks), maximum_round_trip_error_pixels(error) {}
};
enum class RawDocumentOutcome {
    Mapped, InvalidRawPixel, LensDomainUnavailable, UndistortionNonconvergence,
    NumericalFailure, OutsideDocumentDomain, RoundTripExceeded
};
constexpr std::string_view RawDocumentOutcomeCode(RawDocumentOutcome value) {
    switch (value) {
    case RawDocumentOutcome::Mapped: return "raw-document-mapped";
    case RawDocumentOutcome::InvalidRawPixel: return "invalid-raw-pixel";
    case RawDocumentOutcome::LensDomainUnavailable: return "lens-domain-unavailable";
    case RawDocumentOutcome::UndistortionNonconvergence: return "undistortion-nonconvergence";
    case RawDocumentOutcome::NumericalFailure: return "inverse-numerical-failure";
    case RawDocumentOutcome::OutsideDocumentDomain: return "outside-document-domain";
    case RawDocumentOutcome::RoundTripExceeded: return "round-trip-exceeded";
    }
    return {};
}
struct RawDocumentResult {
    RawDocumentOutcome outcome;
    // Only successful points are exposed as usable document coordinates.
    std::optional<render::Point> document_mm;
    std::optional<double> round_trip_error_pixels;
    std::uint32_t newton_updates;
};

// The CLOSED outer ceil-raster rectangle is the allowed inverse domain.
// Conservative positive-Jacobian disk test; no root selection outside it.
// Invalid camera/raster/extent/policy throws; point failures return reasons.
[[nodiscard]] RawDocumentResult MapRawPixelToDocument(const render::CameraProjection& camera,
    const render::OutputRaster& raster, RawImageExtent image, render::Point raw_pixel, const RawInversePolicy& policy);

struct RawPixelCorrespondence {
    render::Point camera_a_raw_pixel, camera_b_raw_pixel;
    RawPixelCorrespondence(render::Point a, render::Point b) : camera_a_raw_pixel(a), camera_b_raw_pixel(b) {}
};
enum class RawPairCorrectionOutcome { Estimated, InvalidCorrespondences, PointMappingFailed, CorrectionRejected };
struct RawPairDocumentCorrectionResult {
    RawPairCorrectionOutcome outcome;
    // Input order preserved, both sides evaluated, no failed-point pruning.
    std::vector<std::array<RawDocumentResult,2>> points;
    // Present only after ALL points mapped (or for count rejection).
    std::optional<DocumentCorrectionResult> correction;
};
// A nominal document coordinates are the output reference; fit maps A -> B.
// Only correction->corrected_camera may be used, and it replaces B alone.
// Does not detect matches, certify their identity, or authorize a product job.
[[nodiscard]] RawPairDocumentCorrectionResult EstimateRawPairDocumentCorrection(
    const render::CameraProjection& camera_a, const render::CameraProjection& camera_b,
    const render::OutputRaster& raster, RawImageExtent image_a, RawImageExtent image_b,
    std::span<const RawPixelCorrespondence> correspondences,
    const RawInversePolicy& inverse_policy, const DocumentCorrectionPolicy& correction_policy);

} // namespace a0::m2::correction
