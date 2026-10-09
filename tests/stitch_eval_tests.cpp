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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
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
void Synthetic(const fs::path& path,unsigned width,unsigned height,unsigned camera) {
    fs::create_directories(path.parent_path());IWICImagingFactory* raw{};Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&raw)));Com<IWICImagingFactory> factory(raw);
    IWICStream* sr{};Hr(factory->CreateStream(&sr));Com<IWICStream> stream(sr);Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    IWICBitmapEncoder* er{};Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&er));Com<IWICBitmapEncoder> encoder(er);Hr(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
    IWICBitmapFrameEncode* fr{};IPropertyBag2* br{};Hr(encoder->CreateNewFrame(&fr,&br));Com<IWICBitmapFrameEncode> frame(fr);Com<IPropertyBag2> bag(br);
    Hr(frame->Initialize(bag.get()));Hr(frame->SetSize(width,height));WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format));Require(format==GUID_WICPixelFormat24bppBGR,"source JPEG BGR format");
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width)*3);
    for(unsigned y=0;y<height;++y){for(unsigned x=0;x<width;++x){row[x*3]=static_cast<std::uint8_t>(30+(x/3+y/7+camera*19)%180);row[x*3+1]=static_cast<std::uint8_t>(40+(x/5+y/3+camera*37)%170);row[x*3+2]=static_cast<std::uint8_t>(50+(x/7+y/5+camera*53)%160);}Hr(frame->WritePixels(1,width*3,static_cast<UINT>(row.size()),row.data()));}
    Hr(frame->Commit());Hr(encoder->Commit());
}
void CheckJpeg(const fs::path& path) {
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
    UINT frames{};Hr(decoder->GetFrameCount(&frames));Require(frames==1,"output one decoded frame");IWICBitmapFrameDecode* fr{};Hr(decoder->GetFrame(0,&fr));Com<IWICBitmapFrameDecode> frame(fr);UINT width{},height{};Hr(frame->GetSize(&width,&height));Require(width==4&&height==4,"actual JPEG decoded dimensions");
    IWICFormatConverter* cr{};Hr(factory->CreateFormatConverter(&cr));Com<IWICFormatConverter> converter(cr);Hr(converter->Initialize(frame.get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));std::array<BYTE,48> pixels{};Hr(converter->CopyPixels(nullptr,12,static_cast<UINT>(pixels.size()),pixels.data()));
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
}
}
int wmain(int argc,wchar_t* argv[]) {
    if(argc!=4&&argc!=5){std::cerr<<"fixture-dir eval-exe product-adapter-exe [existing-owned-evidence-directory] required\n";return 2;}
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    int result=0;try{Temp temp;Tests(fs::absolute(argv[1]),fs::absolute(argv[2]),fs::absolute(argv[3]),temp.root,argc==5?fs::absolute(argv[4]):fs::path{});std::cout<<"stitch_eval checks="<<checks<<" failures=0\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';result=1;}CoUninitialize();return result;
}
