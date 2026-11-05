#pragma once
#include "a0/m2/render.hpp"
#include <chrono>
#include <filesystem>
#include <string>
#include <cmath>
#include <stdexcept>
#include <optional>
#include <stdexcept>
#include <string_view>
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
// Fixed composite review (#400, parent #396). The mapping and the seam column
// come from the fixed composite condition of the setup (#399); a capture only
// measures the brightness ratio and, inside a limit, a scale difference between
// the two cameras. The names below are the report's codes.
//
// applied: the scale difference is inside the limit (equal to it is inside) and
// the scale was multiplied into B's placement about B's image center.
// exceeded: over the limit, nothing applied (the gate fails it later).
// not_estimated: no usable estimate, nothing applied. seam_outside_overlap: the
// correction would leave the seam outside the overlap, nothing applied.
// output_uncovered (lens-aware v7 only, #405): the correction would leave an output pixel that
// neither camera shows, nothing applied.
enum class FixedScaleStatus {applied,exceeded,not_estimated,seam_outside_overlap,output_uncovered};
constexpr std::string_view FixedScaleStatusCode(FixedScaleStatus v){
    switch(v){
    case FixedScaleStatus::applied:return "applied";
    case FixedScaleStatus::exceeded:return "exceeded";
    case FixedScaleStatus::not_estimated:return "not-estimated";
    case FixedScaleStatus::seam_outside_overlap:return "seam-outside-overlap";
    case FixedScaleStatus::output_uncovered:return "output-uncovered";
    }
    return {};
}
// Why no estimate was usable. The middle four are the similarity estimator's own
// undetermined reasons. too_few_tiles: fewer than three tiles over two rows lie
// within the estimator's search window around the fixed mapping.
// degenerate_fit: the scale-only fit has no lever arm or a non-positive result.
enum class FixedScaleReason {none,no_tiles,insufficient_tiles,outside_image,tied_starts,too_few_tiles,degenerate_fit};
constexpr std::string_view FixedScaleReasonCode(FixedScaleReason v){
    switch(v){
    case FixedScaleReason::none:return "none";
    case FixedScaleReason::no_tiles:return "no-tiles";
    case FixedScaleReason::insufficient_tiles:return "insufficient-tiles";
    case FixedScaleReason::outside_image:return "outside-image";
    case FixedScaleReason::tied_starts:return "tied-starts";
    case FixedScaleReason::too_few_tiles:return "too-few-tiles-near-fixed-mapping";
    case FixedScaleReason::degenerate_fit:return "degenerate-fit";
    }
    return {};
}
struct FixedScaleCorrection {
    FixedScaleStatus status{FixedScaleStatus::not_estimated};
    FixedScaleReason reason{FixedScaleReason::no_tiles};
    // From the condition record; no code default.
    double limit_percent{};
    // Present when a scale-only fit exists. difference_percent is
    // |estimated*100-100|, the value compared with the limit.
    std::optional<double> estimated_scale,difference_percent;
    // The factor multiplied into B's placement: the estimate when applied, else 1.
    double applied_scale{1};
    // Fixed point of the correction: B's image center moved by the fixed mapping, in A's pixels.
    render::Point center{};
    unsigned used_tiles{};
    FitResidual residual; // of the fitted model (scale and free shift) over the used tiles
    // What the scale could not explain: the mean shift left after the scale, in
    // A pixels with P as the origin (t = k q + d). Present with estimated_scale.
    // Diagnostic only; it is not applied. A large value means B moved (shake, a
    // pattern period) or the scale acts about B's own center rather than about P.
    render::Point unexplained_shift{};
};
// The #365 similarity estimate of this capture, kept for diagnosis only. It is
// never used for the geometry (only its scale-only reduction above may be).
struct FixedSimilarityDiagnostic {
    OverlapEstimateStatus status{OverlapEstimateStatus::undetermined};
    SimilarityUndeterminedReason reason{SimilarityUndeterminedReason::no_tiles};
    bool wide_search{};
    unsigned tiles_searched{},tiles_matched{},tiles_agreed{};
    std::optional<SimilarityFit> similarity; // B centre relative to A, as the estimator reports it
    FitResidual similarity_residual;
};
// The scale limit of the condition record is the one used (the value copied at
// setup). The merge policy file may disagree; that never changes the composite,
// it is only recorded and shown so the condition can be made again.
// same / differs: the policy has a scale limit, equal or not. The others: the
// policy file is absent, unreadable, or has no scaleAutoCorrectionMaxPercent.
enum class FixedPolicyCheck {same,differs,policy_absent,policy_unreadable,policy_without_scale_limit};
constexpr std::string_view FixedPolicyCheckCode(FixedPolicyCheck v){
    switch(v){
    case FixedPolicyCheck::same:return "same";
    case FixedPolicyCheck::differs:return "differs";
    case FixedPolicyCheck::policy_absent:return "policy-absent";
    case FixedPolicyCheck::policy_unreadable:return "policy-unreadable";
    case FixedPolicyCheck::policy_without_scale_limit:return "policy-without-scale-limit";
    }
    return {};
}
struct FixedPolicyLimit {
    FixedPolicyCheck check{FixedPolicyCheck::policy_absent};
    std::optional<double> policy_percent; // present for same and differs only
};
// What a v6 report records about the fixed composite it was made with.
struct SavedFixedComposite {
    std::string condition_sha256;           // SHA-256 of the condition file used
    std::uint32_t seam_column{};            // output pixel column, after the crop
    std::string seam_origin;                // "band-center-provisional" or "operator"
    FixedPolicyLimit policy_limit;
    FixedScaleCorrection scale;
    FixedSimilarityDiagnostic diagnostic;
};

// The per-capture scale of the lens-aware fixed composite (#405 W3, DecideFixedScaleV2 in
// fixed_scale_correction_v2.hpp) and what a v7 report records about it. It lives here, not
// in that header, because the report types below hold it.
struct FixedScaleCorrectionV2 {
    FixedScaleStatus status{FixedScaleStatus::not_estimated};
    FixedScaleReason reason{FixedScaleReason::no_tiles};
    // From the condition record; no code default.
    double limit_percent{};
    // Present when a fit exists. difference_percent is |estimated*100-100|, the value compared with the limit.
    std::optional<double> estimated_scale,difference_percent;
    // The factor put into B's mapping: the estimate when applied, else 1.
    double applied_scale{1};
    // Fixed point of the correction: document mm, and the same point in output raster pixels
    // (document point minus the raster's top left, over the pitch, minus half a pixel: the
    // coordinates ScaledDocumentOutputToCamera takes its samples in).
    render::Point center_mm{},center_output_pixels{};
    unsigned used_tiles{};
    // Rows of the estimator's tile grid that used tiles came from: how many distinct, and the
    // first and last. Meaningful when used_tiles > 0. A short span means a short lever arm.
    unsigned used_rows{},first_used_row{},last_used_row{};
    // Residual of the fitted model (scale and free shift) over the used tiles.
    FitResidual residual_mm,residual_output_pixels;
    // t = k q + d: what the scale could not explain. Present with estimated_scale. Diagnostic
    // only, never applied. A large value means B moved (shake, a pattern period) or the
    // scale acts about B's own optical centre rather than about P.
    render::Point unexplained_shift_mm{},unexplained_shift_output_pixels{};
};

// What a v7 report records about the lens-aware fixed composite it was made with (#405 W4).
// The mapping itself is not recorded: it is the profile's, named here by hash and fingerprint,
// with the condition's raster; the condition and the profile files hold the values. Everything
// that can be derived from these fields is derived again on reading (still_review_report_impl.hpp).
struct SavedFixedCompositeLens {
    std::string condition_sha256;           // SHA-256 of the condition file used
    std::string profile_file;               // the condition's plain file name for the profile
    std::string profile_file_sha256,profile_fingerprint_sha256;
    std::optional<std::string> profile_id;  // the profile's own identifier, when it has one
    // The output raster of the condition (micrometres of the document plane, DPI, pixels); the
    // JPEG is exactly this size and does not change with the capture.
    render::OutputRaster raster{render::DocumentRegionUm{0,0,0,0},0,0,0};
    std::uint32_t seam_column{};            // output pixel column of the raster
    std::string seam_origin;                // "band-center-provisional" or "operator"
    FixedPolicyLimit policy_limit;
    FixedScaleCorrectionV2 scale;
    // The #365 estimator's own summary for this capture (its tiles feed the scale; its fit is not used).
    OverlapEstimateStatus estimate_status{OverlapEstimateStatus::undetermined};
    SimilarityUndeterminedReason estimate_reason{SimilarityUndeterminedReason::no_tiles};
    bool wide_search{};
    unsigned tiles_searched{},tiles_matched{},tiles_agreed{};
};
// How far this capture's B sat from where the condition puts it: the estimate
// minus the fixed translation, the rotation difference in degrees, the scale
// as a ratio. Recomputed from recorded values; absent without an estimate.
struct FixedMappingDelta {double x{},y{},rotation_degrees{},scale_ratio{1};};
inline std::optional<FixedMappingDelta> FixedDiagnosticDelta(const FixedSimilarityDiagnostic& d,int fixed_x,int fixed_y,double fixed_rotation,double fixed_scale){
    if(!d.similarity)return std::nullopt;
    return FixedMappingDelta{d.similarity->x-fixed_x,d.similarity->y-fixed_y,d.similarity->rotation_degrees-fixed_rotation,d.similarity->scale/fixed_scale};
}

struct FixedCompositeCondition; // fixed_composite_condition.hpp
struct FixedCompositeConditionV2; // fixed_composite_condition_v2.hpp
struct FixedCompositeProfileBinding; // fixed_composite_condition_v2.hpp

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
    // #400: compose with the fixed composite condition instead of an estimated
    // or manual geometry. The condition is owned by the caller and must outlive
    // the call. Exclusive with manual_revision, automatic_alignment and blend
    // (the seam is the condition's vertical column). condition_sha256 is the
    // SHA-256 of the exact file bytes the condition was read from.
    struct FixedComposite {const FixedCompositeCondition* condition{};std::string condition_sha256;FixedPolicyLimit policy_limit;};
    std::optional<FixedComposite> fixed_composite;
    // #405 W4: compose with the lens-aware fixed composite condition (v2) and the profile it names,
    // already verified by LoadFixedCompositeConditionAny. Both are owned by the caller and must outlive
    // the call. Exclusive with fixed_composite, manual_revision, automatic_alignment and blend. The report
    // is v7. The originals must be the profile's frame size.
    struct FixedCompositeLens {
        const FixedCompositeConditionV2* condition{};
        const FixedCompositeProfileBinding* profile{};
        std::string condition_sha256;
        FixedPolicyLimit policy_limit;
    };
    std::optional<FixedCompositeLens> fixed_composite_lens;
};
// Why a fixed composite could not be made for a capture. The writer throws it
// before any output exists, so the caller knows that only the originals remain.
// profile_absent / profile_differs (#405 W4): a lens-aware (v2) condition whose profile file is not
// beside it, or is there but is not the one the condition names or cannot be used.
enum class FixedCompositeSkip {condition_absent,condition_invalid,orientation_differs,size_differs,seam_outside_overlap,profile_absent,profile_differs};
constexpr std::string_view FixedCompositeSkipCode(FixedCompositeSkip v){
    switch(v){
    case FixedCompositeSkip::condition_absent:return "condition-absent";
    case FixedCompositeSkip::condition_invalid:return "condition-invalid";
    case FixedCompositeSkip::orientation_differs:return "orientation-differs";
    case FixedCompositeSkip::size_differs:return "size-differs";
    case FixedCompositeSkip::seam_outside_overlap:return "seam-outside-overlap";
    case FixedCompositeSkip::profile_absent:return "profile-absent";
    case FixedCompositeSkip::profile_differs:return "profile-differs";
    }
    return {};
}
class FixedCompositeUnavailable : public std::runtime_error {
public:
    FixedCompositeUnavailable(FixedCompositeSkip skip,const std::string& detail):
        std::runtime_error("fixed composite unavailable ("+std::string(FixedCompositeSkipCode(skip))+"): "+detail),skip_(skip){}
    FixedCompositeSkip Skip() const noexcept {return skip_;}
private:
    FixedCompositeSkip skip_;
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
    std::optional<SavedFixedComposite> fixed_composite; // v6 reports only
    // v7 only (#405 W4). alignment_milliseconds is then the scale estimate (prior, tiles, decision),
    // blend_milliseconds the composite.
    std::optional<SavedFixedCompositeLens> fixed_composite_lens;
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
    std::optional<SeamBlendRecord> blend; // seam-cut only; absent is the linear feather (v6: the fixed seam, fixed_seam set)
    // v6 reports only. translation_x/y, rotation_degrees and scale are then the
    // fixed mapping; fixed_composite->scale.applied_scale is multiplied about
    // scale.center on top of it. crop and width/height are what was rendered.
    std::optional<SavedFixedComposite> fixed_composite;
    // v7 reports only (#405 W4). translation_x/y, rotation_degrees, scale and crop carry nothing then
    // (0, 0, 0, 1, empty): the mapping is the profile's. width/height are the condition's raster.
    // A tool that places B by a similarity (re-Merge, the saved-condition maker) must refuse it.
    std::optional<SavedFixedCompositeLens> fixed_composite_lens;
};
// StillReview manual fields (#382). They start at the geometry the report
// actually rendered, so a re-Merge without edits keeps the parent geometry.
inline StillAlignmentGeometry SavedStillReviewGeometry(const SavedStillReview& r){
    return {r.translation_x,r.translation_y,r.rotation_degrees,r.scale};
}
// The estimated similarity, entered only by an explicit operator action.
// Absent without an estimate or when it cannot be rendered for this pair.
inline std::optional<StillAlignmentGeometry> EstimatedStillReviewGeometry(const SavedStillReview& r){
    if(!r.automatic || !r.automatic->similarity)return std::nullopt;
    const auto& s=*r.automatic->similarity;
    const StillAlignmentGeometry g{static_cast<int>(std::llround(s.x)),static_cast<int>(std::llround(s.y)),s.rotation_degrees,s.scale};
    try{(void)SimilarityReviewGeometry(g.x,g.y,g.rotation_degrees,g.scale,r.source_width,r.source_height);}
    catch(const std::invalid_argument&){return std::nullopt;}
    return g;
}
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
// A v7 report against the profile it names (the binding LoadFixedCompositeConditionAny returns): the
// profile's hash, fingerprint and identifier, the raster, the centre of the scale correction, and, for
// a provisional seam, the band middle must all be what the report says. The report alone cannot check
// these (it does not carry the profile). Throws std::invalid_argument if the report is not v7 or differs.
void VerifySavedFixedCompositeLens(const SavedStillReview& review,const FixedCompositeProfileBinding& profile);
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
