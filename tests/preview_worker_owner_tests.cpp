// Only configured with the SDK stub: no test process can open a physical camera.
#include "a0/phase0/preview_worker_owner.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"
#include <Windows.h>
#include <filesystem>
#include <iostream>
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
}
int main() {
    wchar_t temporary[MAX_PATH]{}, executable[32768]{};
    if (!GetTempPathW(MAX_PATH, temporary) || !GetModuleFileNameW(nullptr, executable, 32768)) return 2;
    const auto root = fs::path(temporary) / (L"A0WorkerOwnerTest-" + std::to_wstring(GetCurrentProcessId()));
    const auto worker = fs::path(executable).parent_path() / L"A0CameraStitcher.PreviewWorker.exe";
    const auto name = "A0.Poc.TestLease.WorkerOwner." + std::to_string(GetCurrentProcessId());
    try {
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
        Check(RemoveDirectoryW(root.c_str()), "fixture root removed");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
    std::cout << "{\"mode\":\"stub-dual-worker-owner\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
