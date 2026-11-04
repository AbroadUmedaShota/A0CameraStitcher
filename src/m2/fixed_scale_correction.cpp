#include "a0/m2/experimental/fixed_scale_correction.hpp"
#include <algorithm>
#include <array>
#include <set>
#include <vector>
#include <stdexcept>

namespace a0::m2::experimental {
namespace {
struct Window {double left{},top{};unsigned height{};};
Window WindowOf(const render::PairRenderParameters& base,unsigned width,unsigned height){
    const auto plan=render::PlanPairCanvas(width,height,width,height,base);
    return {plan.origin_x+base.crop.left,plan.origin_y+base.crop.top,plan.output_height};
}
}

render::Point FixedScaleCenter(int x,int y,double rotation_degrees,double scale,unsigned width,unsigned height,std::uint32_t seam_column){
    const auto base=SimilarityReviewGeometry(x,y,rotation_degrees,scale,width,height);
    const auto w=WindowOf(base,width,height);
    return {w.left+seam_column,w.top+w.height/2.0};
}

render::PairRenderParameters FixedCompositeScaledGeometry(int x,int y,double rotation_degrees,double scale,
    unsigned width,unsigned height,std::uint32_t seam_column,double k){
    auto base=SimilarityReviewGeometry(x,y,rotation_degrees,scale,width,height);
    if(k==1)return base;
    if(!std::isfinite(k) || k<=0 || width<4 || height<4)throw std::invalid_argument("invalid fixed composite scale correction");
    const auto window=WindowOf(base,width,height);
    // The one place that decides the center (FixedScaleCenter); the fit uses it too.
    const auto p=FixedScaleCenter(x,y,rotation_degrees,scale,width,height,seam_column);
    const auto& m=base.camera_b_to_camera_a;
    render::PairRenderParameters q{{k*m[0],k*m[1],k*(m[2]-p.x)+p.x,k*m[3],k*m[4],k*(m[5]-p.y)+p.y,0,0,1},base.layout,{}};
    // The axis-aligned rectangle inscribed in the scaled B with one input pixel
    // inset, centered where B's center now is: the same covered rectangle
    // SimilarityReviewGeometry derives, for a non-integer placement.
    const double linear=std::hypot(m[0],m[3]),c=m[0]/linear,s=m[3]/linear,scaled=k*linear;
    const auto& n=q.camera_b_to_camera_a;
    const double cx=n[0]*width/2.0+n[1]*height/2.0+n[2],cy=n[3]*width/2.0+n[4]*height/2.0+n[5];
    const double w=width-2.0,h=height-2.0;
    const auto fit=1/std::max(std::abs(c)+std::abs(s)*h/w,std::abs(c)+std::abs(s)*w/h);
    const double left=cx-scaled*w*fit/2,right=cx+scaled*w*fit/2,top=cy-scaled*h*fit/2,bottom=cy+scaled*h*fit/2;
    const double end_x=std::floor(right),begin_y=std::ceil(std::max(0.0,top)),end_y=std::floor(std::min(double(height),bottom));
    if(left>=width || end_x<=width || end_y<=begin_y)throw std::invalid_argument("scaled fixed composite has no covered window");
    const auto raw=render::PlanPairCanvas(width,height,width,height,q);
    const double crop_left=window.left-raw.origin_x,crop_top=begin_y-raw.origin_y;
    const double crop_right=raw.origin_x+raw.canvas_width-end_x,crop_bottom=raw.origin_y+raw.canvas_height-end_y;
    if(crop_left<0 || crop_top<0 || crop_right<0 || crop_bottom<0)throw std::invalid_argument("scaled fixed composite window leaves its canvas");
    q.crop={static_cast<std::uint32_t>(crop_left),static_cast<std::uint32_t>(crop_top),static_cast<std::uint32_t>(crop_right),static_cast<std::uint32_t>(crop_bottom)};
    (void)render::PlanPairCanvas(width,height,width,height,q);
    return q;
}

FixedScaleCorrection DecideFixedScale(const FixedCompositeCondition& condition,const OverlapSimilarityEstimate& e){
    FixedScaleCorrection out;
    out.limit_percent=condition.scale_auto_correction_max_percent;
    out.center=FixedScaleCenter(condition);
    out.status=FixedScaleStatus::not_estimated;
    if(e.status!=OverlapEstimateStatus::estimated_not_judged){
        switch(e.reason){
        case SimilarityUndeterminedReason::no_tiles:out.reason=FixedScaleReason::no_tiles;break;
        case SimilarityUndeterminedReason::insufficient_tiles:out.reason=FixedScaleReason::insufficient_tiles;break;
        case SimilarityUndeterminedReason::outside_image:out.reason=FixedScaleReason::outside_image;break;
        case SimilarityUndeterminedReason::tied_starts:out.reason=FixedScaleReason::tied_starts;break;
        case SimilarityUndeterminedReason::none:out.reason=FixedScaleReason::degenerate_fit;break;
        }
        return out;
    }
    const auto& h=FixedCompositeRenderParameters(condition).camera_b_to_camera_a;
    const double radius=e.search_radius;
    std::set<unsigned> rows;std::vector<std::array<double,4>> used; // q.x,q.y,t.x,t.y
    for(const auto& tile:e.tiles){
        if(!tile.agreed)continue;
        const auto predicted=render::Transform(h,{tile.b_x,tile.b_y});
        if(std::hypot(tile.a_x-predicted.x,tile.a_y-predicted.y)>radius)continue;
        used.push_back({predicted.x-out.center.x,predicted.y-out.center.y,tile.a_x-out.center.x,tile.a_y-out.center.y});
        rows.insert(tile.row);
    }
    out.used_tiles=static_cast<unsigned>(used.size());
    if(used.size()<3 || rows.size()<2){out.reason=FixedScaleReason::too_few_tiles;return out;}
    // Inside the estimate only, the translation is free: t = k q + d. Without it
    // a shift that is not a scale (shake, one pattern period) leaks into k in
    // proportion to how far the tiles sit to one side of P. Rotation stays H's.
    // Only k is applied; d is what the scale could not explain, recorded as is.
    const double n=static_cast<double>(used.size());
    double mqx=0,mqy=0,mtx=0,mty=0;
    for(const auto& u:used){mqx+=u[0];mqy+=u[1];mtx+=u[2];mty+=u[3];}
    mqx/=n;mqy/=n;mtx/=n;mty/=n;
    double spread=0,cross=0;
    for(const auto& u:used){
        const double qx=u[0]-mqx,qy=u[1]-mqy;
        spread+=qx*qx+qy*qy;cross+=qx*(u[2]-mtx)+qy*(u[3]-mty);
    }
    const double k=cross/spread;
    if(!(spread>0) || !std::isfinite(k) || k<=0){out.reason=FixedScaleReason::degenerate_fit;return out;}
    out.unexplained_shift={mtx-k*mqx,mty-k*mqy};
    double sum=0,maximum=0;
    for(const auto& u:used){
        const double d=std::hypot(u[2]-k*u[0]-out.unexplained_shift.x,u[3]-k*u[1]-out.unexplained_shift.y);sum+=d*d;maximum=std::max(maximum,d);
    }
    out.residual={std::sqrt(sum/n),maximum};
    out.estimated_scale=k;out.difference_percent=StillScaleDifferencePercent(k);
    out.reason=FixedScaleReason::none;
    if(*out.difference_percent<=out.limit_percent){out.status=FixedScaleStatus::applied;out.applied_scale=k;}
    else out.status=FixedScaleStatus::exceeded;
    return out;
}

FixedSimilarityDiagnostic MakeFixedSimilarityDiagnostic(const OverlapSimilarityEstimate& e){
    FixedSimilarityDiagnostic d;
    d.status=e.status;d.reason=e.reason;d.wide_search=e.wide_search;
    d.tiles_searched=static_cast<unsigned>(e.tiles.size());d.tiles_matched=e.matched;d.tiles_agreed=e.agreed;
    if(e.status==OverlapEstimateStatus::estimated_not_judged){d.similarity=e.similarity;d.similarity_residual=e.similarity_residual;}
    return d;
}

std::string_view FixedScaleNotice(FixedScaleStatus status){
    switch(status){
    case FixedScaleStatus::applied:return {};
    case FixedScaleStatus::exceeded:return "倍率の差が上限を超えた。ピントか高さを確かめてください";
    case FixedScaleStatus::not_estimated:return "倍率の差を推定できなかったため、倍率は補正していません";
    case FixedScaleStatus::seam_outside_overlap:return "倍率を補正すると継ぎ目が重なりの外に出るため、倍率は補正していません";
    }
    return {};
}
std::string_view FixedPolicyNotice(FixedPolicyCheck check){
    switch(check){
    case FixedPolicyCheck::same:return {};
    case FixedPolicyCheck::differs:return "方針ファイルの倍率の上限が固定の合成条件と違います。固定の合成条件を作り直してください";
    case FixedPolicyCheck::policy_absent:
    case FixedPolicyCheck::policy_unreadable:
    case FixedPolicyCheck::policy_without_scale_limit:return "方針ファイルの倍率の上限を確かめられないため、固定の合成条件の記録の値で判定しました";
    }
    return {};
}
std::string_view FixedCompositeSkipText(FixedCompositeSkip skip){
    switch(skip){
    case FixedCompositeSkip::condition_absent:return "固定の合成条件がありません。設置の時に作ってください";
    case FixedCompositeSkip::condition_invalid:return "固定の合成条件を読めません（内容が正しくありません）。設置の時に作り直してください";
    case FixedCompositeSkip::orientation_differs:return "固定の合成条件の取り付けの向きが、この撮影と合いません";
    case FixedCompositeSkip::size_differs:return "固定の合成条件の画像の大きさが、この撮影と合いません";
    case FixedCompositeSkip::seam_outside_overlap:return "固定の合成条件の継ぎ目の列が、この撮影の重なりの外にあります";
    }
    return {};
}
}
