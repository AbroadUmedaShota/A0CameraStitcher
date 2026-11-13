// Recalibration of camera B from one pair (#434, docs/design/recalibration-from-pair.md): the revision kind `recalibrated` of the condition v3
// (format, strict reader, the chain on disk, what the old readers do with a kind they do not know), the pose baked into a new draft rig
// profile, the raster and the seam of the new revision, the creation check, and the whole path on rendered synthetic pairs.
// Camera free; files go to a temporary folder only. Argument 1 is the shared rig profile v2 fixture folder.
// Every range, line, limit and tolerance below is a value of this test; the product code has no default for any of them. The rig is
// synthetic (two cameras side by side, 10 pixels per mm) and the paper is a generated texture: nothing here is a measurement.
//
// The cases that render pairs can be narrowed while working on them: A0_RECAL_CASES holds a comma-separated list of case names.
#include "a0/common/protocol_json.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/m2/experimental/fixed_composite_recalibration.hpp"
#include "a0/m2/experimental/fixed_composite_selection.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/phase0/experimental/fixed_composite_cli_v3.hpp"
#include "a0/phase0/experimental/fixed_composite_tool_v3.hpp"
#include "a0/phase0/experimental/pair_review_synthetic.hpp"
#include "registration_fixtures.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace a0::m2::experimental;
namespace rc=a0::m2::experimental::recalibration;
namespace cr=a0::m2::experimental::content_registration;
namespace render=a0::m2::render;
namespace ps=a0::phase0::experimental::pair_synthetic;
namespace px=a0::phase0::experimental;
namespace fx=a0::tests::registration_fixtures;
namespace fs=std::filesystem;
namespace json=a0::common::protocol_json;
using namespace std::chrono_literals;
using a0::m2::RigProfileV2;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr)
template<class Call> bool Throws(Call call){
    try{call();}catch(const std::invalid_argument&){return true;}catch(const std::runtime_error&){return true;}
    return false;
}
#define REJECT(expr) Check(Throws([&]{(void)(expr);}),"rejects: " #expr)
void CheckNear(double value,double expected,double tolerance,const std::string& what){
    ++checks;
    if(!(std::abs(value-expected)<=tolerance)){++failures;std::cerr<<"FAILED: "<<what<<": "<<value<<" expected "<<expected<<" +/- "<<tolerance<<'\n';}
}

const auto kNow=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2000}/1/1}+12h};
const FixedCompositeSha256 kHash=ps::Hash();
std::string HashOf(const std::string& text){return kHash(std::vector<unsigned char>(text.begin(),text.end()));}
std::string ReadAll(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void WriteAll(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;}
std::string Replace(std::string text,const std::string& from,const std::string& to){
    const auto at=text.find(from);
    if(at==std::string::npos){Check(false,("test token absent: "+from).c_str());return text;}
    text.replace(at,from.size(),to);return text;
}
bool Contains(const std::string& text,const std::string& part){return text.find(part)!=std::string::npos;}
std::set<std::string> Listing(const fs::path& folder){
    std::set<std::string> names;
    if(fs::exists(folder))for(const auto& e:fs::directory_iterator(folder))names.insert(e.path().filename().string());
    return names;
}
fs::path TempParent(){
    char* value=nullptr;size_t length=0;
    if(_dupenv_s(&value,&length,"A0_TEST_TMPDIR")==0 && value!=nullptr){
        const std::string text(value);std::free(value);
        if(!text.empty())return fs::path(text);
    }
    return fs::temp_directory_path();
}
fs::path NewFolder(const std::string& name){
    const auto root=TempParent()/("a0-recal-"+name+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);return root;
}
bool SameBits(double a,double b){return std::memcmp(&a,&b,sizeof a)==0;}
std::string fixture;   // the calibrated draft of the shared rig profile fixtures

// ---- Section 1. The revision kind ------------------------------------------------------------------------------------------
void Kinds(){
    CHECK(FixedCompositeRevisionKindText(FixedCompositeRevisionKind::recalibrated)=="recalibrated");
    CHECK(ParseFixedCompositeRevisionKind("recalibrated")==FixedCompositeRevisionKind::recalibrated);
    for(const auto k:{FixedCompositeRevisionKind::created,FixedCompositeRevisionKind::migrated_from_v2,FixedCompositeRevisionKind::saved_estimate,
        FixedCompositeRevisionKind::manual_adjustment,FixedCompositeRevisionKind::rollback,FixedCompositeRevisionKind::values,FixedCompositeRevisionKind::recalibrated})
        CHECK(ParseFixedCompositeRevisionKind(FixedCompositeRevisionKindText(k))==k);
    // The kinds before it mean what they meant.
    CHECK(FixedCompositeRevisionKindText(FixedCompositeRevisionKind::created)=="created" && FixedCompositeRevisionKindText(FixedCompositeRevisionKind::saved_estimate)=="saved-estimate");
    CHECK(!ParseFixedCompositeRevisionKind("Recalibrated") && !ParseFixedCompositeRevisionKind("recalibrated-x") && !ParseFixedCompositeRevisionKind("re-calibrated"));
}

// A recalibrated revision through the shared fixture builder: the text the builder makes is the text the writer writes, byte for byte.
fx::ConditionV3Options RecalibratedOptions(){
    fx::ConditionV3Options o;
    o.sequence=2;o.parent_sha256=fx::Hex64('d');o.change.kind="recalibrated";o.change.report_sha256=fx::Hex64('e');o.change.pair_id="pair-0007";
    return o;
}
void FormatAndReader(){
    const auto o=RecalibratedOptions();
    const auto text=fx::ConditionV3Text(o);
    const auto c=ParseFixedCompositeConditionV3(text);
    CHECK(c.revision.sequence==2 && c.revision.change.kind==FixedCompositeRevisionKind::recalibrated);
    CHECK(c.revision.change.report_sha256==fx::Hex64('e') && c.revision.change.pair_id=="pair-0007");
    CHECK(!c.revision.change.v2_sha256 && !c.revision.change.entered && !c.revision.change.restored_sha256 && !c.revision.change.maximum_seam_displacement_px);
    CHECK(c.base_pose.IsZero());
    CHECK(FormatFixedCompositeConditionV3(c)==text);                                   // the builder's text is the writer's text
    CHECK(text.find("\"change\":{\"kind\":\"recalibrated\",\"reportSha256\":\""+fx::Hex64('e')+"\",\"pairId\":\"pair-0007\"}")!=std::string::npos);   // the order of the members
    // Rejections.
    {auto bad=o;bad.sequence=1;bad.parent_sha256=std::nullopt;REJECT(ParseFixedCompositeConditionV3(fx::ConditionV3Text(bad)));}   // revision 1 is created or migrated-from-v2
    {auto bad=o;bad.base_pose.tx_mm=1;REJECT(ParseFixedCompositeConditionV3(fx::ConditionV3Text(bad)));}                              // the pose is in the profile
    {auto bad=o;bad.base_pose.rz_deg=0.001;REJECT(ParseFixedCompositeConditionV3(fx::ConditionV3Text(bad)));}
    {auto bad=o;bad.change.report_sha256="abc";REJECT(ParseFixedCompositeConditionV3(fx::ConditionV3Text(bad)));}
    {auto bad=o;bad.change.pair_id="not a pair";REJECT(ParseFixedCompositeConditionV3(fx::ConditionV3Text(bad)));}
    REJECT(ParseFixedCompositeConditionV3(Replace(text,",\"pairId\":\"pair-0007\"","")));                                     // a member missing
    REJECT(ParseFixedCompositeConditionV3(Replace(text,"\"pairId\":\"pair-0007\"}","\"pairId\":\"pair-0007\",\"v2Sha256\":\""+fx::Hex64('1')+"\"}")));   // a member of another kind
    REJECT(ParseFixedCompositeConditionV3(Replace(text,"\"reportSha256\":","\"reportSha257\":")));
    REJECT(ParseFixedCompositeConditionV3(Replace(text,"\"kind\":\"recalibrated\"","\"kind\":\"recalibrated-x\"")));          // a kind that no reader knows
    // The writer refuses what the reader refuses.
    {auto bad=c;bad.base_pose.tx_mm=1;REJECT(FormatFixedCompositeConditionV3(bad));}
    {auto bad=c;bad.revision.sequence=1;bad.revision.parent_sha256=std::nullopt;REJECT(FormatFixedCompositeConditionV3(bad));}
    {auto bad=c;bad.revision.change.pair_id=std::nullopt;REJECT(FormatFixedCompositeConditionV3(bad));}
    {auto bad=c;bad.revision.change.v2_sha256=fx::Hex64('1');REJECT(FormatFixedCompositeConditionV3(bad));}
    // A first revision is still created or migrated-from-v2 only, and the kinds' members are unchanged.
    {auto bad=fx::ConditionV3Options{};bad.change.kind="recalibrated";REJECT(ParseFixedCompositeConditionV3(fx::ConditionV3Text(bad)));}
    {auto saved=o;saved.change.kind="saved-estimate";CHECK(ParseFixedCompositeConditionV3(fx::ConditionV3Text(saved)).revision.change.kind==FixedCompositeRevisionKind::saved_estimate);}
}

// ---- A settings folder with a chain whose second revision recalibrates ---------------------------------------------------------
std::string Draft(){return fixture;}
// 18 mm x 8 mm of the sheet at 254 DPI (0.1 mm per pixel), inside the 39 mm overlap of the fixture cameras: 180 x 80 pixels.
render::OutputRaster SmallRaster(){return render::OutputRaster{render::DocumentRegionUm{580000,100000,598000,108000},254,180,80};}
std::uint32_t SeamOf(const std::string& profile){
    return FixedCompositeProvisionalSeamColumn(RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(SmallRaster(),render::Resampling::bilinear));
}
FixedCompositeV3Request ChainRequest(const std::string& profile){
    FixedCompositeV3Request r;
    r.profile_bytes=profile;r.profile_file=FixedCompositeProfileFileName(HashOf(profile));r.policy_sha256=std::string(64,'d');
    r.sequence=1;r.change.kind=FixedCompositeRevisionKind::created;
    r.raster=SmallRaster();r.seam_column=SeamOf(profile);r.seam_origin=FixedSeamOrigin::band_center_provisional;
    r.registration.envelope.position_mm=5;r.registration.envelope.rotation_deg=1.5;r.registration.envelope.tilt_deg=2.5;r.registration.envelope.height_percent=3;
    r.registration.limits.brown_lowe_alpha=8;r.registration.limits.brown_lowe_beta=0.3;r.registration.limits.minimum_unique_blocks=3;
    r.registration.limits.maximum_prediction_sigma_px=0.5;r.registration.limits.maximum_residual_rms_px=0.75;r.registration.limits.maximum_residual_p95_px=1.5;
    r.registration.limits.maximum_bin_bias_px=0.4;r.registration.limits.convergence_px=0.05;r.registration.limits.start_agreement_px=0.25;
    r.registration.geometry={2,72,96};
    r.registration.surface_envelope=SurfaceEnvelope{10,4};r.registration.apply_beyond_envelope=true;
    r.registration.parallax=FixedCompositeConditionV3::Registration::Parallax{1.0};
    r.seam_gate={1.0,8.0};
    return r;
}
const FixedCompositeCreationCheckV3 kAccept=[](const FixedCompositeConditionV3&,const FixedCompositeProfileBinding&){};

rc::Provenance SampleProvenance(const std::string& name){
    return rc::Provenance{"pair-reanchor-"+name,"2000-01-01T00:00:00Z",std::string(64,'b'),rc::ProfileFit{0.25,0.75,40}};
}

struct Chain {
    fs::path settings,revisions;
    std::string profile_one,profile_two;
    std::vector<std::string> texts;
    std::vector<FixedCompositeConditionV3> records;
};
FixedCompositeConditionV3 AppendRecord(Chain& chain,FixedCompositeV3Request request){
    request.sequence=static_cast<std::uint32_t>(chain.texts.size()+1);
    request.parent_sha256=chain.texts.empty()?std::nullopt:std::optional<std::string>(HashOf(chain.texts.back()));
    const auto c=MakeFixedCompositeConditionV3(request,kNow,kHash,kAccept);
    CopyFixedCompositeProfile(chain.settings,request.profile_bytes,kHash);
    WriteFixedCompositeConditionV3(chain.revisions,c);
    chain.texts.push_back(ReadAll(chain.revisions/FixedCompositeRevisionFileName(c.revision.sequence)));
    chain.records.push_back(c);
    return c;
}
// r1 created with the draft, r2 recalibrated with the re-anchored draft (a changed profile, the same raster and seam), r3 values.
Chain MakeRecalibratedChain(const std::string& name,bool with_values){
    Chain chain;
    chain.settings=NewFolder(name);chain.revisions=chain.settings/std::string(kFixedCompositeRevisionFolderName);
    fs::create_directories(chain.revisions);
    chain.profile_one=Draft();
    chain.profile_two=rc::ReanchoredProfileText(chain.profile_one,cr::PoseParameters{0,0,0.2,0.5,0,0},SampleProvenance("chain"));
    AppendRecord(chain,ChainRequest(chain.profile_one));
    auto second=ChainRequest(chain.profile_two);
    second.change.kind=FixedCompositeRevisionKind::recalibrated;second.change.report_sha256=std::string(64,'e');second.change.pair_id="pair-0007";
    AppendRecord(chain,second);
    if(with_values){
        auto third=ChainRequest(chain.profile_two);
        third.change.kind=FixedCompositeRevisionKind::values;third.policy_sha256=std::string(64,'f');third.registration.envelope.position_mm=6;
        AppendRecord(chain,third);
    }
    return chain;
}

void ChainOnDisk(){
    auto chain=MakeRecalibratedChain("chain",true);
    const auto& one=chain.records[0];const auto& two=chain.records[1];const auto& three=chain.records[2];
    CHECK(one.source.profile_file!=two.source.profile_file && one.source.profile_file_sha256!=two.source.profile_file_sha256 &&
        one.source.profile_fingerprint_sha256!=two.source.profile_fingerprint_sha256);
    // The chain is read through; the revision in force is the last, with its own profile (values carries the profile of the recalibration on).
    const auto active=LoadActiveFixedCompositeRevisionV3(chain.revisions,kHash);
    CHECK(active && active->condition.revision.sequence==3 && active->condition.revision.change.kind==FixedCompositeRevisionKind::values);
    CHECK(active && active->condition.source.profile_file==two.source.profile_file && active->profile.profile_file_sha256==two.source.profile_file_sha256);
    CHECK(three.raster.width_pixels==two.raster.width_pixels && three.seam.column==two.seam.column);
    // The recalibrated revision itself is read with the new profile, the first with the old: neither file was touched.
    const auto second=LoadFixedCompositeConditionV3(chain.revisions/"r000002.json",kHash);
    CHECK(second.condition.revision.change.kind==FixedCompositeRevisionKind::recalibrated && second.profile.profile_file_sha256==two.source.profile_file_sha256);
    const auto first=LoadFixedCompositeConditionV3(chain.revisions/"r000001.json",kHash);
    CHECK(first.profile.profile_file_sha256==one.source.profile_file_sha256 && ReadAll(chain.revisions/"r000001.json")==chain.texts[0]);
    // Both profiles stay beside the revisions; the old one is not replaced or removed.
    CHECK(fs::exists(chain.settings/one.source.profile_file) && fs::exists(chain.settings/two.source.profile_file));
    // The version-aware entrance reads the chain too.
    const auto selection=SelectFixedComposite(chain.settings/"wpd-fixed-composite-condition.json",kHash);
    CHECK(selection.IsV3());

    // A rollback cannot cross the profile: the tool refuses to restore revision 1 (the profile of r1 is not the one in force).
    {
        auto folder=MakeRecalibratedChain("rollback",false);
        const auto before=Listing(folder.revisions);
        px::FixedConditionV3Request request;
        request.settings_folder=folder.settings;request.kind=FixedCompositeRevisionKind::rollback;request.restore_sequence=1;
        const auto r=px::MakeFixedCompositeConditionV3File(request,kHash,kNow);
        CHECK(!r.made && Contains(r.message,"別の rig"));
        CHECK(Listing(folder.revisions)==before);
        // The tool does not make the recalibrated kind, and writes nothing for it.
        px::FixedConditionV3Request own;
        own.settings_folder=folder.settings;own.kind=FixedCompositeRevisionKind::recalibrated;
        const auto made=px::MakeFixedCompositeConditionV3File(own,kHash,kNow);
        CHECK(!made.made && Contains(made.message,"recalibrated") && Listing(folder.revisions)==before);
        const auto parsed=px::ParseFixedConditionV3Arguments({L"recalibrated",L"--settings-folder",folder.settings.wstring()});
        CHECK(!parsed.request && Contains(parsed.error,"recalibrated"));
        // Restoring a revision of the same profile still works: r3 values restored from r2 would be a rollback within the profile; here the
        // chain has r1 and r2 only, so restoring r2 is refused as "not earlier than the one in force"; the point is the message above.
        std::error_code ignored;fs::remove_all(folder.settings,ignored);
    }
    // Rollback within one profile: r3 (values) to r2 (recalibrated) is allowed and carries r2's content, profile and raster.
    {
        auto folder=MakeRecalibratedChain("rollback-within",true);
        px::FixedConditionV3Request request;
        request.settings_folder=folder.settings;request.kind=FixedCompositeRevisionKind::rollback;request.restore_sequence=2;
        request.layer_two=LayerTwoStrainDesignV3{64.0,0.5};
        const auto r=px::MakeFixedCompositeConditionV3File(request,kHash,kNow);
        CHECK(r.made);
        const auto active_after=LoadActiveFixedCompositeRevisionV3(folder.revisions,kHash);
        CHECK(active_after && active_after->condition.revision.sequence==4 && active_after->condition.revision.change.kind==FixedCompositeRevisionKind::rollback &&
            active_after->condition.source.profile_file==folder.records[1].source.profile_file && active_after->condition.registration.envelope.position_mm==5);
        std::error_code ignored;fs::remove_all(folder.settings,ignored);
    }

    // What a reader that does not know the kind does: the whole chain is refused, never the revision before it. (The old reader is
    // stood for by a kind that this reader does not know either.)
    {
        auto unknown=MakeRecalibratedChain("unknown",false);
        const auto path=unknown.revisions/"r000002.json";
        WriteAll(path,Replace(ReadAll(path),"\"kind\":\"recalibrated\"","\"kind\":\"recalibrated-x\""));
        REJECT(LoadActiveFixedCompositeRevisionV3(unknown.revisions,kHash));
        REJECT(LoadFixedCompositeConditionV3(path,kHash));
        const auto picked=SelectFixedComposite(unknown.settings/"wpd-fixed-composite-condition.json",kHash);
        CHECK(!picked.loaded && picked.reason==FixedCompositeSkip::condition_invalid);   // no condition: no composite, the originals stay
        CHECK(!picked.IsV3());
        // The revision before it is still a good one on its own: the refusal is of the chain, and nothing fell back to it.
        CHECK(LoadFixedCompositeConditionV3(unknown.revisions/"r000001.json",kHash).condition.revision.sequence==1);
        std::error_code ignored;fs::remove_all(unknown.settings,ignored);
    }
    // A chain whose recalibrated revision is the only thing wrong with the profile: the file named by the revision is missing.
    {
        auto missing=MakeRecalibratedChain("missing",false);
        fs::remove(missing.settings/missing.records[1].source.profile_file);
        const auto picked=SelectFixedComposite(missing.settings/"wpd-fixed-composite-condition.json",kHash);
        CHECK(!picked.loaded && picked.reason==FixedCompositeSkip::profile_absent);
        std::error_code ignored;fs::remove_all(missing.settings,ignored);
    }
    std::error_code ignored;fs::remove_all(chain.settings,ignored);
}

// ---- Section 2. The profile with B's pose baked in -----------------------------------------------------------------------
// The side-by-side rig without a lens: A is 10 px/mm over X from -10 to 726 mm, B 10 px/mm over X from 566 to 1302 mm.
std::string RigText(bool turned){
    using ps::detail::WithMatrix;
    if(turned)return WithMatrix(WithMatrix(fixture,0,{-10,0,7359-100.0,0,-10,4911-100.0}),1,{-10,0,7359+5660.0,0,-10,4911-100.0});
    return WithMatrix(WithMatrix(fixture,0,{10,0,100,0,10,100}),1,{10,0,-5660,0,10,100});
}
render::Matrix3 MatrixOf(const render::CameraProjection& c){return c.document_to_image;}
render::Point Project(const render::Matrix3& h,render::Point x){render::Point p{};if(!render::TryTransform(h,x,p))throw std::runtime_error("projection failed");return p;}

// The pose of B for a camera moved by `shift_mm` along the sheet's x (the plane appears shifted the other way), found on the model.
cr::PoseParameters PoseForShift(const render::CameraProjection& camera_b,double shift_mm,bool turned){
    const cr::PoseModel model(camera_b);
    const render::Point x0{934,236};
    const auto target=Project(MatrixOf(camera_b),{x0.x-shift_mm,x0.y});
    const auto at=[&](double tx){return Project(model.CameraHomography(cr::PoseParameters{0,0,0,tx,0,0}),x0).x-target.x;};
    double a=0,b=turned?-1:1;
    for(int i=0;i<6;++i){
        const double fa=at(a),fb=at(b);
        if(fa==fb)break;
        const double c=b-fb*(b-a)/(fb-fa);a=b;b=c;
    }
    return cr::PoseParameters{0,0,0,b,0,0};
}

const json::JsonValue& At(const json::JsonValue& v,const char* key){return v.object.at(key);}
struct JsonFailure {[[noreturn]] static void Fail(std::string_view code,std::string_view){throw std::invalid_argument("test json "+std::string(code));}};
json::JsonValue Tree(const std::string& text){return json::BasicJsonParser<JsonFailure>(text).Parse();}
std::string Dump(const json::JsonValue& v){return json::SerializeJsonWith<JsonFailure>(v);}

void ProfileBake(){
    // A parent with values in the places the new profile must copy or clear.
    std::string parent=RigText(false);
    parent=Replace(parent,"\"profileId\": \"synthetic-fingerprint-draft\"","\"profileId\": \"parent-profile\"");
    const auto parent_tree=Tree(parent);
    const render::OutputRaster raster=ps::Raster(560000,100000,740000,260000,254);
    const auto parent_parameters=RigProfileV2::Parse(parent).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
    const cr::PoseParameters pose=PoseForShift(parent_parameters.camera_b,30.0,false);
    const auto provenance=SampleProvenance("bake");
    const auto text=rc::ReanchoredProfileText(parent,pose,provenance);
    const auto parsed=RigProfileV2::Parse(text);
    CHECK(parsed.IsCalibrated() && !parsed.IsApproved() && !parsed.ProfileId());                  // a draft with no profileId
    CHECK(parsed.FingerprintSha256()!=RigProfileV2::Parse(parent).FingerprintSha256());
    const auto child=parsed.ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);

    // A is the parent's, bit for bit; B's lens is the parent's; B's matrix is the pose's homography.
    const auto same_camera=[](const render::CameraProjection& a,const render::CameraProjection& b){
        bool same=SameBits(a.intrinsics.fx_pixels,b.intrinsics.fx_pixels) && SameBits(a.intrinsics.fy_pixels,b.intrinsics.fy_pixels) &&
            SameBits(a.intrinsics.cx_pixels,b.intrinsics.cx_pixels) && SameBits(a.intrinsics.cy_pixels,b.intrinsics.cy_pixels) &&
            SameBits(a.distortion.k1,b.distortion.k1) && SameBits(a.distortion.k2,b.distortion.k2) && SameBits(a.distortion.k3,b.distortion.k3) &&
            SameBits(a.distortion.p1,b.distortion.p1) && SameBits(a.distortion.p2,b.distortion.p2);
        for(int i=0;i<9;++i)same=same && SameBits(a.document_to_image[static_cast<std::size_t>(i)],b.document_to_image[static_cast<std::size_t>(i)]);
        return same;
    };
    CHECK(same_camera(child.camera_a,parent_parameters.camera_a));
    CHECK(child.layout==parent_parameters.layout);
    const cr::PoseModel model(parent_parameters.camera_b);
    const auto expected=model.CameraHomography(pose);
    double worst=0;
    for(int i=0;i<9;++i)worst=std::max(worst,std::abs(child.camera_b.document_to_image[static_cast<std::size_t>(i)]-expected[static_cast<std::size_t>(i)])/std::max(1.0,std::abs(expected[static_cast<std::size_t>(i)])));
    CHECK(worst<=1e-9 && child.camera_b.document_to_image[8]==1);
    const render::CameraProjection same_lens_b(parent_parameters.camera_b.intrinsics,parent_parameters.camera_b.distortion,child.camera_b.document_to_image);
    CHECK(same_camera(child.camera_b,same_lens_b));
    // The matrix is that of B moved by 30 mm along the baseline: a point of the sheet is seen 300 px to the left of where it was.
    CheckNear(Project(child.camera_b.document_to_image,{700,200}).x,Project(parent_parameters.camera_b.document_to_image,{700-30,200}).x,0.05,"the baked matrix is B moved along the baseline");
    CHECK(Project(child.camera_b.document_to_image,{700,200}).x<Project(parent_parameters.camera_b.document_to_image,{700,200}).x-200);

    // The calibration block: the parent's where B did not change, the pair's where it did, null where it was measured by hand.
    const auto tree=Tree(text);
    const auto& cal=At(tree,"calibration");const auto& old=At(parent_tree,"calibration");
    CHECK(At(cal,"method").string=="chart-fiducial-planar" && At(cal,"chartId").string==At(old,"chartId").string);
    CHECK(At(At(cal,"tool"),"toolId").string==std::string(rc::kToolId) && At(At(cal,"tool"),"version").string==std::string(rc::kToolVersion));
    CHECK(At(cal,"provenanceId").string==provenance.provenance_id && At(cal,"measuredAt").string==provenance.measured_at_utc);
    CHECK(At(At(cal,"inputSha256"),"CAM-A").string==At(At(old,"inputSha256"),"CAM-A").string && At(At(cal,"inputSha256"),"CAM-B").string==provenance.input_sha256_camera_b);
    const auto& fit=At(cal,"fitResidualPixels");const auto& old_fit=At(old,"fitResidualPixels");
    CHECK(Dump(At(fit,"CAM-A"))==Dump(At(old_fit,"CAM-A")));
    CHECK(At(At(fit,"CAM-B"),"rms").string==fx::detail::Num(0.25) && At(At(fit,"CAM-B"),"max").string==fx::detail::Num(0.75));
    const auto& rig=At(cal,"rigMeasurements");const auto& old_rig=At(old,"rigMeasurements");
    CHECK(At(rig,"baselineMm").kind==json::JsonKind::null_value && At(rig,"overlapMm").kind==json::JsonKind::null_value);
    CHECK(At(At(rig,"cameraToDocumentMm"),"CAM-B").kind==json::JsonKind::null_value);
    CHECK(Dump(At(At(rig,"cameraToDocumentMm"),"CAM-A"))==Dump(At(At(old_rig,"cameraToDocumentMm"),"CAM-A")) && Dump(At(rig,"lensFocalLengthMm"))==Dump(At(old_rig,"lensFocalLengthMm")));
    CHECK(At(tree,"status").string=="draft" && At(tree,"profileId").kind==json::JsonKind::null_value);                      // the parent's id is not carried
    CHECK(Dump(At(tree,"documentPlane"))==Dump(At(parent_tree,"documentPlane")) && Dump(At(tree,"conventions"))==Dump(At(parent_tree,"conventions")));

    // A parent with values in the hand-measured places: they are the parent's and are cleared for B only.
    {
        auto measured=Replace(parent,"\"baselineMm\": null","\"baselineMm\": 576.5");
        measured=Replace(measured,"\"overlapMm\": null","\"overlapMm\": 160.25");
        const auto t=Tree(rc::ReanchoredProfileText(measured,pose,provenance));
        CHECK(At(At(At(t,"calibration"),"rigMeasurements"),"baselineMm").kind==json::JsonKind::null_value && At(At(At(t,"calibration"),"rigMeasurements"),"overlapMm").kind==json::JsonKind::null_value);
    }
    // Re-anchoring twice composes through the profile: the second child of the first child is a pose of the first child.
    {
        const auto grand=rc::ReanchoredProfileText(text,cr::PoseParameters{0,0,0,5,0,0},SampleProvenance("again"));
        const auto g=RigProfileV2::Parse(grand).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        const auto expected_grand=cr::PoseModel(child.camera_b).CameraHomography(cr::PoseParameters{0,0,0,5,0,0});
        double w=0;
        for(int i=0;i<9;++i)w=std::max(w,std::abs(g.camera_b.document_to_image[static_cast<std::size_t>(i)]-expected_grand[static_cast<std::size_t>(i)])/std::max(1.0,std::abs(expected_grand[static_cast<std::size_t>(i)])));
        CHECK(w<=1e-9 && Dump(At(At(Tree(grand),"calibration"),"tool"))==Dump(At(cal,"tool")));
    }
    // Refusals.
    REJECT(rc::ReanchoredProfileText(Replace(parent,"\"status\": \"draft\"","\"status\": \"approved\""),pose,provenance));
    REJECT(rc::ReanchoredProfileText(parent,cr::PoseParameters{0,0,std::nan(""),0,0,0},provenance));
    {auto bad=provenance;bad.provenance_id="bad id";REJECT(rc::ReanchoredProfileText(parent,pose,bad));}
    {auto bad=provenance;bad.input_sha256_camera_b="abc";REJECT(rc::ReanchoredProfileText(parent,pose,bad));}
    {auto bad=provenance;bad.fit.rms_px=-1;REJECT(rc::ReanchoredProfileText(parent,pose,bad));}
    REJECT(rc::ReanchoredProfileText("{}",pose,provenance));
    REJECT(rc::ReanchoredProfileText(Replace(parent,"\"calibration\": {","\"calibration\": null, \"ignored\": {"),pose,provenance));
}

// The fit of the new mapping in camera B's pixels: one output pixel is two camera pixels at 127 DPI on this rig (10 px/mm, 0.2 mm per pixel).
void FitUnits(){
    const render::OutputRaster raster=ps::Raster(560000,100000,740000,260000,127);
    const auto parameters=RigProfileV2::Parse(RigText(false)).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
    cr::RegistrationResult estimate;
    const auto observation=[](double x,double y,bool one,double residual,double weight){
        cr::ObservationReport o;o.x=x;o.y=y;o.one_directional=one;o.normal={one?1.0:0.0,one?0.0:0.0};o.residual_px=residual;o.weight=weight;return o;
    };
    estimate.diagnostics.observations={observation(300,200,false,1.0,1),observation(310,210,true,0.5,1),observation(320,220,false,9.0,0),observation(330,230,false,2.0,0.5)};
    const auto fit=rc::FitInCameraPixels(estimate,raster,parameters.camera_b);
    // Counted: the points with weight above 0. Scale 2 camera px per output px, whichever way the residual points.
    CHECK(fit.points==3);
    CheckNear(fit.max_px,4.0,1e-6,"the largest residual is 2 output px = 4 camera px");
    CheckNear(fit.rms_px,std::sqrt((2.0*2.0+1.0*1.0+4.0*4.0)/3.0),1e-6,"the rms is in camera pixels");
    CHECK(rc::FitInCameraPixels(cr::RegistrationResult{},raster,parameters.camera_b).points==0);
}

// ---- Section 3. The raster, the seam and the candidate, from a pose (no images) ----------------------------------------------
struct Active {
    fs::path settings;
    FixedCompositeConditionV3 condition;
    std::string sha256,profile;
};
// The revision r000001 of a settings folder for the rig, with this test's ranges (the values `edit` leaves are SampleCondition's).
Active MakeActive(const std::string& name,const std::string& profile,MountOrientation orientation,const render::OutputRaster& raster,
    const std::function<void(FixedCompositeConditionV3&)>& edit={}){
    Active a;
    a.settings=NewFolder(name);a.profile=profile;
    auto c=ps::SampleCondition(a.settings,profile,orientation,raster);
    if(edit)edit(c);
    ValidateFixedCompositeConditionV3(c);
    const auto revisions=a.settings/std::string(kFixedCompositeRevisionFolderName);
    fs::create_directories(revisions);
    WriteFixedCompositeConditionV3(revisions,c);
    const auto loaded=LoadActiveFixedCompositeRevisionV3(revisions,kHash);
    if(!loaded)throw std::runtime_error("the written revision did not come back");
    a.condition=loaded->condition;a.sha256=loaded->file_sha256;
    return a;
}
rc::Inputs MakeInputs(const Active& a,const render::BgrImage* image_a,const render::BgrImage* image_b){
    rc::Inputs in;
    in.camera_a=image_a;in.camera_b=image_b;in.active=a.condition;in.active_sha256=a.sha256;in.profile_bytes=a.profile;
    in.layer_two=LayerTwoStrainDesignV3{192.0,0.1};   // this test's design constants of layer 2's strain bound
    in.pair_id="pair-0042";in.original_a_sha256=std::string(64,'1');in.original_b_sha256=std::string(64,'2');in.measured_at_utc="2000-01-01T00:00:00Z";
    in.hash=kHash;in.design=cr::kRegistrationDesignDraft;in.thread_count=4;in.now=kNow;
    return in;
}

// This test's ranges: B may move 5 mm, turn 1 degree, tilt 0.5 degree, change height by 2 percent; the paper may be 10 mm up.
void TestRanges(FixedCompositeConditionV3& c){
    c.registration.envelope={5,1,0.5,2};
    c.registration.surface_envelope=SurfaceEnvelope{10,2};
    c.registration.limits.convergence_px=0.05;
    c.registration.limits.minimum_unique_blocks=2;
}

void GeometryFromPose(){
    const auto profile=RigText(false);
    const render::OutputRaster raster=ps::Raster(560000,100000,740000,260000,254);   // 180 mm x 160 mm, 1800 x 1600 px
    const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
    auto active=MakeActive("geometry",profile,MountOrientation::normal,raster,TestRanges);
    const auto parent=BindFixedCompositeProfileV3(active.condition,profile,kHash);
    const rc::ProfileFit fit{0.1,0.3,50};
    const std::string record="{\"schema\":\"test-record\"}";

    // The raster follows B's cover: the right edge moves by the shift, in whole pixels; the other edges and the DPI stay.
    for(const double shift:{5.0,30.0,60.0,-30.0}){
        const auto pose=PoseForShift(parameters.camera_b,shift,false);
        const auto grown=rc::ProposeRaster(active.condition,parent,pose,rc::RasterPlacement::follow_camera_b);
        CHECK(grown.dpi==raster.dpi && grown.height_pixels==raster.height_pixels && grown.region_um.left==raster.region_um.left &&
            grown.region_um.top==raster.region_um.top && grown.region_um.bottom==raster.region_um.bottom);
        CHECK(std::abs(static_cast<long long>(grown.width_pixels)-static_cast<long long>(raster.width_pixels)-std::llround(shift*10))<=1);
        CHECK(Throws([&]{render::ValidateOutputRaster(grown);})==false);
        const auto kept=rc::ProposeRaster(active.condition,parent,pose,rc::RasterPlacement::keep);
        CHECK(kept.width_pixels==raster.width_pixels && kept.region_um.right==raster.region_um.right);
    }
    CHECK(rc::ProposeRaster(active.condition,parent,cr::PoseParameters{},rc::RasterPlacement::follow_camera_b).width_pixels==raster.width_pixels);

    for(const double shift:{5.0,30.0,60.0}){
        const auto pose=PoseForShift(parameters.camera_b,shift,false);
        const auto proposal=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),pose,fit,record);
        CHECK(proposal.refusals.empty() && proposal.candidate && !proposal.ready);   // not ready: nothing has verified it against the pair
        if(!proposal.candidate)continue;
        const auto& cand=*proposal.candidate;const auto& c=cand.condition;
        // The revision: recalibrated, one on, the parent named by the hash of its file, the pose in the profile.
        CHECK(c.revision.sequence==2 && c.revision.parent_sha256==active.sha256 && c.revision.change.kind==FixedCompositeRevisionKind::recalibrated);
        CHECK(c.revision.change.pair_id=="pair-0042" && c.revision.change.report_sha256==HashOf(record) && proposal.estimate_record_sha256==HashOf(record));
        CHECK(c.base_pose.IsZero());
        // What a recalibrated revision may change is changed (profile, raster, seam); every other member is the parent's, as the format says.
        auto expected=active.condition;
        expected.created_utc=c.created_utc;expected.revision=c.revision;expected.source=c.source;expected.raster=c.raster;expected.seam=c.seam;
        CHECK(FormatFixedCompositeConditionV3(expected)==FormatFixedCompositeConditionV3(c));
        CHECK(c.source.policy_sha256==active.condition.source.policy_sha256 && c.orientation==active.condition.orientation);
        CHECK(c.source.profile_file!=active.condition.source.profile_file && c.source.profile_file==cand.profile_name &&
            c.source.profile_file_sha256==HashOf(cand.profile_bytes) && c.source.profile_file_sha256!=active.condition.source.profile_file_sha256);
        // The default placements: the right edge follows B, the seam is the middle of the new band.
        CHECK(std::abs(static_cast<long long>(c.raster.width_pixels)-static_cast<long long>(raster.width_pixels)-std::llround(shift*10))<=1);
        CHECK(c.seam.origin==FixedSeamOrigin::band_center_provisional && c.seam.column==FixedCompositeProvisionalSeamColumn(cand.binding.parameters));
        // The text is what the writer would write, and the reader takes it with its profile.
        CHECK(cand.text==FormatFixedCompositeConditionV3(c)+"\n" && cand.sha256==HashOf(cand.text));
        CHECK(FormatFixedCompositeConditionV3(ParseFixedCompositeConditionV3(cand.text.substr(0,cand.text.size()-1)))+"\n"==cand.text);
        CHECK(cand.binding.profile_file_sha256==c.source.profile_file_sha256 && cand.binding.profile_fingerprint_sha256==c.source.profile_fingerprint_sha256);
        // B's mapping in the new profile is the pose's homography; A's block is the parent's.
        const cr::PoseModel model(parent.parameters.camera_b);
        const auto homography=model.CameraHomography(pose);
        double worst=0;
        for(int i=0;i<9;++i)worst=std::max(worst,std::abs(cand.binding.parameters.camera_b.document_to_image[static_cast<std::size_t>(i)]-homography[static_cast<std::size_t>(i)])/std::max(1.0,std::abs(homography[static_cast<std::size_t>(i)])));
        CHECK(worst<=1e-9);
        for(int i=0;i<9;++i)CHECK(SameBits(cand.binding.parameters.camera_a.document_to_image[static_cast<std::size_t>(i)],parent.parameters.camera_a.document_to_image[static_cast<std::size_t>(i)]));
        // B's sheet point under a pixel moved by `shift`: the pixel that showed X now shows X + shift.
        CheckNear(Project(cand.binding.parameters.camera_b.document_to_image,{700,200}).x,Project(parent.parameters.camera_b.document_to_image,{700-shift,200}).x,0.05,"B's mapping follows the shift");
        // The screen's numbers: the overlap is what is left of the 160 mm, the seam moves by half the shift, B's motion is to the right.
        const auto& d=proposal.display;
        CHECK(d.has_motion && d.has_overlap && d.has_edge_distance);
        CheckNear(d.narrowest_overlap_mm,160.0-shift,0.3,"the narrowest overlap");
        CheckNear(d.seam_x_new_mm-d.seam_x_previous_mm,(646.0+shift/2)-650.05,0.3,"the seam moved to the middle of the new band");
        CheckNear(d.motion.camera_motion_mm.x,shift,0.2,"B moved to the right of the drawing");
        CheckNear(d.motion.camera_motion_mm.y,0.0,0.2,"B did not move up or down");
        CheckNear(d.pose.position_mm,shift,0.2,"the size of the move");
        CHECK(d.width_new_px==c.raster.width_pixels && d.width_previous_px==raster.width_pixels && d.right_new_mm>d.right_previous_mm);
        CheckNear(d.edge_distance_a_px,7359-(10*(646.0+shift/2)+100),3.0,"the seam's distance to the right edge of A");
        CheckNear(d.edge_distance_b_px,10*(646.0+shift/2)-5660-10*shift,3.0,"the seam's distance to the left edge of B");
        CheckNear(d.edge_share_a,d.edge_distance_a_px/7360,1e-9,"the share of the width");
        // The selection a sample is composed with is the candidate itself, read as v3 (the thin step R5).
        const auto selection=rc::SelectionOfCandidate(cand);
        CHECK(selection.IsV3());
        const auto* v3=selection.loaded?std::get_if<LoadedFixedCompositeConditionV3>(&*selection.loaded):nullptr;
        CHECK(v3 && v3->file_sha256==cand.sha256 && v3->condition.revision.sequence==2 && v3->profile.profile_file_sha256==cand.binding.profile_file_sha256);
        // The check ran and its facts are kept.
        CHECK(!cand.check.details.empty());
    }

    // Where the seam goes and how big the raster is are two choices, each way.
    {
        const auto pose=PoseForShift(parameters.camera_b,30.0,false);
        for(const auto seam:{rc::SeamPlacement::band_center,rc::SeamPlacement::keep_previous})
            for(const auto placement:{rc::RasterPlacement::follow_camera_b,rc::RasterPlacement::keep}){
                auto in=MakeInputs(active,nullptr,nullptr);in.seam=seam;in.raster=placement;
                const auto proposal=rc::ProposeFromPose(in,pose,fit,record);
                // B's cover moves right by 30 mm: with the raster kept, B's right edge is still beyond it.
                const std::string which=std::string(seam==rc::SeamPlacement::band_center?"band-center":"keep-previous")+"/"+(placement==rc::RasterPlacement::follow_camera_b?"follow":"keep");
                CHECK(proposal.display.has_motion);
                if(!proposal.candidate){
                    // A refusal names its reason and keeps the numbers for the screen; it never leaves a candidate.
                    CHECK(!proposal.refusals.empty() && !proposal.ready && !proposal.detail.empty());
                    std::cerr<<"note: "<<which<<" refused: "<<proposal.detail<<'\n';
                    continue;
                }
                const auto& c=proposal.candidate->condition;
                if(seam==rc::SeamPlacement::keep_previous){CHECK(c.seam.column==active.condition.seam.column && c.seam.origin==FixedSeamOrigin::operator_set);}
                else CHECK(c.seam.origin==FixedSeamOrigin::band_center_provisional);
                if(placement==rc::RasterPlacement::keep){CHECK(c.raster.width_pixels==raster.width_pixels && c.raster.region_um.right==raster.region_um.right);}
                else CHECK(c.raster.width_pixels>raster.width_pixels);
            }
    }
    std::error_code ignored;fs::remove_all(active.settings,ignored);
}

// ---- Section 4. The whole path on rendered pairs -------------------------------------------------------------------------------
// The paper is a generated texture over the sheet (value noise of three scales, no repeat), seen by both cameras through their true
// mappings; the stored frames are the full sensor size (7360 x 4912) that the profile fixes. Nothing here uses the code under test.
struct Texture {
    static constexpr double kLeft=-30,kTop=-30;
    struct Octave {double spacing,amplitude;int columns,rows;std::vector<float> nodes;};
    std::vector<Octave> octaves;
    explicit Texture(std::uint64_t seed){
        const double widths[]{10.0,4.0,1.6};const double amplitudes[]{70.0,55.0,45.0};
        std::uint64_t state=seed;
        const auto next=[&]{state+=0x9e3779b97f4a7c15ull;std::uint64_t z=state;z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;z=(z^(z>>27))*0x94d049bb133111ebull;z^=z>>31;return z;};
        for(int k=0;k<3;++k){
            Octave o{widths[k],amplitudes[k],static_cast<int>(1400/widths[k])+3,static_cast<int>(560/widths[k])+3,{}};
            o.nodes.resize(static_cast<std::size_t>(o.columns)*static_cast<std::size_t>(o.rows));
            for(auto& v:o.nodes)v=static_cast<float>(static_cast<double>(next()>>11)/9007199254740992.0*2.0-1.0);
            octaves.push_back(std::move(o));
        }
    }
    [[nodiscard]] double operator()(double x,double y) const {
        double value=128;
        for(const auto& o:octaves){
            const double gx=(x-kLeft)/o.spacing,gy=(y-kTop)/o.spacing;
            const double fx=std::floor(gx),fy=std::floor(gy);
            const int ix=std::clamp(static_cast<int>(fx),0,o.columns-2),iy=std::clamp(static_cast<int>(fy),0,o.rows-2);
            const double tx=std::clamp(gx-ix,0.0,1.0),ty=std::clamp(gy-iy,0.0,1.0);
            const double sx=tx*tx*(3-2*tx),sy=ty*ty*(3-2*ty);
            const auto node=[&](int cx,int cy){return static_cast<double>(o.nodes[static_cast<std::size_t>(cy)*static_cast<std::size_t>(o.columns)+static_cast<std::size_t>(cx)]);};
            const double top=node(ix,iy)*(1-sx)+node(ix+1,iy)*sx,bottom=node(ix,iy+1)*(1-sx)+node(ix+1,iy+1)*sx;
            value+=o.amplitude*(top*(1-sy)+bottom*sy);
        }
        return value;
    }
};

// The stored frame of a camera whose true document-to-image matrix is h, over the paper.
render::BgrImage RenderFrame(const render::Matrix3& h,const std::function<double(double,double)>& paper){
    render::BgrImage image{ps::kWidth,ps::kHeight,std::vector<std::uint8_t>(static_cast<std::size_t>(ps::kWidth)*ps::kHeight*3)};
    const auto inverse=render::Invert(h);
    const unsigned workers=4;
    std::vector<std::thread> threads;
    for(unsigned w=0;w<workers;++w)threads.emplace_back([&,w]{
        for(unsigned v=w;v<ps::kHeight;v+=workers)
            for(unsigned u=0;u<ps::kWidth;++u){
                render::Point sheet{};
                const auto value=render::TryTransform(inverse,{static_cast<double>(u),static_cast<double>(v)},sheet)?paper(sheet.x,sheet.y):128.0;
                const auto byte=static_cast<std::uint8_t>(std::clamp(std::lround(value),0L,255L));
                auto* at=&image.bgr[(static_cast<std::size_t>(v)*ps::kWidth+u)*3];
                at[0]=at[1]=at[2]=byte;
            }
    });
    for(auto& t:threads)t.join();
    return image;
}
render::Matrix3 Multiply(const render::Matrix3& a,const render::Matrix3& b){
    render::Matrix3 r{};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k)r[static_cast<std::size_t>(3*i+j)]+=a[static_cast<std::size_t>(3*i+k)]*b[static_cast<std::size_t>(3*k+j)];
    return r;
}

struct Case {
    std::string name;
    double shift_mm;          // B moved apart along the sheet's x
    double rotation_deg;      // B turned about its optical axis
    double scale_error;       // the true lens against the profile's nominal one: the image scale about the principal point
    bool turned;              // the rig mounted a half turn
};
// The true document-to-image matrices of the two cameras for a case. The profile has the nominal ones.
struct Truth {render::Matrix3 a,b;};
Truth TruthOf(const Case& c){
    const render::Matrix3 a0=c.turned?render::Matrix3{-10,0,7359-100.0,0,-10,4911-100.0,0,0,1}:render::Matrix3{10,0,100,0,10,100,0,0,1};
    const render::Matrix3 b0=c.turned?render::Matrix3{-10,0,7359+5660.0,0,-10,4911-100.0,0,0,1}:render::Matrix3{10,0,-5660,0,10,100,0,0,1};
    // B moved by `shift`: the feature at X is seen where X - shift was seen; turned about the axis by the angle: about the middle of its view.
    const double t=c.rotation_deg*std::numbers::pi/180,cx=934.0,cy=236.0;
    const render::Matrix3 move{1,0,-c.shift_mm,0,1,0,0,0,1};
    const render::Matrix3 to{1,0,-cx,0,1,-cy,0,0,1},from{1,0,cx,0,1,cy,0,0,1},turn{std::cos(t),-std::sin(t),0,std::sin(t),std::cos(t),0,0,0,1};
    const auto document=Multiply(from,Multiply(turn,Multiply(to,move)));
    Truth truth{a0,Multiply(b0,document)};
    if(c.scale_error!=0){
        const double s=1+c.scale_error,px=3679.5,py=2455.5;
        const render::Matrix3 lens{s,0,px*(1-s),0,s,py*(1-s),0,0,1};
        truth.a=Multiply(lens,truth.a);truth.b=Multiply(lens,truth.b);
    }
    return truth;
}
// How far B's pixel for an output point is from the pixel that truly shows what A shows there (camera B's pixels): the seam's misalignment
// if the composite were made with `b_baked` and no per-capture estimate.
double AlignmentError(const render::Matrix3& a_nominal,const render::Matrix3& b_baked,const Truth& truth,render::Point x){
    const auto pixel_a=Project(a_nominal,x);
    const auto scene=Project(render::Invert(truth.a),pixel_a);
    const auto want=Project(truth.b,scene),have=Project(b_baked,x);
    return std::hypot(have.x-want.x,have.y-want.y);
}
double WorstAlignment(const render::Matrix3& a_nominal,const render::Matrix3& b_baked,const Truth& truth,double left_mm,double right_mm){
    double worst=0;
    for(double x=left_mm;x<=right_mm;x+=(right_mm-left_mm)/12)for(const double y:{110.0,160.0,210.0,255.0})worst=std::max(worst,AlignmentError(a_nominal,b_baked,truth,{x,y}));
    return worst;
}
bool CaseWanted(const std::string& name){
    char* value=nullptr;size_t length=0;
    if(_dupenv_s(&value,&length,"A0_RECAL_CASES")!=0 || value==nullptr)return true;
    const std::string list(value);std::free(value);
    return list.empty() || ("," + list + ",").find("," + name + ",")!=std::string::npos;
}

void Pipeline(){
    const Texture texture(7919317);
    const std::function<double(double,double)> paper=[&](double x,double y){return texture(x,y);};
    const render::OutputRaster raster=ps::Raster(560000,100000,740000,260000,254);
    const Case cases[]{
        {"shift-5",5,0,0,false},
        {"shift-30",30,0,0,false},
        {"shift-60",60,0,0,false},
        {"shift-30-turn",30,0.5,0,false},
        {"shift-30-nominal-k",30,0,0.003,false},
        {"shift-30-mount-half-turn",30,0,0,true},
    };
    for(const auto& c:cases){
        if(!CaseWanted(c.name))continue;
        const auto began=std::chrono::steady_clock::now();
        const auto profile=RigText(c.turned);
        const auto orientation=c.turned?MountOrientation::rotated_180:MountOrientation::normal;
        auto active=MakeActive("pipeline-"+c.name,profile,orientation,raster,TestRanges);
        const auto truth=TruthOf(c);
        const auto image_a=RenderFrame(truth.a,paper);
        const auto image_b=RenderFrame(truth.b,paper);
        const double rendered=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();
        auto in=MakeInputs(active,&image_a,&image_b);
        std::vector<std::string> stages;
        in.progress=[&](rc::Stage s){stages.emplace_back(rc::StageCode(s));};
        const auto proposal=rc::ProposeRecalibration(in);
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();
        std::cerr<<c.name<<": ready="<<proposal.ready<<" rendered "<<rendered<<" s, whole "<<seconds<<" s; wide "<<proposal.wide.determined<<" ["<<proposal.wide.reason<<"] rms "
            <<proposal.wide.residual_rms_mm<<" mm; estimate "<<cr::RegistrationStateCode(proposal.estimate.state)<<"; verification seam "<<proposal.verification.seam_displacement_px
            <<" px (line "<<proposal.verification.limit_px<<"), pose-only "<<proposal.verification.seam_displacement_pose_only_px<<"; refusals";
        for(const auto r:proposal.refusals)std::cerr<<' '<<rc::RefusalCode(r);
        std::cerr<<" detail: "<<proposal.detail<<'\n';
        {
            const auto& d=proposal.estimate.diagnostics;
            std::cerr<<c.name<<": wide pose tx "<<proposal.wide.pose.tx_mm<<" ty "<<proposal.wide.pose.ty_mm<<" rz "<<proposal.wide.pose.rz_deg<<" correspondences "<<proposal.wide.correspondences
                <<"; coarse blocks "<<d.coarse_blocks.size()<<" unique "<<d.unique_blocks<<" consistent "<<d.consistent_blocks<<" gate points "<<d.gate_points<<" points "<<d.points
                <<" outer "<<d.outer_iterations<<" radius "<<d.coarse_radius_x_px<<","<<d.coarse_radius_y_px<<" has_coarse_pose "<<d.coarse_pose.has_value()<<'\n';
        }
        CHECK(proposal.wide.determined);
        CHECK(proposal.ready && proposal.refusals.empty() && proposal.candidate);
        CHECK(stages.size()==5 && stages.front()=="wide-search" && stages.back()=="verify");
        if(!proposal.candidate){std::error_code ignored;fs::remove_all(active.settings,ignored);continue;}
        const auto& cand=*proposal.candidate;
        // The new profile puts B where the pair says it is: the seam is aligned where the pair shows both cameras.
        const auto parent_parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        const auto a_nominal=MatrixOf(parent_parameters.camera_a);
        const auto baked=MatrixOf(cand.binding.parameters.camera_b);
        const auto parent_b=MatrixOf(parent_parameters.camera_b);
        const double left_edge=626.0-(c.shift_mm==60?0.0:0.0);
        const double before=WorstAlignment(a_nominal,parent_b,truth,left_edge,700.0);
        const double after=WorstAlignment(a_nominal,baked,truth,left_edge,700.0);
        std::cerr<<c.name<<": seam misalignment before "<<before<<" px, after "<<after<<" px; pose "<<proposal.pose.tx_mm<<" "<<proposal.pose.ty_mm<<" "<<proposal.pose.rz_deg<<" ("
            <<proposal.pose.rx_deg<<" "<<proposal.pose.ry_deg<<" "<<proposal.pose.tz_percent<<"); fit "<<proposal.fit.rms_px<<" px; overlap "<<proposal.display.narrowest_overlap_mm
            <<" mm; edge distance "<<proposal.display.edge_share_a<<" / "<<proposal.display.edge_share_b<<'\n';
        CHECK(after<=0.5);
        CHECK(before>after);
        CHECK(cand.condition.revision.change.kind==FixedCompositeRevisionKind::recalibrated && cand.condition.revision.change.report_sha256==proposal.estimate_record_sha256);
        // The record of the estimate: the schema, the pair, the parent, what the fit left, and no path.
        const auto record=Tree(proposal.estimate_record_text);
        CHECK(At(record,"schema").string==std::string(rc::kEstimateRecordSchema) && At(record,"pairId").string=="pair-0042");
        CHECK(At(At(record,"parent"),"revisionSha256").string==active.sha256 && At(At(record,"parent"),"reanchoredBefore").boolean==false);
        CHECK(At(At(record,"estimate"),"state").string=="verified" && At(At(record,"estimate"),"informationConditionNumber").kind==json::JsonKind::number);
        CHECK(proposal.estimate_record_text.find(":\\")==std::string::npos && proposal.estimate_record_text.find("/Users")==std::string::npos);
        CHECK(proposal.verification.performed && proposal.verification.passed && proposal.verification.seam_displacement_px<=proposal.verification.limit_px);
        // What the writer would do: the profile copy, then the revision; the chain reads, and the revision in force is the candidate.
        CHECK(CopyFixedCompositeProfile(active.settings,cand.profile_bytes,kHash)==cand.profile_name);
        WriteFixedCompositeConditionV3(active.settings/std::string(kFixedCompositeRevisionFolderName),cand.condition);
        const auto now_active=LoadActiveFixedCompositeRevisionV3(active.settings/std::string(kFixedCompositeRevisionFolderName),kHash);
        CHECK(now_active && now_active->file_sha256==cand.sha256 && now_active->condition.revision.sequence==2);
        CHECK(ReadAll(active.settings/active.condition.source.profile_file)==profile);   // the old profile is where it was
        // The result does not depend on the thread count: one thread gives the same bytes.
        if(c.name=="shift-30"){
            auto single=in;single.thread_count=1;single.progress=nullptr;
            const auto again=rc::ProposeRecalibration(single);
            CHECK(again.ready && again.candidate && again.candidate->text==cand.text && again.candidate->profile_bytes==cand.profile_bytes &&
                again.estimate_record_text==proposal.estimate_record_text);
        }
        // The verification sees a profile that does not fit the pair: the same candidate against a pair whose B is 3 mm elsewhere (inside the
        // range, so the estimate is trusted) leaves the seam far from nothing, and does not pass.
        if(c.name=="shift-30"){
            const Truth elsewhere=TruthOf(Case{"elsewhere",33,0,0,false});
            const auto other_b=RenderFrame(elsewhere.b,paper);
            auto other=in;other.camera_b=&other_b;other.progress=nullptr;
            const auto verification=rc::VerifyCandidate(other,cand);
            std::cerr<<"mismatched pair: state "<<cr::RegistrationStateCode(verification.state)<<" displacement "<<verification.seam_displacement_px<<" px (line "<<verification.limit_px<<")\n";
            CHECK(verification.performed && !verification.passed);
            CHECK(verification.state!=cr::RegistrationState::verified || verification.seam_displacement_px>verification.limit_px);
        }
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
}

// What is refused, and why. The pairs are rendered like the ones above; only the paper or the pose of B is made to fail.
void Refusals(){
    const Texture texture(7919317);
    const render::OutputRaster raster=ps::Raster(560000,100000,740000,260000,254);
    const auto profile=RigText(false);
    const auto parent=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
    const cr::PoseModel model(parent.camera_b);
    const std::function<double(double,double)> paper=[&](double x,double y){return texture(x,y);};
    const std::function<double(double,double)> white=[](double,double){return 255.0;};
    const std::function<double(double,double)> periodic=[](double x,double y){return 150+45*std::cos(2*std::numbers::pi*x/20.0)+45*std::cos(2*std::numbers::pi*y/20.0);};
    const auto has=[](const rc::Proposal& p,rc::Refusal r){return std::find(p.refusals.begin(),p.refusals.end(),r)!=p.refusals.end();};
    const auto describe=[](const std::string& name,const rc::Proposal& p){
        std::cerr<<name<<": ready="<<p.ready<<" refusals";
        for(const auto r:p.refusals)std::cerr<<' '<<rc::RefusalCode(r);
        std::cerr<<" detail: "<<p.detail<<'\n';
    };
    const Truth moved=TruthOf(Case{"x",30,0,0,false});

    // A white sheet and a sheet that is only a repeat: nothing is found, nothing is made.
    if(CaseWanted("refuse-paper")){
        const std::pair<const char*,const std::function<double(double,double)>*> sheets[]{{"white",&white},{"periodic",&periodic}};
        for(const auto& [name,sheet]:sheets){
            auto active=MakeActive(std::string("refuse-")+name,profile,MountOrientation::normal,raster,TestRanges);
            const auto a=RenderFrame(moved.a,*sheet),b=RenderFrame(moved.b,*sheet);
            const auto proposal=rc::ProposeRecalibration(MakeInputs(active,&a,&b));
            describe(name,proposal);
            CHECK(!proposal.ready && !proposal.refusals.empty() && !proposal.candidate && !proposal.verification.performed);
            CHECK(has(proposal,rc::Refusal::no_overlap) || has(proposal,rc::Refusal::estimate_not_verified));
            std::error_code ignored;fs::remove_all(active.settings,ignored);
        }
    }
    // A camera that was turned or raised, not moved: the lines on the pose are the condition's own (tilt 1 degree here, height 3 percent).
    if(CaseWanted("refuse-pose")){
        struct PoseCase {const char* name;cr::PoseParameters pose;rc::Refusal reason;};
        const PoseCase pose_cases[]{{"tilt",{1.5,0,0,-30,0,0},rc::Refusal::tilt_exceeded},{"height",{0,0,0,-30,0,4.0},rc::Refusal::height_exceeded}};
        for(const auto& c:pose_cases){
            auto active=MakeActive(std::string("refuse-")+c.name,profile,MountOrientation::normal,raster,[](FixedCompositeConditionV3& e){
                TestRanges(e);
                e.registration.envelope={5,1,1.0,3};   // the estimate itself has room (the lines on p-hat - p_b); the lines on the pose are these
            });
            const Truth truth{MatrixOf(parent.camera_a),model.CameraHomography(c.pose)};
            const auto a=RenderFrame(truth.a,paper),b=RenderFrame(truth.b,paper);
            const auto proposal=rc::ProposeRecalibration(MakeInputs(active,&a,&b));
            describe(c.name,proposal);
            CHECK(!proposal.ready && !proposal.candidate && has(proposal,c.reason) && has(proposal,rc::Refusal::estimate_not_verified));
            std::error_code ignored;fs::remove_all(active.settings,ignored);
        }
    }
    // The estimate on the new profile must leave the seam alone to within the line: a line below what any estimate reaches refuses.
    if(CaseWanted("refuse-verify")){
        auto active=MakeActive("refuse-verify",profile,MountOrientation::normal,raster,[](FixedCompositeConditionV3& c){TestRanges(c);c.registration.limits.convergence_px=1e-4;});
        const auto a=RenderFrame(moved.a,paper),b=RenderFrame(moved.b,paper);
        const auto proposal=rc::ProposeRecalibration(MakeInputs(active,&a,&b));
        describe("verify",proposal);
        // A line below what the estimate reaches is refused at the fine stage or by the verification; never made.
        CHECK(!proposal.ready && !proposal.candidate && !proposal.refusals.empty());
        CHECK(has(proposal,rc::Refusal::estimate_not_verified) || has(proposal,rc::Refusal::reestimate_displacement) || has(proposal,rc::Refusal::reestimate_not_verified));
        // The caller's stop: nothing is estimated after it.
        auto stopping=MakeInputs(active,&a,&b);
        stopping.control.stop=[]{return true;};
        const auto stopped=rc::ProposeRecalibration(stopping);
        CHECK(!stopped.ready && has(stopped,rc::Refusal::stopped) && !stopped.candidate);
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
}

// Too far apart, beyond the sheet, beyond the render limit: refused from a pose alone (no images).
void RefusalsFromPose(){
    const auto profile=RigText(false);
    const render::OutputRaster raster=ps::Raster(560000,100000,740000,260000,254);
    const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
    auto active=MakeActive("refusals-pose",profile,MountOrientation::normal,raster,TestRanges);
    const rc::ProfileFit fit{0.1,0.3,50};
    const auto has=[](const rc::Proposal& p,rc::Refusal r){return std::find(p.refusals.begin(),p.refusals.end(),r)!=p.refusals.end();};
    // Apart by 130 mm: 30 mm of the 160 are left, less than the ranges need.
    {
        const auto proposal=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),PoseForShift(parameters.camera_b,130.0,false),fit,"{}");
        CHECK(!proposal.candidate && !proposal.ready && has(proposal,rc::Refusal::creation_check_failed) && !proposal.detail.empty());
        CHECK(proposal.display.has_overlap);
        CheckNear(proposal.display.narrowest_overlap_mm,30.0,0.3,"the overlap that is left is still reported");
        std::cerr<<"too far apart: "<<proposal.detail<<'\n';
    }
    // 60 mm: the middle of the new band passes; the old seam place and the old raster do not (the owner's two choices, each way).
    {
        const auto pose=PoseForShift(parameters.camera_b,60.0,false);
        auto keep=MakeInputs(active,nullptr,nullptr);keep.seam=rc::SeamPlacement::keep_previous;keep.raster=rc::RasterPlacement::keep;
        const auto old_place=rc::ProposeFromPose(keep,pose,fit,"{}");
        std::cerr<<"old seam place at 60 mm: candidate="<<old_place.candidate.has_value()<<" "<<old_place.detail<<'\n';
        CHECK(!old_place.candidate && has(old_place,rc::Refusal::creation_check_failed));
        const auto middle=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),pose,fit,"{}");
        CHECK(middle.candidate && middle.refusals.empty());
    }
    // Beyond the sheet, and beyond what the render accepts.
    {
        const auto proposal=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),PoseForShift(parameters.camera_b,500.0,false),fit,"{}");
        CHECK(!proposal.candidate && has(proposal,rc::Refusal::raster_outside_sheet));
        const auto huge=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),PoseForShift(parameters.camera_b,3500.0,false),fit,"{}");
        CHECK(!huge.candidate && (has(huge,rc::Refusal::raster_too_large) || has(huge,rc::Refusal::raster_outside_sheet)));
    }
    // Inputs that are wrong are the caller's error, not a refusal.
    {
        auto bad=MakeInputs(active,nullptr,nullptr);bad.pair_id="not a pair";
        REJECT(rc::ProposeFromPose(bad,cr::PoseParameters{},fit,"{}"));
        auto no_hash=MakeInputs(active,nullptr,nullptr);no_hash.hash=nullptr;
        REJECT(rc::ProposeFromPose(no_hash,cr::PoseParameters{},fit,"{}"));
        auto wrong_profile=MakeInputs(active,nullptr,nullptr);wrong_profile.profile_bytes=RigText(true);
        REJECT(rc::ProposeFromPose(wrong_profile,cr::PoseParameters{},fit,"{}"));
        REJECT(rc::ProposeRecalibration(MakeInputs(active,nullptr,nullptr)));   // images are required
    }
    std::error_code ignored;fs::remove_all(active.settings,ignored);
}

}

int main(int argc,char** argv){
    if(argc<2){std::cerr<<"usage: m2_fixed_composite_recalibration_contracts <rig-profile-fixture-folder>\n";return 2;}
    fixture=ReadAll(fs::path(argv[1])/"calibrated-draft.json");
    if(fixture.empty()){std::cerr<<"the fixture is missing\n";return 2;}
    try{
        const auto started=std::chrono::steady_clock::now();
        const auto lap=[&](const char* name){
            std::cerr<<name<<" done at "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()<<" s\n";
        };
        Kinds();lap("kinds");
        FormatAndReader();lap("format and reader");
        ChainOnDisk();lap("chain on disk");
        ProfileBake();lap("profile bake");
        FitUnits();lap("fit units");
        GeometryFromPose();lap("geometry from a pose");
        RefusalsFromPose();lap("refusals from a pose");
        Pipeline();lap("pipeline");
        Refusals();lap("refusals");
    }catch(const std::exception& e){
        std::cerr<<"FAILED: an exception: "<<e.what()<<'\n';
        return 1;
    }
    std::cout<<"fixed composite recalibration: "<<checks<<" checks, "<<failures<<" failures\n";
    return failures==0?0:1;
}
