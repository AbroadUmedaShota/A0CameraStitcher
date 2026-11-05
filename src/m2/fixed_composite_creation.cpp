#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/m2/optical_planner.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace a0::m2::experimental {
namespace {
constexpr const char* kPrefix="fixed composite creation: ";
[[noreturn]] void Reject(const std::string& what){throw std::invalid_argument(kPrefix+what);}
// The reason of a failed check without its own prefix, to put the scale in front of it.
std::string Reason(const std::invalid_argument& e){
    std::string text=e.what();
    if(text.starts_with(kPrefix))text.erase(0,std::char_traits<char>::length(kPrefix));
    return text;
}

std::string Text(double v){
    char text[40];const auto r=std::to_chars(text,text+sizeof text,v);
    return std::string(text,r.ptr);
}

// A frame of the fixed sensor size. The checks use only the size of a frame and the
// blank pixels are never looked at; one object serves as both A and B.
render::BgrImage BlankFrame(){
    render::BgrImage frame;
    frame.width=static_cast<std::uint32_t>(kD810SensorWidthPixels);frame.height=static_cast<std::uint32_t>(kD810SensorHeightPixels);
    frame.bgr.assign(render::PixelCount(frame.width,frame.height)*3,0);
    return frame;
}

bool Covers(const render::OutputToCamera& map,std::uint32_t x,std::uint32_t y){
    render::Point raw{};
    if(!map({static_cast<double>(x),static_cast<double>(y)},raw))return false;
    if(!std::isfinite(raw.x) || !std::isfinite(raw.y))throw std::invalid_argument("output-to-camera mapping returned nonfinite camera coordinates");
    return raw.x>=0.0 && raw.y>=0.0 && raw.x<static_cast<double>(kD810SensorWidthPixels) && raw.y<static_cast<double>(kD810SensorHeightPixels);
}

void RequireSeam(const render::BgrImage& frame,const MappedFixedSeamInput& input,std::uint32_t seam_column){
    bool fits=false;
    try{fits=CanRenderMappedFixedSeam(frame,frame,input,seam_column,kSeamBlendDesign);}
    catch(const std::invalid_argument& e){Reject(std::string("the overlap of A and B cannot carry a seam: ")+e.what());}
    if(!fits)Reject("seam column "+std::to_string(seam_column)+" and half the blend width ("+std::to_string((kSeamBlendDesign.width_pixels-1)/2)+
        " columns) are not inside the overlap of A and B on every row");
}
}

std::vector<double> FixedCompositeCheckedScales(double percent){
    if(!std::isfinite(percent) || percent<0 || percent>=100)Reject("the scale limit must be finite and in [0,100) percent");
    const double limit=percent/100;
    if(limit==0)return {1.0};
    return {1.0-limit,1.0,1.0+limit};
}

void RequireFixedCompositeCoverage(const MappedFixedSeamInput& input){
    if(!input.camera_a || !input.camera_b || !input.width || !input.height)Reject("coverage needs both mappings and a non-empty raster");
    for(std::uint32_t y=0;y<input.height;++y){
        std::uint32_t a_end=0;
        while(a_end<input.width && Covers(input.camera_a,a_end,y))++a_end;
        if(a_end==0)Reject("row "+std::to_string(y)+": camera A does not cover the first column, so it is not the left camera there");
        if(a_end==input.width)continue; // A alone covers the row
        std::uint32_t b_start=input.width;
        while(b_start>a_end && Covers(input.camera_b,b_start-1,y))--b_start;
        if(b_start>a_end)Reject("row "+std::to_string(y)+": columns "+std::to_string(a_end)+" to "+std::to_string(b_start-1)+" are covered by neither camera");
    }
}

void RequireFixedCompositeSeam(const MappedFixedSeamInput& input,std::uint32_t seam_column){
    RequireSeam(BlankFrame(),input,seam_column);
}

FixedCompositeBand FixedCompositeBandOfRows(const std::vector<MappedOverlapRow>& rows){
    if(rows.empty())Reject("no rows");
    std::uint32_t first=0,end=std::numeric_limits<std::uint32_t>::max();
    for(const auto& r:rows){first=std::max(first,r.first);end=std::min(end,r.end);}
    if(!(end>first))Reject("no column is covered by both cameras on every row");
    return {first,end};
}

std::uint32_t FixedCompositeBandCenter(const FixedCompositeBand& band){
    if(!(band.last_exclusive>band.first))Reject("empty band");
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(band.first)+band.last_exclusive)/2);
}

namespace {
FixedCompositeBand UnitBand(const render::BgrImage& frame,const render::DocumentRenderParameters& parameters){
    return FixedCompositeBandOfRows(MappedFixedSeamBand(frame,frame,ScaledDocumentMappedFixedSeamInput(parameters,1.0)));
}
}

FixedCompositeBand FixedCompositeBandAtUnitScale(const render::DocumentRenderParameters& parameters){
    return UnitBand(BlankFrame(),parameters);
}

std::uint32_t FixedCompositeProvisionalSeamColumn(const render::DocumentRenderParameters& parameters){
    return FixedCompositeBandCenter(FixedCompositeBandAtUnitScale(parameters));
}

FixedCompositeCreationCheck MakeFixedCompositeCreationCheck(){
    return [](const FixedCompositeConditionV2& c,const FixedCompositeProfileBinding& binding){
        const auto frame=BlankFrame();
        for(const double scale:FixedCompositeCheckedScales(c.scale_auto_correction_max_percent)){
            try{
                const auto input=ScaledDocumentMappedFixedSeamInput(binding.parameters,scale);
                RequireFixedCompositeCoverage(input);
                RequireSeam(frame,input,c.seam.column);
            }catch(const std::invalid_argument& e){
                Reject("at scale "+Text(scale)+" of camera B: "+Reason(e));
            }
        }
        if(c.seam.origin==FixedSeamOrigin::band_center_provisional){
            const auto expected=FixedCompositeBandCenter(UnitBand(frame,binding.parameters));
            if(c.seam.column!=expected)
                Reject("a provisional seam column is the middle of the band ("+std::to_string(expected)+"), not "+std::to_string(c.seam.column));
        }
    };
}

double FixedCompositeCapturePixelsPerMm(const render::CameraProjection& camera,render::Point document_mm){
    constexpr double step=0.5; // mm each side
    render::Point xp{},xm{},yp{},ym{};
    if(!render::TryProjectDocument(camera,{document_mm.x+step,document_mm.y},xp) || !render::TryProjectDocument(camera,{document_mm.x-step,document_mm.y},xm) ||
        !render::TryProjectDocument(camera,{document_mm.x,document_mm.y+step},yp) || !render::TryProjectDocument(camera,{document_mm.x,document_mm.y-step},ym))
        throw std::invalid_argument("document projection is not numerically representable");
    const double a=(xp.x-xm.x)/(2*step),b=(yp.x-ym.x)/(2*step),c=(xp.y-xm.y)/(2*step),d=(yp.y-ym.y)/(2*step);
    return std::sqrt(std::abs(a*d-b*c));
}
}
