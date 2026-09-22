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
void Bootstrap(Child& child, const std::string& epoch, std::chrono::milliseconds lifetime) {
    const std::string body = "{\"pipe\":\"" + child.pipe + "\",\"epoch\":\"" + epoch +
        "\",\"capability\":\"" + child.capability + "\",\"lifetimeMs\":" + std::to_string(lifetime.count()) + "}";
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
bool CloseChild(Child& child, const std::string& epoch) {
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
        "\",\"sequence\":1,\"operation\":\"close\",\"candidate\":\"\"}";
    auto size = static_cast<std::uint32_t>(request.size());
    Transfer(pipe.value, &size, sizeof(size), true, deadline);
    Transfer(pipe.value, request.data(), size, true, deadline);
    Transfer(pipe.value, &size, sizeof(size), false, deadline);
    Require(size && size <= 4096, "close receipt length rejected");
    std::string response(size, '\0');
    Transfer(pipe.value, response.data(), size, false, deadline);
    const auto receipt = json::BasicJsonParser<Failure>(response).Parse();
    Require(receipt.kind == json::JsonKind::object && receipt.object.size() == 6, "close envelope rejected");
    const auto text = [&](const char* key, json::JsonKind kind = json::JsonKind::string) {
        return json::RequireFieldWith<Failure>(receipt, key, kind).string;
    };
    Require(text("schema") == "a0.preview-worker.v1" && text("epoch") == epoch && text("status") == "closed" &&
        text("sequence", json::JsonKind::number) == "1" &&
        text("workerPid", json::JsonKind::number) == std::to_string(server), "close context mismatch");
    const auto& payload = json::RequireFieldWith<Failure>(receipt, "payload", json::JsonKind::object);
    Require(payload.object.size() == 5, "close evidence fields rejected");
    for (const char* key : {"liveViewOff", "sourceClosed", "moduleClosed", "processClaimReleased", "safeToExit"})
        Require(json::RequireFieldWith<Failure>(payload, key, json::JsonKind::boolean).boolean, "SDK closure unconfirmed");
    unsigned char ack = 0x06;
    Transfer(pipe.value, &ack, 1, true, deadline);
    DWORD code{};
    return WaitForSingleObject(child.process.value, 5000) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(child.process.value, &code) && code == 0;
}
}

struct PreviewWorkerOwner::Impl {
    std::unique_ptr<HardwareProcessLease> lease;
    std::array<Child, 2> children;
    std::string epoch = Nonce();
    bool close_attempted{}, closed{};
    Impl(std::string_view name, const std::filesystem::path* root,
         const std::filesystem::path& executable, std::chrono::milliseconds lifetime) {
        Require(executable.is_absolute() && std::filesystem::is_regular_file(executable), "worker executable unavailable");
        Require(lifetime.count() > 0 && lifetime <= std::chrono::minutes(10), "worker lifetime rejected");
        lease = root ? std::make_unique<HardwareProcessLease>(name, std::chrono::milliseconds(0), *root)
                     : std::make_unique<HardwareProcessLease>();
        lease->ArmDualDelegation();
        Require(!lease->RecoveredAbandonedOwner(), "previous owner abandoned; quarantine retained");
        for (auto& child : children) Spawn(child, executable);
        lease->RegisterDualWorkers(children[0].process.value, children[1].process.value);
        // Children cannot enter the host until both handles are registered.
        for (auto& child : children) Bootstrap(child, epoch, lifetime);
    }
    bool Close() noexcept {
        if (close_attempted) return closed;
        close_attempted = true;
        bool both = true;
        for (auto& child : children) {
            try { both = CloseChild(child, epoch) && both; }
            catch (...) { both = false; }
        }
        if (!both) return false;
        try {
            lease->DisarmDualDelegation({{true,true,true,children[0].process.value},
                                        {true,true,true,children[1].process.value}});
            closed = true;
        } catch (...) {}
        return closed;
    }
};
PreviewWorkerOwner::PreviewWorkerOwner()
    : impl_(std::make_unique<Impl>("", nullptr, SiblingWorker(), std::chrono::seconds(60))) {}
PreviewWorkerOwner::PreviewWorkerOwner(std::string_view name, const std::filesystem::path& root,
    const std::filesystem::path& executable, std::chrono::milliseconds lifetime)
    : impl_(std::make_unique<Impl>(name, &root, executable, lifetime)) {}
PreviewWorkerOwner::~PreviewWorkerOwner() = default; // Destruction never disarms or kills children.
bool PreviewWorkerOwner::Close() noexcept { return impl_->Close(); }
std::array<std::uint32_t, 2> PreviewWorkerOwner::ProcessIds() const noexcept {
    return {GetProcessId(impl_->children[0].process.value), GetProcessId(impl_->children[1].process.value)};
}
} // namespace a0::phase0::experimental
