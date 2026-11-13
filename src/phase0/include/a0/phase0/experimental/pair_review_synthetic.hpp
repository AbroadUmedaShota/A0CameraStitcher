#pragma once
// A synthetic rig for the pair capture's smoke and tests (#417 J8w): the rig profile fixture (v2) turned into two cameras side by side
// (10 pixels per mm), a settings folder with that profile and revision r000001 of a condition v3, and full-size flat JPEG frames
// (7360 x 4912, the frame the profile's camera model fixes). Development only. No camera and no window. Nothing here is a
// measurement of a real rig.
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/document_render.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The review writer's test hook, defined in the offline stitcher library: runs after the composite JPEG is encoded to its partial and
// before it is flushed and renamed. Development smoke and tests only (the same declaration as in tests/offline_stitcher_tests.cpp).
// Two more: after the composite JPEG is saved and verified (before the gate and the report), and after the report is saved (before the
// writer returns). The smoke closes the deadline there.
namespace a0::m2::detail {
void SetBeforePartialFlushTestHook(void (*hook)()) noexcept;
void SetAfterReviewJpegTestHook(void (*hook)()) noexcept;
void SetBeforeReviewReturnTestHook(void (*hook)()) noexcept;
}

namespace a0::phase0::experimental::pair_synthetic {
namespace ex = a0::m2::experimental;
namespace render = a0::m2::render;
inline constexpr unsigned kWidth=7360,kHeight=4912;
// Output column of the seam in TallRaster (X = 650 mm).
inline constexpr std::uint32_t kSeam=900;

inline std::string ReadText(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
inline std::vector<unsigned char> ReadBytes(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
inline void WriteText(const std::filesystem::path& path,const std::string& text){std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;}
inline std::string Sha(const std::filesystem::path& path){return ex::FixedCompositeBytesSha256(ReadBytes(path));}
inline ex::FixedCompositeSha256 Hash(){return [](const std::vector<unsigned char>& bytes){return ex::FixedCompositeBytesSha256(bytes);};}

namespace detail {
inline std::string ReplaceAll(std::string text,const std::string& from,const std::string& to){
    for(size_t at=0;(at=text.find(from,at))!=std::string::npos;at+=to.size())text.replace(at,from.size(),to);
    return text;
}
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
inline void Hr(HRESULT hr){if(FAILED(hr))throw std::runtime_error("synthetic frame WIC failure");}
}

// The rig profile v2 fixture with the matrices of the side-by-side rig (turned: the same rig mounted a half turn) and a small lens.
inline std::string ProfileText(const std::string& base,bool turned){
    using namespace detail;
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
// 18 mm wide around the overlap (A only, the overlap, B only), 46 cm tall, 0.1 mm pixels. Seam column 900 is X = 650.
inline render::OutputRaster TallRaster(){return Raster(560000,10000,740000,470000,254);}

// The condition of revision `sequence`, with sample ranges and lines of a test (not owner values). The profile is copied into `folder`.
inline ex::FixedCompositeConditionV3 SampleCondition(const std::filesystem::path& folder,const std::string& profile,ex::MountOrientation orientation,
    const render::OutputRaster& raster,std::uint32_t sequence=1,std::optional<std::string> parent=std::nullopt,
    ex::FixedCompositeRevisionKind kind=ex::FixedCompositeRevisionKind::created){
    namespace cr=ex::content_registration;
    const auto hash=Hash();
    std::filesystem::create_directories(folder);
    ex::FixedCompositeConditionV3 c;
    c.created_utc="2026-11-04T12:00:00Z";c.revision.sequence=sequence;c.revision.parent_sha256=std::move(parent);c.revision.change.kind=kind;
    c.source.profile_file=ex::CopyFixedCompositeProfile(folder,profile,hash);
    c.source.profile_file_sha256=hash(std::vector<unsigned char>(profile.begin(),profile.end()));
    c.source.profile_fingerprint_sha256=a0::m2::RigProfileV2::Parse(profile).FingerprintSha256();
    c.source.policy_sha256=std::string(64,'c');
    c.orientation=orientation;c.raster=raster;c.seam.column=kSeam;c.seam.origin=ex::FixedSeamOrigin::operator_set;
    auto& g=c.registration;
    g.envelope={10,2,1,5};
    g.limits.brown_lowe_alpha=8;g.limits.brown_lowe_beta=0.3;g.limits.minimum_unique_blocks=2;g.limits.maximum_prediction_sigma_px=1.0;
    g.limits.maximum_residual_rms_px=0.5;g.limits.maximum_residual_p95_px=1.0;g.limits.maximum_bin_bias_px=0.5;g.limits.convergence_px=0.02;g.limits.start_agreement_px=0.3;
    g.geometry={8,72,256};g.surface_envelope=ex::SurfaceEnvelope{40,8};g.apply_beyond_envelope=true;
    g.parallax=ex::FixedCompositeConditionV3::Registration::Parallax{1.0};
    c.seam_gate={1.0,0.25};
    ex::ValidateFixedCompositeConditionV3(c);
    return c;
}

struct Settings {std::filesystem::path folder;ex::LoadedFixedCompositeConditionV3 loaded;std::string profile;};
// A settings folder with the profile and revision r000001, read back by the v3 reader (the revision in force).
inline Settings MakeSettings(const std::filesystem::path& folder,const std::string& profile,ex::MountOrientation orientation,const render::OutputRaster& raster){
    const auto c=SampleCondition(folder,profile,orientation,raster);
    const auto revisions=folder/std::string(ex::kFixedCompositeRevisionFolderName);
    std::filesystem::create_directories(revisions);
    ex::WriteFixedCompositeConditionV3(revisions,c);
    auto loaded=ex::LoadActiveFixedCompositeRevisionV3(revisions,Hash());
    if(!loaded)throw std::runtime_error("the written revision did not come back");
    return {folder,std::move(*loaded),profile};
}

inline render::BgrImage Flat(std::uint8_t value){return {kWidth,kHeight,std::vector<std::uint8_t>(static_cast<size_t>(kWidth)*kHeight*3,value)};}
// Writes the image as a JPEG (24 bit BGR). COM must be initialised on the calling thread.
inline void WriteJpeg(const std::filesystem::path& path,const render::BgrImage& image){
    using namespace detail;
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream));Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder));Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> props;Hr(encoder->CreateNewFrame(&frame,&props));Hr(frame->Initialize(props.Get()));Hr(frame->SetSize(image.width,image.height));
    auto format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format));auto pixels=image.bgr;
    Hr(frame->WritePixels(image.height,image.width*3,static_cast<UINT>(pixels.size()),pixels.data()));Hr(frame->Commit());Hr(encoder->Commit());
}
// The two originals of a pair under `dir`, in the alias folders the review writer requires (CAM-A, CAM-B).
struct Frames {std::filesystem::path a,b,root;};
inline Frames WriteFrames(const std::filesystem::path& dir,const render::BgrImage& a,const render::BgrImage& b){
    std::filesystem::create_directories(dir/"CAM-A");std::filesystem::create_directories(dir/"CAM-B");
    WriteJpeg(dir/"CAM-A/original.jpg",a);WriteJpeg(dir/"CAM-B/original.jpg",b);
    return {dir/"CAM-A/original.jpg",dir/"CAM-B/original.jpg",dir};
}
}
