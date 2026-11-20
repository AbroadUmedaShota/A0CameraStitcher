#pragma once
// A registered composite review (#417) written by the real writer from synthetic frames: a condition v3 revision, the
// profile it names, two full-size JPEG originals of a textured drawing and WriteUncalibratedStillReview. The writer's report is v9
// (#437); without a confirmed position it is the v8 report plus the schema and two null members. The same construction as
// tests/still_review_registration_tests.cpp (RequestFor + WriteUncalibratedStillReview), kept here so that the window and seam crop
// tests open a review that the writer made, not a hand-built report. Camera free; nothing outside the folder it is given is written.
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
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace registered_review_fixture {
using namespace a0::m2::experimental;
namespace render=a0::m2::render;
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;

inline const FixedCompositeSha256 kHash=[](const std::vector<unsigned char>& bytes){return FixedCompositeBytesSha256(bytes);};
inline constexpr unsigned kWidth=7360,kHeight=4912;
inline constexpr std::uint32_t kSeam=900;

inline std::string ReadText(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
inline std::vector<unsigned char> ReadFileBytes(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
inline std::string Sha(const fs::path& path){return FixedCompositeBytesSha256(ReadFileBytes(path));}
inline std::string ReplaceAll(std::string text,const std::string& from,const std::string& to){
    for(size_t at=0;(at=text.find(from,at))!=std::string::npos;at+=to.size())text.replace(at,from.size(),to);
    return text;
}
inline void Hr(HRESULT hr){if(FAILED(hr))throw std::runtime_error("fixture WIC failure");}

// ---- the rig profile (the fixture with the matrices of the side-by-side rig) ----
inline std::string Num(double v){char text[48];if(v==std::floor(v))std::snprintf(text,sizeof text,"%.1f",v);else std::snprintf(text,sizeof text,"%.17g",v);return text;}
inline std::string WithMatrix(const std::string& text,int camera,const std::array<double,6>& m){
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
// `turned` is the same rig mounted a half turn.
inline std::string ProfileText(const std::string& base,bool turned){
    std::string t=base;
    if(turned){t=WithMatrix(t,0,{-10,0,7359-100.0,0,-10,4911-100.0});t=WithMatrix(t,1,{-10,0,7359+5660.0,0,-10,4911-100.0});}
    else{t=WithMatrix(t,0,{10,0,100,0,10,100});t=WithMatrix(t,1,{10,0,-5660,0,10,100});}
    t=ReplaceAll(t,"\"k1\": 0.0","\"k1\": -0.05");t=ReplaceAll(t,"\"k2\": 0.0","\"k2\": 0.02");t=ReplaceAll(t,"\"p1\": 0.0","\"p1\": 0.0005");
    return t;
}
inline render::OutputRaster Raster(long long left,long long top,long long right,long long bottom,unsigned dpi){
    const auto pixels=[&](long long um){return static_cast<std::uint32_t>((um*static_cast<long long>(dpi)+25399)/25400);};
    return render::OutputRaster{render::DocumentRegionUm{left,top,right,bottom},dpi,pixels(right-left),pixels(bottom-top)};
}
// 18 mm wide around the overlap (A only, the overlap, B only), 46 cm tall, 0.1 mm pixels (1800 x 4600). Seam column 900.
inline render::OutputRaster TallRaster(){return Raster(560000,10000,740000,470000,254);}

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
inline void FillStrip(render::BgrImage& image,const render::CameraProjection& camera,unsigned u0,unsigned u1,const World& world){
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
inline void WriteJpeg(const fs::path& path,const render::BgrImage& image){
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream));Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder));Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> props;Hr(encoder->CreateNewFrame(&frame,&props));Hr(frame->Initialize(props.Get()));Hr(frame->SetSize(image.width,image.height));
    auto format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format));auto pixels=image.bgr;
    Hr(frame->WritePixels(image.height,image.width*3,static_cast<UINT>(pixels.size()),pixels.data()));Hr(frame->Commit());Hr(encoder->Commit());
}
inline render::BgrImage Flat(std::uint8_t value){return {kWidth,kHeight,std::vector<std::uint8_t>(static_cast<size_t>(kWidth)*kHeight*3,value)};}
struct Frames {fs::path a,b,root;};
inline Frames MakeFrames(const fs::path& root,const std::string& name,const render::BgrImage& a,const render::BgrImage& b){
    const auto dir=root/name;fs::create_directories(dir/"CAM-A");fs::create_directories(dir/"CAM-B");
    WriteJpeg(dir/"CAM-A/original.jpg",a);WriteJpeg(dir/"CAM-B/original.jpg",b);
    return {dir/"CAM-A/original.jpg",dir/"CAM-B/original.jpg",dir};
}

// ---- a settings folder with the profile and revision r000001 of a condition v3, read back by the V3 reader ----
struct Settings {fs::path folder;LoadedFixedCompositeConditionV3 loaded;std::string profile;};
inline Settings MakeSettings(const fs::path& folder,const std::string& profile,MountOrientation orientation,const render::OutputRaster& raster){
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
inline Frames TexturedFrames(const fs::path& root,const std::string& name,const Settings& s,bool turned,const World& world){
    const auto& p=s.loaded.profile.parameters;
    auto a=Flat(128),b=Flat(128);
    FillStrip(a,p.camera_a,turned?0:5560,turned?1800:kWidth,world);
    FillStrip(b,p.camera_b,turned?5560:0,turned?kWidth:1800,world);
    return MakeFrames(root,name,a,b);
}
inline StillReviewRequest RequestFor(const Frames& f,const char* output,const Settings& s,unsigned threads){
    PreviewReviewGeometry geometry{460,307,229,3,11,s.loaded.condition.orientation};
    StillReviewRequest q{f.a,f.b,f.root/output,Sha(f.a),Sha(f.b),"synthetic-pair-registered",geometry,std::chrono::steady_clock::now()+std::chrono::seconds(900)};
    q.fixed_composite_registration=StillReviewRequest::FixedCompositeRegistration{&s.loaded.condition,&s.loaded.profile,s.loaded.file_sha256,threads};
    return q;
}
// A position of this condition that a person set by hand (the confirmed position the window gives a capture, #437): sample values of a
// test, the same as the ones tests/still_review_registration_tests.cpp gives the writer. The hand sum is 12 px to the right, 4 px up and 0.05 degrees clockwise.
inline ConfirmedPosition HandPosition(const Settings& s){
    ConfirmedPosition p;
    p.mapping=RegisteredMapping{content_registration::PoseParameters{0.1,0,0.05,0.5,0.25,0},SurfacePlane{0.5,0.001,0},std::nullopt};
    p.source=ConfirmedPositionSource::manual;p.serial=4;p.condition_sha256=s.loaded.file_sha256;
    p.base_source=ConfirmedPositionSource::auto_start;p.manual_entered=HandCounts{-3,1,-1};
    return p;
}
}
