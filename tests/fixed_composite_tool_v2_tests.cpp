// The development entry that makes the fixed composite condition v2 (#405 W6):
// what it refuses, that it never overwrites, the profile copy, the policy limit and the
// mount orientation it records, and a dry run. Camera free; every file lives in a
// temporary folder. Argument 1 is the shared rig profile v2 fixture folder.
#include "a0/phase0/experimental/fixed_composite_tool_v2.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace px=a0::phase0::experimental;
namespace m2=a0::m2::experimental;
namespace fs=std::filesystem;
using namespace std::chrono_literals;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr)

const auto kNow=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026}/11/4}+12h};
const m2::FixedCompositeSha256 kHash=[](const std::vector<unsigned char>& bytes){return m2::FixedCompositeBytesSha256(bytes);};
std::string HashOf(const std::string& text){return m2::FixedCompositeBytesSha256(std::vector<unsigned char>(text.begin(),text.end()));}

std::string ReadAll(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void WriteAll(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;}
fs::path TempParent(){
#ifdef _MSC_VER
    char* value=nullptr;size_t length=0;
    if(_dupenv_s(&value,&length,"A0_TEST_TMPDIR")==0 && value!=nullptr){
        const std::string text(value);std::free(value);
        if(!text.empty())return fs::path(text);
    }
#endif
    return fs::temp_directory_path();
}
std::set<std::string> Listing(const fs::path& folder){
    std::set<std::string> names;
    for(const auto& e:fs::directory_iterator(folder))names.insert(e.path().filename().string());
    return names;
}
bool Contains(const std::string& text,const std::string& part){return text.find(part)!=std::string::npos;}
bool Holds(const fs::path& folder,std::initializer_list<std::string> names){return Listing(folder)==std::set<std::string>(names);}

constexpr const char* kPolicy="{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":1}";
constexpr const char* kPolicyWithoutScale="{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2}";
constexpr const char* kRotated="{\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":\"rotated-180\"}";

std::string draft,approved,blank;
fs::path root,profile_path;

// 18 mm x 8 mm inside the 39 mm overlap of the fixture cameras, 180 x 80 pixels at 254 DPI.
px::FixedConditionV2Request Request(const fs::path& folder){
    px::FixedConditionV2Request r;
    r.profile=profile_path;r.settings_folder=folder;r.region_um={580000,100000,598000,108000};r.dpi=254;
    return r;
}
fs::path Folder(const std::string& name,const char* policy=kPolicy){
    const auto folder=root/name;fs::create_directories(folder);
    if(policy)WriteAll(folder/px::kMergePolicySettingsName,policy);
    return folder;
}
px::FixedConditionV2MakeResult Make(const px::FixedConditionV2Request& r){return px::MakeFixedCompositeConditionV2File(r,kHash,kNow);}

void Hashing(){
    CHECK(HashOf("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(HashOf("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

void Refusals(){
    // Nothing is made, and nothing is written, for each of these.
    const auto refused=[&](const char* what,const px::FixedConditionV2Request& r,const fs::path& watched){
        const auto before=Listing(watched);
        const auto result=Make(r);
        Check(!result.made && !result.checked && !result.message.empty(),what);
        Check(Listing(watched)==before,(std::string("nothing written: ")+what).c_str());
        return result;
    };
    {
        auto r=Request(root/"no-such-folder");
        const auto result=Make(r);
        CHECK(!result.made && Contains(result.message,"設定のフォルダがありません"));
    }
    {
        const auto folder=Folder("no-policy",nullptr);
        const auto result=refused("no policy file",Request(folder),folder);
        CHECK(Contains(result.message,"方針ファイル"));
    }
    {
        const auto folder=Folder("policy-without-scale",kPolicyWithoutScale);
        const auto result=refused("policy without a scale limit",Request(folder),folder);
        CHECK(Contains(result.message,"倍率の上限"));
    }
    {
        const auto folder=Folder("broken-policy","{\"schema\":\"a0.wpd-merge-policy.v1\"}");
        refused("a policy that is not exact",Request(folder),folder);
    }
    {
        const auto folder=Folder("bad-mount");
        WriteAll(folder/px::kMountOrientationSettingsName,"{\"schema\":\"x\"}");
        const auto result=refused("an unreadable mount orientation",Request(folder),folder);
        CHECK(Contains(result.message,"取り付けの向き"));
    }
    const auto folder=Folder("bad-inputs");
    auto request=Request(folder);
    request.profile=root/"missing-profile.json";
    refused("a profile that does not exist",request,folder);
    request.profile=root;
    refused("a profile that is a folder",request,folder);
    WriteAll(root/"approved.json",approved);request.profile=root/"approved.json";
    refused("an approved profile",request,folder);
    WriteAll(root/"blank.json",blank);request.profile=root/"blank.json";
    refused("a template profile",request,folder);
    WriteAll(root/"big.json",std::string(m2::kMaximumFixedCompositeProfileBytes+1,' '));request.profile=root/"big.json";
    refused("an oversize profile",request,folder);
    request=Request(folder);
    request.dpi=0;refused("DPI 0",request,folder);
    request.dpi=65536;refused("DPI beyond 65535",request,folder);
    request=Request(folder);request.region_um={598000,100000,580000,108000};refused("right left of left",request,folder);
    request=Request(folder);request.region_um={580000,108000,598000,108000};refused("empty height",request,folder);
    request=Request(folder);request.region_um={-1,100000,598000,108000};refused("negative region",request,folder);
    request=Request(folder);request.region_um={580000,100000,2000000000,108000};refused("region beyond the range",request,folder);
    request=Request(folder);request.region_um={580000,100000,1250000,108000};refused("region beyond the sheet",request,folder);
    request=Request(folder);request.region_um={0,0,1189000,841000};request.dpi=65535;refused("too many pixels",request,folder);
    // A region only camera A reaches has no band; one only B reaches has no camera A at the left.
    request=Request(folder);request.region_um={100000,100000,110000,108000};
    refused("a region with no overlap",request,folder);
    request=Request(folder);request.region_um={1000000,100000,1010000,108000};
    refused("a region camera A does not reach",request,folder);
}

void ChecksOfTheRecord(){
    // The range of the limit comes from the policy file: 20 % leaves a gap between A and B at scale 0.8.
    const auto folder=Folder("wide-limit","{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":20}");
    auto request=Request(folder);request.region_um={590000,100000,640000,108000};
    const auto before=Listing(folder);
    const auto result=Make(request);
    CHECK(!result.made && !result.checked && Contains(result.message,"検査に通りませんでした") && Contains(result.message,"0.8"));
    CHECK(Listing(folder)==before);
    // The same region with the 1 % policy is fine.
    const auto fine=Folder("narrow-limit");
    request=Request(fine);request.region_um={590000,100000,640000,108000};
    const auto made=Make(request);
    CHECK(made.made);
    // A limit of 0 checks scale 1 only.
    const auto zero=Folder("zero-limit","{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":0}");
    request=Request(zero);
    const auto none=Make(request);
    CHECK(none.made);
    bool one_scale=false;
    for(const auto& d:none.details)one_scale=one_scale || Contains(d,"checked_scales=1");
    CHECK(one_scale);
    // A limit of 100 % or more is not a scale range.
    const auto huge=Folder("huge-limit","{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":100}");
    request=Request(huge);
    CHECK(!Make(request).made);
    CHECK(Holds(huge,{"wpd-merge-policy.json"}));
}

void DryRun(){
    const auto folder=Folder("dry");
    auto request=Request(folder);request.dry_run=true;
    const auto result=Make(request);
    CHECK(!result.made && result.checked && Contains(result.message,"--dry-run"));
    CHECK(Holds(folder,{"wpd-merge-policy.json"}));
    // The facts it reports, with no value of any rig in them.
    std::string all;for(const auto& d:result.details)all+=d+"\n";
    CHECK(Contains(all,"profile_id=synthetic-fingerprint-draft"));
    CHECK(Contains(all,"mount_orientation=normal"));
    CHECK(Contains(all,"output_dpi=254 width_pixels=180 height_pixels=80"));
    CHECK(Contains(all,"scale_limit_percent=1 checked_scales=0.99,1,1.01"));
    CHECK(Contains(all,"band_first=0 band_last_exclusive=180 seam_column=90 seam_origin=band-center-provisional"));
    CHECK(Contains(all,"capture_equivalent_dpi_a=203.2 capture_equivalent_dpi_b=203.2")); // 8 px/mm
    // A failing check is not a pass in a dry run either.
    request.region_um={100000,100000,110000,108000};
    const auto failed=Make(request);
    CHECK(!failed.made && !failed.checked);
}

void Making(){
    const auto folder=Folder("make");
    const auto request=Request(folder);
    const auto result=Make(request);
    CHECK(result.made && result.checked && result.file==folder/m2::kFixedCompositeConditionName);
    const auto name="wpd-fixed-composite-profile-"+HashOf(draft).substr(0,16)+".json";
    CHECK(result.profile_file==folder/name);
    // Exactly the policy, the condition and the profile copy: no partial, nothing else.
    CHECK(Holds(folder,{"wpd-merge-policy.json","wpd-fixed-composite-condition.json",name}));
    CHECK(ReadAll(folder/name)==draft);
    CHECK(ReadAll(profile_path)==draft); // the source is untouched
    const auto loaded=m2::LoadFixedCompositeConditionAny(folder/m2::kFixedCompositeConditionName,kHash);
    CHECK(loaded && std::holds_alternative<m2::LoadedFixedCompositeConditionV2>(*loaded));
    if(loaded && std::holds_alternative<m2::LoadedFixedCompositeConditionV2>(*loaded)){
        const auto& v2=std::get<m2::LoadedFixedCompositeConditionV2>(*loaded);
        const auto& c=v2.condition;
        CHECK(c.source.profile_file==name && c.source.profile_file_sha256==HashOf(draft));
        CHECK(c.orientation==m2::MountOrientation::normal && c.scale_auto_correction_max_percent==1);
        CHECK(c.raster.dpi==254 && c.raster.width_pixels==180 && c.raster.height_pixels==80);
        CHECK(c.raster.region_um.left==580000 && c.raster.region_um.top==100000 && c.raster.region_um.right==598000 && c.raster.region_um.bottom==108000);
        CHECK(c.seam.column==90 && c.seam.origin==m2::FixedSeamOrigin::band_center_provisional && !c.parent_sha256);
        CHECK(c.created_utc=="2026-11-04T12:00:00Z");
        CHECK(v2.profile.profile_id && *v2.profile.profile_id=="synthetic-fingerprint-draft");
        // The record passes the whole creation check again, on what was read back.
        try{m2::MakeFixedCompositeCreationCheck()(c,v2.profile);}catch(const std::exception& e){Check(false,e.what());}
    }
    // A second run, or one with other numbers, never replaces it.
    const auto bytes=ReadAll(folder/m2::kFixedCompositeConditionName);
    const auto again=Make(request);
    CHECK(!again.made && Contains(again.message,"既に"));
    auto other=request;other.dpi=300;
    CHECK(!Make(other).made);
    CHECK(ReadAll(folder/m2::kFixedCompositeConditionName)==bytes);
    CHECK(Listing(folder).size()==3);
}

void ExistingFiles(){
    // A v1 condition stops it and is named as v1; the file is not touched.
    {
        const auto folder=Folder("has-v1");
        const std::string v1="{\"schema\":\"a0.fixed-composite-condition.v1\"}";
        WriteAll(folder/m2::kFixedCompositeConditionName,v1);
        const auto result=Make(Request(folder));
        CHECK(!result.made && Contains(result.message,"v1") && Contains(result.message,"移して"));
        CHECK(ReadAll(folder/m2::kFixedCompositeConditionName)==v1 && Listing(folder).size()==2);
    }
    // Any other file of that name stops it too.
    {
        const auto folder=Folder("has-junk");
        WriteAll(folder/m2::kFixedCompositeConditionName,"not json");
        const auto result=Make(Request(folder));
        CHECK(!result.made && Contains(result.message,"既に") && !Contains(result.message,"v1"));
        CHECK(ReadAll(folder/m2::kFixedCompositeConditionName)=="not json");
    }
    // A leftover partial of the condition stops the write; nothing replaces it.
    {
        const auto folder=Folder("has-partial");
        WriteAll(folder/"wpd-fixed-composite-condition.json.partial","someone else");
        const auto result=Make(Request(folder));
        CHECK(!result.made && !fs::exists(folder/m2::kFixedCompositeConditionName));
        CHECK(ReadAll(folder/"wpd-fixed-composite-condition.json.partial")=="someone else");
    }
    const auto name="wpd-fixed-composite-profile-"+HashOf(draft).substr(0,16)+".json";
    // The profile name taken by other bytes: refused, the other file stays, no condition.
    {
        const auto folder=Folder("name-taken");
        WriteAll(folder/name,"other bytes");
        const auto result=Make(Request(folder));
        CHECK(!result.made && Contains(result.message,"profile"));
        CHECK(ReadAll(folder/name)=="other bytes" && !fs::exists(folder/m2::kFixedCompositeConditionName));
    }
    // The same bytes already there: used as is, not rewritten.
    {
        const auto folder=Folder("copy-there");
        WriteAll(folder/name,draft);
        const auto before=fs::last_write_time(folder/name);
        const auto result=Make(Request(folder));
        CHECK(result.made && ReadAll(folder/name)==draft && fs::last_write_time(folder/name)==before);
    }
}

void Orientation(){
    const auto folder=Folder("rotated");
    WriteAll(folder/px::kMountOrientationSettingsName,kRotated);
    const auto result=Make(Request(folder));
    CHECK(result.made);
    const auto loaded=m2::LoadFixedCompositeConditionAny(folder/m2::kFixedCompositeConditionName,kHash);
    CHECK(loaded && std::holds_alternative<m2::LoadedFixedCompositeConditionV2>(*loaded) &&
        std::get<m2::LoadedFixedCompositeConditionV2>(*loaded).condition.orientation==m2::MountOrientation::rotated_180);
    bool says=false;for(const auto& d:result.details)says=says || Contains(d,"mount_orientation=rotated-180");
    CHECK(says);
}
}

int main(int argc,char* argv[]){
    try{
        if(argc!=2)throw std::invalid_argument("expected the shared rig profile v2 fixture folder");
        const fs::path fixtures=argv[1];
        draft=ReadAll(fixtures/"calibrated-draft.json");approved=ReadAll(fixtures/"approved.json");blank=ReadAll(fixtures/"template.json");
        if(draft.empty() || approved.empty() || blank.empty())throw std::runtime_error("fixture profiles are missing");
        root=TempParent()/("a0-fixed-composite-tool-v2-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root/"source");
        profile_path=root/"source"/"draft.json";WriteAll(profile_path,draft);
        Hashing();Refusals();ChecksOfTheRecord();DryRun();Making();ExistingFiles();Orientation();
        std::error_code ignored;fs::remove_all(root,ignored);
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
