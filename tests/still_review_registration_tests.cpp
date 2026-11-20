// The JPEG path of the registered fixed composite (#417 J8): a condition v3 revision and the profile it names compose a
// two-camera pair into the condition's raster through the capture registration (estimate, layer 2, level, composite, then
// the gate), and the report v8 records it and reads back strictly. The tools that place B by a similarity refuse v8.
//
// Everything is synthetic and camera free, and no window is opened. The frames are full size (7360 x 4912, what the
// profile's camera model fixes) written as JPEG files, from the rig profile v2 fixture (10 pixels per mm, the cameras side
// by side) with a drawing-like texture in the strip around the overlap, as the v7 test of #405 builds them. The "turned"
// rig is the same one mounted a half turn.
//
// Argument 1 is the shared rig profile v2 fixture folder.
#include "a0/m2/experimental/still_review.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/m2/experimental/fixed_composite_selection.hpp"
#include "a0/m2/experimental/fixed_scale_correction.hpp"
#include "a0/m2/experimental/fixed_scale_correction_v2.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "similarity_scene.hpp"
#include "registered_review_fixture.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace a0::m2::experimental;
namespace render=a0::m2::render;
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what,int line){++checks;if(!value){++failures;std::cerr<<"FAILED (line "<<line<<"): "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr,__LINE__)
template<class Call> bool Throws(Call call){try{call();}catch(const std::exception&){return true;}return false;}
#define REJECT(expr) Check(Throws([&]{(void)(expr);}),"rejects: " #expr,__LINE__)
using Clock=std::chrono::steady_clock;
double Since(Clock::time_point t){return std::chrono::duration<double>(Clock::now()-t).count();}

const auto kCreated=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026}/11/4}+12h};
void WriteText(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;}
using namespace registered_review_fixture;
std::vector<unsigned char> ReadBytes(const fs::path& path){return ReadFileBytes(path);}

struct Outcome {StillReviewResult result;SavedStillReview saved;std::string text;double seconds{};};
Outcome Run(const StillReviewRequest& request){
    Outcome o;const auto started=Clock::now();
    o.result=WriteUncalibratedStillReview(request);o.seconds=Since(started);
    o.text=ReadText(o.result.report);
    // The registered members alone first, so that a refusal says which check refused.
    if(request.fixed_composite_registration)try{(void)ReadFixedCompositeRegistrationReport(o.text,kHash);}
    catch(const std::exception& error){std::cerr<<"  the reader refused the written report ("<<o.text.size()<<" bytes): "<<error.what()<<std::endl;}
    o.saved=LoadSavedStillReview(o.result.report);
    return o;
}
std::string Describe(const Outcome& o){
    const auto& f=*o.saved.fixed_composite_registration;
    return std::string(GeometryLevelCode(f.registration.level.level))+", estimate "+std::string(content_registration::RegistrationStateCode(f.registration.registration.state))+
        ", layer 2 "+std::string(parallax_correction::ParallaxStateCode(f.registration.parallax.state))+", gate "+std::string(registration_gate::GateStateCode(f.registration.gate.state))+
        ", "+std::string(AssessmentStateText(f.assessment.state))+" ["+[&]{std::string w;for(const auto& x:f.warnings)w+=(w.empty()?"":" ")+std::string(WarningCodeText(x.code));return w;}()+
        "], gate findings ["+[&]{std::string w;for(const auto& x:f.registration.gate.findings)w+=(w.empty()?"":" ")+x.reason;for(const auto& x:f.registration.gate.gate_findings)w+=" "+x.reason;return w;}()+
        "], "+std::to_string(o.seconds)+" s";
}

fs::path root;
std::string base_profile;

void Written(const Outcome& o,const Settings& s,const char* name){
    std::cout<<name<<": "<<Describe(o)<<'\n';
    CHECK(o.saved.fixed_composite_registration.has_value() && !o.saved.fixed_composite_lens && o.text.find("\"a0.uncalibrated-still-review.v9\"")!=std::string::npos);
    CHECK(o.text.find("\"translation\"")==std::string::npos && o.text.find("\"geometryBasis\":\"fixed-composite-registration\"")!=std::string::npos);
    CHECK(o.saved.width==s.loaded.condition.raster.width_pixels && o.saved.height==s.loaded.condition.raster.height_pixels);
    const auto& f=*o.saved.fixed_composite_registration;
    CHECK(f.condition_sha256==s.loaded.file_sha256 && f.condition_sequence==1 && f.seam_column==kSeam && f.layout.mixing_width_px==kSeamBlendDesign.width_pixels);
    CHECK(f.registration.gate.state!=registration_gate::GateState::not_run);   // 900 s is time for every stage
    CHECK(o.result.registration_milliseconds.has_value() && o.result.fixed_composite_registration.has_value());
    // What the writer kept is what the reader reads.
    CHECK(FixedCompositeRegistrationJson(*o.result.fixed_composite_registration)==FixedCompositeRegistrationJson(f));
}

void Tamper(const Outcome& o){
    std::cout<<"a changed report is refused\n";
    const auto damaged=root/"damaged";fs::create_directories(damaged);
    fs::copy_file(o.saved.jpeg,damaged/"uncalibrated-review.jpg",fs::copy_options::overwrite_existing);
    const auto report=damaged/"uncalibrated-review.json";
    const auto loads=[&](const std::string& text){WriteText(report,text);try{(void)LoadSavedStillReview(report);}catch(const std::exception&){return false;}return true;};
    CHECK(loads(o.text));
    {
        // A report that was written without a position is, apart from its schema, the v8 report plus two null members. The v8 file as a whole is read by
        // the loader (the schema picks the reader) and gives the record that the v9 file gives.
        const std::string nulls=",\"confirmedPosition\":null,\"confirmedPositionCheck\":null";
        auto v8=o.text;
        const auto at=v8.find(nulls);
        Check(at!=std::string::npos && v8.find(nulls,at+1)==std::string::npos,"the v9 file has the two null members once",__LINE__);
        if(at!=std::string::npos)v8.erase(at,nulls.size());
        const std::string v9_schema="a0.uncalibrated-still-review.v9";
        const auto schema=v8.find(v9_schema);
        Check(schema!=std::string::npos,"the v9 file names its schema",__LINE__);
        if(schema!=std::string::npos)v8.replace(schema,v9_schema.size(),"a0.uncalibrated-still-review.v8");
        WriteText(report,v8);
        try{
            const auto read=LoadSavedStillReview(report);
            const auto& v8_record=read.fixed_composite_registration;
            CHECK(v8_record.has_value() && v8_record->registration.level.level==o.saved.fixed_composite_registration->registration.level.level &&
                !v8_record->registration.confirmed_position && !v8_record->registration.confirmed_check);
            if(v8_record){
                CHECK(FixedCompositeRegistrationJson(*v8_record)==FixedCompositeRegistrationJson(*o.saved.fixed_composite_registration));
                CHECK(FixedCompositeRegistrationJson(*v8_record,RegistrationReportFormat::v9)==FixedCompositeRegistrationJson(*o.saved.fixed_composite_registration,RegistrationReportFormat::v9));
            }
        }catch(const std::exception& error){Check(false,("the v8 file is read: "+std::string(error.what())).c_str(),__LINE__);}
    }
    const auto changed=[&](const std::string& from,const std::string& to){
        const auto at=o.text.find(from);
        if(at==std::string::npos||o.text.find(from,at+1)!=std::string::npos){Check(false,("token not unique: "+from).c_str(),__LINE__);return std::string(o.text);}
        auto t=o.text;t.replace(at,from.size(),to);return t;
    };
    const auto rejected=[&](const std::string& from,const std::string& to,int line){
        const bool ok=!loads(changed(from,to));
        if(!ok)std::cerr<<"  accepted a changed report: "<<from<<" -> "<<to<<'\n';
        Check(ok,"a changed report is refused",line);
    };
#define REJECTED(a,b) rejected(a,b,__LINE__)
    const auto& f=*o.saved.fixed_composite_registration;
    REJECTED("a0.uncalibrated-still-review.v9","a0.uncalibrated-still-review.v7");
    REJECTED("a0.uncalibrated-still-review.v9","a0.uncalibrated-still-review.v8");   // v8 has no confirmedPosition members
    REJECTED("a0.uncalibrated-still-review.v9","a0.uncalibrated-still-review.v10");
    REJECTED("\"productQualityAccepted\":false","\"productQualityAccepted\":true");
    REJECTED("\"geometryBasis\":\"fixed-composite-registration\"","\"geometryBasis\":\"fixed-composite-lens\"");
    REJECTED("\"fixedCompositeRegistration\":{","\"fixedCompositeRegistration\":{\"extra\":0,");
    REJECTED("\"output\":{","\"translation\":{\"x\":1,\"y\":0},\"output\":{");
    REJECTED("\"level\":\""+std::string(GeometryLevelCode(f.registration.level.level))+"\"",f.registration.level.level==GeometryLevel::g1?"\"level\":\"G2\"":"\"level\":\"G1\"");
    REJECTED("\"geometryHash\":\""+f.registration.geometry_hash+"\"}","\"geometryHash\":\""+std::string(64,'0')+"\"}");
    REJECTED("\"assessment\":{\"state\":\""+std::string(AssessmentStateText(f.assessment.state))+"\"",
        std::string("\"assessment\":{\"state\":\"")+(f.assessment.state==AssessmentState::within_criteria?"not-judged":"within-criteria")+"\"");
    REJECTED("\"method\":\"fixed-seam\",\"widthPixels\":8","\"method\":\"fixed-seam\",\"widthPixels\":10");   // the hash's mixing width is the blend's
    REJECTED("\"sourceDimensions\":{\"width\":7360","\"sourceDimensions\":{\"width\":7359");
    REJECTED("\"sequence\":1},","\"sequence\":0},");
    REJECTED("\"mixingWidthPx\":8","\"mixingWidthPx\":6");
#undef REJECTED
}

void Refusals(const Outcome& o,const Settings& s,const Frames& frames){
    std::cout<<"tools that place B by a similarity refuse v8\n";
    REJECT(MakeFixedCompositeCondition(o.saved,kCreated,1.0));
    REJECT(SavedStillReviewRenderParameters(o.saved));
    REJECT(CompareStillReviewBlends(o.saved,root/"compare",64,64));
    // The registered composite is exclusive.
    auto both=RequestFor(frames,"both",s,4);both.blend=BlendMethod::seam_cut;REJECT(WriteUncalibratedStillReview(both));
    auto no_hash=RequestFor(frames,"no-hash",s,4);no_hash.fixed_composite_registration->condition_sha256="x";REJECT(WriteUncalibratedStillReview(no_hash));
    // A thread count past the limit is refused with its own reason (not as a missing condition), before anything is made.
    {
        auto many=RequestFor(frames,"many-threads",s,kSeamBlendMaxThreads+1);
        std::string reason;
        try{(void)WriteUncalibratedStillReview(many);}catch(const std::invalid_argument& e){reason=e.what();}
        CHECK(reason=="registered fixed composite review: thread count must be 0 (one per hardware thread) or 1..256" && !fs::exists(frames.root/"many-threads"));
    }
    // Another orientation: no composite, only the originals.
    auto turned=RequestFor(frames,"turned-capture",s,4);turned.geometry.orientation=MountOrientation::rotated_180;
    bool skipped=false;try{(void)WriteUncalibratedStillReview(turned);}catch(const FixedCompositeUnavailable& e){skipped=e.Skip()==FixedCompositeSkip::orientation_differs;}
    CHECK(skipped && !fs::exists(frames.root/"turned-capture"));
}

// v8 at p = p_b = 0, pi = 0, q = 0 (the estimate is not trusted on frames without content, so G4) against v7 at scale 1 (no
// estimate either) on the same frames: the same JPEG bytes.
void SameAsV7(const Settings& s){
    std::cout<<"v8 at G4 is v7 at scale 1\n";
    const auto frames=MakeFrames(root,"flat",Flat(128),Flat(128));
    const auto v8=Run(RequestFor(frames,"v8",s,4));
    std::cout<<"  v8: "<<Describe(v8)<<'\n';
    // The v7 condition of the same profile, raster and seam (the scale limit is a sample value).
    const auto v2folder=root/"settings-v2";fs::create_directories(v2folder);
    FixedCompositeV2Request request;
    request.profile_file=CopyFixedCompositeProfile(v2folder,s.profile,kHash);request.profile_bytes=s.profile;request.orientation=MountOrientation::normal;
    request.raster=s.loaded.condition.raster;request.scale_auto_correction_max_percent=1;request.seam_column=kSeam;request.seam_origin=FixedSeamOrigin::operator_set;
    const auto condition=MakeFixedCompositeConditionV2(request,kCreated,kHash,FixedCompositeCreationCheck([](const FixedCompositeConditionV2&,const FixedCompositeProfileBinding&){}));
    WriteFixedCompositeConditionV2(v2folder/kFixedCompositeConditionName,condition);
    const auto selection=SelectFixedComposite(v2folder/kFixedCompositeConditionName,kHash);
    CHECK(selection.IsV2());
    const auto& loaded=std::get<LoadedFixedCompositeConditionV2>(*selection.loaded);
    PreviewReviewGeometry geometry{460,307,229,3,11,MountOrientation::normal};
    StillReviewRequest q{frames.a,frames.b,frames.root/"v7",Sha(frames.a),Sha(frames.b),"synthetic-pair-registered",geometry,Clock::now()+std::chrono::seconds(900)};
    q.fixed_composite_lens=StillReviewRequest::FixedCompositeLens{&loaded.condition,&loaded.profile,loaded.file_sha256,ComparePolicyLimit(1,1.0,FixedPolicyCheck::policy_absent)};
    const auto v7=Run(q);
    CHECK(v7.saved.fixed_composite_lens && v7.saved.fixed_composite_lens->scale.applied_scale==1);
    const auto& f=*v8.saved.fixed_composite_registration;
    CHECK(f.registration.level.level==GeometryLevel::g4 && f.registration.level.mapping.pose.IsZero() && f.registration.level.mapping.IsPoseOnly());
    CHECK(v8.saved.jpeg_sha256==v7.saved.jpeg_sha256);
    CHECK(f.assessment.state!=AssessmentState::within_criteria);
}
// The confirmed position through the writer (#437, report v9): what is written, read back and drawn with.
// `textured` is the pair the estimate is trusted on (G1 without a position) and `plain` its capture without one: a position of this condition that
// was set by hand is the level and changes the JPEG; the condition's own position and another condition's change nothing, byte for byte.
// Flat frames follow: the estimate decides nothing there, so any position of this condition is the level, whatever its source.
void ConfirmedThroughTheWriter(const Settings& s,const Frames& textured,const Outcome& plain){
    std::cout<<"a confirmed position through the writer\n";
    const auto& base=s.loaded.condition.base_pose;
    CHECK(plain.text.find("\"a0.uncalibrated-still-review.v9\"")!=std::string::npos &&
        plain.text.find("\"confirmedPosition\":null,\"confirmedPositionCheck\":null,\"geometryLevel\":{\"level\":\"G1\"")!=std::string::npos);
    CHECK(!plain.saved.fixed_composite_registration->registration.confirmed_position && !plain.saved.fixed_composite_registration->registration.confirmed_check);
    int counter=0;
    const auto run_with=[&](const Frames& frames,const ConfirmedPosition& p){
        const auto name="with-"+std::to_string(++counter);
        auto q=RequestFor(frames,name.c_str(),s,4);
        q.fixed_composite_registration->confirmed_position=p;
        return Run(q);
    };
    const auto position=[&](ConfirmedPositionSource source,const content_registration::PoseParameters& pose,const SurfacePlane& plane,const std::string& sha){
        ConfirmedPosition p;p.mapping=RegisteredMapping{pose,plane,std::nullopt};p.source=source;p.serial=4;p.condition_sha256=sha;
        if(source==ConfirmedPositionSource::manual){p.base_source=ConfirmedPositionSource::auto_start;p.manual_entered=HandCounts{-3,1,-1};}
        return p;
    };
    const auto registration=[](const Outcome& o)->const CaptureRegistration&{return o.saved.fixed_composite_registration->registration;};
    const content_registration::PoseParameters pose{0.1,0,0.05,0.5,0.25,0};
    const SurfacePlane plane{0.5,0.001,0};
    // The condition's own position, and a position of another condition: recorded, not used; the JPEG is the capture without a position, byte for byte.
    {
        const auto own=run_with(textured,position(ConfirmedPositionSource::auto_start,base,{},s.loaded.file_sha256));
        CHECK(own.saved.jpeg_sha256==plain.saved.jpeg_sha256 && registration(own).level.level==registration(plain).level.level && registration(own).confirmed_position && !registration(own).confirmed_check->used);
        const auto foreign=run_with(textured,position(ConfirmedPositionSource::manual,pose,plane,std::string(64,'e')));
        CHECK(foreign.saved.jpeg_sha256==plain.saved.jpeg_sha256 && registration(foreign).level.level==registration(plain).level.level && registration(foreign).confirmed_position && !registration(foreign).confirmed_check->used);
        CHECK(foreign.text.find("\"source\":\"manual\",\"baseSource\":\"auto-start\"")!=std::string::npos &&
            foreign.text.find("\"manualEntered\":{\"seamDxPx\":-12,\"seamDyPx\":4,\"rotationDeg\":-0.050000000000000003}")!=std::string::npos);
    }
    // A position of this condition set by hand: the level, the JPEG is its drawing and not the estimate's, the record has it as given, and the estimate's
    // difference to it is recorded. A tool that places B by a similarity refuses the report, as it refuses every registered one.
    {
        const auto given=position(ConfirmedPositionSource::manual,pose,plane,s.loaded.file_sha256);
        const auto o=run_with(textured,given);
        const auto& r=registration(o);
        std::cout<<"  hand position on the textured pair: level "<<GeometryLevelCode(r.level.level)<<", difference "<<(r.confirmed_check&&r.confirmed_check->seam_difference_px?std::to_string(*r.confirmed_check->seam_difference_px):std::string("none"))<<'\n';
        CHECK(registration(plain).level.level==GeometryLevel::g1 && r.level.level==GeometryLevel::confirmed && r.level.reasons.empty() && r.confirmed_check && r.confirmed_check->used);
        CHECK(r.confirmed_check && r.confirmed_check->seam_difference_px && *r.confirmed_check->seam_difference_px>0);
        CHECK(r.confirmed_position && r.confirmed_position->mapping.pose.rx_deg==pose.rx_deg && r.confirmed_position->mapping.pose.tx_mm==pose.tx_mm && r.confirmed_position->mapping.surface.h0_mm==0.5 &&
            r.confirmed_position->source==ConfirmedPositionSource::manual && r.confirmed_position->serial==4 && r.confirmed_position->manual_entered==given.manual_entered);
        CHECK(r.level.mapping.pose.ty_mm==pose.ty_mm && r.level.mapping.surface.slope_x==0.001 && !r.level.mapping.parallax);
        CHECK(o.saved.jpeg_sha256!=plain.saved.jpeg_sha256);
        // The estimate on the originals is the one without a position, bit for bit (the position is not an input of it).
        CHECK(std::memcmp(&r.registration.pose,&registration(plain).registration.pose,sizeof r.registration.pose)==0 && r.registration.covariance==registration(plain).registration.covariance &&
            r.registration.state==registration(plain).registration.state);
        bool notice=false;
        for(const auto& w:o.saved.fixed_composite_registration->warnings)notice=notice || w.code==WarningCode::position_by_hand;
        CHECK(notice);
        REJECT(MakeFixedCompositeCondition(o.saved,kCreated,1.0));
        REJECT(SavedStillReviewRenderParameters(o.saved));
    }
    // Flat frames: no estimate, so any source of a position of this condition is the level (and the drawing is the same grey either way).
    {
        const auto frames=MakeFrames(root,"flat-confirmed",Flat(128),Flat(128));
        const auto flat=Run(RequestFor(frames,"plain",s,4));
        CHECK(registration(flat).level.level==GeometryLevel::g4 && flat.text.find("\"confirmedPosition\":null,\"confirmedPositionCheck\":null,\"geometryLevel\":{\"level\":\"G4\"")!=std::string::npos);
        for(const auto source:{ConfirmedPositionSource::after_capture,ConfirmedPositionSource::manual}){
            const auto given=position(source,pose,plane,s.loaded.file_sha256);
            const auto o=run_with(frames,given);
            const auto& r=registration(o);
            CHECK(r.level.level==GeometryLevel::confirmed && r.confirmed_check && r.confirmed_check->used && !r.confirmed_check->seam_difference_px && r.confirmed_position->source==source);
            bool notice=false;
            for(const auto& w:o.saved.fixed_composite_registration->warnings)notice=notice || w.code==(source==ConfirmedPositionSource::manual?WarningCode::position_by_hand:WarningCode::position_from_live);
            CHECK(notice);
        }
        // A position the report cannot carry: refused with nothing created.
        auto bad=position(ConfirmedPositionSource::auto_start,pose,plane,s.loaded.file_sha256);bad.mapping.pose.rz_deg=std::numeric_limits<double>::quiet_NaN();
        auto q=RequestFor(frames,"bad-position",s,4);q.fixed_composite_registration->confirmed_position=bad;
        REJECT(WriteUncalibratedStillReview(q));
        CHECK(!fs::exists(frames.root/"bad-position"));
    }
}

}

int main(int argc,char** argv){
    try{
        if(argc<2)throw std::invalid_argument("usage: still_review_registration_tests <rig-profile-v2 fixture folder>");
        Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
        base_profile=ReadText(fs::path(argv[1])/"calibrated-draft.json");
        root=fs::current_path()/("still-review-registration-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        fs::create_directories(root);
        const auto started=Clock::now();
        const World world(17);
        const auto upright=MakeSettings(root/"settings",ProfileText(base_profile,false),MountOrientation::normal,TallRaster());
        const auto frames=TexturedFrames(root,"textured",upright,false,world);
        // Threads 1 and 4: the same composite and the same report bytes.
        const auto four=Run(RequestFor(frames,"review-4",upright,4));
        Written(four,upright,"upright, 4 threads");
        const auto one=Run(RequestFor(frames,"review-1",upright,1));
        Written(one,upright,"upright, 1 thread");
        CHECK(one.text==four.text && one.saved.jpeg_sha256==four.saved.jpeg_sha256);
        // The same rig mounted a half turn: the same level, read back with the orientation.
        const auto turned_settings=MakeSettings(root/"settings-turned",ProfileText(base_profile,true),MountOrientation::rotated_180,TallRaster());
        const auto turned_frames=TexturedFrames(root,"textured-turned",turned_settings,true,world);
        const auto turned=Run(RequestFor(turned_frames,"review",turned_settings,4));
        Written(turned,turned_settings,"turned");
        CHECK(turned.saved.geometry.orientation==MountOrientation::rotated_180 && turned.text.find("\"mountOrientation\":\"rotated-180\"")!=std::string::npos);
        CHECK(turned.saved.fixed_composite_registration->registration.level.level==four.saved.fixed_composite_registration->registration.level.level);
        Tamper(four);
        Refusals(four,upright,frames);
        SameAsV7(upright);
        ConfirmedThroughTheWriter(upright,frames,four);
        std::cout<<"checks="<<checks<<" failures="<<failures<<" seconds="<<Since(started)<<"\n";
        std::error_code ignored;fs::remove_all(root,ignored);
        return failures?1:0;
    }catch(const std::exception& error){
        std::cerr<<"exception: "<<error.what()<<"\n";
        return 2;
    }
}
