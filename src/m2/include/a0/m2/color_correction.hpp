#pragma once
#include "a0/m2/document_render.hpp"
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace a0::m2::correction {
inline constexpr std::size_t kMaximumColorSamples=4096;
struct ColorCorrespondence { std::array<double,3> camera_a_bgr, camera_b_bgr; };
// Explicit development budgets in stored 8-bit DN; no approved optical defaults.
struct ColorCorrectionPolicy {
    std::size_t minimum_samples;
    double minimum_sample_dn, maximum_sample_dn, minimum_stddev_dn;
    double maximum_gain_difference_percent, maximum_offset_dn, maximum_residual_dn;
    ColorCorrectionPolicy(std::size_t count,double low,double high,double contrast,double gain,double offset,double residual)
        : minimum_samples(count),minimum_sample_dn(low),maximum_sample_dn(high),minimum_stddev_dn(contrast),
          maximum_gain_difference_percent(gain),maximum_offset_dn(offset),maximum_residual_dn(residual) {}
};
enum class ColorCorrectionOutcome {
    Estimated, InsufficientSamples, TooManySamples, InvalidSamples, SampleRangeRejected,
    InsufficientContrast, NonpositiveGain, NumericalFailure, GainExceeded, OffsetExceeded, ResidualExceeded
};
constexpr std::string_view ColorCorrectionOutcomeCode(ColorCorrectionOutcome v) {
    switch(v) {
    case ColorCorrectionOutcome::Estimated:return "color-estimated";
    case ColorCorrectionOutcome::InsufficientSamples:return "insufficient-color-samples";
    case ColorCorrectionOutcome::TooManySamples:return "too-many-color-samples";
    case ColorCorrectionOutcome::InvalidSamples:return "invalid-color-samples";
    case ColorCorrectionOutcome::SampleRangeRejected:return "color-sample-range-rejected";
    case ColorCorrectionOutcome::InsufficientContrast:return "insufficient-color-contrast";
    case ColorCorrectionOutcome::NonpositiveGain:return "nonpositive-color-gain";
    case ColorCorrectionOutcome::NumericalFailure:return "color-numerical-failure";
    case ColorCorrectionOutcome::GainExceeded:return "color-gain-exceeded";
    case ColorCorrectionOutcome::OffsetExceeded:return "color-offset-exceeded";
    case ColorCorrectionOutcome::ResidualExceeded:return "color-residual-exceeded";
    } return {};
}
struct ChannelAffine { double gain,offset_dn; };
struct ColorResiduals { double rms_dn,maximum_dn; };
struct ColorChannelDiagnostics {
    double camera_a_stddev_dn,camera_b_stddev_dn;
    std::optional<ChannelAffine> estimate;
    std::optional<ColorResiduals> residuals;
};
struct ColorViolation { ColorCorrectionOutcome reason; std::size_t channel; std::optional<std::size_t> sample_index; };
struct ColorCorrectionResult {
    ColorCorrectionOutcome outcome;
    std::size_t sample_count;
    std::array<std::optional<ColorChannelDiagnostics>,3> channels;
    std::vector<ColorViolation> violations;
    // Exists only when all channels, all samples and all budgets pass.
    std::optional<render::BgrAffineTransform> transform;
};
[[nodiscard]] ColorCorrectionResult EstimateBoundedColorCorrection(
    std::span<const ColorCorrespondence> samples,const ColorCorrectionPolicy& policy);

enum class ColorSampleOutcome { Sampled,InvalidSeed,SampleUnavailable };
struct DocumentColorSample {
    render::Point document_mm;
    ColorSampleOutcome outcome;
    std::optional<ColorCorrespondence> colors;
};
enum class ImageColorOutcome { Estimated,TooManySeeds,SamplingFailed,CorrectionRejected };
struct ImageColorCorrectionResult {
    ImageColorOutcome outcome;
    std::vector<DocumentColorSample> points;
    std::optional<ColorCorrectionResult> correction;
};
// A uses its fixed mapping, B its already geometrically corrected mapping.
// Bilinear color observations; all explicit seeds must succeed, no pruning.
[[nodiscard]] ImageColorCorrectionResult EstimateImageColorCorrection(
    const render::BgrImage& a,const render::BgrImage& b,
    const render::CameraProjection& camera_a,const render::CameraProjection& aligned_camera_b,
    const render::OutputRaster& raster,std::span<const render::Point> document_seeds_mm,
    const ColorCorrectionPolicy& policy);
} // namespace a0::m2::correction
