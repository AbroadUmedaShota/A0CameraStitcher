// The cheap cover check of a hand step of the confirmed position (CheckRegisteredMappingCover, #437 stage 2) and the one a capture makes on the
// mapping it chose with its decoded frames (RegisteredMappingCoverOnThreads, reached through RegisteredMappingCoverOfCapturedFrames) have to answer
// alike: a hand step that passes the first is not refused by the second after the shot, and the JPEG is made with the position that was drawn.
// The second counts its own way on frames of the full sensor size and is the first itself on any other size, so this holds blank frames of the full
// sensor size and puts both through mappings near the edge: along each axis of the mapping (position, rotation, height, the paper surface) the last
// mapping that passes and the first that fails are found by bisection with the first check, then both checks answer on those and the mappings around
// them, on an output that is inside the overlap and on outputs that reach past one camera. Camera, window and file free. Argument 1 is the shared
// rig profile v2 fixture folder. The expected answer of one check is the other check's: neither is made from the function that is checked here.
#include "a0/m2/experimental/confirmed_position_check.hpp"
#include "a0/m2/experimental/fixed_composite_creation_v3.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include "a0/m2/experimental/still_review.hpp"
#include "a0/m2/optical_planner.hpp"
#include "confirmed_position_rig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace m2=a0::m2::experimental;
namespace render=a0::m2::render;
namespace fs=std::filesystem;
using a0::tests::confirmed_position_rig::MakeRig;
using a0::tests::confirmed_position_rig::ReadAll;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what,int line){++checks;if(!value){++failures;std::cerr<<"FAILED line "<<line<<": "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr,__LINE__)

struct Answer {
    bool seam{},output{};
    [[nodiscard]] bool Passes() const {return seam && output;}
    friend bool operator==(const Answer& a,const Answer& b){return a.seam==b.seam && a.output==b.output;}
};
// A mapping that cannot be checked does not cover, the way SelectGeometryLevel reads a candidate.
template<class Call> Answer Safe(Call call){
    try{const auto c=call();return {c.seam_covered,c.output_covered};}
    catch(const std::invalid_argument&){return {false,false};}
}

struct Family {
    std::string name;
    std::function<m2::RegisteredMapping(double)> at;
    double start;        // the first distance tried (the search doubles it)
    double limit;        // the farthest tried
};
std::vector<Family> Families(){
    const auto pose=[](auto set){return [set](double s){m2::RegisteredMapping m;set(m,s);return m;};};
    std::vector<Family> f;
    for(const double sign:{1.0,-1.0}){
        const std::string tag=sign>0?"+":"-";
        f.push_back({"tx_mm"+tag,pose([sign](m2::RegisteredMapping& m,double s){m.pose.tx_mm=sign*s;}),1.0,4096});
        f.push_back({"ty_mm"+tag,pose([sign](m2::RegisteredMapping& m,double s){m.pose.ty_mm=sign*s;}),1.0,4096});
        f.push_back({"rz_deg"+tag,pose([sign](m2::RegisteredMapping& m,double s){m.pose.rz_deg=sign*s;}),0.5,60});
        f.push_back({"rx_deg"+tag,pose([sign](m2::RegisteredMapping& m,double s){m.pose.rx_deg=sign*s;}),0.5,40});
        f.push_back({"tz_percent"+tag,pose([sign](m2::RegisteredMapping& m,double s){m.pose.tz_percent=sign*s;}),1.0,60});
        f.push_back({"h0_mm"+tag,pose([sign](m2::RegisteredMapping& m,double s){m.surface.h0_mm=sign*s;}),2.0,600});
        f.push_back({"rz_deg"+tag+" with ty 4 mm",pose([sign](m2::RegisteredMapping& m,double s){m.pose.rz_deg=sign*s;m.pose.ty_mm=4;}),0.5,60});
        f.push_back({"slope_x"+tag+" with h0 3 mm",pose([sign](m2::RegisteredMapping& m,double s){m.surface.slope_x=sign*s*0.001;m.surface.h0_mm=3;}),1.0,500});
    }
    return f;
}
}

int main(int argc,char* argv[]){
    try{
        if(argc!=2)throw std::invalid_argument("expected the shared rig profile v2 fixture folder");
        const auto draft=ReadAll(fs::path(argv[1])/"calibrated-draft.json");
        if(draft.empty())throw std::runtime_error("fixture profile is missing");
        const auto rig=MakeRig(draft);
        std::cerr<<std::setprecision(12);

        // Frames of the full sensor size: what a capture of the D810 pair decodes to. The check reads only their size.
        const auto width=static_cast<std::uint32_t>(a0::m2::kD810SensorWidthPixels),height=static_cast<std::uint32_t>(a0::m2::kD810SensorHeightPixels);
        render::BgrImage frame_a,frame_b;
        for(auto* f:{&frame_a,&frame_b}){f->width=width;f->height=height;f->bgr.assign(render::PixelCount(width,height)*3,0);}
        CHECK(frame_a.width==7360 && frame_a.height==4912);

        // Outputs over the same cameras. Camera A covers the document from 0 to 600.5 mm and camera B from 561.5 to 1175.5 mm along x (measured at the
        // rows of these outputs), so the overlap is 561.5 to 600.5. The first output is the tool's own, inside the overlap. The others reach past camera A
        // on the right (the right edge is camera B's alone), or reach past camera B on the left (the left edge is camera A's alone); the seam stays in the overlap.
        struct Output {std::string name;render::OutputRaster raster;std::uint32_t seam;};
        const std::vector<Output> outputs{
            {"inside the overlap",render::OutputRaster{render::DocumentRegionUm{580000,100000,598000,108000},254,180,80},90},
            {"reaches past camera A",render::OutputRaster{render::DocumentRegionUm{590000,100000,616000,108000},254,260,80},80},
            {"reaches past camera A, wide",render::OutputRaster{render::DocumentRegionUm{585000,100000,620000,108000},254,350,80},130},
            {"reaches past camera B",render::OutputRaster{render::DocumentRegionUm{550000,100000,590000,108000},254,400,80},300}};

        std::map<std::string,int> seen;   // how many mappings gave each answer
        int compared=0,disagreed=0;
        for(const auto& output:outputs){
            auto parameters=rig.binding.parameters;parameters.raster=output.raster;
            const auto fast=[&](const m2::RegisteredMapping& m){
                return Safe([&]{return m2::CheckRegisteredMappingCover(parameters,m,output.seam,m2::kSeamBlendDesign);});
            };
            const auto capture=[&](const m2::RegisteredMapping& m,unsigned threads){
                return Safe([&]{return m2::RegisteredMappingCoverOfCapturedFrames(parameters,m,output.seam,m2::kSeamBlendDesign,frame_a,frame_b,threads);});
            };
            const auto compare=[&](const m2::RegisteredMapping& m,const std::string& where){
                const auto a=fast(m),b1=capture(m,1),b3=capture(m,3);
                ++compared;
                if(!(a==b1) || !(a==b3)){
                    ++disagreed;
                    std::cerr<<"  DISAGREE ("<<output.name<<", "<<where<<"): check seam="<<a.seam<<" output="<<a.output<<"; capture seam="<<b1.seam<<" output="<<b1.output<<'\n';
                }
                ++seen[a.Passes()?"passes":(a.seam?"seam covered, output not covered":(a.output?"seam not covered, output covered":"neither"))];
            };
            if(!fast(m2::RegisteredMapping{}).Passes()){std::cerr<<"  output \""<<output.name<<"\": the mapping without a move does not pass; skipped\n";continue;}
            std::cerr<<"[output "<<output.name<<"]\n";
            compare(m2::RegisteredMapping{},"no move");
            for(const auto& family:Families()){
                const auto passes=[&](double s){return fast(family.at(s)).Passes();};
                double low=0,high=family.start;
                while(high<=family.limit && passes(high)){low=high;high*=2;}
                if(high>family.limit){std::cerr<<"  "<<family.name<<": nothing fails up to "<<family.limit<<'\n';continue;}
                for(int i=0;i<44;++i){const double mid=(low+high)/2;if(passes(mid))low=mid;else high=mid;}
                const auto at_high=fast(family.at(high));
                std::cerr<<"  "<<family.name<<": last pass "<<low<<", first fail "<<high<<" (seam "<<at_high.seam<<", output "<<at_high.output<<")\n";
                CHECK(!at_high.Passes() && fast(family.at(low)).Passes());
                const double span=high-low;
                // The two sides of the edge, one step either way, and the way in and out of it.
                for(const double s:{low,high,std::nextafter(low,0.0),std::nextafter(high,1e9),low-span,high+span,low-8*span,high+8*span,(low+high)/2,low*0.5,high*1.5})
                    if(s>=0)compare(family.at(s),family.name+" at "+std::to_string(s));
            }
        }
        std::cerr<<"compared "<<compared<<" mappings: ";
        for(const auto& [what,count]:seen)std::cerr<<what<<" "<<count<<"; ";
        std::cerr<<'\n';
        CHECK(compared>200);
        CHECK(disagreed==0);
        // Both ways of failing and the way of passing were compared, so that an agreement on one answer cannot stand for the others.
        CHECK(seen["passes"]>20);
        CHECK(seen["seam not covered, output covered"]>5);
        CHECK(seen["seam covered, output not covered"]>5);
        CHECK(seen["neither"]>5);

        // A frame that is not of the full size is checked by CheckRegisteredMappingCover itself, so the answer is the same by construction.
        {
            render::BgrImage small_a{64,48,std::vector<std::uint8_t>(64*48*3,0)},small_b=small_a;
            auto parameters=rig.binding.parameters;
            for(const double ty:{0.0,3.0,4096.0}){
                m2::RegisteredMapping m;m.pose.ty_mm=ty;
                const auto a=Safe([&]{return m2::CheckRegisteredMappingCover(parameters,m,90,m2::kSeamBlendDesign);});
                const auto b=Safe([&]{return m2::RegisteredMappingCoverOfCapturedFrames(parameters,m,90,m2::kSeamBlendDesign,small_a,small_b,2);});
                CHECK(a==b);
            }
        }
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
