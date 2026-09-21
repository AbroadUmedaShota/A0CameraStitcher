// Software-only two-process IPC proof. Child mode never imports Nikon SDK code.
#include "a0/phase0/dual_live_worker_poc.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace a0::phase0;
namespace {
int failures = 0;
void Check(bool value, std::string_view text) { if (!value) { ++failures; std::cerr << "FAIL: " << text << '\n'; } }

std::wstring PipePath(std::string_view name) { return L"\\\\.\\pipe\\" + std::wstring(name.begin(), name.end()); }
std::array<unsigned char, 4> Header(std::uint32_t size) { return {static_cast<unsigned char>(size),static_cast<unsigned char>(size>>8U),static_cast<unsigned char>(size>>16U),static_cast<unsigned char>(size>>24U)}; }
std::uint32_t Parse(const std::array<unsigned char,4>& h) { return std::uint32_t(h[0]) | (std::uint32_t(h[1])<<8U) | (std::uint32_t(h[2])<<16U) | (std::uint32_t(h[3])<<24U); }
bool WriteAll(HANDLE h, const void* p, std::size_t n) { DWORD w{}; return WriteFile(h,p,static_cast<DWORD>(n),&w,nullptr) && w==n; }
bool ReadAll(HANDLE h, void* p, std::size_t n) { DWORD r{}; return ReadFile(h,p,static_cast<DWORD>(n),&r,nullptr) && r==n; }

int Worker(std::string_view pipe_name, std::string_view worker, std::string_view expected_generation, std::string_view expected_token, std::uintptr_t controller_handle) {
    // A worker cannot be activated by a command line alone: it needs the
    // controller-created inherited event. This is a test-only ownership gate.
    const HANDLE controller_ready = reinterpret_cast<HANDLE>(controller_handle);
    if (controller_ready == nullptr || WaitForSingleObject(controller_ready, 0) != WAIT_OBJECT_0) return 1;
    const HANDLE pipe = CreateNamedPipeW(PipePath(pipe_name).c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT, 1, 4096, 4096, 5000, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 2;
    const BOOL connected = ConnectNamedPipe(pipe, nullptr);
    if (!connected && GetLastError()!=ERROR_PIPE_CONNECTED) { CloseHandle(pipe); return 3; }
    std::array<unsigned char,4> header{};
    if (!ReadAll(pipe,header.data(),header.size())) { CloseHandle(pipe); return 4; }
    std::string request(Parse(header),'\0');
    if (!ReadAll(pipe,request.data(),request.size())) { CloseHandle(pipe); return 5; }
    const std::string expected = std::string(expected_generation)+"|"+std::string(expected_token)+"|frame";
    const std::string response = request==expected
        ? std::string(worker)+"|"+std::string(expected_generation)+"|1|frame"
        : "REJECT";
    const auto out = Header(static_cast<std::uint32_t>(response.size()));
    const bool ok = WriteAll(pipe,out.data(),out.size()) && WriteAll(pipe,response.data(),response.size());
    FlushFileBuffers(pipe); DisconnectNamedPipe(pipe); CloseHandle(pipe);
    return ok ? 0 : 6;
}

std::string Unique(std::string_view label) { static std::atomic<unsigned long long> s{}; return "A0.DualLivePoc."+std::string(label)+"."+std::to_string(GetCurrentProcessId())+"."+std::to_string(++s); }
HANDLE StartChild(const std::string& pipe, const std::string& worker, const std::string& generation, const std::string& token, HANDLE controller_ready) {
    std::array<wchar_t,32768> path{}; GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    std::wstring command=L"\""+std::wstring(path.data())+L"\" --dual-live-worker "+std::wstring(pipe.begin(),pipe.end())+L" "+std::wstring(worker.begin(),worker.end())+L" "+std::wstring(generation.begin(),generation.end())+L" "+std::wstring(token.begin(),token.end())+L" "+std::to_wstring(reinterpret_cast<std::uintptr_t>(controller_ready));
    STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) return nullptr;
    CloseHandle(pi.hThread); return pi.hProcess;
}
DWORD StartUnownedChildExitCode(const std::string& pipe, const std::string& worker, const std::string& generation, const std::string& token) {
    std::array<wchar_t,32768> path{}; GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    std::wstring command=L"\""+std::wstring(path.data())+L"\" --dual-live-worker "+std::wstring(pipe.begin(),pipe.end())+L" "+std::wstring(worker.begin(),worker.end())+L" "+std::wstring(generation.begin(),generation.end())+L" "+std::wstring(token.begin(),token.end())+L" 0";
    STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) return 99;
    CloseHandle(pi.hThread); WaitForSingleObject(pi.hProcess,5000); DWORD exit_code{}; GetExitCodeProcess(pi.hProcess,&exit_code); CloseHandle(pi.hProcess); return exit_code;
}
std::string Request(const std::string& pipe, const std::string& payload) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3); HANDLE h=INVALID_HANDLE_VALUE;
    while(std::chrono::steady_clock::now()<deadline) { h=CreateFileW(PipePath(pipe).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr); if(h!=INVALID_HANDLE_VALUE) break; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    if(h==INVALID_HANDLE_VALUE) return {};
    const auto head=Header(static_cast<std::uint32_t>(payload.size())); if(!WriteAll(h,head.data(),head.size())||!WriteAll(h,payload.data(),payload.size())) {CloseHandle(h);return{};}
    std::array<unsigned char,4> out{}; if(!ReadAll(h,out.data(),out.size())){CloseHandle(h);return{};} std::string response(Parse(out),'\0');
    if(!ReadAll(h,response.data(),response.size())) response.clear(); CloseHandle(h); return response;
}
void WaitChild(HANDLE p) { const auto status=WaitForSingleObject(p,5000); Check(status==WAIT_OBJECT_0,"worker child must exit after one IPC response"); CloseHandle(p); }

void TestTwoProcessIpcAndCoordinator() {
    const std::string generation="g-1", token="cap-1", a=Unique("a"), b=Unique("b");
    Check(StartUnownedChildExitCode(Unique("unowned"),"CAM-A",generation,token)==1,"worker launched without controller ownership handle must reject before opening IPC");
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE controller_ready=CreateEventW(&inheritable,TRUE,TRUE,nullptr);
    Check(controller_ready!=nullptr,"controller must create an inheritable worker ownership gate");
    HANDLE pa=StartChild(a,"CAM-A",generation,token,controller_ready), pb=StartChild(b,"CAM-B",generation,token,controller_ready);
    Check(pa&&pb,"two independent worker processes must start");
    const auto ra=Request(a,generation+"|"+token+"|frame"), rb=Request(b,generation+"|"+token+"|frame"); WaitChild(pa); WaitChild(pb); CloseHandle(controller_ready);
    Check(ra=="CAM-A|g-1|1|frame"&&rb=="CAM-B|g-1|1|frame","both workers must return machine-readable frame envelopes over separate IPC pipes");
    DualLiveWorkerPocCoordinator c(generation,token);
    Check(c.AcceptFrame("CAM-A",generation,token,1,{1,2}),"A current worker frame must be accepted");
    Check(c.AcceptFrame("CAM-B",generation,token,1,{3,4}),"B current worker frame must be accepted");
    Check(!c.AcceptFrame("CAM-A","old",token,2,{9}),"old generation frame must be rejected");
    Check(!c.AcceptFrame("CAM-A",generation,"wrong",2,{9}),"wrong capability token must be rejected");
    Check(!c.AcceptFrame("CAM-A",generation,token,1,{9}),"older/equal frame must not replace newest frame");
    c.ReportWorkerExited("CAM-B");
    Check(!c.CanBeginNewOperation()&&!c.AcceptFrame("CAM-A",generation,token,2,{9}),"one worker exit must block further live operations without reconnect");
    Check(!c.CanBeginCaptureOrWpd(),"two-process live POC must never authorize capture or WPD handoff");
    DualLiveWorkerPocCoordinator parent_loss(generation,token);
    parent_loss.ReportControllerExited();
    Check(!parent_loss.CanBeginNewOperation(),"controller termination must block operations without worker restart");
}
void TestOpenBeforeFilter() {
    const std::vector<std::uint32_t> ids{71,83}; std::vector<std::uint32_t> opened;
    const auto selected=SelectAssignedSourceBeforeOpen(ids,83);
    if(selected) opened.push_back(*selected); // fake trace: only the returned plan is opened.
    Check(opened==std::vector<std::uint32_t>{83},"worker-local assigned filter must cause zero non-assigned Source opens");
    Check(!SelectAssignedSourceBeforeOpen(ids,99),"missing assignment must fail before any Source open");
    Check(!SelectAssignedSourceBeforeOpen({83,83},83),"ambiguous assignment must fail before any Source open");
}
}
int main(int argc,char** argv) {
    if(argc==7&&std::string_view(argv[1])=="--dual-live-worker") return Worker(argv[2],argv[3],argv[4],argv[5],static_cast<std::uintptr_t>(std::strtoull(argv[6],nullptr,10)));
    TestTwoProcessIpcAndCoordinator(); TestOpenBeforeFilter();
    std::cout << (failures==0?"PASS":"FAIL") << " dual live worker POC\n"; return failures==0?0:1;
}
