#include "a0/m2/experimental/evaluation_render_conditions.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace a0::m2::experimental {
namespace {
using namespace render;
void Require(bool ok, const char* message) {
    if (!ok) throw std::invalid_argument(message);
}
bool SameSize(SourceDimensions a, SourceDimensions b) {
    return a.width==b.width && a.height==b.height;
}
void ImageSize(const BgrImage& image, SourceDimensions size) {
    Require(SameSize({image.width,image.height},size), "evaluation source dimensions mismatch");
    Require(image.bgr.size()==PixelCount(size.width,size.height)*3, "evaluation source buffer length mismatch");
}
void Hash(const std::string& value) {
    Require(value.size()==64 && value.find_first_not_of("0123456789abcdef")==std::string::npos,
        "evaluation identity requires lowercase SHA-256");
}
void CorrectionIdentity(const std::optional<EvaluationCorrectionIdentity>& value) {
    if (value) { Hash(value->job_sha256); Hash(value->report_sha256); }
}
OutputToCamera PreviewProjection(CameraProjection camera, OutputRaster raster, StillToLiveMapping mapping) {
    ValidateCameraProjection(camera,raster);
    return [camera,raster,mapping](Point sample, Point& live) {
        const double pitch=25.4/raster.dpi;
        const Point document{raster.region_um.left/1000.+(sample.x+.5)*pitch,
            raster.region_um.top/1000.+(sample.y+.5)*pitch};
        Point raw{};
        Require(TryProjectDocument(camera,document,raw), "evaluation projection is not numerically representable");
        // Positive offsets/padded Live images must not add sensor coverage.
        if (raw.x<0 || raw.y<0 || raw.x>=mapping.still.width || raw.y>=mapping.still.height) return false;
        live={mapping.scale_x*raw.x+mapping.offset_x,mapping.scale_y*raw.y+mapping.offset_y};
        Require(std::isfinite(live.x) && std::isfinite(live.y), "Live mapping overflow");
        return true;
    };
}
} // namespace

EvaluationRenderConditions::EvaluationRenderConditions(DocumentRenderParameters parameters,
    SourceDimensions a, SourceDimensions b, std::optional<BgrAffineTransform> color, EvaluationRenderIdentity identity)
    : parameters_(std::move(parameters)), sources_{{a,b}}, color_(std::move(color)), identity_(std::move(identity)) {
    (void)PixelCount(a.width,a.height); (void)PixelCount(b.width,b.height);
    ValidateCameraProjection(parameters_.camera_a,parameters_.raster);
    ValidateCameraProjection(parameters_.camera_b,parameters_.raster);
    Require(parameters_.layout==StitchLayout::camera_a_left_camera_b_right ||
        parameters_.layout==StitchLayout::camera_a_top_camera_b_bottom, "unknown evaluation layout");
    Require(parameters_.resampling==Resampling::bilinear || parameters_.resampling==Resampling::bicubic_catmull_rom,
        "unknown evaluation kernel");
    Hash(identity_.profile_fingerprint); Hash(identity_.profile_file_sha256);
    CorrectionIdentity(identity_.correction); CorrectionIdentity(identity_.color);
    Require(!identity_.color || identity_.correction.has_value(), "color identity requires geometry identity");
    Require(color_.has_value()==identity_.color.has_value(), "color transform and provenance must agree");
    if (color_) for (std::size_t c=0; c<3; ++c)
        Require(std::isfinite(color_->gain[c]) && color_->gain[c]>0 && std::isfinite(color_->offset_dn[c]),
            "invalid evaluated color transform");
}

PairRenderResult EvaluationRenderConditions::RenderStill(const BgrImage& a, const BgrImage& b) const {
    ImageSize(a,sources_[0]); ImageSize(b,sources_[1]);
    return color_ ? RenderDocumentPair(a,b,parameters_,*color_) : RenderDocumentPair(a,b,parameters_);
}

PairRenderResult EvaluationRenderConditions::RenderPreviewForEvaluation(const BgrImage& a, const BgrImage& b,
    const OutputRaster& raster, const LiveViewMappingPair& mapping) const {
    Require(mapping.profile_fingerprint==identity_.profile_fingerprint, "Live mapping profile mismatch");
    Require(mapping.evidence==MappingEvidence::synthetic_fixture, "only synthetic Live mapping evaluation is supported");
    Require(mapping.normal_magnification, "magnified Live mapping rejected");
    ValidateOutputRaster(raster);
    const auto& original=parameters_.raster.region_um; const auto& crop=raster.region_um;
    Require(crop.left==original.left && crop.top==original.top && crop.right==original.right && crop.bottom==original.bottom,
        "preview must retain the selected document crop");
    Require(raster.dpi<=parameters_.raster.dpi, "preview density exceeds selected still density");
    for (std::size_t i=0; i<2; ++i) {
        const auto& m=mapping.cameras[i];
        Require(SameSize(m.still,sources_[i]), "Live mapping original dimensions mismatch");
        (void)PixelCount(m.live.width,m.live.height);
        Require(std::isfinite(m.scale_x) && m.scale_x>0 && std::isfinite(m.scale_y) && m.scale_y>0 &&
            std::isfinite(m.offset_x) && std::isfinite(m.offset_y), "invalid explicit Live mapping");
    }
    ImageSize(a,mapping.cameras[0].live); ImageSize(b,mapping.cameras[1].live);
    const MappedRenderParameters mapped{raster.width_pixels,raster.height_pixels,
        PreviewProjection(parameters_.camera_a,raster,mapping.cameras[0]),
        PreviewProjection(parameters_.camera_b,raster,mapping.cameras[1]),parameters_.layout,parameters_.resampling};
    return color_ ? RenderMappedPair(a,b,mapped,*color_) : RenderMappedPair(a,b,mapped);
}
} // namespace a0::m2::experimental
