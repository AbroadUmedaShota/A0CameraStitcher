#pragma once
#include "a0/m2/experimental/review_viewport.hpp"
#include <numbers>

namespace a0::m2::experimental {
// Development-only similarity, about B's image center. x/y move that center
// relative to A. The crop is a conservative covered rectangle, not a quality
// acceptance tolerance. Product projection/profile APIs are not involved.
inline render::PairRenderParameters SimilarityReviewGeometry(int x,int y,double degrees,double scale,unsigned width,unsigned height){
    (void)render::PixelCount(width,height);
    if(((width<4 || height<4) && (degrees!=0 || scale!=1)) || x<=0 || x>=static_cast<int>(width) || y<=-static_cast<int>(height) || y>=static_cast<int>(height) ||
        !std::isfinite(degrees) || degrees<-180 || degrees>180 || !std::isfinite(scale) || scale<=0)
        throw std::invalid_argument("invalid review similarity");
    const auto angle=degrees*std::numbers::pi/180,c=std::cos(angle),s=std::sin(angle),cx=width/2.0,cy=height/2.0;
    render::PairRenderParameters p{{scale*c,-scale*s,x+cx-scale*c*cx+scale*s*cy,scale*s,scale*c,y+cy-scale*s*cx-scale*c*cy,0,0,1},StitchLayout::camera_a_left_camera_b_right,{}};
    const auto raw=render::PlanPairCanvas(width,height,width,height,p);
    if(degrees==0 && scale==1){const auto trim=static_cast<unsigned>(std::abs(y));p.crop={0,trim,0,trim};return p;}
    // An axis-aligned rectangle inscribed in B, with one input pixel inset
    // for non-identity transforms. Every retained output sample is covered.
    const double w=width-2.0,h=height-2.0;
    const auto k=1/std::max(std::abs(c)+std::abs(s)*h/w,std::abs(c)+std::abs(s)*w/h);
    double left=x+cx-scale*w*k/2,right=x+cx+scale*w*k/2,top=y+cy-scale*h*k/2,bottom=y+cy+scale*h*k/2;
    const auto end_x=std::floor(right),begin_y=std::ceil(std::max(0.0,top)),end_y=std::floor(std::min(double(height),bottom));
    if(left>=width || end_x<=width || end_y<=begin_y)throw std::invalid_argument("review similarity has no covered left/right crop");
    p.crop={static_cast<unsigned>(-raw.origin_x),static_cast<unsigned>(begin_y-raw.origin_y),
        static_cast<unsigned>(raw.origin_x+raw.canvas_width-end_x),static_cast<unsigned>(raw.origin_y+raw.canvas_height-end_y)};
    (void)render::PlanPairCanvas(width,height,width,height,p);return p;
}
enum class ReviewComparison { overlay, difference, camera_a, camera_b };
inline std::vector<std::uint8_t> CompareReviewBgra(const render::BgrImage& a,const render::BgrImage& b,
    const render::PairRenderParameters& parameters,const ReviewViewport& view,ReviewComparison mode){
    if(a.width!=b.width || a.height!=b.height || a.bgr.size()!=render::PixelCount(a.width,a.height)*3 || b.bgr.size()!=a.bgr.size())
        throw std::invalid_argument("invalid comparison pair");
    const auto plan=render::PlanPairCanvas(a.width,a.height,b.width,b.height,parameters);
    if(view.image_width!=plan.output_width || view.image_height!=plan.output_height)throw std::invalid_argument("comparison viewport mismatch");
    const auto inverse=render::Invert(parameters.camera_b_to_camera_a);const auto scale=view.Scale();const auto origin=view.Origin();
    std::vector<std::uint8_t> result(static_cast<size_t>(render::PixelCount(view.view_width,view.view_height))*4,0);
    for(unsigned y=0;y<view.view_height;++y)for(unsigned x=0;x<view.view_width;++x){
        const auto ix=std::floor((x+0.5-origin.x)/scale),iy=std::floor((y+0.5-origin.y)/scale);
        if(ix<0 || iy<0 || ix>=view.image_width || iy>=view.image_height)continue;
        const auto gx=plan.origin_x+parameters.crop.left+ix,gy=plan.origin_y+parameters.crop.top+iy;
        std::array<double,3> va{},vb{};render::Point source{};
        const bool ha=render::SampleBilinear(a,gx,gy,va),hb=render::TryTransform(inverse,{gx,gy},source) && render::SampleBilinear(b,source.x,source.y,vb);
        const auto at=(static_cast<size_t>(y)*view.view_width+x)*4;
        for(int channel=0;channel<3;++channel){
            double value=((static_cast<unsigned>(ix)/12+static_cast<unsigned>(iy)/12)%2)?40:64;
            if(mode==ReviewComparison::overlay){if(ha && hb)value=(va[channel]+vb[channel])/2;else if(ha)value=va[channel];else if(hb)value=vb[channel];}
            else if(mode==ReviewComparison::difference){if(ha && hb)value=std::abs(va[channel]-vb[channel]);}
            else if(mode==ReviewComparison::camera_a && ha)value=va[channel];
            else if(mode==ReviewComparison::camera_b && hb)value=vb[channel];
            result[at+channel]=static_cast<std::uint8_t>(std::clamp(std::lround(value),0L,255L));
        }
    }
    return result;
}
}
