#pragma once
// Software-only IPC proof. No Nikon SDK or WPD dependency.
#include "a0/phase0/dual_live_worker_poc.hpp"
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace a0::phase0;
namespace {
constexpr DWORD kTimeout = 1500;
// Steady-state IPC (an already-connected worker answering a request) stays on
// kTimeout. A wait that can include OS process creation/scheduling for a
// freshly spawned child -- connecting to it, reaping its exit, or a worker's
// very first read before the controller has finished spawning/connecting its
// sibling -- uses this larger budget instead. Only the specific call sites
// documented at their use need it; this must never become the default for
// ordinary request/response I/O.
constexpr DWORD kSpawnTimeout = 15000;
constexpr std::size_t kMaxWire = 1024;
HANDLE cleanup_job = nullptr; // Outer runner owns this; never inherited.
int failures = 0;

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (valid()) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};
void Check(bool ok, std::string_view message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void Require(bool ok, std::string_view message) {
    if (!ok) throw std::runtime_error(std::string(message));
}
bool Number(std::string_view text, std::uint64_t& result) {
    if (text.empty() || text.front() == '0') return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && result > 0;
}
std::vector<std::string> Split(std::string_view text) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const auto end = text.find('|', start);
        fields.emplace_back(text.substr(start, end == text.npos ? end : end - start));
        if (end == text.npos) return fields;
        start = end + 1;
    }
}
std::string Unique() {
    static std::atomic<unsigned> counter{};
    return "A0.Poc." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(++counter);
}
std::wstring Wide(std::string_view text) { return {text.begin(), text.end()}; }
std::wstring PipePath(std::string_view name) { return L"\\\\.\\pipe\\" + Wide(name); }
std::wstring Exe() {
    std::array<wchar_t, 32768> path{};
    Require(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())) != 0, "exe path");
    return path.data();
}
std::array<unsigned char, 4> Header(std::uint32_t size) {
    return {static_cast<unsigned char>(size), static_cast<unsigned char>(size >> 8U),
            static_cast<unsigned char>(size >> 16U), static_cast<unsigned char>(size >> 24U)};
}
std::uint32_t Size(const std::array<unsigned char, 4>& h) {
    return h[0] | (std::uint32_t(h[1]) << 8U) | (std::uint32_t(h[2]) << 16U) | (std::uint32_t(h[3]) << 24U);
}

// Never return while pending I/O references stack memory. If cancellation of
// this local test pipe cannot converge, exit this fake-only process. The outer
// job cleans up all children. Hardware processes never use this helper.
void CancelAndDrain(HANDLE pipe, OVERLAPPED& pending) {
    CancelIoEx(pipe, &pending);
    if (WaitForSingleObject(pending.hEvent, kTimeout) != WAIT_OBJECT_0) {
        TerminateProcess(GetCurrentProcess(), 80);
        std::terminate();
    }
    DWORD ignored{};
    GetOverlappedResult(pipe, &pending, &ignored, FALSE);
}
bool Complete(HANDLE pipe, OVERLAPPED& pending, HANDLE parent, DWORD& transferred, DWORD timeout = kTimeout) {
    HANDLE waits[]{pending.hEvent, parent};
    const DWORD wait = WaitForMultipleObjects(parent ? 2U : 1U, waits, FALSE, timeout);
    if (wait != WAIT_OBJECT_0) { CancelAndDrain(pipe, pending); return false; }
    return GetOverlappedResult(pipe, &pending, &transferred, FALSE) != FALSE;
}
bool Io(HANDLE pipe, void* data, DWORD length, bool write, HANDLE parent = nullptr, DWORD timeout = kTimeout) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) return false;
    OVERLAPPED pending{};
    pending.hEvent = event.value;
    DWORD done{};
    const BOOL immediate = write ? WriteFile(pipe, data, length, &done, &pending)
                                 : ReadFile(pipe, data, length, &done, &pending);
    if (immediate) return done == length;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    return Complete(pipe, pending, parent, done, timeout) && done == length;
}
bool ReadMessage(HANDLE pipe, std::string& result, HANDLE parent = nullptr, DWORD timeout = kTimeout) {
    std::array<unsigned char, 4> header{};
    if (!Io(pipe, header.data(), 4, false, parent, timeout)) return false;
    const auto size = Size(header);
    if (size == 0 || size > kMaxWire) return false; // Before allocation.
    result.assign(size, '\0');
    return Io(pipe, result.data(), size, false, parent, timeout);
}
bool WriteMessage(HANDLE pipe, const std::string& message, HANDLE parent = nullptr) {
    if (message.empty() || message.size() > kMaxWire) return false;
    auto header = Header(static_cast<std::uint32_t>(message.size()));
    return Io(pipe, header.data(), 4, true, parent) &&
           Io(pipe, const_cast<char*>(message.data()), static_cast<DWORD>(message.size()), true, parent);
}
HANDLE CreateServer(const std::string& name) {
    return CreateNamedPipeW(PipePath(name).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, static_cast<DWORD>(kMaxWire), static_cast<DWORD>(kMaxWire), kTimeout, nullptr);
}
// Every real caller connects right after spawning the peer process (Start()),
// so this wait inherently includes OS process creation/scheduling and
// defaults to the spawn budget, not the steady-state one.
bool Connect(HANDLE pipe, DWORD timeout = kSpawnTimeout) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) return false;
    OVERLAPPED pending{};
    pending.hEvent = event.value;
    if (ConnectNamedPipe(pipe, &pending)) return true;
    const auto error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    DWORD ignored{};
    return Complete(pipe, pending, nullptr, ignored, timeout);
}
HANDLE Start(const std::wstring& arguments, HANDLE inherited_parent) {
    SIZE_T size{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> memory(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
    Require(InitializeProcThreadAttributeList(attributes, 1, 0, &size) != FALSE, "attributes");
    HANDLE handles[]{inherited_parent};
    const bool restricted = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles, sizeof(handles), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    auto command = L"\"" + Exe() + L"\" " + arguments;
    const bool created = restricted && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
        &startup.StartupInfo, &process) != FALSE;
    DeleteProcThreadAttributeList(attributes);
    Require(created, "child creation");
    if (cleanup_job && !AssignProcessToJobObject(cleanup_job, process.hProcess)) {
        TerminateProcess(process.hProcess, 81);
        WaitForSingleObject(process.hProcess, kTimeout);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        throw std::runtime_error("test cleanup job assignment");
    }
    const DWORD resumed = ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    if (resumed == static_cast<DWORD>(-1)) {
        TerminateProcess(process.hProcess, 81);
        WaitForSingleObject(process.hProcess, kTimeout);
        CloseHandle(process.hProcess);
        throw std::runtime_error("child resume");
    }
    return process.hProcess;
}
DWORD ExitCode(HANDLE process) {
    DWORD code = STILL_ACTIVE;
    Require(GetExitCodeProcess(process, &code) != FALSE, "read exit code");
    return code;
}
void Reap(HANDLE process) {
    if (!process) return;
    // A just-spawned or just-stopped child's exit can be slow to observe under
    // load, so this uses the spawn budget rather than the steady-state one.
    if (WaitForSingleObject(process, kSpawnTimeout) != WAIT_OBJECT_0) {
        TerminateProcess(process, 82); // Only isolated test-owned fake processes.
        WaitForSingleObject(process, kSpawnTimeout);
    }
}

} // namespace

