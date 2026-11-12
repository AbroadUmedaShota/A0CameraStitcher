#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/fast_seam_measurer.hpp"
#include "a0/m2/experimental/fixed_composite_profile_common.hpp"
#include "a0/m2/experimental/parallax_correction.hpp"
#include "a0/m2/experimental/registration_gate.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/experimental/seam_blend.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace a0::m2::experimental {
// What one capture decides about camera B's registration, and what the report v8 records of it (#417,
// docs/design/fixed-composite-registration.md 11.7, 11.9, 11.17, 11.18, 11.22, 11.25; the field names and units are in
// docs/design/fixed-composite-registration-formats.md). Camera, file and clock free. This header declares shapes and the
// entry points only; the lane that owns capture_registration.cpp writes the bodies.
//
// The values here do not depend on the condition file's type: the lines that were used are copied into
// RegistrationLines, so the report and its reader need no condition header.

// ---- Geometry levels (11.18) ----
// G1 pose, surface and layer 2; G2 pose and surface; G3 a uniform shift (surface height and the orthogonal translation) of
// the coarse stage; G4 the fixed composite condition's mapping (the base pose, no surface, no layer 2). A composite is made at
// every level.
enum class GeometryLevel {g1,g2,g3,g4};
[[nodiscard]] constexpr std::string_view GeometryLevelCode(GeometryLevel level){
    switch(level){
    case GeometryLevel::g1:return "G1";
    case GeometryLevel::g2:return "G2";
    case GeometryLevel::g3:return "G3";
    case GeometryLevel::g4:return "G4";
    }
    return {};
}
[[nodiscard]] constexpr std::optional<GeometryLevel> ParseGeometryLevel(std::string_view text){
    for(const auto l:{GeometryLevel::g1,GeometryLevel::g2,GeometryLevel::g3,GeometryLevel::g4})if(GeometryLevelCode(l)==text)return l;
    return std::nullopt;
}

// What the coarse stage found when the fit is not trusted and the shift is unique (the G3 candidate, 11.18): the paper height
// h0 that explains the shift along the baseline, and B's translation (B's camera frame, mm, added to p_b) that explains the
// shift across it. The estimator turns the coarse stage's output-pixel shift into these; the level maps them to
// {p_b + (0, 0, 0, tx, ty, 0), (h0, 0, 0), no layer 2}.
struct UniformShift {
    double surface_height_mm{};
    double translation_x_mm{};
    double translation_y_mm{};
};

// The cheap check of 11.23.4 for one candidate mapping: the seam (with half the blend width) lies inside the overlap on every
// row, and every pixel of the output is covered by A or B.
struct MappingCoverCheck {
    bool seam_covered{};
    bool output_covered{};
};
using MappingCoverCheckFunction=std::function<MappingCoverCheck(const RegisteredMapping&)>;

struct RegistrationLines;
struct GeometryLevelInput {
    const content_registration::RegistrationResult* registration{};   // the pose estimate (required)
    const SurfaceEstimate* surface{};                                  // required
    const parallax_correction::ParallaxResult* parallax{};             // null: layer 2 was not tried or is not allowed
    std::optional<UniformShift> coarse_shift;                          // the G3 candidate, when the coarse stage gave one
    content_registration::PoseParameters base_pose;                    // p_b: the pose of G4
    const RegistrationLines* lines{};                                  // the condition's ranges and lines (required; apply_beyond_envelope set)
    std::uint32_t seam_column{};                                       // the condition's seam column (layer 2's w is centred on it)
    MappingCoverCheckFunction cover_check;                             // required
};
struct GeometryLevelDecision {
    GeometryLevel level{GeometryLevel::g4};
    // Why the level is below the best one that was tried, in kebab-case codes. Known: seam-outside-overlap-at-level,
    // output-uncovered-at-level (11.18); a new code is added with its writer, reader, text and test in one commit.
    std::vector<std::string> reasons;
    struct Applied {bool pose{},surface{},parallax{};} applied;
    bool beyond_envelope_applied{};
    RegisteredMapping mapping;     // the mapping of the chosen level
};
// Thrown by SelectGeometryLevel when even G4 (the condition's own mapping) fails the cheap check: the condition is broken and no
// composite is made (11.18). SeamCovered() is false when the seam left the overlap, true when an output pixel is not covered.
class GeometryLevelUnavailable : public std::runtime_error {
public:
    GeometryLevelUnavailable(bool seam_covered,const std::string& detail):std::runtime_error(detail),seam_covered_(seam_covered){}
    [[nodiscard]] bool SeamCovered() const noexcept {return seam_covered_;}
private:
    bool seam_covered_;
};
// The one function that chooses the level (11.18). Pure. Candidates from the top: G1 {p-hat, pi-hat, q}, G2 {p-hat, pi-hat},
// G3 {p_b + the coarse translation, (h0, 0, 0)}, G4 {p_b}. An estimate is a candidate when it is trusted (verified or
// out_of_envelope, with an estimate) and, when a component is beyond its range (measured from p_b), apply_beyond_envelope is
// true; layer 2 beyond maxLiftMm likewise. G3 only when the estimate is not trusted. A candidate whose mapping fails
// cover_check (or cannot be built: the check throws std::invalid_argument) is passed over with the reason
// seam-outside-overlap-at-level or output-uncovered-at-level. Throws std::invalid_argument for a missing required input and
// GeometryLevelUnavailable when G4 fails the check.
[[nodiscard]] GeometryLevelDecision SelectGeometryLevel(const GeometryLevelInput& input);

// The applied flags as the report records them: pose for G1 to G3, surface when the mapping's plane is not zero, parallax when
// the mapping has a profile. The reader derives them again from the level and the mapping.
[[nodiscard]] GeometryLevelDecision::Applied AppliedOfLevel(GeometryLevel level,const RegisteredMapping& mapping);

// Layer 2's result as the mapping's profile (registration_mapping.hpp): q at the rows (0 where a row is not applied), the
// baseline, the row spacing and w's a and L from the design. nullopt unless the result is applied, has rows, and its rows are
// evenly spaced by design.row_spacing_px. Pure.
[[nodiscard]] std::optional<ParallaxProfile> ParallaxProfileOf(const parallax_correction::ParallaxResult& parallax,std::uint32_t seam_column);

// ---- The lines a report was judged against (11.17.3, 11.20) ----
// Copied from the condition at capture time and recorded in the report, so a reader derives the warnings again from the
// report alone. Nothing here has a default in code.
struct RegistrationLines {
    content_registration::RegistrationEnvelope envelope;
    std::optional<SurfaceEnvelope> surface_envelope;           // null: no surface is estimated
    std::optional<bool> apply_beyond_envelope;
    std::optional<double> surface_scale_warn_percent;          // the condition carries the field; the owner's value is null (no scale warning)
    std::optional<double> max_lift_mm;                         // null: no paper-lift line, layer 2 is not tried
    double seam_limit_px=std::numeric_limits<double>::quiet_NaN();        // seamGate.limitPx: L
    double seam_guard_band_px=std::numeric_limits<double>::quiet_NaN();   // seamGate.guardBandPx: U
};

// ---- Warnings (11.7, 11.17.2) ----
enum class WarningClass {exceeded,unverified,notice};
[[nodiscard]] constexpr std::string_view WarningClassCode(WarningClass c){
    return c==WarningClass::exceeded?"exceeded":c==WarningClass::unverified?"unverified":c==WarningClass::notice?"notice":std::string_view{};
}

// The 21 codes of 11.17.2 without surface-scale (the owner's decision 10: no warning about the scale of the drawing). A
// code is added with its writer, reader, text and test in one commit.
enum class WarningCode {
    camera_position,camera_rotation,camera_tilt,camera_height,
    surface_height,surface_tilt,
    output_edge_uncovered,
    seam_lift,seam_lift_limit,fold_near_seam,gutter_in_band,
    seam_displacement,seam_dx_unmeasured,seam_unmeasurable,
    placement_unverified,placement_mismatch,
    gate_configuration,gate_not_run,seam_wide_search,
    geometry_level,time_budget,
};
inline constexpr std::array<std::pair<WarningCode,std::string_view>,21> kWarningCodeTexts{{
    {WarningCode::camera_position,"camera-position"},{WarningCode::camera_rotation,"camera-rotation"},
    {WarningCode::camera_tilt,"camera-tilt"},{WarningCode::camera_height,"camera-height"},
    {WarningCode::surface_height,"surface-height"},{WarningCode::surface_tilt,"surface-tilt"},
    {WarningCode::output_edge_uncovered,"output-edge-uncovered"},
    {WarningCode::seam_lift,"seam-lift"},{WarningCode::seam_lift_limit,"seam-lift-limit"},
    {WarningCode::fold_near_seam,"fold-near-seam"},{WarningCode::gutter_in_band,"gutter-in-band"},
    {WarningCode::seam_displacement,"seam-displacement"},{WarningCode::seam_dx_unmeasured,"seam-dx-unmeasured"},
    {WarningCode::seam_unmeasurable,"seam-unmeasurable"},
    {WarningCode::placement_unverified,"placement-unverified"},{WarningCode::placement_mismatch,"placement-mismatch"},
    {WarningCode::gate_configuration,"gate-configuration"},{WarningCode::gate_not_run,"gate-not-run"},
    {WarningCode::seam_wide_search,"seam-wide-search"},
    {WarningCode::geometry_level,"geometry-level"},{WarningCode::time_budget,"time-budget"},
}};
[[nodiscard]] constexpr std::string_view WarningCodeText(WarningCode code){
    for(const auto& e:kWarningCodeTexts)if(e.first==code)return e.second;
    return {};
}
[[nodiscard]] constexpr std::optional<WarningCode> ParseWarningCode(std::string_view text){
    for(const auto& e:kWarningCodeTexts)if(e.second==text)return e.first;
    return std::nullopt;
}

// The component a warning is about (11.17.2: camera-position etc.).
enum class WarningComponent {
    camera_position,camera_rotation,camera_tilt,camera_height,
    surface_height,surface_tilt,output_edge,
    seam_lift,fold,gutter,seam,placement,gate,geometry_level,time_budget,
};
inline constexpr std::array<std::pair<WarningComponent,std::string_view>,15> kWarningComponentTexts{{
    {WarningComponent::camera_position,"camera-position"},{WarningComponent::camera_rotation,"camera-rotation"},
    {WarningComponent::camera_tilt,"camera-tilt"},{WarningComponent::camera_height,"camera-height"},
    {WarningComponent::surface_height,"surface-height"},{WarningComponent::surface_tilt,"surface-tilt"},
    {WarningComponent::output_edge,"output-edge"},{WarningComponent::seam_lift,"seam-lift"},
    {WarningComponent::fold,"fold"},{WarningComponent::gutter,"gutter"},{WarningComponent::seam,"seam"},
    {WarningComponent::placement,"placement"},{WarningComponent::gate,"gate"},
    {WarningComponent::geometry_level,"geometry-level"},{WarningComponent::time_budget,"time-budget"},
}};
[[nodiscard]] constexpr std::string_view WarningComponentText(WarningComponent c){
    for(const auto& e:kWarningComponentTexts)if(e.first==c)return e.second;
    return {};
}
[[nodiscard]] constexpr std::optional<WarningComponent> ParseWarningComponent(std::string_view text){
    for(const auto& e:kWarningComponentTexts)if(e.second==text)return e.first;
    return std::nullopt;
}

enum class WarningUnit {px,mm,deg,percent,rows,pixels};
inline constexpr std::array<std::pair<WarningUnit,std::string_view>,6> kWarningUnitTexts{{
    {WarningUnit::px,"px"},{WarningUnit::mm,"mm"},{WarningUnit::deg,"deg"},{WarningUnit::percent,"percent"},
    {WarningUnit::rows,"rows"},{WarningUnit::pixels,"pixels"},
}};
[[nodiscard]] constexpr std::string_view WarningUnitText(WarningUnit u){
    for(const auto& e:kWarningUnitTexts)if(e.first==u)return e.second;
    return {};
}
[[nodiscard]] constexpr std::optional<WarningUnit> ParseWarningUnit(std::string_view text){
    for(const auto& e:kWarningUnitTexts)if(e.second==text)return e.first;
    return std::nullopt;
}

// The codes of how to put it right (11.17.2). Nothing about pressing, flattening, weights or boards (decision D-d; invariant 17).
enum class Remedy {
    shift_drawing_horizontal,place_drawing_again,check_camera_mount,check_camera_mount_or_focus,retake,
    retake_then_recalibrate,review_visually,contact_technician,none,
};
inline constexpr std::array<std::pair<Remedy,std::string_view>,9> kRemedyTexts{{
    {Remedy::shift_drawing_horizontal,"shift-drawing-horizontal"},{Remedy::place_drawing_again,"place-drawing-again"},
    {Remedy::check_camera_mount,"check-camera-mount"},{Remedy::check_camera_mount_or_focus,"check-camera-mount-or-focus"},
    {Remedy::retake,"retake"},{Remedy::retake_then_recalibrate,"retake-then-recalibrate"},
    {Remedy::review_visually,"review-visually"},{Remedy::contact_technician,"contact-technician"},{Remedy::none,"none"},
}};
[[nodiscard]] constexpr std::string_view RemedyText(Remedy r){
    for(const auto& e:kRemedyTexts)if(e.first==r)return e.second;
    return {};
}
[[nodiscard]] constexpr std::optional<Remedy> ParseRemedy(std::string_view text){
    for(const auto& e:kRemedyTexts)if(e.second==text)return e.first;
    return std::nullopt;
}

enum class WarningSource {condition,design};
[[nodiscard]] constexpr std::string_view WarningSourceText(WarningSource s){return s==WarningSource::condition?"condition":s==WarningSource::design?"design":std::string_view{};}

struct RowSpan {double first{},last{};};
struct RegistrationWarning {
    WarningCode code{WarningCode::geometry_level};
    WarningClass warning_class{WarningClass::notice};
    WarningComponent component{WarningComponent::geometry_level};
    double value{};                          // not rounded
    std::optional<double> limit;             // a notice may have none
    WarningUnit unit{WarningUnit::px};
    std::optional<RowSpan> rows_px;          // output rows
    std::optional<RowSpan> rows_mm;          // mm from the top of the seam
    std::vector<std::string> interpretations;   // reading codes, e.g. surface-raised, camera-moved-along-baseline
    Remedy remedy{Remedy::none};
    std::optional<double> remedy_amount_mm;  // only for shift_drawing_horizontal: the N of "shift by about N cm"
    bool applied{};                          // the amount was put into the composite
    WarningSource source{WarningSource::condition};
};

// ---- Assessment (11.17.1) ----
enum class AssessmentState {within_criteria,outside_criteria,not_judged};
[[nodiscard]] constexpr std::string_view AssessmentStateText(AssessmentState s){
    return s==AssessmentState::within_criteria?"within-criteria":s==AssessmentState::outside_criteria?"outside-criteria":
        s==AssessmentState::not_judged?"not-judged":std::string_view{};
}
struct RegistrationAssessment {
    AssessmentState state{AssessmentState::not_judged};
    std::vector<WarningCode> exceeded,unverified;
    // qualityAccepted and acceptanceEvidence are always false and are written as such by the report writer.
};

// ---- Time budget (11.25) ----
// The stages in the order they are dropped (11.25.3). Compositing, encoding, writing and the report are never dropped.
enum class ReviewStage {
    wide_search_diagnostic,band_measurement,parallax,fold_refit,fine_and_fit,coarse,gate,
};
inline constexpr std::array<std::pair<ReviewStage,std::string_view>,7> kReviewStageTexts{{
    {ReviewStage::wide_search_diagnostic,"wide-search-diagnostic"},{ReviewStage::band_measurement,"band-measurement"},
    {ReviewStage::parallax,"parallax"},{ReviewStage::fold_refit,"fold-refit"},{ReviewStage::fine_and_fit,"fine-and-fit"},
    {ReviewStage::coarse,"coarse"},{ReviewStage::gate,"gate"},
}};
[[nodiscard]] constexpr std::string_view ReviewStageText(ReviewStage s){
    for(const auto& e:kReviewStageTexts)if(e.first==s)return e.second;
    return {};
}
[[nodiscard]] constexpr std::optional<ReviewStage> ParseReviewStage(std::string_view text){
    for(const auto& e:kReviewStageTexts)if(e.second==text)return e.first;
    return std::nullopt;
}
// The estimate of each stage's cost in milliseconds and the reserve kept for compositing and the report. A design constant
// set from measurement (kReviewStageCostDraft, by the lane that measures); recorded in the report as costModel.
struct ReviewStageCosts {
    std::array<double,7> stage_ms{};      // indexed by ReviewStage
    double tail_reserve_ms=std::numeric_limits<double>::quiet_NaN();
};
struct SkippedStage {ReviewStage stage{};std::string reason;};   // reason: "time-budget", or "stage-failed" (the stage threw)
struct TimeBudgetRecord {
    std::vector<ReviewStage> planned;
    std::vector<SkippedStage> skipped;
    ReviewStageCosts cost_model;
};
// Which stages to run for the remaining time (pure; no clock inside). The tail reserve is always kept; stages are dropped in
// the order of ReviewStage. Throws std::invalid_argument for a negative or non-finite input.
[[nodiscard]] TimeBudgetRecord PlanReviewStages(double remaining_ms,const ReviewStageCosts& costs);

// ---- Where layer 2's points come from (design 11.31) ----
// band: the points of the band measurement (the seam gate's measurer), as before. fast_measurer: the points of the fast measurer
// (fast_seam_measurer.hpp) after its two stops; a capture that does not pass the stops uses the band's points.
enum class LayerTwoPoints {band, fast_measurer};
// What a capture recorded of the choice when the design selected the fast measurer and layer 2 was tried; empty otherwise.
struct LayerTwoPointSource {
    bool fast_measurer_used{};                 // layer 2 was given the fast measurer's points
    std::uint32_t points_used{};               // how many (0 when not used)
    std::optional<double> edge_share{};        // the share of content points that ended at the search edge (empty: not measured)
    double edge_share_line{};                  // the line it was held to (the design's)
    std::optional<std::string> fallback_reason;   // not used: edge-share, no-points or measurer-failed
};

// ---- What the capture decided ----
struct CaptureRegistration {
    content_registration::RegistrationResult registration;   // the pose estimate; components are measured from p_b (RegistrationComponentsFromBase)
    SurfaceEstimate surface;                                  // the paper plane
    parallax_correction::ParallaxResult parallax;            // layer 2 and the folds
    std::optional<LayerTwoPointSource> point_source;         // empty: layer 2 was not given points by the fast measurer's choice (the design's band)
    std::optional<UniformShift> coarse_shift;                // the G3 candidate the level was chosen with (recorded whether or not G3 was used)
    GeometryLevelDecision level;
    registration_gate::RegistrationGateResult gate;
    TimeBudgetRecord time_budget;
    std::string geometry_hash;                                // RegistrationGeometryHash, 64 lower-case hex
};

// The four range components of p-hat - p_b (11.19.2): |(dtx, dty)| mm, |drz| deg, |(drx, dry)| deg, |dtz| percent, each against
// the envelope; exceeded is value > limit (equal is inside). Pure; the writer and the reader use it alike.
[[nodiscard]] std::vector<content_registration::ComponentCheck> RegistrationComponentsFromBase(const content_registration::PoseParameters& pose,
    const content_registration::PoseParameters& base_pose,const content_registration::RegistrationEnvelope& envelope);

// Warnings and the assessment are made by one pure function each, from the recorded quantities only, by the writer and by the
// reader alike (11.17.3, invariant 16). A reader that gets another list than it derived refuses the report. The order is the
// order of WarningCode, and inside one code the order the quantities are recorded in. raster gives the pixel size for the
// millimetre fields.
[[nodiscard]] std::vector<RegistrationWarning> DeriveRegistrationWarnings(const CaptureRegistration& capture,
    const RegistrationLines& lines,const content_registration::PoseParameters& base_pose,const render::OutputRaster& raster);
// exceeded if any warning is exceeded; else not_judged if any is unverified; else within_criteria when the gate passed; any
// other combination is a writer's error and throws std::logic_error.
[[nodiscard]] RegistrationAssessment DeriveRegistrationAssessment(const std::vector<RegistrationWarning>& warnings,
    registration_gate::GateState gate_state);

// ---- Geometry hash (11.22.2) ----
// SHA-256 of the bytes below in this order; numbers are IEEE-754 doubles written as 16 hex digits, not rounded.
struct GeometryHashInput {
    std::string condition_sha256,profile_fingerprint_sha256;
    render::OutputRaster raster{render::DocumentRegionUm{0,0,0,0},0,0,0};
    std::uint32_t seam_column{};
    GeometryLevel level{GeometryLevel::g4};
    content_registration::PoseParameters base_pose,applied_pose;
    SurfacePlane applied_surface;
    std::optional<ParallaxProfile> applied_parallax;   // q samples and w's a and L
    std::uint32_t mixing_width_px{},omega_half_width_px{};
};
[[nodiscard]] std::string RegistrationGeometryHash(const GeometryHashInput& input,const FixedCompositeSha256& hash);

// ---- The report v8's record (docs/design/fixed-composite-registration-formats.md 4) ----
// What a v8 report records about the registered fixed composite it was made with (#417, design 11.9, 11.17, 11.27). The
// mapping is not recorded as a table: it is the profile's with the applied pose, surface and layer-2 samples named in the
// level. Whatever can be derived from these fields is derived again on reading (the components, the applied flags, the
// warnings, the assessment, the geometry hash), and a report that differs from its derivation is refused. Fields that are
// derived are kept as they were read, so that the reader can compare.
struct SavedFixedCompositeRegistration {
    // The condition (v3) the capture was made with, and the profile it names, as v7 does.
    std::string condition_sha256;
    std::uint32_t condition_sequence{};
    std::string profile_file;
    std::string profile_file_sha256,profile_fingerprint_sha256;
    std::optional<std::string> profile_id;
    render::OutputRaster raster{render::DocumentRegionUm{0,0,0,0},0,0,0};
    std::uint32_t seam_column{};
    std::string seam_origin;                // "band-center-provisional" or "operator"
    // W, Omega and the band of this capture (condition registration.geometry and the blend's width): fixedCompositeRegistration.layout.
    content_registration::RegistrationGeometry layout;
    content_registration::PoseParameters base_pose;
    RegistrationLines limits;               // the lines the capture was judged against, copied from the condition
    CaptureRegistration registration;       // the pose estimate, surface, layer 2 and folds, level, gate, time budget, hash
    // The unchanged seam gate's own report object (a0.seam-gate.v1, SeamGateReportJson) embedded as gate.seamGate; empty when the
    // gate did not run. A reader keeps it in the canonical form of the JSON reader (members in name order).
    std::optional<std::string> seam_gate_json;
    std::vector<RegistrationWarning> warnings;   // top level of the report
    RegistrationAssessment assessment;           // top level of the report
};
// The geometry hash input of a record (the applied mapping of its level, the layout and the condition).
[[nodiscard]] GeometryHashInput GeometryHashInputOf(const SavedFixedCompositeRegistration& record);

// The JSON values of the three v8 members the registered composite owns: fixedCompositeRegistration (object), warnings (array),
// assessment (object). No spaces, members in the formats document's order, numbers with 17 significant digits in the classic
// locale, -0 written as 0. The record must be consistent (its derived fields equal to their derivation); the writer checks it
// and throws std::logic_error otherwise, so a report it writes is one the reader accepts.
[[nodiscard]] std::string FixedCompositeRegistrationJson(const SavedFixedCompositeRegistration& record);
[[nodiscard]] std::string RegistrationWarningsJson(const std::vector<RegistrationWarning>& warnings);
[[nodiscard]] std::string RegistrationAssessmentJson(const RegistrationAssessment& assessment);
// Fills the derived fields of a record from its recorded quantities: the components from base, the warnings, the assessment, the
// geometry hash (also in the gate). The capture path calls it once before writing; the reader compares against it.
void DeriveSavedFixedCompositeRegistration(SavedFixedCompositeRegistration& record,const FixedCompositeSha256& hash);
// ---- The capture path (11.9, with 11.25: the composite is drawn before the gate) ----
// One capture on the stored-order originals: the estimate (pose and surface), layer 2, the level and its mapping, the composite
// drawn with that mapping (PrepareCaptureRegistration); then, after the caller has written the JPEG, the gate on Omega with the
// very same pair (MeasureCaptureRegistrationGate). Camera, file and clock free: the caller passes the remaining time as a
// function, and only which stages run may depend on it (invariant 14); the values of a stage that ran do not.
//
// The parts, in order: the estimate of the pose and the paper plane (content_registration, with the condition's surface range and
// base pose; it stops with time-budget when the time left would not hold the gate); the band measured once with B placed by the
// estimate (M0), whose points feed layer 2 (parallax_correction) and whose anchors the gate takes; the level and its mapping (each
// candidate through the caller's cheap check); the composite; later the gate on Omega with the very same pair (registration_gate,
// entry (b)). The fold refit (S5) is not in the build: its stage is planned and does nothing.

// Design constants of the capture path (*proposal*; recorded in the report as gate.design and timeBudget.costModel).
struct CaptureRegistrationDesign {
    seam_gate::SeamGateDesign omega;          // the measurer on Omega: the gate's points
    seam_gate::SeamGateDesign band;           // the measurer on the whole band (anchors and a diagnostic). Its lattice may be coarser;
                                              // its global placement settings must be the Omega design's (the gate checks it)
    registration_gate::GateDesign gate;       // GOmega's constants; the wide search radius is widened to the surface envelope's shift
    parallax_correction::ParallaxDesign parallax;   // layer 2 (kParallaxDesignDraft); the band's lattice and patch are its points'
    ReviewStageCosts costs;                   // CaptureRegistrationDesignDraft's (*estimates* from the measured capture path)
    // The estimator looks at the time only between its outer iterations, so it stops when the time left would not hold the stages
    // after it, the tail and this much more (about one outer iteration), ms. Not in the report (the time plan's record is the costs).
    double estimator_stop_margin_ms{};
    // The split of the gate rows (11.4, 11.5): row i of layer 2 and of Omega's lattice is a verify row when i % verify_period_rows
    // == verify_phase (parallax_correction::ParallaxRows). Layer 2 leaves out every point that shares a pixel with a verify row, and
    // the gate decides on the verify rows only, measuring the correct rows apart for reference (registration_gate::SplitGateOmega).
    // 0: no split. Recorded in the report as parallax.split.
    std::uint32_t verify_period_rows{};
    std::uint32_t verify_phase{};
    // The band of the band-measurement stage, the seam column +- this (RegistrationGateRequest::band_measure_half_width_px): not the
    // estimator's band (the condition's bandHalfWidthPx), which it does not change. Layer 2 reads the points within its column
    // radius of the seam, so the band has to hold the radius and the point patch (CaptureBandReachesLayerTwo); a wider band only
    // measures points nothing reads. The gate's own band, measured when this stage did not (the level is G3 or G4, or the time plan
    // dropped it), stays the estimator's: its anchors are then the only evidence of the placement.
    std::uint32_t band_measure_half_width_px{};
    // The anchors the band-measurement stage evaluates at most (the first ones of the measurer's spread order). The stage's band is
    // kept for the gate only when the level places B by the estimate (G1, G2), where the estimate confirms the placement and an
    // anchor can only contradict it, so a few anchors are enough and the rest cost most of the stage (each searches a window of
    // global_search_radius_px). The anchors stay a check against a placement one period of a repeating pattern off: a contradicting
    // anchor still fails the gate. With at least the band design's global_minimum_anchors, "verified by the anchors" stays possible.
    // The stage's points do not depend on it. 0: the band design's own maximum. The band that the gate measures itself (G3, G4, or
    // when this stage did not run) always evaluates the band design's maximum.
    std::uint32_t band_stage_anchors{};
    // Where layer 2's points come from (design 11.31). The default is the band measurement's points, so a capture is what it was
    // before the fast measurer existed. fast_measurer: the band is still measured (the gate's anchors need it), and layer 2 is given
    // the fast measurer's points when they pass its stops, else the band's. fast_measurer holds the measurer's constants.
    LayerTwoPoints layer_two_points{LayerTwoPoints::band};
    fast_seam_measurer::Design fast_measurer{fast_seam_measurer::kDesignDraft};
};
// Whether the design's band holds layer 2's points (parallax.column_radius_px rounded up, and the point patch) and Omega's half width.
// A band_measure_half_width_px of 0 leaves the band to the estimator's, as before, and always holds.
[[nodiscard]] bool CaptureBandReachesLayerTwo(const CaptureRegistrationDesign& design, std::uint32_t omega_half_width_px);
// The first values (*proposal*): kSeamGateDesignCalibrated on Omega and, on the band, the same with the point spacing doubled
// (a quarter of the points; *to be measured*); GOmega's constants as its own tests use them; the stage costs of the formats
// document's order, in milliseconds (*estimates*, rounded up from the capture path on a loaded PC); one verify row in six, from
// the first (the owner's decision; the shortest period that leaves seam column points between two verify rows is five).
[[nodiscard]] CaptureRegistrationDesign CaptureRegistrationDesignDraft();

struct CaptureRegistrationRequest {
    const render::BgrImage* camera_a{};       // the stored pixel order the profile's mapping is defined on
    const render::BgrImage* camera_b{};
    const render::DocumentRenderParameters* parameters{};   // the profile's cameras and the condition's raster (A left, B right); required
    // The estimator's settings from the condition: geometry (seam column, the blend's width as the mixing width, Omega's margin,
    // the guard, the band half width), envelope, limits, design.
    content_registration::RegistrationSettings estimator;
    content_registration::PoseParameters base_pose;
    RegistrationLines lines;
    SeamBlendSettings blend;
    CaptureRegistrationDesign design;
    MappingCoverCheckFunction cover_check;    // the cheap check of a candidate mapping (the caller's: it needs the condition library)
    std::function<double()> remaining_ms;     // milliseconds left before the deadline; empty: no deadline, every stage runs
    unsigned thread_count{1};                 // 1..256; the result does not depend on it
};
// How long the stages took, from remaining_ms (empty without it). For the log only: never in the report.
struct CaptureStageMilliseconds {
    std::array<std::optional<double>,7> stage{};    // indexed by ReviewStage
    std::optional<double> composite;
};
struct PreparedCaptureRegistration {
    CaptureRegistration capture;              // the gate not yet measured (not_run); no hash or warnings yet
    MappedFixedSeamInput mapped;              // the pair of the chosen level: the composite's and the gate's
    SeamBlendResult composite;
    render::Point baseline{};                 // e_b in the output frame, from the calibrated camera centres
    double baseline_px_per_mm{};              // k: output px along e_b per mm of paper height at the seam's middle
    CaptureStageMilliseconds milliseconds;
    // The band measured once before layer 2 (B placed by the pose and the plane): layer 2's points and the gate's anchors. Kept for
    // the gate only when the level placed B by the estimate (G1, G2); the gate takes it.
    std::optional<seam_gate::SeamMeasurement> band;
    std::optional<registration_gate::RegistrationGateLayout> layout_before_layer_two;
};
// A stage that throws (the estimate with its settings; layer 2 with the mapping M0 and the band under it) is dropped with the
// reason stage-failed and the capture goes on: the estimate gives G4 (registration.reasons estimate-failed), layer 2 gives G2 at
// most (parallax.reasons layer-two-failed); std::bad_alloc is passed on. Throws std::invalid_argument for a programming error
// (sizes, missing inputs), GeometryLevelUnavailable when not even G4 can be drawn, and what RenderMappedFixedSeam throws.
[[nodiscard]] PreparedCaptureRegistration PrepareCaptureRegistration(const CaptureRegistrationRequest& request);
// The gate on Omega with prepared.mapped (entry (b) of registration_gate.hpp: Omega and, when the time allows, the band measured
// here, then decided). Fills prepared.capture.gate and the time budget; the gate is not_run when the time does not allow it. When
// layer 2 is applied, Omega is first aligned to layer 2's rows (AlignGateOmegaToRows) and, when layer 2 split its rows, split the
// same way (registration_gate::SplitGateOmega): the verify rows decide, the correct rows are measured for reference in the same
// stage. A gate that throws leaves the gate not_run
// with the reason stage-failed (the wide search, and a band not measured before layer 2, with it); std::bad_alloc is passed on.
void MeasureCaptureRegistrationGate(const CaptureRegistrationRequest& request,PreparedCaptureRegistration& prepared);

// Layer 2's holes are built around the gate rows of the layout before layer 2 (parallax_correction::ParallaxRows: row i at
// first_row + i * row_step), and the measurer puts its rows at the first row of Omega's band + spacing / 2, then every spacing.
// The final layout, made with layer 2 in B's mapping, can begin at another row where layer 2 moved B's coverage near the top or
// the bottom edge, and the gate's rows would then leave layer 2's. This makes them the same: the rows of the final Omega before
// the first of layer 2's lattice rows it covers, and from layer 2's last row on, are left out of the measurement (their band and
// transition emptied) and added to outside_measurable (merged, in order). Returns the number of output rows left out. Throws
// std::invalid_argument when row_step is not the spacing or row_count is 0, and std::logic_error if the lattice still differs.
std::uint32_t AlignGateOmegaToRows(registration_gate::RegistrationGateLayout& layout,std::uint32_t first_row,std::uint32_t row_step,
    std::uint32_t row_count,std::uint32_t spacing);

// What names the condition and the profile in the record (the condition v3's own fields; the capture path does not read the
// condition type).
struct RegistrationRecordIdentity {
    std::string condition_sha256;
    std::uint32_t condition_sequence{};
    std::string profile_file,profile_file_sha256,profile_fingerprint_sha256;
    std::optional<std::string> profile_id;
    std::string seam_origin;                  // "band-center-provisional" or "operator"
};
// The report's record of a capture that went through both steps above, with every derived field derived (the warnings, the
// assessment, the geometry hash): what the writer writes. The seam gate's own report is embedded when the gate ran.
[[nodiscard]] SavedFixedCompositeRegistration RecordOfCapture(const RegistrationRecordIdentity& identity,const CaptureRegistrationRequest& request,
    const PreparedCaptureRegistration& prepared,const FixedCompositeSha256& hash);

// The strict reader of a whole v8 report text's three members above (the rest of the report is the caller's). Rejects a
// missing, extra or duplicated member, a value of the wrong kind or out of range, an unknown code, any derived field that
// differs from its derivation (components, applied flags, warnings, assessment, both geometry hashes), and any text that the
// writer would not write for the values read. Every failure is std::invalid_argument.
[[nodiscard]] SavedFixedCompositeRegistration ReadFixedCompositeRegistrationReport(std::string_view report_text,const FixedCompositeSha256& hash);
}
