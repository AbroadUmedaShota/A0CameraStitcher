// Per-capture scale correction of a fixed composite (#400): the geometry that
// scales B about a point of the seam column, the scale-only fit with rotation
// and translation tied to the fixed mapping, its decision against the limit of
// the condition record, and the policy comparison. Camera and file free.
#include "a0/m2/experimental/fixed_scale_correction.hpp"
#include "a0/m2/experimental/overlap_estimation.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include "similarity_scene.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

using namespace a0::m2::experimental;
namespace render=a0::m2::render;
namespace scene=similarity_scene;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr)
template<class Call> bool Throws(Call call){
    try{call();}catch(const std::invalid_argument&){return true;}catch(const std::runtime_error&){return true;}
    return false;
}
#define REJECT(expr) Check(Throws([&]{(void)(expr);}),"rejects: " #expr)

FixedCompositeCondition MakeCondition(int tx,int ty,double rotation,double scale,unsigned w,unsigned h,double limit){
    FixedCompositeCondition c;
    c.created_utc="2026-11-04T12:00:00Z";
    c.source.review_report_sha256=std::string(64,'a');c.source.pair_id="pair-1";
    c.source.method=(rotation==0 && scale==1)?FixedCompositeMethod::manual_still_pixels:FixedCompositeMethod::manual_still_similarity;
    c.source_width=w;c.source_height=h;
    c.mapping.translation_x=tx;c.mapping.translation_y=ty;c.mapping.rotation_degrees=rotation;c.mapping.scale=scale;
    const auto p=SimilarityReviewGeometry(tx,ty,rotation,scale,w,h);
    c.mapping.crop=p.crop;
    const auto plan=render::PlanPairCanvas(w,h,w,h,p);
    c.output_width=plan.output_width;c.output_height=plan.output_height;
    c.scale_auto_correction_max_percent=limit;
    c.seam={ProvisionalSeamColumn(p,w,h),FixedSeamOrigin::band_center_provisional};
    ValidateFixedCompositeCondition(c);
    return c;
}
bool Near(double a,double b,double tolerance){return std::abs(a-b)<=tolerance;}
bool SameMatrix(const render::Matrix3& a,const render::Matrix3& b){
    for(size_t i=0;i<9;++i)if(a[i]!=b[i])return false;return true;
}
double Angle(const render::Matrix3& m){return std::atan2(m[3],m[0])*180/3.14159265358979323846;}

// An estimate whose agreed tiles follow x_A = P + k (H(b) - P) exactly, with
// H the fixed mapping of a translation-only condition. dxs/dys: where H puts
// each tile, relative to P.
OverlapSimilarityEstimate ModelEstimate(const FixedCompositeCondition& c,double k,const std::vector<std::pair<int,int>>& offsets,unsigned radius=50){
    OverlapSimilarityEstimate e;
    e.status=OverlapEstimateStatus::estimated_not_judged;e.reason=SimilarityUndeterminedReason::none;e.search_radius=radius;
    const auto p=FixedScaleCenter(c);
    unsigned row=0;
    for(const auto& [dx,dy]:offsets){
        const double predicted_x=p.x+dx,predicted_y=p.y+dy;
        SimilarityTile t{};
        t.a_x=p.x+k*(predicted_x-p.x);t.a_y=p.y+k*(predicted_y-p.y);
        t.b_x=predicted_x-c.mapping.translation_x;t.b_y=predicted_y-c.mapping.translation_y; // identity rotation and scale
        t.row=row++;t.column=0;t.matched=true;t.agreed=true;e.tiles.push_back(t);
    }
    e.matched=e.agreed=static_cast<unsigned>(e.tiles.size());
    e.similarity={double(c.mapping.translation_x),double(c.mapping.translation_y),0,k};
    return e;
}
const std::vector<std::pair<int,int>> kOffsets{{-60,-300},{40,-200},{-20,-100},{70,100},{-50,200},{30,300}};
}

int main(){
    try{
        const unsigned w=1840,h=1228;
        const auto flat=MakeCondition(930,9,0,1,w,h,1);
        const auto turned=MakeCondition(930,9,0.4,1,w,h,1);

        // Geometry. k = 1 is the fixed mapping itself.
        for(const auto* c:{&flat,&turned}){
            const auto base=FixedCompositeRenderParameters(*c);
            const auto same=FixedCompositeScaledGeometry(*c,1);
            CHECK(SameMatrix(base.camera_b_to_camera_a,same.camera_b_to_camera_a) && base.crop.left==same.crop.left && base.crop.top==same.crop.top &&
                base.crop.right==same.crop.right && base.crop.bottom==same.crop.bottom);
            const auto p=FixedScaleCenter(*c);
            const auto plan=render::PlanPairCanvas(w,h,w,h,base);
            // The center is on the seam column, at the vertical middle of the window.
            CHECK(p.x==plan.origin_x+base.crop.left+c->seam.column && p.y==plan.origin_y+base.crop.top+plan.output_height/2.0);
            for(const double k:{0.99,0.996,1.004,1.01}){
                const auto q=FixedCompositeScaledGeometry(*c,k);
                const auto qplan=render::PlanPairCanvas(w,h,w,h,q);
                // Rotation stays that of the condition; the scale is k times the fixed one.
                CHECK(Near(Angle(q.camera_b_to_camera_a),Angle(base.camera_b_to_camera_a),1e-9));
                CHECK(Near(std::hypot(q.camera_b_to_camera_a[0],q.camera_b_to_camera_a[3]),k*std::hypot(base.camera_b_to_camera_a[0],base.camera_b_to_camera_a[3]),1e-12));
                // The window keeps its left edge, so the seam column keeps its meaning.
                CHECK(qplan.origin_x+q.crop.left==plan.origin_x+base.crop.left);
                // The point P is fixed, and every point of the seam line stays on it.
                const auto inverse=render::Invert(base.camera_b_to_camera_a);
                for(const double y:{p.y-500,p.y-100,p.y,p.y+300,p.y+500}){
                    const auto b=render::Transform(inverse,{p.x,y});
                    const auto moved=render::Transform(q.camera_b_to_camera_a,b);
                    CHECK(Near(moved.x,p.x,1e-9));
                    if(y==p.y)CHECK(Near(moved.y,p.y,1e-9));
                    else CHECK(Near(moved.y-p.y,k*(y-p.y),1e-9)); // vertical stretch about P
                }
                // The seam column can be rendered where the window is covered.
                CHECK(c->seam.column<qplan.output_width);
            }
        }
        REJECT(FixedCompositeScaledGeometry(flat,0));
        REJECT(FixedCompositeScaledGeometry(flat,-1));
        REJECT(FixedCompositeScaledGeometry(flat,std::nan("")));
        REJECT(FixedCompositeScaledGeometry(flat,1e9));

        // Decision on an exact model. 1.0078125 = 1 + 2^-7, so its percent
        // difference 0.78125 is exact in binary and "equal to the limit" is testable.
        const double k_exact=1.0078125;
        auto limited=MakeCondition(930,9,0,1,w,h,0.78125);
        auto decided=DecideFixedScale(limited,ModelEstimate(limited,k_exact,kOffsets));
        CHECK(decided.status==FixedScaleStatus::applied && decided.reason==FixedScaleReason::none);
        CHECK(decided.estimated_scale && *decided.estimated_scale==k_exact && decided.applied_scale==k_exact);
        CHECK(decided.difference_percent && *decided.difference_percent==0.78125 && decided.limit_percent==0.78125);
        CHECK(decided.used_tiles==kOffsets.size() && decided.residual.maximum==0);
        CHECK(decided.center.x==FixedScaleCenter(limited).x && decided.center.y==FixedScaleCenter(limited).y);
        limited.scale_auto_correction_max_percent=std::nextafter(0.78125,0.0);
        decided=DecideFixedScale(limited,ModelEstimate(limited,k_exact,kOffsets));
        CHECK(decided.status==FixedScaleStatus::exceeded && decided.applied_scale==1 && decided.estimated_scale && *decided.estimated_scale==k_exact);
        // The same on the other side of 1.
        limited.scale_auto_correction_max_percent=1;
        decided=DecideFixedScale(limited,ModelEstimate(limited,0.9921875,kOffsets));
        CHECK(decided.status==FixedScaleStatus::applied && decided.applied_scale==0.9921875);
        decided=DecideFixedScale(limited,ModelEstimate(limited,1.02,kOffsets));
        CHECK(decided.status==FixedScaleStatus::exceeded && decided.applied_scale==1 && decided.difference_percent && Near(*decided.difference_percent,2,1e-9));
        // A zero limit corrects nothing but an exact 1.
        limited.scale_auto_correction_max_percent=0;
        CHECK(DecideFixedScale(limited,ModelEstimate(limited,1,kOffsets)).status==FixedScaleStatus::applied);
        CHECK(DecideFixedScale(limited,ModelEstimate(limited,k_exact,kOffsets)).status==FixedScaleStatus::exceeded);
        limited.scale_auto_correction_max_percent=1;

        // Tiles that disagree with the model leave a residual; the fit still takes the least-squares k.
        {
            auto noisy=ModelEstimate(limited,k_exact,kOffsets);
            noisy.tiles[1].a_y+=1;noisy.tiles[4].a_x-=1;
            decided=DecideFixedScale(limited,noisy);
            CHECK(decided.status==FixedScaleStatus::applied && decided.residual.maximum>0 && decided.residual.rms>0 && Near(*decided.estimated_scale,k_exact,1e-3));
        }
        // Cannot be estimated: nothing is applied and the reason is recorded.
        {
            OverlapSimilarityEstimate undetermined;undetermined.status=OverlapEstimateStatus::undetermined;
            const std::pair<SimilarityUndeterminedReason,FixedScaleReason> mapping[]{
                {SimilarityUndeterminedReason::no_tiles,FixedScaleReason::no_tiles},{SimilarityUndeterminedReason::insufficient_tiles,FixedScaleReason::insufficient_tiles},
                {SimilarityUndeterminedReason::outside_image,FixedScaleReason::outside_image},{SimilarityUndeterminedReason::tied_starts,FixedScaleReason::tied_starts}};
            for(const auto& [from,to]:mapping){
                undetermined.reason=from;decided=DecideFixedScale(limited,undetermined);
                CHECK(decided.status==FixedScaleStatus::not_estimated && decided.reason==to && !decided.estimated_scale && decided.applied_scale==1 && decided.limit_percent==1);
            }
            // Fewer than three tiles, or one row only.
            auto two=ModelEstimate(limited,k_exact,{kOffsets[0],kOffsets[1]});
            decided=DecideFixedScale(limited,two);
            CHECK(decided.status==FixedScaleStatus::not_estimated && decided.reason==FixedScaleReason::too_few_tiles && decided.applied_scale==1);
            auto one_row=ModelEstimate(limited,k_exact,kOffsets);
            for(auto& t:one_row.tiles)t.row=3;
            decided=DecideFixedScale(limited,one_row);
            CHECK(decided.status==FixedScaleStatus::not_estimated && decided.reason==FixedScaleReason::too_few_tiles);
            // A pattern repeated one period away: every match is far outside the search window of the fixed mapping.
            auto repeated=ModelEstimate(limited,k_exact,kOffsets);
            for(auto& t:repeated.tiles)t.a_x+=120;
            decided=DecideFixedScale(limited,repeated);
            CHECK(decided.status==FixedScaleStatus::not_estimated && decided.reason==FixedScaleReason::too_few_tiles && decided.used_tiles==0);
            // Tiles that are not in the consensus are not used.
            auto outsiders=ModelEstimate(limited,k_exact,kOffsets);
            for(size_t i=0;i<outsiders.tiles.size();++i)outsiders.tiles[i].agreed=i<2;
            CHECK(DecideFixedScale(limited,outsiders).reason==FixedScaleReason::too_few_tiles);
            // Every tile at the center: no lever arm.
            auto central=ModelEstimate(limited,k_exact,{{0,0},{0,0},{0,0}});
            decided=DecideFixedScale(limited,central);
            CHECK(decided.status==FixedScaleStatus::not_estimated && decided.reason==FixedScaleReason::degenerate_fit);
            // A fit that would flip B.
            auto flipped=ModelEstimate(limited,-0.5,kOffsets,100000);
            decided=DecideFixedScale(limited,flipped);
            CHECK(decided.status==FixedScaleStatus::not_estimated && decided.reason==FixedScaleReason::degenerate_fit);
        }
        // The estimate is kept as a diagnostic, unchanged.
        {
            auto e=ModelEstimate(limited,1.003,kOffsets);e.wide_search=true;e.similarity_residual={0.5,1.5};
            const auto d=MakeFixedSimilarityDiagnostic(e);
            CHECK(d.status==e.status && d.wide_search && d.tiles_searched==kOffsets.size() && d.tiles_agreed==kOffsets.size() && d.similarity && d.similarity->scale==1.003 && d.similarity_residual.rms==0.5);
            const auto delta=FixedDiagnosticDelta(d,930,9,0,1);
            CHECK(delta && delta->x==0 && delta->y==0 && delta->rotation_degrees==0 && delta->scale_ratio==1.003);
            OverlapSimilarityEstimate none;
            CHECK(!MakeFixedSimilarityDiagnostic(none).similarity && !FixedDiagnosticDelta(MakeFixedSimilarityDiagnostic(none),930,9,0,1));
        }

        // Policy file against the condition record: classification only.
        CHECK(ComparePolicyLimit(1,1.0,FixedPolicyCheck::policy_absent).check==FixedPolicyCheck::same);
        CHECK(ComparePolicyLimit(1,0.5,FixedPolicyCheck::policy_absent).check==FixedPolicyCheck::differs && *ComparePolicyLimit(1,0.5,FixedPolicyCheck::policy_absent).policy_percent==0.5);
        for(const auto v:{FixedPolicyCheck::policy_absent,FixedPolicyCheck::policy_unreadable,FixedPolicyCheck::policy_without_scale_limit}){
            const auto r=ComparePolicyLimit(1,std::nullopt,v);CHECK(r.check==v && !r.policy_percent);
        }
        CHECK(FixedPolicyNotice(FixedPolicyCheck::same).empty());
        CHECK(!FixedPolicyNotice(FixedPolicyCheck::differs).empty() && !FixedPolicyNotice(FixedPolicyCheck::policy_absent).empty());
        CHECK(FixedScaleNotice(FixedScaleStatus::applied).empty() && !FixedScaleNotice(FixedScaleStatus::exceeded).empty());
        for(const auto s:{FixedCompositeSkip::condition_absent,FixedCompositeSkip::condition_invalid,FixedCompositeSkip::orientation_differs,FixedCompositeSkip::size_differs,FixedCompositeSkip::seam_outside_overlap})
            CHECK(!FixedCompositeSkipText(s).empty() && !FixedCompositeSkipCode(s).empty());

        // End to end on a generated scene: the estimator's tiles, then the
        // scale-only fit. B differs from the condition by a scale k about P.
        // Truth in the generator's form, a = k R (b - c) + c + t', with
        // t' = (k - 1) c + k t0 + (1 - k) P for the fixed mapping t0.
        const auto world=scene::MakeScene(static_cast<int>(w),static_cast<int>(h),960,40,17);
        const auto a=scene::ViewA(world,w,h);
        const auto p=FixedScaleCenter(flat);
        const double cx=w/2.0,cy=h/2.0;
        const auto view_b=[&](double k,const scene::Scene& s){
            return scene::ViewB(s,w,h,(k-1)*cx+k*930+(1-k)*p.x,(k-1)*cy+k*9+(1-k)*p.y,0,k,1.0);
        };
        struct Case {double truth;FixedScaleStatus status;};
        for(const auto& test:{Case{1.004,FixedScaleStatus::applied},Case{0.9965,FixedScaleStatus::applied},Case{1.0,FixedScaleStatus::applied},Case{1.02,FixedScaleStatus::exceeded}}){
            const auto b=view_b(test.truth,world);
            const auto estimate=EstimateOverlapSimilarity(a,b,930,9);
            const auto result=DecideFixedScale(flat,estimate);
            std::cout<<"truth="<<test.truth<<" status="<<FixedScaleStatusCode(result.status)<<" estimated="<<(result.estimated_scale?*result.estimated_scale:0.0)
                <<" used="<<result.used_tiles<<" rms="<<result.residual.rms<<" agreed="<<estimate.agreed<<'\n';
            CHECK(result.status==test.status);
            CHECK(result.estimated_scale && Near(*result.estimated_scale,test.truth,1.5e-3));
            CHECK(result.used_tiles>=3 && result.residual.rms<2);
            if(result.status!=FixedScaleStatus::applied)continue;
            // With the factor applied the seam stays where the condition puts it,
            // and the displacement across it shrinks against the uncorrected mapping.
            const auto scaled=FixedCompositeScaledGeometry(flat,result.applied_scale);
            const auto base=FixedCompositeRenderParameters(flat);
            CHECK(CanRenderFixedSeam(a,b,scaled,flat.seam.column,kSeamBlendDesign));
            const auto corrected=RenderPairFixedSeam(a,b,scaled,flat.seam.column,kSeamBlendDesign);
            CHECK(corrected.record.fixed_seam && std::all_of(corrected.record.seam_x.begin(),corrected.record.seam_x.end(),[&](std::uint32_t x){return x==flat.seam.column;}));
            const auto spread=[&](const render::PairRenderParameters& q,const render::BgrImage& image){
                const auto plan=render::PlanPairCanvas(w,h,w,h,q);
                const std::vector<std::uint32_t> seam(plan.output_height,flat.seam.column);
                double worst=0;int valid=0;
                for(const auto& probe:ProbeSeamDisplacement(a,image,q,seam,128,32,std::max(1u,plan.output_height/24)))
                    if(probe.valid){++valid;worst=std::max(worst,std::hypot(probe.dx,probe.dy));}
                return std::pair{worst,valid};
            };
            const auto before=spread(base,b),after=spread(scaled,b);
            std::cout<<"  displacement uncorrected max="<<before.first<<" corrected max="<<after.first<<" probes="<<before.second<<"/"<<after.second<<'\n';
            CHECK(after.second>0 && after.first<=before.first+1e-9);
            if(std::abs(test.truth-1)>2e-3)CHECK(after.first<before.first);
        }
        // Another scene with the same fixed fields: the mapping and the seam do not depend on the content.
        {
            const auto other=scene::MakeScene(static_cast<int>(w),static_cast<int>(h),960,40,99);
            const auto oa=scene::ViewA(other,w,h),ob=view_b(1.003,other);
            const auto r1=DecideFixedScale(flat,EstimateOverlapSimilarity(a,view_b(1.003,world),930,9)),r2=DecideFixedScale(flat,EstimateOverlapSimilarity(oa,ob,930,9));
            CHECK(r1.status==FixedScaleStatus::applied && r2.status==FixedScaleStatus::applied);
            CHECK(r1.center.x==r2.center.x && r1.center.y==r2.center.y && r1.limit_percent==r2.limit_percent);
            const auto g1=FixedCompositeScaledGeometry(flat,r1.applied_scale),g2=FixedCompositeScaledGeometry(flat,r2.applied_scale);
            CHECK(Near(Angle(g1.camera_b_to_camera_a),Angle(g2.camera_b_to_camera_a),1e-9) && Near(Angle(g1.camera_b_to_camera_a),0,1e-9));
        }
    }catch(const std::exception& error){
        std::cerr<<"unexpected exception: "<<error.what()<<'\n';return 2;
    }
    std::cout<<"checks="<<checks<<" failures="<<failures<<'\n';
    return failures?1:0;
}
