#include "a0/m2/experimental/confirmed_position_check.hpp"

#include "a0/m2/experimental/fixed_composite_creation_v3.hpp"

#include <cmath>
#include <stdexcept>

namespace a0::m2::experimental {
namespace {
namespace cr=content_registration;
bool Finite(const RegisteredMapping& m){
    const auto& p=m.pose;const auto& s=m.surface;
    for(const double v:{p.rx_deg,p.ry_deg,p.rz_deg,p.tx_mm,p.ty_mm,p.tz_percent,s.h0_mm,s.slope_x,s.slope_y})if(!std::isfinite(v))return false;
    return true;
}
}

HandStepVerdict CheckHandStep(const HandStepInput& in){
    if(!in.parameters)throw std::invalid_argument("a hand step needs the condition's profile");
    const auto& raster=in.parameters->raster;
    if(raster.height_pixels==0 || in.seam_column>=raster.width_pixels)throw std::invalid_argument("the seam column is outside the output raster");
    if(in.hand_base.parallax)throw std::invalid_argument("a hand step starts from a position without layer 2");
    if(!Finite(in.hand_base))throw std::invalid_argument("a hand step needs a finite starting position");

    HandStepVerdict verdict;
    verdict.mapping=in.hand_base;

    // A sum of 0 is not a step: the position goes back to the one before the first hand press, as it is. That position was drawn already,
    // so it is not measured again (a position that is outside the envelope can be gone back to).
    if(in.total.IsZero()){
        verdict.status=HandStepStatus::accepted;
        return verdict;
    }

    // Solve the sum once. Whatever the equations cannot do is an unsolvable step: the position stays.
    RegisteredMapping result;
    try{
        const cr::PoseModel model_b(in.parameters->camera_b);
        const auto centre_a=cr::PoseModel(in.parameters->camera_a).Centre({});
        const auto origin=cr::SurfaceOriginMm(raster,in.seam_column);
        const auto frame=cr::FrameOfRaster(raster);
        const double middle=(static_cast<double>(raster.height_pixels)-1)/2;
        result=SolveHandStep(model_b,centre_a,origin,frame,in.hand_base,in.total,static_cast<double>(in.seam_column),middle);
    }catch(const std::invalid_argument&){
        verdict.status=HandStepStatus::refused_unsolvable;
        return verdict;
    }
    if(!Finite(result)){verdict.status=HandStepStatus::refused_unsolvable;return verdict;}
    verdict.mapping=result;

    // The pose envelope: all four components of the result, measured from p_b. Exactly at a limit is inside.
    verdict.components=RegistrationComponentsFromBase(result.pose,in.base_pose,in.envelope);
    for(const auto& c:verdict.components)
        if(c.exceeded){verdict.status=HandStepStatus::refused_envelope;return verdict;}

    // The cheap cover check, the way SelectGeometryLevel reads a candidate: a mapping that cannot be checked does not cover.
    verdict.cover_checked=true;
    try{
        const auto cover=CheckRegisteredMappingCover(*in.parameters,result,in.seam_column,in.blend);
        verdict.seam_covered=cover.seam_covered;verdict.output_covered=cover.output_covered;
    }catch(const std::invalid_argument&){
        verdict.seam_covered=false;verdict.output_covered=false;
    }
    verdict.status=(verdict.seam_covered && verdict.output_covered)?HandStepStatus::accepted:HandStepStatus::refused_cover;
    return verdict;
}
}
