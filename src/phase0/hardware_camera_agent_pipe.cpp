#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "a0/phase0/hardware_camera_agent.hpp"
#include "a0/phase0/dual_hardware_camera_agent.hpp"
#include "a0/phase0/dual_binding_camera_agent.hpp"
#include "a0/phase0/agent_host_lifetime.hpp"

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

struct PipeBudget {
    AgentHostLifetime& host;
    bool serve_once;
    std::function<void(std::string_view)> before_stage;
    std::function<void(std::string_view, std::uint32_t)> observe_timeout;

    std::uint64_t Remaining() const noexcept {
        return serve_once ? std::numeric_limits<std::uint64_t>::max()
                          : host.RemainingMilliseconds();
    }
    void Before(std::string_view stage) const {
        if (before_stage) before_stage(stage);
    }
};

bool TransferOverlapped(
    HANDLE pipe,
    void* buffer,
    DWORD bytes,
    bool write,
    const std::function<DWORD()>& remaining_timeout,
    DWORD& transferred) {
    if (remaining_timeout() == 0) return false;
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return false;
    if (remaining_timeout() == 0) {
        CloseHandle(overlapped.hEvent);
        return false;
    }
    const BOOL started = write
        ? WriteFile(pipe, buffer, bytes, &transferred, &overlapped)
        : ReadFile(pipe, buffer, bytes, &transferred, &overlapped);
    if (!started) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            CloseHandle(overlapped.hEvent);
            return false;
        }
        const DWORD timeout_ms = remaining_timeout();
        const DWORD wait = timeout_ms == 0 ? WAIT_TIMEOUT
            : WaitForSingleObject(overlapped.hEvent, timeout_ms);
        if (wait != WAIT_OBJECT_0) {
            (void)CancelIoEx(pipe, &overlapped);
            // OVERLAPPED and its buffer must outlive OS cancellation. This
            // safety drain can outlast the budget; it is not a hard-kill cap.
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
    return transferred > 0 && remaining_timeout() != 0;
}

bool TransferExact(HANDLE pipe, void* buffer, std::size_t bytes, bool write,
                   DWORD timeout_ms, PipeBudget& budget, std::string_view stage) {
    budget.Before(stage);
    if (budget.Remaining() == 0) return false;
    AgentHostLifetime stage_budget(timeout_ms, budget.host.TickSource());
    const auto remaining = [&]() -> DWORD {
        return static_cast<DWORD>(std::min({
            stage_budget.RemainingMilliseconds(), budget.Remaining(),
            static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max() - 1)}));
    };
    auto* data = static_cast<unsigned char*>(buffer);
    std::size_t offset = 0;
    while (offset < bytes) {
        const DWORD remaining_ms = remaining();
        if (remaining_ms == 0) return false;
        if (budget.observe_timeout) budget.observe_timeout(stage, remaining_ms);
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD transferred = 0;
        if (!TransferOverlapped(
                pipe, data + offset, chunk, write, remaining, transferred)) return false;
        offset += transferred;
    }
    return true;
}

bool ReadExact(HANDLE pipe, void* destination, std::size_t bytes, DWORD timeout_ms,
               PipeBudget& budget, std::string_view stage) {
    return TransferExact(pipe, destination, bytes, false, timeout_ms, budget, stage);
}

bool WriteExact(HANDLE pipe, const void* source, std::size_t bytes, DWORD timeout_ms,
                PipeBudget& budget, std::string_view stage) {
    return TransferExact(pipe, const_cast<void*>(source), bytes, true, timeout_ms, budget, stage);
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
    const FailureInjection& failure_injection,
    PipeBudget& budget) {
    std::array<unsigned char, 4> header{};
    if (!ReadExact(pipe, header.data(), header.size(), kFrameReadTimeoutMs, budget, "header")) {
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
    if (!ReadExact(pipe, request.data(), request.size(), kFrameReadTimeoutMs, budget, "body")) {
        return ProcessOneConnectionOutcome::failed_before_dispatch;
    }

    // Dispatch is deliberately completed before attempting to write. In
    // serve-once capture mode, a WPF/client disconnect therefore cannot abort
    // the camera transaction or trigger another shutter command.
    budget.Before("dispatch");
    if (budget.Remaining() == 0) return ProcessOneConnectionOutcome::failed_before_dispatch;
    const std::function<std::uint64_t()> remaining_host = [&budget] { return budget.Remaining(); };
    const std::string response = [&] {
        if constexpr (requires { dispatcher.Handle(request, remaining_host); }) {
            if (!budget.serve_once) return dispatcher.Handle(request, remaining_host);
        }
        return dispatcher.Handle(request);
    }();
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
            pipe, response_header.data(), response_header.size(), kResponseWriteTimeoutMs,
            budget, "response-header")) {
        return ProcessOneConnectionOutcome::dispatched_delivery_failed;
    }
    if (failure_injection.fail_response_body_write ||
        !WriteExact(pipe, response.data(), response.size(), kResponseWriteTimeoutMs,
            budget, "response-body")) {
        return ProcessOneConnectionOutcome::dispatched_delivery_failed;
    }
    // A response is delivered only after the client confirms that it read and
    // validated the complete frame. The same bounded OVERLAPPED read used by
    // request framing makes timeout cancellation target this exact operation.
    unsigned char acknowledgment = 0;
    if (failure_injection.fail_delivery_ack_wait ||
        !ReadExact(pipe, &acknowledgment, 1U, kResponseWriteTimeoutMs, budget, "ack") ||
        acknowledgment != kDeliveryAcknowledgment) {
        return ProcessOneConnectionOutcome::dispatched_delivery_failed;
    }
    // A client can send the fixed ACK byte before reading the response. Keep
    // the existing outbound drain so that early ACK is not delivery proof.
    // Do not enter it at an expired deadline. Once entered, this synchronous
    // safety drain can exceed the host budget; it cannot be safely hard-killed.
    budget.Before("flush");
    if (failure_injection.fail_response_flush || budget.Remaining() == 0 ||
        !FlushFileBuffers(pipe)) {
        return ProcessOneConnectionOutcome::dispatched_delivery_failed;
    }
    return ProcessOneConnectionOutcome::complete_delivery;
}

// Shared named-pipe accept/serve loop. Dispatcher is duck-typed exactly like
// ProcessOneConnection above (only Handle/OnIdle/ShouldStop are required).
//
// The monotonic host budget is initialized once and is never replenished
// by requests (including rejected ones). Dual binding and capture reuse the
// same origin through shared_lifetime. Every I/O stage consults this budget;
// synchronous journal/response drain and safe cancellation/SDK cleanup can still
// outlast it, so it is not a promise of forced process termination.
template <typename Dispatcher, typename FailureInjection>
int RunNamedPipeServerLoop(
    std::string_view pipe_name,
    Dispatcher& dispatcher,
    bool serve_once,
    const FailureInjection& failure_injection,
    std::chrono::milliseconds lifetime_budget,
    const std::function<std::uint64_t()>& lifetime_ticks_for_testing = {},
    AgentHostLifetime* shared_lifetime = nullptr) {
    std::optional<AgentHostLifetime> local_lifetime;
    if (shared_lifetime == nullptr) {
        local_lifetime.emplace(static_cast<std::uint64_t>(
            std::max(lifetime_budget, std::chrono::milliseconds(0)).count()),
            lifetime_ticks_for_testing);
    }
    AgentHostLifetime& lifetime = shared_lifetime ? *shared_lifetime : *local_lifetime;
    PipeBudget budget{lifetime, serve_once, {}, {}};
    if constexpr (requires { failure_injection.before_stage_for_testing; }) {
        budget.before_stage = failure_injection.before_stage_for_testing;
        budget.observe_timeout = failure_injection.wait_timeout_for_testing;
    }
    if (!IsSafePipeName(pipe_name)) {
        throw std::invalid_argument("hardware Camera Agent pipe name is invalid");
    }
    const std::wstring full_name =
        L"\\\\.\\pipe\\" + std::wstring(pipe_name.begin(), pipe_name.end());
    CurrentLogonPipeSecurity security;
    for (;;) {
        if (budget.Remaining() == 0) return 0;
        dispatcher.OnIdle();
        if (dispatcher.ShouldStop() || budget.Remaining() == 0) {
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
        budget.Before("accept");
        const auto accept_timeout = [&] {
            return static_cast<DWORD>(std::min<std::uint64_t>(
                serve_once ? kAcceptTimeoutMs : 5000U, budget.Remaining()));
        };
        if (accept_timeout() == 0) {
            CloseHandle(connect_overlapped.hEvent);
            CloseHandle(pipe);
            return 0;
        }
        if (budget.observe_timeout) budget.observe_timeout("accept", accept_timeout());
        const BOOL connected = ConnectNamedPipe(pipe, &connect_overlapped);
        const DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
        bool connection_ready = connected != FALSE || connect_error == ERROR_PIPE_CONNECTED;
        if (!connection_ready && connect_error == ERROR_IO_PENDING) {
            const DWORD timeout_ms = accept_timeout();
            const DWORD wait = timeout_ms == 0 ? WAIT_TIMEOUT : WaitForSingleObject(
                connect_overlapped.hEvent, timeout_ms);
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
            ProcessOneConnection(pipe, dispatcher, failure_injection, budget);
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
        std::chrono::minutes(10), failure_injection.lifetime_ticks_for_testing);
}

int RunDualHardwareCameraAgentNamedPipeServer(
    std::string_view pipe_name,
    DualHardwareCameraAgentDispatcher& dispatcher,
    bool serve_once,
    DualHardwareCameraAgentPipeFailureInjectionForTesting failure_injection,
    std::optional<std::chrono::milliseconds> lifetime_budget_for_testing,
    AgentHostLifetime* shared_lifetime) {
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
        failure_injection.lifetime_ticks_for_testing, shared_lifetime);
}

int RunDualBindingCameraAgentNamedPipeServer(
    std::string_view pipe_name,
    DualBindingCameraAgentDispatcher& dispatcher,
    bool serve_once,
    DualBindingCameraAgentPipeFailureInjectionForTesting failure_injection,
    std::optional<std::chrono::milliseconds> lifetime_budget_for_testing,
    AgentHostLifetime* shared_lifetime) {
    // Same fixed-from-launch lifetime as the other two hosts. Binding is if
    // anything the one that most needs it: a session that stays addressable
    // forever is a session an operator can confirm long after they stopped
    // looking at the Live View that justified it.
    return RunNamedPipeServerLoop(
        pipe_name,
        dispatcher,
        serve_once,
        failure_injection,
        lifetime_budget_for_testing.value_or(std::chrono::minutes(10)), {}, shared_lifetime);
}

} // namespace a0::phase0
