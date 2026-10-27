#include "a0/m2/experimental/seam_blend.hpp"
#include "a0/m2/experimental/review_alignment.hpp"
#include "similarity_scene.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
using namespace a0::m2::experimental;
using a0::m2::render::BgrImage;
namespace render=a0::m2::render;

namespace {
BgrImage Flat(unsigned w,unsigned h,std::array<std::uint8_t,3> bgr){
    BgrImage image{w,h,std::vector<std::uint8_t>(static_cast<size_t>(w)*h*3)};
    for(size_t i=0;i<image.bgr.size();i+=3)for(int c=0;c<3;++c)image.bgr[i+c]=bgr[c];
    return image;
}
void Fill(BgrImage& image,unsigned x0,unsigned x1,unsigned y0,unsigned y1,std::uint8_t v){
    for(unsigned y=y0;y<y1 && y<image.height;++y)for(unsigned x=x0;x<x1 && x<image.width;++x)for(int c=0;c<3;++c)image.bgr[(static_cast<size_t>(y)*image.width+x)*3+c]=v;
}
std::array<int,3> Pixel(const BgrImage& image,unsigned x,unsigned y){const auto i=(static_cast<size_t>(y)*image.width+x)*3;return {image.bgr[i],image.bgr[i+1],image.bgr[i+2]};}
double Lum(const BgrImage& image,unsigned x,unsigned y){const auto p=Pixel(image,x,y);return p[0]*0.114+p[1]*0.587+p[2]*0.299;}
render::PairRenderParameters Shift(double dx){return {{1,0,dx,0,1,0,0,0,1},a0::m2::StitchLayout::camera_a_left_camera_b_right,{}};}
}

int main(){
    int checks=0,failures=0;auto check=[&](bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAIL "<<what<<'\n';}};
    auto reject=[&](auto call,const char* what){bool threw=false;try{call();}catch(const std::invalid_argument&){threw=true;}check(threw,what);};
    const SeamBlendSettings small{4,8,8,16};
    try{
        // Path codec.
        const std::vector<std::uint32_t> path{5,5,6,5,4,4};
        check(EncodeSeamSteps(path)=="0+--0" && ExpandSeamSteps(5,"0+--0",10)==path,"seam steps round trip");
        const auto summary=SummarizeSeam(path);check(summary.x_minimum==4 && summary.x_maximum==6 && summary.x_mean==29.0/6,"seam summary");
        reject([]{(void)ExpandSeamSteps(0,"-",10);},"step below zero");reject([]{(void)ExpandSeamSteps(9,"+",10);},"step past width");
        reject([]{(void)ExpandSeamSteps(3,"0x",10);},"unknown step");reject([]{(void)ExpandSeamSteps(10,"",10);},"start outside");
        reject([]{(void)EncodeSeamSteps({3,5});},"jump of two columns");reject([]{(void)SummarizeSeam({});},"empty seam");
        check(ExpandSeamSteps(7,"",10)==std::vector<std::uint32_t>{7},"single row path");
        reject([]{ValidateSeamBlendSettings({0,8,8,16});},"zero blend width");reject([]{ValidateSeamBlendSettings({4,8,0,16});},"zero window");
        ValidateSeamBlendSettings(kSeamBlendDesign);

        // Known misregistration: B's vertical lines sit 4 px right of A's
        // except in a blank corridor whose position bends half-way down.
        const unsigned w=240,h=160;auto a=Flat(w,h,{232,232,232}),b=a;
        const auto corridor=[](unsigned y){return y<60?185.0:y<120?185+(y-60)/2.0:215.0;};
        for(unsigned y=0;y<h;++y)for(unsigned g=120;g<240;g+=6){
            if(std::abs(g+3-corridor(y))<15)continue;
            Fill(a,g,g+3,y,y+1,40);Fill(b,g+3-120,g+6-120,y,y+1,40);
        }
        const auto seam=RenderPairSeamCut(a,b,Shift(120),small);
        bool inside=true,steps=true;
        for(unsigned y=0;y<h;++y){const auto x=seam.record.seam_x[y];
            if(std::abs(x-corridor(y))>20 || Lum(a,x,y)<230 || Lum(b,x-120,y)<230)inside=false;
            if(y && std::abs(int(x)-int(seam.record.seam_x[y-1]))>1)steps=false;}
        check(inside,"seam follows the blank corridor");if(!inside)for(unsigned y=0;y<h;y+=8)std::cerr<<y<<":"<<seam.record.seam_x[y]<<" ";std::cerr<<'\n';check(steps,"seam moves at most one column per row");
        check(seam.image.width==360 && seam.image.height==h,"seam canvas equals the pair canvas");
        // Away from the band only A or only B is used, byte for byte as RenderPair.
        const auto feather=render::RenderPair(a,b,Shift(120));
        // No line is doubled: left of the corridor the output has A's lines only.
        check(Lum(seam.image,163,10)<60 && Lum(seam.image,166,10)>220,"left of the seam only A is visible");
        check(Lum(seam.image,232,10)<60 && Lum(seam.image,229,10)>220,"right of the seam only B is visible");
        check(Lum(feather.image,163,10)<150 && Lum(feather.image,166,10)<200,"the feather render doubles the line");
        const auto again=RenderPairSeamCut(a,b,Shift(120),small);
        check(again.image.bgr==seam.image.bgr && again.record.seam_x==seam.record.seam_x,"deterministic");

        // Drawing-like scene under a known 4,2 px misregistration: the seam
        // crosses far less ink than the straight middle of the band.
        const unsigned sw=480,sh=320;const auto world=similarity_scene::MakeScene(sw,sh,260,20,23);
        const auto sa=similarity_scene::ViewA(world,sw,sh),sb=similarity_scene::ViewB(world,sw,sh,244,2,0,1,1);
        const auto p=SimilarityReviewGeometry(240,0,0,1,sw,sh);const auto scene=RenderPairSeamCut(sa,sb,p,small);
        const auto inverse=render::Invert(p.camera_b_to_camera_a);
        const auto ink=[&](unsigned x,unsigned y){
            std::array<double,3> va{},vb{};render::Point s{};
            const bool ha=render::SampleBilinear(sa,x,y,va),hb=render::TryTransform(inverse,{double(x),double(y)},s) && render::SampleBilinear(sb,s.x,s.y,vb);
            return ha && hb && (va[1]<180)!=(vb[1]<180); // ink in only one of them

        };
        unsigned seam_ink=0,middle_ink=0;
        for(unsigned y=0;y<sh;++y){seam_ink+=ink(scene.record.seam_x[y],y);middle_ink+=ink(360,y);}
        // The fewest ink crossings any path with the same margin and step limit can have.
        std::vector<unsigned> best(sw,0),next(sw);const unsigned lo=240+small.edge_margin_pixels,hi=480-small.edge_margin_pixels;
        for(unsigned y=0;y<sh;++y){for(unsigned x=lo;x<hi;++x){unsigned m=best[x];if(y){if(x>lo)m=std::min(m,best[x-1]);if(x+1<hi)m=std::min(m,best[x+1]);}next[x]=m+ink(x,y);}std::swap(best,next);}
        unsigned fewest=~0u;for(unsigned x=lo;x<hi;++x)fewest=std::min(fewest,best[x]);
        std::cout<<"scene_seam_ink="<<seam_ink<<" fewest_ink="<<fewest<<" middle_ink="<<middle_ink<<" seam_x="<<scene.record.summary.x_minimum<<".."<<scene.record.summary.x_maximum<<'\n';
        check(middle_ink>20 && seam_ink*2<=middle_ink && seam_ink<=fewest+(middle_ink-fewest)/2,"seam crosses ink close to the fewest possible");
        bool margin=true;for(const auto x:scene.record.seam_x)if(x<240+small.edge_margin_pixels || x>=480-small.edge_margin_pixels)margin=false;
        check(margin,"seam keeps the edge margin");
        // Exactly registered pair: ratio one, and away from the band only A or
        // only B is used, byte for byte as RenderPair.
        const auto exact_b=similarity_scene::ViewB(world,sw,sh,240,0,0,1,1);const auto registered=RenderPairSeamCut(sa,exact_b,p,small);
        check(registered.record.ratio_minimum==1 && registered.record.ratio_maximum==1 && !registered.record.saturation_limited_pixels && registered.record.difference_maximum==0,"equal brightness gives ratio one");
        const auto registered_feather=render::RenderPair(sa,exact_b,p);bool outside=true;
        for(unsigned y=0;y<sh;++y)for(unsigned x:{0u,100u,239u,480u,600u,719u})if(Pixel(registered.image,x,y)!=Pixel(registered_feather.image,x,y))outside=false;
        check(outside,"outside the band the seam render equals the feather render");
        // Displacement probe recovers the known offset: B shows A's content
        // 4 px left and 2 px up of where A has it.
        const auto probes=ProbeSeamDisplacement(sa,sb,p,scene.record.seam_x,32,8,24);unsigned valid=0,exact=0;
        for(const auto& q:probes)if(q.valid){++valid;if(q.dx==-4 && q.dy==-2)++exact;}
        std::cout<<"probes="<<probes.size()<<" valid="<<valid<<" exact="<<exact<<'\n';
        check(valid>=3 && exact*10>=valid*8,"probe finds the misregistration");
        reject([&]{(void)ProbeSeamDisplacement(sa,sb,p,{1,2},32,8,24);},"probe needs a full seam");

        // Brightness: B darker by a gain that changes down the rows.
        auto dim=similarity_scene::ViewB(world,sw,sh,240,0,0,1,1);
        const auto gain=[&](unsigned y){return 0.80+0.15*y/(sh-1);};
        for(unsigned y=0;y<sh;++y)for(unsigned x=0;x<sw;++x)for(int c=0;c<3;++c){auto& v=dim.bgr[(static_cast<size_t>(y)*sw+x)*3+c];v=static_cast<std::uint8_t>(std::lround(v*gain(y)));}
        const auto lit=RenderPairSeamCut(sa,dim,p,small);double step=0,raw=0;unsigned n=0;
        for(unsigned y=0;y<sh;++y){
            const auto xs=lit.record.seam_x[y];
            for(unsigned x=xs+3;x<=xs+6;++x){step+=std::abs(Lum(lit.image,x,y)-Lum(sa,x,y));
                std::array<double,3> vb{};render::SampleBilinear(dim,x-240.0,y,vb);raw+=std::abs(vb[1]-Lum(sa,x,y));++n;}
        }
        std::cout<<"brightness_step="<<step/n<<" uncorrected="<<raw/n<<" ratio="<<lit.record.ratio_minimum<<".."<<lit.record.ratio_maximum<<'\n';
        check(step/n<3 && raw/n>20,"ratio removes the step at the seam");
        check(std::abs(lit.record.ratio_minimum-1/0.95)<0.03 && std::abs(lit.record.ratio_maximum-1/0.80)<0.03,"ratio follows the row-wise change");
        check(lit.row_ratio.front()>lit.row_ratio.back(),"ratio is not one global value");
        check(lit.record.difference_mean<4,"seam difference after correction is small");

        // One gain for the three channels; saturation lowers the gain.
        const auto ca=Flat(200,80,{200,220,250}),cb=Flat(200,80,{160,176,200});
        const auto color=RenderPairSeamCut(ca,cb,Shift(100),small);
        const auto right=Pixel(color.image,color.record.seam_x[40]+3,40);
        check(std::abs(color.record.ratio_minimum-1.25)<1e-6 && std::abs(right[0]-200)<=1 && std::abs(right[1]-220)<=1 && std::abs(right[2]-250)<=1,"colour ratio kept under gain");
        const auto sa2=Flat(200,80,{250,250,250}),sb2=Flat(200,80,{180,200,240});
        const auto clipped=RenderPairSeamCut(sa2,sb2,Shift(100),small);const auto c2=Pixel(clipped.image,clipped.record.seam_x[40]+3,40);
        check(clipped.record.saturation_limited_pixels>0 && c2[2]==255 && std::abs(c2[0]-191)<=1 && std::abs(c2[1]-213)<=1,"saturation keeps the colour ratio");
        check(Pixel(clipped.image,299,40)==std::array<int,3>{180,200,240},"gain fades to one at the right edge");

        // Rotation and vertical offset through the review similarity crop.
        const auto turned=similarity_scene::ViewB(world,sw,sh,241,3,1.0,1,0.9);const auto q=SimilarityReviewGeometry(241,3,1.0,1,sw,sh);
        const auto rotated=RenderPairSeamCut(sa,turned,q,kSeamBlendDesign);const auto plan=render::PlanPairCanvas(sw,sh,sw,sh,q);
        bool rotated_steps=true;for(size_t y=1;y<rotated.record.seam_x.size();++y)if(std::abs(int(rotated.record.seam_x[y])-int(rotated.record.seam_x[y-1]))>1)rotated_steps=false;
        check(rotated.image.width==plan.output_width && rotated.image.height==plan.output_height && rotated.record.seam_x.size()==plan.output_height && rotated_steps,"rotated pair renders with a connected seam");
        check(rotated.record.ratio_minimum>1.05 && rotated.record.ratio_maximum<1.17,"rotated pair ratio near the gain");

        // Rejections.
        auto vertical=Shift(120);vertical.layout=a0::m2::StitchLayout::camera_a_top_camera_b_bottom;
        reject([&]{(void)RenderPairSeamCut(a,b,vertical,small);},"top/bottom layout");
        reject([&]{(void)RenderPairSeamCut(a,b,Shift(260),small);},"no overlap");
        reject([&]{(void)RenderPairSeamCut(a,b,Shift(120),{4,8,8,40000});},"invalid settings");
        auto broken=b;broken.bgr.pop_back();reject([&]{(void)RenderPairSeamCut(a,broken,Shift(120),small);},"broken buffer");
    }catch(const std::exception& e){++failures;std::cerr<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
