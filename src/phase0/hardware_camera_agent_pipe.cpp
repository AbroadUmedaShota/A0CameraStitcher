#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "a0/phase0/hardware_camera_agent.hpp"
#include "a0/phase0/dual_hardware_camera_agent.hpp"
#include "a0/phase0/dual_binding_camera_agent.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// This translation unit hosts ONE generic Windows named-pipe server loop
// (RunNamedPipeServerLoop) shared by both the Single-camera host
// (RunHardwareCameraAgentNamedPipeServer, declared in
// hardware_camera_agent.hpp) and the Dual-camera host
// (RunDualHardwareCameraAgentNamedPipeServer, declared in
// dual_hardware_camera_agent.hpp). Framing, the current-logon pipe security
// descriptor, and the bounded response delivery-ACK contract are all
// dispatcher-independent and therefore live here exactly once. The two
// dispatcher types are unrelated (no shared base class) and are connected
// only structurally, via the template's use of dispatcher.Handle(),
// dispatcher.OnIdle(), and dispatcher.ShouldStop().
//
// The binding host (RunDualBindingCameraAgentNamedPipeServer, declared in
// dual_binding_camera_agent.hpp, GitHub Issue #61) is the third caller and was
// added the same way: a wrapper, not a copy. Copying the loop is how the hosts
// would drift into disagreeing about what an oversize frame or a missing
// acknowledgment means.
namespace a0::phase0 {
namespace {

constexpr std::uint32_t kMaximumPipeFrameBytes = 1024U * 1024U;
constexpr DWORD kAcceptTimeoutMs = 15000;
constexpr DWORD kFrameReadTimeoutMs = 5000;
constexpr DWORD kResponseWriteTimeoutMs = 1000;
constexpr unsigned char kDeliveryAcknowledgment = 0x06U;
constexpr int kFailedBeforeDispatchExitCode = 2;
constexpr int kDispatchedDeliveryFailureExitCode = 3;

enum class ProcessOneConnectionOutcome {
    failed_before_dispatch,
    dispatched_delivery_failed,
    complete_delivery,
};

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

// Dispatcher and FailureInjection are duck-typed rather than sharing a base
// class: Dispatcher only needs Handle(std::string_view) -> std::string, and
// FailureInjection only needs the transport-failure bool members read below. This lets
// HardwareCameraAgentDispatcher and DualHardwareCameraAgentDispatcher (two
// unrelated final classes with independently evolving protocols) share this
// transport-level function without either one depending on the other.
template <typename Dispatcher, typename FailureInjection>
ProcessOneConnectionOutcome ProcessOneConnection(
    HANDLE pipe,
    Dispatcher& dispatcher,
    const FailureInjection& failure_injection) {
    std::array<unsigned char, 4> header{};
    if (!ReadExact(pipe, header.data(), header.size(), kFrameReadTimeoutMs)) {
        return ProcessOneConnectionOutcome::failed_before_dispatch;
    }
    const std::uint32_t length =
        static_cast<std::uint32_t>(header[0]) |
        (static_cast<std::uint32_t>(header[1]) << 8U) |
        (static_cast<std::uint32_t>(header[2]) << 16U) |
        (static_cast<std::uint32_t>(header[3]) << 24U);
    if (length == 0 || length > kMaximumPipeFrameBytes) {
        return ProcessOneConnectionOutcome::failed_before_dispatch;
    }

    std::string request(length, '\0');
    if (!ReadExact(pipe, request.data(), request.size(), kFrameReadTimeoutMs)) {
        return ProcessOneConnectionOutcome::failed_before_dispatch;
    }

    // Dispatch is deliberately completed before attempting to write. In
    // serve-once capture mode, a WPF/client disconnect therefore cannot abort
    // the camera transaction or trigger another shutter command.
    const std::string response = dispatcher.Handle(request);
    if (response.empty() || response.size() > kMaximumPipeFrameBytes) {
        return ProcessOneConnectionOutcome::dispatched_delivery_failed;
    }
    const std::uint32_t response_length = static_cast<std::uint32_t>(response.size());
    const std::array<unsigned char, 4> response_header{
        static_cast<unsigned char>(response_length & 0xFFU),
        static_cast<unsigned char>((response_length >> 8U) & 0xFFU),
        static_cast<unsigned char>((response_length >> 16U) & 0xFFU),
        static_cast<unsigned char>((response_length >> 24U) & 0xFFU),
    };
    if (failure_injection.fail_response_header_write ||
        !WriteExact(
            pipe, response_header.data(), response_header.size(), kResponseWriteTimeoutMs)) {
        return ProcessOneConnectionOutcome::dispatched_delivery_failed;
    }
    if (failure_injection.fail_response_body_write ||
        !WriteExact(pipe, response.data(), response.size(), kResponseWriteTimeoutMs)) {
        return ProcessOneConnectionOutcome::dispatched_delivery_failed;
    }
    // A response is delivered only after the client confirms that it read and
    // validated the complete frame. The same bounded OVERLAPPED read used by
    // request framing makes timeout cancellation target this exact operation.
    unsigned char acknowledgment = 0;
    if (failure_injection.fail_delivery_ack_wait ||
        !ReadExact(pipe, &acknowledgment, 1U, kResponseWriteTimeoutMs) ||
        acknowledgment != kDeliveryAcknowledgment) {
        return ProcessOneConnectionOutcome::dispatched_delivery_failed;
    }
    if (failure_injection.fail_response_flush || !FlushFileBuffers(pipe)) {
        return ProcessOneConnectionOutcome::dispatched_delivery_failed;
    }
    return ProcessOneConnectionOutcome::complete_delivery;
}

// Shared named-pipe accept/serve loop. Dispatcher is duck-typed exactly like
// ProcessOneConnection above (only Handle/OnIdle/ShouldStop are required).
//
// lifetime_budget is the process's fixed self-termination bound: computed
// once from GetTickCount64() at the top of this function and never
// recomputed, identically for both the Single- and Dual-camera hosts (see
// the doc comments on the two public entry points below for the specific
// budget each one uses). This preserves the exact behavior that existed
// before this loop was generalized for Single, and gives Dual the same
// documented "absolute lifetime cap from launch" contract instead of an
// idle-activity-extended one, so an operator/CI cannot keep the process
// alive indefinitely just by sending it a steady trickle of requests
// (including rejected ones).
template <typename Dispatcher, typename FailureInjection>
int RunNamedPipeServerLoop(
    std::string_view pipe_name,
    Dispatcher& dispatcher,
    bool serve_once,
    const FailureInjection& failure_injection,
    std::chrono::milliseconds lifetime_budget,
    const std::function<std::uint64_t()>& lifetime_ticks_for_testing = {}) {
    if (!IsSafePipeName(pipe_name)) {
        throw std::invalid_argument("hardware Camera Agent pipe name is invalid");
    }
    const std::wstring full_name =
        L"\\\\.\\pipe\\" + std::wstring(pipe_name.begin(), pipe_name.end());
    CurrentLogonPipeSecurity security;
    const ULONGLONG lifetime_budget_ms = static_cast<ULONGLONG>(
        std::max(lifetime_budget, std::chrono::milliseconds(0)).count());
    const auto lifetime_now = [&]() -> ULONGLONG {
        return lifetime_ticks_for_testing ? lifetime_ticks_for_testing() : GetTickCount64();
    };
    const ULONGLONG server_deadline = lifetime_now() + lifetime_budget_ms;

    for (;;) {
        dispatcher.OnIdle();
        if (dispatcher.ShouldStop() || (!serve_once && lifetime_now() >= server_deadline)) {
            return 0;
        }
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
            if (serve_once) return kFailedBeforeDispatchExitCode;
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, &connect_overlapped);
        const DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
        bool connection_ready = connected != FALSE || connect_error == ERROR_PIPE_CONNECTED;
        if (!connection_ready && connect_error == ERROR_IO_PENDING) {
            const DWORD wait = WaitForSingleObject(
                connect_overlapped.hEvent,
                serve_once ? kAcceptTimeoutMs : 5000U);
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
            if (serve_once) return kFailedBeforeDispatchExitCode;
            continue;
        }

        const ProcessOneConnectionOutcome outcome =
            ProcessOneConnection(pipe, dispatcher, failure_injection);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        if (outcome == ProcessOneConnectionOutcome::dispatched_delivery_failed) {
            return kDispatchedDeliveryFailureExitCode;
        }
        if (serve_once) {
            return outcome == ProcessOneConnectionOutcome::complete_delivery
                ? 0
                : kFailedBeforeDispatchExitCode;
        }
        if (dispatcher.ShouldStop()) {
            return outcome == ProcessOneConnectionOutcome::complete_delivery
                ? 0
                : kFailedBeforeDispatchExitCode;
        }
    }
}

} // namespace

int RunHardwareCameraAgentNamedPipeServer(
    std::string_view pipe_name,
    HardwareCameraAgentDispatcher& dispatcher,
    bool serve_once,
    HardwareCameraAgentPipeFailureInjectionForTesting failure_injection) {
    // Unchanged from the pre-generalization behavior: a fixed 600s deadline
    // computed once at process start, never extended.
    return RunNamedPipeServerLoop(
        pipe_name,
        dispatcher,
        serve_once,
        failure_injection,
        std::chrono::minutes(10));
}

int RunDualHardwareCameraAgentNamedPipeServer(
    std::string_view pipe_name,
    DualHardwareCameraAgentDispatcher& dispatcher,
    bool serve_once,
    DualHardwareCameraAgentPipeFailureInjectionForTesting failure_injection,
    std::optional<std::chrono::milliseconds> lifetime_budget_for_testing) {
    // Same fixed-from-launch policy as RunHardwareCameraAgentNamedPipeServer
    // (Orchestrator decision, 2026-08-17): a rolling/idle-extended deadline
    // was considered but rejected as needlessly complex and because it let
    // any steady trickle of requests -- rejected ones included -- keep the
    // process alive indefinitely, which conflicts with the documented
    // 600-second maximum lifetime this host must honor (see
    // docs/HARDWARE_CAMERA_AGENT_DUAL_V2.md). lifetime_budget_for_testing
    // lets contract tests bound the wait for this deadline instead of
    // waiting out the real 600s production budget; production callers never
    // pass it. Pipe-name uniqueness across launches is the launcher's
    // responsibility (see the doc); this host just serves whatever safe
    // pipe name it is given.
    return RunNamedPipeServerLoop(
        pipe_name,
        dispatcher,
        serve_once,
        failure_injection,
        lifetime_budget_for_testing.value_or(std::chrono::minutes(10)),
        failure_injection.lifetime_ticks_for_testing);
}

int RunDualBindingCameraAgentNamedPipeServer(
    std::string_view pipe_name,
    DualBindingCameraAgentDispatcher& dispatcher,
    bool serve_once,
    DualBindingCameraAgentPipeFailureInjectionForTesting failure_injection,
    std::optional<std::chrono::milliseconds> lifetime_budget_for_testing) {
    // Same fixed-from-launch lifetime as the other two hosts. Binding is if
    // anything the one that most needs it: a session that stays addressable
    // forever is a session an operator can confirm long after they stopped
    // looking at the Live View that justified it.
    return RunNamedPipeServerLoop(
        pipe_name,
        dispatcher,
        serve_once,
        failure_injection,
        lifetime_budget_for_testing.value_or(std::chrono::minutes(10)));
}

} // namespace a0::phase0
