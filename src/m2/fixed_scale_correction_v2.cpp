#include "a0/m2/experimental/fixed_scale_correction_v2.hpp"
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/m2/optical_planner.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <vector>

namespace a0::m2::experimental {
namespace {
FixedScaleReason ReasonOf(SimilarityUndeterminedReason reason){
    switch(reason){
    case SimilarityUndeterminedReason::no_tiles:return FixedScaleReason::no_tiles;
    case SimilarityUndeterminedReason::insufficient_tiles:return FixedScaleReason::insufficient_tiles;
    case SimilarityUndeterminedReason::outside_image:return FixedScaleReason::outside_image;
    case SimilarityUndeterminedReason::tied_starts:return FixedScaleReason::tied_starts;
    case SimilarityUndeterminedReason::none:break;
    }
    return FixedScaleReason::degenerate_fit;
}

// q.x, q.y (B's document point from P), t.x, t.y (A's document point from P), in mm.
struct UsedTile {double qx,qy,tx,ty;unsigned row;};

// The document points of one agreed tile, or false when either has none or the tile is not
// within the estimator's search window.
bool UseTile(const SimilarityTile& tile,const FixedCompositeConditionV2& condition,const render::DocumentRenderParameters& parameters,
    unsigned width,unsigned height,double radius_pixels,render::Point center,UsedTile& used){
    const auto raw_a=FixedScaleRawPixelOfTile(tile.a_x,tile.a_y,condition.orientation,width,height);
    const auto raw_b=FixedScaleRawPixelOfTile(tile.b_x,tile.b_y,condition.orientation,width,height);
    render::Point xa{},xb{};
    if(!TryDocumentPointOfRawPixel(parameters.camera_a,raw_a,xa) || !TryDocumentPointOfRawPixel(parameters.camera_b,raw_b,xb))return false;
    double pixels_per_mm=0;
    try{pixels_per_mm=FixedCompositeCapturePixelsPerMm(parameters.camera_a,xa);}
    catch(const std::invalid_argument&){return false;}
    if(!(pixels_per_mm>0) || !std::isfinite(pixels_per_mm))return false;
    if(std::hypot(xa.x-xb.x,xa.y-xb.y)>radius_pixels/pixels_per_mm)return false;
    used={xb.x-center.x,xb.y-center.y,xa.x-center.x,xa.y-center.y,tile.row};
    return true;
}
}

render::Point FixedScaleRawPixelOfTile(double tile_x,double tile_y,MountOrientation orientation,unsigned frame_width,unsigned frame_height){
    (void)MountOrientationName(orientation);
    const double x=tile_x-0.5,y=tile_y-0.5;
    if(orientation==MountOrientation::rotated_180)return {(static_cast<double>(frame_width)-1)-x,(static_cast<double>(frame_height)-1)-y};
    return {x,y};
}

FixedScaleCorrectionV2 DecideFixedScaleV2(const FixedCompositeConditionV2& condition,const FixedCompositeProfileBinding& profile,
    const OverlapSimilarityEstimate& e,unsigned frame_width,unsigned frame_height){
    ValidateFixedCompositeConditionV2(condition);
    if(profile.profile_fingerprint_sha256!=condition.source.profile_fingerprint_sha256)
        throw std::invalid_argument("the profile binding is not the profile the fixed composite condition names");
    if(frame_width!=kD810SensorWidthPixels || frame_height!=kD810SensorHeightPixels)
        throw std::invalid_argument("the scale estimate must come from frames of the size the rig profile fixes");
    const auto& parameters=profile.parameters;
    // The one centre of the correction, the one the drawn mapping uses.
    const auto center=FixedCompositeScaleCenterMm(parameters.camera_b);
    const double pitch=25.4/condition.raster.dpi;
    const double left=static_cast<double>(condition.raster.region_um.left)/1000.0,top=static_cast<double>(condition.raster.region_um.top)/1000.0;

    FixedScaleCorrectionV2 out;
    out.limit_percent=condition.scale_auto_correction_max_percent;
    out.center_mm=center;
    out.center_output_pixels={(center.x-left)/pitch-0.5,(center.y-top)/pitch-0.5};
    out.status=FixedScaleStatus::not_estimated;
    if(e.status!=OverlapEstimateStatus::estimated_not_judged){out.reason=ReasonOf(e.reason);return out;}

    std::vector<UsedTile> used;
    std::set<unsigned> rows;
    for(const auto& tile:e.tiles){
        UsedTile u{};
        if(!tile.agreed || !UseTile(tile,condition,parameters,frame_width,frame_height,e.search_radius,center,u))continue;
        used.push_back(u);rows.insert(u.row);
    }
    out.used_tiles=static_cast<unsigned>(used.size());
    out.used_rows=static_cast<unsigned>(rows.size());
    if(!rows.empty()){out.first_used_row=*rows.begin();out.last_used_row=*rows.rbegin();}
    if(used.size()<3 || rows.size()<2){out.reason=FixedScaleReason::too_few_tiles;return out;}

    const double n=static_cast<double>(used.size());
    double mqx=0,mqy=0,mtx=0,mty=0;
    for(const auto& u:used){mqx+=u.qx;mqy+=u.qy;mtx+=u.tx;mty+=u.ty;}
    mqx/=n;mqy/=n;mtx/=n;mty/=n;
    double spread=0,cross=0;
    for(const auto& u:used){
        const double qx=u.qx-mqx,qy=u.qy-mqy;
        spread+=qx*qx+qy*qy;cross+=qx*(u.tx-mtx)+qy*(u.ty-mty);
    }
    const double k=cross/spread;
    if(!(spread>0) || !std::isfinite(k) || k<=0){out.reason=FixedScaleReason::degenerate_fit;return out;}
    const render::Point shift{mtx-k*mqx,mty-k*mqy};
    double sum=0,maximum=0;
    for(const auto& u:used){
        const double d=std::hypot(u.tx-k*u.qx-shift.x,u.ty-k*u.qy-shift.y);
        sum+=d*d;maximum=std::max(maximum,d);
    }
    out.unexplained_shift_mm=shift;
    out.unexplained_shift_output_pixels={shift.x/pitch,shift.y/pitch};
    out.residual_mm={std::sqrt(sum/n),maximum};
    out.residual_output_pixels={out.residual_mm.rms/pitch,out.residual_mm.maximum/pitch};
    out.estimated_scale=k;out.difference_percent=StillScaleDifferencePercent(k);
    out.reason=FixedScaleReason::none;
    if(*out.difference_percent<=out.limit_percent){out.status=FixedScaleStatus::applied;out.applied_scale=k;}
    else out.status=FixedScaleStatus::exceeded;
    return out;
}
}
