// Fixed composite condition v3 record (#417, docs/design/fixed-composite-registration.md 11.9, 11.19, 11.20, 11.27): format, strict
// reader, the revision chain on disk, the new-file writer, the reference to a draft rig profile, creation and the version-aware
// entrance. Camera free; files go to a temporary folder only. Argument 1 is the shared rig profile v2 fixture folder.
// Every range, line and limit below is a value of this test; the product code has no default for any of them.
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/fixed_composite_selection.hpp"
#include "a0/m2/experimental/parallax_correction.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "registration_fixtures.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <regex>
#include <sstream>
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
content_registration::RegistrationLimits Limits(){
    content_registration::RegistrationLimits l;
    l.brown_lowe_alpha=8;l.brown_lowe_beta=0.3;l.minimum_unique_blocks=3;l.maximum_prediction_sigma_px=0.5;l.maximum_residual_rms_px=0.75;
    l.maximum_residual_p95_px=1.5;l.maximum_bin_bias_px=0.4;l.convergence_px=0.05;l.start_agreement_px=0.25;
    return l;
}
FixedCompositeV3Request Request(const std::string& profile){
    FixedCompositeV3Request r;
    r.profile_file="wpd-fixed-composite-profile-test.json";r.profile_bytes=profile;r.policy_sha256=std::string(64,'d');
    r.sequence=1;r.change.kind=FixedCompositeRevisionKind::created;
    r.raster=Raster();r.seam_column=50;r.seam_origin=FixedSeamOrigin::band_center_provisional;
    r.registration.envelope.position_mm=5;r.registration.envelope.rotation_deg=1.5;r.registration.envelope.tilt_deg=2.5;r.registration.envelope.height_percent=3;
    r.registration.limits=Limits();
    r.registration.geometry={2,72,96};
    r.registration.surface_envelope=SurfaceEnvelope{40,8};
    r.registration.apply_beyond_envelope=true;
    r.registration.surface_scale_warn_percent=std::nullopt;   // the owner's value: no warning about the scale of the drawing
    r.registration.parallax=FixedCompositeConditionV3::Registration::Parallax{1.0};
    r.registration.live=std::nullopt;
    r.seam_gate={1.0,8.0};
    r.revision_rules.manual_adjustment_max_seam_px=std::nullopt;   // the owner's value: no upper bound
    return r;
}
// The creation check of the entrance is the lane's second part; this one accepts and records what it was given.
struct Spy {
    int calls=0;FixedCompositeConditionV3 condition;std::optional<FixedCompositeProfileBinding> binding;
    FixedCompositeCreationCheckV3 Check(){
        return [this](const FixedCompositeConditionV3& c,const FixedCompositeProfileBinding& b){++calls;condition=c;binding=b;};
    }
};
const FixedCompositeCreationCheckV3 kAccept=[](const FixedCompositeConditionV3&,const FixedCompositeProfileBinding&){};
FixedCompositeConditionV3 Make(const FixedCompositeV3Request& r){return MakeFixedCompositeConditionV3(r,kCreated,kHash,kAccept);}

std::string draft,approved,blank;

// A chain of revisions on disk: the revisions folder under a settings folder that holds the profile.
struct Chain {
    fs::path settings,revisions;
    std::vector<std::string> texts;
    std::vector<FixedCompositeConditionV3> records;
};
Chain MakeSettings(const std::string& name){
    Chain c;
    c.settings=TempParent()/("a0-fixed-composite-v3-"+name+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    c.revisions=c.settings/std::string(kFixedCompositeRevisionFolderName);
    fs::create_directories(c.revisions);
    WriteAll(c.settings/"wpd-fixed-composite-profile-test.json",draft);
    return c;
}
// Appends a revision: the parent hash is the SHA-256 of the previous file.
FixedCompositeConditionV3 Append(Chain& chain,FixedCompositeRevisionChangeV3 change,const std::function<void(FixedCompositeV3Request&)>& edit={}){
    auto request=Request(draft);
    if(!chain.records.empty()){
        request.registration=chain.records.back().registration;request.seam_gate=chain.records.back().seam_gate;
        request.revision_rules=chain.records.back().revision_rules;request.base_pose=chain.records.back().base_pose;
        request.policy_sha256=chain.records.back().source.policy_sha256;
    }
    request.sequence=static_cast<std::uint32_t>(chain.texts.size()+1);
    request.parent_sha256=chain.texts.empty()?std::nullopt:std::optional<std::string>(HashOf(chain.texts.back()));
    request.change=std::move(change);
    if(edit)edit(request);
    const auto condition=Make(request);
    WriteFixedCompositeConditionV3(chain.revisions,condition);
    chain.texts.push_back(ReadAll(chain.revisions/FixedCompositeRevisionFileName(condition.revision.sequence)));
    chain.records.push_back(condition);
    return condition;
}
FixedCompositeRevisionChangeV3 Change(FixedCompositeRevisionKind kind){FixedCompositeRevisionChangeV3 c;c.kind=kind;return c;}
FixedCompositeRevisionChangeV3 SavedEstimate(){
    auto c=Change(FixedCompositeRevisionKind::saved_estimate);c.report_sha256=std::string(64,'e');c.pair_id="pair-1";return c;
}
FixedCompositeRevisionChangeV3 Manual(double dx,double dy,double rz,double max){
    auto c=Change(FixedCompositeRevisionKind::manual_adjustment);
    c.entered=FixedCompositeRevisionChangeV3::Entered{dx,dy,rz};c.maximum_seam_displacement_px=max;return c;
}
FixedCompositeRevisionChangeV3 Rollback(const std::string& restored){auto c=Change(FixedCompositeRevisionKind::rollback);c.restored_sha256=restored;return c;}

void Names(){
    CHECK(FixedCompositeRevisionFileName(1)=="r000001.json" && FixedCompositeRevisionFileName(123456)=="r123456.json" && FixedCompositeRevisionFileName(999999)=="r999999.json");
    REJECT(FixedCompositeRevisionFileName(0));
    REJECT(FixedCompositeRevisionFileName(kMaximumFixedCompositeRevisionSequence+1));
    CHECK(ParseFixedCompositeRevisionFileName("r000001.json")==1u && ParseFixedCompositeRevisionFileName("r000042.json")==42u && ParseFixedCompositeRevisionFileName("r999999.json")==999999u);
    for(const char* bad:{"","r1.json","r0000001.json","r00001.json","r000000.json","R000001.json","r000001.JSON","r000001.json.partial","r00000a.json","x000001.json",
        "r000001.jsn","r-00001.json","r+00001.json","r 00001.json","wpd-fixed-composite-condition.json"})
        Check(!ParseFixedCompositeRevisionFileName(bad),("not a revision name: "+std::string(bad)).c_str());
    CHECK(std::string(kFixedCompositeConditionSchemaV3)=="a0.fixed-composite-condition.v3");
    CHECK(kMaximumFixedCompositeConditionV3Bytes>kMaximumFixedCompositeConditionV2Bytes);
    CHECK(FixedCompositeRevisionKindText(FixedCompositeRevisionKind::migrated_from_v2)=="migrated-from-v2");
    for(const auto k:{FixedCompositeRevisionKind::created,FixedCompositeRevisionKind::migrated_from_v2,FixedCompositeRevisionKind::saved_estimate,
        FixedCompositeRevisionKind::manual_adjustment,FixedCompositeRevisionKind::rollback,FixedCompositeRevisionKind::values})
        CHECK(ParseFixedCompositeRevisionKind(FixedCompositeRevisionKindText(k))==k);
    CHECK(!ParseFixedCompositeRevisionKind("Created") && !ParseFixedCompositeRevisionKind("") && !ParseFixedCompositeRevisionKind("manual_adjustment"));
}

// Numbers are written with 17 significant digits (the document's section 2), as the standard library prints them.
std::string N(double v){std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<(v==0?0.0:v);return out.str();}
std::string CanonicalTail(){
    return "\"registration\":{\"envelope\":{\"positionMm\":"+N(5)+",\"rotationDeg\":"+N(1.5)+",\"tiltDeg\":"+N(2.5)+",\"heightPercent\":"+N(3)+"},"
    "\"limits\":{\"brownLoweAlpha\":"+N(8)+",\"brownLoweBeta\":"+N(0.3)+",\"minimumUniqueBlocks\":3,\"maximumPredictionSigmaPx\":"+N(0.5)+
    ",\"maximumResidualRmsPx\":"+N(0.75)+",\"maximumResidualP95Px\":"+N(1.5)+",\"maximumBinBiasPx\":"+N(0.4)+",\"convergencePx\":"+N(0.05)+
    ",\"startAgreementPx\":"+N(0.25)+"},"
    "\"geometry\":{\"omegaMarginPx\":2,\"guardPx\":72,\"bandHalfWidthPx\":96},\"surfaceEnvelope\":{\"heightMm\":"+N(40)+",\"tiltDeg\":"+N(8)+"},"
    "\"applyBeyondEnvelope\":true,\"surfaceScaleWarnPercent\":null,\"parallax\":{\"maxLiftMm\":"+N(1)+"},\"live\":null},"
    "\"seamGate\":{\"limitPx\":"+N(1)+",\"guardBandPx\":"+N(8)+"},\"revisionRules\":{\"manualAdjustmentMaxSeamPx\":null}}";
}

void MakeAndFormat(){
    Spy spy;
    const auto request=Request(draft);
    const auto c=MakeFixedCompositeConditionV3(request,kCreated,kHash,spy.Check());
    const auto profile=a0::m2::RigProfileV2::Parse(draft);
    CHECK(c.created_utc=="2026-11-04T12:34:56Z");
    CHECK(c.revision.sequence==1 && !c.revision.parent_sha256 && c.revision.change.kind==FixedCompositeRevisionKind::created);
    CHECK(c.source.profile_file=="wpd-fixed-composite-profile-test.json" && c.source.profile_file_sha256==HashOf(draft) && c.source.policy_sha256==std::string(64,'d'));
    CHECK(c.source.profile_fingerprint_sha256==profile.FingerprintSha256());
    CHECK(c.base_pose.IsZero());
    // The creation check ran once, on the finished record and the verified profile.
    CHECK(spy.calls==1 && spy.binding && spy.condition.source.profile_file_sha256==c.source.profile_file_sha256);
    CHECK(spy.binding->parameters.layout==a0::m2::StitchLayout::camera_a_left_camera_b_right && spy.binding->parameters.raster.width_pixels==100);
    CHECK(spy.binding->profile_id && *spy.binding->profile_id=="synthetic-fingerprint-draft");
    // Canonical text, exactly.
    const auto text=FormatFixedCompositeConditionV3(c);
    const auto expected=std::string("{\"schema\":\"a0.fixed-composite-condition.v3\",\"approval\":\"unapproved\",\"createdUtc\":\"2026-11-04T12:34:56Z\","
        "\"revision\":{\"sequence\":1,\"parentSha256\":null,\"change\":{\"kind\":\"created\"}},\"source\":{\"profileFile\":\"wpd-fixed-composite-profile-test.json\","
        "\"profileFileSha256\":\"")+c.source.profile_file_sha256+"\",\"profileFingerprintSha256\":\""+c.source.profile_fingerprint_sha256+"\",\"policySha256\":\""+
        std::string(64,'d')+"\"},\"mountOrientation\":\"normal\",\"outputRaster\":{\"regionUm\":{\"left\":100000,\"top\":100000,\"right\":110000,\"bottom\":108000},"
        "\"dpi\":254,\"widthPixels\":100,\"heightPixels\":80},\"seam\":{\"column\":50,\"origin\":\"band-center-provisional\"},"
        "\"basePose\":{\"rxDeg\":0,\"ryDeg\":0,\"rzDeg\":0,\"txMm\":0,\"tyMm\":0,\"tzPercent\":0},"+CanonicalTail();
    CHECK(text==expected);
    CHECK(text.size()<kMaximumFixedCompositeConditionV3Bytes);
    // The strict reader gives the canonical text back, byte for byte.
    CHECK(FormatFixedCompositeConditionV3(ParseFixedCompositeConditionV3(text))==text);
    CHECK(FixedCompositeConditionSchemaOf(text.substr(0,0)+"{\"schema\":\"a0.fixed-composite-condition.v3\"}")=="a0.fixed-composite-condition.v3");
    // v2's scale limit is not carried (design 11.20.2).
    CHECK(text.find("scaleAutoCorrection")==std::string::npos);
    // The record is a value: nothing in it points back into the request.
    CHECK(c.registration.apply_beyond_envelope==true && !c.registration.surface_scale_warn_percent && !c.revision_rules.manual_adjustment_max_seam_px);

    // Every layer that has no range is written as null, and the record still round-trips.
    auto bare=Request(draft);
    bare.registration.surface_envelope=std::nullopt;bare.registration.parallax=std::nullopt;bare.registration.apply_beyond_envelope=false;
    bare.orientation=MountOrientation::rotated_180;bare.seam_origin=FixedSeamOrigin::operator_set;bare.seam_column=61;
    const auto bare_text=FormatFixedCompositeConditionV3(Make(bare));
    CHECK(bare_text.find("\"surfaceEnvelope\":null")!=std::string::npos && bare_text.find("\"parallax\":null")!=std::string::npos &&
        bare_text.find("\"applyBeyondEnvelope\":false")!=std::string::npos && bare_text.find("\"mountOrientation\":\"rotated-180\"")!=std::string::npos &&
        bare_text.find("\"origin\":\"operator\"")!=std::string::npos);
    CHECK(FormatFixedCompositeConditionV3(ParseFixedCompositeConditionV3(bare_text))==bare_text);
    {
        const auto parsed=ParseFixedCompositeConditionV3(bare_text);
        CHECK(!parsed.registration.surface_envelope && !parsed.registration.parallax && parsed.registration.apply_beyond_envelope==false);
    }
    // The optional numbers are real values when they are numbers: a zero warning line and a bound are not absences.
    auto numbers=Request(draft);
    numbers.registration.surface_scale_warn_percent=0.0;numbers.revision_rules.manual_adjustment_max_seam_px=4.5;
    numbers.registration.live=FixedCompositeConditionV3::Registration::Live{Limits()};
    numbers.registration.limits.brown_lowe_beta=0.0;    // the lower end of the accepted range
    const auto numbers_text=FormatFixedCompositeConditionV3(Make(numbers));
    CHECK(numbers_text.find("\"surfaceScaleWarnPercent\":0,")!=std::string::npos && numbers_text.find("\"manualAdjustmentMaxSeamPx\":4.5")!=std::string::npos &&
        numbers_text.find("\"live\":{\"limits\":{")!=std::string::npos);
    const auto numbers_back=ParseFixedCompositeConditionV3(numbers_text);
    CHECK(numbers_back.registration.surface_scale_warn_percent==0.0 && numbers_back.revision_rules.manual_adjustment_max_seam_px==4.5 && numbers_back.registration.live);
    CHECK(FormatFixedCompositeConditionV3(numbers_back)==numbers_text);
    // A negative zero is written as 0 and not read back as a different value.
    auto negative_zero=Request(draft);negative_zero.registration.surface_scale_warn_percent=-0.0;
    CHECK(FormatFixedCompositeConditionV3(Make(negative_zero)).find("\"surfaceScaleWarnPercent\":0,")!=std::string::npos);
    // Numbers keep every digit.
    auto precise=Request(draft);precise.registration.envelope.position_mm=0.1+0.2;precise.registration.limits.convergence_px=1.0/3.0;
    const auto precise_text=FormatFixedCompositeConditionV3(Make(precise));
    CHECK(ParseFixedCompositeConditionV3(precise_text).registration.envelope.position_mm==0.1+0.2 && ParseFixedCompositeConditionV3(precise_text).registration.limits.convergence_px==1.0/3.0);
    CHECK(FormatFixedCompositeConditionV3(ParseFixedCompositeConditionV3(precise_text))==precise_text);
}

void KindsRoundTrip(){
    auto chain=MakeSettings("kinds");
    Append(chain,Change(FixedCompositeRevisionKind::created));
    const auto estimate=Append(chain,SavedEstimate(),[](FixedCompositeV3Request& r){r.base_pose={0.5,-0.25,0.125,1.5,-1.25,0.5};});
    CHECK(estimate.revision.sequence==2 && estimate.revision.parent_sha256==HashOf(chain.texts[0]) && estimate.base_pose.tx_mm==1.5);
    const auto manual=Append(chain,Manual(0.5,-0.25,0.01,12.75),[](FixedCompositeV3Request& r){r.base_pose.tx_mm=1.75;});
    CHECK(manual.revision.change.entered && manual.revision.change.entered->seam_dx_px==0.5 && manual.revision.change.maximum_seam_displacement_px==12.75);
    const auto values=Append(chain,Change(FixedCompositeRevisionKind::values),[](FixedCompositeV3Request& r){r.policy_sha256=std::string(64,'f');r.registration.envelope.position_mm=6;});
    CHECK(values.source.policy_sha256==std::string(64,'f'));
    const auto rolled=Append(chain,Rollback(HashOf(chain.texts[1])),[&](FixedCompositeV3Request& r){
        r.registration=chain.records[1].registration;r.base_pose=chain.records[1].base_pose;r.policy_sha256=chain.records[1].source.policy_sha256;});
    CHECK(rolled.revision.change.restored_sha256==HashOf(chain.texts[1]));
    for(size_t i=0;i<chain.texts.size();++i){
        const auto parsed=ParseFixedCompositeConditionV3(chain.texts[i]);
        CHECK(FormatFixedCompositeConditionV3(parsed)+"\n"==chain.texts[i]);
        CHECK(parsed.revision.change.kind==chain.records[i].revision.change.kind);
    }
    // A manual adjustment of any size is recorded, never refused: the owner decided there is no upper bound.
    const auto huge=Append(chain,Manual(500,-400,3.5,640.5),[&](FixedCompositeV3Request& r){r.base_pose.tx_mm=2;});
    CHECK(huge.revision.change.maximum_seam_displacement_px==640.5);
    CHECK(ParseFixedCompositeConditionV3(chain.texts.back()).revision.change.maximum_seam_displacement_px==640.5);
    // ... and a bound that is a number is only a number in the record: it does not refuse an adjustment either way.
    const auto over=Append(chain,Manual(1,1,0,1000),[&](FixedCompositeV3Request& r){r.revision_rules.manual_adjustment_max_seam_px=2.0;r.base_pose.tx_mm=2.5;});
    CHECK(over.revision_rules.manual_adjustment_max_seam_px==2.0 && over.revision.change.maximum_seam_displacement_px==1000);
    CHECK(LoadActiveFixedCompositeRevisionV3(chain.revisions,kHash)->condition.revision.sequence==7);
    std::error_code ignored;fs::remove_all(chain.settings,ignored);
}

void MakeRejects(){
    int calls=0;
    REJECT(MakeFixedCompositeConditionV3(Request(draft),kCreated,kHash,FixedCompositeCreationCheckV3{}));
    REJECT(MakeFixedCompositeConditionV3(Request(draft),kCreated,FixedCompositeSha256{},kAccept));
    REJECT(MakeFixedCompositeConditionV3(Request(draft),kCreated,kHash,[&](const FixedCompositeConditionV3&,const FixedCompositeProfileBinding&){++calls;throw std::invalid_argument("seam outside the cover");}));
    CHECK(calls==1);
    const auto never=[&](const FixedCompositeConditionV3&,const FixedCompositeProfileBinding&){++calls;};
    calls=0;
    const auto reject=[&](const char* what,const std::function<void(FixedCompositeV3Request&)>& change){
        auto request=Request(draft);change(request);
        Check(Throws([&]{(void)MakeFixedCompositeConditionV3(request,kCreated,kHash,never);}),what);
    };
    const auto nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();
    reject("approved profile",[&](auto& r){r.profile_bytes=approved;});
    reject("template profile",[&](auto& r){r.profile_bytes=blank;});
    reject("profile bytes that are not a profile",[&](auto& r){r.profile_bytes="{}";});
    reject("empty profile bytes",[&](auto& r){r.profile_bytes.clear();});
    reject("oversize profile",[&](auto& r){r.profile_bytes=std::string(kMaximumFixedCompositeProfileBytes+1,' ');});
    reject("camera order top/bottom",[&](auto& r){r.profile_bytes=Replace(draft,"CAM-A-left-CAM-B-right","CAM-A-top-CAM-B-bottom");});
    reject("empty profile name",[&](auto& r){r.profile_file.clear();});
    reject("profile name with a folder",[&](auto& r){r.profile_file="sub/p.json";});
    reject("profile name that is the condition itself",[&](auto& r){r.profile_file="wpd-fixed-composite-condition.json";});
    reject("policy hash empty",[&](auto& r){r.policy_sha256.clear();});
    reject("policy hash upper case",[&](auto& r){r.policy_sha256=std::string(64,'D');});
    reject("raster width off by one",[&](auto& r){r.raster.width_pixels=101;});
    reject("raster unset",[&](auto& r){r.raster=render::OutputRaster{render::DocumentRegionUm{0,0,0,0},0,0,0};});
    reject("seam column at the width",[&](auto& r){r.seam_column=100;});
    reject("unknown orientation",[&](auto& r){r.orientation=static_cast<MountOrientation>(9);});
    reject("unknown seam origin",[&](auto& r){r.seam_origin=static_cast<FixedSeamOrigin>(9);});
    // Revision identity.
    reject("sequence 0",[&](auto& r){r.sequence=0;});
    reject("sequence beyond the largest",[&](auto& r){r.sequence=kMaximumFixedCompositeRevisionSequence+1;});
    reject("revision 1 with a parent",[&](auto& r){r.parent_sha256=kParent;});
    reject("revision 2 without a parent",[&](auto& r){r.sequence=2;r.change.kind=FixedCompositeRevisionKind::values;});
    reject("parent too short",[&](auto& r){r.sequence=2;r.change.kind=FixedCompositeRevisionKind::values;r.parent_sha256=std::string(63,'c');});
    reject("created as revision 2",[&](auto& r){r.sequence=2;r.parent_sha256=kParent;});
    reject("values as revision 1",[&](auto& r){r.change.kind=FixedCompositeRevisionKind::values;});
    reject("saved-estimate as revision 1",[&](auto& r){r.change=SavedEstimate();});
    reject("migrated-from-v2 without its hash",[&](auto& r){r.change.kind=FixedCompositeRevisionKind::migrated_from_v2;});
    reject("created with a stray member",[&](auto& r){r.change.v2_sha256=std::string(64,'a');});
    reject("created with a pose",[&](auto& r){r.base_pose.tx_mm=1;});
    reject("migrated with a pose",[&](auto& r){r.change.kind=FixedCompositeRevisionKind::migrated_from_v2;r.change.v2_sha256=std::string(64,'a');r.base_pose.rz_deg=0.5;});
    const auto later=[&](const char* what,const std::function<void(FixedCompositeRevisionChangeV3&)>& edit,FixedCompositeRevisionChangeV3 base){
        reject(what,[&](auto& r){r.sequence=2;r.parent_sha256=kParent;edit(base);r.change=base;});
    };
    later("saved-estimate without a report",[](auto& c){c.report_sha256.reset();},SavedEstimate());
    later("saved-estimate with a bad pair",[](auto& c){c.pair_id="bad pair!";},SavedEstimate());
    later("saved-estimate with a short report hash",[](auto& c){c.report_sha256=std::string(63,'e');},SavedEstimate());
    later("saved-estimate with a stray restored hash",[](auto& c){c.restored_sha256=std::string(64,'a');},SavedEstimate());
    later("manual adjustment without what was entered",[](auto& c){c.entered.reset();},Manual(1,1,0,1));
    later("manual adjustment without its measure",[](auto& c){c.maximum_seam_displacement_px.reset();},Manual(1,1,0,1));
    later("manual adjustment with a non-finite entry",[&](auto& c){c.entered->seam_dx_px=nan;},Manual(1,1,0,1));
    later("manual adjustment with an infinite rotation",[&](auto& c){c.entered->rotation_deg=inf;},Manual(1,1,0,1));
    later("manual adjustment with a negative measure",[](auto& c){c.maximum_seam_displacement_px=-0.5;},Manual(1,1,0,1));
    later("manual adjustment with an unset entry",[](auto& c){c.entered=FixedCompositeRevisionChangeV3::Entered{};},Manual(1,1,0,1));
    later("rollback without its hash",[](auto& c){c.restored_sha256.reset();},Rollback(std::string(64,'a')));
    later("rollback with a bad hash",[](auto& c){c.restored_sha256=std::string(64,'A');},Rollback(std::string(64,'a')));
    later("values with a stray report",[](auto& c){c.report_sha256=std::string(64,'e');},Change(FixedCompositeRevisionKind::values));
    later("created after the first revision",[](auto&){},Change(FixedCompositeRevisionKind::created));
    // Base pose.
    reject("base pose not finite",[&](auto& r){r.sequence=2;r.parent_sha256=kParent;r.change=SavedEstimate();r.base_pose.rx_deg=nan;});
    reject("base pose outside the position range",[&](auto& r){r.sequence=2;r.parent_sha256=kParent;r.change=SavedEstimate();r.base_pose.tx_mm=3;r.base_pose.ty_mm=4.5;});
    reject("base pose outside the rotation range",[&](auto& r){r.sequence=2;r.parent_sha256=kParent;r.change=SavedEstimate();r.base_pose.rz_deg=-1.6;});
    reject("base pose outside the tilt range",[&](auto& r){r.sequence=2;r.parent_sha256=kParent;r.change=SavedEstimate();r.base_pose.rx_deg=2;r.base_pose.ry_deg=2;});
    reject("base pose outside the height range",[&](auto& r){r.sequence=2;r.parent_sha256=kParent;r.change=SavedEstimate();r.base_pose.tz_percent=3.5;});
    // Ranges, lines and limits: none has a default in code, so an unset one is refused.
    reject("envelope unset",[&](auto& r){r.registration.envelope=content_registration::RegistrationEnvelope{};});
    reject("envelope position 0",[&](auto& r){r.registration.envelope.position_mm=0;});
    reject("envelope rotation negative",[&](auto& r){r.registration.envelope.rotation_deg=-1;});
    reject("envelope tilt infinite",[&](auto& r){r.registration.envelope.tilt_deg=inf;});
    reject("envelope height NaN",[&](auto& r){r.registration.envelope.height_percent=nan;});
    reject("limits unset",[&](auto& r){r.registration.limits=content_registration::RegistrationLimits{};});
    reject("Brown and Lowe beta 1",[&](auto& r){r.registration.limits.brown_lowe_beta=1;});
    reject("Brown and Lowe alpha negative",[&](auto& r){r.registration.limits.brown_lowe_alpha=-1;});
    reject("no unique block needed",[&](auto& r){r.registration.limits.minimum_unique_blocks=0;});
    reject("prediction sigma 0",[&](auto& r){r.registration.limits.maximum_prediction_sigma_px=0;});
    reject("residual rms NaN",[&](auto& r){r.registration.limits.maximum_residual_rms_px=nan;});
    reject("residual p95 negative",[&](auto& r){r.registration.limits.maximum_residual_p95_px=-1;});
    reject("bin bias infinite",[&](auto& r){r.registration.limits.maximum_bin_bias_px=inf;});
    reject("convergence 0",[&](auto& r){r.registration.limits.convergence_px=0;});
    reject("start agreement NaN",[&](auto& r){r.registration.limits.start_agreement_px=nan;});
    reject("geometry unset",[&](auto& r){r.registration.geometry={0,0,0};});
    reject("band not past margin and guard",[&](auto& r){r.registration.geometry={10,14,24};});
    // #417: the estimate's points stay off Omega (guard more than half the fine patch) and Omega fits inside layer 2's flat
    // width a. The relations of the design constants, not a number of this test.
    constexpr std::uint32_t kPatch=content_registration::kRegistrationDesignDraft.fine_patch_px;
    const auto kA=static_cast<std::uint32_t>(parallax_correction::kParallaxDesignDraft.weight_flat_px);
    constexpr std::uint32_t kHalfMixing=(kSeamBlendDesign.width_pixels+1)/2;
    reject("guard of 0",[&](auto& r){r.registration.geometry={2,0,96};});
    reject("guard of half the fine patch",[&](auto& r){r.registration.geometry={2,kPatch/2,kPatch+8};});
    reject("Omega wider than layer 2's a",[&](auto& r){r.registration.geometry={kA-kHalfMixing+1,72,kA+80};});
    const auto makes=[&](const char* what,const std::function<void(FixedCompositeV3Request&)>& change){
        auto request=Request(draft);change(request);
        Check(!Throws([&]{(void)MakeFixedCompositeConditionV3(request,kCreated,kHash,kAccept);}),what);
    };
    makes("guard one pixel past half the fine patch",[&](auto& r){r.registration.geometry={2,kPatch/2+1,kPatch+8};});
    makes("Omega as wide as layer 2's a",[&](auto& r){r.registration.geometry={kA-kHalfMixing,72,kA+80};});
    makes("Omega wider than a without layer 2",[&](auto& r){r.registration.geometry={kA-kHalfMixing+1,72,kA+80};r.registration.parallax=std::nullopt;});
    reject("surface height 0",[&](auto& r){r.registration.surface_envelope=SurfaceEnvelope{0,8};});
    reject("surface tilt NaN",[&](auto& r){r.registration.surface_envelope=SurfaceEnvelope{40,nan};});
    reject("surface tilt 90",[&](auto& r){r.registration.surface_envelope=SurfaceEnvelope{40,90};});
    reject("surface envelope unset",[&](auto& r){r.registration.surface_envelope=SurfaceEnvelope{};});
    reject("apply-beyond not set",[&](auto& r){r.registration.apply_beyond_envelope=std::nullopt;});
    reject("scale warning negative",[&](auto& r){r.registration.surface_scale_warn_percent=-0.1;});
    reject("scale warning NaN",[&](auto& r){r.registration.surface_scale_warn_percent=nan;});
    reject("max lift 0",[&](auto& r){r.registration.parallax=FixedCompositeConditionV3::Registration::Parallax{0};});
    reject("max lift unset",[&](auto& r){r.registration.parallax=FixedCompositeConditionV3::Registration::Parallax{};});
    reject("live limits unset",[&](auto& r){r.registration.live=FixedCompositeConditionV3::Registration::Live{};});
    reject("seam gate line unset",[&](auto& r){r.seam_gate=FixedCompositeConditionV3::SeamGateLine{};});
    reject("seam gate line 0",[&](auto& r){r.seam_gate.limit_px=0;});
    reject("seam gate guard negative",[&](auto& r){r.seam_gate.guard_band_px=-1;});
    reject("manual bound 0",[&](auto& r){r.revision_rules.manual_adjustment_max_seam_px=0;});
    reject("manual bound NaN",[&](auto& r){r.revision_rules.manual_adjustment_max_seam_px=nan;});
    CHECK(calls==0);
    (void)MakeFixedCompositeConditionV3(Request(draft),kCreated,kHash,never);
    CHECK(calls==1);
}

// Every renamed member of the canonical text must be refused (one unknown, one missing), whichever object it is in.
void ParseRejects(){
    const auto good=FormatFixedCompositeConditionV3(Make(Request(draft)));
    const auto bad=[&](const std::string& text,const std::string& what){Check(Throws([&]{(void)ParseFixedCompositeConditionV3(text);}),("rejects: "+what).c_str());};
    // Renaming each member in turn.
    static const std::regex key("\"([A-Za-z0-9]+)\":");
    int renamed=0;
    for(auto it=std::sregex_iterator(good.begin(),good.end(),key);it!=std::sregex_iterator();++it){
        const auto at=static_cast<size_t>(it->position(1));
        auto text=good;text.insert(at+it->length(1),"X");
        bad(text,"unknown or missing member "+it->str(1)+" at "+std::to_string(at));++renamed;
    }
    CHECK(renamed>=66);
    // Adding an unknown member at the top and in each object.
    bad(Replace(good,"{\"schema\"","{\"extra\":1,\"schema\""),"unknown top-level member");
    for(const char* object:{"\"revision\":{","\"change\":{","\"source\":{","\"outputRaster\":{","\"regionUm\":{","\"seam\":{","\"basePose\":{","\"registration\":{",
        "\"envelope\":{","\"limits\":{","\"geometry\":{","\"surfaceEnvelope\":{","\"parallax\":{","\"seamGate\":{","\"revisionRules\":{"})
        bad(Replace(good,object,std::string(object)+"\"extra\":1,"),std::string("unknown member in ")+object);
    // Duplicate keys.
    bad(Replace(good,"\"approval\":\"unapproved\",","\"approval\":\"unapproved\",\"approval\":\"unapproved\","),"duplicate approval");
    bad(Replace(good,"\"guardPx\":72,","\"guardPx\":72,\"guardPx\":72,"),"duplicate guardPx");
    bad(Replace(good,"\"kind\":\"created\"","\"kind\":\"created\",\"kind\":\"created\""),"duplicate kind");
    // Approval, schema.
    bad(Replace(good,"unapproved","approved"),"approved");
    bad(Replace(good,"\"approval\":\"unapproved\"","\"approval\":true"),"approval not a string");
    bad(Replace(good,"fixed-composite-condition.v3","fixed-composite-condition.v2"),"v2 schema");
    bad(Replace(good,"fixed-composite-condition.v3","fixed-composite-condition.v4"),"future schema");
    bad(Replace(good,"\"schema\":\"a0.fixed-composite-condition.v3\"","\"schema\":3"),"schema not a string");
    // Kinds of value.
    bad(Replace(good,"\"sequence\":1","\"sequence\":\"1\""),"sequence a string");
    bad(Replace(good,"\"sequence\":1","\"sequence\":1.0"),"sequence not whole");
    bad(Replace(good,"\"sequence\":1","\"sequence\":0"),"sequence 0");
    bad(Replace(good,"\"sequence\":1","\"sequence\":1000000"),"sequence beyond");
    bad(Replace(good,"\"sequence\":1","\"sequence\":-1"),"sequence negative");
    bad(Replace(good,"\"parentSha256\":null","\"parentSha256\":\""+kParent+"\""),"revision 1 with a parent");
    bad(Replace(good,"\"parentSha256\":null","\"parentSha256\":5"),"parent a number");
    bad(Replace(good,"\"kind\":\"created\"","\"kind\":\"Created\""),"kind in the wrong case");
    bad(Replace(good,"\"kind\":\"created\"","\"kind\":\"values\""),"values as revision 1");
    bad(Replace(good,"\"kind\":\"created\"","\"kind\":\"saved-estimate\",\"reportSha256\":\""+std::string(64,'e')+"\",\"pairId\":\"p\""),"saved-estimate as revision 1");
    bad(Replace(good,"\"kind\":\"created\"","\"kind\":5"),"kind a number");
    bad(Replace(good,"\"createdUtc\":\"2026-11-04T12:34:56Z\"","\"createdUtc\":\"2026-02-30T12:34:56Z\""),"not a date");
    bad(Replace(good,"\"createdUtc\":\"2026-11-04T12:34:56Z\"","\"createdUtc\":\"2026-11-04 12:34:56\""),"not UTC");
    bad(Replace(good,"\"mountOrientation\":\"normal\"","\"mountOrientation\":\"sideways\""),"unknown orientation");
    bad(Replace(good,"\"origin\":\"band-center-provisional\"","\"origin\":\"auto\""),"unknown origin");
    bad(Replace(good,"\"column\":50","\"column\":100"),"seam column at the width");
    bad(Replace(good,"\"column\":50","\"column\":50.5"),"seam column fractional");
    bad(Replace(good,"\"dpi\":254","\"dpi\":250"),"raster arithmetic off");
    bad(Replace(good,"\"profileFile\":\"wpd-fixed-composite-profile-test.json\"","\"profileFile\":\"..\\\\x.json\""),"profile name climbs out");
    bad(Replace(good,"\"policySha256\":\""+std::string(64,'d')+"\"","\"policySha256\":\""+std::string(63,'d')+"\""),"policy hash short");
    bad(Replace(good,"\"policySha256\":\""+std::string(64,'d')+"\"","\"policySha256\":null"),"policy hash null");
    bad(Replace(good,"\"txMm\":0","\"txMm\":\"0\""),"pose a string");
    bad(Replace(good,"\"txMm\":0","\"txMm\":1"),"base pose of revision 1 not zero");
    bad(Replace(good,"\"positionMm\":5","\"positionMm\":0"),"envelope 0");
    bad(Replace(good,"\"positionMm\":5","\"positionMm\":-5"),"envelope negative");
    bad(Replace(good,"\"positionMm\":5","\"positionMm\":null"),"envelope null");
    bad(Replace(good,"\"positionMm\":5","\"positionMm\":1e999"),"envelope infinite");
    bad(Replace(good,"\"brownLoweBeta\":"+N(0.3),"\"brownLoweBeta\":1"),"beta 1");
    bad(Replace(good,"\"minimumUniqueBlocks\":3","\"minimumUniqueBlocks\":0"),"no blocks");
    bad(Replace(good,"\"minimumUniqueBlocks\":3","\"minimumUniqueBlocks\":2.5"),"blocks fractional");
    bad(Replace(good,"\"convergencePx\":"+N(0.05),"\"convergencePx\":0"),"convergence 0");
    bad(Replace(good,"\"bandHalfWidthPx\":96","\"bandHalfWidthPx\":6"),"band not past margin and guard");
    bad(Replace(good,"\"omegaMarginPx\":2","\"omegaMarginPx\":-2"),"margin negative");
    // #417: read with the same checks as made (the fine patch of the design is 64 px, layer 2's a 48 px, the mixing width 8 px).
    bad(Replace(good,"\"guardPx\":72,","\"guardPx\":32,"),"guard of half the fine patch");
    bad(Replace(good,"\"guardPx\":72,","\"guardPx\":0,"),"guard of 0");
    bad(Replace(good,"\"omegaMarginPx\":2,\"guardPx\":72","\"omegaMarginPx\":45,\"guardPx\":40"),"Omega wider than layer 2's a");
    CHECK(!Throws([&]{(void)ParseFixedCompositeConditionV3(Replace(good,"\"guardPx\":72,","\"guardPx\":33,"));}));
    CHECK(!Throws([&]{(void)ParseFixedCompositeConditionV3(Replace(good,"\"omegaMarginPx\":2,\"guardPx\":72","\"omegaMarginPx\":44,\"guardPx\":40"));}));
    bad(Replace(good,"\"surfaceEnvelope\":{\"heightMm\":40,\"tiltDeg\":8}","\"surfaceEnvelope\":{\"heightMm\":40}"),"surface envelope missing a member");
    bad(Replace(good,"\"surfaceEnvelope\":{\"heightMm\":40,\"tiltDeg\":8}","\"surfaceEnvelope\":{\"heightMm\":0,\"tiltDeg\":8}"),"surface height 0");
    bad(Replace(good,"\"surfaceEnvelope\":{\"heightMm\":40,\"tiltDeg\":8}","\"surfaceEnvelope\":{\"heightMm\":40,\"tiltDeg\":90}"),"surface tilt 90");
    bad(Replace(good,"\"surfaceEnvelope\":{\"heightMm\":40,\"tiltDeg\":8}","\"surfaceEnvelope\":[]"),"surface envelope an array");
    bad(Replace(good,"\"surfaceEnvelope\":{\"heightMm\":40,\"tiltDeg\":8}","\"surfaceEnvelope\":false"),"surface envelope false");
    bad(Replace(good,"\"applyBeyondEnvelope\":true","\"applyBeyondEnvelope\":null"),"apply-beyond null");
    bad(Replace(good,"\"applyBeyondEnvelope\":true","\"applyBeyondEnvelope\":1"),"apply-beyond a number");
    bad(Replace(good,"\"applyBeyondEnvelope\":true","\"applyBeyondEnvelope\":\"true\""),"apply-beyond a string");
    bad(Replace(good,"\"surfaceScaleWarnPercent\":null","\"surfaceScaleWarnPercent\":-1"),"warning negative");
    bad(Replace(good,"\"surfaceScaleWarnPercent\":null","\"surfaceScaleWarnPercent\":\"1\""),"warning a string");
    bad(Replace(good,"\"parallax\":{\"maxLiftMm\":1}","\"parallax\":{\"maxLiftMm\":0}"),"max lift 0");
    bad(Replace(good,"\"parallax\":{\"maxLiftMm\":1}","\"parallax\":{}"),"parallax empty");
    bad(Replace(good,"\"parallax\":{\"maxLiftMm\":1}","\"parallax\":1"),"parallax a number");
    bad(Replace(good,"\"live\":null","\"live\":{}"),"live empty");
    bad(Replace(good,"\"live\":null","\"live\":{\"limits\":{}}"),"live limits empty");
    bad(Replace(good,"\"limitPx\":1","\"limitPx\":0"),"gate line 0");
    bad(Replace(good,"\"guardBandPx\":8","\"guardBandPx\":null"),"gate guard null");
    bad(Replace(good,"\"manualAdjustmentMaxSeamPx\":null","\"manualAdjustmentMaxSeamPx\":0"),"manual bound 0");
    bad(Replace(good,"\"manualAdjustmentMaxSeamPx\":null","\"manualAdjustmentMaxSeamPx\":\"4\""),"manual bound a string");
    // The text as a whole.
    bad(good+"x","trailing text");
    bad("","empty");
    bad("[]","an array");
    bad("{}","an empty object");
    bad(good.substr(0,good.size()-1),"truncated");
    bad(std::string(kMaximumFixedCompositeConditionV3Bytes+1,' '),"beyond the ceiling");
    bad(good+std::string(kMaximumFixedCompositeConditionV3Bytes,' '),"padded beyond the ceiling");
    CHECK(!Throws([&]{(void)ParseFixedCompositeConditionV3(std::string("\xEF\xBB\xBF")+good);}));   // an encoding marker is not content
    // The text of a record of every kind is also refused when its members are those of another kind.
    auto chain=MakeSettings("parse");
    Append(chain,Change(FixedCompositeRevisionKind::created));
    const auto text_est=[&]{Append(chain,SavedEstimate());return chain.texts.back();}();
    bad(Replace(text_est,"\"pairId\":\"pair-1\"","\"pairId\":\"bad pair\""),"bad pair identity");
    bad(Replace(text_est,"\"reportSha256\":\""+std::string(64,'e')+"\",","") ,"saved-estimate without its report");
    bad(Replace(text_est,"\"kind\":\"saved-estimate\"","\"kind\":\"values\""),"values with report members");
    bad(Replace(text_est,"\"kind\":\"saved-estimate\"","\"kind\":\"created\""),"created as revision 2");
    bad(Replace(text_est,"\"sequence\":2","\"sequence\":1"),"revision 1 with a parent");
    bad(Replace(text_est,"\"parentSha256\":\""+HashOf(chain.texts[0])+"\"","\"parentSha256\":null"),"revision 2 without a parent");
    const auto text_manual=[&]{Append(chain,Manual(1,2,0.5,3));return chain.texts.back();}();
    bad(Replace(text_manual,"\"seamDxPx\":1","\"seamDxPx\":null"),"entered null");
    bad(Replace(text_manual,"\"maximumSeamDisplacementPx\":3","\"maximumSeamDisplacementPx\":-3"),"measure negative");
    bad(Replace(text_manual,"\"entered\":{\"seamDxPx\":1,","\"entered\":{"),"entered missing a member");
    CHECK(!Throws([&]{(void)ParseFixedCompositeConditionV3(text_manual);}));
    std::error_code ignored;fs::remove_all(chain.settings,ignored);
}

// The registration policy file: the same members as the record's registration, seamGate and revisionRules.
void PolicyReader(){
    const std::string schema="a0.wpd-registration-policy.v1";
    const auto policy=std::string("{\"schema\":\"a0.wpd-registration-policy.v1\",")+CanonicalTail();
    const auto values=ParseFixedCompositeRegistrationPolicyV3(policy,schema);
    const auto record=Make(Request(draft));
    CHECK(FormatFixedCompositeConditionV3([&]{auto c=record;c.registration=values.registration;c.seam_gate=values.seam_gate;c.revision_rules=values.revision_rules;return c;}())==FormatFixedCompositeConditionV3(record));
    CHECK(values.registration.apply_beyond_envelope==true && !values.registration.surface_scale_warn_percent && !values.revision_rules.manual_adjustment_max_seam_px);
    CHECK(values.registration.parallax && values.registration.parallax->max_lift_mm==1 && values.registration.surface_envelope->height_mm==40 && !values.registration.live);
    const auto bad=[&](const std::string& text,const std::string& what){Check(Throws([&]{(void)ParseFixedCompositeRegistrationPolicyV3(text,schema);}),("policy rejects: "+what).c_str());};
    static const std::regex key("\"([A-Za-z0-9]+)\":");
    int renamed=0;
    for(auto it=std::sregex_iterator(policy.begin(),policy.end(),key);it!=std::sregex_iterator();++it){
        auto text=policy;text.insert(static_cast<size_t>(it->position(1))+it->length(1),"X");
        bad(text,"unknown or missing member "+it->str(1));++renamed;
    }
    CHECK(renamed>=34);
    bad(Replace(policy,"a0.wpd-registration-policy.v1","a0.wpd-registration-policy.v2"),"other schema");
    bad(Replace(policy,"{\"schema\"","{\"extra\":1,\"schema\""),"extra member");
    bad(Replace(policy,"\"seamGate\":{\"limitPx\":1,\"guardBandPx\":8}","\"seamGate\":null"),"seam gate null");
    bad(Replace(policy,"\"positionMm\":5","\"positionMm\":null"),"an unset range");
    bad(Replace(policy,"\"applyBeyondEnvelope\":true","\"applyBeyondEnvelope\":null"),"apply-beyond null");
    bad(Replace(policy,"\"guardPx\":72,","\"guardPx\":72,\"guardPx\":72,"),"duplicate key");
    bad(Replace(policy,"\"guardPx\":72,","\"guardPx\":32,"),"guard of half the fine patch");
    bad(Replace(policy,"\"omegaMarginPx\":2,\"guardPx\":72","\"omegaMarginPx\":45,\"guardPx\":40"),"Omega wider than layer 2's a");
    bad(Replace(policy,"\"limitPx\":1","\"limitPx\":0"),"a line of 0");
    bad(std::string(kMaximumFixedCompositeRegistrationPolicyV3Bytes+1,' '),"beyond the ceiling");
    bad("","empty");
    CHECK(!Throws([&]{(void)ParseFixedCompositeRegistrationPolicyV3(std::string("\xEF\xBB\xBF")+policy,schema);}));
    // A layer that is not applied is null in the policy as in the record.
    const auto off=Replace(Replace(policy,"\"surfaceEnvelope\":{\"heightMm\":40,\"tiltDeg\":8}","\"surfaceEnvelope\":null"),"\"parallax\":{\"maxLiftMm\":1}","\"parallax\":null");
    const auto off_values=ParseFixedCompositeRegistrationPolicyV3(off,schema);
    CHECK(!off_values.registration.surface_envelope && !off_values.registration.parallax);
}

// The canonical texts that the shared fixture builder (registration_fixtures.hpp) makes are the texts this writer writes, byte for byte.
void FixtureTexts(){
    namespace rf=a0::tests::registration_fixtures;
    const auto same=[&](const rf::ConditionV3Options& options,const std::string& what){
        const auto text=rf::ConditionV3Text(options);
        Check(!Throws([&]{(void)ParseFixedCompositeConditionV3(text);}),("the reader accepts the fixture: "+what).c_str());
        Check(FormatFixedCompositeConditionV3(ParseFixedCompositeConditionV3(text))==text,("the writer writes the fixture back: "+what).c_str());
    };
    same({},"default");
    {rf::ConditionV3Options o;o.change.kind="migrated-from-v2";same(o,"migrated-from-v2");}
    for(const char* kind:{"saved-estimate","manual-adjustment","rollback","values"}){
        rf::ConditionV3Options o;o.sequence=2;o.parent_sha256=rf::Hex64('9');o.change.kind=kind;same(o,kind);
    }
    {rf::ConditionV3Options o;o.surface_envelope.reset();o.parallax_max_lift_mm.reset();o.apply_beyond_envelope=false;same(o,"layers off");}
    {rf::ConditionV3Options o;o.live_limits=rf::Limits9{};o.surface_scale_warn_percent=0.0;o.manual_adjustment_max_seam_px=4.5;same(o,"reserved members set");}
    {rf::ConditionV3Options o;o.mount_orientation="rotated-180";o.seam.origin="operator";same(o,"rotated mount, operator seam");}
    {rf::ConditionV3Options o;o.envelope.position_mm=0.1+0.2;o.limits.brown_lowe_beta=1.0/3.0;same(o,"digits");}
    // The fixture builder's wrong texts are refused.
    {rf::ConditionV3Options o;o.approval="approved";REJECT(ParseFixedCompositeConditionV3(rf::ConditionV3Text(o)));}
    {rf::ConditionV3Options o;o.schema="a0.fixed-composite-condition.v2";REJECT(ParseFixedCompositeConditionV3(rf::ConditionV3Text(o)));}
}

void Binding(){
    const auto c=Make(Request(draft));
    const auto binding=BindFixedCompositeProfileV3(c,draft,kHash);
    CHECK(binding.profile_file_sha256==c.source.profile_file_sha256 && binding.profile_fingerprint_sha256==c.source.profile_fingerprint_sha256);
    CHECK(binding.parameters.raster.width_pixels==100 && binding.parameters.layout==a0::m2::StitchLayout::camera_a_left_camera_b_right);
    REJECT(BindFixedCompositeProfileV3(c," "+draft,kHash));
    REJECT(BindFixedCompositeProfileV3(c,approved,kHash));
    REJECT(BindFixedCompositeProfileV3(c,draft,FixedCompositeSha256{}));
    REJECT(BindFixedCompositeProfileV3(c,std::string(kMaximumFixedCompositeProfileBytes+1,' '),kHash));
    auto garbage=c;garbage.source.profile_file_sha256=HashOf("not json");
    REJECT(BindFixedCompositeProfileV3(garbage,"not json",kHash));
    auto outside=c;outside.raster=render::OutputRaster{render::DocumentRegionUm{100000,100000,1200000,108000},254,11000,80};
    REJECT(BindFixedCompositeProfileV3(outside,draft,kHash));
    auto wrong_fingerprint=c;wrong_fingerprint.source.profile_fingerprint_sha256=std::string(64,'0');
    REJECT(BindFixedCompositeProfileV3(wrong_fingerprint,draft,kHash));
}

void Files(){
    auto chain=MakeSettings("files");
    const auto folder=chain.revisions;
    // Absent folder and empty folder: no revision, not an error.
    CHECK(!LoadActiveFixedCompositeRevisionV3(chain.settings/"nothing-here",kHash));
    CHECK(!LoadActiveFixedCompositeRevisionV3(folder,kHash));
    WriteAll(folder/"r000001.json.partial","a writer that has not finished");
    CHECK(!LoadActiveFixedCompositeRevisionV3(folder,kHash));
    fs::remove(folder/"r000001.json.partial");
    // The first revision.
    const auto first=Append(chain,Change(FixedCompositeRevisionKind::created));
    CHECK(fs::exists(folder/"r000001.json") && !fs::exists(folder/"r000001.json.partial"));
    CHECK(chain.texts[0]==FormatFixedCompositeConditionV3(first)+"\n");
    {
        const auto active=LoadActiveFixedCompositeRevisionV3(folder,kHash);
        CHECK(active && active->condition.revision.sequence==1 && active->file_sha256==HashOf(chain.texts[0]));
        CHECK(active && active->profile.profile_file_sha256==HashOf(draft) && active->profile.parameters.raster.width_pixels==100);
        const auto direct=LoadFixedCompositeConditionV3(folder/"r000001.json",kHash);
        CHECK(direct.file_sha256==HashOf(chain.texts[0]) && direct.condition.seam.column==50);
    }
    // Never overwrite, even for an identical record; a number is written once.
    REJECT(WriteFixedCompositeConditionV3(folder,first));
    CHECK(ReadAll(folder/"r000001.json")==chain.texts[0]);
    // The next revision takes its parent from the file before it; the highest number is in force.
    Append(chain,SavedEstimate(),[](FixedCompositeV3Request& r){r.base_pose.tx_mm=1;});
    Append(chain,Manual(0.5,0,0,2),[](FixedCompositeV3Request& r){r.base_pose.tx_mm=1.5;});
    {
        const auto active=LoadActiveFixedCompositeRevisionV3(folder,kHash);
        CHECK(active && active->condition.revision.sequence==3 && active->condition.base_pose.tx_mm==1.5 && active->file_sha256==HashOf(chain.texts[2]));
    }
    // A number is never skipped; the writer refuses a gap.
    {
        auto skip=Request(draft);skip.sequence=5;skip.parent_sha256=HashOf(chain.texts[2]);skip.change=Change(FixedCompositeRevisionKind::values);
        REJECT(WriteFixedCompositeConditionV3(folder,Make(skip)));
        CHECK(!fs::exists(folder/"r000005.json") && !fs::exists(folder/"r000005.json.partial"));
    }
    // Two writers of the same number: the second fails and the first file is intact.
    {
        auto again=Request(draft);again.sequence=3;again.parent_sha256=HashOf(chain.texts[1]);again.change=Manual(9,9,9,9);again.base_pose.tx_mm=1;
        REJECT(WriteFixedCompositeConditionV3(folder,Make(again)));
        CHECK(ReadAll(folder/"r000003.json")==chain.texts[2]);
    }
    // A partial that is already there is not reused or removed.
    {
        WriteAll(folder/"r000004.json.partial","someone else");
        auto next=Request(draft);next.sequence=4;next.parent_sha256=HashOf(chain.texts[2]);next.change=Change(FixedCompositeRevisionKind::values);
        next.registration=chain.records[2].registration;next.base_pose=chain.records[2].base_pose;
        REJECT(WriteFixedCompositeConditionV3(folder,Make(next)));
        CHECK(ReadAll(folder/"r000004.json.partial")=="someone else" && !fs::exists(folder/"r000004.json"));
        // A partial is not counted as a revision (and is not an error).
        CHECK(LoadActiveFixedCompositeRevisionV3(folder,kHash)->condition.revision.sequence==3);
        fs::remove(folder/"r000004.json.partial");
        WriteFixedCompositeConditionV3(folder,Make(next));
        chain.texts.push_back(ReadAll(folder/"r000004.json"));chain.records.push_back(Make(next));
    }
    // Going back is a new revision that carries the content of an earlier one; nothing is removed or renamed.
    {
        const auto before=chain.texts;
        Append(chain,Rollback(HashOf(chain.texts[1])),[&](FixedCompositeV3Request& r){
            r.registration=chain.records[1].registration;r.base_pose=chain.records[1].base_pose;r.policy_sha256=chain.records[1].source.policy_sha256;});
        const auto active=LoadActiveFixedCompositeRevisionV3(folder,kHash);
        CHECK(active && active->condition.revision.sequence==5 && active->condition.base_pose.tx_mm==1 && active->condition.revision.change.kind==FixedCompositeRevisionKind::rollback);
        for(size_t i=0;i<before.size();++i)CHECK(ReadAll(folder/FixedCompositeRevisionFileName(static_cast<std::uint32_t>(i+1)))==before[i]);
    }
    // A rollback must carry the content of the revision it names, and that revision must be earlier in the chain.
    {
        auto other=MakeSettings("rollback");
        Append(other,Change(FixedCompositeRevisionKind::created));
        Append(other,SavedEstimate(),[](FixedCompositeV3Request& r){r.base_pose.tx_mm=1;});
        // A rollback whose pose is not the restored one.
        Append(other,Rollback(HashOf(other.texts[0])),[](FixedCompositeV3Request& r){r.base_pose.tx_mm=1;});
        REJECT(LoadActiveFixedCompositeRevisionV3(other.revisions,kHash));
        // A rollback of a hash that is no earlier revision.
        auto third=MakeSettings("rollback-unknown");
        Append(third,Change(FixedCompositeRevisionKind::created));
        Append(third,Rollback(std::string(64,'a')));
        REJECT(LoadActiveFixedCompositeRevisionV3(third.revisions,kHash));
        std::error_code ignored;fs::remove_all(other.settings,ignored);fs::remove_all(third.settings,ignored);
    }
    // A broken chain is refused, never guessed: a gap, a parent that is not the previous file, a file that is not a revision.
    {
        const auto copy_chain=[&](const std::string& name){
            auto c=MakeSettings(name);
            for(size_t i=0;i<chain.texts.size();++i)WriteAll(c.revisions/FixedCompositeRevisionFileName(static_cast<std::uint32_t>(i+1)),chain.texts[i]);
            return c;
        };
        { // the unchanged copy is fine
            auto c=copy_chain("chain-copy");
            CHECK(LoadActiveFixedCompositeRevisionV3(c.revisions,kHash)->condition.revision.sequence==5);
            fs::remove(c.revisions/"r000002.json");
            REJECT(LoadActiveFixedCompositeRevisionV3(c.revisions,kHash));                         // a gap
            std::error_code ignored;fs::remove_all(c.settings,ignored);
        }
        { // a revision whose parent is another file
            auto c=copy_chain("chain-parent");
            WriteAll(c.revisions/"r000003.json",Replace(chain.texts[2],HashOf(chain.texts[1]),HashOf(chain.texts[0])));
            REJECT(LoadActiveFixedCompositeRevisionV3(c.revisions,kHash));
            std::error_code ignored;fs::remove_all(c.settings,ignored);
        }
        { // a revision that was changed after the next one named its hash
            auto c=copy_chain("chain-changed");
            WriteAll(c.revisions/"r000002.json",chain.texts[1]+" ");
            REJECT(LoadActiveFixedCompositeRevisionV3(c.revisions,kHash));
            std::error_code ignored;fs::remove_all(c.settings,ignored);
        }
        { // the number in the name is not the record's
            auto c=copy_chain("chain-number");
            WriteAll(c.revisions/"r000002.json",chain.texts[2]);WriteAll(c.revisions/"r000003.json",chain.texts[1]);
            REJECT(LoadActiveFixedCompositeRevisionV3(c.revisions,kHash));
            std::error_code ignored;fs::remove_all(c.settings,ignored);
        }
        { // the chain starts at 2
            auto c=copy_chain("chain-start");
            fs::remove(c.revisions/"r000001.json");
            REJECT(LoadActiveFixedCompositeRevisionV3(c.revisions,kHash));
            std::error_code ignored;fs::remove_all(c.settings,ignored);
        }
        for(const char* stray:{"notes.txt","r1.json","R000006.json","r000006.JSON","r000006.json.bak","wpd-fixed-composite-condition.json"}){
            auto c=copy_chain("chain-stray");
            WriteAll(c.revisions/stray,"x");
            Check(Throws([&]{(void)LoadActiveFixedCompositeRevisionV3(c.revisions,kHash);}),(std::string("a stray file is refused: ")+stray).c_str());
            std::error_code ignored;fs::remove_all(c.settings,ignored);
        }
        { // a folder where a revision should be
            auto c=copy_chain("chain-dir");
            fs::remove(c.revisions/"r000005.json");fs::create_directories(c.revisions/"r000005.json");
            REJECT(LoadActiveFixedCompositeRevisionV3(c.revisions,kHash));
            std::error_code ignored;fs::remove_all(c.settings,ignored);
        }
        { // a damaged and an oversize active revision
            auto c=copy_chain("chain-damaged");
            WriteAll(c.revisions/"r000005.json",Replace(chain.texts[4],"unapproved","approved"));
            REJECT(LoadActiveFixedCompositeRevisionV3(c.revisions,kHash));
            WriteAll(c.revisions/"r000005.json",std::string(kMaximumFixedCompositeConditionV3Bytes+1,' '));
            REJECT(LoadActiveFixedCompositeRevisionV3(c.revisions,kHash));
            std::error_code ignored;fs::remove_all(c.settings,ignored);
        }
        { // the revisions folder is a file
            auto c=copy_chain("chain-file");
            fs::remove_all(c.revisions);WriteAll(c.revisions,"x");
            REJECT(LoadActiveFixedCompositeRevisionV3(c.revisions,kHash));
            std::error_code ignored;fs::remove_all(c.settings,ignored);
        }
    }
    // The profile sits in the settings folder and is checked on every load.
    {
        const auto profile_file=chain.settings/"wpd-fixed-composite-profile-test.json";
        const auto reload=[&]{return Throws([&]{(void)LoadActiveFixedCompositeRevisionV3(folder,kHash);});};
        WriteAll(profile_file," "+draft);CHECK(reload());
        WriteAll(profile_file,approved);CHECK(reload());
        WriteAll(profile_file,"");CHECK(reload());
        fs::remove(profile_file);
        try{(void)LoadActiveFixedCompositeRevisionV3(folder,kHash);CHECK(false);}
        catch(const FixedCompositeProfileError& e){CHECK(e.GetKind()==FixedCompositeProfileError::Kind::missing);}
        catch(...){CHECK(false);}
        WriteAll(profile_file,Replace(draft,"CAM-A-left-CAM-B-right","CAM-A-top-CAM-B-bottom"));
        try{(void)LoadActiveFixedCompositeRevisionV3(folder,kHash);CHECK(false);}
        catch(const FixedCompositeProfileError& e){CHECK(e.GetKind()==FixedCompositeProfileError::Kind::differs);}
        catch(...){CHECK(false);}
        fs::remove(profile_file);fs::create_directories(profile_file);CHECK(reload());       // a folder in its place
        fs::remove(profile_file);
        WriteAll(profile_file,draft);CHECK(!reload());                                       // restored: usable again
    }
    // The profile beside the revisions folder, not inside it.
    {
        auto c=MakeSettings("profile-place");
        fs::remove(c.settings/"wpd-fixed-composite-profile-test.json");
        WriteAll(c.revisions/"wpd-fixed-composite-profile-test.json",draft);
        Append(c,Change(FixedCompositeRevisionKind::created));
        REJECT(LoadActiveFixedCompositeRevisionV3(c.revisions,kHash));
        std::error_code ignored;fs::remove_all(c.settings,ignored);
    }
    // A revision file is read only from the revisions folder, under its own number.
    {
        const auto elsewhere=chain.settings/"elsewhere";fs::create_directories(elsewhere);
        WriteAll(elsewhere/"r000001.json",chain.texts[0]);
        REJECT(LoadFixedCompositeConditionV3(elsewhere/"r000001.json",kHash));
        WriteAll(chain.settings/"wpd-fixed-composite-condition.json",chain.texts[0]);
        REJECT(LoadFixedCompositeConditionV3(chain.settings/"wpd-fixed-composite-condition.json",kHash));
        REJECT(LoadFixedCompositeConditionV3(folder/"r000002.json.nothing",kHash));
        REJECT(LoadFixedCompositeConditionV3(folder/"r000001.json",FixedCompositeSha256{}));
        WriteAll(folder/"r000009.json",chain.texts[0]);
        REJECT(LoadFixedCompositeConditionV3(folder/"r000009.json",kHash));      // the number in the name is not the record's
        fs::remove(folder/"r000009.json");
    }
    // Invalid input writes nothing, and a missing folder is not created.
    {
        auto invalid=chain.records[0];invalid.source.profile_file="bad/name.json";invalid.revision.sequence=6;
        REJECT(WriteFixedCompositeConditionV3(folder,invalid));
        CHECK(!fs::exists(folder/"r000006.json") && !fs::exists(folder/"r000006.json.partial"));
        REJECT(WriteFixedCompositeConditionV3(chain.settings/"missing-folder",chain.records[0]));
        CHECK(!fs::exists(chain.settings/"missing-folder"));
        fs::create_directories(folder/"r000006.json");
        auto next=chain.records[4];next.revision.sequence=6;next.revision.parent_sha256=HashOf(chain.texts[4]);next.revision.change=Change(FixedCompositeRevisionKind::values);
        REJECT(WriteFixedCompositeConditionV3(folder,next));                    // a directory in the way is not replaced
        fs::remove(folder/"r000006.json");
    }
    std::error_code ignored;fs::remove_all(chain.settings,ignored);
}

// LoadFixedCompositeConditionAny: v1 and v2 read as before; a v3 record is the revision in force beside the condition file.
void AnyEntrance(){
    namespace detail=fixed_composite_detail;
    auto chain=MakeSettings("any");
    Append(chain,Change(FixedCompositeRevisionKind::created));
    const auto revision=chain.revisions/"r000001.json";
    const auto legacy_name=chain.settings/kFixedCompositeConditionName;
    // A revision file named directly is read as v3 (the default entrance returns v3 since the capture path is wired).
    {
        const auto direct=LoadFixedCompositeConditionAny(revision,kHash);
        CHECK(direct && std::holds_alternative<LoadedFixedCompositeConditionV3>(*direct));
    }
    // The condition file's name with revisions beside it: the revision in force, whichever of the entrances asks.
    {
        const auto any=LoadFixedCompositeConditionAny(legacy_name,kHash);
        CHECK(any && std::holds_alternative<LoadedFixedCompositeConditionV3>(*any));
        if(any && std::holds_alternative<LoadedFixedCompositeConditionV3>(*any)){
            const auto& v3=std::get<LoadedFixedCompositeConditionV3>(*any);
            CHECK(v3.file_sha256==HashOf(chain.texts[0]) && v3.condition.revision.sequence==1);
        }
        const auto selection=SelectFixedComposite(legacy_name,kHash);
        CHECK(selection.loaded && selection.IsV3() && !selection.IsV2());
    }
    // The newest revision of a longer chain is the one in force.
    Append(chain,SavedEstimate(),[](FixedCompositeV3Request& r){r.base_pose.tx_mm=1.5;});
    {
        const auto selection=SelectFixedComposite(legacy_name,kHash);
        CHECK(selection.IsV3() && std::get<LoadedFixedCompositeConditionV3>(*selection.loaded).condition.revision.sequence==2);
    }
    // A name that is not the condition file's is read as itself: nothing there is nothing.
    CHECK(!detail::LoadFixedCompositeConditionAnyWith(chain.settings/"nothing.json",kHash,true));
    // With the switch off the reader is v2's, byte for byte: the revisions folder is not looked at and a revision file is refused.
    REJECT(detail::LoadFixedCompositeConditionAnyWith(revision,kHash,false));
    CHECK(!detail::LoadFixedCompositeConditionAnyWith(legacy_name,kHash,false));
    // A v3 record is kept as a revision only: the same text under the condition file's name is refused when there is no revision
    // beside it to read instead.
    {
        auto other=MakeSettings("any-copy");
        Append(other,Change(FixedCompositeRevisionKind::created));
        WriteAll(other.settings/kFixedCompositeConditionName,other.texts[0]);
        fs::remove_all(other.revisions);
        REJECT(LoadFixedCompositeConditionAny(other.settings/kFixedCompositeConditionName,kHash));
        const auto selection=SelectFixedComposite(other.settings/kFixedCompositeConditionName,kHash);
        CHECK(!selection.loaded && selection.reason==FixedCompositeSkip::condition_invalid && !selection.IsV3());
        std::error_code ignored;fs::remove_all(other.settings,ignored);
    }
    // A revisions folder that holds no revision is nothing; a v2 file beside it is read as before.
    {
        auto bare=MakeSettings("any-empty");
        FixedCompositeV2Request request;
        request.profile_file="wpd-fixed-composite-profile-test.json";request.profile_bytes=draft;request.raster=Raster();
        request.scale_auto_correction_max_percent=1;request.seam_column=50;
        const auto v2=MakeFixedCompositeConditionV2(request,kCreated,kHash,[](const FixedCompositeConditionV2&,const FixedCompositeProfileBinding&){});
        CHECK(!LoadFixedCompositeConditionAny(bare.settings/kFixedCompositeConditionName,kHash));
        WriteFixedCompositeConditionV2(bare.settings/kFixedCompositeConditionName,v2);
        const auto selection=SelectFixedComposite(bare.settings/kFixedCompositeConditionName,kHash);
        CHECK(selection.IsV2() && !selection.IsV3());
        // A stray partial of a writer that has not finished does not count as a revision.
        WriteAll(bare.revisions/"r000001.json.partial","x");
        CHECK(SelectFixedComposite(bare.settings/kFixedCompositeConditionName,kHash).IsV2());
        fs::remove(bare.revisions/"r000001.json.partial");
        // A v2 file and a revision are two sources: refused, as a damaged condition.
        Append(bare,Change(FixedCompositeRevisionKind::created));
        {
            const auto selection_both=SelectFixedComposite(bare.settings/kFixedCompositeConditionName,kHash);
            CHECK(!selection_both.loaded && selection_both.reason==FixedCompositeSkip::condition_invalid && selection_both.detail.find("two sources")!=std::string::npos);
            REJECT(LoadFixedCompositeConditionAny(bare.settings/kFixedCompositeConditionName,kHash));
        }
        // The same with a v1 file.
        fs::remove(bare.settings/kFixedCompositeConditionName);
        WriteAll(bare.settings/kFixedCompositeConditionName,"{\"schema\":\"a0.fixed-composite-condition.v1\"}");
        {
            const auto selection_both=SelectFixedComposite(bare.settings/kFixedCompositeConditionName,kHash);
            CHECK(!selection_both.loaded && selection_both.reason==FixedCompositeSkip::condition_invalid);
        }
        // Without the file, the revision is in force again.
        fs::remove(bare.settings/kFixedCompositeConditionName);
        CHECK(SelectFixedComposite(bare.settings/kFixedCompositeConditionName,kHash).IsV3());
        std::error_code ignored;fs::remove_all(bare.settings,ignored);
    }
    // A chain that is not whole is a damaged condition (no guess); a profile that is not there is the profile's absence.
    {
        auto broken=MakeSettings("any-broken");
        Append(broken,Change(FixedCompositeRevisionKind::created));
        Append(broken,SavedEstimate());
        fs::remove(broken.revisions/"r000001.json");
        const auto selection=SelectFixedComposite(broken.settings/kFixedCompositeConditionName,kHash);
        CHECK(!selection.loaded && selection.reason==FixedCompositeSkip::condition_invalid);
        std::error_code ignored;fs::remove_all(broken.settings,ignored);
        auto lonely=MakeSettings("any-profile");
        Append(lonely,Change(FixedCompositeRevisionKind::created));
        fs::remove(lonely.settings/"wpd-fixed-composite-profile-test.json");
        const auto lonely_selection=SelectFixedComposite(lonely.settings/kFixedCompositeConditionName,kHash);
        CHECK(!lonely_selection.loaded && lonely_selection.reason==FixedCompositeSkip::profile_absent);
        fs::remove_all(lonely.settings,ignored);
    }
    // With the switch off the ceiling is v2's: a text beyond it is too large before its schema is read.
    WriteAll(legacy_name,std::string(kMaximumFixedCompositeConditionV2Bytes+1,' '));
    REJECT(detail::LoadFixedCompositeConditionAnyWith(legacy_name,kHash,false));
    // With it on a v2 file is read as v2 (and one beyond v2's ceiling is still refused).
    {
        const auto v2_root=chain.settings/"v2";fs::create_directories(v2_root);
        FixedCompositeV2Request request;
        request.profile_file="wpd-fixed-composite-profile-test.json";request.profile_bytes=draft;request.raster=Raster();
        request.scale_auto_correction_max_percent=1;request.seam_column=50;
        const auto v2=MakeFixedCompositeConditionV2(request,kCreated,kHash,[](const FixedCompositeConditionV2&,const FixedCompositeProfileBinding&){});
        WriteFixedCompositeConditionV2(v2_root/kFixedCompositeConditionName,v2);
        WriteAll(v2_root/"wpd-fixed-composite-profile-test.json",draft);
        for(const bool accept:{false,true}){
            const auto any=detail::LoadFixedCompositeConditionAnyWith(v2_root/kFixedCompositeConditionName,kHash,accept);
            Check(any && std::holds_alternative<LoadedFixedCompositeConditionV2>(*any),"v2 is read as v2 whichever way the v3 switch is set");
        }
        CHECK(LoadFixedCompositeConditionAny(v2_root/kFixedCompositeConditionName,kHash).has_value());
        const auto text=ReadAll(v2_root/kFixedCompositeConditionName);
        WriteAll(v2_root/"large.json",text.substr(0,text.size()-2)+std::string(kMaximumFixedCompositeConditionV2Bytes,' ')+"}\n");
        REJECT(detail::LoadFixedCompositeConditionAnyWith(v2_root/"large.json",kHash,true));
        // v2's bytes are what they were: the v3 work does not change the v2 format.
        CHECK(FormatFixedCompositeConditionV2(v2).find("a0.fixed-composite-condition.v2")!=std::string::npos && ReadAll(v2_root/kFixedCompositeConditionName)==FormatFixedCompositeConditionV2(v2)+"\n");
    }
    std::error_code ignored;fs::remove_all(chain.settings,ignored);
}
}

int main(int argc,char* argv[]){
    try{
        if(argc!=2)throw std::invalid_argument("expected the shared rig profile v2 fixture folder");
        const fs::path fixtures=argv[1];
        draft=ReadAll(fixtures/"calibrated-draft.json");approved=ReadAll(fixtures/"approved.json");blank=ReadAll(fixtures/"template.json");
        if(draft.empty() || approved.empty() || blank.empty())throw std::runtime_error("fixture profiles are missing");
        Names();MakeAndFormat();KindsRoundTrip();MakeRejects();ParseRejects();PolicyReader();FixtureTexts();Binding();Files();AnyEntrance();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
