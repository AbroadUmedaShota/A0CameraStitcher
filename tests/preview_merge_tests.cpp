#include "a0/phase0/experimental/preview_merge.hpp"
#include "a0/phase0/experimental/preview_alignment.hpp"
#include <iostream>
using namespace a0::phase0::experimental;
using a0::m2::render::BgrImage;
int main(){
    int checks=0,failures=0;auto check=[&](bool value){++checks;if(!value)++failures;};
    auto reject=[&](auto call){bool threw=false;try{call();}catch(const std::invalid_argument&){threw=true;}check(threw);};
    BgrImage a{8,4,std::vector<std::uint8_t>(8*4*3)},b=a;
    for(size_t i=0;i<a.bgr.size();i+=3){a.bgr[i+2]=200;b.bgr[i]=200;}
    const auto merged=RenderPreviewMerge(a,b,4,0);check(merged.image.width==12 && merged.image.height==4);
    auto pixel=[&](unsigned x,unsigned channel){return merged.image.bgr[x*3+channel];};
    check(pixel(0,2)==200 && pixel(0,0)==0);check(pixel(11,0)==200 && pixel(11,2)==0);
    check(pixel(6,0)==100 && pixel(6,2)==100);check(merged.seam_navigation.available);
    for(int dy:{-3,-1,1,3}){const auto r=RenderPreviewMerge(a,b,4,dy);check(r.image.width==12 && r.image.height==4-std::abs(dy));}
    reject([&]{RenderPreviewMerge(a,b,0,0);});reject([&]{RenderPreviewMerge(a,b,8,0);});
    reject([&]{RenderPreviewMerge(a,b,4,4);});reject([&]{RenderPreviewMerge(a,b,4,-4);});
    auto bad=b;bad.width=7;reject([&]{RenderPreviewMerge(a,bad,4,0);});bad=b;bad.bgr.pop_back();reject([&]{RenderPreviewMerge(a,bad,4,0);});
    const auto now=std::chrono::steady_clock::now();using namespace std::chrono_literals;
    check(PreviewPairFresh(now,now-1s,now));check(!PreviewPairFresh(now,now-2s,now));
    check(!PreviewPairFresh(now,now,now-2s));check(!PreviewPairFresh(now,{},now));check(!PreviewPairFresh(now,now+1ms,now));
    // Independent planar scene: each camera crops a different known rectangle.
    auto world=[](int x,int y){unsigned v=static_cast<unsigned>(x+1000)*2654435761u ^ static_cast<unsigned>(y+1000)*2246822519u;v^=v>>13;v*=3266489917u;return static_cast<unsigned char>(v%96);};
    for(const auto geometry:{std::array<int,4>{80,60,32,3},std::array<int,4>{80,60,47,-4},std::array<int,4>{320,200,113,7}}){
        const auto [width,height,dx,dy]=geometry;BgrImage left{static_cast<unsigned>(width),static_cast<unsigned>(height),{}},right=left;
        left.bgr.resize(static_cast<size_t>(width)*height*3);right.bgr.resize(left.bgr.size());
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)for(int c=0;c<3;++c){
            const auto index=(static_cast<size_t>(y)*width+x)*3+c;left.bgr[index]=world(x,y);right.bgr[index]=static_cast<unsigned char>(world(x+dx,y+dy)*2+20);
        }
        const auto proposal=EstimatePreviewTranslation(left,right);
        check(proposal && proposal->overlap_pixels==static_cast<unsigned>(width-dx) && proposal->vertical_pixels==dy && proposal->correlation>0.999);
    }
    check(!EstimatePreviewTranslation(a,b)); // Uniform views carry no alignment evidence.
    reject([&]{EstimatePreviewTranslation(a,bad);});
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
