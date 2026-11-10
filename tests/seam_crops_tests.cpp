// Seam crops of a saved composite (#418 stage 0). Synthetic data only; no camera.
// Pure decisions (rows, columns), the pixels of the crops against the composite, the PNG
// record, a real seam-cut review written and read back, and the inspector itself
// (--export-seam-crops, and the window's own check with --seam-crops-smoke).
#include "a0/m2/experimental/seam_crops.hpp"
#include "a0/phase0/experimental/seam_crops_export.hpp"
#include "a0/phase0/phase0.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <source_location>

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
    Check(RunInspector(inspector,L"--report \""+made.report.wstring()+L"\" --seam-crops-smoke",240000)==0);
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
        std::error_code ignored;fs::remove_all(root,ignored);
    }catch(const std::exception& error){std::cerr<<"exception: "<<error.what()<<'\n';++failures;}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
