// The JPEG path of the lens-aware fixed composite (#405 W4): a v2 condition and its profile
// compose a two-camera pair into the condition's fixed raster, the per-capture scale goes through
// the real tile estimator of #365, and the v7 report records it and reads back strictly.
//
// Everything is synthetic and camera free. The two images are full frames (7360 x 4912, what the
// profile's camera model fixes) written as JPEG files: a drawing-like texture (similarity_scene.hpp)
// laid on the document plane and seen through the profile's own mappings, with camera B showing it
// scaled by a known k about the correction centre P (the truth of fixed_scale_correction_v2_tests.cpp).
// Only the strip around the overlap carries the texture; the rest of each frame is flat.
//
// Fixture rig (10 pixels per mm, the cameras side by side): A is u = 10 X + 100, v = 10 Y + 100;
// B is u = 10 X - 5660, v = 10 Y + 100. A sees X in [-10, 726), B in [566, 1302): 160 mm of overlap
// (1600 pixels) as the tile estimator, which looks for A left and B right, expects. The "turned"
// variant is the same rig mounted a half turn: the stored frames are the half-turned ones, the
// profile maps to them, and the condition says rotated-180.
//
// Argument 1 is the shared rig profile v2 fixture folder. "--measure" runs the full-size timing
// (output as fine as the captures) and prints the numbers; it asserts nothing.
#include "a0/m2/experimental/still_review.hpp"
#include "a0/m2/experimental/fixed_composite_selection.hpp"
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/m2/experimental/fixed_scale_correction_v2.hpp"
#include "a0/m2/experimental/fixed_scale_correction.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/optical_planner.hpp"
#include "a0/phase0/experimental/fixed_composite_lens_text.hpp"
#include "a0/phase0/experimental/fixed_composite_preview.hpp"
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
template<class Call> bool Throws(Call call){
    try{call();}catch(const std::exception&){return true;}
    return false;
}
#define REJECT(expr) Check(Throws([&]{(void)(expr);}),"rejects: " #expr,__LINE__)
bool Near(double a,double b,double tolerance){return std::abs(a-b)<=tolerance;}
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

// ---- rig profiles ----
enum class Variant {plain,lens,turned};
MountOrientation OrientationOf(Variant v){return v==Variant::turned?MountOrientation::rotated_180:MountOrientation::normal;}
std::string Num(double v){
    char text[48];
    if(v==std::floor(v))std::snprintf(text,sizeof text,"%.1f",v);else std::snprintf(text,sizeof text,"%.17g",v);
    return text;
}
// documentToImage of the fixture file, in its own layout, replaced for the n-th camera.
std::string WithMatrix(const std::string& text,int camera,const std::array<double,6>& m){
    size_t at=0;
    for(int i=0;i<=camera;++i){at=text.find("\"documentToImage\": [",at);if(at==std::string::npos)throw std::runtime_error("fixture has no matrix");if(i<camera)at+=10;}
    const size_t open=text.find('[',at);
    int depth=0;size_t close=open;
    for(;close<text.size();++close){if(text[close]=='[')++depth;else if(text[close]==']'&&--depth==0)break;}
    std::string body="[\n";
    for(int row=0;row<3;++row){
        const double r[3]{row<2?m[row*3]:0.0,row<2?m[row*3+1]:0.0,row<2?m[row*3+2]:1.0};
        body+="          [\n            "+Num(r[0])+",\n            "+Num(r[1])+",\n            "+Num(r[2])+"\n          ]"+(row<2?",\n":"\n");
    }
    body+="        ]";
    return text.substr(0,open)+body+text.substr(close+1);
}
std::string ProfileText(const std::string& base,Variant v){
    std::string t=base;
    if(v==Variant::turned){
        // The same rig mounted a half turn: stored pixel = (7359 - u, 4911 - v) of the upright one.
        t=WithMatrix(t,0,{-10,0,7359-100.0,0,-10,4911-100.0});
        t=WithMatrix(t,1,{-10,0,7359+5660.0,0,-10,4911-100.0});
    }else{
        t=WithMatrix(t,0,{10,0,100,0,10,100});
        t=WithMatrix(t,1,{10,0,-5660,0,10,100});
    }
    if(v!=Variant::plain){
        t=ReplaceAll(t,"\"k1\": 0.0","\"k1\": -0.05");
        t=ReplaceAll(t,"\"k2\": 0.0","\"k2\": 0.02");
        t=ReplaceAll(t,"\"p1\": 0.0","\"p1\": 0.0005");
    }
    return t;
}

render::OutputRaster Raster(long long left,long long top,long long right,long long bottom,unsigned dpi){
    const auto pixels=[&](long long um){return static_cast<std::uint32_t>((um*static_cast<long long>(dpi)+25399)/25400);};
    return render::OutputRaster{render::DocumentRegionUm{left,top,right,bottom},dpi,pixels(right-left),pixels(bottom-top)};
}
// 18 mm x 5 mm inside the strip, 0.1 mm pixels: A only, the overlap, and B only. Seam column 900 is X = 650.
render::OutputRaster SmallRaster(){return Raster(560000,10000,740000,60000,254);}
// The whole sheet width at the captures' own fineness (10 pixels per mm), for the timing.
render::OutputRaster FullRaster(){return Raster(0,10000,1180000,460000,254);}
constexpr std::uint32_t kSmallSeam=900;

// ---- the world: a drawing laid on the document plane ----
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

struct Truth {double scale{1};render::Point shift{0,0};};

// Fills columns [u0,u1) of a frame with the world as camera `camera` sees it, B's through `truth`
// about `center`. The document point of a pixel is solved on a grid of 8 pixels and interpolated.
void FillStrip(render::BgrImage& image,const render::CameraProjection& camera,unsigned u0,unsigned u1,const World& world,
    const Truth& truth,render::Point center,unsigned threads=4){
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
                const double X=(p00.x*(1-fx)+p10.x*fx)*(1-fy)+(p01.x*(1-fx)+p11.x*fx)*fy,Y=(p00.y*(1-fx)+p10.y*fx)*(1-fy)+(p01.y*(1-fx)+p11.y*fx)*fy;
                const double wx=center.x+truth.scale*(X-center.x)+truth.shift.x,wy=center.y+truth.scale*(Y-center.y)+truth.shift.y;
                similarity_scene::Put(out,world.At(wx,wy));
            }
        }
    };
    std::vector<std::thread> pool;
    for(unsigned t=0;t<threads;++t)pool.emplace_back(rows,kHeight*t/threads,kHeight*(t+1)/threads);
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
render::BgrImage Flat(unsigned w,unsigned h,std::uint8_t value){return {w,h,std::vector<std::uint8_t>(static_cast<size_t>(w)*h*3,value)};}

// ---- a rig on disk: settings folder with the profile and the condition, and a pair of frames ----
struct Settings {
    fs::path folder;
    LoadedFixedCompositeConditionV2 loaded;
};
Settings MakeSettings(const fs::path& folder,const std::string& profile,MountOrientation orientation,const render::OutputRaster& raster,double limit,
    std::uint32_t seam,FixedSeamOrigin origin,bool creation_check=true){
    fs::create_directories(folder);
    const auto name=CopyFixedCompositeProfile(folder,profile,kHash);
    FixedCompositeV2Request request;
    request.profile_file=name;request.profile_bytes=profile;request.orientation=orientation;request.raster=raster;
    request.scale_auto_correction_max_percent=limit;request.seam_column=seam;request.seam_origin=origin;
    // A condition that skips the creation check can hold a seam that a correction later pushes out of the overlap.
    const auto condition=MakeFixedCompositeConditionV2(request,kCreated,kHash,creation_check?MakeFixedCompositeCreationCheck():FixedCompositeCreationCheck([](const FixedCompositeConditionV2&,const FixedCompositeProfileBinding&){}));
    const auto file=folder/kFixedCompositeConditionName;
    WriteFixedCompositeConditionV2(file,condition);
    const auto selection=SelectFixedComposite(file,kHash);
    if(!selection.IsV2())throw std::runtime_error("the written condition did not come back as v2");
    return {folder,std::get<LoadedFixedCompositeConditionV2>(*selection.loaded)};
}

struct Frames {fs::path a,b,root;};
Frames MakeFrames(const fs::path& root,const std::string& name,const render::BgrImage& a,const render::BgrImage& b){
    const auto dir=root/name;fs::create_directories(dir/"CAM-A");fs::create_directories(dir/"CAM-B");
    WriteJpeg(dir/"CAM-A/original.jpg",a);WriteJpeg(dir/"CAM-B/original.jpg",b);
    return {dir/"CAM-A/original.jpg",dir/"CAM-B/original.jpg",dir};
}
// The textured pair: A as the profile maps it, B scaled by `truth` about the correction centre.
Frames TexturedFrames(const fs::path& root,const std::string& name,const Settings& s,Variant variant,const World& world,const Truth& truth){
    const auto& p=s.loaded.profile.parameters;
    const auto center=FixedCompositeScaleCenterMm(p.camera_b);
    auto a=Flat(kWidth,kHeight,128),b=Flat(kWidth,kHeight,128);
    const bool turned=variant==Variant::turned;
    FillStrip(a,p.camera_a,turned?0:5560,turned?1800:kWidth,world,Truth{},center);
    FillStrip(b,p.camera_b,turned?5560:0,turned?kWidth:1800,world,truth,center);
    return MakeFrames(root,name,a,b);
}

StillReviewRequest RequestFor(const Frames& f,const char* output,const Settings& s,const FixedPolicyLimit& policy,const std::string& pair_id="synthetic-pair-lens"){
    PreviewReviewGeometry geometry{460,307,229,3,11,s.loaded.condition.orientation};
    StillReviewRequest q{f.a,f.b,f.root/output,Sha(f.a),Sha(f.b),pair_id,geometry,Clock::now()+std::chrono::seconds(900)};
    q.fixed_composite_lens=StillReviewRequest::FixedCompositeLens{&s.loaded.condition,&s.loaded.profile,s.loaded.file_sha256,policy};
    return q;
}
const FixedPolicyLimit kSamePolicy=ComparePolicyLimit(1,1.0,FixedPolicyCheck::policy_absent);

struct Outcome {StillReviewResult result;SavedStillReview saved;std::string text;double seconds{};};
Outcome Run(const StillReviewRequest& request){
    Outcome o;const auto started=Clock::now();
    o.result=WriteUncalibratedStillReview(request);o.seconds=Since(started);
    o.saved=LoadSavedStillReview(o.result.report);o.text=ReadText(o.result.report);
    return o;
}

// Mean absolute difference of the saved composite from the world over the columns [from,to) of the small raster.
// With the scale right, B shows the world displaced by its own shift (the scale cannot remove a shift), so the
// expected picture is the world at X + shift; pass B's shift for B's side and 0 for A's.
double ErrorAgainstWorld(const Outcome& o,const World& world,unsigned from,unsigned to,render::Point shift){
    const auto image=LoadStillReviewImage(o.saved);
    const auto& raster=o.saved.fixed_composite_lens->raster;
    const double pitch=25.4/raster.dpi,left=raster.region_um.left/1000.0,top=raster.region_um.top/1000.0;
    double sum=0;size_t n=0;
    for(unsigned j=40;j+40<image.height;++j)for(unsigned i=from;i<to;++i){
        const double expected=world.At(left+(i+0.5)*pitch+shift.x,top+(j+0.5)*pitch+shift.y);
        sum+=std::abs(image.bgr[(static_cast<size_t>(j)*image.width+i)*3+1]-expected);++n;
    }
    return sum/n;
}

std::string base_profile;
fs::path root;

// ---- selection: which condition is in force, and why not ----
void Selection(){
    std::cout<<"selection\n";
    const auto folder=root/"selection";
    const auto profile=ProfileText(base_profile,Variant::lens);
    const auto s=MakeSettings(folder,profile,MountOrientation::normal,SmallRaster(),1,kSmallSeam,FixedSeamOrigin::operator_set);
    const auto file=folder/kFixedCompositeConditionName;
    // v2 and the file it names.
    {
        const auto selection=SelectFixedComposite(file,kHash);
        CHECK(selection.IsV2() && selection.loaded && selection.detail.empty());
        const auto& v2=std::get<LoadedFixedCompositeConditionV2>(*selection.loaded);
        CHECK(v2.file_sha256==Sha(file) && v2.profile.profile_file_sha256==s.loaded.condition.source.profile_file_sha256 && v2.condition.seam.column==kSmallSeam);
    }
    // No file is not an error; it is no condition.
    {
        const auto none=SelectFixedComposite(folder/"absent.json",kHash);
        CHECK(!none.loaded && none.reason==FixedCompositeSkip::condition_absent && none.detail.empty());
    }
    // Damaged, wrong schema, extra member.
    const auto text=ReadText(file);
    const auto broken=[&](const std::string& content,FixedCompositeSkip expected,const char* what){
        const auto f=folder/"broken.json";WriteText(f,content);
        const auto selection=SelectFixedComposite(f,kHash);
        Check(!selection.loaded && selection.reason==expected && !selection.detail.empty(),what,__LINE__);
        fs::remove(f);
    };
    broken("{",FixedCompositeSkip::condition_invalid,"damaged JSON");
    broken(ReplaceAll(text,"fixed-composite-condition.v2","fixed-composite-condition.v9"),FixedCompositeSkip::condition_invalid,"unknown schema");
    broken(ReplaceAll(text,"\"approval\":\"unapproved\"","\"approval\":\"approved\""),FixedCompositeSkip::condition_invalid,"approved");
    broken(ReplaceAll(text,"\"seam\":{","\"extra\":0,\"seam\":{"),FixedCompositeSkip::condition_invalid,"unknown member");
    // The profile: missing, replaced by another valid profile, truncated, not a regular file.
    const auto profile_path=folder/s.loaded.condition.source.profile_file;
    const auto original_profile=ReadText(profile_path);
    fs::rename(profile_path,folder/"moved-away.json");
    {const auto sel=SelectFixedComposite(file,kHash);CHECK(!sel.loaded && sel.reason==FixedCompositeSkip::profile_absent);}
    WriteText(profile_path,ProfileText(base_profile,Variant::plain));
    {const auto sel=SelectFixedComposite(file,kHash);CHECK(!sel.loaded && sel.reason==FixedCompositeSkip::profile_differs);}
    WriteText(profile_path,original_profile.substr(0,original_profile.size()/2));
    {const auto sel=SelectFixedComposite(file,kHash);CHECK(!sel.loaded && sel.reason==FixedCompositeSkip::profile_differs);}
    fs::remove(profile_path);fs::create_directory(profile_path);
    {const auto sel=SelectFixedComposite(file,kHash);CHECK(!sel.loaded && sel.reason==FixedCompositeSkip::profile_differs);}
    fs::remove(profile_path);
    WriteText(profile_path,original_profile);
    {const auto sel=SelectFixedComposite(file,kHash);CHECK(sel.IsV2());} // the control: the same bytes again
    // A profile that is there but cannot be read (another handle holds it exclusively) is the profile's problem, in the
    // same words for the JPEG path and the live C. The kind is the typed error's.
    {
        const auto kind_of=[&]()->std::optional<FixedCompositeProfileError::Kind>{
            try{(void)LoadFixedCompositeConditionAny(file,kHash);}
            catch(const FixedCompositeProfileError& error){return error.GetKind();}
            catch(const std::exception&){}
            return std::nullopt;
        };
        CHECK(!kind_of());
        const HANDLE held=CreateFileW(profile_path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        CHECK(held!=INVALID_HANDLE_VALUE);
        const auto locked=kind_of();
        const auto sel=SelectFixedComposite(file,kHash);
        CloseHandle(held);
        CHECK(locked && *locked==FixedCompositeProfileError::Kind::differs && !sel.loaded && sel.reason==FixedCompositeSkip::profile_differs);
        fs::rename(profile_path,folder/"moved-away-2.json");
        const auto missing=kind_of();
        CHECK(missing && *missing==FixedCompositeProfileError::Kind::missing);
        fs::rename(folder/"moved-away-2.json",profile_path);
        CHECK(!kind_of());
    }
    // The range of the limit is the creating side's: below 100 percent.
    {
        const auto limited=[&](const char* value){return ReplaceAll(text,"\"scaleAutoCorrectionMaxPercent\":1,",std::string("\"scaleAutoCorrectionMaxPercent\":")+value+",");};
        CHECK(!Throws([&]{(void)ParseFixedCompositeConditionV2(limited("99.5"));}));
        REJECT(ParseFixedCompositeConditionV2(limited("100")));
        REJECT(ParseFixedCompositeConditionV2(limited("250")));
        broken(limited("100"),FixedCompositeSkip::condition_invalid,"limit of 100 percent");
    }
    // A v1 file in the same name still reads as v1 and is untouched by any of this.
    {
        const auto v1_folder=root/"selection-v1";fs::create_directories(v1_folder);
        FixedCompositeCondition c;c.created_utc="2026-11-04T12:00:00Z";c.source.review_report_sha256=std::string(64,'a');c.source.pair_id="pair-1";
        c.source_width=640;c.source_height=480;c.mapping.translation_x=300;c.mapping.translation_y=3;
        const auto p=SimilarityReviewGeometry(300,3,0,1,640,480);c.mapping.crop=p.crop;
        const auto plan=render::PlanPairCanvas(640,480,640,480,p);c.output_width=plan.output_width;c.output_height=plan.output_height;
        c.scale_auto_correction_max_percent=1;c.seam={ProvisionalSeamColumn(p,640,480),FixedSeamOrigin::band_center_provisional};
        WriteFixedCompositeCondition(v1_folder/kFixedCompositeConditionName,c);
        const auto sel=SelectFixedComposite(v1_folder/kFixedCompositeConditionName,kHash);
        CHECK(sel.loaded && !sel.IsV2() && std::holds_alternative<LoadedFixedCompositeCondition>(*sel.loaded));
    }
}

// ---- refusals that need no real frames ----
void Refusals(const Settings& s,const Frames& small_frames){
    std::cout<<"refusals\n";
    const auto before_a=ReadBytes(small_frames.a),before_b=ReadBytes(small_frames.b);
    const auto skip_of=[&](StillReviewRequest q,const char* output)->std::optional<FixedCompositeSkip>{
        q.output_directory=small_frames.root/output;
        try{(void)WriteUncalibratedStillReview(q);}
        catch(const FixedCompositeUnavailable& error){
            // Nothing but the originals is left behind (and the directory was never created).
            if(fs::exists(q.output_directory))return std::nullopt;
            return error.Skip();
        }
        return std::nullopt;
    };
    // Frames that are not the profile's frame: refused before anything is written.
    auto q=RequestFor(small_frames,"size",s,kSamePolicy);
    CHECK(skip_of(q,"size")==FixedCompositeSkip::size_differs);
    // The orientation of the capture is not the one the condition was made in.
    q=RequestFor(small_frames,"orientation",s,kSamePolicy);q.geometry.orientation=MountOrientation::rotated_180;
    CHECK(skip_of(q,"orientation")==FixedCompositeSkip::orientation_differs);
    // A profile binding that is not the one the condition names.
    {
        const auto other=MakeSettings(root/"refusal-other",ProfileText(base_profile,Variant::plain),MountOrientation::normal,SmallRaster(),1,kSmallSeam,FixedSeamOrigin::operator_set);
        q=RequestFor(small_frames,"binding",s,kSamePolicy);
        q.fixed_composite_lens->profile=&other.loaded.profile;
        CHECK(skip_of(q,"binding")==FixedCompositeSkip::profile_differs);
        auto raster_changed=s.loaded.profile;
        raster_changed.parameters.raster.dpi=127;
        q=RequestFor(small_frames,"raster",s,kSamePolicy);q.fixed_composite_lens->profile=&raster_changed;
        CHECK(skip_of(q,"raster")==FixedCompositeSkip::profile_differs);
    }
    // A condition that fails its own validation.
    {
        auto broken=s.loaded.condition;broken.seam.column=broken.raster.width_pixels;
        q=RequestFor(small_frames,"invalid",s,kSamePolicy);q.fixed_composite_lens->condition=&broken;
        CHECK(skip_of(q,"invalid")==FixedCompositeSkip::condition_invalid);
    }
    // Exclusive with every other geometry source, and a condition hash is required.
    {
        q=RequestFor(small_frames,"exclusive",s,kSamePolicy);q.blend=BlendMethod::seam_cut;
        REJECT(WriteUncalibratedStillReview(q));
        q=RequestFor(small_frames,"exclusive2",s,kSamePolicy);q.automatic_alignment=StillReviewRequest::AutomaticAlignment{};
        REJECT(WriteUncalibratedStillReview(q));
        q=RequestFor(small_frames,"exclusive3",s,kSamePolicy);q.fixed_composite=StillReviewRequest::FixedComposite{};
        REJECT(WriteUncalibratedStillReview(q));
        q=RequestFor(small_frames,"nohash",s,kSamePolicy);q.fixed_composite_lens->condition_sha256="short";
        REJECT(WriteUncalibratedStillReview(q));
        q=RequestFor(small_frames,"noprofile",s,kSamePolicy);q.fixed_composite_lens->profile=nullptr;
        REJECT(WriteUncalibratedStillReview(q));
        CHECK(!fs::exists(small_frames.root/"exclusive") && !fs::exists(small_frames.root/"nohash"));
    }
    CHECK(ReadBytes(small_frames.a)==before_a && ReadBytes(small_frames.b)==before_b);
}

// ---- one capture, end to end ----
struct Case {const char* name;Variant variant;Truth truth;std::uint64_t seed;};
struct Rig {
    Case c;
    Settings settings;
    Frames frames;
    Outcome outcome;
};
Rig RunCase(const Case& c,const World& world,double limit=1){
    std::cout<<"capture "<<c.name<<'\n';
    Rig rig{c,MakeSettings(root/(std::string("settings-")+c.name),ProfileText(base_profile,c.variant),OrientationOf(c.variant),SmallRaster(),limit,kSmallSeam,FixedSeamOrigin::operator_set),{},{}};
    rig.frames=TexturedFrames(root,c.name,rig.settings,c.variant,world,c.truth);
    const auto before_a=ReadBytes(rig.frames.a),before_b=ReadBytes(rig.frames.b);
    rig.outcome=Run(RequestFor(rig.frames,"review",rig.settings,ComparePolicyLimit(limit,limit,FixedPolicyCheck::policy_absent)));
    CHECK(ReadBytes(rig.frames.a)==before_a && ReadBytes(rig.frames.b)==before_b); // the originals are untouched
    return rig;
}

// A rig of the three kinds recovers the scale it was given, through the real tile estimator.
void Recovery(const Rig& rig,const World& world,unsigned index){
    const auto& c=rig.c;const auto& o=rig.outcome;const auto& s=rig.settings;
    std::cout<<"recovery "<<c.name<<'\n';
    CHECK(o.saved.fixed_composite_lens && o.result.fixed_composite_lens && !o.saved.fixed_composite && !o.saved.automatic);
    const auto& r=*o.saved.fixed_composite_lens;const auto& k=r.scale;
    std::cout<<"  truth="<<c.truth.scale<<" estimated="<<(k.estimated_scale?*k.estimated_scale:0.0)<<" status="<<FixedScaleStatusCode(k.status)<<" used="<<k.used_tiles<<" rows="<<k.first_used_row<<".."<<k.last_used_row
        <<" residual_px="<<k.residual_output_pixels.maximum<<" shift_mm="<<k.unexplained_shift_mm.x<<","<<k.unexplained_shift_mm.y<<" tiles="<<r.tiles_agreed<<'/'<<r.tiles_matched<<'/'<<r.tiles_searched
        <<" estimate_seconds="<<o.result.alignment_milliseconds/1000<<" render_seconds="<<o.result.blend_milliseconds/1000<<" total_seconds="<<o.seconds<<'\n';
    CHECK(k.status==FixedScaleStatus::applied && k.reason==FixedScaleReason::none);
    CHECK(k.estimated_scale && Near(*k.estimated_scale,c.truth.scale,6e-4) && k.applied_scale==*k.estimated_scale);
    CHECK(k.used_tiles>=8 && k.used_rows>=4 && k.last_used_row>k.first_used_row);
    // The injected shift comes back as the unexplained shift (mm), not as a leak into the scale.
    CHECK(Near(k.unexplained_shift_mm.x,c.truth.shift.x,0.05) && Near(k.unexplained_shift_mm.y,c.truth.shift.y,0.05));
    CHECK(Near(k.unexplained_shift_output_pixels.x*0.1,k.unexplained_shift_mm.x,1e-9));
    CHECK(o.saved.width==s.loaded.condition.raster.width_pixels && o.saved.height==s.loaded.condition.raster.height_pixels);
    CHECK(r.seam_column==kSmallSeam && r.seam_origin=="operator" && o.saved.blend && o.saved.blend->fixed_seam && o.saved.blend->seam_x.front()==kSmallSeam);
    CHECK(r.condition_sha256==s.loaded.file_sha256 && r.profile_file_sha256==s.loaded.profile.profile_file_sha256 && r.profile_fingerprint_sha256==s.loaded.profile.profile_fingerprint_sha256);
    CHECK(r.profile_id==s.loaded.profile.profile_id && r.profile_id.has_value() && r.profile_file==s.loaded.condition.source.profile_file);
    const auto center=FixedCompositeScaleCenterMm(s.loaded.profile.parameters.camera_b);
    CHECK(k.center_mm.x==center.x && k.center_mm.y==center.y);
    VerifySavedFixedCompositeLens(o.saved,s.loaded.profile);
    const auto orientation=OrientationOf(c.variant);
    CHECK(o.saved.geometry.orientation==orientation && (orientation==MountOrientation::rotated_180)==(o.text.find("\"mountOrientation\":\"rotated-180\"")!=std::string::npos));
    CHECK(o.text.find("\"schema\":\"a0.uncalibrated-still-review.v7\"")!=std::string::npos && o.text.find("\"translation\"")==std::string::npos && o.text.find("\"crop\"")==std::string::npos &&
        o.text.find("\"productQualityAccepted\":false,\"physicalScaleVerified\":false,\"liveToStillFieldOfViewVerified\":false")!=std::string::npos);
    // What was written is what is read.
    const auto& w=*o.result.fixed_composite_lens;
    CHECK(w.condition_sha256==r.condition_sha256 && w.seam_column==r.seam_column && w.raster.dpi==r.raster.dpi && w.raster.width_pixels==r.raster.width_pixels);
    CHECK(w.scale.status==k.status && w.scale.reason==k.reason && w.scale.estimated_scale==k.estimated_scale && w.scale.difference_percent==k.difference_percent && w.scale.applied_scale==k.applied_scale);
    CHECK(w.scale.used_tiles==k.used_tiles && w.scale.used_rows==k.used_rows && w.scale.first_used_row==k.first_used_row && w.scale.last_used_row==k.last_used_row);
    CHECK(w.scale.residual_mm.rms==k.residual_mm.rms && w.scale.residual_output_pixels.maximum==k.residual_output_pixels.maximum);
    CHECK(w.scale.unexplained_shift_mm.x==k.unexplained_shift_mm.x && w.scale.unexplained_shift_output_pixels.y==k.unexplained_shift_output_pixels.y);
    CHECK(w.scale.center_output_pixels.x==k.center_output_pixels.x && w.estimate_status==r.estimate_status && w.tiles_agreed==r.tiles_agreed);
    // The composite is the world: closer to it on B's side than the A side is only by resampling,
    // and much closer than the same pair drawn without the correction.
    const double corrected=ErrorAgainstWorld(o,world,kSmallSeam+8,kSmallSeam+120,c.truth.shift),a_side=ErrorAgainstWorld(o,world,kSmallSeam-120,kSmallSeam-8,{0,0});
    std::cout<<"  mean abs difference from the world (levels): B side="<<corrected<<" A side="<<a_side<<'\n';
    CHECK(corrected<a_side+3);
    if(index==1){
        // The same pair with the correction off (limit 0): the scale is estimated and not applied.
        const auto off=MakeSettings(root/"settings-off",ProfileText(base_profile,c.variant),orientation,SmallRaster(),0,kSmallSeam,FixedSeamOrigin::operator_set);
        const auto without=Run(RequestFor(rig.frames,"review-off",off,ComparePolicyLimit(0,0.0,FixedPolicyCheck::policy_absent)));
        const auto& u=*without.saved.fixed_composite_lens;
        CHECK(u.scale.status==FixedScaleStatus::exceeded && u.scale.applied_scale==1 && u.scale.estimated_scale && Near(*u.scale.estimated_scale,c.truth.scale,6e-4));
        const double uncorrected=ErrorAgainstWorld(without,world,kSmallSeam+8,kSmallSeam+120,c.truth.shift);
        std::cout<<"  uncorrected B side="<<uncorrected<<'\n';
        CHECK(uncorrected>10*corrected && uncorrected>10);
        // The mapping and the seam are the same with or without the correction.
        CHECK(u.raster.width_pixels==r.raster.width_pixels && u.seam_column==r.seam_column && u.scale.center_mm.x==k.center_mm.x);
    }
}

// Inside the limit, exactly at it, and beyond it, from one pair. The estimate is deterministic.
void Limits(const Rig& rig){
    std::cout<<"limits\n";
    const auto& w=rig.outcome.saved.fixed_composite_lens->scale;
    CHECK(w.status==FixedScaleStatus::applied && w.difference_percent);
    const double difference=*w.difference_percent;
    const auto profile=ProfileText(base_profile,rig.c.variant);
    const auto run_with=[&](const char* name,double limit){
        const auto s=MakeSettings(root/(std::string("settings-limit-")+name),profile,OrientationOf(rig.c.variant),SmallRaster(),limit,kSmallSeam,FixedSeamOrigin::operator_set);
        return Run(RequestFor(rig.frames,name,s,ComparePolicyLimit(limit,limit,FixedPolicyCheck::policy_absent)));
    };
    const auto at=run_with("exact",difference);
    const auto& a=at.saved.fixed_composite_lens->scale;
    CHECK(a.status==FixedScaleStatus::applied && a.estimated_scale==w.estimated_scale && a.applied_scale==*w.estimated_scale && a.limit_percent==difference);
    const double below=std::nextafter(difference,0.0);
    const auto under=run_with("under",below);
    const auto& b=under.saved.fixed_composite_lens->scale;
    CHECK(b.status==FixedScaleStatus::exceeded && b.applied_scale==1 && b.estimated_scale==w.estimated_scale && b.limit_percent==below && b.reason==FixedScaleReason::none);
    CHECK(Run(RequestFor(rig.frames,"under-text",rig.settings,kSamePolicy)).saved.fixed_composite_lens->scale.status==FixedScaleStatus::applied); // inside: the control
    std::cout<<"  difference="<<difference<<"%\n";
}

// No texture to measure: nothing is estimated and nothing is corrected, and the composite is still made.
void NotEstimated(){
    std::cout<<"not estimated\n";
    const auto s=MakeSettings(root/"settings-flat",ProfileText(base_profile,Variant::lens),MountOrientation::normal,SmallRaster(),1,kSmallSeam,FixedSeamOrigin::operator_set);
    const auto flat=MakeFrames(root,"flat",Flat(kWidth,kHeight,128),Flat(kWidth,kHeight,128));
    const auto o=Run(RequestFor(flat,"review",s,kSamePolicy));
    const auto& r=*o.saved.fixed_composite_lens;const auto& k=r.scale;
    std::cout<<"  reason="<<FixedScaleReasonCode(k.reason)<<" estimator="<<OverlapEstimateStatusCode(r.estimate_status)<<'/'<<SimilarityUndeterminedReasonCode(r.estimate_reason)<<" tiles="<<r.tiles_agreed<<'/'<<r.tiles_matched<<'/'<<r.tiles_searched<<'\n';
    CHECK(k.status==FixedScaleStatus::not_estimated && k.reason!=FixedScaleReason::none && k.applied_scale==1 && !k.estimated_scale && !k.difference_percent && k.used_tiles==0);
    CHECK(o.saved.width==s.loaded.condition.raster.width_pixels && fs::exists(o.saved.jpeg));
    CHECK(o.text.find("\"residual\":null,\"unexplainedShift\":null")!=std::string::npos);
    VerifySavedFixedCompositeLens(o.saved,s.loaded.profile);
    CHECK(o.text.find(std::string(FixedScaleNotice(k.status)))!=std::string::npos);
}

// A correction that would take the seam out of the overlap is not applied. A condition made by the creating
// entrance cannot hold such a seam; this one skips the creation check, to reach the guard.
void SeamAfterCorrection(const Rig& lens_rig){
    std::cout<<"seam after correction\n";
    // The overlap at scale 1 begins at band.first. A seam just inside it is drawn at scale 1; B's frame edge moves right
    // when the scale is below 1 (0.4 % about P, ~370 mm away: ~15 columns), which takes the seam out.
    const auto band=FixedCompositeBandAtUnitScale(lens_rig.settings.loaded.profile.parameters);
    std::cout<<"  band at scale 1: "<<band.first<<".."<<band.last_exclusive<<"\n";
    CHECK(band.first>=20 && band.first+8<kSmallSeam);
    const auto inside=band.first+4;
    const auto s=MakeSettings(root/"settings-seam-outside",ProfileText(base_profile,Variant::lens),MountOrientation::normal,SmallRaster(),1,inside,FixedSeamOrigin::operator_set,false);
    const auto o=Run(RequestFor(lens_rig.frames,"review-seam",s,kSamePolicy));
    const auto& k=o.saved.fixed_composite_lens->scale;
    std::cout<<"  estimated="<<(k.estimated_scale?*k.estimated_scale:0.0)<<" status="<<FixedScaleStatusCode(k.status)<<'\n';
    CHECK(k.status==FixedScaleStatus::seam_outside_overlap && k.reason==FixedScaleReason::none && k.applied_scale==1 && k.estimated_scale && Near(*k.estimated_scale,lens_rig.c.truth.scale,6e-4));
    CHECK(k.difference_percent && *k.difference_percent<=k.limit_percent && o.saved.fixed_composite_lens->seam_column==inside);
    // The composite is still made, with B at scale 1.
    CHECK(o.saved.width==s.loaded.condition.raster.width_pixels && o.saved.blend && o.saved.blend->seam_x.front()==inside);
    // At scale 1 the seam is outside as well when it is further left: refused before any estimate.
    const auto edge=MakeSettings(root/"settings-seam-edge",ProfileText(base_profile,Variant::lens),MountOrientation::normal,SmallRaster(),1,band.first-10,FixedSeamOrigin::operator_set,false);
    auto q=RequestFor(lens_rig.frames,"review-edge",edge,kSamePolicy);
    bool skipped=false;
    try{(void)WriteUncalibratedStillReview(q);}catch(const FixedCompositeUnavailable& error){skipped=error.Skip()==FixedCompositeSkip::seam_outside_overlap;}
    CHECK(skipped && !fs::exists(q.output_directory));
}

// ---- an output the cameras do not fully show (a condition edited by hand) ----
// Whether every output pixel of this raster is shown by A or B at scale k.
bool Covered(const render::DocumentRenderParameters& profile,const render::OutputRaster& raster,double k){
    auto parameters=profile;parameters.raster=raster;
    try{RequireFixedCompositeCoverage(ScaledDocumentMappedFixedSeamInput(parameters,k));}
    catch(const std::invalid_argument&){return false;}
    return true;
}
void Coverage(const Rig& lens_rig){
    std::cout<<"coverage\n";
    const auto profile_text=ProfileText(base_profile,Variant::lens);
    const auto& profile=lens_rig.settings.loaded.profile.parameters;
    const auto flat=MakeFrames(root,"flat-coverage",Flat(kWidth,kHeight,128),Flat(kWidth,kHeight,128));
    // (1) Not covered even at scale 1 (the region runs below what the cameras see). Made without the creation check.
    {
        const auto raster=Raster(560000,10000,740000,500000,254);
        CHECK(!Covered(profile,raster,1.0));
        const auto s=MakeSettings(root/"settings-uncovered",profile_text,MountOrientation::normal,raster,1,kSmallSeam,FixedSeamOrigin::operator_set,false);
        auto q=RequestFor(flat,"review-uncovered",s,kSamePolicy);
        const auto before_a=ReadBytes(flat.a),before_b=ReadBytes(flat.b);
        bool refused=false;
        try{(void)WriteUncalibratedStillReview(q);}
        catch(const FixedCompositeUnavailable& error){refused=error.Skip()==FixedCompositeSkip::size_differs;}
        // The way the capture takes it: composite skipped, originals kept, no output folder.
        CHECK(refused && !fs::exists(q.output_directory) && ReadBytes(flat.a)==before_a && ReadBytes(flat.b)==before_b);
        // The live C refuses the same condition for the same reason.
        namespace px=a0::phase0::experimental;
        const auto plan=px::PlanFixedPreviewV2(s.loaded,MountOrientation::normal,640,427);
        std::cout<<"  C: "<<(plan.skip?std::string(FixedCompositeSkipCode(*plan.skip)):std::string("drawn"))<<" "<<plan.detail<<std::endl;
        CHECK(plan.skip && *plan.skip==FixedCompositeSkip::size_differs);
        // The creating entrance refuses it, so only a hand-edited record gets here.
        REJECT(MakeSettings(root/"settings-uncovered-checked",profile_text,MountOrientation::normal,raster,1,kSmallSeam,FixedSeamOrigin::operator_set,true));
    }
    // (2) Covered at scale 1, not at the scale this pair measures: the scale is not applied and the status says why.
    {
        const double k=*lens_rig.outcome.saved.fixed_composite_lens->scale.estimated_scale;
        long long bottom=0;
        for(long long um=470000;um<=490000 && !bottom;um+=500){
            const auto raster=Raster(560000,10000,740000,um,254);
            if(Covered(profile,raster,1.0) && !Covered(profile,raster,k))bottom=um;
        }
        std::cout<<"  bottom of the region where scale 1 is covered and scale "<<k<<" is not: "<<bottom<<" um\n";
        CHECK(bottom!=0);
        const auto raster=Raster(560000,10000,740000,bottom,254);
        const auto s=MakeSettings(root/"settings-uncovered-scaled",profile_text,MountOrientation::normal,raster,1,kSmallSeam,FixedSeamOrigin::operator_set,false);
        const auto o=Run(RequestFor(lens_rig.frames,"review-uncovered-scaled",s,kSamePolicy));
        const auto& c=o.saved.fixed_composite_lens->scale;
        CHECK(c.status==FixedScaleStatus::output_uncovered && c.reason==FixedScaleReason::none && c.applied_scale==1 && c.estimated_scale && *c.estimated_scale==k);
        CHECK(o.saved.width==raster.width_pixels && o.saved.height==raster.height_pixels && fs::exists(o.saved.jpeg));
        CHECK(o.text.find("\"status\":\"output-uncovered\"")!=std::string::npos);
        VerifySavedFixedCompositeLens(o.saved,s.loaded.profile);
        namespace px=a0::phase0::experimental;
        CHECK(px::FixedCompositeLensLine(*o.saved.fixed_composite_lens).find(L"倍率は補正していません")!=std::wstring::npos);
    }
}

// The same condition on pictures with other content: the mapping and the seam do not follow the pictures.
std::string FixedPart(const std::string& text){
    const auto from=text.find("\"fixedCompositeLens\":{"),to=text.find("\"scaleCorrection\"");
    return from==std::string::npos||to==std::string::npos||to<from?std::string():text.substr(from,to-from);
}
void SameMapping(const Rig& lens_rig,const World& other_world){
    std::cout<<"same mapping, other content\n";
    const auto other=TexturedFrames(root,"other-content",lens_rig.settings,Variant::lens,other_world,{1.0,{0,0}});
    const auto o=Run(RequestFor(other,"review",lens_rig.settings,kSamePolicy));
    const auto& a=*lens_rig.outcome.saved.fixed_composite_lens;const auto& b=*o.saved.fixed_composite_lens;
    CHECK(!FixedPart(o.text).empty() && FixedPart(o.text)==FixedPart(lens_rig.outcome.text));
    CHECK(b.scale.center_mm.x==a.scale.center_mm.x && b.scale.center_mm.y==a.scale.center_mm.y && b.scale.center_output_pixels.x==a.scale.center_output_pixels.x);
    CHECK(o.saved.width==lens_rig.outcome.saved.width && o.saved.height==lens_rig.outcome.saved.height && b.seam_column==a.seam_column);
    // The scale is the capture's own: the truth here is 1, the other pair's was not.
    CHECK(b.scale.estimated_scale && Near(*b.scale.estimated_scale,1.0,6e-4) && b.scale.estimated_scale!=a.scale.estimated_scale);
    CHECK(o.saved.blend->seam_x==lens_rig.outcome.saved.blend->seam_x);
}

// ---- the report: derivable values are derived again, and a changed report is refused ----
void Tamper(const Rig& rig,const Settings& other_profile){
    std::cout<<"report\n";
    const auto& o=rig.outcome;
    const auto damaged=root/"damaged";fs::create_directories(damaged);
    fs::copy_file(o.saved.jpeg,damaged/"uncalibrated-review.jpg",fs::copy_options::overwrite_existing);
    const auto report=damaged/"uncalibrated-review.json";
    const auto loads=[&](const std::string& text){WriteText(report,text);try{(void)LoadSavedStillReview(report);}catch(const std::exception&){return false;}return true;};
    // The control: the report as written, copied beside its image, is accepted.
    CHECK(loads(o.text));
    const auto changed=[&](const std::string& from,const std::string& to){
        const auto at=o.text.find(from);
        if(at==std::string::npos||o.text.find(from,at+1)!=std::string::npos){Check(false,("token not unique: "+from).c_str(),__LINE__);return std::string(o.text);}
        auto t=o.text;t.replace(at,from.size(),to);return t;
    };
    const auto rejected=[&](const std::string& from,const std::string& to){
        const bool ok=!loads(changed(from,to));
        if(!ok)std::cerr<<"  accepted a changed report: "<<from<<" -> "<<to<<'\n';
        Check(ok,"a changed report is refused",__LINE__);
    };
    const auto& k=o.saved.fixed_composite_lens->scale;
    char text[64];
    rejected("a0.uncalibrated-still-review.v7","a0.uncalibrated-still-review.v6");
    rejected("a0.uncalibrated-still-review.v7","a0.uncalibrated-still-review.v8");
    rejected("\"productQualityAccepted\":false","\"productQualityAccepted\":true");
    rejected("\"geometryBasis\":\"fixed-composite-lens\"","\"geometryBasis\":\"fixed-composite\"");
    rejected("\"fixedCompositeLens\":{","\"fixedCompositeLens\":{\"extra\":0,");
    rejected("\"output\":{","\"translation\":{\"x\":1,\"y\":0},\"output\":{");
    rejected("\"status\":\"applied\"","\"status\":\"exceeded\"");
    rejected("\"status\":\"applied\"","\"status\":\"seam-outside-overlap\"");
    rejected("\"reason\":\"none\",\"limitPercent\"","\"reason\":\"too-few-tiles-near-fixed-mapping\",\"limitPercent\"");
    std::snprintf(text,sizeof text,"%.17g",*k.difference_percent);
    rejected(std::string("\"differencePercent\":")+text,"\"differencePercent\":0.5");
    rejected("\"dpi\":254","\"dpi\":127");
    rejected("\"widthPixels\":1800,\"heightPixels\":500},\"seam\"","\"widthPixels\":1799,\"heightPixels\":500},\"seam\"");
    rejected("\"seam\":{\"column\":900,","\"seam\":{\"column\":1800,");
    rejected("\"seam\":{\"column\":900,\"origin\":\"operator\"}","\"seam\":{\"column\":900,\"origin\":\"unknown\"}");
    rejected("\"method\":\"fixed-seam\"","\"method\":\"seam-cut\"");
    rejected("\"conditionPercent\":1,","\"conditionPercent\":2,");
    rejected("\"check\":\"same\"","\"check\":\"differs\"");
    rejected("\"sourceDimensions\":{\"width\":7360","\"sourceDimensions\":{\"width\":7359");
    rejected("\"estimator\":\"a0.overlap-similarity.v2\",\"status\":\"estimated-not-judged\"","\"estimator\":\"a0.overlap-similarity.v1\",\"status\":\"estimated-not-judged\"");
    rejected("\"profile\":{\"file\":\"","\"profile\":{\"file\":\"..\\\\");
    std::snprintf(text,sizeof text,"\"usedTiles\":%u",k.used_tiles);
    rejected(text,"\"usedTiles\":4000");
    // Values that are self-consistent cannot be told by the report alone; the profile tells them.
    const auto& r=*o.saved.fixed_composite_lens;
    const auto swapped=changed(r.profile_file_sha256,std::string(64,'1'));
    CHECK(loads(swapped));
    {
        auto record=LoadSavedStillReview(report);
        REJECT(VerifySavedFixedCompositeLens(record,rig.settings.loaded.profile));
        (void)LoadSavedStillReview(o.result.report);
    }
    CHECK(loads(changed("\"profileId\":\"","\"profileId\":\"x")));
    {const auto record=LoadSavedStillReview(report);REJECT(VerifySavedFixedCompositeLens(record,rig.settings.loaded.profile));}
    // Verification against the right profile passes; against another profile or a changed raster it does not.
    VerifySavedFixedCompositeLens(o.saved,rig.settings.loaded.profile);
    REJECT(VerifySavedFixedCompositeLens(o.saved,other_profile.loaded.profile));
    {auto profile=rig.settings.loaded.profile;profile.parameters.raster.dpi=127;REJECT(VerifySavedFixedCompositeLens(o.saved,profile));}
    {auto profile=rig.settings.loaded.profile;profile.profile_fingerprint_sha256=std::string(64,'2');REJECT(VerifySavedFixedCompositeLens(o.saved,profile));}
    // A v6 report has no lens record to verify.
    {SavedStillReview none;REJECT(VerifySavedFixedCompositeLens(none,rig.settings.loaded.profile));}
    // Files the report names are bound too: a changed image is refused.
    {
        auto bytes=ReadBytes(o.saved.jpeg);bytes[bytes.size()/2]^=1;
        WriteText(damaged/"uncalibrated-review.jpg",std::string(bytes.begin(),bytes.end()));
        CHECK(!loads(o.text));
        fs::copy_file(o.saved.jpeg,damaged/"uncalibrated-review.jpg",fs::copy_options::overwrite_existing);
        CHECK(loads(o.text));
    }
    // The tools that place B by a similarity refuse a lens-aware report.
    REJECT(SavedStillReviewRenderParameters(o.saved));
    REJECT(CompareStillReviewBlends(o.saved,root/"compare",400,300));
    REJECT(MakeFixedCompositeCondition(o.saved,kCreated,1));
    CHECK(!fs::exists(root/"compare-feather.jpg"));
}

// The texts of the inspector and the status line come from the read record.
void Texts(const Rig& rig){
    std::cout<<"texts\n";
    namespace px=a0::phase0::experimental;
    const auto& saved=rig.outcome.saved;const auto& f=*saved.fixed_composite_lens;
    const auto line=px::FixedCompositeLensLine(f),details=px::FixedCompositeLensDetails(saved);
    CHECK(line.find(L"校正の写像・未承認")!=std::wstring::npos && line.find(L"桃色の縦線")!=std::wstring::npos && line.find(L"補正しました")!=std::wstring::npos);
    CHECK(details.find(L"固定の合成条件（校正の写像・未承認）")!=std::wstring::npos && details.find(L"継ぎ目の列: "+std::to_wstring(f.seam_column))!=std::wstring::npos &&
        details.find(L"倍率の補正")!=std::wstring::npos && details.find(L"倍率で説明できなかったずれ")!=std::wstring::npos && details.find(L"撮影ごとに変わりません")!=std::wstring::npos &&
        details.find(L"補正の中心 P")!=std::wstring::npos && details.find(L"254 DPI")!=std::wstring::npos);
    CHECK(details.find(L"再Merge")!=std::wstring::npos);
}

// ---- the inspector opens the report: its own smoke (the seam line, the scale notes, no manual tools) ----
void RunInspector(const char* executable,const fs::path& report){
    std::cout<<"inspector"<<std::endl;
    std::wstring command=L"\""+fs::path(executable).wstring()+L"\" --report \""+report.wstring()+L"\" --smoke";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process))throw std::runtime_error("cannot start the inspector");
    const auto waited=WaitForSingleObject(process.hProcess,240000);
    DWORD code=1;
    if(waited==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&code);else TerminateProcess(process.hProcess,2);
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
    CHECK(waited==WAIT_OBJECT_0 && code==0);
}

// ---- time at the captures' own size (printed, not asserted) ----
void Measure(const World& world,Variant variant){
    std::cout<<"measure\n";
    const auto profile=ProfileText(base_profile,variant);
    const auto folder=root/"settings-full";
    auto step=Clock::now();
    const auto s=MakeSettings(folder,profile,OrientationOf(variant),FullRaster(),1,6500,FixedSeamOrigin::operator_set);
    std::cout<<"make_condition_seconds="<<Since(step)<<'\n';
    step=Clock::now();
    const auto selection=SelectFixedComposite(folder/kFixedCompositeConditionName,kHash);
    std::cout<<"read_condition_seconds="<<Since(step)<<" v2="<<selection.IsV2()<<'\n';
    step=Clock::now();
    const auto frames=TexturedFrames(root,"full",s,variant,world,{1.003,{0.1,-0.1}});
    std::cout<<"fixture_seconds="<<Since(step)<<'\n';
    const auto o=Run(RequestFor(frames,"review",s,kSamePolicy));
    const auto& r=*o.saved.fixed_composite_lens;
    std::cout<<"output="<<o.saved.width<<"x"<<o.saved.height<<" status="<<FixedScaleStatusCode(r.scale.status)<<" estimated="<<(r.scale.estimated_scale?*r.scale.estimated_scale:0.0)<<" used_tiles="<<r.scale.used_tiles<<'\n';
    std::cout<<"estimate_seconds="<<o.result.alignment_milliseconds/1000<<" render_seconds="<<o.result.blend_milliseconds/1000<<" write_review_total_seconds="<<o.seconds<<'\n';
    step=Clock::now();(void)LoadSavedStillReview(o.result.report);
    std::cout<<"read_report_seconds="<<Since(step)<<'\n';
}

}

int main(int argc,char** argv){
    try{
        if(argc<2)throw std::invalid_argument("usage: still_review_lens_tests <rig-profile-v2 fixture folder> [--measure | <StillReview executable>]");
        const bool measure=argc>=3 && (std::string(argv[2])=="--measure" || std::string(argv[2])=="--measure-turned");
        Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
        base_profile=ReadText(fs::path(argv[1])/"calibrated-draft.json");
        root=fs::current_path()/("still-review-lens-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        fs::create_directories(root);
        const auto started=Clock::now();
        const World world(17);
        if(measure){Measure(world,std::string(argv[2])=="--measure-turned"?Variant::turned:Variant::lens);std::error_code ignored;fs::remove_all(root,ignored);return 0;}
        Selection();
        const Case cases[]{{"plain",Variant::plain,{1.004,{0.3,-0.2}},17},{"lens",Variant::lens,{0.996,{-0.25,0.15}},17},{"turned",Variant::turned,{1.006,{0.2,0.25}},17}};
        std::vector<Rig> rigs;
        for(const auto& c:cases)rigs.push_back(RunCase(c,world,1));
        for(unsigned i=0;i<rigs.size();++i)Recovery(rigs[i],world,i);
        const auto& lens=rigs[1];
        Limits(lens);
        NotEstimated();
        SeamAfterCorrection(lens);
        Coverage(lens);
        SameMapping(lens,World(99));
        const auto other_profile=MakeSettings(root/"settings-other-profile",ProfileText(base_profile,Variant::plain),MountOrientation::normal,SmallRaster(),1,kSmallSeam,FixedSeamOrigin::operator_set);
        Tamper(lens,other_profile);
        Texts(lens);
        // Frames of another size, for the refusals that need no real pair.
        {
            const auto tiny=MakeFrames(root,"tiny",Flat(64,48,128),Flat(64,48,128));
            Refusals(lens.settings,tiny);
        }
        if(argc>=3)RunInspector(argv[2],lens.outcome.result.report);
        std::cout<<"checks="<<checks<<" failures="<<failures<<" seconds="<<Since(started)<<"\n";
        std::error_code ignored;fs::remove_all(root,ignored);
        return failures?1:0;
    }catch(const std::exception& error){
        std::cerr<<"exception: "<<error.what()<<"\n";
        return 2;
    }
}
