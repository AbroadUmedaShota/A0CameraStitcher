#include "a0/phase0/preview_worker_owner.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/common/protocol_json.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace a0::phase0::experimental {
namespace {
namespace json = a0::common::protocol_json;
namespace timing = preview_worker_timing;
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
    // Parent-side end of the worker's serving lifetime, taken before the
    // bootstrap is written; the worker's own clock starts later.
    ULONGLONG serving_end{};
    // When this child's close attempt ended: the start of its exit window.
    ULONGLONG close_attempt_end{};
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
// lifetimeMs is the worker's operation deadline, servingMs its serving lifetime.
void Bootstrap(Child& child, const std::string& epoch, std::chrono::milliseconds operation_deadline,
               std::chrono::milliseconds serving_lifetime, std::string_view name, const std::filesystem::path* root) {
    std::string body = "{\"pipe\":\"" + child.pipe + "\",\"epoch\":\"" + epoch +
        "\",\"capability\":\"" + child.capability + "\",\"lifetimeMs\":" + std::to_string(operation_deadline.count()) +
        ",\"servingMs\":" + std::to_string(serving_lifetime.count());
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
    child.serving_end = GetTickCount64() + static_cast<ULONGLONG>(serving_lifetime.count());
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
// `deadline` is an absolute GetTickCount64 value computed by the caller from
// the operation's budget (ParentExchangeDeadline, plus S on the first exchange
// with the child), capped by the worker's serving lifetime and the owner's
// session limit.
json::JsonValue Exchange(Child& child, std::size_t worker_index, const std::string& epoch,
                         std::string_view operation,
                         std::optional<PreviewWorkerFailureObservation>& first_failure, ULONGLONG deadline,
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
    // A budget already spent is not a reason to connect: the worker is left
    // alone and closes itself when its serving lifetime ends.
    Require(GetTickCount64() < deadline, "exchange budget exhausted");
    const auto path = L"\\\\.\\pipe\\" + Wide(child.pipe);
    Handle pipe;
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
    observation.request_written = true;
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
        // A quarantined close reply carries no category of its own. Name it
        // here, so the catch below cannot relabel a delivered, validated and
        // ACKed reply as worker_ack_unconfirmed.
        if (observation.category.empty()) observation.category = "worker_operation_failed";
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
bool SeenExited(const PreviewWorkerExitObservation& observation) noexcept {
    return observation.at_close.exited || (observation.after_both_closes && observation.after_both_closes->exited) ||
        (observation.at_repeated_close && observation.at_repeated_close->exited);
}
// Diagnostic only: never changes the close verdict, first_failure, or the
// quarantine decision, and adds no wait. A child whose exit window ended
// before the other child's did gets one more 0 ms look after both windows,
// which can still record an exit that happened in the meantime. The windowed
// observation is kept as is; this only adds a later one.
void RecheckAfterBothCloses(Child& child) noexcept {
    auto& observation = child.exit_observation;
    if (observation.kind == PreviewWorkerExitCheckKind::not_checked || observation.at_close.exited) return;
    observation.after_both_closes = CheckProcess(child.process.value, 0);
}
}

struct PreviewWorkerOwner::Impl {
    std::unique_ptr<HardwareProcessLease> lease;
    std::array<Child, 2> children;
    std::string epoch;
    const DWORD owner_thread = GetCurrentThreadId();
    const ULONGLONG construction_tick = GetTickCount64();
    bool close_attempted{}, closed{};
    bool inject_ack_write_failure{};
    std::optional<PreviewWorkerFailureObservation> first_failure;
    timing::WorkerOperationBudgetTable budget_table = timing::kWorkerOperationBudgets;
#if !defined(A0_NIKON_SDK_AVAILABLE)
    timing::TimeScale test_scale{}; // Test-root owners only.
#endif
    // construction_tick + kPreviewSessionLimit: no exchange deadline and no
    // exit window of this owner extends past it.
    ULONGLONG session_end{};
    // construction_tick + the operation deadline the bootstrap carries
    // (unscaled): from then on only close is sent.
    ULONGLONG operation_end{};
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
        // The UI checks its deadline once per UI command, but one UI command
        // can send up to four worker commands. Past the owner's own operation
        // deadline every command but close is refused here, before any IPC:
        // no sequence is consumed and delivery_failed is left as it is, so
        // close is still sent normally.
        if (op != "close" && GetTickCount64() >= operation_end) {
            if (!first_failure) {
                PreviewWorkerFailureObservation failure;
                failure.worker_index = worker;
                failure.operation = op;
                failure.category = "owner_operation_deadline_expired";
                first_failure = std::move(failure);
            }
            throw TransportError("owner_operation_deadline_expired", "operation deadline expired");
        }
        auto& child = children.at(worker);
        return Exchange(child, worker, epoch, op, first_failure, ExchangeDeadline(child, worker, op), candidate,
                        inject_ack_write_failure && worker == 0 && op == "enumerate");
    }};
    void RequireOwnerThread() const { Require(GetCurrentThreadId() == owner_thread, "camera owner thread required"); }
    timing::TimeScale Scale() const noexcept {
#if defined(A0_NIKON_SDK_AVAILABLE)
        return {};
#else
        return test_scale;
#endif
    }
    ULONGLONG Scaled(std::chrono::milliseconds value) const noexcept {
        return static_cast<ULONGLONG>(Scale().Apply(value).count());
    }
    ULONGLONG ExchangeDeadline(const Child& child, std::size_t worker, std::string_view operation) {
        const auto* budget = timing::FindWorkerOperationBudget(budget_table, operation);
        if (!budget) {
            if (!first_failure) {
                PreviewWorkerFailureObservation failure;
                failure.worker_index = worker;
                failure.operation = operation;
                failure.category = "worker_owner_failed";
                first_failure = std::move(failure);
            }
            Require(false, "operation has no budget");
        }
        // No sequence consumed yet: this is the parent's first exchange with
        // the child, which may still be starting, so S is added to D.
        const bool first_exchange = child.sequence == 0;
        const auto now = GetTickCount64();
        return std::min({now + Scaled(timing::ParentExchangeDeadline(*budget, first_exchange)), child.serving_end,
                         session_end});
    }
    Impl(std::string_view name, const std::filesystem::path* root,
         const std::filesystem::path& executable, std::chrono::milliseconds operation_deadline,
         std::chrono::milliseconds serving_lifetime, const PreviewWorkerTestTiming* test_timing = nullptr,
         const std::function<void(std::array<std::uint32_t, 2>)>& after_spawn = {},
         bool inject_ack_write_failure_for_testing = false)
        : inject_ack_write_failure(inject_ack_write_failure_for_testing) {
      try {
#if defined(A0_NIKON_SDK_AVAILABLE)
        Require(root == nullptr && !after_spawn && !inject_ack_write_failure && test_timing == nullptr,
                "test startup context unavailable in SDK build");
#else
        Require(test_timing == nullptr || root != nullptr, "test timing requires isolated test root");
        if (test_timing) {
            test_scale = test_timing->scale;
            if (test_timing->budget_table) budget_table = *test_timing->budget_table;
        }
#endif
        Require(!inject_ack_write_failure || root != nullptr, "ACK fault requires isolated test root");
        Require(executable.is_absolute() && std::filesystem::is_regular_file(executable), "worker executable unavailable");
        // Checked before the delegation marker is armed and before any worker
        // exists: the worker must still accept the last close after the
        // longest command sent right before the operation deadline.
        Require(timing::ServingLifetimeFits(operation_deadline, serving_lifetime, budget_table, Scale()),
                "worker lifetimes do not cover the close step");
        session_end = construction_tick + Scaled(timing::kPreviewSessionLimit);
        operation_end = construction_tick + static_cast<ULONGLONG>(operation_deadline.count());
        lease = root ? std::make_unique<HardwareProcessLease>(name, std::chrono::milliseconds(0), *root)
                     : std::make_unique<HardwareProcessLease>();
        lease->ArmDualDelegation();
        Require(!lease->RecoveredAbandonedOwner(), "previous owner abandoned; quarantine retained");
        for (auto& child : children) Spawn(child, executable);
        if (after_spawn) after_spawn({GetProcessId(children[0].process.value), GetProcessId(children[1].process.value)});
        lease->RegisterDualWorkers(children[0].process.value, children[1].process.value);
        epoch = lease->DelegationEpoch();
        // Children cannot enter the host until both handles are registered.
        for (auto& child : children) Bootstrap(child, epoch, operation_deadline, serving_lifetime, name, root);
      } catch (...) {
        // Observe before member destruction. Marker cleanup is deliberately not attempted.
        throw PreviewWorkerStartupError(children[0].process.value || children[1].process.value);
      }
    }
    struct ExitWindow {
        PreviewWorkerExitCheckKind kind;
        ULONGLONG milliseconds;
    };
    // The window follows the last reply the parent validated for this child:
    // its close reply when close was sent, otherwise the command that left the
    // child terminal.
    ExitWindow ChooseExitWindow(const Child& child) const noexcept {
        const auto& last = child.last_exchange;
        const bool sent = child.exit_observation.close_reply && child.exit_observation.close_reply->sent;
        if (last && last->response_validated && last->reported_close) {
            if (!last->reported_close->safe_to_exit) return {PreviewWorkerExitCheckKind::instant_at_close_failure, 0};
            if (last->ack_write_completed) {
                const bool closed_reply = sent && last->response_status == PreviewWorkerReplyStatus::closed;
                return {closed_reply ? PreviewWorkerExitCheckKind::waited_after_close_ack
                                     : PreviewWorkerExitCheckKind::waited_after_failure_receipt,
                        Scaled(timing::kExitWindowAfterSafeReceipt)};
            }
        }
        return {PreviewWorkerExitCheckKind::waited_for_worker_cleanup,
                Scaled(timing::ExitWindowWithoutReceipt(budget_table))};
    }
    // Close step 1 for one child: send close at most once and record what came
    // back. The child's exit window starts when this returns.
    void SendCloseOnce(std::size_t worker) noexcept {
        auto& child = children[worker];
        try {
            PreviewWorkerCloseReply reply;
            if (child.delivery_failed) {
                // An earlier command is unconfirmed or ended terminally: close
                // is never sent to this child. That command already recorded
                // first_failure; the fallback below only keeps the invariant.
                if (!first_failure) {
                    PreviewWorkerFailureObservation failure = child.last_exchange.value_or(PreviewWorkerFailureObservation{});
                    failure.worker_index = worker;
                    if (failure.operation.empty()) failure.operation = "close";
                    if (failure.category.empty()) failure.category = "worker_ipc_unconfirmed";
                    first_failure = std::move(failure);
                }
            } else {
                reply.attempted = true;
                try {
                    (void)Exchange(child, worker, epoch, "close", first_failure, ExchangeDeadline(child, worker, "close"));
                } catch (...) {
                    // Recorded in child.last_exchange and first_failure.
                }
                if (child.last_exchange && child.last_exchange->operation == "close") {
                    const auto& exchange = *child.last_exchange;
                    // Sent only once the whole request was written; an
                    // exchange that ended before that delivered nothing.
                    reply.sent = exchange.request_written;
                    reply.response_received = exchange.response_received;
                    reply.response_validated = exchange.response_validated;
                    reply.ack_write_completed = exchange.ack_write_completed;
                    reply.status = exchange.response_status;
                    reply.receipt = exchange.reported_close;
                }
            }
            child.exit_observation.close_reply = std::move(reply);
        } catch (...) {
            // Allocation failure only; the exit observation below still runs.
        }
        child.close_attempt_end = GetTickCount64();
    }
    // Close step 2 for one child: observe its exit inside its own window. The
    // window end is absolute, so observing the children one after the other
    // does not add their windows up.
    bool ObserveExit(std::size_t worker) noexcept {
        auto& child = children[worker];
        auto& observation = child.exit_observation;
        try {
            const auto window = ChooseExitWindow(child);
            observation.kind = window.kind;
            const auto window_end = child.close_attempt_end + window.milliseconds;
            const auto end = std::min(window_end, session_end);
            observation.window_capped_by_session_limit = session_end < window_end;
            const auto now = GetTickCount64();
            // The window bounds how long the parent waits, not the worker. A
            // look that starts after the end is recorded, and its result still
            // counts: the verdict needs the receipt, exit 0 and the disarm,
            // not a time. A 0 ms window is a look by design and is not flagged.
            observation.looked_after_window_end = window.milliseconds > 0 && now >= end;
            const auto wait = now < end ? static_cast<DWORD>(std::min<ULONGLONG>(end - now, INFINITE - 1)) : 0;
            observation.at_close = CheckProcess(child.process.value, wait);
            // Exchange() returns normally for close only after a validated
            // `closed` receipt whose ACK was written; then delivery_failed is false.
            const bool receipt_confirmed = observation.close_reply && observation.close_reply->sent && !child.delivery_failed;
            const bool clean = receipt_confirmed && observation.at_close.exited && observation.at_close.code_available &&
                observation.at_close.exit_code == 0;
            if (!clean && !first_failure) {
                PreviewWorkerFailureObservation failure = child.last_exchange.value_or(PreviewWorkerFailureObservation{});
                failure.worker_index = worker;
                failure.operation = "close";
                // Still running after its window: the parent cannot tell an SDK
                // stop in progress from a worker held for human recovery.
                failure.category = !observation.at_close.exited && !observation.at_close.wait_failed
                    ? "worker_stop_in_progress" : "worker_exit_unconfirmed";
                first_failure = std::move(failure);
            }
            return clean;
        } catch (...) {
            return false;
        }
    }
    // A repeated Close() sends nothing and changes no verdict: it only looks,
    // with 0 ms, at children no earlier look saw exit.
    void LookAtRepeatedClose() noexcept {
        for (auto& child : children) {
            auto& observation = child.exit_observation;
            if (observation.kind == PreviewWorkerExitCheckKind::not_checked || SeenExited(observation)) continue;
            observation.at_repeated_close = CheckProcess(child.process.value, 0);
            ++observation.repeated_close_looks;
        }
    }
    bool Close() noexcept {
        // Do not send shutdown or mutate workflow state from another thread.
        // The UI must queue this operation to the thread holding the OS lease.
        if (GetCurrentThreadId() != owner_thread) return false;
        commissioning.End();
        if (close_attempted) {
            LookAtRepeatedClose();
            return closed;
        }
        close_attempted = true;
        // Step 1: at most one close per child, one after the other.
        for (std::size_t worker = 0; worker < children.size(); ++worker) SendCloseOnce(worker);
        // Step 2: both exits, each in the window that starts at its own close attempt.
        bool both = true;
        for (std::size_t worker = 0; worker < children.size(); ++worker) both = ObserveExit(worker) && both;
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
    try {
        impl_ = std::make_unique<Impl>("", nullptr, SiblingWorker(), timing::kDefaultOperationDeadline,
                                       timing::kDefaultServingLifetime);
    }
    catch (const PreviewWorkerStartupError&) { throw; }
    catch (...) { throw PreviewWorkerStartupError(false); }
}
PreviewWorkerOwner::PreviewWorkerOwner(std::string_view name, const std::filesystem::path& root,
    const std::filesystem::path& executable, std::chrono::milliseconds lifetime,
    std::function<void(std::array<std::uint32_t, 2>)> after_spawn, bool inject_ack_write_failure) {
    try {
        impl_ = std::make_unique<Impl>(name, &root, executable, lifetime, timing::kPreviewSessionLimit, nullptr,
                                       after_spawn, inject_ack_write_failure);
    }
    catch (const PreviewWorkerStartupError&) { throw; }
    catch (...) { throw PreviewWorkerStartupError(false); }
}
PreviewWorkerOwner::PreviewWorkerOwner(std::string_view name, const std::filesystem::path& root,
    const std::filesystem::path& executable, const PreviewWorkerTestTiming& timing,
    std::function<void(std::array<std::uint32_t, 2>)> after_spawn) {
    try {
        impl_ = std::make_unique<Impl>(name, &root, executable, timing.operation_deadline, timing.serving_lifetime,
                                       &timing, after_spawn);
    }
    catch (const PreviewWorkerStartupError&) { throw; }
    catch (...) { throw PreviewWorkerStartupError(false); }
}
PreviewWorkerOwner::~PreviewWorkerOwner() = default; // Destruction never disarms or kills children.
bool PreviewWorkerOwner::Close() noexcept { return impl_->Close(); }
std::wstring FormatPreviewWorkerFailure(const PreviewWorkerFailureObservation& failure) {
    const auto token = [](const std::string& value) { return std::wstring(value.begin(), value.end()); };
    const auto receipt = failure.reported_close
        ? (failure.reported_close->Complete() ? L"complete_reported" : L"incomplete_reported") : L"missing";
    auto text = L"worker=" + (failure.worker_index < 2 ? std::to_wstring(failure.worker_index) : L"global") +
        L" operation=" + token(failure.operation) +
        L" category=" + (failure.category.empty() ? L"missing" : token(failure.category)) +
        L" response=" + (failure.response_validated ? L"validated" : failure.response_received ? L"invalid" : L"missing") +
        L" ack_write=" + (failure.ack_write_completed ? L"completed_not_processed" : L"unconfirmed") +
        L" close_receipt=" + receipt;
    if (failure.category == "worker_stop_in_progress") {
        // " Stopping: the parent cannot tell whether the worker is still stopping
        // its SDK or is held for human recovery." (Japanese, escaped because this
        // file is not compiled as UTF-8.)
        text += L" \u505c\u6b62\u51e6\u7406\u4e2d: SDK\u306e\u505c\u6b62\u4e2d\u304b\u3001"
                L"\u4eba\u306e\u5fa9\u65e7\u5f85\u3061\u3067\u4fdd\u6301\u3055\u308c\u3066\u3044\u308b\u304b\u306f"
                L"\u89aa\u304b\u3089\u306f\u533a\u5225\u3067\u304d\u307e\u305b\u3093\u3002";
    }
    return text;
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
