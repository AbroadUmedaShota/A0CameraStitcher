#pragma once
#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include "a0/m2/seam_gate.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a0::m2::experimental::registration_gate {
// The gate for the registered composite (#417, docs/design/fixed-composite-registration.md 11.5, 11.6, 11.22 "GOmega").
// It calls seam_gate::MeasureSeam and seam_gate::DecideSeamGate unchanged, on Omega only, with the same mapping pair the
// composite was drawn from, and adds what the gate itself does not say: which rows measured the baseline direction, the
// maxima split by kind of point, where the anchors came from, a diagnostic wide search, the quantities before and after layer
// 2, and the cause of an excess. seam_gate.hpp, seam_gate.cpp and seam_gate_decision.cpp are not changed (the owner's
// decision; their change is #394's). Pure computation: no camera, file or clock; no random choice; the same inputs give the
// same result for any thread count.
//
// The decision reads the points of Omega only (#404). The whole band is measured separately: its anchors are the ones the
// decision uses for the global placement check (11.6 item 3), and its values are returned as a diagnostic that never enters
// the decision.
//
// What was measured decides; what could not be measured is reported apart (11.30). A value over the line and a contradicted
// placement fail as the seam gate says. The seam gate's reasons about what it could not measure (unmeasurable-content,
// no-measurable-block, unpredicted-empty-block, global-placement-unverified) are kept in `findings` as they are, and decided
// again from the rows of Omega (GateRows) and the placement's basis (PlacementBasis): see GateFinding for the reasons.

// seam_gate::SeamGateState has no "not run"; the report's gate state has (11.25: the time budget did not allow the gate).
enum class GateState {pass, fail, undetermined, not_run};
[[nodiscard]] constexpr std::string_view GateStateCode(GateState state){
    return state==GateState::pass?"pass":state==GateState::fail?"fail":state==GateState::undetermined?"undetermined":
        state==GateState::not_run?"not-run":std::string_view{};
}
[[nodiscard]] constexpr std::optional<GateState> ParseGateState(std::string_view text){
    for(const auto s:{GateState::pass,GateState::fail,GateState::undetermined,GateState::not_run})if(GateStateCode(s)==text)return s;
    return std::nullopt;
}

// Why the gate failed, when it did (11.22.3). The three states of the seam gate stay as they are; the cause is this part's.
enum class GateCause {
    none,          // no excess
    lift,          // the excess is a paper that stands up (the baseline component dominates and the sign agrees)
    pose,          // the rest is explained by a pose or a similarity
    displacement,  // an excess that is neither
    unmeasurable,  // the failure is that something could not be measured
};
[[nodiscard]] constexpr std::string_view GateCauseCode(GateCause cause){
    switch(cause){
    case GateCause::none:return "none";
    case GateCause::lift:return "lift";
    case GateCause::pose:return "pose";
    case GateCause::displacement:return "displacement";
    case GateCause::unmeasurable:return "unmeasurable";
    }
    return {};
}
[[nodiscard]] constexpr std::optional<GateCause> ParseGateCause(std::string_view text){
    for(const auto c:{GateCause::none,GateCause::lift,GateCause::pose,GateCause::displacement,GateCause::unmeasurable})if(GateCauseCode(c)==text)return c;
    return std::nullopt;
}
// The remedy code of the warning seam-displacement for a cause (addendum 11.17.2 and 11.22.3). Only codes of the remedy set
// of 11.17.2: never one that presses or flattens the paper (D-d). "none" for none.
[[nodiscard]] constexpr std::string_view GateCauseRemedy(GateCause cause){
    switch(cause){
    case GateCause::lift:return "shift-drawing-horizontal";
    case GateCause::pose:return "retake-then-recalibrate";
    case GateCause::displacement:return "retake";
    case GateCause::unmeasurable:return "shift-drawing-horizontal";
    case GateCause::none:return "none";
    }
    return {};
}

struct RowRange {std::uint32_t first{},last{};};   // output rows, inclusive

// The rows of Omega by what could be seen and measured in them (11.6 item 1, 11.30). A row is one row of the measurement
// lattice (point_spacing_px output rows centred on the lattice row); rows without a point inside the coverage are not counted.
// Every counted row is exactly one of:
//   blank                 no point of the row has content in either camera: no seam can be seen there, and the row is left out
//                         of the decision (it is reported, never a reason)
//   unconfirmed           a point of the row is over the line and the remeasurement left it unconfirmed (#385 Q1)
//   measured              a counted point of the row is accepted (its block is not refused, below)
//   content unmeasured    content, and no accepted counted point
// A counted point is refused with its block when the seam gate doubts the block's points: an unconfirmed point in it (#385
// Q1), or a value that differs from every measured neighbour by more than the paper can bend (neighbor_inconsistent). A block
// the seam gate called unmeasurable because no two of its counted points agree, or because it has one counted point only, is
// not refused: Omega is one lattice column wide, so a block holds four points one above the other, up to 96 px apart, and the
// agreement (a fraction of a pixel) then reads the paper's shape rather than a doubt; every counted point passed the measurer's
// own checks and its value is in the maximum.
// One-sided content: at a point of a row with content unmeasured, the measurer found no peak (not uncertain, not inconsistent
// both ways), one camera has at least twice the seam gate's content count and the other fewer than a quarter of it even at
// half the gradient level (so that a dark area or one camera's lower exposure is not a mark one camera alone shows).
// A measured row measures the baseline component when an accepted counted point is two-directional, or one-directional
// with |n . e_b| >= c_min. A measured row that does not holds lines along the baseline only: the seam's visible displacement
// there is the one the lines show (n . d, which the maximum holds), and the part along the lines cannot be seen.
struct GateRows {
    double dx_measured_share{};            // rows_dx_measured / rows_with_content (0 when no row has content)
    std::uint32_t rows_with_content{};     // = rows_unconfirmed + rows_measured + rows_content_unmeasured
    std::uint32_t rows_dx_measured{};      // measured rows that measured the baseline component
    // The measured rows that did not measure the baseline component (lines along the baseline only).
    std::vector<RowRange> unmeasured;
    // Output rows where the composite mixes but the measurer cannot reach (the inset of RequiredBandInsetPx at the top and
    // the bottom of the overlap). The transition handed to the seam gate leaves them out; they are reported here instead.
    std::vector<RowRange> outside_measurable;
    std::uint32_t rows_blank{};
    std::uint32_t rows_measured{};
    std::uint32_t rows_content_unmeasured{};
    std::uint32_t rows_one_sided{};        // of the rows with content unmeasured: content in one camera only at a point (Omega only)
    std::uint32_t rows_unconfirmed{};
    double content_unmeasured_share{};     // rows_content_unmeasured / rows_with_content (0 when no row has content)
    std::vector<RowRange> blank;           // the blank rows
    std::vector<RowRange> content_unmeasured;   // the rows with content that nothing measured (unconfirmed rows included)
    // The split (fields added with it): the correct rows of layer 2, left out of the decision (SplitGateOmega). rows_corrected
    // counts their lattice rows that the reference measurement holds (blank or with content); corrected are their output rows.
    // Every count above is over the verify rows only. 0 and empty without the split.
    std::uint32_t rows_corrected{};
    std::vector<RowRange> corrected;
};

// What a one-directional point measures: the component along its normal n. across_baseline: |n . e_b| < c_min, the normal
// is (nearly) perpendicular to the baseline, so the value says nothing of the left-right displacement. with_baseline: the
// normal has a baseline component of at least c_min.
enum class OneDirectionalComponent {across_baseline, with_baseline};
[[nodiscard]] constexpr std::string_view OneDirectionalComponentCode(OneDirectionalComponent c){
    return c==OneDirectionalComponent::across_baseline?"across-baseline":"with-baseline";
}

// The largest displacement by the kind of point (11.6 item 2). A one-directional point measures only the component along
// its normal, so its maximum is reported with that normal and the component's name; it is never the maximum of a
// two-directional displacement.
struct GateMaxima {
    std::optional<double> two_directional_px;
    std::optional<double> one_directional_px;
    std::optional<render::Point> one_directional_normal;                   // unit normal of the point that gave one_directional_px
    std::optional<OneDirectionalComponent> one_directional_component;      // the component that point measured
};

// The same quantities before and after layer 2 (11.5: after is measured, before is after plus the correction, not measured
// again). Equal when layer 2 was not applied.
struct GateQuantities {
    std::optional<double> maximum_px;      // after: the seam gate's maximum (measured and predicted). before: the same with the correction added back
    GateMaxima maxima;
};

// What a cause of kind `lift` carries (11.22.3). The members of the report's `gate.lift` are *reserved* in the formats
// document until the lane that writes the body settles them; this is the first shape, from the design text.
struct GateLift {
    std::uint32_t points_exceeding{};      // confirmed points over the line
    double baseline_share{};               // share of them with the baseline component dominating (before layer 2)
    double sign_agreement_share{};         // share of those with the common sign
    std::optional<double> height_estimate_mm;   // the median baseline component / k; empty when k was not given
};

// The diagnostic wide search of 11.6 item 4 (reported, never part of the maximum). Members are *reserved* in the formats
// document; this is the first shape.
struct GateWideSearch {
    std::uint32_t radius_px{};             // searched along e_b over +-radius
    std::uint32_t blocks{};                // blocks of rows along the seam that had content in A
    std::uint32_t unique_blocks{};         // with one strict peak above the correlation floor, not on the edge
    std::uint32_t agreeing_blocks{};       // in the largest group that agrees within the consistency
    std::optional<double> offset_px;       // along the baseline, at the seam (B position minus A position); empty without agreement
    std::optional<double> offset_mm;       // offset_px times the output pixel size, when that was given
    std::optional<RowRange> rows;          // the rows of the agreeing blocks
};

// Design constants of this part. None is a pass/fail line; none has a default in code (NaN or 0 until set, rejected). The
// values are recorded in the report. Members are *reserved* (11.27: every `design` content is added in the order the
// constants are decided).
struct GateDesign {
    double minimum_baseline_component=std::numeric_limits<double>::quiet_NaN();   // c_min, (0, 1]
    double lift_ratio=std::numeric_limits<double>::quiet_NaN();                   // |r_e| / |r_perp| at or above this for a point to count as lift (11.22.3: 3, a proposal)
    double lift_share=std::numeric_limits<double>::quiet_NaN();                   // share of the exceeding points that must (11.22.3: 0.9, a proposal), (0, 1]
    // The wide search (11.6 item 4).
    std::uint32_t wide_search_radius_px{};          // along e_b; the caller derives it from the surface envelope
    std::uint32_t wide_search_half_width_px{};      // columns of A's template: the seam column +- this
    std::uint32_t wide_search_block_rows{};         // rows of one block
    double wide_search_minimum_correlation=std::numeric_limits<double>::quiet_NaN();   // (0, 1)
    double wide_search_secondary_ratio=std::numeric_limits<double>::quiet_NaN();       // a second peak at or above this times the best: not unique, (0, 1]
    double wide_search_consistency_px=std::numeric_limits<double>::quiet_NaN();        // blocks within this agree
    std::uint32_t wide_search_minimum_blocks{};     // agreeing blocks an offset requires
    double wide_search_trigger_share=std::numeric_limits<double>::quiet_NaN();         // share of Omega's content points that are ambiguous or on the search boundary that runs the wide search, (0, 1]
    // The largest share of Omega's rows with content that may be left unmeasured (GateRows::content_unmeasured_share) with the
    // gate still passing, [0, 1) (11.30). Unconfirmed rows and rows with content in one camera only never pass, whatever this is.
    double maximum_unmeasured_content_share=std::numeric_limits<double>::quiet_NaN();
};

// Where the gate's placement check came from (11.30). anchors: the seam gate's wide check verified enough anchors. estimate: it
// did not, nothing contradicted it, and B is placed by a trusted estimate whose coarse stage placed it by unique blocks of W
// (levels G1 and G2). none: neither (the finding placement-unverified), or an anchor contradicted the placement.
enum class PlacementBasis {anchors, estimate, none};
[[nodiscard]] constexpr std::string_view PlacementBasisCode(PlacementBasis b){
    return b==PlacementBasis::anchors?"anchors":b==PlacementBasis::estimate?"estimate":b==PlacementBasis::none?"none":std::string_view{};
}
[[nodiscard]] constexpr std::optional<PlacementBasis> ParsePlacementBasis(std::string_view text){
    for(const auto b:{PlacementBasis::anchors,PlacementBasis::estimate,PlacementBasis::none})if(PlacementBasisCode(b)==text)return b;
    return std::nullopt;
}
// What the caller knows of an estimate that placed B (content_registration's diagnostics). Set by the capture path only when the
// level placed B by the estimate and the estimate is trusted.
struct PlacementByEstimate {std::uint32_t unique_blocks{},consistent_blocks{};};

// A finding of this part (not of the seam gate): reason and remedy, in this order. Remedies are from the set of 11.17.2.
//   unconfirmed-content      review-visually          a row of Omega holds a point over the line left unconfirmed (#385 Q1)
//   one-sided-content        review-visually          a row of Omega shows content in one camera only, and nothing measured it
//   nothing-measured         review-visually          no row of Omega was measured (a blank Omega included: D1 of #385)
//   content-unmeasured       review-visually          the share of the rows with content left unmeasured is over the design's line
//   dx-unmeasured            review-visually          rows of Omega were measured, and none of them measured the baseline component
//   placement-unverified     review-visually          the placement is verified neither by the anchors nor by a trusted estimate
//   transition-outside-band  contact-technician       the composite mixes outside the measured Omega (gate-configuration)
//   gate-undetermined        contact-technician       the seam gate itself was undetermined (its reason is in `findings`)
struct GateFinding {std::string reason,remedy;};

// The correct rows of the split measured with the same pair, for reference only (never part of the decision, the state, a finding
// or a warning): layer 2 was estimated from points that share pixels with them, so their values are not independent of the
// correction. after: the seam gate's maximum (DecideSeamGate on these rows alone) and the maxima by kind; before: the same with
// the correction added back. The rows as RowsOf counts them.
struct GateCorrectedReference {
    GateQuantities before,after;
    std::uint32_t rows_with_content{},rows_measured{};
};

// The seam gate run on the whole band: a diagnostic, never part of the decision (#404).
struct GateBandDiagnostic {
    seam_gate::SeamGateResult result;
    GateMaxima maxima;
    GateRows rows;
};

struct RegistrationGateResult {
    GateState state{GateState::not_run};
    GateCause cause{GateCause::none};
    std::string remedy{"none"};                          // GateCauseRemedy(cause)
    std::vector<seam_gate::SeamGateFinding> findings;    // reason and remedy codes of the seam gate, kept as they are
    std::vector<GateFinding> gate_findings;              // this part's findings, in the order of the list above
    std::optional<GateLift> lift;
    GateRows rows;
    GateMaxima maxima;                                   // after layer 2 (what the decision saw)
    GateQuantities before,after;
    std::optional<std::pair<std::uint32_t,std::uint32_t>> anchor_columns;   // [first,last] output columns the anchors were taken from
    PlacementBasis placement{PlacementBasis::none};
    std::optional<GateWideSearch> wide_search;
    GateDesign design{};
    std::string geometry_hash;                           // set by the capture path (capture_registration.hpp)
    // The unchanged seam gate result and the measurement it decided on (Omega's points; the anchors of the band when the band
    // was measured); empty when the state is not_run.
    std::optional<seam_gate::SeamGateResult> seam_gate;
    std::optional<seam_gate::SeamMeasurement> measurement;
    std::optional<GateBandDiagnostic> band;              // empty when the band was not measured
    // The split (fields added with it): the correct rows' reference and its measurement; empty without the split.
    std::optional<GateCorrectedReference> corrected;
    std::optional<seam_gate::SeamMeasurement> corrected_measurement;
};

// What the measure takes. The mapping pair is the one the composite was rendered from (the maker is in
// fixed_composite_maps.hpp); the gate measures with that very pair.
struct RegistrationGateRequest {
    const render::BgrImage* camera_a{};
    const render::BgrImage* camera_b{};
    MappedFixedSeamInput mapped;                         // output size and the two mappings (B includes pose, surface and layer 2)
    RegisteredMapping registered;                        // the values behind `mapped`; registered.parallax is set exactly when layer 2 was applied
    content_registration::RegistrationGeometry layout;   // the seam column, the blend width and Omega's margin, the band half width
    SeamBlendSettings blend;                             // the blend the composite used (the transition band is its width)
    seam_gate::SeamGateDesign gate_design;
    seam_gate::SeamGateLimits gate_limits;               // the owner's line and guard band (condition v3: seamGate)
    GateDesign design;
    render::Point baseline{};                            // e_b, the unit vector along the baseline in the output frame (from the calibration, not from the layout)
    // Optional calibration prediction for blocks without content (seam_gate::MeasureSeam's); empty: none.
    seam_gate::DisplacementPrediction prediction;
    // The correction of layer 2 at an output position, output px, as the displacement it removed: d_before = d_after + shift.
    // Required exactly when registered.parallax is set (the caller passes the function the mapping was built with).
    seam_gate::DisplacementPrediction correction;
    std::optional<double> baseline_px_per_mm;            // k: output px along e_b per mm of paper height (for the lift's height); empty: not reported
    std::optional<double> output_pixel_mm;               // the output pixel size (for the wide search in mm); empty: not reported
    bool wide_search{};                                  // run the diagnostic of 11.6 item 4 (the caller decides: level G3/G4); it also runs on design.wide_search_trigger_share
    bool measure_whole_band_for_anchors{true};           // 11.6 item 3; false when the time budget dropped it (anchors from Omega only)
    // The band the gate measures is the seam column +- this, inside the overlap (11.6). It is the gate's own value and not the
    // estimator's band (layout.band_half_width_px), which it never widens: the half width in use is the smaller of the two.
    // 0: layout.band_half_width_px. Otherwise at least Omega's half width.
    std::uint32_t band_measure_half_width_px{};
    // Set when B is placed by a trusted estimate (11.30); empty otherwise. It verifies the placement when the anchors do not.
    std::optional<PlacementByEstimate> placement_by_estimate;
    unsigned thread_count{1};                            // 1..256
};

// Omega and the whole band on the composite's raster, with the request's mapping pair. Both bands lie inside the overlap
// shrunk by seam_gate::RequiredBandInsetPx(gate_design). The transition is the fixed seam's (SeamCutTransitionBand of the
// seam column and blend.width_pixels) on the rows where the shrunk overlap is not empty; the other rows are listed in
// outside_measurable.
struct RegistrationGateLayout {
    seam_gate::SeamGateGeometry omega;     // band = seam column +- Omega's half width
    seam_gate::SeamGateGeometry band;      // band = seam column +- layout.band_half_width_px
    std::uint32_t omega_half_width_px{};
    std::vector<RowRange> outside_measurable;
    // The split (SplitGateOmega): omega holds the verify rows only; the correct rows' band and transition are in corrected_omega,
    // and their output rows in corrected. Empty without the split.
    std::optional<seam_gate::SeamGateGeometry> corrected_omega;
    std::vector<RowRange> corrected;
};

// The measurements a decision reads. omega must be measured on layout.omega (any thread count). band, when present, on
// layout.band; its camera B mapping may be the one before layer 2 (the caller that shares the band's points with layer 2
// measures once), since only its anchors and a diagnostic are read from it. corrected, exactly when the layout has
// corrected_omega, on it (the reference of the correct rows).
struct RegistrationGateMeasurements {
    seam_gate::SeamMeasurement omega;
    std::optional<seam_gate::SeamMeasurement> band;
    std::optional<seam_gate::SeamMeasurement> corrected;
};

// The split of Omega's rows (11.4, 11.5, 11.30): Omega's lattice rows are layer 2's gate rows (row i at first_row + i * row_step,
// AlignGateOmegaToRows done), and row i is a verify row when i % period == phase (parallax_correction::IsVerifyRow). The output
// rows of every correct row (spacing / 2 either side of it) are moved from layout.omega to layout.corrected_omega (band and
// transition) and listed in layout.corrected, so the decision reads the verify rows only and the correct rows are measured apart
// for reference. Returns the number of output rows moved. Throws std::invalid_argument for a period of 0, a phase not below it, a
// row step that is not the spacing, no rows, a layout already split or whose rows do not match its height, and std::logic_error
// when a row of the layout is not on the lattice.
std::uint32_t SplitGateOmega(RegistrationGateLayout& layout,std::uint32_t first_row,std::uint32_t row_step,std::uint32_t row_count,
    std::uint32_t period,std::uint32_t phase,std::uint32_t spacing);

// Throws std::invalid_argument for a programming error: missing images or mappings, sizes, an unset design, a baseline that is
// not a unit vector, a correction without registered.parallax or the reverse.
void ValidateRegistrationGateRequest(const RegistrationGateRequest& request);
// seam_gate::OverlapBand on up to thread_count threads (1..256, else std::invalid_argument): the same per-row scan and walk, the
// rows dealt to the threads, then the same narrowing by the inset on the calling thread. The result equals OverlapBand's for every
// thread_count, and so does an exception (that of the lowest row that throws, with OverlapBand's message). The mappings are called
// from several threads at once, so they must be immutable and deterministic (as for seam_gate::MeasureSeam).
[[nodiscard]] std::vector<seam_gate::BandRow> OverlapBandOnThreads(const seam_gate::SeamGateGeometry& geometry, std::uint32_t camera_a_width,
    std::uint32_t camera_a_height, std::uint32_t camera_b_width, std::uint32_t camera_b_height, std::uint32_t inset_px, unsigned thread_count);
// The overlap is scanned on request.thread_count threads (OverlapBandOnThreads); the layout does not depend on the count.
[[nodiscard]] RegistrationGateLayout MakeRegistrationGateLayout(const RegistrationGateRequest& request);
// Decides from measurements made by the caller (entry (b)). Throws std::invalid_argument when a point of measurements.omega
// lies outside layout.omega's band, or a measurement does not belong to the raster. The images of the request are read only by
// the wide search.
[[nodiscard]] RegistrationGateResult DecideRegistrationGate(const RegistrationGateRequest& request, const RegistrationGateLayout& layout,
    RegistrationGateMeasurements measurements);
// Measures Omega on layout.omega as MeasureRegistrationGate does. The decision takes the global placement check from the band's
// measurement when the band was measured, and drops Omega's own (DecideRegistrationGate), so with global_decided_by_band Omega's
// check runs on the least the design allows (one anchor). Omega's points are the same, and so is the design the measurement
// records. global_decided_by_band is true exactly when the caller passes the band's measurement to DecideRegistrationGate.
[[nodiscard]] seam_gate::SeamMeasurement MeasureGateOmega(const RegistrationGateRequest& request, const RegistrationGateLayout& layout,
    bool global_decided_by_band);
// Measures Omega (and the band when measure_whole_band_for_anchors), decides with the unchanged gate, and adds the quantities
// above (entry (a)). A problem that belongs to the data is a state of the result, not an exception.
[[nodiscard]] RegistrationGateResult MeasureRegistrationGate(const RegistrationGateRequest& request);

// The output pixels a gate point's measurement reads in A's placement: the matching patch, the smoothing of the feature, and
// the variants of the remeasurement when the point was remeasured (the larger patch, the patch moved along the seam both
// ways). Half-open: [x0, x1) x [y0, y1). Layer 2 keeps the points it builds a row's correction from out of these (11.5).
struct GatePatch {std::int64_t x0{},y0{},x1{},y1{};};
[[nodiscard]] GatePatch GatePatchOf(const seam_gate::SeamPoint& point, const seam_gate::SeamGateDesign& design);
// Every point of a measurement that is not outside the coverage, in the measurement's order.
[[nodiscard]] std::vector<GatePatch> RegistrationGatePatches(const seam_gate::SeamMeasurement& measurement);
[[nodiscard]] constexpr bool PatchesShareNoPixel(const GatePatch& a, const GatePatch& b){
    return a.x1<=b.x0 || b.x1<=a.x0 || a.y1<=b.y0 || b.y1<=a.y0;
}
}
