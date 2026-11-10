#pragma once
#include "a0/m2/experimental/still_review.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace a0::m2::experimental {
// Native-size crops of a saved composite around its seam: one at the top, one in the
// middle and one at the bottom of the rows where both cameras are valid (#418 stage 0).
// Pure: no camera, no file, no clock, no window. Every crop is a plain copy of composite
// pixels, so the pixel at (x,y) of a crop is the composite pixel at (left+x, top+y).

// A half-open run of composite rows [first, end).
struct SeamRowRange {std::uint32_t first{},end{};};

// The rows where the seam column is covered by both cameras.
// ※仮定: until the gate's transition band (#404) exists, the range is every row of the
// composite. The fixed-seam writers (RenderPairFixedSeam, RenderMappedFixedSeam) throw
// unless the seam column, and the half blend width around it, is covered by both cameras
// on every output row, and the searched seam (RenderPairSeamCut) throws for a row with no
// overlap. A composite that was written therefore has both cameras on every row at the
// seam. Replace the body when #404 and #413 give the range; the callers do not change.
inline SeamRowRange SeamValidRows(const SavedStillReview& record){
    return {0,record.height};
}

struct SeamCropRowPlan {std::uint32_t height{},top{},middle{},bottom{};}; // first rows of the three crops
// The one place that decides which rows are cropped. The top crop touches the first valid
// row, the bottom crop touches the last one, the middle crop is centred in the range (the
// extra row of an odd remainder goes below). A range shorter than requested_height gives
// every crop the whole range. An empty range, or requested_height 0, gives no plan.
inline std::optional<SeamCropRowPlan> DecideSeamCropRows(SeamRowRange valid,std::uint32_t requested_height){
    if(valid.end<=valid.first || requested_height==0)return std::nullopt;
    const auto length=valid.end-valid.first;const auto height=std::min(requested_height,length);
    return SeamCropRowPlan{height,valid.first,valid.first+(length-height)/2,valid.end-height};
}

struct SeamCropColumns {std::uint32_t left{},width{},center_index{};}; // center_index: the seam column inside the crop
// The crop is symmetric about the seam column: width 2*half+1, so the seam column is exactly
// in the middle. half is requested_width/2, reduced where the image ends. A column outside the
// image, or requested_width 0, gives nothing.
inline std::optional<SeamCropColumns> DecideSeamCropColumns(std::uint32_t seam_column,std::uint32_t image_width,std::uint32_t requested_width){
    if(seam_column>=image_width || requested_width==0)return std::nullopt;
    const auto half=std::min({requested_width/2,seam_column,image_width-1-seam_column});
    return SeamCropColumns{seam_column-half,2*half+1,half};
}

// The seam column the report records for an output row. A fixed composite (v6, v7) has one
// column for every row. A searched seam-cut has one column per row. A linear feather has
// none.
inline std::optional<std::uint32_t> SeamColumnAtRow(const SavedStillReview& record,std::uint32_t row){
    if(record.fixed_composite)return record.fixed_composite->seam_column;
    if(record.fixed_composite_lens)return record.fixed_composite_lens->seam_column;
    if(record.blend && row<record.blend->seam_x.size())return record.blend->seam_x[row];
    return std::nullopt;
}

struct SeamCrop {
    std::uint32_t left{},top{};      // composite pixel of the crop's (0,0)
    std::uint32_t seam_column{};     // composite column of the crop's centre
    std::uint32_t center_index{};    // the same column inside the crop
    render::BgrImage image;
};
enum class SeamCropSlot {top,middle,bottom};
struct SeamCrops {std::array<SeamCrop,3> crop;};

// The row whose seam column centres a crop that starts at `top` and is `height` tall.
inline std::uint32_t SeamCropCentreRow(std::uint32_t top,std::uint32_t height){return top+height/2;}

// Three crops of at most cell_width x cell_height. Nothing when the composite has no usable
// seam column or row range. Throws std::invalid_argument when the image is not the record's.
inline std::optional<SeamCrops> ExtractSeamCrops(const SavedStillReview& record,const render::BgrImage& composite,std::uint32_t cell_width,std::uint32_t cell_height){
    if(composite.width!=record.width || composite.height!=record.height || composite.bgr.size()!=render::PixelCount(composite.width,composite.height)*3)
        throw std::invalid_argument("seam crops: image is not the record's composite");
    auto valid=SeamValidRows(record);valid.end=std::min(valid.end,composite.height);
    const auto plan=DecideSeamCropRows(valid,cell_height);if(!plan)return std::nullopt;
    SeamCrops out;const std::array<std::uint32_t,3> tops{plan->top,plan->middle,plan->bottom};
    for(size_t i=0;i<3;++i){
        const auto seam=SeamColumnAtRow(record,SeamCropCentreRow(tops[i],plan->height));if(!seam)return std::nullopt;
        const auto columns=DecideSeamCropColumns(*seam,composite.width,cell_width);if(!columns)return std::nullopt;
        auto& crop=out.crop[i];crop.left=columns->left;crop.top=tops[i];crop.seam_column=*seam;crop.center_index=columns->center_index;
        crop.image.width=columns->width;crop.image.height=plan->height;crop.image.bgr.resize(static_cast<size_t>(columns->width)*plan->height*3);
        for(std::uint32_t y=0;y<plan->height;++y)
            std::copy_n(composite.bgr.data()+(static_cast<size_t>(tops[i]+y)*composite.width+columns->left)*3,static_cast<size_t>(columns->width)*3,
                crop.image.bgr.data()+static_cast<size_t>(y)*columns->width*3);
    }
    return out;
}

// The three crops side by side in one image, separated by gap_pixels of gap_bgr. Crops of
// different widths (near an image edge) are top aligned; the rest of the sheet is the gap
// colour. x_offsets[i] is the sheet column of crop i's left edge.
struct SeamCropSheet {render::BgrImage image;std::array<std::uint32_t,3> x_offsets{};};
inline SeamCropSheet ComposeSeamCropSheet(const SeamCrops& crops,std::uint32_t gap_pixels,std::array<std::uint8_t,3> gap_bgr){
    std::uint32_t width=gap_pixels*2,height=0;
    for(const auto& c:crops.crop){width+=c.image.width;height=std::max(height,c.image.height);}
    SeamCropSheet sheet;sheet.image.width=width;sheet.image.height=height;sheet.image.bgr.resize(render::PixelCount(width,height)*3);
    for(size_t i=0;i<sheet.image.bgr.size();i+=3)std::copy(gap_bgr.begin(),gap_bgr.end(),sheet.image.bgr.begin()+static_cast<std::ptrdiff_t>(i));
    std::uint32_t x=0;
    for(size_t i=0;i<3;++i){
        const auto& c=crops.crop[i].image;sheet.x_offsets[i]=x;
        for(std::uint32_t y=0;y<c.height;++y)
            std::copy_n(c.bgr.data()+static_cast<size_t>(y)*c.width*3,static_cast<size_t>(c.width)*3,sheet.image.bgr.data()+(static_cast<size_t>(y)*width+x)*3);
        x+=c.width+gap_pixels;
    }
    return sheet;
}
}
