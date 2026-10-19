#include "a0/common/protocol_json.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <wincodec.h>
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Process-only contract evidence: no offline stitcher, renderer, profile
// reader, manifest implementation, or product SHA helper is linked here.
namespace {
namespace fs=std::filesystem;
using Json=a0::common::protocol_json::JsonValue;
using Kind=a0::common::protocol_json::JsonKind;
int checks=0;
void Require(bool condition,const std::string& reason) { ++checks; if(!condition) throw std::runtime_error(reason); }
void Hr(HRESULT result) { if(FAILED(result)) throw std::runtime_error("test WIC operation failed"); }
template<class T> struct Release { void operator()(T* item) const { if(item) item->Release(); } };
template<class T> using Com=std::unique_ptr<T,Release<T>>;
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle):value(handle) {}
    ~Handle(){ if(value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
};
std::vector<std::uint8_t> Bytes(const fs::path& path) {
    std::ifstream input(path,std::ios::binary); if(!input) throw std::runtime_error("test fixture read failed");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
std::string Text(const fs::path& path) { auto bytes=Bytes(path); return {bytes.begin(),bytes.end()}; }
void Write(const fs::path& path,const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if(!output) throw std::runtime_error("test fixture write failed");
}
void WriteText(const fs::path& path,const std::string& text) { Write(path,{text.begin(),text.end()}); }
std::string Sha(const fs::path& path) {
    const auto bytes=Bytes(path); BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("test SHA provider failed");
    DWORD length{},copied{}; std::vector<std::uint8_t> object,digest(32);
    try {
        if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&copied,0)<0) throw std::runtime_error("test SHA property failed");
        object.resize(length);
        if(BCryptCreateHash(algorithm,&hash,object.data(),length,nullptr,0,0)<0 ||
            BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0)<0 ||
            BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) throw std::runtime_error("test SHA operation failed");
    } catch(...) { if(hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm,0); throw; }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm,0);
    constexpr char hex[]="0123456789abcdef"; std::string result;
    for(auto byte:digest){ result+=hex[byte>>4]; result+=hex[byte&15]; } return result;
}
std::wstring FinalPath(HANDLE handle) {
    DWORD size=GetFinalPathNameByHandleW(handle,nullptr,0,VOLUME_NAME_GUID);
    if(!size) throw std::runtime_error("test final path unavailable");
    std::wstring value(size,L'\0'); auto used=GetFinalPathNameByHandleW(handle,value.data(),size,VOLUME_NAME_GUID);
    if(!used || used>=size) throw std::runtime_error("test final path resolution failed"); value.resize(used); return value;
}
void DeleteOwnedTree(const fs::path& root) {
    for(const auto& item:fs::directory_iterator(root)) {
        DWORD attributes=GetFileAttributesW(item.path().c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES) throw std::runtime_error("owned cleanup metadata failed");
        if(attributes&FILE_ATTRIBUTE_DIRECTORY) {
            if(!(attributes&FILE_ATTRIBUTE_REPARSE_POINT)) DeleteOwnedTree(item.path());
            if(!RemoveDirectoryW(item.path().c_str())) throw std::runtime_error("owned cleanup directory failed");
        } else if(!DeleteFileW(item.path().c_str())) throw std::runtime_error("owned cleanup file failed");
    }
}
struct Temp {
    fs::path root; BY_HANDLE_FILE_INFORMATION identity{}; std::wstring resolved,parent;
    Temp() {
        auto base=fs::temp_directory_path(); root=base/("a0-stitch-eval-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        Require(fs::create_directory(root),"test root exclusive creation");
        Handle directory(CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        Handle ancestor(CreateFileW(base.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        Require(directory.value!=INVALID_HANDLE_VALUE && ancestor.value!=INVALID_HANDLE_VALUE && GetFileInformationByHandle(directory.value,&identity),"test root identity");
        resolved=FinalPath(directory.value); parent=FinalPath(ancestor.value); if(parent.back()!=L'\\') parent+=L'\\';
        Require(resolved.starts_with(parent) && resolved.substr(parent.size()).find(L'\\')==std::wstring::npos,"owned root physical boundary");
    }
    ~Temp() {
        try {
            Handle directory(CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            BY_HANDLE_FILE_INFORMATION current{};
            if(directory.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory.value,&current) ||
                current.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT || current.dwVolumeSerialNumber!=identity.dwVolumeSerialNumber ||
                current.nFileIndexHigh!=identity.nFileIndexHigh || current.nFileIndexLow!=identity.nFileIndexLow || FinalPath(directory.value)!=resolved)
                throw std::runtime_error("owned cleanup identity changed; preserved");
            DeleteOwnedTree(root); if(!RemoveDirectoryW(root.c_str())) throw std::runtime_error("owned cleanup root failed");
        } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; }
    }
};
std::wstring Quote(const std::wstring& value) {
    std::wstring result=L"\""; unsigned slashes=0;
    for(wchar_t ch:value){ if(ch==L'\\'){++slashes;continue;} result.append(ch==L'\"'?slashes*2+1:slashes,L'\\'); slashes=0;result+=ch; }
    result.append(slashes*2,L'\\');return result+L"\"";
}
struct CliResult { DWORD exit; std::string out,error; };
CliResult Run(const fs::path& exe,const fs::path& root,const std::vector<std::wstring>& args,const std::wstring& spoof_local={}) {
    static unsigned sequence=0; const auto tag=std::to_string(++sequence);
    fs::path out=root/("stdout-"+tag),err=root/("stderr-"+tag);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    Handle output(CreateFileW(out.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Handle errors(CreateFileW(err.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr));
    Require(output.value!=INVALID_HANDLE_VALUE && errors.value!=INVALID_HANDLE_VALUE && input.value!=INVALID_HANDLE_VALUE,"child log reservation");
    std::wstring command=Quote(exe.native());for(const auto& arg:args) command+=L" "+Quote(arg);
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=output.value;startup.hStdError=errors.value;startup.hStdInput=input.value;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> environment;
    if(!spoof_local.empty()) {
        auto raw=GetEnvironmentStringsW();Require(raw!=nullptr,"child environment available");std::vector<std::wstring> entries;
        for(const wchar_t* item=raw;*item;item+=std::wcslen(item)+1)if(_wcsnicmp(item,L"LOCALAPPDATA=",13)!=0)entries.emplace_back(item);
        FreeEnvironmentStringsW(raw);entries.push_back(L"LOCALAPPDATA="+spoof_local);
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return CompareStringOrdinal(a.c_str(),static_cast<int>(a.size()),b.c_str(),static_cast<int>(b.size()),TRUE)==CSTR_LESS_THAN;});
        for(const auto& entry:entries){environment.insert(environment.end(),entry.begin(),entry.end());environment.push_back(L'\0');}environment.push_back(L'\0');
    }
    Require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,environment.empty()?nullptr:environment.data(),nullptr,&startup,&process)!=0,"actual CLI child started");
    Handle child(process.hProcess),thread(process.hThread);
    if(WaitForSingleObject(child.value,60'000)!=WAIT_OBJECT_0){TerminateProcess(child.value,199);WaitForSingleObject(child.value,5'000);throw std::runtime_error("owned CLI child timeout");}
    DWORD exit{};Require(GetExitCodeProcess(child.value,&exit)!=0,"child exit code available"); return {exit,Text(out),Text(err)};
}
std::string TrimLines(std::string text) { text.erase(std::remove(text.begin(),text.end(),'\r'),text.end());while(!text.empty()&&text.back()=='\n')text.pop_back();return text; }
std::vector<std::wstring> Arguments(const fs::path& root,const fs::path& output,const std::wstring& kernel=L"bilinear") {
    return {L"--camera-a",(root/"CAM-A/original.jpg").native(),L"--camera-b",(root/"CAM-B/original.jpg").native(),
        L"--profile-file",(root/"draft.json").native(),L"--product-root",(root/"product").native(),L"--output-directory",output.native(),
        L"--left-um",L"565000",L"--top-um",L"400000",L"--right-um",L"566000",L"--bottom-um",L"401000",L"--dpi",L"100",L"--width-pixels",L"4",L"--height-pixels",L"4",L"--resampling",kernel};
}
void Change(std::vector<std::wstring>& args,const std::wstring& key,const std::wstring& value) {
    auto found=std::find(args.begin(),args.end(),key);if(found==args.end() || ++found==args.end())throw std::runtime_error("test option missing");*found=value;
}
void Reject(const fs::path& exe,const fs::path& root,const std::vector<std::wstring>& args,const std::string& stage,const fs::path& no_output={}) {
    auto result=Run(exe,root,args);
    Require(result.exit==2 && result.out.empty() && TrimLines(result.error)=="error="+stage+"-failed","specific CLI refusal "+stage+"; actual="+result.error);
    if(!no_output.empty()) Require(!fs::exists(no_output),"refusal does not create output");
}
// Independent raw-pixel similarity oracle for JPEG scenarios. The inverse is
// evaluated explicitly in scalar arithmetic, without any production warp code.
struct SyntheticSimilarity {
    double a,b,dx,dy,cx,cy;
    bool color;
};
void Synthetic(const fs::path& path,unsigned width,unsigned height,unsigned camera,const SyntheticSimilarity* pose=nullptr) {
    fs::create_directories(path.parent_path());IWICImagingFactory* raw{};Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&raw)));Com<IWICImagingFactory> factory(raw);
    IWICStream* sr{};Hr(factory->CreateStream(&sr));Com<IWICStream> stream(sr);Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    IWICBitmapEncoder* er{};Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&er));Com<IWICBitmapEncoder> encoder(er);Hr(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
    IWICBitmapFrameEncode* fr{};IPropertyBag2* br{};Hr(encoder->CreateNewFrame(&fr,&br));Com<IWICBitmapFrameEncode> frame(fr);Com<IPropertyBag2> bag(br);
    if(camera>=2){PROPBAG2 property{};property.pstrName=const_cast<wchar_t*>(L"ImageQuality");VARIANT quality{};quality.vt=VT_R4;quality.fltVal=camera>=6?1.f:.98f;Hr(bag->Write(1,&property,&quality));}
    if(camera>=5){PROPBAG2 property{};property.pstrName=const_cast<wchar_t*>(L"JpegYCrCbSubsampling");VARIANT subsampling{};subsampling.vt=VT_UI1;subsampling.bVal=WICJpegYCrCbSubsampling444;Hr(bag->Write(1,&property,&subsampling));}
    Hr(frame->Initialize(bag.get()));Hr(frame->SetSize(width,height));WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format));Require(format==GUID_WICPixelFormat24bppBGR,"source JPEG BGR format");
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width)*3);
    for(unsigned y=0;y<height;++y){for(unsigned x=0;x<width;++x){
        if(pose) {
            double value=100;
            // A generous scene window surrounds all requested output pixels.
            if(x>=3200&&x<3456&&y>=128&&y<384) {
                const double qx=x-pose->cx-pose->dx,qy=y-pose->cy-pose->dy,det=pose->a*pose->a+pose->b*pose->b;
                const double u=(pose->a*qx+pose->b*qy)/det+pose->cx-3300;
                const double v=(-pose->b*qx+pose->a*qy)/det+pose->cy-256;
                value=128+24*std::sin(.31*u)+24*std::cos(.29*v)+8*std::sin(.21*u-.23*v);
            }
            row[x*3]=static_cast<BYTE>(std::lround(pose->color?value*1.25+8:value));
            row[x*3+1]=static_cast<BYTE>(std::lround(pose->color?value*.75+12:value));
            row[x*3+2]=static_cast<BYTE>(std::lround(pose->color?value*.5+16:value));
        }
        else if(camera>=7) {
            const bool shifted=camera==8||camera==9||camera==11;
            const double u=static_cast<double>(x)-3300-(shifted?2.35:0),v=static_cast<double>(y)-256+(shifted?1.65:0);
            double value=100;
            if(x>=3200&&x<3456&&y>=128&&y<384)value=camera>=10 ?
                128+24*std::sin(.31*u+.13*v)+20*std::cos(.19*v-.07*u)+16*std::sin(.21*u-.23*v) :
                128+24*std::sin(.31*u)+24*std::cos(.29*v)+8*std::sin(.21*u-.23*v);
            // Independent continuous scene; no production sampling/projection.
            row[x*3]=static_cast<BYTE>(std::lround(camera==9?value*1.25+8:value));
            row[x*3+1]=static_cast<BYTE>(std::lround(camera==9?value*.75+12:value));
            row[x*3+2]=static_cast<BYTE>(std::lround(camera==9?value*.5+16:value));
        }
        else if(camera<2){row[x*3]=static_cast<std::uint8_t>(30+(x/3+y/7+camera*19)%180);row[x*3+1]=static_cast<std::uint8_t>(40+(x/5+y/3+camera*37)%170);row[x*3+2]=static_cast<std::uint8_t>(50+(x/7+y/5+camera*53)%160);}
        else {
            // Independent known translation in stored pixels: B(x,y)=A(x-3,y+2).
            const int u=static_cast<int>(x)-(camera==3||camera>=5?3:0),v=static_cast<int>(y)+(camera==3||camera>=5?2:0);
            auto h=static_cast<std::uint32_t>(u)*0x45d9f3bu+static_cast<std::uint32_t>(v)*0x119de1f3u;h^=h>>16;h*=0x45d9f3bu;h^=h>>16;
            const auto value=static_cast<std::uint8_t>(camera!=4&&u>=3200&&u<3456&&v>=128&&v<384?32+4*(h%32):100);
            row[x*3]=row[x*3+1]=row[x*3+2]=value;
            if(camera>=5){row[x*3]=static_cast<std::uint8_t>(value*1.25+8);row[x*3+1]=static_cast<std::uint8_t>(value*.75+12);row[x*3+2]=static_cast<std::uint8_t>(value*.5+16);}
        }
    }Hr(frame->WritePixels(1,width*3,static_cast<UINT>(row.size()),row.data()));}
    Hr(frame->Commit());Hr(encoder->Commit());
}
void CheckJpeg(const fs::path& path,unsigned expected_width=4,unsigned expected_height=4) {
    const auto bytes=Bytes(path);Require(bytes.size()>4 && bytes[0]==255 && bytes[1]==216 && bytes[bytes.size()-2]==255 && bytes.back()==217,"JPEG exact envelope");
    unsigned comments=0,jfif=0;bool density=false,sos=false;std::size_t p=2;
    while(p+1<bytes.size()){
        Require(bytes[p++]==255,"JPEG marker start");while(p<bytes.size() && bytes[p]==255)++p;Require(p<bytes.size(),"JPEG marker available");auto marker=bytes[p++];
        if(marker==218){sos=true;break;}Require(marker!=217 && marker!=0 && marker!=1 && !(marker>=208&&marker<=215) && p+2<=bytes.size(),"JPEG header marker type");
        std::size_t length=bytes[p]*256U+bytes[p+1];Require(length>=2&&length<=bytes.size()-p,"JPEG marker length");const auto begin=bytes.begin()+static_cast<std::ptrdiff_t>(p+2);std::size_t count=length-2;
        if(marker==254){++comments;Require(std::string(begin,begin+static_cast<std::ptrdiff_t>(count))=="profileStatus=draft;quality=not-evaluated","JPEG internal draft and quality label");}
        if(marker==224 && count>=5 && std::equal(begin,begin+5,"JFIF\0")){++jfif;Require(count>=14,"JFIF header size");density=bytes[p+9]==1&&bytes[p+10]*256U+bytes[p+11]==100&&bytes[p+12]*256U+bytes[p+13]==100;}
        if(marker==225 && count>=6)Require(!std::equal(begin,begin+6,"Exif\0\0"),"output carries no EXIF");p+=length;
    }
    Require(sos&&comments==1&&jfif==1&&density,"JPEG labels and physical DPI exact");
    IWICImagingFactory* raw{};Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&raw)));Com<IWICImagingFactory> factory(raw);
    IWICBitmapDecoder* dr{};Hr(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&dr));Com<IWICBitmapDecoder> decoder(dr);
    UINT frames{};Hr(decoder->GetFrameCount(&frames));Require(frames==1,"output one decoded frame");IWICBitmapFrameDecode* fr{};Hr(decoder->GetFrame(0,&fr));Com<IWICBitmapFrameDecode> frame(fr);UINT width{},height{};Hr(frame->GetSize(&width,&height));Require(width==expected_width&&height==expected_height,"actual JPEG decoded dimensions");
    IWICFormatConverter* cr{};Hr(factory->CreateFormatConverter(&cr));Com<IWICFormatConverter> converter(cr);Hr(converter->Initialize(frame.get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));std::vector<BYTE> pixels(static_cast<std::size_t>(width)*height*3);Hr(converter->CopyPixels(nullptr,width*3,static_cast<UINT>(pixels.size()),pixels.data()));
    bool varies=false;for(std::size_t i=3;i<pixels.size();i+=3)if(!std::equal(pixels.begin(),pixels.begin()+3,pixels.begin()+static_cast<std::ptrdiff_t>(i)))varies=true;Require(varies,"real nonconstant source yields decoded nonconstant crop");
}
struct Failure { [[noreturn]] static void Fail(std::string_view,std::string_view reason){throw std::runtime_error(std::string(reason));} };
void Fields(const Json& value,std::initializer_list<std::string_view> expected){Require(value.kind==Kind::object&&value.object.size()==expected.size(),"strict manifest object field count");for(auto name:expected)Require(value.object.contains(std::string(name)),"manifest required field");}
const Json& At(const Json& value,const char* name){return value.object.at(name);}
void String(const Json& value,const char* name,const std::string& expected){const auto& leaf=At(value,name);Require(leaf.kind==Kind::string&&leaf.string==expected,std::string("manifest string ")+name);}
void Number(const Json& value,const char* name,std::uint64_t expected){const auto& leaf=At(value,name);Require(leaf.kind==Kind::number&&leaf.string==std::to_string(expected),std::string("manifest exact integer ")+name);}
void Manifest(const fs::path& root,const fs::path& output,const std::string& kernel,const std::string& golden) {
    const auto manifest=a0::common::protocol_json::BasicJsonParser<Failure>(Text(output/"stitch-eval.manifest.json")).Parse();
    Fields(manifest,{"schemaVersion","profileStatus","quality","profileVersion","profileFingerprintSha256","resampling","outputRaster","inputs","output","engine"});
    String(manifest,"schemaVersion","a0.stitch-eval-manifest.v1");String(manifest,"profileStatus","draft");String(manifest,"quality","not-evaluated");String(manifest,"profileVersion","2.0.0");String(manifest,"profileFingerprintSha256",golden);String(manifest,"resampling",kernel);
    const auto& raster=At(manifest,"outputRaster");Fields(raster,{"regionUm","dpi","widthPixels","heightPixels"});Number(raster,"dpi",100);Number(raster,"widthPixels",4);Number(raster,"heightPixels",4);const auto& region=At(raster,"regionUm");Fields(region,{"left","top","right","bottom"});Number(region,"left",565000);Number(region,"top",400000);Number(region,"right",566000);Number(region,"bottom",401000);
    const auto& inputs=At(manifest,"inputs");Require(inputs.kind==Kind::array&&inputs.array.size()==2,"manifest ordered input pair");for(unsigned i=0;i<2;++i){const auto& input=inputs.array[i];Fields(input,{"cameraAlias","sha256","encodedSizeBytes"});std::string alias=i==0?"CAM-A":"CAM-B";const auto path=root/alias/"original.jpg";String(input,"cameraAlias",alias);String(input,"sha256",Sha(path));Number(input,"encodedSizeBytes",fs::file_size(path));}
    const auto& image=At(manifest,"output");Fields(image,{"relativePath","sha256","encodedSizeBytes","widthPixels","heightPixels"});String(image,"relativePath","evaluation.jpg");String(image,"sha256",Sha(output/"evaluation.jpg"));Number(image,"encodedSizeBytes",fs::file_size(output/"evaluation.jpg"));Number(image,"widthPixels",4);Number(image,"heightPixels",4);
    const auto& engine=At(manifest,"engine");Fields(engine,{"name","version"});String(engine,"name","a0.m2.stitch-eval");String(engine,"version","1.0.0");Require(!fs::exists(output/"stitch-job.manifest.json")&&!fs::exists(output/"stitched.jpg"),"evaluation produces no product artifact names");
    unsigned entries=0;for(const auto& item:fs::directory_iterator(output)){++entries;Require(item.is_regular_file(),"published eval contains only regular leaves");}Require(entries==2,"exactly separate eval JPEG and manifest");
}
void Junction(const fs::path& link,const fs::path& target) {
    Require(fs::create_directory(link),"owned junction reservation");std::wstring substitute=L"\\??\\"+fs::absolute(target).native(),print=fs::absolute(target).native();
    const auto payload=(substitute.size()+print.size()+2)*sizeof(wchar_t);std::vector<std::uint8_t> buffer(16+payload);auto put16=[&](std::size_t index,WORD value){std::memcpy(buffer.data()+index,&value,sizeof(value));};DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;std::memcpy(buffer.data(),&tag,sizeof(tag));put16(4,static_cast<WORD>(8+payload));put16(8,0);put16(10,static_cast<WORD>(substitute.size()*2));put16(12,static_cast<WORD>((substitute.size()+1)*2));put16(14,static_cast<WORD>(print.size()*2));std::memcpy(buffer.data()+16,substitute.c_str(),(substitute.size()+1)*2);std::memcpy(buffer.data()+16+(substitute.size()+1)*2,print.c_str(),(print.size()+1)*2);
    Handle handle(CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));DWORD written{};Require(handle.value!=INVALID_HANDLE_VALUE&&DeviceIoControl(handle.value,FSCTL_SET_REPARSE_POINT,buffer.data(),static_cast<DWORD>(buffer.size()),nullptr,0,&written,nullptr),"owned unprivileged junction setup");
}
void Evidence(const fs::path& directory,const fs::path& source,const std::wstring& leaf) {
    if(directory.empty())return;
    for(auto ancestor=directory;!ancestor.empty();ancestor=ancestor.parent_path()) {
        auto attributes=GetFileAttributesW(ancestor.c_str());Require(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY) && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"evidence directory remains ordinary");
        if(ancestor==ancestor.parent_path())break;
    }
    Handle parent(CreateFileW(directory.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));Require(parent.value!=INVALID_HANDLE_VALUE,"evidence parent lock");
    auto target=directory/leaf;Handle file(CreateFileW(target.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));Require(file.value!=INVALID_HANDLE_VALUE,"new evidence file exclusive reservation");
    const auto bytes=Bytes(source);DWORD written{};Require(WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) && written==bytes.size() && FlushFileBuffers(file.value),"positive manifest evidence copy");Require(Bytes(target)==bytes,"evidence reread agrees with actual manifest");
}
fs::path ActualProductRoot() {
    const auto module=LoadLibraryW(L"shell32.dll");Require(module!=nullptr,"KnownFolder module available");
    using KnownFolder=HRESULT(WINAPI*)(const GUID&,DWORD,HANDLE,wchar_t**);
    const auto symbol=GetProcAddress(module,"SHGetKnownFolderPath");KnownFolder query{};static_assert(sizeof(query)==sizeof(symbol));std::memcpy(&query,&symbol,sizeof(query));
    constexpr GUID local{0xf1b32785,0x6fba,0x4fcf,{0x9d,0x55,0x7b,0x8e,0x7f,0x15,0x70,0x91}};wchar_t* value{};
    const auto status=query?query(local,0,nullptr,&value):E_FAIL;fs::path path;if(SUCCEEDED(status)&&value)path=fs::path(value)/L"A0CameraStitcher";CoTaskMemFree(value);FreeLibrary(module);Require(!path.empty(),"actual product KnownFolder available");return path;
}
std::string Replace(std::string text,const std::string& from,const std::string& to) {
    const auto p=text.find(from);Require(p!=std::string::npos,"correction mutation fixture found");text.replace(p,from.size(),to);return text;
}
Json Parse(const fs::path& path) {return a0::common::protocol_json::BasicJsonParser<Failure>(Text(path)).Parse();}
double Real(const Json& value){Require(value.kind==Kind::number,"diagnostic real number");return std::stod(value.string);}
std::vector<BYTE> DecodedCrop(const fs::path& path) {
    IWICImagingFactory* raw{};Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&raw)));Com<IWICImagingFactory> factory(raw);
    IWICBitmapDecoder* dr{};Hr(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&dr));Com<IWICBitmapDecoder> decoder(dr);
    IWICBitmapFrameDecode* fr{};Hr(decoder->GetFrame(0,&fr));Com<IWICBitmapFrameDecode> frame(fr);UINT width{},height{};Hr(frame->GetSize(&width,&height));Require(width==40&&height==32,"comparison crops share requested raster");
    IWICFormatConverter* cr{};Hr(factory->CreateFormatConverter(&cr));Com<IWICFormatConverter> converter(cr);Hr(converter->Initialize(frame.get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));std::vector<BYTE> pixels(width*height*3);Hr(converter->CopyPixels(nullptr,width*3,static_cast<UINT>(pixels.size()),pixels.data()));return pixels;
}
double PixelError(const std::vector<BYTE>& a,const std::vector<BYTE>& b) {
    Require(a.size()==b.size()&&!a.empty(),"pixel comparison size");double sum=0;for(std::size_t n=0;n<a.size();++n)sum+=std::abs(static_cast<int>(a[n])-b[n]);return sum/a.size();
}
void RefinementTests(const fs::path& fixtures,const fs::path& eval,const fs::path& parent,const fs::path& evidence) {
    const auto root=parent/"refinement";fs::create_directory(root);fs::create_directory(root/"product");
    const auto a=root/"CAM-A/original.jpg",b=root/"CAM-B/original.jpg",colored=root/"colored/original.jpg";
    WriteText(root/"draft.json",Replace(Text(fixtures/"calibrated-draft.json"),"9400.0","4800.0"));
    const auto job=Text(fixtures.parent_path()/"stitch-correction/refinement-job.json");WriteText(root/"job.json",job);
    const auto color_job=Text(fixtures.parent_path()/"stitch-correction/color-job.json");WriteText(root/"color-job.json",color_job);
    auto coarse_job=Replace(job,"a0.stitch-correction-job.v2","a0.stitch-correction-job.v1");
    const auto refinement_at=coarse_job.find("\"refinement\""),comma=coarse_job.rfind(',',refinement_at),refinement_end=coarse_job.find('}',refinement_at);
    Require(comma!=std::string::npos&&refinement_end!=std::string::npos,"coarse control removes only explicit refinement object");
    coarse_job.erase(comma,refinement_end-comma+1);WriteText(root/"coarse.json",coarse_job);
    Synthetic(a,7360,4912,7);Synthetic(b,7360,4912,8);Synthetic(colored,7360,4912,9);
    fs::create_directory(root/"reference");Write(root/"reference/original.jpg",Bytes(a));
    const std::array<fs::path,6> inputs{a,b,colored,root/"draft.json",root/"job.json",root/"color-job.json"};
    std::array<std::string,6> hashes;for(unsigned i=0;i<inputs.size();++i)hashes[i]=Sha(inputs[i]);
    auto Args=[&](const fs::path& out,const std::wstring& kernel=L"bilinear",bool color=false) {
        auto args=Arguments(root,out,kernel);Change(args,L"--right-um",L"575000");Change(args,L"--bottom-um",L"408000");
        Change(args,L"--width-pixels",L"40");Change(args,L"--height-pixels",L"32");
        args.insert(args.end(),{L"--correction-job",(root/"job.json").native()});
        if(color){Change(args,L"--camera-b",colored.native());args.insert(args.end(),{L"--color-job",(root/"color-job.json").native()});}
        return args;
    };
    auto Report=[&](const fs::path& out,const fs::path& policy,const fs::path& image_b,const fs::path& image_a=fs::path{}) {
        const auto report=Parse(out/"correction-report.json");
        String(report,"schemaVersion","a0.stitch-correction-report.v2");String(report,"profileStatus","draft");String(report,"quality","not-evaluated");
        Require(!At(report,"productApproved").boolean&&At(report,"previewRequiresManifest").boolean,"refinement never authorizes product or standalone preview");
        String(report,"jobSha256",Sha(policy));String(report,"profileFileSha256",hashes[3]);
        String(At(report,"inputs").array[0],"sha256",image_a.empty()?hashes[0]:Sha(image_a));String(At(report,"inputs").array[1],"sha256",Sha(image_b));
        Require(Text(out/"correction-report.json").find(Text(policy))!=std::string::npos,"refinement exact policy snapshot");
        Require(Text(out/"correction-report.json").find(root.string())==std::string::npos,"refinement diagnostics contain no absolute path");
        return report;
    };
    for(const auto& kernel:std::array<std::wstring,2>{L"bilinear",L"bicubic-catmull-rom"}) {
        const auto truth=root/(kernel+L"-truth"),coarse=root/(kernel+L"-coarse");
        auto oracle=Args(truth,kernel);oracle.resize(oracle.size()-2);Change(oracle,L"--camera-b",(root/"reference/original.jpg").native());
        auto baseline=Args(coarse,kernel);Change(baseline,L"--correction-job",(root/"coarse.json").native());
        Require(Run(eval,root,oracle).exit==0&&Run(eval,root,baseline).exit==0,"independent unshifted and coarse-only JPEG controls succeed");
        const auto expected=DecodedCrop(truth/"evaluation.jpg");const double before=PixelError(DecodedCrop(coarse/"evaluation.jpg"),expected);
        Require(before>1,"coarse-only displacement has measurable JPEG error");
        for(const auto color:{false,true}) {
            std::vector<std::string> previous;
            for(unsigned repeat=0;repeat<2;++repeat) {
                const auto tag=std::wstring(L"refinement-")+(color?L"color-":L"gray-")+kernel+L"-"+std::to_wstring(repeat);
                const auto out=root/tag;
                const auto result=Run(eval,root,Args(out,kernel,color));
                if(result.exit!=0&&fs::exists(out/"correction-report.json"))Evidence(evidence,out/"correction-report.json",L"failed-refinement-report.json");
                Require(result.exit==0&&result.error.empty()&&TrimLines(result.out)=="result=evaluated\nprofileStatus=draft\nquality=not-evaluated\nmanifest=stitch-eval.manifest.json","refined JPEG evaluation succeeds: "+result.error+result.out);
                const auto report=Report(out,inputs[4],color?colored:b);String(report,"outcome","correction-estimated");
                const auto& points=At(At(report,"matching"),"points").array;Require(points.size()==6,"all six frozen seeds used with refinement");
                double maximum_error=0;
                for(const auto& p:points) {
                    String(p,"outcome","patch-matched");const auto& raw=At(p,"rawCorrespondence").array;
                    maximum_error=std::max(maximum_error,std::hypot(Real(raw[1].array[0])-Real(raw[0].array[0])-2.35,Real(raw[1].array[1])-Real(raw[0].array[1])+1.65));
                    for(const auto name:{"forward","reverse"}) {
                        const auto& search=At(p,name);Number(search,"examinedCandidates",81);
                        const auto& levels=At(search,"refinement").array;Require(levels.size()==7,"both directions retain all seven refinement levels");
                        double step=.125,score=Real(At(search,"bestScore"));
                        for(const auto& level:levels) {
                            Fields(level,{"stepMm","outcome","bestDocumentMm","bestScore","secondScore","maximumTargetIntensityStddev","examinedCandidates","eligibleCandidates"});
                            step/=2;String(level,"outcome","patch-matched");Number(level,"examinedCandidates",9);Number(level,"eligibleCandidates",9);
                            Require(Real(At(level,"stepMm"))==step&&Real(At(level,"bestScore"))>=score&&Real(At(level,"bestScore"))>Real(At(level,"secondScore")),"refinement diagnostics show the complete unique half-step peak");score=Real(At(level,"bestScore"));
                        }
                    }
                    const auto& last=At(At(p,"forward"),"refinement").array.back();const auto& xy=At(last,"bestDocumentMm").array;
                    Require(std::abs(Real(raw[1].array[0])-(8*Real(xy[1])+100))<1e-8&&std::abs(Real(raw[1].array[1])-(-8*Real(xy[0])+4800))<1e-8,"published raw coordinates use final refined position and fixed projection");
                }
                Require(maximum_error<.15,"known fractional JPEG shift recovered within 0.15 raw pixels");
                const auto m=Parse(out/"stitch-eval.manifest.json");String(m,"schemaVersion","a0.stitch-eval-manifest.v4");String(At(m,"engine"),"version","4.0.0");
                const auto& binding=At(m,"correction");String(binding,"jobSha256",hashes[4]);String(binding,"profileFileSha256",hashes[3]);
                String(At(binding,"report"),"sha256",Sha(out/"correction-report.json"));Number(At(binding,"report"),"encodedSizeBytes",fs::file_size(out/"correction-report.json"));
                String(At(m,"output"),"sha256",Sha(out/"evaluation.jpg"));Number(At(m,"output"),"encodedSizeBytes",fs::file_size(out/"evaluation.jpg"));
                if(color) {
                    const auto c=Parse(out/"color-report.json");String(c,"schemaVersion","a0.stitch-color-report.v1");String(c,"outcome","color-estimated");
                    String(c,"geometryReportSha256",Sha(out/"correction-report.json"));Number(At(c,"fit"),"sampleCount",6);
                    String(At(At(m,"colorCorrection"),"report"),"sha256",Sha(out/"color-report.json"));String(At(m,"colorCorrection"),"jobSha256",hashes[5]);
                    const auto& selected=At(At(report,"selection"),"selectedIndices").array;const auto& candidates=At(At(report,"selection"),"candidates").array;
                    for(std::size_t n=0;n<selected.size();++n) {
                        const auto& wanted=At(candidates[std::stoull(selected[n].string)],"documentMm").array;
                        const auto& found=At(At(c,"points").array[n],"documentMm").array;
                        Require(Real(wanted[0])==Real(found[0])&&Real(wanted[1])==Real(found[1]),"refined color uses entire identical ordered seed set");
                    }
                } else Require(!m.object.contains("colorCorrection"),"geometry-only v4 has no color record");
                unsigned count=0;for(const auto& entry:fs::directory_iterator(out)){++count;Require(entry.is_regular_file(),"refinement output regular leaves only");}
                Require(count==(color?4u:3u),"refined output has exact reports JPEG and final manifest");CheckJpeg(out/"evaluation.jpg",40,32);
                const double after=PixelError(DecodedCrop(out/"evaluation.jpg"),expected);Require(after<1.5&&after<before*.5,"final JPEG improves versus coarse-only control with independent unshifted oracle");
                if(!repeat)std::cout<<"refinement_jpeg_error color="<<color<<" kernel="<<(kernel==L"bilinear"?"bilinear":"bicubic-catmull-rom")<<" rawMax="<<maximum_error<<" before="<<before<<" after="<<after<<'\n';
                std::vector<std::string> current{Sha(out/"correction-report.json"),Sha(out/"evaluation.jpg"),Sha(out/"stitch-eval.manifest.json")};
                if(color)current.push_back(Sha(out/"color-report.json"));if(repeat)Require(current==previous,"refinement report JPEG and manifest repeat byte-identically");else previous=current;
                Evidence(evidence,out/"correction-report.json",tag+L"-report.json");Evidence(evidence,out/"evaluation.jpg",tag+L".jpg");Evidence(evidence,out/"stitch-eval.manifest.json",tag+L"-manifest.json");if(color)Evidence(evidence,out/"color-report.json",tag+L"-color.json");
            }
        }
        Evidence(evidence,coarse/"evaluation.jpg",L"refinement-coarse-"+kernel+L".jpg");Evidence(evidence,truth/"evaluation.jpg",L"refinement-truth-"+kernel+L".jpg");
    }
    auto RejectJob=[&](const std::wstring& name,const std::string& policy,const fs::path& image_b,const fs::path& image_a=fs::path{}) {
        const auto out=root/name,file=root/(name+L".json");WriteText(file,policy);auto args=Args(out);Change(args,L"--correction-job",file.native());Change(args,L"--camera-b",image_b.native());
        if(!image_a.empty())Change(args,L"--camera-a",image_a.native());
        const auto r=Run(eval,root,args);Require(r.exit==3&&r.error.empty()&&TrimLines(r.out)=="result=correction-rejected\nprofileStatus=draft\nquality=not-evaluated\nreport=correction-report.json","refinement refusal is report-only: "+fs::path(name).string()+"; "+r.error+r.out);
        Require(!fs::exists(out/"evaluation.jpg")&&!fs::exists(out/"stitch-eval.manifest.json")&&!fs::exists(out/"color-report.json"),"rejected refinement cannot publish image/manifest/color");
        const auto report=Report(out,file,image_b,image_a);Evidence(evidence,out/"correction-report.json",L"refinement-"+name+L"-report.json");return report;
    };
    const auto over=RejectJob(L"overlimit",Replace(job,"\"maximumDisplacementOutputPixels\": 20","\"maximumDisplacementOutputPixels\": 0"),b);
    const auto& fit=At(At(At(over,"matching"),"rawMapping"),"fit");Require(!At(fit,"candidateAvailable").boolean&&!At(fit,"violations").array.empty(),"refined over-limit fit retained without clamping");
    const auto reverse=RejectJob(L"reverse",Replace(job,"\"maximumReverseErrorMm\": 0.025","\"maximumReverseErrorMm\": 0"),b);
    Require(std::any_of(At(At(reverse,"matching"),"points").array.begin(),At(At(reverse,"matching"),"points").array.end(),[](const auto& p){return At(p,"outcome").string=="reverse-inconsistent"&&At(p,"rawCorrespondence").kind==Kind::null_value;}),"reverse error remains mandatory after both refined searches");
    Synthetic(root/"flat-target/original.jpg",7360,4912,4);RejectJob(L"blank",job,root/"flat-target/original.jpg");
    auto work=Replace(Replace(Replace(Replace(job,"\"gridColumns\": 5","\"gridColumns\": 32"),"\"gridRows\": 4","\"gridRows\": 32"),"\"maximumSeeds\": 6","\"maximumSeeds\": 181"),"\"minimumSpacingMm\": 1","\"minimumSpacingMm\": 0.01");
    work=Replace(work,"\"levels\": 7","\"levels\": 8");
    const auto limited=RejectJob(L"work",work,b);String(At(limited,"matching"),"outcome","work-limit-exceeded");Require(At(At(limited,"matching"),"points").array.empty(),"work cap accounts for fine candidates before matching");
    Synthetic(root/"ambiguous-a/original.jpg",7360,4912,10);Synthetic(root/"ambiguous-b/original.jpg",7360,4912,11);
    const auto ambiguous=RejectJob(L"ambiguous",job,root/"ambiguous-b/original.jpg",root/"ambiguous-a/original.jpg");
    Require(std::any_of(At(At(ambiguous,"matching"),"points").array.begin(),At(At(ambiguous,"matching"),"points").array.end(),[](const auto& p){return At(p,"outcome").string=="ambiguous-patch"&&At(p,"reverse").kind==Kind::object&&At(At(p,"reverse"),"refinement").array.empty();}),"genuine weak reverse coarse separation is preserved as rejection before fine search");
    {const auto file=root/"bad-color.json",out=root/"color-overlimit";WriteText(file,Replace(color_job,"\"maximumGainDifferencePercent\": 150","\"maximumGainDifferencePercent\": 0"));auto args=Args(out,L"bilinear",true);Change(args,L"--color-job",file.native());
        const auto r=Run(eval,root,args);Require(r.exit==3&&TrimLines(r.out)=="result=color-rejected\nprofileStatus=draft\nquality=not-evaluated\nreport=color-report.json"&&!fs::exists(out/"evaluation.jpg")&&!fs::exists(out/"stitch-eval.manifest.json"),"color rejection after refined geometry remains diagnostic-only");
        Report(out,inputs[4],colored);const auto c=Parse(out/"color-report.json");String(c,"geometryReportSha256",Sha(out/"correction-report.json"));Require(!At(At(c,"fit"),"candidateAvailable").boolean,"rejected refined color has no applicable transform");Evidence(evidence,out/"correction-report.json",L"refinement-color-overlimit-report.json");Evidence(evidence,out/"color-report.json",L"refinement-color-overlimit-color.json");}
    unsigned n=0;for(const auto value:{"0","9","7.0","7e0","null","4294967296"}) {
        const auto file=root/("bad-"+std::to_string(++n)+".json"),out=root/("invalid-"+std::to_string(n));WriteText(file,Replace(job,"\"levels\": 7",std::string("\"levels\": ")+value));auto args=Args(out);Change(args,L"--correction-job",file.native());Reject(eval,root,args,"correction-job",out);
    }
    for(unsigned i=0;i<inputs.size();++i)Require(Sha(inputs[i])==hashes[i],"all refined success/refusal paths preserve source profile and jobs");
    Require(fs::is_empty(root/"product"),"refinement leaves product storage untouched");Evidence(evidence,inputs[4],L"refinement-job.json");
}
void SimilarityJpegTests(const fs::path& fixtures,const fs::path& eval,const fs::path& parent,const fs::path& evidence) {
    const auto root=parent/"similarity";fs::create_directory(root);fs::create_directory(root/"product");
    WriteText(root/"draft.json",Replace(Text(fixtures/"calibrated-draft.json"),"9400.0","4800.0"));
    const auto job=Text(fixtures.parent_path()/"stitch-correction/refinement-job.json");WriteText(root/"job.json",job);
    WriteText(root/"color-job.json",Text(fixtures.parent_path()/"stitch-correction/color-job.json"));
    Synthetic(root/"CAM-A/original.jpg",7360,4912,7);fs::create_directory(root/"reference");Write(root/"reference/original.jpg",Bytes(root/"CAM-A/original.jpg"));
    const auto a_hash=Sha(root/"CAM-A/original.jpg"),profile_hash=Sha(root/"draft.json"),job_hash=Sha(root/"job.json"),color_hash=Sha(root/"color-job.json");
    auto Args=[&](const fs::path& out,const fs::path& b,const std::wstring& kernel,bool correction,bool color) {
        auto args=Arguments(root,out,kernel);Change(args,L"--camera-b",b.native());Change(args,L"--right-um",L"575000");Change(args,L"--bottom-um",L"408000");
        Change(args,L"--width-pixels",L"40");Change(args,L"--height-pixels",L"32");
        if(correction)args.insert(args.end(),{L"--correction-job",(root/"job.json").native()});
        if(color)args.insert(args.end(),{L"--color-job",(root/"color-job.json").native()});return args;
    };
    std::ostringstream oracle;oracle.imbue(std::locale::classic());oracle<<std::setprecision(17);
    oracle<<"{\"schemaVersion\":\"a0.synthetic-similarity-jpeg-test.v1\",\"synthetic\":true,\"productApproved\":false,\"quality\":\"not-evaluated\",\"widthPixels\":7360,\"heightPixels\":4912,\"encoderQualitySetting\":1,\"encoderSubsamplingSetting\":\"444\",\"cases\":[";
    bool first=true;
    for(const double sign:{1.,-1.}) {
        const std::wstring label=sign>0?L"positive":L"negative";
        const double angle=sign*.0025,scale=1+sign*.001;
        const SyntheticSimilarity truth{scale*std::cos(angle),scale*std::sin(angle),2.35,-1.65,3332,240,false};
        const auto b=root/(label+L"-gray/original.jpg"),colored=root/(label+L"-color/original.jpg");
        Synthetic(b,7360,4912,7,&truth);auto color_truth=truth;color_truth.color=true;Synthetic(colored,7360,4912,7,&color_truth);
        const auto b_hash=Sha(b),colored_hash=Sha(colored);
        auto Report=[&](const fs::path& out,const fs::path& policy,bool color) {
            const auto r=Parse(out/"correction-report.json");String(r,"schemaVersion","a0.stitch-correction-report.v2");String(r,"profileStatus","draft");String(r,"quality","not-evaluated");
            Require(!At(r,"productApproved").boolean&&At(r,"previewRequiresManifest").boolean,"similarity report remains draft and requires manifest");
            String(r,"profileFileSha256",profile_hash);String(r,"jobSha256",Sha(policy));String(At(r,"inputs").array[0],"sha256",a_hash);String(At(r,"inputs").array[1],"sha256",color?colored_hash:b_hash);
            Require(Text(out/"correction-report.json").find(Text(policy))!=std::string::npos,"similarity exact explicit policy preserved");
            Require(Text(out/"correction-report.json").find(root.string())==std::string::npos,"similarity diagnostics omit absolute paths");return r;
        };
        std::array<std::string,4> estimated_values;
        for(const auto& kernel:std::array<std::wstring,2>{L"bilinear",L"bicubic-catmull-rom"}) {
            const auto reference=root/(label+L"-truth-"+kernel),nominal=root/(label+L"-nominal-"+kernel);
            Require(Run(eval,root,Args(reference,root/"reference/original.jpg",kernel,false,false)).exit==0&&
                Run(eval,root,Args(nominal,b,kernel,false,false)).exit==0,"similarity unshifted oracle and fixed-only negative control render");
            const auto expected=DecodedCrop(reference/"evaluation.jpg");const double gray_before=PixelError(DecodedCrop(nominal/"evaluation.jpg"),expected);Require(gray_before>3,"known pose causes visible fixed-only JPEG error");
            for(const bool color:{false,true}) {
                double before=gray_before;
                if(color) {
                    const auto color_nominal=root/(label+L"-nominal-color-"+kernel);
                    Require(Run(eval,root,Args(color_nominal,colored,kernel,false,false)).exit==0,"colored fixed-only comparison uses the identical input pair");
                    before=PixelError(DecodedCrop(color_nominal/"evaluation.jpg"),expected);Require(before>3,"known color and pose differences are visible before correction");
                }
                std::vector<std::string> previous;
                for(unsigned repeat=0;repeat<2;++repeat) {
                    const auto tag=L"similarity-"+label+(color?L"-color-":L"-gray-")+kernel+L"-"+std::to_wstring(repeat);
                    const auto out=root/tag;const auto result=Run(eval,root,Args(out,color?colored:b,kernel,true,color));
                    if(result.exit!=0&&fs::exists(out/"correction-report.json"))Evidence(evidence,out/"correction-report.json",L"failed-similarity-report.json");
                    Require(result.exit==0&&result.error.empty()&&TrimLines(result.out)=="result=evaluated\nprofileStatus=draft\nquality=not-evaluated\nmanifest=stitch-eval.manifest.json","similarity JPEG correction succeeds: "+result.error+result.out);
                    const auto r=Report(out,root/"job.json",color);String(r,"outcome","correction-estimated");
                    const auto& matching=At(r,"matching");const auto& points=At(matching,"points").array;Require(points.size()==6,"similarity uses all six frozen seeds");
                    double raw_error=0;
                    for(const auto& p:points) {
                        String(p,"outcome","patch-matched");const auto& pair=At(p,"rawCorrespondence").array;
                        const double x=Real(pair[0].array[0])-truth.cx,y=Real(pair[0].array[1])-truth.cy;
                        raw_error=std::max(raw_error,std::hypot(Real(pair[1].array[0])-(truth.a*x-truth.b*y+truth.cx+truth.dx),Real(pair[1].array[1])-(truth.b*x+truth.a*y+truth.cy+truth.dy)));
                        for(const auto name:{"forward","reverse"})Require(At(At(p,name),"refinement").array.size()==7,"both similarity directions retain all fine levels");
                    }
                    Require(raw_error<.15,"rotated/scaled JPEG matches the independent raw-pixel oracle within 0.15 pixels");
                    const auto& fit=At(At(matching,"rawMapping"),"fit");Require(At(fit,"candidateAvailable").boolean&&At(fit,"violations").array.empty(),"similarity fit obeys all explicit limits");
                    const auto& t=At(fit,"estimate").array;const double a=Real(t[0]),b_value=Real(t[1]),tx=Real(t[2]),ty=Real(t[3]);
                    if(!color&&kernel==L"bilinear"&&!repeat)for(unsigned i=0;i<4;++i)estimated_values[i]=t[i].string;
                    double corner_error=0,maximum_displacement=0;
                    // The ceil raster is 10.16 x 8.128 mm, not the nominal 10 x 8.
                    for(const double py:{400.,400.+32*25.4/100})for(const double px:{565.,565.+40*25.4/100}) {
                        const double original_x=8*py+100,original_y=-8*px+4800;
                        const double oracle_x=truth.a*(original_x-truth.cx)-truth.b*(original_y-truth.cy)+truth.cx+truth.dx;
                        const double oracle_y=truth.b*(original_x-truth.cx)+truth.a*(original_y-truth.cy)+truth.cy+truth.dy;
                        const double fitted_x=8*(b_value*px+a*py+ty)+100,fitted_y=-8*(a*px-b_value*py+tx)+4800;
                        corner_error=std::max(corner_error,std::hypot(fitted_x-oracle_x,fitted_y-oracle_y));
                        maximum_displacement=std::max(maximum_displacement,std::hypot(fitted_x-original_x,fitted_y-original_y)/8*100/25.4);
                    }
                    Require(corner_error<.15,"fitted document transform agrees with the independent raw oracle at all full-raster corners");
                    Require(std::abs(Real(At(At(fit,"metrics"),"maximumDisplacementOutputPixels"))-maximum_displacement)<1e-9,"reported displacement bounds the full ceil raster with correct output-pixel units");
                    Require(b_value*truth.b>0&&std::abs(std::hypot(a,b_value)-scale)<.0008,"signed rotation and expansion/contraction survive raw-to-document conversion");
                    const auto m=Parse(out/"stitch-eval.manifest.json");String(m,"schemaVersion","a0.stitch-eval-manifest.v4");String(At(m,"engine"),"version","4.0.0");
                    String(At(At(m,"correction"),"report"),"sha256",Sha(out/"correction-report.json"));String(At(m,"correction"),"jobSha256",job_hash);String(At(m,"output"),"sha256",Sha(out/"evaluation.jpg"));
                    if(color) {const auto c=Parse(out/"color-report.json");String(c,"outcome","color-estimated");String(c,"geometryReportSha256",Sha(out/"correction-report.json"));Number(At(c,"fit"),"sampleCount",6);String(At(At(m,"colorCorrection"),"report"),"sha256",Sha(out/"color-report.json"));}
                    CheckJpeg(out/"evaluation.jpg",40,32);const double after=PixelError(DecodedCrop(out/"evaluation.jpg"),expected);Require(after<1.5&&after*4<before,"final JPEG improves over fixed-only control using independent unshifted reference");
                    std::vector<std::string> current{Sha(out/"correction-report.json"),Sha(out/"evaluation.jpg"),Sha(out/"stitch-eval.manifest.json")};if(color)current.push_back(Sha(out/"color-report.json"));
                    if(repeat)Require(current==previous,"similarity reports final JPEG and manifest repeat byte-identically");else previous=current;
                    if(!repeat)std::cout<<"similarity_jpeg_error sign="<<sign<<" color="<<color<<" kernel="<<(kernel==L"bilinear"?"bilinear":"bicubic-catmull-rom")<<" rawMax="<<raw_error<<" cornerMax="<<corner_error<<" before="<<before<<" after="<<after<<'\n';
                    Evidence(evidence,out/"correction-report.json",tag+L"-report.json");Evidence(evidence,out/"stitch-eval.manifest.json",tag+L"-manifest.json");Evidence(evidence,out/"evaluation.jpg",tag+L".jpg");if(color)Evidence(evidence,out/"color-report.json",tag+L"-color.json");
                    if(!first)oracle<<',';first=false;
                    oracle<<"{\"sign\":"<<sign<<",\"color\":"<<(color?"true":"false")<<",\"resampling\":\""<<(kernel==L"bilinear"?"bilinear":"bicubic-catmull-rom")<<"\",\"repeat\":"<<repeat
                        <<",\"rawSimilarity\":["<<truth.a<<','<<truth.b<<','<<truth.dx<<','<<truth.dy<<','<<truth.cx<<','<<truth.cy<<"],\"cameraASha256\":\""<<a_hash<<"\",\"cameraBSha256\":\""<<(color?colored_hash:b_hash)
                        <<"\",\"geometryReportSha256\":\""<<current[0]<<"\",\"maximumCorrespondenceErrorRawPixels\":"<<raw_error<<",\"maximumCornerErrorRawPixels\":"<<corner_error<<",\"fixedOnlyMeanAbsoluteErrorDn\":"<<before<<",\"meanAbsoluteErrorDn\":"<<after<<'}';
                }
            }
        }
        for(const auto& bound:std::array<std::pair<std::string,std::string>,2>{{{"maximumRotationDegrees","rotation-exceeded"},{"maximumScaleDifferencePercent","scale-exceeded"}}}) {
            const auto suffix=std::wstring(bound.first.begin(),bound.first.end()),tag=L"similarity-"+label+L"-"+suffix;
            const auto policy=root/(suffix+L".json"),out=root/tag;WriteText(policy,Replace(job,"\""+bound.first+"\": 2","\""+bound.first+"\": 0.01"));
            auto args=Args(out,b,L"bilinear",true,false);Change(args,L"--correction-job",policy.native());const auto result=Run(eval,root,args);
            Require(result.exit==3&&result.error.empty()&&TrimLines(result.out)=="result=correction-rejected\nprofileStatus=draft\nquality=not-evaluated\nreport=correction-report.json","same JPEG with tighter pose budget is diagnostic-only");
            Require(!fs::exists(out/"evaluation.jpg")&&!fs::exists(out/"stitch-eval.manifest.json")&&!fs::exists(out/"color-report.json"),"pose-budget failure cannot publish a successful image or manifest");
            const auto r=Report(out,policy,false);const auto& fit=At(At(At(r,"matching"),"rawMapping"),"fit");String(fit,"outcome",bound.second);
            Require(!At(fit,"candidateAvailable").boolean&&At(fit,"violations").array.size()==1&&At(fit,"violations").array[0].string==bound.second,"only the independently tightened budget is violated");
            for(unsigned i=0;i<4;++i)Require(At(fit,"estimate").array[i].string==estimated_values[i],"rejected pose retains the exact same estimate without clamping or refitting");
            Evidence(evidence,out/"correction-report.json",tag+L"-report.json");
        }
        Require(Sha(b)==b_hash&&Sha(colored)==colored_hash,"similarity gray and color JPEGs remain immutable");
    }
    oracle<<"]}\n";WriteText(root/"oracle.json",oracle.str());Evidence(evidence,root/"oracle.json",L"similarity-oracle.json");
    Require(Sha(root/"CAM-A/original.jpg")==a_hash&&Sha(root/"draft.json")==profile_hash&&Sha(root/"job.json")==job_hash&&Sha(root/"color-job.json")==color_hash,"similarity operations preserve A profile and both policy files");
    Require(fs::is_empty(root/"product"),"similarity evaluation never writes product storage");
}
void ColorTests(const fs::path& fixtures,const fs::path& eval,const fs::path& root,const fs::path& evidence){
    const auto outputs=root/"color";fs::create_directory(outputs);
    const auto job=Text(fixtures.parent_path()/"stitch-correction/color-job.json");WriteText(root/"color-job.json",job);
    auto geometry_job=Text(root/"job.json");geometry_job=Replace(geometry_job,"\"gridColumns\": 5","\"gridColumns\": 16");geometry_job=Replace(geometry_job,"\"gridRows\": 4","\"gridRows\": 12");geometry_job=Replace(geometry_job,"\"maximumSeeds\": 6","\"maximumSeeds\": 32");geometry_job=Replace(geometry_job,"\"minimumSpacingMm\": 1","\"minimumSpacingMm\": 0.5");WriteText(root/"color-geometry-job.json",geometry_job);
    // High-precision JPEG is the coefficient oracle; .98 compression below is
    // retained as a genuine residual-limit rejection, not silently tolerated.
    const auto policy_path=root/"color-job.json",b_path=root/"colored/original.jpg";Synthetic(b_path,7360,4912,6);
    const std::array<fs::path,5> sources{root/"CAM-A/original.jpg",b_path,root/"draft.json",root/"color-geometry-job.json",policy_path};
    std::array<std::string,5> hashes;for(unsigned n=0;n<5;++n)hashes[n]=Sha(sources[n]);
    auto Args=[&](const fs::path& out,const std::wstring& kernel=L"bilinear"){
        auto args=Arguments(root,out,kernel);Change(args,L"--camera-b",b_path.native());Change(args,L"--right-um",L"575000");Change(args,L"--bottom-um",L"408000");Change(args,L"--width-pixels",L"40");Change(args,L"--height-pixels",L"32");args.insert(args.end(),{L"--correction-job",sources[3].native(),L"--color-job",policy_path.native()});return args;
    };
    auto Report=[&](const fs::path& out,const fs::path& policy,const std::string& outcome){
        const auto r=Parse(out/"color-report.json");Fields(r,{"schemaVersion","profileStatus","quality","productApproved","previewRequiresManifest","model","channelOrder","outcome","profileFileSha256","profileFingerprintSha256","jobSha256","geometryReportSha256","outputRaster","policy","inputs","points","fit"});
        String(r,"schemaVersion","a0.stitch-color-report.v1");String(r,"profileStatus","draft");String(r,"quality","not-evaluated");String(r,"model","encoded-dn-diagonal-affine");String(r,"channelOrder","BGR");String(r,"outcome",outcome);
        Require(At(r,"productApproved").kind==Kind::boolean&&!At(r,"productApproved").boolean&&At(r,"previewRequiresManifest").kind==Kind::boolean&&At(r,"previewRequiresManifest").boolean,"color report never approves product or standalone preview");
        String(r,"profileFileSha256",hashes[2]);String(r,"profileFingerprintSha256",Sha(root/"canonical.txt"));String(r,"jobSha256",Sha(policy));String(r,"geometryReportSha256",Sha(out/"correction-report.json"));
        Require(Text(out/"color-report.json").find(Text(policy))!=std::string::npos,"exact color policy snapshot");Require(Text(out/"color-report.json").find(root.string())==std::string::npos,"no absolute path in color report");
        const auto& pair=At(r,"inputs");Require(pair.array.size()==2,"color report input pair");for(unsigned n=0;n<2;++n){String(pair.array[n],"cameraAlias",n?"CAM-B":"CAM-A");String(pair.array[n],"sha256",hashes[n]);Number(pair.array[n],"encodedSizeBytes",fs::file_size(sources[n]));}
        const auto geometry=Parse(out/"correction-report.json");const auto& selection=At(geometry,"selection");const auto& indices=At(selection,"selectedIndices").array;const auto& points=At(r,"points").array;Require(points.size()==indices.size()&&points.size()==32,"color uses entire fixed geometry seed set");
        for(unsigned n=0;n<points.size();++n){const auto index=static_cast<std::size_t>(std::stoull(indices[n].string));const auto& p=points[n];String(p,"outcome","color-sampled");const auto& expected=At(At(selection,"candidates").array[index],"documentMm").array;const auto& actual=At(p,"documentMm").array;Require(Real(actual[0])==Real(expected[0])&&Real(actual[1])==Real(expected[1]),"color seed order and position preserved");Require(At(p,"cameraABgr").array.size()==3&&At(p,"cameraBBgr").array.size()==3,"all unmodified paired color observations retained");}
        return r;
    };
    for(const auto& kernel:std::array<std::wstring,2>{L"bilinear",L"bicubic-catmull-rom"}){
        const auto before_dir=outputs/(kernel+L"-geometry-only"),truth_dir=outputs/(kernel+L"-reference");auto geometry_args=Args(before_dir,kernel);geometry_args.resize(geometry_args.size()-2);
        auto truth_args=geometry_args;truth_args.resize(truth_args.size()-2);Change(truth_args,L"--camera-b",(root/"reference/original.jpg").native());Change(truth_args,L"--output-directory",truth_dir.native());
        Require(Run(eval,root,geometry_args).exit==0&&Run(eval,root,truth_args).exit==0,"color comparison reference and geometry-only actual JPEG renders");
        const auto expected_pixels=DecodedCrop(truth_dir/"evaluation.jpg");const double before=PixelError(DecodedCrop(before_dir/"evaluation.jpg"),expected_pixels);Require(before>5,"known color cast visible after geometry-only correction");
        std::array<std::string,4> previous;
        for(unsigned repeat=0;repeat<2;++repeat){const auto out=outputs/(kernel+L"-"+std::to_wstring(repeat));const auto result=Run(eval,root,Args(out,kernel));
            if(result.exit!=0&&fs::exists(out/"color-report.json")){const auto diagnostic=Parse(out/"color-report.json");const auto& fit=At(diagnostic,"fit");std::cerr<<"color_failure fit="<<At(fit,"outcome").string<<'\n';for(const auto& c:At(fit,"channels").array)if(c.kind==Kind::object&&At(c,"estimate").kind==Kind::object)std::cerr<<"gain="<<At(At(c,"estimate"),"gain").string<<" offset="<<At(At(c,"estimate"),"offsetDn").string<<" maxResidual="<<(At(c,"residuals").kind==Kind::object?At(At(c,"residuals"),"maximumDn").string:"unavailable")<<'\n';Evidence(evidence,out/"color-report.json",L"failed-color-report.json");}
            Require(result.exit==0&&result.error.empty()&&TrimLines(result.out)=="result=evaluated\nprofileStatus=draft\nquality=not-evaluated\nmanifest=stitch-eval.manifest.json","actual saved JPEG color correction succeeds: "+result.error+result.out);
            const auto r=Report(out,policy_path,"color-estimated");const auto& fit=At(r,"fit");String(fit,"outcome","color-estimated");Number(fit,"sampleCount",32);Require(At(fit,"candidateAvailable").boolean&&At(fit,"violations").array.empty(),"all color channels and bounds pass");const auto& channels=At(fit,"channels").array;Require(channels.size()==3,"three separate channel diagnostics");
            const std::array<double,3> gains{.8,4./3,2},offsets{-6.4,-16,-32};for(unsigned c=0;c<3;++c){const auto& estimate=At(channels[c],"estimate");Require(std::abs(Real(At(estimate,"gain"))-gains[c])<.08&&std::abs(Real(At(estimate,"offsetDn"))-offsets[c])<5,"independent known JPEG color coefficients recovered within codec tolerance; channel="+std::to_string(c)+" gain="+At(estimate,"gain").string+" offset="+At(estimate,"offsetDn").string);Require(Real(At(At(channels[c],"residuals"),"maximumDn"))<=6,"all observed color residuals within explicit limit");}
            Require(Sha(out/"correction-report.json")==Sha(before_dir/"correction-report.json"),"color option preserves geometry report bytes for same originals");
            const auto m=Parse(out/"stitch-eval.manifest.json");Fields(m,{"schemaVersion","profileStatus","quality","profileVersion","profileFingerprintSha256","resampling","outputRaster","inputs","output","correction","colorCorrection","engine"});String(m,"schemaVersion","a0.stitch-eval-manifest.v3");String(At(m,"engine"),"version","3.0.0");const auto& binding=At(m,"colorCorrection");Fields(binding,{"report","jobSha256"});String(binding,"jobSha256",hashes[4]);const auto& record=At(binding,"report");Fields(record,{"relativePath","sha256","encodedSizeBytes"});String(record,"relativePath","color-report.json");String(record,"sha256",Sha(out/"color-report.json"));Number(record,"encodedSizeBytes",fs::file_size(out/"color-report.json"));String(At(At(m,"correction"),"report"),"sha256",Sha(out/"correction-report.json"));String(At(m,"output"),"sha256",Sha(out/"evaluation.jpg"));
            unsigned entries=0;for(const auto& item:fs::directory_iterator(out)){++entries;Require(item.is_regular_file(),"color eval publishes regular leaves only");}Require(entries==4,"color eval has two reports JPEG and final manifest");CheckJpeg(out/"evaluation.jpg",40,32);
            const double after=PixelError(DecodedCrop(out/"evaluation.jpg"),expected_pixels);Require(after<before*.25,"published JPEG improves after actual color application");if(!repeat)std::cout<<"color_jpeg_error kernel="<<(kernel==L"bilinear"?"bilinear":"bicubic-catmull-rom")<<" before="<<before<<" after="<<after<<'\n';
            const std::array<std::string,4> now{Sha(out/"correction-report.json"),Sha(out/"color-report.json"),Sha(out/"evaluation.jpg"),Sha(out/"stitch-eval.manifest.json")};if(repeat)Require(now==previous,"color repeat produces byte-identical reports JPEG and manifest");else previous=now;
            const auto tag=L"color-"+kernel+L"-"+std::to_wstring(repeat);Evidence(evidence,out/"color-report.json",tag+L"-report.json");Evidence(evidence,out/"stitch-eval.manifest.json",tag+L"-manifest.json");Evidence(evidence,out/"evaluation.jpg",tag+L".jpg");Evidence(evidence,out/"correction-report.json",tag+L"-geometry.json");
        }
    }
    auto Refusal=[&](const std::string& name,const std::string& policy,const std::string& reason){const auto file=root/(name+"-color-job.json"),out=outputs/name;WriteText(file,policy);auto args=Args(out);Change(args,L"--color-job",file.native());const auto result=Run(eval,root,args);Require(result.exit==3&&result.error.empty()&&TrimLines(result.out)=="result=color-rejected\nprofileStatus=draft\nquality=not-evaluated\nreport=color-report.json","color rejection has stable distinct report-only result");const auto report=Report(out,file,"color-fit-rejected");const auto& fit=At(report,"fit");String(fit,"outcome",reason);Require(!At(fit,"candidateAvailable").boolean,"rejected color has no applicable coefficients");unsigned count=0;for(const auto& item:fs::directory_iterator(out)){++count;Require(item.path().filename()==L"correction-report.json"||item.path().filename()==L"color-report.json","rejected color cannot publish JPEG or final manifest");}Require(count==2,"rejected color retains only geometry and color diagnostics");Evidence(evidence,out/"color-report.json",L"color-"+std::wstring(name.begin(),name.end())+L"-report.json");return report;};
    const auto over=Refusal("overlimit",Replace(job,"\"maximumGainDifferencePercent\": 150","\"maximumGainDifferencePercent\": 0"),"color-gain-exceeded");Require(!At(At(over,"fit"),"violations").array.empty()&&At(At(At(over,"fit"),"channels").array[0],"estimate").kind==Kind::object,"over-limit estimate and violations retained");
    Refusal("low-contrast",Replace(job,"\"minimumStddevDn\": 2","\"minimumStddevDn\": 1000"),"insufficient-color-contrast");Refusal("few-samples",Replace(job,"\"minimumSamples\": 3","\"minimumSamples\": 33"),"insufficient-color-samples");Refusal("signal-range",Replace(job,"\"maximumSampleDn\": 255","\"maximumSampleDn\": 1"),"color-sample-range-rejected");
    {const auto noisy=root/"noisy-colored/original.jpg",out=outputs/"jpeg-residual";Synthetic(noisy,7360,4912,5);auto args=Args(out);Change(args,L"--camera-b",noisy.native());const auto result=Run(eval,root,args);Require(result.exit==3&&TrimLines(result.out)=="result=color-rejected\nprofileStatus=draft\nquality=not-evaluated\nreport=color-report.json"&&!fs::exists(out/"evaluation.jpg")&&!fs::exists(out/"stitch-eval.manifest.json"),"JPEG residual over explicit limit is refused");const auto r=Parse(out/"color-report.json");String(r,"outcome","color-fit-rejected");String(At(r,"fit"),"outcome","color-residual-exceeded");String(At(r,"inputs").array[1],"sha256",Sha(noisy));String(r,"geometryReportSha256",Sha(out/"correction-report.json"));Require(!At(At(r,"fit"),"candidateAvailable").boolean&&Real(At(At(At(At(r,"fit"),"channels").array[2],"residuals"),"maximumDn"))>6,"over-limit red residual preserved without clamping or dropping points");Number(At(r,"fit"),"sampleCount",32);Evidence(evidence,out/"color-report.json",L"color-jpeg-residual-report.json");}
    {const auto out=outputs/"geometry-rejected";auto args=Args(out);Change(args,L"--camera-b",(root/"blank/original.jpg").native());const auto result=Run(eval,root,args);Require(result.exit==3&&TrimLines(result.out)=="result=correction-rejected\nprofileStatus=draft\nquality=not-evaluated\nreport=correction-report.json"&&!fs::exists(out/"color-report.json")&&!fs::exists(out/"stitch-eval.manifest.json")&&!fs::exists(out/"evaluation.jpg"),"geometry rejection does not run color or publish success");}
    unsigned sequence=0;auto Bad=[&](const std::string& text,const char* stage="color-job"){const auto file=root/("bad-color-"+std::to_string(++sequence)+".json"),out=outputs/("bad-"+std::to_string(sequence));WriteText(file,text);auto args=Args(out);Change(args,L"--color-job",file.native());Reject(eval,root,args,stage,out);};
    Bad(Replace(job,"\"minimumSamples\": 3","\"minimumSamples\": 3.0"));Bad(Replace(job,"\"minimumSamples\": 3","\"minimumSamples\": 3e0"));Bad(Replace(job,"\"minimumSamples\": 3","\"minimumSamples\": 4294967296"));Bad(Replace(job,"\"minimumSamples\": 3,",""));Bad(Replace(job,"\"minimumSamples\": 3","\"minimumSamples\": 3, \"minimumSamples\": 3"));Bad(Replace(job,"\"minimumSamples\": 3","\"unknown\": 3"));Bad(Replace(job,"\"minimumSamples\": 3","\"minimumSamples\": 2"));Bad(Replace(job,"\"minimumSamples\": 3","\"minimumSamples\": 4097"));
    Bad(Replace(job,"\"minimumSampleDn\": 0","\"minimumSampleDn\": -1"));Bad(Replace(job,"\"maximumSampleDn\": 255","\"maximumSampleDn\": 256"));Bad(Replace(job,"\"minimumStddevDn\": 2","\"minimumStddevDn\": 0"));Bad(Replace(job,"\"maximumOffsetDn\": 80","\"maximumOffsetDn\": -1"));Bad(Replace(job,"\"maximumResidualDn\": 6","\"maximumResidualDn\": 1e999"));Bad(Replace(job,"a0.stitch-color-job.v1","a0.stitch-color-job.v2"));Bad(job+"{}");Bad(std::string(9,'[')+std::string(9,']'));Bad(std::string(65537,' '),"inputs");
    for(const auto& alias:{sources[0],sources[1],sources[2],sources[3]}){auto out=outputs/("alias-"+std::to_string(++sequence));auto args=Args(out);Change(args,L"--color-job",alias.native());Reject(eval,root,args,"inputs",out);}
    {auto out=outputs/"color-only";auto args=Arguments(root,out);args.insert(args.end(),{L"--color-job",policy_path.native()});Reject(eval,root,args,"arguments",out);}
    {auto out=outputs/"color-duplicate";auto args=Args(out);args.insert(args.end(),{L"--color-job",policy_path.native()});Reject(eval,root,args,"arguments",out);}
    {Require(CreateHardLinkW((root/"color-alias.json").c_str(),sources[3].c_str(),nullptr)!=0,"owned hardlink color fixture");auto out=outputs/"hardlink";auto args=Args(out);Change(args,L"--color-job",(root/"color-alias.json").native());Reject(eval,root,args,"inputs",out);}
    {Junction(root/"color-redirect",root);auto out=outputs/"reparse";auto args=Args(out);Change(args,L"--color-job",(root/"color-redirect/color-job.json").native());Reject(eval,root,args,"paths",out);}
    {Handle writer(CreateFileW(policy_path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));Require(writer.value!=INVALID_HANDLE_VALUE,"owned color job writer");auto out=outputs/"write-locked";Reject(eval,root,Args(out),"inputs",out);}
    {auto out=outputs/"occupied";fs::create_directory(out);WriteText(out/"color-report.json","preserve");Reject(eval,root,Args(out),"paths");Require(Text(out/"color-report.json")=="preserve","occupied color report untouched");}
    for(unsigned n=0;n<5;++n)Require(Sha(sources[n])==hashes[n],"color success and failure preserve all originals profile and jobs");Require(fs::is_empty(root/"product"),"color eval leaves product root empty");Evidence(evidence,policy_path,L"color-job.json");
}
void CorrectionTests(const fs::path& fixtures,const fs::path& eval,const fs::path& parent,const fs::path& evidence) {
    const auto root=parent/"correction";fs::create_directory(root);fs::create_directory(root/"product");
    WriteText(root/"draft.json",Replace(Text(fixtures/"calibrated-draft.json"),"9400.0","4800.0"));
    Require(Sha(fixtures/"calibrated-draft.canonical.txt")==TrimLines(Text(fixtures/"calibrated-draft.sha256")),"canonical bytes are independent fingerprint oracle");
    WriteText(root/"canonical.txt",Replace(Text(fixtures/"calibrated-draft.canonical.txt"),"/cameras/CAM-B/projection/documentToImage/1/2=f:40c25c0000000000","/cameras/CAM-B/projection/documentToImage/1/2=f:40b2c00000000000"));
    const auto fingerprint=Sha(root/"canonical.txt");
    const auto job=Text(fixtures.parent_path()/"stitch-correction/job.json");WriteText(root/"job.json",job);
    Synthetic(root/"CAM-A/original.jpg",7360,4912,2);Synthetic(root/"CAM-B/original.jpg",7360,4912,3);
    fs::create_directory(root/"reference");Write(root/"reference/original.jpg",Bytes(root/"CAM-A/original.jpg"));
    const std::array<fs::path,4> inputs{root/"CAM-A/original.jpg",root/"CAM-B/original.jpg",root/"draft.json",root/"job.json"};
    const std::array<std::string,4> hashes{Sha(inputs[0]),Sha(inputs[1]),Sha(inputs[2]),Sha(inputs[3])};
    auto Args=[&](const fs::path& out,const std::wstring& kernel=L"bilinear") {
        auto args=Arguments(root,out,kernel);Change(args,L"--right-um",L"575000");Change(args,L"--bottom-um",L"408000");Change(args,L"--width-pixels",L"40");Change(args,L"--height-pixels",L"32");args.insert(args.end(),{L"--correction-job",(root/"job.json").native()});return args;
    };
    auto Report=[&](const fs::path& output,const fs::path& policy,const std::string& outcome,const fs::path& b) {
        const auto report=Parse(output/"correction-report.json");
        Fields(report,{"schemaVersion","profileStatus","quality","productApproved","previewRequiresManifest","outcome","profileFileSha256","profileFingerprintSha256","jobSha256","outputRaster","policy","inputs","selection","matching"});
        String(report,"schemaVersion","a0.stitch-correction-report.v1");String(report,"profileStatus","draft");String(report,"quality","not-evaluated");String(report,"outcome",outcome);
        Require(At(report,"productApproved").kind==Kind::boolean&&!At(report,"productApproved").boolean&&At(report,"previewRequiresManifest").kind==Kind::boolean&&At(report,"previewRequiresManifest").boolean,"diagnostic cannot approve product or preview");
        String(report,"profileFileSha256",hashes[2]);String(report,"profileFingerprintSha256",fingerprint);String(report,"jobSha256",Sha(policy));
        Require(Text(output/"correction-report.json").find(Text(policy))!=std::string::npos,"exact parsed policy snapshot retained");
        const auto& pair=At(report,"inputs");Require(pair.kind==Kind::array&&pair.array.size()==2,"report ordered original pair");
        for(unsigned n=0;n<2;++n){const auto path=n==0?inputs[0]:b;Fields(pair.array[n],{"cameraAlias","sha256","encodedSizeBytes"});String(pair.array[n],"cameraAlias",n==0?"CAM-A":"CAM-B");String(pair.array[n],"sha256",Sha(path));Number(pair.array[n],"encodedSizeBytes",fs::file_size(path));}
        const auto& selection=At(report,"selection");Fields(selection,{"geometryOutcome","selectedIndices","candidates"});Require(At(selection,"candidates").array.size()==20,"all grid candidates retained");
        Require(Text(output/"correction-report.json").find(root.string())==std::string::npos,"report discloses no absolute input path");return report;
    };
    for(const auto& kernel:std::array<std::wstring,2>{L"bilinear",L"bicubic-catmull-rom"}) {
        const auto nominal=root/(kernel+L"-uncorrected"),truth=root/(kernel+L"-reference");auto uncorrected=Args(nominal,kernel);uncorrected.resize(uncorrected.size()-2);auto oracle=uncorrected;Change(oracle,L"--output-directory",truth.native());Change(oracle,L"--camera-b",(root/"reference/original.jpg").native());
        Require(Run(eval,root,uncorrected).exit==0&&Run(eval,root,oracle).exit==0,"uncorrected and independent unshifted JPEG oracle renders succeed");
        const auto expected_pixels=DecodedCrop(truth/"evaluation.jpg");const double before=PixelError(DecodedCrop(nominal/"evaluation.jpg"),expected_pixels);Require(before>5,"injected source displacement produces measurable image error");
        std::array<std::string,3> previous;
        for(unsigned repeat=0;repeat<2;++repeat){
            const auto output=root/(kernel+L"-"+std::to_wstring(repeat));const auto result=Run(eval,root,Args(output,kernel));
            Require(result.exit==0&&result.error.empty()&&TrimLines(result.out)=="result=evaluated\nprofileStatus=draft\nquality=not-evaluated\nmanifest=stitch-eval.manifest.json","actual auto-correction CLI succeeds; stderr="+result.error+" stdout="+result.out);
            const auto report=Report(output,inputs[3],"correction-estimated",inputs[1]);const auto& selection=At(report,"selection");Require(At(selection,"selectedIndices").array.size()==6,"automatic selection retains six seeds");
            const auto& matching=At(report,"matching");String(matching,"outcome","estimated");Require(At(matching,"points").array.size()==6,"all selected matching points recorded");
            for(const auto& point:At(matching,"points").array){String(point,"outcome","patch-matched");Require(At(point,"rawCorrespondence").array.size()==2&&At(point,"reverse").kind==Kind::object,"both directions and raw points retained");}
            const auto& raw=At(matching,"rawMapping");String(raw,"outcome","estimated");Require(At(raw,"points").array.size()==6,"all inverse mapping diagnostics recorded");const auto& fit=At(raw,"fit");String(fit,"outcome","correction-estimated");
            const auto& estimate=At(fit,"estimate").array;Require(estimate.size()==4,"similarity estimate has four parameters");
            // H maps document x to raw -y, document y to raw +x, at 8px/mm.
            const std::array<double,4> expected{1,0,.25,.375};for(unsigned n=0;n<4;++n)Require(std::abs(Real(estimate[n])-expected[n])<1e-8,"known stored-pixel translation recovered in document coordinates");
            Require(At(fit,"candidateAvailable").boolean&&At(fit,"violations").array.empty(),"bounded candidate returned without clamp");Number(At(fit,"metrics"),"pointCount",6);
            const auto manifest=Parse(output/"stitch-eval.manifest.json");Fields(manifest,{"schemaVersion","profileStatus","quality","profileVersion","profileFingerprintSha256","resampling","outputRaster","inputs","output","correction","engine"});
            String(manifest,"schemaVersion","a0.stitch-eval-manifest.v2");String(manifest,"profileStatus","draft");String(manifest,"quality","not-evaluated");String(manifest,"profileFingerprintSha256",fingerprint);String(At(manifest,"engine"),"version","2.0.0");
            const auto& binding=At(manifest,"correction");Fields(binding,{"report","jobSha256","profileFileSha256"});String(binding,"jobSha256",hashes[3]);String(binding,"profileFileSha256",hashes[2]);const auto& record=At(binding,"report");Fields(record,{"relativePath","sha256","encodedSizeBytes"});String(record,"relativePath","correction-report.json");String(record,"sha256",Sha(output/"correction-report.json"));Number(record,"encodedSizeBytes",fs::file_size(output/"correction-report.json"));String(At(manifest,"output"),"sha256",Sha(output/"evaluation.jpg"));Number(At(manifest,"outputRaster"),"widthPixels",40);Number(At(manifest,"outputRaster"),"heightPixels",32);
            Number(At(At(report,"outputRaster"),"regionUm"),"right",575000);Number(At(report,"outputRaster"),"widthPixels",40);Number(At(report,"outputRaster"),"heightPixels",32);
            unsigned entries=0;for(const auto& item:fs::directory_iterator(output)){++entries;Require(item.is_regular_file(),"corrected output regular leaf");}Require(entries==3,"report JPEG and final manifest only");CheckJpeg(output/"evaluation.jpg",40,32);
            const double after=PixelError(DecodedCrop(output/"evaluation.jpg"),expected_pixels);Require(after<before*.25,"published corrected JPEG improves against unshifted source oracle");
            if(!repeat)std::cout<<"correction_pixel_error kernel="<<(kernel==L"bilinear"?"bilinear":"bicubic-catmull-rom")<<" before="<<before<<" after="<<after<<'\n';
            std::array<std::string,3> current{Sha(output/"correction-report.json"),Sha(output/"evaluation.jpg"),Sha(output/"stitch-eval.manifest.json")};if(repeat)Require(current==previous,"repeat correction report JPEG and manifest bytes agree");else previous=current;
            const auto tag=L"correction-"+kernel+L"-"+std::to_wstring(repeat);Evidence(evidence,output/"correction-report.json",tag+L"-report.json");Evidence(evidence,output/"stitch-eval.manifest.json",tag+L"-manifest.json");Evidence(evidence,output/"evaluation.jpg",tag+L".jpg");
        }
    }
    auto Rejected=[&](const std::string& name,std::vector<std::wstring> args,const fs::path& policy,const std::string& outcome,const fs::path& b) {
        const auto output=root/name;Change(args,L"--output-directory",output.native());auto result=Run(eval,root,args);
        Require(result.exit==3&&result.error.empty()&&TrimLines(result.out)=="result=correction-rejected\nprofileStatus=draft\nquality=not-evaluated\nreport=correction-report.json","numerical refusal returns distinct diagnostic-only exit");
        unsigned entries=0;for(const auto& item:fs::directory_iterator(output)){++entries;Require(item.path().filename()==L"correction-report.json","numerical rejection publishes only diagnostic");}Require(entries==1,"rejected correction cannot publish evaluation image or success manifest");
        auto report=Report(output,policy,outcome,b);Evidence(evidence,output/"correction-report.json",L"correction-"+std::wstring(name.begin(),name.end())+L"-report.json");return report;
    };
    WriteText(root/"overlimit.json",Replace(job,"\"maximumDisplacementOutputPixels\": 20","\"maximumDisplacementOutputPixels\": 0"));
    {auto args=Args(root/"unused");Change(args,L"--correction-job",(root/"overlimit.json").native());const auto report=Rejected("overlimit",args,root/"overlimit.json","matching-or-correction-rejected",inputs[1]);const auto& fit=At(At(At(report,"matching"),"rawMapping"),"fit");Require(!At(fit,"candidateAvailable").boolean&&!At(fit,"violations").array.empty()&&At(fit,"estimate").kind==Kind::array&&Real(At(At(fit,"metrics"),"maximumDisplacementOutputPixels"))>1,"over-limit estimate and measurements retained without candidate");}
    {auto args=Arguments(root,root/"unused");args.insert(args.end(),{L"--correction-job",inputs[3].native()});const auto report=Rejected("no-support",args,inputs[3],"selection-rejected",inputs[1]);Require(At(report,"matching").kind==Kind::null_value,"selection failure never starts matching");}
    Synthetic(root/"blank/original.jpg",7360,4912,4);
    {auto args=Args(root/"unused");Change(args,L"--camera-b",(root/"blank/original.jpg").native());const auto report=Rejected("blank-target",args,inputs[3],"matching-or-correction-rejected",root/"blank/original.jpg");const auto& matching=At(report,"matching");String(matching,"outcome","matching-failed");Require(At(matching,"points").array.size()==6&&At(matching,"rawMapping").kind==Kind::null_value,"no failed seed is silently dropped or reselected");}
    unsigned n=0;auto BadJob=[&](const std::string& text,const char* stage="correction-job") {const auto policy=root/("bad-job-"+std::to_string(++n)+".json"),output=root/("invalid-"+std::to_string(n));WriteText(policy,text);auto args=Args(output);Change(args,L"--correction-job",policy.native());Reject(eval,root,args,stage,output);};
    BadJob(Replace(job,"\"gridColumns\": 5","\"gridColumns\": 5.0"));BadJob(Replace(job,"\"gridRows\": 4","\"gridRows\": 4e0"));BadJob(Replace(job,"\"maximumSeeds\": 6","\"maximumSeeds\": 4294967296"));BadJob(Replace(job,"\"gridColumns\": 5","\"gridColumns\": 0"));BadJob(Replace(job,"\"gridRows\": 4","\"gridRows\": 65"));
    BadJob(Replace(job,"\"patchRadiusSamples\": 3","\"patchRadiusSamples\": 9"));BadJob(Replace(job,"\"searchRadiusSteps\": 4","\"searchRadiusSteps\": 33"));BadJob(Replace(job,"\"minimumCorrelation\": 0.9","\"minimumCorrelation\": 1.1"));BadJob(Replace(job,"\"minimumGradientAxisRatio\": 0.02","\"minimumGradientAxisRatio\": 0"));BadJob(Replace(job,"\"maximumRoundTripErrorPixels\": 1e-9","\"maximumRoundTripErrorPixels\": 1e999"));BadJob(Replace(job,"\"minimumPoints\": 3","\"minimumPoints\": 2"));BadJob(Replace(job,"\"maximumRotationDegrees\": 2","\"maximumRotationDegrees\": 181"));BadJob(Replace(job,"\"maximumScaleDifferencePercent\": 2","\"maximumScaleDifferencePercent\": -1"));
    BadJob(Replace(job,"\"gridColumns\": 5","\"gridColumns\": 5, \"gridColumns\": 5"));BadJob(Replace(job,"\"gridColumns\": 5","\"unknown\": 5"));BadJob(Replace(job,"a0.stitch-correction-job.v1","a0.stitch-correction-job.v2"));BadJob(job+"{}");BadJob(std::string(9,'[')+std::string(9,']'));BadJob(std::string(65537,' '),"inputs");
    BadJob(Replace(job,"\"gridColumns\": 5,",""));BadJob(Replace(job,"\"patchSpacingMm\": 0.125","\"patchSpacingMm\": null"));BadJob(Replace(job,"\"maximumNewtonUpdates\": 40","\"maximumNewtonUpdates\": 0"));BadJob(Replace(job,"\"maximumSeeds\": 6","\"maximumSeeds\": 257"));BadJob(Replace(job,"\"minimumPoints\": 3","\"minimumPoints\": -0"));
    {const auto occupied=root/"occupied";fs::create_directory(occupied);WriteText(occupied/"correction-report.json","preserve diagnostic");auto args=Args(occupied);Reject(eval,root,args,"paths");Require(Text(occupied/"correction-report.json")=="preserve diagnostic","occupied correction report is not replaced");}
    {Handle writer(CreateFileW(inputs[3].c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));Require(writer.value!=INVALID_HANDLE_VALUE,"owned job write-lock fixture");Reject(eval,root,Args(root/"locked-job"),"inputs",root/"locked-job");}
    {auto args=Args(root/"alias-refused");Change(args,L"--correction-job",inputs[2].native());Reject(eval,root,args,"inputs",root/"alias-refused");}
    {Require(CreateHardLinkW((root/"job-alias.json").c_str(),inputs[0].c_str(),nullptr)!=0,"owned correction job hardlink fixture");auto args=Args(root/"hardlink-refused");Change(args,L"--correction-job",(root/"job-alias.json").native());Reject(eval,root,args,"inputs",root/"hardlink-refused");}
    {Junction(root/"job-redirect",root);auto args=Args(root/"reparse-refused");Change(args,L"--correction-job",(root/"job-redirect/job.json").native());Reject(eval,root,args,"paths",root/"reparse-refused");}
    for(unsigned i=0;i<4;++i)Require(Sha(inputs[i])==hashes[i],"correction success and refusal preserve input snapshots");Require(fs::is_empty(root/"product"),"correction never writes product root");
    ColorTests(fixtures,eval,root,evidence);
    Evidence(evidence,inputs[2],L"correction-synthetic-profile.json");Evidence(evidence,inputs[3],L"correction-job.json");
}
void Tests(const fs::path& fixtures,const fs::path& eval,const fs::path& product,const fs::path& root,const fs::path& evidence) {
    fs::create_directory(root/"product");const auto draft=Text(fixtures/"calibrated-draft.json");WriteText(root/"draft.json",draft);const std::string golden="f133057b1e9cbd0ad76574e56f10ff3f3a20069cdae56fa737ebed84d9f62113";
    Require(TrimLines(Text(fixtures/"calibrated-draft.sha256"))==golden,"independent golden draft SHA vector");
    Synthetic(root/"CAM-A/original.jpg",7360,4912,0);Synthetic(root/"CAM-B/original.jpg",7360,4912,1);const auto a=Sha(root/"CAM-A/original.jpg"),b=Sha(root/"CAM-B/original.jpg"),profile=Sha(root/"draft.json");
    // Focused fail-before-publication gap probe precedes any success fixture.
    auto bad=Arguments(root,root/"lexical-gap");Change(bad,L"--width-pixels",L"4.0");Reject(eval,root,bad,"arguments",root/"lexical-gap");
    for(const auto& kernel:std::array<std::wstring,2>{L"bilinear",L"bicubic-catmull-rom"}) {
        const char* kernel_text=kernel==L"bilinear"?"bilinear":"bicubic-catmull-rom";
        std::string expected_hash,expected_manifest;for(unsigned repeat=0;repeat<2;++repeat){auto directory=root/(kernel+L"-"+std::to_wstring(repeat));auto result=Run(eval,root,Arguments(root,directory,kernel));Require(result.exit==0&&result.error.empty()&&TrimLines(result.out)=="result=evaluated\nprofileStatus=draft\nquality=not-evaluated\nmanifest=stitch-eval.manifest.json","real eval success stdout exact");CheckJpeg(directory/"evaluation.jpg");Manifest(root,directory,kernel_text,golden);Evidence(evidence,directory/"stitch-eval.manifest.json",L"stitch-eval-"+kernel+L"-"+std::to_wstring(repeat)+L".json");if(repeat==0){expected_hash=Sha(directory/"evaluation.jpg");expected_manifest=Sha(directory/"stitch-eval.manifest.json");}else Require(Sha(directory/"evaluation.jpg")==expected_hash&&Sha(directory/"stitch-eval.manifest.json")==expected_manifest,"repeat same kernel is byte-identical JPEG and manifest");}
    }
    unsigned sequence=0;auto New=[&]{return root/("rejected-"+std::to_string(++sequence));};
    auto Option=[&](const std::wstring& key,const std::wstring& value,const char* stage){auto output=New();auto args=Arguments(root,output);Change(args,key,value);Reject(eval,root,args,stage,output);};
    for(const auto& value:std::array<std::wstring,6>{L"4.0",L"4e0",L"+4",L"04",L"-4",L"18446744073709551616"})Option(L"--width-pixels",value,"arguments");
    Option(L"--width-pixels",L"5","arguments");Option(L"--resampling",L"nearest","arguments");
    {auto output=New();auto args=Arguments(root,output);args.resize(args.size()-2);Reject(eval,root,args,"arguments",output);}
    {auto output=New();auto args=Arguments(root,output);args.insert(args.end(),{L"--dpi",L"100"});Reject(eval,root,args,"arguments",output);}
    {auto output=New();auto args=Arguments(root,output);args.insert(args.end(),{L"--unknown",L"value"});Reject(eval,root,args,"arguments",output);}
    for(const auto& fixture:std::array<const char*,2>{"approved.json","template.json"})Option(L"--profile-file",(fixtures/fixture).native(),"profile");
    auto owner=draft;const auto null_block=owner.find("\"outputRaster\": null");Require(null_block!=std::string::npos,"owner mutation fixture found");owner.replace(null_block,std::strlen("\"outputRaster\": null"),"\"outputRaster\": {}");WriteText(root/"owner.json",owner);Option(L"--profile-file",(root/"owner.json").native(),"profile");WriteText(root/"malformed.json","{not json}");Option(L"--profile-file",(root/"malformed.json").native(),"profile");
    Option(L"--output-directory",(root/"product").native(),"paths");Option(L"--output-directory",(root/"product/inside").native(),"paths");Option(L"--output-directory",root.native(),"paths");
    std::wstring upper=(root/"product/CASE").native();std::transform(upper.begin(),upper.end(),upper.begin(),[](wchar_t c){return static_cast<wchar_t>(towupper(c));});Option(L"--output-directory",upper,"paths");
    // An ordinary prefix sibling is allowed: comparison must use components.
    {auto output=root/"product-sibling";auto result=Run(eval,root,Arguments(root,output));Require(result.exit==0,"component-prefix sibling allowed");Manifest(root,output,"bilinear",golden);}
    std::wstring short_path(32768,L'\0');DWORD short_size=GetShortPathNameW((root/"product").c_str(),short_path.data(),static_cast<DWORD>(short_path.size()));if(short_size&&short_size<short_path.size()){short_path.resize(short_size);if(short_path!=(root/"product").native())Option(L"--output-directory",short_path+L"\\short-alias-child","paths");}
    // A malformed owned profile prevents publication even if the protection
    // regresses: without the guard this reaches profile-failed, not output.
    // The unique protected child is never created, deleted, or used as a fixture.
    const auto actual_product=ActualProductRoot();if(fs::is_directory(actual_product)) {
        const auto absent_child=actual_product/(L"a0-stitch-eval-denied-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        Require(!fs::exists(absent_child),"protected unique child precondition absent");
        auto args=Arguments(root,absent_child);Change(args,L"--profile-file",(root/"malformed.json").native());auto result=Run(eval,root,args,(root/"spoof-localappdata").native());
        Require(result.exit==2&&result.out.empty()&&TrimLines(result.error)=="error=paths-failed"&&!fs::exists(absent_child),"actual protected child rejected before malformed profile with spoofed LOCALAPPDATA");
    }
    fs::create_directory(root/"alias-target");Junction(root/"redirect",root/"alias-target");Option(L"--output-directory",(root/"redirect/new").native(),"paths");Junction(root/"input-redirect",root/"CAM-A");Option(L"--camera-a",(root/"input-redirect/original.jpg").native(),"paths");
    auto occupied=root/"occupied";fs::create_directory(occupied);Require(CreateHardLinkW((occupied/"evaluation.jpg").c_str(),(root/"CAM-A/original.jpg").c_str(),nullptr)!=0,"owned existing output hardlink fixture");auto occupied_args=Arguments(root,occupied);Reject(eval,root,occupied_args,"paths");Require(Sha(occupied/"evaluation.jpg")==a,"occupied hardlinked output is preserved");
    fs::create_directory(root/"invalid-inputs");WriteText(root/"invalid-inputs/not-jpeg.jpg","not a JPEG");Option(L"--camera-a",(root/"invalid-inputs/not-jpeg.jpg").native(),"jpeg");auto truncated=Bytes(root/"CAM-A/original.jpg");truncated.resize(truncated.size()/2);Write(root/"invalid-inputs/truncated.jpg",truncated);Option(L"--camera-a",(root/"invalid-inputs/truncated.jpg").native(),"jpeg");Synthetic(root/"invalid-inputs/small.jpg",16,12,0);Option(L"--camera-a",(root/"invalid-inputs/small.jpg").native(),"jpeg");
    Option(L"--camera-b",(root/"CAM-A/original.jpg").native(),"inputs");
    {auto output=New();auto args=Arguments(root,output);Change(args,L"--left-um",L"1180000");Change(args,L"--right-um",L"1181000");Change(args,L"--top-um",L"0");Change(args,L"--bottom-um",L"1000");Reject(eval,root,args,"render",output);}
    const auto product_job=New();std::wstring hash(golden.begin(),golden.end());auto refused=Run(product,root,{L"stitch-v2",L"--camera-a",(root/"CAM-A/original.jpg").native(),L"--camera-b",(root/"CAM-B/original.jpg").native(),L"--job-directory",product_job.native(),L"--profile-file",(root/"draft.json").native(),L"--expected-profile-sha256",hash,L"--assessed-at",L"2026-10-09T00:00:00Z",L"--resampling",L"bilinear",L"--stitch-job-id",L"11111111111111111111111111111111",L"--capture-transaction-id",L"22222222222222222222222222222222",L"--completed-at",L"2026-10-09T00:00:00Z"});Require(refused.exit==2&&refused.out.empty()&&refused.error.find("approved calibrated profile required for use")!=std::string::npos&&!fs::exists(product_job),"actual product adapter still rejects calibrated draft");
    Require(Sha(root/"CAM-A/original.jpg")==a&&Sha(root/"CAM-B/original.jpg")==b&&Sha(root/"draft.json")==profile,"all success/failure attempts preserve both originals and draft");Require(fs::is_empty(root/"product"),"product root untouched");
    CorrectionTests(fixtures,eval,root,evidence);
    RefinementTests(fixtures,eval,root,evidence);
    SimilarityJpegTests(fixtures,eval,root,evidence);
}
}
int wmain(int argc,wchar_t* argv[]) {
    const bool refinement_only=argc==6&&std::wstring_view(argv[5])==L"--refinement-only";
    const bool similarity_only=argc==6&&std::wstring_view(argv[5])==L"--similarity-only";
    if(argc!=4&&argc!=5&&!refinement_only&&!similarity_only){std::cerr<<"fixture-dir eval-exe product-adapter-exe [existing-owned-evidence-directory [--refinement-only|--similarity-only]] required\n";return 2;}
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    int result=0;try{Temp temp;const auto evidence=argc>=5?fs::absolute(argv[4]):fs::path{};if(refinement_only)RefinementTests(fs::absolute(argv[1]),fs::absolute(argv[2]),temp.root,evidence);else if(similarity_only)SimilarityJpegTests(fs::absolute(argv[1]),fs::absolute(argv[2]),temp.root,evidence);else Tests(fs::absolute(argv[1]),fs::absolute(argv[2]),fs::absolute(argv[3]),temp.root,evidence);std::cout<<"stitch_eval checks="<<checks<<" failures=0\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';result=1;}CoUninitialize();return result;
}
