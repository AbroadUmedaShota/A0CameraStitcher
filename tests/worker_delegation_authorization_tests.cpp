// Stub-only: exercises the durable delegated-worker authorization seam without
// constructing a Nikon transport or touching a physical camera.
#include "a0/phase0/hardware_process_lease.hpp"

#include <Windows.h>

#include <array>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

using namespace a0::phase0;
namespace fs = std::filesystem;

namespace {
int failures{};
void Check(bool value, const char *message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}
struct Handle final {
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE value) : value(value) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value(other.value) { other.value = nullptr; }
    Handle &operator=(Handle &&other) noexcept {
        if (this != &other) {
            if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
};
fs::path Root() {
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) throw std::runtime_error("temporary root unavailable");
    return fs::path(temporary) / (L"A0WorkerDelegationAuthorization-" + std::to_wstring(GetCurrentProcessId()));
}
fs::path Marker(const fs::path &root) {
    DWORD session{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) throw std::runtime_error("session unavailable");
    return root / (L"armed-session-" + std::to_wstring(session) + L".marker");
}
std::wstring Quote(const fs::path &value) { return L"\"" + value.wstring() + L"\""; }
std::wstring ToWide(std::string_view value) { return {value.begin(), value.end()}; }
std::string Utf8(const fs::path &value) {
    const auto raw = value.u8string();
    return {raw.begin(), raw.end()};
}
std::string JsonEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const auto character : value) {
        if (character == '\\' || character == '"') result += '\\';
        result += character;
    }
    return result;
}
std::uintptr_t ParseNumber(const char *value) {
    std::uintptr_t result{};
    const auto end = value + std::char_traits<char>::length(value);
    const auto parsed = std::from_chars(value, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !result) throw std::runtime_error("numeric argument");
    return result;
}
bool WaitExited(HANDLE process, DWORD expected) {
    DWORD code{};
    return WaitForSingleObject(process, 5000) == WAIT_OBJECT_0 && GetExitCodeProcess(process, &code) && code == expected;
}

// Each child proves the exact positive/negative identity relation while its
// parent still owns the lease. It never opens an SDK transport.
int ValidateChild(int argc, char **argv) {
    if (argc != 8) return 9;
    const auto parent = reinterpret_cast<HANDLE>(ParseNumber(argv[2]));
    const auto expected = std::string_view(argv[7]) == "allow";
    const auto actual = HardwareProcessLease::ValidateWorkerDelegation(
        parent, argv[3], argv[4], fs::u8path(argv[5]));
    // These are independent negative probes on a registered child's PID.
    const auto wrong_epoch = HardwareProcessLease::ValidateWorkerDelegation(
        parent, std::string(32, '0'), argv[4], fs::u8path(argv[5]));
    const auto forged_parent = HardwareProcessLease::ValidateWorkerDelegation(
        nullptr, argv[3], argv[4], fs::u8path(argv[5]));
    const auto missing_record = HardwareProcessLease::ValidateWorkerDelegation(
        parent, argv[3], argv[4], fs::u8path(argv[6]));
    // A name the file system would rewrite (8.3 short-name syntax) that does not
    // exist yet is refused before anything is created, with the parent handle
    // and epoch unchanged. The folder must not appear: before the alias check,
    // a missing directory under the root was created on the way to the marker.
    const auto aliased_root = fs::u8path(argv[5]) / L"probe~1";
    const auto aliased_record = HardwareProcessLease::ValidateWorkerDelegation(
        parent, argv[3], argv[4], aliased_root);
    std::error_code existence_error;
    const auto aliased_created = fs::exists(aliased_root, existence_error);
    CloseHandle(parent);
    return actual == expected && !wrong_epoch && !forged_parent && !missing_record && !aliased_record &&
                   !aliased_created && !existence_error
               ? 0
               : 7;
}

Handle DuplicateParentForChild() {
    HANDLE copy{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &copy,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0))
        throw std::runtime_error("parent handle duplicate");
    return Handle(copy);
}
Handle SpawnValidateChild(const fs::path &executable, HANDLE inherited_parent, std::string_view epoch,
                          std::string_view lease_name, const fs::path &root, const fs::path &missing_root,
                          bool expected_allow, bool suspended) {
    std::wstring command = Quote(executable) + L" --validate " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(inherited_parent)) + L" " + ToWide(epoch) + L" " +
        ToWide(lease_name) + L" " + Quote(root) + L" " + Quote(missing_root) + L" " +
        (expected_allow ? L"allow" : L"deny");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION created{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | (suspended ? CREATE_SUSPENDED : 0), nullptr, nullptr, &startup, &created))
        throw std::runtime_error("validate child launch");
    CloseHandle(created.hThread);
    return Handle(created.hProcess);
}

// `body`, when not empty, replaces the forged bootstrap JSON; the packet length
// always matches the body.
Handle SpawnForgedWorker(const fs::path &worker, HANDLE inherited_parent, std::string_view lease_name,
                         const fs::path &root, std::string_view body = {}) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE reader{}, writer{};
    if (!CreatePipe(&reader, &writer, &security, 0) || !SetHandleInformation(writer, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("bootstrap pipe");
    Handle read_handle(reader), write_handle(writer);
    std::wstring command = Quote(worker) + L" --delegated-worker " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(inherited_parent)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(read_handle.value));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION created{};
    if (!CreateProcessW(worker.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &created))
        throw std::runtime_error("forged worker launch");
    CloseHandle(created.hThread);
    // The epoch is intentionally malformed; every other field, including both
    // lifetimes, is well formed. The worker must reject before it constructs
    // the stub transport, regardless of the valid parent handle.
    const std::string wire = !body.empty() ? std::string(body) :
        "{\"pipe\":\"A0.Poc.Forged\",\"epoch\":\"not-an-epoch\",\"capability\":\"x\",\"lifetimeMs\":1000,\"servingMs\":2000,\"leaseName\":\"" +
        JsonEscape(lease_name) + "\",\"testMarkerRoot\":\"" + JsonEscape(Utf8(root)) + "\"}";
    const std::uint32_t size = static_cast<std::uint32_t>(wire.size());
    DWORD written{};
    if (!WriteFile(write_handle.value, &size, sizeof(size), &written, nullptr) || written != sizeof(size) ||
        !WriteFile(write_handle.value, wire.data(), size, &written, nullptr) || written != size)
        throw std::runtime_error("forged bootstrap write");
    write_handle.value = nullptr; // Close once, now; EOF is not part of the protocol packet.
    CloseHandle(writer);
    return Handle(created.hProcess);
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "--validate") return ValidateChild(argc, argv);
        if (argc != 1) return 9;
        wchar_t raw_executable[32768]{};
        if (!GetModuleFileNameW(nullptr, raw_executable, 32768)) return 2;
        const fs::path executable(raw_executable);
        const fs::path worker = executable.parent_path() / L"A0CameraStitcher.PreviewWorker.exe";
        const auto root = Root();
        const auto missing = root / "missing";
        const auto lease_name = "A0.Poc.TestLease.WorkerAuthorization." + std::to_string(GetCurrentProcessId());
        {
            HardwareProcessLease lease(lease_name, std::chrono::milliseconds::zero(), root);
            lease.ArmDualDelegation();
            auto parent_a = DuplicateParentForChild();
            auto parent_b = DuplicateParentForChild();
            auto registered_a = SpawnValidateChild(executable, parent_a.value, "placeholder", lease_name, root, missing, false, false);
            auto registered_b = SpawnValidateChild(executable, parent_b.value, "placeholder", lease_name, root, missing, false, false);
            lease.RegisterDualWorkers(registered_a.value, registered_b.value);
            const auto &epoch = lease.DelegationEpoch();

            // The parent handles are valid, but placeholders were intentionally
            // supplied before registration. This proves the stale/forged epoch
            // path is denied without introducing a second authorization grant.
            Check(WaitExited(registered_a.value, 0) && WaitExited(registered_b.value, 0),
                  "registered workers reject a forged pre-registration epoch");

            auto parent_rogue = DuplicateParentForChild();
            auto rogue = SpawnValidateChild(executable, parent_rogue.value, epoch, lease_name, root, missing, false, false);
            Check(WaitExited(rogue.value, 0), "unregistered child is denied despite a live parent and epoch");

            auto parent_forged = DuplicateParentForChild();
            auto forged_worker = SpawnForgedWorker(worker, parent_forged.value, lease_name, root);
            Check(WaitExited(forged_worker.value, 2), "worker main rejects forged bootstrap before SDK transport");

            // A packet of the right length whose JSON is broken: the parser
            // throws inside main, which is exit 4 (exception before any
            // dispatcher, SDK untouched), not the explicit-rejection exit 2.
            auto parent_broken = DuplicateParentForChild();
            auto broken_worker = SpawnForgedWorker(worker, parent_broken.value, lease_name, root,
                                                   "{\"pipe\":\"A0.Poc.Broken\",\"epoch\":");
            Check(WaitExited(broken_worker.value, 4), "worker main exits 4 for a broken bootstrap JSON before SDK transport");

            // All registered children ended normally. Their SDK-independent
            // close evidence is only a fixture cleanup proof for this test.
            lease.DisarmDualDelegation({{true, true, true, registered_a.value}, {true, true, true, registered_b.value}});
        }
        Check(!fs::exists(Marker(root)), "only verified registered worker exits remove the test marker");
        Check(RemoveDirectoryW(missing.c_str()) || GetLastError() == ERROR_PATH_NOT_FOUND, "missing record fixture cleanup");
        Check(RemoveDirectoryW(root.c_str()), "authorization fixture root removed");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
    std::cout << "{\"mode\":\"stub-worker-delegation-authorization\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
