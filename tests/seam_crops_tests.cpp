// Seam crops of a saved composite (#418 stage 0). Synthetic data only; no camera.
// Pure decisions (rows, columns), the pixels of the crops against the composite, the PNG
// record, a real seam-cut review written and read back, and the inspector itself
// (--export-seam-crops, and the window's own check with --seam-crops-smoke).
// With the rig profile v2 fixture (argument 2) it also writes registered reviews (upright and mounted a half turn) with the real
// writer and opens them: the window, the export, and a JPEG changed by one byte (#418). What the window holds and draws (the picture, every
// pixel of the three crops read back from the drawing surface, the sentences of its panel) is compared with the saved JPEG decoded here by WIC
// and with the sentences made here from the record, never with the window's own copies. The second argument is required. The writer's report is v10; the cases are a v10
// report without a position, a v10 report with a position set by hand (level confirmed), and the first of them in the v8 form (what a
// capture made before v9 saved).
#include "a0/m2/experimental/seam_crops.hpp"
#include "a0/phase0/experimental/seam_crops_export.hpp"
#include "a0/phase0/experimental/live_registration_display.hpp"
#include "a0/phase0/experimental/pair_review_registration.hpp"
#include "a0/phase0/phase0.hpp"
#include "registered_review_fixture.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <source_location>
#include <string>

using namespace a0::m2::experimental;
namespace fs=std::filesystem;
namespace px=a0::phase0::experimental;
using Microsoft::WRL::ComPtr;
using a0::m2::render::BgrImage;

namespace {
int checks=0,failures=0;
void Check(bool value,std::source_location where=std::source_location::current()){
    ++checks;if(!value){++failures;std::cerr<<"check failed at line "<<where.line()<<'\n';}
}
template<class F> bool Throws(F call){try{call();}catch(const std::exception&){return true;}return false;}
void Hr(HRESULT hr){if(FAILED(hr))throw std::runtime_error("fixture WIC failure");}

// A composite whose every pixel differs from its neighbours, so a shifted copy cannot match.
BgrImage Pattern(std::uint32_t width,std::uint32_t height){
    BgrImage image{width,height,std::vector<std::uint8_t>(static_cast<size_t>(width)*height*3)};
    for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x){
        auto* p=image.bgr.data()+(static_cast<size_t>(y)*width+x)*3;
        p[0]=static_cast<std::uint8_t>((x*3+y)%251);p[1]=static_cast<std::uint8_t>((x+y*5)%241);p[2]=static_cast<std::uint8_t>((x*7+y*11)%239);
    }
    return image;
}
bool SameWindow(const BgrImage& crop,const BgrImage& composite,std::uint32_t left,std::uint32_t top){
    if(left+crop.width>composite.width || top+crop.height>composite.height)return false;
    for(std::uint32_t y=0;y<crop.height;++y)for(std::uint32_t x=0;x<crop.width;++x)
        for(int c=0;c<3;++c)
            if(crop.bgr[(static_cast<size_t>(y)*crop.width+x)*3+c]!=composite.bgr[(static_cast<size_t>(top+y)*composite.width+left+x)*3+c])return false;
    return true;
}
SavedStillReview FixedRecord(std::uint32_t width,std::uint32_t height,std::uint32_t seam){
    SavedStillReview r;r.width=width;r.height=height;r.fixed_composite.emplace();r.fixed_composite->seam_column=seam;return r;
}
SavedStillReview LensRecord(std::uint32_t width,std::uint32_t height,std::uint32_t seam){
    SavedStillReview r;r.width=width;r.height=height;r.fixed_composite_lens.emplace();r.fixed_composite_lens->seam_column=seam;return r;
}
SavedStillReview SearchedRecord(std::uint32_t width,std::uint32_t height,std::vector<std::uint32_t> seam_x){
    SavedStillReview r;r.width=width;r.height=height;r.blend.emplace();r.blend->seam_x=std::move(seam_x);return r;
}

void Rows(){
    // Top touches the first valid row, bottom the last, middle is centred.
    auto plan=DecideSeamCropRows({10,110},30);
    Check(plan && plan->height==30 && plan->top==10 && plan->middle==45 && plan->bottom==80);
    // Odd remainder: the spare row goes below the middle crop.
    plan=DecideSeamCropRows({0,101},30);
    Check(plan && plan->top==0 && plan->middle==35 && plan->bottom==71 && plan->bottom+plan->height==101);
    // The range is exactly the requested height: all three are the same rows.
    plan=DecideSeamCropRows({4,34},30);
    Check(plan && plan->top==4 && plan->middle==4 && plan->bottom==4 && plan->height==30);
    // A narrow range shrinks the crops to the range; none leaves it.
    plan=DecideSeamCropRows({10,25},40);
    Check(plan && plan->height==15 && plan->top==10 && plan->middle==10 && plan->bottom==10);
    plan=DecideSeamCropRows({7,8},40);
    Check(plan && plan->height==1 && plan->top==7 && plan->middle==7 && plan->bottom==7);
    // An empty or reversed range, or no height, gives no plan.
    Check(!DecideSeamCropRows({5,5},30) && !DecideSeamCropRows({9,3},30) && !DecideSeamCropRows({0,0},30) && !DecideSeamCropRows({0,100},0));
    // The rows stay inside the range and are ordered, for many sizes.
    bool inside=true;
    for(std::uint32_t first=0;first<12;++first)for(std::uint32_t length=1;length<40;++length)for(std::uint32_t request=1;request<45;request+=3){
        const auto p=DecideSeamCropRows({first,first+length},request);
        if(!p || p->top!=first || p->bottom+p->height!=first+length || p->middle<p->top || p->middle>p->bottom || p->height!=std::min(request,length))inside=false;
        // centred within one row
        if(p){const auto above=p->middle-first,below=first+length-(p->middle+p->height);if(above>below || below-above>1)inside=false;}
    }
    Check(inside);
    // The record's range is the whole composite until the gate's transition band exists (※仮定).
    const auto range=SeamValidRows(FixedRecord(200,120,100));
    Check(range.first==0 && range.end==120);
}

void Columns(){
    auto c=DecideSeamCropColumns(100,200,41);
    Check(c && c->left==80 && c->width==41 && c->center_index==20 && c->left+c->center_index==100);
    // An even request gives the next smaller odd width, so the seam column stays in the middle.
    c=DecideSeamCropColumns(100,200,40);
    Check(c && c->width==41 && c->center_index==20);
    // Near an edge the crop narrows on both sides, the seam column stays in the middle.
    c=DecideSeamCropColumns(3,200,41);
    Check(c && c->left==0 && c->width==7 && c->center_index==3);
    c=DecideSeamCropColumns(198,200,41);
    Check(c && c->left==197 && c->width==3 && c->center_index==1);
    c=DecideSeamCropColumns(0,200,41);
    Check(c && c->left==0 && c->width==1 && c->center_index==0);
    c=DecideSeamCropColumns(199,200,41);
    Check(c && c->left==199 && c->width==1);
    Check(!DecideSeamCropColumns(200,200,41) && !DecideSeamCropColumns(500,200,41) && !DecideSeamCropColumns(10,200,0) && !DecideSeamCropColumns(0,0,41));
    // A request of 1 is the seam column alone.
    c=DecideSeamCropColumns(50,200,1);Check(c && c->left==50 && c->width==1 && c->center_index==0);
}

void Pixels(){
    const auto composite=Pattern(200,120);
    // A fixed composite (v6): one column for every row.
    {
        const auto record=FixedRecord(200,120,100);
        const auto crops=ExtractSeamCrops(record,composite,41,40);
        Check(crops.has_value());
        if(crops){
            const std::array<std::uint32_t,3> tops{0,40,80};
            for(size_t i=0;i<3;++i){
                const auto& c=crops->crop[i];
                Check(c.image.width==41 && c.image.height==40 && c.top==tops[i] && c.left==80 && c.seam_column==100 && c.center_index==20);
                Check(c.left+c.center_index==100 && SameWindow(c.image,composite,c.left,c.top));
                // The centre column of the crop is the composite's seam column.
                const auto* centre=c.image.bgr.data()+(static_cast<size_t>(5)*c.image.width+c.center_index)*3;
                const auto* source=composite.bgr.data()+(static_cast<size_t>(c.top+5)*composite.width+100)*3;
                Check(std::equal(centre,centre+3,source));
            }
        }
    }
    // A taller request than the middle fits: the top touches row 0, the bottom touches the last row.
    {
        const auto crops=ExtractSeamCrops(FixedRecord(200,120,100),composite,41,50);
        Check(crops && crops->crop[0].top==0 && crops->crop[1].top==35 && crops->crop[2].top==70 && crops->crop[2].top+crops->crop[2].image.height==120);
        Check(crops && SameWindow(crops->crop[1].image,composite,80,35) && SameWindow(crops->crop[2].image,composite,80,70));
    }
    // A cell taller than the composite: every crop is the whole height.
    {
        const auto crops=ExtractSeamCrops(FixedRecord(200,120,100),composite,41,500);
        Check(crops && crops->crop[0].image.height==120 && crops->crop[0].top==0 && crops->crop[1].top==0 && crops->crop[2].top==0 && SameWindow(crops->crop[2].image,composite,80,0));
    }
    // A lens-aware composite (v7) names its column the same way.
    {
        const auto crops=ExtractSeamCrops(LensRecord(200,120,77),composite,21,30);
        Check(crops && crops->crop[0].seam_column==77 && crops->crop[1].seam_column==77 && crops->crop[2].seam_column==77 && crops->crop[1].left==67 && SameWindow(crops->crop[1].image,composite,67,crops->crop[1].top));
    }
    // A searched seam moves per row: each crop is centred on the seam at its own centre row.
    {
        std::vector<std::uint32_t> seam(120);
        for(std::uint32_t y=0;y<120;++y)seam[y]=y<40?3:y<80?100:198;
        const auto record=SearchedRecord(200,120,seam);
        const auto crops=ExtractSeamCrops(record,composite,41,40);
        Check(crops.has_value());
        if(crops){
            Check(crops->crop[0].seam_column==3 && crops->crop[0].image.width==7 && crops->crop[0].left==0 && crops->crop[0].center_index==3);
            Check(crops->crop[1].seam_column==100 && crops->crop[1].image.width==41 && crops->crop[1].left==80);
            Check(crops->crop[2].seam_column==198 && crops->crop[2].image.width==3 && crops->crop[2].left==197);
            for(const auto& c:crops->crop)Check(SameWindow(c.image,composite,c.left,c.top) && c.left+c.center_index==c.seam_column);
            // Sheet: crops side by side, gap colour elsewhere, each crop's pixels where x_offsets says.
            const std::array<std::uint8_t,3> gap{9,8,7};
            const auto sheet=ComposeSeamCropSheet(*crops,8,gap);
            Check(sheet.image.width==7+41+3+16 && sheet.image.height==40 && sheet.x_offsets[0]==0 && sheet.x_offsets[1]==15 && sheet.x_offsets[2]==64);
            for(size_t i=0;i<3;++i){
                const auto& c=crops->crop[i];BgrImage window{c.image.width,c.image.height,std::vector<std::uint8_t>(c.image.bgr.size())};
                for(std::uint32_t y=0;y<c.image.height;++y)
                    std::copy_n(sheet.image.bgr.data()+(static_cast<size_t>(y)*sheet.image.width+sheet.x_offsets[i])*3,static_cast<size_t>(c.image.width)*3,window.bgr.data()+static_cast<size_t>(y)*c.image.width*3);
                Check(window.bgr==c.image.bgr);
            }
            // The gap columns between crops are the gap colour.
            for(std::uint32_t y=0;y<40;++y)for(const auto x:{7u,8u,14u,56u,63u})
                Check(std::equal(gap.begin(),gap.end(),sheet.image.bgr.begin()+(static_cast<size_t>(y)*sheet.image.width+x)*3));
        }
    }
    // No seam column recorded (a linear feather): nothing to cut.
    Check(!ExtractSeamCrops(SearchedRecord(200,120,{}),composite,41,40));
    { SavedStillReview none;none.width=200;none.height=120;Check(!ExtractSeamCrops(none,composite,41,40)); }
    // A seam column outside the image, an empty cell, and an image that is not the record's.
    Check(!ExtractSeamCrops(FixedRecord(200,120,200),composite,41,40) && !ExtractSeamCrops(FixedRecord(200,120,100),composite,0,40) && !ExtractSeamCrops(FixedRecord(200,120,100),composite,41,0));
    Check(Throws([&]{(void)ExtractSeamCrops(FixedRecord(201,120,100),composite,41,40);}));
    // Breaking the offset must fail: a crop one column to the right is not the composite window.
    {
        const auto crops=ExtractSeamCrops(FixedRecord(200,120,100),composite,41,40);
        Check(crops && !SameWindow(crops->crop[0].image,composite,crops->crop[0].left+1,crops->crop[0].top) && !SameWindow(crops->crop[0].image,composite,crops->crop[0].left,crops->crop[0].top+1));
    }
}

BgrImage ReadPng(const fs::path& file){
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmapDecoder> decoder;Hr(factory->CreateDecoderFromFilename(file.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder));
    GUID container{};Hr(decoder->GetContainerFormat(&container));if(!IsEqualGUID(container,GUID_ContainerFormatPng))throw std::runtime_error("not a PNG");
    ComPtr<IWICBitmapFrameDecode> frame;Hr(decoder->GetFrame(0,&frame));
    ComPtr<IWICFormatConverter> converter;Hr(factory->CreateFormatConverter(&converter));
    Hr(converter->Initialize(frame.Get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    UINT width=0,height=0;Hr(converter->GetSize(&width,&height));
    BgrImage image{width,height,std::vector<std::uint8_t>(static_cast<size_t>(width)*height*3)};
    Hr(converter->CopyPixels(nullptr,width*3,static_cast<UINT>(image.bgr.size()),image.bgr.data()));
    return image;
}
// The saved JPEG decoded here, by WIC directly, so that what the window and the export show is compared with a decode that the
// inspector and its reader had no part in.
BgrImage ReadJpeg(const fs::path& file){
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmapDecoder> decoder;Hr(factory->CreateDecoderFromFilename(file.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder));
    GUID container{};Hr(decoder->GetContainerFormat(&container));if(!IsEqualGUID(container,GUID_ContainerFormatJpeg))throw std::runtime_error("not a JPEG");
    ComPtr<IWICBitmapFrameDecode> frame;Hr(decoder->GetFrame(0,&frame));
    ComPtr<IWICFormatConverter> converter;Hr(factory->CreateFormatConverter(&converter));
    Hr(converter->Initialize(frame.Get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    UINT width=0,height=0;Hr(converter->GetSize(&width,&height));
    BgrImage image{width,height,std::vector<std::uint8_t>(static_cast<size_t>(width)*height*3)};
    Hr(converter->CopyPixels(nullptr,width*3,static_cast<UINT>(image.bgr.size()),image.bgr.data()));
    return image;
}
std::vector<char> ReadBytes(const fs::path& file){std::ifstream f(file,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void WriteJpeg(const fs::path& path,const BgrImage& image){
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream));Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder));Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> props;Hr(encoder->CreateNewFrame(&frame,&props));Hr(frame->Initialize(props.Get()));Hr(frame->SetSize(image.width,image.height));
    auto format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format));
    Hr(frame->WritePixels(image.height,image.width*3,static_cast<UINT>(image.bgr.size()),const_cast<BYTE*>(image.bgr.data())));Hr(frame->Commit());Hr(encoder->Commit());
}

void Png(const fs::path& root){
    const auto composite=Pattern(120,90);
    const auto crops=ExtractSeamCrops(FixedRecord(120,90,60),composite,31,30);
    Check(crops.has_value());if(!crops)return;
    const auto sheet=ComposeSeamCropSheet(*crops,px::kSeamCropExportGap,px::kSeamCropExportGapBgr);
    const auto file=root/"sheet.png";
    px::WriteBgrPngNoOverwrite(file,sheet.image);
    const auto read=ReadPng(file);
    Check(read.width==sheet.image.width && read.height==sheet.image.height && read.bgr==sheet.image.bgr); // lossless: the record is the pixels
    Check(!fs::exists(root/"sheet.png.partial"));
    // An existing file is not overwritten, and its bytes stay.
    const auto before=ReadBytes(file);
    Check(Throws([&]{px::WriteBgrPngNoOverwrite(file,Pattern(5,5));}) && ReadBytes(file)==before);
    // A leftover partial file blocks as well and is not removed.
    { std::ofstream(root/"other.png.partial")<<"x"; }
    Check(Throws([&]{px::WriteBgrPngNoOverwrite(root/"other.png",Pattern(5,5));}) && !fs::exists(root/"other.png") && fs::exists(root/"other.png.partial"));
    // The message of a leftover partial file names it (and not the output), and its bytes stay.
    {std::string message;try{px::WriteBgrPngNoOverwrite(root/"other.png",Pattern(5,5));}catch(const std::exception& error){message=error.what();}
        Check(message.find(".partial")!=std::string::npos && ReadBytes(root/"other.png.partial")==std::vector<char>{'x'});}
    // The writer's own exclusive create (the one the rename follows): taken path -> the leftover error, bytes untouched.
    Check(Throws([&]{px::seam_crops_detail::WriteNewFileDurably(root/"other.png.partial",{1,2,3});}) && ReadBytes(root/"other.png.partial")==std::vector<char>{'x'});
    {const auto fresh=root/"fresh.bin";px::seam_crops_detail::WriteNewFileDurably(fresh,{1,2,3,4});Check(ReadBytes(fresh)==std::vector<char>{1,2,3,4});}
    // Only a .png, only into a folder that exists, only a real image.
    Check(Throws([&]{px::WriteBgrPngNoOverwrite(root/"sheet.jpg",Pattern(5,5));}) && !fs::exists(root/"sheet.jpg"));
    Check(Throws([&]{px::WriteBgrPngNoOverwrite(root/"missing"/"sheet.png",Pattern(5,5));}));
    Check(Throws([&]{px::WriteBgrPngNoOverwrite(root/"empty.png",BgrImage{});}) && !fs::exists(root/"empty.png"));
}

DWORD RunInspector(const fs::path& executable,const std::wstring& arguments,DWORD timeout_ms){
    std::wstring command=L"\""+executable.wstring()+L"\" "+arguments;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process))throw std::runtime_error("cannot start the inspector");
    const auto waited=WaitForSingleObject(process.hProcess,timeout_ms);DWORD code=1;
    if(waited==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&code);else{TerminateProcess(process.hProcess,2);code=2;}
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return code;
}

struct Captured {DWORD code{};std::string out,err;};
// The inspector run with its standard output and error kept (files in `scratch`), so that a run can be judged by what it printed.
Captured RunCaptured(const fs::path& executable,const std::wstring& arguments,DWORD timeout_ms,const fs::path& scratch){
    static unsigned serial=0;++serial;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    const auto open=[&](const wchar_t* name){
        const auto path=scratch/(std::wstring(name)+std::to_wstring(serial)+L".txt");
        const HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&inherit,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot make the output file");
        return std::pair<HANDLE,fs::path>{h,path};
    };
    const auto out=open(L"inspector-out-"),err=open(L"inspector-err-");
    std::wstring command=L"\""+executable.wstring()+L"\" "+arguments;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=out.first;startup.hStdError=err.first;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&startup,&process)){CloseHandle(out.first);CloseHandle(err.first);throw std::runtime_error("cannot start the inspector");}
    const auto waited=WaitForSingleObject(process.hProcess,timeout_ms);Captured result;result.code=1;
    if(waited==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&result.code);else{TerminateProcess(process.hProcess,2);result.code=2;}
    CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseHandle(out.first);CloseHandle(err.first);
    const auto text=[](const fs::path& file){const auto bytes=ReadBytes(file);return std::string(bytes.begin(),bytes.end());};
    result.out=text(out.second);result.err=text(err.second);return result;
}
bool Has(const std::string& text,const std::string& part){return text.find(part)!=std::string::npos;}
// The integer after `name` (given with its "="), for the first place where `name` starts a word of the output.
std::optional<long long> Field(const std::string& text,const std::string& name){
    for(size_t at=text.find(name);at!=std::string::npos;at=text.find(name,at+1)){
        if(at>0 && text[at-1]!=' ' && text[at-1]!='\n')continue; // the whole word, not the end of a longer one
        try{return std::stoll(text.substr(at+name.size()));}catch(const std::exception&){return std::nullopt;}
    }
    return std::nullopt;
}
BgrImage Cut(const BgrImage& image,std::uint32_t left,std::uint32_t width){
    BgrImage out{width,image.height,std::vector<std::uint8_t>(static_cast<size_t>(width)*image.height*3)};
    for(std::uint32_t y=0;y<image.height;++y)std::copy_n(image.bgr.data()+(static_cast<size_t>(y)*image.width+left)*3,static_cast<size_t>(width)*3,out.bgr.data()+static_cast<size_t>(y)*width*3);
    return out;
}
std::set<std::string> Entries(const fs::path& folder){std::set<std::string> names;for(const auto& e:fs::directory_iterator(folder))names.insert(e.path().filename().string());return names;}
// The word after `name` (given with its "="), for the first place where `name` starts a word of the output; up to the next space or break.
std::string Token(const std::string& text,const std::string& name){
    for(size_t at=text.find(name);at!=std::string::npos;at=text.find(name,at+1)){
        if(at>0 && text[at-1]!=' ' && text[at-1]!='\n')continue;
        const auto from=at+name.size(),end=text.find_first_of(" \r\n",from);
        return text.substr(from,end==std::string::npos?std::string::npos:end-from);
    }
    return {};
}
// What follows `prefix` on every output line that starts with it (the line break and a CR left out).
std::vector<std::string> LinesAfter(const std::string& text,const std::string& prefix){
    std::vector<std::string> lines;
    for(size_t at=0;at<text.size();){
        auto end=text.find('\n',at);if(end==std::string::npos)end=text.size();
        auto line=text.substr(at,end-at);if(!line.empty() && line.back()=='\r')line.pop_back();
        if(line.rfind(prefix,0)==0)lines.push_back(line.substr(prefix.size()));
        at=end+1;
    }
    return lines;
}
// A window of an image: the pixels at [left,left+width) x [top,top+height). Empty when it does not lie inside.
BgrImage WindowOf(const BgrImage& image,long long left,long long top,long long width,long long height){
    if(left<0 || top<0 || width<=0 || height<=0 || left+width>image.width || top+height>image.height)return {};
    BgrImage out{static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),std::vector<std::uint8_t>(static_cast<size_t>(width)*static_cast<size_t>(height)*3)};
    for(long long y=0;y<height;++y)std::copy_n(image.bgr.data()+(static_cast<size_t>(top+y)*image.width+static_cast<size_t>(left))*3,static_cast<size_t>(width)*3,out.bgr.data()+static_cast<size_t>(y)*static_cast<size_t>(width)*3);
    return out;
}
// The SHA-256 of every file under the folders, by path: what the window must leave as it found it.
using Digest=std::map<std::string,std::string>;
Digest TreeDigest(const std::vector<fs::path>& folders){
    Digest digest;
    for(const auto& folder:folders)for(const auto& e:fs::recursive_directory_iterator(folder))if(e.is_regular_file()){
        const auto bytes=ReadBytes(e.path());
        digest[e.path().string()]=FixedCompositeBytesSha256(std::vector<unsigned char>(bytes.begin(),bytes.end()));
    }
    return digest;
}
std::string What(const std::function<void()>& call){try{call();}catch(const std::exception& error){return error.what();}return {};}
constexpr const char* kRejectedReview="saved uncalibrated review failed validation";

// The picture the window holds (its SHA-256, size) against the saved JPEG decoded here by WIC. The window's own copy is not the reference.
void CheckPicture(const std::string& out,const BgrImage& jpeg){
    const auto sha=Token(out,"image_sha256=");
    Check(sha.size()==64 && sha==FixedCompositeBytesSha256(jpeg.bgr));
    Check(Field(out,"image_width=")==jpeg.width && Field(out,"image_height=")==jpeg.height);
}
// The crops the window drew: where each was cut (inside the JPEG, centred on the seam column, the rows of DecideSeamCropRows) and every pixel
// read back from the drawing surface, against the same window of the JPEG decoded here.
void CheckCrops(const std::string& out,const BgrImage& jpeg,std::uint32_t seam){
    const auto rows=LinesAfter(out,"seam_crop ");
    Check(rows.size()==3);
    if(rows.size()!=3)return;
    std::optional<long long> common;
    for(size_t i=0;i<3;++i){
        const auto& row=rows[i];
        const auto left=Field(row,"left="),top=Field(row,"top="),width=Field(row,"width="),height=Field(row,"height="),column=Field(row,"seam_column=");
        Check(Field(row,"index=")==static_cast<long long>(i) && left && top && width && height && column);
        if(!(left && top && width && height && column))continue;
        Check(*column==seam && *left<=*column && *width==2*(*column-*left)+1 && *left+*width<=jpeg.width);
        Check(*width>=33 && *height>=33 && *height<=jpeg.height);
        if(!common)common=*height;
        Check(*height==*common);
        const long long expected_top=i==0?0:i==2?static_cast<long long>(jpeg.height)-*height:(static_cast<long long>(jpeg.height)-*height)/2;
        Check(*top==expected_top);
        const auto window=WindowOf(jpeg,*left,*top,*width,*height);
        Check(!window.bgr.empty());
        const auto expected=FixedCompositeBytesSha256(window.bgr);
        const auto drawn=Token(row,"drawn_sha256="),cut=Token(row,"crop_sha256=");
        Check(drawn.size()==64 && drawn==expected);   // every pixel the display holds after the draw
        Check(cut.size()==64 && cut==expected);       // and the crop that was handed to the draw
    }
}
// The sentences in the panel, in the panel's order, against the ones made here from the record: the status lines, then (a position set by
// hand and used) the amounts of the hand, a blank line, then the warnings. The position's sentence and the hand's sentence are there only for
// a position that was given, and are not there otherwise.
void CheckPanel(const std::string& out,const SavedStillReview& record,bool by_hand){
    const auto& registration=*record.fixed_composite_registration;
    StillReviewResult shown;shown.fixed_composite_registration=registration;
    const auto expected=px::DescribeRegistrationReview(shown);
    const auto lines=LinesAfter(out,"panel_line=");
    Check(!expected.status.empty() && lines.size()>expected.status.size());
    size_t at=0;bool in_order=true;
    for(const auto& sentence:expected.status){in_order=in_order && at<lines.size() && lines[at]==sentence;++at;}
    Check(in_order);
    const std::string hand_prefix="手で直しました（",position_prefix="手で直した位置で";
    size_t hands=0,positions=0;
    for(const auto& line:lines){if(line.rfind(hand_prefix,0)==0)++hands;if(line.rfind(position_prefix,0)==0)++positions;}
    if(by_hand){
        const auto& position=registration.registration.confirmed_position;
        Check(position.has_value());
        const auto position_text=px::ConfirmedPositionText(registration);
        Check(!position_text.empty() && positions==1 && std::count(lines.begin(),lines.end(),position_text)==1);
        if(position){
            const auto hand=px::LiveHandSumText(position->manual_entered);
            Check(hands==1 && at<lines.size() && lines[at]==hand);
        }
        ++at;
    }else Check(hands==0 && positions==0);
    Check(at<lines.size() && lines[at].empty());
    ++at;
    bool warnings=true;
    for(const auto& warning:expected.warnings){warnings=warnings && at<lines.size() && lines[at]==warning;++at;}
    Check(warnings);
}

// A registered review (report v8 or v9) in the window and in the export. The window and the export show the saved JPEG alone: what
// they show is compared with the JPEG decoded here. The window does not decode the originals, and each manual tool is off and
// refused. A narrow screen (the window's buttons need 1175 pixels) fails the window's layout check and nothing else; the checks of
// "it opened" and of what the window did are read from the output, so they hold on any screen width.
// `schema`: the report's version. `by_hand`: the JPEG was made with a position set by hand (v9, level confirmed). Every other report
// has no position, so the sentences of the position and of the hand amounts are not on the window.
struct Expect {const char* schema;bool by_hand;};
// `guarded`: folders besides the report's own that the window must leave as they were (the settings folder with the rig profile, the fixtures).
void OneRegistered(const fs::path& root,const fs::path& inspector,const fs::path& report,bool turned,const std::string& label,const Expect& expect,const std::vector<fs::path>& guarded){
    std::cout<<"registered review: "<<label<<'\n';
    // Everything the window may be near, as it is before any run: the report, the JPEG, the originals, the settings and the profile.
    std::vector<fs::path> watched=guarded;watched.push_back(report.parent_path().parent_path());
    const auto digest_before=TreeDigest(watched);
    Check(digest_before.size()>=4);
    const auto record=LoadSavedStillReview(report);
    Check(record.fixed_composite_registration && !record.fixed_composite_lens && record.blend && !record.blend->seam_x.empty());
    Check((record.geometry.orientation==MountOrientation::rotated_180)==turned);
    {
        std::ifstream in(report,std::ios::binary);const std::string text{std::istreambuf_iterator<char>(in),{}};
        Check(text.find(std::string("\"a0.uncalibrated-still-review.")+expect.schema+"\"")!=std::string::npos);
    }
    if(!record.fixed_composite_registration)return;
    {
        const auto& capture=record.fixed_composite_registration->registration;
        Check((capture.level.level==GeometryLevel::confirmed)==expect.by_hand);
        if(expect.by_hand){
            Check(capture.confirmed_position && capture.confirmed_check && capture.confirmed_check->used && capture.confirmed_position->source==ConfirmedPositionSource::manual);
            // What a person is told, from the report's values alone; the window shows these two sentences (read from its output below).
            Check(px::ConfirmedPositionText(*record.fixed_composite_registration).rfind("手で直した位置で JPEG を作りました（",0)==0);
            Check(capture.confirmed_position && px::LiveHandSumText(capture.confirmed_position->manual_entered)==
                "手で直しました（Bの絵の動き 左右 右へ 12 px・上下 上へ 4 px・回り 時計回りに 0.05°、撮影の画素。合計）。");
        }else{
            Check(!capture.confirmed_position && !capture.confirmed_check && px::ConfirmedPositionText(*record.fixed_composite_registration).empty());
        }
    }
    const auto seam=record.fixed_composite_registration->seam_column;
    const auto jpeg=ReadJpeg(record.jpeg);
    Check(jpeg.width==record.width && jpeg.height==record.height && record.height>=px::kSeamCropExportHeight && (record.height-px::kSeamCropExportHeight)%2==0);
    // The reader's decode, which the window cuts from, is the decode made here.
    Check(LoadStillReviewImage(record).bgr==jpeg.bgr);

    // The export, from the report's folder: every pixel of the three crops is the JPEG's pixel at the same place; the centre is the seam column.
    const auto review_folder=report.parent_path();
    const auto png=root/(std::string("registered-")+label+".png");
    const auto exported=RunCaptured(inspector,L"--export-seam-crops \""+review_folder.wstring()+L"\" \""+png.wstring()+L"\"",120000,root);
    Check(exported.code==0 && Has(exported.out,"seam_crops_exported=1") && fs::exists(png));
    const std::array<std::uint32_t,3> tops{0,(record.height-px::kSeamCropExportHeight)/2,record.height-px::kSeamCropExportHeight};
    Check(Field(exported.out,"top_rows_from=")==tops[0] && Field(exported.out,"middle_rows_from=")==tops[1] && Field(exported.out,"bottom_rows_from=")==tops[2]);
    Check(Field(exported.out,"top_seam_column=")==seam && Field(exported.out,"middle_seam_column=")==seam && Field(exported.out,"bottom_seam_column=")==seam);
    if(fs::exists(png)){
        const auto sheet=ReadPng(png);
        const std::uint32_t width=px::kSeamCropExportWidth,centre=(width-1)/2,left=seam-centre,step=width+px::kSeamCropExportGap;
        Check(sheet.width==3*width+2*px::kSeamCropExportGap && sheet.height==px::kSeamCropExportHeight);
        if(sheet.width==3*width+2*px::kSeamCropExportGap){
            for(size_t i=0;i<3;++i){
                const auto crop=Cut(sheet,static_cast<std::uint32_t>(i)*step,width);
                Check(SameWindow(crop,jpeg,left,tops[i]));
                // One column to either side, or one row down, is not the crop: the picture is not flat there.
                Check(!SameWindow(crop,jpeg,left+1,tops[i]) && !SameWindow(crop,jpeg,left-1,tops[i]) && !SameWindow(crop,jpeg,left,tops[i]+1));
            }
        }
    }

    // The window, with the seam crops smoke. The report opens, whatever the screen width.
    const auto before=Entries(review_folder.parent_path());
    const auto crops=RunCaptured(inspector,L"--report \""+report.wstring()+L"\" --seam-crops-smoke",240000,root);
    Check(Has(crops.out,"review_loaded=1 ui_window_visible=1"));
    Check(!Has(crops.err,"invalid review similarity") && !Has(crops.err,"still review failed"));
    Check(Has(crops.out,"fixed_mapping_window=1 registration=1 original_decodes=0")); // the originals are not decoded
    const auto tools=Field(crops.out,"manual_tools=");
    Check(tools && *tools>0 && Field(crops.out,"manual_tools_refused=")==tools && Field(crops.out,"manual_tools_enabled=")==0); // each tool is off and refused (the re-Merge too)
    Check(Field(crops.out,"v7_text_shown=")==0 && Field(crops.out,"registration_text_shown=")==1);                                // the report's own sentences, not v7's
    // Every status line of the report is on the panel; the position's sentence and the hand amounts are there exactly when the report has them.
    const auto lines=Field(crops.out,"status_lines=");
    Check(lines && *lines>0 && Field(crops.out,"status_lines_shown=")==lines);
    const long long hand=expect.by_hand?1:0;
    Check(Field(crops.out,"confirmed_position_text_expected=")==hand && Field(crops.out,"confirmed_position_text_shown=")==hand);
    Check(Field(crops.out,"hand_sum_expected=")==hand && Field(crops.out,"hand_sum_shown=")==hand);
    Check(Field(crops.out,"crops_stages_passed=")==1 && Field(crops.out,"crop_pixel_failures=")==0 && Field(crops.out,"crop_stage=")==4 && Field(crops.out,"stage_failed=")==0);
    // What the window itself holds and drew, judged against the JPEG decoded here (not against the window's own copy of it): the picture, the
    // three crops with every pixel read back from the drawing surface, and the sentences of the panel.
    CheckPicture(crops.out,jpeg);
    CheckCrops(crops.out,jpeg,seam);
    CheckPanel(crops.out,record,expect.by_hand);
    Check(Entries(review_folder.parent_path())==before);                                                                             // the refusals wrote nothing
    // The window's whole verdict, as before: the layout check stays. Only a screen too narrow for the buttons may fail it.
    if(Has(crops.out,"button_layout_failure="))std::cout<<"note: the screen is too narrow for the window's buttons; its layout check fails and the run exits 1 (it did the same for every report)\n";
    Check(crops.code==0);
    // The plain --smoke, the one the lens contract uses.
    const auto plain=RunCaptured(inspector,L"--report \""+report.wstring()+L"\" --smoke",240000,root);
    Check(Has(plain.out,"review_loaded=1 ui_window_visible=1") && !Has(plain.err,"invalid review similarity"));
    Check(plain.code==0);
    CheckPicture(plain.out,jpeg);
    CheckPanel(plain.out,record,expect.by_hand);
    Check(Entries(review_folder.parent_path())==before);
    // After the exports and both window runs the report, the JPEG, the originals, the settings and the profile are the same bytes.
    Check(TreeDigest(watched)==digest_before);
}
// The JPEG changed by one byte: the report no longer opens, in the window or in the export, and no PNG is made.
void ChangedJpeg(const fs::path& root,const fs::path& inspector,const StillReviewResult& made,const std::string& label){
    std::cout<<"registered review: one byte of the JPEG changed ("<<label<<")\n";
    const auto folder=root/("damaged-"+label);fs::create_directories(folder);
    fs::copy_file(made.report,folder/"uncalibrated-review.json",fs::copy_options::overwrite_existing);
    fs::copy_file(made.jpeg,folder/"uncalibrated-review.jpg",fs::copy_options::overwrite_existing);
    // The copy as it is opens (so that the refusals below are for the byte).
    const auto control=RunCaptured(inspector,L"--export-seam-crops \""+folder.wstring()+L"\" \""+(root/("damaged-control-"+label+".png")).wstring()+L"\"",120000,root);
    Check(control.code==0 && fs::exists(root/("damaged-control-"+label+".png")));
    auto bytes=ReadBytes(folder/"uncalibrated-review.jpg");
    Check(bytes.size()>1000);
    bytes[bytes.size()/2]=static_cast<char>(bytes[bytes.size()/2]^0x01);
    { std::ofstream out(folder/"uncalibrated-review.jpg",std::ios::binary|std::ios::trunc);out.write(bytes.data(),static_cast<std::streamsize>(bytes.size())); }
    // The refusal is for the changed byte (the saved JPEG no longer matches the report), not for another cause: the reason is the reader's
    // one reason for a report that fails its validation, in the reader and in what the export and the window print.
    Check(What([&]{const auto record=LoadSavedStillReview(folder/"uncalibrated-review.json");(void)LoadStillReviewImage(record);})==kRejectedReview);
    const auto exported=RunCaptured(inspector,L"--export-seam-crops \""+folder.wstring()+L"\" \""+(root/("damaged-"+label+".png")).wstring()+L"\"",120000,root);
    Check(exported.code!=0 && !fs::exists(root/("damaged-"+label+".png")) && !Has(exported.out,"seam_crops_exported=1"));
    Check(Has(exported.err,"seam_crops_exported=0") && Has(exported.err,std::string("reason=")+kRejectedReview));
    for(const auto* mode:{L"--seam-crops-smoke",L"--smoke"}){
        const auto window=RunCaptured(inspector,L"--report \""+(folder/"uncalibrated-review.json").wstring()+L"\" "+mode,240000,root);
        Check(window.code!=0 && !Has(window.out,"review_loaded=1"));
        Check(Has(window.err,std::string("still review failed: ")+kRejectedReview));
    }
}
// What a capture made before v9 saved: the record of the same capture as a writer before v10 left it (the G1 and G2 mappings apply the
// estimated plane, no surface rule), written in the v8 form beside the same JPEG, in a folder of its own. The JPEG was drawn with the plane
// turned about the seam line, which differs a little from the record's G1 plane in this v8 text; the window and the export show the saved
// JPEG only and never draw it again from the record, so what is checked of them here does not depend on that difference.
fs::path AsV8(const fs::path& root,const StillReviewResult& made,const std::string& label){
    const auto folder=root/("v8-"+label)/"review";fs::create_directories(folder);
    std::string text;{std::ifstream in(made.report,std::ios::binary);text.assign(std::istreambuf_iterator<char>(in),{});}
    const auto read=LoadSavedStillReview(made.report);
    Check(read.fixed_composite_registration.has_value());
    if(!read.fixed_composite_registration)return folder/"uncalibrated-review.json";
    const auto& written=*read.fixed_composite_registration;
    auto before=written;
    const auto level=before.registration.level.level;
    if(level==GeometryLevel::g1 || level==GeometryLevel::g2)
        before.registration.level.mapping.surface=before.registration.surface.state==SurfaceState::estimated?before.registration.surface.plane:SurfacePlane{};
    before.registration.seam_line_lambda.reset();
    DeriveSavedFixedCompositeRegistration(before,registered_review_fixture::kHash);
    bool replaced=true;
    const auto replace_once=[&](const std::string& from,const std::string& to){
        const auto at=text.find(from);
        const bool once=at!=std::string::npos && text.find(from,at+1)==std::string::npos;
        Check(once);replaced=replaced && once;
        if(once)text.replace(at,from.size(),to);
    };
    replace_once("\"fixedCompositeRegistration\":"+FixedCompositeRegistrationJson(written,RegistrationReportFormat::v10),
        "\"fixedCompositeRegistration\":"+FixedCompositeRegistrationJson(before));
    replace_once("\"warnings\":"+RegistrationWarningsJson(written.warnings)+",\"assessment\":"+RegistrationAssessmentJson(written.assessment),
        "\"warnings\":"+RegistrationWarningsJson(before.warnings)+",\"assessment\":"+RegistrationAssessmentJson(before.assessment));
    replace_once("a0.uncalibrated-still-review.v10","a0.uncalibrated-still-review.v8");
    if(!replaced)return folder/"uncalibrated-review.json";
    { std::ofstream out(folder/"uncalibrated-review.json",std::ios::binary|std::ios::trunc);out.write(text.data(),static_cast<std::streamsize>(text.size())); }
    fs::copy_file(made.jpeg,folder/"uncalibrated-review.jpg",fs::copy_options::overwrite_existing);
    return folder/"uncalibrated-review.json";
}
void Registered(const fs::path& root,const fs::path& inspector,const fs::path& fixture){
    namespace rf=registered_review_fixture;
    fs::create_directories(root);
    const auto base_profile=rf::ReadText(fixture/"calibrated-draft.json");
    const rf::World world(17);
    std::optional<StillReviewResult> upright,upright_hand;
    for(const bool turned:{false,true}){
        const auto name=std::string(turned?"turned":"upright");
        const auto settings=rf::MakeSettings(root/("settings-"+name),rf::ProfileText(base_profile,turned),turned?MountOrientation::rotated_180:MountOrientation::normal,rf::TallRaster());
        const auto frames=rf::TexturedFrames(root,"frames-"+name,settings,turned,world);
        // A capture without a position (v10), the same capture with a position set by hand (v10, level confirmed), the first in the v8 form.
        const auto made=WriteUncalibratedStillReview(rf::RequestFor(frames,"review",settings,4));
        auto hand_request=rf::RequestFor(frames,"review-hand",settings,4);
        hand_request.fixed_composite_registration->confirmed_position=rf::HandPosition(settings);
        const auto by_hand=WriteUncalibratedStillReview(hand_request);
        Check(ReadBytes(made.jpeg)!=ReadBytes(by_hand.jpeg));   // the position changed the saved JPEG: the two cases do not show the same picture
        const std::vector<fs::path> guarded{settings.folder,fixture};
        OneRegistered(root,inspector,made.report,turned,name,{"v10",false},guarded);
        OneRegistered(root,inspector,by_hand.report,turned,name+"-hand",{"v10",true},guarded);
        OneRegistered(root,inspector,AsV8(root,made,name),turned,name+"-v8",{"v8",false},guarded);
        if(!turned){upright=made;upright_hand=by_hand;}
    }
    if(upright)ChangedJpeg(root,inspector,*upright,"v9");
    if(upright_hand)ChangedJpeg(root,inspector,*upright_hand,"v9-hand");
}

// A real seam-cut review of a small synthetic pair, written and read back.
void RealReview(const fs::path& root,const fs::path& inspector){
    const auto gradient=[](std::uint32_t seed){
        BgrImage image{64,48,std::vector<std::uint8_t>(64*48*3)};
        for(std::uint32_t y=0;y<48;++y)for(std::uint32_t x=0;x<64;++x){
            auto* p=image.bgr.data()+(static_cast<size_t>(y)*64+x)*3;
            p[0]=static_cast<std::uint8_t>(40+x*2+seed);p[1]=static_cast<std::uint8_t>(60+y*3);p[2]=static_cast<std::uint8_t>(90+((x*5+y*7+seed)%40));
        }
        return image;
    };
    fs::create_directories(root/"CAM-A");fs::create_directories(root/"CAM-B");
    WriteJpeg(root/"CAM-A/original.jpg",gradient(0));WriteJpeg(root/"CAM-B/original.jpg",gradient(0));
    const auto bytes_a=ReadBytes(root/"CAM-A/original.jpg"),bytes_b=ReadBytes(root/"CAM-B/original.jpg");
    const auto hash=[](const std::vector<char>& b){return a0::phase0::Sha256Hex(std::vector<unsigned char>(b.begin(),b.end()));};
    StillReviewRequest request{root/"CAM-A/original.jpg",root/"CAM-B/original.jpg",root/"run"/"review",hash(bytes_a),hash(bytes_b),"synthetic-pair-1",{16,12,8,1,9},
        std::chrono::steady_clock::now()+std::chrono::seconds(60)};
    request.blend=BlendMethod::seam_cut;
    fs::create_directories(root/"run");
    const auto made=WriteUncalibratedStillReview(request);
    const auto record=LoadSavedStillReview(made.report);
    const auto composite=LoadStillReviewImage(record);
    Check(record.blend && !record.blend->seam_x.empty() && record.width==96 && record.height==44);
    const auto crops=ExtractSeamCrops(record,composite,31,20);
    Check(crops.has_value());
    if(crops){
        const std::array<std::uint32_t,3> tops{0,12,24};
        for(size_t i=0;i<3;++i){
            const auto& c=crops->crop[i];
            Check(c.top==tops[i] && c.image.height==20 && c.image.width==31);
            // The centre is the report's seam at the crop's centre row; the pixels are the composite's.
            Check(c.seam_column==record.blend->seam_x[c.top+10] && c.left+c.center_index==c.seam_column && SameWindow(c.image,composite,c.left,c.top));
        }
        Check(crops->crop[2].top+crops->crop[2].image.height==record.height);
    }
    // The export from the result folder (the run folder or its review folder) is the same record.
    const auto sheet_expected=ComposeSeamCropSheet(*ExtractSeamCrops(record,composite,px::kSeamCropExportWidth,px::kSeamCropExportHeight),px::kSeamCropExportGap,px::kSeamCropExportGapBgr);
    const auto exported=px::ExportSeamCrops(root/"run",root/"export-a.png");
    Check(exported.sheet.bgr==sheet_expected.image.bgr && ReadPng(root/"export-a.png").bgr==sheet_expected.image.bgr);
    Check(exported.height==44 && exported.top[0]==0 && exported.top[2]==0 && exported.seam_column[1]==record.blend->seam_x[22]);
    (void)px::ExportSeamCrops(root/"run"/"review",root/"export-b.png");
    Check(ReadPng(root/"export-b.png").bgr==sheet_expected.image.bgr);
    // Not an existing file; not a folder without a report.
    Check(Throws([&]{(void)px::ExportSeamCrops(root/"run",root/"export-a.png");}));
    Check(Throws([&]{(void)px::ExportSeamCrops(root,root/"export-c.png");}) && !fs::exists(root/"export-c.png"));
    // The reviews's original pair and composite are untouched by the export.
    Check(ReadBytes(root/"CAM-A/original.jpg")==bytes_a && ReadBytes(root/"CAM-B/original.jpg")==bytes_b && ReadBytes(made.jpeg).size()>0);
    if(inspector.empty())return;

    // The command line of the inspector.
    const auto out=root/"cli.png";
    Check(RunInspector(inspector,L"--export-seam-crops \""+(root/"run"/"review").wstring()+L"\" \""+out.wstring()+L"\"",120000)==0 && fs::exists(out));
    Check(fs::exists(out) && ReadPng(out).bgr==sheet_expected.image.bgr);
    const auto cli_bytes=ReadBytes(out);
    Check(RunInspector(inspector,L"--export-seam-crops \""+(root/"run"/"review").wstring()+L"\" \""+out.wstring()+L"\"",120000)!=0 && ReadBytes(out)==cli_bytes); // never overwrites
    Check(RunInspector(inspector,L"--export-seam-crops \""+root.wstring()+L"\" \""+(root/"cli-none.png").wstring()+L"\"",120000)!=0 && !fs::exists(root/"cli-none.png"));
    // The window: off at the start, three crops whose pixels are read back from the screen, the seam line over them, off again.
    {
        // The same expectation as before (exit code 0). The output also says that a report with a manual placement decodes its two originals.
        const auto run=RunCaptured(inspector,L"--report \""+made.report.wstring()+L"\" --seam-crops-smoke",240000,root);
        Check(run.code==0);
        Check(Has(run.out,"review_loaded=1 ui_window_visible=1") && Field(run.out,"fixed_mapping=")==0 && Field(run.out,"original_decodes=")==2);
    }
}
}

int main(int argc,char** argv){
    try{
        Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
        const auto root=fs::current_path()/("seam-crops-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        fs::create_directories(root);
        Rows();Columns();Pixels();
        Png(root);
        RealReview(root/"real",argc>1?fs::path(argv[1]):fs::path());
        if(argc>2)Registered(root/"registered",fs::path(argv[1]),fs::path(argv[2]));
        else{
            // The registered reviews are the contract of #418: running without them is a failure, never a pass.
            std::cerr<<"the registered reviews were not run: pass the StillReview executable and the rig profile v2 fixture folder\n";
            Check(false);
        }
        std::error_code ignored;fs::remove_all(root,ignored);
    }catch(const std::exception& error){std::cerr<<"exception: "<<error.what()<<'\n';++failures;}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
