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
#include <array>
#include <string_view>

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
// residual_max_pixels: a similarity residual maximum above it is warned.
struct StillMergePolicy {
    std::string sha256;
    double rotation_max_degrees{};
    std::optional<double> residual_max_pixels;
};
inline void ValidateStillMergePolicy(const StillMergePolicy& p){
    if(p.sha256.size()!=64 || p.sha256.find_first_not_of("0123456789abcdef")!=std::string::npos ||
        !std::isfinite(p.rotation_max_degrees) || p.rotation_max_degrees<0 || p.rotation_max_degrees>180 ||
        (p.residual_max_pixels && (!std::isfinite(*p.residual_max_pixels) || *p.residual_max_pixels<0)))
        throw std::invalid_argument("invalid still merge policy");
}
// corrected: similarity applied. rotation-exceeded: over the policy limit,
// translation only plus a warning. no-policy: values recorded, translation
// only. undetermined: the live-preview translation, as before #365.
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
struct AutomaticStillAlignment {
    OverlapEstimateStatus status{OverlapEstimateStatus::undetermined};
    SimilarityUndeterminedReason reason{SimilarityUndeterminedReason::no_tiles};
    bool wide_search{};
    unsigned tiles_searched{},tiles_matched{},tiles_agreed{};
    // Present only when estimated; residuals are over the agreed tiles.
    std::optional<SimilarityFit> similarity;
    double translation_x{},translation_y{};
    FitResidual similarity_residual,translation_residual;
    StillAlignmentDecision decision{StillAlignmentDecision::undetermined};
    bool rotation_exceeded{},residual_exceeded{};
    std::optional<StillMergePolicy> policy;
};
// Equal to the limit is inside. An over-limit rotation is never clamped or
// partly applied. A residual warning needs the policy value; it never blocks.
inline AutomaticStillAlignment DecideStillAlignment(const OverlapSimilarityEstimate& e,const std::optional<StillMergePolicy>& policy){
    if(policy)ValidateStillMergePolicy(*policy);
    AutomaticStillAlignment r;r.status=e.status;r.reason=e.reason;r.wide_search=e.wide_search;
    r.tiles_searched=static_cast<unsigned>(e.tiles.size());r.tiles_matched=e.matched;r.tiles_agreed=e.agreed;r.policy=policy;
    if(e.status!=OverlapEstimateStatus::estimated_not_judged)return r;
    r.similarity=e.similarity;r.translation_x=e.translation_x;r.translation_y=e.translation_y;
    r.similarity_residual=e.similarity_residual;r.translation_residual=e.translation_residual;
    if(!policy){r.decision=StillAlignmentDecision::no_policy;return r;}
    r.rotation_exceeded=std::abs(e.similarity.rotation_degrees)>policy->rotation_max_degrees;
    r.decision=r.rotation_exceeded?StillAlignmentDecision::rotation_exceeded:StillAlignmentDecision::corrected;
    r.residual_exceeded=policy->residual_max_pixels && e.similarity_residual.maximum>*policy->residual_max_pixels;
    return r;
}
// Integer B-center position actually rendered, and the applied similarity.
inline std::array<int,2> StillAlignmentTranslation(const AutomaticStillAlignment& a){
    if(!a.similarity)throw std::invalid_argument("still alignment has no estimate");
    const bool similarity=a.decision==StillAlignmentDecision::corrected;
    return {static_cast<int>(std::llround(similarity?a.similarity->x:a.translation_x)),static_cast<int>(std::llround(similarity?a.similarity->y:a.translation_y))};
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
};
struct StillReviewResult {
    std::filesystem::path jpeg,report;
    std::string sha256;
    std::uint32_t width{},height{};
    render::BgrImage thumbnail;
    std::optional<AutomaticStillAlignment> automatic;
    double alignment_milliseconds{};
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
    std::optional<AutomaticStillAlignment> automatic; // v4 reports only
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
}
