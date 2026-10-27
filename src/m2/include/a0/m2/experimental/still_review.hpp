#pragma once
#include "a0/m2/render.hpp"
#include <chrono>
#include <filesystem>
#include <string>
#include <cmath>
#include <stdexcept>
#include <optional>
#include "a0/m2/experimental/review_alignment.hpp"
#include "a0/m2/experimental/mount_orientation.hpp"
#include "a0/m2/experimental/overlap_estimation.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include <array>
#include <string_view>
#include <vector>

namespace a0::m2::experimental {
// Every translation, crop and adjustment below is expressed in the frame of
// the oriented render inputs. Reports omit orientation when it is normal.
struct PreviewReviewGeometry {
    std::uint32_t width{},height{},overlap{};
    int vertical{};
    std::uint64_t revision{};
    MountOrientation orientation{MountOrientation::normal};
};
// Approximate normalized-frame translation only. Optical field-of-view
// equivalence has NOT been measured. Never produces a rig profile or approval.
inline render::PairRenderParameters ScalePreviewReviewGeometry(
    PreviewReviewGeometry g,std::uint32_t width,std::uint32_t height){
    if(g.width<2 || g.width>640 || !g.height || g.height>640 || !g.overlap || g.overlap>=g.width ||
        g.vertical<=-static_cast<int>(g.height) || g.vertical>=static_cast<int>(g.height))
        throw std::invalid_argument("invalid review preview geometry");
    (void)MountOrientationName(g.orientation);(void)render::PixelCount(width,height);
    const auto dx=std::llround(double(g.width-g.overlap)*width/g.width);
    const auto dy=std::llround(double(g.vertical)*height/g.height);
    if(dx<=0 || dx>=width || std::abs(dy)>=height)throw std::invalid_argument("review translation is outside image");
    const auto trim=static_cast<std::uint32_t>(std::abs(dy));
    render::PairRenderParameters p{{1,0,double(dx),0,1,double(dy),0,0,1},StitchLayout::camera_a_left_camera_b_right,{0,trim,0,trim}};
    (void)render::PlanPairCanvas(width,height,width,height,p);return p;
}
// Development merge policy (#365), read from an administrator-written local
// file. No value has a code default: without a file nothing is corrected.
// rotation_max_degrees: |rotation| up to and including it is corrected.
// scale_max_percent: |scale*100-100| up to and including it is corrected,
// only together with the rotation; absent means scale is never corrected.
// residual_max_pixels: a similarity residual maximum above it is warned.
struct StillMergePolicy {
    std::string sha256;
    double rotation_max_degrees{};
    std::optional<double> residual_max_pixels;
    std::optional<double> scale_max_percent;
};
inline void ValidateStillMergePolicy(const StillMergePolicy& p){
    const auto optional_valid=[](const std::optional<double>& v){return !v || (std::isfinite(*v) && *v>=0);};
    if(p.sha256.size()!=64 || p.sha256.find_first_not_of("0123456789abcdef")!=std::string::npos ||
        !std::isfinite(p.rotation_max_degrees) || p.rotation_max_degrees<0 || p.rotation_max_degrees>180 ||
        !optional_valid(p.residual_max_pixels) || !optional_valid(p.scale_max_percent))
        throw std::invalid_argument("invalid still merge policy");
}
// The scale difference compared with scale_max_percent, in percent.
inline double StillScaleDifferencePercent(double scale){return std::abs(scale*100-100);}
// Rotation, which decides whether anything beyond translation is applied.
// corrected: rotation applied (scale per StillScaleDecision). rotation-exceeded:
// over the limit, translation only plus a warning. no-policy: values recorded,
// translation only. undetermined: the live-preview translation, as before #365.
enum class StillAlignmentDecision {corrected,rotation_exceeded,no_policy,undetermined};
constexpr std::string_view StillAlignmentDecisionCode(StillAlignmentDecision v){
    switch(v){
    case StillAlignmentDecision::corrected:return "corrected";
    case StillAlignmentDecision::rotation_exceeded:return "rotation-exceeded";
    case StillAlignmentDecision::no_policy:return "no-policy";
    case StillAlignmentDecision::undetermined:return "undetermined";
    }
    return {};
}
// Scale, judged against its own limit. corrected: applied with the rotation.
// scale-exceeded: over the limit, not applied, warned (also when the rotation
// is over its limit). no-scale-policy: the policy has no scale limit, not
// applied. not-applied: inside the limit but the rotation is over its limit,
// so only translation is applied. no-policy / undetermined: as the decision.
enum class StillScaleDecision {corrected,scale_exceeded,no_scale_policy,not_applied,no_policy,undetermined};
constexpr std::string_view StillScaleDecisionCode(StillScaleDecision v){
    switch(v){
    case StillScaleDecision::corrected:return "corrected";
    case StillScaleDecision::scale_exceeded:return "scale-exceeded";
    case StillScaleDecision::no_scale_policy:return "no-scale-policy";
    case StillScaleDecision::not_applied:return "not-applied";
    case StillScaleDecision::no_policy:return "no-policy";
    case StillScaleDecision::undetermined:return "undetermined";
    }
    return {};
}
struct AutomaticStillAlignment {
    OverlapEstimateStatus status{OverlapEstimateStatus::undetermined};
    SimilarityUndeterminedReason reason{SimilarityUndeterminedReason::no_tiles};
    bool wide_search{};
    unsigned tiles_searched{},tiles_matched{},tiles_agreed{};
    // Present only when estimated; residuals are over the agreed tiles.
    // rigid: the similarity's rotation with scale 1, translation refitted.
    std::optional<SimilarityFit> similarity,rigid;
    double translation_x{},translation_y{};
    FitResidual similarity_residual,rigid_residual,translation_residual;
    StillAlignmentDecision decision{StillAlignmentDecision::undetermined};
    StillScaleDecision scale_decision{StillScaleDecision::undetermined};
    bool rotation_exceeded{},scale_exceeded{},residual_exceeded{};
    std::optional<StillMergePolicy> policy;
};
// Rotation and scale are each compared with their own limit; equal to a limit
// is inside. Within the rotation limit the rotation is applied, and the scale
// only when the policy has a scale limit and the scale is inside it; otherwise
// the scale stays 1 and the rigid fit is applied. Over the rotation limit only
// the translation is applied. No over-limit value is clamped or partly applied.
// A residual warning needs the policy value; it never blocks.
inline AutomaticStillAlignment DecideStillAlignment(const OverlapSimilarityEstimate& e,const std::optional<StillMergePolicy>& policy){
    if(policy)ValidateStillMergePolicy(*policy);
    AutomaticStillAlignment r;r.status=e.status;r.reason=e.reason;r.wide_search=e.wide_search;
    r.tiles_searched=static_cast<unsigned>(e.tiles.size());r.tiles_matched=e.matched;r.tiles_agreed=e.agreed;r.policy=policy;
    if(e.status!=OverlapEstimateStatus::estimated_not_judged)return r;
    r.similarity=e.similarity;r.rigid=e.rigid;r.translation_x=e.translation_x;r.translation_y=e.translation_y;
    r.similarity_residual=e.similarity_residual;r.rigid_residual=e.rigid_residual;r.translation_residual=e.translation_residual;
    if(!policy){r.decision=StillAlignmentDecision::no_policy;r.scale_decision=StillScaleDecision::no_policy;return r;}
    r.rotation_exceeded=std::abs(e.similarity.rotation_degrees)>policy->rotation_max_degrees;
    r.decision=r.rotation_exceeded?StillAlignmentDecision::rotation_exceeded:StillAlignmentDecision::corrected;
    r.scale_exceeded=policy->scale_max_percent && StillScaleDifferencePercent(e.similarity.scale)>*policy->scale_max_percent;
    r.scale_decision=!policy->scale_max_percent?StillScaleDecision::no_scale_policy:r.scale_exceeded?StillScaleDecision::scale_exceeded:
        r.rotation_exceeded?StillScaleDecision::not_applied:StillScaleDecision::corrected;
    r.residual_exceeded=policy->residual_max_pixels && e.similarity_residual.maximum>*policy->residual_max_pixels;
    return r;
}
// The geometry actually rendered: integer B-center position, rotation, scale.
struct StillAlignmentGeometry {int x{},y{};double rotation_degrees{},scale{1};};
inline StillAlignmentGeometry AppliedStillAlignment(const AutomaticStillAlignment& a){
    if(!a.similarity || !a.rigid)throw std::invalid_argument("still alignment has no estimate");
    const auto at=[](double x,double y,double degrees,double scale){return StillAlignmentGeometry{static_cast<int>(std::llround(x)),static_cast<int>(std::llround(y)),degrees,scale};};
    if(a.decision!=StillAlignmentDecision::corrected)return at(a.translation_x,a.translation_y,0,1);
    if(a.scale_decision==StillScaleDecision::corrected)return at(a.similarity->x,a.similarity->y,a.similarity->rotation_degrees,a.similarity->scale);
    return at(a.rigid->x,a.rigid->y,a.rigid->rotation_degrees,1);
}
// Residual over the agreed tiles of the fit actually applied.
inline FitResidual AppliedStillResidual(const AutomaticStillAlignment& a){
    if(a.decision!=StillAlignmentDecision::corrected)return a.translation_residual;
    return a.scale_decision==StillScaleDecision::corrected?a.similarity_residual:a.rigid_residual;
}
// Report warning codes, in this order.
inline std::vector<std::string_view> StillAlignmentWarnings(const AutomaticStillAlignment& a){
    std::vector<std::string_view> w;
    if(a.rotation_exceeded)w.push_back("rotation-exceeded");
    if(a.scale_exceeded)w.push_back("scale-exceeded");
    if(a.residual_exceeded)w.push_back("fit-residual-exceeded");
    return w;
}
// Report geometryBasis of an automatic result.
inline std::string_view StillAlignmentBasis(const AutomaticStillAlignment& a){
    if(!a.similarity)return "live-preview";
    if(a.decision!=StillAlignmentDecision::corrected)return "auto-still-translation";
    return a.scale_decision==StillScaleDecision::corrected?"auto-still-similarity":"auto-still-rotation";
}
struct StillReviewRequest {
    std::filesystem::path camera_a_original,camera_b_original,output_directory;
    std::string camera_a_sha256,camera_b_sha256,pair_id;
    PreviewReviewGeometry geometry;
    std::chrono::steady_clock::time_point deadline;
    struct ManualRevision {
        int x{},y{};
        std::filesystem::path parent_report;
        std::string parent_report_sha256;
        double rotation_degrees{},scale{1};
    };
    std::optional<ManualRevision> manual_revision;
    // Pair capture only (#365): estimate on the oriented stills, starting at
    // the scaled live geometry, and decide with the policy. Not with a manual
    // revision. Absent keeps the earlier v1 report and render unchanged.
    struct AutomaticAlignment {std::optional<StillMergePolicy> policy;};
    std::optional<AutomaticAlignment> automatic_alignment;
    // #368: seam_cut adds the report blend record; linear_feather keeps the
    // earlier render and report bytes.
    BlendMethod blend{BlendMethod::linear_feather};
};
struct StillReviewResult {
    std::filesystem::path jpeg,report;
    std::string sha256;
    std::uint32_t width{},height{};
    render::BgrImage thumbnail;
    std::optional<AutomaticStillAlignment> automatic;
    double alignment_milliseconds{};
    std::optional<SeamBlendRecord> blend;
    double blend_milliseconds{};
};
// Exists only in the explicitly enabled development preview build. A separate
// uncalibrated review report, never a successful product StitchJob manifest.
StillReviewResult WriteUncalibratedStillReview(const StillReviewRequest& request);
struct SavedStillReview {
    std::filesystem::path report,jpeg,camera_a_original,camera_b_original;
    std::string report_sha256,jpeg_sha256,camera_a_sha256,camera_b_sha256,pair_id;
    std::uint64_t jpeg_bytes{},camera_a_bytes{},camera_b_bytes{};
    std::uint32_t source_width{},source_height{},width{},height{};
    PreviewReviewGeometry geometry;
    int translation_x{},translation_y{};
    std::optional<StillReviewRequest::ManualRevision> manual_revision;
    double rotation_degrees{},scale{1};
    StitchCropPixels crop;
    std::optional<AutomaticStillAlignment> automatic; // v5 reports only
    std::optional<SeamBlendRecord> blend; // seam-cut only; absent is the linear feather
};
inline render::PairRenderParameters StillPixelReviewGeometry(int x,int y,std::uint32_t width,std::uint32_t height){
    (void)render::PixelCount(width,height);
    if(x<=0 || x>=static_cast<int>(width) || y<=-static_cast<int>(height) || y>=static_cast<int>(height))
        throw std::invalid_argument("manual review translation is outside image");
    const auto trim=static_cast<std::uint32_t>(std::abs(y));
    render::PairRenderParameters p{{1,0,double(x),0,1,double(y),0,0,1},StitchLayout::camera_a_left_camera_b_right,{0,trim,0,trim}};
    (void)render::PlanPairCanvas(width,height,width,height,p);return p;
}
// Camera-free, explicit-report loading. Verifies schema, false quality flags,
// aliases, immutable input/output hashes/dimensions and immediate parent link.
SavedStillReview LoadSavedStillReview(const std::filesystem::path& report);
// Local, camera-free handoff. Uses the same strict reader and carries paths
// only to the explicitly invoked development tool; never publish this JSON.
std::string DescribeSavedReviewEvaluationInputs(const std::filesystem::path& report);
render::BgrImage LoadStillReviewImage(const SavedStillReview& review);
// Decoded A/B render inputs, turned in memory by the report's orientation.
std::array<render::BgrImage,2> LoadStillReviewSources(const SavedStillReview& review);
// Development comparison (#368): renders the report's pair with the linear
// feather and with the seam cut into new files <prefix>-feather.jpg,
// <prefix>-seam.jpg and native-size crops around the seam middle
// (<prefix>-feather-crop.jpg, <prefix>-seam-crop.jpg). Never overwrites.
struct StillBlendComparison {
    double feather_milliseconds{},seam_milliseconds{};
    bool feather_matches_saved{};
    SeamBlendRecord seam;
    std::vector<SeamDisplacementProbe> probes;
    std::uint32_t crop_left{},crop_top{},crop_width{},crop_height{};
};
StillBlendComparison CompareStillReviewBlends(const SavedStillReview& review,const std::filesystem::path& prefix,std::uint32_t crop_width,std::uint32_t crop_height);
}
