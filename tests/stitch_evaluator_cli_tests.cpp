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
        auto base=fs::temp_directory_path(); root=base/("a0-independent-evaluator-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
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
void Change(std::vector<std::wstring>& args,const std::wstring& key,const std::wstring& value) {
    auto found=std::find(args.begin(),args.end(),key);if(found==args.end() || ++found==args.end())throw std::runtime_error("test option missing");*found=value;
}
void Reject(const fs::path& exe,const fs::path& root,const std::vector<std::wstring>& args,const std::string& stage,const fs::path& no_output={}) {
    auto result=Run(exe,root,args);
    Require(result.exit==2 && result.out.empty() && TrimLines(result.error)=="error="+stage+"-failed","specific CLI refusal "+stage+"; actual="+result.error);
    if(!no_output.empty()) Require(!fs::exists(no_output),"refusal does not create output");
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

std::vector<std::wstring> Arguments(const fs::path& root,const fs::path& output) {
    return {L"--image",(root/"inputs/image.jpg").native(),L"--ground-truth",(root/"inputs/ground-truth.json").native(),
        L"--plan-file",(root/"inputs/plan.json").native(),L"--thresholds-file",(root/"inputs/thresholds.json").native(),
        L"--validity-file",(root/"inputs/validity.json").native(),L"--product-root",(root/"product").native(),
        L"--output-directory",output.native()};
}
// Analytic fixture is independent of the generator and renderer. Integer
// symmetric dots and rings have known centers before lossy JPEG encoding.
void Jpeg(const fs::path& path,unsigned damage) {
    std::vector<BYTE> pixels(256*256*3,230);
    auto pixel=[&](int x,int y,BYTE value){for(unsigned c=0;c<3;++c)pixels[(y*256+x)*3+c]=value;};
    for(int row=0;row<3;++row)for(int col=0;col<3;++col){
        int cx=48+col*80,cy=48+row*80;if(damage==1)cx+=2;if(damage==3)cx+=(col-1)*4;
        if(damage==4&&row==0&&col==0)continue;
        for(int dy=-10;dy<=10;++dy)for(int dx=-10;dx<=10;++dx){double r2=dx*dx+dy*dy;
            if(r2<=6.25||(r2>=36&&r2<=100))pixel(cx+dx,cy+dy,0);
        }
    }
    for(int y=224;y<232;++y)for(int x=112;x<120;++x)pixel(x,y,128);
    for(int y=224;y<232;++y)for(int x=144;x<152;++x)pixel(x,y,damage==2?176:128);
    IWICImagingFactory* raw{};Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&raw)));Com<IWICImagingFactory> factory(raw);
    IWICStream* sr{};Hr(factory->CreateStream(&sr));Com<IWICStream> stream(sr);Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    IWICBitmapEncoder* er{};Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&er));Com<IWICBitmapEncoder> encoder(er);Hr(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
    IWICBitmapFrameEncode* fr{};IPropertyBag2* br{};Hr(encoder->CreateNewFrame(&fr,&br));Com<IWICBitmapFrameEncode> frame(fr);Com<IPropertyBag2> bag(br);
    PROPBAG2 option{};option.pstrName=const_cast<wchar_t*>(L"ImageQuality");VARIANT quality{};quality.vt=VT_R4;quality.fltVal=1;Hr(bag->Write(1,&option,&quality));
    Hr(frame->Initialize(bag.get()));Hr(frame->SetSize(256,256));WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format));Require(format==GUID_WICPixelFormat24bppBGR,"JPEG fixture BGR");
    Hr(frame->WritePixels(256,768,static_cast<UINT>(pixels.size()),pixels.data()));Hr(frame->Commit());Hr(encoder->Commit());
}
void Bind(const fs::path& inputs,bool hole=false) {
    const std::string prefix="P5\n256 256\n1\n";std::vector<std::uint8_t> mask(prefix.begin(),prefix.end());mask.resize(prefix.size()+256*256,1);
    if(hole)for(unsigned y=40;y<48;++y)for(unsigned x=40;x<48;++x)mask[prefix.size()+y*256+x]=0;
    Write(inputs/"validity.pgm",mask);
    WriteText(inputs/"validity.json","{\"schemaVersion\":\"a0.stitch-output-validity.v1\",\"imageSha256\":\""+Sha(inputs/"image.jpg")+"\",\"maskSha256\":\""+Sha(inputs/"validity.pgm")+"\",\"widthPixels\":256,\"heightPixels\":256,\"maskRelativePath\":\"validity.pgm\"}");
}
struct Failure { [[noreturn]] static void Fail(std::string_view,std::string_view){throw std::runtime_error("test JSON parse failure");} };
std::string Dump(const Json& value) {
    if(value.kind==Kind::null_value)return "null";if(value.kind==Kind::number)return value.string;if(value.kind==Kind::boolean)return value.boolean?"true":"false";
    if(value.kind==Kind::string)return "\""+a0::common::protocol_json::JsonEscape(value.string)+"\"";
    std::string out=value.kind==Kind::object?"{":"[";bool first=true;
    if(value.kind==Kind::object)for(const auto& [key,leaf]:value.object){if(!first)out+=',';first=false;out+="\""+a0::common::protocol_json::JsonEscape(key)+"\":"+Dump(leaf);}
    else for(const auto& leaf:value.array){if(!first)out+=',';first=false;out+=Dump(leaf);}return out+(value.kind==Kind::object?"}":"]");
}
Json Report(const fs::path& directory,const fs::path& inputs,const std::string& assessment) {
    auto report=a0::common::protocol_json::BasicJsonParser<Failure>(Text(directory/"stitch-evaluation.report.json")).Parse();
    Require(report.object.at("schemaVersion").string=="a0.stitch-evaluation-report.v1"&&report.object.at("quality").string=="not-evaluated","independent unevaluated report schema");
    Require(report.object.at("thresholdAssessment").string==assessment,"report assessment matches expected");
    const auto& hashes=report.object.at("sourceHashes").object;
    for(const auto& pair:std::array<std::pair<const char*,const char*>,6>{{{"image","image.jpg"},{"groundTruth","ground-truth.json"},{"plan","plan.json"},{"thresholds","thresholds.json"},{"validity","validity.json"},{"mask","validity.pgm"}}})
        Require(hashes.at(pair.first).string==Sha(inputs/pair.second),"report binds actual source bytes");
    Require(report.object.at("metrics").array.size()==8,"all eight metrics emitted");
    unsigned entries=0;for(const auto& item:fs::directory_iterator(directory)){++entries;Require(item.is_regular_file(),"output only regular report");}Require(entries==1,"output exactly one report");
    auto encoded=Text(directory/"stitch-evaluation.report.json");Require(encoded.find("original.jpg")==std::string::npos&&encoded.find(inputs.string())==std::string::npos,"report contains no original names or local absolute paths");
    return report;
}
bool Code(const Json& report,const char* code){for(const auto& value:report.object.at("diagnosticCodes").array)if(value.string==code)return true;return false;}
void Tests(const fs::path& fixtures,const fs::path& eval,const fs::path& generator,const fs::path& root,const fs::path& evidence) {
    auto inputs=root/"inputs";fs::create_directory(inputs);fs::create_directory(root/"product");
    for(const auto* leaf:{"ground-truth.json","plan.json","thresholds.json"})WriteText(inputs/leaf,Text(fixtures/leaf));
    Jpeg(inputs/"image.jpg",0);Bind(inputs);
    auto pristine=Sha(inputs/"image.jpg");auto gt=Sha(inputs/"ground-truth.json"),plan=Sha(inputs/"plan.json"),thresholds=Sha(inputs/"thresholds.json");
    unsigned sequence=0;auto New=[&]{return root/("result-"+std::to_string(++sequence));};
    auto Record=[&](const fs::path& output,const std::string& assessment){auto result=Run(eval,root,Arguments(root,output));Require(result.exit==0&&result.error.empty()&&TrimLines(result.out)=="result=recorded\nquality=not-evaluated\nthresholdAssessment="+assessment+"\nreport=stitch-evaluation.report.json","actual JPEG evaluator stdout exact");return Report(output,inputs,assessment);};
    std::string first;for(unsigned n=0;n<2;++n){auto output=New();auto report=Record(output,"thresholds-met");Require(report.object.at("diagnosticCodes").array.empty(),"pristine compressed analytical image has no diagnostics");if(!n)first=Text(output/"stitch-evaluation.report.json");else Require(first==Text(output/"stitch-evaluation.report.json"),"repeat report byte identical");Evidence(evidence,output/"stitch-evaluation.report.json",L"pristine-"+std::to_wstring(n)+L".json");}
    for(unsigned damage=1;damage<=4;++damage){Jpeg(inputs/"image.jpg",damage);Bind(inputs);auto output=New();auto report=Record(output,"thresholds-not-met");
        if(damage==1)Require(Code(report,"fiducial-position-out-of-range")&&!Code(report,"local-dpi-out-of-range"),"translation detected without global registration, preserves DPI");
        if(damage==2)Require(Code(report,"seam-luminance-out-of-range")&&Code(report,"seam-color-out-of-range"),"exposure step detected in declared equal-content patches");
        if(damage==3)Require(Code(report,"local-dpi-out-of-range"),"local stretch detected");
        if(damage==4)Require(Code(report,"fiducial-coverage-below-minimum"),"missing marker remains coverage denominator");
        Evidence(evidence,output/"stitch-evaluation.report.json",L"damage-"+std::to_wstring(damage)+L".json");}
    Jpeg(inputs/"image.jpg",0);Bind(inputs,true);{auto output=New();auto report=Record(output,"thresholds-not-met");Require(Code(report,"pixel-coverage-below-minimum")&&Code(report,"fiducial-coverage-below-minimum"),"bound actual mask hole affects both denominators");}
    Bind(inputs);WriteText(inputs/"plan.json",Text(fixtures/"plan.json"));
    auto Option=[&](const std::wstring& key,const std::wstring& value,const char* stage){auto output=New();auto args=Arguments(root,output);Change(args,key,value);Reject(eval,root,args,stage,output);};
    {auto output=New();auto args=Arguments(root,output);args.resize(args.size()-2);Reject(eval,root,args,"arguments",output);}
    {auto output=New();auto args=Arguments(root,output);args[0]=L"--unknown";Reject(eval,root,args,"arguments",output);}
    Option(L"--output-directory",(inputs/"forbidden").native(),"paths");Option(L"--output-directory",(root/"product/forbidden").native(),"paths");
    Option(L"--output-directory",root.native(),"paths");Option(L"--image",L"inputs/image.jpg","paths");Option(L"--image",(inputs/"image.jpg:stream").native(),"paths");
    auto occupied=New();fs::create_directory(occupied);WriteText(occupied/"stitch-evaluation.report.json","preserve");Option(L"--output-directory",occupied.native(),"paths");Require(Text(occupied/"stitch-evaluation.report.json")=="preserve","preexisting output preserved");
    fs::create_directory(root/"alias-target");Junction(root/"redirect",root/"alias-target");Option(L"--output-directory",(root/"redirect/new").native(),"paths");Junction(root/"input-redirect",inputs);Option(L"--image",(root/"input-redirect/image.jpg").native(),"paths");
    WriteText(inputs/"bad.jpg","not jpeg");Option(L"--image",(inputs/"bad.jpg").native(),"jpeg");
    auto truncated=Bytes(inputs/"image.jpg");truncated.resize(truncated.size()/2);Write(inputs/"truncated.jpg",truncated);Option(L"--image",(inputs/"truncated.jpg").native(),"jpeg");
    WriteText(inputs/"bad.json","{}");Option(L"--plan-file",(inputs/"bad.json").native(),"contracts");Option(L"--ground-truth",(inputs/"plan.json").native(),"inputs");
    WriteText(inputs/"bad-utf8.json","{\"x\":\"\xc0\xaf\"}");Option(L"--thresholds-file",(inputs/"bad-utf8.json").native(),"contracts");
    auto validity=Text(inputs/"validity.json");auto mismatch=validity;auto hash=mismatch.find(Sha(inputs/"image.jpg"));Require(hash!=std::string::npos,"hash mutation site");mismatch[hash]=mismatch[hash]=='0'?'1':'0';WriteText(inputs/"validity.json",mismatch);{auto output=New();Reject(eval,root,Arguments(root,output),"validity",output);}WriteText(inputs/"validity.json",validity);
    const auto mask=Bytes(inputs/"validity.pgm");
    for(unsigned fault=0;fault<3;++fault){auto malformed=mask;if(!fault)malformed.back()=2;else if(fault==1)malformed.push_back(1);else{const std::string header="P5\n256 256\n255\n";malformed.assign(header.begin(),header.end());malformed.resize(header.size()+256*256,1);}Write(inputs/"validity.pgm",malformed);auto mutated=validity;
        const auto key=validity.find("\"maskSha256\":\"");Require(key!=std::string::npos,"mask hash mutation site");mutated.replace(key+14,64,Sha(inputs/"validity.pgm"));WriteText(inputs/"validity.json",mutated);auto output=New();Reject(eval,root,Arguments(root,output),"validity",output);}
    Bind(inputs);
    {auto bad=Text(fixtures/"plan.json");const auto begin=bad.find("\"fiducials\": 3");Require(begin!=std::string::npos,"minimum mutation site");bad.replace(begin,std::strlen("\"fiducials\": 3"),"\"fiducials\": 10");WriteText(inputs/"plan.json",bad);auto output=New();auto report=Record(output,"incomplete-measurements");Require(Code(report,"insufficient-valid-measurements"),"insufficient minimum still records numerical coverage and diagnostic");Evidence(evidence,output/"stitch-evaluation.report.json",L"incomplete.json");WriteText(inputs/"plan.json",Text(fixtures/"plan.json"));}
    const auto protectedRoot=ActualProductRoot();if(fs::is_directory(protectedRoot)){
        auto absent=protectedRoot/(L"a0-evaluator-denied-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));Require(!fs::exists(absent),"protected unique child absent");auto args=Arguments(root,absent);Change(args,L"--plan-file",(inputs/"bad.json").native());auto result=Run(eval,root,args,(root/"spoof-localappdata").native());Require(result.exit==2&&result.out.empty()&&TrimLines(result.error)=="error=paths-failed"&&!fs::exists(absent),"real KnownFolder protected before malformed plan with spoofed LOCALAPPDATA");}
    Require(Sha(inputs/"image.jpg")==pristine&&Sha(inputs/"ground-truth.json")==gt&&Sha(inputs/"plan.json")==plan&&Sha(inputs/"thresholds.json")==thresholds,"original independent fixture bytes preserved through all runs");Require(fs::is_empty(root/"product"),"product root remains empty");
    // Actual producer interoperability is process-only. Its compatible /2
    // document annotation must locate points without sharing projection code.
    auto truth=a0::common::protocol_json::BasicJsonParser<Failure>(Text(fixtures/"ground-truth.json")).Parse();
    auto spec=truth.object.at("spec");spec.object.at("image").object.at("width_px").string="256";spec.object.at("image").object.at("height_px").string="256";
    WriteText(root/"producer-spec.json",Dump(spec));auto generated=root/"generated";
    auto generation=Run(generator,root,{L"--spec",(root/"producer-spec.json").native(),L"--output",generated.native(),L"--threads",L"2"});
    Require(generation.exit==0&&generation.error.empty()&&generation.out.find("result=generated-synthetic-pair")!=std::string::npos,"actual generator emits annotated synthetic pair");
    const auto cameraA=Sha(generated/"CAM-A/original.jpg"),cameraB=Sha(generated/"CAM-B/original.jpg"),annotation=Sha(generated/"ground-truth.json");
    Write(inputs/"image.jpg",Bytes(generated/"CAM-A/original.jpg"));WriteText(inputs/"ground-truth.json",Text(generated/"ground-truth.json"));Bind(inputs);
    auto output=New();auto report=Record(output,"thresholds-met");Require(report.object.at("diagnosticCodes").array.empty(),"actual producer identity-raster JPEG matches independent document oracle");
    Evidence(evidence,output/"stitch-evaluation.report.json",L"producer.json");
    Require(Sha(generated/"CAM-A/original.jpg")==cameraA&&Sha(generated/"CAM-B/original.jpg")==cameraB&&Sha(generated/"ground-truth.json")==annotation,"both actual generated originals and annotation remain unchanged");
}
}
int wmain(int argc,wchar_t* argv[]) {
    if(argc!=4&&argc!=5){std::cerr<<"fixture-dir evaluator-exe generator-exe [existing-owned-evidence-directory] required\n";return 2;}
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    int result=0;try{Temp temp;Tests(fs::absolute(argv[1]),fs::absolute(argv[2]),fs::absolute(argv[3]),temp.root,argc==5?fs::absolute(argv[4]):fs::path{});std::cout<<"stitch_evaluator_cli checks="<<checks<<" failures=0\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';result=1;}CoUninitialize();return result;
}
