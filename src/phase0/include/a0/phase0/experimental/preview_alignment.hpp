#pragma once
#include "a0/m2/render.hpp"
#include <algorithm>
#include <cmath>
#include <optional>

namespace a0::phase0::experimental {
struct PreviewTranslationCandidate {unsigned overlap_pixels{};int vertical_pixels{};double correlation{};};
namespace preview_alignment_detail {
struct Gray {int width{},height{};std::vector<double> pixels;};
inline Gray Reduce(const a0::m2::render::BgrImage& image,int factor){
    if(image.width<4 || image.height<4 || image.width>640 || image.height>640 ||
        image.bgr.size()!=static_cast<size_t>(image.width)*image.height*3)throw std::invalid_argument("invalid alignment frame");
    Gray result{static_cast<int>(image.width)/factor,static_cast<int>(image.height)/factor,{}};
    result.pixels.resize(static_cast<size_t>(result.width)*result.height);
    for(int y=0;y<result.height;++y)for(int x=0;x<result.width;++x){
        double value=0;
        for(int yy=0;yy<factor;++yy)for(int xx=0;xx<factor;++xx){
            const auto index=(static_cast<size_t>(y*factor+yy)*image.width+x*factor+xx)*3;
            value+=image.bgr[index]*0.114+image.bgr[index+1]*0.587+image.bgr[index+2]*0.299;
        }
        result.pixels[static_cast<size_t>(y)*result.width+x]=value/(factor*factor);
    }
    return result;
}
// Normalized correlation over the actual shared rectangle. Gain and offset
// cancel algebraically; no image-dependent exposure correction is learned.
inline std::optional<double> Correlate(const Gray& a,const Gray& b,int dx,int dy,int stride){
    const int x0=dx,x1=a.width,y0=std::max(0,dy),y1=std::min(a.height,b.height+dy);
    if(dx<=0 || dx>=a.width || x1-x0<2 || y1-y0<2)return {};
    double sum_a=0,sum_b=0,aa=0,bb=0,ab=0,n=0;
    for(int y=y0;y<y1;y+=stride)for(int x=x0;x<x1;x+=stride){
        const double va=a.pixels[static_cast<size_t>(y)*a.width+x],vb=b.pixels[static_cast<size_t>(y-dy)*b.width+x-dx];
        sum_a+=va;sum_b+=vb;aa+=va*va;bb+=vb*vb;ab+=va*vb;++n;
    }
    const double variance_a=aa-sum_a*sum_a/n,variance_b=bb-sum_b*sum_b/n;
    if(n<4 || variance_a<=0 || variance_b<=0)return {};
    const double score=(ab-sum_a*sum_b/n)/std::sqrt(variance_a*variance_b);
    if(!std::isfinite(score))return {};return score;
}
}
// Button-triggered, bounded translation proposal for an UNCALIBRATED preview.
// The score is a diagnostic, never an optical acceptance threshold. Rotation,
// perspective, lens/color correction and approved-profile learning are absent.
// A user must visually review the result and can refine its integer shifts.
// Superseded in the live C by EstimateOverlapTranslation (#364); kept as the
// baseline that the negative-control tests run on the same inputs.
inline std::optional<PreviewTranslationCandidate> EstimatePreviewTranslation(
    const a0::m2::render::BgrImage& a,const a0::m2::render::BgrImage& b){
    using namespace preview_alignment_detail;
    if(a.width!=b.width || a.height!=b.height)throw std::invalid_argument("alignment frame dimensions differ");
    const int factor=std::max(1,static_cast<int>((std::max(a.width,a.height)+95)/96));
    const auto small_a=Reduce(a,factor),small_b=Reduce(b,factor);
    struct Match {int dx{},dy{};double score{-2};};Match best;bool tied=false;
    const int minimum_dx=std::max(1,(small_a.width+9)/10),maximum_dx=small_a.width*9/10;
    const int maximum_dy=std::min(100/factor,small_a.height/4);
    for(int dy=-maximum_dy;dy<=maximum_dy;++dy)for(int dx=minimum_dx;dx<=maximum_dx;++dx){
        const auto score=Correlate(small_a,small_b,dx,dy,1);if(!score)continue;
        if(*score>best.score+1e-12){best={dx,dy,*score};tied=false;}
        else if(std::abs(*score-best.score)<=1e-12)tied=true;
    }
    if(best.score==-2 || tied)return {};
    const auto full_a=Reduce(a,1),full_b=Reduce(b,1);
    const int center_x=best.dx*factor,center_y=best.dy*factor;
    best.score=-2;tied=false;
    const int max_vertical=std::min(100,static_cast<int>(a.height)/4);
    for(int dy=std::max(-max_vertical,center_y-factor);dy<=std::min(max_vertical,center_y+factor);++dy)
        for(int dx=std::max(static_cast<int>((a.width+9)/10),center_x-factor);dx<=std::min(static_cast<int>(a.width*9/10),center_x+factor);++dx){
            const auto score=Correlate(full_a,full_b,dx,dy,2);if(!score)continue;
            if(*score>best.score+1e-12){best={dx,dy,*score};tied=false;}
            else if(std::abs(*score-best.score)<=1e-12)tied=true;
        }
    if(best.score==-2 || tied)return {};
    return PreviewTranslationCandidate{a.width-static_cast<unsigned>(best.dx),best.dy,best.score};
}
}
