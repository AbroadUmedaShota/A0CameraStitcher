#pragma once
#include "a0/m2/experimental/seam_crops.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace a0::phase0::experimental {
// Writes the seam crops of a saved review (#418 stage 0) into one PNG, for a record that is
// made without opening a window. File-only: the saved review is read, nothing of it is
// changed, no camera is touched. An existing file is never overwritten.

// Size of each crop in the PNG, in composite pixels (not scaled). The crops are narrower
// where the seam is near an image edge, and shorter where the valid rows are fewer.
inline constexpr std::uint32_t kSeamCropExportWidth=481,kSeamCropExportHeight=640,kSeamCropExportGap=8;
inline constexpr std::array<std::uint8_t,3> kSeamCropExportGapBgr{32,32,32};

inline const wchar_t* SeamCropsReportName(){return L"uncalibrated-review.json";}

// The report of a result folder: the folder itself, or its `review` folder (the layout of a
// pair capture's run folder). An explicit folder only; no search for a newest one.
inline std::filesystem::path ResolveSeamCropsReport(const std::filesystem::path& folder){
    namespace fs=std::filesystem;std::error_code error;
    for(const auto& candidate:{folder/SeamCropsReportName(),folder/L"review"/SeamCropsReportName()})
        if(fs::is_regular_file(candidate,error))return candidate;
    throw std::invalid_argument("no saved review report in the folder");
}

namespace seam_crops_detail {
using Microsoft::WRL::ComPtr;
inline void Hr(HRESULT hr){if(FAILED(hr))throw std::runtime_error("image encoder failure");}
struct Apartment {
    bool initialised;
    Apartment():initialised(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED))){}
    ~Apartment(){if(initialised)CoUninitialize();}
};
inline void EncodePng(const std::filesystem::path& file,const a0::m2::render::BgrImage& image){
    Apartment apartment;
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream));Hr(stream->InitializeFromFilename(file.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;Hr(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> properties;Hr(encoder->CreateNewFrame(&frame,&properties));Hr(frame->Initialize(properties.Get()));
    Hr(frame->SetSize(image.width,image.height));auto format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format));
    if(!IsEqualGUID(format,GUID_WICPixelFormat24bppBGR))throw std::runtime_error("image encoder failure");
    Hr(frame->WritePixels(image.height,image.width*3,static_cast<UINT>(image.bgr.size()),const_cast<BYTE*>(image.bgr.data())));
    Hr(frame->Commit());Hr(encoder->Commit());
}
}

// Writes `image` to `file` (a .png path) through `file`.partial and a rename that fails if the
// destination exists. Throws when the file or the partial file already exists, the extension
// is not .png, or the parent folder is missing. Removes the partial file on failure.
inline void WriteBgrPngNoOverwrite(const std::filesystem::path& file,const a0::m2::render::BgrImage& image){
    namespace fs=std::filesystem;
    auto extension=file.extension().wstring();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
    if(extension!=L".png")throw std::invalid_argument("the output file must be a .png");
    if(!image.width || !image.height || image.bgr.size()!=a0::m2::render::PixelCount(image.width,image.height)*3)throw std::invalid_argument("invalid image");
    const auto parent=file.has_parent_path()?file.parent_path():fs::path(L".");
    std::error_code error;
    if(!fs::is_directory(parent,error))throw std::invalid_argument("the output folder does not exist");
    auto partial=file;partial+=L".partial";
    if(fs::exists(file,error) || fs::exists(partial,error))throw std::invalid_argument("the output file already exists; it is not overwritten");
    try{
        seam_crops_detail::EncodePng(partial,image);
        if(!MoveFileExW(partial.c_str(),file.c_str(),0))throw std::runtime_error("the output file could not be completed");
    }catch(...){fs::remove(partial,error);throw;}
}

struct SeamCropsExported {
    a0::m2::render::BgrImage sheet;
    std::array<std::uint32_t,3> top{},seam_column{},width{};
    std::uint32_t height{};
};
// Reads the saved review of `folder`, cuts the three crops and writes them side by side to
// `output` (top, middle, bottom from the left). Throws when the report cannot be read, has no
// seam column, or the output cannot be written without overwriting.
inline SeamCropsExported ExportSeamCrops(const std::filesystem::path& folder,const std::filesystem::path& output){
    namespace m2=a0::m2::experimental;
    const auto record=m2::LoadSavedStillReview(ResolveSeamCropsReport(folder));
    const auto image=m2::LoadStillReviewImage(record);
    const auto crops=m2::ExtractSeamCrops(record,image,kSeamCropExportWidth,kSeamCropExportHeight);
    if(!crops)throw std::invalid_argument("the saved review has no seam column or no valid rows");
    auto sheet=m2::ComposeSeamCropSheet(*crops,kSeamCropExportGap,kSeamCropExportGapBgr);
    WriteBgrPngNoOverwrite(output,sheet.image);
    SeamCropsExported result;result.height=crops->crop[0].image.height;
    for(size_t i=0;i<3;++i){result.top[i]=crops->crop[i].top;result.seam_column[i]=crops->crop[i].seam_column;result.width[i]=crops->crop[i].image.width;}
    result.sheet=std::move(sheet.image);
    return result;
}
}
