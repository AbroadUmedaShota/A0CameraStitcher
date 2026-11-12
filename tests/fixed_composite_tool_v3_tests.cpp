// The development entry that makes and revises the fixed composite condition v3 (#417) and the creation checks it runs: refusals, no
// overwrite, the revision chain, the policy file, manual adjustment and rollback, migration from v2, and the checks against a brute
// force over the pose range. Camera free; every file lives in a temporary folder. Argument 1 is the shared rig profile v2 fixture folder.
// Every range, line and limit below is a value of this test; the product code has no default for any of them.
#include "a0/phase0/experimental/fixed_composite_tool_v3.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace px=a0::phase0::experimental;
namespace m2=a0::m2::experimental;
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

const auto kNow=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026}/11/10}+12h};
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
    if(fs::exists(folder))for(const auto& e:fs::directory_iterator(folder))names.insert(e.path().filename().string());
    return names;
}
bool Contains(const std::string& text,const std::string& part){return text.find(part)!=std::string::npos;}
std::string Replace(std::string text,const std::string& from,const std::string& to){
    const auto at=text.find(from);
    if(at==std::string::npos){Check(false,("test token absent: "+from).c_str());return text;}
    text.replace(at,from.size(),to);return text;
}

std::string draft;
fs::path profile_path;

// The policy file of this test. The first line of the registration part has the pose range; the rest is as the condition test's.
std::string Policy(double position_mm=5,bool parallax=true,bool surface=true,double surface_height_mm=10){
    return std::string("{\"schema\":\"a0.wpd-registration-policy.v1\",\"registration\":{\"envelope\":{\"positionMm\":")+std::to_string(position_mm)+
        ",\"rotationDeg\":1.5,\"tiltDeg\":2.5,\"heightPercent\":3},"
        "\"limits\":{\"brownLoweAlpha\":8,\"brownLoweBeta\":0.3,\"minimumUniqueBlocks\":3,\"maximumPredictionSigmaPx\":0.5,\"maximumResidualRmsPx\":0.75,"
        "\"maximumResidualP95Px\":1.5,\"maximumBinBiasPx\":0.4,\"convergencePx\":0.05,\"startAgreementPx\":0.25},"
        "\"geometry\":{\"omegaMarginPx\":2,\"guardPx\":72,\"bandHalfWidthPx\":96},\"surfaceEnvelope\":"+(surface?"{\"heightMm\":"+std::to_string(surface_height_mm)+",\"tiltDeg\":4}":"null")+
        ",\"applyBeyondEnvelope\":true,\"surfaceScaleWarnPercent\":null,\"parallax\":"+(parallax?"{\"maxLiftMm\":1}":"null")+",\"live\":null},"
        "\"seamGate\":{\"limitPx\":1,\"guardBandPx\":8},\"revisionRules\":{\"manualAdjustmentMaxSeamPx\":null}}";
}
fs::path NewFolder(const std::string& name){
    const auto root=TempParent()/("a0-fixed-tool-v3-"+name+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);return root;
}
// 18 mm x 8 mm inside the 39 mm overlap of the fixture cameras, 180 x 80 pixels at 254 DPI.
px::FixedConditionV3Request Request(const fs::path& folder,m2::FixedCompositeRevisionKind kind=m2::FixedCompositeRevisionKind::created){
    px::FixedConditionV3Request r;
    r.settings_folder=folder;r.kind=kind;r.profile=profile_path;r.region_um={580000,100000,598000,108000};r.dpi=254;
    r.layer_two=m2::LayerTwoStrainDesignV3{64.0,0.5};   // this test's design constants
    return r;
}
using Kind=m2::FixedCompositeRevisionKind;
const fs::path kRevisions=std::string(m2::kFixedCompositeRevisionFolderName);

void Refusals(){
    const auto root=NewFolder("refusals");
    // No folder.
    auto missing=Request(root/"nope");
    CHECK(!px::MakeFixedCompositeConditionV3File(missing,kHash,kNow).made);
    // No policy file, and nothing is written.
    auto r=px::MakeFixedCompositeConditionV3File(Request(root),kHash,kNow);
    CHECK(!r.made && !r.checked && Contains(r.message,"方針ファイル") && Listing(root).empty());
    // A broken policy: unknown member, unset value, null where a range is needed.
    WriteAll(root/"wpd-registration-policy.json",Replace(Policy(),"\"registration\":{","\"registration\":{\"extra\":1,"));
    r=px::MakeFixedCompositeConditionV3File(Request(root),kHash,kNow);
    CHECK(!r.made && Contains(r.message,"方針ファイル") && Listing(root)==std::set<std::string>{"wpd-registration-policy.json"});
    WriteAll(root/"wpd-registration-policy.json",Replace(Policy(),"\"positionMm\":5.000000","\"positionMm\":null"));
    CHECK(!px::MakeFixedCompositeConditionV3File(Request(root),kHash,kNow).made);
    WriteAll(root/"wpd-registration-policy.json",std::string(px::kMaximumRegistrationPolicyBytes+1,' '));
    CHECK(!px::MakeFixedCompositeConditionV3File(Request(root),kHash,kNow).made);
    WriteAll(root/"wpd-registration-policy.json",Policy());
    // Layer 2 is enabled but the design constants were not given: fail closed.
    auto no_design=Request(root);no_design.layer_two.reset();
    r=px::MakeFixedCompositeConditionV3File(no_design,kHash,kNow);
    CHECK(!r.made && Contains(r.message,"検査に通りませんでした") && !fs::exists(root/kRevisions));
    // A layer-2 design whose strain bound the lift line breaks.
    auto strict=Request(root);strict.layer_two=m2::LayerTwoStrainDesignV3{64.0,0.01};
    CHECK(!px::MakeFixedCompositeConditionV3File(strict,kHash,kNow).made);
    // Bad inputs of the raster and the profile.
    auto bad=Request(root);bad.region_um={598000,100000,580000,108000};CHECK(!px::MakeFixedCompositeConditionV3File(bad,kHash,kNow).made);
    bad=Request(root);bad.dpi=0;CHECK(!px::MakeFixedCompositeConditionV3File(bad,kHash,kNow).made);
    bad=Request(root);bad.profile=root/"no-profile.json";CHECK(!px::MakeFixedCompositeConditionV3File(bad,kHash,kNow).made);
    bad=Request(root);bad.region_um={100000,100000,110000,108000};   // outside B's cover
    CHECK(!px::MakeFixedCompositeConditionV3File(bad,kHash,kNow).made);
    // A later kind needs a revision in force; saved-estimate needs the v8 reader.
    for(const auto kind:{Kind::values,Kind::manual_adjustment,Kind::rollback}){
        auto later=Request(root,kind);later.entered=m2::FixedCompositeRevisionChangeV3::Entered{1,0,0};later.restore_sequence=1;
        r=px::MakeFixedCompositeConditionV3File(later,kHash,kNow);
        CHECK(!r.made && Contains(r.message,"改訂がまだありません"));
    }
    r=px::MakeFixedCompositeConditionV3File(Request(root,Kind::saved_estimate),kHash,kNow);
    CHECK(!r.made && Contains(r.message,"saved-estimate"));
    // A condition file of an older version is not replaced.
    WriteAll(root/m2::kFixedCompositeConditionName,"{\"schema\":\"a0.fixed-composite-condition.v1\"}");
    r=px::MakeFixedCompositeConditionV3File(Request(root),kHash,kNow);
    CHECK(!r.made && Contains(r.message,"v1"));
    WriteAll(root/m2::kFixedCompositeConditionName,"{\"schema\":\"a0.fixed-composite-condition.v2\"}");
    r=px::MakeFixedCompositeConditionV3File(Request(root),kHash,kNow);
    CHECK(!r.made && Contains(r.message,"固定の合成条件が既にあります"));
    CHECK(!fs::exists(root/kRevisions));
    std::error_code ignored;fs::remove_all(root,ignored);
}

void Chain(){
    const auto root=NewFolder("chain");
    WriteAll(root/"wpd-registration-policy.json",Policy());
    // A dry run checks everything and writes nothing.
    auto dry=Request(root);dry.dry_run=true;
    auto r=px::MakeFixedCompositeConditionV3File(dry,kHash,kNow);
    CHECK(r.checked && !r.made && Listing(root)==std::set<std::string>{"wpd-registration-policy.json"});
    CHECK(Contains(r.message,"dry-run"));
    // r000001.
    r=px::MakeFixedCompositeConditionV3File(Request(root),kHash,kNow);
    Check(r.made && r.checked,("created: "+r.message).c_str());
    const auto first_file=root/kRevisions/"r000001.json";
    CHECK(fs::exists(first_file) && r.file==first_file);
    CHECK(Listing(root).size()==3);   // policy, the profile copy, the revisions folder
    const auto first_text=ReadAll(first_file);
    {
        const auto active=m2::LoadActiveFixedCompositeRevisionV3(root/kRevisions,kHash);
        CHECK(active && active->condition.revision.sequence==1 && active->condition.revision.change.kind==Kind::created && active->condition.base_pose.IsZero());
        CHECK(active && active->condition.source.policy_sha256==HashOf(ReadAll(root/"wpd-registration-policy.json")));
        CHECK(active && active->condition.raster.width_pixels==180 && active->condition.seam.column==90 && active->condition.seam.origin==m2::FixedSeamOrigin::band_center_provisional);
        CHECK(active && active->condition.registration.parallax && active->condition.registration.parallax->max_lift_mm==1 && active->condition.registration.surface_envelope);
        CHECK(active && !active->condition.registration.surface_scale_warn_percent && !active->condition.revision_rules.manual_adjustment_max_seam_px);
        // The record is not a v2 record: no scale limit.
        CHECK(!Contains(first_text,"scaleAutoCorrection"));
    }
    bool has_details=false;
    for(const auto& d:r.details)if(Contains(d,"k_px_per_mm="))has_details=true;
    CHECK(has_details);
    // Never overwrite: r000001 is not made again, and the bytes survive.
    r=px::MakeFixedCompositeConditionV3File(Request(root),kHash,kNow);
    CHECK(!r.made && ReadAll(first_file)==first_text);

    // values: the same policy has nothing to write; a changed one is revision 2.
    r=px::MakeFixedCompositeConditionV3File(Request(root,Kind::values),kHash,kNow);
    CHECK(!r.made && Contains(r.message,"同じ"));
    WriteAll(root/"wpd-registration-policy.json",Policy(6));
    r=px::MakeFixedCompositeConditionV3File(Request(root,Kind::values),kHash,kNow);
    Check(r.made,("values: "+r.message).c_str());
    {
        const auto active=m2::LoadActiveFixedCompositeRevisionV3(root/kRevisions,kHash);
        CHECK(active && active->condition.revision.sequence==2 && active->condition.revision.parent_sha256==HashOf(first_text));
        CHECK(active && active->condition.registration.envelope.position_mm==6 && active->condition.source.policy_sha256==HashOf(ReadAll(root/"wpd-registration-policy.json")));
    }
    CHECK(ReadAll(first_file)==first_text);

    // manual adjustment: the displacement entered is the displacement of the new base pose at the seam's middle; there is no cap on its size.
    const auto before_manual=m2::LoadActiveFixedCompositeRevisionV3(root/kRevisions,kHash);
    auto manual=Request(root,Kind::manual_adjustment);
    manual.entered=m2::FixedCompositeRevisionChangeV3::Entered{12.5,-7.25,0.1};
    r=px::MakeFixedCompositeConditionV3File(manual,kHash,kNow);
    Check(r.made,("manual: "+r.message).c_str());
    {
        const auto active=m2::LoadActiveFixedCompositeRevisionV3(root/kRevisions,kHash);
        CHECK(active && active->condition.revision.sequence==3 && active->condition.revision.change.kind==Kind::manual_adjustment);
        if(active){
            const auto& c=active->condition;
            CHECK(c.revision.change.entered && c.revision.change.entered->seam_dx_px==12.5 && c.revision.change.entered->rotation_deg==0.1);
            CHECK(c.base_pose.rz_deg==0.1 && c.base_pose.tx_mm!=0 && c.base_pose.ty_mm!=0 && c.base_pose.rx_deg==0 && c.base_pose.tz_percent==0);
            namespace d=px::fixed_tool_v3_detail;
            const d::cr::PoseModel model(active->profile.parameters.camera_b);
            const auto frame=d::cr::FrameOfRaster(active->profile.parameters.raster);
            const auto at=d::RelativeDisplacement(model,frame,before_manual->condition.base_pose,c.base_pose,c.seam.column,(c.raster.height_pixels-1)/2.0);
            CHECK(std::abs(at.x-12.5)<1e-6 && std::abs(at.y+7.25)<1e-6);
            CHECK(c.revision.change.maximum_seam_displacement_px && *c.revision.change.maximum_seam_displacement_px>=std::hypot(12.5,7.25)-1.0);
            // The range and the lines are carried, the policy hash is the earlier revision's.
            CHECK(c.registration.envelope.position_mm==6 && c.source.policy_sha256==before_manual->condition.source.policy_sha256);
        }
    }
    // A very large adjustment is not refused for its size; it is refused only when the base pose leaves the pose range, and nothing is written.
    {
        const auto listing=Listing(root/kRevisions);
        auto huge=Request(root,Kind::manual_adjustment);
        huge.entered=m2::FixedCompositeRevisionChangeV3::Entered{4000,0,0};
        r=px::MakeFixedCompositeConditionV3File(huge,kHash,kNow);
        CHECK(!r.made && Contains(r.message,"作りません") && Listing(root/kRevisions)==listing);
        // Not a number: refused.
        huge.entered=m2::FixedCompositeRevisionChangeV3::Entered{std::nan(""),0,0};
        CHECK(!px::MakeFixedCompositeConditionV3File(huge,kHash,kNow).made);
        auto none=Request(root,Kind::manual_adjustment);
        CHECK(!px::MakeFixedCompositeConditionV3File(none,kHash,kNow).made);
        // A large adjustment that stays inside the pose range is made: 28 px at this raster is about 2.8 mm.
        auto large=Request(root,Kind::manual_adjustment);
        large.entered=m2::FixedCompositeRevisionChangeV3::Entered{-20,15,0};
        r=px::MakeFixedCompositeConditionV3File(large,kHash,kNow);
        Check(r.made,("large manual: "+r.message).c_str());
    }
    // rollback: a new revision that carries the content of an earlier one; nothing is removed.
    const auto texts_before=[&]{std::vector<std::string> t;for(int i=1;i<=4;++i)t.push_back(ReadAll(root/kRevisions/m2::FixedCompositeRevisionFileName(i)));return t;}();
    for(const std::uint32_t bad_sequence:{0u,4u,5u,6u,100u}){
        auto bad=Request(root,Kind::rollback);bad.restore_sequence=bad_sequence;
        CHECK(!px::MakeFixedCompositeConditionV3File(bad,kHash,kNow).made);
    }
    auto back=Request(root,Kind::rollback);back.restore_sequence=1;
    r=px::MakeFixedCompositeConditionV3File(back,kHash,kNow);
    Check(r.made,("rollback: "+r.message).c_str());
    {
        const auto active=m2::LoadActiveFixedCompositeRevisionV3(root/kRevisions,kHash);
        CHECK(active && active->condition.revision.sequence==5 && active->condition.revision.change.kind==Kind::rollback);
        CHECK(active && active->condition.revision.change.restored_sha256==HashOf(first_text));
        CHECK(active && active->condition.base_pose.IsZero() && active->condition.registration.envelope.position_mm==5);
        CHECK(active && active->condition.revision.parent_sha256==HashOf(texts_before[3]));
    }
    for(int i=1;i<=4;++i)CHECK(ReadAll(root/kRevisions/m2::FixedCompositeRevisionFileName(i))==texts_before[i-1]);
    // Going back to the revision in force's own content (a rollback of revision 5 from revision 5's successor) is also allowed.
    auto again=Request(root,Kind::rollback);again.restore_sequence=2;
    CHECK(px::MakeFixedCompositeConditionV3File(again,kHash,kNow).made);
    CHECK(m2::LoadActiveFixedCompositeRevisionV3(root/kRevisions,kHash)->condition.registration.envelope.position_mm==6);
    // A foreign file in the revisions folder stops every later write: the history is not guessed.
    WriteAll(root/kRevisions/"notes.txt","x");
    CHECK(!px::MakeFixedCompositeConditionV3File(Request(root,Kind::values),kHash,kNow).made);
    fs::remove(root/kRevisions/"notes.txt");
    std::error_code ignored;fs::remove_all(root,ignored);
}

void MigrationAndLayers(){
    const auto root=NewFolder("migrate");
    WriteAll(root/"wpd-registration-policy.json",Policy(5,false,false));
    // A v2 record made the old way, then moved out of the way by a person.
    m2::FixedCompositeV2Request v2_request;
    v2_request.profile_file=m2::FixedCompositeProfileFileName(HashOf(draft));v2_request.profile_bytes=draft;
    v2_request.raster=render::OutputRaster{render::DocumentRegionUm{580000,100000,598000,108000},254,180,80};
    v2_request.scale_auto_correction_max_percent=1;v2_request.seam_column=90;v2_request.orientation=m2::MountOrientation::rotated_180;
    const auto v2=m2::MakeFixedCompositeConditionV2(v2_request,kNow,kHash,[](const m2::FixedCompositeConditionV2&,const m2::FixedCompositeProfileBinding&){});
    WriteAll(root/"moved-v2.json",m2::FormatFixedCompositeConditionV2(v2)+"\n");
    (void)m2::CopyFixedCompositeProfile(root,draft,kHash);
    auto request=Request(root,Kind::migrated_from_v2);request.v2_condition=root/"moved-v2.json";
    // Without the file it is refused; a v2 file in place stops it; layers without a range are written as null.
    auto no_file=request;no_file.v2_condition=root/"absent.json";
    CHECK(!px::MakeFixedCompositeConditionV3File(no_file,kHash,kNow).made);
    auto r=px::MakeFixedCompositeConditionV3File(request,kHash,kNow);
    Check(r.made,("migrated: "+r.message).c_str());
    const auto active=m2::LoadActiveFixedCompositeRevisionV3(root/kRevisions,kHash);
    CHECK(active && active->condition.revision.change.kind==Kind::migrated_from_v2 && active->condition.revision.change.v2_sha256==HashOf(ReadAll(root/"moved-v2.json")));
    if(active){
        const auto& c=active->condition;
        CHECK(c.orientation==m2::MountOrientation::rotated_180 && c.seam.column==90 && c.raster.width_pixels==180 && c.base_pose.IsZero());
        CHECK(!c.registration.surface_envelope && !c.registration.parallax);
        CHECK(!Contains(ReadAll(root/kRevisions/"r000001.json"),"scaleAutoCorrection"));
    }
    // Without a layer-2 line no design is needed.
    auto second=NewFolder("nolayer");
    WriteAll(second/"wpd-registration-policy.json",Policy(5,false,false));
    auto plain=Request(second);plain.layer_two.reset();
    Check(px::MakeFixedCompositeConditionV3File(plain,kHash,kNow).made,"layers off need no design");
    std::error_code ignored;fs::remove_all(root,ignored);fs::remove_all(second,ignored);
}

// The check against a brute force: the pose range is walked on a fixed set of poses with the exact scan of v2 (RequireFixedCompositeSeam
// and RequireFixedCompositeCoverage), and the check must agree at a range 20 percent smaller and 20 percent larger than the one the
// brute force says is the last that holds.
bool BruteForceHolds(const m2::FixedCompositeProfileBinding& binding,std::uint32_t seam,double position_mm,double tilt_deg,double rotation_deg,double height_percent){
    namespace cr=m2::content_registration;
    constexpr double kPi=3.14159265358979323846;
    std::vector<cr::PoseParameters> poses;
    for(int a=0;a<8;++a){
        const double tx=position_mm*std::cos(a*kPi/4),ty=position_mm*std::sin(a*kPi/4);
        for(const double s:{-1.0,1.0})for(const double t:{-1.0,1.0})
            for(int b=0;b<4;++b)
                poses.push_back({tilt_deg*std::cos(b*kPi/2),tilt_deg*std::sin(b*kPi/2),s*rotation_deg,tx,ty,t*height_percent});
    }
    for(const auto& pose:poses){
        try{
            const auto input=m2::PoseCorrectedDocumentMappedFixedSeamInput(binding.parameters,pose);
            m2::RequireFixedCompositeCoverage(input);
            m2::RequireFixedCompositeSeam(input,seam);
        }catch(const std::invalid_argument&){return false;}
    }
    return true;
}
bool BruteForceHoldsSurface(const m2::FixedCompositeProfileBinding& binding,std::uint32_t seam,double height_mm,double tilt_deg){
    constexpr double kPi=3.14159265358979323846;
    const double slope=std::tan(tilt_deg*kPi/180);
    for(int a=0;a<8;++a)for(const double u:{-1.0,0.0,1.0}){
        m2::RegisteredMapping mapping;
        mapping.surface.h0_mm=u*height_mm;mapping.surface.slope_x=slope*std::cos(a*kPi/4);mapping.surface.slope_y=slope*std::sin(a*kPi/4);
        try{
            const auto input=m2::RegisteredDocumentMappedFixedSeamInput(binding.parameters,mapping,seam);
            m2::RequireFixedCompositeCoverage(input);
            m2::RequireFixedCompositeSeam(input,seam);
        }catch(const std::invalid_argument&){return false;}
    }
    return true;
}
void AgainstBruteForce(){
    const auto root=NewFolder("brute");
    WriteAll(root/"wpd-registration-policy.json",Policy());
    // A region whose left edge is close to B's cover: B covers document x above 561 mm, the region starts at 565 mm.
    auto request=Request(root);request.region_um={565000,100000,575000,108000};
    auto layer_off=Policy(5,false,true);
    WriteAll(root/"wpd-registration-policy.json",layer_off);
    request.layer_two.reset();
    const auto parameters=a0::m2::RigProfileV2::Parse(draft).ValidateCalibrationForEvaluation(
        render::OutputRaster{render::DocumentRegionUm{565000,100000,575000,108000},254,100,80},render::Resampling::bilinear);
    m2::FixedCompositeProfileBinding binding{parameters,std::nullopt,HashOf(draft),std::string(64,'0')};
    const auto seam=m2::FixedCompositeProvisionalSeamColumn(parameters);
    // The largest position range that the brute force accepts (others fixed), by bisection.
    double lo=0.1,hi=40;
    for(int i=0;i<10;++i){
        const double mid=(lo+hi)/2;
        if(BruteForceHolds(binding,seam,mid,0.5,0.5,0.5))lo=mid;else hi=mid;
    }
    const double last_good=lo;
    CHECK(last_good>1 && last_good<30);
    (void)root;
    const auto policy_with=[&](double position){
        auto text=Policy(position,false,false);   // the brute force over the pose range has no paper surface
        text=Replace(text,"\"rotationDeg\":1.5,\"tiltDeg\":2.5,\"heightPercent\":3","\"rotationDeg\":0.5,\"tiltDeg\":0.5,\"heightPercent\":0.5");
        return text;
    };
    // The record has to be made with a pose range that holds the base pose (zero), which any range does.
    const auto run=[&](double position){
        WriteAll(root/"wpd-registration-policy.json",policy_with(position));
        auto r=request;
        // A new folder per try: nothing is overwritten.
        const auto folder=NewFolder("brute-try");
        WriteAll(folder/"wpd-registration-policy.json",policy_with(position));
        r.settings_folder=folder;r.dry_run=true;
        const auto result=px::MakeFixedCompositeConditionV3File(r,kHash,kNow);
        std::error_code ignored;fs::remove_all(folder,ignored);
        return result;
    };
    const auto inside=run(last_good*0.8),outside=run(last_good*1.2);
    Check(inside.checked,("accepts 80 percent of the brute force's last good range: "+inside.message).c_str());
    Check(!outside.checked,"refuses 120 percent of the brute force's last good range");
    // The same for the paper surface's range (the pose range kept small).
    {
        double slo=0.5,shi=60;
        for(int i=0;i<10;++i){
            const double mid=(slo+shi)/2;
            if(BruteForceHoldsSurface(binding,seam,mid,2))slo=mid;else shi=mid;
        }
        const auto surface_run=[&](double height){
            const auto folder=NewFolder("brute-surface");
            auto text=Policy(0.5,false,true,height);
            text=Replace(text,"\"rotationDeg\":1.5,\"tiltDeg\":2.5,\"heightPercent\":3","\"rotationDeg\":0.1,\"tiltDeg\":0.1,\"heightPercent\":0.1");
            text=Replace(text,"\"tiltDeg\":4}","\"tiltDeg\":2}");
            WriteAll(folder/"wpd-registration-policy.json",text);
            auto r=request;r.settings_folder=folder;r.dry_run=true;
            const auto result=px::MakeFixedCompositeConditionV3File(r,kHash,kNow);
            std::error_code ignored;fs::remove_all(folder,ignored);
            return result;
        };
        CHECK(slo>1 && slo<50);
        const auto inside_surface=surface_run(slo*0.7),outside_surface=surface_run(slo*1.3);
        Check(inside_surface.checked,("accepts 70 percent of the brute force's last good surface height: "+inside_surface.message).c_str());
        Check(!outside_surface.checked,"refuses 130 percent of the brute force's last good surface height");
        bool reported=false;
        for(const auto& d:inside_surface.details)if(d.find("surface_range_outer_edge_uncovered=")!=std::string::npos)reported=true;
        CHECK(reported);
    }
    // A broken range far outside is refused with a reason that names the row and the check.
    const auto far=run(last_good*3);
    CHECK(!far.checked && Contains(far.message,"row "));
    std::error_code ignored;fs::remove_all(root,ignored);
}

// The same check on records made without the tool: a base pose, a lift line, a provisional seam column that is not the band middle.
void CreationCheckDirect(){
    m2::FixedCompositeV3Request request;
    request.profile_file="wpd-fixed-composite-profile-test.json";request.profile_bytes=draft;request.policy_sha256=std::string(64,'d');
    request.sequence=1;request.change.kind=Kind::created;
    request.raster=render::OutputRaster{render::DocumentRegionUm{580000,100000,598000,108000},254,180,80};
    request.seam_column=90;request.seam_origin=m2::FixedSeamOrigin::band_center_provisional;
    request.registration.envelope={5,1.5,2.5,3};
    request.registration.limits.brown_lowe_alpha=8;request.registration.limits.brown_lowe_beta=0.3;request.registration.limits.minimum_unique_blocks=3;
    request.registration.limits.maximum_prediction_sigma_px=0.5;request.registration.limits.maximum_residual_rms_px=0.75;request.registration.limits.maximum_residual_p95_px=1.5;
    request.registration.limits.maximum_bin_bias_px=0.4;request.registration.limits.convergence_px=0.05;request.registration.limits.start_agreement_px=0.25;
    request.registration.geometry={2,72,96};
    request.registration.surface_envelope=m2::SurfaceEnvelope{10,4};request.registration.apply_beyond_envelope=true;
    request.registration.parallax=m2::FixedCompositeConditionV3::Registration::Parallax{1.0};
    request.seam_gate={1.0,8.0};
    m2::CreationCheckDetailsV3 details;
    const m2::CreationCheckOptionsV3 options{m2::LayerTwoStrainDesignV3{64.0,0.5}};
    const auto make=[&](const m2::FixedCompositeV3Request& r,const m2::CreationCheckOptionsV3& o){
        return m2::MakeFixedCompositeConditionV3(r,kNow,kHash,m2::MakeFixedCompositeCreationCheckV3(&details,o));
    };
    const auto c=make(request,options);
    CHECK(details.lift_cap_px>0 && details.maximum_strain>0 && details.maximum_strain<=0.5);
    // k and the cap follow the design: cap = maxLiftMm x k; strain = (pi/2) cap / L.
    {
        const auto binding=m2::BindFixedCompositeProfileV3(c,draft,kHash);
        const auto g=m2::BaselineGeometryOfCondition(c,binding);
        CHECK(std::abs(g.e_b.x*g.e_b.x+g.e_b.y*g.e_b.y-1)<1e-12 && g.baseline_mm>0 && g.k_px_per_mm>0);
        CHECK(std::abs(details.lift_cap_px-1.0*g.k_px_per_mm)<1e-9);
        CHECK(std::abs(details.maximum_strain-3.14159265358979323846/2*details.lift_cap_px/64.0)<1e-9);
        // The fixture: A and B are 575 mm apart along x at 1250 mm over the table, 10 output px per mm: k = 10 * 575 / (1250 - 10).
        CHECK(std::abs(g.e_b.x-1)<1e-9 && std::abs(g.baseline_mm-575)<1e-6 && std::abs(g.k_px_per_mm-10.0*575.0/(1250.0-10.0))<1e-6);
    }
    // Strain bound: a shorter falloff or a larger lift breaks it.
    REJECT(make(request,m2::CreationCheckOptionsV3{m2::LayerTwoStrainDesignV3{4.0,0.5}}));
    REJECT(make(request,m2::CreationCheckOptionsV3{m2::LayerTwoStrainDesignV3{std::nan(""),0.5}}));
    REJECT(make(request,m2::CreationCheckOptionsV3{m2::LayerTwoStrainDesignV3{64.0,0}}));
    REJECT(make(request,m2::CreationCheckOptionsV3{}));
    auto big_lift=request;big_lift.registration.parallax=m2::FixedCompositeConditionV3::Registration::Parallax{50.0};
    REJECT(make(big_lift,options));
    // Layer 2 off: no design needed and no cap.
    auto off=request;off.registration.parallax=std::nullopt;
    (void)make(off,m2::CreationCheckOptionsV3{});
    CHECK(details.lift_cap_px==0);
    // A provisional seam column that is not the band middle; an operator column is only checked for cover.
    auto moved=request;moved.seam_column=91;
    REJECT(make(moved,options));
    moved.seam_origin=m2::FixedSeamOrigin::operator_set;
    (void)make(moved,options);
    // A base pose near the edge of the range is checked from there: the range around it reaches the cover's edge sooner.
    auto edge=request;edge.sequence=2;edge.parent_sha256=std::string(64,'c');edge.change.kind=Kind::values;edge.base_pose={0,0,0,0,4.9,0};
    (void)make(edge,options);
    // The cover of the region near B's edge: a range that moves B past the seam column minus half the blend width is refused.
    auto near=request;near.raster=render::OutputRaster{render::DocumentRegionUm{565000,100000,575000,108000},254,100,80};near.seam_column=50;
    near.registration.parallax=std::nullopt;near.registration.surface_envelope=std::nullopt;near.registration.envelope={1,0.5,0.5,0.5};
    (void)make(near,options);
    near.registration.envelope.position_mm=30;
    REJECT(make(near,options));
    // The paper surface's range lowers the same margin: a surface that can be lifted by 2 mm leaves it, one by 20 mm does not.
    near.registration.envelope={0.5,0.2,0.2,0.2};
    near.registration.surface_envelope=m2::SurfaceEnvelope{2,2};
    (void)make(near,options);
    CHECK(details.details.size()>=1);
    near.registration.surface_envelope=m2::SurfaceEnvelope{20,2};
    REJECT(make(near,options));
    // Without a range for the surface the surface is not part of the check.
    near.registration.surface_envelope=std::nullopt;
    (void)make(near,options);
    bool outer_edge_reported=false;
    for(const auto& line:details.details)if(line.find("surface_range_outer_edge_uncovered=disabled")!=std::string::npos)outer_edge_reported=true;
    CHECK(outer_edge_reported);
    // The per-capture cheap check: the seam and the outer edge at the base pose.
    {
        const auto binding=m2::BindFixedCompositeProfileV3(c,draft,kHash);
        m2::RegisteredMapping mapping;
        const auto cover=m2::CheckRegisteredMappingCover(binding.parameters,mapping,c.seam.column,m2::kSeamBlendDesign);
        CHECK(cover.seam_covered && cover.output_covered && cover.uncovered_edge_pixels==0);
        // A pose that pushes B's cover over the seam column.
        bool lost_somewhere=false;
        for(const double shift:{-60.0,60.0}){
            m2::RegisteredMapping far;far.pose.ty_mm=shift;
            lost_somewhere=lost_somewhere || !m2::CheckRegisteredMappingCover(binding.parameters,far,c.seam.column,m2::kSeamBlendDesign).seam_covered;
        }
        CHECK(lost_somewhere);
        // A paper surface goes through the registered mapping maker: a small one keeps the seam and the edge covered, a mapping that moves B
        // over the seam does not, and the zero surface is the pose-only answer.
        m2::RegisteredMapping surface;surface.surface.h0_mm=5;
        const auto lifted=m2::CheckRegisteredMappingCover(binding.parameters,surface,c.seam.column,m2::kSeamBlendDesign);
        CHECK(lifted.seam_covered && lifted.output_covered);
        m2::RegisteredMapping zero_surface;
        CHECK(m2::CheckRegisteredMappingCover(binding.parameters,zero_surface,c.seam.column,m2::kSeamBlendDesign).seam_covered==cover.seam_covered);
        bool refused_or_lost=false;
        for(const double sign:{-1.0,1.0}){
            m2::RegisteredMapping too_high;too_high.surface.h0_mm=sign*60;too_high.pose.ty_mm=sign*8;
            try{refused_or_lost=refused_or_lost || !m2::CheckRegisteredMappingCover(binding.parameters,too_high,c.seam.column,m2::kSeamBlendDesign).seam_covered;}
            catch(const std::invalid_argument&){refused_or_lost=true;}
        }
        CHECK(refused_or_lost);
    }
}
}

int main(int argc,char* argv[]){
    try{
        if(argc!=2)throw std::invalid_argument("expected the shared rig profile v2 fixture folder");
        const fs::path fixtures=argv[1];
        profile_path=fixtures/"calibrated-draft.json";
        draft=ReadAll(profile_path);
        if(draft.empty())throw std::runtime_error("fixture profile is missing");
        const auto step=[](const char* name){std::cerr<<"["<<name<<"]"<<std::endl;};
        step("refusals");Refusals();step("chain");Chain();step("migration");MigrationAndLayers();step("direct");CreationCheckDirect();step("brute force");AgainstBruteForce();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
