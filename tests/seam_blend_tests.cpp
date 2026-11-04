#include "a0/m2/experimental/seam_blend.hpp"
#include "a0/m2/experimental/review_alignment.hpp"
#include "similarity_scene.hpp"
#include <array>
#include <cmath>
#include <cstring>
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
// FNV-1a over the image bytes, the seam columns and the bit patterns of the row ratios.
std::uint64_t Digest(const SeamBlendResult& r){
    std::uint64_t h=14695981039346656037ull;const auto mix=[&](std::uint64_t v,int bytes){for(int i=0;i<bytes;++i){h^=(v>>(8*i))&0xff;h*=1099511628211ull;}};
    for(const auto v:r.image.bgr)mix(v,1);
    for(const auto v:r.record.seam_x)mix(v,4);
    for(const auto v:r.row_ratio){std::uint64_t bits{};std::memcpy(&bits,&v,sizeof bits);mix(bits,8);}
    return h;
}
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

        // Pinned outputs of RenderPairSeamCut (#398 split the path search from the blend; values taken before the split).
        check(Digest(seam)==13560569947543128899ull && Digest(scene)==1709069572549631090ull && Digest(lit)==13321898565532633760ull &&
            Digest(color)==2041195848238639772ull && Digest(clipped)==17039523872241258339ull && Digest(rotated)==2139992240002134470ull,
            "RenderPairSeamCut output is unchanged by the split");
        check(!seam.record.fixed_seam,"a searched seam is not marked fixed");

        // ---- Fixed vertical seam (#396, #398) ----
        // Flat A and B of equal luminance, B 100 columns to the right: both are present on columns 100..199.
        {
            const unsigned fw=200,fh=80,fcolumn=150;
            const auto fa=Flat(fw,fh,{200,200,200}),fb=Flat(fw,fh,{100,200,238}); // 0.114*100+0.587*200+0.299*238 = 200
            const auto fixed=RenderPairFixedSeam(fa,fb,Shift(100),fcolumn,small);
            check(fixed.record.fixed_seam && fixed.record.seam_x.size()==fh && fixed.record.summary.x_minimum==fcolumn && fixed.record.summary.x_maximum==fcolumn,"fixed seam record");
            bool same_column=true;for(const auto x:fixed.record.seam_x)same_column=same_column && x==fcolumn;
            check(same_column && fixed.image.width==300 && fixed.image.height==fh,"every row uses the seam column; canvas equals the pair canvas");
            check(fixed.record.settings.width_pixels==small.width_pixels && fixed.record.settings.edge_margin_pixels==small.edge_margin_pixels,"settings are recorded");
            check(fixed.record.ratio_minimum>0.999 && fixed.record.ratio_maximum<1.001,"equal luminance gives ratio one");
            // Blend width 4: weights 0.25, 0.5, 0.75 on columns seam-1..seam+1; A only left of that, B only right of it.
            unsigned wrong=0;
            for(unsigned y=0;y<fh;++y)for(unsigned x=0;x<300;++x){
                const int d=static_cast<int>(x)-static_cast<int>(fcolumn);
                const double t=d<=-2?0.0:d>=2?1.0:d/4.0+0.5;
                const auto got=Pixel(fixed.image,x,y);
                if(std::abs(got[0]-(200-100*t))>1 || std::abs(got[1]-200)>1 || std::abs(got[2]-(200+38*t))>1)++wrong;
            }
            check(!wrong,"left of the blend only A, right of it only B, mixed inside the blend width");
            const auto again=RenderPairFixedSeam(fa,fb,Shift(100),fcolumn,small);
            check(again.image.bgr==fixed.image.bgr && again.row_ratio==fixed.row_ratio && again.record.seam_x==fixed.record.seam_x,"fixed seam is deterministic");
            // edge_margin_pixels is not applied: a column next to the rim of the overlap renders with a margin far larger than the overlap.
            const auto rim=RenderPairFixedSeam(fa,fb,Shift(100),101,{4,5000,8,16});
            check(rim.record.seam_x.front()==101 && rim.record.settings.edge_margin_pixels==5000,"edge margin does not restrict a fixed seam");
            const auto rim_right=RenderPairFixedSeam(fa,fb,Shift(100),198,small);
            check(rim_right.record.seam_x.back()==198,"a column next to the right rim of the overlap renders");

            // Failure: the columns whose weight is strictly between 0 and 1 must be in the overlap (columns 100..199).
            for(const std::uint32_t column:{0u,50u,99u,100u,199u,200u,299u,300u,4000000000u,0xffffffffu})
                reject([&]{(void)RenderPairFixedSeam(fa,fb,Shift(100),column,small);},"seam column out of the overlap (width 4)");
            reject([&]{(void)RenderPairFixedSeam(fa,fb,Shift(100),101,{5,8,8,16});},"half of an odd blend width leaves the overlap (101, width 5)");
            check(RenderPairFixedSeam(fa,fb,Shift(100),102,{5,8,8,16}).record.seam_x.front()==102,"odd blend width: 102 is the first column");
            reject([&]{(void)RenderPairFixedSeam(fa,fb,Shift(100),99,{1,8,8,16});},"blend width 1 needs the seam column itself");
            check(RenderPairFixedSeam(fa,fb,Shift(100),100,{1,8,8,16}).record.seam_x.front()==100,"blend width 1 accepts the first overlap column");
            reject([&]{(void)RenderPairFixedSeam(fa,fb,Shift(100),102,kSeamBlendDesign);},"design width 8 needs 3 columns on each side");
            check(RenderPairFixedSeam(fa,fb,Shift(100),103,kSeamBlendDesign).record.seam_x.front()==103,"design width 8: 103 is the first column");
            reject([&]{(void)RenderPairFixedSeam(fa,fb,Shift(100),fcolumn,{4,8,8,40000});},"fixed seam: invalid settings");
            reject([&]{(void)RenderPairFixedSeam(fa,fb,vertical,fcolumn,small);},"fixed seam: top/bottom layout");
            reject([&]{(void)RenderPairFixedSeam(fa,fb,Shift(260),fcolumn,small);},"fixed seam: no overlap");
            auto fbroken=fb;fbroken.bgr.pop_back();reject([&]{(void)RenderPairFixedSeam(fa,fbroken,Shift(100),fcolumn,small);},"fixed seam: broken buffer");
            // CanRenderFixedSeam agrees with RenderPairFixedSeam on every column around the overlap (columns 100..199).
            bool agrees=true;
            for(std::uint32_t column=90;column<=210;++column){
                bool renders=true;try{(void)RenderPairFixedSeam(fa,fb,Shift(100),column,small);}catch(const std::invalid_argument&){renders=false;}
                agrees=agrees && CanRenderFixedSeam(fa,fb,Shift(100),column,small)==renders && renders==(column>=101 && column<=198);
            }
            check(agrees,"CanRenderFixedSeam matches RenderPairFixedSeam; columns 101..198 fit blend width 4");
            check(!CanRenderFixedSeam(fa,fb,Shift(100),0xffffffffu,small) && CanRenderFixedSeam(fa,fb,Shift(100),101,{4,5000,8,16}),"CanRenderFixedSeam: far column no, rim column yes");
            reject([&]{(void)CanRenderFixedSeam(fa,fb,Shift(100),fcolumn,{4,8,8,40000});},"CanRenderFixedSeam: invalid settings");
            reject([&]{(void)CanRenderFixedSeam(fa,fb,vertical,fcolumn,small);},"CanRenderFixedSeam: top/bottom layout");

            // The same blend as the searched seam: when the search settles on a straight column, fixing that column gives the same output.
            const auto ca2=Flat(200,80,{200,220,250}),cb2=Flat(200,80,{160,176,200});
            const auto cut=RenderPairSeamCut(ca2,cb2,Shift(100),small);
            bool straight=true;for(const auto x:cut.record.seam_x)straight=straight && x==cut.record.seam_x.front();
            check(straight,"flat pair: the searched seam is a straight column");
            const auto pinned=RenderPairFixedSeam(ca2,cb2,Shift(100),cut.record.seam_x.front(),small);
            check(Digest(pinned)==Digest(cut) && pinned.record.difference_mean==cut.record.difference_mean && pinned.record.difference_maximum==cut.record.difference_maximum &&
                pinned.record.ratio_minimum==cut.record.ratio_minimum && pinned.record.saturation_limited_pixels==cut.record.saturation_limited_pixels,"fixing the searched column reproduces RenderPairSeamCut");
            check(pinned.record.fixed_seam && !cut.record.fixed_seam,"only the fixed render is marked fixed");
        }

        // The seam and the placement do not depend on the content: textures shifted by different amounts.
        {
            const unsigned nw=200,nh=64,column=150,dx=100;
            const auto noise=[&](std::uint64_t seed,unsigned shift){
                BgrImage image{nw,nh,std::vector<std::uint8_t>(static_cast<size_t>(nw)*nh*3)};similarity_scene::Lcg r{seed};
                std::vector<std::uint8_t> base(static_cast<size_t>(nw)*nh);for(auto& v:base)v=static_cast<std::uint8_t>(60+r.Next(121));
                for(unsigned y=0;y<nh;++y)for(unsigned x=0;x<nw;++x)for(int c=0;c<3;++c)image.bgr[(static_cast<size_t>(y)*nw+x)*3+c]=base[static_cast<size_t>(y)*nw+(x+shift)%nw];
                return image;
            };
            // Expected pixel: the documented blend with the result's own row ratio.
            const auto mismatches=[&](const BgrImage& a,const BgrImage& b,const SeamBlendResult& r){
                unsigned wrong=0;const double seam_x=column,total=nw+dx;
                for(unsigned y=0;y<nh;++y)for(unsigned x=0;x<total;++x){
                    const double target=std::clamp((x-seam_x)/4+0.5,0.0,1.0);const bool ha=x<nw,hb=x>=dx;
                    const double w=ha && hb?target:hb?1:0,gain=x<=seam_x?r.row_ratio[y]:r.row_ratio[y]+(1-r.row_ratio[y])*(x-seam_x)/(total-seam_x);
                    for(int c=0;c<3;++c){
                        const double va=ha?a.bgr[(static_cast<size_t>(y)*nw+x)*3+c]:0,vb=hb?b.bgr[(static_cast<size_t>(y)*nw+(x-dx))*3+c]:0;
                        if(std::abs(Pixel(r.image,x,y)[c]-((1-w)*va+w*gain*vb))>1)++wrong;
                    }
                }
                return wrong;
            };
            for(const auto& [sa_shift,sb_shift] : {std::pair{0u,0u},std::pair{7u,0u},std::pair{0u,11u},std::pair{29u,53u}}){
                const auto a=noise(11,sa_shift),b=noise(23,sb_shift);const auto r=RenderPairFixedSeam(a,b,Shift(dx),column,small);
                bool fixed_columns=r.record.seam_x.size()==nh;for(const auto x:r.record.seam_x)fixed_columns=fixed_columns && x==column;
                check(fixed_columns && r.record.saturation_limited_pixels==0 && mismatches(a,b,r)==0,"a given column stays the seam whatever the content, and B stays where the mapping puts it");
            }
        }

        // Brightness: registered scene, B uniformly 0.8; the ratio is the #368 window-sum ratio and removes the step at the seam.
        {
            const unsigned column=360;auto dimmed=exact_b;for(auto& v:dimmed.bgr)v=static_cast<std::uint8_t>(std::lround(v*0.8));
            const auto r=RenderPairFixedSeam(sa,dimmed,p,column,small);
            const auto lum=[&](const BgrImage& image,double x,double y){std::array<double,3> v{};render::SampleBilinear(image,x,y,v);return v[0]*0.114+v[1]*0.587+v[2]*0.299;};
            std::vector<double> window_a(sh,0),window_b(sh,0);
            for(unsigned y=0;y<sh;++y)for(unsigned x=column-small.brightness_half_width_pixels;x<=column+small.brightness_half_width_pixels;++x){window_a[y]+=lum(sa,x,y);window_b[y]+=lum(dimmed,x-240.0,y);}
            const auto box=[&](const std::vector<double>& v){
                const long radius=small.brightness_smoothing_rows;std::vector<double> out(v.size(),0);
                for(long k=0;k<long(v.size());++k)for(long j=std::max(0L,k-radius);j<=std::min(long(v.size())-1,k+radius);++j)out[k]+=v[j];
                return out;
            };
            const auto smooth_a=box(box(window_a)),smooth_b=box(box(window_b));double worst=0;
            for(unsigned y=0;y<sh;++y)worst=std::max(worst,std::abs(r.row_ratio[y]/(smooth_a[y]/smooth_b[y])-1));
            std::cout<<"fixed_seam_ratio_error="<<worst<<" ratio="<<r.record.ratio_minimum<<".."<<r.record.ratio_maximum<<'\n';
            check(worst<1e-4,"ratio is the window-sum ratio smoothed along rows");
            check(std::abs(r.record.ratio_minimum-1.25)<0.02 && std::abs(r.record.ratio_maximum-1.25)<0.02,"ratio follows the dimming");
            double step=0,raw_step=0;unsigned n=0;
            for(unsigned y=0;y<sh;++y)for(unsigned x=column+3;x<=column+6;++x){step+=std::abs(Lum(r.image,x,y)-lum(sa,x,y));raw_step+=std::abs(lum(dimmed,x-240.0,y)-lum(sa,x,y));++n;}
            std::cout<<"fixed_seam_step="<<step/n<<" uncorrected="<<raw_step/n<<'\n';
            check(step/n<3 && raw_step/n>20,"ratio removes the step at the fixed seam");
        }

        // The overlap changes from row to row (rotated B): a column outside it on one row fails, one inside every row renders.
        {
            const unsigned rw=480,rh=320;
            const auto rotated_world=similarity_scene::MakeScene(rw,rh,260,20,23);
            const auto ra=similarity_scene::ViewA(rotated_world,rw,rh),rb=similarity_scene::ViewB(rotated_world,rw,rh,241,3,2.0,1,1);
            const auto rq=SimilarityReviewGeometry(241,3,2.0,1,rw,rh);
            const auto rplan=render::PlanPairCanvas(rw,rh,rw,rh,rq);const auto rinverse=render::Invert(rq.camera_b_to_camera_a);
            const double gx=rplan.origin_x+rq.crop.left,gy=rplan.origin_y+rq.crop.top;
            unsigned max_first=0,min_last=~0u,min_first=~0u;
            for(unsigned y=0;y<rplan.output_height;++y){
                unsigned first=~0u,last=0;
                for(unsigned x=0;x<rplan.output_width;++x){
                    std::array<double,3> va{},vb{};render::Point s{};
                    if(render::SampleBilinear(ra,gx+x,gy+y,va) && render::TryTransform(rinverse,{gx+x,gy+y},s) && render::SampleBilinear(rb,s.x,s.y,vb)){first=std::min(first,x);last=x;}
                }
                max_first=std::max(max_first,first);min_first=std::min(min_first,first);min_last=std::min(min_last,last);
            }
            std::cout<<"overlap first column "<<min_first<<".."<<max_first<<" last column from "<<min_last<<'\n';
            check(max_first>=min_first+6 && min_last>=max_first+40,"rotated pair: the overlap edge moves between rows");
            reject([&]{(void)RenderPairFixedSeam(ra,rb,rq,max_first-3,small);},"seam outside the overlap on some rows (left)");
            reject([&]{(void)RenderPairFixedSeam(ra,rb,rq,min_last+3,small);},"seam outside the overlap on some rows (right)");
            reject([&]{(void)RenderPairFixedSeam(ra,rb,rq,max_first,small);},"blend half outside the overlap on the tightest row");
            check(!CanRenderFixedSeam(ra,rb,rq,max_first-3,small) && !CanRenderFixedSeam(ra,rb,rq,min_last+3,small) && !CanRenderFixedSeam(ra,rb,rq,max_first,small) &&
                CanRenderFixedSeam(ra,rb,rq,max_first+1,small) && CanRenderFixedSeam(ra,rb,rq,min_last-1,small) && !CanRenderFixedSeam(ra,rb,rq,min_last,small),"CanRenderFixedSeam follows the tightest row");
            const auto inside_rows=RenderPairFixedSeam(ra,rb,rq,max_first+1,small);
            bool inside_columns=inside_rows.record.seam_x.size()==rplan.output_height;for(const auto x:inside_rows.record.seam_x)inside_columns=inside_columns && x==max_first+1;
            check(inside_columns,"a column inside the overlap on every row renders with that column in every row");

            // Scaling the condition to a smaller view (Live View size): the geometry corresponds at the scale ratio.
            const unsigned column=(max_first+min_last)/2;const FixedSeamCondition capture{rq,column};
            const auto same=ScaleFixedSeamCondition(capture,rw,rh,rw,rh);
            check(same.parameters.camera_b_to_camera_a==rq.camera_b_to_camera_a && same.seam_column==column && same.parameters.crop.left==rq.crop.left &&
                same.parameters.crop.top==rq.crop.top && same.parameters.crop.right==rq.crop.right && same.parameters.crop.bottom==rq.crop.bottom,"scale 1 gives the same condition");
            const auto b_capture=render::TransformedBounds(rw,rh,rq.camera_b_to_camera_a);
            for(const auto& [vw,vh] : {std::pair{240u,160u},std::pair{360u,240u},std::pair{320u,160u},std::pair{100u,64u}}){
                const double sx=double(vw)/rw,sy=double(vh)/rh;const auto scaled=ScaleFixedSeamCondition(capture,rw,rh,vw,vh);
                const auto vplan=render::PlanPairCanvas(vw,vh,vw,vh,scaled.parameters);
                const auto b_view=render::TransformedBounds(vw,vh,scaled.parameters.camera_b_to_camera_a);
                const double seam_capture=gx+column,seam_view=vplan.origin_x+scaled.parameters.crop.left+scaled.seam_column;
                // B is placed exactly at the scale ratio.
                const bool placed=std::abs(b_view.minimum_x-sx*b_capture.minimum_x)<1e-6 && std::abs(b_view.maximum_x-sx*b_capture.maximum_x)<1e-6 &&
                    std::abs(b_view.minimum_y-sy*b_capture.minimum_y)<1e-6 && std::abs(b_view.maximum_y-sy*b_capture.maximum_y)<1e-6;
                // The seam is within half a view pixel of its scaled position.
                const bool seam_ok=std::abs(seam_view-sx*seam_capture)<=0.5+1e-9;
                // The window never reaches outside the scaled capture window and its edges moved by less than 2 view pixels.
                const double wx0=vplan.origin_x+scaled.parameters.crop.left,wx1=wx0+vplan.output_width,wy0=vplan.origin_y+scaled.parameters.crop.top,wy1=wy0+vplan.output_height;
                const double ex0=wx0-sx*gx,ex1=sx*(gx+rplan.output_width)-wx1,ey0=wy0-sy*gy,ey1=sy*(gy+rplan.output_height)-wy1;
                const bool window_ok=std::min({ex0,ex1,ey0,ey1})>=-1e-9 && std::max({ex0,ex1,ey0,ey1})<2;
                std::cout<<"view "<<vw<<"x"<<vh<<" seam "<<seam_view<<" vs "<<sx*seam_capture<<" window edges moved "<<ex0<<" "<<ex1<<" "<<ey0<<" "<<ey1<<'\n';
                check(placed && seam_ok && window_ok,"scaled condition keeps B, the seam and the window at the scale ratio");
            }
            // Rendering at 1/2 scale from box-downsampled frames: the scaled condition is a valid fixed seam of the view.
            const auto half=[&](const BgrImage& image){
                BgrImage out{rw/2,rh/2,std::vector<std::uint8_t>(static_cast<size_t>(rw/2)*(rh/2)*3)};
                for(unsigned y=0;y<rh/2;++y)for(unsigned x=0;x<rw/2;++x)for(int c=0;c<3;++c){
                    const auto at=[&](unsigned i,unsigned j){return int(image.bgr[(static_cast<size_t>(j)*rw+i)*3+c]);};
                    out.bgr[(static_cast<size_t>(y)*(rw/2)+x)*3+c]=static_cast<std::uint8_t>((at(2*x,2*y)+at(2*x+1,2*y)+at(2*x,2*y+1)+at(2*x+1,2*y+1)+2)/4);
                }
                return out;
            };
            const auto view=ScaleFixedSeamCondition(capture,rw,rh,rw/2,rh/2);
            const auto live=RenderPairFixedSeam(half(ra),half(rb),view.parameters,view.seam_column,small);
            bool view_columns=!live.record.seam_x.empty();for(const auto x:live.record.seam_x)view_columns=view_columns && x==view.seam_column;
            check(view_columns && live.record.fixed_seam,"the scaled condition renders as a fixed seam at the view size");
            reject([&]{(void)ScaleFixedSeamCondition(capture,0,rh,240,160);},"scale: empty capture size");
            reject([&]{(void)ScaleFixedSeamCondition(capture,rw,rh,240,0);},"scale: empty view size");
            reject([&]{(void)ScaleFixedSeamCondition({rq,rplan.output_width},rw,rh,240,160);},"scale: seam column outside the capture output");
            auto top_bottom=capture;top_bottom.parameters.layout=a0::m2::StitchLayout::camera_a_top_camera_b_bottom;
            reject([&]{(void)ScaleFixedSeamCondition(top_bottom,rw,rh,240,160);},"scale: top/bottom layout");
        }
    }catch(const std::exception& e){++failures;std::cerr<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
