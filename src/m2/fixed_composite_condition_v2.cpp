#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <cctype>
#include <cwctype>
#include <iterator>

namespace a0::m2::experimental {
namespace {
namespace json=a0::common::protocol_json;
using json::JsonKind;
using json::JsonValue;

[[noreturn]] void Reject(const std::string& what){throw std::invalid_argument("fixed composite condition v2: "+what);}
struct Failure {[[noreturn]] static void Fail(std::string_view code,std::string_view){Reject("JSON "+std::string(code));}};

// Largest region coordinate the record accepts, in micrometres (1 km). The
// profile's own sheet bounds are far inside this; the record just needs a
// range that cannot overflow the exact raster arithmetic.
constexpr long long kMaximumRegionUm=1'000'000'000;

std::runtime_error IoError(const char* what){
    return std::runtime_error(std::string("fixed composite condition v2 ")+what+" (error "+std::to_string(GetLastError())+")");
}
void RemoveOwnPartial(const std::filesystem::path& partial){
    std::error_code ignored;std::filesystem::remove(partial,ignored);
}

std::string_view WithoutBom(std::string_view text){
    if(text.starts_with("\xEF\xBB\xBF"))text.remove_prefix(3); // encoding marker only
    return text;
}

const JsonValue& Object(const JsonValue& v,const char* where,std::initializer_list<const char*> fields){
    if(v.kind!=JsonKind::object || v.object.size()!=fields.size())Reject(std::string(where)+" has the wrong members");
    for(const auto* f:fields)if(!v.object.contains(f))Reject(std::string(where)+" has the wrong members");
    return v;
}
const JsonValue& At(const JsonValue& v,const char* f){return v.object.at(f);}
std::string Str(const JsonValue& v,const char* what){
    if(v.kind!=JsonKind::string)Reject(std::string(what)+" is not a string");
    return v.string;
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

bool IsReservedDeviceName(std::string_view name){
    // The part before the first dot names the device on Windows, whatever follows.
    std::string stem(name.substr(0,name.find('.')));
    for(auto& ch:stem)ch=static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if(stem=="CON" || stem=="PRN" || stem=="AUX" || stem=="NUL")return true;
    return stem.size()==4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3]>='1' && stem[3]<='9';
}
bool EqualsIgnoringCase(std::string_view a,std::wstring_view wide){
    if(a.size()!=wide.size())return false;
    for(size_t i=0;i<a.size();++i)if(std::tolower(static_cast<unsigned char>(a[i]))!=std::towlower(wide[i]))return false;
    return true;
}

// Reads at most `limit` bytes of a regular file. not_found is reported through
// `found` so the caller decides whether absence is allowed.
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
std::vector<unsigned char> Bytes(std::string_view text){return std::vector<unsigned char>(text.begin(),text.end());}

// Binds an already parsed profile. `file_sha256` is the hash of the bytes the
// profile was parsed from.
FixedCompositeProfileBinding BindParsed(const FixedCompositeConditionV2& c,const RigProfileV2& profile,const std::string& file_sha256){
    if(file_sha256!=c.source.profile_file_sha256)Reject("the profile file differs from the recorded SHA-256");
    const auto fingerprint=profile.FingerprintSha256();
    if(fingerprint!=c.source.profile_fingerprint_sha256)Reject("the profile fingerprint differs from the record");
    // Calibrated draft only (an approved profile belongs to the product path,
    // never to a development condition), valid over the recorded raster.
    auto parameters=profile.ValidateCalibrationForEvaluation(c.raster,render::Resampling::bilinear);
    if(parameters.layout!=StitchLayout::camera_a_left_camera_b_right)Reject("the profile does not put camera A on the left and B on the right");
    return {std::move(parameters),profile.ProfileId(),file_sha256,fingerprint};
}
}

bool IsFixedCompositeProfileFileName(std::string_view name){
    if(name.size()<6 || name.size()>96 || !name.ends_with(".json"))return false;
    if(!std::isalnum(static_cast<unsigned char>(name.front())))return false;
    for(const char ch:name)if(!(std::isalnum(static_cast<unsigned char>(ch)) || ch=='.' || ch=='_' || ch=='-'))return false;
    if(name.find("..")!=std::string_view::npos || IsReservedDeviceName(name))return false;
    return !EqualsIgnoringCase(name,kFixedCompositeConditionName);
}

std::string FixedCompositeProfileFileName(const std::string& profile_file_sha256){
    if(!fixed_composite_detail::IsLowerHex64(profile_file_sha256))throw std::invalid_argument("profile SHA-256 is not 64 lower-case hex digits");
    return "wpd-fixed-composite-profile-"+profile_file_sha256.substr(0,16)+".json";
}

void ValidateFixedCompositeConditionV2(const FixedCompositeConditionV2& c){
    using namespace fixed_composite_detail;
    if(!IsUtcTimestamp(c.created_utc))Reject("createdUtc is not a UTC time");
    if(c.parent_sha256 && !IsLowerHex64(*c.parent_sha256))Reject("parentSha256 is not 64 lower-case hex digits");
    if(!IsFixedCompositeProfileFileName(c.source.profile_file))Reject("profileFile is not a plain file name");
    if(!IsLowerHex64(c.source.profile_file_sha256) || !IsLowerHex64(c.source.profile_fingerprint_sha256))Reject("profile hashes are not 64 lower-case hex digits");
    (void)MountOrientationName(c.orientation);(void)FixedSeamOriginName(c.seam.origin);
    // Exact micrometre/DPI/pixel arithmetic and the decoded-pixel ceiling.
    render::ValidateOutputRaster(c.raster);
    if(!std::isfinite(c.scale_auto_correction_max_percent) || c.scale_auto_correction_max_percent<0)Reject("scaleAutoCorrectionMaxPercent is not a finite non-negative percent");
    if(c.seam.column>=c.raster.width_pixels)Reject("seam column is outside the output raster");
}

std::string FormatFixedCompositeConditionV2(const FixedCompositeConditionV2& c){
    ValidateFixedCompositeConditionV2(c);
    const auto number=[](double v){
        char text[40];const auto r=std::to_chars(text,text+sizeof text,v==0?0.0:v);
        if(r.ec!=std::errc{})throw std::invalid_argument("number is not recordable");
        return std::string(text,r.ptr);
    };
    const auto& r=c.raster;const auto& g=r.region_um;
    std::string s="{\"schema\":\"";s+=kFixedCompositeConditionSchemaV2;
    s+="\",\"approval\":\"unapproved\",\"createdUtc\":\""+c.created_utc+"\",\"revision\":{\"parentSha256\":"+
        (c.parent_sha256?"\""+*c.parent_sha256+"\"":std::string("null"))+
        "},\"source\":{\"profileFile\":\""+c.source.profile_file+"\",\"profileFileSha256\":\""+c.source.profile_file_sha256+
        "\",\"profileFingerprintSha256\":\""+c.source.profile_fingerprint_sha256+"\"},\"mountOrientation\":\""+MountOrientationName(c.orientation)+
        "\",\"outputRaster\":{\"regionUm\":{\"left\":"+std::to_string(g.left)+",\"top\":"+std::to_string(g.top)+",\"right\":"+std::to_string(g.right)+
        ",\"bottom\":"+std::to_string(g.bottom)+"},\"dpi\":"+std::to_string(r.dpi)+",\"widthPixels\":"+std::to_string(r.width_pixels)+
        ",\"heightPixels\":"+std::to_string(r.height_pixels)+"},\"scaleAutoCorrectionMaxPercent\":"+number(c.scale_auto_correction_max_percent)+
        ",\"seam\":{\"column\":"+std::to_string(c.seam.column)+",\"origin\":\""+std::string(FixedSeamOriginName(c.seam.origin))+"\"}}";
    return s;
}

FixedCompositeConditionV2 ParseFixedCompositeConditionV2(std::string_view text){
    const auto root=json::BasicJsonParser<Failure,kMaximumFixedCompositeConditionV2Bytes>(WithoutBom(text)).Parse();
    Object(root,"condition",{"schema","approval","createdUtc","revision","source","mountOrientation","outputRaster","scaleAutoCorrectionMaxPercent","seam"});
    // The only state a condition can be in; "approved" and anything else is refused.
    if(Str(At(root,"schema"),"schema")!=kFixedCompositeConditionSchemaV2 || Str(At(root,"approval"),"approval")!="unapproved")Reject("schema or approval is not accepted");
    FixedCompositeConditionV2 c;
    c.created_utc=Str(At(root,"createdUtc"),"createdUtc");
    const auto& revision=Object(At(root,"revision"),"revision",{"parentSha256"});
    const auto& parent=At(revision,"parentSha256");
    if(parent.kind!=JsonKind::null_value)c.parent_sha256=Str(parent,"parentSha256");
    const auto& source=Object(At(root,"source"),"source",{"profileFile","profileFileSha256","profileFingerprintSha256"});
    c.source.profile_file=Str(At(source,"profileFile"),"profileFile");
    c.source.profile_file_sha256=Str(At(source,"profileFileSha256"),"profileFileSha256");
    c.source.profile_fingerprint_sha256=Str(At(source,"profileFingerprintSha256"),"profileFingerprintSha256");
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
    c.scale_auto_correction_max_percent=Real(At(root,"scaleAutoCorrectionMaxPercent"),"scaleAutoCorrectionMaxPercent");
    const auto& seam=Object(At(root,"seam"),"seam",{"column","origin"});
    c.seam.column=static_cast<std::uint32_t>(Whole(At(seam,"column"),"column",0,render::kMaximumImageDimension));
    const auto origin=ParseFixedSeamOrigin(Str(At(seam,"origin"),"origin"));
    if(!origin)Reject("seam origin is unknown");
    c.seam.origin=*origin;
    ValidateFixedCompositeConditionV2(c); // invalid_argument as well
    return c;
}

std::string FixedCompositeConditionSchemaOf(std::string_view text){
    const auto root=json::BasicJsonParser<Failure,kMaximumFixedCompositeConditionV2Bytes>(WithoutBom(text)).Parse();
    if(root.kind!=JsonKind::object || !root.object.contains("schema"))Reject("no schema");
    return Str(At(root,"schema"),"schema");
}

FixedCompositeProfileBinding BindFixedCompositeProfile(const FixedCompositeConditionV2& c,std::string_view profile_bytes,const FixedCompositeSha256& hash){
    if(!hash)Reject("no SHA-256 provider");
    ValidateFixedCompositeConditionV2(c);
    if(profile_bytes.size()>kMaximumFixedCompositeProfileBytes)Reject("the profile is too large");
    // Hash first: bytes that are not the recorded file are not parsed at all.
    const auto file_sha256=hash(Bytes(profile_bytes));
    if(file_sha256!=c.source.profile_file_sha256)Reject("the profile file differs from the recorded SHA-256");
    return BindParsed(c,RigProfileV2::Parse(profile_bytes),file_sha256);
}

FixedCompositeConditionV2 MakeFixedCompositeConditionV2(const FixedCompositeV2Request& request,std::chrono::system_clock::time_point created,
    const FixedCompositeSha256& hash,const FixedCompositeCreationCheck& check){
    if(!hash)Reject("no SHA-256 provider");
    if(!check)Reject("no creation check: coverage and seam are not verified, so no record is made");
    if(request.profile_bytes.size()>kMaximumFixedCompositeProfileBytes)Reject("the profile is too large");
    FixedCompositeConditionV2 c;
    c.created_utc=FormatFixedCompositeTime(created);
    c.parent_sha256=request.parent_sha256;
    c.source.profile_file=request.profile_file;
    c.source.profile_file_sha256=hash(Bytes(request.profile_bytes));
    c.orientation=request.orientation;
    c.raster=request.raster;
    c.scale_auto_correction_max_percent=request.scale_auto_correction_max_percent;
    c.seam={request.seam_column,request.seam_origin};
    const auto profile=RigProfileV2::Parse(request.profile_bytes);
    c.source.profile_fingerprint_sha256=profile.FingerprintSha256();
    ValidateFixedCompositeConditionV2(c);
    const auto binding=BindParsed(c,profile,c.source.profile_file_sha256);
    check(c,binding);
    ValidateFixedCompositeConditionV2(c);
    return c;
}

std::optional<LoadedFixedCompositeConditionAny> LoadFixedCompositeConditionAny(const std::filesystem::path& file,const FixedCompositeSha256& hash){
    if(!hash)Reject("no SHA-256 provider");
    bool found{};
    const auto text=ReadRegularFile(file,kMaximumFixedCompositeConditionV2Bytes,"fixed composite condition",found);
    if(!found)return {};
    const auto schema=FixedCompositeConditionSchemaOf(text);
    if(schema==kFixedCompositeConditionSchema)
        return LoadedFixedCompositeConditionAny{LoadedFixedCompositeCondition{ParseFixedCompositeCondition(text),hash(Bytes(text))}};
    if(schema!=kFixedCompositeConditionSchemaV2)Reject("unknown schema");
    auto condition=ParseFixedCompositeConditionV2(text);
    // The profile sits in the same folder; the name was checked to be a plain one.
    const auto profile_path=file.parent_path()/condition.source.profile_file;
    bool profile_found{};
    const auto profile_bytes=ReadRegularFile(profile_path,kMaximumFixedCompositeProfileBytes,"fixed composite profile",profile_found);
    if(!profile_found)Reject("the referenced profile is missing from the condition's folder");
    auto binding=BindFixedCompositeProfile(condition,profile_bytes,hash);
    return LoadedFixedCompositeConditionAny{LoadedFixedCompositeConditionV2{std::move(condition),hash(Bytes(text)),std::move(binding)}};
}

void WriteFixedCompositeConditionV2(const std::filesystem::path& file,const FixedCompositeConditionV2& condition){
    // Format validates; the file ends with a newline.
    const auto text=FormatFixedCompositeConditionV2(condition)+"\n";
    std::error_code error;
    const auto parent=file.parent_path();
    if(file.filename().empty() || (!parent.empty() && !std::filesystem::is_directory(parent,error)))
        throw std::invalid_argument("fixed composite condition folder does not exist");
    const auto existing=std::filesystem::symlink_status(file,error);
    if(existing.type()!=std::filesystem::file_type::not_found) // MSVC also sets `error` for a missing file
        throw std::invalid_argument("fixed composite condition already exists; never overwrite");
    auto partial=file;partial+=L".partial";
    // Exclusive: a partial that is already there is somebody else's, not ours to reuse.
    const HANDLE handle=CreateFileW(partial.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE){
        if(GetLastError()==ERROR_FILE_EXISTS || GetLastError()==ERROR_ALREADY_EXISTS)
            throw std::invalid_argument("fixed composite condition partial already exists");
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
        if(reread!=text || FormatFixedCompositeConditionV2(ParseFixedCompositeConditionV2(reread))+"\n"!=text)
            throw std::runtime_error("fixed composite condition partial changed");
        // No MOVEFILE_REPLACE_EXISTING: an existing target makes the move fail.
        if(!MoveFileExW(partial.c_str(),file.c_str(),MOVEFILE_WRITE_THROUGH))
            throw IoError("cannot be moved into place without replacing");
    }catch(...){
        RemoveOwnPartial(partial);throw;
    }
}
}
