#pragma once
#include "a0/m2/render.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace a0::m2::experimental {
// Pure inspection geometry. 100% means exactly one display pixel per saved
// image pixel; fit and zoom never alter the saved image or stitch parameters.
struct ReviewViewport {
    unsigned image_width{},image_height{},view_width{},view_height{};
    double zoom{},center_x{},center_y{}; // zoom 0 = fit, otherwise 1 or 2
    double Scale() const {
        if(!image_width || !image_height || !view_width || !view_height)throw std::invalid_argument("empty review viewport");
        if(zoom!=0 && zoom!=1 && zoom!=2)throw std::invalid_argument("unsupported review zoom");
        return zoom?zoom:std::min(double(view_width)/image_width,double(view_height)/image_height);
    }
    void Constrain(){
        const auto scale=Scale();
        auto clamp=[](double center,double extent,double visible){return visible>=extent?extent/2:std::clamp(center,visible/2,extent-visible/2);};
        center_x=clamp(center_x,image_width,view_width/scale);center_y=clamp(center_y,image_height,view_height/scale);
    }
    render::Point Origin() const {
        const auto scale=Scale();return {std::round(view_width/2.0-center_x*scale),std::round(view_height/2.0-center_y*scale)};
    }
    void Pan(double display_dx,double display_dy){const auto scale=Scale();center_x-=display_dx/scale;center_y-=display_dy/scale;Constrain();}
    std::optional<std::array<unsigned,2>> Pixel(unsigned x,unsigned y) const {
        if(x>=view_width || y>=view_height)return {};
        const auto scale=Scale();const auto o=Origin();
        const auto ix=std::floor((x+0.5-o.x)/scale),iy=std::floor((y+0.5-o.y)/scale);
        if(ix<0 || iy<0 || ix>=image_width || iy>=image_height)return {};
        return std::array<unsigned,2>{static_cast<unsigned>(ix),static_cast<unsigned>(iy)};
    }
};
// Visible pixels only, with a 32-bit DIB-compatible stride and black letterbox.
inline std::vector<std::uint8_t> ReviewViewportBgra(const render::BgrImage& image,const ReviewViewport& view){
    if(image.width!=view.image_width || image.height!=view.image_height || image.bgr.size()!=render::PixelCount(image.width,image.height)*3)
        throw std::invalid_argument("review image/viewport mismatch");
    const auto count=render::PixelCount(view.view_width,view.view_height);
    std::vector<std::uint8_t> pixels(static_cast<size_t>(count)*4,0);
    const auto scale=view.Scale();const auto origin=view.Origin();
    std::vector<int> columns(view.view_width);
    for(unsigned x=0;x<view.view_width;++x)columns[x]=static_cast<int>(std::floor((x+0.5-origin.x)/scale));
    for(unsigned y=0;y<view.view_height;++y){
        const auto iy=static_cast<int>(std::floor((y+0.5-origin.y)/scale));if(iy<0 || iy>=static_cast<int>(image.height))continue;
        for(unsigned x=0;x<view.view_width;++x){const auto ix=columns[x];if(ix<0 || ix>=static_cast<int>(image.width))continue;
            const auto source=(static_cast<size_t>(iy)*image.width+ix)*3,target=(static_cast<size_t>(y)*view.view_width+x)*4;
            std::copy_n(image.bgr.data()+source,3,pixels.data()+target);
        }
    }
    return pixels;
}
}
