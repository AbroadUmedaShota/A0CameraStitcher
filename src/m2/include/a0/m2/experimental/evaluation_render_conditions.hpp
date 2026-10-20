#pragma once

#include "a0/m2/document_render.hpp"

#include <optional>
#include <string>

namespace a0::m2::experimental {

// Camera-free evaluation only. This is not an approved profile, a product
// stitch request, or evidence of measured Live/JPEG optical correspondence.
struct SourceDimensions { std::uint32_t width, height; };
struct EvaluationCorrectionIdentity { std::string job_sha256, report_sha256; };
struct EvaluationRenderIdentity {
    std::string profile_fingerprint, profile_file_sha256;
    std::optional<EvaluationCorrectionIdentity> correction, color;
};

// Explicit raw stored-order SAMPLE INDEX mapping, after lens projection:
// live_x = scale_x * still_x + offset_x (and likewise y).
// No inferred dimension ratio, implicit half-pixel offset, or zoom fallback.
struct StillToLiveMapping {
    SourceDimensions still, live;
    double scale_x, scale_y, offset_x, offset_y;
};
enum class MappingEvidence { unverified, synthetic_fixture };
struct LiveViewMappingPair {
    std::string profile_fingerprint;
    std::array<StillToLiveMapping,2> cameras;
    MappingEvidence evidence;
    bool normal_magnification;
};

// Owns a copy of the already evaluated fixed-rig + bounded correction result.
// Caller verifies input/profile/job bytes and numerical acceptance before
// construction; hash labels are provenance, not authentication of image bytes.
// There is no estimator, learning, I/O, or product approval in this pure class.
class EvaluationRenderConditions final {
public:
    EvaluationRenderConditions(render::DocumentRenderParameters parameters,
        SourceDimensions camera_a, SourceDimensions camera_b,
        std::optional<render::BgrAffineTransform> camera_b_color,
        EvaluationRenderIdentity identity);

    const render::DocumentRenderParameters& Parameters() const noexcept { return parameters_; }
    const EvaluationRenderIdentity& Identity() const noexcept { return identity_; }
    const std::optional<render::BgrAffineTransform>& CameraBColor() const noexcept { return color_; }
    render::PairRenderResult RenderStill(const render::BgrImage& a, const render::BgrImage& b) const;
    // Same declared document crop, same kernel/color/layout, lower/equal DPI.
    // Only explicit SYNTHETIC correspondence is currently supported. Physical
    // correspondence approval and normal product C are separate work (#309/#308).
    render::PairRenderResult RenderPreviewForEvaluation(const render::BgrImage& a, const render::BgrImage& b,
        const render::OutputRaster& raster, const LiveViewMappingPair& mapping) const;

private:
    const render::DocumentRenderParameters parameters_;
    const std::array<SourceDimensions,2> sources_;
    const std::optional<render::BgrAffineTransform> color_;
    const EvaluationRenderIdentity identity_;
};

} // namespace a0::m2::experimental
