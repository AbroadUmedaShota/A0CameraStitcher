#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/fixed_composite_creation_v3.hpp"
#include "a0/m2/experimental/fixed_composite_selection.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::experimental::recalibration {
// Recalibration of camera B from one pair (#434, docs/design/recalibration-from-pair.md). After the cameras were moved apart (or
// together), the lens and camera A are kept, and the mapping of camera B is taken again from the content of one pair of originals:
// the pose of B is estimated against the active revision's profile, baked into the document-to-image matrix of B in a new draft
// profile, and a new revision of the condition (kind `recalibrated`) is made that names the new profile, a new output raster and a
// new seam column. The previous revision and its profile are never touched.
//
// Pure computation: no camera, no clock (the time is a parameter), no file read or written, no hash of its own (the SHA-256
// provider is a parameter). The caller (the development entrance, the preview) writes what a ready proposal holds, in the order
// of the design: the profile copy first, then the revision.
//
//   R1 EstimateWideStart      the wide search: the pair is matched over the whole range of overlaps (10 to 90 percent of the
//                             width), the matched tiles are carried to the document through the calibrated mappings of A and
//                             B, and the rigid shift of the document plane (tx, ty, rz) between them is the start p0.
//   R2 (in ProposeRecalibration)  the estimator of #417 in six degrees of freedom (no paper surface), base pose p0, with the
//                             condition's ranges, limits and geometry. It has to be `verified`.
//   R3 ReanchoredProfileText  B's document-to-image matrix of the new draft profile is PoseModel(B).CameraHomography(p-hat).
//   R4 ProposeFromPose        the output raster, the seam column, the candidate revision and the creation check of
//                             fixed_composite_creation_v3.hpp.
//      VerifyCandidate        the estimator again on the new profile, the way the capture path runs it: the pose it finds has to
//                             be close to nothing (the line is registration.limits.convergencePx).
//   R5 SelectionOfCandidate   the candidate as the in-memory selection that the pair composite takes (a thin step; the composite
//                             itself is the existing one).
//
// There is no default in code for any range, line or limit: they come from the active revision's condition. The estimator's design
// constants (RegistrationDesign) are the caller's, as in the capture path.

inline constexpr std::string_view kEstimateRecordSchema="a0.pair-recalibration-estimate.v1";
// What the new profile's calibration says made it. `method` stays the planar chart's (the lens and A are the chart's); the tool
// tells the difference (docs/design/rig-profile-v2.md, notes of section 7).
inline constexpr std::string_view kToolId="a0-pair-reanchor";
inline constexpr std::string_view kToolVersion="1.0.0";

// Where the seam goes in the new revision (the owner's decision Q1; both are supported).
//  band_center    the middle of the new overlap band of the new profile (origin band-center-provisional). The default.
//  keep_previous  the column of the active revision, the same place on the drawing (origin operator).
enum class SeamPlacement {band_center,keep_previous};
// How big the composite's raster is (the owner's decision Q2).
//  follow_camera_b  the left, top, bottom and DPI of the active revision; the right edge moves by the amount that the right edge
//                   of B's cover (the smallest over the rows) moved. The default.
//  keep             the active revision's raster.
enum class RasterPlacement {follow_camera_b,keep};

// Why a proposal is not ready. More than one can be set.
enum class Refusal {
    no_overlap,              // R1 found no overlap, or the new profile has no overlap band
    estimate_failed,         // the estimator refused its settings or threw
    estimate_not_verified,   // R2 is not `verified` (the reasons are in the estimate)
    tilt_exceeded,           // |(rx, ry)| of the estimate is over registration.envelope.tiltDeg: the camera was turned, not moved
    height_exceeded,         // |tz| of the estimate is over registration.envelope.heightPercent: the height, focus or zoom changed
    profile_invalid,         // the new profile or the new record does not pass the readers
    raster_too_large,        // the new raster is over the render limits
    raster_outside_sheet,    // the new raster goes beyond the sheet that the profile describes
    creation_check_failed,   // the creation check of fixed_composite_creation_v3.hpp refused (the sentence is in `detail`)
    reestimate_not_verified, // the estimator on the new profile is not `verified`
    reestimate_displacement, // the estimator on the new profile still moves the seam by more than registration.limits.convergencePx
    stopped,                 // the caller's stop was asked for
};
[[nodiscard]] std::string_view RefusalCode(Refusal refusal);

// Thrown by the pieces that make part of a proposal (ProposeRaster, ProposeFromPose's helpers); the reason is the refusal that
// the proposal gets. A std::invalid_argument, so a caller that does not tell the reasons apart treats it as any other bad input.
class RecalibrationRefused : public std::invalid_argument {
public:
    RecalibrationRefused(Refusal reason,const std::string& what):std::invalid_argument(what),reason_(reason){}
    [[nodiscard]] Refusal GetReason() const noexcept {return reason_;}
private:
    Refusal reason_;
};

enum class Stage {wide_search,fine_fit,bake,check,verify};
[[nodiscard]] std::string_view StageCode(Stage stage);

struct Inputs {
    // The two originals in the stored pixel order that the profile's mappings are defined on. Needed by R1, R2 and VerifyCandidate;
    // ProposeFromPose and the pieces that take a pose do not read them.
    const render::BgrImage* camera_a{};
    const render::BgrImage* camera_b{};
    // The revision in force, its SHA-256 (the parent of the new revision) and the bytes of the profile file it names. The profile
    // is verified against the revision before anything is estimated.
    FixedCompositeConditionV3 active;
    std::string active_sha256;
    std::string profile_bytes;
    SeamPlacement seam{SeamPlacement::band_center};
    RasterPlacement raster{RasterPlacement::follow_camera_b};
    // The design constants of the strain bound of layer 2, as MakeFixedCompositeCreationCheckV3 takes them (required when the
    // condition enables layer 2).
    std::optional<LayerTwoStrainDesignV3> layer_two;
    // The pair: its identity (pair-...), the SHA-256 of the two originals, and the time of the shot (UTC, YYYY-MM-DDTHH:MM:SSZ).
    std::string pair_id,original_a_sha256,original_b_sha256,measured_at_utc;
    FixedCompositeSha256 hash;
    // The estimator's design constants (the capture path passes content_registration::kRegistrationDesignDraft) and the threads of
    // its measurer (1..256; the result does not depend on it).
    content_registration::RegistrationDesign design{};
    unsigned thread_count{1};
    // Asked often by the estimator and between the stages; true ends the work (Refusal::stopped).
    content_registration::RegistrationControl control;
    // The time of the new revision (the caller's clock).
    std::chrono::system_clock::time_point now{};
    // Called when a stage begins (the screen shows its name and the seconds since the call).
    std::function<void(Stage)> progress;
};

// R1. `determined` false: no start. `reason` is the overlap estimator's reason code (no-tiles, insufficient-tiles, outside-image,
// tied-starts) or `no-correspondence` / `fit-failed` / `prior-rejected`.
struct WideStart {
    bool determined{};
    std::string reason;
    unsigned tiles_agreed{};
    unsigned correspondences{};      // tiles whose two points both have a document point
    // p0: only tx, ty and rz are set. Against the active revision's profile (pose 0), like the estimator's pose.
    content_registration::PoseParameters pose;
    // The fit of the document points of B to those of A, mm.
    double residual_rms_mm{},residual_max_mm{};
};
// Throws std::invalid_argument for a programming error (missing images, different sizes, a profile the revision does not name).
[[nodiscard]] WideStart EstimateWideStart(const Inputs& inputs);

// What a pose estimate fitted, in the pixels of camera B (the calibration's fitResidualPixels): the residual of each point that
// the fit kept, carried from output pixels to camera B's pixels by the local scale of the new mapping. A measured value, never a
// line.
struct ProfileFit {
    double rms_px{},max_px{};
    unsigned points{};
};
[[nodiscard]] ProfileFit FitInCameraPixels(const content_registration::RegistrationResult& estimate,const render::OutputRaster& raster,
    const render::CameraProjection& camera_b_after);

// What the new profile's calibration block records.
struct Provenance {
    std::string provenance_id;         // identifier (the record of the estimate is what it points at)
    std::string measured_at_utc;
    std::string input_sha256_camera_b; // the pair's original of camera B
    ProfileFit fit;                    // camera B's pixels
};
// R3. The new draft profile as text. Every value that is not named below is the parent's, byte for byte (A's whole block, the lens
// of B, the conventions, the sheet): camera B's documentToImage is PoseModel(B).CameraHomography(p_hat); status stays draft and
// profileId is null (a changed profile is a new profile, and the parent's approval is not carried); calibration.method and chartId
// are the parent's, toolId is kToolId, inputSha256 and fitResidualPixels of CAM-B are the pair's, those of CAM-A the parent's, and
// baselineMm, overlapMm and cameraToDocumentMm of CAM-B are null (they were measured by hand and B moved). Throws
// std::invalid_argument when the parent is not a calibrated draft or the result does not pass RigProfileV2::Parse.
[[nodiscard]] std::string ReanchoredProfileText(std::string_view parent_profile_bytes,const content_registration::PoseParameters& p_hat,
    const Provenance& provenance);

// The new output raster: the active revision's, with the right edge moved by the amount that B's cover moved (follow_camera_b), or
// the same (keep). `parent` is the active revision's binding. Throws RecalibrationRefused (raster_too_large) when the result is
// over the render limits.
[[nodiscard]] render::OutputRaster ProposeRaster(const FixedCompositeConditionV3& active,const FixedCompositeProfileBinding& parent,
    const content_registration::PoseParameters& p_hat,RasterPlacement placement);

// A candidate revision that passed the creation check, with the new profile it names. Nothing here is written anywhere.
struct Candidate {
    std::string profile_bytes,profile_name;
    FixedCompositeConditionV3 condition;
    std::string text,sha256;                    // the revision file as it would be written (Format + newline) and its SHA-256
    FixedCompositeProfileBinding binding;       // the new profile, bound to the candidate
    CreationCheckDetailsV3 check;
};

// What the screen shows (the drawing's frame; sizes in mm and degrees). Filled as far as it can be, also for a refusal.
struct Display {
    // B's move against the active revision's placement (p-hat - the active base pose), as the drawing shows it (right, left, up, down).
    bool has_motion{};
    OutputFrameDisplay motion;
    content_registration::PoseDisplay pose;
    // The narrowest overlap of A and B over the rows of the new raster (mm), and where.
    bool has_overlap{};
    double narrowest_overlap_mm{};
    std::uint32_t narrowest_overlap_row{};
    // The seam: column and place on the drawing (the middle of the column, document x in mm), before and after.
    std::uint32_t seam_column_previous{},seam_column_new{};
    double seam_x_previous_mm{},seam_x_new_mm{};
    // The raster, before and after: width and height in pixels, and the region's left and right edge in mm.
    // `has_new` is true when the whole "after" side (seam, raster) is filled. A refusal can leave it half filled (the width and the right
    // edge are known before the sheet check); the "after" values must not be shown then.
    bool has_new{};
    std::uint32_t width_previous_px{},height_previous_px{},width_new_px{},height_new_px{};
    double right_previous_mm{},right_new_mm{};
    // How near the seam is to the edges of the photographs (the nearest over the rows), in camera pixels and as a share of the
    // width: A's right edge and B's left edge. A seam near an edge is where the calibrated lens model is least known.
    bool has_edge_distance{};
    double edge_distance_a_px{},edge_distance_b_px{};
    double edge_share_a{},edge_share_b{};
};

// The estimator on the new profile (the capture path's settings from the candidate), and where it leaves the seam.
struct Verification {
    bool performed{};
    content_registration::RegistrationState state{content_registration::RegistrationState::undetermined};
    std::vector<std::string> reasons;
    // The counted points of the last fit and the inliers among them (the estimate's diagnostics). What
    // content_registration::AssessEvidenceShortage needs, with the reasons, to tell "too few marks" from "does not fit". 0 when no fit ran.
    std::uint32_t inliers{},points{};
    content_registration::PoseParameters pose;       // p-hat' (pose 0 is a perfect profile)
    content_registration::SurfacePlane surface;
    // The largest displacement over the rows of the seam column that the estimate (pose and, when estimated, the paper plane)
    // would apply on top of the new profile, and the same for the pose alone. Output pixels.
    double seam_displacement_px{},seam_displacement_pose_only_px{};
    double limit_px{};                               // registration.limits.convergencePx of the candidate
    bool passed{};
};

struct Proposal {
    bool ready{};                           // every check passed; only then may anything be written
    std::vector<Refusal> refusals;
    std::string detail;                     // English, for the log; no path
    WideStart wide;
    content_registration::RegistrationResult estimate;   // R2
    bool has_pose{};
    content_registration::PoseParameters pose;           // p-hat of R2 against the active revision's profile
    ProfileFit fit;
    std::string estimate_record_text,estimate_record_sha256;   // a0.pair-recalibration-estimate.v1; the revision's reportSha256
    std::optional<Candidate> candidate;     // set only when the creation check passed
    Verification verification;
    Display display;
};

// R1 to R4 and the verification. Reads no file and writes none; the images are not modified. Throws std::invalid_argument for a
// programming error (missing images or hash, an invalid pair identity, a profile that the active revision does not name) and
// passes std::bad_alloc on; every other failure is a refusal in the proposal (`ready` false). `stop` of the control ends the work
// early with Refusal::stopped.
[[nodiscard]] Proposal ProposeRecalibration(const Inputs& inputs);

// R3 and R4 for a pose that is already known (the pieces after the estimate; no images, no estimator): the new profile from
// p_hat, the raster and the seam, the candidate revision and the creation check, and the Display. `estimate_record_text` is the
// record that the revision's reportSha256 is the hash of. On a refusal the proposal has the reason and the Display as far as it was
// made, and no candidate.
[[nodiscard]] Proposal ProposeFromPose(const Inputs& inputs,const content_registration::PoseParameters& p_hat,const ProfileFit& fit,
    std::string estimate_record_text);

// The estimator on the candidate's profile the way a capture runs it (six degrees, or eight with the condition's paper range, base
// pose 0, the condition's ranges, limits, geometry and the caller's design), and the displacement it would still apply at the seam.
[[nodiscard]] Verification VerifyCandidate(const Inputs& inputs,const Candidate& candidate);

// R5, the thin step: the candidate as the in-memory selection that the pair composite takes (the same value
// SelectFixedComposite returns for a revision read from the settings folder), so that a sample can be composed before anything is
// written.
[[nodiscard]] FixedCompositeSelection SelectionOfCandidate(const Candidate& candidate);
}
