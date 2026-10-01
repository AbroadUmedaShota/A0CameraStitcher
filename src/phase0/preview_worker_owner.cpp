#include "a0/phase0/preview_worker_owner.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/common/protocol_json.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace a0::phase0::experimental {
namespace {
namespace json = a0::common::protocol_json;
struct Failure {
    [[noreturn]] static void Fail(std::string_view a, std::string_view b) { throw TransportError(std::string(a), std::string(b)); }
};
void Require(bool condition, const char* message) {
    if (!condition) throw TransportError("worker_owner_failed", message);
}
struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
std::string Nonce() {
    std::array<unsigned char, 16> bytes{};
    Require(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0,
            "capability generation failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
std::filesystem::path SiblingWorker() {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    Require(count && count < path.size(), "executable location unavailable");
    return std::filesystem::path(path.data()).parent_path() / L"A0CameraStitcher.PreviewWorker.exe";
}
std::wstring Wide(const std::string& text) { return {text.begin(), text.end()}; }
struct Child {
    Handle process, bootstrap_writer;
    std::string pipe = "A0.Preview." + std::to_string(GetCurrentProcessId()) + "." + Nonce();
    std::string capability = Nonce();
    std::uint64_t sequence{};
    bool delivery_failed{};
    std::optional<PreviewWorkerFailureObservation> last_exchange;
    PreviewWorkerExitObservation exit_observation;
};
void Spawn(Child& child, const std::filesystem::path& executable) {
    Handle parent, reader;
    Require(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent.value,
                            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0), "parent handle failed");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Require(CreatePipe(&reader.value, &child.bootstrap_writer.value, &security, 2048), "bootstrap pipe failed");
    Require(SetHandleInformation(child.bootstrap_writer.value, HANDLE_FLAG_INHERIT, 0), "bootstrap inheritance failed");
    SIZE_T size{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    Require(size != 0, "attribute size failed");
    std::vector<unsigned char> memory(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
    Require(InitializeProcThreadAttributeList(attributes, 1, 0, &size), "attributes failed");
    HANDLE inherited[]{parent.value, reader.value};
    const bool restricted = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION created{};
    auto command = L"\"" + executable.wstring() + L"\" --delegated-worker " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(parent.value)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(reader.value));
    const bool success = restricted && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &created);
    DeleteProcThreadAttributeList(attributes);
    Require(success, "worker launch failed");
    CloseHandle(created.hThread);
    child.process.value = created.hProcess;
}
void Bootstrap(Child& child, const std::string& epoch, std::chrono::milliseconds lifetime,
               std::string_view name, const std::filesystem::path* root) {
    std::string body = "{\"pipe\":\"" + child.pipe + "\",\"epoch\":\"" + epoch +
        "\",\"capability\":\"" + child.capability + "\",\"lifetimeMs\":" + std::to_string(lifetime.count());
    if (root) {
        const auto encoded = root->u8string();
        const std::string path(encoded.begin(), encoded.end());
        body += ",\"leaseName\":\"" + json::JsonEscape(name) + "\",\"testMarkerRoot\":\"" + json::JsonEscape(path) + "\"";
    }
    body += "}";
    Require(body.size() <= 1024, "bootstrap too large");
    const auto size = static_cast<std::uint32_t>(body.size());
    std::vector<char> packet(sizeof(size) + body.size());
    std::memcpy(packet.data(), &size, sizeof(size));
    std::memcpy(packet.data() + sizeof(size), body.data(), body.size());
    DWORD written{};
    const bool ok = WriteFile(child.bootstrap_writer.value, packet.data(), static_cast<DWORD>(packet.size()), &written, nullptr)
                    && written == packet.size();
    CloseHandle(child.bootstrap_writer.value);
    child.bootstrap_writer.value = nullptr;
    Require(ok, "bootstrap delivery unconfirmed");
}
void Transfer(HANDLE pipe, void* buffer, DWORD size, bool write, ULONGLONG deadline) {
    DWORD offset{};
    while (offset < size) {
        Require(GetTickCount64() < deadline, "pipe deadline");
        Handle event;
        event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Require(event.value != nullptr, "pipe event failed");
        OVERLAPPED pending{}; pending.hEvent = event.value;
        DWORD done{};
        const BOOL immediate = write ? WriteFile(pipe, static_cast<char*>(buffer) + offset, size - offset, &done, &pending)
                                     : ReadFile(pipe, static_cast<char*>(buffer) + offset, size - offset, &done, &pending);
        if (!immediate) {
            Require(GetLastError() == ERROR_IO_PENDING, "pipe transfer failed");
            const auto now = GetTickCount64();
            const auto remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
            if (WaitForSingleObject(event.value, remaining) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &pending);
                // Retain the lease and buffers until cancellation is observed.
                // No forced termination of a real worker or pending I/O.
                WaitForSingleObject(event.value, INFINITE);
                Require(false, "pipe completion unconfirmed");
            }
            Require(GetOverlappedResult(pipe, &pending, &done, FALSE), "pipe result failed");
        }
        Require(done > 0, "pipe ended");
        offset += done;
    }
}
json::JsonValue Exchange(Child& child, std::size_t worker_index, const std::string& epoch,
                         std::string_view operation,
                         std::optional<PreviewWorkerFailureObservation>& first_failure,
                         std::string_view candidate = "", bool inject_ack_write_failure = false) {
    PreviewWorkerFailureObservation observation;
    observation.worker_index = worker_index;
    observation.operation = operation;
    const auto remember_failure = [&] {
        child.last_exchange = observation;
        if (!first_failure) first_failure = observation;
    };
    if (child.delivery_failed) {
        remember_failure();
        Require(false, "previous command delivery unconfirmed");
    }
    // Consume the sequence before I/O; any failed exchange forbids further sends.
    const auto sequence = ++child.sequence;
    child.delivery_failed = true;
    try {
    const auto path = L"\\\\.\\pipe\\" + Wide(child.pipe);
    Handle pipe;
    const auto deadline = GetTickCount64() + 30000;
    do {
        Require(WaitForSingleObject(child.process.value, 0) == WAIT_TIMEOUT, "worker ended before close receipt");
        pipe.value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe.value != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        Require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY, "pipe connection failed");
        Sleep(10); // Readiness polling only; no command has been sent.
    } while (GetTickCount64() < deadline);
    Require(pipe.value != INVALID_HANDLE_VALUE, "worker pipe unavailable");
    ULONG server{};
    Require(GetNamedPipeServerProcessId(pipe.value, &server) && server == GetProcessId(child.process.value),
            "pipe is not the registered worker");
    std::string request = "{\"schema\":\"a0.preview-worker.v1\",\"epoch\":\"" + epoch +
        "\",\"capability\":\"" + child.capability +
        "\",\"sequence\":" + std::to_string(sequence) + ",\"operation\":\"" + std::string(operation) +
        "\",\"candidate\":\"" + json::JsonEscape(candidate) + "\"}";
    auto size = static_cast<std::uint32_t>(request.size());
    Transfer(pipe.value, &size, sizeof(size), true, deadline);
    Transfer(pipe.value, request.data(), size, true, deadline);
    Transfer(pipe.value, &size, sizeof(size), false, deadline);
    constexpr std::size_t maximum = 512U * 1024U + 4096;
    Require(size && size <= (operation == "frame" ? maximum : 4096), "worker reply length rejected");
    std::string response(size, '\0');
    Transfer(pipe.value, response.data(), size, false, deadline);
    observation.response_received = true;
    auto reply = ParsePreviewWorkerReply(response, epoch, server, sequence, operation);
    observation.response_validated = true;
    observation.response_status = reply.status;
    observation.category = reply.error_category;
    observation.reported_close = reply.close_receipt;
    unsigned char ack = 0x06;
#if !defined(A0_NIKON_SDK_AVAILABLE)
    if (inject_ack_write_failure) {
        // Test-root owner only: close this fake IPC handle after a validated
        // reply, forcing the actual ACK WriteFile path to fail deterministically.
        CloseHandle(pipe.value);
        pipe.value = INVALID_HANDLE_VALUE;
    }
#else
    (void)inject_ack_write_failure;
#endif
    Transfer(pipe.value, &ack, 1, true, deadline);
    observation.ack_write_completed = true;
    child.last_exchange = observation;
    if (reply.status == PreviewWorkerReplyStatus::failed ||
        reply.status == PreviewWorkerReplyStatus::quarantined) {
        // The worker may have already exited or may not have processed the ACK.
        // Keep this command terminal and never re-send close to it.
        remember_failure();
        throw TransportError("worker_operation_failed", "worker reported a terminal failure");
    }
    child.delivery_failed = false;
    return std::move(reply.payload);
    } catch (...) {
        if (observation.category.empty())
            observation.category = observation.response_validated ? "worker_ack_unconfirmed" :
                                   observation.response_received ? "worker_reply_invalid" : "worker_ipc_unconfirmed";
        remember_failure();
        throw;
    }
}
PreviewWorkerProcessCheck CheckProcess(HANDLE process, DWORD timeout_ms) noexcept {
    PreviewWorkerProcessCheck check;
    const auto wait = WaitForSingleObject(process, timeout_ms);
    if (wait == WAIT_OBJECT_0) {
        check.exited = true;
        DWORD code{};
        if (GetExitCodeProcess(process, &code)) {
            check.code_available = true;
            check.exit_code = static_cast<std::uint32_t>(code);
        }
    } else if (wait != WAIT_TIMEOUT) {
        check.wait_failed = true;
    }
    return check;
}
bool CloseChild(Child& child, std::size_t worker_index, const std::string& epoch,
                std::optional<PreviewWorkerFailureObservation>& first_failure) {
    // The OS exit observation below must be captured whether or not the close
    // exchange itself threw (e.g. the child already exited, or a prior command
    // left this child terminal): otherwise an early-exited worker's exit code
    // is lost exactly when a diagnostic needs it most. first_failure semantics
    // are unchanged: Exchange() already records the real cause via
    // remember_failure() before any exception reaches here.
    //
    // The wait itself must not grow on the failure path. The existing 5000 ms
    // wait below belongs only to the already-working success path (the close
    // exchange was answered and ACKed; the process is expected to exit almost
    // immediately after). On the failure path a worker may still be mid
    // teardown -- that can legitimately take far longer than 5s -- so this is
    // a single 0 ms observation taken at the moment the close was attempted,
    // not a wait for the worker to finish. Impl::Close() may take one more 0 ms
    // look after both children's close steps (see RecheckAfterBothCloses);
    // redesigning how long/whether to wait for a still-alive worker is a
    // separate change.
    bool exchange_threw = false;
    try {
        Exchange(child, worker_index, epoch, "close", first_failure);
    } catch (...) {
        exchange_threw = true;
    }
    PreviewWorkerExitObservation observation;
    observation.kind = exchange_threw ? PreviewWorkerExitCheckKind::instant_at_close_failure
                                      : PreviewWorkerExitCheckKind::waited_after_close_ack;
    observation.at_close = CheckProcess(child.process.value, exchange_threw ? 0 : 5000);
    child.exit_observation = observation;
    const bool clean = !exchange_threw && observation.at_close.code_available && observation.at_close.exit_code == 0;
    if (!clean && !first_failure) {
        PreviewWorkerFailureObservation failure = child.last_exchange.value_or(PreviewWorkerFailureObservation{});
        failure.worker_index = worker_index;
        failure.operation = "close";
        failure.category = "worker_exit_unconfirmed";
        first_failure = std::move(failure);
    }
    return clean;
}
// Diagnostic only: never changes the close verdict, first_failure, or the
// quarantine decision, and adds no wait. A child whose close exchange failed
// was looked at only once, with 0 ms, at the moment of its close attempt; a
// worker that was already tearing down then (for example one that exited before
// replying) is usually finished by the time the other child's close exchange
// and success-path wait are done, so a second 0 ms look here can record its exit
// code. The at-close observation is kept as is; this only adds a later one.
void RecheckAfterBothCloses(Child& child) noexcept {
    auto& observation = child.exit_observation;
    if (observation.kind != PreviewWorkerExitCheckKind::instant_at_close_failure || observation.at_close.exited) return;
    observation.after_both_closes = CheckProcess(child.process.value, 0);
}
}

struct PreviewWorkerOwner::Impl {
    std::unique_ptr<HardwareProcessLease> lease;
    std::array<Child, 2> children;
    std::string epoch;
    const DWORD owner_thread = GetCurrentThreadId();
    bool close_attempted{}, closed{};
    bool inject_ack_write_failure{};
    std::optional<PreviewWorkerFailureObservation> first_failure;
    PreviewCommissioning commissioning{[this](std::size_t worker, std::string_view op, std::string_view candidate) {
        RequireOwnerThread();
        Require(!close_attempted, "owner already closing");
        for (std::size_t index = 0; index < children.size(); ++index) {
            if (WaitForSingleObject(children[index].process.value, 0) != WAIT_TIMEOUT) {
                if (!first_failure) {
                    PreviewWorkerFailureObservation failure;
                    failure.worker_index = index;
                    failure.operation = op;
                    failure.category = "worker_prior_exit";
                    first_failure = std::move(failure);
                }
                Require(false, "paired worker no longer alive");
            }
        }
        return Exchange(children.at(worker), worker, epoch, op, first_failure, candidate,
                        inject_ack_write_failure && worker == 0 && op == "enumerate");
    }};
    void RequireOwnerThread() const { Require(GetCurrentThreadId() == owner_thread, "camera owner thread required"); }
    Impl(std::string_view name, const std::filesystem::path* root,
         const std::filesystem::path& executable, std::chrono::milliseconds lifetime,
         const std::function<void(std::array<std::uint32_t, 2>)>& after_spawn = {},
         bool inject_ack_write_failure_for_testing = false)
        : inject_ack_write_failure(inject_ack_write_failure_for_testing) {
      try {
#if defined(A0_NIKON_SDK_AVAILABLE)
        Require(root == nullptr && !after_spawn && !inject_ack_write_failure,
                "test startup context unavailable in SDK build");
#endif
        Require(!inject_ack_write_failure || root != nullptr, "ACK fault requires isolated test root");
        Require(executable.is_absolute() && std::filesystem::is_regular_file(executable), "worker executable unavailable");
        Require(lifetime.count() > 0 && lifetime <= std::chrono::minutes(10), "worker lifetime rejected");
        lease = root ? std::make_unique<HardwareProcessLease>(name, std::chrono::milliseconds(0), *root)
                     : std::make_unique<HardwareProcessLease>();
        lease->ArmDualDelegation();
        Require(!lease->RecoveredAbandonedOwner(), "previous owner abandoned; quarantine retained");
        for (auto& child : children) Spawn(child, executable);
        if (after_spawn) after_spawn({GetProcessId(children[0].process.value), GetProcessId(children[1].process.value)});
        lease->RegisterDualWorkers(children[0].process.value, children[1].process.value);
        epoch = lease->DelegationEpoch();
        // Children cannot enter the host until both handles are registered.
        for (auto& child : children) Bootstrap(child, epoch, lifetime, name, root);
      } catch (...) {
        // Observe before member destruction. Marker cleanup is deliberately not attempted.
        throw PreviewWorkerStartupError(children[0].process.value || children[1].process.value);
      }
    }
    bool Close() noexcept {
        // Do not send shutdown or mutate workflow state from another thread.
        // The UI must queue this operation to the thread holding the OS lease.
        if (GetCurrentThreadId() != owner_thread) return false;
        commissioning.End();
        if (close_attempted) return closed;
        close_attempted = true;
        bool both = true;
        for (std::size_t worker = 0; worker < children.size(); ++worker) {
            try { both = CloseChild(children[worker], worker, epoch, first_failure) && both; }
            catch (...) { both = false; }
        }
        for (auto& child : children) RecheckAfterBothCloses(child);
        if (!both) return false;
        try {
            lease->DisarmDualDelegation({{true,true,true,children[0].process.value},
                                        {true,true,true,children[1].process.value}});
            closed = true;
        } catch (...) {
            if (!first_failure) {
                PreviewWorkerFailureObservation failure;
                failure.worker_index = children.size();
                failure.operation = "disarm";
                failure.category = "delegation_disarm_unconfirmed";
                first_failure = std::move(failure);
            }
        }
        return closed;
    }
};
PreviewWorkerOwner::PreviewWorkerOwner() {
    try { impl_ = std::make_unique<Impl>("", nullptr, SiblingWorker(), std::chrono::seconds(60)); }
    catch (const PreviewWorkerStartupError&) { throw; }
    catch (...) { throw PreviewWorkerStartupError(false); }
}
PreviewWorkerOwner::PreviewWorkerOwner(std::string_view name, const std::filesystem::path& root,
    const std::filesystem::path& executable, std::chrono::milliseconds lifetime,
    std::function<void(std::array<std::uint32_t, 2>)> after_spawn, bool inject_ack_write_failure) {
    try { impl_ = std::make_unique<Impl>(name, &root, executable, lifetime, after_spawn, inject_ack_write_failure); }
    catch (const PreviewWorkerStartupError&) { throw; }
    catch (...) { throw PreviewWorkerStartupError(false); }
}
PreviewWorkerOwner::~PreviewWorkerOwner() = default; // Destruction never disarms or kills children.
bool PreviewWorkerOwner::Close() noexcept { return impl_->Close(); }
std::wstring FormatPreviewWorkerFailure(const PreviewWorkerFailureObservation& failure) {
    const auto token = [](const std::string& value) { return std::wstring(value.begin(), value.end()); };
    const auto receipt = failure.reported_close
        ? (failure.reported_close->Complete() ? L"complete_reported" : L"incomplete_reported") : L"missing";
    return L"worker=" + (failure.worker_index < 2 ? std::to_wstring(failure.worker_index) : L"global") +
        L" operation=" + token(failure.operation) +
        L" category=" + (failure.category.empty() ? L"missing" : token(failure.category)) +
        L" response=" + (failure.response_validated ? L"validated" : failure.response_received ? L"invalid" : L"missing") +
        L" ack_write=" + (failure.ack_write_completed ? L"completed_not_processed" : L"unconfirmed") +
        L" close_receipt=" + receipt;
}
std::optional<PreviewWorkerFailureObservation> PreviewWorkerOwner::FirstFailure() const {
    return impl_->first_failure;
}
std::array<PreviewWorkerExitObservation, 2> PreviewWorkerOwner::ExitObservations() const noexcept {
    return {impl_->children[0].exit_observation, impl_->children[1].exit_observation};
}
std::array<std::uint32_t, 2> PreviewWorkerOwner::ProcessIds() const noexcept {
    return {GetProcessId(impl_->children[0].process.value), GetProcessId(impl_->children[1].process.value)};
}
std::array<std::string, 2> PreviewWorkerOwner::Enumerate(std::size_t worker) {
    impl_->RequireOwnerThread();
    try { return impl_->commissioning.Enumerate(worker); }
    catch (...) { impl_->Close(); throw; }
}
std::vector<unsigned char> PreviewWorkerOwner::Preview(std::size_t worker, std::string_view candidate) {
    impl_->RequireOwnerThread();
    try { return impl_->commissioning.Preview(worker, candidate); }
    catch (...) { impl_->Close(); throw; }
}
void PreviewWorkerOwner::ConfirmAndSuspend(std::size_t worker, ObservedPreviewBody body) {
    impl_->RequireOwnerThread();
    try { impl_->commissioning.ConfirmAndSuspend(worker, body); }
    catch (...) { impl_->Close(); throw; }
}
void PreviewWorkerOwner::StartBoth() {
    impl_->RequireOwnerThread();
    try { impl_->commissioning.StartBoth(); }
    catch (...) { impl_->Close(); throw; }
}
std::vector<unsigned char> PreviewWorkerOwner::Read(ObservedPreviewBody body) {
    impl_->RequireOwnerThread();
    try { return impl_->commissioning.Read(body); }
    catch (...) { impl_->Close(); throw; }
}
} // namespace a0::phase0::experimental
