#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/optical_planner.hpp"
#include "calibration_lens_model.hpp"

#include <cmath>
#include <stdexcept>

namespace a0::m2::experimental {
namespace {
// Stopping rules of the lens inverse. Numerical only: Newton converges quadratically,
// so these stop it at double precision; they accept or reject no optical result.
constexpr std::uint32_t kInverseMaximumIterations=100;
constexpr double kInverseTolerancePixels=1e-9;

void RequireScale(double scale){
    if(!std::isfinite(scale) || !(scale>0))throw std::invalid_argument("fixed composite scale must be finite and positive");
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
}
