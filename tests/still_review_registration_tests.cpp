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

const FixedCompositeSha256 kHash=[](const std::vector<unsigned char>& bytes){return FixedCompositeBytesSha256(bytes);};
const auto kCreated=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026}/11/4}+12h};
constexpr unsigned kWidth=7360,kHeight=4912;

std::string ReadText(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
std::vector<unsigned char> ReadBytes(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void WriteText(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;}
std::string Sha(const fs::path& path){return FixedCompositeBytesSha256(ReadBytes(path));}
std::string ReplaceAll(std::string text,const std::string& from,const std::string& to){
    for(size_t at=0;(at=text.find(from,at))!=std::string::npos;at+=to.size())text.replace(at,from.size(),to);
    return text;
}

// ---- the rig profile (the fixture with the matrices of the side-by-side rig, as the v7 test of #405) ----
std::string Num(double v){char text[48];if(v==std::floor(v))std::snprintf(text,sizeof text,"%.1f",v);else std::snprintf(text,sizeof text,"%.17g",v);return text;}
std::string WithMatrix(const std::string& text,int camera,const std::array<double,6>& m){
    size_t at=0;
    for(int i=0;i<=camera;++i){at=text.find("\"documentToImage\": [",at);if(at==std::string::npos)throw std::runtime_error("fixture has no matrix");if(i<camera)at+=10;}
    const size_t open=text.find('[',at);int depth=0;size_t close=open;
    for(;close<text.size();++close){if(text[close]=='[')++depth;else if(text[close]==']'&&--depth==0)break;}
    std::string body="[\n";
    for(int row=0;row<3;++row){
        const double r[3]{row<2?m[row*3]:0.0,row<2?m[row*3+1]:0.0,row<2?m[row*3+2]:1.0};
        body+="          [\n            "+Num(r[0])+",\n            "+Num(r[1])+",\n            "+Num(r[2])+"\n          ]"+(row<2?",\n":"\n");
    }
    return text.substr(0,open)+body+"        ]"+text.substr(close+1);
}
std::string ProfileText(const std::string& base,bool turned){
    std::string t=base;
    if(turned){t=WithMatrix(t,0,{-10,0,7359-100.0,0,-10,4911-100.0});t=WithMatrix(t,1,{-10,0,7359+5660.0,0,-10,4911-100.0});}
    else{t=WithMatrix(t,0,{10,0,100,0,10,100});t=WithMatrix(t,1,{10,0,-5660,0,10,100});}
    t=ReplaceAll(t,"\"k1\": 0.0","\"k1\": -0.05");t=ReplaceAll(t,"\"k2\": 0.0","\"k2\": 0.02");t=ReplaceAll(t,"\"p1\": 0.0","\"p1\": 0.0005");
    return t;
}
render::OutputRaster Raster(long long left,long long top,long long right,long long bottom,unsigned dpi){
    const auto pixels=[&](long long um){return static_cast<std::uint32_t>((um*static_cast<long long>(dpi)+25399)/25400);};
    return render::OutputRaster{render::DocumentRegionUm{left,top,right,bottom},dpi,pixels(right-left),pixels(bottom-top)};
}
// 18 mm wide around the overlap (A only, the overlap, B only), 46 cm tall, 0.1 mm pixels. Seam column 900 is X = 650.
render::OutputRaster TallRaster(){return Raster(560000,10000,740000,470000,254);}
constexpr std::uint32_t kSeam=900;

// ---- the world and the frames ----
struct World {
    similarity_scene::Scene scene;
    static constexpr double kX0=546,kY0=-13,kPerMm=5;
    explicit World(std::uint64_t seed):scene(similarity_scene::MakeScene(static_cast<int>((756-kX0)*kPerMm),static_cast<int>((485-kY0)*kPerMm),0,0,seed)){}
    double At(double X,double Y) const {
        const double sx=scene.ox+(X-kX0)*kPerMm,sy=scene.oy+(Y-kY0)*kPerMm;
        const int x0=static_cast<int>(std::floor(sx)),y0=static_cast<int>(std::floor(sy));
        if(x0<0||y0<0||x0+1>=scene.width||y0+1>=scene.height)return 232;
        const double fx=sx-x0,fy=sy-y0;const auto* t=scene.v.data()+static_cast<size_t>(y0)*scene.width+x0;const auto* b=t+scene.width;
        return (t[0]*(1-fx)+t[1]*fx)*(1-fy)+(b[0]*(1-fx)+b[1]*fx)*fy;
    }
};
void FillStrip(render::BgrImage& image,const render::CameraProjection& camera,unsigned u0,unsigned u1,const World& world){
    constexpr unsigned step=8;
    const unsigned gw=(u1-u0+step-1)/step+2,gh=(kHeight+step-1)/step+2;
    std::vector<render::Point> grid(static_cast<size_t>(gw)*gh);
    for(unsigned gj=0;gj<gh;++gj)for(unsigned gi=0;gi<gw;++gi){
        render::Point doc{};
        if(!TryDocumentPointOfRawPixel(camera,{double(u0+gi*step),double(gj*step)},doc))throw std::runtime_error("fixture grid has no document point");
        grid[static_cast<size_t>(gj)*gw+gi]=doc;
    }
    const auto rows=[&](unsigned v0,unsigned v1){
        for(unsigned v=v0;v<v1;++v){
            const unsigned gj=v/step;const double fy=(v%step)/double(step);
            auto* out=image.bgr.data()+(static_cast<size_t>(v)*kWidth+u0)*3;
            for(unsigned u=u0;u<u1;++u,out+=3){
                const unsigned gi=(u-u0)/step;const double fx=((u-u0)%step)/double(step);
                const auto& p00=grid[static_cast<size_t>(gj)*gw+gi];const auto& p10=grid[static_cast<size_t>(gj)*gw+gi+1];
                const auto& p01=grid[static_cast<size_t>(gj+1)*gw+gi];const auto& p11=grid[static_cast<size_t>(gj+1)*gw+gi+1];
                similarity_scene::Put(out,world.At((p00.x*(1-fx)+p10.x*fx)*(1-fy)+(p01.x*(1-fx)+p11.x*fx)*fy,(p00.y*(1-fx)+p10.y*fx)*(1-fy)+(p01.y*(1-fx)+p11.y*fx)*fy));
            }
        }
    };
    std::vector<std::thread> pool;
    for(unsigned t=0;t<4;++t)pool.emplace_back(rows,kHeight*t/4,kHeight*(t+1)/4);
    for(auto& th:pool)th.join();
}
void Hr(HRESULT hr){if(FAILED(hr))throw std::runtime_error("fixture WIC failure");}
void WriteJpeg(const fs::path& path,const render::BgrImage& image){
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream));Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder));Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> props;Hr(encoder->CreateNewFrame(&frame,&props));Hr(frame->Initialize(props.Get()));Hr(frame->SetSize(image.width,image.height));
    auto format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format));auto pixels=image.bgr;
    Hr(frame->WritePixels(image.height,image.width*3,static_cast<UINT>(pixels.size()),pixels.data()));Hr(frame->Commit());Hr(encoder->Commit());
}
render::BgrImage Flat(std::uint8_t value){return {kWidth,kHeight,std::vector<std::uint8_t>(static_cast<size_t>(kWidth)*kHeight*3,value)};}
struct Frames {fs::path a,b,root;};
Frames MakeFrames(const fs::path& root,const std::string& name,const render::BgrImage& a,const render::BgrImage& b){
    const auto dir=root/name;fs::create_directories(dir/"CAM-A");fs::create_directories(dir/"CAM-B");
    WriteJpeg(dir/"CAM-A/original.jpg",a);WriteJpeg(dir/"CAM-B/original.jpg",b);
    return {dir/"CAM-A/original.jpg",dir/"CAM-B/original.jpg",dir};
}

// ---- a settings folder with the profile and revision r000001 of a condition v3, read back by the V3 reader ----
struct Settings {fs::path folder;LoadedFixedCompositeConditionV3 loaded;std::string profile;};
Settings MakeSettings(const fs::path& folder,const std::string& profile,MountOrientation orientation,const render::OutputRaster& raster){
    fs::create_directories(folder);
    FixedCompositeConditionV3 c;
    c.created_utc="2026-11-04T12:00:00Z";c.revision.sequence=1;c.revision.change.kind=FixedCompositeRevisionKind::created;
    c.source.profile_file=CopyFixedCompositeProfile(folder,profile,kHash);
    c.source.profile_file_sha256=kHash(std::vector<unsigned char>(profile.begin(),profile.end()));
    c.source.profile_fingerprint_sha256=a0::m2::RigProfileV2::Parse(profile).FingerprintSha256();
    c.source.policy_sha256=std::string(64,'c');
    c.orientation=orientation;c.raster=raster;c.seam.column=kSeam;c.seam.origin=FixedSeamOrigin::operator_set;
    // Sample ranges and lines of a test, not owner values.
    auto& g=c.registration;
    g.envelope={10,2,1,5};
    g.limits.brown_lowe_alpha=8;g.limits.brown_lowe_beta=0.3;g.limits.minimum_unique_blocks=2;g.limits.maximum_prediction_sigma_px=1.0;
    g.limits.maximum_residual_rms_px=0.5;g.limits.maximum_residual_p95_px=1.0;g.limits.maximum_bin_bias_px=0.5;g.limits.convergence_px=0.02;g.limits.start_agreement_px=0.3;
    g.geometry={8,72,256};g.surface_envelope=SurfaceEnvelope{40,8};g.apply_beyond_envelope=true;g.parallax=FixedCompositeConditionV3::Registration::Parallax{1.0};
    c.seam_gate={1.0,0.25};
    ValidateFixedCompositeConditionV3(c);
    const auto revisions=folder/std::string(kFixedCompositeRevisionFolderName);
    fs::create_directories(revisions);
    WriteFixedCompositeConditionV3(revisions,c);
    auto loaded=LoadActiveFixedCompositeRevisionV3(revisions,kHash);
    if(!loaded)throw std::runtime_error("the written revision did not come back");
    return {folder,std::move(*loaded),profile};
}
Frames TexturedFrames(const fs::path& root,const std::string& name,const Settings& s,bool turned,const World& world){
    const auto& p=s.loaded.profile.parameters;
    auto a=Flat(128),b=Flat(128);
    FillStrip(a,p.camera_a,turned?0:5560,turned?1800:kWidth,world);
    FillStrip(b,p.camera_b,turned?5560:0,turned?kWidth:1800,world);
    return MakeFrames(root,name,a,b);
}
StillReviewRequest RequestFor(const Frames& f,const char* output,const Settings& s,unsigned threads){
    PreviewReviewGeometry geometry{460,307,229,3,11,s.loaded.condition.orientation};
    StillReviewRequest q{f.a,f.b,f.root/output,Sha(f.a),Sha(f.b),"synthetic-pair-registered",geometry,Clock::now()+std::chrono::seconds(900)};
    q.fixed_composite_registration=StillReviewRequest::FixedCompositeRegistration{&s.loaded.condition,&s.loaded.profile,s.loaded.file_sha256,threads};
    return q;
}
struct Outcome {StillReviewResult result;SavedStillReview saved;std::string text;double seconds{};};
Outcome Run(const StillReviewRequest& request){
    Outcome o;const auto started=Clock::now();
    o.result=WriteUncalibratedStillReview(request);o.seconds=Since(started);
    o.text=ReadText(o.result.report);
    // The registered members alone first, so that a refusal says which check refused.
    if(request.fixed_composite_registration)try{(void)ReadFixedCompositeRegistrationReport(o.text,kHash);}
    catch(const std::exception& error){std::cerr<<"  the v8 reader refused the written report ("<<o.text.size()<<" bytes): "<<error.what()<<std::endl;}
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
    CHECK(o.saved.fixed_composite_registration.has_value() && !o.saved.fixed_composite_lens && o.text.find("\"a0.uncalibrated-still-review.v8\"")!=std::string::npos);
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
    REJECTED("a0.uncalibrated-still-review.v8","a0.uncalibrated-still-review.v7");
    REJECTED("a0.uncalibrated-still-review.v8","a0.uncalibrated-still-review.v9");
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
        std::cout<<"checks="<<checks<<" failures="<<failures<<" seconds="<<Since(started)<<"\n";
        std::error_code ignored;fs::remove_all(root,ignored);
        return failures?1:0;
    }catch(const std::exception& error){
        std::cerr<<"exception: "<<error.what()<<"\n";
        return 2;
    }
}
