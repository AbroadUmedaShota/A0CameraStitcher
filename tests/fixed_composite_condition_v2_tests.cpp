// Fixed composite condition v2 record (#405 W1): format, strict reader, the
// reference to a draft rig profile by SHA-256 and fingerprint, creation, the
// version-aware loader and the new-file writer. Camera free; files go to a
// temporary folder only. Argument 1 is the shared rig profile v2 fixture folder.
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace a0::m2::experimental;
namespace render=a0::m2::render;
namespace fs=std::filesystem;
using namespace std::chrono_literals;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr)
template<class Call> bool Throws(Call call){
    try{call();}catch(const std::invalid_argument&){return true;}catch(const std::runtime_error&){return true;}
    return false;
}
#define REJECT(expr) Check(Throws([&]{(void)(expr);}),"rejects: " #expr)

const auto kCreated=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026}/11/4}+12h+34min+56s+789ms};
// Test fixture values only; the product code has no default for the limit or the raster.
constexpr double kScaleLimit=1;
const std::string kParent(64,'c');

std::string FakeHash(const std::vector<unsigned char>& bytes){
    unsigned long long h=1469598103934665603ULL;for(const auto b:bytes){h^=b;h*=1099511628211ULL;}
    char text[17];std::snprintf(text,sizeof text,"%016llx",h);return std::string(48,'0')+text;
}
const FixedCompositeSha256 kHash=[](const std::vector<unsigned char>& bytes){return FakeHash(bytes);};
std::string HashOf(const std::string& text){return FakeHash(std::vector<unsigned char>(text.begin(),text.end()));}

std::string ReadAll(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void WriteAll(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;}
std::string Replace(std::string text,const std::string& from,const std::string& to){
    const auto at=text.find(from);
    if(at==std::string::npos){Check(false,("test token absent: "+from).c_str());return text;}
    text.replace(at,from.size(),to);return text;
}
fs::path TempParent(){
#ifdef _MSC_VER
    char* value=nullptr;size_t length=0;
    if(_dupenv_s(&value,&length,"A0_TEST_TMPDIR")==0 && value!=nullptr){
        const std::string text(value);std::free(value);
        if(!text.empty())return fs::path(text);
    }
#else
    if(const char* value=std::getenv("A0_TEST_TMPDIR");value!=nullptr && *value!='\0')return fs::path(value);
#endif
    return fs::temp_directory_path();
}

// 10 mm x 8 mm of the sheet at 254 DPI (0.1 mm per pixel): 100 x 80 pixels.
render::OutputRaster Raster(){return render::OutputRaster{render::DocumentRegionUm{100000,100000,110000,108000},254,100,80};}
FixedCompositeV2Request Request(const std::string& profile){
    FixedCompositeV2Request r;
    r.profile_file="wpd-fixed-composite-profile-test.json";r.profile_bytes=profile;
    r.raster=Raster();r.scale_auto_correction_max_percent=kScaleLimit;
    r.seam_column=50;r.seam_origin=FixedSeamOrigin::band_center_provisional;
    return r;
}
// The creation check of the entrance (#405 W2/W6) is not built yet. This one
// accepts and records what it was given.
struct Spy {
    int calls=0;FixedCompositeConditionV2 condition;std::optional<FixedCompositeProfileBinding> binding;
    FixedCompositeCreationCheck Check(){
        return [this](const FixedCompositeConditionV2& c,const FixedCompositeProfileBinding& b){++calls;condition=c;binding=b;};
    }
};
const FixedCompositeCreationCheck kAccept=[](const FixedCompositeConditionV2&,const FixedCompositeProfileBinding&){};
FixedCompositeConditionV2 Make(const std::string& profile,Spy& spy){return MakeFixedCompositeConditionV2(Request(profile),kCreated,kHash,spy.Check());}
FixedCompositeConditionV2 Make(const std::string& profile){Spy spy;return Make(profile,spy);}

bool SameCondition(const FixedCompositeConditionV2& a,const FixedCompositeConditionV2& b){
    const auto& g=a.raster.region_um;const auto& h=b.raster.region_um;
    return a.created_utc==b.created_utc && a.parent_sha256==b.parent_sha256 && a.source.profile_file==b.source.profile_file &&
        a.source.profile_file_sha256==b.source.profile_file_sha256 && a.source.profile_fingerprint_sha256==b.source.profile_fingerprint_sha256 &&
        a.orientation==b.orientation && g.left==h.left && g.top==h.top && g.right==h.right && g.bottom==h.bottom &&
        a.raster.dpi==b.raster.dpi && a.raster.width_pixels==b.raster.width_pixels && a.raster.height_pixels==b.raster.height_pixels &&
        a.scale_auto_correction_max_percent==b.scale_auto_correction_max_percent && a.seam.column==b.seam.column && a.seam.origin==b.seam.origin;
}

std::string draft,approved,blank;

void FileNames(){
    CHECK(IsFixedCompositeProfileFileName("wpd-fixed-composite-profile-0123456789abcdef.json"));
    CHECK(IsFixedCompositeProfileFileName("p.json") && IsFixedCompositeProfileFileName("A_b-1.2.json"));
    for(const char* bad:{"","json",".json","a.txt","a.JSON","a/b.json","a\\b.json","..\\a.json","../a.json","a..b.json","C:a.json","c:\\a.json",
        "a b.json","a:stream.json","-a.json",".a.json","_a.json","con.json","CON.json","nul.json","Aux.x.json","com1.json","lpt9.json","a\xC3\xA9.json",
        "wpd-fixed-composite-condition.json","WPD-Fixed-Composite-Condition.JSON"})
        Check(!IsFixedCompositeProfileFileName(bad),("rejected profile file name: "+std::string(bad)).c_str());
    CHECK(IsFixedCompositeProfileFileName("com0.json") && IsFixedCompositeProfileFileName("com10.json") && IsFixedCompositeProfileFileName("console.json"));
    CHECK(!IsFixedCompositeProfileFileName(std::string(92,'a')+".json") && IsFixedCompositeProfileFileName(std::string(91,'a')+".json"));
    CHECK(!IsFixedCompositeProfileFileName(std::string("a\0b.json",8)));
    const auto sha=std::string("0123456789abcdef")+std::string(48,'f');
    CHECK(FixedCompositeProfileFileName(sha)=="wpd-fixed-composite-profile-0123456789abcdef.json");
    CHECK(IsFixedCompositeProfileFileName(FixedCompositeProfileFileName(sha)));
    REJECT(FixedCompositeProfileFileName(std::string(63,'a')));
    REJECT(FixedCompositeProfileFileName(std::string(64,'A')));
}

void MakeAndFormat(){
    Spy spy;
    const auto c=Make(draft,spy);
    const auto profile=a0::m2::RigProfileV2::Parse(draft);
    CHECK(c.created_utc=="2026-11-04T12:34:56Z");
    CHECK(!c.parent_sha256);
    CHECK(c.source.profile_file=="wpd-fixed-composite-profile-test.json" && c.source.profile_file_sha256==HashOf(draft));
    CHECK(c.source.profile_fingerprint_sha256==profile.FingerprintSha256());
    CHECK(c.orientation==MountOrientation::normal && c.scale_auto_correction_max_percent==kScaleLimit);
    CHECK(c.raster.dpi==254 && c.raster.width_pixels==100 && c.raster.height_pixels==80 && c.raster.region_um.right==110000);
    CHECK(c.seam.column==50 && c.seam.origin==FixedSeamOrigin::band_center_provisional);
    // The creation check ran once, on the finished record and the verified profile.
    CHECK(spy.calls==1 && SameCondition(spy.condition,c) && spy.binding);
    CHECK(spy.binding->parameters.layout==a0::m2::StitchLayout::camera_a_left_camera_b_right);
    CHECK(spy.binding->parameters.raster.dpi==254 && spy.binding->parameters.raster.width_pixels==100);
    CHECK(spy.binding->parameters.resampling==render::Resampling::bilinear);
    CHECK(spy.binding->profile_id && *spy.binding->profile_id=="synthetic-fingerprint-draft");
    CHECK(spy.binding->profile_file_sha256==c.source.profile_file_sha256 && spy.binding->profile_fingerprint_sha256==c.source.profile_fingerprint_sha256);
    // The profile's cameras are applied unchanged.
    const auto direct=profile.ValidateCalibrationForEvaluation(Raster(),render::Resampling::bilinear);
    CHECK(spy.binding->parameters.camera_b.document_to_image==direct.camera_b.document_to_image && spy.binding->parameters.camera_a.intrinsics.fx_pixels==10000);
    // Canonical text, exactly.
    const auto text=FormatFixedCompositeConditionV2(c);
    const auto expected=std::string("{\"schema\":\"a0.fixed-composite-condition.v2\",\"approval\":\"unapproved\",\"createdUtc\":\"2026-11-04T12:34:56Z\","
        "\"revision\":{\"parentSha256\":null},\"source\":{\"profileFile\":\"wpd-fixed-composite-profile-test.json\",\"profileFileSha256\":\"")+
        c.source.profile_file_sha256+"\",\"profileFingerprintSha256\":\""+c.source.profile_fingerprint_sha256+
        "\"},\"mountOrientation\":\"normal\",\"outputRaster\":{\"regionUm\":{\"left\":100000,\"top\":100000,\"right\":110000,\"bottom\":108000},"
        "\"dpi\":254,\"widthPixels\":100,\"heightPixels\":80},\"scaleAutoCorrectionMaxPercent\":1,\"seam\":{\"column\":50,\"origin\":\"band-center-provisional\"}}";
    CHECK(text==expected);
    CHECK(text.size()<kMaximumFixedCompositeConditionV2Bytes);
    CHECK(SameCondition(ParseFixedCompositeConditionV2(text),c));
    CHECK(FormatFixedCompositeConditionV2(ParseFixedCompositeConditionV2(text))==text);
    // Parent hash, orientation, origin and a fractional limit survive a round trip.
    auto request=Request(draft);
    request.parent_sha256=kParent;request.orientation=MountOrientation::rotated_180;request.seam_origin=FixedSeamOrigin::operator_set;
    request.seam_column=61;request.scale_auto_correction_max_percent=2.5;
    const auto second=MakeFixedCompositeConditionV2(request,kCreated,kHash,kAccept);
    CHECK(second.parent_sha256==kParent && second.orientation==MountOrientation::rotated_180 && second.seam.origin==FixedSeamOrigin::operator_set);
    const auto second_text=FormatFixedCompositeConditionV2(second);
    CHECK(second_text.find("\"parentSha256\":\""+kParent+"\"")!=std::string::npos && second_text.find("\"mountOrientation\":\"rotated-180\"")!=std::string::npos);
    CHECK(second_text.find("\"scaleAutoCorrectionMaxPercent\":2.5")!=std::string::npos && second_text.find("\"origin\":\"operator\"")!=std::string::npos);
    CHECK(SameCondition(ParseFixedCompositeConditionV2(second_text),second));
    // A zero limit is a real value (never correct), not an absence.
    request.scale_auto_correction_max_percent=0;
    CHECK(MakeFixedCompositeConditionV2(request,kCreated,kHash,kAccept).scale_auto_correction_max_percent==0);
    // The record is a value: nothing in it points back into the request.
    CHECK(FixedCompositeConditionSchemaOf(text)=="a0.fixed-composite-condition.v2");
}

void MakeRejects(){
    // The creation check is mandatory and its refusal stops the record.
    REJECT(MakeFixedCompositeConditionV2(Request(draft),kCreated,kHash,FixedCompositeCreationCheck{}));
    REJECT(MakeFixedCompositeConditionV2(Request(draft),kCreated,FixedCompositeSha256{},kAccept));
    int calls=0;
    REJECT(MakeFixedCompositeConditionV2(Request(draft),kCreated,kHash,[&](const FixedCompositeConditionV2&,const FixedCompositeProfileBinding&){++calls;throw std::invalid_argument("seam outside overlap");}));
    CHECK(calls==1);
    // Nothing reaches the check when the inputs are wrong.
    const auto never=[&](const FixedCompositeConditionV2&,const FixedCompositeProfileBinding&){++calls;};
    calls=0;
    const auto reject=[&](const char* what,auto change){
        auto request=Request(draft);change(request);
        Check(Throws([&]{(void)MakeFixedCompositeConditionV2(request,kCreated,kHash,never);}),what);
    };
    reject("approved profile",[&](auto& r){r.profile_bytes=approved;});
    reject("template profile",[&](auto& r){r.profile_bytes=blank;});
    reject("profile bytes that are not a profile",[&](auto& r){r.profile_bytes="{}";});
    reject("empty profile bytes",[&](auto& r){r.profile_bytes.clear();});
    reject("oversize profile",[&](auto& r){r.profile_bytes=std::string(kMaximumFixedCompositeProfileBytes+1,' ');});
    reject("camera order top/bottom",[&](auto& r){r.profile_bytes=Replace(draft,"CAM-A-left-CAM-B-right","CAM-A-top-CAM-B-bottom");});
    reject("camera order not in the profile grammar",[&](auto& r){r.profile_bytes=Replace(draft,"CAM-A-left-CAM-B-right","CAM-B-left-CAM-A-right");});
    reject("empty profile name",[&](auto& r){r.profile_file.clear();});
    reject("profile name with a folder",[&](auto& r){r.profile_file="sub/p.json";});
    reject("profile name with a parent folder",[&](auto& r){r.profile_file="..\\p.json";});
    reject("profile name on another drive",[&](auto& r){r.profile_file="D:p.json";});
    reject("profile name that is the condition itself",[&](auto& r){r.profile_file="wpd-fixed-composite-condition.json";});
    reject("raster width off by one",[&](auto& r){r.raster.width_pixels=101;});
    reject("raster height off by one",[&](auto& r){r.raster.height_pixels=79;});
    reject("raster that is unset",[&](auto& r){r.raster=render::OutputRaster{render::DocumentRegionUm{0,0,0,0},0,0,0};});
    reject("negative region",[&](auto& r){r.raster=render::OutputRaster{render::DocumentRegionUm{-1000,100000,110000,108000},254,101,80};});
    reject("empty region",[&](auto& r){r.raster=render::OutputRaster{render::DocumentRegionUm{100000,100000,100000,108000},254,0,80};});
    reject("DPI 0",[&](auto& r){r.raster.dpi=0;});
    reject("region beyond the sheet",[&](auto& r){r.raster=render::OutputRaster{render::DocumentRegionUm{100000,100000,1200000,108000},254,11000,80};});
    reject("limit not set",[&](auto& r){r.scale_auto_correction_max_percent=std::numeric_limits<double>::quiet_NaN();});
    reject("limit infinite",[&](auto& r){r.scale_auto_correction_max_percent=std::numeric_limits<double>::infinity();});
    reject("limit negative",[&](auto& r){r.scale_auto_correction_max_percent=-0.5;});
    reject("seam column at the width",[&](auto& r){r.seam_column=100;});
    reject("seam column far outside",[&](auto& r){r.seam_column=4000000000u;});
    reject("parent too short",[&](auto& r){r.parent_sha256=std::string(63,'c');});
    reject("parent upper case",[&](auto& r){r.parent_sha256=std::string(64,'C');});
    reject("parent empty",[&](auto& r){r.parent_sha256=std::string();});
    reject("unknown orientation",[&](auto& r){r.orientation=static_cast<MountOrientation>(9);});
    reject("unknown seam origin",[&](auto& r){r.seam_origin=static_cast<FixedSeamOrigin>(9);});
    CHECK(calls==0);
    // The unchanged request is fine, so the cases above fail for their own reason.
    CHECK(Throws([&]{auto r=Request(draft);r.seam_column=100;(void)MakeFixedCompositeConditionV2(r,kCreated,kHash,never);}));
    (void)MakeFixedCompositeConditionV2(Request(draft),kCreated,kHash,never);
    CHECK(calls==1);
}

void ParseRejects(){
    const auto good=FormatFixedCompositeConditionV2(Make(draft));
    const auto bad=[&](const std::string& text,const char* what){Check(Throws([&]{(void)ParseFixedCompositeConditionV2(text);}),what);};
    bad(Replace(good,"unapproved","approved"),"approved");
    bad(Replace(good,"\"approval\":\"unapproved\"","\"approval\":true"),"approval is not a string");
    bad(Replace(good,"\"approval\":\"unapproved\",",""),"approval missing");
    bad(Replace(good,"fixed-composite-condition.v2","fixed-composite-condition.v1"),"v1 schema");
    bad(Replace(good,"fixed-composite-condition.v2","fixed-composite-condition.v3"),"future schema");
    bad(Replace(good,"\"schema\":\"a0.fixed-composite-condition.v2\"","\"schema\":2"),"schema not a string");
    bad(Replace(good,"{\"schema\"","{\"extra\":1,\"schema\""),"unknown member");
    bad(Replace(good,"\"seam\":{\"column\":50,","\"seam\":{\"extra\":1,\"column\":50,"),"unknown nested member");
    bad(Replace(good,"\"seam\":{\"column\":50,\"origin\":\"band-center-provisional\"}","\"seam\":{\"column\":50}"),"missing nested member");
    bad(Replace(good,"\"revision\":{\"parentSha256\":null},",""),"revision missing");
    bad(Replace(good,"\"revision\":{\"parentSha256\":null}","\"revision\":{}"),"parentSha256 missing");
    bad(Replace(good,"\"parentSha256\":null","\"parentSha256\":1"),"parentSha256 a number");
    bad(Replace(good,"\"parentSha256\":null","\"parentSha256\":\"\""),"parentSha256 empty");
    bad(Replace(good,"\"parentSha256\":null","\"parentSha256\":\""+std::string(64,'A')+"\""),"parentSha256 upper case");
    bad(Replace(good,"\"parentSha256\":null","\"parentSha256\":\""+std::string(65,'a')+"\""),"parentSha256 too long");
    bad(Replace(good,"\"parentSha256\":null","\"parentSha256\":null,\"parentSha256\":null"),"duplicate key");
    bad(Replace(good,"\"createdUtc\":\"2026-11-04T12:34:56Z\"","\"createdUtc\":\"2026-02-30T12:34:56Z\""),"impossible date");
    bad(Replace(good,"\"createdUtc\":\"2026-11-04T12:34:56Z\"","\"createdUtc\":\"2026-11-04T12:34:56.5Z\""),"fractional seconds");
    bad(Replace(good,"\"mountOrientation\":\"normal\"","\"mountOrientation\":\"rotated-90\""),"unknown orientation");
    bad(Replace(good,"\"mountOrientation\":\"normal\"","\"mountOrientation\":3"),"orientation not a string");
    bad(Replace(good,"\"origin\":\"band-center-provisional\"","\"origin\":\"auto\""),"unknown seam origin");
    for(const char* name:{"a/b.json","..\\b.json","..\\..\\b.json","C:\\b.json","b.txt","","con.json"})
        bad(Replace(good,"wpd-fixed-composite-profile-test.json",name),"bad profile file name in the record");
    bad(Replace(good,"\"profileFile\":\"wpd-fixed-composite-profile-test.json\"","\"profileFile\":null"),"profile file null");
    bad(Replace(good,"\"profileFileSha256\":\"","\"profileFileSha256\":\"A"),"profile hash upper case / long");
    bad(Replace(good,"\"profileFingerprintSha256\":\"","\"profileFingerprintSha256\":\"g"),"fingerprint hash not hex");
    bad(Replace(good,"\"dpi\":254","\"dpi\":254.0"),"dpi with a fraction");
    bad(Replace(good,"\"dpi\":254","\"dpi\":2.54e2"),"dpi in exponent form");
    bad(Replace(good,"\"dpi\":254","\"dpi\":\"254\""),"dpi a string");
    bad(Replace(good,"\"dpi\":254","\"dpi\":0"),"dpi 0");
    bad(Replace(good,"\"dpi\":254","\"dpi\":65536"),"dpi too large");
    bad(Replace(good,"\"widthPixels\":100","\"widthPixels\":101"),"width disagrees with micrometres and DPI");
    bad(Replace(good,"\"heightPixels\":80","\"heightPixels\":8e1"),"height in exponent form");
    bad(Replace(good,"\"widthPixels\":100","\"widthPixels\":0"),"width 0");
    bad(Replace(good,"\"widthPixels\":100","\"widthPixels\":32769"),"width over the image limit");
    bad(Replace(good,"\"left\":100000","\"left\":-0"),"minus zero");
    bad(Replace(good,"\"left\":100000","\"left\":-100000"),"negative region");
    bad(Replace(good,"\"left\":100000","\"left\":1e5"),"region in exponent form");
    bad(Replace(good,"\"left\":100000","\"left\":100000.5"),"region with a fraction");
    bad(Replace(good,"\"left\":100000","\"left\":99999999999999999999"),"region overflows");
    bad(Replace(good,"\"left\":100000","\"left\":1000000001"),"region beyond the range");
    bad(Replace(good,"\"right\":110000","\"right\":100000"),"empty region");
    bad(Replace(good,"\"regionUm\":{","\"regionMm\":{"),"region renamed");
    bad(Replace(good,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":-1"),"negative limit");
    bad(Replace(good,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":NaN"),"limit NaN");
    bad(Replace(good,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":1e999"),"limit overflows");
    bad(Replace(good,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":null"),"limit null");
    bad(Replace(good,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":{\"value\":1}"),"limit an object");
    bad(Replace(good,"\"column\":50","\"column\":100"),"seam at the width");
    bad(Replace(good,"\"column\":50","\"column\":-1"),"seam negative");
    bad(Replace(good,"\"column\":50","\"column\":50.5"),"seam fractional");
    bad(good+"x","trailing characters");
    bad(good+" {}","trailing value");
    bad("","empty");
    bad("[]","array");
    bad("null","null");
    bad(std::string(kMaximumFixedCompositeConditionV2Bytes+1,' '),"oversize");
    bad(good.substr(0,good.size()-1),"truncated");
    // Accepted forms.
    CHECK(Throws([&]{(void)ParseFixedCompositeConditionV2(Replace(good,"\"dpi\":254","\"dpi\":254.0"));}));
    CHECK(ParseFixedCompositeConditionV2(std::string("\xEF\xBB\xBF")+good).seam.column==50);
    CHECK(ParseFixedCompositeConditionV2(" \n"+good+"\n").seam.column==50);
    CHECK(ParseFixedCompositeConditionV2(Replace(good,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":0")).scale_auto_correction_max_percent==0);
    CHECK(ParseFixedCompositeConditionV2(Replace(good,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":2.5e0")).scale_auto_correction_max_percent==2.5);
    CHECK(ParseFixedCompositeConditionV2(Replace(good,"\"column\":50","\"column\":99")).seam.column==99);
    // The v1 reader still refuses a v2 record, and the v2 reader a v1 one.
    REJECT(ParseFixedCompositeCondition(good));
    // The schema probe.
    CHECK(FixedCompositeConditionSchemaOf("{\"schema\":\"x\"}")=="x");
    CHECK(FixedCompositeConditionSchemaOf(std::string("\xEF\xBB\xBF")+"{\"schema\":\"a0.fixed-composite-condition.v1\"}")==kFixedCompositeConditionSchema);
    REJECT(FixedCompositeConditionSchemaOf("{}"));
    REJECT(FixedCompositeConditionSchemaOf("{\"schema\":1}"));
    REJECT(FixedCompositeConditionSchemaOf("[\"schema\"]"));
    REJECT(FixedCompositeConditionSchemaOf("{\"schema\":\"a\",\"schema\":\"b\"}"));
    REJECT(FixedCompositeConditionSchemaOf(""));
    // Validation of a record built by hand.
    auto c=ParseFixedCompositeConditionV2(good);
    ValidateFixedCompositeConditionV2(c);
    auto edited=c;edited.source.profile_file="x/y.json";REJECT(ValidateFixedCompositeConditionV2(edited));
    edited=c;edited.created_utc="2026-11-04";REJECT(FormatFixedCompositeConditionV2(edited));
    edited=c;edited.scale_auto_correction_max_percent=std::numeric_limits<double>::quiet_NaN();REJECT(FormatFixedCompositeConditionV2(edited));
    edited=c;edited.raster.dpi=200;REJECT(FormatFixedCompositeConditionV2(edited));
    edited=c;edited.seam.column=c.raster.width_pixels;REJECT(FormatFixedCompositeConditionV2(edited));
    CHECK(Throws([&]{FixedCompositeConditionV2 unset;(void)FormatFixedCompositeConditionV2(unset);}));
}

void Binding(){
    const auto c=Make(draft);
    const auto profile_ok=BindFixedCompositeProfile(c,draft,kHash);
    CHECK(profile_ok.parameters.layout==a0::m2::StitchLayout::camera_a_left_camera_b_right && profile_ok.parameters.raster.dpi==254);
    // Same profile, different bytes: the fingerprint ignores white space and
    // member order, the file hash does not.
    auto spaced=draft;spaced.insert(spaced.find('{')+1," ");
    CHECK(a0::m2::RigProfileV2::Parse(spaced).FingerprintSha256()==c.source.profile_fingerprint_sha256);
    REJECT(BindFixedCompositeProfile(c,spaced,kHash));
    CHECK(Throws([&]{(void)BindFixedCompositeProfile(c,draft+"\n",kHash);}));
    // Right file hash, wrong fingerprint.
    auto wrong_fingerprint=c;wrong_fingerprint.source.profile_fingerprint_sha256=std::string(64,'d');
    REJECT(BindFixedCompositeProfile(wrong_fingerprint,draft,kHash));
    // Another profile with another fingerprint, recorded consistently with its own bytes: fine,
    // but a profile changed in one coefficient while the record keeps the old hashes is not.
    const auto changed=Replace(draft,"\"k1\": 0.0","\"k1\": 0.0001");
    CHECK(changed!=draft);
    REJECT(BindFixedCompositeProfile(c,changed,kHash));
    auto consistent=c;consistent.source.profile_file_sha256=HashOf(changed);
    REJECT(BindFixedCompositeProfile(consistent,changed,kHash)); // fingerprint still the old one
    consistent.source.profile_fingerprint_sha256=a0::m2::RigProfileV2::Parse(changed).FingerprintSha256();
    CHECK(BindFixedCompositeProfile(consistent,changed,kHash).profile_fingerprint_sha256==consistent.source.profile_fingerprint_sha256);
    // An approved profile or a template never binds, whatever the record says.
    for(const auto* bytes:{&approved,&blank}){
        auto forged=c;forged.source.profile_file_sha256=HashOf(*bytes);
        forged.source.profile_fingerprint_sha256=a0::m2::RigProfileV2::Parse(*bytes).FingerprintSha256();
        REJECT(BindFixedCompositeProfile(forged,*bytes,kHash));
    }
    // Bytes that are not a profile are not parsed past the hash.
    auto garbage=c;garbage.source.profile_file_sha256=HashOf("not json");
    REJECT(BindFixedCompositeProfile(garbage,"not json",kHash));
    REJECT(BindFixedCompositeProfile(c,draft,FixedCompositeSha256{}));
    REJECT(BindFixedCompositeProfile(c,std::string(kMaximumFixedCompositeProfileBytes+1,' '),kHash));
    // The camera order and the raster are read from the profile and the record.
    const auto top_bottom=Replace(draft,"CAM-A-left-CAM-B-right","CAM-A-top-CAM-B-bottom");
    auto vertical=c;vertical.source.profile_file_sha256=HashOf(top_bottom);
    vertical.source.profile_fingerprint_sha256=a0::m2::RigProfileV2::Parse(top_bottom).FingerprintSha256();
    REJECT(BindFixedCompositeProfile(vertical,top_bottom,kHash));
    auto outside=c;outside.raster=render::OutputRaster{render::DocumentRegionUm{100000,100000,1200000,108000},254,11000,80};
    REJECT(BindFixedCompositeProfile(outside,draft,kHash));
    // The profile text is not kept or changed by binding.
    CHECK(a0::m2::RigProfileV2::Parse(draft).FingerprintSha256()==c.source.profile_fingerprint_sha256);
}

void Files(const fs::path& fixtures){
    (void)fixtures;
    const auto root=TempParent()/("a0-fixed-composite-v2-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto file=root/kFixedCompositeConditionName;
    const auto partial=fs::path(file.native()+L".partial");
    const auto profile_file=root/"wpd-fixed-composite-profile-test.json";
    // Absent: no condition, not an error. (Same answer the v1 loader gives.)
    CHECK(!LoadFixedCompositeConditionAny(file,kHash));
    CHECK(!LoadFixedCompositeCondition(file,kHash));
    const auto condition=Make(draft);
    WriteFixedCompositeConditionV2(file,condition);
    CHECK(fs::exists(file) && !fs::exists(partial));
    const auto bytes=ReadAll(file);
    CHECK(bytes==FormatFixedCompositeConditionV2(condition)+"\n");
    // The profile is not beside it yet: not usable, and not mistaken for absence.
    REJECT(LoadFixedCompositeConditionAny(file,kHash));
    WriteAll(profile_file,draft);
    const auto loaded=LoadFixedCompositeConditionAny(file,kHash);
    CHECK(loaded && std::holds_alternative<LoadedFixedCompositeConditionV2>(*loaded));
    if(loaded && std::holds_alternative<LoadedFixedCompositeConditionV2>(*loaded)){
        const auto& v2=std::get<LoadedFixedCompositeConditionV2>(*loaded);
        CHECK(SameCondition(v2.condition,condition) && v2.file_sha256==HashOf(bytes));
        CHECK(v2.profile.profile_file_sha256==HashOf(draft) && v2.profile.profile_fingerprint_sha256==condition.source.profile_fingerprint_sha256);
        CHECK(v2.profile.parameters.layout==a0::m2::StitchLayout::camera_a_left_camera_b_right && v2.profile.parameters.raster.width_pixels==100);
        CHECK(v2.profile.profile_id && *v2.profile.profile_id=="synthetic-fingerprint-draft");
    }
    // The v1 loader refuses a v2 file: code that only knows v1 fails closed.
    REJECT(LoadFixedCompositeCondition(file,kHash));
    // Never overwrite, even for an identical record; the bytes survive.
    REJECT(WriteFixedCompositeConditionV2(file,condition));
    auto other=condition;other.seam.column=51;other.seam.origin=FixedSeamOrigin::operator_set;
    REJECT(WriteFixedCompositeConditionV2(file,other));
    CHECK(ReadAll(file)==bytes && !fs::exists(partial));
    // A partial that is already there is not reused or removed.
    const auto second=root/"second.json";const auto second_partial=fs::path(second.native()+L".partial");
    WriteAll(second_partial,"someone else");
    REJECT(WriteFixedCompositeConditionV2(second,condition));
    CHECK(ReadAll(second_partial)=="someone else" && !fs::exists(second));
    fs::remove(second_partial);
    WriteFixedCompositeConditionV2(second,other);
    {
        const auto again=LoadFixedCompositeConditionAny(second,kHash);
        CHECK(again && std::get<LoadedFixedCompositeConditionV2>(*again).condition.seam.origin==FixedSeamOrigin::operator_set);
    }
    // An invalid record writes nothing.
    auto invalid=condition;invalid.source.profile_file="bad/name.json";
    const auto third=root/"third.json";
    REJECT(WriteFixedCompositeConditionV2(third,invalid));
    CHECK(!fs::exists(third) && !fs::exists(fs::path(third.native()+L".partial")));
    // A missing folder is not created; a directory in the way is not replaced.
    REJECT(WriteFixedCompositeConditionV2(root/"missing-folder"/"x.json",condition));
    CHECK(!fs::exists(root/"missing-folder"));
    fs::create_directories(root/"as-directory.json");
    REJECT(WriteFixedCompositeConditionV2(root/"as-directory.json",condition));
    REJECT(LoadFixedCompositeConditionAny(root/"as-directory.json",kHash));
    // Damaged and oversize condition files fail; they are not read as absent.
    const auto damaged=root/"damaged.json";
    WriteAll(damaged,"{\"schema\":\"a0.fixed-composite-condition.v2\"}");REJECT(LoadFixedCompositeConditionAny(damaged,kHash));
    WriteAll(damaged,"{\"schema\":\"a0.fixed-composite-condition.v3\"}");REJECT(LoadFixedCompositeConditionAny(damaged,kHash));
    WriteAll(damaged,"{}");REJECT(LoadFixedCompositeConditionAny(damaged,kHash));
    WriteAll(damaged,"");REJECT(LoadFixedCompositeConditionAny(damaged,kHash));
    WriteAll(damaged,std::string(kMaximumFixedCompositeConditionV2Bytes+1,' '));REJECT(LoadFixedCompositeConditionAny(damaged,kHash));
    WriteAll(damaged,Replace(bytes,"unapproved","approved"));REJECT(LoadFixedCompositeConditionAny(damaged,kHash));
    WriteAll(damaged,bytes+"x");REJECT(LoadFixedCompositeConditionAny(damaged,kHash));
    WriteAll(damaged,std::string("\xEF\xBB\xBF")+bytes);CHECK(LoadFixedCompositeConditionAny(damaged,kHash).has_value());
    REJECT(LoadFixedCompositeConditionAny(damaged,FixedCompositeSha256{}));
    // A name that climbs out of the folder is refused when the record is read,
    // even if a file with the right bytes sits there.
    fs::create_directories(root/"inner");
    WriteAll(root/"outside.json",draft);
    WriteAll(root/"inner"/kFixedCompositeConditionName,Replace(bytes,"wpd-fixed-composite-profile-test.json","..\\outside.json"));
    REJECT(LoadFixedCompositeConditionAny(root/"inner"/kFixedCompositeConditionName,kHash));
    WriteAll(root/"inner"/kFixedCompositeConditionName,Replace(bytes,"wpd-fixed-composite-profile-test.json","../outside.json"));
    REJECT(LoadFixedCompositeConditionAny(root/"inner"/kFixedCompositeConditionName,kHash));
    // The profile beside the condition is checked on every load.
    const auto reload=[&]{return Throws([&]{(void)LoadFixedCompositeConditionAny(file,kHash);});};
    WriteAll(profile_file," "+draft);CHECK(reload());                                       // same fingerprint, other bytes
    WriteAll(profile_file,Replace(draft,"\"k1\": 0.0","\"k1\": 0.0001"));CHECK(reload());   // other coefficient
    WriteAll(profile_file,approved);CHECK(reload());                                        // an approved profile
    WriteAll(profile_file,Replace(draft,"CAM-A-left-CAM-B-right","CAM-A-top-CAM-B-bottom"));CHECK(reload());
    WriteAll(profile_file,"");CHECK(reload());
    WriteAll(profile_file,std::string(kMaximumFixedCompositeProfileBytes+1,' '));CHECK(reload());
    fs::remove(profile_file);fs::create_directories(profile_file);CHECK(reload());          // a folder in its place
    fs::remove(profile_file);
    WriteAll(profile_file,draft);CHECK(!reload());                                          // restored: usable again
    // A v1 record next to its own folder still loads as v1, through the same entrance.
    {
        const auto v1_root=root/"v1";fs::create_directories(v1_root);
        const auto v1_file=v1_root/kFixedCompositeConditionName;
        SavedStillReview review;
        review.report_sha256=std::string(64,'a');review.pair_id="pair-1";review.source_width=400;review.source_height=300;
        review.translation_x=200;review.translation_y=6;review.rotation_degrees=0;review.scale=1;
        const auto p=SimilarityReviewGeometry(200,6,0,1,400,300);
        review.crop=p.crop;const auto plan=render::PlanPairCanvas(400,300,400,300,p);review.width=plan.output_width;review.height=plan.output_height;
        const auto v1=MakeFixedCompositeCondition(review,kCreated,1);
        WriteFixedCompositeCondition(v1_file,v1);
        const auto v1_bytes=ReadAll(v1_file);
        const auto any=LoadFixedCompositeConditionAny(v1_file,kHash);
        const auto direct=LoadFixedCompositeCondition(v1_file,kHash);
        CHECK(any && direct && std::holds_alternative<LoadedFixedCompositeCondition>(*any));
        if(any && direct && std::holds_alternative<LoadedFixedCompositeCondition>(*any)){
            const auto& got=std::get<LoadedFixedCompositeCondition>(*any);
            CHECK(FormatFixedCompositeCondition(got.condition)==FormatFixedCompositeCondition(direct->condition) && got.file_sha256==direct->file_sha256 && got.file_sha256==HashOf(v1_bytes));
        }
        // A v2 record cannot be written over a v1 file: a person moves the v1 file first.
        REJECT(WriteFixedCompositeConditionV2(v1_file,condition));
        CHECK(ReadAll(v1_file)==v1_bytes);
        // A damaged v1 file fails through the same entrance, as it does through the v1 loader.
        WriteAll(damaged,Replace(v1_bytes,"unapproved","approved"));REJECT(LoadFixedCompositeConditionAny(damaged,kHash));
        WriteAll(damaged,Replace(v1_bytes,"\"scale\":1","\"scale\":1,\"scale\":1"));REJECT(LoadFixedCompositeConditionAny(damaged,kHash));
    }
    // A move away and rewrite is the switch from v1 to v2.
    {
        const auto swap=root/"swap";fs::create_directories(swap);
        fs::copy_file(profile_file,swap/"wpd-fixed-composite-profile-test.json");
        WriteFixedCompositeConditionV2(swap/kFixedCompositeConditionName,condition);
        CHECK(std::holds_alternative<LoadedFixedCompositeConditionV2>(*LoadFixedCompositeConditionAny(swap/kFixedCompositeConditionName,kHash)));
    }
    std::error_code ignored;fs::remove_all(root,ignored);
}
}

int main(int argc,char* argv[]){
    try{
        if(argc!=2)throw std::invalid_argument("expected the shared rig profile v2 fixture folder");
        const fs::path fixtures=argv[1];
        draft=ReadAll(fixtures/"calibrated-draft.json");approved=ReadAll(fixtures/"approved.json");blank=ReadAll(fixtures/"template.json");
        if(draft.empty() || approved.empty() || blank.empty())throw std::runtime_error("fixture profiles are missing");
        FileNames();MakeAndFormat();MakeRejects();ParseRejects();Binding();Files(fixtures);
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
