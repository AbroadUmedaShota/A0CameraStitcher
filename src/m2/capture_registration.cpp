#include "a0/m2/experimental/capture_registration.hpp"

#include "a0/common/protocol_json.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iomanip>
#include <locale>
#include <new>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace a0::m2::experimental {
namespace {
namespace cr=content_registration;
namespace pc=parallax_correction;
namespace rg=registration_gate;
namespace sg=seam_gate;
namespace json=a0::common::protocol_json;

// ---- Writing ----
// Numbers as the formats document says: 17 significant digits, classic locale, -0 as 0.
std::string Num(double v){
    if(!std::isfinite(v))throw std::logic_error("a report number is not finite");
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<(v==0?0.0:v);return out.str();
}
std::string Int(std::uint64_t v){return std::to_string(v);}
std::string Text(std::string_view v){return "\""+json::JsonEscape(v)+"\"";}
std::string Bool(bool v){return v?"true":"false";}
std::string OptNum(const std::optional<double>& v){return v?Num(*v):"null";}
std::string Strings(const std::vector<std::string>& v){
    std::string s="[";for(std::size_t i=0;i<v.size();++i)s+=(i?",":"")+Text(v[i]);return s+"]";
}
std::string PointJson(const render::Point& p){return "{\"x\":"+Num(p.x)+",\"y\":"+Num(p.y)+"}";}
std::string PoseJson(const cr::PoseParameters& p){
    return "{\"rxDeg\":"+Num(p.rx_deg)+",\"ryDeg\":"+Num(p.ry_deg)+",\"rzDeg\":"+Num(p.rz_deg)+",\"txMm\":"+Num(p.tx_mm)+
        ",\"tyMm\":"+Num(p.ty_mm)+",\"tzPercent\":"+Num(p.tz_percent)+"}";
}
std::string_view SurfaceStateCode(SurfaceState s){
    return s==SurfaceState::estimated?"estimated":s==SurfaceState::not_estimated?"not-estimated":"disabled";
}
std::string RowRangesJson(const std::vector<rg::RowRange>& v){
    std::string s="[";for(std::size_t i=0;i<v.size();++i)s+=std::string(i?",":"")+"{\"first\":"+Int(v[i].first)+",\"last\":"+Int(v[i].last)+"}";return s+"]";
}
std::string FindingsJson(const std::vector<std::pair<std::string,std::string>>& v){
    std::string s="[";for(std::size_t i=0;i<v.size();++i)s+=std::string(i?",":"")+"{\"reason\":"+Text(v[i].first)+",\"remedy\":"+Text(v[i].second)+"}";return s+"]";
}
std::string QuantitiesJson(const rg::GateQuantities& q){
    return "{\"maximumPx\":"+OptNum(q.maximum_px)+",\"twoDirectionalPx\":"+OptNum(q.maxima.two_directional_px)+",\"oneDirectionalPx\":"+OptNum(q.maxima.one_directional_px)+"}";
}
std::string CanonicalText(const std::string& text);
bool GateRan(const rg::RegistrationGateResult& g){return g.state!=rg::GateState::not_run;}
// Why a stage is in timeBudget.skipped: the time plan dropped it, or it threw and the capture went on without it (the composite
// is made in every case, the owner's decision 1).
constexpr std::string_view kTimeBudgetReason="time-budget",kStageFailedReason="stage-failed";
// What a stage that threw leaves in its own record (registration.reasons, parallax.reasons) and in the interpretations of the
// warning that says the level or the gate is short of what was planned.
constexpr std::string_view kEstimateFailed="estimate-failed",kLayerTwoFailed="layer-two-failed",kGateFailed="gate-failed";
// The fast measurer threw (LayerTwoPointSource::fallback_reason); the other codes are fast_seam_measurer::Selection's.
constexpr std::string_view kMeasurerFailed="measurer-failed";
bool HasText(const std::vector<std::string>& v,std::string_view text){return std::find(v.begin(),v.end(),text)!=v.end();}
bool StageFailed(const TimeBudgetRecord& t,ReviewStage stage){
    return std::any_of(t.skipped.begin(),t.skipped.end(),[&](const SkippedStage& s){return s.stage==stage && s.reason==kStageFailedReason;});
}

std::string LayoutJson(const cr::RegistrationGeometry& g){
    return "{\"mixingWidthPx\":"+Int(g.mixing_width_px)+",\"omegaMarginPx\":"+Int(g.omega_margin_px)+",\"guardPx\":"+Int(g.guard_px)+
        ",\"bandHalfWidthPx\":"+Int(g.band_half_width_px)+"}";
}
std::string LimitsJson(const RegistrationLines& l){
    std::string s="{\"envelope\":{\"positionMm\":"+Num(l.envelope.position_mm)+",\"rotationDeg\":"+Num(l.envelope.rotation_deg)+",\"tiltDeg\":"+
        Num(l.envelope.tilt_deg)+",\"heightPercent\":"+Num(l.envelope.height_percent)+"},\"surfaceEnvelope\":";
    s+=l.surface_envelope?"{\"heightMm\":"+Num(l.surface_envelope->height_mm)+",\"tiltDeg\":"+Num(l.surface_envelope->tilt_deg)+"}":std::string("null");
    if(!l.apply_beyond_envelope)throw std::logic_error("the report's lines have no applyBeyondEnvelope");
    s+=",\"applyBeyondEnvelope\":"+Bool(*l.apply_beyond_envelope)+",\"surfaceScaleWarnPercent\":"+OptNum(l.surface_scale_warn_percent)+
        ",\"maxLiftMm\":"+OptNum(l.max_lift_mm)+",\"seamGate\":{\"limitPx\":"+Num(l.seam_limit_px)+",\"guardBandPx\":"+Num(l.seam_guard_band_px)+"}}";
    return s;
}
bool LevelAppliesEstimate(GeometryLevel l){return l==GeometryLevel::g1 || l==GeometryLevel::g2;}
std::string OutputFrameJson(const std::optional<OutputFrameDisplay>& f){
    if(!f)return "null";
    return "{\"cameraMotionMm\":"+PointJson(f->camera_motion_mm)+",\"lensDirection\":"+PointJson(f->lens_direction)+",\"lensTiltDeg\":"+Num(f->lens_tilt_deg)+
        ",\"rotationClockwiseDeg\":"+Num(f->rotation_clockwise_deg)+",\"surfaceHighSide\":"+PointJson(f->surface_high_side)+
        ",\"baselineEquivalentMotionMm\":"+PointJson(f->baseline_equivalent_motion_mm)+"}";
}
std::string RegistrationJson(const cr::RegistrationResult& r,GeometryLevel level){
    std::string s="{\"state\":"+Text(cr::RegistrationStateCode(r.state))+",\"reasons\":"+Strings(r.reasons)+",\"hasEstimate\":"+Bool(r.has_estimate)+
        ",\"pose\":"+(r.has_estimate?PoseJson(r.pose):std::string("null"))+",\"poseSigma\":"+(r.has_estimate?PoseJson(r.pose_sigma):std::string("null"))+",\"covariance\":";
    if(r.has_estimate){s+="[";for(std::size_t i=0;i<r.covariance.size();++i)s+=(i?",":"")+Num(r.covariance[i]);s+="]";}else s+="null";
    s+=",\"components\":[";
    for(std::size_t i=0;i<r.components.size();++i){const auto& c=r.components[i];
        s+=std::string(i?",":"")+"{\"component\":"+Text(cr::EnvelopeComponentCode(c.component))+",\"valueFromBase\":"+Num(c.value)+",\"limit\":"+Num(c.limit)+",\"exceeded\":"+Bool(c.exceeded)+"}";}
    const auto& d=r.display;
    s+="],\"display\":{\"positionMm\":"+Num(d.position_mm)+",\"txMm\":"+Num(d.tx_mm)+",\"tyMm\":"+Num(d.ty_mm)+",\"rotationDeg\":"+Num(d.rotation_deg)+
        ",\"tiltDeg\":"+Num(d.tilt_deg)+",\"rxDeg\":"+Num(d.rx_deg)+",\"ryDeg\":"+Num(d.ry_deg)+",\"heightPercent\":"+Num(d.height_percent)+",\"heightMm\":"+Num(d.height_mm)+
        ",\"outputFrame\":"+OutputFrameJson(d.output_frame)+"},\"applied\":"+Bool(LevelAppliesEstimate(level))+",\"design\":{},\"diagnostics\":{\"counts\":{\"outerIterations\":"+Int(r.diagnostics.outer_iterations)+
        ",\"points\":"+Int(r.diagnostics.points)+",\"inliers\":"+Int(r.diagnostics.inliers)+",\"gatePoints\":"+Int(r.diagnostics.gate_points)+
        ",\"uniqueBlocks\":"+Int(r.diagnostics.unique_blocks)+",\"consistentBlocks\":"+Int(r.diagnostics.consistent_blocks)+"}}}";
    return s;
}
std::string SurfaceJson(const SurfaceEstimate& f){
    const bool e=f.state==SurfaceState::estimated;
    const auto v=[&](double x){return e?Num(x):std::string("null");};
    std::string s="{\"state\":"+Text(SurfaceStateCode(f.state))+",\"heightMm\":"+v(f.plane.h0_mm)+",\"slopeX\":"+v(f.plane.slope_x)+",\"slopeY\":"+v(f.plane.slope_y)+
        ",\"tiltDeg\":"+v(f.tilt_deg)+",\"covariance\":";
    if(e){s+="[";for(std::size_t i=0;i<f.covariance.size();++i)s+=(i?",":"")+Num(f.covariance[i]);s+="]";}else s+="null";
    s+=",\"baselineEquivalentMm\":"+v(f.baseline_equivalent_mm)+",\"scale\":";
    s+=e?"{\"factor\":"+Num(f.scale.factor)+",\"minimum\":"+Num(f.scale.minimum)+",\"maximum\":"+Num(f.scale.maximum)+"}":std::string("null");
    s+=",\"exceeded\":{\"height\":"+Bool(f.exceeded.height)+",\"tilt\":"+Bool(f.exceeded.tilt)+"},\"applied\":"+Bool(f.applied)+"}";
    return s;
}
std::string ParallaxJson(const pc::ParallaxResult& p,GeometryLevel level){
    const bool values=!p.rows.empty();
    std::string s="{\"state\":"+Text(pc::ParallaxStateCode(p.state))+",\"reasons\":"+Strings(p.reasons)+",\"baseline\":"+(values?PointJson(p.baseline):std::string("null"))+
        ",\"kPxPerMm\":"+(values?Num(p.k_px_per_mm):std::string("null"))+",\"design\":";
    if(values){const auto& d=p.design;
        s+="{\"columnSpacingPx\":"+Num(d.column_spacing_px)+",\"columnRadiusPx\":"+Num(d.column_radius_px)+",\"pointPatchPx\":"+Int(d.point_patch_px)+
            ",\"weightFlatPx\":"+Num(d.weight_flat_px)+",\"weightFalloffPx\":"+Num(d.weight_falloff_px)+",\"maximumStrain\":"+Num(d.maximum_strain)+
            ",\"foldNearSeamPx\":"+Num(d.fold_near_seam_px)+"}";
    }else s+="{}";
    s+=",\"grid\":"+(values?"{\"seamColumn\":"+Int(p.seam_column)+",\"firstRow\":"+Int(p.first_row)+",\"rowStep\":"+Int(p.row_step)+"}":std::string("null"))+",\"split\":";
    if(values){const auto& m=p.summary;
        s+="{\"mode\":"+Text(p.verify_period_rows?"split":"hole")+",\"verifyPeriodRows\":"+Int(p.verify_period_rows)+",\"verifyPhase\":"+Int(p.verify_phase)+
            ",\"rowsVerify\":"+Int(m.rows_verify)+",\"rowsCorrect\":"+Int(m.rows_correct)+",\"rowsWithHole\":"+Int(m.rows_with_hole)+",\"pointsExcluded\":"+Int(m.points_excluded)+"}";
    }else s+="null";
    s+=",\"profile\":[";
    for(std::size_t i=0;i<p.rows.size();++i){const auto& r=p.rows[i];
        s+=std::string(i?",":"")+"{\"y\":"+Num(r.y)+",\"qPx\":"+Num(r.q)+",\"sigmaPx\":"+Num(r.sigma_q)+",\"nEff\":"+Num(r.n_eff)+",\"applied\":"+Bool(r.applied)+
            ",\"points\":"+Int(r.points)+",\"liftMm\":"+Num(r.lift_mm)+",\"state\":"+Text(pc::ParallaxRowStateCode(r.state))+",\"reason\":"+(r.reason.empty()?std::string("null"):Text(r.reason))+
            ",\"role\":"+Text(pc::ParallaxRowRoleCode(r.role))+",\"hole\":"+Bool(r.hole)+"}";}
    s+="],\"summary\":";
    if(values){const auto& m=p.summary;
        s+="{\"maxShiftPx\":"+Num(m.max_shift_px)+",\"maxShiftMm\":"+Num(m.max_shift_mm)+",\"atRow\":"+Num(m.at_row)+",\"maxStrain\":"+Num(m.max_strain)+
            ",\"columns\":{\"first\":"+Int(m.first_column)+",\"last\":"+Int(m.last_column)+"},\"points\":"+Int(m.points)+",\"pointsInput\":"+Int(m.points_input)+
            ",\"pointsBaseline\":"+Int(m.points_baseline)+",\"rowsMeasured\":"+Int(m.rows_measured)+",\"rowsFilled\":"+Int(m.rows_filled)+",\"holeXPx\":"+Num(m.hole_x_px)+
            ",\"holeYPx\":"+Num(m.hole_y_px)+",\"pointsCrossValidated\":"+Int(m.points_cross_validated)+",\"foldCandidates\":"+Int(m.fold_candidates)+
            ",\"foldResidualScalePx\":"+Num(m.fold_residual_scale_px)+"}";
    }else s+="null";
    s+=",\"lift\":";
    if(values){const auto& f=p.lift;
        s+="{\"limitMm\":"+(std::isfinite(f.limit_mm)?Num(f.limit_mm):std::string("null"))+",\"maximumMm\":"+Num(f.maximum_mm)+",\"atRow\":"+Num(f.at_row)+
            ",\"exceeded\":"+Bool(f.exceeded)+",\"exceededRows\":"+Int(f.exceeded_rows)+",\"firstExceededRow\":"+Num(f.first_exceeded_row)+",\"lastExceededRow\":"+Num(f.last_exceeded_row)+"}";
    }else s+="null";
    return s+",\"applied\":"+Bool(level==GeometryLevel::g1)+"}";
}
std::string FoldsJson(const std::vector<pc::FoldLine>& v){
    std::string s="[";
    for(std::size_t i=0;i<v.size();++i){const auto& f=v[i];
        s+=std::string(i?",":"")+"{\"kind\":"+Text(pc::FoldLineKindCode(f.kind))+",\"positionPx\":"+Num(f.position_px)+",\"angleDeg\":"+Num(f.angle_deg)+
            ",\"stepSign\":"+(f.step_sign<0?std::string("-1"):Int(static_cast<std::uint64_t>(f.step_sign)))+",\"stepPx\":"+Num(f.step_px)+",\"stepMm\":"+Num(f.step_mm)+
            ",\"distanceFromSeamPx\":"+Num(f.distance_from_seam_px)+",\"crossesSeam\":"+Bool(f.crosses_seam)+",\"first\":"+PointJson(f.first)+",\"last\":"+PointJson(f.last)+
            ",\"lengthPx\":"+Num(f.length_px)+",\"points\":"+Int(f.points)+",\"nearSeam\":"+Bool(f.near_seam)+",\"shiftToClearPx\":"+OptNum(f.shift_to_clear_px)+"}";}
    return s+"]";
}
// Layer 2's point source (formats 4.8.1): null for the band's points, else what the capture chose.
std::string PointSourceJson(const std::optional<LayerTwoPointSource>& p){
    if(!p)return "null";
    return "{\"fastMeasurerUsed\":"+Bool(p->fast_measurer_used)+",\"pointsUsed\":"+Int(p->points_used)+",\"edgeShare\":"+OptNum(p->edge_share)+
        ",\"edgeShareLine\":"+Num(p->edge_share_line)+",\"fallbackReason\":"+(p->fallback_reason?Text(*p->fallback_reason):std::string("null"))+"}";
}
// What the record's point source must satisfy against the rest of the record (the reader and the writer check the same). An empty
// string: consistent; else the first thing that is not.
const char* PointSourceFault(const CaptureRegistration& c){
    if(!c.point_source)return "";
    const auto& p=*c.point_source;
    if(!(p.edge_share_line>=0 && p.edge_share_line<=1) || (p.edge_share && !(*p.edge_share>=0 && *p.edge_share<=1)))return "point source range";
    if(p.fast_measurer_used!=!p.fallback_reason)return "point source use and reason";
    if(p.fast_measurer_used){
        if(!p.points_used || !p.edge_share || *p.edge_share>p.edge_share_line)return "point source used";
        if(c.parallax.rows.empty() || c.parallax.summary.points_input!=p.points_used)return "point source points";
        return "";
    }
    if(p.points_used)return "point source points not used";
    const auto& why=*p.fallback_reason;
    if(why=="edge-share")return p.edge_share && *p.edge_share>p.edge_share_line?"":"point source edge share";
    if(why=="no-points")return p.edge_share && *p.edge_share<=p.edge_share_line?"":"point source no points";
    if(why==kMeasurerFailed)return p.edge_share?"point source failure":"";
    return "point source reason";
}
std::string MappingJson(const RegisteredMapping& m){
    std::string s="{\"pose\":"+PoseJson(m.pose)+",\"surface\":{\"h0Mm\":"+Num(m.surface.h0_mm)+",\"slopeX\":"+Num(m.surface.slope_x)+",\"slopeY\":"+Num(m.surface.slope_y)+"},\"parallax\":";
    if(m.parallax){const auto& p=*m.parallax;
        s+="{\"baseline\":"+PointJson(p.baseline)+",\"rowSpacingPx\":"+Num(p.row_spacing_px)+",\"firstRowPx\":"+Num(p.first_row_px)+",\"qPx\":[";
        for(std::size_t i=0;i<p.q_px.size();++i)s+=(i?",":"")+Num(p.q_px[i]);
        s+="],\"aPx\":"+Num(p.a_px)+",\"lPx\":"+Num(p.l_px)+",\"seamColumn\":"+Int(p.seam_column)+"}";
    }else s+="null";
    return s+"}";
}
std::string LevelJson(const GeometryLevelDecision& d,const std::optional<UniformShift>& shift){
    std::string s="{\"level\":"+Text(GeometryLevelCode(d.level))+",\"reasons\":"+Strings(d.reasons)+",\"applied\":{\"pose\":"+Bool(d.applied.pose)+",\"surface\":"+
        Bool(d.applied.surface)+",\"parallax\":"+Bool(d.applied.parallax)+"},\"beyondEnvelopeApplied\":"+Bool(d.beyond_envelope_applied)+",\"mapping\":"+MappingJson(d.mapping)+",\"coarseShift\":";
    s+=shift?"{\"surfaceHeightMm\":"+Num(shift->surface_height_mm)+",\"translationXMm\":"+Num(shift->translation_x_mm)+",\"translationYMm\":"+Num(shift->translation_y_mm)+"}":std::string("null");
    return s+"}";
}
std::string GateJson(const rg::RegistrationGateResult& g,const std::optional<std::string>& seam_gate_json){
    const bool ran=GateRan(g);
    std::vector<std::pair<std::string,std::string>> findings,own;
    for(const auto& f:g.findings)findings.emplace_back(f.reason,f.remedy);
    for(const auto& f:g.gate_findings)own.emplace_back(f.reason,f.remedy);
    std::string s="{\"state\":"+Text(rg::GateStateCode(g.state))+",\"findings\":"+FindingsJson(findings)+",\"gateFindings\":"+FindingsJson(own)+
        ",\"cause\":"+Text(rg::GateCauseCode(g.cause))+",\"remedy\":"+Text(g.remedy)+",\"lift\":";
    if(g.lift)s+="{\"pointsExceeding\":"+Int(g.lift->points_exceeding)+",\"baselineShare\":"+Num(g.lift->baseline_share)+",\"signAgreementShare\":"+
        Num(g.lift->sign_agreement_share)+",\"heightEstimateMm\":"+OptNum(g.lift->height_estimate_mm)+"}";
    else s+="null";
    s+=",\"rows\":{\"dxMeasuredShare\":"+(ran?Num(g.rows.dx_measured_share):std::string("null"))+",\"rowsWithContent\":"+Int(g.rows.rows_with_content)+
        ",\"rowsDxMeasured\":"+Int(g.rows.rows_dx_measured)+",\"unmeasured\":"+RowRangesJson(g.rows.unmeasured)+",\"outsideMeasurable\":"+RowRangesJson(g.rows.outside_measurable)+
        ",\"rowsBlank\":"+Int(g.rows.rows_blank)+",\"rowsMeasured\":"+Int(g.rows.rows_measured)+",\"rowsContentUnmeasured\":"+Int(g.rows.rows_content_unmeasured)+
        ",\"rowsOneSided\":"+Int(g.rows.rows_one_sided)+",\"rowsUnconfirmed\":"+Int(g.rows.rows_unconfirmed)+
        ",\"contentUnmeasuredShare\":"+(ran?Num(g.rows.content_unmeasured_share):std::string("null"))+",\"blank\":"+RowRangesJson(g.rows.blank)+
        ",\"contentUnmeasured\":"+RowRangesJson(g.rows.content_unmeasured)+",\"rowsCorrected\":"+Int(g.rows.rows_corrected)+",\"corrected\":"+RowRangesJson(g.rows.corrected)+"}";
    s+=",\"maxima\":{\"twoDirectionalPx\":"+OptNum(g.maxima.two_directional_px)+",\"oneDirectionalPx\":"+OptNum(g.maxima.one_directional_px)+",\"oneDirectionalNormal\":"+
        (g.maxima.one_directional_normal?PointJson(*g.maxima.one_directional_normal):std::string("null"))+",\"oneDirectionalComponent\":"+
        (g.maxima.one_directional_component?Text(rg::OneDirectionalComponentCode(*g.maxima.one_directional_component)):std::string("null"))+"}";
    s+=",\"before\":"+QuantitiesJson(g.before)+",\"after\":"+QuantitiesJson(g.after)+",\"corrected\":";
    if(g.corrected){const auto& k=*g.corrected;
        s+="{\"maximumPx\":"+OptNum(k.after.maximum_px)+",\"twoDirectionalPx\":"+OptNum(k.after.maxima.two_directional_px)+",\"oneDirectionalPx\":"+OptNum(k.after.maxima.one_directional_px)+
            ",\"beforeMaximumPx\":"+OptNum(k.before.maximum_px)+",\"rowsWithContent\":"+Int(k.rows_with_content)+",\"rowsMeasured\":"+Int(k.rows_measured)+",\"independent\":false}";
    }else s+="null";
    s+=",\"anchors\":{\"range\":";
    s+=g.anchor_columns?"{\"firstColumn\":"+Int(g.anchor_columns->first)+",\"lastColumn\":"+Int(g.anchor_columns->second)+"}":std::string("null");
    s+="},\"placement\":{\"verifiedBy\":"+Text(rg::PlacementBasisCode(g.placement))+"},\"wideSearch\":";
    if(g.wide_search){const auto& w=*g.wide_search;
        s+="{\"radiusPx\":"+Int(w.radius_px)+",\"blocks\":"+Int(w.blocks)+",\"uniqueBlocks\":"+Int(w.unique_blocks)+",\"agreeingBlocks\":"+Int(w.agreeing_blocks)+
            ",\"offsetPx\":"+OptNum(w.offset_px)+",\"offsetMm\":"+OptNum(w.offset_mm)+",\"rows\":"+
            (w.rows?"{\"first\":"+Int(w.rows->first)+",\"last\":"+Int(w.rows->last)+"}":std::string("null"))+"}";
    }else s+="null";
    s+=",\"design\":";
    if(ran){const auto& d=g.design;
        s+="{\"minimumBaselineComponent\":"+Num(d.minimum_baseline_component)+",\"liftRatio\":"+Num(d.lift_ratio)+",\"liftShare\":"+Num(d.lift_share)+
            ",\"wideSearchRadiusPx\":"+Int(d.wide_search_radius_px)+",\"wideSearchHalfWidthPx\":"+Int(d.wide_search_half_width_px)+",\"wideSearchBlockRows\":"+Int(d.wide_search_block_rows)+
            ",\"wideSearchMinimumCorrelation\":"+Num(d.wide_search_minimum_correlation)+",\"wideSearchSecondaryRatio\":"+Num(d.wide_search_secondary_ratio)+
            ",\"wideSearchConsistencyPx\":"+Num(d.wide_search_consistency_px)+",\"wideSearchMinimumBlocks\":"+Int(d.wide_search_minimum_blocks)+
            ",\"wideSearchTriggerShare\":"+Num(d.wide_search_trigger_share)+",\"maximumUnmeasuredContentShare\":"+Num(d.maximum_unmeasured_content_share)+"}";
    }else s+="{}";
    s+=",\"geometryHash\":"+Text(g.geometry_hash)+",\"seamGate\":"+(seam_gate_json?CanonicalText(*seam_gate_json):std::string("null"))+"}";
    return s;
}
std::string TimeBudgetJson(const TimeBudgetRecord& t){
    std::string s="{\"plannedStages\":[";
    for(std::size_t i=0;i<t.planned.size();++i)s+=(i?",":"")+Text(ReviewStageText(t.planned[i]));
    s+="],\"skipped\":[";
    for(std::size_t i=0;i<t.skipped.size();++i)s+=std::string(i?",":"")+"{\"stage\":"+Text(ReviewStageText(t.skipped[i].stage))+",\"reason\":"+Text(t.skipped[i].reason)+"}";
    s+="],\"costModel\":{\"stageMs\":{";
    for(std::size_t i=0;i<kReviewStageTexts.size();++i)s+=std::string(i?",":"")+Text(kReviewStageTexts[i].second)+":"+Num(t.cost_model.stage_ms[i]);
    return s+"},\"tailReserveMs\":"+Num(t.cost_model.tail_reserve_ms)+"}}";
}
std::string RecordJsonUnchecked(const SavedFixedCompositeRegistration& r){
    const auto& g=r.raster.region_um;const auto& c=r.registration;
    std::string s="{\"condition\":{\"sha256\":"+Text(r.condition_sha256)+",\"sequence\":"+Int(r.condition_sequence)+"},\"profile\":{\"file\":"+Text(r.profile_file)+
        ",\"fileSha256\":"+Text(r.profile_file_sha256)+",\"fingerprintSha256\":"+Text(r.profile_fingerprint_sha256)+",\"profileId\":"+(r.profile_id?Text(*r.profile_id):std::string("null"))+
        "},\"raster\":{\"regionUm\":{\"left\":"+std::to_string(g.left)+",\"top\":"+std::to_string(g.top)+",\"right\":"+std::to_string(g.right)+",\"bottom\":"+std::to_string(g.bottom)+
        "},\"dpi\":"+Int(r.raster.dpi)+",\"widthPixels\":"+Int(r.raster.width_pixels)+",\"heightPixels\":"+Int(r.raster.height_pixels)+"},\"seam\":{\"column\":"+Int(r.seam_column)+
        ",\"origin\":"+Text(r.seam_origin)+"},\"layout\":"+LayoutJson(r.layout)+",\"basePose\":"+PoseJson(r.base_pose)+",\"limits\":"+LimitsJson(r.limits)+
        ",\"registration\":"+RegistrationJson(c.registration,c.level.level)+",\"surface\":"+SurfaceJson(c.surface)+",\"parallax\":"+ParallaxJson(c.parallax,c.level.level)+
        ",\"folds\":"+FoldsJson(c.parallax.folds)+",\"pointSource\":"+PointSourceJson(c.point_source)+",\"geometryLevel\":"+LevelJson(c.level,c.coarse_shift)+",\"gate\":"+GateJson(c.gate,r.seam_gate_json)+
        ",\"timeBudget\":"+TimeBudgetJson(c.time_budget)+",\"geometryHash\":"+Text(c.geometry_hash)+"}";
    return s;
}

// ---- Derivation helpers ----
bool Trusted(const cr::RegistrationResult& r){
    return r.has_estimate && (r.state==cr::RegistrationState::verified || r.state==cr::RegistrationState::out_of_envelope);
}
bool AnyExceeded(const std::vector<cr::ComponentCheck>& v){for(const auto& c:v)if(c.exceeded)return true;return false;}
double PitchMm(const render::OutputRaster& raster){
    if(!raster.dpi)throw std::invalid_argument("raster has no density");
    return 25.4/raster.dpi;
}
// The rows of layer 2 above maxLiftMm: |q| / k > line, on applied rows.
struct LiftExcess {bool any{};double maximum_mm{};double first{},last{};};
LiftExcess LayerTwoBeyond(const pc::ParallaxResult& p,const RegistrationLines& l){
    // S4 judges the rows against the condition's line (ParallaxResult::lift); measured rows only.
    LiftExcess e;
    if(!l.max_lift_mm || p.rows.empty() || !p.lift.exceeded)return e;
    e.any=true;e.maximum_mm=p.lift.maximum_mm;e.first=p.lift.first_exceeded_row;e.last=p.lift.last_exceeded_row;
    return e;
}
bool ShiftBeyond(const UniformShift& s,const RegistrationLines& l){
    const double position=std::hypot(s.translation_x_mm,s.translation_y_mm);
    return position>l.envelope.position_mm || (l.surface_envelope && std::abs(s.surface_height_mm)>l.surface_envelope->height_mm);
}
// Whether the chosen level put in an amount beyond a range (the report's beyondEnvelopeApplied).
bool BeyondApplied(const CaptureRegistration& c,const RegistrationLines& l){
    switch(c.level.level){
    case GeometryLevel::g1:
        if(LayerTwoBeyond(c.parallax,l).any)return true;
        [[fallthrough]];
    case GeometryLevel::g2:
        return AnyExceeded(c.registration.components) || (c.surface.state==SurfaceState::estimated && (c.surface.exceeded.height || c.surface.exceeded.tilt));
    case GeometryLevel::g3:
        return c.coarse_shift && ShiftBeyond(*c.coarse_shift,l);
    case GeometryLevel::g4:
        return false;
    }
    return false;
}
RegistrationWarning Make(WarningCode code,WarningClass k,WarningComponent component,double value,std::optional<double> limit,WarningUnit unit,Remedy remedy){
    RegistrationWarning w;w.code=code;w.warning_class=k;w.component=component;w.value=value;w.limit=limit;w.unit=unit;w.remedy=remedy;w.source=WarningSource::condition;
    return w;
}
bool HasReason(const std::vector<sg::SeamGateFinding>& v,std::string_view reason){
    for(const auto& f:v)if(f.reason==reason)return true;return false;
}
bool HasOwnReason(const std::vector<rg::GateFinding>& v,std::string_view reason){
    for(const auto& f:v)if(f.reason==reason)return true;return false;
}
Remedy RemedyOf(std::string_view text,Remedy fallback){const auto r=ParseRemedy(text);return r?*r:fallback;}

void PutHex(std::string& out,const char* name,double v){
    // -0 is 0: the report writes it as 0 (formats 2), so the reader's hash and the writer's must not tell them apart.
    if(v==0)v=0.0;
    std::uint64_t bits{};std::memcpy(&bits,&v,sizeof bits);
    char buffer[17];static constexpr char digits[]="0123456789abcdef";
    for(int i=0;i<16;++i)buffer[i]=digits[(bits>>(60-4*i))&15u];
    buffer[16]=0;out+=name;out+='=';out+=buffer;out+='\n';
}
void PutInt(std::string& out,const char* name,std::int64_t v){out+=name;out+='=';out+=std::to_string(v);out+='\n';}
void PutText(std::string& out,const char* name,std::string_view v){out+=name;out+='=';out+=v;out+='\n';}
void PutPose(std::string& out,const char* name,const cr::PoseParameters& p){
    const std::string n(name);
    PutHex(out,(n+".rx").c_str(),p.rx_deg);PutHex(out,(n+".ry").c_str(),p.ry_deg);PutHex(out,(n+".rz").c_str(),p.rz_deg);
    PutHex(out,(n+".tx").c_str(),p.tx_mm);PutHex(out,(n+".ty").c_str(),p.ty_mm);PutHex(out,(n+".tz").c_str(),p.tz_percent);
}
bool LowerHex64(const std::string& s){return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;}

// ---- Reading ----
[[noreturn]] void Reject(const char* what){throw std::invalid_argument(std::string("report v8: ")+what);}
struct ReadFailure {[[noreturn]] static void Fail(std::string_view,std::string_view detail){throw std::invalid_argument("report v8: "+std::string(detail));}};
using Value=json::JsonValue;
void Fields(const Value& v,std::initializer_list<const char*> names){
    if(v.kind!=json::JsonKind::object || v.object.size()!=names.size())Reject("members");
    for(const auto* n:names)if(!v.object.contains(n))Reject(n);
}
const Value& At(const Value& v,const char* name){
    if(v.kind!=json::JsonKind::object)Reject("object");
    const auto it=v.object.find(name);if(it==v.object.end())Reject(name);return it->second;
}
bool IsNull(const Value& v){return v.kind==json::JsonKind::null_value;}
std::string ReadText(const Value& v){if(v.kind!=json::JsonKind::string || v.string.empty() || v.string.find('\0')!=std::string::npos)Reject("text");return v.string;}
std::string ReadHash(const Value& v){auto s=ReadText(v);if(!LowerHex64(s))Reject("hash");return s;}
bool ReadBool(const Value& v){if(v.kind!=json::JsonKind::boolean)Reject("boolean");return v.boolean;}
double ReadNum(const Value& v){
    if(v.kind!=json::JsonKind::number)Reject("number");double n{};
    const auto r=std::from_chars(v.string.data(),v.string.data()+v.string.size(),n,std::chars_format::general);
    if(r.ec!=std::errc{} || r.ptr!=v.string.data()+v.string.size() || !std::isfinite(n))Reject("number");return n;
}
std::optional<double> ReadOptNum(const Value& v){if(IsNull(v))return std::nullopt;return ReadNum(v);}
std::uint64_t ReadUnsigned(const Value& v,std::uint64_t maximum){
    if(v.kind!=json::JsonKind::number || v.string.empty() || v.string.find_first_not_of("0123456789")!=std::string::npos)Reject("unsigned");
    std::uint64_t n{};const auto r=std::from_chars(v.string.data(),v.string.data()+v.string.size(),n);
    if(r.ec!=std::errc{} || r.ptr!=v.string.data()+v.string.size() || n>maximum)Reject("unsigned");return n;
}
std::uint32_t ReadU32(const Value& v){return static_cast<std::uint32_t>(ReadUnsigned(v,0xffffffffu));}
std::int64_t ReadSigned(const Value& v,std::int64_t minimum,std::int64_t maximum){
    if(v.kind!=json::JsonKind::number || v.string.empty())Reject("integer");std::int64_t n{};
    const auto r=std::from_chars(v.string.data(),v.string.data()+v.string.size(),n);
    if(r.ec!=std::errc{} || r.ptr!=v.string.data()+v.string.size() || n<minimum || n>maximum)Reject("integer");return n;
}
const std::vector<Value>& ReadArray(const Value& v){if(v.kind!=json::JsonKind::array)Reject("array");return v.array;}
std::vector<std::string> ReadStrings(const Value& v){std::vector<std::string> out;for(const auto& e:ReadArray(v))out.push_back(ReadText(e));return out;}
render::Point ReadPoint(const Value& v){Fields(v,{"x","y"});return {ReadNum(At(v,"x")),ReadNum(At(v,"y"))};}
cr::PoseParameters ReadPose(const Value& v){
    Fields(v,{"rxDeg","ryDeg","rzDeg","txMm","tyMm","tzPercent"});
    return {ReadNum(At(v,"rxDeg")),ReadNum(At(v,"ryDeg")),ReadNum(At(v,"rzDeg")),ReadNum(At(v,"txMm")),ReadNum(At(v,"tyMm")),ReadNum(At(v,"tzPercent"))};
}
template<class E,class Code,std::size_t N>
E ReadCode(const Value& v,const std::array<E,N>& all,Code code){
    const auto t=ReadText(v);for(const auto e:all)if(code(e)==t)return e;Reject("code");
}
std::vector<rg::RowRange> ReadRowRanges(const Value& v){
    std::vector<rg::RowRange> out;
    for(const auto& e:ReadArray(v)){Fields(e,{"first","last"});rg::RowRange r{ReadU32(At(e,"first")),ReadU32(At(e,"last"))};if(r.last<r.first)Reject("row range");out.push_back(r);}
    return out;
}
std::vector<std::pair<std::string,std::string>> ReadFindings(const Value& v){
    std::vector<std::pair<std::string,std::string>> out;
    for(const auto& e:ReadArray(v)){Fields(e,{"reason","remedy"});out.emplace_back(ReadText(At(e,"reason")),ReadText(At(e,"remedy")));}
    return out;
}
rg::GateQuantities ReadQuantities(const Value& v){
    Fields(v,{"maximumPx","twoDirectionalPx","oneDirectionalPx"});rg::GateQuantities q;
    q.maximum_px=ReadOptNum(At(v,"maximumPx"));q.maxima.two_directional_px=ReadOptNum(At(v,"twoDirectionalPx"));q.maxima.one_directional_px=ReadOptNum(At(v,"oneDirectionalPx"));
    return q;
}
std::string Canonical(const Value& v){return json::SerializeJsonWith<ReadFailure>(v);}
Value ParseText(std::string_view text){return json::BasicJsonParser<ReadFailure>(text).Parse();}
// The embedded seam gate object in the JSON reader's canonical form (members in name order), so that a record read back writes
// the same text as the record it was read from.
std::string CanonicalText(const std::string& text){
    const auto v=ParseText(text);
    if(v.kind!=json::JsonKind::object)throw std::logic_error("the embedded seam gate report is not an object");
    return Canonical(v);
}

SavedFixedCompositeRegistration ReadRecord(const Value& v){
    Fields(v,{"condition","profile","raster","seam","layout","basePose","limits","registration","surface","parallax","folds","pointSource","geometryLevel","gate","timeBudget","geometryHash"});
    SavedFixedCompositeRegistration r;
    const auto& condition=At(v,"condition");Fields(condition,{"sha256","sequence"});
    r.condition_sha256=ReadHash(At(condition,"sha256"));r.condition_sequence=static_cast<std::uint32_t>(ReadUnsigned(At(condition,"sequence"),999999));
    if(!r.condition_sequence)Reject("condition sequence");
    const auto& profile=At(v,"profile");Fields(profile,{"file","fileSha256","fingerprintSha256","profileId"});
    // A plain name (the full rule, IsFixedCompositeProfileFileName, is the condition library's; the StillReview reader applies it too).
    r.profile_file=ReadText(At(profile,"file"));
    if(r.profile_file.size()<6 || r.profile_file.size()>96 || r.profile_file.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-")!=std::string::npos ||
        r.profile_file.find("..")!=std::string::npos || r.profile_file.compare(r.profile_file.size()-5,5,".json")!=0)Reject("profile file");
    r.profile_file_sha256=ReadHash(At(profile,"fileSha256"));r.profile_fingerprint_sha256=ReadHash(At(profile,"fingerprintSha256"));
    if(!IsNull(At(profile,"profileId"))){r.profile_id=ReadText(At(profile,"profileId"));if(r.profile_id->size()>256)Reject("profile id");}
    const auto& raster=At(v,"raster");Fields(raster,{"regionUm","dpi","widthPixels","heightPixels"});
    const auto& region=At(raster,"regionUm");Fields(region,{"left","top","right","bottom"});
    constexpr std::int64_t kUm=1'000'000'000;
    r.raster=render::OutputRaster{render::DocumentRegionUm{ReadSigned(At(region,"left"),0,kUm),ReadSigned(At(region,"top"),0,kUm),ReadSigned(At(region,"right"),0,kUm),
        ReadSigned(At(region,"bottom"),0,kUm)},static_cast<std::uint32_t>(ReadUnsigned(At(raster,"dpi"),65535)),
        static_cast<std::uint32_t>(ReadUnsigned(At(raster,"widthPixels"),render::kMaximumImageDimension)),static_cast<std::uint32_t>(ReadUnsigned(At(raster,"heightPixels"),render::kMaximumImageDimension))};
    try{render::ValidateOutputRaster(r.raster);}catch(const std::invalid_argument&){Reject("raster");}
    const auto& seam=At(v,"seam");Fields(seam,{"column","origin"});
    r.seam_column=ReadU32(At(seam,"column"));r.seam_origin=ReadText(At(seam,"origin"));
    if(r.seam_column>=r.raster.width_pixels || (r.seam_origin!="band-center-provisional" && r.seam_origin!="operator"))Reject("seam");
    const auto& layout=At(v,"layout");Fields(layout,{"mixingWidthPx","omegaMarginPx","guardPx","bandHalfWidthPx"});
    r.layout.seam_column=r.seam_column;r.layout.mixing_width_px=static_cast<std::uint32_t>(ReadUnsigned(At(layout,"mixingWidthPx"),4096));
    r.layout.omega_margin_px=static_cast<std::uint32_t>(ReadUnsigned(At(layout,"omegaMarginPx"),65535));r.layout.guard_px=static_cast<std::uint32_t>(ReadUnsigned(At(layout,"guardPx"),65535));
    r.layout.band_half_width_px=static_cast<std::uint32_t>(ReadUnsigned(At(layout,"bandHalfWidthPx"),65535));
    r.base_pose=ReadPose(At(v,"basePose"));
    // limits
    const auto& lim=At(v,"limits");Fields(lim,{"envelope","surfaceEnvelope","applyBeyondEnvelope","surfaceScaleWarnPercent","maxLiftMm","seamGate"});
    const auto& env=At(lim,"envelope");Fields(env,{"positionMm","rotationDeg","tiltDeg","heightPercent"});
    r.limits.envelope={ReadNum(At(env,"positionMm")),ReadNum(At(env,"rotationDeg")),ReadNum(At(env,"tiltDeg")),ReadNum(At(env,"heightPercent"))};
    for(const double e:{r.limits.envelope.position_mm,r.limits.envelope.rotation_deg,r.limits.envelope.tilt_deg,r.limits.envelope.height_percent})if(!(e>0))Reject("envelope");
    if(!IsNull(At(lim,"surfaceEnvelope"))){const auto& se=At(lim,"surfaceEnvelope");Fields(se,{"heightMm","tiltDeg"});
        r.limits.surface_envelope=SurfaceEnvelope{ReadNum(At(se,"heightMm")),ReadNum(At(se,"tiltDeg"))};
        if(!(r.limits.surface_envelope->height_mm>0) || !(r.limits.surface_envelope->tilt_deg>0))Reject("surface envelope");}
    r.limits.apply_beyond_envelope=ReadBool(At(lim,"applyBeyondEnvelope"));
    r.limits.surface_scale_warn_percent=ReadOptNum(At(lim,"surfaceScaleWarnPercent"));
    if(r.limits.surface_scale_warn_percent && *r.limits.surface_scale_warn_percent<0)Reject("scale line");
    r.limits.max_lift_mm=ReadOptNum(At(lim,"maxLiftMm"));if(r.limits.max_lift_mm && !(*r.limits.max_lift_mm>0))Reject("lift line");
    const auto& gl=At(lim,"seamGate");Fields(gl,{"limitPx","guardBandPx"});
    r.limits.seam_limit_px=ReadNum(At(gl,"limitPx"));r.limits.seam_guard_band_px=ReadNum(At(gl,"guardBandPx"));
    if(!(r.limits.seam_limit_px>0) || !(r.limits.seam_guard_band_px>=0))Reject("seam gate line");
    auto& c=r.registration;
    // registration
    {
        const auto& g=At(v,"registration");Fields(g,{"state","reasons","hasEstimate","pose","poseSigma","covariance","components","display","applied","design","diagnostics"});
        auto& e=c.registration;
        e.state=ReadCode(At(g,"state"),std::array{cr::RegistrationState::verified,cr::RegistrationState::undetermined,cr::RegistrationState::out_of_envelope},
            [](cr::RegistrationState s){return std::string(cr::RegistrationStateCode(s));});
        e.reasons=ReadStrings(At(g,"reasons"));e.has_estimate=ReadBool(At(g,"hasEstimate"));
        if(e.has_estimate){
            e.pose=ReadPose(At(g,"pose"));e.pose_sigma=ReadPose(At(g,"poseSigma"));
            const auto& cov=ReadArray(At(g,"covariance"));if(cov.size()!=36)Reject("covariance");
            for(std::size_t i=0;i<36;++i)e.covariance[i]=ReadNum(cov[i]);
        }else if(!IsNull(At(g,"pose")) || !IsNull(At(g,"poseSigma")) || !IsNull(At(g,"covariance")))Reject("estimate");
        for(const auto& item:ReadArray(At(g,"components"))){
            Fields(item,{"component","valueFromBase","limit","exceeded"});
            cr::ComponentCheck k;
            k.component=ReadCode(At(item,"component"),std::array{cr::EnvelopeComponent::position,cr::EnvelopeComponent::rotation,cr::EnvelopeComponent::tilt,cr::EnvelopeComponent::height},
                [](cr::EnvelopeComponent x){return std::string(cr::EnvelopeComponentCode(x));});
            k.value=ReadNum(At(item,"valueFromBase"));k.limit=ReadNum(At(item,"limit"));k.exceeded=ReadBool(At(item,"exceeded"));
            e.components.push_back(k);
        }
        const auto& d=At(g,"display");Fields(d,{"positionMm","txMm","tyMm","rotationDeg","tiltDeg","rxDeg","ryDeg","heightPercent","heightMm","outputFrame"});
        e.display={ReadNum(At(d,"positionMm")),ReadNum(At(d,"txMm")),ReadNum(At(d,"tyMm")),ReadNum(At(d,"rotationDeg")),ReadNum(At(d,"tiltDeg")),
            ReadNum(At(d,"rxDeg")),ReadNum(At(d,"ryDeg")),ReadNum(At(d,"heightPercent")),ReadNum(At(d,"heightMm"))};
        if(!IsNull(At(d,"outputFrame"))){
            const auto& f=At(d,"outputFrame");Fields(f,{"cameraMotionMm","lensDirection","lensTiltDeg","rotationClockwiseDeg","surfaceHighSide","baselineEquivalentMotionMm"});
            e.display.output_frame=OutputFrameDisplay{ReadPoint(At(f,"cameraMotionMm")),ReadPoint(At(f,"lensDirection")),ReadNum(At(f,"lensTiltDeg")),
                ReadNum(At(f,"rotationClockwiseDeg")),ReadPoint(At(f,"surfaceHighSide")),ReadPoint(At(f,"baselineEquivalentMotionMm"))};
        }
        (void)ReadBool(At(g,"applied"));Fields(At(g,"design"),{});
        const auto& diag=At(g,"diagnostics");Fields(diag,{"counts"});const auto& n=At(diag,"counts");
        Fields(n,{"outerIterations","points","inliers","gatePoints","uniqueBlocks","consistentBlocks"});
        e.diagnostics.outer_iterations=ReadU32(At(n,"outerIterations"));e.diagnostics.points=ReadU32(At(n,"points"));e.diagnostics.inliers=ReadU32(At(n,"inliers"));
        e.diagnostics.gate_points=ReadU32(At(n,"gatePoints"));e.diagnostics.unique_blocks=ReadU32(At(n,"uniqueBlocks"));e.diagnostics.consistent_blocks=ReadU32(At(n,"consistentBlocks"));
    }
    // surface
    {
        const auto& s=At(v,"surface");Fields(s,{"state","heightMm","slopeX","slopeY","tiltDeg","covariance","baselineEquivalentMm","scale","exceeded","applied"});
        auto& f=c.surface;
        f.state=ReadCode(At(s,"state"),std::array{SurfaceState::estimated,SurfaceState::not_estimated,SurfaceState::disabled},[](SurfaceState x){return std::string(SurfaceStateCode(x));});
        if(f.state==SurfaceState::estimated){
            f.plane={ReadNum(At(s,"heightMm")),ReadNum(At(s,"slopeX")),ReadNum(At(s,"slopeY"))};f.tilt_deg=ReadNum(At(s,"tiltDeg"));
            const auto& cov=ReadArray(At(s,"covariance"));if(cov.size()!=9)Reject("surface covariance");
            for(std::size_t i=0;i<9;++i)f.covariance[i]=ReadNum(cov[i]);
            f.baseline_equivalent_mm=ReadNum(At(s,"baselineEquivalentMm"));
            const auto& k=At(s,"scale");Fields(k,{"factor","minimum","maximum"});f.scale={ReadNum(At(k,"factor")),ReadNum(At(k,"minimum")),ReadNum(At(k,"maximum"))};
        }else for(const auto* n:{"heightMm","slopeX","slopeY","tiltDeg","covariance","baselineEquivalentMm","scale"})if(!IsNull(At(s,n)))Reject("surface value");
        const auto& x=At(s,"exceeded");Fields(x,{"height","tilt"});f.exceeded={ReadBool(At(x,"height")),ReadBool(At(x,"tilt"))};
        f.applied=ReadBool(At(s,"applied"));
        if((f.state==SurfaceState::disabled)!=!r.limits.surface_envelope)Reject("surface state against the condition");
    }
    // parallax and folds
    {
        const auto& p=At(v,"parallax");Fields(p,{"state","reasons","baseline","kPxPerMm","design","grid","split","profile","summary","lift","applied"});
        auto& q=c.parallax;
        q.state=ReadCode(At(p,"state"),std::array{pc::ParallaxState::applied,pc::ParallaxState::not_applied,pc::ParallaxState::disabled},
            [](pc::ParallaxState x){return std::string(pc::ParallaxStateCode(x));});
        q.reasons=ReadStrings(At(p,"reasons"));(void)ReadBool(At(p,"applied"));
        for(const auto& e:ReadArray(At(p,"profile"))){
            Fields(e,{"y","qPx","sigmaPx","nEff","applied","points","liftMm","state","reason","role","hole"});
            pc::ParallaxRow row;row.y=ReadNum(At(e,"y"));row.q=ReadNum(At(e,"qPx"));row.sigma_q=ReadNum(At(e,"sigmaPx"));row.n_eff=ReadNum(At(e,"nEff"));
            row.applied=ReadBool(At(e,"applied"));row.points=ReadU32(At(e,"points"));row.lift_mm=ReadNum(At(e,"liftMm"));
            row.state=ReadCode(At(e,"state"),std::array{pc::ParallaxRowState::measured,pc::ParallaxRowState::filled},[](pc::ParallaxRowState x){return std::string(pc::ParallaxRowStateCode(x));});
            if(!IsNull(At(e,"reason")))row.reason=ReadText(At(e,"reason"));
            row.role=ReadCode(At(e,"role"),std::array{pc::ParallaxRowRole::verify,pc::ParallaxRowRole::correct},[](pc::ParallaxRowRole x){return std::string(pc::ParallaxRowRoleCode(x));});
            row.hole=ReadBool(At(e,"hole"));
            q.rows.push_back(std::move(row));
        }
        if(!q.rows.empty()){
            q.baseline=ReadPoint(At(p,"baseline"));q.k_px_per_mm=ReadNum(At(p,"kPxPerMm"));
            const auto& d=At(p,"design");Fields(d,{"columnSpacingPx","columnRadiusPx","pointPatchPx","weightFlatPx","weightFalloffPx","maximumStrain","foldNearSeamPx"});
            q.design.column_spacing_px=ReadNum(At(d,"columnSpacingPx"));q.design.column_radius_px=ReadNum(At(d,"columnRadiusPx"));
            q.design.point_patch_px=ReadU32(At(d,"pointPatchPx"));q.design.weight_flat_px=ReadNum(At(d,"weightFlatPx"));q.design.weight_falloff_px=ReadNum(At(d,"weightFalloffPx"));
            q.design.maximum_strain=ReadNum(At(d,"maximumStrain"));q.design.fold_near_seam_px=ReadNum(At(d,"foldNearSeamPx"));
            const auto& grid=At(p,"grid");Fields(grid,{"seamColumn","firstRow","rowStep"});
            q.seam_column=ReadU32(At(grid,"seamColumn"));q.first_row=ReadU32(At(grid,"firstRow"));q.row_step=ReadU32(At(grid,"rowStep"));
            // The split: the roles are derived again from the period, the phase and the row's index (never read as they are).
            const auto& split=At(p,"split");Fields(split,{"mode","verifyPeriodRows","verifyPhase","rowsVerify","rowsCorrect","rowsWithHole","pointsExcluded"});
            q.verify_period_rows=static_cast<std::uint32_t>(ReadUnsigned(At(split,"verifyPeriodRows"),4096));q.verify_phase=ReadU32(At(split,"verifyPhase"));
            const auto mode=ReadText(At(split,"mode"));
            if(mode!=(q.verify_period_rows?"split":"hole") || (q.verify_period_rows?q.verify_phase>=q.verify_period_rows:q.verify_phase!=0))Reject("parallax split");
            pc::ParallaxRows grid_rows;grid_rows.verify_period_rows=q.verify_period_rows;grid_rows.verify_phase=q.verify_phase;
            std::uint32_t verify=0,holes=0;
            for(std::size_t i=0;i<q.rows.size();++i){
                const bool expected=pc::IsVerifyRow(grid_rows,i);
                if((q.rows[i].role==pc::ParallaxRowRole::verify)!=expected || (!q.verify_period_rows && !q.rows[i].hole))Reject("parallax row role");
                verify+=expected;holes+=q.rows[i].hole;
            }
            q.summary.rows_verify=ReadU32(At(split,"rowsVerify"));q.summary.rows_correct=ReadU32(At(split,"rowsCorrect"));
            q.summary.rows_with_hole=ReadU32(At(split,"rowsWithHole"));q.summary.points_excluded=ReadU32(At(split,"pointsExcluded"));
            if(q.summary.rows_verify!=verify || q.summary.rows_correct!=q.rows.size()-verify || q.summary.rows_with_hole!=holes ||
                (!q.verify_period_rows && q.summary.points_excluded!=0))Reject("parallax split counts");
            const auto& m=At(p,"summary");
            Fields(m,{"maxShiftPx","maxShiftMm","atRow","maxStrain","columns","points","pointsInput","pointsBaseline","rowsMeasured","rowsFilled","holeXPx","holeYPx",
                "pointsCrossValidated","foldCandidates","foldResidualScalePx"});
            const auto& cols=At(m,"columns");Fields(cols,{"first","last"});
            auto& u=q.summary;
            u.max_shift_px=ReadNum(At(m,"maxShiftPx"));u.max_shift_mm=ReadNum(At(m,"maxShiftMm"));u.at_row=ReadNum(At(m,"atRow"));u.max_strain=ReadNum(At(m,"maxStrain"));
            u.first_column=ReadU32(At(cols,"first"));u.last_column=ReadU32(At(cols,"last"));u.points=ReadU32(At(m,"points"));u.points_input=ReadU32(At(m,"pointsInput"));
            u.points_baseline=ReadU32(At(m,"pointsBaseline"));u.rows_measured=ReadU32(At(m,"rowsMeasured"));u.rows_filled=ReadU32(At(m,"rowsFilled"));
            u.hole_x_px=ReadNum(At(m,"holeXPx"));u.hole_y_px=ReadNum(At(m,"holeYPx"));u.points_cross_validated=ReadU32(At(m,"pointsCrossValidated"));
            u.fold_candidates=ReadU32(At(m,"foldCandidates"));u.fold_residual_scale_px=ReadNum(At(m,"foldResidualScalePx"));
            const auto& f=At(p,"lift");Fields(f,{"limitMm","maximumMm","atRow","exceeded","exceededRows","firstExceededRow","lastExceededRow"});
            auto& lift=q.lift;
            if(!IsNull(At(f,"limitMm")))lift.limit_mm=ReadNum(At(f,"limitMm"));
            lift.maximum_mm=ReadNum(At(f,"maximumMm"));lift.at_row=ReadNum(At(f,"atRow"));lift.exceeded=ReadBool(At(f,"exceeded"));
            lift.exceeded_rows=ReadU32(At(f,"exceededRows"));lift.first_exceeded_row=ReadNum(At(f,"firstExceededRow"));lift.last_exceeded_row=ReadNum(At(f,"lastExceededRow"));
        }else{
            for(const auto* n:{"baseline","kPxPerMm","grid","split","summary","lift"})if(!IsNull(At(p,n)))Reject("parallax values");
            Fields(At(p,"design"),{});
        }
        if((q.state==pc::ParallaxState::disabled)!=!r.limits.max_lift_mm)Reject("parallax state against the condition");
        for(const auto& e:ReadArray(At(v,"folds"))){
            Fields(e,{"kind","positionPx","angleDeg","stepSign","stepPx","stepMm","distanceFromSeamPx","crossesSeam","first","last","lengthPx","points","nearSeam","shiftToClearPx"});
            pc::FoldLine f;
            f.kind=ReadCode(At(e,"kind"),std::array{pc::FoldLineKind::fold,pc::FoldLineKind::gutter},[](pc::FoldLineKind x){return std::string(pc::FoldLineKindCode(x));});
            f.position_px=ReadNum(At(e,"positionPx"));f.angle_deg=ReadNum(At(e,"angleDeg"));f.step_sign=static_cast<int>(ReadSigned(At(e,"stepSign"),-1,1));
            f.step_px=ReadNum(At(e,"stepPx"));f.step_mm=ReadNum(At(e,"stepMm"));f.distance_from_seam_px=ReadNum(At(e,"distanceFromSeamPx"));f.crosses_seam=ReadBool(At(e,"crossesSeam"));
            f.first=ReadPoint(At(e,"first"));f.last=ReadPoint(At(e,"last"));f.length_px=ReadNum(At(e,"lengthPx"));f.points=ReadU32(At(e,"points"));
            f.near_seam=ReadBool(At(e,"nearSeam"));f.shift_to_clear_px=ReadOptNum(At(e,"shiftToClearPx"));
            q.folds.push_back(f);
        }
    }
    // pointSource
    if(!IsNull(At(v,"pointSource"))){
        const auto& o=At(v,"pointSource");Fields(o,{"fastMeasurerUsed","pointsUsed","edgeShare","edgeShareLine","fallbackReason"});
        LayerTwoPointSource ps;
        ps.fast_measurer_used=ReadBool(At(o,"fastMeasurerUsed"));ps.points_used=ReadU32(At(o,"pointsUsed"));ps.edge_share=ReadOptNum(At(o,"edgeShare"));ps.edge_share_line=ReadNum(At(o,"edgeShareLine"));
        if(!IsNull(At(o,"fallbackReason")))ps.fallback_reason=ReadText(At(o,"fallbackReason"));
        c.point_source=std::move(ps);
        if(*PointSourceFault(c))Reject(PointSourceFault(c));
    }
    // geometryLevel
    {
        const auto& l=At(v,"geometryLevel");Fields(l,{"level","reasons","applied","beyondEnvelopeApplied","mapping","coarseShift"});
        auto& d=c.level;
        const auto level=ParseGeometryLevel(ReadText(At(l,"level")));if(!level)Reject("level");d.level=*level;
        d.reasons=ReadStrings(At(l,"reasons"));
        for(const auto& reason:d.reasons)if(reason!="seam-outside-overlap-at-level" && reason!="output-uncovered-at-level")Reject("level reason");
        const auto& a=At(l,"applied");Fields(a,{"pose","surface","parallax"});d.applied={ReadBool(At(a,"pose")),ReadBool(At(a,"surface")),ReadBool(At(a,"parallax"))};
        d.beyond_envelope_applied=ReadBool(At(l,"beyondEnvelopeApplied"));
        const auto& m=At(l,"mapping");Fields(m,{"pose","surface","parallax"});
        d.mapping.pose=ReadPose(At(m,"pose"));
        const auto& s=At(m,"surface");Fields(s,{"h0Mm","slopeX","slopeY"});d.mapping.surface={ReadNum(At(s,"h0Mm")),ReadNum(At(s,"slopeX")),ReadNum(At(s,"slopeY"))};
        if(!IsNull(At(m,"parallax"))){
            const auto& p=At(m,"parallax");Fields(p,{"baseline","rowSpacingPx","firstRowPx","qPx","aPx","lPx","seamColumn"});
            ParallaxProfile f;f.baseline=ReadPoint(At(p,"baseline"));f.row_spacing_px=ReadNum(At(p,"rowSpacingPx"));f.first_row_px=ReadNum(At(p,"firstRowPx"));
            for(const auto& e:ReadArray(At(p,"qPx")))f.q_px.push_back(ReadNum(e));
            f.a_px=ReadNum(At(p,"aPx"));f.l_px=ReadNum(At(p,"lPx"));f.seam_column=ReadU32(At(p,"seamColumn"));
            d.mapping.parallax=std::move(f);
        }
        if(!IsNull(At(l,"coarseShift"))){
            const auto& s2=At(l,"coarseShift");Fields(s2,{"surfaceHeightMm","translationXMm","translationYMm"});
            c.coarse_shift=UniformShift{ReadNum(At(s2,"surfaceHeightMm")),ReadNum(At(s2,"translationXMm")),ReadNum(At(s2,"translationYMm"))};
        }
    }
    // gate
    {
        const auto& g=At(v,"gate");
        Fields(g,{"state","findings","gateFindings","cause","remedy","lift","rows","maxima","before","after","corrected","anchors","placement","wideSearch","design","geometryHash","seamGate"});
        auto& t=c.gate;
        const auto state=rg::ParseGateState(ReadText(At(g,"state")));if(!state)Reject("gate state");t.state=*state;
        for(auto& f:ReadFindings(At(g,"findings")))t.findings.push_back({std::move(f.first),std::move(f.second)});
        for(auto& f:ReadFindings(At(g,"gateFindings")))t.gate_findings.push_back({std::move(f.first),std::move(f.second)});
        const auto cause=rg::ParseGateCause(ReadText(At(g,"cause")));if(!cause)Reject("gate cause");t.cause=*cause;
        t.remedy=ReadText(At(g,"remedy"));if(t.remedy!=rg::GateCauseRemedy(t.cause))Reject("gate remedy");
        if(!IsNull(At(g,"lift"))){const auto& f=At(g,"lift");Fields(f,{"pointsExceeding","baselineShare","signAgreementShare","heightEstimateMm"});
            t.lift=rg::GateLift{ReadU32(At(f,"pointsExceeding")),ReadNum(At(f,"baselineShare")),ReadNum(At(f,"signAgreementShare")),ReadOptNum(At(f,"heightEstimateMm"))};}
        const auto& rows=At(g,"rows");Fields(rows,{"dxMeasuredShare","rowsWithContent","rowsDxMeasured","unmeasured","outsideMeasurable","rowsBlank","rowsMeasured",
            "rowsContentUnmeasured","rowsOneSided","rowsUnconfirmed","contentUnmeasuredShare","blank","contentUnmeasured","rowsCorrected","corrected"});
        const bool ran=t.state!=rg::GateState::not_run;
        if(ran){t.rows.dx_measured_share=ReadNum(At(rows,"dxMeasuredShare"));t.rows.content_unmeasured_share=ReadNum(At(rows,"contentUnmeasuredShare"));}
        else if(!IsNull(At(rows,"dxMeasuredShare")) || !IsNull(At(rows,"contentUnmeasuredShare")))Reject("gate rows");
        t.rows.rows_with_content=ReadU32(At(rows,"rowsWithContent"));t.rows.rows_dx_measured=ReadU32(At(rows,"rowsDxMeasured"));
        t.rows.rows_blank=ReadU32(At(rows,"rowsBlank"));t.rows.rows_measured=ReadU32(At(rows,"rowsMeasured"));
        t.rows.rows_content_unmeasured=ReadU32(At(rows,"rowsContentUnmeasured"));t.rows.rows_one_sided=ReadU32(At(rows,"rowsOneSided"));
        t.rows.rows_unconfirmed=ReadU32(At(rows,"rowsUnconfirmed"));
        // Every row with content is unconfirmed, measured or unmeasured (GateRows); the shares are their quotients.
        if(t.rows.rows_dx_measured>t.rows.rows_measured || t.rows.rows_one_sided>t.rows.rows_content_unmeasured ||
            static_cast<std::uint64_t>(t.rows.rows_unconfirmed)+t.rows.rows_measured+t.rows.rows_content_unmeasured!=t.rows.rows_with_content)Reject("gate rows");
        if(ran){
            const double n=t.rows.rows_with_content;
            if(t.rows.dx_measured_share!=(n?t.rows.rows_dx_measured/n:0.0) || t.rows.content_unmeasured_share!=(n?t.rows.rows_content_unmeasured/n:0.0))Reject("gate row shares");
        }
        t.rows.unmeasured=ReadRowRanges(At(rows,"unmeasured"));t.rows.outside_measurable=ReadRowRanges(At(rows,"outsideMeasurable"));
        t.rows.blank=ReadRowRanges(At(rows,"blank"));t.rows.content_unmeasured=ReadRowRanges(At(rows,"contentUnmeasured"));
        t.rows.rows_corrected=ReadU32(At(rows,"rowsCorrected"));t.rows.corrected=ReadRowRanges(At(rows,"corrected"));
        // The correct rows' reference: only when the gate ran on layer 2's split rows (G1 with the split), never independent.
        const auto& corrected=At(g,"corrected");
        const bool split=ran && c.level.level==GeometryLevel::g1 && c.parallax.verify_period_rows>0 && !c.parallax.rows.empty();
        if(IsNull(corrected)){
            if(split || t.rows.rows_corrected || !t.rows.corrected.empty())Reject("gate corrected rows");
        }else{
            if(!split)Reject("gate corrected rows");
            Fields(corrected,{"maximumPx","twoDirectionalPx","oneDirectionalPx","beforeMaximumPx","rowsWithContent","rowsMeasured","independent"});
            rg::GateCorrectedReference k;
            k.after.maximum_px=ReadOptNum(At(corrected,"maximumPx"));k.after.maxima.two_directional_px=ReadOptNum(At(corrected,"twoDirectionalPx"));
            k.after.maxima.one_directional_px=ReadOptNum(At(corrected,"oneDirectionalPx"));k.before.maximum_px=ReadOptNum(At(corrected,"beforeMaximumPx"));
            k.rows_with_content=ReadU32(At(corrected,"rowsWithContent"));k.rows_measured=ReadU32(At(corrected,"rowsMeasured"));
            if(ReadBool(At(corrected,"independent")) || k.rows_measured>k.rows_with_content || k.rows_with_content>t.rows.rows_corrected)Reject("gate corrected rows");
            t.corrected=k;
        }
        const auto& mx=At(g,"maxima");Fields(mx,{"twoDirectionalPx","oneDirectionalPx","oneDirectionalNormal","oneDirectionalComponent"});
        t.maxima.two_directional_px=ReadOptNum(At(mx,"twoDirectionalPx"));t.maxima.one_directional_px=ReadOptNum(At(mx,"oneDirectionalPx"));
        if(!IsNull(At(mx,"oneDirectionalNormal")))t.maxima.one_directional_normal=ReadPoint(At(mx,"oneDirectionalNormal"));
        if(!IsNull(At(mx,"oneDirectionalComponent")))t.maxima.one_directional_component=ReadCode(At(mx,"oneDirectionalComponent"),
            std::array{rg::OneDirectionalComponent::across_baseline,rg::OneDirectionalComponent::with_baseline},[](rg::OneDirectionalComponent x){return std::string(rg::OneDirectionalComponentCode(x));});
        t.before=ReadQuantities(At(g,"before"));t.after=ReadQuantities(At(g,"after"));
        const auto& anchors=At(g,"anchors");Fields(anchors,{"range"});
        if(!IsNull(At(anchors,"range"))){const auto& a=At(anchors,"range");Fields(a,{"firstColumn","lastColumn"});
            t.anchor_columns=std::pair{ReadU32(At(a,"firstColumn")),ReadU32(At(a,"lastColumn"))};if(t.anchor_columns->second<t.anchor_columns->first)Reject("anchors");}
        {const auto& pl=At(g,"placement");Fields(pl,{"verifiedBy"});const auto basis=rg::ParsePlacementBasis(ReadText(At(pl,"verifiedBy")));if(!basis)Reject("placement");t.placement=*basis;
            if(!ran && t.placement!=rg::PlacementBasis::none)Reject("placement of a gate not run");}
        if(!IsNull(At(g,"wideSearch"))){const auto& w=At(g,"wideSearch");Fields(w,{"radiusPx","blocks","uniqueBlocks","agreeingBlocks","offsetPx","offsetMm","rows"});
            rg::GateWideSearch s;s.radius_px=ReadU32(At(w,"radiusPx"));s.blocks=ReadU32(At(w,"blocks"));s.unique_blocks=ReadU32(At(w,"uniqueBlocks"));
            s.agreeing_blocks=ReadU32(At(w,"agreeingBlocks"));s.offset_px=ReadOptNum(At(w,"offsetPx"));s.offset_mm=ReadOptNum(At(w,"offsetMm"));
            if(!IsNull(At(w,"rows"))){const auto& rr=At(w,"rows");Fields(rr,{"first","last"});s.rows=rg::RowRange{ReadU32(At(rr,"first")),ReadU32(At(rr,"last"))};}
            t.wide_search=s;}
        const auto& design=At(g,"design");
        if(ran){
            Fields(design,{"minimumBaselineComponent","liftRatio","liftShare","wideSearchRadiusPx","wideSearchHalfWidthPx","wideSearchBlockRows","wideSearchMinimumCorrelation",
                "wideSearchSecondaryRatio","wideSearchConsistencyPx","wideSearchMinimumBlocks","wideSearchTriggerShare","maximumUnmeasuredContentShare"});
            auto& d=t.design;
            d.minimum_baseline_component=ReadNum(At(design,"minimumBaselineComponent"));d.lift_ratio=ReadNum(At(design,"liftRatio"));d.lift_share=ReadNum(At(design,"liftShare"));
            d.wide_search_radius_px=ReadU32(At(design,"wideSearchRadiusPx"));d.wide_search_half_width_px=ReadU32(At(design,"wideSearchHalfWidthPx"));
            d.wide_search_block_rows=ReadU32(At(design,"wideSearchBlockRows"));d.wide_search_minimum_correlation=ReadNum(At(design,"wideSearchMinimumCorrelation"));
            d.wide_search_secondary_ratio=ReadNum(At(design,"wideSearchSecondaryRatio"));d.wide_search_consistency_px=ReadNum(At(design,"wideSearchConsistencyPx"));
            d.wide_search_minimum_blocks=ReadU32(At(design,"wideSearchMinimumBlocks"));d.wide_search_trigger_share=ReadNum(At(design,"wideSearchTriggerShare"));
            d.maximum_unmeasured_content_share=ReadNum(At(design,"maximumUnmeasuredContentShare"));
            if(!(d.maximum_unmeasured_content_share>=0 && d.maximum_unmeasured_content_share<1))Reject("gate design");
        }else Fields(design,{});
        t.geometry_hash=ReadHash(At(g,"geometryHash"));
        const auto& sgv=At(g,"seamGate");
        if(ran){
            if(sgv.kind!=json::JsonKind::object || ReadText(At(sgv,"schema"))!=sg::kSeamGateReportSchema)Reject("seam gate report");
            const auto inner=ReadText(At(sgv,"state"));
            // 11.30: a pass of this part over a failed seam gate only when every reason of the seam gate is one this part decides
            // again (what could not be measured, the placement), and the embedded reasons are the ones recorded; a fail of this
            // part over a passed seam gate only for what could not be measured.
            if(t.state==rg::GateState::pass && inner!="pass"){
                static constexpr std::array<std::string_view,4> again{"unmeasurable-content","no-measurable-block","unpredicted-empty-block","global-placement-unverified"};
                bool ok=inner=="fail" && !t.findings.empty() && sgv.object.count("findings");
                if(ok){
                    const auto& inner_findings=ReadArray(At(sgv,"findings"));
                    ok=inner_findings.size()==t.findings.size();
                    for(std::size_t i=0;ok && i<inner_findings.size();++i){
                        const auto& f=inner_findings[i];
                        ok=f.kind==json::JsonKind::object && f.object.count("reason") && ReadText(At(f,"reason"))==t.findings[i].reason &&
                            std::find(again.begin(),again.end(),t.findings[i].reason)!=again.end();
                    }
                }
                if(!ok)Reject("seam gate state");
            }
            if(t.state==rg::GateState::fail && inner!="fail" && !(inner=="pass" && t.cause==rg::GateCause::unmeasurable))Reject("seam gate state");
            r.seam_gate_json=Canonical(sgv);
        }else{
            if(!IsNull(sgv))Reject("seam gate of a gate not run");
            if(t.maxima.two_directional_px || t.maxima.one_directional_px || t.before.maximum_px || t.after.maximum_px || t.anchor_columns || t.lift || t.wide_search ||
                !t.findings.empty() || !t.gate_findings.empty())Reject("values of a gate not run");
        }
    }
    // timeBudget
    {
        const auto& b=At(v,"timeBudget");Fields(b,{"plannedStages","skipped","costModel"});
        auto& t=c.time_budget;
        for(const auto& e:ReadArray(At(b,"plannedStages"))){const auto s=ParseReviewStage(ReadText(e));if(!s)Reject("stage");t.planned.push_back(*s);}
        for(const auto& e:ReadArray(At(b,"skipped"))){Fields(e,{"stage","reason"});const auto s=ParseReviewStage(ReadText(At(e,"stage")));
            const auto reason=ReadText(At(e,"reason"));
            if(!s || (reason!=kTimeBudgetReason && reason!=kStageFailedReason))Reject("skipped stage");t.skipped.push_back({*s,reason});}
        const auto& m=At(b,"costModel");Fields(m,{"stageMs","tailReserveMs"});
        const auto& ms=At(m,"stageMs");if(ms.kind!=json::JsonKind::object || ms.object.size()!=kReviewStageTexts.size())Reject("stage costs");
        for(std::size_t i=0;i<kReviewStageTexts.size();++i){t.cost_model.stage_ms[i]=ReadNum(At(ms,std::string(kReviewStageTexts[i].second).c_str()));if(t.cost_model.stage_ms[i]<0)Reject("stage cost");}
        t.cost_model.tail_reserve_ms=ReadNum(At(m,"tailReserveMs"));if(t.cost_model.tail_reserve_ms<0)Reject("tail reserve");
        // Every stage is either planned or skipped, once, in the order of ReviewStage.
        std::array<int,kReviewStageTexts.size()> seen{};
        int last=-1;for(const auto s:t.planned){const int i=static_cast<int>(s);if(i<=last)Reject("stage order");last=i;seen[i]++;}
        last=-1;for(const auto& s:t.skipped){const int i=static_cast<int>(s.stage);if(i<=last)Reject("stage order");last=i;seen[i]++;}
        for(const int n:seen)if(n!=1)Reject("stages");
        const bool gate_skipped=std::any_of(t.skipped.begin(),t.skipped.end(),[](const SkippedStage& s){return s.stage==ReviewStage::gate;});
        if(gate_skipped!=(c.gate.state==rg::GateState::not_run))Reject("gate against the time budget");
    }
    c.geometry_hash=ReadHash(At(v,"geometryHash"));
    return r;
}

RegistrationWarning ReadWarning(const Value& v){
    Fields(v,{"code","class","component","value","limit","unit","rowsPx","rowsMm","interpretations","remedy","remedyAmountMm","applied","source"});
    RegistrationWarning w;
    const auto code=ParseWarningCode(ReadText(At(v,"code")));if(!code)Reject("warning code");w.code=*code;
    w.warning_class=ReadCode(At(v,"class"),std::array{WarningClass::exceeded,WarningClass::unverified,WarningClass::notice},[](WarningClass x){return std::string(WarningClassCode(x));});
    const auto component=ParseWarningComponent(ReadText(At(v,"component")));if(!component)Reject("warning component");w.component=*component;
    w.value=ReadNum(At(v,"value"));w.limit=ReadOptNum(At(v,"limit"));
    const auto unit=ParseWarningUnit(ReadText(At(v,"unit")));if(!unit)Reject("warning unit");w.unit=*unit;
    const auto range=[&](const char* name)->std::optional<RowSpan>{const auto& x=At(v,name);if(IsNull(x))return std::nullopt;Fields(x,{"first","last"});
        return RowSpan{ReadNum(At(x,"first")),ReadNum(At(x,"last"))};};
    w.rows_px=range("rowsPx");w.rows_mm=range("rowsMm");w.interpretations=ReadStrings(At(v,"interpretations"));
    const auto remedy=ParseRemedy(ReadText(At(v,"remedy")));if(!remedy)Reject("remedy");w.remedy=*remedy;
    w.remedy_amount_mm=ReadOptNum(At(v,"remedyAmountMm"));w.applied=ReadBool(At(v,"applied"));
    w.source=ReadCode(At(v,"source"),std::array{WarningSource::condition,WarningSource::design},[](WarningSource x){return std::string(WarningSourceText(x));});
    return w;
}
RegistrationAssessment ReadAssessment(const Value& v){
    Fields(v,{"state","exceeded","unverified","qualityAccepted","acceptanceEvidence"});
    RegistrationAssessment a;
    a.state=ReadCode(At(v,"state"),std::array{AssessmentState::within_criteria,AssessmentState::outside_criteria,AssessmentState::not_judged},
        [](AssessmentState x){return std::string(AssessmentStateText(x));});
    for(const auto* list:{"exceeded","unverified"})
        for(const auto& e:ReadArray(At(v,list))){const auto code=ParseWarningCode(ReadText(e));if(!code)Reject("assessment code");
            (std::strcmp(list,"exceeded")==0?a.exceeded:a.unverified).push_back(*code);}
    if(ReadBool(At(v,"qualityAccepted")) || ReadBool(At(v,"acceptanceEvidence")))Reject("quality flags");
    return a;
}
bool SamePose(const cr::PoseParameters& a,const cr::PoseParameters& b){
    return a.rx_deg==b.rx_deg && a.ry_deg==b.ry_deg && a.rz_deg==b.rz_deg && a.tx_mm==b.tx_mm && a.ty_mm==b.ty_mm && a.tz_percent==b.tz_percent;
}
bool SamePlane(const SurfacePlane& a,const SurfacePlane& b){return a.h0_mm==b.h0_mm && a.slope_x==b.slope_x && a.slope_y==b.slope_y;}
bool SameProfile(const ParallaxProfile& a,const ParallaxProfile& b){
    return a.baseline.x==b.baseline.x && a.baseline.y==b.baseline.y && a.row_spacing_px==b.row_spacing_px && a.first_row_px==b.first_row_px &&
        a.q_px==b.q_px && a.a_px==b.a_px && a.l_px==b.l_px && a.seam_column==b.seam_column;
}
}  // namespace

// ---- Pure pieces ----
std::vector<cr::ComponentCheck> RegistrationComponentsFromBase(const cr::PoseParameters& pose,const cr::PoseParameters& base,const cr::RegistrationEnvelope& envelope){
    const auto check=[](cr::EnvelopeComponent component,double value,double limit){
        if(!std::isfinite(value) || !(limit>0))throw std::invalid_argument("components need a finite pose and a set envelope");
        return cr::ComponentCheck{component,value,limit,value>limit};
    };
    return {
        check(cr::EnvelopeComponent::position,std::hypot(pose.tx_mm-base.tx_mm,pose.ty_mm-base.ty_mm),envelope.position_mm),
        check(cr::EnvelopeComponent::rotation,std::abs(pose.rz_deg-base.rz_deg),envelope.rotation_deg),
        check(cr::EnvelopeComponent::tilt,std::hypot(pose.rx_deg-base.rx_deg,pose.ry_deg-base.ry_deg),envelope.tilt_deg),
        check(cr::EnvelopeComponent::height,std::abs(pose.tz_percent-base.tz_percent),envelope.height_percent),
    };
}
GeometryLevelDecision::Applied AppliedOfLevel(GeometryLevel level,const RegisteredMapping& mapping){
    return {level!=GeometryLevel::g4,!mapping.surface.IsZero(),mapping.parallax.has_value()};
}
std::optional<ParallaxProfile> ParallaxProfileOf(const pc::ParallaxResult& p,std::uint32_t seam_column){
    if(!p.Applied() || p.rows.empty() || p.seam_column!=seam_column)return std::nullopt;
    try{return pc::ToParallaxProfile(p);}catch(const std::invalid_argument&){return std::nullopt;}
}

std::vector<RegistrationWarning> DeriveRegistrationWarnings(const CaptureRegistration& c,const RegistrationLines& l,const cr::PoseParameters& base,const render::OutputRaster& raster){
    if(!l.apply_beyond_envelope || !(l.seam_limit_px>0) || !(l.seam_guard_band_px>=0))throw std::invalid_argument("warnings need the condition's lines");
    const double pitch=PitchMm(raster);
    const auto level=c.level.level;
    std::vector<RegistrationWarning> out;
    // The pose components, from p_b, when the estimate is trusted; for G3 the coarse translation.
    const bool trusted=Trusted(c.registration);
    if(trusted){
        const auto components=RegistrationComponentsFromBase(c.registration.pose,base,l.envelope);
        const bool applied=LevelAppliesEstimate(level);
        const std::array<std::tuple<WarningCode,WarningComponent,WarningUnit,Remedy>,4> table{{
            {WarningCode::camera_position,WarningComponent::camera_position,WarningUnit::mm,Remedy::check_camera_mount},
            {WarningCode::camera_rotation,WarningComponent::camera_rotation,WarningUnit::deg,Remedy::check_camera_mount},
            {WarningCode::camera_tilt,WarningComponent::camera_tilt,WarningUnit::deg,Remedy::check_camera_mount},
            {WarningCode::camera_height,WarningComponent::camera_height,WarningUnit::percent,Remedy::check_camera_mount_or_focus}}};
        for(std::size_t i=0;i<4;++i)if(components[i].exceeded){
            auto w=Make(std::get<0>(table[i]),WarningClass::exceeded,std::get<1>(table[i]),components[i].value,components[i].limit,std::get<2>(table[i]),std::get<3>(table[i]));
            w.applied=applied;out.push_back(std::move(w));
        }
    }else if(level==GeometryLevel::g3 && c.coarse_shift){
        const double position=std::hypot(c.coarse_shift->translation_x_mm,c.coarse_shift->translation_y_mm);
        if(position>l.envelope.position_mm){auto w=Make(WarningCode::camera_position,WarningClass::exceeded,WarningComponent::camera_position,position,l.envelope.position_mm,WarningUnit::mm,Remedy::check_camera_mount);
            w.applied=true;out.push_back(std::move(w));}
    }
    // The paper surface (layer 1).
    if(c.surface.state==SurfaceState::estimated && l.surface_envelope && (trusted || LevelAppliesEstimate(level))){
        const double height=std::abs(c.surface.plane.h0_mm);
        if(height>l.surface_envelope->height_mm){
            auto w=Make(WarningCode::surface_height,WarningClass::exceeded,WarningComponent::surface_height,c.surface.plane.h0_mm,l.surface_envelope->height_mm,WarningUnit::mm,Remedy::place_drawing_again);
            w.interpretations={"surface-raised","camera-moved-along-baseline"};w.applied=c.surface.applied;out.push_back(std::move(w));
        }
        if(c.surface.tilt_deg>l.surface_envelope->tilt_deg){
            auto w=Make(WarningCode::surface_tilt,WarningClass::exceeded,WarningComponent::surface_tilt,c.surface.tilt_deg,l.surface_envelope->tilt_deg,WarningUnit::deg,Remedy::place_drawing_again);
            w.applied=c.surface.applied;out.push_back(std::move(w));
        }
    }else if(level==GeometryLevel::g3 && c.coarse_shift && l.surface_envelope && std::abs(c.coarse_shift->surface_height_mm)>l.surface_envelope->height_mm){
        auto w=Make(WarningCode::surface_height,WarningClass::exceeded,WarningComponent::surface_height,c.coarse_shift->surface_height_mm,l.surface_envelope->height_mm,WarningUnit::mm,Remedy::place_drawing_again);
        w.interpretations={"surface-raised","camera-moved-along-baseline"};w.applied=true;out.push_back(std::move(w));
    }
    // Layer 2.
    const auto& p=c.parallax;
    if(level==GeometryLevel::g1 && p.summary.max_shift_px>l.seam_limit_px){
        auto w=Make(WarningCode::seam_lift,WarningClass::notice,WarningComponent::seam_lift,p.summary.max_shift_px,l.seam_limit_px,WarningUnit::px,Remedy::none);
        w.rows_px=RowSpan{p.summary.at_row,p.summary.at_row};w.rows_mm=RowSpan{p.summary.at_row*pitch,p.summary.at_row*pitch};w.applied=true;out.push_back(std::move(w));
    }
    if(const auto excess=LayerTwoBeyond(p,l);excess.any && p.state!=pc::ParallaxState::disabled){
        auto w=Make(WarningCode::seam_lift_limit,WarningClass::exceeded,WarningComponent::seam_lift,excess.maximum_mm,*l.max_lift_mm,WarningUnit::mm,Remedy::place_drawing_again);
        w.rows_px=RowSpan{excess.first,excess.last};w.rows_mm=RowSpan{excess.first*pitch,excess.last*pitch};w.applied=level==GeometryLevel::g1;out.push_back(std::move(w));
    }
    for(const auto& f:p.folds){
        if(f.kind!=pc::FoldLineKind::fold || !f.near_seam)continue;
        // The signed distance (positive: the line is right of the seam column), as the texts read it.
        const double side=f.crosses_seam?0.0:(std::min(f.first.x,f.last.x)>static_cast<double>(p.seam_column)?1.0:-1.0);
        auto w=Make(WarningCode::fold_near_seam,WarningClass::notice,WarningComponent::fold,side*f.distance_from_seam_px,p.design.fold_near_seam_px,WarningUnit::px,Remedy::shift_drawing_horizontal);
        w.rows_px=RowSpan{f.position_px,f.position_px};w.rows_mm=RowSpan{f.position_px*pitch,f.position_px*pitch};
        if(f.shift_to_clear_px)w.remedy_amount_mm=std::abs(*f.shift_to_clear_px)*pitch;
        w.source=WarningSource::design;out.push_back(std::move(w));
    }
    // The gate.
    const auto& g=c.gate;
    if(g.state!=rg::GateState::not_run){
        bool gate_warned=false;
        if(g.after.maximum_px && *g.after.maximum_px+l.seam_guard_band_px>l.seam_limit_px){
            auto w=Make(WarningCode::seam_displacement,WarningClass::exceeded,WarningComponent::seam,*g.after.maximum_px+l.seam_guard_band_px,l.seam_limit_px,WarningUnit::px,
                g.cause==rg::GateCause::none?Remedy::retake:RemedyOf(rg::GateCauseRemedy(g.cause),Remedy::retake));
            if(g.cause!=rg::GateCause::none)w.interpretations={std::string(rg::GateCauseCode(g.cause))};
            out.push_back(std::move(w));gate_warned=true;
        }
        // 11.30: what could not be measured is unverified when the gate made it a reason, and a notice (information) otherwise.
        // Blank rows (no content in either camera) are not a warning: they are in gate.rows only.
        const auto span=[&](const std::vector<rg::RowRange>& v,RegistrationWarning& w){
            if(v.empty())return;
            const double first=v.front().first,last=v.back().last;
            w.rows_px=RowSpan{first,last};w.rows_mm=RowSpan{first*pitch,last*pitch};
        };
        const bool dx_reason=HasOwnReason(g.gate_findings,"dx-unmeasured");
        if(dx_reason || !g.rows.unmeasured.empty()){
            auto w=Make(WarningCode::seam_dx_unmeasured,dx_reason?WarningClass::unverified:WarningClass::notice,WarningComponent::seam,
                static_cast<double>(g.rows.rows_measured-g.rows.rows_dx_measured),std::nullopt,WarningUnit::rows,dx_reason?Remedy::review_visually:Remedy::none);
            span(g.rows.unmeasured,w);
            out.push_back(std::move(w));gate_warned=gate_warned || dx_reason;
        }
        {
            std::vector<std::string> reasons;
            for(const char* reason:{"unconfirmed-content","one-sided-content","nothing-measured","content-unmeasured"})if(HasOwnReason(g.gate_findings,reason))reasons.emplace_back(reason);
            const auto rows=g.rows.rows_content_unmeasured+g.rows.rows_unconfirmed;
            if(!reasons.empty() || rows){
                auto w=Make(WarningCode::seam_unmeasurable,reasons.empty()?WarningClass::notice:WarningClass::unverified,WarningComponent::seam,static_cast<double>(rows),
                    std::nullopt,WarningUnit::rows,reasons.empty()?Remedy::none:Remedy::review_visually);
                span(g.rows.content_unmeasured,w);w.interpretations=std::move(reasons);
                gate_warned=gate_warned || !w.interpretations.empty();out.push_back(std::move(w));
            }
        }
        if(HasOwnReason(g.gate_findings,"placement-unverified")){
            out.push_back(Make(WarningCode::placement_unverified,WarningClass::unverified,WarningComponent::placement,0,std::nullopt,WarningUnit::rows,Remedy::review_visually));gate_warned=true;}
        if(HasReason(g.findings,"global-placement-mismatch")){
            out.push_back(Make(WarningCode::placement_mismatch,WarningClass::exceeded,WarningComponent::placement,0,std::nullopt,WarningUnit::rows,Remedy::retake));gate_warned=true;}
        // A gate that did not pass with nothing above to say why is a configuration problem of the gate (transition outside the
        // band, or the seam gate's own undetermined reason): never left without a warning.
        if(HasOwnReason(g.gate_findings,"transition-outside-band") || HasReason(g.findings,"transition-outside-band") || (g.state!=rg::GateState::pass && !gate_warned)){
            auto w=Make(WarningCode::gate_configuration,WarningClass::unverified,WarningComponent::gate,0,std::nullopt,WarningUnit::rows,Remedy::contact_technician);
            for(const auto& f:g.findings)w.interpretations.push_back(f.reason);
            out.push_back(std::move(w));
        }
    }else{
        auto w=Make(WarningCode::gate_not_run,WarningClass::unverified,WarningComponent::gate,0,std::nullopt,WarningUnit::rows,Remedy::review_visually);
        if(StageFailed(c.time_budget,ReviewStage::gate))w.interpretations.emplace_back(kGateFailed);
        out.push_back(std::move(w));
    }
    if(g.wide_search && g.wide_search->offset_px){
        auto w=Make(WarningCode::seam_wide_search,WarningClass::notice,WarningComponent::seam,*g.wide_search->offset_px,std::nullopt,WarningUnit::px,Remedy::none);
        if(g.wide_search->rows)w.rows_px=RowSpan{static_cast<double>(g.wide_search->rows->first),static_cast<double>(g.wide_search->rows->last)};
        w.source=WarningSource::design;out.push_back(std::move(w));
    }
    if(level!=GeometryLevel::g1){
        // A stage that threw is why the level is lower (the estimate: G4; layer 2: G2 at most).
        auto w=Make(WarningCode::geometry_level,WarningClass::notice,WarningComponent::geometry_level,0,std::nullopt,WarningUnit::rows,Remedy::none);
        if(HasText(c.registration.reasons,kEstimateFailed))w.interpretations.emplace_back(kEstimateFailed);
        if(HasText(p.reasons,kLayerTwoFailed))w.interpretations.emplace_back(kLayerTwoFailed);
        out.push_back(std::move(w));
    }
    const auto timed_out=[](const SkippedStage& s){return s.stage!=ReviewStage::gate && s.reason==kTimeBudgetReason;};
    if(std::any_of(c.time_budget.skipped.begin(),c.time_budget.skipped.end(),timed_out)){
        auto w=Make(WarningCode::time_budget,WarningClass::notice,WarningComponent::time_budget,0,std::nullopt,WarningUnit::rows,Remedy::none);
        w.source=WarningSource::design;
        for(const auto& s:c.time_budget.skipped)if(timed_out(s))w.interpretations.emplace_back(ReviewStageText(s.stage));
        out.push_back(std::move(w));
    }
    std::stable_sort(out.begin(),out.end(),[](const RegistrationWarning& a,const RegistrationWarning& b){return static_cast<int>(a.code)<static_cast<int>(b.code);});
    return out;
}

RegistrationAssessment DeriveRegistrationAssessment(const std::vector<RegistrationWarning>& warnings,rg::GateState gate_state){
    RegistrationAssessment a;
    for(const auto& w:warnings){
        if(w.warning_class==WarningClass::exceeded)a.exceeded.push_back(w.code);
        else if(w.warning_class==WarningClass::unverified)a.unverified.push_back(w.code);
    }
    if(!a.exceeded.empty())a.state=AssessmentState::outside_criteria;
    else if(!a.unverified.empty())a.state=AssessmentState::not_judged;
    else if(gate_state==rg::GateState::pass)a.state=AssessmentState::within_criteria;
    else throw std::logic_error("the gate did not pass and no warning says why");
    return a;
}

std::string RegistrationGeometryHash(const GeometryHashInput& in,const FixedCompositeSha256& hash){
    if(!hash)throw std::invalid_argument("geometry hash needs a SHA-256 provider");
    if(!LowerHex64(in.condition_sha256) || !LowerHex64(in.profile_fingerprint_sha256))throw std::invalid_argument("geometry hash needs the condition and profile hashes");
    std::string s="a0.registration-geometry-hash.v1\n";
    PutText(s,"condition",in.condition_sha256);PutText(s,"profileFingerprint",in.profile_fingerprint_sha256);
    const auto& g=in.raster.region_um;
    PutInt(s,"raster.left",g.left);PutInt(s,"raster.top",g.top);PutInt(s,"raster.right",g.right);PutInt(s,"raster.bottom",g.bottom);
    PutInt(s,"raster.dpi",in.raster.dpi);PutInt(s,"raster.width",in.raster.width_pixels);PutInt(s,"raster.height",in.raster.height_pixels);
    PutInt(s,"seamColumn",in.seam_column);PutText(s,"level",GeometryLevelCode(in.level));
    PutPose(s,"basePose",in.base_pose);PutPose(s,"appliedPose",in.applied_pose);
    PutHex(s,"surface.h0",in.applied_surface.h0_mm);PutHex(s,"surface.slopeX",in.applied_surface.slope_x);PutHex(s,"surface.slopeY",in.applied_surface.slope_y);
    if(in.applied_parallax){
        const auto& p=*in.applied_parallax;
        PutText(s,"parallax","present");PutHex(s,"parallax.baselineX",p.baseline.x);PutHex(s,"parallax.baselineY",p.baseline.y);
        PutHex(s,"parallax.rowSpacing",p.row_spacing_px);PutHex(s,"parallax.firstRow",p.first_row_px);PutInt(s,"parallax.samples",static_cast<std::int64_t>(p.q_px.size()));
        for(const double q:p.q_px)PutHex(s,"parallax.q",q);
        PutHex(s,"parallax.a",p.a_px);PutHex(s,"parallax.l",p.l_px);PutInt(s,"parallax.seamColumn",p.seam_column);
    }else PutText(s,"parallax","none");
    PutInt(s,"mixingWidth",in.mixing_width_px);PutInt(s,"omegaHalfWidth",in.omega_half_width_px);
    const auto digest=hash(std::vector<unsigned char>(s.begin(),s.end()));
    if(!LowerHex64(digest))throw std::invalid_argument("the SHA-256 provider gave no lower-case hex digest");
    return digest;
}

GeometryHashInput GeometryHashInputOf(const SavedFixedCompositeRegistration& r){
    GeometryHashInput in;
    in.condition_sha256=r.condition_sha256;in.profile_fingerprint_sha256=r.profile_fingerprint_sha256;in.raster=r.raster;in.seam_column=r.seam_column;
    in.level=r.registration.level.level;in.base_pose=r.base_pose;in.applied_pose=r.registration.level.mapping.pose;
    in.applied_surface=r.registration.level.mapping.surface;in.applied_parallax=r.registration.level.mapping.parallax;
    in.mixing_width_px=r.layout.mixing_width_px;in.omega_half_width_px=r.layout.omega_half_width_px();
    return in;
}

// The split's fields that follow from the period, the phase and the rows: each row's role, and the counts of the roles and of
// the rows with a hole (parallax.split). Throws std::invalid_argument for a phase not below the period.
static void DeriveSplit(pc::ParallaxResult& p){
    if(p.rows.empty())return;
    pc::ParallaxRows grid;grid.verify_period_rows=p.verify_period_rows;grid.verify_phase=p.verify_phase;
    auto& m=p.summary;m.rows_verify=m.rows_correct=m.rows_with_hole=0;
    for(std::size_t i=0;i<p.rows.size();++i){
        auto& row=p.rows[i];
        row.role=pc::IsVerifyRow(grid,i)?pc::ParallaxRowRole::verify:pc::ParallaxRowRole::correct;
        ++(row.role==pc::ParallaxRowRole::verify?m.rows_verify:m.rows_correct);
        m.rows_with_hole+=row.hole;
    }
}
// Everything derived except the geometry hash (which needs the SHA-256 provider).
static void DeriveRecordFields(SavedFixedCompositeRegistration& r){
    auto& c=r.registration;
    if(!r.limits.apply_beyond_envelope)throw std::invalid_argument("the record's lines have no applyBeyondEnvelope");
    DeriveSplit(c.parallax);
    c.registration.components=c.registration.has_estimate?RegistrationComponentsFromBase(c.registration.pose,r.base_pose,r.limits.envelope):std::vector<cr::ComponentCheck>{};
    c.registration.exceeded.clear();
    for(const auto& k:c.registration.components)if(k.exceeded)c.registration.exceeded.push_back(k.component);
    if(c.surface.state==SurfaceState::estimated && r.limits.surface_envelope)
        c.surface.exceeded={std::abs(c.surface.plane.h0_mm)>r.limits.surface_envelope->height_mm,c.surface.tilt_deg>r.limits.surface_envelope->tilt_deg};
    else c.surface.exceeded={};
    c.surface.applied=LevelAppliesEstimate(c.level.level) && c.surface.state==SurfaceState::estimated;
    c.level.applied=AppliedOfLevel(c.level.level,c.level.mapping);
    c.level.beyond_envelope_applied=BeyondApplied(c,r.limits);
    r.warnings=DeriveRegistrationWarnings(c,r.limits,r.base_pose,r.raster);
    r.assessment=DeriveRegistrationAssessment(r.warnings,c.gate.state);
}
void DeriveSavedFixedCompositeRegistration(SavedFixedCompositeRegistration& r,const FixedCompositeSha256& hash){
    DeriveRecordFields(r);
    r.registration.geometry_hash=RegistrationGeometryHash(GeometryHashInputOf(r),hash);
    r.registration.gate.geometry_hash=r.registration.geometry_hash;
}

std::string RegistrationWarningsJson(const std::vector<RegistrationWarning>& warnings){
    std::string s="[";
    for(std::size_t i=0;i<warnings.size();++i){const auto& w=warnings[i];
        const auto span=[](const std::optional<RowSpan>& r){return r?"{\"first\":"+Num(r->first)+",\"last\":"+Num(r->last)+"}":std::string("null");};
        s+=std::string(i?",":"")+"{\"code\":"+Text(WarningCodeText(w.code))+",\"class\":"+Text(WarningClassCode(w.warning_class))+",\"component\":"+Text(WarningComponentText(w.component))+
            ",\"value\":"+Num(w.value)+",\"limit\":"+OptNum(w.limit)+",\"unit\":"+Text(WarningUnitText(w.unit))+",\"rowsPx\":"+span(w.rows_px)+",\"rowsMm\":"+span(w.rows_mm)+
            ",\"interpretations\":"+Strings(w.interpretations)+",\"remedy\":"+Text(RemedyText(w.remedy))+",\"remedyAmountMm\":"+OptNum(w.remedy_amount_mm)+
            ",\"applied\":"+Bool(w.applied)+",\"source\":"+Text(WarningSourceText(w.source))+"}";
    }
    return s+"]";
}
std::string RegistrationAssessmentJson(const RegistrationAssessment& a){
    const auto codes=[](const std::vector<WarningCode>& v){std::string s="[";for(std::size_t i=0;i<v.size();++i)s+=(i?",":"")+Text(WarningCodeText(v[i]));return s+"]";};
    return "{\"state\":"+Text(AssessmentStateText(a.state))+",\"exceeded\":"+codes(a.exceeded)+",\"unverified\":"+codes(a.unverified)+",\"qualityAccepted\":false,\"acceptanceEvidence\":false}";
}
std::string FixedCompositeRegistrationJson(const SavedFixedCompositeRegistration& r){
    // The writer refuses a record whose derived fields are not their derivation: what it writes, the reader accepts.
    const auto text=RecordJsonUnchecked(r);
    const auto check=[&](bool ok,const char* what){if(!ok)throw std::logic_error(std::string("report v8 record is not consistent: ")+what);};
    check(*PointSourceFault(r.registration)==0,PointSourceFault(r.registration));
    check(LowerHex64(r.registration.geometry_hash) && r.registration.gate.geometry_hash==r.registration.geometry_hash,"geometry hash");
    auto derived=r;
    try{DeriveRecordFields(derived);}catch(const std::exception& error){throw std::logic_error(std::string("report v8 record cannot be derived: ")+error.what());}
    check(RecordJsonUnchecked(derived)==text,"derived fields");
    check(RegistrationWarningsJson(derived.warnings)==RegistrationWarningsJson(r.warnings),"warnings");
    check(RegistrationAssessmentJson(derived.assessment)==RegistrationAssessmentJson(r.assessment),"assessment");
    return text;
}

// ---- Levels and the time plan ----
GeometryLevelDecision SelectGeometryLevel(const GeometryLevelInput& in){
    if(!in.registration || !in.surface || !in.lines || !in.cover_check || !in.lines->apply_beyond_envelope)
        throw std::invalid_argument("geometry level needs the estimate, the surface, the lines with applyBeyondEnvelope and the cheap check");
    const auto& l=*in.lines;const bool apply=*l.apply_beyond_envelope;const auto& e=*in.registration;
    std::vector<std::pair<GeometryLevel,RegisteredMapping>> candidates;
    bool estimate_beyond=false,layer_two_beyond=false;
    if(Trusted(e)){
        const auto components=RegistrationComponentsFromBase(e.pose,in.base_pose,l.envelope);
        const bool surface=in.surface->state==SurfaceState::estimated && l.surface_envelope.has_value();
        const SurfacePlane plane=surface?in.surface->plane:SurfacePlane{};
        const bool surface_beyond=surface && (std::abs(plane.h0_mm)>l.surface_envelope->height_mm || in.surface->tilt_deg>l.surface_envelope->tilt_deg);
        estimate_beyond=AnyExceeded(components) || surface_beyond;
        if(!estimate_beyond || apply){
            if(in.parallax){
                layer_two_beyond=LayerTwoBeyond(*in.parallax,l).any;
                auto profile=ParallaxProfileOf(*in.parallax,in.seam_column);
                if(profile && (!layer_two_beyond || apply))candidates.push_back({GeometryLevel::g1,RegisteredMapping{e.pose,plane,std::move(profile)}});
            }
            candidates.push_back({GeometryLevel::g2,RegisteredMapping{e.pose,plane,std::nullopt}});
        }
    }else if(in.coarse_shift && (!ShiftBeyond(*in.coarse_shift,l) || apply)){
        auto pose=in.base_pose;pose.tx_mm+=in.coarse_shift->translation_x_mm;pose.ty_mm+=in.coarse_shift->translation_y_mm;
        candidates.push_back({GeometryLevel::g3,RegisteredMapping{pose,SurfacePlane{l.surface_envelope?in.coarse_shift->surface_height_mm:0.0,0,0},std::nullopt}});
    }
    candidates.push_back({GeometryLevel::g4,RegisteredMapping{in.base_pose,{},std::nullopt}});
    GeometryLevelDecision d;
    for(auto& [level,mapping]:candidates){
        MappingCoverCheck check{};
        try{check=in.cover_check(mapping);}catch(const std::invalid_argument&){check={false,false};}
        if(check.seam_covered && check.output_covered){
            d.level=level;d.mapping=std::move(mapping);
            d.applied=AppliedOfLevel(d.level,d.mapping);
            d.beyond_envelope_applied=level==GeometryLevel::g1?(estimate_beyond || layer_two_beyond):level==GeometryLevel::g2?estimate_beyond:
                level==GeometryLevel::g3?ShiftBeyond(*in.coarse_shift,l):false;
            return d;
        }
        if(level==GeometryLevel::g4)throw GeometryLevelUnavailable(check.seam_covered,check.seam_covered?"the condition's mapping leaves an output pixel uncovered":
            "the condition's mapping leaves the seam outside the overlap");
        d.reasons.emplace_back(check.seam_covered?"output-uncovered-at-level":"seam-outside-overlap-at-level");
    }
    throw std::logic_error("no geometry level");
}

bool CaptureBandReachesLayerTwo(const CaptureRegistrationDesign& design,std::uint32_t omega_half_width_px){
    const auto half=design.band_measure_half_width_px;
    if(half==0)return true;
    const double radius=design.parallax.column_radius_px;
    if(!std::isfinite(radius) || radius<0 || radius>65535)return false;
    return half>=omega_half_width_px && half>=static_cast<std::uint64_t>(std::ceil(radius))+design.parallax.point_patch_px;
}

TimeBudgetRecord PlanReviewStages(double remaining_ms,const ReviewStageCosts& costs){
    if(!std::isfinite(remaining_ms) || remaining_ms<0 || !std::isfinite(costs.tail_reserve_ms) || costs.tail_reserve_ms<0)
        throw std::invalid_argument("the time plan needs a finite remaining time and tail reserve");
    double total=0;
    for(const double c:costs.stage_ms){if(!std::isfinite(c) || c<0)throw std::invalid_argument("a stage cost is not finite and non-negative");total+=c;}
    const double budget=remaining_ms-costs.tail_reserve_ms;
    std::array<bool,kReviewStageTexts.size()> keep{};keep.fill(true);
    for(std::size_t i=0;i<keep.size() && total>budget;++i){keep[i]=false;total-=costs.stage_ms[i];}
    TimeBudgetRecord t;t.cost_model=costs;
    for(std::size_t i=0;i<keep.size();++i){
        const auto stage=kReviewStageTexts[i].first;
        if(keep[i])t.planned.push_back(stage);else t.skipped.push_back({stage,"time-budget"});
    }
    return t;
}

CaptureRegistrationDesign CaptureRegistrationDesignDraft(){
    CaptureRegistrationDesign d{sg::kSeamGateDesignCalibrated,sg::kSeamGateDesignCalibrated,{},pc::kParallaxDesignDraft,{}};
    // The band is measured once for layer 2's points and the gate's anchors: layer 2's lattice and patch, the gate's global check.
    d.band.point_spacing_px=static_cast<std::uint32_t>(d.parallax.column_spacing_px);d.band.patch_px=d.parallax.point_patch_px;
    auto& g=d.gate;
    g.minimum_baseline_component=0.5;g.lift_ratio=3;g.lift_share=0.9;
    g.wide_search_radius_px=40;g.wide_search_half_width_px=64;g.wide_search_block_rows=96;g.wide_search_minimum_correlation=0.5;
    g.wide_search_secondary_ratio=0.8;g.wide_search_consistency_px=1.5;g.wide_search_minimum_blocks=2;g.wide_search_trigger_share=0.5;
    // 11.30 (*proposal*, for the owner to confirm: it changes O3 of #385 for Omega): at most one row with content in four left
    // unmeasured.
    g.maximum_unmeasured_content_share=0.25;
    // In the order of ReviewStage: wide-search-diagnostic, band-measurement, parallax, fold-refit, fine-and-fit, coarse, gate.
    // The typical time of each stage, rounded up, on the capture path measured on the review's threads on a loaded 4-core PC
    // (*estimates*; a quieter PC is faster, and the measurements are kept with the issue, not here). Typical rather than the
    // slowest: the first plan adds them all, and the sum of the slowest drops stages that the capture then has time for. The
    // tail is the cheap check of the level, the composite and the JPEG with its verification. The wide search did not run in
    // those captures (G1); its cost is the first estimate.
    // The band stage and the gate after #417's two cuts (the band stage's first band_stage_anchors anchors, the correct rows' reference
    // measurement with one anchor), set from the process CPU time before and after and the wall time of several captures on two
    // threads on a PC that other builds were using (the measurements are kept with the issue, not here). The gate's earlier value did
    // not include the reference measurement. *To be measured* again on a quiet PC.
    d.costs.stage_ms={5000,16000,1000,0,25000,10000,6000};
    d.costs.tail_reserve_ms=30000;
    // About one outer iteration of the estimator's fine stage on that PC. With all stages kept, the estimator stops when less than
    // 68 s are left (the band, layer 2, the gate, the tail and this margin).
    d.estimator_stop_margin_ms=15000;
    // The band stage measures the seam column +- 256 px, at the lattice columns the estimator's band has (the half widths differ by a
    // multiple of the point spacing). Layer 2 is given the column radius (192 px) and the point patch (32 px) at least
    // (CaptureBandReachesLayerTwo), and its regression reads a point up to Omega's half width + the gate's footprint + the kernel's
    // reach (3 sigma) + the point's own footprint from the seam: 225 px with Omega's 12 px, 257 px for a point remeasured with the
    // larger patch, and the lattice has no column between 248 and 264. A wider band measured points nothing reads.
    d.band_measure_half_width_px=256;
    // The band stage evaluates the first global_minimum_anchors anchors only (the band design's maximum is 64): that band is kept for
    // a level that places B by the estimate, and the anchors then only guard against a placement a period off. The rest of the
    // anchors were most of the stage's time (each searches a window of the global radius). One anchor is not enough: on a pattern
    // with a lookalike a period off, the contradicting anchor was not the first of the order and four found it. A capture whose full
    // set verifies the placement with the 5th or later anchors reports the estimate as the basis instead. (*to be measured*)
    d.band_stage_anchors=d.band.global_minimum_anchors;
    // One strip per measurement (the performance lane's proposal, *to be measured*).
    d.omega.strip_rows=4096;d.band.strip_rows=4096;
    // One verify row in six, from the first (the owner's decision).
    d.verify_period_rows=6;d.verify_phase=0;
    return d;
}

namespace {
// Which stages still run: the first plan, and at each stage a plan again with the time left and the costs of the stages not yet
// run. Only the choice of stages depends on the time.
class StagePlan {
public:
    // Without a prior record: the first plan of the capture. With one: the choices it made stay, and the stages it ran count as done.
    StagePlan(const std::function<double()>& remaining,const ReviewStageCosts& costs,const TimeBudgetRecord* prior=nullptr,
        std::initializer_list<ReviewStage> not_run={}):remaining_(remaining),costs_(costs){
        keep_.fill(true);done_.fill(false);why_.fill(kTimeBudgetReason);
        if(prior){
            for(const auto& s:prior->skipped){keep_[static_cast<std::size_t>(s.stage)]=false;why_[static_cast<std::size_t>(s.stage)]=s.reason==kStageFailedReason?kStageFailedReason:kTimeBudgetReason;}
            for(const auto s:prior->planned)done_[static_cast<std::size_t>(s)]=true;
            for(const auto s:not_run)done_[static_cast<std::size_t>(s)]=false;
        }else if(remaining_){const auto first=PlanReviewStages(Remaining(),costs_);for(const auto& s:first.skipped)keep_[static_cast<std::size_t>(s.stage)]=false;}
    }
    // Whether the stage runs now; a stage dropped here is recorded as skipped.
    bool Begin(ReviewStage stage){
        const auto i=static_cast<std::size_t>(stage);
        if(!keep_[i])return false;
        if(remaining_){
            ReviewStageCosts rest=costs_;
            for(std::size_t j=0;j<keep_.size();++j)if(done_[j] || !keep_[j])rest.stage_ms[j]=0;
            const auto again=PlanReviewStages(Remaining(),rest);
            for(const auto& s:again.skipped)if(s.stage==stage){keep_[i]=false;return false;}
        }
        started_=remaining_?std::optional<double>(Remaining()):std::nullopt;
        return true;
    }
    std::optional<double> End(ReviewStage stage){
        done_[static_cast<std::size_t>(stage)]=true;
        if(!started_)return std::nullopt;
        return *started_-Remaining();
    }
    // A stage dropped for the time, or (kStageFailedReason) because it threw.
    void Drop(ReviewStage stage,std::string_view why=kTimeBudgetReason){keep_[static_cast<std::size_t>(stage)]=false;why_[static_cast<std::size_t>(stage)]=why;}
    [[nodiscard]] bool Kept(ReviewStage stage) const {return keep_[static_cast<std::size_t>(stage)];}
    [[nodiscard]] TimeBudgetRecord Record() const {
        TimeBudgetRecord t;t.cost_model=costs_;
        for(std::size_t i=0;i<keep_.size();++i){
            const auto stage=kReviewStageTexts[i].first;
            if(keep_[i])t.planned.push_back(stage);else t.skipped.push_back({stage,std::string(why_[i])});
        }
        return t;
    }
    [[nodiscard]] std::optional<double> Now() const {return remaining_?std::optional<double>(Remaining()):std::nullopt;}
private:
    double Remaining() const {const double r=remaining_();return std::isfinite(r) && r>0?r:0.0;}
    std::function<double()> remaining_;
    ReviewStageCosts costs_;
    std::array<bool,kReviewStageTexts.size()> keep_{},done_{};
    std::array<std::string_view,kReviewStageTexts.size()> why_{};
    std::optional<double> started_;
};

// The measurer with at most `anchors` global placement checks (0, or a number the design's maximum does not exceed: its own). The
// anchors are the first ones of the measurer's spread order, so they are the first of the design's own anchors, with the same results;
// the points, blocks and everything else the measurer makes do not depend on how many are evaluated. The measurement carries the
// caller's design again (MeasureGateOmega does the same), so the gate sees the constants it was given.
sg::SeamMeasurement MeasureSeamWithAnchors(const render::BgrImage& a,const render::BgrImage& b,const sg::SeamGateGeometry& geometry,
    const sg::SeamGateDesign& design,const sg::SeamGateLimits& limits,const sg::DisplacementPrediction& prediction,unsigned threads,std::uint32_t anchors){
    if(!anchors || anchors>=design.global_maximum_evaluations)return sg::MeasureSeam(a,b,geometry,design,limits,prediction,threads);
    auto fewer=design;
    fewer.global_maximum_evaluations=anchors;
    fewer.global_minimum_anchors=std::min(design.global_minimum_anchors,anchors);
    auto m=sg::MeasureSeam(a,b,geometry,fewer,limits,prediction,threads);
    m.design=design;
    return m;
}

// Runs one stage of the capture. A stage that throws is dropped and the capture goes on one level lower: the composite is made in
// every case (the owner's decision 1). std::bad_alloc is not a stage's failure and goes to the caller.
template<class Stage> bool RunStage(Stage&& stage){
    try{stage();return true;}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){return false;}
}
// The estimate as recorded when it did not run (time-budget) or threw (estimate-failed): undetermined, no estimate, so the level is
// G4 (G3 needs the coarse stage's shift).
cr::RegistrationResult UnestimatedRegistration(const RegistrationLines& lines,std::string_view reason){
    cr::RegistrationResult e;
    e.state=cr::RegistrationState::undetermined;e.reasons={std::string(reason)};
    e.surface.state=lines.surface_envelope?SurfaceState::not_estimated:SurfaceState::disabled;
    e.surface.estimate.state=e.surface.state;
    return e;
}

// The points layer 2 is given (design 11.31). The band's own points, unless the design selects the fast measurer and its points pass
// the stops; the choice is recorded in `source` (empty for the band). The fast measurer measures on the pair of M0 (B placed by the
// pose and the plane, no layer 2) at the band's lattice. Anything it throws except std::bad_alloc leaves the band's points.
std::vector<pc::ParallaxPoint> PointsForLayerTwo(const CaptureRegistrationRequest& r,const MappedFixedSeamInput& pair,const sg::SeamMeasurement& band,
    const pc::BaselineGeometry& baseline,std::optional<LayerTwoPointSource>& source){
    source.reset();
    if(r.design.layer_two_points!=LayerTwoPoints::fast_measurer)return pc::PointsFromSeamMeasurement(band);
    const auto& design=r.design.fast_measurer;
    LayerTwoPointSource record;
    record.edge_share_line=design.edge_share_line;
    try{
        fast_seam_measurer::Request fq;
        fq.camera_a=r.camera_a;fq.camera_b=r.camera_b;fq.map_a=pair.camera_a;fq.map_b=pair.camera_b;fq.output_height=pair.height;
        for(const auto& p:band.points)if(p.status!=sg::PointStatus::outside_coverage)fq.lattice.push_back({p.x,p.y});
        fq.lattice_spacing_px=r.design.band.point_spacing_px;fq.patch_px=r.design.parallax.point_patch_px;
        fq.baseline=baseline.direction;fq.minimum_baseline_component=r.design.parallax.minimum_baseline_component;fq.thread_count=r.thread_count;
        auto measured=fast_seam_measurer::Measure(fq,design);
        auto selection=fast_seam_measurer::Select(measured,fq,design);
        record.edge_share=selection.edge_share;
        if(selection.use){
            record.fast_measurer_used=true;record.points_used=static_cast<std::uint32_t>(selection.points.size());
            source=record;
            return std::move(selection.points);
        }
        record.fallback_reason=selection.fallback_reason;
    }catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){
        record.edge_share.reset();record.fallback_reason=std::string(kMeasurerFailed);
    }
    source=record;
    return pc::PointsFromSeamMeasurement(band);
}

// The gate's request for a mapping (the composite's pair is made from the same mapping by the same maker).
rg::RegistrationGateRequest GateRequestOf(const CaptureRegistrationRequest& r,const PreparedCaptureRegistration& p,const MappedFixedSeamInput& mapped,
    const RegisteredMapping& mapping){
    rg::RegistrationGateRequest g;
    g.camera_a=r.camera_a;g.camera_b=r.camera_b;g.mapped=mapped;g.registered=mapping;g.layout=r.estimator.geometry;g.blend=r.blend;
    g.gate_design=r.design.omega;g.gate_limits={r.lines.seam_limit_px,r.lines.seam_guard_band_px,"condition"};g.design=r.design.gate;
    if(r.lines.surface_envelope){
        const double shift=std::ceil(r.lines.surface_envelope->height_mm*p.baseline_px_per_mm);
        if(std::isfinite(shift) && shift>g.design.wide_search_radius_px && shift<65536)g.design.wide_search_radius_px=static_cast<std::uint32_t>(shift);
    }
    g.baseline=p.baseline;
    if(mapping.parallax){
        const auto profile=*mapping.parallax;
        g.correction=[profile](render::Point at,render::Point& shift){
            const double s=ParallaxShiftAtRow(profile,at.y)*ParallaxSeamWeight(std::abs(at.x-static_cast<double>(profile.seam_column)),profile.a_px,profile.l_px);
            shift={s*profile.baseline.x,s*profile.baseline.y};return true;
        };
    }
    g.baseline_px_per_mm=p.baseline_px_per_mm;g.output_pixel_mm=25.4/r.parameters->raster.dpi;g.thread_count=r.thread_count;
    return g;
}
// The gate rows of Omega's lattice (seam_gate::MeasureSeam puts its rows at the first band row + spacing / 2, then every spacing).
pc::ParallaxRows RowsOfOmega(const rg::RegistrationGateLayout& layout,const sg::SeamGateDesign& design,std::uint32_t seam_column){
    pc::ParallaxRows rows;
    rows.output_width=layout.omega.width;rows.output_height=layout.omega.height;rows.seam_column=seam_column;rows.omega_half_width_px=layout.omega_half_width_px;
    rows.gate_footprint=pc::GateFootprint(design,true);rows.row_step=design.point_spacing_px;
    std::uint32_t y0=layout.omega.height,y1=0;
    for(std::uint32_t y=0;y<layout.omega.height;++y)if(layout.omega.band[y].begin<layout.omega.band[y].end){y0=std::min(y0,y);y1=y+1;}
    if(y0>=y1)return rows;
    rows.first_row=y0+design.point_spacing_px/2;
    for(std::uint32_t y=rows.first_row;y<y1;y+=design.point_spacing_px)++rows.row_count;
    return rows;
}
}

PreparedCaptureRegistration PrepareCaptureRegistration(const CaptureRegistrationRequest& r){
    if(!r.camera_a || !r.camera_b || !r.parameters || !r.cover_check || !r.lines.apply_beyond_envelope || r.thread_count<1 || r.thread_count>256)
        throw std::invalid_argument("capture registration needs both images, the parameters, the cheap check, applyBeyondEnvelope and 1..256 threads");
    if(r.estimator.geometry.mixing_width_px!=r.blend.width_pixels)throw std::invalid_argument("the mixing width is not the blend's");
    if(!std::isfinite(r.design.estimator_stop_margin_ms) || r.design.estimator_stop_margin_ms<0)
        throw std::invalid_argument("the estimator's stop margin is not finite and non-negative");
    if(r.design.verify_period_rows && r.design.verify_phase>=r.design.verify_period_rows)
        throw std::invalid_argument("the verify rows' phase is not below their period");
    if(!CaptureBandReachesLayerTwo(r.design,r.estimator.geometry.omega_half_width_px()))
        throw std::invalid_argument("the measured band does not hold layer 2's column radius and point patch, or Omega");
    if(r.design.layer_two_points==LayerTwoPoints::fast_measurer){
        fast_seam_measurer::ValidateDesign(r.design.fast_measurer);
        if(r.design.parallax.point_patch_px%4!=0 || r.design.band.point_spacing_px<1)throw std::invalid_argument("the fast measurer needs a point patch that is a multiple of 4 and a lattice spacing");
    }
    // The estimator takes the condition's surface range and base pose (S2): the range is applied to p-hat - p_b.
    auto settings=r.estimator;settings.surface_envelope=r.lines.surface_envelope;settings.base_pose=r.base_pose;
    PreparedCaptureRegistration out;
    StagePlan plan(r.remaining_ms,r.design.costs);
    auto& c=out.capture;
    {
        const auto calibrated=pc::MakeBaselineGeometry(*r.parameters,r.base_pose,0.0);
        out.baseline=calibrated.direction;out.baseline_px_per_mm=calibrated.k_px_per_mm;
    }
    // 1 The pose and the paper plane. The estimator runs the coarse and the fine stages in one call, so both must be allowed; it
    // stops (time-budget, the coarse result kept) when the time left would not hold the stages after it.
    const bool fine=plan.Begin(ReviewStage::fine_and_fit);
    if(fine && plan.Kept(ReviewStage::coarse)){
        (void)plan.Begin(ReviewStage::coarse);
        cr::RegistrationControl control;
        if(r.remaining_ms){
            // The stages after it that the plan still keeps (a stage the first plan dropped needs no time), the tail, and the margin
            // of the estimator's last outer iteration.
            double after=r.design.costs.tail_reserve_ms+r.design.estimator_stop_margin_ms;
            for(const auto stage:{ReviewStage::band_measurement,ReviewStage::parallax,ReviewStage::gate})
                if(plan.Kept(stage))after+=r.design.costs.stage_ms[static_cast<std::size_t>(stage)];
            control.stop=[remaining=r.remaining_ms,after]{return remaining()<after;};
        }
        // Settings the estimator refuses, or an estimate that throws: G4, with the reason (estimate-failed).
        if(!RunStage([&]{cr::ValidateRegistrationSettings(settings);c.registration=cr::EstimateRegistration(*r.camera_a,*r.camera_b,*r.parameters,settings,r.thread_count,control);})){
            c.registration=UnestimatedRegistration(r.lines,kEstimateFailed);
            plan.Drop(ReviewStage::fine_and_fit,kStageFailedReason);plan.Drop(ReviewStage::coarse,kStageFailedReason);
        }
        out.milliseconds.stage[static_cast<std::size_t>(ReviewStage::coarse)]=plan.End(ReviewStage::coarse);
        out.milliseconds.stage[static_cast<std::size_t>(ReviewStage::fine_and_fit)]=plan.End(ReviewStage::fine_and_fit);
        if(std::find(c.registration.reasons.begin(),c.registration.reasons.end(),"time-budget")!=c.registration.reasons.end())plan.Drop(ReviewStage::fine_and_fit);
    }else{
        plan.Drop(ReviewStage::fine_and_fit);plan.Drop(ReviewStage::coarse);
        c.registration=UnestimatedRegistration(r.lines,kTimeBudgetReason);
    }
    c.surface=c.registration.surface.estimate;c.surface.applied=false;
    // The G3 candidate: the coarse stage's uniform shift (h0 and the translation across the baseline), from p_b.
    const auto& shift=c.registration.diagnostics.coarse_shift;
    if(!Trusted(c.registration) && shift.determined)
        c.coarse_shift=UniformShift{r.lines.surface_envelope?shift.surface.h0_mm:0.0,shift.pose.tx_mm-r.base_pose.tx_mm,shift.pose.ty_mm-r.base_pose.ty_mm};
    // 2 Layer 2 (S4) on the band measured once with B placed by the pose and the plane (M0); the same measurement gives the gate its
    // anchors. 3 The fold refit (S5) is not in the build.
    const SurfacePlane plane=c.surface.state==SurfaceState::estimated?c.surface.plane:SurfacePlane{};
    if(!r.lines.max_lift_mm)c.parallax=pc::UnestimatedParallax(pc::ParallaxState::disabled,"disabled");
    else if(!Trusted(c.registration))c.parallax=pc::UnestimatedParallax(pc::ParallaxState::not_applied,"pose-untrusted");
    else if(!plan.Begin(ReviewStage::band_measurement)){
        // Layer 2 has no points without the band.
        plan.Drop(ReviewStage::parallax);
        c.parallax=pc::UnestimatedParallax(pc::ParallaxState::not_applied,"time-budget");
    }else if(!RunStage([&]{
        const RegisteredMapping m0{c.registration.pose,plane,std::nullopt};
        const auto pair=RegisteredDocumentMappedFixedSeamInput(*r.parameters,m0,r.estimator.geometry.seam_column);
        auto request=GateRequestOf(r,out,pair,m0);
        request.band_measure_half_width_px=r.design.band_measure_half_width_px;   // the band stage only; the gate's own band stays the estimator's
        out.layout_before_layer_two=rg::MakeRegistrationGateLayout(request);
        // Few anchors (design.band_stage_anchors): this band is kept only for a level that places B by the estimate.
        out.band=MeasureSeamWithAnchors(*r.camera_a,*r.camera_b,out.layout_before_layer_two->band,r.design.band,request.gate_limits,request.prediction,r.thread_count,r.design.band_stage_anchors);
        out.milliseconds.stage[static_cast<std::size_t>(ReviewStage::band_measurement)]=plan.End(ReviewStage::band_measurement);
        if(!plan.Begin(ReviewStage::parallax))c.parallax=pc::UnestimatedParallax(pc::ParallaxState::not_applied,"time-budget");
        else{
            pc::ParallaxRequest q;
            q.baseline=pc::MakeBaselineGeometry(*r.parameters,c.registration.pose,plane.h0_mm);
            q.points=PointsForLayerTwo(r,pair,*out.band,q.baseline,c.point_source);
            q.rows=RowsOfOmega(*out.layout_before_layer_two,r.design.omega,r.estimator.geometry.seam_column);
            q.rows.verify_period_rows=r.design.verify_period_rows;q.rows.verify_phase=r.design.verify_phase;
            q.design=r.design.parallax;q.max_lift_mm=r.lines.max_lift_mm;q.apply_beyond_line=r.lines.apply_beyond_envelope;q.thread_count=r.thread_count;
            c.parallax=q.rows.row_count?pc::EstimateParallax(q):pc::UnestimatedParallax(pc::ParallaxState::not_applied,"too-few-points");
            out.milliseconds.stage[static_cast<std::size_t>(ReviewStage::parallax)]=plan.End(ReviewStage::parallax);
        }
    })){
        // Layer 2, or the mapping M0 and the band under it, threw: the level goes on without layer 2 (G2 at most), with the reason
        // (layer-two-failed). A band measured before the throw is kept for the gate; a band that was not measured is measured
        // again by the gate on its own layout.
        if(!out.band)out.layout_before_layer_two.reset();
        c.point_source.reset();
        plan.Drop(ReviewStage::parallax,kStageFailedReason);
        c.parallax=pc::UnestimatedParallax(pc::ParallaxState::not_applied,std::string(kLayerTwoFailed));
    }
    if(plan.Begin(ReviewStage::fold_refit))out.milliseconds.stage[static_cast<std::size_t>(ReviewStage::fold_refit)]=plan.End(ReviewStage::fold_refit);
    // 4 The level and its mapping, with the cheap check.
    GeometryLevelInput level;
    level.registration=&c.registration;level.surface=&c.surface;level.parallax=c.parallax.Applied()?&c.parallax:nullptr;level.coarse_shift=c.coarse_shift;
    level.base_pose=r.base_pose;level.lines=&r.lines;level.seam_column=r.estimator.geometry.seam_column;level.cover_check=r.cover_check;
    c.level=SelectGeometryLevel(level);
    // The band measured with M0 is the band of the chosen mapping only when the level placed B by the estimate.
    if(c.level.level!=GeometryLevel::g1 && c.level.level!=GeometryLevel::g2){out.band.reset();out.layout_before_layer_two.reset();}
    // 5 The composite, with the pair the gate will measure.
    const auto before=plan.Now();
    out.mapped=RegisteredDocumentMappedFixedSeamInput(*r.parameters,c.level.mapping,r.estimator.geometry.seam_column);
    // On the request's threads: the bytes and the record do not depend on the count (seam_blend.hpp, Threads).
    out.composite=RenderMappedFixedSeam(*r.camera_a,*r.camera_b,out.mapped,r.estimator.geometry.seam_column,r.blend,{},r.thread_count);
    if(before)out.milliseconds.composite=*before-*plan.Now();
    c.gate=rg::RegistrationGateResult{};
    c.time_budget=plan.Record();
    return out;
}

std::uint32_t AlignGateOmegaToRows(rg::RegistrationGateLayout& layout,std::uint32_t first_row,std::uint32_t row_step,std::uint32_t row_count,std::uint32_t spacing){
    if(!spacing || row_step!=spacing || !row_count || first_row<spacing/2)throw std::invalid_argument("layer 2's rows are not on the gate's lattice");
    auto& o=layout.omega;
    if(o.band.size()!=o.height || (!o.transition.empty() && o.transition.size()!=o.height))throw std::invalid_argument("the layout's rows do not match its height");
    const std::uint64_t begin=first_row-spacing/2,end=begin+static_cast<std::uint64_t>(row_count)*spacing;
    const auto filled=[&](std::uint64_t y){return y<o.height && o.band[y].begin<o.band[y].end;};
    std::uint64_t first=end;
    for(std::uint64_t y=begin;y<end && y<o.height;++y)if(filled(y)){first=y;break;}
    // The first row of layer 2's lattice at or after the first row the final Omega covers.
    const std::uint64_t start=first>=end?end:begin+(first-begin+spacing-1)/spacing*spacing;
    std::uint32_t dropped=0;
    std::vector<rg::RowRange> out=layout.outside_measurable;
    for(std::uint32_t y=0;y<o.height;++y){
        if(y>=start && y<end)continue;
        const bool band=o.band[y].begin<o.band[y].end,mixes=!o.transition.empty() && o.transition[y].begin<o.transition[y].end;
        if(!band && !mixes)continue;
        o.band[y]={};
        if(mixes){o.transition[y]={};out.push_back({y,y});}
        ++dropped;
    }
    std::sort(out.begin(),out.end(),[](const rg::RowRange& a,const rg::RowRange& b){return a.first<b.first;});
    layout.outside_measurable.clear();
    for(const auto& range:out){
        auto& v=layout.outside_measurable;
        if(!v.empty() && range.first<=v.back().last+1)v.back().last=std::max(v.back().last,range.last);else v.push_back(range);
    }
    // The measurer's lattice now starts on layer 2's rows (seam_gate::MeasureSeam: the first band row + spacing / 2).
    for(std::uint32_t y=0;y<o.height;++y)if(filled(y)){
        if(y<begin || (y-begin)%spacing!=0 || y>=end)throw std::logic_error("the gate's rows still leave layer 2's");
        break;
    }
    return dropped;
}

static void MeasureGateStages(const CaptureRegistrationRequest& r,PreparedCaptureRegistration& prepared){
    auto& c=prepared.capture;
    // The stages already decided keep their choice; the gate's own stages are planned again with the time left. The band was
    // measured already when layer 2 used it.
    const bool band_done=prepared.band.has_value();
    StagePlan plan(r.remaining_ms,r.design.costs,&c.time_budget,
        band_done?std::initializer_list<ReviewStage>{ReviewStage::wide_search_diagnostic,ReviewStage::gate}:
            std::initializer_list<ReviewStage>{ReviewStage::wide_search_diagnostic,ReviewStage::band_measurement,ReviewStage::gate});
    const bool wide=plan.Begin(ReviewStage::wide_search_diagnostic);if(wide)(void)plan.End(ReviewStage::wide_search_diagnostic);
    const bool band=band_done || plan.Begin(ReviewStage::band_measurement);
    if(band && !band_done)(void)plan.End(ReviewStage::band_measurement);
    if(!plan.Begin(ReviewStage::gate)){
        c.gate=rg::RegistrationGateResult{};c.gate.state=rg::GateState::not_run;
        if(band && !band_done)plan.Drop(ReviewStage::band_measurement);
        if(wide)plan.Drop(ReviewStage::wide_search_diagnostic);
        c.time_budget=plan.Record();
        return;
    }
    auto g=GateRequestOf(r,prepared,prepared.mapped,c.level.mapping);
    g.wide_search=wide && (c.level.level==GeometryLevel::g3 || c.level.level==GeometryLevel::g4);
    // 11.30: B placed by a trusted estimate whose own wide check contradicted nothing verifies the placement when the anchors do not.
    if(LevelAppliesEstimate(c.level.level) && Trusted(c.registration) && c.registration.diagnostics.global_contradicted==0)
        g.placement_by_estimate=rg::PlacementByEstimate{c.registration.diagnostics.unique_blocks,c.registration.diagnostics.consistent_blocks};
    g.measure_whole_band_for_anchors=band;
    auto layout=rg::MakeRegistrationGateLayout(g);
    // The band measured before layer 2 (anchors and the diagnostic) is decided on the band it was measured on: layer 2 can move B's
    // coverage at the band's top and bottom rows, and the band's points must lie in the band they are read against.
    if(band_done && prepared.layout_before_layer_two)layout.band=prepared.layout_before_layer_two->band;
    // The gate measures on layer 2's rows (its holes are built around them), and decides on the verify rows of layer 2's split.
    if(c.level.mapping.parallax && !c.parallax.rows.empty()){
        const auto count=static_cast<std::uint32_t>(c.parallax.rows.size());
        (void)AlignGateOmegaToRows(layout,c.parallax.first_row,c.parallax.row_step,count,r.design.omega.point_spacing_px);
        if(c.parallax.verify_period_rows)
            (void)rg::SplitGateOmega(layout,c.parallax.first_row,c.parallax.row_step,count,c.parallax.verify_period_rows,c.parallax.verify_phase,r.design.omega.point_spacing_px);
    }
    rg::RegistrationGateMeasurements m;
    m.omega=rg::MeasureGateOmega(g,layout,band);
    // The correct rows, for reference only (the same pair and design; never in the decision). Its global placement check is read by
    // nothing, so one anchor is the least it can run (the points do not depend on it).
    if(layout.corrected_omega)m.corrected=MeasureSeamWithAnchors(*r.camera_a,*r.camera_b,*layout.corrected_omega,g.gate_design,g.gate_limits,g.prediction,g.thread_count,1);
    if(band_done)m.band=std::move(*prepared.band);
    else if(band)m.band=sg::MeasureSeam(*r.camera_a,*r.camera_b,layout.band,r.design.band,g.gate_limits,g.prediction,g.thread_count);
    prepared.band.reset();
    c.gate=rg::DecideRegistrationGate(g,layout,std::move(m));
    prepared.milliseconds.stage[static_cast<std::size_t>(ReviewStage::gate)]=plan.End(ReviewStage::gate);
    c.time_budget=plan.Record();
}

void MeasureCaptureRegistrationGate(const CaptureRegistrationRequest& r,PreparedCaptureRegistration& prepared){
    const auto before=prepared.capture.time_budget;
    const bool band_done=prepared.band.has_value();
    if(RunStage([&]{MeasureGateStages(r,prepared);}))return;
    // The gate threw: the composite stands and is not judged (the gate not run, its warning unverified, gate-failed). The stages
    // the gate runs with it (the wide search, and the band when it was not measured before layer 2) go with it.
    auto& c=prepared.capture;
    c.gate=rg::RegistrationGateResult{};c.gate.state=rg::GateState::not_run;
    prepared.band.reset();
    prepared.milliseconds.stage[static_cast<std::size_t>(ReviewStage::gate)].reset();
    std::array<std::optional<std::string>,kReviewStageTexts.size()> skipped;
    for(const auto& s:before.skipped)skipped[static_cast<std::size_t>(s.stage)]=s.reason;
    for(const auto stage:{ReviewStage::wide_search_diagnostic,ReviewStage::band_measurement,ReviewStage::gate}){
        if(stage==ReviewStage::band_measurement && band_done)continue;
        auto& s=skipped[static_cast<std::size_t>(stage)];
        if(!s)s=std::string(kStageFailedReason);
    }
    TimeBudgetRecord t;t.cost_model=before.cost_model;
    for(std::size_t i=0;i<skipped.size();++i){
        const auto stage=kReviewStageTexts[i].first;
        if(skipped[i])t.skipped.push_back({stage,*skipped[i]});else t.planned.push_back(stage);
    }
    c.time_budget=std::move(t);
}

SavedFixedCompositeRegistration ReadFixedCompositeRegistrationReport(std::string_view text,const FixedCompositeSha256& hash){
    if(!hash)throw std::invalid_argument("report v8 reader needs a SHA-256 provider");
    const auto root=ParseText(text);
    if(root.kind!=json::JsonKind::object)Reject("root");
    const auto& fcr=At(root,"fixedCompositeRegistration");const auto& warnings=At(root,"warnings");const auto& assessment=At(root,"assessment");
    auto r=ReadRecord(fcr);
    for(const auto& w:ReadArray(warnings))r.warnings.push_back(ReadWarning(w));
    r.assessment=ReadAssessment(assessment);
    // The level's mapping is the one its estimates give (11.18).
    const auto& c=r.registration;const auto& m=c.level.mapping;
    switch(c.level.level){
    case GeometryLevel::g1:
    case GeometryLevel::g2:{
        if(!Trusted(c.registration) || !SamePose(m.pose,c.registration.pose))Reject("level pose");
        const SurfacePlane expected=c.surface.state==SurfaceState::estimated?c.surface.plane:SurfacePlane{};
        if(!SamePlane(m.surface,expected))Reject("level surface");
        if(c.level.level==GeometryLevel::g1){
            // The samples are layer 2's rows on its grid, with w's a and L from its design (parallax_correction::ToParallaxProfile).
            if(!m.parallax || !c.parallax.Applied() || c.parallax.rows.empty())Reject("level profile");
            const auto profile=ParallaxProfileOf(c.parallax,r.seam_column);
            if(!profile || !SameProfile(*profile,*m.parallax))Reject("level profile");
        }else if(m.parallax)Reject("level profile");
        break;}
    case GeometryLevel::g3:{
        if(Trusted(c.registration) || !c.coarse_shift || m.parallax)Reject("level G3");
        auto pose=r.base_pose;pose.tx_mm+=c.coarse_shift->translation_x_mm;pose.ty_mm+=c.coarse_shift->translation_y_mm;
        const SurfacePlane plane{r.limits.surface_envelope?c.coarse_shift->surface_height_mm:0.0,0,0};
        if(!SamePose(m.pose,pose) || !SamePlane(m.surface,plane))Reject("level G3 mapping");
        break;}
    case GeometryLevel::g4:
        if(!SamePose(m.pose,r.base_pose) || !m.surface.IsZero() || m.parallax)Reject("level G4 mapping");
        break;
    }
    if(r.layout.seam_column!=r.seam_column)Reject("layout");
    // Every derived field is its derivation, and the text is the one the writer writes for these values.
    auto derived=r;
    try{DeriveSavedFixedCompositeRegistration(derived,hash);}
    catch(const std::invalid_argument&){Reject("derivation");}
    catch(const std::logic_error&){Reject("assessment");}
    if(const auto want=RecordJsonUnchecked(derived),got=RecordJsonUnchecked(r);want!=got){
        std::size_t at=0;while(at<want.size() && at<got.size() && want[at]==got[at])++at;
        const auto from=at>40?at-40:0;
        throw std::invalid_argument("report v8: derived fields (expected ..."+want.substr(from,80)+"..., read ..."+got.substr(from,80)+"...)");
    }
    if(RegistrationWarningsJson(derived.warnings)!=RegistrationWarningsJson(r.warnings))Reject("warnings");
    if(RegistrationAssessmentJson(derived.assessment)!=RegistrationAssessmentJson(r.assessment))Reject("assessment");
    if(Canonical(ParseText(RecordJsonUnchecked(r)))!=Canonical(fcr) || Canonical(ParseText(RegistrationWarningsJson(r.warnings)))!=Canonical(warnings) ||
        Canonical(ParseText(RegistrationAssessmentJson(r.assessment)))!=Canonical(assessment))Reject("text");
    return r;
}
}

namespace a0::m2::experimental {
SavedFixedCompositeRegistration RecordOfCapture(const RegistrationRecordIdentity& id,const CaptureRegistrationRequest& request,
    const PreparedCaptureRegistration& prepared,const FixedCompositeSha256& hash){
    SavedFixedCompositeRegistration r;
    r.condition_sha256=id.condition_sha256;r.condition_sequence=id.condition_sequence;r.profile_file=id.profile_file;
    r.profile_file_sha256=id.profile_file_sha256;r.profile_fingerprint_sha256=id.profile_fingerprint_sha256;r.profile_id=id.profile_id;
    r.raster=request.parameters->raster;r.seam_column=request.estimator.geometry.seam_column;r.seam_origin=id.seam_origin;
    r.layout=request.estimator.geometry;r.base_pose=request.base_pose;r.limits=request.lines;r.registration=prepared.capture;
    const auto& g=prepared.capture.gate;
    if(g.state!=registration_gate::GateState::not_run){
        if(!g.seam_gate)throw std::invalid_argument("a gate that ran has no seam gate result");
        r.seam_gate_json=seam_gate::SeamGateReportJson(*g.seam_gate,g.measurement?&*g.measurement:nullptr);
    }
    DeriveSavedFixedCompositeRegistration(r,hash);
    return r;
}
}
