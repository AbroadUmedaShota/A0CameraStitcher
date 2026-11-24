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
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/experimental/fixed_composite_recalibration.hpp"
#include "a0/m2/experimental/fixed_composite_selection.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/phase0/experimental/fixed_composite_cli_v3.hpp"
#include "a0/phase0/experimental/fixed_composite_preview.hpp"
#include "a0/phase0/experimental/fixed_composite_tool_v3.hpp"
#include "a0/phase0/experimental/pair_review_synthetic.hpp"
#include "registration_fixtures.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
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
// The same rig with B moved `left_mm` to the left on the drawing, so that its right side lies on the sheet (at 1302 - left_mm).
std::string RigTextMovedLeft(bool turned,double left_mm){
    using ps::detail::WithMatrix;
    if(turned)return WithMatrix(WithMatrix(fixture,0,{-10,0,7359-100.0,0,-10,4911-100.0}),1,{-10,0,7359+5660.0-10*left_mm,0,-10,4911-100.0});
    return WithMatrix(WithMatrix(fixture,0,{10,0,100,0,10,100}),1,{10,0,-5660+10*left_mm,0,10,100});
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
        const auto grown=rc::ProposeRaster(active.condition,parent,pose,rc::RasterPlacement::follow_camera_b,rc::SeamPlacement::band_center);
        CHECK(grown.dpi==raster.dpi && grown.height_pixels==raster.height_pixels && grown.region_um.left==raster.region_um.left &&
            grown.region_um.top==raster.region_um.top && grown.region_um.bottom==raster.region_um.bottom);
        CHECK(std::abs(static_cast<long long>(grown.width_pixels)-static_cast<long long>(raster.width_pixels)-std::llround(shift*10))<=1);
        CHECK(Throws([&]{render::ValidateOutputRaster(grown);})==false);
        const auto kept=rc::ProposeRaster(active.condition,parent,pose,rc::RasterPlacement::keep,rc::SeamPlacement::band_center);
        CHECK(kept.width_pixels==raster.width_pixels && kept.region_um.right==raster.region_um.right);
    }
    CHECK(rc::ProposeRaster(active.condition,parent,cr::PoseParameters{},rc::RasterPlacement::follow_camera_b,rc::SeamPlacement::band_center).width_pixels==raster.width_pixels);

    // A hand adjustment beyond the range (pose_outside_envelope, #434): the raster is counted from the profile's own placement, not from that
    // base pose. Beyond the range by a little, and by so much that camera B covers no row of the raster at that pose (the old measure threw).
    {
        const auto pose=PoseForShift(parameters.camera_b,30.0,false);
        const auto from_profile=rc::ProposeRaster(active.condition,parent,pose,rc::RasterPlacement::follow_camera_b,rc::SeamPlacement::band_center);
        for(const double tx:{12.0,3000.0}){
            auto beyond=active.condition;
            beyond.base_pose=cr::PoseParameters{0,0,0,tx,0,0};
            CHECK(std::hypot(beyond.base_pose.tx_mm,beyond.base_pose.ty_mm)>beyond.registration.envelope.position_mm);
            std::optional<render::OutputRaster> counted;
            CHECK(!Throws([&]{counted=rc::ProposeRaster(beyond,parent,pose,rc::RasterPlacement::follow_camera_b,rc::SeamPlacement::band_center);}));
            CHECK(counted && counted->width_pixels==from_profile.width_pixels && counted->region_um.right==from_profile.region_um.right);
            CHECK(rc::ProposeRaster(beyond,parent,pose,rc::RasterPlacement::keep,rc::SeamPlacement::band_center).width_pixels==raster.width_pixels);
        }
        // Inside the range the base pose still counts: the same pose from another base pose gives another raster.
        auto inside=active.condition;
        inside.base_pose=cr::PoseParameters{0,0,0,3.0,0,0};
        CHECK(rc::ProposeRaster(inside,parent,pose,rc::RasterPlacement::follow_camera_b,rc::SeamPlacement::band_center).width_pixels!=from_profile.width_pixels);
    }

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
        CHECK(d.has_new && d.height_new_px==c.raster.height_pixels);
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

    // The text of the estimate's record is not an input of the registration: two records that differ only in their text give the same
    // mappings, raster, seam and base pose to the bit. What follows the text is the hashes (the condition's report hash, the profile's
    // provenance identifier, and so the profile's and the revision's hashes).
    {
        const auto pose=PoseForShift(parameters.camera_b,30.0,false);
        const auto first=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),pose,fit,record);
        const auto second=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),pose,fit,"{\"schema\":\"another-test-record\",\"referenceCamera\":\"CAM-X\"}");
        CHECK(first.candidate && second.candidate);
        if(first.candidate && second.candidate){
            const auto& a=*first.candidate;const auto& b=*second.candidate;
            for(int i=0;i<9;++i){
                const auto k=static_cast<std::size_t>(i);
                CHECK(SameBits(a.binding.parameters.camera_a.document_to_image[k],b.binding.parameters.camera_a.document_to_image[k]));
                CHECK(SameBits(a.binding.parameters.camera_b.document_to_image[k],b.binding.parameters.camera_b.document_to_image[k]));
            }
            const auto& ra=a.condition.raster;const auto& rb=b.condition.raster;
            CHECK(ra.width_pixels==rb.width_pixels && ra.height_pixels==rb.height_pixels && ra.dpi==rb.dpi &&
                ra.region_um.left==rb.region_um.left && ra.region_um.top==rb.region_um.top && ra.region_um.right==rb.region_um.right && ra.region_um.bottom==rb.region_um.bottom);
            CHECK(a.condition.seam.column==b.condition.seam.column && a.condition.seam.origin==b.condition.seam.origin);
            CHECK(a.condition.base_pose.IsZero() && b.condition.base_pose.IsZero());
            CHECK(first.estimate_record_sha256!=second.estimate_record_sha256 && a.condition.revision.change.report_sha256!=b.condition.revision.change.report_sha256);
            CHECK(a.profile_bytes!=b.profile_bytes && a.profile_name!=b.profile_name && a.sha256!=b.sha256);
        }
    }

    // A rig mounted a half turn stores the photographs the other way round: the same physical move gives the same distances of the seam to
    // the photographs' edges on the drawing's right (A) and left (B) as for the normal mounting (the stored x runs backwards).
    {
        const auto turned_profile=RigText(true);
        const auto turned_parameters=RigProfileV2::Parse(turned_profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        auto turned=MakeActive("geometry-turned",turned_profile,MountOrientation::rotated_180,raster,TestRanges);
        for(const double shift:{5.0,30.0,60.0}){
            const auto pose=PoseForShift(turned_parameters.camera_b,shift,true);
            const auto proposal=rc::ProposeFromPose(MakeInputs(turned,nullptr,nullptr),pose,fit,record);
            CHECK(proposal.refusals.empty() && proposal.candidate);
            const auto& d=proposal.display;
            CHECK(d.has_edge_distance);
            CheckNear(d.edge_distance_a_px,7359-(10*(646.0+shift/2)+100),3.0,"half turn: the seam's distance to the drawing-right edge of A");
            CheckNear(d.edge_distance_b_px,10*(646.0+shift/2)-5660-10*shift,3.0,"half turn: the seam's distance to the drawing-left edge of B");
            CheckNear(d.motion.camera_motion_mm.x,shift,0.2,"half turn: B moved to the right of the drawing");
        }
        std::error_code ignored_turned;fs::remove_all(turned.settings,ignored_turned);
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

// ---- Section 3b. The raster of a recalibration, worked out from the sides of the sensors ------------------------------------------
// A second working of what ProposeRaster gives with RasterPlacement::follow_camera_b (docs/design/recalibration-from-pair.md 3.2), for the
// synthetic rig, whose lens coefficients are all 0, and for poses without a paper surface. Without a lens, the stored position of the output
// sample (x, y) is a ratio of linear forms of (x, y): the samples that a camera sees are those on the inner side of the lines that its
// sensor's four sides make on the output raster. An end of a camera's cover on a column or on a row is where that column or row crosses one
// of the lines, and is solved here in closed form. The steps are those of the design's text: gate 1, the rows that every vertex of the
// creation check's range keeps on the columns the seam can take, the last column, the bring-back to what B's move allows, and the right
// edge that follows B's right side. From the code under test this takes the mappings (the pose model, the paper's homography and its
// origin, the camera matrices), the range's vertices and axis ends, and the answer it is compared with (ProposeRaster); none of its
// searches for an end.
namespace sides {
constexpr double kInf=std::numeric_limits<double>::infinity();
constexpr double kSensorWidth=ps::kWidth,kSensorHeight=ps::kHeight;
using Form=std::array<double,3>;   // a x + b y + c over the output sample (x, y)
double Eval(const Form& f,double x,double y){return f[0]*x+f[1]*y+f[2];}
Form Sum(double a,const Form& p,double b,const Form& q,double c,const Form& r){
    return {a*p[0]+b*q[0]+c*r[0],a*p[1]+b*q[1]+c*r[1],a*p[2]+b*q[2]+c*r[2]};
}
// One camera over one raster: the stored position of the sample (x, y) is (u / w, v / w), where the denominator g of the document
// homography (when there is one) and w are both positive.
struct Cover {Form u,v,w,g;};
Cover CoverOf(const render::OutputRaster& raster,const render::Matrix3& camera,const std::optional<render::Matrix3>& document=std::nullopt){
    const double p=25.4/raster.dpi,left=static_cast<double>(raster.region_um.left)/1000.0,top=static_cast<double>(raster.region_um.top)/1000.0;
    const Form x{p,0,left+0.5*p},y{0,p,top+0.5*p},one{0,0,1};
    Form gx=x,gy=y,gw=one;
    if(document){
        const auto& d=*document;
        gx=Sum(d[0],x,d[1],y,d[2],one);gy=Sum(d[3],x,d[4],y,d[5],one);gw=Sum(d[6],x,d[7],y,d[8],one);
    }
    const auto& h=camera;
    return {Sum(h[0],gx,h[1],gy,h[2],gw),Sum(h[3],gx,h[4],gy,h[5],gw),Sum(h[6],gx,h[7],gy,h[8],gw),gw};
}
// The half planes of the cover: g > 0, w > 0, u >= 0, u < W w and, for the whole sensor, v >= 0, v < H w. `whole` false is the stored x
// only (the two sides that run across the rows in both mountings of the rig).
std::vector<Form> HalfPlanes(const Cover& c,bool whole){
    std::vector<Form> s{c.g,c.w,c.u,Sum(kSensorWidth,c.w,-1,c.u,0,c.u)};
    if(whole){s.push_back(c.v);s.push_back(Sum(kSensorHeight,c.w,-1,c.v,0,c.v));}
    return s;
}
bool Sees(const Cover& c,double x,double y,bool whole){
    const double g=Eval(c.g,x,y),w=Eval(c.w,x,y);
    if(!(g>0) || !(w>0))return false;
    const double u=Eval(c.u,x,y)/w,v=Eval(c.v,x,y)/w;
    return u>=0 && u<kSensorWidth && (!whole || (v>=0 && v<kSensorHeight));
}
// The samples of a column (x = fixed) or of a row (y = fixed) that the camera sees: one interval of the other coordinate.
struct Line {bool any{};double first{},last{};};
Line On(const Cover& c,bool whole,bool column,double fixed){
    double first=-kInf,last=kInf;
    for(const auto& f:HalfPlanes(c,whole)){
        const double a=column?f[1]:f[0],b=(column?f[0]:f[1])*fixed+f[2];
        if(a>0)first=std::max(first,-b/a);
        else if(a<0)last=std::min(last,-b/a);
        else if(!(b>0))return {};
    }
    return {first<=last,first,last};
}
// The whole numbers of that interval inside [low, high]. The closed form and the sample's own arithmetic can differ in the last bit at a
// side; the ends are settled on the samples themselves.
using Span=std::pair<long long,long long>;
std::optional<Span> Integers(const Cover& c,bool whole,bool column,double fixed,long long low,long long high){
    const auto line=On(c,whole,column,fixed);
    if(!line.any || low>high)return std::nullopt;
    const auto sees=[&](long long n){return column?Sees(c,fixed,static_cast<double>(n),whole):Sees(c,static_cast<double>(n),fixed,whole);};
    long long first=std::max(low,static_cast<long long>(std::ceil(std::max(line.first,static_cast<double>(low)-1.0))));
    long long last=std::min(high,static_cast<long long>(std::floor(std::min(line.last,static_cast<double>(high)+1.0))));
    for(int i=0;i<2 && first<=last && !sees(first);++i)++first;
    for(int i=0;i<2 && last>=first && !sees(last);++i)--last;
    for(int i=0;i<2 && first>low && sees(first-1);++i)--first;
    for(int i=0;i<2 && last<high && sees(last+1);++i)++last;
    if(first>last || !sees(first) || !sees(last))return std::nullopt;
    return Span{first,last};
}

// The raster with the rows first..last of `old`: both edges move inwards to a whole micrometre (design 3.2), the height is the ceiling.
render::OutputRaster OfRows(const render::OutputRaster& old,long long first,long long last){
    const long long dpi=old.dpi,top0=old.region_um.top;
    const long long top=top0+(first*25400+dpi-1)/dpi;
    const long long bottom=std::min<long long>(old.region_um.bottom,top0+((last+1)*25400)/dpi);
    return render::OutputRaster{render::DocumentRegionUm{old.region_um.left,top,old.region_um.right,bottom},old.dpi,old.width_pixels,
        static_cast<std::uint32_t>(((bottom-top)*dpi+25399)/25400)};
}
// The raster with `width` pixels from the same left edge: the right edge is the largest whole micrometre whose ceiling is that width.
render::OutputRaster OfWidth(const render::OutputRaster& old,long long width){
    return render::OutputRaster{render::DocumentRegionUm{old.region_um.left,old.region_um.top,old.region_um.left+width*25400/static_cast<long long>(old.dpi),
        old.region_um.bottom},old.dpi,static_cast<std::uint32_t>(width),old.height_pixels};
}
bool SameRegion(const render::OutputRaster& a,const render::OutputRaster& b){
    return a.region_um.left==b.region_um.left && a.region_um.top==b.region_um.top && a.region_um.right==b.region_um.right && a.region_um.bottom==b.region_um.bottom &&
        a.dpi==b.dpi && a.width_pixels==b.width_pixels && a.height_pixels==b.height_pixels;
}
std::string Text(const render::OutputRaster& r){
    return std::to_string(r.region_um.left)+","+std::to_string(r.region_um.top)+","+std::to_string(r.region_um.right)+","+std::to_string(r.region_um.bottom)+" "+
        std::to_string(r.width_pixels)+"x"+std::to_string(r.height_pixels);
}

struct Answer {
    // False when the scene's numbers sit on a rounding boundary of the code's own searches, or on a rule that is not worked out here
    // (then the scene does not decide anything and has to be changed).
    bool decisive{true};
    std::string undecided;
    render::OutputRaster raster{render::DocumentRegionUm{0,0,0,0},0,0,0};   // what ProposeRaster has to give (set first thing)
    // What decided each end of the rows: gate (nothing given up, gate 1 holds), none (nothing given up otherwise), edge (the raster's own
    // edge), seam vertices, overlap, last column, move (brought back to what B's move allows).
    std::string top_by,bottom_by;
    long long first{},last{};        // the rows kept (the whole raster when nothing is given up)
    // B's ends on the seam columns: before at the parent's column, after at the leftmost column the new seam can take (rows, unclamped).
    bool has_ends{};
    double column_before{},column_after{};
    Line ends_before,ends_after;
    long long allowed_top{},allowed_bottom{};
    double right_moved{};            // how far the smallest of B's right side moved over the rows that decide the width (pixels)
    std::optional<std::size_t> top_vertex,bottom_vertex;   // the vertex that last narrowed each end in the seam columns' step
    std::string stopped;             // why no row is given up after gate 1 (for the messages)
    long long low{-1},high{-1};      // the leftmost and the rightmost column the new seam can take (for the messages)
};
void Undecided(Answer& a,const std::string& why){a.decisive=false;a.undecided+=(a.undecided.empty()?"":"; ")+why;}
// Whether a value that the code rounds is far enough from the rounding's boundary that its own search (doubling and bisection, a few
// thousandths of a pixel) cannot land on the other side.
bool FarFromHalf(double v){const double f=v-std::floor(v);return std::abs(f-0.5)>=0.05;}
bool FarFromWhole(double v){const double f=v-std::floor(v);return f>=0.02 && f<=0.98;}

Answer RasterBySides(const FixedCompositeConditionV3& active,const FixedCompositeProfileBinding& parent,const cr::PoseParameters& pose,rc::SeamPlacement seam,
    const std::vector<CreationOffsetV3>& vertices,const std::vector<CreationOffsetV3>& pose_vertices){
    Answer out;
    const auto& old=active.raster;
    const long long height=old.height_pixels,width=old.width_pixels,last_row=height-1,middle=last_row/2;
    out.raster=old;out.first=0;out.last=last_row;
    // The scenes' parents are at the profile's own placement and have no paper lift (no layer 2): what follows takes the parent's
    // placement as the calibrated mapping and the columns the seam can take without a lift cap.
    if(!active.base_pose.IsZero() || active.registration.parallax){Undecided(out,"a base pose or layer 2 in the parent");return out;}
    const bool keep=seam==rc::SeamPlacement::keep_previous;
    const auto& camera_a=parent.parameters.camera_a;const auto& camera_b=parent.parameters.camera_b;
    const cr::PoseModel model_b(camera_b);
    const Cover a=CoverOf(old,camera_a.document_to_image);
    const Cover before=CoverOf(old,camera_b.document_to_image);
    const Cover after=pose.IsZero()?before:CoverOf(old,camera_b.document_to_image,model_b.Homography(pose));
    // The moved B as the new profile names it, and the range's offsets around it.
    const render::CameraProjection moved_b(camera_b.intrinsics,camera_b.distortion,model_b.CameraHomography(pose));
    const cr::PoseModel model_moved(moved_b);
    const Cover moved=CoverOf(old,moved_b.document_to_image);
    const auto centre_a=cr::PoseModel(camera_a).Centre({});
    const auto at_offset=[&](const render::OutputRaster& raster,const CreationOffsetV3& o,std::uint32_t origin_column){
        const auto p=CreationOffsetPoseV3(cr::PoseParameters{},o);
        const auto s=CreationOffsetSurfaceV3(o);
        if(!s.IsZero())return CoverOf(raster,moved_b.document_to_image,cr::RegisteredHomography(model_moved,centre_a,cr::SurfaceOriginMm(raster,origin_column),p,s));
        if(!p.IsZero())return CoverOf(raster,moved_b.document_to_image,model_moved.Homography(p));
        return CoverOf(raster,moved_b.document_to_image);
    };
    // Half the blend width, as the creation check counts it: the columns of the blend beyond the seam column.
    const long long half=(static_cast<long long>(kSeamBlendDesign.width_pixels)+1)/2-1;

    // ---- The right edge: by B's right side alone, when on the parent's middle row the whole cover ends there ----
    const auto right_side=[&](const Cover& c,long long y,bool whole){const auto l=On(c,whole,false,static_cast<double>(y));return l.any && l.last>=0?l.last:kInf;};
    const auto smallest=[&](const Cover& c,long long first,long long last,bool whole){double s=kInf;for(long long y=first;y<=last;++y)s=std::min(s,right_side(c,y,whole));return s;};
    bool by_sides{};
    {
        const auto whole=On(before,true,false,static_cast<double>(middle)),across=On(before,false,false,static_cast<double>(middle));
        by_sides=whole.any && across.any && whole.last==across.last;
        if(whole.any && across.any && whole.last<across.last && across.last-whole.last<1)Undecided(out,"the parent's middle row ends near a corner of B");
    }
    const auto right_move=[&](long long first,long long last,bool whole)->std::optional<long long>{
        const double moved_by=smallest(after,first,last,whole)-smallest(before,first,last,whole);
        if(!std::isfinite(moved_by))return std::nullopt;
        out.right_moved=moved_by;
        if(!FarFromHalf(moved_by))Undecided(out,"the right edge moves by half a pixel off a whole one");
        return std::llround(moved_by);
    };
    const auto delta=by_sides?right_move(0,last_row,false):right_move(0,last_row,true);
    if(!delta){Undecided(out,"B has no right side on the raster");return out;}
    const auto by_width=OfWidth(old,width+*delta);
    out.raster=by_width;out.top_by=out.bottom_by="none";

    // ---- Gate 1: the creation check's named conditions on the raster that the right edge alone gives ----
    {
        auto points=CreationRangeAxisEndsV3(active);
        points.insert(points.begin(),CreationOffsetV3{});
        const Cover a_wide=CoverOf(by_width,camera_a.document_to_image);
        if(keep){
            const auto column=active.seam.column;
            bool holds=column<by_width.width_pixels;
            for(const auto& o:points){
                if(!holds)break;
                const auto b=at_offset(by_width,o,column);
                const double x=static_cast<double>(column);
                for(long long y=0;y<height && holds;++y){
                    const double r=static_cast<double>(y);
                    holds=Sees(b,x,r,true) && Sees(a_wide,x,r,true) && Sees(a_wide,x+static_cast<double>(half),r,true);
                }
            }
            if(holds){out.top_by=out.bottom_by="gate";return out;}
        }else{
            // The candidate's seam column is the middle of its band. Gate 1 is taken here only where it fails for every column of the raster at
            // the ends of the pose's own axes (without a paper surface B's cover does not depend on the seam column).
            long long low=0,high=static_cast<long long>(by_width.width_pixels)-1;
            for(const auto& o:points){
                if(!CreationOffsetSurfaceV3(o).IsZero())continue;
                const auto b=at_offset(by_width,o,0);
                for(long long y=0;y<height && low<=high;++y){
                    const auto rb=Integers(b,true,false,static_cast<double>(y),0,static_cast<long long>(by_width.width_pixels)-1);
                    const auto ra=Integers(a_wide,true,false,static_cast<double>(y),0,static_cast<long long>(by_width.width_pixels)-1+half);
                    if(!rb || !ra){low=high+1;break;}
                    low=std::max({low,rb->first,ra->first});high=std::min({high,rb->second,ra->second-half});
                }
                if(low>high)break;
            }
            if(low<=high){Undecided(out,"gate 1 is not decided by the ends of the pose's axes alone");return out;}
        }
    }

    // ---- The rows: the overlap of A and B on every row, then the range's vertices on the columns the seam can take ----
    // A row "has an overlap" in the code when its search finds the run; that search is not worked out here, so the rows are taken both
    // ways the search can end (a run of any length, a run of at least 8 columns), and the scene decides only when both give the same.
    std::vector<char> any(static_cast<std::size_t>(height)),eight(static_cast<std::size_t>(height));
    std::vector<long long> run_first(static_cast<std::size_t>(height)),run_end(static_cast<std::size_t>(height));
    for(long long y=0;y<height;++y){
        const auto ra=Integers(a,true,false,static_cast<double>(y),0,width-1),rb=Integers(moved,true,false,static_cast<double>(y),0,width-1);
        if(!ra || !rb)continue;
        const long long f=std::max(ra->first,rb->first),l=std::min(ra->second,rb->second);
        if(f>l)continue;
        const auto k=static_cast<std::size_t>(y);
        any[k]=1;eight[k]=l-f+1>=8;run_first[k]=f;run_end[k]=l+1;
    }
    const auto clamp_column=[&](long long c){return static_cast<std::uint32_t>(std::clamp<long long>(c,0,width-1));};
    const auto rows_with=[&](const std::vector<char>& ok)->Answer{
        Answer r=out;
        long long smallest_first=std::numeric_limits<long long>::max(),smallest_end=smallest_first;
        bool any_row=false;
        for(long long y=0;y<height;++y){
            const auto k=static_cast<std::size_t>(y);
            if(!ok[k])continue;
            any_row=true;smallest_first=std::min(smallest_first,run_first[k]);smallest_end=std::min(smallest_end,run_end[k]);
        }
        if(!any_row){r.stopped="no row has an overlap";return r;}
        const long long low=keep?static_cast<long long>(active.seam.column):(smallest_first+smallest_end)/2;
        r.low=low;
        long long f=0,l=last_row;
        std::string top_by="edge",bottom_by="edge";
        // The rows in which B covers `column` at every one of `offsets`, from the middle row; false when one of them does not cover the
        // column on the middle row (then nothing is given up).
        const auto narrow=[&](const std::vector<CreationOffsetV3>& offsets,std::uint32_t origin_column,double column,const char* name,bool record)->bool{
            if(middle<f || middle>l)return false;
            long long nf=f,nl=l;
            std::optional<std::size_t> top_i,bottom_i;
            for(std::size_t i=0;i<offsets.size();++i){
                const auto c=at_offset(old,offsets[i],origin_column);
                if(!Sees(c,column,static_cast<double>(middle),true))return false;
                const auto s=Integers(c,true,true,column,nf,nl);
                if(!s)return false;
                if(s->first>nf){nf=s->first;top_i=i;}
                if(s->second<nl){nl=s->second;bottom_i=i;}
            }
            if(nf>f){f=nf;top_by=name;if(record)r.top_vertex=top_i;}
            if(nl<l){l=nl;bottom_by=name;if(record)r.bottom_vertex=bottom_i;}
            return true;
        };
        if(!narrow(vertices,clamp_column(low),static_cast<double>(low-half),"seam vertices",true)){r.stopped="a vertex misses the left seam column on the middle row";return r;}
        if(!ok[static_cast<std::size_t>(middle)]){r.stopped="the middle row has no overlap";return r;}
        {
            long long cf=middle,cl=middle;
            while(cf>f && ok[static_cast<std::size_t>(cf-1)])--cf;
            while(cl<l && ok[static_cast<std::size_t>(cl+1)])++cl;
            if(cf>f){f=cf;top_by="overlap";}
            if(cl<l){l=cl;bottom_by="overlap";}
        }
        long long high=low;
        if(!keep){
            long long largest_first=0,largest_end=0;
            for(long long y=f;y<=l;++y){largest_first=std::max(largest_first,run_first[static_cast<std::size_t>(y)]);largest_end=std::max(largest_end,run_end[static_cast<std::size_t>(y)]);}
            high=(largest_first+largest_end)/2;
        }
        r.high=high;
        if(!narrow(vertices,clamp_column(high),static_cast<double>(high+half),"seam vertices",true)){r.stopped="a vertex misses the right seam column on the middle row";return r;}
        if(!by_sides){r.stopped="B's sides do not end the parent's middle row";return r;}
        // The last column of the new raster: when A does not see it on every row kept, the rows in which B sees it at every vertex of the
        // pose's range (once; when B does not see it on the middle row at one of them, the rows stay).
        const auto first_move=right_move(f,l,false);
        if(!first_move){r.stopped="no right side over the rows";return r;}
        {
            const double last_column=static_cast<double>(width+*first_move)-1.0;
            bool a_all=true;
            for(long long y=f;y<=l && a_all;++y)a_all=Sees(a,last_column,static_cast<double>(y),true);
            if(!a_all){
                const long long keep_f=f,keep_l=l;const std::string keep_top=top_by,keep_bottom=bottom_by;
                if(!narrow(pose_vertices,clamp_column(high),last_column,"last column",false)){f=keep_f;l=keep_l;top_by=keep_top;bottom_by=keep_bottom;}
            }
        }
        // What B's move allows: how far B's upper and lower ends on the seam column moved, the parent's at its column and the new one at
        // the leftmost column the new seam can take, each looked for up to one raster height beyond the raster.
        r.column_before=static_cast<double>(active.seam.column);r.column_after=static_cast<double>(low);
        if(!Sees(before,r.column_before,static_cast<double>(middle),true) || !Sees(after,r.column_after,static_cast<double>(middle),true)){r.stopped="B misses a seam column on the middle row";return r;}
        r.ends_before=On(before,true,true,r.column_before);r.ends_after=On(after,true,true,r.column_after);r.has_ends=true;
        const auto top_of=[&](const Line& e){return std::max(e.first,-static_cast<double>(height));};
        const auto bottom_of=[&](const Line& e){return std::min(e.last,2.0*static_cast<double>(height)-1.0);};
        const double up=top_of(r.ends_after)-top_of(r.ends_before),down=bottom_of(r.ends_before)-bottom_of(r.ends_after);
        r.allowed_top=static_cast<long long>(std::floor(std::max(0.0,up)));r.allowed_bottom=static_cast<long long>(std::floor(std::max(0.0,down)));
        const long long kf=std::min(f,r.allowed_top),kl=std::max(l,last_row-r.allowed_bottom);
        if(kf<f){top_by="move";if(!FarFromWhole(up))Undecided(r,"B's upper end moved by a whole number of rows");}
        if(kl>l){bottom_by="move";if(!FarFromWhole(down))Undecided(r,"B's lower end moved by a whole number of rows");}
        if(kf==0 && kl==last_row){r.stopped="the move allows no row to be given up";return r;}
        const auto move=right_move(kf,kl,false);
        if(!move){r.stopped="no right side over the rows kept";return r;}
        r.right_moved=out.right_moved;
        r.first=kf;r.last=kl;r.top_by=top_by;r.bottom_by=bottom_by;
        const auto rows=OfRows(old,kf,kl);
        r.raster=*move==0?rows:OfWidth(rows,static_cast<long long>(rows.width_pixels)+*move);
        return r;
    };
    const auto by_any=rows_with(any),by_eight=rows_with(eight);
    auto answer=by_any;
    if(!SameRegion(by_any.raster,by_eight.raster) || by_any.top_by!=by_eight.top_by || by_any.bottom_by!=by_eight.bottom_by)
        Undecided(answer,"the rows that have an overlap decide by the rule of the code's search");
    if(!out.decisive)Undecided(answer,out.undecided);
    return answer;
}

// The product's own mapping, at a point: whether the sample is inside the sensor (the whole sensor, or the stored x only).
bool MappingSees(const render::OutputToCamera& map,double x,double y,bool whole){
    render::Point raw{};
    try{if(!map({x,y},raw))return false;}
    catch(const std::invalid_argument&){return false;}
    return std::isfinite(raw.x) && std::isfinite(raw.y) && raw.x>=0 && raw.x<kSensorWidth && (!whole || (raw.y>=0 && raw.y<kSensorHeight));
}
}

void RasterFromSensorSides(){
    // Six scenes of the synthetic rig: B moved along the baseline and up or down, turned, in both mountings, with the paper range of the
    // condition and without it, and with the seam in the middle of the band and at the previous column. Each is meant for one rule of the
    // design: what decides an end of the rows kept is named, and the working above has to agree with it before the code is compared.
    const auto ranges=[](bool surface){
        return std::function<void(FixedCompositeConditionV3&)>([surface](FixedCompositeConditionV3& c){
            TestRanges(c);c.registration.parallax.reset();
            if(!surface)c.registration.surface_envelope.reset();
        });
    };
    struct Scene {
        const char* name;bool turned;double moved_left_mm;render::OutputRaster raster;cr::PoseParameters pose;rc::SeamPlacement seam;bool surface;
        const char* top_by;const char* bottom_by;
    };
    const auto band=rc::SeamPlacement::band_center,previous=rc::SeamPlacement::keep_previous;
    const Scene scenes[]{
        // Up by 12 mm, to the left by 15 mm, turned 0.3 degree: the vertices reach past what the move allows at the bottom.
        {"up-left-turned",false,0,ps::Raster(560000,300000,740000,470000,254),{0,0,0.3,15,12,0},band,true,"edge","move"},
        // Down by 30 mm, apart by 10 mm, turned: the same at the top. (Turned the other way, B's upper side would cross the rows just under it
        // near A's right edge, a row there would have an overlap of a few columns, and the leftmost column the seam can take would fall near
        // B's left side, where a vertex does not cover it: no row is given up then.)
        {"down-apart-turned",false,0,ps::Raster(560000,5000,740000,165000,254),{0,0,0.5,-10,-30,0},band,true,"move","edge"},
        // Up by 20 mm, tilted, on a raster that ends far above B's lower edge: the vertices keep more rows than the move would give up.
        {"up-tilted-vertices",false,0,ps::Raster(560000,290000,740000,460000,254),{0.2,0,0.4,8,20,0},band,false,"edge","seam vertices"},
        // Half turn, to the right by 20 mm, up by 3 mm, turned: gate 1 holds at the previous seam column and only the right edge moves.
        {"half-turn-gate",true,0,ps::Raster(560000,100000,740000,260000,254),{0,0,0.6,20,-3,0},previous,true,"gate","gate"},
        // Half turn, up by 10 mm, to the left by 25 mm, turned: the move decides at the bottom.
        {"half-turn-up-left",true,0,ps::Raster(560000,300000,740000,470000,254),{0,0,-0.4,-25,-10,0},band,true,"edge","move"},
        // B moved left on the drawing so that the raster reaches near its right side, A not seeing the last column: up by 29 mm and turned so
        // that B's lower side rises towards the right, B's lower side at the last column decides the bottom.
        {"near-right-last-column",false,152.0,ps::Raster(560000,280000,1127000,450000,254),{0,0,0.8,0,29,0},previous,true,"edge","last column"},
    };
    for(const auto& scene:scenes){
        const std::string name=std::string("sides ")+scene.name;
        const auto profile=scene.moved_left_mm>0?RigTextMovedLeft(scene.turned,scene.moved_left_mm):RigText(scene.turned);
        auto active=MakeActive(std::string("sides-")+scene.name,profile,scene.turned?MountOrientation::rotated_180:MountOrientation::normal,scene.raster,ranges(scene.surface));
        const auto parent=BindFixedCompositeProfileV3(active.condition,profile,kHash);
        const auto vertices=CreationRangeVerticesV3(active.condition,true),pose_vertices=CreationRangeVerticesV3(active.condition,false);
        const auto want=sides::RasterBySides(active.condition,parent,scene.pose,scene.seam,vertices,pose_vertices);
        std::optional<render::OutputRaster> got;
        try{got=rc::ProposeRaster(active.condition,parent,scene.pose,rc::RasterPlacement::follow_camera_b,scene.seam);}
        catch(const std::exception& e){std::cerr<<name<<": ProposeRaster threw: "<<e.what()<<'\n';}
        std::cerr<<name<<": rows "<<want.first<<".."<<want.last<<" (top "<<want.top_by<<", bottom "<<want.bottom_by<<"), allowed "<<want.allowed_top<<"/"<<want.allowed_bottom<<
            ", right "<<want.right_moved<<" px, seam columns "<<want.low<<".."<<want.high<<", vertex "<<(want.top_vertex?std::to_string(*want.top_vertex):"-")<<"/"<<(want.bottom_vertex?std::to_string(*want.bottom_vertex):"-")<<
            ", worked out "<<sides::Text(want.raster)<<", code "<<(got?sides::Text(*got):std::string("none"))<<(want.stopped.empty()?"":", stopped: "+want.stopped)<<(want.decisive?"":" (not decided: "+want.undecided+")")<<'\n';
        Check(want.decisive,(name+": the scene's numbers are away from every rounding boundary of the code's searches").c_str());
        Check(want.top_by==scene.top_by && want.bottom_by==scene.bottom_by,(name+": the rule the scene is for decides the rows").c_str());
        Check(got.has_value() && sides::SameRegion(*got,want.raster),(name+": ProposeRaster gives the raster worked out from the sides").c_str());

        // The lines are the product's own mappings to 1e-6 mm on the drawing: just inside an end of B's cover the mapping sees the sample,
        // just outside it does not. B's upper and lower ends on the seam columns (where they are inside what is looked at), and B's right
        // side on the first, the middle and the last row kept.
        const double probe=1e-6/(25.4/scene.raster.dpi);
        const cr::PoseModel model_b(parent.parameters.camera_b);
        const auto map_before=PoseCorrectedDocumentOutputToCamera(scene.raster,parent.parameters.camera_b,model_b,cr::PoseParameters{});
        const auto map_after=PoseCorrectedDocumentOutputToCamera(scene.raster,parent.parameters.camera_b,model_b,scene.pose);
        const double h=static_cast<double>(scene.raster.height_pixels);
        const sides::Cover cover_before=sides::CoverOf(scene.raster,parent.parameters.camera_b.document_to_image);
        const sides::Cover cover_after=sides::CoverOf(scene.raster,parent.parameters.camera_b.document_to_image,model_b.Homography(scene.pose));
        struct Placement {const render::OutputToCamera* map;const sides::Cover* cover;double column;sides::Line ends;};
        const Placement placements[]{{&map_before,&cover_before,want.column_before,want.ends_before},{&map_after,&cover_after,want.column_after,want.ends_after}};
        for(const auto& at:placements){
            if(want.has_ends){
                if(at.ends.first>-h)
                    Check(sides::MappingSees(*at.map,at.column,at.ends.first+probe,true) && !sides::MappingSees(*at.map,at.column,at.ends.first-probe,true),(name+": B's upper end on the seam column").c_str());
                if(at.ends.last<2*h-1)
                    Check(sides::MappingSees(*at.map,at.column,at.ends.last-probe,true) && !sides::MappingSees(*at.map,at.column,at.ends.last+probe,true),(name+": B's lower end on the seam column").c_str());
            }
            for(const long long y:{want.first,(want.first+want.last)/2,want.last}){
                const auto side=sides::On(*at.cover,false,false,static_cast<double>(y));
                Check(side.any && sides::MappingSees(*at.map,side.last-probe,static_cast<double>(y),false) && !sides::MappingSees(*at.map,side.last+probe,static_cast<double>(y),false),
                    (name+": B's right side on a row").c_str());
            }
        }
        // In the scene of the vertices, the end is decided by one vertex alone: without it the working gives another raster (a vertex that
        // the code dropped would be seen).
        if(std::string(scene.bottom_by)=="seam vertices"){
            Check(want.bottom_vertex.has_value(),(name+": a vertex decides the bottom").c_str());
            if(want.bottom_vertex){
                auto fewer=vertices;fewer.erase(fewer.begin()+static_cast<std::ptrdiff_t>(*want.bottom_vertex));
                const auto without=sides::RasterBySides(active.condition,parent,scene.pose,scene.seam,fewer,pose_vertices);
                std::cerr<<name<<": without vertex "<<*want.bottom_vertex<<" "<<sides::Text(without.raster)<<'\n';
                Check(!sides::SameRegion(without.raster,want.raster),(name+": the vertex that decides the bottom is the only one").c_str());
            }
        }
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
}

// ---- Frozen results of the scenes that move B along the baseline only ----------------------------------------------------------
// What a recalibration of a pose that moves B along the sheet's x makes: the raster, the seam column, B's matrix (the bits of the nine
// numbers), and the hashes of the candidate revision and of its profile. The strings below were taken from the code as it stood before
// the vertical placement of the raster and the separate reasons of a refusal were added; any later change that moves one of them is a
// change of what a left-right recalibration makes. A different compiler or math library can move the last bit of a matrix and with it
// the hashes: a mismatch is then to be looked at, not simply replaced.
std::string BitsOf(double value){return std::format("{:016x}",std::bit_cast<std::uint64_t>(value));}
std::string FrozenOf(const rc::Candidate& candidate){
    const auto& r=candidate.condition.raster;
    std::string text="raster_um="+std::to_string(r.region_um.left)+","+std::to_string(r.region_um.top)+","+std::to_string(r.region_um.right)+","+std::to_string(r.region_um.bottom)+
        " px="+std::to_string(r.width_pixels)+"x"+std::to_string(r.height_pixels)+" dpi="+std::to_string(r.dpi)+" seam="+std::to_string(candidate.condition.seam.column)+" b=";
    const auto& h=candidate.binding.parameters.camera_b.document_to_image;
    for(std::size_t i=0;i<h.size();++i)text+=(i?",":"")+BitsOf(h[i]);
    return text+" revision="+HashOf(candidate.text)+" profile="+HashOf(candidate.profile_bytes);
}

void FrozenLeftRight(){
    struct Scene {const char* name;bool turned;double shift;const char* frozen;};
    const Scene scenes[]{
        {"normal-5",false,5,"raster_um=560000,100000,745000,260000 px=1850x1600 dpi=254 seam=885 b=4024000000000000,0000000000000000,c0b64e0000000000,0000000000000000,4024000000000000,4059000000000000,0000000000000000,0000000000000000,3ff0000000000000 revision=c362750b13475bd27d8bf95c41f2c7f07f40720c8ff5cdfa50a1531076466326 profile=044e75dc2a3f7683461c614bfc21d25fb3ff98194c5730a46015577c5ab3bd9c"},
        {"normal-30",false,30,"raster_um=560000,100000,770000,260000 px=2100x1600 dpi=254 seam=1010 b=4024000000000000,0000000000000000,c0b7480000000000,0000000000000000,4024000000000000,4059000000000000,0000000000000000,0000000000000000,3ff0000000000000 revision=1ec1147cbd6aa49e7d2e28745bced2e47aeaab64d29d3e4038c9008d2c15951c profile=d3338e5ba9fbdcee07c4e5fea4b36c4eb6175d5c78f045db00fc4637edfbf790"},
        {"normal-60",false,60,"raster_um=560000,100000,800000,260000 px=2400x1600 dpi=254 seam=1160 b=4024000000000000,0000000000000000,c0b8740000000000,0000000000000000,4024000000000000,4059000000000000,0000000000000000,0000000000000000,3ff0000000000000 revision=e52a73ec03fd58d3a7724d47577020d1060c282255dda77b715ed98c8a884aa5 profile=4d2631cca824431b7f95b8026f3356bf3919446596c29f897c691d922179f02e"},
        {"normal-minus-30",false,-30,"raster_um=560000,100000,710000,260000 px=1500x1600 dpi=254 seam=750 b=4024000000000000,0000000000000000,c0b4f00000000000,0000000000000000,4024000000000000,4059000000000000,0000000000000000,0000000000000000,3ff0000000000000 revision=68507a534286fb7444f1d3d56e0e7c933f4d334f900c3211c813967e474d183b profile=1e0ee1f6070850574e85529ef8299f1f82667b9d61fe9977ab25f9df350a0c65"},
        {"half-turn-5",true,5,"raster_um=560000,100000,745000,260000 px=1850x1600 dpi=254 seam=884 b=c024000000000000,0000000000000000,40c9868000000000,0000000000000000,c024000000000000,40b2cb0000000000,0000000000000000,0000000000000000,3ff0000000000000 revision=836106830bbc29aaf4e97f7cce2c53dd863405d460bd7931d48d64d96545c117 profile=98d5f46a97795b241873c3bc38ae37d93e2554677e04c9b73ed5c3d8eee26563"},
        {"half-turn-30",true,30,"raster_um=560000,100000,770000,260000 px=2100x1600 dpi=254 seam=1009 b=c024000000000000,0000000000000000,40ca038000000000,0000000000000000,c024000000000000,40b2cb0000000000,0000000000000000,0000000000000000,3ff0000000000000 revision=7b9b21d5c211dae3559e78ffe35999c88b3ff804ef25042eec10d26e37d7349b profile=0a4980a352483d8127e2d5a53f9a4eedec5cd4390072c42915005def52c7e884"},
        {"half-turn-60",true,60,"raster_um=560000,100000,800000,260000 px=2400x1600 dpi=254 seam=1159 b=c024000000000000,0000000000000000,40ca998000000000,0000000000000000,c024000000000000,40b2cb0000000000,0000000000000000,0000000000000000,3ff0000000000000 revision=faadee3eab906ef8a4ebaaabfc2127037af99bcbb4aa07762d10f3845cbd987d profile=5de6f03e766e6c91dd510ed05166cf00396e1694b39d75ceafdd84c27c92b928"},
    };
    const render::OutputRaster raster=ps::Raster(560000,100000,740000,260000,254);
    const rc::ProfileFit fit{0.1,0.3,50};
    for(const auto& scene:scenes){
        const auto profile=RigText(scene.turned);
        const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        auto active=MakeActive(std::string("frozen-")+scene.name,profile,scene.turned?MountOrientation::rotated_180:MountOrientation::normal,raster,TestRanges);
        const auto pose=PoseForShift(parameters.camera_b,scene.shift,scene.turned);
        const auto proposal=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),pose,fit,"{\"schema\":\"test-record\"}");
        CHECK(proposal.refusals.empty() && proposal.candidate);
        if(proposal.candidate){
            const auto actual=FrozenOf(*proposal.candidate);
            if(actual!=scene.frozen)std::cerr<<"FROZEN "<<scene.name<<" "<<actual<<'\n';
            CHECK(actual==scene.frozen);
        }
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
    // Too far apart along the baseline (only 30 mm of the 160 are left): the creation check refuses, with its own sentence, and no other
    // reason comes with it.
    {
        const auto profile=RigText(false);
        const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        auto active=MakeActive("frozen-apart",profile,MountOrientation::normal,raster,TestRanges);
        const auto proposal=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),PoseForShift(parameters.camera_b,130.0,false),fit,"{}");
        CHECK(!proposal.candidate && proposal.refusals.size()==1 && proposal.refusals.front()==rc::Refusal::creation_check_failed && proposal.refusal_reason.empty());
        CHECK(proposal.detail.rfind("fixed composite creation v3: row 0: the seam and half the blend width inside B's cover (left) does not hold over the range",0)==0);
        CHECK(!proposal.display.target_cover_shortfall.present && proposal.display.has_new);   // the plan was whole; the check refused it
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
}

// ---- Up and down: the rows that the target camera does not see ------------------------------------------------------------------
// The pose of B for a camera moved by `move_mm` along the sheet's y (positive is down on the drawing), found on the model.
cr::PoseParameters PoseForVertical(const render::CameraProjection& camera_b,double move_mm,bool turned){
    const cr::PoseModel model(camera_b);
    const render::Point x0{934,236};
    const auto target=Project(MatrixOf(camera_b),{x0.x,x0.y-move_mm});
    const auto at=[&](double ty){return Project(model.CameraHomography(cr::PoseParameters{0,0,0,0,ty,0}),x0).y-target.y;};
    double a=0,b=turned?-1:1;
    for(int i=0;i<6;++i){
        const double fa=at(a),fb=at(b);
        if(fa==fb)break;
        const double c=b-fb*(b-a)/(fb-fa);a=b;b=c;
    }
    return cr::PoseParameters{0,0,0,0,b,0};
}
std::string ReasonOf(const rc::Proposal& p,rc::Refusal r){return rc::RefusalReasonOf(p,r);}
bool HasRefusal(const rc::Proposal& p,rc::Refusal r){return std::find(p.refusals.begin(),p.refusals.end(),r)!=p.refusals.end();}

// The synthetic rig sees 10 px per mm and its sensors end at 481.2 mm of the sheet's y (4912 px, the first at 10 mm); the pixel pitch of the
// 254 DPI rasters below is 0.1 mm. The rows that B does not see below a raster that ends at `raster_bottom_mm` are what lies beyond B's
// edge once B has moved by `move_mm`: the expectation is this arithmetic, not a run of the code under test.
constexpr double kRigBottomMm=(4912.0-100.0)/10.0,kRigTopMm=-10.0,kPitchMm=0.1;
// The sensor of the synthetic rig in pixels (the D810's): the live view's scale against the stored frame is the view's size over these.
constexpr double kSensorWidthPx=7360.0,kSensorHeightPx=4912.0;
// The rig with B moved to the left so that B's right side is on the sheet, that right side (the normal mounting's; the half turn's is
// 0.1 mm short of it), and how far the creation check's range reaches beyond the
// last column of a raster near it: the smallest margin at which the parent passes the check, found once by running it (a value of this test).
constexpr double kRigMovedLeftMm=152.0,kRigRightMm=1302.0-kRigMovedLeftMm,kRightSideReachMm=2.25;
// How far apart the columns on which an end of B is measured can be, in mm of the drawing's y, at a turn of 1 degree.
constexpr double kColumnSlackMm=0.05;
// The answers around gate 1, up by 4 to 8 mm in steps of 0.5 mm, the same in both mountings (run once and written down; see VerticalFromPose).
constexpr const char* kGateOne="full,full,full,refused,refused,cut 65,cut 70,cut 75,cut 80";
// The smallest move to the right, in whole mm, at which the turn of the range loses the seam column at the top and the bottom rows of the
// tall raster below (run once and written down; see VerticalFromPose).
constexpr double kSpreadShiftMm=139.0;
// The rows at the bottom of the kept raster that B does not see at the wide search's pose of the pair moved up by 12 mm, in each mounting
// (run once and written down; see VerticalPair).
constexpr std::uint32_t kKeptRowsNormal=8,kKeptRowsHalfTurn=9;

// Where B's sensor ends on the drawing for a document-to-image matrix of the synthetic rig (a plane without a lens), worked out from the
// matrix and the sensor's size alone (not by the search of the code under test): the lowest y of the drawing at x whose image is inside the
// sensor's height, and the rightmost x at y whose image is inside its width. Either side of the sensor can be the one on the drawing's
// bottom (right) depending on the mounting: the larger of the two is.
double LowestY(const render::Matrix3& h,double x){
    const auto at=[&](double r){return (r*(h[6]*x+h[8])-h[3]*x-h[5])/(h[4]-r*h[7]);};
    return std::max(at(0.0),at(static_cast<double>(ps::kHeight)));
}
double RightmostX(const render::Matrix3& h,double y){
    const auto at=[&](double r){return (r*(h[7]*y+h[8])-h[1]*y-h[2])/(h[0]-r*h[6]);};
    return std::max(at(0.0),at(static_cast<double>(ps::kWidth)));
}
// How far B's lower (upper) end on the drawing's column x moved up (down) between two placements, in mm: what may be given up at the bottom
// (top) of the raster, the owner's decision 1.
double LowerEndMovedUpMm(const render::CameraProjection& camera_b,const cr::PoseParameters& pose,double x){
    const cr::PoseModel model(camera_b);
    return LowestY(model.CameraHomography(cr::PoseParameters{}),x)-LowestY(model.CameraHomography(pose),x);
}
// The new width of a raster whose kept rows span the drawing's y from top_mm to bottom_mm (the row centres at the ends): the right edge moves
// by how far the smallest of B's right side over those rows moved, in whole pixels of 0.1 mm.
long long WidthFollowingRightSide(const render::CameraProjection& camera_b,const cr::PoseParameters& pose,long long width,double top_mm,double bottom_mm){
    const cr::PoseModel model(camera_b);
    const auto before=model.CameraHomography(cr::PoseParameters{}),after=model.CameraHomography(pose);
    const double y0=top_mm+kPitchMm/2,y1=bottom_mm-kPitchMm/2;
    const double moved=std::min(RightmostX(after,y0),RightmostX(after,y1))-std::min(RightmostX(before,y0),RightmostX(before,y1));
    return width+std::llround(moved/kPitchMm);
}
bool SameRaster(const render::OutputRaster& a,const render::OutputRaster& b){
    return a.region_um.left==b.region_um.left && a.region_um.top==b.region_um.top && a.region_um.right==b.region_um.right && a.region_um.bottom==b.region_um.bottom &&
        a.dpi==b.dpi && a.width_pixels==b.width_pixels && a.height_pixels==b.height_pixels;
}
// The seam column of a raster on the drawing (mm), from its left edge and its pixel pitch.
double SeamXMm(const FixedCompositeConditionV3& c){return static_cast<double>(c.raster.region_um.left)/1000.0+(static_cast<double>(c.seam.column)+0.5)*(25.4/c.raster.dpi);}

void VerticalFromPose(){
    const rc::ProfileFit fit{0.1,0.3,50};
    for(const bool turned:{false,true}){
        const auto profile=RigText(turned);
        const auto orientation=turned?MountOrientation::rotated_180:MountOrientation::normal;
        // A raster near the lower edge of what the cameras see: it passes the creation check at the profile's own placement.
        const render::OutputRaster raster=ps::Raster(560000,300000,740000,470000,254);
        const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        auto active=MakeActive("vertical",profile,orientation,raster,TestRanges);
        const auto parent=BindFixedCompositeProfileV3(active.condition,profile,kHash);
        CHECK(rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),cr::PoseParameters{},fit,"{}").candidate.has_value());
        const auto from=[&](double move){return rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),PoseForVertical(parameters.camera_b,move,turned),fit,"{}");};

        // Up by a few mm, and by more: the rows at the bottom that B no longer sees are given up, by what B moved and no more (the owner's
        // decision 1). The parent's raster ends 11.2 mm above B's lower edge, less than the vertices of the check's range reach (about 16.5 mm),
        // so what B's move allows is what decides: the bottom edge moves up by the move, to within one row and never by more, and the new
        // raster keeps the parent's 11.2 mm below it. The top edge, the left, the width and the DPI stay, and the new raster is inside the old
        // one. The candidate's condition and its profile name the same raster.
        for(const double move:{-8.0,-12.0,-16.0,-24.0,-40.0}){
            const auto p=from(move);
            CHECK(p.candidate && p.refusals.empty() && !p.ready && p.display.has_new && !p.display.target_cover_shortfall.present);
            if(!p.candidate)continue;
            const auto& r=p.candidate->condition.raster;
            CHECK(r.region_um.top==raster.region_um.top && r.region_um.left==raster.region_um.left && r.region_um.right==raster.region_um.right && r.dpi==raster.dpi &&
                r.width_pixels==raster.width_pixels);
            CHECK(r.region_um.bottom<raster.region_um.bottom && r.height_pixels<raster.height_pixels && r.region_um.bottom>r.region_um.top);
            CHECK(!Throws([&]{render::ValidateOutputRaster(r);}));
            CHECK(SameRaster(r,p.candidate->binding.parameters.raster));
            const double bottom_mm=static_cast<double>(r.region_um.bottom)/1000.0;
            const double given_up=470.0-bottom_mm;
            CHECK(given_up<=-move+1e-9 && given_up>=-move-kPitchMm-1e-9);
            // What the screen and the report say: the height before and after, the bottom edge before and after, the top edge unchanged.
            const auto& d=p.display;
            CHECK(d.height_previous_px==raster.height_pixels && d.height_new_px==r.height_pixels && d.top_new_mm==d.top_previous_mm && d.bottom_new_mm<d.bottom_previous_mm);
            CheckNear(d.bottom_new_mm,bottom_mm,1e-9,"the bottom edge on the screen");
            CheckNear(d.motion.camera_motion_mm.y,move,0.2,"the move up is shown");
        }
        // Around gate 1: up by 0.5 mm at a time. While the check's own named conditions hold on the raster as it is, nothing is given up and the
        // check decides; past that, the rows are given up by the move. The sequence of answers is pinned (`full` = a candidate on the whole
        // raster, `refused` = creation-check-failed on the whole raster, `cut N` = a candidate N rows shorter).
        {
            std::string answers;
            for(int step=8;step<=16;++step){
                const double move=-0.5*step;
                const auto p=from(move);
                std::string answer;
                if(p.candidate && p.refusals.empty()){
                    const auto rows=static_cast<long long>(raster.height_pixels)-static_cast<long long>(p.candidate->condition.raster.height_pixels);
                    answer=rows==0?"full":"cut "+std::to_string(rows);
                    if(rows>0)CHECK(static_cast<double>(rows)*kPitchMm<=-move+1e-9 && static_cast<double>(rows)*kPitchMm>=-move-kPitchMm-1e-9);
                }else if(p.refusals.size()==1 && p.refusals.front()==rc::Refusal::creation_check_failed && p.display.has_new && p.display.height_new_px==raster.height_pixels)answer="refused";
                else answer="other";
                answers+=(answers.empty()?"":",")+answer;
            }
            const std::string expected=kGateOne;
            if(answers!=expected)std::cerr<<"GATE1 "<<(turned?"half-turn":"normal")<<" "<<answers<<'\n';
            Check(answers==expected,"the answers around gate 1");
        }
        // Moved to the left as well as up (the overlap grows): the rows are given up by the move up and the right edge follows the move to
        // the left. The seam column of the new raster lies left of the parent's, so nothing about the parent's column holds this back.
        {
            auto pose=PoseForShift(parameters.camera_b,-30.0,turned);pose.ty_mm=PoseForVertical(parameters.camera_b,-12.0,turned).ty_mm;
            const auto p=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),pose,fit,"{}");
            CHECK(p.candidate && p.refusals.empty());
            if(p.candidate){
                const auto& r=p.candidate->condition.raster;
                const double given_up=470.0-static_cast<double>(r.region_um.bottom)/1000.0;
                CHECK(r.region_um.top==raster.region_um.top && r.region_um.left==raster.region_um.left && given_up<=12.0+1e-9 && given_up>=12.0-kPitchMm-1e-9);
                CHECK(std::llabs(static_cast<long long>(r.width_pixels)-(static_cast<long long>(raster.width_pixels)-300))<=1);
                CHECK(SameRaster(r,p.candidate->binding.parameters.raster));
                CHECK(SeamXMm(p.candidate->condition)<SeamXMm(active.condition));
            }
        }
        // The rows are given up for B's move up or down and not for a position that is out of range: the seam that cannot be held at the edge
        // of the range is still refused by the check, with the check's own sentence, whether or not B also moved up.
        {
            auto pose=PoseForShift(parameters.camera_b,130.0,turned);pose.ty_mm=PoseForVertical(parameters.camera_b,-8.0,turned).ty_mm;
            const auto p=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),pose,fit,"{}");
            CHECK(!p.candidate && p.refusals.size()==1 && p.refusals.front()==rc::Refusal::creation_check_failed && !p.display.target_cover_shortfall.present);
            // Moved 100 mm apart and 8 mm up: the rows are given up by the 8 mm and no more, so the new raster keeps the parent's 11.2 mm below it
            // on the seam column. The seam is now 50 mm further from B's centre, where the turn and the height of the range move B's lower edge
            // further than at the parent's seam: the check refuses near the bottom, and the height is not traded for it.
            auto closer=PoseForShift(parameters.camera_b,100.0,turned);closer.ty_mm=PoseForVertical(parameters.camera_b,-8.0,turned).ty_mm;
            const auto q=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),closer,fit,"{}");
            if(q.candidate || q.refusals.size()!=1)std::cerr<<"apart and up: "<<q.detail<<'\n';
            CHECK(!q.candidate && q.refusals.size()==1 && q.refusals.front()==rc::Refusal::creation_check_failed && !q.display.target_cover_shortfall.present);
            CHECK(Contains(q.detail,"the seam and half the blend width inside B's cover (left) does not hold over the range"));
            CHECK(q.display.has_new && q.display.top_new_mm==q.display.top_previous_mm);
            const double given_up=470.0-q.display.bottom_new_mm;
            CHECK(given_up<=8.0+1e-9 && given_up>=8.0-kPitchMm-1e-9 && std::llabs(static_cast<long long>(q.display.width_new_px)-static_cast<long long>(raster.width_pixels)-1000)<=1);
        }
        // A turn of B by 1 degree and a move up, in both mountings the same answer: a candidate, its width what B's right side over the rows
        // that are kept gives (worked out from the matrices; B's turned lower side, which crosses those rows far right of the raster, does not
        // decide it, and it used to leave no width), its bottom edge moved up by how far B's lower end moved on the seam column (the move and
        // the turn's part of it), no more. Up by 8 mm the turn brings B's lower end on the seam column back down to 3 mm above where it was,
        // the check's named conditions hold and nothing is given up; up by 12 mm (7 mm on the seam column) the rows are given up by that.
        const auto turned_up=[&](const Active& on,const render::CameraProjection& camera_b,const render::OutputRaster& grid,double move,bool cut,const char* what){
            auto pose=PoseForVertical(camera_b,move,turned);pose.rz_deg=1.0;
            const auto p=rc::ProposeFromPose(MakeInputs(on,nullptr,nullptr),pose,fit,"{}");
            Check(p.candidate.has_value() && p.refusals.empty(),what);
            if(!p.candidate){std::cerr<<what<<": "<<p.detail<<'\n';return;}
            const auto& r=p.candidate->condition.raster;
            const double grid_bottom=static_cast<double>(grid.region_um.bottom)/1000.0,bottom_mm=static_cast<double>(r.region_um.bottom)/1000.0;
            const double allowed=LowerEndMovedUpMm(camera_b,pose,SeamXMm(on.condition));
            const double given_up=grid_bottom-bottom_mm;
            const auto width=WidthFollowingRightSide(camera_b,pose,grid.width_pixels,static_cast<double>(grid.region_um.top)/1000.0,bottom_mm);
            // The code measures the new end on the leftmost column the new seam can take, a few mm from the parent's: at a turn of 1 degree
            // that is a few hundredths of a mm, inside kColumnSlackMm.
            std::cerr<<what<<": given up "<<given_up<<" mm (allowed "<<allowed<<"), width "<<r.width_pixels<<" (expected "<<width<<")\n";
            Check(r.region_um.top==grid.region_um.top && r.region_um.left==grid.region_um.left && r.dpi==grid.dpi,what);
            if(cut)Check(given_up<=allowed+kColumnSlackMm && given_up>=allowed-kPitchMm-kColumnSlackMm,what);
            else Check(given_up==0 && allowed>0,what);
            Check(std::llabs(static_cast<long long>(r.width_pixels)-width)<=1,what);
            Check(SameRaster(r,p.candidate->binding.parameters.raster),what);
        };
        turned_up(active,parameters.camera_b,raster,-8.0,false,turned?"a turn and a move up by 8 mm, half turn":"a turn and a move up by 8 mm, normal");
        turned_up(active,parameters.camera_b,raster,-12.0,true,turned?"a turn and a move up by 12 mm, half turn":"a turn and a move up by 12 mm, normal");
        // The same with the parent's right edge near B's right side, so that B's right side is what the right edge follows: B is moved to
        // the left on the drawing (its right side has to be on the sheet), and the raster ends the check's reach and 1 mm short of it.
        {
            const auto moved_profile=RigTextMovedLeft(turned,kRigMovedLeftMm);
            const render::OutputRaster near_right=ps::Raster(560000,300000,static_cast<long long>(std::llround((kRigRightMm-kRightSideReachMm-1.0)*1000.0)),470000,254);
            const auto moved_parameters=RigProfileV2::Parse(moved_profile).ValidateCalibrationForEvaluation(near_right,render::Resampling::bilinear);
            auto close=MakeActive("vertical-near-right",moved_profile,orientation,near_right,TestRanges);
            CHECK(rc::ProposeFromPose(MakeInputs(close,nullptr,nullptr),cr::PoseParameters{},fit,"{}").candidate.has_value());   // the parent passes the check
            const char* what=turned?"a turn and a move up near B's right side, half turn":"a turn and a move up near B's right side, normal";
            auto pose=PoseForVertical(moved_parameters.camera_b,-12.0,turned);pose.rz_deg=1.0;   // past gate 1 on this raster
            const auto close_parent=BindFixedCompositeProfileV3(close.condition,moved_profile,kHash);
            const auto r=rc::ProposeRaster(close.condition,close_parent,pose,rc::RasterPlacement::follow_camera_b,rc::SeamPlacement::band_center);
            const double bottom_mm=static_cast<double>(r.region_um.bottom)/1000.0,given_up=470.0-bottom_mm;
            const double allowed=LowerEndMovedUpMm(moved_parameters.camera_b,pose,SeamXMm(close.condition));
            const auto width=WidthFollowingRightSide(moved_parameters.camera_b,pose,near_right.width_pixels,300.0,bottom_mm);
            std::cerr<<what<<": given up "<<given_up<<" mm (allowed "<<allowed<<"), width "<<near_right.width_pixels<<" -> "<<r.width_pixels<<" (expected "<<width<<")\n";
            // On this rig B's centre is nearer the seam and the vertices of the range reach less far there: the rows they hold (95 rows, 9.5 mm)
            // are fewer than B's move allows (9.66 mm), so they decide. Never more than the move.
            Check(r.region_um.top==near_right.region_um.top && given_up<=allowed+kColumnSlackMm && std::abs(given_up-9.5)<1e-9,what);
            Check(std::llabs(static_cast<long long>(r.width_pixels)-width)<=1,what);
            // The whole proposal: the check, which places the paper surface's origin on the new raster's middle row and holds the seam on the
            // candidate's own profile, finds a point of its range at which B's turned lower side ends B's cover right of the seam on a bottom
            // row. It refuses, and no more rows are given up for it (the vertices and the check differ by this; the check decides last).
            const auto p=rc::ProposeFromPose(MakeInputs(close,nullptr,nullptr),pose,fit,"{}");
            if(p.candidate || p.refusals.size()!=1)std::cerr<<what<<": "<<p.detail<<'\n';
            Check(!p.candidate && p.refusals.size()==1 && p.refusals.front()==rc::Refusal::creation_check_failed && p.display.has_new &&
                p.display.height_new_px==r.height_pixels && p.display.width_new_px==r.width_pixels,what);
            // Moved to the left by 30 mm and up by 40 mm, no turn: the right edge follows B's right side to the left (300 px), and the rows are
            // given up by the move up (40 mm on the seam column) to within one row and no more. The last column of the new raster, which A does
            // not see, is inside B's right side on the rows that are kept (worked out from B's matrix).
            {
                const char* named=turned?"left and far up near B's right side, half turn":"left and far up near B's right side, normal";
                auto left_up=PoseForShift(moved_parameters.camera_b,-30.0,turned);left_up.ty_mm=PoseForVertical(moved_parameters.camera_b,-40.0,turned).ty_mm;
                const auto q=rc::ProposeFromPose(MakeInputs(close,nullptr,nullptr),left_up,fit,"{}");
                if(!q.candidate || !q.refusals.empty())std::cerr<<named<<": "<<q.detail<<'\n';
                Check(q.candidate.has_value() && q.refusals.empty(),named);
                if(q.candidate){
                    const auto& got=q.candidate->condition.raster;
                    const double got_bottom=static_cast<double>(got.region_um.bottom)/1000.0,got_given_up=470.0-got_bottom;
                    const double got_allowed=LowerEndMovedUpMm(moved_parameters.camera_b,left_up,SeamXMm(close.condition));
                    const auto got_width=WidthFollowingRightSide(moved_parameters.camera_b,left_up,near_right.width_pixels,300.0,got_bottom);
                    std::cerr<<named<<": given up "<<got_given_up<<" mm (allowed "<<got_allowed<<"), width "<<near_right.width_pixels<<" -> "<<got.width_pixels<<" (expected "<<got_width<<")\n";
                    Check(got.region_um.top==near_right.region_um.top && got.region_um.left==near_right.region_um.left && got.dpi==near_right.dpi,named);
                    Check(got_given_up<=got_allowed+kColumnSlackMm && got_given_up>=got_allowed-kPitchMm-kColumnSlackMm && std::abs(got_allowed-40.0)<0.2,named);
                    Check(std::llabs(static_cast<long long>(got.width_pixels)-got_width)<=1 &&
                        std::llabs(static_cast<long long>(got.width_pixels)-(static_cast<long long>(near_right.width_pixels)-300))<=1,named);
                    const auto b_after=cr::PoseModel(moved_parameters.camera_b).CameraHomography(left_up);
                    const double last_column_mm=static_cast<double>(got.region_um.left)/1000.0+(static_cast<double>(got.width_pixels)-0.5)*kPitchMm;
                    Check(last_column_mm<RightmostX(b_after,300.0+kPitchMm/2) && last_column_mm<RightmostX(b_after,got_bottom-kPitchMm/2),named);
                    Check(SameRaster(got,q.candidate->binding.parameters.raster),named);
                }
            }
            std::error_code ignored_close;fs::remove_all(close.settings,ignored_close);
        }
        // Far away: no row at all.
        {
            const auto p=from(-200.0);
            const auto& s=p.display.target_cover_shortfall;
            CHECK(!p.candidate && p.refusals.size()==1 && p.refusals.front()==rc::Refusal::band_uncovered && ReasonOf(p,rc::Refusal::band_uncovered)=="all-rows");
            CHECK(s.present && s.all_rows && s.rows_top==raster.height_pixels && s.rows_bottom==raster.height_pixels);
            CheckNear(s.top_mm,static_cast<double>(raster.height_pixels)*kPitchMm,1e-9,"all rows in mm");
        }
        // Down is away from the lower edge: nothing is missing there.
        CHECK(from(8.0).candidate.has_value() && from(40.0).candidate.has_value());
        // `keep` does not change the raster, so the rows are missing in the same way.
        {
            auto in=MakeInputs(active,nullptr,nullptr);in.raster=rc::RasterPlacement::keep;
            const auto p=rc::ProposeFromPose(in,PoseForVertical(parameters.camera_b,-16.0,turned),fit,"{}");
            CHECK(!p.candidate && p.refusals.size()==1 && p.refusals.front()==rc::Refusal::band_uncovered && ReasonOf(p,rc::Refusal::band_uncovered)=="bottom" &&
                p.display.target_cover_shortfall.present && p.display.target_cover_shortfall.at_estimate);
        }
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
    const auto profile=RigText(false);
    // The top edge: a raster whose first rows B stops seeing when it moves down. Both mountings: the bound on what is given up holds at the
    // top as it does at the bottom.
    for(const bool turned:{false,true}){
        const auto top_profile=RigText(turned);
        const render::OutputRaster raster=ps::Raster(560000,5000,740000,165000,254);
        const auto parameters=RigProfileV2::Parse(top_profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        auto active=MakeActive("vertical-top",top_profile,turned?MountOrientation::rotated_180:MountOrientation::normal,raster,TestRanges);
        CHECK(rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),cr::PoseParameters{},fit,"{}").candidate.has_value());
        const auto p=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),PoseForVertical(parameters.camera_b,30.0,turned),fit,"{}");
        if(!p.candidate)std::cerr<<"top edge "<<(turned?"half turn":"normal")<<": "<<p.detail<<'\n';
        CHECK(p.candidate && p.refusals.empty() && !p.display.target_cover_shortfall.present);
        if(p.candidate){
            // The top edge moves down past the rows above B's upper edge and the reach of the range; the bottom edge stays.
            const auto& r=p.candidate->condition.raster;
            const double top_mm=static_cast<double>(r.region_um.top)/1000.0;
            CHECK(r.region_um.bottom==raster.region_um.bottom && r.region_um.left==raster.region_um.left && r.region_um.right==raster.region_um.right && r.width_pixels==raster.width_pixels);
            CHECK(r.region_um.top>raster.region_um.top && r.height_pixels<raster.height_pixels && !Throws([&]{render::ValidateOutputRaster(r);}));
            // B's upper edge moved down by 30 mm, past the parent's 15 mm above the raster: the top edge moves down by the move, to within one
            // row and never by more.
            const double given_up=top_mm-5.0;
            CHECK(given_up<=30.0+1e-9 && given_up>=30.0-kPitchMm-1e-9);
            CHECK(SameRaster(r,p.candidate->binding.parameters.raster));
            CHECK(p.display.top_new_mm>p.display.top_previous_mm && p.display.bottom_new_mm==p.display.bottom_previous_mm && p.display.height_new_px==r.height_pixels);
        }
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
    // Spread too far apart on a tall raster with a wide range of turns: at the top and the bottom rows the turn of the range moves B's left side
    // further than its position does, so the seam column is lost there first while the middle row is still held at every vertex. B did not
    // move up or down, so no row is given up (the height is not traded for the spread): the check refuses on the whole raster, naming the turn.
    {
        const render::OutputRaster tall=ps::Raster(560000,20000,740000,450000,254);
        const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(tall,render::Resampling::bilinear);
        const auto wide_turns=[](FixedCompositeConditionV3& c){TestRanges(c);c.registration.envelope.rotation_deg=3;};
        auto active=MakeActive("spread-tall",profile,MountOrientation::normal,tall,wide_turns);
        CHECK(rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),cr::PoseParameters{},fit,"{}").candidate.has_value());
        const auto p=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),PoseForShift(parameters.camera_b,kSpreadShiftMm,false),fit,"{}");
        if(p.candidate || p.refusals.size()!=1 || !Contains(p.detail,"the rotation is at the"))std::cerr<<"SPREAD "<<p.detail<<'\n';
        CHECK(!p.candidate && p.refusals.size()==1 && p.refusals.front()==rc::Refusal::creation_check_failed);
        CHECK(p.display.has_new && p.display.height_new_px==tall.height_pixels && p.display.top_new_mm==p.display.top_previous_mm && p.display.bottom_new_mm==p.display.bottom_previous_mm);
        CHECK(Contains(p.detail,"the seam column is not covered by both cameras when the rotation is at the"));
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
    // Both edges: a raster taller than what the cameras see (it never passed the creation check; a pose is all this needs), B moved down by
    // 12 mm. Kept as it is, B misses the rows at the top and A and B the rows at the bottom. Following B, the top edge moves down by B's move
    // (the 10 mm above the raster that B saw before are kept above it), and the rows at the bottom, which B's move did not cost, are not given
    // up: the band is refused there alone.
    {
        const render::OutputRaster raster=ps::Raster(560000,0,740000,500000,254);
        const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        auto active=MakeActive("vertical-both",profile,MountOrientation::normal,raster,TestRanges);
        const auto pose=PoseForVertical(parameters.camera_b,12.0,false);
        {
            auto in=MakeInputs(active,nullptr,nullptr);in.raster=rc::RasterPlacement::keep;
            const auto p=rc::ProposeFromPose(in,pose,fit,"{}");
            const auto& s=p.display.target_cover_shortfall;
            CHECK(!p.candidate && p.refusals.size()==1 && p.refusals.front()==rc::Refusal::band_uncovered && ReasonOf(p,rc::Refusal::band_uncovered)=="top-and-bottom");
            CHECK(s.present && !s.all_rows);
            CheckNear(static_cast<double>(s.rows_top),(kRigTopMm+12.0)/kPitchMm,1.5,"the rows above what B sees");
            CheckNear(static_cast<double>(s.rows_bottom),(500.0-kRigBottomMm)/kPitchMm,1.5,"the rows below what A and B see");
        }
        {
            const auto parent=BindFixedCompositeProfileV3(active.condition,profile,kHash);
            const auto r=rc::ProposeRaster(active.condition,parent,pose,rc::RasterPlacement::follow_camera_b,rc::SeamPlacement::band_center);
            const double given_up=static_cast<double>(r.region_um.top)/1000.0;
            CHECK(given_up<=12.0+1e-9 && given_up>=12.0-kPitchMm-1e-9 && r.region_um.bottom==raster.region_um.bottom);
            const auto p=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),pose,fit,"{}");
            const auto& s=p.display.target_cover_shortfall;
            CHECK(!p.candidate && p.refusals.size()==1 && p.refusals.front()==rc::Refusal::band_uncovered && ReasonOf(p,rc::Refusal::band_uncovered)=="bottom");
            CHECK(s.present && !s.all_rows && s.rows_top==0);
            CheckNear(static_cast<double>(s.rows_bottom),(500.0-kRigBottomMm)/kPitchMm,1.5,"the rows below what A and B see, following B");
        }
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
    // The edges are whole micrometres that are not outside the rows that are kept: the top is the ceiling and the bottom the floor of the rows'
    // extent (a DPI whose pixel is not a whole number of micrometres), and the height is the exact ceiling of what is left.
    {
        const render::OutputRaster raster=ps::Raster(560000,300000,740000,470000,180);
        const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        auto active=MakeActive("vertical-dpi",profile,MountOrientation::normal,raster,TestRanges);
        const auto parent=BindFixedCompositeProfileV3(active.condition,profile,kHash);
        const auto bottom_cut=rc::ProposeRaster(active.condition,parent,PoseForVertical(parameters.camera_b,-16.0,false),rc::RasterPlacement::follow_camera_b,rc::SeamPlacement::band_center);
        const auto top_cut=[&]{
            const render::OutputRaster tall=ps::Raster(560000,5000,740000,165000,180);
            auto upper=MakeActive("vertical-dpi-top",profile,MountOrientation::normal,tall,TestRanges);
            const auto upper_parent=BindFixedCompositeProfileV3(upper.condition,profile,kHash);
            const auto cut=rc::ProposeRaster(upper.condition,upper_parent,PoseForVertical(parameters.camera_b,30.0,false),rc::RasterPlacement::follow_camera_b,rc::SeamPlacement::band_center);
            std::error_code ignored_upper;fs::remove_all(upper.settings,ignored_upper);
            return cut;
        }();
        CHECK(bottom_cut.region_um.top==raster.region_um.top && bottom_cut.region_um.bottom<raster.region_um.bottom && !Throws([&]{render::ValidateOutputRaster(bottom_cut);}));
        {
            // The bottom is the floor of j rows, j being the new height.
            const std::int64_t extent=bottom_cut.region_um.bottom-bottom_cut.region_um.top;
            CHECK(extent==static_cast<std::int64_t>(bottom_cut.height_pixels)*25400/180);
        }
        CHECK(top_cut.region_um.top>5000 && top_cut.region_um.bottom==165000 && !Throws([&]{render::ValidateOutputRaster(top_cut);}));
        {
            // The top is the ceiling of k rows for the k that is just below it: k = floor(shift / pitch), shift = ceil(k pitch).
            const std::int64_t shift=top_cut.region_um.top-5000;
            const std::int64_t k=shift*180/25400;
            CHECK(k>0 && (k*25400+179)/180==shift);
        }
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
    // The width that follows B's cover is gone: a different refusal, with no rows to count.
    {
        const render::OutputRaster raster=ps::Raster(560000,100000,740000,260000,254);
        const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        auto active=MakeActive("collapsed",profile,MountOrientation::normal,raster,TestRanges);
        const auto p=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),PoseForShift(parameters.camera_b,-190.0,false),fit,"{}");
        CHECK(!p.candidate && !p.ready && p.refusals.size()==1 && p.refusals.front()==rc::Refusal::range_collapsed && ReasonOf(p,rc::Refusal::range_collapsed)=="no-width");
        CHECK(!p.display.target_cover_shortfall.present && !p.display.has_new && p.display.has_motion);
        CHECK(!HasRefusal(p,rc::Refusal::no_overlap) && !HasRefusal(p,rc::Refusal::band_uncovered));
        // The first reason given for a refusal stays, and a refusal without one has none.
        CHECK(ReasonOf(p,rc::Refusal::no_overlap).empty() && ReasonOf(p,rc::Refusal::creation_check_failed).empty());
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
    // The codes of the two new refusals, and that the old one is not one of them.
    CHECK(rc::RefusalCode(rc::Refusal::band_uncovered)=="band-uncovered" && rc::RefusalCode(rc::Refusal::range_collapsed)=="range-collapsed" && rc::RefusalCode(rc::Refusal::no_overlap)=="no-overlap");
}

// ---- The live view and the JPEG on a recalibration's own candidate ---------------------------------------------------------------
// The candidate of a pose moved up (its raster gave up rows at the bottom), read the way a sample composite reads it (SelectionOfCandidate),
// through the live C and through the JPEG's maker. At the capture's scale C's mapping of camera B is the maker's, bit for bit; at the live
// view's scale every point of C's grid is the JPEG's output at the same point of the drawing. The drawing's point of a grid pixel is worked
// out here from the candidate's raster (its edges and DPI) and the view's scale, not from the grid that the code under test made.
void CandidateLiveAndJpeg(){
    const rc::ProfileFit fit{0.1,0.3,50};
    // 1800 x 1100 samples: within the live grid's bound at the capture's scale (px::kFixedPreviewMaximumOutputPixels).
    const render::OutputRaster raster=ps::Raster(560000,360000,740000,470000,254);
    const std::vector<RegisteredMapping> mappings{
        RegisteredMapping{cr::PoseParameters{},{},std::nullopt},
        RegisteredMapping{cr::PoseParameters{0.15,-0.1,0.3,2.0,-1.5,0.4},{},std::nullopt},
        RegisteredMapping{cr::PoseParameters{},SurfacePlane{6.0,0.004,-0.002},std::nullopt},
        RegisteredMapping{cr::PoseParameters{0.15,-0.1,0.3,2.0,-1.5,0.4},SurfacePlane{9.0,0.005,-0.003},std::nullopt}};
    const std::vector<std::pair<unsigned,unsigned>> views{{640,427},{400,267},{640,400}};
    for(const bool turned:{false,true}){
        const char* what=turned?"the live view and the JPEG on a candidate, half turn":"the live view and the JPEG on a candidate, normal";
        const auto profile=RigText(turned);
        const auto orientation=turned?MountOrientation::rotated_180:MountOrientation::normal;
        const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        auto active=MakeActive("live-jpeg",profile,orientation,raster,TestRanges);
        const auto p=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),PoseForVertical(parameters.camera_b,-12.0,turned),fit,"{}");
        if(!p.candidate)std::cerr<<what<<": "<<p.detail<<'\n';
        Check(p.candidate.has_value() && p.refusals.empty(),what);
        const auto selection=p.candidate?rc::SelectionOfCandidate(*p.candidate):FixedCompositeSelection{};
        const auto* v3=selection.loaded?std::get_if<LoadedFixedCompositeConditionV3>(&*selection.loaded):nullptr;
        Check(v3!=nullptr,what);
        if(!v3){std::error_code ignored;fs::remove_all(active.settings,ignored);continue;}
        const auto& r=v3->condition.raster;
        const auto& rendered=v3->profile.parameters;
        const auto seam=v3->condition.seam.column;
        // The candidate gave up rows at the bottom only; its edges are whole rows of 0.1 mm.
        Check(r.region_um.top==raster.region_um.top && r.region_um.bottom<raster.region_um.bottom && r.height_pixels<raster.height_pixels &&
            (r.region_um.bottom-r.region_um.top)%100==0 && SameRaster(r,rendered.raster),what);
        const cr::PoseModel model(rendered.camera_b);
        // At the capture's scale: every row, every fifth column.
        {
            const auto grid=px::MakeFixedPreviewGrid(r,1.0);
            Check(grid.width==r.width_pixels && grid.height==r.height_pixels,what);
            const RegisteredMapping fixed{cr::PoseParameters{},{},std::nullopt};
            const px::LiveRegistrationContext context(rendered,r,seam,grid,1.0,1.0,7360,4912,false,fixed);
            for(const auto& mapping:mappings){
                const auto mine=px::LiveViewRegisteredOutputToCamera(context,mapping);
                const auto theirs=RegisteredDocumentOutputToCamera(r,rendered.camera_a,rendered.camera_b,model,mapping,seam);
                bool same=true;long long n=0;
                for(std::uint32_t y=0;y<grid.height;++y)for(std::uint32_t x=0;x<grid.width;x+=5){
                    render::Point a{},b{};
                    const bool oa=mine({double(x),double(y)},a),ob=theirs({double(x),double(y)},b);
                    same=same && oa==ob && (!oa || (a.x==b.x && a.y==b.y));++n;
                }
                Check(same && n==static_cast<long long>(grid.height)*((grid.width+4)/5),what);
            }
        }
        // At the live view's scale: every point of C's grid, none passed over.
        for(const auto& view:views){
            const auto plan=px::PlanFixedPreviewV3(*v3,orientation,view.first,view.second);
            Check(!plan.skip && plan.live!=nullptr,what);
            if(plan.skip || !plan.live)continue;
            // The scale of the live view against the stored frame is the view's size over the sensor's, worked out here and not read from the plan.
            Check(plan.scale_x==static_cast<double>(view.first)/kSensorWidthPx && plan.scale_y==static_cast<double>(view.second)/kSensorHeightPx,what);
            const double s=plan.scale_x,pitch_jpeg=25.4/static_cast<double>(r.dpi),pitch_live=pitch_jpeg/s;
            const double left=static_cast<double>(r.region_um.left)/1000.0,top=static_cast<double>(r.region_um.top)/1000.0;
            for(const auto& mapping:{mappings[1],mappings[3]}){
                const auto mine=px::LiveViewRegisteredOutputToCamera(*plan.live,mapping);
                const auto jpeg=RegisteredDocumentOutputToCamera(r,rendered.camera_a,rendered.camera_b,model,mapping,seam);
                double worst=0;long long n=0,picked=0;
                for(std::uint32_t y=0;y<plan.output_height;++y)for(std::uint32_t x=0;x<plan.output_width;++x){
                    ++picked;
                    const render::Point drawing{left+(x+.5)*pitch_live,top+(y+.5)*pitch_live};
                    const render::Point at{(drawing.x-left)/pitch_jpeg-.5,(drawing.y-top)/pitch_jpeg-.5};
                    render::Point raw{},live{};
                    if(!jpeg(at,raw) || !mine({double(x),double(y)},live))continue;
                    render::Point want{raw.x*s,raw.y*plan.scale_y};
                    if(turned)want={(view.first-1.0)-want.x,(view.second-1.0)-want.y};
                    worst=std::max(worst,std::hypot(want.x-live.x,want.y-live.y));++n;
                }
                if(worst>=1e-6 || n!=picked)std::cerr<<what<<": view "<<view.first<<"x"<<view.second<<" worst "<<worst<<" px, compared "<<n<<" of "<<picked<<'\n';
                Check(n==picked && picked==static_cast<long long>(plan.output_width)*plan.output_height && picked>0 && worst<1e-6,what);
            }
        }
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
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
    double shift_y_mm{};      // B moved along the sheet's y (positive is down on the drawing)
};
// The true document-to-image matrices of the two cameras for a case. The profile has the nominal ones.
struct Truth {render::Matrix3 a,b;};
Truth TruthOf(const Case& c){
    const render::Matrix3 a0=c.turned?render::Matrix3{-10,0,7359-100.0,0,-10,4911-100.0,0,0,1}:render::Matrix3{10,0,100,0,10,100,0,0,1};
    const render::Matrix3 b0=c.turned?render::Matrix3{-10,0,7359+5660.0,0,-10,4911-100.0,0,0,1}:render::Matrix3{10,0,-5660,0,10,100,0,0,1};
    // B moved by `shift`: the feature at X is seen where X - shift was seen; turned about the axis by the angle: about the middle of its view.
    const double t=c.rotation_deg*std::numbers::pi/180,cx=934.0,cy=236.0;
    const render::Matrix3 move{1,0,-c.shift_mm,0,1,-c.shift_y_mm,0,0,1};
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
        {
            // The cameras are named by their roles and by their aliases; the writer takes the aliases from the roles' constants.
            const std::string reference(rc::kReferenceCamera),target(rc::kTargetCamera);
            CHECK(At(record,"referenceCamera").string==reference && At(record,"targetCamera").string==target);
            CHECK(reference=="CAM-A" && target=="CAM-B");
            const auto& originals=At(record,"originalSha256");
            CHECK(originals.kind==json::JsonKind::object && originals.object.size()==2 && originals.object.count(reference)==1 && originals.object.count(target)==1);
            CHECK(At(originals,reference.c_str()).string==in.original_a_sha256 && At(originals,target.c_str()).string==in.original_b_sha256);
            const auto& residual=At(At(record,"estimate"),"targetResidualPx");
            CHECK(residual.kind==json::JsonKind::object && residual.object.count("rms")==1 && residual.object.count("max")==1 && residual.object.count("points")==1);
            // The shape as written: the roles after the time, the hashes in the alias order, and no field that names a camera by a letter.
            const auto& text=proposal.estimate_record_text;
            CHECK(text.find("\"measuredAt\":\"2000-01-01T00:00:00Z\",\"referenceCamera\":\"CAM-A\",\"targetCamera\":\"CAM-B\",\"originalSha256\":{\"CAM-A\":\""+std::string(64,'1')+
                "\",\"CAM-B\":\""+std::string(64,'2')+"\"},\"parent\":")!=std::string::npos);
            for(const char* old:{"\"inputs\"","cameraASha256","cameraBSha256","cameraBResidualPx"})CHECK(text.find(old)==std::string::npos);
            // What the record says is what the computation did: the target's input hash is the one the new profile names, the reference's block is the parent's.
            const auto new_profile=Tree(cand.profile_bytes);
            const auto old_profile=Tree(profile);
            const auto& new_inputs=At(At(new_profile,"calibration"),"inputSha256");
            const auto& old_inputs=At(At(old_profile,"calibration"),"inputSha256");
            CHECK(At(new_inputs,target.c_str()).string==At(originals,target.c_str()).string);
            CHECK(At(new_inputs,reference.c_str()).string==At(old_inputs,reference.c_str()).string);
        }
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

// A pair on which B moved up, with a raster near the lower edge of what the cameras see. The wide search finds the move; the raster that
// follows B's cover gives up the rows at the bottom that B no longer sees (it used to stop at "no overlap"), the fine stage runs on the rows
// that are left, and the candidate is made, checked and verified on them. Both mountings: the same physical move up is up on the drawing.
void VerticalPair(){
    const Texture texture(7919317);
    const std::function<double(double,double)> paper=[&](double x,double y){return texture(x,y);};
    const render::OutputRaster raster=ps::Raster(560000,300000,740000,470000,254);
    for(const bool turned:{false,true}){
        const Case c{turned?"vertical-up-12-turn":"vertical-up-12",0,0,0,turned,-12.0};
        if(!CaseWanted(c.name))continue;
        const auto profile=RigText(turned);
        auto active=MakeActive("pipeline-"+c.name,profile,turned?MountOrientation::rotated_180:MountOrientation::normal,raster,TestRanges);
        const auto truth=TruthOf(c);
        const auto image_a=RenderFrame(truth.a,paper);
        const auto image_b=RenderFrame(truth.b,paper);
        auto in=MakeInputs(active,&image_a,&image_b);
        std::vector<std::string> stages;
        in.progress=[&](rc::Stage s){stages.emplace_back(rc::StageCode(s));};
        const auto p=rc::ProposeRecalibration(in);
        CHECK(p.wide_performed && p.wide.determined && p.wide.reason.empty() && p.wide.tiles_agreed>0 && p.wide.correspondences>0);
        CHECK(p.ready && p.refusals.empty() && p.candidate && p.verification.performed && p.verification.passed);
        CHECK(stages.size()==5 && stages.front()=="wide-search" && stages.back()=="verify");
        // The raster of the fine stage and the candidate's both gave up rows at the bottom and kept the top, the left and the DPI.
        // Each gives up no more than B's lower end on the seam column moved at the pose it was chosen at (the wide search's, the fine stage's),
        // to within one row: B moved up by 12 mm, so about 12 mm (it used to be about 17 mm, the reach of the range's vertices).
        const auto parent_parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
        const auto given_up_within=[&](const render::OutputRaster& r,const cr::PoseParameters& pose,const char* what){
            const double given_up=470.0-static_cast<double>(r.region_um.bottom)/1000.0;
            const double allowed=LowerEndMovedUpMm(parent_parameters.camera_b,pose,SeamXMm(active.condition));
            std::cerr<<c.name<<": "<<what<<" gives up "<<given_up<<" mm, B's lower end moved up "<<allowed<<" mm, height "<<r.height_pixels<<" rows\n";
            Check(given_up<=allowed+kColumnSlackMm && given_up>=allowed-kPitchMm-kColumnSlackMm,what);
        };
        CHECK(p.fine_raster.has_value());
        if(p.fine_raster){
            CHECK(p.fine_raster->region_um.top==raster.region_um.top && p.fine_raster->region_um.bottom<raster.region_um.bottom && p.fine_raster->height_pixels<raster.height_pixels);
            CHECK(p.fine_raster->region_um.left==raster.region_um.left && p.fine_raster->dpi==raster.dpi);
            given_up_within(*p.fine_raster,p.wide.pose,"the fine stage's raster");
        }
        const auto& d=p.display;
        CHECK(d.has_new && d.has_start_motion && d.has_motion && !d.target_cover_shortfall.present);
        CHECK(d.height_previous_px==raster.height_pixels && d.height_new_px<d.height_previous_px && d.bottom_new_mm<d.bottom_previous_mm && d.top_new_mm==d.top_previous_mm);
        CheckNear(d.top_previous_mm,300.0,1e-9,"the raster's top edge");
        CheckNear(d.bottom_previous_mm,470.0,1e-9,"the raster's bottom edge");
        CheckNear(d.start_motion.camera_motion_mm.y,-12.0,2.0,"the wide search saw B up");
        CheckNear(d.motion.camera_motion_mm.y,-12.0,1.0,"the fine stage saw B up");
        CheckNear(d.motion.camera_motion_mm.x,0.0,1.0,"and not to the side");
        if(p.candidate){
            const auto& cand=*p.candidate;const auto& r=cand.condition.raster;
            CHECK(r.region_um.top==raster.region_um.top && r.region_um.bottom<raster.region_um.bottom && r.height_pixels==d.height_new_px && r.width_pixels==raster.width_pixels);
            given_up_within(r,p.pose,"the candidate's raster");
            CheckNear(470.0-static_cast<double>(r.region_um.bottom)/1000.0,12.0,0.2,"the candidate gives up about the 12 mm that B moved up");
            CHECK(SameRaster(r,cand.binding.parameters.raster));
            // The new profile puts B where the pair says it is: the seam is aligned over the rows that are kept.
            const auto a_nominal=MatrixOf(parent_parameters.camera_a);
            const auto baked=MatrixOf(cand.binding.parameters.camera_b);
            const double bottom_mm=static_cast<double>(r.region_um.bottom)/1000.0;
            double after=0,before=0;
            for(double x=626.0;x<=700.0;x+=74.0/12)for(const double y:{320.0,380.0,bottom_mm-10.0}){
                after=std::max(after,AlignmentError(a_nominal,baked,truth,{x,y}));
                before=std::max(before,AlignmentError(a_nominal,MatrixOf(parent_parameters.camera_b),truth,{x,y}));
            }
            CHECK(after<=0.5 && before>after);
            // The condition says what the screen says: the log line of the raster's change would carry these four numbers.
            CHECK(cand.condition.seam.column==FixedCompositeProvisionalSeamColumn(cand.binding.parameters));
        }
        // The same pair with the raster kept (`keep`): at the wide search's pose the rows at the bottom of the parent's raster are below B's
        // lower edge, so the band is refused at the start pose, before the fine stage runs. The number of rows is pinned (run once and written
        // down); it is what B's lower edge at the wide search's pose, worked out from B's matrix, leaves above the bottom edge.
        {
            auto kept=MakeInputs(active,&image_a,&image_b);kept.raster=rc::RasterPlacement::keep;
            std::vector<std::string> kept_stages;
            kept.progress=[&](rc::Stage s){kept_stages.emplace_back(rc::StageCode(s));};
            const auto q=rc::ProposeRecalibration(kept);
            const auto& s=q.display.target_cover_shortfall;
            const std::uint32_t pinned=turned?kKeptRowsHalfTurn:kKeptRowsNormal;
            const cr::PoseModel model_b(parent_parameters.camera_b);
            const auto at_start=model_b.CameraHomography(q.wide.pose);
            // A row is seen by B on the band when B's lower edge is below it on at least one column of the band (566 to 726 mm on this rig).
            const double lowest=std::max(LowestY(at_start,566.0),LowestY(at_start,726.0));
            std::cerr<<c.name<<" kept: refusals";
            for(const auto r:q.refusals)std::cerr<<' '<<rc::RefusalCode(r);
            std::cerr<<" reason "<<ReasonOf(q,rc::Refusal::band_uncovered)<<" rows_bottom "<<s.rows_bottom<<" bottom_mm "<<s.bottom_mm<<" B's lower edge "<<lowest<<" mm\n";
            if(s.rows_bottom!=pinned)std::cerr<<"KEPT "<<c.name<<" rows_bottom="<<s.rows_bottom<<'\n';
            CHECK(!q.candidate && !q.ready && q.refusals.size()==1 && q.refusals.front()==rc::Refusal::band_uncovered && ReasonOf(q,rc::Refusal::band_uncovered)=="bottom");
            CHECK(q.wide.determined && kept_stages.size()==1 && kept_stages.front()=="wide-search" && !q.fine_raster && !q.verification.performed);
            CHECK(s.present && !s.at_estimate && !s.all_rows && s.rows_top==0 && s.rows_bottom==pinned);
            CheckNear(s.bottom_mm,static_cast<double>(pinned)*kPitchMm,1e-9,"the rows B does not see below the kept raster, in mm");
            CheckNear(s.top_mm,0.0,1e-12,"no row is missing at the top");
            CheckNear(static_cast<double>(pinned),(470.0-lowest)/kPitchMm,1.5,"the rows below B's lower edge at the wide search's pose");
            CHECK(!q.display.has_new && q.display.has_start_motion && q.display.height_previous_px==raster.height_pixels);
        }
        std::error_code ignored;fs::remove_all(active.settings,ignored);
    }
    // The seam placement that the caller names decides where the fine stage's raster ends. B moved up by more than the range's vertices
    // reach below the raster's bottom: the rows that are given up are then the ones the vertices hold, which depend on the seam column. With the
    // seam kept at a column that is not the middle of the band the raster differs from the one that the middle of the band gives, so a fine
    // stage that took another placement than the caller's would be seen here. The fine stage is stopped as it starts: only its raster is read.
    {
        const render::OutputRaster higher=ps::Raster(560000,300000,740000,450000,254);
        for(const bool turned:{false,true}){
            const Case c{turned?"vertical-up-30-keep-turn":"vertical-up-30-keep",0,0,0,turned,-30.0};
            if(!CaseWanted(c.name))continue;
            const auto profile=RigText(turned);
            const auto orientation=turned?MountOrientation::rotated_180:MountOrientation::normal;
            auto active=MakeActive("pipeline-"+c.name,profile,orientation,higher,[](FixedCompositeConditionV3& condition){TestRanges(condition);condition.seam.column=1500;});
            const auto truth=TruthOf(c);
            const auto image_a=RenderFrame(truth.a,paper);
            const auto image_b=RenderFrame(truth.b,paper);
            auto in=MakeInputs(active,&image_a,&image_b);
            in.seam=rc::SeamPlacement::keep_previous;
            std::atomic<bool> fine_stage_started{false};
            in.progress=[&](rc::Stage stage){if(stage==rc::Stage::fine_fit)fine_stage_started=true;};
            in.control.stop=[&]{return fine_stage_started.load();};
            const auto p=rc::ProposeRecalibration(in);
            CHECK(p.wide_performed && p.wide.determined && fine_stage_started.load() && HasRefusal(p,rc::Refusal::stopped));
            const auto parent=BindFixedCompositeProfileV3(active.condition,profile,kHash);
            const auto by_keep=rc::ProposeRaster(active.condition,parent,p.wide.pose,rc::RasterPlacement::follow_camera_b,rc::SeamPlacement::keep_previous);
            const auto by_band=rc::ProposeRaster(active.condition,parent,p.wide.pose,rc::RasterPlacement::follow_camera_b,rc::SeamPlacement::band_center);
            std::cerr<<c.name<<": rows "<<by_keep.height_pixels<<" with the seam kept, "<<by_band.height_pixels<<" with the middle of the band, of "<<higher.height_pixels<<"\n";
            CHECK(by_keep.region_um.bottom<higher.region_um.bottom && by_band.region_um.bottom<higher.region_um.bottom);
            CHECK(!SameRaster(by_keep,by_band));
            CHECK(p.fine_raster.has_value());
            if(p.fine_raster){
                CHECK(SameRaster(*p.fine_raster,by_keep));
                CHECK(!SameRaster(*p.fine_raster,by_band));
            }
            std::error_code ignored;fs::remove_all(active.settings,ignored);
        }
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

    // A white sheet and a sheet that is only a repeat: nothing is found, nothing is made. Each sheet has one answer (run once and written
    // down): the wide search finds too few tiles that agree, so it stops there with `no-overlap` alone, its reason the wide search's own, and
    // the fine stage does not run.
    if(CaseWanted("refuse-paper")){
        struct Sheet {const char* name;const std::function<double(double,double)>* paper;const char* reason;};
        const Sheet sheets[]{{"white",&white,"insufficient-tiles"},{"periodic",&periodic,"insufficient-tiles"}};
        for(const auto& sheet:sheets){
            auto active=MakeActive(std::string("refuse-")+sheet.name,profile,MountOrientation::normal,raster,TestRanges);
            const auto a=RenderFrame(moved.a,*sheet.paper),b=RenderFrame(moved.b,*sheet.paper);
            auto in=MakeInputs(active,&a,&b);
            std::vector<std::string> stages;
            in.progress=[&](rc::Stage s){stages.emplace_back(rc::StageCode(s));};
            const auto proposal=rc::ProposeRecalibration(in);
            describe(sheet.name,proposal);
            const auto reason=rc::RefusalReasonOf(proposal,rc::Refusal::no_overlap);
            if(proposal.refusals.size()!=1 || reason!=sheet.reason)std::cerr<<"PAPER "<<sheet.name<<" reason "<<reason<<'\n';
            CHECK(!proposal.ready && !proposal.candidate && !proposal.verification.performed);
            CHECK(proposal.refusals.size()==1 && proposal.refusals.front()==rc::Refusal::no_overlap && reason==sheet.reason);
            CHECK(proposal.wide_performed && !proposal.wide.determined && proposal.wide.reason==sheet.reason);
            CHECK(!proposal.fine_raster && !proposal.display.has_start_motion && !proposal.display.target_cover_shortfall.present);
            CHECK(stages.size()==1 && stages.front()=="wide-search");
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
        CHECK(!proposal.display.has_new);   // the width is known, the seam and the height are not: the screen must not show the "after" side
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
        // The time of the new revision and, when the condition enables layer two, its strain-bound constants are the caller's to give.
        auto no_time=MakeInputs(active,nullptr,nullptr);no_time.now={};
        REJECT(rc::ProposeFromPose(no_time,cr::PoseParameters{},fit,"{}"));
        auto no_layer=MakeInputs(active,nullptr,nullptr);no_layer.layer_two.reset();
        CHECK(active.condition.registration.parallax.has_value());
        REJECT(rc::ProposeFromPose(no_layer,cr::PoseParameters{},fit,"{}"));
    }
    std::error_code ignored;fs::remove_all(active.settings,ignored);
}


// ---- The diagnostics that the check on the new profile leaves -------------------------------------------------------------------
// The check copies the estimator's diagnostics of its own estimate. The candidate of a move along the baseline is checked against its pair, and
// the same estimate is run here with the settings that the check takes from the candidate: each copied field is compared with the field it was
// copied from, and none of them is a default.
void VerificationDiagnostics(){
    if(!CaseWanted("verification-diagnostics"))return;
    const Texture texture(7919317);
    const std::function<double(double,double)> paper=[&](double x,double y){return texture(x,y);};
    const render::OutputRaster raster=ps::Raster(560000,100000,740000,260000,254);
    const Case c{"verification-diagnostics",30,0,0,false};
    const auto profile=RigText(false);
    auto active=MakeActive("verification-diagnostics",profile,MountOrientation::normal,raster,TestRanges);
    const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
    const auto truth=TruthOf(c);
    const auto image_a=RenderFrame(truth.a,paper);
    const auto image_b=RenderFrame(truth.b,paper);
    const auto in=MakeInputs(active,&image_a,&image_b);
    const auto made=rc::ProposeFromPose(in,PoseForShift(parameters.camera_b,30.0,false),rc::ProfileFit{0.1,0.3,50},"{}");
    CHECK(made.candidate.has_value());
    if(made.candidate){
        const auto& candidate=*made.candidate;
        const auto v=rc::VerifyCandidate(in,candidate);
        const auto& condition=candidate.condition;
        cr::RegistrationSettings settings;
        settings.geometry={condition.seam.column,kSeamBlendDesign.width_pixels,condition.registration.geometry.omega_margin_px,condition.registration.geometry.guard_px,
            condition.registration.geometry.band_half_width_px};
        settings.envelope=condition.registration.envelope;settings.limits=condition.registration.limits;settings.design=in.design;
        settings.surface_envelope=condition.registration.surface_envelope;settings.base_pose=condition.base_pose;
        const auto r=cr::EstimateRegistration(image_a,image_b,candidate.binding.parameters,settings,in.thread_count,in.control);
        const auto& d=r.diagnostics;
        CHECK(v.performed && v.state==r.state);
        // The fields are not left at their defaults here, so that a copy that is missing cannot pass for one that is equal.
        CHECK(d.outer_iterations>0 && d.outer_iterations!=cr::EvidenceCounts{}.outer_iterations && !d.change_history_px.empty());
        CHECK(v.evidence.outer_iterations==d.outer_iterations);
        CHECK(v.converged==d.converged && v.last_change_px==d.last_change_px && v.change_history_px==d.change_history_px);
        CHECK(v.second_start==d.second_start && v.second_start_difference_px==d.second_start_difference_px);
        const auto& a=v.fit_window;const auto& b=d.fit_window;
        CHECK(a.grown==b.grown && a.left_px==b.left_px && a.right_px==b.right_px && a.left_available_px==b.left_available_px && a.right_available_px==b.right_available_px &&
            a.left_cut==b.left_cut && a.right_cut==b.right_cut && a.core_points==b.core_points && a.refused_points==b.refused_points);
        CHECK(b.core_points>0 && v.fit_window.core_points>0);
        std::cerr<<"verification diagnostics: "<<d.outer_iterations<<" iterations, converged "<<d.converged<<", last change "<<d.last_change_px<<" px, second start "
            <<cr::SecondStartCode(d.second_start)<<", fit window "<<b.core_points<<" points\n";
    }
    std::error_code ignored;fs::remove_all(active.settings,ignored);
}

// ---- A candidate read back from its text ------------------------------------------------------------------------------------------
// The capture path does not hold the candidate in memory: it reads the revision file that a confirmation wrote. The text of a candidate, written
// beside its profile and read by the condition's reader, gives the range and the seam column of the candidate, and the live view and the JPEG
// agree on what is read back as they do on the candidate in memory: at the capture's scale bit for bit, at the live view's scales to 1e-6
// live pixels.
LoadedFixedCompositeConditionV3 ReadBack(const rc::Candidate& candidate,const std::string& name){
    const auto settings=NewFolder(name);
    const auto revisions=settings/std::string(kFixedCompositeRevisionFolderName);
    fs::create_directories(revisions);
    CopyFixedCompositeProfile(settings,candidate.profile_bytes,kHash);
    const auto file=revisions/FixedCompositeRevisionFileName(candidate.condition.revision.sequence);
    WriteAll(file,candidate.text);
    auto loaded=LoadFixedCompositeConditionV3(file,kHash);
    std::error_code ignored;fs::remove_all(settings,ignored);
    return loaded;
}
// Which edges of the parent's raster the candidate's raster moved in.
enum class EdgesMoved {bottom,right_and_bottom,top};
void LiveAndJpegOnReadBack(const rc::Candidate& candidate,const render::OutputRaster& parent,EdgesMoved moved,const std::string& name){
    const char* what=name.c_str();
    const auto read=ReadBack(candidate,"read-back-files");
    const auto& r=read.condition.raster;
    const auto& rendered=read.profile.parameters;
    const auto seam=read.condition.seam.column;
    const auto orientation=read.condition.orientation;
    const bool turned=orientation==MountOrientation::rotated_180;
    // What was written is what is read: the revision's hash, the range, the seam column and the raster that the profile is bound to.
    CHECK(read.file_sha256==candidate.sha256 && SameRaster(r,candidate.condition.raster) && seam==candidate.condition.seam.column && SameRaster(r,rendered.raster));
    // The scene is one in which the range's top, bottom or right edge differs from the parent's.
    CHECK(r.region_um.left==parent.region_um.left && r.dpi==parent.dpi);
    switch(moved){
    case EdgesMoved::bottom:CHECK(r.region_um.top==parent.region_um.top && r.region_um.bottom<parent.region_um.bottom && r.region_um.right==parent.region_um.right);break;
    case EdgesMoved::right_and_bottom:CHECK(r.region_um.top==parent.region_um.top && r.region_um.bottom<parent.region_um.bottom && r.region_um.right<parent.region_um.right);break;
    case EdgesMoved::top:CHECK(r.region_um.top>parent.region_um.top && r.region_um.bottom==parent.region_um.bottom && r.region_um.right==parent.region_um.right);break;
    }
    const cr::PoseModel model(rendered.camera_b);
    const std::vector<RegisteredMapping> mappings{
        RegisteredMapping{cr::PoseParameters{},{},std::nullopt},
        RegisteredMapping{cr::PoseParameters{0.15,-0.1,0.3,2.0,-1.5,0.4},{},std::nullopt},
        RegisteredMapping{cr::PoseParameters{},SurfacePlane{6.0,0.004,-0.002},std::nullopt},
        RegisteredMapping{cr::PoseParameters{0.15,-0.1,0.3,2.0,-1.5,0.4},SurfacePlane{9.0,0.005,-0.003},std::nullopt}};
    // At the capture's scale: every row, every fifth column.
    {
        const auto grid=px::MakeFixedPreviewGrid(r,1.0);
        CHECK(grid.width==r.width_pixels && grid.height==r.height_pixels);
        const RegisteredMapping fixed{cr::PoseParameters{},{},std::nullopt};
        const px::LiveRegistrationContext context(rendered,r,seam,grid,1.0,1.0,7360,4912,false,fixed);
        for(const auto& mapping:mappings){
            const auto mine=px::LiveViewRegisteredOutputToCamera(context,mapping);
            const auto theirs=RegisteredDocumentOutputToCamera(r,rendered.camera_a,rendered.camera_b,model,mapping,seam);
            bool same=true;long long n=0;
            for(std::uint32_t y=0;y<grid.height;++y)for(std::uint32_t x=0;x<grid.width;x+=5){
                render::Point a{},b{};
                const bool oa=mine({double(x),double(y)},a),ob=theirs({double(x),double(y)},b);
                same=same && oa==ob && (!oa || (a.x==b.x && a.y==b.y));++n;
            }
            CHECK(same && n==static_cast<long long>(grid.height)*((grid.width+4)/5));
        }
    }
    // At the live view's scales: every point of C's grid, none passed over.
    const std::vector<std::pair<unsigned,unsigned>> views{{640,427},{400,267},{640,400}};
    for(const auto& view:views){
        const auto plan=px::PlanFixedPreviewV3(read,orientation,view.first,view.second);
        if(plan.skip)std::cerr<<what<<": view "<<view.first<<"x"<<view.second<<" skipped: "<<plan.detail<<'\n';
        CHECK(!plan.skip && plan.live!=nullptr);
        if(plan.skip || !plan.live)continue;
        const double s=static_cast<double>(view.first)/kSensorWidthPx,sy=static_cast<double>(view.second)/kSensorHeightPx;
        CHECK(plan.scale_x==s && plan.scale_y==sy);
        const double pitch_jpeg=25.4/static_cast<double>(r.dpi),pitch_live=pitch_jpeg/s;
        const double left=static_cast<double>(r.region_um.left)/1000.0,top=static_cast<double>(r.region_um.top)/1000.0;
        for(const auto& mapping:{mappings[1],mappings[3]}){
            const auto mine=px::LiveViewRegisteredOutputToCamera(*plan.live,mapping);
            const auto jpeg=RegisteredDocumentOutputToCamera(r,rendered.camera_a,rendered.camera_b,model,mapping,seam);
            double worst=0;long long n=0,picked=0;
            for(std::uint32_t y=0;y<plan.output_height;++y)for(std::uint32_t x=0;x<plan.output_width;++x){
                ++picked;
                const render::Point drawing{left+(x+.5)*pitch_live,top+(y+.5)*pitch_live};
                const render::Point at{(drawing.x-left)/pitch_jpeg-.5,(drawing.y-top)/pitch_jpeg-.5};
                render::Point raw{},live{};
                if(!jpeg(at,raw) || !mine({double(x),double(y)},live))continue;
                render::Point want{raw.x*s,raw.y*sy};
                if(turned)want={(view.first-1.0)-want.x,(view.second-1.0)-want.y};
                worst=std::max(worst,std::hypot(want.x-live.x,want.y-live.y));++n;
            }
            if(worst>=1e-6 || n!=picked)std::cerr<<what<<": view "<<view.first<<"x"<<view.second<<" worst "<<worst<<" px, compared "<<n<<" of "<<picked<<'\n';
            CHECK(n==picked && picked==static_cast<long long>(plan.output_width)*plan.output_height && picked>0 && worst<1e-6);
        }
    }
}
void ReadBackOf(const std::string& name,const std::string& profile,MountOrientation orientation,const render::OutputRaster& raster,const cr::PoseParameters& pose,EdgesMoved moved){
    const rc::ProfileFit fit{0.1,0.3,50};
    auto active=MakeActive("read-back-"+name,profile,orientation,raster,TestRanges);
    const auto p=rc::ProposeFromPose(MakeInputs(active,nullptr,nullptr),pose,fit,"{}");
    if(!p.candidate)std::cerr<<name<<": "<<p.detail<<'\n';
    Check(p.candidate.has_value() && p.refusals.empty(),name.c_str());
    if(p.candidate)LiveAndJpegOnReadBack(*p.candidate,raster,moved,name);
    std::error_code ignored;fs::remove_all(active.settings,ignored);
}
void CandidateReadBack(){
    for(const bool turned:{false,true}){
        const std::string mount=turned?"half turn":"normal";
        const auto orientation=turned?MountOrientation::rotated_180:MountOrientation::normal;
        // B moved up: the rows at the bottom are given up.
        {
            const auto profile=RigText(turned);
            const render::OutputRaster raster=ps::Raster(560000,360000,740000,470000,254);
            const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
            ReadBackOf("up, "+mount,profile,orientation,raster,PoseForVertical(parameters.camera_b,-12.0,turned),EdgesMoved::bottom);
        }
        // B moved to the left and up, the raster's right edge near B's right side: the right edge follows B's right side and the rows at the
        // bottom are given up. A coarse raster keeps the grid of the capture's scale small.
        {
            const auto profile=RigTextMovedLeft(turned,kRigMovedLeftMm);
            const render::OutputRaster raster=ps::Raster(560000,300000,static_cast<long long>(std::llround((kRigRightMm-kRightSideReachMm-1.0)*1000.0)),470000,100);
            const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
            auto pose=PoseForShift(parameters.camera_b,-30.0,turned);
            pose.ty_mm=PoseForVertical(parameters.camera_b,-40.0,turned).ty_mm;
            ReadBackOf("left and up near the right side, "+mount,profile,orientation,raster,pose,EdgesMoved::right_and_bottom);
        }
        // B moved down, the raster near the top of what B sees: the rows at the top are given up. The raster is short, so that what is left keeps
        // the grid of the capture's scale within its bound.
        {
            const auto profile=RigText(turned);
            const render::OutputRaster raster=ps::Raster(560000,5000,740000,125000,254);
            const auto parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
            ReadBackOf("down near the top, "+mount,profile,orientation,raster,PoseForVertical(parameters.camera_b,30.0,turned),EdgesMoved::top);
        }
    }
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
        RasterFromSensorSides();lap("the raster from the sides of the sensors");
        FrozenLeftRight();lap("frozen left-right scenes");
        VerticalFromPose();lap("up and down from a pose");
        CandidateLiveAndJpeg();lap("the live view and the JPEG on a candidate");
        RefusalsFromPose();lap("refusals from a pose");
        Pipeline();lap("pipeline");
        VerticalPair();lap("vertical pair");
        Refusals();lap("refusals");
        CandidateReadBack();lap("candidates read back from their text");
        VerificationDiagnostics();lap("the diagnostics of the check on the new profile");
    }catch(const std::exception& e){
        std::cerr<<"FAILED: an exception: "<<e.what()<<'\n';
        return 1;
    }
    std::cout<<"fixed composite recalibration: "<<checks<<" checks, "<<failures<<" failures\n";
    return failures==0?0:1;
}
