#include "a0/m2/stitch_color_job.hpp"
#include "a0/common/protocol_json.hpp"
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace a0::m2::correction {
namespace {
using namespace a0::common::protocol_json;
[[noreturn]] void Invalid(){throw std::invalid_argument("invalid stitch color job or report bindings");}
struct Failure { [[noreturn]] static void Fail(std::string_view,std::string_view){Invalid();} };
double Num(const JsonValue& v){
    if(v.kind!=JsonKind::number)Invalid();double n{};const auto p=std::from_chars(v.string.data(),v.string.data()+v.string.size(),n);
    if(p.ec!=std::errc{}||p.ptr!=v.string.data()+v.string.size()||!std::isfinite(n))Invalid();return n;
}
std::uint32_t Int(const JsonValue& v){
    if(v.kind!=JsonKind::number||v.string.find_first_not_of("0123456789")!=std::string::npos)Invalid();std::uint32_t n{};
    const auto p=std::from_chars(v.string.data(),v.string.data()+v.string.size(),n);if(p.ec!=std::errc{}||p.ptr!=v.string.data()+v.string.size())Invalid();return n;
}
std::string Q(std::string_view value){return '"'+std::string(value)+'"';} // fixed codes and validated SHA only
std::string N(double n){if(!std::isfinite(n))return "null";char buffer[64];const auto p=std::to_chars(buffer,buffer+sizeof(buffer),n);if(p.ec!=std::errc{})Invalid();return {buffer,p.ptr};}
void Hash(std::string_view h){if(h.size()!=64||h.find_first_not_of("0123456789abcdef")!=std::string_view::npos)Invalid();}
std::string Bgr(const std::array<double,3>& v){return '['+N(v[0])+','+N(v[1])+','+N(v[2])+']';}
std::string Fit(const ColorCorrectionResult& r){
    if(r.sample_count>kMaximumColorSamples||r.violations.size()>9)Invalid();
    std::string out="{\"outcome\":"+Q(ColorCorrectionOutcomeCode(r.outcome))+",\"sampleCount\":"+std::to_string(r.sample_count)+",\"channels\":[";
    for(std::size_t c=0;c<3;++c){if(c)out+=',';if(!r.channels[c]){out+="null";continue;}const auto& d=*r.channels[c];
        out+="{\"cameraAStddevDn\":"+N(d.camera_a_stddev_dn)+",\"cameraBStddevDn\":"+N(d.camera_b_stddev_dn)+",\"estimate\":";
        out+=d.estimate ? "{\"gain\":"+N(d.estimate->gain)+",\"offsetDn\":"+N(d.estimate->offset_dn)+'}' : "null";
        out+=",\"residuals\":";out+=d.residuals ? "{\"rmsDn\":"+N(d.residuals->rms_dn)+",\"maximumDn\":"+N(d.residuals->maximum_dn)+'}' : "null";out+='}';
    }
    out+="],\"violations\":[";
    for(std::size_t n=0;n<r.violations.size();++n){if(n)out+=',';const auto& v=r.violations[n];if(v.channel>=3||(v.sample_index&&*v.sample_index>=r.sample_count))Invalid();
        out+="{\"reason\":"+Q(ColorCorrectionOutcomeCode(v.reason))+",\"channelIndex\":"+std::to_string(v.channel)+",\"sampleIndex\":"+(v.sample_index?std::to_string(*v.sample_index):"null")+'}';
    }
    return out+"],\"candidateAvailable\":"+(r.transform?"true}":"false}");
}
} // namespace
StitchColorJob ParseStitchColorJob(std::string_view text){
    if(text.empty()||text.size()>kMaximumColorJobBytes)Invalid();int depth=0;bool quoted=false,escaped=false;
    for(char c:text){if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
        else if(c=='"')quoted=true;else if(c=='{'||c=='['){if(++depth>8)Invalid();}else if(c=='}'||c==']'){if(--depth<0)Invalid();}}
    const auto root=BasicJsonParser<Failure,kMaximumColorJobBytes>(text).Parse();
    constexpr std::array<std::string_view,8> keys{"schemaVersion","minimumSamples","minimumSampleDn","maximumSampleDn","minimumStddevDn","maximumGainDifferencePercent","maximumOffsetDn","maximumResidualDn"};
    if(root.kind!=JsonKind::object||root.object.size()!=keys.size())Invalid();for(auto k:keys)if(!root.object.contains(std::string(k)))Invalid();
    const auto& schema=root.object.at("schemaVersion");if(schema.kind!=JsonKind::string||schema.string!="a0.stitch-color-job.v1")Invalid();
    const auto& o=root.object;
    const ColorCorrectionPolicy p{Int(o.at("minimumSamples")),Num(o.at("minimumSampleDn")),Num(o.at("maximumSampleDn")),Num(o.at("minimumStddevDn")),Num(o.at("maximumGainDifferencePercent")),Num(o.at("maximumOffsetDn")),Num(o.at("maximumResidualDn"))};
    // Reuse the pure estimator's policy validation; empty data performs no fit.
    (void)EstimateBoundedColorCorrection({},p);
    return {p,std::string(text)};
}
std::string StitchColorReport(const StitchColorJob& job,const ImageColorCorrectionResult& r,const StitchColorReportBindings& b){
    render::ValidateOutputRaster(b.raster);if(r.points.size()>kMaximumColorSamples)Invalid();
    Hash(b.profile_file_sha256);Hash(b.profile_fingerprint_sha256);Hash(b.job_sha256);Hash(b.geometry_report_sha256);
    for(const auto& input:b.inputs){Hash(input.sha256);if(input.encoded_size_bytes==0||input.encoded_size_bytes>67108864)Invalid();}
    std::string out="{\"schemaVersion\":\"a0.stitch-color-report.v1\",\"profileStatus\":\"draft\",\"quality\":\"not-evaluated\",\"productApproved\":false,\"previewRequiresManifest\":true,\"model\":\"encoded-dn-diagonal-affine\",\"channelOrder\":\"BGR\",\"outcome\":";
    out+=Q(r.outcome==ImageColorOutcome::Estimated?"color-estimated":r.outcome==ImageColorOutcome::SamplingFailed?"color-sampling-failed":r.outcome==ImageColorOutcome::TooManySeeds?"too-many-color-seeds":"color-fit-rejected");
    out+=",\"profileFileSha256\":"+Q(b.profile_file_sha256)+",\"profileFingerprintSha256\":"+Q(b.profile_fingerprint_sha256)+",\"jobSha256\":"+Q(b.job_sha256)+",\"geometryReportSha256\":"+Q(b.geometry_report_sha256);
    const auto& region=b.raster.region_um;
    out+=",\"outputRaster\":{\"regionUm\":{\"left\":"+std::to_string(region.left)+",\"top\":"+std::to_string(region.top)+",\"right\":"+std::to_string(region.right)+",\"bottom\":"+std::to_string(region.bottom)+"},\"dpi\":"+std::to_string(b.raster.dpi)+",\"widthPixels\":"+std::to_string(b.raster.width_pixels)+",\"heightPixels\":"+std::to_string(b.raster.height_pixels)+'}';
    out+=",\"policy\":"+job.source_json+",\"inputs\":[";
    for(std::size_t n=0;n<2;++n){if(n)out+=',';out+="{\"cameraAlias\":"+Q(n==0?"CAM-A":"CAM-B")+",\"sha256\":"+Q(b.inputs[n].sha256)+",\"encodedSizeBytes\":"+std::to_string(b.inputs[n].encoded_size_bytes)+'}';}
    out+="],\"points\":[";
    for(std::size_t n=0;n<r.points.size();++n){if(n)out+=',';const auto& p=r.points[n];out+="{\"documentMm\":["+N(p.document_mm.x)+','+N(p.document_mm.y)+"],\"outcome\":"+Q(p.outcome==ColorSampleOutcome::Sampled?"color-sampled":p.outcome==ColorSampleOutcome::InvalidSeed?"invalid-color-seed":"color-sample-unavailable")+",\"cameraABgr\":"+(p.colors?Bgr(p.colors->camera_a_bgr):"null")+",\"cameraBBgr\":"+(p.colors?Bgr(p.colors->camera_b_bgr):"null")+'}';}
    out+="],\"fit\":"+(r.correction?Fit(*r.correction):"null")+"}\n";
    if(out.size()>kMaximumColorReportBytes)throw std::length_error("color report exceeds bound");return out;
}
} // namespace a0::m2::correction
