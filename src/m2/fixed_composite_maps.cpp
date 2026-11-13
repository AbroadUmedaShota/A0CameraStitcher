#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/optical_planner.hpp"
#include "calibration_lens_model.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <stdexcept>

namespace a0::m2::experimental {
namespace {
// Stopping rules of the lens inverse. Numerical only: Newton converges quadratically,
// so these stop it at double precision; they accept or reject no optical result.
constexpr std::uint32_t kInverseMaximumIterations=100;
constexpr double kInverseTolerancePixels=1e-9;
// The baseline of a profile is a computed unit vector; this only catches one that is not normalized.
constexpr double kBaselineUnitTolerance=1e-9;

void RequireScale(double scale){
    if(!std::isfinite(scale) || !(scale>0))throw std::invalid_argument("fixed composite scale must be finite and positive");
}
void Require(bool condition,const char* message){
    if(!condition)throw std::invalid_argument(message);
}
double LargestShift(const ParallaxProfile& profile){
    double largest=0;
    for(const double q:profile.q_px)largest=std::max(largest,std::abs(q));
    return largest;
}
}

bool TryDocumentPointOfRawPixel(const render::CameraProjection& camera,render::Point raw_pixel,render::Point& document_mm){
    const auto& i=camera.intrinsics;const auto& d=camera.distortion;
    if(!std::isfinite(raw_pixel.x) || !std::isfinite(raw_pixel.y) || !(i.fx_pixels>0) || !(i.fy_pixels>0))return false;
    const calibration::PinholeIntrinsics k{i.fx_pixels,i.fy_pixels,i.cx_pixels,i.cy_pixels};
    const calibration::BrownConradyDistortion lens{d.k1,d.k2,d.k3,d.p1,d.p2};
    const auto undistorted=calibration::lens_detail::UndistortPixel(k,lens,raw_pixel.x,raw_pixel.y,kInverseMaximumIterations,kInverseTolerancePixels);
    if(!undistorted)return false;
    render::Matrix3 inverse{};
    try{inverse=render::Invert(camera.document_to_image);}catch(const std::invalid_argument&){return false;}
    return render::TryTransform(inverse,{undistorted->x,undistorted->y},document_mm);
}

render::Point FixedCompositeScaleCenterMm(const render::CameraProjection& camera_b){
    const render::Point grid_center{(static_cast<double>(kD810SensorWidthPixels)-1)/2,(static_cast<double>(kD810SensorHeightPixels)-1)/2};
    render::Point center{};
    if(!TryDocumentPointOfRawPixel(camera_b,grid_center,center))
        throw std::invalid_argument("the center of camera B's pixel grid has no document point");
    return center;
}

render::OutputToCamera ScaledDocumentOutputToCamera(const render::OutputRaster& raster,const render::CameraProjection& camera,
    render::Point center_mm,double scale){
    RequireScale(scale);
    if(!std::isfinite(center_mm.x) || !std::isfinite(center_mm.y))throw std::invalid_argument("fixed composite scale center must be finite");
    auto unscaled=render::DocumentOutputToCamera(raster,camera); // validates the raster
    if(scale==1.0)return unscaled;
    // The document point of an output sample is recomputed with the arithmetic of
    // DocumentOutputToCamera, then scaled; the projection is the same function.
    const double pitch=25.4/raster.dpi;
    const double left=static_cast<double>(raster.region_um.left)/1000.0,top=static_cast<double>(raster.region_um.top)/1000.0;
    return [camera,pitch,left,top,center_mm,scale](render::Point output,render::Point& raw){
        const render::Point document{left+(output.x+.5)*pitch,top+(output.y+.5)*pitch};
        const render::Point scaled{center_mm.x+(document.x-center_mm.x)/scale,center_mm.y+(document.y-center_mm.y)/scale};
        if(!render::TryProjectDocument(camera,scaled,raw))throw std::invalid_argument("document projection is not numerically representable");
        return true;
    };
}

MappedFixedSeamInput ScaledDocumentMappedFixedSeamInput(const render::DocumentRenderParameters& parameters,double scale){
    RequireScale(scale);
    auto input=DocumentMappedFixedSeamInput(parameters);
    if(scale!=1.0)input.camera_b=ScaledDocumentOutputToCamera(parameters.raster,parameters.camera_b,FixedCompositeScaleCenterMm(parameters.camera_b),scale);
    return input;
}
render::OutputToCamera HomographyCorrectedDocumentOutputToCamera(const render::OutputRaster& raster,const render::CameraProjection& camera,
    const render::Matrix3& g){
    for(const double v:g)if(!std::isfinite(v))throw std::invalid_argument("the document homography of a pose must be finite");
    (void)render::DocumentOutputToCamera(raster,camera); // validates the raster
    const double pitch=25.4/raster.dpi;
    const double left=static_cast<double>(raster.region_um.left)/1000.0,top=static_cast<double>(raster.region_um.top)/1000.0;
    // The document point of an output sample is recomputed with the arithmetic of DocumentOutputToCamera, then moved;
    // the projection is the same function.
    return [camera,pitch,left,top,g](render::Point output,render::Point& raw){
        const render::Point document{left+(output.x+.5)*pitch,top+(output.y+.5)*pitch};
        render::Point moved{};
        if(!render::TryTransform(g,document,moved))return false;
        if(!render::TryProjectDocument(camera,moved,raw))throw std::invalid_argument("document projection is not numerically representable");
        return true;
    };
}

render::OutputToCamera PoseCorrectedDocumentOutputToCamera(const render::OutputRaster& raster,const render::CameraProjection& camera,
    const content_registration::PoseModel& model,const content_registration::PoseParameters& pose){
    for(const double v:{pose.rx_deg,pose.ry_deg,pose.rz_deg,pose.tx_mm,pose.ty_mm,pose.tz_percent})
        if(!std::isfinite(v))throw std::invalid_argument("a pose must be finite");
    if(pose.IsZero())return render::DocumentOutputToCamera(raster,camera); // the calibrated mapping, bit for bit
    return HomographyCorrectedDocumentOutputToCamera(raster,camera,model.Homography(pose));
}

MappedFixedSeamInput PoseCorrectedDocumentMappedFixedSeamInput(const render::DocumentRenderParameters& parameters,
    const content_registration::PoseParameters& pose){
    auto input=DocumentMappedFixedSeamInput(parameters);
    if(!pose.IsZero()){
        const content_registration::PoseModel model(parameters.camera_b);
        input.camera_b=PoseCorrectedDocumentOutputToCamera(parameters.raster,parameters.camera_b,model,pose);
    }
    return input;
}

// ---- Layer 2 pieces ------------------------------------------------------------------------------------------------------------
double ParallaxSeamWeight(double u,double a,double l){
    Require(std::isfinite(u) && u>=0,"parallax weight: the distance must be finite and not negative");
    Require(std::isfinite(a) && a>=0,"parallax weight: a must be finite and not negative");
    Require(std::isfinite(l) && l>0,"parallax weight: L must be finite and positive");
    if(u<=a)return 1.0;
    if(u>=a+l)return 0.0;
    return 0.5*(1.0+std::cos(std::numbers::pi*(u-a)/l));
}

double ParallaxShiftAtRow(const ParallaxProfile& profile,double y){
    const auto& q=profile.q_px;
    Require(!q.empty(),"parallax profile: no samples");
    Require(std::isfinite(profile.first_row_px),"parallax profile: the first row must be finite");
    Require(std::isfinite(profile.row_spacing_px) && profile.row_spacing_px>0,"parallax profile: the row spacing must be finite and positive");
    Require(std::isfinite(y),"parallax profile: the row must be finite");
    const std::size_t n=q.size();
    // The position in sample units: sample i is at i.
    const double s=(y-profile.first_row_px)/profile.row_spacing_px;
    if(n==1 || s<=0)return q.front();
    if(s>=static_cast<double>(n-1))return q.back();
    const auto k=std::min(static_cast<std::size_t>(std::floor(s)),n-2);
    const double t=s-static_cast<double>(k);
    // Fritsch-Carlson tangents (per sample interval): the harmonic mean of the neighbouring secants where they have the same
    // sign, 0 at a local extremum and at the first and last sample. They keep the interpolant monotone on each interval, and
    // the zero end tangents make it C1 with the constant end values beyond the samples.
    const auto secant=[&q](std::size_t i){return q[i+1]-q[i];};
    const auto tangent=[&](std::size_t i){
        if(i==0 || i==n-1)return 0.0;
        const double left=secant(i-1),right=secant(i);
        if(!(left*right>0))return 0.0;
        return 2.0*left*right/(left+right);
    };
    const double t2=t*t,t3=t2*t;
    const double h00=2*t3-3*t2+1,h10=t3-2*t2+t,h01=-2*t3+3*t2,h11=t3-t2;
    return h00*q[k]+h10*tangent(k)+h01*q[k+1]+h11*tangent(k+1);
}

void ValidateParallaxProfile(const ParallaxProfile& profile){
    Require(!profile.q_px.empty(),"parallax profile: no samples");
    for(const double q:profile.q_px)Require(std::isfinite(q),"parallax profile: a sample must be finite");
    Require(std::isfinite(profile.first_row_px),"parallax profile: the first row must be finite");
    Require(std::isfinite(profile.row_spacing_px) && profile.row_spacing_px>0,"parallax profile: the row spacing must be finite and positive");
    Require(std::isfinite(profile.a_px) && profile.a_px>=0,"parallax profile: a must be finite and not negative");
    Require(std::isfinite(profile.l_px) && profile.l_px>0,"parallax profile: L must be finite and positive");
    const auto& e=profile.baseline;
    Require(std::isfinite(e.x) && std::isfinite(e.y),"parallax profile: the baseline must be finite");
    Require(std::abs(std::hypot(e.x,e.y)-1.0)<=kBaselineUnitTolerance,"parallax profile: the baseline must have unit length");
}

double ParallaxMaximumStrain(const ParallaxProfile& profile){
    Require(!profile.q_px.empty(),"parallax profile: no samples");
    for(const double q:profile.q_px)Require(std::isfinite(q),"parallax profile: a sample must be finite");
    Require(std::isfinite(profile.l_px) && profile.l_px>0,"parallax profile: L must be finite and positive");
    return std::numbers::pi/2*LargestShift(profile)/profile.l_px;
}

// ---- The registered mapping ------------------------------------------------------------------------------------------------------
namespace {
// What both makers check before building anything, so that a pose-only mapping is refused for the same inputs.
void ValidateRegisteredMapping(const render::OutputRaster& raster,const RegisteredMapping& mapping,std::uint32_t seam_column){
    render::ValidateOutputRaster(raster);
    Require(seam_column<raster.width_pixels,"registered mapping: the seam column is outside the raster");
    const auto& p=mapping.pose;
    for(const double v:{p.rx_deg,p.ry_deg,p.rz_deg,p.tx_mm,p.ty_mm,p.tz_percent})Require(std::isfinite(v),"a pose must be finite");
    const auto& s=mapping.surface;
    Require(std::isfinite(s.h0_mm) && std::isfinite(s.slope_x) && std::isfinite(s.slope_y),"registered mapping: the surface plane must be finite");
    if(mapping.parallax){
        ValidateParallaxProfile(*mapping.parallax);
        Require(mapping.parallax->seam_column==seam_column,"registered mapping: the profile is for another seam column");
    }
}
}

render::OutputToCamera RegisteredDocumentOutputToCamera(const render::OutputRaster& raster,const render::CameraProjection& camera_a,
    const render::CameraProjection& camera_b,const content_registration::PoseModel& model_b,const RegisteredMapping& mapping,
    std::uint32_t seam_column){
    ValidateRegisteredMapping(raster,mapping,seam_column);
    const auto& p=mapping.pose;
    const auto& s=mapping.surface;
    if(mapping.IsPoseOnly())return PoseCorrectedDocumentOutputToCamera(raster,camera_b,model_b,p);   // bit for bit

    const bool layer1=!s.IsZero();
    const bool layer2=mapping.parallax && !mapping.parallax->IsZero();
    const bool transformed=layer1 || !p.IsZero();
    render::Matrix3 h{};
    if(layer1){
        const auto centre_a=content_registration::PoseModel(camera_a).Centre({});
        const auto origin=content_registration::SurfaceOriginMm(raster,seam_column);
        // Pi is the physical construction only where both cameras see the surface; the conditions are affine in the document
        // point, so the corners of the raster widened by the largest shift of layer 2 cover every point the mapping uses.
        const double reach=(layer2?LargestShift(*mapping.parallax):0.0)+1.0;
        const auto frame=content_registration::FrameOfRaster(raster);
        const auto centre_b=model_b.Centre(p);
        for(const double ix:{-reach,static_cast<double>(raster.width_pixels)+reach})
            for(const double iy:{-reach,static_cast<double>(raster.height_pixels)+reach})
                Require(content_registration::SurfaceHomographyHoldsAt(centre_a,centre_b,origin,s,{frame.left_mm+ix*frame.pitch_mm,frame.top_mm+iy*frame.pitch_mm}),
                    "registered mapping: the surface is not below both cameras over the raster");
        h=content_registration::RegisteredHomography(model_b,centre_a,origin,p,s);
    }else if(transformed){
        h=model_b.Homography(p);
    }
    for(const double v:h)Require(std::isfinite(v),"the document homography of a registered mapping must be finite");
    const double pitch=25.4/raster.dpi;
    const double left=static_cast<double>(raster.region_um.left)/1000.0,top=static_cast<double>(raster.region_um.top)/1000.0;
    std::shared_ptr<const ParallaxProfile> profile;
    if(layer2)profile=std::make_shared<const ParallaxProfile>(*mapping.parallax);
    const double seam=static_cast<double>(seam_column);
    // The output point is moved along e_b first, then put through render::DocumentOutputToCamera's arithmetic and the document
    // homography, as in HomographyCorrectedDocumentOutputToCamera.
    return [camera_b,pitch,left,top,h,transformed,profile,seam](render::Point output,render::Point& raw){
        if(profile){
            const double w=ParallaxSeamWeight(std::abs(output.x-seam),profile->a_px,profile->l_px);
            if(w!=0){
                const double shift=ParallaxShiftAtRow(*profile,output.y)*w;
                output.x+=shift*profile->baseline.x;
                output.y+=shift*profile->baseline.y;
            }
        }
        const render::Point document{left+(output.x+.5)*pitch,top+(output.y+.5)*pitch};
        render::Point moved=document;
        if(transformed && !render::TryTransform(h,document,moved))return false;
        if(!render::TryProjectDocument(camera_b,moved,raw))throw std::invalid_argument("document projection is not numerically representable");
        return true;
    };
}

MappedFixedSeamInput RegisteredDocumentMappedFixedSeamInput(const render::DocumentRenderParameters& parameters,const RegisteredMapping& mapping,
    std::uint32_t seam_column){
    ValidateRegisteredMapping(parameters.raster,mapping,seam_column);
    if(mapping.IsPoseOnly())return PoseCorrectedDocumentMappedFixedSeamInput(parameters,mapping.pose);
    auto input=DocumentMappedFixedSeamInput(parameters);
    const content_registration::PoseModel model(parameters.camera_b);
    input.camera_b=RegisteredDocumentOutputToCamera(parameters.raster,parameters.camera_a,parameters.camera_b,model,mapping,seam_column);
    return input;
}
}
