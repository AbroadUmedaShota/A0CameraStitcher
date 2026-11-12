#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/m2/experimental/parallax_correction.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <map>
#include <sstream>
#include <numbers>

namespace a0::m2::experimental {
namespace {
namespace json=a0::common::protocol_json;
using json::JsonKind;
using json::JsonValue;
using content_registration::PoseParameters;
using content_registration::RegistrationEnvelope;
using content_registration::RegistrationLimits;

[[noreturn]] void Reject(const std::string& what){throw std::invalid_argument("fixed composite condition v3: "+what);}
struct Failure {[[noreturn]] static void Fail(std::string_view code,std::string_view){Reject("JSON "+std::string(code));}};

// Largest region coordinate the record accepts, in micrometres (1 km), as in v2.
constexpr long long kMaximumRegionUm=1'000'000'000;
// Largest count of blocks, in the limits (a count of blocks of the estimator's coarse stage).
constexpr long long kMaximumBlocks=1'000'000;

std::runtime_error IoError(const char* what){
    return std::runtime_error(std::string("fixed composite condition v3 ")+what+" (error "+std::to_string(GetLastError())+")");
}
void RemoveOwnPartial(const std::filesystem::path& partial){
    std::error_code ignored;std::filesystem::remove(partial,ignored);
}
std::string_view WithoutBom(std::string_view text){
    if(text.starts_with("\xEF\xBB\xBF"))text.remove_prefix(3); // encoding marker only
    return text;
}
std::vector<unsigned char> Bytes(std::string_view text){return std::vector<unsigned char>(text.begin(),text.end());}

const JsonValue& Object(const JsonValue& v,const char* where,std::initializer_list<const char*> fields){
    if(v.kind!=JsonKind::object || v.object.size()!=fields.size())Reject(std::string(where)+" has the wrong members");
    for(const auto* f:fields)if(!v.object.contains(f))Reject(std::string(where)+" has the wrong members");
    return v;
}
const JsonValue& At(const JsonValue& v,const char* f){return v.object.at(f);}
bool IsNull(const JsonValue& v){return v.kind==JsonKind::null_value;}
std::string Str(const JsonValue& v,const char* what){
    if(v.kind!=JsonKind::string)Reject(std::string(what)+" is not a string");
    return v.string;
}
bool Bool(const JsonValue& v,const char* what){
    if(v.kind!=JsonKind::boolean)Reject(std::string(what)+" is not a boolean");
    return v.boolean;
}
long long Whole(const JsonValue& v,const char* what,long long lo,long long hi){
    if(v.kind!=JsonKind::number || v.string.empty())Reject(std::string(what)+" is not a number");
    const size_t digits=v.string[0]=='-'?1:0;
    if(v.string.size()==digits || v.string.find_first_not_of("0123456789",digits)!=std::string::npos)Reject(std::string(what)+" is not a whole number");
    long long n{};const auto r=std::from_chars(v.string.data(),v.string.data()+v.string.size(),n);
    // "-0" is not the canonical zero.
    if(r.ec!=std::errc{} || r.ptr!=v.string.data()+v.string.size() || n<lo || n>hi || (n==0 && digits==1))Reject(std::string(what)+" is out of range");
    return n;
}
double Real(const JsonValue& v,const char* what){
    if(v.kind!=JsonKind::number)Reject(std::string(what)+" is not a number");
    double n{};
    const auto r=std::from_chars(v.string.data(),v.string.data()+v.string.size(),n,std::chars_format::general);
    if(r.ec!=std::errc{} || r.ptr!=v.string.data()+v.string.size() || !std::isfinite(n))Reject(std::string(what)+" is not a finite number");
    return n;
}
std::optional<std::string> OptionalStr(const JsonValue& v,const char* what){
    if(IsNull(v))return std::nullopt;
    return Str(v,what);
}

// Reads at most `limit` bytes of a regular file. not_found is reported through `found` so the caller decides whether absence is allowed.
std::string ReadRegularFile(const std::filesystem::path& file,std::size_t limit,const char* what,bool& found){
    std::error_code error;const auto status=std::filesystem::symlink_status(file,error);
    found=true;
    if(status.type()==std::filesystem::file_type::not_found){found=false;return {};}
    if(error || status.type()!=std::filesystem::file_type::regular)throw std::invalid_argument(std::string(what)+" is not a regular file");
    std::ifstream in(file,std::ios::binary);if(!in)throw std::runtime_error(std::string(what)+" unreadable");
    std::string text;text.resize(limit+1);in.read(text.data(),static_cast<std::streamsize>(text.size()));
    if(in.bad())throw std::runtime_error(std::string(what)+" unreadable");
    text.resize(static_cast<size_t>(in.gcount()));
    if(text.size()>limit)throw std::invalid_argument(std::string(what)+" too large");
    return text;
}

// Writes `text` as a NEW file: an exclusive partial beside it, flushed, read back and compared (and checked by `verify`), then moved
// without replacing. An existing file or partial is never overwritten. (The same steps as the v2 writer's.)
void WriteNewFile(const std::filesystem::path& file,const std::string& text,const char* what,const std::function<bool(const std::string&)>& verify){
    std::error_code error;
    const auto parent=file.parent_path();
    if(file.filename().empty() || (!parent.empty() && !std::filesystem::is_directory(parent,error)))
        throw std::invalid_argument(std::string(what)+" folder does not exist");
    const auto existing=std::filesystem::symlink_status(file,error);
    if(existing.type()!=std::filesystem::file_type::not_found) // MSVC also sets `error` for a missing file
        throw std::invalid_argument(std::string(what)+" already exists; never overwrite");
    auto partial=file;partial+=L".partial";
    // Exclusive: a partial that is already there is somebody else's, not ours to reuse.
    const HANDLE handle=CreateFileW(partial.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE){
        if(GetLastError()==ERROR_FILE_EXISTS || GetLastError()==ERROR_ALREADY_EXISTS)
            throw std::invalid_argument(std::string(what)+" partial already exists");
        throw IoError("partial cannot be created");
    }
    bool open=true;
    try{
        DWORD written{};
        if(!WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) || written!=text.size())throw IoError("write failed");
        if(!FlushFileBuffers(handle))throw IoError("flush failed");
        open=false;if(!CloseHandle(handle))throw IoError("close failed");
    }catch(...){
        if(open)CloseHandle(handle);RemoveOwnPartial(partial);throw;
    }
    try{
        std::string reread;
        { // closed before the move
            std::ifstream in(partial,std::ios::binary);
            reread.assign(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
        }
        if(reread!=text || !verify(reread))
            throw std::runtime_error(std::string(what)+" partial changed");
        // No MOVEFILE_REPLACE_EXISTING: an existing target makes the move fail.
        if(!MoveFileExW(partial.c_str(),file.c_str(),MOVEFILE_WRITE_THROUGH))
            throw IoError("cannot be moved into place without replacing");
    }catch(...){
        RemoveOwnPartial(partial);throw;
    }
}

// ---- Number text ----
// 17 significant digits in the classic locale, so that every double is written exactly and read back as the same double; a zero is written
// as 0 (docs/design/fixed-composite-registration-formats.md, section 2).
std::string Num(double v){
    if(!std::isfinite(v))throw std::invalid_argument("number is not recordable");
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<(v==0?0.0:v);
    return out.str();
}
std::string Quoted(const std::string& s){return "\""+s+"\"";}
std::string OptionalQuoted(const std::optional<std::string>& s){return s?Quoted(*s):std::string("null");}

bool Positive(double v){return std::isfinite(v) && v>0;}

// ---- Validation pieces ----
void ValidateEnvelope(const RegistrationEnvelope& e){
    if(!(Positive(e.position_mm) && Positive(e.rotation_deg) && Positive(e.tilt_deg) && Positive(e.height_percent)))
        Reject("registration.envelope is not set (positive and finite)");
}
// The same rules as the estimator's own check of its limits (content_registration.cpp, ValidateRegistrationSettings).
void ValidateLimits(const RegistrationLimits& l,const char* where){
    const auto fail=[&](const char* what){Reject(std::string(where)+" "+what);};
    if(!(std::isfinite(l.brown_lowe_alpha) && l.brown_lowe_alpha>=0 && std::isfinite(l.brown_lowe_beta) && l.brown_lowe_beta>=0 && l.brown_lowe_beta<1))
        fail("brownLoweAlpha or brownLoweBeta is not set");
    if(l.minimum_unique_blocks<1 || l.minimum_unique_blocks>kMaximumBlocks)fail("minimumUniqueBlocks is not set");
    if(!(Positive(l.maximum_prediction_sigma_px) && Positive(l.maximum_residual_rms_px) && Positive(l.maximum_residual_p95_px) &&
        Positive(l.maximum_bin_bias_px) && Positive(l.convergence_px) && Positive(l.start_agreement_px)))fail("has a limit that is not set (positive and finite)");
}
void ValidateBasePoseFinite(const PoseParameters& p){
    for(const double v:{p.rx_deg,p.ry_deg,p.rz_deg,p.tx_mm,p.ty_mm,p.tz_percent})
        if(!std::isfinite(v))Reject("basePose is not finite");
}
void ValidateChange(const FixedCompositeConditionV3& c){
    const auto& ch=c.revision.change;
    const bool first=c.revision.sequence==1;
    const bool has_v2=ch.v2_sha256.has_value(),has_report=ch.report_sha256.has_value(),has_pair=ch.pair_id.has_value(),
        has_entered=ch.entered.has_value(),has_max=ch.maximum_seam_displacement_px.has_value(),has_restored=ch.restored_sha256.has_value();
    const auto only=[&](bool v2,bool report,bool pair,bool entered,bool max,bool restored){
        if(has_v2!=v2 || has_report!=report || has_pair!=pair || has_entered!=entered || has_max!=max || has_restored!=restored)
            Reject("revision.change has the members of another kind");
    };
    using Kind=FixedCompositeRevisionKind;
    if(FixedCompositeRevisionKindText(ch.kind).empty())Reject("revision.change.kind is unknown");
    // r000001 is made by the tool from nothing or from a v2 record; every later revision changes an earlier one.
    const bool first_kind=ch.kind==Kind::created || ch.kind==Kind::migrated_from_v2;
    if(first!=first_kind)Reject("revision.change.kind does not fit the revision number (created and migrated-from-v2 are revision 1 only)");
    switch(ch.kind){
    case Kind::created:
    case Kind::values:
        only(false,false,false,false,false,false);break;
    case Kind::migrated_from_v2:
        only(true,false,false,false,false,false);
        if(!fixed_composite_detail::IsLowerHex64(*ch.v2_sha256))Reject("revision.change.v2Sha256 is not 64 lower-case hex digits");
        break;
    case Kind::saved_estimate:
        only(false,true,true,false,false,false);
        if(!fixed_composite_detail::IsLowerHex64(*ch.report_sha256))Reject("revision.change.reportSha256 is not 64 lower-case hex digits");
        if(!fixed_composite_detail::IsPairId(*ch.pair_id))Reject("revision.change.pairId is not a pair identity");
        break;
    case Kind::manual_adjustment:{
        only(false,false,false,true,true,false);
        const auto& e=*ch.entered;
        if(!std::isfinite(e.seam_dx_px) || !std::isfinite(e.seam_dy_px) || !std::isfinite(e.rotation_deg))Reject("revision.change.entered is not finite");
        // Recorded, never refused by size (the owner's decision 9): only a negative or non-finite measure is not a measure.
        if(!std::isfinite(*ch.maximum_seam_displacement_px) || *ch.maximum_seam_displacement_px<0)Reject("revision.change.maximumSeamDisplacementPx is not a finite non-negative number");
        break;
    }
    case Kind::rollback:
        only(false,false,false,false,false,true);
        if(!fixed_composite_detail::IsLowerHex64(*ch.restored_sha256))Reject("revision.change.restoredSha256 is not 64 lower-case hex digits");
        break;
    }
    // The base pose of a first revision is the calibration's.
    if(first_kind){
        const auto& p=c.base_pose;
        if(!(p.rx_deg==0 && p.ry_deg==0 && p.rz_deg==0 && p.tx_mm==0 && p.ty_mm==0 && p.tz_percent==0))
            Reject("basePose of a first revision is not zero");
    }
}
}
// |basePose| against the envelope (design 11.19.2): the base pose must lie inside the range measured from the calibration.
void RequireFixedCompositeBasePoseInEnvelopeV3(const FixedCompositeConditionV3& c){
    const auto& p=c.base_pose;const auto& e=c.registration.envelope;
    if(std::hypot(p.tx_mm,p.ty_mm)>e.position_mm || std::abs(p.rz_deg)>e.rotation_deg || std::hypot(p.rx_deg,p.ry_deg)>e.tilt_deg ||
        std::abs(p.tz_percent)>e.height_percent)
        Reject("basePose is outside registration.envelope; start again from the calibration");
}
namespace {

// ---- Format pieces ----
std::string FormatLimits(const RegistrationLimits& l){
    return "{\"brownLoweAlpha\":"+Num(l.brown_lowe_alpha)+",\"brownLoweBeta\":"+Num(l.brown_lowe_beta)+",\"minimumUniqueBlocks\":"+std::to_string(l.minimum_unique_blocks)+
        ",\"maximumPredictionSigmaPx\":"+Num(l.maximum_prediction_sigma_px)+",\"maximumResidualRmsPx\":"+Num(l.maximum_residual_rms_px)+
        ",\"maximumResidualP95Px\":"+Num(l.maximum_residual_p95_px)+",\"maximumBinBiasPx\":"+Num(l.maximum_bin_bias_px)+
        ",\"convergencePx\":"+Num(l.convergence_px)+",\"startAgreementPx\":"+Num(l.start_agreement_px)+"}";
}
std::string FormatChange(const FixedCompositeRevisionChangeV3& ch){
    std::string s="{\"kind\":\""+std::string(FixedCompositeRevisionKindText(ch.kind))+"\"";
    using Kind=FixedCompositeRevisionKind;
    switch(ch.kind){
    case Kind::created:case Kind::values:break;
    case Kind::migrated_from_v2:s+=",\"v2Sha256\":"+Quoted(*ch.v2_sha256);break;
    case Kind::saved_estimate:s+=",\"reportSha256\":"+Quoted(*ch.report_sha256)+",\"pairId\":"+Quoted(*ch.pair_id);break;
    case Kind::manual_adjustment:
        s+=",\"entered\":{\"seamDxPx\":"+Num(ch.entered->seam_dx_px)+",\"seamDyPx\":"+Num(ch.entered->seam_dy_px)+",\"rotationDeg\":"+Num(ch.entered->rotation_deg)+
            "},\"maximumSeamDisplacementPx\":"+Num(*ch.maximum_seam_displacement_px);
        break;
    case Kind::rollback:s+=",\"restoredSha256\":"+Quoted(*ch.restored_sha256);break;
    }
    return s+"}";
}

// ---- Parse pieces ----
RegistrationLimits ParseLimits(const JsonValue& v,const char* where){
    const auto& o=Object(v,where,{"brownLoweAlpha","brownLoweBeta","minimumUniqueBlocks","maximumPredictionSigmaPx","maximumResidualRmsPx","maximumResidualP95Px",
        "maximumBinBiasPx","convergencePx","startAgreementPx"});
    RegistrationLimits l;
    l.brown_lowe_alpha=Real(At(o,"brownLoweAlpha"),"brownLoweAlpha");
    l.brown_lowe_beta=Real(At(o,"brownLoweBeta"),"brownLoweBeta");
    l.minimum_unique_blocks=static_cast<std::uint32_t>(Whole(At(o,"minimumUniqueBlocks"),"minimumUniqueBlocks",0,kMaximumBlocks));
    l.maximum_prediction_sigma_px=Real(At(o,"maximumPredictionSigmaPx"),"maximumPredictionSigmaPx");
    l.maximum_residual_rms_px=Real(At(o,"maximumResidualRmsPx"),"maximumResidualRmsPx");
    l.maximum_residual_p95_px=Real(At(o,"maximumResidualP95Px"),"maximumResidualP95Px");
    l.maximum_bin_bias_px=Real(At(o,"maximumBinBiasPx"),"maximumBinBiasPx");
    l.convergence_px=Real(At(o,"convergencePx"),"convergencePx");
    l.start_agreement_px=Real(At(o,"startAgreementPx"),"startAgreementPx");
    return l;
}
FixedCompositeRevisionChangeV3 ParseChange(const JsonValue& v){
    if(v.kind!=JsonKind::object || !v.object.contains("kind"))Reject("revision.change has the wrong members");
    const auto kind=ParseFixedCompositeRevisionKind(Str(At(v,"kind"),"revision.change.kind"));
    if(!kind)Reject("revision.change.kind is unknown");
    FixedCompositeRevisionChangeV3 ch;ch.kind=*kind;
    using Kind=FixedCompositeRevisionKind;
    switch(*kind){
    case Kind::created:case Kind::values:
        Object(v,"revision.change",{"kind"});break;
    case Kind::migrated_from_v2:
        Object(v,"revision.change",{"kind","v2Sha256"});
        ch.v2_sha256=Str(At(v,"v2Sha256"),"v2Sha256");break;
    case Kind::saved_estimate:
        Object(v,"revision.change",{"kind","reportSha256","pairId"});
        ch.report_sha256=Str(At(v,"reportSha256"),"reportSha256");ch.pair_id=Str(At(v,"pairId"),"pairId");break;
    case Kind::manual_adjustment:{
        Object(v,"revision.change",{"kind","entered","maximumSeamDisplacementPx"});
        const auto& e=Object(At(v,"entered"),"revision.change.entered",{"seamDxPx","seamDyPx","rotationDeg"});
        FixedCompositeRevisionChangeV3::Entered entered;
        entered.seam_dx_px=Real(At(e,"seamDxPx"),"seamDxPx");entered.seam_dy_px=Real(At(e,"seamDyPx"),"seamDyPx");entered.rotation_deg=Real(At(e,"rotationDeg"),"rotationDeg");
        ch.entered=entered;
        ch.maximum_seam_displacement_px=Real(At(v,"maximumSeamDisplacementPx"),"maximumSeamDisplacementPx");
        break;
    }
    case Kind::rollback:
        Object(v,"revision.change",{"kind","restoredSha256"});
        ch.restored_sha256=Str(At(v,"restoredSha256"),"restoredSha256");break;
    }
    return ch;
}

// Parses a text with v3's own ceiling (kMaximumFixedCompositeConditionV3Bytes).
JsonValue ParseJson(std::string_view text){
    return json::BasicJsonParser<Failure,kMaximumFixedCompositeConditionV3Bytes>(WithoutBom(text)).Parse();
}

// Binds an already parsed profile; `file_sha256` is the hash of the bytes the profile was parsed from.
FixedCompositeProfileBinding BindParsed(const FixedCompositeConditionV3& c,const RigProfileV2& profile,const std::string& file_sha256){
    if(file_sha256!=c.source.profile_file_sha256)Reject("the profile file differs from the recorded SHA-256");
    const auto fingerprint=profile.FingerprintSha256();
    if(fingerprint!=c.source.profile_fingerprint_sha256)Reject("the profile fingerprint differs from the record");
    // Calibrated draft only (an approved profile belongs to the product path, never to a development condition), valid over the raster.
    auto parameters=profile.ValidateCalibrationForEvaluation(c.raster,render::Resampling::bilinear);
    if(parameters.layout!=StitchLayout::camera_a_left_camera_b_right)Reject("the profile does not put camera A on the left and B on the right");
    return {std::move(parameters),profile.ProfileId(),file_sha256,fingerprint};
}

// The registration values (registration, seamGate, revisionRules): the members of a record and of the policy file are the same.
FixedCompositeConditionV3::Registration ParseRegistration(const JsonValue& value){
    FixedCompositeConditionV3::Registration r;
    const auto& reg=Object(value,"registration",{"envelope","limits","geometry","surfaceEnvelope","applyBeyondEnvelope","surfaceScaleWarnPercent","parallax","live"});
    const auto& env=Object(At(reg,"envelope"),"registration.envelope",{"positionMm","rotationDeg","tiltDeg","heightPercent"});
    r.envelope.position_mm=Real(At(env,"positionMm"),"positionMm");
    r.envelope.rotation_deg=Real(At(env,"rotationDeg"),"rotationDeg");
    r.envelope.tilt_deg=Real(At(env,"tiltDeg"),"tiltDeg");
    r.envelope.height_percent=Real(At(env,"heightPercent"),"heightPercent");
    r.limits=ParseLimits(At(reg,"limits"),"registration.limits");
    const auto& geometry=Object(At(reg,"geometry"),"registration.geometry",{"omegaMarginPx","guardPx","bandHalfWidthPx"});
    r.geometry.omega_margin_px=static_cast<std::uint32_t>(Whole(At(geometry,"omegaMarginPx"),"omegaMarginPx",0,render::kMaximumImageDimension));
    r.geometry.guard_px=static_cast<std::uint32_t>(Whole(At(geometry,"guardPx"),"guardPx",0,render::kMaximumImageDimension));
    r.geometry.band_half_width_px=static_cast<std::uint32_t>(Whole(At(geometry,"bandHalfWidthPx"),"bandHalfWidthPx",0,render::kMaximumImageDimension));
    if(const auto& surface=At(reg,"surfaceEnvelope");!IsNull(surface)){
        const auto& o=Object(surface,"registration.surfaceEnvelope",{"heightMm","tiltDeg"});
        r.surface_envelope=SurfaceEnvelope{Real(At(o,"heightMm"),"heightMm"),Real(At(o,"tiltDeg"),"tiltDeg")};
    }
    r.apply_beyond_envelope=Bool(At(reg,"applyBeyondEnvelope"),"applyBeyondEnvelope");
    if(const auto& warn=At(reg,"surfaceScaleWarnPercent");!IsNull(warn))r.surface_scale_warn_percent=Real(warn,"surfaceScaleWarnPercent");
    if(const auto& parallax=At(reg,"parallax");!IsNull(parallax)){
        const auto& o=Object(parallax,"registration.parallax",{"maxLiftMm"});
        r.parallax=FixedCompositeConditionV3::Registration::Parallax{Real(At(o,"maxLiftMm"),"maxLiftMm")};
    }
    if(const auto& live=At(reg,"live");!IsNull(live)){
        const auto& o=Object(live,"registration.live",{"limits"});
        r.live=FixedCompositeConditionV3::Registration::Live{ParseLimits(At(o,"limits"),"registration.live.limits")};
    }
    return r;
}
FixedCompositeConditionV3::SeamGateLine ParseSeamGate(const JsonValue& value){
    const auto& gate=Object(value,"seamGate",{"limitPx","guardBandPx"});
    return {Real(At(gate,"limitPx"),"limitPx"),Real(At(gate,"guardBandPx"),"guardBandPx")};
}
FixedCompositeConditionV3::RevisionRules ParseRevisionRules(const JsonValue& value){
    FixedCompositeConditionV3::RevisionRules rules;
    const auto& o=Object(value,"revisionRules",{"manualAdjustmentMaxSeamPx"});
    if(const auto& max=At(o,"manualAdjustmentMaxSeamPx");!IsNull(max))rules.manual_adjustment_max_seam_px=Real(max,"manualAdjustmentMaxSeamPx");
    return rules;
}

// Everything of a record that a rollback must carry over from the revision it restores: all but the time and the revision itself.
bool SameContent(const FixedCompositeConditionV3& a,const FixedCompositeConditionV3& b){
    return FormatFixedCompositeConditionV3([&]{auto x=a;x.created_utc=b.created_utc;x.revision=b.revision;return x;}())==FormatFixedCompositeConditionV3(b);
}

// What the capture path's design constants assume of W and Omega (#417). Read from the headers only (the constants are inline
// constexpr): this library links no estimator, layer 2 or gate.
// - The guard keeps the estimate's points off Omega: a point of the fine stage reads its patch around it, and W starts guardPx
//   from Omega. With a guard of half the patch or less the estimate reads Omega's pixels, and the gate would measure the
//   material the estimate was fitted to (a false pass).
// - Omega's half width (half the blend's mixing width, rounded up, plus omegaMarginPx: RegistrationGeometry, as the gate lays
//   Omega out) is not more than layer 2's a (w = 1 over Omega); layer 2 refuses every request otherwise. Only with layer 2.
void ValidateGeometryAgainstDesign(const FixedCompositeConditionV3::Registration& r){
    constexpr std::uint64_t patch=content_registration::kRegistrationDesignDraft.fine_patch_px;
    if(2*static_cast<std::uint64_t>(r.geometry.guard_px)<=patch)
        Reject("registration.geometry.guardPx is not more than half the estimate's fine patch ("+std::to_string(patch/2)+" px)");
    if(r.parallax){
        const std::uint64_t half=(static_cast<std::uint64_t>(kSeamBlendDesign.width_pixels)+1)/2+r.geometry.omega_margin_px;
        if(static_cast<double>(half)>parallax_correction::kParallaxDesignDraft.weight_flat_px)
            Reject("registration.geometry.omegaMarginPx makes Omega's half width larger than layer 2's flat width a ("+
                std::to_string(static_cast<long long>(parallax_correction::kParallaxDesignDraft.weight_flat_px))+" px)");
    }
}

void ValidateRegistrationValues(const FixedCompositeConditionV3::Registration& r,const FixedCompositeConditionV3::SeamGateLine& seam_gate,
    const FixedCompositeConditionV3::RevisionRules& revision_rules){
    ValidateEnvelope(r.envelope);
    ValidateLimits(r.limits,"registration.limits");
    if(r.geometry.band_half_width_px<1 || r.geometry.band_half_width_px>render::kMaximumImageDimension ||
        r.geometry.omega_margin_px>render::kMaximumImageDimension || r.geometry.guard_px>render::kMaximumImageDimension ||
        static_cast<std::uint64_t>(r.geometry.band_half_width_px)<=static_cast<std::uint64_t>(r.geometry.omega_margin_px)+r.geometry.guard_px)
        Reject("registration.geometry is not set (the band must reach past the margin and the guard)");
    ValidateGeometryAgainstDesign(r);
    if(r.surface_envelope){
        if(!(Positive(r.surface_envelope->height_mm) && Positive(r.surface_envelope->tilt_deg) && r.surface_envelope->tilt_deg<90))
            Reject("registration.surfaceEnvelope is not set (positive and finite, the tilt below 90)");
    }
    if(!r.apply_beyond_envelope)Reject("registration.applyBeyondEnvelope is not set");
    // null is a value: no warning about the scale of the drawing (the owner's decision 10). A number must be a measure.
    if(r.surface_scale_warn_percent && !(std::isfinite(*r.surface_scale_warn_percent) && *r.surface_scale_warn_percent>=0))
        Reject("registration.surfaceScaleWarnPercent is not a finite non-negative percent");
    if(r.parallax && !Positive(r.parallax->max_lift_mm))Reject("registration.parallax.maxLiftMm is not positive and finite");
    if(r.live)ValidateLimits(r.live->limits,"registration.live.limits");
    if(!(Positive(seam_gate.limit_px) && Positive(seam_gate.guard_band_px)))Reject("seamGate is not set (positive and finite)");
    // The field is reserved and its value is null: no upper bound on a manual adjustment (the owner's decision 9). Nothing refuses by it.
    if(revision_rules.manual_adjustment_max_seam_px && !Positive(*revision_rules.manual_adjustment_max_seam_px))
        Reject("revisionRules.manualAdjustmentMaxSeamPx is not positive and finite");
}
}

std::string FixedCompositeRevisionFileName(std::uint32_t sequence){
    if(sequence<1 || sequence>kMaximumFixedCompositeRevisionSequence)throw std::invalid_argument("revision number is outside 1..999999");
    char text[16];std::snprintf(text,sizeof text,"r%06u.json",sequence);
    return text;
}
std::optional<std::uint32_t> ParseFixedCompositeRevisionFileName(std::string_view name){
    if(name.size()!=12 || name[0]!='r' || !name.ends_with(".json"))return std::nullopt;
    std::uint32_t n=0;
    for(size_t i=1;i<=6;++i){if(name[i]<'0' || name[i]>'9')return std::nullopt;n=n*10+static_cast<std::uint32_t>(name[i]-'0');}
    if(n<1)return std::nullopt;
    return n;
}

void ValidateFixedCompositeConditionV3(const FixedCompositeConditionV3& c){
    using namespace fixed_composite_detail;
    if(!IsUtcTimestamp(c.created_utc))Reject("createdUtc is not a UTC time");
    if(c.revision.sequence<1 || c.revision.sequence>kMaximumFixedCompositeRevisionSequence)Reject("revision.sequence is outside 1..999999");
    if(c.revision.parent_sha256 && !IsLowerHex64(*c.revision.parent_sha256))Reject("revision.parentSha256 is not 64 lower-case hex digits");
    if(c.revision.parent_sha256.has_value()!=(c.revision.sequence!=1))Reject("revision.parentSha256 is null for revision 1 and only for it");
    ValidateChange(c);
    if(!IsFixedCompositeProfileFileName(c.source.profile_file))Reject("profileFile is not a plain file name");
    if(!IsLowerHex64(c.source.profile_file_sha256) || !IsLowerHex64(c.source.profile_fingerprint_sha256))Reject("profile hashes are not 64 lower-case hex digits");
    if(!IsLowerHex64(c.source.policy_sha256))Reject("source.policySha256 is not 64 lower-case hex digits");
    (void)MountOrientationName(c.orientation);(void)FixedSeamOriginName(c.seam.origin);
    // Exact micrometre/DPI/pixel arithmetic and the decoded-pixel ceiling.
    render::ValidateOutputRaster(c.raster);
    if(c.seam.column>=c.raster.width_pixels)Reject("seam column is outside the output raster");
    ValidateBasePoseFinite(c.base_pose);
    ValidateRegistrationValues(c.registration,c.seam_gate,c.revision_rules);
    // A hand adjustment has no upper bound (the owner's decision): its base pose is not held to the envelope when it is written or parsed.
    // The readers that draw with the condition apply RequireFixedCompositeBasePoseInEnvelopeV3 themselves.
    if(c.revision.change.kind!=FixedCompositeRevisionKind::manual_adjustment)RequireFixedCompositeBasePoseInEnvelopeV3(c);
}


std::string FormatFixedCompositeConditionV3(const FixedCompositeConditionV3& c){
    ValidateFixedCompositeConditionV3(c);
    const auto& r=c.raster;const auto& g=r.region_um;const auto& p=c.base_pose;const auto& reg=c.registration;const auto& e=reg.envelope;
    std::string s="{\"schema\":\"";s+=kFixedCompositeConditionSchemaV3;
    s+="\",\"approval\":\"unapproved\",\"createdUtc\":\""+c.created_utc+"\",\"revision\":{\"sequence\":"+std::to_string(c.revision.sequence)+
        ",\"parentSha256\":"+OptionalQuoted(c.revision.parent_sha256)+",\"change\":"+FormatChange(c.revision.change)+
        "},\"source\":{\"profileFile\":\""+c.source.profile_file+"\",\"profileFileSha256\":\""+c.source.profile_file_sha256+
        "\",\"profileFingerprintSha256\":\""+c.source.profile_fingerprint_sha256+"\",\"policySha256\":\""+c.source.policy_sha256+
        "\"},\"mountOrientation\":\""+std::string(MountOrientationName(c.orientation))+
        "\",\"outputRaster\":{\"regionUm\":{\"left\":"+std::to_string(g.left)+",\"top\":"+std::to_string(g.top)+",\"right\":"+std::to_string(g.right)+
        ",\"bottom\":"+std::to_string(g.bottom)+"},\"dpi\":"+std::to_string(r.dpi)+",\"widthPixels\":"+std::to_string(r.width_pixels)+
        ",\"heightPixels\":"+std::to_string(r.height_pixels)+"},\"seam\":{\"column\":"+std::to_string(c.seam.column)+",\"origin\":\""+
        std::string(FixedSeamOriginName(c.seam.origin))+"\"},\"basePose\":{\"rxDeg\":"+Num(p.rx_deg)+",\"ryDeg\":"+Num(p.ry_deg)+",\"rzDeg\":"+Num(p.rz_deg)+
        ",\"txMm\":"+Num(p.tx_mm)+",\"tyMm\":"+Num(p.ty_mm)+",\"tzPercent\":"+Num(p.tz_percent)+"},\"registration\":{\"envelope\":{\"positionMm\":"+Num(e.position_mm)+
        ",\"rotationDeg\":"+Num(e.rotation_deg)+",\"tiltDeg\":"+Num(e.tilt_deg)+",\"heightPercent\":"+Num(e.height_percent)+"},\"limits\":"+FormatLimits(reg.limits)+
        ",\"geometry\":{\"omegaMarginPx\":"+std::to_string(reg.geometry.omega_margin_px)+",\"guardPx\":"+std::to_string(reg.geometry.guard_px)+
        ",\"bandHalfWidthPx\":"+std::to_string(reg.geometry.band_half_width_px)+"},\"surfaceEnvelope\":";
    s+=reg.surface_envelope?"{\"heightMm\":"+Num(reg.surface_envelope->height_mm)+",\"tiltDeg\":"+Num(reg.surface_envelope->tilt_deg)+"}":std::string("null");
    s+=std::string(",\"applyBeyondEnvelope\":")+(*reg.apply_beyond_envelope?"true":"false")+",\"surfaceScaleWarnPercent\":"+
        (reg.surface_scale_warn_percent?Num(*reg.surface_scale_warn_percent):std::string("null"))+",\"parallax\":"+
        (reg.parallax?"{\"maxLiftMm\":"+Num(reg.parallax->max_lift_mm)+"}":std::string("null"))+",\"live\":"+
        (reg.live?"{\"limits\":"+FormatLimits(reg.live->limits)+"}":std::string("null"))+"},\"seamGate\":{\"limitPx\":"+Num(c.seam_gate.limit_px)+
        ",\"guardBandPx\":"+Num(c.seam_gate.guard_band_px)+"},\"revisionRules\":{\"manualAdjustmentMaxSeamPx\":"+
        (c.revision_rules.manual_adjustment_max_seam_px?Num(*c.revision_rules.manual_adjustment_max_seam_px):std::string("null"))+"}}";
    return s;
}

FixedCompositeConditionV3 ParseFixedCompositeConditionV3(std::string_view text){
    const auto root=ParseJson(text);
    Object(root,"condition",{"schema","approval","createdUtc","revision","source","mountOrientation","outputRaster","seam","basePose","registration","seamGate","revisionRules"});
    // The only state a condition can be in; "approved" and anything else is refused.
    if(Str(At(root,"schema"),"schema")!=kFixedCompositeConditionSchemaV3 || Str(At(root,"approval"),"approval")!="unapproved")Reject("schema or approval is not accepted");
    FixedCompositeConditionV3 c;
    c.created_utc=Str(At(root,"createdUtc"),"createdUtc");
    const auto& revision=Object(At(root,"revision"),"revision",{"sequence","parentSha256","change"});
    c.revision.sequence=static_cast<std::uint32_t>(Whole(At(revision,"sequence"),"revision.sequence",1,kMaximumFixedCompositeRevisionSequence));
    c.revision.parent_sha256=OptionalStr(At(revision,"parentSha256"),"parentSha256");
    c.revision.change=ParseChange(At(revision,"change"));
    const auto& source=Object(At(root,"source"),"source",{"profileFile","profileFileSha256","profileFingerprintSha256","policySha256"});
    c.source.profile_file=Str(At(source,"profileFile"),"profileFile");
    c.source.profile_file_sha256=Str(At(source,"profileFileSha256"),"profileFileSha256");
    c.source.profile_fingerprint_sha256=Str(At(source,"profileFingerprintSha256"),"profileFingerprintSha256");
    c.source.policy_sha256=Str(At(source,"policySha256"),"policySha256");
    const auto orientation=ParseMountOrientation(Str(At(root,"mountOrientation"),"mountOrientation"));
    if(!orientation)Reject("mountOrientation is unknown");
    c.orientation=*orientation;
    const auto& raster=Object(At(root,"outputRaster"),"outputRaster",{"regionUm","dpi","widthPixels","heightPixels"});
    const auto& region=Object(At(raster,"regionUm"),"regionUm",{"left","top","right","bottom"});
    const auto um=[&](const char* f){return static_cast<std::int64_t>(Whole(At(region,f),f,0,kMaximumRegionUm));};
    const auto left=um("left"),top=um("top"),right=um("right"),bottom=um("bottom");
    c.raster=render::OutputRaster{render::DocumentRegionUm{left,top,right,bottom},
        static_cast<std::uint32_t>(Whole(At(raster,"dpi"),"dpi",1,65535)),
        static_cast<std::uint32_t>(Whole(At(raster,"widthPixels"),"widthPixels",1,render::kMaximumImageDimension)),
        static_cast<std::uint32_t>(Whole(At(raster,"heightPixels"),"heightPixels",1,render::kMaximumImageDimension))};
    const auto& seam=Object(At(root,"seam"),"seam",{"column","origin"});
    c.seam.column=static_cast<std::uint32_t>(Whole(At(seam,"column"),"column",0,render::kMaximumImageDimension));
    const auto origin=ParseFixedSeamOrigin(Str(At(seam,"origin"),"origin"));
    if(!origin)Reject("seam origin is unknown");
    c.seam.origin=*origin;
    const auto& pose=Object(At(root,"basePose"),"basePose",{"rxDeg","ryDeg","rzDeg","txMm","tyMm","tzPercent"});
    c.base_pose={Real(At(pose,"rxDeg"),"rxDeg"),Real(At(pose,"ryDeg"),"ryDeg"),Real(At(pose,"rzDeg"),"rzDeg"),
        Real(At(pose,"txMm"),"txMm"),Real(At(pose,"tyMm"),"tyMm"),Real(At(pose,"tzPercent"),"tzPercent")};
    c.registration=ParseRegistration(At(root,"registration"));
    c.seam_gate=ParseSeamGate(At(root,"seamGate"));
    c.revision_rules=ParseRevisionRules(At(root,"revisionRules"));
    ValidateFixedCompositeConditionV3(c); // invalid_argument as well
    return c;
}

FixedCompositeRegistrationValuesV3 ParseFixedCompositeRegistrationPolicyV3(std::string_view text,std::string_view expected_schema){
    const auto root=json::BasicJsonParser<Failure,kMaximumFixedCompositeRegistrationPolicyV3Bytes>(WithoutBom(text)).Parse();
    Object(root,"registration policy",{"schema","registration","seamGate","revisionRules"});
    if(Str(At(root,"schema"),"schema")!=expected_schema)Reject("registration policy: the schema is not accepted");
    FixedCompositeRegistrationValuesV3 values;
    values.registration=ParseRegistration(At(root,"registration"));
    values.seam_gate=ParseSeamGate(At(root,"seamGate"));
    values.revision_rules=ParseRevisionRules(At(root,"revisionRules"));
    ValidateRegistrationValues(values.registration,values.seam_gate,values.revision_rules);
    return values;
}

FixedCompositeProfileBinding BindFixedCompositeProfileV3(const FixedCompositeConditionV3& c,std::string_view profile_bytes,const FixedCompositeSha256& hash){
    if(!hash)Reject("no SHA-256 provider");
    ValidateFixedCompositeConditionV3(c);
    if(profile_bytes.size()>kMaximumFixedCompositeProfileBytes)Reject("the profile is too large");
    // Hash first: bytes that are not the recorded file are not parsed at all.
    const auto file_sha256=hash(Bytes(profile_bytes));
    if(file_sha256!=c.source.profile_file_sha256)Reject("the profile file differs from the recorded SHA-256");
    return BindParsed(c,RigProfileV2::Parse(profile_bytes),file_sha256);
}

FixedCompositeConditionV3 MakeFixedCompositeConditionV3(const FixedCompositeV3Request& request,std::chrono::system_clock::time_point created,
    const FixedCompositeSha256& hash,const FixedCompositeCreationCheckV3& check){
    if(!hash)Reject("no SHA-256 provider");
    if(!check)Reject("no creation check: coverage, seam and strain are not verified, so no record is made");
    if(request.profile_bytes.size()>kMaximumFixedCompositeProfileBytes)Reject("the profile is too large");
    FixedCompositeConditionV3 c;
    c.created_utc=FormatFixedCompositeTime(created);
    c.revision.sequence=request.sequence;
    c.revision.parent_sha256=request.parent_sha256;
    c.revision.change=request.change;
    c.source.profile_file=request.profile_file;
    c.source.profile_file_sha256=hash(Bytes(request.profile_bytes));
    c.source.policy_sha256=request.policy_sha256;
    c.orientation=request.orientation;
    c.raster=request.raster;
    c.seam={request.seam_column,request.seam_origin};
    c.base_pose=request.base_pose;
    c.registration=request.registration;
    c.seam_gate=request.seam_gate;
    c.revision_rules=request.revision_rules;
    const auto profile=RigProfileV2::Parse(request.profile_bytes);
    c.source.profile_fingerprint_sha256=profile.FingerprintSha256();
    ValidateFixedCompositeConditionV3(c);
    const auto binding=BindParsed(c,profile,c.source.profile_file_sha256);
    check(c,binding);
    ValidateFixedCompositeConditionV3(c);
    return c;
}

namespace {
// A revision file is r<6 digits>.json in the folder named kFixedCompositeRevisionFolderName; the profile sits in the folder above it
// (the settings folder, 11.19.4). The number in the name is the record's own.
std::filesystem::path ProfileFolderOfRevisionFile(const std::filesystem::path& file){
    const auto folder=file.parent_path();
    if(folder.filename().string()!=kFixedCompositeRevisionFolderName)
        Reject("a revision file is kept in the folder "+std::string(kFixedCompositeRevisionFolderName));
    return folder.parent_path();
}
LoadedFixedCompositeConditionV3 BindLoaded(const std::filesystem::path& file,const std::string& text,const FixedCompositeSha256& hash){
    auto condition=ParseFixedCompositeConditionV3(text);
    const auto number=ParseFixedCompositeRevisionFileName(file.filename().string());
    if(!number || *number!=condition.revision.sequence)Reject("the file name does not carry the record's revision number");
    const auto profile_path=ProfileFolderOfRevisionFile(file)/condition.source.profile_file;
    bool profile_found{};
    std::string profile_bytes;
    try{profile_bytes=ReadRegularFile(profile_path,kMaximumFixedCompositeProfileBytes,"fixed composite profile",profile_found);}
    catch(const std::exception& error){ // not a regular file, too large, or unreadable (sharing violation, access denied)
        throw FixedCompositeProfileError(FixedCompositeProfileError::Kind::differs,error.what());
    }
    if(!profile_found)throw FixedCompositeProfileError(FixedCompositeProfileError::Kind::missing,"fixed composite condition v3: the referenced profile is missing from the settings folder");
    // Whatever is wrong with a profile that is there is the profile's, not the record's.
    std::optional<FixedCompositeProfileBinding> bound;
    try{bound=BindFixedCompositeProfileV3(condition,profile_bytes,hash);}
    catch(const FixedCompositeProfileError&){throw;}
    catch(const std::invalid_argument& error){throw FixedCompositeProfileError(FixedCompositeProfileError::Kind::differs,error.what());}
    return LoadedFixedCompositeConditionV3{std::move(condition),hash(Bytes(text)),std::move(*bound)};
}
}

LoadedFixedCompositeConditionV3 LoadFixedCompositeConditionV3(const std::filesystem::path& file,const FixedCompositeSha256& hash){
    if(!hash)Reject("no SHA-256 provider");
    bool found{};
    const auto text=ReadRegularFile(file,kMaximumFixedCompositeConditionV3Bytes,"fixed composite condition",found);
    if(!found)Reject("the revision file does not exist");
    return BindLoaded(file,text,hash);
}

std::optional<LoadedFixedCompositeConditionV3> LoadActiveFixedCompositeRevisionV3(const std::filesystem::path& folder,const FixedCompositeSha256& hash){
    if(!hash)Reject("no SHA-256 provider");
    std::error_code error;
    const auto status=std::filesystem::symlink_status(folder,error);
    if(status.type()==std::filesystem::file_type::not_found)return std::nullopt;
    if(error || status.type()!=std::filesystem::file_type::directory)Reject("the revisions folder is not a folder");
    // Every entry is a revision, a partial of a writer that has not finished (not counted), or a reason to refuse. Nothing is guessed.
    std::map<std::uint32_t,std::filesystem::path> revisions;
    for(const auto& entry:std::filesystem::directory_iterator(folder)){
        const auto name=entry.path().filename().string();
        if(name.ends_with(".partial"))continue;
        const auto number=ParseFixedCompositeRevisionFileName(name);
        if(!number)Reject("the revisions folder holds a file that is not a revision: "+name);
        revisions.emplace(*number,entry.path());
    }
    if(revisions.empty())return std::nullopt;
    // Unbroken from r000001.
    std::uint32_t expected=1;
    for(const auto& [number,path]:revisions){
        if(number!=expected)Reject("the revisions are not numbered without a gap from r000001");
        ++expected;(void)path;
    }
    // Each parent hash is the SHA-256 of the previous file's bytes; a rollback carries the content of the revision it names.
    std::string previous_hash;
    std::map<std::string,FixedCompositeConditionV3> by_hash;
    std::string last_text;
    for(const auto& [number,path]:revisions){
        bool found{};
        const auto text=ReadRegularFile(path,kMaximumFixedCompositeConditionV3Bytes,"fixed composite condition",found);
        if(!found)Reject("a revision disappeared while reading");
        const auto condition=ParseFixedCompositeConditionV3(text);
        if(condition.revision.sequence!=number)Reject("the file name does not carry the record's revision number");
        if(number==1){
            if(condition.revision.parent_sha256)Reject("revision 1 names a parent");
        }else if(!condition.revision.parent_sha256 || *condition.revision.parent_sha256!=previous_hash)
            Reject("the parent hash of revision "+std::to_string(number)+" is not the SHA-256 of revision "+std::to_string(number-1));
        if(condition.revision.change.kind==FixedCompositeRevisionKind::rollback){
            const auto restored=by_hash.find(*condition.revision.change.restored_sha256);
            if(restored==by_hash.end())Reject("revision "+std::to_string(number)+" restores a revision that is not earlier in the chain");
            if(!SameContent(condition,restored->second))Reject("revision "+std::to_string(number)+" does not carry the content of the revision it restores");
        }
        previous_hash=hash(Bytes(text));
        by_hash.emplace(previous_hash,condition);
        last_text=text;
    }
    return BindLoaded(revisions.rbegin()->second,last_text,hash);
}

void WriteFixedCompositeConditionV3(const std::filesystem::path& folder,const FixedCompositeConditionV3& condition){
    // Format validates; the file ends with a newline.
    const auto text=FormatFixedCompositeConditionV3(condition)+"\n";
    const auto name=FixedCompositeRevisionFileName(condition.revision.sequence);
    if(condition.revision.sequence>1){
        // A revision is written after the one before it, so a number is never skipped.
        std::error_code error;
        const auto before=std::filesystem::symlink_status(folder/FixedCompositeRevisionFileName(condition.revision.sequence-1),error);
        if(before.type()!=std::filesystem::file_type::regular)throw std::invalid_argument("the previous revision is not there; a revision number is never skipped");
    }
    WriteNewFile(folder/name,text,"fixed composite revision",[&](const std::string& reread){
        return FormatFixedCompositeConditionV3(ParseFixedCompositeConditionV3(reread))+"\n"==text;
    });
}
}
