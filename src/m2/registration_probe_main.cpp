// Development probe of #417 (P0, and the P2 estimator with --estimate). Reads a fixed composite condition v2 and the two original JPEGs of a
// saved pair, places A and B on the document raster by their own fixed mappings (scale 1, no correction),
// measures the B-minus-A displacement field over the whole overlap band with the seam gate's measurer, and fits
// registration models to it. Nothing is composed, nothing is written next to the inputs, no camera is used.
// Not a product entrance: it is built only with A0_BUILD_REGISTRATION_PROBE and linked by nothing else.
#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/experimental/parallax_correction.hpp"
#include "a0/m2/experimental/registration_gate.hpp"
#include "a0/m2/experimental/registration_probe.hpp"
#include "a0/m2/optical_planner.hpp"
#include "a0/m2/seam_gate.hpp"

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace m2x = a0::m2::experimental;
namespace probe = a0::m2::experimental::registration_probe;
namespace gate = a0::m2::seam_gate;
namespace creg = a0::m2::experimental::content_registration;
namespace render = a0::m2::render;

[[noreturn]] void Fail(const std::string& message){throw std::runtime_error(message);}
void Hr(HRESULT result,const char* what){if(FAILED(result))Fail(std::string("image decode failed: ")+what);}
template<class T> struct Release {void operator()(T* v) const noexcept {if(v)v->Release();}};
template<class T> using Com=std::unique_ptr<T,Release<T>>;

struct Apartment {
    Apartment(){Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");}
    ~Apartment(){CoUninitialize();}
};

std::vector<unsigned char> ReadAll(const fs::path& path,std::uintmax_t limit){
    std::error_code ec;
    const auto size=fs::file_size(path,ec);
    if(ec || size==0 || size>limit)Fail("input file is missing or too large");
    std::ifstream in(path,std::ios::binary);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if(!in)Fail("input file could not be read");
    return bytes;
}

// Stored-order pixels as WIC decodes them; EXIF orientation is not applied.
render::BgrImage DecodeJpeg(const std::vector<unsigned char>& bytes){
    IWICImagingFactory* factory_raw{};
    Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory_raw)),"factory");
    Com<IWICImagingFactory> factory(factory_raw);
    IWICStream* stream_raw{};Hr(factory->CreateStream(&stream_raw),"stream");Com<IWICStream> stream(stream_raw);
    Hr(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),static_cast<DWORD>(bytes.size())),"stream init");
    IWICBitmapDecoder* decoder_raw{};
    Hr(factory->CreateDecoderFromStream(stream.get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder_raw),"decoder");
    Com<IWICBitmapDecoder> decoder(decoder_raw);
    GUID container{};Hr(decoder->GetContainerFormat(&container),"container");
    if(container!=GUID_ContainerFormatJpeg)Fail("input is not a JPEG");
    IWICBitmapFrameDecode* frame_raw{};Hr(decoder->GetFrame(0,&frame_raw),"frame");Com<IWICBitmapFrameDecode> frame(frame_raw);
    UINT width=0,height=0;Hr(frame->GetSize(&width,&height),"size");
    IWICFormatConverter* converter_raw{};Hr(factory->CreateFormatConverter(&converter_raw),"converter");Com<IWICFormatConverter> converter(converter_raw);
    Hr(converter->Initialize(frame.get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0.0,WICBitmapPaletteTypeCustom),"convert");
    render::BgrImage image;
    image.width=width;image.height=height;
    image.bgr.resize(static_cast<std::size_t>(render::PixelCount(width,height))*3);
    Hr(converter->CopyPixels(nullptr,width*3U,static_cast<UINT>(image.bgr.size()),image.bgr.data()),"pixels");
    return image;
}

void SavePng(const fs::path& path,unsigned width,unsigned height,const std::vector<std::uint8_t>& bgr){
    IWICImagingFactory* factory_raw{};
    Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory_raw)),"factory");
    Com<IWICImagingFactory> factory(factory_raw);
    IWICStream* stream_raw{};Hr(factory->CreateStream(&stream_raw),"stream");Com<IWICStream> stream(stream_raw);
    Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"png stream");
    IWICBitmapEncoder* encoder_raw{};Hr(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder_raw),"png encoder");Com<IWICBitmapEncoder> encoder(encoder_raw);
    Hr(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache),"png init");
    IWICBitmapFrameEncode* frame_raw{};IPropertyBag2* bag_raw{};Hr(encoder->CreateNewFrame(&frame_raw,&bag_raw),"png frame");
    Com<IWICBitmapFrameEncode> frame(frame_raw);Com<IPropertyBag2> bag(bag_raw);
    Hr(frame->Initialize(bag.get()),"png frame init");Hr(frame->SetSize(width,height),"png size");
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format),"png format");
    Hr(frame->WritePixels(height,width*3,static_cast<UINT>(bgr.size()),const_cast<BYTE*>(bgr.data())),"png pixels");
    Hr(frame->Commit(),"png frame commit");Hr(encoder->Commit(),"png commit");
}

void SaveJpeg(const fs::path& path,unsigned width,unsigned height,const std::vector<std::uint8_t>& bgr,float quality=0.92f){
    IWICImagingFactory* factory_raw{};
    Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory_raw)),"factory");
    Com<IWICImagingFactory> factory(factory_raw);
    IWICStream* stream_raw{};Hr(factory->CreateStream(&stream_raw),"stream");Com<IWICStream> stream(stream_raw);
    Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"jpeg stream");
    IWICBitmapEncoder* encoder_raw{};Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder_raw),"jpeg encoder");Com<IWICBitmapEncoder> encoder(encoder_raw);
    Hr(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache),"jpeg init");
    IWICBitmapFrameEncode* frame_raw{};IPropertyBag2* bag_raw{};Hr(encoder->CreateNewFrame(&frame_raw,&bag_raw),"jpeg frame");
    Com<IWICBitmapFrameEncode> frame(frame_raw);Com<IPropertyBag2> bag(bag_raw);
    PROPBAG2 option{};option.pstrName=const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v;VariantInit(&v);v.vt=VT_R4;v.fltVal=quality;
    Hr(bag->Write(1,&option,&v),"jpeg quality");
    Hr(frame->Initialize(bag.get()),"jpeg frame init");Hr(frame->SetSize(width,height),"jpeg size");
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format),"jpeg format");
    if(format!=GUID_WICPixelFormat24bppBGR)Fail("the JPEG encoder does not take 24-bit BGR");
    Hr(frame->WritePixels(height,width*3,static_cast<UINT>(bgr.size()),const_cast<BYTE*>(bgr.data())),"jpeg pixels");
    Hr(frame->Commit(),"jpeg frame commit");Hr(encoder->Commit(),"jpeg commit");
}

// A box-filtered copy of an image, `factor` pixels to one.
render::BgrImage Shrink(const render::BgrImage& image,unsigned factor){
    render::BgrImage out;
    out.width=image.width/factor;out.height=image.height/factor;
    out.bgr.assign(static_cast<std::size_t>(out.width)*out.height*3,0);
    for(unsigned y=0;y<out.height;++y)for(unsigned x=0;x<out.width;++x)for(int c=0;c<3;++c){
        unsigned sum=0;
        for(unsigned j=0;j<factor;++j)for(unsigned i=0;i<factor;++i)
            sum+=image.bgr[(static_cast<std::size_t>(y*factor+j)*image.width+x*factor+i)*3+c];
        out.bgr[(static_cast<std::size_t>(y)*out.width+x)*3+c]=static_cast<std::uint8_t>((sum+factor*factor/2)/(factor*factor));
    }
    return out;
}

// The part [x0, x0 + w) x [y0, y0 + h) of an image, clipped to it.
render::BgrImage CropOf(const render::BgrImage& image,int x0,int y0,unsigned w,unsigned h){
    x0=std::clamp(x0,0,static_cast<int>(image.width)-1);y0=std::clamp(y0,0,static_cast<int>(image.height)-1);
    w=std::min<unsigned>(w,image.width-static_cast<unsigned>(x0));h=std::min<unsigned>(h,image.height-static_cast<unsigned>(y0));
    render::BgrImage out;out.width=w;out.height=h;out.bgr.resize(static_cast<std::size_t>(w)*h*3);
    for(unsigned y=0;y<h;++y)std::copy_n(image.bgr.data()+(static_cast<std::size_t>(y0+static_cast<int>(y))*image.width+x0)*3,w*3,out.bgr.data()+static_cast<std::size_t>(y)*w*3);
    return out;
}

bool SampleGray(const render::BgrImage& image,render::Point raw,double& value){
    if(!(raw.x>=0 && raw.y>=0 && raw.x<image.width-1 && raw.y<image.height-1))return false;
    const int ix=static_cast<int>(std::floor(raw.x)),iy=static_cast<int>(std::floor(raw.y));
    const double fx=raw.x-ix,fy=raw.y-iy;
    const auto at=[&](int x,int y){const auto* p=image.bgr.data()+(static_cast<std::size_t>(y)*image.width+x)*3;return (p[0]+p[1]+p[2])/3.0;};
    value=(1-fy)*((1-fx)*at(ix,iy)+fx*at(ix+1,iy))+fy*((1-fx)*at(ix,iy+1)+fx*at(ix+1,iy+1));
    return true;
}

// Diagnostic: A and B each placed on the output grid by their own mapping around (cx,cy), brightened, side by side and as an overlay
// (red = A, green and blue = B, so a feature at the same place in both is grey). Written next to the results, never to the inputs.
void DumpCrop(const gate::SeamGateGeometry& g,const render::BgrImage& a,const render::BgrImage& b,int cx,int cy,int size,const fs::path& path){
    const unsigned s=static_cast<unsigned>(size);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(3*s)*s*3,0);
    const auto put=[&](unsigned panel,unsigned x,unsigned y,double r,double gr,double bl){
        const auto at=(static_cast<std::size_t>(y)*3*s+panel*s+x)*3;
        const auto clamp=[](double v){return static_cast<std::uint8_t>(std::clamp(v*4.0,0.0,255.0));};
        out[at]=clamp(bl);out[at+1]=clamp(gr);out[at+2]=clamp(r);
    };
    for(unsigned y=0;y<s;++y)for(unsigned x=0;x<s;++x){
        const render::Point o{static_cast<double>(cx-size/2+static_cast<int>(x)),static_cast<double>(cy-size/2+static_cast<int>(y))};
        render::Point ra{},rb{};double va=0,vb=0;
        const bool ha=g.camera_a(o,ra) && SampleGray(a,ra,va),hb=g.camera_b(o,rb) && SampleGray(b,rb,vb);
        if(ha)put(0,x,y,va,va,va);
        if(hb)put(1,x,y,vb,vb,vb);
        if(ha||hb)put(2,x,y,va,vb,vb);
    }
    SavePng(path,3*s,s,out);
}

std::string Num(double v){
    if(!std::isfinite(v))return "null";
    std::ostringstream s;s<<std::setprecision(6)<<v;return s.str();
}
std::string Csv(double v){
    if(!std::isfinite(v))return "";
    std::ostringstream s;s<<std::setprecision(6)<<v;return s.str();
}


// Test aid: B as it would have been captured after the document plane moved by `theta` in B's camera frame. The calibrated
// mapping of B is then wrong by exactly the pose change, so a fit to the measured field should return the change on top of
// what the pair already shows. The resampling map is computed on a coarse grid and interpolated (it is smooth), then each
// pixel is read bilinearly from the real image, so the texture and the noise are the real ones.
render::BgrImage InjectPose(const render::BgrImage& image,const render::CameraProjection& camera,const probe::PoseModel& pose,
    const probe::PoseParameters& theta,unsigned threads){
    constexpr unsigned kGrid=16;
    auto moved=camera;
    moved.document_to_image=pose.CameraHomography(theta);
    const unsigned gw=image.width/kGrid+2,gh=image.height/kGrid+2;
    const double nan=std::numeric_limits<double>::quiet_NaN();
    std::vector<double> mx(static_cast<std::size_t>(gw)*gh,nan),my(mx);
    for(unsigned j=0;j<gh;++j)for(unsigned i=0;i<gw;++i){
        render::Point doc{},source{};
        if(!m2x::TryDocumentPointOfRawPixel(moved,{static_cast<double>(i*kGrid),static_cast<double>(j*kGrid)},doc))continue;
        if(!render::TryProjectDocument(camera,doc,source))continue;
        mx[static_cast<std::size_t>(j)*gw+i]=source.x;my[static_cast<std::size_t>(j)*gw+i]=source.y;
    }
    render::BgrImage out;
    out.width=image.width;out.height=image.height;out.bgr.assign(image.bgr.size(),0);
    std::atomic<unsigned> next{0};
    const auto work=[&](){
        for(;;){
            const unsigned y=next.fetch_add(1);
            if(y>=image.height)return;
            const unsigned j=y/kGrid;const double fy=(y-j*kGrid)/static_cast<double>(kGrid);
            for(unsigned x=0;x<image.width;++x){
                const unsigned i=x/kGrid;const double fx=(x-i*kGrid)/static_cast<double>(kGrid);
                const std::size_t k00=static_cast<std::size_t>(j)*gw+i,k10=k00+1,k01=k00+gw,k11=k01+1;
                const double sx=(1-fy)*((1-fx)*mx[k00]+fx*mx[k10])+fy*((1-fx)*mx[k01]+fx*mx[k11]);
                const double sy=(1-fy)*((1-fx)*my[k00]+fx*my[k10])+fy*((1-fx)*my[k01]+fx*my[k11]);
                if(!std::isfinite(sx) || !std::isfinite(sy) || sx<0 || sy<0 || sx>=image.width-1 || sy>=image.height-1)continue;
                const int ix=static_cast<int>(sx),iy=static_cast<int>(sy);
                const double ax=sx-ix,ay=sy-iy;
                const auto* p00=image.bgr.data()+(static_cast<std::size_t>(iy)*image.width+ix)*3;
                const auto* p01=p00+static_cast<std::size_t>(image.width)*3;
                auto* o=out.bgr.data()+(static_cast<std::size_t>(y)*image.width+x)*3;
                for(int c=0;c<3;++c){
                    const double v=(1-ay)*((1-ax)*p00[c]+ax*p00[3+c])+ay*((1-ax)*p01[c]+ax*p01[3+c]);
                    o[c]=static_cast<std::uint8_t>(std::clamp(std::lround(v),0L,255L));
                }
            }
        }
    };
    std::vector<std::thread> pool;
    for(unsigned t=1;t<threads;++t)pool.emplace_back(work);
    work();
    for(auto& t:pool)t.join();
    return out;
}
struct Options {
    fs::path condition,image_a,image_b,out,field_before,field_after;
    std::string label="pair";
    unsigned spacing=64,threads=0,global_evaluations=256,patch=0,search=0;
    double prior_x=0,prior_y=0;
    std::optional<probe::PoseParameters> inject;   // test aid: move B by this pose before measuring
    std::vector<std::array<int,3>> crops;   // x,y,size of a diagnostic crop of the placed A and B
    // --estimate: run the content registration estimator (#417 P2) on the pair.
    bool estimate=false;
    std::optional<std::array<double,4>> envelope;   // position mm, rotation deg, tilt deg, height percent
    unsigned mixing_width=8,omega_margin=8,guard=72,band_half_width=100000;
    // Development values for the checks (not product values): loose enough for the residual a draft calibration leaves.
    double bl_alpha=8,bl_beta=0.3,max_prediction_sigma=1.0,max_rms=3.0,max_p95=6.0,max_bias=4.0,convergence=2.0,start_agreement=2.0;
    unsigned min_unique=2;
    unsigned fine_spacing=0,fine_search=0,fine_max_search=0;   // 0: the draft design value
    // --pipeline: the parts of the integration branch in the order the capture path calls them (estimate, layer 2, gate),
    // without the capture path itself. The values the condition v3 will hold are arguments here.
    bool pipeline=false;
    std::optional<std::array<double,2>> surface_envelope;   // height mm, tilt deg; absent: six components, no surface
    std::optional<double> max_lift_mm=1.0;                  // the parallax line; --no-parallax: layer 2 disabled
    bool apply_beyond_line=true;
    double seam_limit_px=1.0,seam_guard_px=0.2;
    unsigned gate_band_half_width=200;                      // the band of layer 2's points and of the gate's anchors
    fs::path markers,view;
    bool render=true;
    std::optional<creg::PoseParameters> pose_override;      // isolation test: use this pose in place of the estimate
    std::optional<std::array<double,3>> surface_override;      // h0 mm, slope x, slope y
    unsigned outer_iterations=0;                               // 0: the draft design value of the estimator
    double parallax_max_strain=0,parallax_falloff=0;           // 0: the draft design values of layer 2 (s_max, L)
    std::optional<creg::PoseParameters> compare_pose;          // --pipeline: a pose found another way (marks), to compare the estimate with at the seam
    bool estimate_only=false;                                  // --pipeline: stop after the estimate (development)
    bool coarse_only=false;                                    // --pipeline: stop the estimator before its fine stage (to time the coarse stage)
    std::vector<std::pair<std::string,double>> design;         // --design name=value: a design constant of the estimator (development)
};
Options Parse(int argc,char** argv){
    Options o;
    // --pipeline starts from the limits of the condition sample (tests/registration_fixtures.hpp), not from the loose
    // development values of --estimate; any limit given on the command line still overrides them.
    for(int i=1;i<argc;++i)if(std::string(argv[i])=="--pipeline"){
        o.pipeline=true;o.estimate=false;
        o.max_prediction_sigma=0.5;o.max_rms=0.4;o.max_p95=1.0;o.max_bias=0.5;o.convergence=0.05;o.start_agreement=0.5;
    }
    for(int i=1;i<argc;++i){
        const std::string key=argv[i];
        const auto value=[&]()->std::string{if(i+1>=argc)Fail("missing value for "+key);return argv[++i];};
        if(key=="--condition")o.condition=value();
        else if(key=="--image-a")o.image_a=value();
        else if(key=="--image-b")o.image_b=value();
        else if(key=="--out")o.out=value();
        else if(key=="--label")o.label=value();
        else if(key=="--spacing")o.spacing=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--threads")o.threads=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--global-evaluations")o.global_evaluations=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--patch")o.patch=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--search")o.search=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--inject-pose"){
            std::istringstream parts(value());probe::PoseParameters p{};char c[5]{};
            if(!(parts>>p.rx_deg>>c[0]>>p.ry_deg>>c[1]>>p.rz_deg>>c[2]>>p.tx_mm>>c[3]>>p.ty_mm>>c[4]>>p.tz_percent))Fail("--inject-pose takes rx,ry,rz (deg),tx,ty (mm),tz (percent)");
            o.inject=p;
        }
        else if(key=="--estimate")o.estimate=!o.pipeline;
        else if(key=="--pipeline"){}
        else if(key=="--surface-envelope"){
            std::istringstream parts(value());std::array<double,2> e{};char c{};
            if(!(parts>>e[0]>>c>>e[1]) || c!=',' || !(e[0]>0) || !(e[1]>0))Fail("--surface-envelope takes height_mm,tilt_deg");
            o.surface_envelope=e;
        }
        else if(key=="--max-lift")o.max_lift_mm=std::stod(value());
        else if(key=="--no-parallax")o.max_lift_mm.reset();
        else if(key=="--apply-beyond-line"){const auto v=value();if(v!="true" && v!="false")Fail("--apply-beyond-line takes true or false");o.apply_beyond_line=v=="true";}
        else if(key=="--seam-limit")o.seam_limit_px=std::stod(value());
        else if(key=="--seam-guard")o.seam_guard_px=std::stod(value());
        else if(key=="--gate-band-half-width")o.gate_band_half_width=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--markers")o.markers=value();
        else if(key=="--view")o.view=value();
        else if(key=="--no-render")o.render=false;
        else if(key=="--pose-override"){
            std::istringstream parts(value());creg::PoseParameters p{};char c[5]{};
            if(!(parts>>p.rx_deg>>c[0]>>p.ry_deg>>c[1]>>p.rz_deg>>c[2]>>p.tx_mm>>c[3]>>p.ty_mm>>c[4]>>p.tz_percent))Fail("--pose-override takes rx,ry,rz (deg),tx,ty (mm),tz (percent)");
            o.pose_override=p;
        }
        else if(key=="--compare-pose"){
            std::istringstream parts(value());creg::PoseParameters p{};char c[5]{};
            if(!(parts>>p.rx_deg>>c[0]>>p.ry_deg>>c[1]>>p.rz_deg>>c[2]>>p.tx_mm>>c[3]>>p.ty_mm>>c[4]>>p.tz_percent))Fail("--compare-pose takes rx,ry,rz (deg),tx,ty (mm),tz (percent)");
            o.compare_pose=p;
        }
        else if(key=="--outer-iterations")o.outer_iterations=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--estimate-only")o.estimate_only=true;
        else if(key=="--coarse-only")o.coarse_only=true;
        else if(key=="--design"){
            const auto v=value();const auto eq=v.find('=');
            if(eq==std::string::npos || eq==0)Fail("--design takes name=value");
            o.design.emplace_back(v.substr(0,eq),std::stod(v.substr(eq+1)));
        }
        else if(key=="--parallax-max-strain")o.parallax_max_strain=std::stod(value());
        else if(key=="--parallax-falloff")o.parallax_falloff=std::stod(value());
        else if(key=="--surface-override"){
            std::istringstream parts(value());std::array<double,3> s{};char c[2]{};
            if(!(parts>>s[0]>>c[0]>>s[1]>>c[1]>>s[2]))Fail("--surface-override takes h0_mm,slope_x,slope_y");
            o.surface_override=s;
        }
        else if(key=="--envelope"){
            std::istringstream parts(value());std::array<double,4> e{};char c[3]{};
            if(!(parts>>e[0]>>c[0]>>e[1]>>c[1]>>e[2]>>c[2]>>e[3]) || c[0]!=',' || c[1]!=',' || c[2]!=',')Fail("--envelope takes position_mm,rotation_deg,tilt_deg,height_percent");
            o.envelope=e;
        }
        else if(key=="--mixing-width")o.mixing_width=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--omega-margin")o.omega_margin=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--guard")o.guard=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--band-half-width")o.band_half_width=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--fine-spacing")o.fine_spacing=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--fine-search")o.fine_search=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--fine-max-search")o.fine_max_search=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--min-unique")o.min_unique=static_cast<unsigned>(std::stoul(value()));
        else if(key=="--max-prediction-sigma")o.max_prediction_sigma=std::stod(value());
        else if(key=="--max-rms")o.max_rms=std::stod(value());
        else if(key=="--max-p95")o.max_p95=std::stod(value());
        else if(key=="--max-bias")o.max_bias=std::stod(value());
        else if(key=="--convergence")o.convergence=std::stod(value());
        else if(key=="--start-agreement")o.start_agreement=std::stod(value());
        else if(key=="--field-before")o.field_before=value();
        else if(key=="--field-after")o.field_after=value();
        else if(key=="--prior"){
            std::istringstream parts(value());char comma{};
            if(!(parts>>o.prior_x>>comma>>o.prior_y) || comma!=',' || !std::isfinite(o.prior_x) || !std::isfinite(o.prior_y))Fail("--prior takes dx,dy in output pixels");
        }
        else if(key=="--crop"){
            std::array<int,3> c{};
            std::istringstream parts(value());char comma1{},comma2{};
            if(!(parts>>c[0]>>comma1>>c[1]>>comma2>>c[2]) || comma1!=',' || comma2!=',' || c[2]<16 || c[2]>1024)Fail("--crop takes x,y,size");
            o.crops.push_back(c);
        }
        else Fail("unknown option "+key);
    }
    if((o.estimate || o.pipeline) && !o.envelope)Fail("--estimate and --pipeline need --envelope (there is no default range)");
    const bool compare=!o.field_before.empty() || !o.field_after.empty();
    if(o.condition.empty() || o.out.empty() || (compare ? (o.field_before.empty() || o.field_after.empty()) : (o.image_a.empty() || o.image_b.empty())))
        Fail("usage: --condition <file> --out <folder> (--image-a <jpg> --image-b <jpg> [--prior dx,dy] | --field-before <field.csv> --field-after <field.csv>)\n"
             "       [--label <name>] [--spacing 64] [--patch 64] [--search 16] [--threads N] [--global-evaluations 256] [--crop x,y,size]\n"
             "       --estimate --envelope <mm,deg,deg,percent> [--mixing-width 8] [--omega-margin 8] [--guard 72] [--band-half-width N] [--min-unique 2]\n"
             "                  [--max-prediction-sigma 1] [--max-rms 3] [--max-p95 6] [--max-bias 4] [--convergence 2] [--start-agreement 2]\n"
             "                  [--fine-spacing 96] [--fine-search 24] [--fine-max-search 48]\n"
             "       --pipeline --envelope <mm,deg,deg,percent> [--surface-envelope <mm,deg>] [--max-lift 1.0 | --no-parallax]\n"
             "                  [--apply-beyond-line true] [--seam-limit 1.0] [--seam-guard 0.2] [--gate-band-half-width 200]\n"
             "                  [--markers <id,ax,ay,bx,by.csv>] [--view <folder>] [--no-render] (and the estimator options above)\n"
             "                  [--estimate-only] [--design name=value]... (a design constant of the estimator, development)");
    if(!o.threads)o.threads=std::max(2u,std::min(8u,std::thread::hardware_concurrency()));
    return o;
}

// Processor time of this process so far (all threads, user and kernel), seconds. Unlike the wall time it does not grow
// while other programs hold the processors, so it says what a stage costs on a busy machine.
double ProcessorSeconds(){
    FILETIME creation{},exit{},kernel{},user{};
    if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user))return 0;
    const auto ticks=[](const FILETIME& t){return (static_cast<std::uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    return static_cast<double>(ticks(kernel)+ticks(user))*1e-7;
}

double Seconds(std::chrono::steady_clock::time_point since){
    return std::chrono::duration<double>(std::chrono::steady_clock::now()-since).count();
}

struct ModelReport {
    probe::ModelFit fit;
    std::vector<double> residual;          // per observation, all counted points
    probe::ResidualStats band_all,band_inliers;
    probe::SeamColumnSeries seam;
    probe::ResidualShape shape;
};

std::string PoseJson(const probe::PoseParameters& p){
    std::ostringstream s;
    s<<"{\"rxDeg\":"<<Num(p.rx_deg)<<",\"ryDeg\":"<<Num(p.ry_deg)<<",\"rzDeg\":"<<Num(p.rz_deg)<<",\"txMm\":"<<Num(p.tx_mm)
     <<",\"tyMm\":"<<Num(p.ty_mm)<<",\"tzPercent\":"<<Num(p.tz_percent)<<"}";
    return s.str();
}
std::string StatsJson(const probe::ResidualStats& s){
    std::ostringstream o;
    o<<"{\"count\":"<<s.count<<",\"mean\":"<<Num(s.mean)<<",\"rms\":"<<Num(s.rms)<<",\"p95\":"<<Num(s.p95)<<",\"max\":"<<Num(s.maximum)<<"}";
    return o.str();
}

// What the measuring step knew, for the summary. Empty for a comparison of two saved fields.
struct MeasurementNotes {
    bool measured{};
    std::uint32_t band_rows{},inset{};
    std::size_t points{};
    unsigned unconfirmed{};
    std::map<std::string,unsigned> status_counts;
    unsigned global_candidates{},global_evaluated{},global_verified{},global_contradicted{},global_indeterminate{};
    unsigned patch{},search{};
    double prior_x{},prior_y{};
};

// Fits every model to the observations and writes summary.txt, models.json, field.csv and seam_residual.csv.
void AnalyzeAndWrite(const Options& options,const render::DocumentRenderParameters& parameters,unsigned seam_column,
    const std::vector<probe::Observation>& observations,const MeasurementNotes& notes){
    const probe::PoseModel pose(parameters.camera_b);
    const probe::PlaneFrame frame=probe::FrameOfRaster(parameters.raster);
    const probe::FitContext context{frame,&pose};
    const unsigned height=parameters.raster.height_pixels;
    const probe::ModelKind kinds[]{probe::ModelKind::none,probe::ModelKind::translation,probe::ModelKind::similarity,
        probe::ModelKind::similarity_vertical_scale,probe::ModelKind::pose6,probe::ModelKind::projective8,probe::ModelKind::polynomial2};
    std::vector<ModelReport> reports;
    for(const auto kind:kinds){
        ModelReport r;
        r.fit=probe::FitModel(kind,observations,context);
        std::vector<double> inliers;
        for(std::size_t i=0;i<observations.size();++i)r.residual.push_back(probe::ResidualOf(observations[i],r.fit));
        r.band_all=probe::StatsOf(r.residual);
        for(std::size_t i=0;i<observations.size();++i)if(r.fit.weight.empty() || r.fit.weight[i]>0)inliers.push_back(r.residual[i]);
        r.band_inliers=probe::StatsOf(inliers);
        r.seam=probe::SeamColumnResidual(observations,r.residual,seam_column,options.spacing,height);
        r.shape=probe::ShapeOf(r.seam);
        reports.push_back(std::move(r));
    }

    fs::create_directories(options.out);
    {
        std::ofstream out(options.out/"summary.txt");
        out<<"label: "<<options.label<<"\n";
        out<<"raster: "<<parameters.raster.width_pixels<<" x "<<parameters.raster.height_pixels<<" px at "<<parameters.raster.dpi<<" dpi, seam column "<<seam_column<<"\n";
        if(notes.measured){
            out<<"band rows: "<<notes.band_rows<<" of "<<height<<", inset "<<notes.inset<<" px, lattice "<<options.spacing<<" px, patch "<<notes.patch
               <<" px, search radius "<<notes.search<<" px, prior ("<<notes.prior_x<<", "<<notes.prior_y<<") px\n";
            out<<"points: "<<notes.points<<", counted "<<observations.size()<<", valid but unconfirmed "<<notes.unconfirmed<<"\n";
            out<<"status:";for(const auto& [k,v]:notes.status_counts)out<<' '<<k<<'='<<v;out<<"\n";
            out<<"global anchors: candidates "<<notes.global_candidates<<", evaluated "<<notes.global_evaluated
               <<", verified "<<notes.global_verified<<", contradicted "<<notes.global_contradicted<<", indeterminate "<<notes.global_indeterminate<<"\n";
        }else out<<"difference of two measurements: "<<observations.size()<<" joined points, lattice "<<options.spacing<<" px\n";
        {unsigned two=0,across_y=0,across_x=0,oblique=0;
         for(const auto& o:observations){if(!o.one_directional)++two;else if(std::abs(o.normal.y)>0.7)++across_y;else if(std::abs(o.normal.x)>0.7)++across_x;else ++oblique;}
         out<<"counted by direction: two-directional "<<two<<", lines across y (constrain dy) "<<across_y<<", lines across x (constrain dx) "<<across_x<<", oblique "<<oblique<<"\n";}
        out<<"pivot depth (mm): "<<Num(pose.PivotDepthMm())<<"\n\n";
        out<<std::fixed<<std::setprecision(3);
        out<<"model                         seam col mean       p95       max      rows | band(all) mean       rms       p95       max | inliers rms       p95       max | inliers/n  robust scale\n";
        for(const auto& r:reports){
            out<<std::left<<std::setw(30)<<probe::ModelName(r.fit.kind)<<std::right
               <<std::setw(10)<<r.seam.stats.mean<<std::setw(10)<<r.seam.stats.p95<<std::setw(10)<<r.seam.stats.maximum<<std::setw(10)<<r.seam.covered_rows
               <<"   "<<std::setw(10)<<r.band_all.mean<<std::setw(10)<<r.band_all.rms<<std::setw(10)<<r.band_all.p95<<std::setw(10)<<r.band_all.maximum
               <<"   "<<std::setw(10)<<r.band_inliers.rms<<std::setw(10)<<r.band_inliers.p95<<std::setw(10)<<r.band_inliers.maximum
               <<"   "<<r.fit.inliers<<'/'<<r.fit.observations<<"  "<<r.fit.robust_scale_px<<(r.fit.converged||r.fit.kind==probe::ModelKind::none?"":"  (not converged)")<<"\n";
        }
        out<<"\nshape of the residual along the seam column (px): top third / middle third / bottom third mean, share in the outer fifths\n";
        for(const auto& r:reports)
            out<<std::left<<std::setw(30)<<probe::ModelName(r.fit.kind)<<std::right<<std::setw(10)<<r.shape.top_third_mean
               <<std::setw(10)<<r.shape.middle_third_mean<<std::setw(10)<<r.shape.bottom_third_mean<<std::setw(8)<<r.shape.end_share<<"\n";
        for(const auto& r:reports){
            if(r.fit.kind!=probe::ModelKind::pose6 || !r.fit.pose)continue;
            const auto& p=*r.fit.pose;
            out<<"\npose-6: rx "<<p.rx_deg<<" ry "<<p.ry_deg<<" rz "<<p.rz_deg<<" deg, tx "<<p.tx_mm<<" ty "<<p.ty_mm<<" mm, tz "<<p.tz_percent<<" %\n";
            if(r.fit.pose_sigma){const auto& s=*r.fit.pose_sigma;
                out<<"sigma:  rx "<<s.rx_deg<<" ry "<<s.ry_deg<<" rz "<<s.rz_deg<<" deg, tx "<<s.tx_mm<<" ty "<<s.ty_mm<<" mm, tz "<<s.tz_percent<<" %\n";}
        }
    }
    {
        std::ofstream out(options.out/"models.json");
        out<<"{\"label\":\""<<options.label<<"\",\"raster\":{\"width\":"<<parameters.raster.width_pixels<<",\"height\":"<<height
           <<",\"dpi\":"<<parameters.raster.dpi<<"},\"seamColumn\":"<<seam_column<<",\"latticePx\":"<<options.spacing
           <<",\"measured\":"<<(notes.measured?"true":"false")<<",\"bandRows\":"<<notes.band_rows<<",\"points\":"<<notes.points<<",\"counted\":"<<observations.size()
           <<",\"unconfirmed\":"<<notes.unconfirmed<<",\"patchPx\":"<<notes.patch<<",\"searchPx\":"<<notes.search
           <<",\"priorPx\":["<<Num(notes.prior_x)<<","<<Num(notes.prior_y)<<"],\"globalVerified\":"<<notes.global_verified<<",\"globalContradicted\":"<<notes.global_contradicted
           <<",\"globalIndeterminate\":"<<notes.global_indeterminate<<",\"pivotDepthMm\":"<<Num(pose.PivotDepthMm())<<",\"models\":[";
        for(std::size_t k=0;k<reports.size();++k){
            const auto& r=reports[k];
            if(k)out<<',';
            out<<"{\"model\":\""<<probe::ModelName(r.fit.kind)<<"\",\"fitted\":"<<(r.fit.fitted?"true":"false")<<",\"converged\":"<<(r.fit.converged?"true":"false")
               <<",\"inliers\":"<<r.fit.inliers<<",\"observations\":"<<r.fit.observations<<",\"robustScalePx\":"<<Num(r.fit.robust_scale_px)
               <<",\"seamColumn\":"<<StatsJson(r.seam.stats)<<",\"seamRowsCovered\":"<<r.seam.covered_rows<<",\"band\":"<<StatsJson(r.band_all)
               <<",\"bandInliers\":"<<StatsJson(r.band_inliers)
               <<",\"shape\":{\"topThird\":"<<Num(r.shape.top_third_mean)<<",\"middleThird\":"<<Num(r.shape.middle_third_mean)
               <<",\"bottomThird\":"<<Num(r.shape.bottom_third_mean)<<",\"endShare\":"<<Num(r.shape.end_share)<<"}";
            out<<",\"parameters\":[";for(std::size_t i=0;i<r.fit.parameters.size();++i){if(i)out<<',';out<<Num(r.fit.parameters[i]);}out<<"]";
            out<<",\"parameterSigma\":[";for(std::size_t i=0;i<r.fit.parameter_sigma.size();++i){if(i)out<<',';out<<Num(r.fit.parameter_sigma[i]);}out<<"]";
            if(r.fit.pose)out<<",\"pose\":"<<PoseJson(*r.fit.pose);
            if(r.fit.pose_sigma)out<<",\"poseSigma\":"<<PoseJson(*r.fit.pose_sigma);
            out<<"}";
        }
        out<<"]}\n";
    }
    // field.csv: one line per point, with the residual and the final weight after every model.
    {
        std::ofstream out(options.out/"field.csv");
        out<<"x,y,one_directional,nx,ny,dx,dy,sigma,visible";
        for(const auto& r:reports)out<<",res_"<<probe::ModelName(r.fit.kind);
        for(const auto& r:reports)if(!r.fit.weight.empty())out<<",w_"<<probe::ModelName(r.fit.kind);
        out<<"\n";
        for(std::size_t i=0;i<observations.size();++i){
            const auto& o=observations[i];
            out<<o.x<<','<<o.y<<','<<(o.one_directional?1:0)<<','<<Csv(o.normal.x)<<','<<Csv(o.normal.y)<<','<<Csv(o.d.x)<<','<<Csv(o.d.y)<<','<<Csv(o.sigma_px)<<','<<Csv(o.visible_px);
            for(const auto& r:reports)out<<','<<Csv(r.residual[i]);
            for(const auto& r:reports)if(!r.fit.weight.empty())out<<','<<Csv(r.fit.weight[i]);
            out<<"\n";
        }
    }
    // seam_residual.csv: every output row.
    {
        std::ofstream out(options.out/"seam_residual.csv");
        out<<"row";
        for(const auto& r:reports)out<<','<<probe::ModelName(r.fit.kind);
        out<<"\n";
        for(std::uint32_t y=0;y<height;++y){
            out<<y;
            for(const auto& r:reports)out<<','<<Csv(r.seam.residual[y]);
            out<<"\n";
        }
    }
}

m2x::LoadedFixedCompositeConditionV2 LoadCondition(const fs::path& path){
    const auto loaded=m2x::LoadFixedCompositeConditionAny(path,m2x::FixedCompositeBytesSha256);
    if(!loaded)Fail("no fixed composite condition at the given path");
    const auto* v2=std::get_if<m2x::LoadedFixedCompositeConditionV2>(&*loaded);
    if(!v2)Fail("the condition is not a v2 record");
    return *v2;
}

// The points of a field.csv written by an earlier run, in file order.
std::vector<probe::Observation> ReadField(const fs::path& path){
    std::ifstream in(path);
    if(!in)Fail("a field file could not be read");
    std::string line;
    std::getline(in,line);
    std::vector<std::string> names;
    {std::istringstream header(line);std::string cell;while(std::getline(header,cell,','))names.push_back(cell);}
    const auto column=[&](const std::string& name)->std::size_t{
        const auto found=std::find(names.begin(),names.end(),name);
        if(found==names.end())Fail("a field file lacks the column "+name);
        return static_cast<std::size_t>(found-names.begin());
    };
    const std::size_t cx=column("x"),cy=column("y"),cone=column("one_directional"),cnx=column("nx"),cny=column("ny"),cdx=column("dx"),cdy=column("dy"),csigma=column("sigma"),cvis=column("visible");
    std::vector<probe::Observation> out;
    while(std::getline(in,line)){
        std::vector<std::string> cells;
        {std::istringstream row(line);std::string cell;while(std::getline(row,cell,','))cells.push_back(cell);}
        if(cells.size()<=std::max({cx,cy,cone,cnx,cny,cdx,cdy,csigma,cvis}))continue;
        probe::Observation o;
        o.x=std::stod(cells[cx]);o.y=std::stod(cells[cy]);o.one_directional=cells[cone]=="1";
        o.normal={cells[cnx].empty()?0.0:std::stod(cells[cnx]),cells[cny].empty()?0.0:std::stod(cells[cny])};
        o.d={std::stod(cells[cdx]),std::stod(cells[cdy])};o.sigma_px=std::stod(cells[csigma]);o.visible_px=std::stod(cells[cvis]);
        out.push_back(o);
    }
    return out;
}

int RunMeasure(const Options& options){
    const auto start=std::chrono::steady_clock::now();
    const auto loaded=LoadCondition(options.condition);
    const auto& condition=loaded.condition;
    const auto& parameters=loaded.profile.parameters;

    Apartment apartment;
    std::cerr<<"decoding\n";
    const auto image_a=DecodeJpeg(ReadAll(options.image_a,128ull<<20));
    auto image_b=DecodeJpeg(ReadAll(options.image_b,128ull<<20));
    if(image_a.width!=a0::m2::kD810SensorWidthPixels || image_a.height!=a0::m2::kD810SensorHeightPixels ||
       image_b.width!=image_a.width || image_b.height!=image_a.height)
        Fail("the images are not the frame size the rig profile fixes");
    std::cerr<<"decoded "<<Seconds(start)<<" s\n";
    render::Point inject_shift{0,0};
    if(options.inject){
        const probe::PoseModel inject_model(parameters.camera_b);
        image_b=InjectPose(image_b,parameters.camera_b,inject_model,*options.inject,options.threads);
        inject_shift=probe::PoseDisplacement(inject_model,probe::FrameOfRaster(parameters.raster),*options.inject,condition.seam.column,parameters.raster.height_pixels/2.0);
        std::cerr<<"injected a pose change, expected displacement at the seam ("<<inject_shift.x<<", "<<inject_shift.y<<") px\n";
    }

    // The mapping is defined on the stored pixel order (design 3.7), so the images are not turned.
    auto geometry=gate::DocumentSeamGeometry(parameters);
    auto design=probe::FieldDesign(options.spacing,options.global_evaluations);
    if(options.patch){design.patch_px=options.patch;design.remeasure_patch_px=std::max(design.remeasure_patch_px,options.patch+32);}
    if(options.search){design.search_radius_px=options.search;design.maximum_search_radius_px=std::max(design.maximum_search_radius_px,4*options.search);}
    gate::ValidateSeamGateDesign(design);
    const std::uint32_t inset=gate::RequiredBandInsetPx(design);
    // The band follows the unshifted placement, so two measurements with the same settings share one lattice.
    geometry.band=gate::OverlapBand(geometry,image_a.width,image_a.height,image_b.width,image_b.height,inset);
    std::uint32_t band_rows=0;
    for(const auto& row:geometry.band)if(row.begin<row.end)++band_rows;
    std::cerr<<"band rows "<<band_rows<<" of "<<geometry.height<<"\n";
    if(!band_rows)Fail("the overlap band is empty");

    // A prior: B is looked at where it is expected to show A's patch, so that the true displacement is inside the search window.
    auto measured_geometry=geometry;
    render::Point prior{options.prior_x,options.prior_y};
    if(options.inject){prior.x+=inject_shift.x;prior.y+=inject_shift.y;}   // the prior of the pair plus the expected effect of the injected pose
    if(prior.x!=0 || prior.y!=0){
        const auto camera_b=geometry.camera_b;
        measured_geometry.camera_b=[camera_b,prior](render::Point o,render::Point& raw){return camera_b({o.x+prior.x,o.y+prior.y},raw);};
    }
    const gate::SeamGateLimits limits{1.0,0.2,"probe"};
    const auto measure_start=std::chrono::steady_clock::now();
    const auto measurement=gate::MeasureSeam(image_a,image_b,measured_geometry,design,limits,nullptr,options.threads);
    std::cerr<<"measured "<<Seconds(measure_start)<<" s, points "<<measurement.points.size()<<"\n";

    MeasurementNotes notes;
    notes.measured=true;notes.band_rows=band_rows;notes.inset=inset;notes.points=measurement.points.size();
    notes.patch=design.patch_px;notes.search=design.search_radius_px;notes.prior_x=prior.x;notes.prior_y=prior.y;
    for(const auto& p:measurement.points){
        ++notes.status_counts[std::string(gate::PointStatusCode(p.status))];
        if(p.status==gate::PointStatus::valid && p.confirmation==gate::Confirmation::unconfirmed)++notes.unconfirmed;
    }
    notes.global_candidates=measurement.global.candidates;notes.global_evaluated=measurement.global.evaluated;notes.global_verified=measurement.global.verified;
    notes.global_contradicted=measurement.global.contradicted;notes.global_indeterminate=measurement.global.indeterminate;
    const auto observations=probe::ObservationsOf(measurement,prior);
    if(observations.empty())Fail("no counted point in the band");

    AnalyzeAndWrite(options,parameters,condition.seam.column,observations,notes);
    for(const auto& c:options.crops)DumpCrop(geometry,image_a,image_b,c[0],c[1],c[2],options.out/("crop_"+std::to_string(c[0])+"_"+std::to_string(c[1])+".png"));
    std::cerr<<"done "<<Seconds(start)<<" s\n";
    return 0;
}

std::string Fixed(double v,int digits=3){
    if(!std::isfinite(v))return "nan";
    std::ostringstream s;s<<std::fixed<<std::setprecision(digits)<<v;return s.str();
}

// The content registration estimator on one saved pair, then the displacement that is left in Omega (the seam column
// +- the gate's range) when B is placed by the estimated pose. Omega's points are not points the estimate used.
int RunEstimate(const Options& options){
    const auto loaded=LoadCondition(options.condition);
    const auto& condition=loaded.condition;
    const auto& parameters=loaded.profile.parameters;
    Apartment apartment;
    const auto image_a=DecodeJpeg(ReadAll(options.image_a,128ull<<20));
    const auto image_b=DecodeJpeg(ReadAll(options.image_b,128ull<<20));
    if(image_a.width!=a0::m2::kD810SensorWidthPixels || image_a.height!=a0::m2::kD810SensorHeightPixels ||
       image_b.width!=image_a.width || image_b.height!=image_a.height)
        Fail("the images are not the frame size the rig profile fixes");

    creg::RegistrationSettings settings;
    settings.geometry={condition.seam.column,options.mixing_width,options.omega_margin,options.guard,options.band_half_width};
    settings.envelope={(*options.envelope)[0],(*options.envelope)[1],(*options.envelope)[2],(*options.envelope)[3]};
    settings.limits.brown_lowe_alpha=options.bl_alpha;settings.limits.brown_lowe_beta=options.bl_beta;settings.limits.minimum_unique_blocks=options.min_unique;
    settings.limits.maximum_prediction_sigma_px=options.max_prediction_sigma;settings.limits.maximum_residual_rms_px=options.max_rms;
    settings.limits.maximum_residual_p95_px=options.max_p95;settings.limits.maximum_bin_bias_px=options.max_bias;
    settings.limits.convergence_px=options.convergence;settings.limits.start_agreement_px=options.start_agreement;
    settings.design=creg::kRegistrationDesignDraft;
    if(options.fine_spacing)settings.design.fine_point_spacing_px=options.fine_spacing;
    if(options.fine_search)settings.design.fine_search_radius_px=options.fine_search;
    if(options.fine_max_search)settings.design.fine_maximum_search_radius_px=options.fine_max_search;

    const auto start=std::chrono::steady_clock::now();
    const auto result=creg::EstimateRegistration(image_a,image_b,parameters,settings,options.threads);
    const double estimate_seconds=Seconds(start);
    const creg::PoseModel model(parameters.camera_b);
    std::cerr<<options.label<<": "<<creg::RegistrationStateCode(result.state)<<" in "<<estimate_seconds<<" s\n";

    // What is left in Omega when B is placed by the estimate (also when the state says not to apply it, to see what applying would do).
    struct OmegaStats {unsigned points=0,counted=0;double mean=0,p95=0,maximum=0;double seconds=0;bool measured=false;};
    OmegaStats omega;
    if(result.has_estimate){
        const auto omega_start=std::chrono::steady_clock::now();
        const auto input=m2x::PoseCorrectedDocumentMappedFixedSeamInput(parameters,result.pose);
        gate::SeamGateGeometry geometry;
        geometry.width=input.width;geometry.height=input.height;geometry.camera_a=input.camera_a;geometry.camera_b=input.camera_b;
        auto design=probe::FieldDesign(32,8);
        geometry.band=gate::OverlapBand(geometry,image_a.width,image_a.height,image_b.width,image_b.height,gate::RequiredBandInsetPx(design));
        const std::uint32_t half=settings.geometry.omega_half_width_px();
        for(auto& row:geometry.band){
            if(row.begin>=row.end)continue;
            const auto lo=static_cast<std::uint32_t>(std::max<std::int64_t>(row.begin,static_cast<std::int64_t>(condition.seam.column)-half));
            const auto hi=static_cast<std::uint32_t>(std::min<std::int64_t>(row.end,static_cast<std::int64_t>(condition.seam.column)+half+1));
            row=lo<hi?gate::BandRow{lo,hi}:gate::BandRow{};
        }
        const gate::SeamGateLimits limits{1.0,0.2,"probe"};
        const auto measurement=gate::MeasureSeam(image_a,image_b,geometry,design,limits,nullptr,options.threads);
        std::vector<double> visible;
        for(const auto& p:measurement.points){
            if(p.status==gate::PointStatus::outside_coverage)continue;
            ++omega.points;
            if(p.Counted())visible.push_back(p.visible_px);
        }
        omega.counted=static_cast<unsigned>(visible.size());
        const auto stats=probe::StatsOf(visible);
        omega.mean=stats.mean;omega.p95=stats.p95;omega.maximum=stats.maximum;omega.measured=true;
        omega.seconds=Seconds(omega_start);
    }

    fs::create_directories(options.out);
    const auto& d=result.diagnostics;
    std::ostringstream text;
    text<<"label: "<<options.label<<"\n";
    text<<"state: "<<creg::RegistrationStateCode(result.state)<<"\n";
    text<<"reasons:";for(const auto& r:result.reasons)text<<' '<<r;text<<"\n";
    text<<"envelope (position mm, rotation deg, tilt deg, height %): "<<Fixed((*options.envelope)[0],2)<<", "<<Fixed((*options.envelope)[1],2)<<", "
        <<Fixed((*options.envelope)[2],2)<<", "<<Fixed((*options.envelope)[3],2)<<"\n";
    text<<"exceeded:";for(const auto c:result.exceeded)text<<' '<<creg::EnvelopeComponentCode(c);text<<"\n";
    text<<"estimate seconds: "<<Fixed(estimate_seconds,1)<<"\n";
    if(result.has_estimate){
        const auto& p=result.pose;const auto& s=result.pose_sigma;const auto& y=result.display;
        text<<"pose: rx "<<Fixed(p.rx_deg)<<" ry "<<Fixed(p.ry_deg)<<" rz "<<Fixed(p.rz_deg)<<" deg, tx "<<Fixed(p.tx_mm)<<" ty "<<Fixed(p.ty_mm)<<" mm, tz "<<Fixed(p.tz_percent)<<" %\n";
        text<<"sigma: rx "<<Fixed(s.rx_deg)<<" ry "<<Fixed(s.ry_deg)<<" rz "<<Fixed(s.rz_deg)<<" deg, tx "<<Fixed(s.tx_mm)<<" ty "<<Fixed(s.ty_mm)<<" mm, tz "<<Fixed(s.tz_percent)<<" %\n";
        text<<"display: position "<<Fixed(y.position_mm,2)<<" mm, rotation "<<Fixed(y.rotation_deg,2)<<" deg, tilt "<<Fixed(y.tilt_deg,2)<<" deg, height "<<Fixed(y.height_percent,2)
            <<" % ("<<Fixed(y.height_mm,1)<<" mm of a pivot depth "<<Fixed(model.PivotDepthMm(),0)<<" mm)\n";
        for(const auto& c:result.components)
            text<<"component "<<creg::EnvelopeComponentCode(c.component)<<": "<<Fixed(c.value,2)<<" of "<<Fixed(c.limit,2)<<(c.exceeded?"  EXCEEDED":"")<<"\n";
    }
    text<<"coarse: radius "<<Fixed(d.coarse_radius_x_px,0)<<" x "<<Fixed(d.coarse_radius_y_px,0)<<" px, unique blocks "<<d.unique_blocks<<", consistent "<<d.consistent_blocks<<"\n";
    for(const auto& b:d.coarse_blocks)
        text<<"  block rows "<<b.y0<<"+"<<b.rows<<": "<<creg::CoarseBlockStatusCode(b.status)<<" corr "<<Fixed(b.correlation,2)
            <<(b.status==creg::CoarseBlockStatus::unique?" d ("+Fixed(b.displacement.x,1)+", "+Fixed(b.displacement.y,1)+")":"")<<(b.consistent?" used":"")<<"\n";
    text<<"fine: outer iterations "<<d.outer_iterations<<", gate points "<<d.gate_points<<", counted "<<d.points<<", inliers "<<d.inliers<<", robust scale "<<Fixed(d.robust_scale_px,3)
        <<" px, converged "<<(d.converged?"yes":"no")<<" (changes at the seam column:"<<[&]{std::ostringstream h;for(const double c:d.change_history_px)h<<' '<<Fixed(c,3);return h.str();}()<<" px)\n";
    text<<"checks: brown-lowe threshold "<<Fixed(d.brown_lowe_threshold,1)<<"; prediction sigma max "<<Fixed(d.maximum_prediction_sigma_px,3)<<" px over "<<d.seam_rows_checked
        <<" rows; inlier residual rms "<<Fixed(d.residual.rms)<<" p95 "<<Fixed(d.residual.p95)<<" max "<<Fixed(d.residual.maximum)<<" px; bin bias "<<Fixed(d.maximum_bin_bias_px)
        <<" px; second start "<<creg::SecondStartCode(d.second_start)<<" ("<<Fixed(d.second_start_difference_px)<<" px)\n";
    if(omega.measured)
        text<<"omega (points not used by the estimate, B placed by the estimate): points "<<omega.points<<", counted "<<omega.counted<<", mean "<<Fixed(omega.mean)
            <<" p95 "<<Fixed(omega.p95)<<" max "<<Fixed(omega.maximum)<<" px, measured in "<<Fixed(omega.seconds,1)<<" s\n";
    std::ofstream(options.out/"estimate.txt")<<text.str();
    {
        std::ofstream out(options.out/"observations.csv");
        out<<"x,y,one_directional,nx,ny,dx,dy,sigma,residual,weight\n";
        for(const auto& o:d.observations)
            out<<o.x<<','<<o.y<<','<<(o.one_directional?1:0)<<','<<Csv(o.normal.x)<<','<<Csv(o.normal.y)<<','<<Csv(o.displacement.x)<<','<<Csv(o.displacement.y)<<','
               <<Csv(o.sigma_px)<<','<<Csv(o.residual_px)<<','<<Csv(o.weight)<<"\n";
    }
    std::cout<<text.str();
    return 0;
}

// ---- --pipeline ------------------------------------------------------------------------------
namespace pc = a0::m2::experimental::parallax_correction;
namespace rg = a0::m2::experimental::registration_gate;

std::string StatText(const probe::ComponentStats& s){
    if(!s.count)return "unmeasured (0 points)";
    return "n "+std::to_string(s.count)+", max "+Fixed(s.maximum_abs)+", rms "+Fixed(s.rms)+" px";
}
void WriteComponents(std::ostream& out,const std::string& indent,const probe::SeamComponents& c){
    out<<indent<<"counted points "<<c.counted<<", valid but not counted (no value) "<<c.not_counted<<"\n";
    out<<indent<<"two-directional, along e_b:      "<<StatText(c.two_along)<<"\n";
    out<<indent<<"two-directional, across e_b:     "<<StatText(c.two_across)<<"\n";
    out<<indent<<"two-directional, |d|:            "<<StatText(c.two_magnitude)<<"\n";
    out<<indent<<"one-directional, with baseline:  "<<StatText(c.one_with_baseline)<<"  (n.d only)\n";
    out<<indent<<"one-directional, across baseline:"<<StatText(c.one_across_baseline)<<"  (n.d only; says nothing of dx)\n";
}
std::string OptionalPx(const std::optional<double>& v){return v?Fixed(*v)+" px":std::string("unmeasured");}
std::string RangesText(const std::vector<rg::RowRange>& ranges){
    if(ranges.empty())return "none";
    std::ostringstream s;std::uint32_t total=0;
    for(std::size_t i=0;i<ranges.size();++i){if(i)s<<", ";s<<ranges[i].first<<"-"<<ranges[i].last;total+=ranges[i].last-ranges[i].first+1;}
    return std::to_string(ranges.size())+" ranges, "+std::to_string(total)+" output rows: "+s.str();
}
void WriteGate(std::ostream& out,const std::string& name,const rg::RegistrationGateResult& g){
    out<<name<<": state "<<rg::GateStateCode(g.state)<<", cause "<<rg::GateCauseCode(g.cause)<<", remedy "<<g.remedy<<"\n";
    out<<"  seam gate findings:";if(g.findings.empty())out<<" none";for(const auto& f:g.findings)out<<' '<<f.reason<<'/'<<f.remedy;out<<"\n";
    out<<"  gate findings:";if(g.gate_findings.empty())out<<" none";for(const auto& f:g.gate_findings)out<<' '<<f.reason<<'/'<<f.remedy;out<<"\n";
    if(g.seam_gate)out<<"  seam gate maximum "<<OptionalPx(g.seam_gate->maximum_px)<<", predicted maximum "<<OptionalPx(g.seam_gate->predicted_maximum_px)<<"\n";
    out<<"  maxima: two-directional "<<OptionalPx(g.maxima.two_directional_px)<<", one-directional "<<OptionalPx(g.maxima.one_directional_px);
    if(g.maxima.one_directional_component)out<<" ("<<rg::OneDirectionalComponentCode(*g.maxima.one_directional_component)<<", n "
        <<Fixed(g.maxima.one_directional_normal->x,2)<<","<<Fixed(g.maxima.one_directional_normal->y,2)<<")";
    out<<"\n  before layer 2: maximum "<<OptionalPx(g.before.maximum_px)<<" (two "<<OptionalPx(g.before.maxima.two_directional_px)<<", one "<<OptionalPx(g.before.maxima.one_directional_px)
       <<"); after: maximum "<<OptionalPx(g.after.maximum_px)<<"\n";
    out<<"  rows: with content "<<g.rows.rows_with_content<<", baseline measured "<<g.rows.rows_dx_measured<<" (share "<<Fixed(g.rows.dx_measured_share)<<")\n";
    out<<"  rows where dx is unmeasured: "<<RangesText(g.rows.unmeasured)<<"\n";
    out<<"  rows outside the measurable band: "<<RangesText(g.rows.outside_measurable)<<"\n";
    if(g.measurement){
        const auto& gl=g.measurement->global;
        out<<"  global placement: candidates "<<gl.candidates<<", evaluated "<<gl.evaluated<<", verified "<<gl.verified<<", contradicted "<<gl.contradicted<<", indeterminate "<<gl.indeterminate<<"\n";
    }
    if(g.anchor_columns)out<<"  anchors from columns "<<g.anchor_columns->first<<"-"<<g.anchor_columns->second<<"\n";
    if(g.lift)out<<"  lift: exceeding "<<g.lift->points_exceeding<<", baseline share "<<Fixed(g.lift->baseline_share)<<", sign share "<<Fixed(g.lift->sign_agreement_share)
        <<", height "<<(g.lift->height_estimate_mm?Fixed(*g.lift->height_estimate_mm,2)+" mm":std::string("-"))<<"\n";
    if(g.wide_search)out<<"  wide search: blocks "<<g.wide_search->blocks<<", unique "<<g.wide_search->unique_blocks<<", agreeing "<<g.wide_search->agreeing_blocks
        <<", offset "<<(g.wide_search->offset_px?Fixed(*g.wide_search->offset_px,2)+" px":std::string("-"))<<"\n";
}
void WritePoints(const fs::path& path,const gate::SeamMeasurement& m){
    std::ofstream out(path);
    out<<"x,y,status,counted,kind,nx,ny,dx,dy,visible,uncertainty,confirmation,remeasured\n";
    for(const auto& p:m.points){
        if(p.status==gate::PointStatus::outside_coverage)continue;
        out<<p.x<<','<<p.y<<','<<gate::PointStatusCode(p.status)<<','<<(p.Counted()?1:0)<<','<<(p.kind==gate::PointKind::one_directional?"one":"two")<<','
           <<Csv(p.normal.x)<<','<<Csv(p.normal.y)<<','<<Csv(p.displacement.x)<<','<<Csv(p.displacement.y)<<','<<Csv(p.visible_px)<<','<<Csv(p.uncertainty_px)<<','
           <<(p.confirmation==gate::Confirmation::confirmed?"confirmed":p.confirmation==gate::Confirmation::unconfirmed?"unconfirmed":"-")<<','<<p.remeasurements.size()<<"\n";
    }
}

struct Marker {std::string id;render::Point a,b;};
std::vector<Marker> ReadMarkers(const fs::path& path){
    std::ifstream in(path);
    if(!in)Fail("the marker file could not be read");
    std::string line;std::getline(in,line);
    std::vector<Marker> out;
    while(std::getline(in,line)){
        std::vector<std::string> cells;{std::istringstream row(line);std::string cell;while(std::getline(row,cell,','))cells.push_back(cell);}
        if(cells.size()<5)continue;
        out.push_back({cells[0],{std::stod(cells[1]),std::stod(cells[2])},{std::stod(cells[3]),std::stod(cells[4])}});
    }
    return out;
}

// A design constant of the estimator by name (development: to try a value without a build).
void ApplyDesign(creg::RegistrationDesign& d,const std::vector<std::pair<std::string,double>>& values){
    for(const auto& [name,v]:values){
        const auto u=[&]{return static_cast<std::uint32_t>(v);};
        if(name=="coarse-block-rows")d.coarse_block_rows=u();
        else if(name=="coarse-radius-scale")d.coarse_radius_scale=v;
        else if(name=="coarse-maximum-radius")d.coarse_maximum_radius_px=u();
        else if(name=="coarse-secondary-ratio")d.coarse_secondary_ratio=v;
        else if(name=="coarse-consistency")d.coarse_consistency_px=v;
        else if(name=="coarse-vote-peaks")d.coarse_vote_peaks=u();
        else if(name=="coarse-alias-peaks")d.coarse_alias_peaks=u();
        else if(name=="coarse-alias-minimum-score")d.coarse_alias_minimum_score=v;
        else if(name=="coarse-alias-margin")d.coarse_alias_margin=v;
        else if(name=="window-core")d.window_core_px=u();
        else if(name=="window-group-points")d.window_group_points=u();
        else if(name=="window-core-bias")d.window_core_bias_px=v;
        else if(name=="window-reach")d.window_reach_px=u();
        else if(name=="prior-sigma-factor")d.prior_sigma_factor=v;
        else if(name=="surface-prior-sigma-factor")d.surface_prior_sigma_factor=v;
        else if(name=="surface-pose-prior-sigma-factor")d.surface_pose_prior_sigma_factor=v;
        else if(name=="proximity-length")d.proximity_length_px=v;
        else if(name=="fine-patch")d.fine_patch_px=u();
        else if(name=="minimum-points")d.minimum_points=u();
        else Fail("unknown design constant "+name);
    }
}

int RunPipeline(const Options& options){
    const auto start=std::chrono::steady_clock::now();
    std::vector<std::pair<std::string,double>> stages;
    auto mark=std::chrono::steady_clock::now();
    std::vector<double> processor;   // processor seconds of each stage (all threads)
    double processor_mark=ProcessorSeconds();
    const auto stage=[&](const std::string& name){
        stages.emplace_back(name,Seconds(mark));processor.push_back(ProcessorSeconds()-processor_mark);
        std::cerr<<name<<" "<<Fixed(stages.back().second,1)<<" s (processor "<<Fixed(processor.back(),1)<<" s)\n";
        mark=std::chrono::steady_clock::now();processor_mark=ProcessorSeconds();
    };

    const auto loaded=LoadCondition(options.condition);
    const auto& parameters=loaded.profile.parameters;
    const std::uint32_t seam=loaded.condition.seam.column;
    const std::uint32_t width=parameters.raster.width_pixels,height=parameters.raster.height_pixels;
    Apartment apartment;
    const auto image_a=DecodeJpeg(ReadAll(options.image_a,128ull<<20));
    const auto image_b=DecodeJpeg(ReadAll(options.image_b,128ull<<20));
    if(image_a.width!=a0::m2::kD810SensorWidthPixels || image_a.height!=a0::m2::kD810SensorHeightPixels ||
       image_b.width!=image_a.width || image_b.height!=image_a.height)
        Fail("the images are not the frame size the rig profile fixes");
    stage("decode");

    // 1 The pose and the paper plane (S2).
    const auto blend=m2x::kSeamBlendDesign;
    creg::RegistrationSettings settings;
    settings.geometry={seam,blend.width_pixels,options.omega_margin,options.guard,options.band_half_width};
    settings.envelope={(*options.envelope)[0],(*options.envelope)[1],(*options.envelope)[2],(*options.envelope)[3]};
    settings.limits.brown_lowe_alpha=options.bl_alpha;settings.limits.brown_lowe_beta=options.bl_beta;settings.limits.minimum_unique_blocks=options.min_unique;
    settings.limits.maximum_prediction_sigma_px=options.max_prediction_sigma;settings.limits.maximum_residual_rms_px=options.max_rms;
    settings.limits.maximum_residual_p95_px=options.max_p95;settings.limits.maximum_bin_bias_px=options.max_bias;
    settings.limits.convergence_px=options.convergence;settings.limits.start_agreement_px=options.start_agreement;
    settings.design=creg::kRegistrationDesignDraft;
    if(options.fine_spacing)settings.design.fine_point_spacing_px=options.fine_spacing;
    if(options.fine_search)settings.design.fine_search_radius_px=options.fine_search;
    if(options.fine_max_search)settings.design.fine_maximum_search_radius_px=options.fine_max_search;
    if(options.outer_iterations)settings.design.maximum_outer_iterations=options.outer_iterations;
    ApplyDesign(settings.design,options.design);
    if(options.surface_envelope)settings.surface_envelope=creg::SurfaceEnvelope{(*options.surface_envelope)[0],(*options.surface_envelope)[1]};
    settings.base_pose={};   // p_b = 0: the condition is the one rebased to B's present place
    creg::RegistrationControl control;
    if(options.coarse_only)control.stop=[]{return true;};   // the caller's stop before the first fine measurement
    const auto estimate=creg::EstimateRegistration(image_a,image_b,parameters,settings,options.threads,control);
    stage("estimate (S2)");
    // What the capture path takes: an estimate that is verified, or trusted and beyond a range (applied with a warning
    // when the condition says applyBeyondEnvelope).
    const bool trusted=estimate.has_estimate && (estimate.state==creg::RegistrationState::verified || estimate.state==creg::RegistrationState::out_of_envelope);
    // An override replaces the estimate (an isolation test of the later stages; the capture path has none).
    const creg::PoseParameters pose=options.pose_override?*options.pose_override:estimate.has_estimate?estimate.pose:creg::PoseParameters{};
    const m2x::SurfacePlane plane=options.surface_override?m2x::SurfacePlane{(*options.surface_override)[0],(*options.surface_override)[1],(*options.surface_override)[2]}:
        options.pose_override?m2x::SurfacePlane{}:estimate.has_estimate && estimate.surface.state==m2x::SurfaceState::estimated?estimate.surface.plane:m2x::SurfacePlane{};

    // The estimate's section of the text (and its points as a table).
    const auto write_estimate=[&](std::ostream& text){
        const auto& d=estimate.diagnostics;
        text<<"[1 estimate] state "<<creg::RegistrationStateCode(estimate.state)
            <<(!trusted?"  (not trusted: the capture path would not apply it; applied here to see the numbers)":
               estimate.state==creg::RegistrationState::out_of_envelope?"  (trusted, beyond a range: applied with a warning when the condition applies beyond the envelope)":"")<<"\n";
        text<<"  reasons:";if(estimate.reasons.empty())text<<" none";for(const auto& r:estimate.reasons)text<<' '<<r;text<<"\n";
        text<<"  exceeded:";if(estimate.exceeded.empty())text<<" none";for(const auto c:estimate.exceeded)text<<' '<<creg::EnvelopeComponentCode(c);text<<"\n";
        if(estimate.has_estimate){
            const auto& p=estimate.pose;const auto& s=estimate.pose_sigma;
            text<<"  pose: rx "<<Fixed(p.rx_deg,4)<<" ry "<<Fixed(p.ry_deg,4)<<" rz "<<Fixed(p.rz_deg,4)<<" deg, tx "<<Fixed(p.tx_mm)<<" ty "<<Fixed(p.ty_mm)<<" mm, tz "<<Fixed(p.tz_percent,4)<<" %\n";
            text<<"  sigma: rx "<<Fixed(s.rx_deg,4)<<" ry "<<Fixed(s.ry_deg,4)<<" rz "<<Fixed(s.rz_deg,4)<<" deg, tx "<<Fixed(s.tx_mm)<<" ty "<<Fixed(s.ty_mm)<<" mm, tz "<<Fixed(s.tz_percent,4)<<" %\n";
            for(const auto& c:estimate.components)text<<"  component "<<creg::EnvelopeComponentCode(c.component)<<": "<<Fixed(c.value)<<" of "<<Fixed(c.limit,2)<<(c.exceeded?"  EXCEEDED":"")<<"\n";
            for(const auto& c:estimate.surface_components)text<<"  component "<<creg::EnvelopeComponentCode(c.component)<<": "<<Fixed(c.value)<<" of "<<Fixed(c.limit,2)<<(c.exceeded?"  EXCEEDED":"")<<"\n";
        }
        const auto& sf=estimate.surface;
        text<<"  surface: "<<creg::SurfaceStateCode(sf.state);
        if(sf.state==m2x::SurfaceState::estimated)
            text<<", h0 "<<Fixed(sf.plane.h0_mm)<<" mm (sigma "<<Fixed(sf.sigma.h0_mm)<<"), slope x "<<Fixed(sf.plane.slope_x,5)<<" y "<<Fixed(sf.plane.slope_y,5)
                <<", tilt "<<Fixed(sf.display.tilt_deg)<<" deg (x "<<Fixed(sf.display.tilt_x_deg)<<", y "<<Fixed(sf.display.tilt_y_deg)<<"), baseline equivalent "
                <<Fixed(sf.display.baseline_equivalent_mm)<<" mm, scale "<<Fixed(sf.display.scale_factor,5);
        text<<"\n  condition number "<<Num(d.information_condition_number)<<", largest surface-pose correlation "<<Fixed(d.maximum_surface_pose_correlation)<<"\n";
        text<<"  fine: outer "<<d.outer_iterations<<", points "<<d.points<<", inliers "<<d.inliers<<", robust scale "<<Fixed(d.robust_scale_px)<<" px, residual rms "<<Fixed(d.residual.rms)
            <<" p95 "<<Fixed(d.residual.p95)<<" max "<<Fixed(d.residual.maximum)<<" px, bin bias "<<Fixed(d.maximum_bin_bias_px)<<" px, prediction sigma "<<Fixed(d.maximum_prediction_sigma_px)
            <<" px, converged "<<(d.converged?"yes":"no")<<", second start "<<creg::SecondStartCode(d.second_start)<<" ("<<Fixed(d.second_start_difference_px)<<" px)\n";
        if(estimate.surface.state!=m2x::SurfaceState::disabled && estimate.has_estimate)
            text<<"  residual by the baseline: across rms "<<Fixed(d.residual_across.rms)<<" p95 "<<Fixed(d.residual_across.p95)<<" max "<<Fixed(d.residual_across.maximum)<<" px (n "<<d.residual_across.count
                <<"), bin bias "<<Fixed(d.maximum_bin_bias_across_px)<<" px; along rms "<<Fixed(d.residual_along.rms)<<" p95 "<<Fixed(d.residual_along.p95)<<" max "<<Fixed(d.residual_along.maximum)
                <<" px (n "<<d.residual_along.count<<"), bin bias "<<Fixed(d.maximum_bin_bias_along_px)<<" px"<<(d.residual_along_exceeded?"  (along: above the limits, left to layer 2)":"")<<"\n";
        text<<"  coarse: radius "<<Fixed(d.coarse_radius_x_px,0)<<" x "<<Fixed(d.coarse_radius_y_px,0)<<" px, blocks "<<d.coarse_blocks.size()<<", unique "<<d.unique_blocks
            <<", consistent "<<d.consistent_blocks<<", vote candidates "<<d.coarse_vote.candidates<<(d.coarse_vote.decided?" (decided)":" (not decided)")<<"\n";
        {std::map<std::string,unsigned> by;for(const auto& b:d.coarse_blocks)++by[std::string(creg::CoarseBlockStatusCode(b.status))];
         text<<"  coarse blocks by status:";for(const auto& [k,v]:by)text<<' '<<k<<'='<<v;text<<"\n";}
        text<<"  vote: hypotheses "<<d.coarse_vote.hypotheses<<", support "<<d.coarse_vote.support<<" (rival "<<d.coarse_vote.rival_support<<"), anchors "<<d.coarse_vote.anchor_support
            <<" (rival "<<d.coarse_vote.rival_anchor_support<<"), offset ("<<Fixed(d.coarse_vote.offset.x,1)<<", "<<Fixed(d.coarse_vote.offset.y,1)<<") px\n";
        for(const auto& b:d.coarse_blocks){
            text<<"    block rows "<<b.y0<<"+"<<b.rows<<": "<<creg::CoarseBlockStatusCode(b.status)<<", correlation "<<Fixed(b.correlation,3);
            if(b.compared_peaks)text<<", strong peaks compared "<<b.compared_peaks<<", colour agreement "<<Fixed(b.colour_score,3)<<" (lead "<<Fixed(b.colour_margin,3)<<")";
            if(b.voted || b.status==creg::CoarseBlockStatus::unique)text<<", d ("<<Fixed(b.displacement.x,1)<<", "<<Fixed(b.displacement.y,1)<<")";
            text<<(b.voted?", voted":"")<<(b.consistent?", used":"")<<"\n";
        }
        if(d.fit_window.grown){
            const auto& w=d.fit_window;
            text<<"  fit window: left "<<Fixed(w.left_px,0)<<" of "<<Fixed(w.left_available_px,0)<<" px"<<(w.left_cut?" (cut)":"")<<", right "<<Fixed(w.right_px,0)<<" of "
                <<Fixed(w.right_available_px,0)<<" px"<<(w.right_cut?" (cut)":"")<<"; core points "<<w.core_points<<", points beyond the cuts "<<w.refused_points<<"\n";
        }
        if(options.compare_pose && estimate.has_estimate){
            // Where the estimate and the other pose place B's content along the seam column: the difference of the two
            // displacements, over the rows the fit's points span and over every row.
            const creg::PoseModel model(parameters.camera_b);
            const auto frame=creg::FrameOfRaster(parameters.raster);
            const auto other=model.Homography(*options.compare_pose);
            const auto shift=[&](const a0::m2::render::Matrix3& m,double x,double y){
                const double px=frame.left_mm+(x+0.5)*frame.pitch_mm,py=frame.top_mm+(y+0.5)*frame.pitch_mm,w=m[6]*px+m[7]*py+m[8];
                return a0::m2::render::Point{((m[0]*px+m[1]*py+m[2])/w-px)/frame.pitch_mm,((m[3]*px+m[4]*py+m[5])/w-py)/frame.pitch_mm};
            };
            double y0=1e18,y1=-1e18;
            for(const auto& o:d.observations)if(o.weight>0){y0=std::min(y0,o.y);y1=std::max(y1,o.y);}
            struct Stat {double along=0,across=0,sum_across=0;unsigned n=0;} inside,all;
            const auto e_b=estimate.surface.state==m2x::SurfaceState::estimated?estimate.surface.frame.baseline:a0::m2::render::Point{1,0};
            for(std::uint32_t y=0;y<height;y+=32){
                const auto a=shift(estimate.document_homography,seam,y),b=shift(other,seam,y);
                const double dx=a.x-b.x,dy=a.y-b.y,along=dx*e_b.x+dy*e_b.y,across=-dx*e_b.y+dy*e_b.x;
                for(auto* s:{&all,&inside}){
                    if(s==&inside && !(y>=y0 && y<=y1))continue;
                    s->along=std::max(s->along,std::abs(along));s->across=std::max(s->across,std::abs(across));s->sum_across+=across*across;++s->n;
                }
            }
            const auto& p=*options.compare_pose;
            text<<"  against the other pose (rx "<<Fixed(p.rx_deg,4)<<" ry "<<Fixed(p.ry_deg,4)<<" rz "<<Fixed(p.rz_deg,4)<<" deg, tx "<<Fixed(p.tx_mm)<<" ty "<<Fixed(p.ty_mm)<<" mm, tz "<<Fixed(p.tz_percent,4)
                <<" %), difference of the placement along the seam column: rows of the fit "<<Fixed(y0,0)<<"-"<<Fixed(y1,0)<<": across the baseline max "<<Fixed(inside.across)<<" rms "
                <<Fixed(inside.n?std::sqrt(inside.sum_across/inside.n):0.0)<<" px, along max "<<Fixed(inside.along)<<" px; all rows: across max "<<Fixed(all.across)<<", along max "<<Fixed(all.along)<<" px\n";
        }
        text<<"  changes at the seam column per outer iteration:";for(const double c:d.change_history_px)text<<' '<<Fixed(c,3);text<<" px\n";
        unsigned two=0,one=0;for(const auto& o:d.observations)(o.one_directional?one:two)++;
        text<<"  fit points by kind: two-directional "<<two<<", one-directional "<<one<<"\n";
        if(options.pose_override || options.surface_override)
            text<<"  OVERRIDE in the later stages: pose rx "<<Fixed(pose.rx_deg,4)<<" ry "<<Fixed(pose.ry_deg,4)<<" rz "<<Fixed(pose.rz_deg,4)<<" deg, tx "<<Fixed(pose.tx_mm)
                <<" ty "<<Fixed(pose.ty_mm)<<" mm, tz "<<Fixed(pose.tz_percent,4)<<" %; plane h0 "<<Fixed(plane.h0_mm)<<" mm, slopes "<<Fixed(plane.slope_x,5)<<", "<<Fixed(plane.slope_y,5)<<"\n";
        text<<"\n";
        std::ofstream obs(options.out/"estimate_observations.csv");
        obs<<"x,y,one_directional,nx,ny,dx,dy,sigma,residual,weight\n";
        for(const auto& o:d.observations)
            obs<<o.x<<','<<o.y<<','<<(o.one_directional?1:0)<<','<<Csv(o.normal.x)<<','<<Csv(o.normal.y)<<','<<Csv(o.displacement.x)<<','<<Csv(o.displacement.y)<<','
               <<Csv(o.sigma_px)<<','<<Csv(o.residual_px)<<','<<Csv(o.weight)<<"\n";
    };
    if(options.estimate_only){
        fs::create_directories(options.out);
        std::ostringstream text;
        text<<"label: "<<options.label<<" (estimate only)\n";
        text<<"envelope "<<Fixed((*options.envelope)[0],1)<<" mm, "<<Fixed((*options.envelope)[1],1)<<" deg, "<<Fixed((*options.envelope)[2],1)<<" deg, "<<Fixed((*options.envelope)[3],1)
            <<" %; surface envelope "<<(options.surface_envelope?Fixed((*options.surface_envelope)[0],1)+" mm, "+Fixed((*options.surface_envelope)[1],1)+" deg":std::string("none"))
            <<"; estimator band half width "<<options.band_half_width<<" px, guard "<<options.guard<<" px; threads "<<options.threads<<"\n";
        text<<"limits: brown-lowe "<<options.bl_alpha<<"+"<<options.bl_beta<<"n, unique "<<options.min_unique<<", prediction sigma "<<options.max_prediction_sigma<<", rms "<<options.max_rms
            <<", p95 "<<options.max_p95<<", bias "<<options.max_bias<<", convergence "<<options.convergence<<", start agreement "<<options.start_agreement<<"\n\n";
        write_estimate(text);
        text<<"[time] (s)\n";
        for(std::size_t i=0;i<stages.size();++i)text<<"  "<<stages[i].first<<": "<<Fixed(stages[i].second,1)<<" (processor "<<Fixed(processor[i],1)<<")\n";
        std::ofstream(options.out/"pipeline.txt")<<text.str();
        std::cout<<text.str();
        return 0;
    }

    // 2 Layer 1 only (M0): the layout, and the band measured once for layer 2's points and the gate's anchors.
    const auto calibrated_baseline=pc::MakeBaselineGeometry(parameters,{},0.0);
    rg::RegistrationGateRequest g0;
    g0.camera_a=&image_a;g0.camera_b=&image_b;
    g0.registered=m2x::RegisteredMapping{pose,plane,std::nullopt};
    g0.mapped=m2x::RegisteredDocumentMappedFixedSeamInput(parameters,g0.registered,seam);
    g0.layout={seam,blend.width_pixels,options.omega_margin,options.guard,options.gate_band_half_width};
    g0.blend=blend;g0.gate_design=gate::kSeamGateDesignCalibrated;
    g0.gate_limits={options.seam_limit_px,options.seam_guard_px,"probe"};
    auto& gd=g0.design;   // the capture path's draft values (capture_registration's CaptureRegistrationDesignDraft)
    gd.minimum_baseline_component=0.5;gd.lift_ratio=3;gd.lift_share=0.9;
    gd.wide_search_radius_px=40;gd.wide_search_half_width_px=64;gd.wide_search_block_rows=96;gd.wide_search_minimum_correlation=0.5;
    gd.wide_search_secondary_ratio=0.8;gd.wide_search_consistency_px=1.5;gd.wide_search_minimum_blocks=2;gd.wide_search_trigger_share=0.5;
    gd.maximum_unmeasured_content_share=0.25;
    g0.baseline=calibrated_baseline.direction;g0.baseline_px_per_mm=calibrated_baseline.k_px_per_mm;g0.output_pixel_mm=25.4/parameters.raster.dpi;
    g0.wide_search=false;g0.measure_whole_band_for_anchors=true;g0.thread_count=options.threads;
    const auto layout0=rg::MakeRegistrationGateLayout(g0);
    pc::ParallaxDesign parallax_design=pc::kParallaxDesignDraft;
    if(options.parallax_max_strain>0)parallax_design.maximum_strain=options.parallax_max_strain;
    if(options.parallax_falloff>0)parallax_design.weight_falloff_px=options.parallax_falloff;
    auto band_design=gate::kSeamGateDesignCalibrated;
    band_design.point_spacing_px=static_cast<std::uint32_t>(parallax_design.column_spacing_px);
    band_design.patch_px=parallax_design.point_patch_px;
    gate::ValidateSeamGateDesign(band_design);
    stage("layout M0");
    const auto band0=gate::MeasureSeam(image_a,image_b,layout0.band,band_design,g0.gate_limits,{},options.threads);
    stage("band measurement M0 (layer 2 points and anchors)");

    // 3 Layer 2 (S4) on the gate's rows of Omega.
    pc::ParallaxRequest request;
    request.points=pc::PointsFromSeamMeasurement(band0);
    auto& rows=request.rows;
    rows.output_width=width;rows.output_height=height;rows.seam_column=seam;rows.omega_half_width_px=g0.layout.omega_half_width_px();
    rows.gate_footprint=pc::GateFootprint(g0.gate_design,true);
    {
        std::uint32_t y0=height,y1=0;
        for(std::uint32_t y=0;y<height;++y)if(layout0.omega.band[y].begin<layout0.omega.band[y].end){y0=std::min(y0,y);y1=y+1;}
        if(y0>=y1)Fail("Omega is empty");
        rows.row_step=g0.gate_design.point_spacing_px;
        rows.first_row=y0+rows.row_step/2;
        rows.row_count=(y1-1-rows.first_row)/rows.row_step+1;
    }
    request.baseline=pc::MakeBaselineGeometry(parameters,pose,plane.h0_mm);
    request.design=parallax_design;
    request.max_lift_mm=options.max_lift_mm;
    if(options.max_lift_mm)request.apply_beyond_line=options.apply_beyond_line;
    request.thread_count=options.threads;
    const auto parallax=pc::EstimateParallax(request);
    stage("layer 2 (S4)");

    // 4 The mapping with layer 2 (M1) and the gate on Omega (S7a), the band's anchors measured on M0.
    rg::RegistrationGateRequest g1=g0;
    if(parallax.Applied()){
        g1.registered.parallax=pc::ToParallaxProfile(parallax);
        g1.mapped=m2x::RegisteredDocumentMappedFixedSeamInput(parameters,g1.registered,seam);
        const auto profile=*g1.registered.parallax;
        g1.correction=[profile](render::Point at,render::Point& shift){
            const double s=m2x::ParallaxShiftAtRow(profile,at.y)*m2x::ParallaxSeamWeight(std::abs(at.x-static_cast<double>(profile.seam_column)),profile.a_px,profile.l_px);
            shift={s*profile.baseline.x,s*profile.baseline.y};return true;
        };
    }
    auto layout1=rg::MakeRegistrationGateLayout(g1);
    unsigned omega_rows_differ=0;
    for(std::uint32_t y=0;y<height;++y){
        const auto& a=layout0.omega.band[y];const auto& b=layout1.omega.band[y];
        if((a.begin<a.end)!=(b.begin<b.end))++omega_rows_differ;
    }
    layout1.band=layout0.band;   // the anchors' band was measured on M0 (the request allows it)
    stage("mapping M1 and layout");
    const auto omega1=gate::MeasureSeam(image_a,image_b,layout1.omega,g1.gate_design,g1.gate_limits,{},options.threads);
    stage("Omega measurement M1");
    const auto gate1=rg::DecideRegistrationGate(g1,layout1,{omega1,band0});
    stage("gate decision G1");
    // The same gate on layer 1 only (G2), for the comparison.
    const auto omega0=gate::MeasureSeam(image_a,image_b,layout0.omega,g0.gate_design,g0.gate_limits,{},options.threads);
    const auto gate0=rg::DecideRegistrationGate(g0,layout0,{omega0,band0});
    stage("Omega measurement and gate G2");

    // The seam column split by kind of point.
    const auto e_b=calibrated_baseline.direction;
    const auto seam_band0=probe::SeamComponentsOf(band0,seam,static_cast<std::uint32_t>(parallax_design.column_spacing_px),e_b,gd.minimum_baseline_component);
    const auto seam_omega0=probe::SeamComponentsOf(omega0,seam,g0.layout.omega_half_width_px(),e_b,gd.minimum_baseline_component);
    const auto seam_omega1=probe::SeamComponentsOf(omega1,seam,g0.layout.omega_half_width_px(),e_b,gd.minimum_baseline_component);

    fs::create_directories(options.out);
    std::ostringstream text;
    text<<"label: "<<options.label<<"\n";
    text<<"raster "<<width<<" x "<<height<<" px at "<<parameters.raster.dpi<<" dpi, seam column "<<seam<<", Omega half width "<<g0.layout.omega_half_width_px()
        <<" px, blend width "<<blend.width_pixels<<" px, threads "<<options.threads<<"\n";
    text<<"lines: seam limit "<<Fixed(options.seam_limit_px,2)<<" px, guard "<<Fixed(options.seam_guard_px,2)<<" px; envelope "<<Fixed((*options.envelope)[0],1)<<" mm, "
        <<Fixed((*options.envelope)[1],1)<<" deg, "<<Fixed((*options.envelope)[2],1)<<" deg, "<<Fixed((*options.envelope)[3],1)<<" %; surface envelope "
        <<(options.surface_envelope?Fixed((*options.surface_envelope)[0],1)+" mm, "+Fixed((*options.surface_envelope)[1],1)+" deg":std::string("none"))
        <<"; parallax line "<<(options.max_lift_mm?Fixed(*options.max_lift_mm,2)+" mm, apply beyond "+std::string(options.apply_beyond_line?"true":"false"):std::string("none"))<<"\n";
    text<<"estimator band half width "<<options.band_half_width<<" px, guard "<<options.guard<<" px; gate band half width "<<options.gate_band_half_width<<" px\n";
    text<<"design: estimator outer iterations "<<settings.design.maximum_outer_iterations<<", fine spacing "<<settings.design.fine_point_spacing_px<<"; layer 2 s_max "<<parallax_design.maximum_strain<<", a "<<parallax_design.weight_flat_px<<", L "<<parallax_design.weight_falloff_px<<"\n";
    text<<"limits: brown-lowe "<<options.bl_alpha<<"+"<<options.bl_beta<<"n, unique "<<options.min_unique<<", prediction sigma "<<options.max_prediction_sigma<<", rms "<<options.max_rms
        <<", p95 "<<options.max_p95<<", bias "<<options.max_bias<<", convergence "<<options.convergence<<", start agreement "<<options.start_agreement<<"\n\n";

    // 1
    write_estimate(text);
    // G2
    text<<"[2 layer 1 only, M0] band "<<options.gate_band_half_width<<" px each side, lattice "<<band_design.point_spacing_px<<" px, patch "<<band_design.patch_px<<" px: points "<<band0.points.size()<<"\n";
    text<<"  seam column (|x - x_s| <= "<<static_cast<unsigned>(parallax_design.column_spacing_px)<<" px) on the band lattice:\n";WriteComponents(text,"    ",seam_band0);
    text<<"  Omega on the gate's lattice (patch "<<g0.gate_design.patch_px<<"):\n";WriteComponents(text,"    ",seam_omega0);
    WriteGate(text,"  gate G2",gate0);
    text<<"\n";
    // 3
    {
        const auto& s=parallax.summary;
        text<<"[3 layer 2] state "<<pc::ParallaxStateCode(parallax.state)<<", reasons:";if(parallax.reasons.empty())text<<" none";for(const auto& r:parallax.reasons)text<<' '<<r;text<<"\n";
        text<<"  e_b ("<<Fixed(parallax.baseline.x,4)<<", "<<Fixed(parallax.baseline.y,4)<<"), k "<<Fixed(parallax.k_px_per_mm,4)<<" px/mm, gate rows "<<rows.row_count<<" from "<<rows.first_row
            <<" step "<<rows.row_step<<"\n";
        text<<"  q: max |q| "<<Fixed(s.max_shift_px)<<" px ("<<Fixed(s.max_shift_mm)<<" mm) at row "<<Fixed(s.at_row,0)<<", strain "<<Fixed(s.max_strain,5)<<", columns "<<s.first_column<<"-"<<s.last_column<<"\n";
        text<<"  points: input "<<s.points_input<<", with baseline component "<<s.points_baseline<<", used "<<s.points<<"; hole "<<Fixed(s.hole_x_px,0)<<" x "<<Fixed(s.hole_y_px,0)<<" px\n";
        text<<"  rows measured "<<s.rows_measured<<", filled "<<s.rows_filled;
        std::map<std::string,unsigned> filled;for(const auto& r:parallax.rows)if(r.state==pc::ParallaxRowState::filled)++filled[r.reason];
        if(!filled.empty()){text<<" (";bool first=true;for(const auto& [k,v]:filled){if(!first)text<<", ";first=false;text<<k<<" "<<v;}text<<")";}
        text<<"\n  lift: line "<<Num(parallax.lift.limit_mm)<<" mm, max |lift| "<<Fixed(parallax.lift.maximum_mm)<<" mm at row "<<Fixed(parallax.lift.at_row,0)<<", exceeded "<<(parallax.lift.exceeded?"yes":"no");
        if(parallax.lift.exceeded)text<<" ("<<parallax.lift.exceeded_rows<<" rows, "<<Fixed(parallax.lift.first_exceeded_row,0)<<"-"<<Fixed(parallax.lift.last_exceeded_row,0)<<")";
        text<<"\n  folds: "<<parallax.folds.size()<<" (cross-validated points "<<s.points_cross_validated<<", candidates "<<s.fold_candidates<<", scale "<<Fixed(s.fold_residual_scale_px)<<" px)\n";
        for(const auto& f:parallax.folds)
            text<<"    "<<pc::FoldLineKindCode(f.kind)<<" at row "<<Fixed(f.position_px,0)<<", angle "<<Fixed(f.angle_deg,1)<<" deg, step "<<Fixed(f.step_px)<<" px ("<<Fixed(f.step_mm)<<" mm), from seam "
                <<Fixed(f.distance_from_seam_px,0)<<" px"<<(f.crosses_seam?", crosses":"")<<(f.near_seam?", NEAR SEAM":"")<<", length "<<Fixed(f.length_px,0)<<" px, points "<<f.points
                <<", ends ("<<Fixed(f.first.x,0)<<","<<Fixed(f.first.y,0)<<")-("<<Fixed(f.last.x,0)<<","<<Fixed(f.last.y,0)<<")"
                <<(f.shift_to_clear_px?", shift to clear "+Fixed(*f.shift_to_clear_px,0)+" px":std::string())<<"\n";
        text<<"\n";
    }
    // G1
    text<<"[4 layer 2 applied, M1] Omega rows whose emptiness differs from M0: "<<omega_rows_differ<<"\n";
    text<<"  Omega on the gate's lattice:\n";WriteComponents(text,"    ",seam_omega1);
    WriteGate(text,"  gate G1",gate1);
    const bool reached=gate1.state==rg::GateState::pass;
    text<<"  verdict against the line "<<Fixed(options.seam_limit_px,2)<<" px (guard "<<Fixed(options.seam_guard_px,2)<<"): "<<(reached?"PASS":"NOT REACHED")<<"\n\n";

    WritePoints(options.out/"band_m0.csv",band0);
    WritePoints(options.out/"omega_m0.csv",omega0);
    WritePoints(options.out/"omega_m1.csv",omega1);
    {
        std::ofstream out(options.out/"parallax_rows.csv");
        out<<"y,q,sigma_q,n_eff,points,lift_mm,state,reason\n";
        for(const auto& r:parallax.rows)out<<Fixed(r.y,1)<<','<<Csv(r.q)<<','<<Csv(r.sigma_q)<<','<<Csv(r.n_eff)<<','<<r.points<<','<<Csv(r.lift_mm)<<','<<pc::ParallaxRowStateCode(r.state)<<','<<r.reason<<"\n";
    }

    // Chart markers: their two placements through the very mappings, independent of the measurer.
    if(!options.markers.empty()){
        const auto markers=ReadMarkers(options.markers);
        const probe::OutputInverse inv_a(g0.mapped.camera_a,width,height,16),inv_b0(g0.mapped.camera_b,width,height,16),inv_b1(g1.mapped.camera_b,width,height,16);
        const render::Point e_perp{-e_b.y,e_b.x};
        struct Set {const char* name;double reach;std::vector<double> m0,m1,along1,across1;};
        Set sets[]{{"Omega (|u| <= 12)",12.0},{"flat weight (|u| <= 48)",48.0},{"layer 2 reach (|u| <= 240)",240.0},{"all",1e9}};
        std::ofstream out(options.out/"markers.csv");
        out<<"id,x,y,u,d0x,d0y,d1x,d1y,along1,across1\n";
        unsigned placed=0;
        for(const auto& mk:markers){
            const auto oa=inv_a.Find(mk.a,1e-6),ob0=inv_b0.Find(mk.b,1e-6),ob1=inv_b1.Find(mk.b,1e-6);
            if(!oa || !ob0 || !ob1)continue;
            ++placed;
            const render::Point d0{ob0->x-oa->x,ob0->y-oa->y},d1{ob1->x-oa->x,ob1->y-oa->y};
            const double u=oa->x-seam,along=d1.x*e_b.x+d1.y*e_b.y,across=d1.x*e_perp.x+d1.y*e_perp.y;
            out<<mk.id<<','<<Fixed(oa->x,2)<<','<<Fixed(oa->y,2)<<','<<Fixed(u,2)<<','<<Fixed(d0.x,4)<<','<<Fixed(d0.y,4)<<','<<Fixed(d1.x,4)<<','<<Fixed(d1.y,4)
               <<','<<Fixed(along,4)<<','<<Fixed(across,4)<<"\n";
            for(auto& s:sets)if(std::abs(u)<=s.reach){s.m0.push_back(std::hypot(d0.x,d0.y));s.m1.push_back(std::hypot(d1.x,d1.y));s.along1.push_back(std::abs(along));s.across1.push_back(across);}
        }
        text<<"[markers] read "<<markers.size()<<", placed through A, B(M0) and B(M1): "<<placed<<"\n";
        for(const auto& s:sets){
            if(s.m1.empty()){text<<"  "<<s.name<<": no marker\n";continue;}
            const auto a=probe::StatsOf(s.m0),b=probe::StatsOf(s.m1),c=probe::StatsOf(s.along1);
            double sq=0;for(const double v:s.across1)sq+=v*v;
            text<<"  "<<s.name<<": n "<<b.count<<"; |d| M0 mean "<<Fixed(a.mean)<<" max "<<Fixed(a.maximum)<<"; |d| M1 mean "<<Fixed(b.mean)<<" p95 "<<Fixed(b.p95)<<" max "<<Fixed(b.maximum)
                <<"; along e_b max "<<Fixed(c.maximum)<<", across rms "<<Fixed(std::sqrt(sq/s.across1.size()))<<" px\n";
        }
        text<<"\n";
        stage("markers");
    }

    // The composite the gate measured (G1) and, for comparison, layer 1 only (G2). Colour: the default match (rgb-paper).
    if(options.render){
        const fs::path view=options.view.empty()?options.out/"view":options.view;
        fs::create_directories(view);
        const std::uint32_t mid=rows.first_row+(rows.row_count/2)*rows.row_step,top=rows.first_row+256,bottom=rows.first_row+(rows.row_count-1)*rows.row_step-256;
        const std::array<std::pair<const char*,std::uint32_t>,3> where{{{"top",top},{"middle",mid},{"bottom",bottom}}};
        const auto composite1=m2x::RenderMappedFixedSeam(image_a,image_b,g1.mapped,seam,blend);
        stage("render G1");
        const auto quarter=Shrink(composite1.image,4);
        SaveJpeg(view/(options.label+"-g1-full-quarter.jpg"),quarter.width,quarter.height,quarter.bgr);
        for(const auto& [name,y]:where){
            const auto c=CropOf(composite1.image,static_cast<int>(seam)-512,static_cast<int>(y)-256,1024,512);
            SaveJpeg(view/(options.label+"-g1-seam-"+name+"-row"+std::to_string(y)+".jpg"),c.width,c.height,c.bgr,0.95f);
        }
        const auto composite0=m2x::RenderMappedFixedSeam(image_a,image_b,g0.mapped,seam,blend);
        for(const auto& [name,y]:where){
            const auto c=CropOf(composite0.image,static_cast<int>(seam)-512,static_cast<int>(y)-256,1024,512);
            SaveJpeg(view/(options.label+"-g2-seam-"+name+"-row"+std::to_string(y)+".jpg"),c.width,c.height,c.bgr,0.95f);
        }
        stage("render G2 and write the views");
        text<<"[view] "<<view.string()<<": "<<options.label<<"-g1-full-quarter.jpg, seam crops 1024 x 512 (seam at x 512) at rows "<<top<<", "<<mid<<", "<<bottom<<" for G1 and G2\n\n";
    }
    text<<"[time] (s; in brackets the processor seconds of all threads)";
    for(std::size_t i=0;i<stages.size();++i)text<<"\n  "<<stages[i].first<<": "<<Fixed(stages[i].second,1)<<" ["<<Fixed(processor[i],1)<<"]";
    text<<"\n  total: "<<Fixed(Seconds(start),1)<<"\n";
    std::ofstream(options.out/"pipeline.txt")<<text.str();
    std::cout<<text.str();
    return 0;
}

int RunCompare(const Options& options){
    const auto loaded=LoadCondition(options.condition);
    const auto before=ReadField(options.field_before),after=ReadField(options.field_after);
    const auto difference=probe::DifferenceObservations(before,after);
    std::cerr<<"before "<<before.size()<<", after "<<after.size()<<", joined "<<difference.size()<<"\n";
    if(difference.empty())Fail("the two fields share no point");
    MeasurementNotes notes;
    AnalyzeAndWrite(options,loaded.profile.parameters,loaded.condition.seam.column,difference,notes);
    return 0;
}
}

int main(int argc,char** argv){
    try{
        const auto options=Parse(argc,argv);
        if(options.pipeline)return RunPipeline(options);
        if(options.estimate)return RunEstimate(options);
        return options.field_before.empty()?RunMeasure(options):RunCompare(options);
    }
    catch(const std::exception& e){std::cerr<<"error: "<<e.what()<<"\n";return 1;}
}
