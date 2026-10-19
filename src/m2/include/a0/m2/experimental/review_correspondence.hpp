#pragma once
#include "a0/m2/experimental/review_alignment.hpp"

namespace a0::m2::experimental {
// Session-local manual observations, always in the two original pixel grids.
enum class ReviewPointRole { fitting, validation };
struct ReviewCorrespondence {render::Point camera_a,camera_b;ReviewPointRole role{ReviewPointRole::fitting};};
struct ReviewPointResidual {double dx{},dy{},distance{};};
struct ReviewResiduals {std::vector<ReviewPointResidual> points;double rms{},maximum{};};
struct ReviewSimilarityProposal {
    int x{},y{};double degrees{},scale{};
    render::PairRenderParameters parameters;
};
inline bool ReviewSourceContains(render::Point p,unsigned width,unsigned height){
    return std::isfinite(p.x) && std::isfinite(p.y) && p.x>=0 && p.y>=0 && p.x<=width-1.0 && p.y<=height-1.0;
}
inline void ValidateReviewCorrespondences(const std::vector<ReviewCorrespondence>& points,unsigned width,unsigned height){
    (void)render::PixelCount(width,height);
    if(points.size()>32)throw std::invalid_argument("at most 32 review correspondences");
    for(size_t i=0;i<points.size();++i){
        const auto& p=points[i];
        if(p.role!=ReviewPointRole::fitting && p.role!=ReviewPointRole::validation)throw std::invalid_argument("invalid correspondence role");
        if(!ReviewSourceContains(p.camera_a,width,height) || !ReviewSourceContains(p.camera_b,width,height))throw std::invalid_argument("invalid correspondence source pixel");
        for(size_t j=0;j<i;++j)if((p.camera_a.x==points[j].camera_a.x && p.camera_a.y==points[j].camera_a.y) ||
            (p.camera_b.x==points[j].camera_b.x && p.camera_b.y==points[j].camera_b.y))throw std::invalid_argument("repeated correspondence source point");
    }
}
inline ReviewResiduals MeasureReviewCorrespondences(const std::vector<ReviewCorrespondence>& points,const render::PairRenderParameters& parameters,unsigned width,unsigned height,std::optional<ReviewPointRole> role={}){
    ValidateReviewCorrespondences(points,width,height);ReviewResiduals result;double squared{};
    if(role && *role!=ReviewPointRole::fitting && *role!=ReviewPointRole::validation)throw std::invalid_argument("invalid residual role");
    for(const auto& p:points){
        if(role && p.role!=*role)continue;
        const auto transformed=render::Transform(parameters.camera_b_to_camera_a,p.camera_b);
        const auto dx=transformed.x-p.camera_a.x,dy=transformed.y-p.camera_a.y,distance=std::hypot(dx,dy);
        if(!std::isfinite(distance))throw std::invalid_argument("nonfinite review residual");
        result.points.push_back({dx,dy,distance});squared+=distance*distance;result.maximum=std::max(result.maximum,distance);
    }
    if(!result.points.empty())result.rms=std::sqrt(squared/result.points.size());return result;
}
inline ReviewSimilarityProposal ProposeReviewSimilarity(const std::vector<ReviewCorrespondence>& points,unsigned width,unsigned height){
    ValidateReviewCorrespondences(points,width,height);
    const auto count=std::count_if(points.begin(),points.end(),[](const auto& p){return p.role==ReviewPointRole::fitting;});
    if(count<2)throw std::invalid_argument("two distinct fitting point pairs required");
    render::Point ca{},cb{};
    for(const auto& p:points)if(p.role==ReviewPointRole::fitting){ca.x+=p.camera_a.x;ca.y+=p.camera_a.y;cb.x+=p.camera_b.x;cb.y+=p.camera_b.y;}
    ca.x/=count;ca.y/=count;cb.x/=count;cb.y/=count;
    double denominator{},dot{},cross{};
    for(const auto& p:points){
        if(p.role!=ReviewPointRole::fitting)continue;
        const auto ax=p.camera_a.x-ca.x,ay=p.camera_a.y-ca.y,bx=p.camera_b.x-cb.x,by=p.camera_b.y-cb.y;
        denominator+=bx*bx+by*by;dot+=bx*ax+by*ay;cross+=bx*ay-by*ax;
    }
    if(!(denominator>0))throw std::invalid_argument("degenerate correspondence spread");
    const auto a=dot/denominator,b=cross/denominator,scale=std::hypot(a,b),degrees=std::atan2(b,a)*180/std::numbers::pi;
    const auto cx=width/2.0,cy=height/2.0;
    const auto x=ca.x+a*(cx-cb.x)-b*(cy-cb.y)-cx,y=ca.y+b*(cx-cb.x)+a*(cy-cb.y)-cy;
    if(!std::isfinite(x) || !std::isfinite(y) || x<1 || x>width-1.0 || y<=-double(height) || y>=height || !std::isfinite(scale) || scale<=0)
        throw std::invalid_argument("point proposal outside review geometry");
    ReviewSimilarityProposal result{static_cast<int>(std::lround(x)),static_cast<int>(std::lround(y)),degrees,scale,{}};
    // Score the actual integer center shift accepted by the inspector, rather
    // than claiming the smaller continuous least-squares fitting residual.
    result.parameters=SimilarityReviewGeometry(result.x,result.y,degrees,scale,width,height);return result;
}
inline std::optional<render::Point> PickReviewSourcePoint(render::Point cropped_pixel,const render::PairRenderParameters& parameters,unsigned width,unsigned height,bool camera_b){
    const auto plan=render::PlanPairCanvas(width,height,width,height,parameters);
    if(!ReviewSourceContains(cropped_pixel,plan.output_width,plan.output_height))return {};
    render::Point global{cropped_pixel.x+plan.origin_x+parameters.crop.left,cropped_pixel.y+plan.origin_y+parameters.crop.top};
    const auto point=camera_b?render::Transform(render::Invert(parameters.camera_b_to_camera_a),global):global;
    if(!ReviewSourceContains(point,width,height))return {};return point;
}
}
