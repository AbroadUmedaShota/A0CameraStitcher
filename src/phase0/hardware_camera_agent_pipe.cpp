#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "a0/phase0/hardware_camera_agent.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace a0::phase0 {
namespace {

constexpr std::uint32_t kMaximumPipeFrameBytes = 1024U * 1024U;
constexpr DWORD kAcceptTimeoutMs = 15000;
constexpr DWORD kFrameReadTimeoutMs = 5000;
constexpr DWORD kResponseWriteTimeoutMs = 1000;

class CurrentLogonPipeSecurity final {
public:
    CurrentLogonPipeSecurity() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            throw std::runtime_error("hardware Camera Agent could not inspect its logon token");
        }
        DWORD required = 0;
        (void)GetTokenInformation(token, TokenGroups, nullptr, 0, &required);
        if (required == 0) {
            CloseHandle(token);
            throw std::runtime_error("hardware Camera Agent logon token has no group information");
        }
        token_groups_.resize(required);
        if (!GetTokenInformation(
                token, TokenGroups, token_groups_.data(), required, &required)) {
            CloseHandle(token);
            throw std::runtime_error("hardware Camera Agent could not read its logon token groups");
        }
        CloseHandle(token);

        const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(token_groups_.data());
        PSID logon_sid = nullptr;
        for (DWORD index = 0; index < groups->GroupCount; ++index) {
            if ((groups->Groups[index].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
                logon_sid = groups->Groups[index].Sid;
                break;
            }
        }
        if (logon_sid == nullptr || !IsValidSid(logon_sid)) {
            throw std::runtime_error(
                "hardware Camera Agent requires an interactive logon-session SID");
        }

        const DWORD acl_size = static_cast<DWORD>(
            sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(logon_sid));
        acl_.resize(acl_size);
        auto* acl = reinterpret_cast<ACL*>(acl_.data());
        if (!InitializeAcl(acl, acl_size, ACL_REVISION) ||
            !AddAccessAllowedAceEx(
                acl,
                ACL_REVISION,
                0,
                GENERIC_ALL,
                logon_sid)) {
            throw std::runtime_error("hardware Camera Agent could not create its logon-only pipe ACL");
        }
        if (!InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&descriptor_, TRUE, acl, FALSE) ||
            !SetSecurityDescriptorControl(
                &descriptor_, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
            throw std::runtime_error("hardware Camera Agent could not protect its pipe DACL");
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
    }

    SECURITY_ATTRIBUTES* Attributes() noexcept { return &attributes_; }

private:
    std::vector<unsigned char> token_groups_;
    std::vector<unsigned char> acl_;
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
};

bool IsSafePipeName(std::string_view value) noexcept {
    if (value.empty() || value.size() > 120) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 ||
            character == '.' || character == '-' || character == '_';
    });
}

bool TransferOverlapped(
    HANDLE pipe,
    void* buffer,
    DWORD bytes,
    bool write,
    DWORD timeout_ms,
    DWORD& transferred) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return false;
    const BOOL started = write
        ? WriteFile(pipe, buffer, bytes, &transferred, &overlapped)
        : ReadFile(pipe, buffer, bytes, &transferred, &overlapped);
    if (!started) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            CloseHandle(overlapped.hEvent);
            return false;
        }
        const DWORD wait = WaitForSingleObject(overlapped.hEvent, timeout_ms);
        if (wait != WAIT_OBJECT_0) {
            (void)CancelIoEx(pipe, &overlapped);
            (void)WaitForSingleObject(overlapped.hEvent, INFINITE);
            CloseHandle(overlapped.hEvent);
            return false;
        }
        if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
            CloseHandle(overlapped.hEvent);
            return false;
        }
    }
    CloseHandle(overlapped.hEvent);
    return transferred > 0;
}

bool ReadExact(HANDLE pipe, void* destination, std::size_t bytes, DWORD timeout_ms) {
    auto* output = static_cast<unsigned char*>(destination);
    std::size_t offset = 0;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (offset < bytes) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD remaining_timeout = static_cast<DWORD>(std::min<ULONGLONG>(
            deadline - now,
            static_cast<ULONGLONG>(std::numeric_limits<DWORD>::max())));
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD read = 0;
        if (!TransferOverlapped(
                pipe, output + offset, chunk, false, remaining_timeout, read)) return false;
        offset += read;
    }
    return true;
}

bool WriteExact(HANDLE pipe, const void* source, std::size_t bytes, DWORD timeout_ms) {
    const auto* input = static_cast<const unsigned char*>(source);
    std::size_t offset = 0;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (offset < bytes) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD remaining_timeout = static_cast<DWORD>(std::min<ULONGLONG>(
            deadline - now,
            static_cast<ULONGLONG>(std::numeric_limits<DWORD>::max())));
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!TransferOverlapped(
                pipe,
                const_cast<unsigned char*>(input + offset),
                chunk,
                true,
                remaining_timeout,
                written)) return false;
        offset += written;
    }
    return true;
}

bool ProcessOneConnection(HANDLE pipe, HardwareCameraAgentDispatcher& dispatcher) {
    std::array<unsigned char, 4> header{};
    if (!ReadExact(pipe, header.data(), header.size(), kFrameReadTimeoutMs)) return false;
    const std::uint32_t length =
        static_cast<std::uint32_t>(header[0]) |
        (static_cast<std::uint32_t>(header[1]) << 8U) |
        (static_cast<std::uint32_t>(header[2]) << 16U) |
        (static_cast<std::uint32_t>(header[3]) << 24U);
    if (length == 0 || length > kMaximumPipeFrameBytes) return false;

    std::string request(length, '\0');
    if (!ReadExact(pipe, request.data(), request.size(), kFrameReadTimeoutMs)) return false;

    // Dispatch is deliberately completed before attempting to write. In
    // serve-once capture mode, a WPF/client disconnect therefore cannot abort
    // the camera transaction or trigger another shutter command.
    const std::string response = dispatcher.Handle(request);
    if (response.empty() || response.size() > kMaximumPipeFrameBytes) return false;
    const std::uint32_t response_length = static_cast<std::uint32_t>(response.size());
    const std::array<unsigned char, 4> response_header{
        static_cast<unsigned char>(response_length & 0xFFU),
        static_cast<unsigned char>((response_length >> 8U) & 0xFFU),
        static_cast<unsigned char>((response_length >> 16U) & 0xFFU),
        static_cast<unsigned char>((response_length >> 24U) & 0xFFU),
    };
    if (!WriteExact(
            pipe, response_header.data(), response_header.size(), kResponseWriteTimeoutMs)) return true;
    (void)WriteExact(pipe, response.data(), response.size(), kResponseWriteTimeoutMs);
    return true;
}

} // namespace

int RunHardwareCameraAgentNamedPipeServer(
    std::string_view pipe_name,
    HardwareCameraAgentDispatcher& dispatcher,
    bool serve_once) {
    if (!IsSafePipeName(pipe_name)) {
        throw std::invalid_argument("hardware Camera Agent pipe name is invalid");
    }
    const std::wstring full_name =
        L"\\\\.\\pipe\\" + std::wstring(pipe_name.begin(), pipe_name.end());
    CurrentLogonPipeSecurity security;

    for (;;) {
        const HANDLE pipe = CreateNamedPipeW(
            full_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            kMaximumPipeFrameBytes,
            kMaximumPipeFrameBytes,
            0,
            security.Attributes());
        if (pipe == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("hardware Camera Agent named pipe creation failed");
        }

        OVERLAPPED connect_overlapped{};
        connect_overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (connect_overlapped.hEvent == nullptr) {
            CloseHandle(pipe);
            if (serve_once) return 2;
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, &connect_overlapped);
        const DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
        bool connection_ready = connected != FALSE || connect_error == ERROR_PIPE_CONNECTED;
        if (!connection_ready && connect_error == ERROR_IO_PENDING) {
            const DWORD wait = WaitForSingleObject(connect_overlapped.hEvent, kAcceptTimeoutMs);
            if (wait == WAIT_OBJECT_0) {
                DWORD ignored = 0;
                connection_ready =
                    GetOverlappedResult(pipe, &connect_overlapped, &ignored, FALSE) != FALSE;
            } else {
                (void)CancelIoEx(pipe, &connect_overlapped);
                (void)WaitForSingleObject(connect_overlapped.hEvent, INFINITE);
            }
        }
        CloseHandle(connect_overlapped.hEvent);
        if (!connection_ready) {
            CloseHandle(pipe);
            if (serve_once) return 2;
            continue;
        }

        const bool request_processed = ProcessOneConnection(pipe, dispatcher);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        if (serve_once) return request_processed ? 0 : 2;
    }
}

} // namespace a0::phase0
