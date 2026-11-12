// Contracts of the capture registration (#417 J8, docs/design/fixed-composite-registration.md 11.17, 11.18, 11.22, 11.25 and
// docs/design/fixed-composite-registration-formats.md 4): the report v8's record, its writer and strict reader, the derived
// warnings and assessment, and the geometry hash. Camera and file free.
#include "a0/m2/experimental/capture_registration.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include "registration_fixtures.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace ex=a0::m2::experimental;
namespace cr=a0::m2::experimental::content_registration;
namespace pc=a0::m2::experimental::parallax_correction;
namespace rg=a0::m2::experimental::registration_gate;
namespace fx=a0::tests::registration_fixtures;
namespace fsm=a0::m2::experimental::fast_seam_measurer;

namespace {
int failures=0,checks=0;
void Check(bool ok,const char* what,int line){++checks;if(!ok){++failures;std::cerr<<"FAIL line "<<line<<": "<<what<<'\n';}}
#define CHECK(x) Check((x),#x,__LINE__)
template<class F>bool Throws(F f){try{f();}catch(const std::exception&){return true;}return false;}

const ex::FixedCompositeSha256 kHash=[](const std::vector<unsigned char>& bytes){return ex::FixedCompositeBytesSha256(bytes);};

ex::RegistrationLines Lines(bool apply_beyond=true){
    ex::RegistrationLines l;
    l.envelope={10,2,1,5};l.surface_envelope=ex::SurfaceEnvelope{40,8};l.apply_beyond_envelope=apply_beyond;l.max_lift_mm=1.0;
    l.seam_limit_px=1;l.seam_guard_band_px=0.25;
    return l;
}
pc::ParallaxDesign Design(){return pc::kParallaxDesignDraft;}
// A gate that ran and passed, with the quantities a writer records.
rg::RegistrationGateResult PassedGate(){
    rg::RegistrationGateResult g;g.state=rg::GateState::pass;g.cause=rg::GateCause::none;g.remedy="none";
    g.rows.dx_measured_share=1;g.rows.rows_with_content=20;g.rows.rows_dx_measured=20;g.rows.rows_measured=20;g.rows.rows_blank=4;g.rows.blank={{36,99}};
    g.placement=rg::PlacementBasis::anchors;
    g.maxima.two_directional_px=0.4;g.maxima.one_directional_px=0.3;g.maxima.one_directional_normal=a0::m2::render::Point{0,1};
    g.maxima.one_directional_component=rg::OneDirectionalComponent::across_baseline;
    g.after.maximum_px=0.4;g.after.maxima.two_directional_px=0.4;g.after.maxima.one_directional_px=0.3;g.before=g.after;
    g.anchor_columns=std::pair<std::uint32_t,std::uint32_t>{300,700};
    g.design.minimum_baseline_component=0.5;g.design.lift_ratio=3;g.design.lift_share=0.9;g.design.wide_search_radius_px=40;g.design.wide_search_half_width_px=64;
    g.design.wide_search_block_rows=96;g.design.wide_search_minimum_correlation=0.5;g.design.wide_search_secondary_ratio=0.8;g.design.wide_search_consistency_px=1.5;
    g.design.wide_search_minimum_blocks=2;g.design.wide_search_trigger_share=0.5;g.design.maximum_unmeasured_content_share=0.25;
    return g;
}
const char* kSeamGateText="{\"schema\":\"a0.seam-gate.v1\",\"state\":\"pass\",\"findings\":[]}";

ex::TimeBudgetRecord AllStages(){
    ex::TimeBudgetRecord t;
    for(const auto& e:ex::kReviewStageTexts)t.planned.push_back(e.first);
    t.cost_model.stage_ms={100,200,300,400,500,600,700};t.cost_model.tail_reserve_ms=800;
    return t;
}

// A consistent record of the given level. The estimate is verified with a small pose; layer 2 has three rows.
ex::SavedFixedCompositeRegistration Record(ex::GeometryLevel level,bool rotated=false){
    ex::SavedFixedCompositeRegistration r;
    r.condition_sha256=fx::Hex64('7');r.condition_sequence=3;r.profile_file="wpd-fixed-composite-profile-0123456789abcdef.json";
    r.profile_file_sha256=fx::Hex64('a');r.profile_fingerprint_sha256=fx::Hex64('b');
    r.raster=a0::m2::render::OutputRaster{a0::m2::render::DocumentRegionUm{0,0,254000,127000},100,1000,500};
    r.seam_column=500;r.seam_origin=rotated?"operator":"band-center-provisional";
    r.layout.seam_column=500;r.layout.mixing_width_px=8;r.layout.omega_margin_px=8;r.layout.guard_px=16;r.layout.band_half_width_px=256;
    r.base_pose={0.05,0,0,0.5,0,0};
    r.limits=Lines();
    auto& c=r.registration;
    c.registration.state=cr::RegistrationState::verified;c.registration.has_estimate=true;
    c.registration.pose={0.1,-0.1,0.05,1.0,-0.5,0.2};c.registration.pose_sigma={0.01,0.01,0.01,0.1,0.1,0.05};
    for(int i=0;i<36;++i)c.registration.covariance[i]=i%7==0?1e-4:0;
    c.registration.display={1.1180339887498949,1,-0.5,0.05,0.14142135623730953,0.1,-0.1,0.2,0.75};
    c.registration.diagnostics.outer_iterations=2;c.registration.diagnostics.points=120;c.registration.diagnostics.inliers=110;
    c.registration.diagnostics.unique_blocks=8;c.registration.diagnostics.consistent_blocks=8;
    c.surface.state=ex::SurfaceState::estimated;c.surface.plane={2,0.01,-0.005};c.surface.tilt_deg=0.64;
    c.surface.covariance={0.01,0,0,0,1e-6,0,0,0,1e-6};c.surface.baseline_equivalent_mm=3;c.surface.scale={1.001,1.0005,1.002};
    c.parallax.state=pc::ParallaxState::applied;c.parallax.baseline={1,0};c.parallax.k_px_per_mm=2.5;c.parallax.design=Design();
    c.parallax.rows={{100,0.25,0.1,40,true},{164,0.5,0.1,40,true},{228,0,0,0,true}};
    c.parallax.rows[0].state=c.parallax.rows[1].state=pc::ParallaxRowState::measured;c.parallax.rows[2].reason="too-few-points";
    for(auto& row:c.parallax.rows){row.lift_mm=-row.q/2.5;row.points=row.state==pc::ParallaxRowState::measured?30:0;}
    c.parallax.summary={0.5,0.2,164,0.001963,460,940,300};c.parallax.summary.rows_measured=2;c.parallax.summary.rows_filled=1;
    c.parallax.seam_column=500;c.parallax.first_row=100;c.parallax.row_step=64;
    c.parallax.lift.limit_mm=1;c.parallax.lift.maximum_mm=0.2;c.parallax.lift.at_row=164;
    c.gate=PassedGate();
    c.time_budget=AllStages();
    c.level.level=level;
    switch(level){
    case ex::GeometryLevel::g1:
        c.level.mapping={c.registration.pose,c.surface.plane,ex::ParallaxProfileOf(c.parallax,r.seam_column)};break;
    case ex::GeometryLevel::g2:
        c.level.mapping={c.registration.pose,c.surface.plane,std::nullopt};break;
    case ex::GeometryLevel::g3:{
        c.registration.state=cr::RegistrationState::undetermined;c.registration.reasons={"brown-lowe"};
        c.coarse_shift=ex::UniformShift{1.5,0.25,-0.25};
        auto pose=r.base_pose;pose.tx_mm+=0.25;pose.ty_mm+=-0.25;
        c.level.mapping={pose,ex::SurfacePlane{1.5,0,0},std::nullopt};break;}
    case ex::GeometryLevel::g4:
        c.registration.state=cr::RegistrationState::undetermined;c.registration.has_estimate=false;c.registration.reasons={"coarse-no-unique-blocks"};
        c.surface=ex::SurfaceEstimate{};c.surface.state=ex::SurfaceState::not_estimated;
        c.parallax=pc::ParallaxResult{};c.parallax.state=pc::ParallaxState::not_applied;c.parallax.reasons={"not-implemented"};
        c.level.mapping={r.base_pose,{},std::nullopt};break;
    }
    r.seam_gate_json=kSeamGateText;
    ex::DeriveSavedFixedCompositeRegistration(r,kHash);
    return r;
}
std::string ReportText(const ex::SavedFixedCompositeRegistration& r){
    return "{\"schema\":\"a0.uncalibrated-still-review.v8\",\"fixedCompositeRegistration\":"+ex::FixedCompositeRegistrationJson(r)+
        ",\"warnings\":"+ex::RegistrationWarningsJson(r.warnings)+",\"assessment\":"+ex::RegistrationAssessmentJson(r.assessment)+"}";
}
bool Reads(const std::string& text){try{(void)ex::ReadFixedCompositeRegistrationReport(text,kHash);}catch(const std::invalid_argument&){return false;}return true;}
bool HasCode(const std::vector<ex::RegistrationWarning>& v,ex::WarningCode code){for(const auto& w:v)if(w.code==code)return true;return false;}

void RoundTrip(){
    std::cout<<"round trip of every level\n";
    for(const auto level:{ex::GeometryLevel::g1,ex::GeometryLevel::g2,ex::GeometryLevel::g3,ex::GeometryLevel::g4}){
        const auto r=Record(level);
        const auto text=ReportText(r);
        CHECK(Reads(text));
        const auto back=ex::ReadFixedCompositeRegistrationReport(text,kHash);
        CHECK(ReportText(back)==text);
        CHECK(back.registration.level.level==level);
        CHECK(back.registration.geometry_hash==r.registration.geometry_hash && back.registration.gate.geometry_hash==r.registration.geometry_hash);
    }
    // G1 applies layer 2; G2 does not, and says so.
    const auto g1=Record(ex::GeometryLevel::g1),g2=Record(ex::GeometryLevel::g2);
    CHECK(g1.registration.level.applied.parallax && !g2.registration.level.applied.parallax);
    CHECK(!HasCode(g1.warnings,ex::WarningCode::geometry_level) && HasCode(g2.warnings,ex::WarningCode::geometry_level));
    CHECK(g1.assessment.state==ex::AssessmentState::within_criteria && g1.warnings.empty());
    CHECK(g2.assessment.state==ex::AssessmentState::within_criteria);
    CHECK(g1.registration.geometry_hash!=g2.registration.geometry_hash);
}

void Tamper(){
    std::cout<<"a changed report is refused\n";
    const auto r=Record(ex::GeometryLevel::g1);
    const auto text=ReportText(r);
    CHECK(Reads(text));
    const auto changed=[&](const std::string& from,const std::string& to){
        const auto at=text.find(from);
        if(at==std::string::npos || text.find(from,at+1)!=std::string::npos){Check(false,("token not unique: "+from).c_str(),__LINE__);return text;}
        auto t=text;t.replace(at,from.size(),to);return t;
    };
    const auto rejected=[&](const std::string& from,const std::string& to,int line){
        const bool ok=!Reads(changed(from,to));
        if(!ok)std::cerr<<"  accepted a changed report: "<<from<<" -> "<<to<<'\n';
        Check(ok,"a changed report is refused",line);
    };
#define REJECTED(a,b) rejected(a,b,__LINE__)
    REJECTED("\"condition\":{\"sha256\":\""+fx::Hex64('7'),"\"condition\":{\"sha256\":\""+fx::Hex64('8'));   // geometry hash
    REJECTED("\"mapping\":{\"pose\":{\"rxDeg\":0.10000000000000001","\"mapping\":{\"pose\":{\"rxDeg\":0.10000000000000002");   // mapping and hash
    REJECTED("\"qPx\":[0.25,0.5,0]","\"qPx\":[0.25,0.5,0.125]");                       // layer 2 samples
    REJECTED("\"aPx\":48","\"aPx\":49");
    REJECTED("\"weightFlatPx\":48","\"weightFlatPx\":49");                           // the profile is layer 2's rows with its design
    REJECTED("\"level\":\"G1\"","\"level\":\"G2\"");
    REJECTED("\"beyondEnvelopeApplied\":false","\"beyondEnvelopeApplied\":true");
    REJECTED("\"applied\":{\"pose\":true","\"applied\":{\"pose\":false");
    REJECTED("\"state\":\"pass\",\"findings\":[],\"gateFindings\"","\"state\":\"undetermined\",\"findings\":[],\"gateFindings\"");
    REJECTED("\"assessment\":{\"state\":\"within-criteria\"","\"assessment\":{\"state\":\"not-judged\"");
    REJECTED("\"warnings\":[]","\"warnings\":[{\"code\":\"geometry-level\",\"class\":\"notice\",\"component\":\"geometry-level\",\"value\":0,\"limit\":null,\"unit\":\"rows\",\"rowsPx\":null,\"rowsMm\":null,\"interpretations\":[],\"remedy\":\"none\",\"remedyAmountMm\":null,\"applied\":false,\"source\":\"condition\"}]");
    REJECTED("\"geometryHash\":\""+r.registration.geometry_hash+"\"}","\"geometryHash\":\""+std::string(64,'0')+"\"}");
    REJECTED("\"geometryHash\":\""+r.registration.geometry_hash+"\",\"seamGate\"","\"geometryHash\":\""+std::string(64,'0')+"\",\"seamGate\"");
    REJECTED("\"valueFromBase\":0.20000000000000001,","\"valueFromBase\":0.25,");                     // components are derived from p-hat - p_b
    REJECTED("\"exceeded\":{\"height\":false","\"exceeded\":{\"height\":true");
    REJECTED("\"layout\":{","\"layout\":{\"extra\":0,");
    REJECTED("\"folds\":[],","");
    REJECTED("\"guardPx\":16","\"guardPx\":16.0");                                    // not the writer's text for the value
    REJECTED("\"maxLiftMm\":1,","\"maxLiftMm\":null,");                               // a parallax state against the condition
    REJECTED("\"plannedStages\":[\"wide-search-diagnostic\",","\"plannedStages\":[");
    REJECTED("\"seamGate\":{\"findings\":[],\"schema\":\"a0.seam-gate.v1\",\"state\":\"pass\"}","\"seamGate\":{\"findings\":[],\"schema\":\"a0.seam-gate.v1\",\"state\":\"fail\"}");
    REJECTED("\"remedy\":\"none\",\"lift\"","\"remedy\":\"retake\",\"lift\"");
    REJECTED("\"qualityAccepted\":false","\"qualityAccepted\":true");
    // The split (no split here: every row a verify row with a hole; the gate has no correct rows).
    REJECTED("\"mode\":\"hole\"","\"mode\":\"split\"");
    REJECTED("\"reason\":\"too-few-points\",\"role\":\"verify\"","\"reason\":\"too-few-points\",\"role\":\"correct\"");
    REJECTED("\"rowsWithHole\":3","\"rowsWithHole\":2");
    REJECTED("\"rowsCorrected\":0","\"rowsCorrected\":1");
    REJECTED("\"corrected\":null,\"anchors\"","\"corrected\":{\"maximumPx\":0.5,\"twoDirectionalPx\":0.5,\"oneDirectionalPx\":null,\"beforeMaximumPx\":0.5,"
        "\"rowsWithContent\":0,\"rowsMeasured\":0,\"independent\":false},\"anchors\"");
#undef REJECTED
    // A G1 record of the split: the roles follow from the period and the phase, the gate keeps the correct rows' reference.
    {
        auto s=Record(ex::GeometryLevel::g1);
        auto& p=s.registration.parallax;p.verify_period_rows=2;p.verify_phase=1;p.summary.points_excluded=12;
        for(auto& row:p.rows)row.hole=false;
        auto& gate=s.registration.gate;
        gate.rows.rows_corrected=2;gate.rows.corrected={{100,131},{228,259}};
        rg::GateCorrectedReference k;k.after.maximum_px=0.5;k.after.maxima.two_directional_px=0.5;k.before.maximum_px=0.75;k.rows_with_content=2;k.rows_measured=2;
        gate.corrected=k;
        ex::DeriveSavedFixedCompositeRegistration(s,kHash);
        CHECK(p.rows[0].role==pc::ParallaxRowRole::correct && p.rows[1].role==pc::ParallaxRowRole::verify && p.summary.rows_verify==1 && p.summary.rows_correct==2);
        const auto split=ReportText(s);
        CHECK(Reads(split) && ReportText(ex::ReadFixedCompositeRegistrationReport(split,kHash))==split);
        const auto swap=[&](const std::string& from,const std::string& to){auto t=split;const auto at=t.find(from);if(at!=std::string::npos)t.replace(at,from.size(),to);return t;};
        CHECK(!Reads(swap("\"independent\":false","\"independent\":true")));
        CHECK(!Reads(swap("\"verifyPhase\":1","\"verifyPhase\":0")));
        CHECK(!Reads(swap("\"verifyPhase\":1","\"verifyPhase\":2")));
        auto none=s;none.registration.gate.corrected.reset();none.registration.gate.rows.rows_corrected=0;none.registration.gate.rows.corrected.clear();
        CHECK(!Reads(ReportText(none)));   // a G1 gate on split rows keeps the correct rows' reference
    }
    // The members in another order are the same values: accepted (the reader does not depend on the order).
    const auto moved=changed("\"surface\":{\"state\":\"estimated\",","\"surface\":{\"applied\":true,\"state\":\"estimated\",");
    CHECK(!Reads(moved));   // duplicated member: the JSON reader refuses it
}

void Derivations(){
    std::cout<<"warnings and the assessment\n";
    const auto base=Record(ex::GeometryLevel::g2);
    // A pose component exactly at its range is inside; just above is a warning that applies (apply_beyond true).
    {
        auto r=base;r.registration.registration.pose.tz_percent=r.base_pose.tz_percent+5;r.registration.level.mapping.pose=r.registration.registration.pose;
        ex::DeriveSavedFixedCompositeRegistration(r,kHash);
        CHECK(!HasCode(r.warnings,ex::WarningCode::camera_height));
        r.registration.registration.pose.tz_percent=r.base_pose.tz_percent+5.001;r.registration.level.mapping.pose=r.registration.registration.pose;
        ex::DeriveSavedFixedCompositeRegistration(r,kHash);
        CHECK(HasCode(r.warnings,ex::WarningCode::camera_height) && r.registration.level.beyond_envelope_applied);
        CHECK(r.assessment.state==ex::AssessmentState::outside_criteria);
        CHECK(Reads(ReportText(r)));
    }
    // The gate did not run: unverified, never within the criteria.
    {
        auto r=Record(ex::GeometryLevel::g4);
        r.registration.gate=rg::RegistrationGateResult{};r.seam_gate_json.reset();
        r.registration.time_budget.planned.pop_back();r.registration.time_budget.skipped={{ex::ReviewStage::gate,"time-budget"}};
        ex::DeriveSavedFixedCompositeRegistration(r,kHash);
        CHECK(HasCode(r.warnings,ex::WarningCode::gate_not_run) && HasCode(r.warnings,ex::WarningCode::geometry_level));
        CHECK(!HasCode(r.warnings,ex::WarningCode::time_budget));
        CHECK(r.assessment.state==ex::AssessmentState::not_judged);
        CHECK(Reads(ReportText(r)));
    }
    // Over the line: the maximum plus U above L is exceeded; equal is not.
    {
        auto r=base;r.registration.gate.state=rg::GateState::fail;r.registration.gate.cause=rg::GateCause::lift;r.registration.gate.remedy="shift-drawing-horizontal";
        r.registration.gate.after.maximum_px=0.75;r.seam_gate_json="{\"schema\":\"a0.seam-gate.v1\",\"state\":\"fail\"}";
        ex::DeriveSavedFixedCompositeRegistration(r,kHash);
        CHECK(!HasCode(r.warnings,ex::WarningCode::seam_displacement) && HasCode(r.warnings,ex::WarningCode::gate_configuration));
        r.registration.gate.after.maximum_px=0.8;ex::DeriveSavedFixedCompositeRegistration(r,kHash);
        CHECK(HasCode(r.warnings,ex::WarningCode::seam_displacement));
        for(const auto& w:r.warnings)if(w.code==ex::WarningCode::seam_displacement)CHECK(w.remedy==ex::Remedy::shift_drawing_horizontal && w.value==0.8+0.25 && w.limit==1.0);
        CHECK(r.assessment.state==ex::AssessmentState::outside_criteria);
        CHECK(Reads(ReportText(r)));
    }
    // The gate's own findings become the warnings of 11.17.2.
    {
        auto r=base;auto& g=r.registration.gate;g.state=rg::GateState::undetermined;
        g.gate_findings={{"dx-unmeasured","review-visually"},{"placement-unverified","review-visually"}};g.placement=rg::PlacementBasis::none;
        g.rows.rows_dx_measured=0;g.rows.unmeasured={{64,95},{320,351}};g.rows.dx_measured_share=0;
        ex::DeriveSavedFixedCompositeRegistration(r,kHash);
        CHECK(HasCode(r.warnings,ex::WarningCode::seam_dx_unmeasured) && HasCode(r.warnings,ex::WarningCode::placement_unverified));
        CHECK(!HasCode(r.warnings,ex::WarningCode::gate_configuration));
        for(const auto& w:r.warnings)if(w.code==ex::WarningCode::seam_dx_unmeasured)
            CHECK(w.warning_class==ex::WarningClass::unverified && w.value==20 && w.rows_px && w.rows_px->first==64 && w.rows_px->last==351 && w.rows_mm);
        CHECK(r.assessment.state==ex::AssessmentState::not_judged);
        CHECK(Reads(ReportText(r)));
    }
    // 11.30: what could not be measured, when the gate did not make it a reason, is information (a notice), and the gate's pass
    // stands: lines along the baseline on 3 measured rows, content unmeasured on 2 rows of 22, blank rows. The seam gate's own
    // reasons about it (here a failed seam gate) are kept as they are.
    {
        auto r=base;auto& g=r.registration.gate;
        g.rows.rows_with_content=22;g.rows.rows_measured=20;g.rows.rows_dx_measured=17;g.rows.rows_content_unmeasured=2;
        g.rows.dx_measured_share=17.0/22;g.rows.content_unmeasured_share=2.0/22;g.rows.unmeasured={{64,95},{320,351}};g.rows.content_unmeasured={{128,159},{192,223}};
        g.findings={{"unmeasurable-content","shift-paper-and-retake"},{"unpredicted-empty-block","use-calibrated-prediction"},{"global-placement-unverified","place-distinctive-content-and-retake"}};
        g.placement=rg::PlacementBasis::estimate;
        r.seam_gate_json="{\"schema\":\"a0.seam-gate.v1\",\"state\":\"fail\",\"findings\":[{\"reason\":\"unmeasurable-content\",\"remedy\":\"shift-paper-and-retake\"},"
            "{\"reason\":\"unpredicted-empty-block\",\"remedy\":\"use-calibrated-prediction\"},{\"reason\":\"global-placement-unverified\",\"remedy\":\"place-distinctive-content-and-retake\"}]}";
        ex::DeriveSavedFixedCompositeRegistration(r,kHash);
        bool dx=false,unmeasured=false;
        for(const auto& w:r.warnings){
            if(w.code==ex::WarningCode::seam_dx_unmeasured){dx=true;CHECK(w.warning_class==ex::WarningClass::notice && w.value==3 && w.remedy==ex::Remedy::none);}
            if(w.code==ex::WarningCode::seam_unmeasurable){unmeasured=true;CHECK(w.warning_class==ex::WarningClass::notice && w.value==2 && w.interpretations.empty() &&
                w.rows_px && w.rows_px->first==128 && w.rows_px->last==223);}
        }
        CHECK(dx && unmeasured && !HasCode(r.warnings,ex::WarningCode::placement_unverified) && !HasCode(r.warnings,ex::WarningCode::gate_configuration));
        CHECK(r.assessment.state==ex::AssessmentState::within_criteria);
        const auto text=ReportText(r);
        CHECK(Reads(text));
        // A pass over a failed seam gate is read only when the seam gate's reasons are the ones this part decides again.
        auto other=r;other.registration.gate.findings.push_back({"transition-outside-band","check-gate-band"});
        other.seam_gate_json="{\"schema\":\"a0.seam-gate.v1\",\"state\":\"fail\",\"findings\":[{\"reason\":\"unmeasurable-content\",\"remedy\":\"shift-paper-and-retake\"},"
            "{\"reason\":\"unpredicted-empty-block\",\"remedy\":\"use-calibrated-prediction\"},{\"reason\":\"global-placement-unverified\",\"remedy\":\"place-distinctive-content-and-retake\"},"
            "{\"reason\":\"transition-outside-band\",\"remedy\":\"check-gate-band\"}]}";
        ex::DeriveSavedFixedCompositeRegistration(other,kHash);   // the writer writes it; the reader refuses it
        CHECK(!Reads(ReportText(other)));
        auto bare=r;bare.seam_gate_json="{\"schema\":\"a0.seam-gate.v1\",\"state\":\"fail\"}";
        CHECK(!Reads(ReportText(bare)));
        // The rows' counts and shares are consistent, or the report is refused.
        const std::string token="\"rowsContentUnmeasured\":2";
        const auto at=text.find(token);
        CHECK(at!=std::string::npos && !Reads(std::string(text).replace(at,token.size(),"\"rowsContentUnmeasured\":3")));
    }
    // 11.30: the reasons of what could not be measured are unverified, with the reasons as readings.
    {
        auto r=base;auto& g=r.registration.gate;g.state=rg::GateState::fail;g.cause=rg::GateCause::unmeasurable;g.remedy="shift-drawing-horizontal";
        g.rows.rows_with_content=22;g.rows.rows_measured=20;g.rows.rows_dx_measured=20;g.rows.rows_content_unmeasured=2;g.rows.rows_one_sided=1;
        g.rows.dx_measured_share=20.0/22;g.rows.content_unmeasured_share=2.0/22;g.rows.content_unmeasured={{128,159},{192,223}};
        g.gate_findings={{"one-sided-content","review-visually"}};
        r.seam_gate_json=kSeamGateText;   // the seam gate passed: a block measured the row's neighbours
        ex::DeriveSavedFixedCompositeRegistration(r,kHash);
        bool seen=false;
        for(const auto& w:r.warnings)if(w.code==ex::WarningCode::seam_unmeasurable){seen=true;CHECK(w.warning_class==ex::WarningClass::unverified &&
            w.interpretations==std::vector<std::string>{"one-sided-content"} && w.remedy==ex::Remedy::review_visually);}
        CHECK(seen && r.assessment.state==ex::AssessmentState::not_judged && !HasCode(r.warnings,ex::WarningCode::gate_configuration));
        CHECK(Reads(ReportText(r)));
    }
    // Layer 2 above maxLiftMm is exceeded and applied only at G1.
    {
        auto r=Record(ex::GeometryLevel::g1);auto& lift=r.registration.parallax.lift;   // as layer 2 reports a row over the line
        lift.exceeded=true;lift.maximum_mm=1.2;lift.exceeded_rows=1;lift.first_exceeded_row=lift.last_exceeded_row=164;
        ex::DeriveSavedFixedCompositeRegistration(r,kHash);
        CHECK(HasCode(r.warnings,ex::WarningCode::seam_lift_limit) && r.registration.level.beyond_envelope_applied);
        for(const auto& w:r.warnings)if(w.code==ex::WarningCode::seam_lift_limit)CHECK(w.applied && w.rows_px && w.rows_px->first==164 && w.unit==ex::WarningUnit::mm);
        CHECK(Reads(ReportText(r)));
    }
    // The assessment's fourth case is a writer's error.
    CHECK(Throws([]{(void)ex::DeriveRegistrationAssessment({},rg::GateState::fail);}));
    CHECK(ex::DeriveRegistrationAssessment({},rg::GateState::pass).state==ex::AssessmentState::within_criteria);
    // The writer refuses an inconsistent record.
    {
        auto r=base;r.warnings.clear();
        CHECK(!HasCode(base.warnings,ex::WarningCode::geometry_level) || Throws([&]{(void)ex::FixedCompositeRegistrationJson(r);}));
        auto s=base;s.registration.level.beyond_envelope_applied=!s.registration.level.beyond_envelope_applied;
        CHECK(Throws([&]{(void)ex::FixedCompositeRegistrationJson(s);}));
    }
}

void Hash(){
    std::cout<<"geometry hash\n";
    const auto r=Record(ex::GeometryLevel::g1);
    const auto in=ex::GeometryHashInputOf(r);
    const auto h=ex::RegistrationGeometryHash(in,kHash);
    CHECK(h==ex::RegistrationGeometryHash(in,kHash) && h.size()==64);
    std::vector<std::function<void(ex::GeometryHashInput&)>> edits{
        [](ex::GeometryHashInput& x){x.condition_sha256[0]='8';},
        [](ex::GeometryHashInput& x){x.profile_fingerprint_sha256[0]='c';},
        [](ex::GeometryHashInput& x){x.seam_column+=1;},
        [](ex::GeometryHashInput& x){x.level=ex::GeometryLevel::g2;},
        [](ex::GeometryHashInput& x){x.base_pose.rz_deg=std::nextafter(x.base_pose.rz_deg,1.0);},
        [](ex::GeometryHashInput& x){x.applied_pose.tz_percent+=1e-12;},
        [](ex::GeometryHashInput& x){x.applied_surface.slope_y=-x.applied_surface.slope_y;},
        [](ex::GeometryHashInput& x){x.applied_parallax->q_px[1]=std::nextafter(x.applied_parallax->q_px[1],1.0);},
        [](ex::GeometryHashInput& x){x.applied_parallax->a_px+=1;},
        [](ex::GeometryHashInput& x){x.applied_parallax->l_px+=1;},
        [](ex::GeometryHashInput& x){x.applied_parallax->first_row_px+=1;},
        [](ex::GeometryHashInput& x){x.applied_parallax->baseline.y=1e-300;},
        [](ex::GeometryHashInput& x){x.applied_parallax.reset();},
        [](ex::GeometryHashInput& x){x.mixing_width_px+=2;},
        [](ex::GeometryHashInput& x){x.omega_half_width_px+=1;},
        [](ex::GeometryHashInput& x){x.raster.dpi=200;},
    };
    for(std::size_t i=0;i<edits.size();++i){auto x=in;edits[i](x);const bool differs=ex::RegistrationGeometryHash(x,kHash)!=h;
        if(!differs)std::cerr<<"  edit "<<i<<" kept the hash\n";CHECK(differs);}
    // -0 is 0 (the report writes it as 0), and nothing else is rounded: the next double is another hash.
    auto zero=in;zero.applied_surface.slope_x=0.0;auto negative=in;negative.applied_surface.slope_x=-0.0;auto tiny=in;tiny.applied_surface.slope_x=std::nextafter(0.0,1.0);
    CHECK(ex::RegistrationGeometryHash(zero,kHash)==ex::RegistrationGeometryHash(negative,kHash));
    CHECK(ex::RegistrationGeometryHash(zero,kHash)!=ex::RegistrationGeometryHash(tiny,kHash));
    CHECK(Throws([&]{(void)ex::RegistrationGeometryHash(in,ex::FixedCompositeSha256{});}));
}

// The fixture builder writes the writer's shape: its default text is refused only because its sample values are not the
// derivation of each other, not for a member or a type.
// The point source of layer 2 (formats 4.8.1): null for the band's points; else what the capture chose, checked against the record by the
// writer and the reader alike.
void PointSourceReport(){
    std::cout<<"layer 2's point source in the report\n";
    auto r=Record(ex::GeometryLevel::g1);
    r.registration.parallax.summary.points_input=320;   // the points layer 2 was given
    CHECK(!r.registration.point_source && ReportText(r).find("\"folds\":[],\"pointSource\":null,\"geometryLevel\"")!=std::string::npos);
    const auto points=r.registration.parallax.summary.points_input;
    CHECK(points>0);
    const auto with=[&](const ex::LayerTwoPointSource& s){auto c=r;c.registration.point_source=s;ex::DeriveSavedFixedCompositeRegistration(c,kHash);return c;};
    ex::LayerTwoPointSource used;used.fast_measurer_used=true;used.points_used=points;used.edge_share=0.125;used.edge_share_line=0.25;
    const auto u=with(used);
    const auto text=ReportText(u);
    CHECK(Reads(text) && ReportText(ex::ReadFixedCompositeRegistrationReport(text,kHash))==text);
    CHECK(text.find("\"pointSource\":{\"fastMeasurerUsed\":true,\"pointsUsed\":"+std::to_string(points)+",\"edgeShare\":0.125,\"edgeShareLine\":0.25,\"fallbackReason\":null}")!=std::string::npos);
    const auto back=ex::ReadFixedCompositeRegistrationReport(text,kHash);
    CHECK(back.registration.point_source && back.registration.point_source->fast_measurer_used && back.registration.point_source->points_used==points &&
        back.registration.point_source->edge_share && *back.registration.point_source->edge_share==0.125);
    const auto changed=[&](const std::string& from,const std::string& to){
        const auto at=text.find(from);
        if(at==std::string::npos || text.find(from,at+1)!=std::string::npos){Check(false,("token not unique: "+from).c_str(),__LINE__);return text;}
        auto t=text;t.replace(at,from.size(),to);return t;
    };
    const auto refused=[&](const std::string& from,const std::string& to,int line){Check(!Reads(changed(from,to)),"a changed point source is refused",line);};
#define REFUSED(a,b) refused(a,b,__LINE__)
    REFUSED("\"fastMeasurerUsed\":true","\"fastMeasurerUsed\":false");                                  // used, with no reason
    REFUSED("\"pointsUsed\":"+std::to_string(points)+",","\"pointsUsed\":"+std::to_string(points+1)+",");   // not the points layer 2 was given
    REFUSED("\"pointsUsed\":"+std::to_string(points)+",","\"pointsUsed\":0,");
    REFUSED("\"edgeShare\":0.125","\"edgeShare\":0.5");                                                  // over the line, yet used
    REFUSED("\"edgeShare\":0.125","\"edgeShare\":null");
    REFUSED("\"edgeShareLine\":0.25","\"edgeShareLine\":1.5");
    REFUSED("\"fallbackReason\":null","\"fallbackReason\":\"edge-share\"");
    REFUSED("\"pointSource\":{\"fastMeasurerUsed\":true,","\"pointSource\":{\"extra\":0,\"fastMeasurerUsed\":true,");
#undef REFUSED
    {   // The member is not optional: a report without it is refused (formats 1.2).
        auto nul=ReportText(r);
        const std::string token="\"pointSource\":null,";
        const auto at=nul.find(token);
        CHECK(at!=std::string::npos && !Reads(nul.replace(at,token.size(),"")));
    }
    // The three reasons for not using the fast points, each consistent with its share and the line.
    ex::LayerTwoPointSource edge;edge.points_used=0;edge.edge_share=0.5;edge.edge_share_line=0.25;edge.fallback_reason="edge-share";
    ex::LayerTwoPointSource none;none.edge_share=0.0625;none.edge_share_line=0.25;none.fallback_reason="no-points";
    ex::LayerTwoPointSource failed;failed.edge_share=std::nullopt;failed.edge_share_line=0.25;failed.fallback_reason="measurer-failed";
    for(const auto& s:{edge,none,failed}){
        const auto t=ReportText(with(s));
        CHECK(Reads(t) && ReportText(ex::ReadFixedCompositeRegistrationReport(t,kHash))==t);
    }
    // A reason that does not fit the share, an unknown reason, a count with no use: the writer refuses to write them, the reader to read them.
    const auto refuses=[&](ex::LayerTwoPointSource s,int line){
        auto c=r;c.registration.point_source=s;
        Check(Throws([&]{(void)ex::FixedCompositeRegistrationJson(c);}),"the writer refuses an inconsistent point source",line);
    };
    auto bad=edge;bad.edge_share=0.125;refuses(bad,__LINE__);          // edge-share, share under the line
    bad=none;bad.edge_share=0.5;refuses(bad,__LINE__);                 // no-points, share over the line
    bad=failed;bad.edge_share=0.0;refuses(bad,__LINE__);               // a failed measurement has no share
    bad=edge;bad.fallback_reason="mystery";refuses(bad,__LINE__);
    bad=edge;bad.points_used=3;refuses(bad,__LINE__);
    bad=used;bad.points_used=points+1;refuses(bad,__LINE__);           // not layer 2's points
    bad=used;bad.edge_share=0.5;refuses(bad,__LINE__);                 // used over the line
    bad=used;bad.fallback_reason="no-points";refuses(bad,__LINE__);
    // Used while layer 2 has no rows to say how many points it got.
    {
        auto g2=Record(ex::GeometryLevel::g2);
        g2.registration.parallax.rows.clear();
        g2.registration.point_source=used;
        CHECK(Throws([&]{(void)ex::FixedCompositeRegistrationJson(g2);}));
    }
}

void Fixture(){
    std::cout<<"the fixture builder's text\n";
    const auto message=[](const std::string& text){
        try{(void)ex::ReadFixedCompositeRegistrationReport(text,kHash);}catch(const std::invalid_argument& error){return std::string(error.what());}
        return std::string("accepted");
    };
    CHECK(message(fx::ReportV8Text()).rfind("report v8: derived fields",0)==0);
    fx::ReportV8Options g4;g4.level="G4";g4.registration_state="undetermined";g4.has_estimate=false;g4.surface_state="not-estimated";
    g4.gate_state="not-run";g4.time_planned_stages.pop_back();g4.time_skipped={{"gate","time-budget"}};
    CHECK(message(fx::ReportV8Text(g4)).rfind("report v8: derived fields",0)==0);
    fx::ReportV8Options extra;extra.output_path="x";   // a member outside the three is the caller's
    CHECK(message(fx::ReportV8Text(extra)).rfind("report v8: derived fields",0)==0);
}

void Components(){
    std::cout<<"components from the base pose\n";
    const cr::PoseParameters base{0.5,0,0,3,4,1};
    const auto c=ex::RegistrationComponentsFromBase({0.5,0,0,6,8,1},base,{5,2,1,5});
    CHECK(c.size()==4 && c[0].value==5 && !c[0].exceeded && c[1].value==0 && c[3].value==0);
    const auto d=ex::RegistrationComponentsFromBase({0.5,0,0,6,8.0001,1},base,{5,2,1,5});
    CHECK(d[0].exceeded);
    CHECK(Throws([&]{(void)ex::RegistrationComponentsFromBase({},base,{});}));
}

// ---- The time plan, the level, and the capture path ----
namespace render=a0::m2::render;
void Plan(){
    std::cout<<"time plan\n";
    ex::ReviewStageCosts costs;costs.stage_ms={100,200,300,400,500,600,700};costs.tail_reserve_ms=800;
    const double all=100+200+300+400+500+600+700+800;
    auto t=ex::PlanReviewStages(all,costs);
    CHECK(t.planned.size()==7 && t.skipped.empty());                                   // equal fits
    t=ex::PlanReviewStages(all-1,costs);
    CHECK(t.skipped.size()==1 && t.skipped[0].stage==ex::ReviewStage::wide_search_diagnostic && t.skipped[0].reason=="time-budget");
    t=ex::PlanReviewStages(all-100-200,costs);
    CHECK(t.skipped.size()==2 && t.skipped[1].stage==ex::ReviewStage::band_measurement && t.planned.front()==ex::ReviewStage::parallax);
    t=ex::PlanReviewStages(800+700,costs);                                               // only the gate is left
    CHECK(t.planned.size()==1 && t.planned[0]==ex::ReviewStage::gate);
    t=ex::PlanReviewStages(799,costs);                                                   // not even the tail: nothing runs, the composite is still made
    CHECK(t.planned.empty() && t.skipped.size()==7);
    CHECK(Throws([&]{(void)ex::PlanReviewStages(-1,costs);}) && Throws([&]{(void)ex::PlanReviewStages(std::nan(""),costs);}));
    auto bad=costs;bad.stage_ms[3]=-1;CHECK(Throws([&]{(void)ex::PlanReviewStages(1e9,bad);}));
}

ex::MappingCoverCheckFunction CoverAll(){return [](const ex::RegisteredMapping&){return ex::MappingCoverCheck{true,true};};}
void Levels(){
    std::cout<<"geometry level\n";
    const auto r=Record(ex::GeometryLevel::g1);
    const auto lines=Lines(true),strict=Lines(false);
    ex::GeometryLevelInput in;in.registration=&r.registration.registration;in.surface=&r.registration.surface;in.parallax=&r.registration.parallax;
    in.base_pose=r.base_pose;in.lines=&lines;in.seam_column=r.seam_column;in.cover_check=CoverAll();
    auto d=ex::SelectGeometryLevel(in);
    CHECK(d.level==ex::GeometryLevel::g1 && d.reasons.empty() && d.mapping.parallax && d.applied.parallax && !d.beyond_envelope_applied);
    // A candidate that leaves the seam outside the overlap is passed over, with the reason.
    in.cover_check=[](const ex::RegisteredMapping& m){return ex::MappingCoverCheck{!m.parallax,true};};
    d=ex::SelectGeometryLevel(in);
    CHECK(d.level==ex::GeometryLevel::g2 && d.reasons==std::vector<std::string>{"seam-outside-overlap-at-level"});
    const auto base=r.base_pose;
    in.cover_check=[base](const ex::RegisteredMapping& m){
        const bool at_base=m.pose.tx_mm==base.tx_mm && m.pose.rx_deg==base.rx_deg && m.surface.IsZero();
        return ex::MappingCoverCheck{true,at_base};};
    d=ex::SelectGeometryLevel(in);
    CHECK(d.level==ex::GeometryLevel::g4 && d.reasons.size()==2 && d.reasons[0]=="output-uncovered-at-level" && !d.applied.pose);
    // A check that cannot build the mapping counts as a seam that cannot be drawn.
    in.cover_check=[](const ex::RegisteredMapping& m)->ex::MappingCoverCheck{if(m.parallax)throw std::invalid_argument("x");return {true,true};};
    CHECK(ex::SelectGeometryLevel(in).level==ex::GeometryLevel::g2);
    // G4 that fails: the condition is broken, no composite.
    in.cover_check=[](const ex::RegisteredMapping&){return ex::MappingCoverCheck{false,true};};
    bool unavailable=false;try{(void)ex::SelectGeometryLevel(in);}catch(const ex::GeometryLevelUnavailable& e){unavailable=!e.SeamCovered();}
    CHECK(unavailable);
    in.cover_check=CoverAll();
    // Beyond a range: applied and warned with applyBeyondEnvelope, not applied without.
    auto far=r.registration.registration;far.pose.rz_deg=r.base_pose.rz_deg+2.5;
    in.registration=&far;
    d=ex::SelectGeometryLevel(in);CHECK(d.level==ex::GeometryLevel::g1 && d.beyond_envelope_applied);
    in.lines=&strict;d=ex::SelectGeometryLevel(in);CHECK(d.level==ex::GeometryLevel::g4 && !d.beyond_envelope_applied);
    in.registration=&r.registration.registration;
    // Layer 2 beyond maxLiftMm without applyBeyondEnvelope: G2.
    auto lifted=r.registration.parallax;lifted.lift.exceeded=true;in.parallax=&lifted;
    CHECK(ex::SelectGeometryLevel(in).level==ex::GeometryLevel::g2);
    in.lines=&lines;d=ex::SelectGeometryLevel(in);CHECK(d.level==ex::GeometryLevel::g1 && d.beyond_envelope_applied);
    in.parallax=&r.registration.parallax;
    // Not trusted: the coarse shift (G3) or nothing (G4). The pose of G3 is p_b plus the shift, the plane its h0 only.
    auto untrusted=r.registration.registration;untrusted.state=cr::RegistrationState::undetermined;in.registration=&untrusted;
    CHECK(ex::SelectGeometryLevel(in).level==ex::GeometryLevel::g4);
    in.coarse_shift=ex::UniformShift{3,0.5,-0.25};
    d=ex::SelectGeometryLevel(in);
    CHECK(d.level==ex::GeometryLevel::g3 && d.mapping.pose.tx_mm==r.base_pose.tx_mm+0.5 && d.mapping.pose.ty_mm==-0.25 && d.mapping.surface.h0_mm==3 &&
        d.mapping.surface.slope_x==0 && !d.mapping.parallax && d.mapping.pose.rx_deg==r.base_pose.rx_deg);
    in.coarse_shift=ex::UniformShift{50,0,0};in.lines=&strict;CHECK(ex::SelectGeometryLevel(in).level==ex::GeometryLevel::g4);
    in.lines=&lines;d=ex::SelectGeometryLevel(in);CHECK(d.level==ex::GeometryLevel::g3 && d.beyond_envelope_applied);
    // The level and the report agree on the derived flags.
    {
        auto rec=r;rec.registration.registration=untrusted;rec.registration.coarse_shift=in.coarse_shift;rec.registration.level=d;
        rec.registration.surface=ex::SurfaceEstimate{};rec.registration.surface.state=ex::SurfaceState::not_estimated;
        ex::DeriveSavedFixedCompositeRegistration(rec,kHash);
        CHECK(rec.registration.level.beyond_envelope_applied==d.beyond_envelope_applied && Reads(ReportText(rec)));
        CHECK(HasCode(rec.warnings,ex::WarningCode::surface_height) && HasCode(rec.warnings,ex::WarningCode::geometry_level));
    }
    ex::GeometryLevelInput missing;CHECK(Throws([&]{(void)ex::SelectGeometryLevel(missing);}));
}

// A synthetic rig (two cameras over a flat sheet, as the estimator's own tests build it, with a fixed pattern instead of a
// pseudo-random one).
namespace rig {
constexpr unsigned kW=1200,kH=800;
constexpr double kFocal=1560,kPi=3.14159265358979323846;
const render::LensDistortion kLens{-0.035,0.012,0.0,0.0005,-0.0003};
render::Matrix3 Mul(const render::Matrix3& a,const render::Matrix3& b){
    render::Matrix3 r{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k)r[3*i+j]+=a[3*i+k]*b[3*k+j];return r;
}
render::CameraProjection Camera(double x0,double y0,double depth,double roll,double tilt_x,double tilt_y){
    const double d=kPi/180;
    const render::Matrix3 rz{std::cos(roll*d),-std::sin(roll*d),0,std::sin(roll*d),std::cos(roll*d),0,0,0,1};
    const render::Matrix3 rx{1,0,0,0,std::cos(tilt_x*d),-std::sin(tilt_x*d),0,std::sin(tilt_x*d),std::cos(tilt_x*d)};
    const render::Matrix3 ry{std::cos(tilt_y*d),0,std::sin(tilt_y*d),0,1,0,-std::sin(tilt_y*d),0,std::cos(tilt_y*d)};
    const auto rot=Mul(rz,Mul(rx,ry));
    const double c0[3]{rot[0],rot[3],rot[6]},c1[3]{rot[1],rot[4],rot[7]};
    const double t[3]{-x0*c0[0]-y0*c1[0],-x0*c0[1]-y0*c1[1],-x0*c0[2]-y0*c1[2]+depth};
    const double cx=(kW-1)/2.0,cy=(kH-1)/2.0;
    auto h=Mul(render::Matrix3{kFocal,0,cx,0,kFocal,cy,0,0,1},render::Matrix3{c0[0],c1[0],t[0],c0[1],c1[1],t[1],c0[2],c1[2],t[2]});
    const double n=h[8];for(auto& v:h)v/=n;h[8]=1;
    return render::CameraProjection(render::CameraIntrinsics(kFocal,kFocal,cx,cy),kLens,h);
}
bool SheetPoint(const render::CameraProjection& c,double u,double v,render::Point& out){
    const auto& i=c.intrinsics;const auto& d=c.distortion;
    const double xd=(u-i.cx_pixels)/i.fx_pixels,yd=(v-i.cy_pixels)/i.fy_pixels;double x=xd,y=yd;
    for(int it=0;it<60;++it){const double r2=x*x+y*y,radial=1+d.k1*r2+d.k2*r2*r2+d.k3*r2*r2*r2;
        x+=xd-(x*radial+2*d.p1*x*y+d.p2*(r2+2*x*x));y+=yd-(y*radial+d.p1*(r2+2*y*y)+2*d.p2*x*y);}
    return render::TryTransform(render::Invert(c.document_to_image),{i.fx_pixels*x+i.cx_pixels,i.fy_pixels*y+i.cy_pixels},out);
}
// A sum of plane waves with fixed (golden-ratio) wavelengths, directions and phases: no lattice, no repeat, no random numbers.
double Sheet(double x,double y,bool blank){
    if(blank)return 255;
    double s=150;
    for(int k=0;k<40;++k){
        const auto frac=[](double v){return v-std::floor(v);};
        const double wavelength=4.0*std::pow(10.0,frac(0.6180339887*(k+1))),angle=2*kPi*frac(0.7548776662*(k+1)),phase=2*kPi*frac(0.5698402910*(k+1));
        s+=36.0*std::sqrt(2.0/40)*std::cos(2*kPi/wavelength*(std::cos(angle)*x+std::sin(angle)*y)+phase);
    }
    return s;
}
render::BgrImage Raw(const render::CameraProjection& c,bool blank){
    render::BgrImage im{kW,kH,std::vector<std::uint8_t>(static_cast<std::size_t>(kW)*kH*3)};
    for(unsigned v=0;v<kH;++v)for(unsigned u=0;u<kW;++u){render::Point p{};std::uint8_t value=0;
        if(SheetPoint(c,u,v,p))value=static_cast<std::uint8_t>(std::clamp(std::lround(Sheet(p.x,p.y,blank)),0L,255L));
        const std::size_t at=(static_cast<std::size_t>(v)*kW+u)*3;im.bgr[at]=im.bgr[at+1]=im.bgr[at+2]=value;}
    return im;
}
struct Rig {
    render::OutputRaster raster{render::DocumentRegionUm(20000,20000,770000,340000),48,(750000u*48+25399)/25400,(320000u*48+25399)/25400};
    render::CameraProjection a=Camera(270,180,700,0.3,-0.15,0.1),b=Camera(520,180,700,-0.4,0.2,-0.1);
    render::DocumentRenderParameters parameters{raster,a,b,a0::m2::StitchLayout::camera_a_left_camera_b_right,render::Resampling::bilinear};
};
}

ex::CaptureRegistrationRequest Request(const rig::Rig& g,const render::BgrImage& a,const render::BgrImage& b,unsigned threads){
    ex::CaptureRegistrationRequest r;
    r.camera_a=&a;r.camera_b=&b;r.parameters=&g.parameters;
    // The seam column: the middle of the overlap on the raster's middle row.
    const auto band=ex::MappedFixedSeamBand(a,b,ex::DocumentMappedFixedSeamInput(g.parameters));
    const auto& middle=band[band.size()/2];
    const std::uint32_t seam=(middle.first+middle.end)/2;
    r.estimator.geometry={seam,8,4,40,200};
    r.estimator.envelope={10.0,2.0,1.0,1.0};
    r.estimator.limits.brown_lowe_alpha=8;r.estimator.limits.brown_lowe_beta=0.3;r.estimator.limits.minimum_unique_blocks=2;
    r.estimator.limits.maximum_prediction_sigma_px=1.0;r.estimator.limits.maximum_residual_rms_px=0.5;r.estimator.limits.maximum_residual_p95_px=1.0;
    r.estimator.limits.maximum_bin_bias_px=0.5;r.estimator.limits.convergence_px=0.02;r.estimator.limits.start_agreement_px=0.3;
    r.estimator.design=cr::kRegistrationDesignDraft;
    // A 48 dpi raster (about a quarter of a real one): the estimator's own tests shrink these lengths the same way.
    r.estimator.design.coarse_block_rows=256;r.estimator.design.coarse_maximum_radius_px=256;r.estimator.design.fine_point_spacing_px=32;
    r.estimator.design.fine_patch_px=48;r.estimator.design.fine_search_radius_px=16;r.estimator.design.fine_maximum_search_radius_px=64;
    r.estimator.design.fine_global_search_radius_px=36;
    r.lines=Lines();r.lines.envelope=r.estimator.envelope;
    r.blend=ex::kSeamBlendDesign;
    r.design=ex::CaptureRegistrationDesignDraft();
    for(auto* d:{&r.design.omega,&r.design.band}){d->global_search_radius_px=128;d->global_minimum_anchors=2;}
    const auto parameters=g.parameters;
    r.cover_check=[parameters,&a,&b,seam](const ex::RegisteredMapping& m){
        const auto pair=ex::RegisteredDocumentMappedFixedSeamInput(parameters,m,seam);
        return ex::MappingCoverCheck{ex::CanRenderMappedFixedSeam(a,b,pair,seam,ex::kSeamBlendDesign),true};
    };
    r.thread_count=threads;
    return r;
}
ex::RegistrationRecordIdentity Identity(){
    return {fx::Hex64('7'),1,"wpd-fixed-composite-profile-0123456789abcdef.json",fx::Hex64('a'),fx::Hex64('b'),std::nullopt,"band-center-provisional"};
}
std::vector<std::uint8_t> V7Pixels(const rig::Rig& g,const render::BgrImage& a,const render::BgrImage& b,std::uint32_t seam){
    return ex::RenderMappedFixedSeam(a,b,ex::ScaledDocumentMappedFixedSeamInput(g.parameters,1.0),seam,ex::kSeamBlendDesign).image.bgr;
}

// The final gate's Omega aligned to layer 2's rows: layer 2 moved B's coverage at the top, so the final Omega begins 5 rows after
// the layout layer 2's rows were made on (lattice rows from 86, gate rows 102 + 32 i, ten of them).
void AlignToLayerTwo(){
    std::cout<<"the gate's rows are layer 2's\n";
    namespace sg=a0::m2::seam_gate;
    const auto make=[](std::uint32_t top){
        rg::RegistrationGateLayout layout;auto& o=layout.omega;
        o.width=1000;o.height=500;o.band.assign(500,{});o.transition.assign(500,{});
        for(std::uint32_t y=top;y<420;++y){o.band[y]={488,513};o.transition[y]={496,505};}
        layout.outside_measurable={{0,85},{430,499}};
        return layout;
    };
    auto moved=make(91);
    const auto dropped=ex::AlignGateOmegaToRows(moved,102,32,10,32);
    const auto& o=moved.omega;
    // The first of layer 2's lattice rows the final Omega covers is 118 (86 + 32): rows 91..117 and, after layer 2's last lattice
    // row 405, rows 406..419 are left out, with their transition.
    CHECK(dropped==27+14);
    CHECK(o.band[117].begin==o.band[117].end && o.band[118].begin<o.band[118].end && o.band[405].begin<o.band[405].end && o.band[406].begin==o.band[406].end);
    CHECK(o.transition[100].begin==o.transition[100].end && o.transition[200].begin<o.transition[200].end);
    CHECK(moved.outside_measurable.size()==4 && moved.outside_measurable[1].first==91 && moved.outside_measurable[1].last==117 &&
        moved.outside_measurable[2].first==406 && moved.outside_measurable[2].last==419);
    CHECK(sg::CheckTransitionBandContained(o.band,o.transition,o.width).Contained());
    // The measurer's first row (the first band row + 16) is a row of layer 2.
    std::uint32_t first=0;while(o.band[first].begin>=o.band[first].end)++first;
    CHECK(first+16==102+32);
    // Already on the lattice: only the rows after layer 2's last one go.
    auto aligned=make(86);
    CHECK(ex::AlignGateOmegaToRows(aligned,102,32,10,32)==14 && aligned.omega.band[86].begin<aligned.omega.band[86].end && aligned.outside_measurable.size()==3);
    // Rows of another step are a programming error.
    auto other=make(86);
    CHECK(Throws([&]{(void)ex::AlignGateOmegaToRows(other,102,16,10,32);}));
}

// The band the capture path measures is the design's own, apart from the estimator's band, and it holds what layer 2 reads.
void MeasuredBand(){
    std::cout<<"the measured band\n";
    const rig::Rig g;
    const auto a=rig::Raw(g.a,false),b=rig::Raw(g.b,false);
    const auto base=Request(g,a,b,4);
    const std::uint32_t omega_half=base.estimator.geometry.omega_half_width_px();
    // The relation between the constants: the draft's band holds layer 2's column radius (rounded up) and its point patch, and the
    // band is measured with the patch and lattice layer 2's points have.
    const auto draft=ex::CaptureRegistrationDesignDraft();
    const std::uint32_t reach=static_cast<std::uint32_t>(std::ceil(draft.parallax.column_radius_px))+draft.parallax.point_patch_px;
    CHECK(draft.band_measure_half_width_px>=reach && ex::CaptureBandReachesLayerTwo(draft,omega_half));
    CHECK(draft.band.patch_px==draft.parallax.point_patch_px && draft.band.point_spacing_px==draft.parallax.column_spacing_px);
    auto d=draft;
    d.band_measure_half_width_px=reach;CHECK(ex::CaptureBandReachesLayerTwo(d,omega_half));
    d.band_measure_half_width_px=reach-1;CHECK(!ex::CaptureBandReachesLayerTwo(d,omega_half));
    d.band_measure_half_width_px=0;CHECK(ex::CaptureBandReachesLayerTwo(d,omega_half));   // the estimator's band, as before
    d.band_measure_half_width_px=reach;CHECK(!ex::CaptureBandReachesLayerTwo(d,reach+1));
    d=draft;d.parallax.column_radius_px=std::numeric_limits<double>::quiet_NaN();CHECK(!ex::CaptureBandReachesLayerTwo(d,omega_half));
    d=draft;d.parallax.point_patch_px+=8;d.band_measure_half_width_px=reach+7;CHECK(!ex::CaptureBandReachesLayerTwo(d,omega_half));
    auto narrow_band=base;narrow_band.design.band_measure_half_width_px=reach-1;
    CHECK(Throws([&]{(void)ex::PrepareCaptureRegistration(narrow_band);}));
    // The band measured is narrower, layer 2 and the composite are the same, and the report's layout is still the estimator's.
    // This scene is a quarter of a real one and its overlap is narrower than the draft's band, so layer 2 gets a narrower kernel
    // here. The regression reads a point whose footprint is within the kernel's reach of the gate's pixels: at most Omega's half
    // width + the gate footprint (51) + the reach (3 sigma = 48) + a remeasured point's footprint (50) = 157 px from the seam,
    // and the lattice has no column between 157 and the measured band's end + half the spacing.
    auto wide=base;wide.design.parallax.column_radius_px=112;wide.design.parallax.kernel_sigma_x_px=16;
    const std::uint32_t scene_reach=160;
    // Neither band is cut by the overlap (its half width is about 190 px), and the half widths differ by a multiple of the point
    // spacing: the lattice columns of the two bands are the same ones.
    wide.estimator.geometry.band_half_width_px=scene_reach+16;
    wide.design.band_measure_half_width_px=0;
    auto narrow=wide;narrow.design.band_measure_half_width_px=scene_reach;
    auto pw=ex::PrepareCaptureRegistration(wide),pn=ex::PrepareCaptureRegistration(narrow);
    const auto width=[](const rg::RegistrationGateLayout& l){std::uint32_t most=0;for(const auto& row:l.band.band)most=std::max(most,row.end-row.begin);return most;};
    CHECK(pw.layout_before_layer_two && pn.layout_before_layer_two);
    const std::uint32_t ww=width(*pw.layout_before_layer_two),wn=width(*pn.layout_before_layer_two);
    std::cout<<"  band width "<<ww<<" px, measured band width "<<wn<<" px\n";
    CHECK(wn<=2*scene_reach+1 && wn<ww);
    CHECK(pw.capture.parallax.Applied() && pn.capture.parallax.Applied() && pw.capture.parallax.rows.size()==pn.capture.parallax.rows.size());
    bool same_q=pw.capture.parallax.rows.size()==pn.capture.parallax.rows.size();
    for(std::size_t i=0;same_q && i<pw.capture.parallax.rows.size();++i)
        same_q=std::memcmp(&pw.capture.parallax.rows[i].q,&pn.capture.parallax.rows[i].q,sizeof(double))==0 && pw.capture.parallax.rows[i].points==pn.capture.parallax.rows[i].points;
    CHECK(same_q);
    CHECK(pw.composite.image.bgr==pn.composite.image.bgr && pw.capture.level.level==pn.capture.level.level);
    ex::MeasureCaptureRegistrationGate(wide,pw);ex::MeasureCaptureRegistrationGate(narrow,pn);
    CHECK(pn.capture.gate.state==pw.capture.gate.state && pn.capture.gate.band && pw.capture.gate.band);
    CHECK(pn.capture.gate.anchor_columns && pn.capture.gate.anchor_columns->first>=narrow.estimator.geometry.seam_column-scene_reach &&
        pn.capture.gate.anchor_columns->second<=narrow.estimator.geometry.seam_column+scene_reach);
    CHECK(pn.capture.gate.maxima.two_directional_px==pw.capture.gate.maxima.two_directional_px &&
        pn.capture.gate.maxima.one_directional_px==pw.capture.gate.maxima.one_directional_px);
    const auto recw=ex::RecordOfCapture(Identity(),wide,pw,kHash),recn=ex::RecordOfCapture(Identity(),narrow,pn,kHash);
    CHECK(recn.layout.band_half_width_px==narrow.estimator.geometry.band_half_width_px && recn.layout.band_half_width_px==recw.layout.band_half_width_px);
    CHECK(Reads(ReportText(recn)));
    // The band stage's band is not the gate's own: at G4 the level drops the band layer 2 used and the gate measures its band on the
    // estimator's width, where its anchors are the only evidence of the placement.
    auto g4=base;g4.design.parallax.column_radius_px=16;g4.design.band_measure_half_width_px=48;
    g4.cover_check=[inner=base.cover_check](const ex::RegisteredMapping& m){auto c=inner(m);if(!m.IsPoseOnly() || !m.pose.IsZero())c.seam_covered=false;return c;};
    auto pg=ex::PrepareCaptureRegistration(g4);ex::MeasureCaptureRegistrationGate(g4,pg);
    CHECK(pg.capture.level.level==ex::GeometryLevel::g4 && pg.capture.gate.state!=rg::GateState::not_run && pg.capture.gate.anchor_columns);
    CHECK(pg.capture.gate.anchor_columns && pg.capture.gate.anchor_columns->second-pg.capture.gate.anchor_columns->first>2*48);
}

// The band stage evaluates the first band_stage_anchors anchors only (#417): the band's points, layer 2, the composite and the gate's
// decision are those of the run with all of them, the anchors are the first of the full set with its results, the band keeps the
// design the gate was given, and the band the gate measures itself (G4) evaluates all of its anchors. The correct rows' reference
// measurement evaluates one.
void BandStageAnchors(){
    std::cout<<"the band stage's anchors\n";
    const rig::Rig g;
    const auto a=rig::Raw(g.a,false),b=rig::Raw(g.b,false);
    auto full=Request(g,a,b,4);
    full.design.band_stage_anchors=0;
    auto pa=ex::PrepareCaptureRegistration(full);
    CHECK(pa.band && pa.capture.level.level==ex::GeometryLevel::g1);
    const std::uint32_t candidates=pa.band?pa.band->global.candidates:0,all=pa.band?pa.band->global.evaluated:0;
    std::cout<<"  anchors: "<<candidates<<" candidates, "<<all<<" evaluated with all of them\n";
    CHECK(all>=2 && all==std::min<std::uint32_t>(candidates,full.design.band.global_maximum_evaluations));
    ex::MeasureCaptureRegistrationGate(full,pa);
    for(const std::uint32_t n:{1u,2u}){
        auto few=full;few.design.band_stage_anchors=n;
        auto pf=ex::PrepareCaptureRegistration(few);
        CHECK(pf.band && pf.band->global.evaluated==n && pf.band->global.candidates==candidates);
        auto pr=ex::PrepareCaptureRegistration(full);   // the band of the full run, before the gate takes it
        if(pf.band && pr.band){
            bool prefix=pf.band->global.anchors.size()==n;
            for(std::size_t k=0;prefix && k<n;++k){
                const auto& x=pf.band->global.anchors[k];const auto& y=pr.band->global.anchors[k];
                prefix=x.x==y.x && x.y==y.y && x.status==y.status && x.local.x==y.local.x && x.local.y==y.local.y && x.wide.x==y.wide.x && x.wide.y==y.wide.y;
            }
            CHECK(prefix);
            bool points=pf.band->points.size()==pr.band->points.size() && pf.band->blocks.size()==pr.band->blocks.size();
            for(std::size_t i=0;points && i<pf.band->points.size();++i){
                const auto& x=pf.band->points[i];const auto& y=pr.band->points[i];
                points=x.status==y.status && x.x==y.x && x.y==y.y && std::memcmp(&x.displacement,&y.displacement,sizeof(x.displacement))==0;
            }
            CHECK(points);
            // The measurement carries the design it was asked for, so the gate's check of the placement constants holds.
            CHECK(pf.band->design.global_maximum_evaluations==few.design.band.global_maximum_evaluations &&
                pf.band->design.global_minimum_anchors==few.design.band.global_minimum_anchors);
        }
        CHECK(pf.capture.parallax.Applied() && pf.capture.parallax.rows.size()==pa.capture.parallax.rows.size() && pf.capture.level.level==pa.capture.level.level);
        bool same_q=pf.capture.parallax.rows.size()==pa.capture.parallax.rows.size();
        for(std::size_t i=0;same_q && i<pf.capture.parallax.rows.size();++i)same_q=std::memcmp(&pf.capture.parallax.rows[i].q,&pa.capture.parallax.rows[i].q,sizeof(double))==0;
        CHECK(same_q && pf.composite.image.bgr==pa.composite.image.bgr);
        ex::MeasureCaptureRegistrationGate(few,pf);
        const auto& gf=pf.capture.gate;const auto& ga=pa.capture.gate;
        CHECK(gf.state==ga.state && gf.cause==ga.cause && gf.maxima.two_directional_px==ga.maxima.two_directional_px &&
            gf.maxima.one_directional_px==ga.maxima.one_directional_px && gf.after.maximum_px==ga.after.maximum_px && gf.before.maximum_px==ga.before.maximum_px);
        // The basis of the placement: the anchors verify it when enough of those evaluated do (the first ones only here), otherwise
        // the trusted estimate does; the full run may have had more verified anchors than the first n.
        const auto minimum=few.design.band.global_minimum_anchors;
        const bool anchors_few=gf.seam_gate && gf.seam_gate->global_verified>=minimum;
        std::cout<<"  "<<n<<" anchors: verified "<<(gf.seam_gate?gf.seam_gate->global_verified:0)<<" (all: "<<(ga.seam_gate?ga.seam_gate->global_verified:0)<<"), placement by "<<(anchors_few?"anchors":"estimate")<<"\n";
        CHECK(gf.placement==(anchors_few?rg::PlacementBasis::anchors:rg::PlacementBasis::estimate));
        CHECK(!anchors_few || ga.placement==rg::PlacementBasis::anchors);
        CHECK(gf.seam_gate && ga.seam_gate && gf.seam_gate->global_evaluated==n && ga.seam_gate->global_evaluated==all && gf.seam_gate->global_candidates==ga.seam_gate->global_candidates);
        CHECK(gf.seam_gate && ga.seam_gate && gf.seam_gate->global_contradicted<=ga.seam_gate->global_contradicted && gf.seam_gate->global_verified<=ga.seam_gate->global_verified);
        CHECK(Reads(ReportText(ex::RecordOfCapture(Identity(),few,pf,kHash))));
    }
    // The correct rows' reference measurement evaluates one anchor and gives the same maxima whatever the stage's setting.
    CHECK(pa.capture.gate.corrected_measurement && pa.capture.gate.corrected_measurement->global.evaluated==1 &&
        pa.capture.gate.corrected_measurement->design.global_maximum_evaluations==full.design.omega.global_maximum_evaluations);
    // The band the gate measures itself (the level is G4, so the band layer 2 used is not the placement's evidence) evaluates all.
    auto g4=full;g4.design.band_stage_anchors=1;
    g4.cover_check=[inner=full.cover_check](const ex::RegisteredMapping& m){auto c=inner(m);if(!m.IsPoseOnly() || !m.pose.IsZero())c.seam_covered=false;return c;};
    auto pg=ex::PrepareCaptureRegistration(g4);ex::MeasureCaptureRegistrationGate(g4,pg);
    auto g4_full=g4;g4_full.design.band_stage_anchors=0;
    auto ph=ex::PrepareCaptureRegistration(g4_full);ex::MeasureCaptureRegistrationGate(g4_full,ph);
    CHECK(pg.capture.level.level==ex::GeometryLevel::g4 && pg.capture.gate.seam_gate && ph.capture.gate.seam_gate &&
        pg.capture.gate.seam_gate->global_evaluated==ph.capture.gate.seam_gate->global_evaluated && pg.capture.gate.seam_gate->global_evaluated>=2);
}

// Where layer 2's points come from (design 11.31). The default is the band's points and a capture is what it was; with the fast
// measurer selected, layer 2 is given its points when they pass the stops, else the band's, and the choice is recorded.
bool SameRows(const pc::ParallaxResult& x,const pc::ParallaxResult& y){
    bool same=x.rows.size()==y.rows.size() && x.folds.size()==y.folds.size() && x.state==y.state;
    for(std::size_t i=0;same && i<x.rows.size();++i)same=std::memcmp(&x.rows[i].q,&y.rows[i].q,sizeof(double))==0 && x.rows[i].points==y.rows[i].points && x.rows[i].state==y.rows[i].state;
    return same;
}
void PointSource(){
    std::cout<<"layer 2's points: the band's or the fast measurer's\n";
    const rig::Rig g;
    const auto a=rig::Raw(g.a,false),b=rig::Raw(g.b,false);
    // The default is the band; nothing about the fast measurer is recorded.
    auto base=Request(g,a,b,4);
    CHECK(base.design.layer_two_points==ex::LayerTwoPoints::band && ex::CaptureRegistrationDesignDraft().layer_two_points==ex::LayerTwoPoints::band);
    auto pb=ex::PrepareCaptureRegistration(base);
    CHECK(!pb.capture.point_source && pb.capture.parallax.Applied());
    // The fast measurer's constants are not read while the band is selected, even when they are not valid.
    auto unread=base;unread.design.fast_measurer={};
    auto pu=ex::PrepareCaptureRegistration(unread);
    CHECK(!pu.capture.point_source && SameRows(pu.capture.parallax,pb.capture.parallax) && pu.composite.image.bgr==pb.composite.image.bgr);
    // Selected and not valid: refused.
    auto bad=base;bad.design.layer_two_points=ex::LayerTwoPoints::fast_measurer;bad.design.fast_measurer.search_radius_px=3;
    CHECK(Throws([&]{(void)ex::PrepareCaptureRegistration(bad);}));
    // Selected and used: the sheet has no repeat and layer 1 is right, so the points pass the stops.
    auto fast=base;fast.design.layer_two_points=ex::LayerTwoPoints::fast_measurer;
    auto pf=ex::PrepareCaptureRegistration(fast);
    const auto& src=pf.capture.point_source;
    if(src)std::cout<<"  used "<<src->fast_measurer_used<<", points "<<src->points_used<<", edge share "<<(src->edge_share?*src->edge_share:-1)<<" (line "<<src->edge_share_line
        <<"), reason "<<(src->fallback_reason?*src->fallback_reason:"-")<<"; layer 2 "<<pc::ParallaxStateCode(pf.capture.parallax.state)<<", points input "<<pf.capture.parallax.summary.points_input
        <<" (band "<<pb.capture.parallax.summary.points_input<<"), max shift "<<pf.capture.parallax.summary.max_shift_px<<" px, level "<<ex::GeometryLevelCode(pf.capture.level.level)<<'\n';
    CHECK(src && src->fast_measurer_used && src->points_used>0 && !src->fallback_reason && src->edge_share && *src->edge_share<=src->edge_share_line &&
        src->edge_share_line==fast.design.fast_measurer.edge_share_line);
    CHECK(src && pf.capture.parallax.summary.points_input==src->points_used && pf.capture.parallax.Applied() && pf.capture.level.level==ex::GeometryLevel::g1);
    // The fast points move B by about what the band's did, on a flat sheet (the same truth).
    CHECK(std::abs(pf.capture.parallax.summary.max_shift_px-pb.capture.parallax.summary.max_shift_px)<0.5);
    // The band is still measured for the gate: the same anchors, and the gate's decision does not use the fast points.
    CHECK(pf.band && pb.band && pf.band->points.size()==pb.band->points.size() && pf.band->global.evaluated==pb.band->global.evaluated);
    // Threads 1 and 4: the same points, the same q, the same composite.
    auto fast1=fast;fast1.thread_count=1;
    auto p1=ex::PrepareCaptureRegistration(fast1);
    CHECK(p1.capture.point_source && src && p1.capture.point_source->points_used==src->points_used && SameRows(p1.capture.parallax,pf.capture.parallax) &&
        p1.composite.image.bgr==pf.composite.image.bgr);
    // The record of a capture that used the fast points is one the strict reader accepts, and it names what the capture chose.
    ex::MeasureCaptureRegistrationGate(fast,pf);
    {
        const auto record=ex::RecordOfCapture(Identity(),fast,pf,kHash);
        const auto text=ReportText(record);
        CHECK(Reads(text) && text.find("\"pointSource\":{\"fastMeasurerUsed\":true,\"pointsUsed\":"+std::to_string(src?src->points_used:0))!=std::string::npos);
        const auto back=ex::ReadFixedCompositeRegistrationReport(text,kHash);
        CHECK(back.registration.point_source && src && back.registration.point_source->points_used==src->points_used && ReportText(back)==text);
        // The default capture's record says null.
        ex::MeasureCaptureRegistrationGate(base,pb);
        const auto band_text=ReportText(ex::RecordOfCapture(Identity(),base,pb,kHash));
        CHECK(Reads(band_text) && band_text.find("\"pointSource\":null,")!=std::string::npos);
    }
    // Not passing the stops: the band's points, bit for bit the default's.
    const auto falls_back=[&](const char* reason,auto&& change){
        auto r=fast;change(r.design.fast_measurer);
        auto p=ex::PrepareCaptureRegistration(r);
        const auto& s=p.capture.point_source;
        const bool recorded=s && !s->fast_measurer_used && s->points_used==0 && s->fallback_reason && *s->fallback_reason==reason;
        return recorded && SameRows(p.capture.parallax,pb.capture.parallax) && p.composite.image.bgr==pb.composite.image.bgr && p.capture.level.level==pb.capture.level.level;
    };
    CHECK(falls_back("no-points",[](fsm::Design& d){d.content_level_dn=1e9;}));                  // nothing has content
    CHECK(falls_back("measurer-failed",[](fsm::Design& d){d.strip_margin_px=40;}));              // the measurement throws
    // The edge-share stop is covered on a sheet with a known misplacement in m2_fast_seam_measurer_contracts; the capture path here has no misplacement.
}

void Capture(){
    std::cout<<"capture path\n";
    const rig::Rig g;
    const auto a=rig::Raw(g.a,false),b=rig::Raw(g.b,false);
    // Threads 1 and 4: the same composite and the same report bytes.
    auto r1=Request(g,a,b,1),r4=Request(g,a,b,4);
    auto p1=ex::PrepareCaptureRegistration(r1);ex::MeasureCaptureRegistrationGate(r1,p1);
    auto p4=ex::PrepareCaptureRegistration(r4);ex::MeasureCaptureRegistrationGate(r4,p4);
    const auto rec1=ex::RecordOfCapture(Identity(),r1,p1,kHash),rec4=ex::RecordOfCapture(Identity(),r4,p4,kHash);
    std::cout<<"  level "<<ex::GeometryLevelCode(p1.capture.level.level)<<", estimate "<<cr::RegistrationStateCode(p1.capture.registration.state)
        <<", gate "<<rg::GateStateCode(p1.capture.gate.state)<<", assessment "<<ex::AssessmentStateText(rec1.assessment.state)<<'\n';
    CHECK(p1.composite.image.bgr==p4.composite.image.bgr && ReportText(rec1)==ReportText(rec4));
    CHECK(p1.capture.gate.state!=rg::GateState::not_run && rec1.seam_gate_json && Reads(ReportText(rec1)));
    CHECK(p1.composite.image.width==g.raster.width_pixels && p1.composite.image.height==g.raster.height_pixels);
    // B sits at its calibration and the sheet is flat: the estimate is trusted, layer 2 is applied (and moves B by almost
    // nothing), and the seam is within the line.
    CHECK(p1.capture.level.level==ex::GeometryLevel::g1 && p1.capture.registration.state==cr::RegistrationState::verified);
    CHECK(p1.capture.parallax.Applied() && p1.capture.parallax.summary.max_shift_px<0.5 && p1.capture.level.mapping.parallax);
    CHECK(p1.capture.surface.state==ex::SurfaceState::estimated && std::abs(p1.capture.surface.plane.h0_mm)<0.5);
    CHECK(p1.capture.gate.state==rg::GateState::pass);
    CHECK(std::abs(p1.baseline.x)>0.99 && p1.baseline_px_per_mm>0);
    // The gate measured on layer 2's rows (AlignGateOmegaToRows): every point of Omega is on one of them.
    {
        const auto& pr=p1.capture.parallax;
        bool on=p1.capture.gate.measurement && !p1.capture.gate.measurement->points.empty() && pr.row_step>0;
        for(const auto& p:p1.capture.gate.measurement->points)
            on=on && p.y>=pr.first_row && (p.y-pr.first_row)%pr.row_step==0 && (p.y-pr.first_row)/pr.row_step<pr.rows.size();
        CHECK(on);
    }
    // p = p_b = 0, pi = 0, q = 0 (G4): the composite is the v7 composite at scale 1, byte for byte.
    auto forced=Request(g,a,b,4);
    forced.cover_check=[inner=forced.cover_check](const ex::RegisteredMapping& m){auto c=inner(m);if(!m.IsPoseOnly() || !m.pose.IsZero())c.seam_covered=false;return c;};
    auto pf=ex::PrepareCaptureRegistration(forced);
    CHECK(pf.capture.level.level==ex::GeometryLevel::g4 && pf.capture.level.reasons==std::vector<std::string>(2,"seam-outside-overlap-at-level"));
    CHECK(pf.composite.image.bgr==V7Pixels(g,a,b,forced.estimator.geometry.seam_column));
    ex::MeasureCaptureRegistrationGate(forced,pf);
    const auto recf=ex::RecordOfCapture(Identity(),forced,pf,kHash);
    CHECK(Reads(ReportText(recf)) && HasCode(recf.warnings,ex::WarningCode::geometry_level));
    // No time at all: every stage is dropped, the composite is still made (at G4), the gate is not run.
    auto none=Request(g,a,b,4);none.remaining_ms=[]{return 0.0;};
    auto pn=ex::PrepareCaptureRegistration(none);ex::MeasureCaptureRegistrationGate(none,pn);
    const auto recn=ex::RecordOfCapture(Identity(),none,pn,kHash);
    CHECK(pn.capture.level.level==ex::GeometryLevel::g4 && pn.capture.gate.state==rg::GateState::not_run && pn.capture.time_budget.planned.empty());
    CHECK(pn.composite.image.bgr==V7Pixels(g,a,b,none.estimator.geometry.seam_column));
    CHECK(HasCode(recn.warnings,ex::WarningCode::gate_not_run) && HasCode(recn.warnings,ex::WarningCode::time_budget) && recn.assessment.state==ex::AssessmentState::not_judged);
    CHECK(Reads(ReportText(recn)) && !recn.seam_gate_json);
    // The estimator's stop margin is a time: finite and not negative.
    for(const double margin:{-1.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}){
        auto bad=Request(g,a,b,4);bad.design.estimator_stop_margin_ms=margin;
        CHECK(Throws([&]{(void)ex::PrepareCaptureRegistration(bad);}));
    }
    // Time for everything but the band measurement and the wide search: the gate runs on Omega's anchors.
    auto some=Request(g,a,b,4);
    const auto& k=some.design.costs;double sum=k.tail_reserve_ms;for(const double v:k.stage_ms)sum+=v;
    const double left=sum-k.stage_ms[0]-k.stage_ms[1];
    some.remaining_ms=[left]{return left;};
    auto ps=ex::PrepareCaptureRegistration(some);ex::MeasureCaptureRegistrationGate(some,ps);
    const auto recs=ex::RecordOfCapture(Identity(),some,ps,kHash);
    // Without the band, layer 2 has no points: it is dropped too, and the level is G2.
    CHECK(ps.capture.time_budget.skipped.size()==3 && ps.capture.time_budget.skipped[1].stage==ex::ReviewStage::band_measurement &&
        ps.capture.time_budget.skipped[2].stage==ex::ReviewStage::parallax && ps.capture.level.level==ex::GeometryLevel::g2);
    CHECK(ps.capture.gate.state!=rg::GateState::not_run && !ps.capture.gate.band && HasCode(recs.warnings,ex::WarningCode::time_budget) && Reads(ReportText(recs)));
    // The values of a stage that ran do not depend on the time: the same estimate, bit for bit.
    CHECK(std::memcmp(&ps.capture.registration.pose,&p1.capture.registration.pose,sizeof p1.capture.registration.pose)==0 &&
        ps.capture.surface.plane.h0_mm==p1.capture.surface.plane.h0_mm);
    // A blank sheet: nothing to estimate, G4, the v7 composite.
    const auto wa=rig::Raw(g.a,true),wb=rig::Raw(g.b,true);
    auto blank=Request(g,wa,wb,4);
    auto pb=ex::PrepareCaptureRegistration(blank);
    CHECK(pb.capture.level.level==ex::GeometryLevel::g4 && pb.composite.image.bgr==V7Pixels(g,wa,wb,blank.estimator.geometry.seam_column));
    ex::MeasureCaptureRegistrationGate(blank,pb);
    const auto recb=ex::RecordOfCapture(Identity(),blank,pb,kHash);
    CHECK(Reads(ReportText(recb)) && recb.assessment.state!=ex::AssessmentState::within_criteria);
}

// The split on the pixels (11.4, 11.5): B's pixels that only the verify rows' gate points read are changed (the content there moved
// 3 px across), and the band measured again with B placed by the pose and the plane gives layer 2 the same q on every row, bit for
// bit, while the gate's verify rows measured with the changed B see the change. "Only the verify rows read": inside each verify row's
// pixels (VerifyRowPixels) shrunk by the measurer's first search radius and 5 px (the feature smoothing, the interpolation and the
// two mappings' difference), each mapped into B by the final mapping with its 3 x 3 raw neighbours.
void SplitPixels(){
    std::cout<<"the split on the pixels\n";
    namespace sg=a0::m2::seam_gate;
    const rig::Rig g;
    const auto a=rig::Raw(g.a,false),b=rig::Raw(g.b,false);
    auto r=Request(g,a,b,4);
    auto p=ex::PrepareCaptureRegistration(r);ex::MeasureCaptureRegistrationGate(r,p);
    const auto& par=p.capture.parallax;
    CHECK(p.capture.level.level==ex::GeometryLevel::g1 && par.verify_period_rows==6 && par.verify_phase==0 && par.summary.points_excluded>0 && par.summary.rows_verify>0);
    // The gate decided on the verify rows only; the correct rows are its reference, never independent.
    bool verify=p.capture.gate.measurement.has_value() && !p.capture.gate.measurement->points.empty();
    if(verify)for(const auto& q:p.capture.gate.measurement->points)verify=verify && q.y>=par.first_row && ((q.y-par.first_row)/par.row_step)%6==0;
    CHECK(verify && p.capture.gate.corrected && p.capture.gate.corrected_measurement && p.capture.gate.rows.rows_corrected>0);
    // Layer 2's request as the capture path makes it.
    const ex::RegisteredMapping m0{p.capture.registration.pose,p.capture.surface.plane,std::nullopt};
    rg::RegistrationGateRequest gq;
    gq.camera_a=&a;gq.camera_b=&b;gq.mapped=ex::RegisteredDocumentMappedFixedSeamInput(g.parameters,m0,r.estimator.geometry.seam_column);gq.registered=m0;
    gq.layout=r.estimator.geometry;gq.blend=r.blend;gq.gate_design=r.design.omega;gq.gate_limits={r.lines.seam_limit_px,r.lines.seam_guard_band_px,"condition"};
    gq.design=r.design.gate;gq.baseline=p.baseline;gq.thread_count=4;
    const auto layout0=rg::MakeRegistrationGateLayout(gq);
    pc::ParallaxRequest q;
    q.rows.output_width=layout0.omega.width;q.rows.output_height=layout0.omega.height;q.rows.seam_column=par.seam_column;
    q.rows.omega_half_width_px=layout0.omega_half_width_px;q.rows.gate_footprint=pc::GateFootprint(r.design.omega,true);
    q.rows.first_row=par.first_row;q.rows.row_step=par.row_step;q.rows.row_count=static_cast<std::uint32_t>(par.rows.size());
    q.rows.verify_period_rows=par.verify_period_rows;q.rows.verify_phase=par.verify_phase;
    q.baseline=pc::MakeBaselineGeometry(g.parameters,p.capture.registration.pose,p.capture.surface.plane.h0_mm);
    q.design=r.design.parallax;q.max_lift_mm=r.lines.max_lift_mm;q.apply_beyond_line=r.lines.apply_beyond_envelope;q.thread_count=4;
    const auto band=[&](const a0::m2::render::BgrImage& camera_b){return sg::MeasureSeam(a,camera_b,layout0.band,r.design.band,gq.gate_limits,{},4);};
    const auto base_band=band(b);
    q.points=pc::PointsFromSeamMeasurement(base_band);
    const auto base=pc::EstimateParallax(q);
    const auto same_q=[](const pc::ParallaxResult& x,const pc::ParallaxResult& y){
        bool same=x.rows.size()==y.rows.size() && x.folds.size()==y.folds.size();
        for(std::size_t i=0;same && i<x.rows.size();++i)same=std::memcmp(&x.rows[i].q,&y.rows[i].q,sizeof(double))==0;
        return same;
    };
    CHECK(same_q(base,par));   // the request is the capture path's
    auto moved=b;
    const auto shrink=static_cast<std::int64_t>(std::max(r.design.band.search_radius_px,r.design.omega.search_radius_px))+5;
    std::size_t changed=0;
    const auto at=[&](long long u,long long w){return (static_cast<std::size_t>(w)*b.width+static_cast<std::size_t>(u))*3;};
    for(std::size_t i=par.verify_phase;i<par.rows.size();i+=par.verify_period_rows){
        const auto v=pc::VerifyRowPixels(q.rows,i);
        for(auto y=v.y0+shrink;y<v.y1-shrink;++y)for(auto x=v.x0+shrink;x<v.x1-shrink;++x){
            a0::m2::render::Point raw{};
            if(!p.mapped.camera_b({static_cast<double>(x),static_cast<double>(y)},raw))continue;
            const long long u0=std::lround(raw.x),w0=std::lround(raw.y);
            for(long long w=w0-1;w<=w0+1;++w)for(long long u=u0-1;u<=u0+1;++u){
                if(u<0 || w<0 || u+3>=static_cast<long long>(b.width) || w>=static_cast<long long>(b.height))continue;
                for(int k=0;k<3;++k)moved.bgr[at(u,w)+k]=b.bgr[at(u+3,w)+k];
                ++changed;
            }
        }
    }
    const auto moved_band=band(moved);
    // Every band point the change reached is one layer 2 leaves out (its footprint shares a pixel with a verify row).
    std::size_t differs=0,kept_differs=0;
    bool same_lattice=base_band.points.size()==moved_band.points.size();
    for(std::size_t i=0;same_lattice && i<base_band.points.size();++i){
        const auto& x=base_band.points[i];const auto& y=moved_band.points[i];
        same_lattice=x.x==y.x && x.y==y.y;
        if(x.status==y.status && x.Counted()==y.Counted() && std::memcmp(&x.displacement,&y.displacement,sizeof x.displacement)==0)continue;
        ++differs;
        const auto f=rg::GatePatchOf(x,r.design.band),f2=rg::GatePatchOf(y,r.design.band);
        bool touches=false;
        for(std::size_t k=par.verify_phase;k<par.rows.size() && !touches;k+=par.verify_period_rows){
            const auto v=pc::VerifyRowPixels(q.rows,k);
            touches=!rg::PatchesShareNoPixel(v,f) || !rg::PatchesShareNoPixel(v,f2);
        }
        kept_differs+=!touches;
    }
    CHECK(same_lattice && kept_differs==0);
    auto again=q;again.points=pc::PointsFromSeamMeasurement(moved_band);
    const auto after=pc::EstimateParallax(again);
    std::cout<<"  B's pixels changed "<<changed<<", band points whose value changed "<<differs<<", points left out "<<par.summary.points_excluded<<'\n';
    CHECK(changed>0 && differs>0);   // the change reaches the band's points near the verify rows (they are left out)
    CHECK(same_q(base,after));
    // The verify rows see it: the decided layout (aligned and split as the capture path does) measured with the changed B.
    auto gf=gq;gf.mapped=p.mapped;
    auto layout=rg::MakeRegistrationGateLayout(gf);
    (void)ex::AlignGateOmegaToRows(layout,par.first_row,par.row_step,q.rows.row_count,r.design.omega.point_spacing_px);
    (void)rg::SplitGateOmega(layout,par.first_row,par.row_step,q.rows.row_count,par.verify_period_rows,par.verify_phase,r.design.omega.point_spacing_px);
    const auto v_base=sg::MeasureSeam(a,b,layout.omega,r.design.omega,gq.gate_limits,{},4),v_moved=sg::MeasureSeam(a,moved,layout.omega,r.design.omega,gq.gate_limits,{},4);
    bool reproduced=v_base.points.size()==p.capture.gate.measurement->points.size();
    for(std::size_t i=0;reproduced && i<v_base.points.size();++i)
        reproduced=v_base.points[i].y==p.capture.gate.measurement->points[i].y && v_base.points[i].x==p.capture.gate.measurement->points[i].x &&
            std::memcmp(&v_base.points[i].displacement,&p.capture.gate.measurement->points[i].displacement,sizeof v_base.points[i].displacement)==0;
    double largest=0;std::size_t status=0;
    for(std::size_t i=0;i<v_base.points.size() && i<v_moved.points.size();++i){
        const auto& x=v_base.points[i];const auto& y=v_moved.points[i];
        if(x.Counted()!=y.Counted() || x.status!=y.status){++status;continue;}
        if(x.Counted())largest=std::max(largest,std::hypot(x.displacement.x-y.displacement.x,x.displacement.y-y.displacement.y));
    }
    std::cout<<"  the verify rows: points whose status changed "<<status<<", the largest change of a point counted both times "<<largest<<" px\n";
    CHECK(reproduced);
    CHECK(status>0 || largest>1);
}

// #417: a stage that throws is dropped and the capture goes on one level lower; the composite is made in every case and the
// warnings say which stage failed (the owner's decision 1).
bool Interprets(const std::vector<ex::RegistrationWarning>& v,ex::WarningCode code,const std::string& reading){
    for(const auto& w:v)if(w.code==code && std::find(w.interpretations.begin(),w.interpretations.end(),reading)!=w.interpretations.end())return true;
    return false;
}
bool SkippedFor(const ex::TimeBudgetRecord& t,ex::ReviewStage stage,const std::string& reason){
    for(const auto& s:t.skipped)if(s.stage==stage)return s.reason==reason;
    return false;
}
void StageFailures(){
    std::cout<<"a stage that throws is dropped\n";
    const rig::Rig g;
    const auto a=rig::Raw(g.a,false),b=rig::Raw(g.b,false);
    // The estimate throws (a limit the estimator refuses): G4, the v7 composite, the gate still runs on it.
    {
        auto r=Request(g,a,b,4);r.estimator.limits.convergence_px=0;
        ex::PreparedCaptureRegistration p;
        CHECK(!Throws([&]{p=ex::PrepareCaptureRegistration(r);}));
        CHECK(p.capture.level.level==ex::GeometryLevel::g4 && !p.capture.registration.has_estimate &&
            p.capture.registration.reasons==std::vector<std::string>{"estimate-failed"});
        CHECK(p.composite.image.bgr==V7Pixels(g,a,b,r.estimator.geometry.seam_column));
        CHECK(SkippedFor(p.capture.time_budget,ex::ReviewStage::fine_and_fit,"stage-failed") && SkippedFor(p.capture.time_budget,ex::ReviewStage::coarse,"stage-failed"));
        ex::MeasureCaptureRegistrationGate(r,p);
        const auto rec=ex::RecordOfCapture(Identity(),r,p,kHash);
        CHECK(p.capture.gate.state!=rg::GateState::not_run && Interprets(rec.warnings,ex::WarningCode::geometry_level,"estimate-failed"));
        // Not a stage dropped for the time.
        CHECK(!HasCode(rec.warnings,ex::WarningCode::time_budget) && Reads(ReportText(rec)));
    }
    // Layer 2 throws (Omega's half width, 8 px here, above layer 2's a): G2 on the trusted estimate, the gate still runs.
    {
        auto r=Request(g,a,b,4);r.design.parallax.weight_flat_px=7;
        ex::PreparedCaptureRegistration p;
        CHECK(!Throws([&]{p=ex::PrepareCaptureRegistration(r);}));
        CHECK(p.capture.registration.state==cr::RegistrationState::verified && p.capture.level.level==ex::GeometryLevel::g2 && !p.capture.level.mapping.parallax);
        CHECK(!p.capture.parallax.Applied() && p.capture.parallax.reasons==std::vector<std::string>{"layer-two-failed"});
        CHECK(SkippedFor(p.capture.time_budget,ex::ReviewStage::parallax,"stage-failed") &&
            std::find(p.capture.time_budget.planned.begin(),p.capture.time_budget.planned.end(),ex::ReviewStage::band_measurement)!=p.capture.time_budget.planned.end());
        CHECK(p.composite.image.width==g.raster.width_pixels && p.composite.image.height==g.raster.height_pixels);
        ex::MeasureCaptureRegistrationGate(r,p);
        const auto rec=ex::RecordOfCapture(Identity(),r,p,kHash);
        CHECK(p.capture.gate.state!=rg::GateState::not_run && Interprets(rec.warnings,ex::WarningCode::geometry_level,"layer-two-failed"));
        CHECK(!HasCode(rec.warnings,ex::WarningCode::time_budget) && Reads(ReportText(rec)));
    }
    // The gate throws (a design constant it refuses, set after the composite was drawn): the composite stands, not judged.
    {
        auto r=Request(g,a,b,4);
        auto p=ex::PrepareCaptureRegistration(r);
        const auto composite=p.composite.image.bgr;const auto level=p.capture.level.level;
        r.design.gate.maximum_unmeasured_content_share=1.5;
        CHECK(!Throws([&]{ex::MeasureCaptureRegistrationGate(r,p);}));
        CHECK(p.capture.gate.state==rg::GateState::not_run && p.composite.image.bgr==composite && p.capture.level.level==level);
        CHECK(SkippedFor(p.capture.time_budget,ex::ReviewStage::gate,"stage-failed") && SkippedFor(p.capture.time_budget,ex::ReviewStage::wide_search_diagnostic,"stage-failed"));
        const auto rec=ex::RecordOfCapture(Identity(),r,p,kHash);
        CHECK(Interprets(rec.warnings,ex::WarningCode::gate_not_run,"gate-failed") && rec.assessment.state==ex::AssessmentState::not_judged);
        CHECK(!HasCode(rec.warnings,ex::WarningCode::time_budget) && Reads(ReportText(rec)) && !rec.seam_gate_json);
        // The reader takes stage-failed and time-budget only.
        auto text=ReportText(rec);
        const auto at=text.find("\"reason\":\"stage-failed\"");
        CHECK(at!=std::string::npos);
        if(at!=std::string::npos)CHECK(!Reads(text.replace(at,std::string("\"reason\":\"stage-failed\"").size(),"\"reason\":\"stage-broke\"")));
    }
    // Out of memory is not a stage's failure: it goes to the caller.
    {
        auto r=Request(g,a,b,4);
        auto p=ex::PrepareCaptureRegistration(r);
        r.remaining_ms=[]()->double{throw std::bad_alloc();};
        bool passed_on=false;
        try{ex::MeasureCaptureRegistrationGate(r,p);}catch(const std::bad_alloc&){passed_on=true;}
        CHECK(passed_on);
    }
}
}

int main(){
    try{
        RoundTrip();
        Tamper();
        PointSourceReport();
        Derivations();
        Hash();
        Components();
        Fixture();
        Plan();
        Levels();
        AlignToLayerTwo();
        MeasuredBand();
        BandStageAnchors();
        PointSource();
        Capture();
        SplitPixels();
        StageFailures();
    }catch(const std::exception& error){std::cerr<<"unexpected exception: "<<error.what()<<'\n';return 1;}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
