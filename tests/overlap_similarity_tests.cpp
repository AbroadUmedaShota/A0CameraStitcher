// Still similarity estimator (#365) on generated drawing-like scenes.
#include "a0/m2/experimental/overlap_estimation.hpp"
#include "similarity_scene.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace a0::m2::experimental;
using a0::m2::render::BgrImage;
namespace scene=similarity_scene;
struct Truth {double x,y,degrees,scale;};
int main(){
    int checks=0,failures=0;auto check=[&](bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAIL "<<what<<'\n';}};
    auto reject=[&](auto call,const char* what){bool threw=false;try{call();}catch(const std::invalid_argument&){threw=true;}check(threw,what);};
    auto close=[&](const OverlapSimilarityEstimate& e,const Truth& t,const char* what){
        const bool ok=e.status==OverlapEstimateStatus::estimated_not_judged && std::abs(e.similarity.x-t.x)<1 && std::abs(e.similarity.y-t.y)<1 &&
            std::abs(e.similarity.rotation_degrees-t.degrees)<0.01 && std::abs(e.similarity.scale-t.scale)<2e-4;
        check(ok,what);
        std::cout<<what<<" status="<<OverlapEstimateStatusCode(e.status)<<" reason="<<SimilarityUndeterminedReasonCode(e.reason)<<" x="<<e.similarity.x<<" y="<<e.similarity.y
            <<" degrees="<<e.similarity.rotation_degrees<<" scale="<<e.similarity.scale<<" tiles="<<e.agreed<<"/"<<e.matched<<"/"<<e.tiles.size()
            <<" residual_rms="<<e.similarity_residual.rms<<" translation_rms="<<e.translation_residual.rms<<" wide="<<e.wide_search<<'\n';
    };
    try{
        // Full D810 size: a known small rotation and scale is recovered and
        // fits better than translation alone. B is 8% darker (gradient input).
        const unsigned w=7360,h=4912;const Truth t1{3700.4,38.6,0.6,1.0015};
        const auto world=scene::MakeScene(static_cast<int>(w),static_cast<int>(h),3760,120,365);
        const auto a=scene::ViewA(world,w,h),b=scene::ViewB(world,w,h,t1.x,t1.y,t1.degrees,t1.scale,0.92);
        auto started=std::chrono::steady_clock::now();const auto e1=EstimateOverlapSimilarity(a,b,3712,31);
        std::cout<<"full_size_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()<<'\n';
        close(e1,t1,"positive rotation");
        check(e1.similarity_residual.rms<e1.translation_residual.rms && e1.similarity_residual.maximum<e1.translation_residual.maximum,"similarity residual below translation only");
        check(e1.levels==4 && e1.tile_side==256 && e1.search_radius==200 && e1.agreed>=3 && e1.agreed<=e1.matched,"full size plan");
        const Truth t2{3690.2,-22.4,-0.9,0.998};
        const auto b2=scene::ViewB(world,w,h,t2.x,t2.y,t2.degrees,t2.scale,1.0);
        close(EstimateOverlapSimilarity(a,b2,3750,-62),t2,"negative rotation, offset prior");
        // Smaller frames for the remaining contracts.
        const unsigned sw=1840,sh=1228;
        const auto small=scene::MakeScene(static_cast<int>(sw),static_cast<int>(sh),960,40,17);
        const auto sa=scene::ViewA(small,sw,sh);const Truth t3{930.3,9.2,0.4,1.0};
        const auto sb=scene::ViewB(small,sw,sh,t3.x,t3.y,t3.degrees,t3.scale,1.0);
        const auto near=EstimateOverlapSimilarity(sa,sb,925,12);close(near,t3,"small frame");check(near.levels==2 && near.tile_side==64 && near.search_radius==50,"small frame plan");
        // Equal inputs, equal results: no random sampling.
        const auto again=EstimateOverlapSimilarity(sa,sb,925,12);
        check(std::memcmp(&again.similarity,&near.similarity,sizeof(SimilarityFit))==0 && again.agreed==near.agreed && again.translation_x==near.translation_x && again.wide_search==near.wide_search,"deterministic");
        // A prior beyond the search radius: the wide search gives the start.
        const auto far=EstimateOverlapSimilarity(sa,sb,925-300,12);close(far,t3,"wide start");check(far.wide_search,"wide start used");
        // A band of B from another depth moves by 30 px; the consensus drops it.
        auto depth=sb;const auto shifted=scene::ViewB(small,sw,sh,t3.x+30,t3.y,t3.degrees,t3.scale,1.0);
        std::copy(shifted.bgr.begin()+static_cast<ptrdiff_t>(sw)*1000*3,shifted.bgr.end(),depth.bgr.begin()+static_cast<ptrdiff_t>(sw)*1000*3);
        const auto mixed=EstimateOverlapSimilarity(sa,depth,925,12);close(mixed,t3,"other depth band excluded");
        check(mixed.agreed<mixed.matched,"depth band tiles outside consensus");
        // Without texture, or with lines in one direction only, nothing is
        // estimated; the caller keeps its own position.
        const auto blank=scene::Gray(sw,sh);
        const auto none=EstimateOverlapSimilarity(blank,blank,925,12);
        check(none.status==OverlapEstimateStatus::undetermined && none.reason==SimilarityUndeterminedReason::insufficient_tiles && none.agreed==0 && !none.tiles.empty(),"blank undetermined");
        auto stripes=scene::Gray(sw,sh);for(unsigned y=0;y<sh;++y)for(unsigned x=0;x<sw;++x)scene::Put(stripes,x,y,(x/7+x/23)%3?230:40);
        const auto one=EstimateOverlapSimilarity(stripes,stripes,925,0);
        check(one.status==OverlapEstimateStatus::undetermined && one.matched==0,"one-dimensional texture undetermined");
        const auto edge=EstimateOverlapSimilarity(sa,sb,sw-20,12);close(edge,t3,"no room at the prior, wide start");check(edge.wide_search,"wide start after no tiles");
        reject([&]{(void)EstimateOverlapSimilarity(sa,scene::Gray(sw,sh-1),925,0);},"size mismatch");
        reject([&]{(void)EstimateOverlapSimilarity(sa,sb,0,0);},"prior x zero");
        reject([&]{(void)EstimateOverlapSimilarity(sa,sb,925,static_cast<int>(sh));},"prior y outside");
        auto broken=sb;broken.bgr.pop_back();reject([&]{(void)EstimateOverlapSimilarity(sa,broken,925,0);},"broken buffer");
    }catch(const std::exception& e){++failures;std::cerr<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
