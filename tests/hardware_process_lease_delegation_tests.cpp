#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"
#include <Windows.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>

using namespace a0::phase0;
namespace {
int failures{};
void Check(bool v, const char *m) {
    if (!v) {
        ++failures;
        std::cerr << m << '\n';
    }
}
std::filesystem::path Root() {
    wchar_t b[MAX_PATH]{};
    GetTempPathW(MAX_PATH, b);
    return std::filesystem::path(b) / (L"A0LeaseDelegationTest-" + std::to_wstring(GetCurrentProcessId()));
}
std::string Name() {
    return "A0.Poc.TestLease.Delegation." + std::to_string(GetCurrentProcessId());
}
bool Quarantined(const std::string &n, const std::filesystem::path &r) {
    try {
        HardwareProcessLease x(n, std::chrono::milliseconds(0), r);
        return false;
    } catch (const TransportError &e) {
        return e.Category() == "camera_control_delegation_quarantined";
    }
}
template<class Action> bool Rejects(Action action) {
    try { action(); } catch (const TransportError&) { return true; }
    return false;
}
} // namespace
int main(int argc, char **argv) {
    const auto root = argc == 4 ? std::filesystem::path(argv[3]) : Root();
    const auto name = argc == 4 ? std::string(argv[2]) : Name();
    if (argc == 4 && std::string_view(argv[1]) == "--crash") {
        HardwareProcessLease x(name, std::chrono::milliseconds(0), root);
        x.ArmDualDelegation();
        ExitProcess(91);
    }
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
        return 2;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --crash " + std::wstring(name.begin(), name.end()) +
                       L" \"" + root.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi))
        return 2;
    const auto wait = WaitForSingleObject(pi.hProcess, 5000);
    DWORD exit_code{};
    const bool exited = wait == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // Never remove the fixture marker while the child may still be alive.
    if (!exited)
        return 3;
    Check(exit_code == 91, "child must arm before abrupt exit");
    Check(Quarantined(name, root), "crashed child marker quarantines next owner");
    DWORD sid{};
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    auto marker = root / (L"armed-session-" + std::to_wstring(sid) + L".marker");
    if (!DeleteFileW(marker.c_str()))
        return 4;
    {
        HardwareProcessLease x(name, std::chrono::milliseconds(0), root);
        x.ArmDualDelegation();
        Check(x.DualDelegationArmed(), "arm");
        Check(Quarantined(name, root), "same thread must reject armed nested lease");
        DualDelegationCloseEvidence bad{};
        try {
            x.DisarmDualDelegation(bad);
            Check(false, "bad evidence");
        } catch (const TransportError &) {
        }
        Check(x.DualDelegationArmed(), "bad evidence retains marker");
    }
    Check(Quarantined(name, root), "destructor retains marker");
    // Test-only fixture cleanup; this is deliberately not a production recovery API.
    DeleteFileW(marker.c_str());
    RemoveDirectoryW(root.c_str());
    {
        HardwareProcessLease x(name, std::chrono::milliseconds(0), root);
        x.ArmDualDelegation();
        DualDelegationCloseEvidence ok{{true, true, true, true}, {true, true, true, true}};
        x.DisarmDualDelegation(ok);
        Check(!x.DualDelegationArmed(), "typed evidence disarms");
    }
    const DualDelegationCloseEvidence complete{{true,true,true,true},{true,true,true,true}};
    for (int missing = 0; missing < 8; ++missing) {
        {
            HardwareProcessLease lease(name, std::chrono::milliseconds(0), root);
            lease.ArmDualDelegation();
            auto evidence = complete;
            auto& body = missing < 4 ? evidence.camera_a : evidence.camera_b;
            switch (missing % 4) {
                case 0: body.live_view_off = false; break;
                case 1: body.source_closed = false; break;
                case 2: body.module_closed = false; break;
                case 3: body.worker_reaped = false; break;
            }
            Check(Rejects([&] { lease.DisarmDualDelegation(evidence); }), "each missing receipt rejects");
            Check(Rejects([&] { lease.DisarmDualDelegation(complete); }), "failed disarm is terminal");
        }
        Check(Quarantined(name, root), "missing receipt preserves cross-instance quarantine");
        if (!DeleteFileW(marker.c_str())) return 4;
    }
    {
        HardwareProcessLease lease(name, std::chrono::milliseconds(0), root);
        lease.ArmDualDelegation();
        if (!SetFileAttributesW(marker.c_str(), FILE_ATTRIBUTE_READONLY)) return 4;
        Check(Rejects([&] { lease.DisarmDualDelegation(complete); }), "delete failure rejects");
        if (!SetFileAttributesW(marker.c_str(), FILE_ATTRIBUTE_NORMAL)) return 4;
        Check(Rejects([&] { lease.DisarmDualDelegation(complete); }), "delete failure cannot be retried");
    }
    Check(Quarantined(name, root), "delete failure retains marker");
    if (!DeleteFileW(marker.c_str())) return 4;
    Check(Rejects([&] {
        HardwareProcessLease lease("A0CameraStitcher.Phase0.CameraControl.v1",
            std::chrono::milliseconds(0), root);
    }), "production marker location cannot be overridden");
    Check(Rejects([&] {
        HardwareProcessLease lease(name, std::chrono::milliseconds(0), std::filesystem::path(L"relative"));
    }), "relative marker location rejects");
    if (!RemoveDirectoryW(root.c_str())) return 4;
    std::cout << "{\"mode\":\"simulation\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
