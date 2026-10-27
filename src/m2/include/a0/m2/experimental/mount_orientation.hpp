#pragma once
#include "a0/m2/render.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace a0::m2::experimental {
// Development rig mounting of BOTH bodies. rotated-180 turns only in-memory
// render inputs before A is placed left; stored originals are never rotated.
// No 90-degree, mirrored or per-camera orientation exists.
enum class MountOrientation {normal,rotated_180};
inline const char* MountOrientationName(MountOrientation o){
    if(o==MountOrientation::normal)return "normal";
    if(o==MountOrientation::rotated_180)return "rotated-180";
    throw std::invalid_argument("unknown mount orientation");
}
inline std::optional<MountOrientation> ParseMountOrientation(std::string_view text){
    if(text=="normal")return MountOrientation::normal;
    if(text=="rotated-180")return MountOrientation::rotated_180;
    return {};
}
// output(x,y)=input(width-1-x,height-1-y): reversing whole-pixel order turns
// rows and columns together, for odd and even dimensions alike.
inline void Rotate180(render::BgrImage& image){
    if(image.bgr.size()!=static_cast<size_t>(image.width)*image.height*3)throw std::invalid_argument("invalid image for rotation");
    const auto pixels=image.bgr.size()/3;
    for(size_t i=0,j=pixels?pixels-1:0;i<j;++i,--j)std::swap_ranges(image.bgr.begin()+i*3,image.bgr.begin()+i*3+3,image.bgr.begin()+j*3);
}
inline void OrientRenderInput(render::BgrImage& image,MountOrientation o){
    (void)MountOrientationName(o);
    if(o==MountOrientation::rotated_180)Rotate180(image);
}
}
