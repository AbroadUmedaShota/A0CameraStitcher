#pragma once
// Canonical texts of the fixed composite condition v3 and the review report v8 for the tests of #417
// (docs/design/fixed-composite-registration-formats.md). Every test that needs a v3 or v8 text builds it here instead of
// writing the text out, so that when a field is added there is one place to change besides the writer and the reader
// (format rule 1 and 7). The text has the members in the order the document gives, written the way the document says
// (2 section: no spaces, numbers with 17 significant digits in the classic locale, -0 written as 0).
//
// Sample values only. Nothing here is an owner value or a measured value of a rig; the numbers are round numbers chosen so
// that a test can see which member it is reading. The builders do not check that the pieces agree with each other: a report
// whose warnings do not follow from its quantities, or whose geometry hash is not the hash of its geometry, is built as asked.
// A test that needs a consistent report sets the pieces consistently (the writer lane's own test does so with the real
// functions). Header only; the standard library only.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace a0::tests::registration_fixtures {

namespace detail {
inline std::string Num(double v){
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<(v==0?0.0:v);return out.str();
}
inline std::string Int(long long v){return std::to_string(v);}
inline std::string Str(const std::string& v){
    std::string s="\"";
    for(const char c:v){
        if(c=='"'||c=='\\'){s+='\\';s+=c;}
        else if(static_cast<unsigned char>(c)<0x20){char b[8];std::snprintf(b,sizeof b,"\\u%04x",static_cast<unsigned>(c));s+=b;}
        else s+=c;
    }
    return s+"\"";
}
inline std::string Bool(bool v){return v?"true":"false";}
inline std::string OptNum(const std::optional<double>& v){return v?Num(*v):"null";}
inline std::string Strings(const std::vector<std::string>& v){
    std::string s="[";for(std::size_t i=0;i<v.size();++i)s+=(i?",":"")+Str(v[i]);return s+"]";
}
inline std::string Range(const std::optional<std::pair<double,double>>& r,const char* a,const char* b){
    return r?std::string("{\"")+a+"\":"+Num(r->first)+",\""+b+"\":"+Num(r->second)+"}":"null";
}
}

// 64 lower-case hex digits made of one character, for a hash that only has to be well formed.
inline std::string Hex64(char digit='a'){return std::string(64,digit);}

// ---- Pieces shared by v3 and v8 ----
struct Pose {double rx_deg{},ry_deg{},rz_deg{},tx_mm{},ty_mm{},tz_percent{};};
inline std::string PoseJson(const Pose& p){
    using namespace detail;
    return "{\"rxDeg\":"+Num(p.rx_deg)+",\"ryDeg\":"+Num(p.ry_deg)+",\"rzDeg\":"+Num(p.rz_deg)+",\"txMm\":"+Num(p.tx_mm)+
        ",\"tyMm\":"+Num(p.ty_mm)+",\"tzPercent\":"+Num(p.tz_percent)+"}";
}
struct Limits9 {
    double brown_lowe_alpha{8},brown_lowe_beta{0.3};
    std::uint32_t minimum_unique_blocks{3};
    double maximum_prediction_sigma_px{0.5},maximum_residual_rms_px{0.4},maximum_residual_p95_px{1},maximum_bin_bias_px{0.5},convergence_px{0.05},start_agreement_px{0.5};
};
inline std::string Limits9Json(const Limits9& l){
    using namespace detail;
    return "{\"brownLoweAlpha\":"+Num(l.brown_lowe_alpha)+",\"brownLoweBeta\":"+Num(l.brown_lowe_beta)+",\"minimumUniqueBlocks\":"+Int(l.minimum_unique_blocks)+
        ",\"maximumPredictionSigmaPx\":"+Num(l.maximum_prediction_sigma_px)+",\"maximumResidualRmsPx\":"+Num(l.maximum_residual_rms_px)+
        ",\"maximumResidualP95Px\":"+Num(l.maximum_residual_p95_px)+",\"maximumBinBiasPx\":"+Num(l.maximum_bin_bias_px)+
        ",\"convergencePx\":"+Num(l.convergence_px)+",\"startAgreementPx\":"+Num(l.start_agreement_px)+"}";
}
struct Envelope {double position_mm{10},rotation_deg{2},tilt_deg{1},height_percent{5};};
inline std::string EnvelopeJson(const Envelope& e){
    using namespace detail;
    return "{\"positionMm\":"+Num(e.position_mm)+",\"rotationDeg\":"+Num(e.rotation_deg)+",\"tiltDeg\":"+Num(e.tilt_deg)+
        ",\"heightPercent\":"+Num(e.height_percent)+"}";
}
struct SurfaceEnvelope {double height_mm{20},tilt_deg{4};};
struct Raster {
    long long left_um{0},top_um{0},right_um{254000},bottom_um{127000};
    std::uint32_t dpi{100},width_pixels{1000},height_pixels{500};
};
inline std::string RasterJson(const Raster& r){
    using namespace detail;
    return "{\"regionUm\":{\"left\":"+Int(r.left_um)+",\"top\":"+Int(r.top_um)+",\"right\":"+Int(r.right_um)+",\"bottom\":"+Int(r.bottom_um)+
        "},\"dpi\":"+Int(r.dpi)+",\"widthPixels\":"+Int(r.width_pixels)+",\"heightPixels\":"+Int(r.height_pixels)+"}";
}
struct Seam {std::uint32_t column{500};std::string origin{"band-center-provisional"};};
inline std::string SeamJson(const Seam& s){return "{\"column\":"+detail::Int(s.column)+",\"origin\":"+detail::Str(s.origin)+"}";}
struct SeamGateLine {double limit_px{1},guard_band_px{0.2};};
struct Profile {
    std::string file{"wpd-fixed-composite-profile-0123456789abcdef.json"};
    std::string file_sha256{Hex64('a')},fingerprint_sha256{Hex64('b')};
};

// ---- Condition v3 ----
struct ChangeV3 {
    std::string kind{"created"};                 // created, migrated-from-v2, saved-estimate, manual-adjustment, rollback, values
    std::string v2_sha256{Hex64('1')},report_sha256{Hex64('2')},pair_id{"pair-0001"},restored_sha256{Hex64('3')};
    double entered_seam_dx_px{1},entered_seam_dy_px{-1},entered_rotation_deg{0.25},maximum_seam_displacement_px{1.5};
};
struct ConditionV3Options {
    std::string schema{"a0.fixed-composite-condition.v3"};
    std::string approval{"unapproved"};
    std::string created_utc{"2026-01-01T00:00:00Z"};
    std::uint32_t sequence{1};
    std::optional<std::string> parent_sha256;                // none: null (sequence 1)
    ChangeV3 change;
    Profile profile;
    std::string policy_sha256{Hex64('c')};
    std::string mount_orientation{"normal"};
    Raster raster;
    Seam seam;
    Pose base_pose;
    Envelope envelope;
    Limits9 limits;
    std::uint32_t omega_margin_px{8},guard_px{72},band_half_width_px{256};
    std::optional<SurfaceEnvelope> surface_envelope{SurfaceEnvelope{}};   // none: null
    bool apply_beyond_envelope{true};
    std::optional<double> surface_scale_warn_percent;                     // none: null
    std::optional<double> parallax_max_lift_mm{1};                        // none: null
    std::optional<Limits9> live_limits;                                   // none: live is null
    SeamGateLine seam_gate;
    std::optional<double> manual_adjustment_max_seam_px;                  // none: null
};
inline std::string ChangeV3Json(const ChangeV3& c){
    using namespace detail;
    std::string s="{\"kind\":"+Str(c.kind);
    if(c.kind=="migrated-from-v2")s+=",\"v2Sha256\":"+Str(c.v2_sha256);
    else if(c.kind=="saved-estimate")s+=",\"reportSha256\":"+Str(c.report_sha256)+",\"pairId\":"+Str(c.pair_id);
    else if(c.kind=="manual-adjustment")
        s+=",\"entered\":{\"seamDxPx\":"+Num(c.entered_seam_dx_px)+",\"seamDyPx\":"+Num(c.entered_seam_dy_px)+",\"rotationDeg\":"+Num(c.entered_rotation_deg)+
            "},\"maximumSeamDisplacementPx\":"+Num(c.maximum_seam_displacement_px);
    else if(c.kind=="rollback")s+=",\"restoredSha256\":"+Str(c.restored_sha256);
    return s+"}";
}
// The canonical text of a condition v3, without the trailing newline the writer adds.
inline std::string ConditionV3Text(const ConditionV3Options& o={}){
    using namespace detail;
    std::string s="{\"schema\":"+Str(o.schema)+",\"approval\":"+Str(o.approval)+",\"createdUtc\":"+Str(o.created_utc)+
        ",\"revision\":{\"sequence\":"+Int(o.sequence)+",\"parentSha256\":"+(o.parent_sha256?Str(*o.parent_sha256):std::string("null"))+
        ",\"change\":"+ChangeV3Json(o.change)+"},\"source\":{\"profileFile\":"+Str(o.profile.file)+",\"profileFileSha256\":"+Str(o.profile.file_sha256)+
        ",\"profileFingerprintSha256\":"+Str(o.profile.fingerprint_sha256)+",\"policySha256\":"+Str(o.policy_sha256)+
        "},\"mountOrientation\":"+Str(o.mount_orientation)+",\"outputRaster\":"+RasterJson(o.raster)+",\"seam\":"+SeamJson(o.seam)+
        ",\"basePose\":"+PoseJson(o.base_pose)+",\"registration\":{\"envelope\":"+EnvelopeJson(o.envelope)+",\"limits\":"+Limits9Json(o.limits)+
        ",\"geometry\":{\"omegaMarginPx\":"+Int(o.omega_margin_px)+",\"guardPx\":"+Int(o.guard_px)+",\"bandHalfWidthPx\":"+Int(o.band_half_width_px)+
        "},\"surfaceEnvelope\":"+(o.surface_envelope?"{\"heightMm\":"+Num(o.surface_envelope->height_mm)+",\"tiltDeg\":"+Num(o.surface_envelope->tilt_deg)+"}":std::string("null"))+
        ",\"applyBeyondEnvelope\":"+Bool(o.apply_beyond_envelope)+",\"surfaceScaleWarnPercent\":"+OptNum(o.surface_scale_warn_percent)+
        ",\"parallax\":"+(o.parallax_max_lift_mm?"{\"maxLiftMm\":"+Num(*o.parallax_max_lift_mm)+"}":std::string("null"))+
        ",\"live\":"+(o.live_limits?"{\"limits\":"+Limits9Json(*o.live_limits)+"}":std::string("null"))+
        "},\"seamGate\":{\"limitPx\":"+Num(o.seam_gate.limit_px)+",\"guardBandPx\":"+Num(o.seam_gate.guard_band_px)+
        "},\"revisionRules\":{\"manualAdjustmentMaxSeamPx\":"+OptNum(o.manual_adjustment_max_seam_px)+"}}";
    return s;
}
// The file as the writer writes it (the text and one newline).
inline std::string ConditionV3File(const ConditionV3Options& o={}){return ConditionV3Text(o)+"\n";}

// ---- Report v8 ----
struct WarningSpec {
    std::string code{"geometry-level"},klass{"notice"},component{"geometry-level"};
    double value{};
    std::optional<double> limit;
    std::string unit{"px"};
    std::optional<std::pair<double,double>> rows_px,rows_mm;
    std::vector<std::string> interpretations;
    std::string remedy{"none"};
    std::optional<double> remedy_amount_mm;
    bool applied{};
    std::string source{"condition"};
};
inline std::string WarningJson(const WarningSpec& w){
    using namespace detail;
    return "{\"code\":"+Str(w.code)+",\"class\":"+Str(w.klass)+",\"component\":"+Str(w.component)+",\"value\":"+Num(w.value)+
        ",\"limit\":"+OptNum(w.limit)+",\"unit\":"+Str(w.unit)+",\"rowsPx\":"+Range(w.rows_px,"first","last")+",\"rowsMm\":"+Range(w.rows_mm,"first","last")+
        ",\"interpretations\":"+Strings(w.interpretations)+",\"remedy\":"+Str(w.remedy)+",\"remedyAmountMm\":"+OptNum(w.remedy_amount_mm)+
        ",\"applied\":"+Bool(w.applied)+",\"source\":"+Str(w.source)+"}";
}

struct ReportV8Options {
    // Members that v7 has (written as v7 writes them).
    std::string schema{"a0.uncalibrated-still-review.v8"};
    std::string pair_id{"pair-0001"};
    std::string input_a_path{"originals/camera-a.jpg"},input_b_path{"originals/camera-b.jpg"};
    std::string input_a_sha256{Hex64('4')},input_b_sha256{Hex64('5')};
    std::uint64_t input_a_bytes{1000000},input_b_bytes{1000000};
    std::uint32_t preview_width{320},preview_height{240},preview_overlap{64};
    int preview_vertical{0};
    std::uint64_t preview_revision{1};
    bool rotated_180{false};                          // adds "mountOrientation":"rotated-180"
    std::uint32_t source_width{7360},source_height{4912};
    std::string output_path{"uncalibrated-review.jpg"};
    std::uint32_t output_width{1000},output_height{500};
    std::uint64_t output_bytes{200000};
    std::string output_sha256{Hex64('6')};
    // fixedCompositeRegistration.
    std::string condition_sha256{Hex64('7')};
    std::uint32_t condition_sequence{1};
    Profile profile;
    std::optional<std::string> profile_id;            // none: null
    Raster raster;
    Seam seam;
    std::uint32_t mixing_width_px{8},omega_margin_px{8},guard_px{16},band_half_width_px{256};   // fixedCompositeRegistration.layout
    Pose base_pose;
    Envelope envelope;
    std::optional<SurfaceEnvelope> surface_envelope{SurfaceEnvelope{}};
    bool apply_beyond_envelope{true};
    std::optional<double> surface_scale_warn_percent;
    std::optional<double> max_lift_mm{1};
    SeamGateLine seam_gate;
    // registration (the pose estimate): verified with a small pose, no component over its range.
    std::string registration_state{"verified"};       // verified, undetermined, out-of-envelope
    std::vector<std::string> registration_reasons;
    bool has_estimate{true};
    Pose pose{0.1,-0.1,0.05,0.5,-0.5,0.2};
    Pose pose_sigma{0.01,0.01,0.01,0.1,0.1,0.05};
    bool pose_applied{true};
    // surface.
    std::string surface_state{"estimated"};           // estimated, not-estimated, disabled
    double surface_height_mm{2},surface_slope_x{0.01},surface_slope_y{-0.005},surface_tilt_deg{0.6},surface_baseline_equivalent_mm{3};
    bool surface_applied{true};
    // parallax: the value until layer 2 exists.
    std::string parallax_state{"not-applied"};        // applied, not-applied, disabled
    std::vector<std::string> parallax_reasons{"not-implemented"};
    // pointSource (formats 4.8.1): null (the band's points) unless set.
    struct PointSourceSpec {
        bool used{false};
        unsigned points_used{0};
        std::optional<double> edge_share;
        double edge_share_line{0.25};
        std::optional<std::string> fallback_reason;
    };
    std::optional<PointSourceSpec> point_source;
    // geometryLevel.
    std::string level{"G2"};
    std::vector<std::string> level_reasons;
    bool level_applied_pose{true},level_applied_surface{true},level_applied_parallax{false},beyond_envelope_applied{false};
    // geometryLevel.mapping: G1 to G3 the estimated pose (or mapping_pose), G4 the base pose; the estimated plane unless
    // mapping_surface is false or the level is G4. Layer 2's profile is null (layer 2 is not in the build yet).
    std::optional<Pose> mapping_pose;
    bool mapping_surface{true};
    std::optional<std::pair<double,double>> coarse_shift_translation;   // geometryLevel.coarseShift (surfaceHeightMm 0); none: null
    // gate.
    std::string gate_state{"pass"};                   // pass, fail, undetermined, not-run
    std::string gate_cause{"none"};                   // none, lift, pose, displacement, unmeasurable
    double gate_dx_measured_share{1};
    std::optional<double> gate_two_directional_px{0.4},gate_one_directional_px{0.3};
    std::vector<std::pair<std::string,std::string>> gate_findings;       // findings (the seam gate's): reason, remedy
    std::vector<std::pair<std::string,std::string>> gate_own_findings;   // gateFindings (the registration gate's): reason, remedy
    // Every stage is planned or skipped once, in the order they are dropped (formats document 5.4).
    std::vector<std::string> time_planned_stages{"wide-search-diagnostic","band-measurement","parallax","fold-refit","fine-and-fit","coarse","gate"};
    std::vector<std::pair<std::string,std::string>> time_skipped;    // stage, reason
    std::string geometry_hash{Hex64('9')};
    // Top level after the fixedCompositeRegistration.
    std::vector<WarningSpec> warnings;
    std::string assessment_state{"within-criteria"};  // within-criteria, outside-criteria, not-judged
    std::vector<std::string> assessment_exceeded,assessment_unverified;
};

inline std::string ReportV8Text(const ReportV8Options& o={}){
    using namespace detail;
    const bool estimated=o.surface_state=="estimated";
    const bool gate_ran=o.gate_state!="not-run";
    std::string point_source_json="null";
    if(o.point_source){const auto& p=*o.point_source;
        point_source_json="{\"fastMeasurerUsed\":"+Bool(p.used)+",\"pointsUsed\":"+Int(p.points_used)+",\"edgeShare\":"+(p.edge_share?Num(*p.edge_share):std::string("null"))+
            ",\"edgeShareLine\":"+Num(p.edge_share_line)+",\"fallbackReason\":"+(p.fallback_reason?Str(*p.fallback_reason):std::string("null"))+"}";}
    const auto optional_pose=[&](bool present){return present?PoseJson(o.pose):std::string("null");};
    // registration
    std::string reg="{\"state\":"+Str(o.registration_state)+",\"reasons\":"+Strings(o.registration_reasons)+",\"hasEstimate\":"+Bool(o.has_estimate)+
        ",\"pose\":"+optional_pose(o.has_estimate)+",\"poseSigma\":"+(o.has_estimate?PoseJson(o.pose_sigma):std::string("null"))+",\"covariance\":";
    if(o.has_estimate){reg+="[";for(int i=0;i<36;++i)reg+=std::string(i?",":"")+((i%7==0)?Num(0.0001):std::string("0"));reg+="]";}else reg+="null";
    reg+=",\"components\":[";
    if(o.has_estimate){
        const std::pair<const char*,std::pair<double,double>> rows[]={{"position",{0.7,o.envelope.position_mm}},{"rotation",{0.05,o.envelope.rotation_deg}},
            {"tilt",{0.14,o.envelope.tilt_deg}},{"height",{0.2,o.envelope.height_percent}}};
        bool first=true;
        for(const auto& r:rows){
            reg+=std::string(first?"":",")+"{\"component\":"+Str(r.first)+",\"valueFromBase\":"+Num(r.second.first)+",\"limit\":"+Num(r.second.second)+
                ",\"exceeded\":"+Bool(r.second.first>r.second.second)+"}";
            first=false;
        }
    }
    reg+="],\"display\":{\"positionMm\":0.75,\"txMm\":0.5,\"tyMm\":-0.5,\"rotationDeg\":0.0625,\"tiltDeg\":0.125,\"rxDeg\":0.125,\"ryDeg\":-0.125,\"heightPercent\":0.25,\"heightMm\":0.25,\"outputFrame\":null}"
        ",\"applied\":"+Bool(o.pose_applied)+",\"design\":{},\"diagnostics\":{\"counts\":{\"outerIterations\":2,\"points\":120,\"inliers\":110,\"gatePoints\":0,\"uniqueBlocks\":8,\"consistentBlocks\":8}}}";
    // surface
    std::string surf="{\"state\":"+Str(o.surface_state)+",\"heightMm\":"+(estimated?Num(o.surface_height_mm):std::string("null"))+",\"slopeX\":"+(estimated?Num(o.surface_slope_x):std::string("null"))+
        ",\"slopeY\":"+(estimated?Num(o.surface_slope_y):std::string("null"))+",\"tiltDeg\":"+(estimated?Num(o.surface_tilt_deg):std::string("null"))+",\"covariance\":";
    if(estimated)surf+="["+Num(0.01)+",0,0,0,"+Num(0.000001)+",0,0,0,"+Num(0.000001)+"]";else surf+="null";
    surf+=",\"baselineEquivalentMm\":"+(estimated?Num(o.surface_baseline_equivalent_mm):std::string("null"))+",\"scale\":";
    if(estimated)surf+="{\"factor\":1.0009765625,\"minimum\":1.00048828125,\"maximum\":1.001953125}";else surf+="null";
    surf+=",\"exceeded\":{\"height\":"+Bool(estimated&&o.surface_envelope&&o.surface_height_mm>o.surface_envelope->height_mm)+
        ",\"tilt\":"+Bool(estimated&&o.surface_envelope&&o.surface_tilt_deg>o.surface_envelope->tilt_deg)+"},\"applied\":"+Bool(o.surface_applied&&estimated)+"}";
    // parallax: the fixed value until layer 2 exists; an applied one carries a one-row profile.
    const bool par=o.parallax_state=="applied";
    std::string pl="{\"state\":"+Str(o.parallax_state)+",\"reasons\":"+Strings(o.parallax_reasons)+",\"baseline\":"+(par?std::string("{\"x\":1,\"y\":0}"):std::string("null"))+
        ",\"kPxPerMm\":"+(par?std::string("2.5"):std::string("null"))+",\"design\":"+
        (par?std::string("{\"columnSpacingPx\":16,\"columnRadiusPx\":192,\"pointPatchPx\":32,\"weightFlatPx\":48,\"weightFalloffPx\":192,\"maximumStrain\":0.10000000000000001,\"foldNearSeamPx\":64}"):std::string("{}"))+
        ",\"grid\":"+(par?std::string("{\"seamColumn\":500,\"firstRow\":250,\"rowStep\":32}"):std::string("null"))+
        ",\"split\":"+(par?std::string("{\"mode\":\"split\",\"verifyPeriodRows\":6,\"verifyPhase\":0,\"rowsVerify\":1,\"rowsCorrect\":0,\"rowsWithHole\":0,\"pointsExcluded\":20}"):std::string("null"))+
        ",\"profile\":"+(par?std::string("[{\"y\":250,\"qPx\":0.5,\"sigmaPx\":0.125,\"nEff\":40,\"applied\":true,\"points\":30,\"liftMm\":-0.20000000000000001,\"state\":\"measured\",\"reason\":null,"
            "\"role\":\"verify\",\"hole\":false}]"):std::string("[]"))+
        ",\"summary\":"+(par?std::string("{\"maxShiftPx\":0.5,\"maxShiftMm\":0.25,\"atRow\":250,\"maxStrain\":0.0009765625,\"columns\":{\"first\":400,\"last\":700},\"points\":300,"
            "\"pointsInput\":320,\"pointsBaseline\":300,\"rowsMeasured\":1,\"rowsFilled\":0,\"holeXPx\":160,\"holeYPx\":112,\"pointsCrossValidated\":280,\"foldCandidates\":0,\"foldResidualScalePx\":0.125}"):std::string("null"))+
        ",\"lift\":"+(par?std::string("{\"limitMm\":1,\"maximumMm\":0.20000000000000001,\"atRow\":250,\"exceeded\":false,\"exceededRows\":0,\"firstExceededRow\":0,\"lastExceededRow\":0}"):std::string("null"))+
        ",\"applied\":"+Bool(par)+"}";
    // geometryLevel
    const Pose mapping_pose=o.mapping_pose?*o.mapping_pose:(o.level=="G4"?o.base_pose:o.pose);
    const bool plane=o.mapping_surface&&estimated&&o.level!="G4";
    const std::string level="{\"level\":"+Str(o.level)+",\"reasons\":"+Strings(o.level_reasons)+",\"applied\":{\"pose\":"+Bool(o.level_applied_pose)+",\"surface\":"+
        Bool(o.level_applied_surface)+",\"parallax\":"+Bool(o.level_applied_parallax)+"},\"beyondEnvelopeApplied\":"+Bool(o.beyond_envelope_applied)+
        ",\"mapping\":{\"pose\":"+PoseJson(mapping_pose)+",\"surface\":{\"h0Mm\":"+Num(plane?o.surface_height_mm:0)+",\"slopeX\":"+Num(plane?o.surface_slope_x:0)+
        ",\"slopeY\":"+Num(plane?o.surface_slope_y:0)+"},\"parallax\":null},\"coarseShift\":"+
        (o.coarse_shift_translation?"{\"surfaceHeightMm\":0,\"translationXMm\":"+Num(o.coarse_shift_translation->first)+",\"translationYMm\":"+
            Num(o.coarse_shift_translation->second)+"}":std::string("null"))+"}";
    // gate
    std::string findings="[";
    for(std::size_t i=0;i<o.gate_findings.size();++i)findings+=std::string(i?",":"")+"{\"reason\":"+Str(o.gate_findings[i].first)+",\"remedy\":"+Str(o.gate_findings[i].second)+"}";
    findings+="]";
    const std::string maxima_pair="{\"twoDirectionalPx\":"+(gate_ran?OptNum(o.gate_two_directional_px):std::string("null"))+",\"oneDirectionalPx\":"+
        (gate_ran?OptNum(o.gate_one_directional_px):std::string("null"));
    const std::string quantities="{\"maximumPx\":"+(gate_ran?OptNum(o.gate_two_directional_px):std::string("null"))+",\"twoDirectionalPx\":"+
        (gate_ran?OptNum(o.gate_two_directional_px):std::string("null"))+",\"oneDirectionalPx\":"+(gate_ran?OptNum(o.gate_one_directional_px):std::string("null"))+"}";
    std::string own="[";
    for(std::size_t i=0;i<o.gate_own_findings.size();++i)own+=std::string(i?",":"")+"{\"reason\":"+Str(o.gate_own_findings[i].first)+",\"remedy\":"+Str(o.gate_own_findings[i].second)+"}";
    own+="]";
    const std::string remedy=o.gate_cause=="lift"||o.gate_cause=="unmeasurable"?"shift-drawing-horizontal":o.gate_cause=="pose"?"retake-then-recalibrate":
        o.gate_cause=="displacement"?"retake":"none";
    const long long rows_measured=gate_ran?std::llround(20*o.gate_dx_measured_share):0;
    std::string gate="{\"state\":"+Str(o.gate_state)+",\"findings\":"+findings+",\"gateFindings\":"+own+",\"cause\":"+Str(o.gate_cause)+",\"remedy\":"+Str(remedy)+
        ",\"lift\":null,\"rows\":{\"dxMeasuredShare\":"+(gate_ran?Num(static_cast<double>(rows_measured)/20):std::string("null"))+",\"rowsWithContent\":"+Int(gate_ran?20:0)+
        ",\"rowsDxMeasured\":"+Int(rows_measured)+",\"unmeasured\":[],\"outsideMeasurable\":[],\"rowsBlank\":0,\"rowsMeasured\":"+Int(gate_ran?20:0)+
        ",\"rowsContentUnmeasured\":0,\"rowsOneSided\":0,\"rowsUnconfirmed\":0,\"contentUnmeasuredShare\":"+(gate_ran?std::string("0"):std::string("null"))+
        ",\"blank\":[],\"contentUnmeasured\":[],\"rowsCorrected\":0,\"corrected\":[]},\"maxima\":"+maxima_pair+
        ",\"oneDirectionalNormal\":"+(gate_ran&&o.gate_one_directional_px?std::string("{\"x\":0,\"y\":1}"):std::string("null"))+",\"oneDirectionalComponent\":"+
        (gate_ran&&o.gate_one_directional_px?std::string("\"across-baseline\""):std::string("null"))+"},\"before\":"+quantities+",\"after\":"+quantities+
        ",\"corrected\":null,\"anchors\":{\"range\":"+(gate_ran?std::string("{\"firstColumn\":300,\"lastColumn\":700}"):std::string("null"))+"},\"placement\":{\"verifiedBy\":"+Str(gate_ran?"anchors":"none")+"},\"wideSearch\":null,\"design\":"+
        (gate_ran?std::string("{\"minimumBaselineComponent\":0.5,\"liftRatio\":3,\"liftShare\":0.90000000000000002,\"wideSearchRadiusPx\":40,\"wideSearchHalfWidthPx\":64,"
            "\"wideSearchBlockRows\":96,\"wideSearchMinimumCorrelation\":0.5,\"wideSearchSecondaryRatio\":0.80000000000000004,\"wideSearchConsistencyPx\":1.5,"
            "\"wideSearchMinimumBlocks\":2,\"wideSearchTriggerShare\":0.5,\"maximumUnmeasuredContentShare\":0.25}"):std::string("{}"))+",\"geometryHash\":"+Str(o.geometry_hash)+
        ",\"seamGate\":"+(gate_ran?"{\"findings\":[],\"schema\":\"a0.seam-gate.v1\",\"state\":"+Str(o.gate_state=="undetermined"?"pass":o.gate_state)+"}":std::string("null"))+"}";
    // timeBudget
    std::string skipped="[";
    for(std::size_t i=0;i<o.time_skipped.size();++i)skipped+=std::string(i?",":"")+"{\"stage\":"+Str(o.time_skipped[i].first)+",\"reason\":"+Str(o.time_skipped[i].second)+"}";
    skipped+="]";
    const std::string budget="{\"plannedStages\":"+Strings(o.time_planned_stages)+",\"skipped\":"+skipped+
        ",\"costModel\":{\"stageMs\":{\"wide-search-diagnostic\":100,\"band-measurement\":200,\"parallax\":300,\"fold-refit\":400,\"fine-and-fit\":500,\"coarse\":600,\"gate\":700},\"tailReserveMs\":800}}";
    // limits
    const std::string limits="{\"envelope\":"+EnvelopeJson(o.envelope)+",\"surfaceEnvelope\":"+
        (o.surface_envelope?"{\"heightMm\":"+Num(o.surface_envelope->height_mm)+",\"tiltDeg\":"+Num(o.surface_envelope->tilt_deg)+"}":std::string("null"))+
        ",\"applyBeyondEnvelope\":"+Bool(o.apply_beyond_envelope)+",\"surfaceScaleWarnPercent\":"+OptNum(o.surface_scale_warn_percent)+",\"maxLiftMm\":"+OptNum(o.max_lift_mm)+
        ",\"seamGate\":{\"limitPx\":"+Num(o.seam_gate.limit_px)+",\"guardBandPx\":"+Num(o.seam_gate.guard_band_px)+"}}";
    std::string warnings="[";
    for(std::size_t i=0;i<o.warnings.size();++i)warnings+=std::string(i?",":"")+WarningJson(o.warnings[i]);
    warnings+="]";
    std::string s="{\"schema\":"+Str(o.schema)+",\"pairId\":"+Str(o.pair_id)+
        ",\"state\":\"UncalibratedReviewReady\",\"productQualityAccepted\":false,\"physicalScaleVerified\":false,\"liveToStillFieldOfViewVerified\":false,\"automaticRetryCount\":0,\"inputs\":["
        "{\"alias\":\"CAM-A\",\"path\":"+Str(o.input_a_path)+",\"sha256\":"+Str(o.input_a_sha256)+",\"bytes\":"+Int(static_cast<long long>(o.input_a_bytes))+"},"
        "{\"alias\":\"CAM-B\",\"path\":"+Str(o.input_b_path)+",\"sha256\":"+Str(o.input_b_sha256)+",\"bytes\":"+Int(static_cast<long long>(o.input_b_bytes))+"}],"
        "\"previewGeometry\":{\"width\":"+Int(o.preview_width)+",\"height\":"+Int(o.preview_height)+",\"overlap\":"+Int(o.preview_overlap)+",\"vertical\":"+Int(o.preview_vertical)+
        ",\"revision\":"+Int(static_cast<long long>(o.preview_revision))+"},";
    if(o.rotated_180)s+="\"mountOrientation\":\"rotated-180\",";
    s+="\"sourceDimensions\":{\"width\":"+Int(o.source_width)+",\"height\":"+Int(o.source_height)+"},\"output\":{\"path\":"+Str(o.output_path)+",\"width\":"+Int(o.output_width)+
        ",\"height\":"+Int(o.output_height)+",\"bytes\":"+Int(static_cast<long long>(o.output_bytes))+",\"sha256\":"+Str(o.output_sha256)+"},"
        "\"geometryBasis\":\"fixed-composite-registration\",\"fixedCompositeRegistration\":{\"condition\":{\"sha256\":"+Str(o.condition_sha256)+",\"sequence\":"+Int(o.condition_sequence)+
        "},\"profile\":{\"file\":"+Str(o.profile.file)+",\"fileSha256\":"+Str(o.profile.file_sha256)+",\"fingerprintSha256\":"+Str(o.profile.fingerprint_sha256)+
        ",\"profileId\":"+(o.profile_id?Str(*o.profile_id):std::string("null"))+"},\"raster\":"+RasterJson(o.raster)+",\"seam\":"+SeamJson(o.seam)+
        ",\"layout\":{\"mixingWidthPx\":"+Int(o.mixing_width_px)+",\"omegaMarginPx\":"+Int(o.omega_margin_px)+",\"guardPx\":"+Int(o.guard_px)+
        ",\"bandHalfWidthPx\":"+Int(o.band_half_width_px)+"},\"basePose\":"+PoseJson(o.base_pose)+",\"limits\":"+limits+",\"registration\":"+reg+",\"surface\":"+surf+",\"parallax\":"+pl+",\"folds\":[],\"pointSource\":"+point_source_json+",\"geometryLevel\":"+level+
        ",\"gate\":"+gate+",\"timeBudget\":"+budget+",\"geometryHash\":"+Str(o.geometry_hash)+"},"
        "\"blend\":{\"method\":\"fixed-seam\",\"widthPixels\":8,\"edgeMarginPixels\":64,\"differenceMean\":0.5,\"differenceMaximum\":3,"
        "\"brightness\":{\"windowHalfWidthPixels\":32,\"smoothingRows\":128,\"ratioMinimum\":0.984375,\"ratioMaximum\":1.015625,\"saturationLimitedPixels\":0}},"
        "\"warnings\":"+warnings+",\"assessment\":{\"state\":"+Str(o.assessment_state)+",\"exceeded\":"+Strings(o.assessment_exceeded)+",\"unverified\":"+Strings(o.assessment_unverified)+
        ",\"qualityAccepted\":false,\"acceptanceEvidence\":false}}";
    return s;
}
}
