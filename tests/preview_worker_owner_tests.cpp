// Only configured with the SDK stub: no test process can open a physical camera.
#include "a0/phase0/preview_worker_owner.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"
#include <Windows.h>
#include <filesystem>
#include <iostream>
#include <charconv>
using namespace a0::phase0;
using namespace a0::phase0::experimental;
namespace fs = std::filesystem;
namespace {
int failures{};
void Check(bool value, const char* message) { if (!value) { ++failures; std::cerr << message << '\n'; } }
fs::path Marker(const fs::path& root) {
    DWORD session{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) throw std::runtime_error("session lookup");
    return root / (L"armed-session-" + std::to_wstring(session) + L".marker");
}
struct OwnedHandle {
    HANDLE value{};
    ~OwnedHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
// The helper owns a real PreviewWorkerOwner, but this target is SDK-stub only.
// The outer observer retains child handles before permitting the abrupt exit.
int AbandonHelper(char** argv, const fs::path& temporary, const fs::path& worker) {
    auto number = [](const char* text) {
        std::uintptr_t result{};
        const auto end = text + std::char_traits<char>::length(text);
        const auto parsed = std::from_chars(text, end, result);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !result) throw std::runtime_error("helper argument");
        return result;
    };
    const auto observer = number(argv[2]);
    const auto output = reinterpret_cast<HANDLE>(number(argv[3]));
    const auto release = reinterpret_cast<HANDLE>(number(argv[4]));
    const auto root = temporary / (L"A0WorkerOwnerTest-" + std::to_wstring(observer)) / "abandoned";
    const auto name = "A0.Poc.TestLease.WorkerOwner." + std::to_string(observer);
    PreviewWorkerOwner owner(name, root, worker, std::chrono::seconds(10));
    const auto ids = owner.ProcessIds();
    DWORD written{};
    if (!WriteFile(output, ids.data(), sizeof(ids), &written, nullptr) || written != sizeof(ids)) return 92;
    if (WaitForSingleObject(release, 5000) != WAIT_OBJECT_0) return 93;
    ExitProcess(91); // Deliberately bypasses the owner's destructor; stub only.
}
void CheckAbandonedOwner(const fs::path& executable, const fs::path& root, const std::string& name) {
    OwnedHandle reader, writer, release, parent;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    if (!CreatePipe(&reader.value, &writer.value, &security, 0) ||
        !SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("observer pipe");
    release.value = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!release.value) throw std::runtime_error("observer barrier");
    auto command = L"\"" + executable.wstring() + L"\" --abandon " + std::to_wstring(GetCurrentProcessId()) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(writer.value)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(release.value));
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION created{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &created)) throw std::runtime_error("observer launch");
    parent.value = created.hProcess; CloseHandle(created.hThread);
    CloseHandle(writer.value); writer.value = nullptr;
    const auto deadline = GetTickCount64() + 5000;
    DWORD available{};
    while (GetTickCount64() < deadline) {
        if (!PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available >= sizeof(std::array<std::uint32_t, 2>)) break;
        Sleep(10);
    }
    if (available < sizeof(std::array<std::uint32_t, 2>)) throw std::runtime_error("observer identities unavailable");
    std::array<std::uint32_t, 2> ids{}; DWORD received{};
    if (!ReadFile(reader.value, ids.data(), sizeof(ids), &received, nullptr) || received != sizeof(ids))
        throw std::runtime_error("observer identities incomplete");
    std::array<OwnedHandle, 2> workers;
    for (std::size_t i = 0; i != workers.size(); ++i) {
        workers[i].value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[i]);
        if (!workers[i].value) throw std::runtime_error("observer worker handle");
    }
    Check(fs::exists(Marker(root / "abandoned")), "real parent armed marker before worker bootstrap");
    if (!SetEvent(release.value)) throw std::runtime_error("observer release");
    DWORD code{};
    if (WaitForSingleObject(parent.value, 5000) != WAIT_OBJECT_0 || !GetExitCodeProcess(parent.value, &code))
        throw std::runtime_error("parent exit unconfirmed");
    Check(code == 91, "parent exited without destructors");
    for (auto& worker : workers) {
        if (WaitForSingleObject(worker.value, 5000) != WAIT_OBJECT_0 || !GetExitCodeProcess(worker.value, &code))
            throw std::runtime_error("worker exit unconfirmed: preserve marker");
        // Parent may disappear before bootstrap validation (2) or after host start (3).
        Check(code == 2 || code == 3, "parent death is not normal authorized completion");
    }
    bool blocked{};
    try { HardwareProcessLease denied(name, std::chrono::milliseconds(0), root / "abandoned"); }
    catch (const TransportError& error) { blocked = error.Category() == "camera_control_delegation_quarantined"; }
    Check(blocked && fs::exists(Marker(root / "abandoned")), "real parent death preserves next-owner exclusion");
    // Test-only marker, exact path, with the parent and both stub workers observed exited.
    if (!DeleteFileW(Marker(root / "abandoned").c_str())) throw std::runtime_error("fixture marker cleanup");
    Check(RemoveDirectoryW((root / "abandoned").c_str()), "abandoned fixture directory removed");
}
}
int main(int argc, char** argv) {
    wchar_t temporary[MAX_PATH]{}, executable[32768]{};
    if (!GetTempPathW(MAX_PATH, temporary) || !GetModuleFileNameW(nullptr, executable, 32768)) return 2;
    const auto root = fs::path(temporary) / (L"A0WorkerOwnerTest-" + std::to_wstring(GetCurrentProcessId()));
    const auto worker = fs::path(executable).parent_path() / L"A0CameraStitcher.PreviewWorker.exe";
    const auto name = "A0.Poc.TestLease.WorkerOwner." + std::to_string(GetCurrentProcessId());
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--abandon") return AbandonHelper(argv, temporary, worker);
        if (argc != 1) return 2;
        {
            PreviewWorkerOwner owner(name, root / "normal", worker, std::chrono::seconds(10));
            const auto ids = owner.ProcessIds();
            Check(ids[0] && ids[1] && ids[0] != ids[1], "two actual registered worker processes");
            Check(fs::exists(Marker(root / "normal")), "marker armed before bootstrap");
            Check(owner.Close(), "both IPC receipts and OS exits allow disarm");
            Check(owner.Close(), "repeated close returns cached success without resend");
            Check(!fs::exists(Marker(root / "normal")), "marker removed only after verified close");
        }
        {
            HardwareProcessLease available(name, std::chrono::milliseconds(0), root / "normal");
            Check(!available.RecoveredAbandonedOwner(), "normal owner releases fresh lease");
        }
        Check(RemoveDirectoryW((root / "normal").c_str()), "normal fixture directory removed");
        {
            PreviewWorkerOwner owner(name, root / "expired", worker, std::chrono::milliseconds(300));
            const auto ids = owner.ProcessIds();
            for (auto id : ids) {
                HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
                if (!process) return 3;
                const auto wait = WaitForSingleObject(process, 5000);
                DWORD code{};
                const bool ended = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process, &code);
                CloseHandle(process);
                if (!ended) return 3; // Never remove quarantine while a worker might live.
                Check(code == 3, "uncommanded expiry is not a successful close receipt");
            }
            Check(!owner.Close() && !owner.Close(), "missing receipts remain terminal without retries");
            Check(fs::exists(Marker(root / "expired")), "clean OS exit alone never disarms");
        }
        bool blocked{};
        try { HardwareProcessLease denied(name, std::chrono::milliseconds(0), root / "expired"); }
        catch (const TransportError& error) { blocked = error.Category() == "camera_control_delegation_quarantined"; }
        Check(blocked, "next owner is quarantined after incomplete shutdown evidence");
        // Exact test-created marker; both stub workers were observed exited above.
        if (!DeleteFileW(Marker(root / "expired").c_str())) return 4;
        Check(RemoveDirectoryW((root / "expired").c_str()), "expired fixture directory removed");
        CheckAbandonedOwner(executable, root, name);
        Check(RemoveDirectoryW(root.c_str()), "fixture root removed");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
    std::cout << "{\"mode\":\"stub-dual-worker-owner\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
