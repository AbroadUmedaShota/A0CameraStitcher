#include "a0/m2/stitch_correction_job.hpp"
#include "a0/common/protocol_json.hpp"
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace a0::m2::correction {
namespace {
using namespace a0::common::protocol_json;
[[noreturn]] void Invalid() { throw std::invalid_argument("invalid stitch correction job or bindings"); }
struct Failure { [[noreturn]] static void Fail(std::string_view,std::string_view) { Invalid(); } };
void Keys(const JsonValue& v, std::initializer_list<const char*> keys) {
    if (v.kind != JsonKind::object || v.object.size() != keys.size()) Invalid();
    for (const auto k : keys) if (!v.object.contains(k)) Invalid();
}
const JsonValue& At(const JsonValue& v, const char* k) { if (v.kind != JsonKind::object || !v.object.contains(k)) Invalid(); return v.object.at(k); }
double Num(const JsonValue& v) {
    if (v.kind != JsonKind::number) Invalid(); double n{};
    const auto p = std::from_chars(v.string.data(),v.string.data()+v.string.size(),n);
    if (p.ec != std::errc{} || p.ptr != v.string.data()+v.string.size() || !std::isfinite(n)) Invalid(); return n;
}
std::uint32_t Int(const JsonValue& v) {
    if (v.kind != JsonKind::number || v.string.find_first_not_of("0123456789") != std::string::npos) Invalid();
    std::uint32_t n{}; const auto p = std::from_chars(v.string.data(),v.string.data()+v.string.size(),n);
    if (p.ec != std::errc{} || p.ptr != v.string.data()+v.string.size()) Invalid(); return n;
}
std::string Q(std::string_view s) { return "\""+std::string(s)+"\""; } // fixed codes/validated SHA only
std::string N(double n) {
    if (!std::isfinite(n)) return "null";
    char text[64]; const auto p = std::to_chars(text,text+sizeof(text),n);
    if (p.ec != std::errc{}) Invalid(); return std::string(text,p.ptr);
}
std::string Point(render::Point p) { return '['+N(p.x)+','+N(p.y)+']'; }
std::string Number(const std::optional<double>& v) { return v ? N(*v) : "null"; }
std::string Point(const std::optional<render::Point>& v) { return v ? Point(*v) : "null"; }
void Hash(std::string_view s) { if (s.size() != 64 || s.find_first_not_of("0123456789abcdef") != std::string_view::npos) Invalid(); }
std::string Search(const PatchSearchResult& p, const std::optional<PatchRefinementPolicy>& refinement) {
    if(p.refinement.size()>(refinement ? refinement->levels : 0))Invalid();
    std::string s="{\"outcome\":"+Q(ImagePatchOutcomeCode(p.outcome))+",\"bestDocumentMm\":"+Point(p.best_document_mm)+
        ",\"bestScore\":"+Number(p.best_score)+",\"secondScore\":"+Number(p.second_score)+
        ",\"sourceIntensityStddev\":"+Number(p.source_intensity_stddev)+",\"maximumTargetIntensityStddev\":"+Number(p.maximum_target_intensity_stddev)+
        ",\"examinedCandidates\":"+std::to_string(p.examined_candidates)+",\"eligibleCandidates\":"+std::to_string(p.eligible_candidates);
    if(refinement) {
        if(p.outcome==ImagePatchOutcome::Matched && p.refinement.size()!=refinement->levels)Invalid();
        s+=",\"refinement\":[";
        for(std::size_t n=0;n<p.refinement.size();++n) {
            if(n)s+=','; const auto& level=p.refinement[n];
            if(level.examined_candidates>9 || level.eligible_candidates>level.examined_candidates)Invalid();
            s+="{\"stepMm\":"+N(level.step_mm)+",\"outcome\":"+Q(ImagePatchOutcomeCode(level.outcome))+
                ",\"bestDocumentMm\":"+Point(level.best_document_mm)+",\"bestScore\":"+Number(level.best_score)+
                ",\"secondScore\":"+Number(level.second_score)+",\"maximumTargetIntensityStddev\":"+Number(level.maximum_target_intensity_stddev)+
                ",\"examinedCandidates\":"+std::to_string(level.examined_candidates)+",\"eligibleCandidates\":"+std::to_string(level.eligible_candidates)+'}';
        }
        s+=']';
    }
    return s+'}';
}
std::string Raw(const RawDocumentResult& p) {
    return "{\"outcome\":"+Q(RawDocumentOutcomeCode(p.outcome))+",\"documentMm\":"+Point(p.document_mm)+
        ",\"roundTripErrorPixels\":"+Number(p.round_trip_error_pixels)+",\"newtonUpdates\":"+std::to_string(p.newton_updates)+'}';
}
std::string Fit(const DocumentCorrectionResult& r) {
    std::string s="{\"outcome\":"+Q(DocumentCorrectionOutcomeCode(r.outcome))+",\"estimate\":";
    if (r.estimate) { const auto t=*r.estimate; s+='['+N(t.a)+','+N(t.b)+','+N(t.translation_x_mm)+','+N(t.translation_y_mm)+']'; } else s+="null";
    s+=",\"metrics\":";
    if (r.metrics) { const auto m=*r.metrics;
        s+="{\"pointCount\":"+std::to_string(m.point_count)+",\"rmsFitResidualOutputPixels\":"+N(m.rms_fit_residual_output_pixels)+
            ",\"maximumFitResidualOutputPixels\":"+N(m.maximum_fit_residual_output_pixels)+",\"maximumDisplacementOutputPixels\":"+N(m.maximum_displacement_output_pixels)+
            ",\"rotationDegrees\":"+N(m.rotation_degrees)+",\"scaleDifferencePercent\":"+N(m.scale_difference_percent)+'}';
    } else s+="null";
    s+=",\"violations\":["; for (std::size_t i=0;i<r.violations.size();++i) { if(i)s+=','; s+=Q(DocumentCorrectionOutcomeCode(r.violations[i])); }
    return s+"],\"candidateAvailable\":"+(r.corrected_camera ? "true}" : "false}");
}
} // namespace

StitchCorrectionJob ParseStitchCorrectionJob(std::string_view text) {
    // Bound nesting before the shared parser allocates. Braces inside JSON
    // strings do not count; malformed strings are still rejected by the parser.
    if (text.empty() || text.size()>kMaximumCorrectionJobBytes) Invalid();
    int depth=0; bool quoted=false,escaped=false;
    for (const char c : text) {
        if (quoted) { if(escaped)escaped=false; else if(c=='\\')escaped=true; else if(c=='"')quoted=false; }
        else if(c=='"')quoted=true; else if(c=='{' || c=='[') { if(++depth>8)Invalid(); } else if(c=='}' || c==']') { if(--depth<0)Invalid(); }
    }
    const auto root=BasicJsonParser<Failure,kMaximumCorrectionJobBytes>(text).Parse();
    const auto& schema=At(root,"schemaVersion");
    if(schema.kind!=JsonKind::string || (schema.string!="a0.stitch-correction-job.v1" && schema.string!="a0.stitch-correction-job.v2"))Invalid();
    std::optional<PatchRefinementPolicy> refinement;
    if(schema.string=="a0.stitch-correction-job.v2") {
        Keys(root,{"schemaVersion","seedSelection","matching","inverse","correction","refinement"});
        const auto& r=At(root,"refinement"); Keys(r,{"levels"});
        const auto levels=Int(At(r,"levels")); if(levels<1 || levels>8)Invalid();
        refinement.emplace(levels);
    } else Keys(root,{"schemaVersion","seedSelection","matching","inverse","correction"});
    const auto& s=At(root,"seedSelection"); Keys(s,{"gridColumns","gridRows","maximumSeeds","minimumSpacingMm","minimumMinorGradientEigenvalue","minimumGradientAxisRatio"});
    const auto& m=At(root,"matching"); Keys(m,{"patchRadiusSamples","patchSpacingMm","searchRadiusSteps","searchStepMm","minimumIntensityStddev","minimumCorrelation","minimumBestSecondGap","maximumReverseErrorMm"});
    const auto& i=At(root,"inverse"); Keys(i,{"maximumNewtonUpdates","maximumBacktrackingSteps","maximumRoundTripErrorPixels"});
    const auto& c=At(root,"correction"); Keys(c,{"minimumPoints","minimumRmsSpreadMm","minimumAxisVarianceRatio","maximumFitResidualOutputPixels","maximumDisplacementOutputPixels","maximumRotationDegrees","maximumScaleDifferencePercent"});
    StitchCorrectionJob job{
        {Int(At(s,"gridColumns")),Int(At(s,"gridRows")),Int(At(s,"maximumSeeds")),Num(At(s,"minimumSpacingMm")),Num(At(s,"minimumMinorGradientEigenvalue")),Num(At(s,"minimumGradientAxisRatio"))},
        {Int(At(m,"patchRadiusSamples")),Num(At(m,"patchSpacingMm")),Int(At(m,"searchRadiusSteps")),Num(At(m,"searchStepMm")),Num(At(m,"minimumIntensityStddev")),Num(At(m,"minimumCorrelation")),Num(At(m,"minimumBestSecondGap")),Num(At(m,"maximumReverseErrorMm"))},
        refinement,
        {Int(At(i,"maximumNewtonUpdates")),Int(At(i,"maximumBacktrackingSteps")),Num(At(i,"maximumRoundTripErrorPixels"))},
        {Int(At(c,"minimumPoints")),Num(At(c,"minimumRmsSpreadMm")),Num(At(c,"minimumAxisVarianceRatio")),Num(At(c,"maximumFitResidualOutputPixels")),Num(At(c,"maximumDisplacementOutputPixels")),Num(At(c,"maximumRotationDegrees")),Num(At(c,"maximumScaleDifferencePercent"))},std::string(text)};
    const auto& sp=job.selection; const auto& mp=job.matching; const auto& ip=job.inverse; const auto& cp=job.correction;
    if(sp.grid_columns<1 || sp.grid_columns>64 || sp.grid_rows<1 || sp.grid_rows>64 || sp.maximum_seeds<3 || sp.maximum_seeds>256 ||
        !(sp.minimum_spacing_mm>0) || !(sp.minimum_minor_gradient_eigenvalue>0) || !(sp.minimum_gradient_axis_ratio>0 && sp.minimum_gradient_axis_ratio<1) ||
        mp.patch_radius_samples<1 || mp.patch_radius_samples>8 || mp.search_radius_steps<1 || mp.search_radius_steps>32 || !(mp.patch_spacing_mm>0) || !(mp.search_step_mm>0) ||
        !(mp.minimum_intensity_stddev>0) || mp.minimum_correlation< -1 || mp.minimum_correlation>1 || !(mp.minimum_best_second_gap>0) || mp.minimum_best_second_gap>2 || mp.maximum_reverse_error_mm<0 ||
        ip.maximum_newton_updates<1 || ip.maximum_newton_updates>1000 || ip.maximum_backtracking_steps<1 || ip.maximum_backtracking_steps>64 || !(ip.maximum_round_trip_error_pixels>0) ||
        cp.minimum_points<3 || cp.minimum_points>4096 || !(cp.minimum_rms_spread_mm>0) || !(cp.minimum_axis_variance_ratio>0 && cp.minimum_axis_variance_ratio<1) ||
        cp.maximum_fit_residual_output_pixels<0 || cp.maximum_displacement_output_pixels<0 || cp.maximum_rotation_degrees<0 || cp.maximum_rotation_degrees>180 || cp.maximum_scale_difference_percent<0) Invalid();
    return job;
}

std::string StitchCorrectionReport(const StitchCorrectionJob& job, const AutoImagePatchCorrectionResult& r, const CorrectionReportBindings& b) {
    render::ValidateOutputRaster(b.raster);
    if(r.selection.candidates.size()>4096 || r.selection.selected_indices.size()>kMaximumPatchSeeds)Invalid();
    for(const auto index:r.selection.selected_indices)if(index>=r.selection.candidates.size())Invalid();
    if(r.correction && (r.correction->points.size()>kMaximumPatchSeeds || (r.correction->correction && r.correction->correction->points.size()>kMaximumPatchSeeds)))Invalid();
    Hash(b.profile_file_sha256); Hash(b.profile_fingerprint_sha256); Hash(b.job_sha256);
    for(const auto& input:b.inputs) { Hash(input.sha256); if(input.encoded_size_bytes==0)Invalid(); }
    // The immutable job can only be created by the strict parser.
    std::string out="{\"schemaVersion\":"+Q(job.refinement ? "a0.stitch-correction-report.v2" : "a0.stitch-correction-report.v1")+",\"profileStatus\":\"draft\",\"quality\":\"not-evaluated\",\"productApproved\":false,\"previewRequiresManifest\":true,";
    out+="\"outcome\":"+Q(r.outcome==AutoImagePatchOutcome::Estimated ? "correction-estimated" : r.outcome==AutoImagePatchOutcome::SelectionRejected ? "selection-rejected" : "matching-or-correction-rejected");
    out+=",\"profileFileSha256\":"+Q(b.profile_file_sha256)+",\"profileFingerprintSha256\":"+Q(b.profile_fingerprint_sha256)+",\"jobSha256\":"+Q(b.job_sha256);
    const auto& region=b.raster.region_um;
    out+=",\"outputRaster\":{\"regionUm\":{\"left\":"+std::to_string(region.left)+",\"top\":"+std::to_string(region.top)+",\"right\":"+std::to_string(region.right)+",\"bottom\":"+std::to_string(region.bottom)+"},\"dpi\":"+std::to_string(b.raster.dpi)+",\"widthPixels\":"+std::to_string(b.raster.width_pixels)+",\"heightPixels\":"+std::to_string(b.raster.height_pixels)+'}';
    out+=",\"policy\":"+job.source_json+",\"inputs\":[";
    for(std::size_t n=0;n<2;++n) { if(n)out+=','; out+="{\"cameraAlias\":"+Q(n==0 ? "CAM-A" : "CAM-B")+",\"sha256\":"+Q(b.inputs[n].sha256)+",\"encodedSizeBytes\":"+std::to_string(b.inputs[n].encoded_size_bytes)+'}'; }
    out+="],\"selection\":{\"geometryOutcome\":"+Q(DocumentCorrectionOutcomeCode(r.selection.geometry_outcome))+",\"selectedIndices\":[";
    for(std::size_t n=0;n<r.selection.selected_indices.size();++n) { if(n)out+=',';out+=std::to_string(r.selection.selected_indices[n]); }
    out+="],\"candidates\":[";
    for(std::size_t n=0;n<r.selection.candidates.size();++n) {
        if(n)out+=','; const auto& p=r.selection.candidates[n];
        out+="{\"column\":"+std::to_string(p.column)+",\"row\":"+std::to_string(p.row)+",\"documentMm\":"+Point(p.document_mm)+",\"outcome\":"+Q(PatchSeedOutcomeCode(p.outcome))+",\"texture\":";
        if(p.texture) { const auto t=*p.texture; out+="{\"intensityStddev\":"+N(t.intensity_stddev)+",\"minorGradientEigenvalue\":"+N(t.minor_gradient_eigenvalue)+",\"majorGradientEigenvalue\":"+N(t.major_gradient_eigenvalue)+",\"gradientAxisRatio\":"+N(t.gradient_axis_ratio)+'}'; } else out+="null";
        out+='}';
    }
    out+=" ]},\"matching\":";
    if(r.correction) {
        const auto& m=*r.correction; out+="{\"outcome\":"+Q(m.outcome==ImagePatchCorrectionOutcome::Estimated ? "estimated" : m.outcome==ImagePatchCorrectionOutcome::WorkLimitExceeded ? "work-limit-exceeded" : m.outcome==ImagePatchCorrectionOutcome::InvalidSeeds ? "invalid-seeds" : m.outcome==ImagePatchCorrectionOutcome::MatchingFailed ? "matching-failed" : "correction-rejected")+",\"points\":[";
        for(std::size_t n=0;n<m.points.size();++n) {
            if(n)out+=',';const auto& p=m.points[n];out+="{\"seedDocumentMm\":"+Point(p.seed_document_mm)+",\"outcome\":"+Q(ImagePatchOutcomeCode(p.outcome))+",\"forward\":"+Search(p.forward,job.refinement)+",\"reverse\":"+(p.reverse ? Search(*p.reverse,job.refinement) : "null")+",\"reverseErrorMm\":"+Number(p.reverse_error_mm)+",\"rawCorrespondence\":";
            out+=p.correspondence ? "["+Point(p.correspondence->camera_a_raw_pixel)+','+Point(p.correspondence->camera_b_raw_pixel)+']' : "null"; out+='}';
        }
        out+="],\"rawMapping\":";
        if(m.correction) {
            const auto& raw=*m.correction; out+="{\"outcome\":"+Q(raw.outcome==RawPairCorrectionOutcome::Estimated ? "estimated" : raw.outcome==RawPairCorrectionOutcome::InvalidCorrespondences ? "invalid-correspondences" : raw.outcome==RawPairCorrectionOutcome::PointMappingFailed ? "point-mapping-failed" : "correction-rejected")+",\"points\":[";
            for(std::size_t n=0;n<raw.points.size();++n) { if(n)out+=',';out+='['+Raw(raw.points[n][0])+','+Raw(raw.points[n][1])+']'; }
            out+="],\"fit\":"+(raw.correction ? Fit(*raw.correction) : "null")+'}';
        } else out+="null";
        out+='}';
    } else out+="null";
    out+="}\n";
    if(out.size()>kMaximumCorrectionReportBytes)throw std::length_error("correction report exceeds bound"); return out;
}
} // namespace a0::m2::correction
