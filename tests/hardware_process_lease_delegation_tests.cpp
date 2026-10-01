#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"
#include <Windows.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

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
#ifdef A0_MARKER_DIAGNOSTIC_TEST_ROOT
bool DiagnosticCliAcceptsTestRoot(const std::filesystem::path &root, std::string &output,
                                  bool &child_finished) {
    child_finished = true;
    wchar_t own_path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, own_path, MAX_PATH)) return false;
    const auto cli = std::filesystem::path(own_path).parent_path() / L"A0CameraStitcher.MarkerDiagnostic.exe";
    std::wstring command = L"\"" + cli.wstring() + L"\" --test-root \"" + root.wstring() + L"\"";
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE reader{}, writer{};
    if (!CreatePipe(&reader, &writer, &security, 0)) return false;
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writer;
    startup.hStdError = writer;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child)) {
        CloseHandle(reader);
        CloseHandle(writer);
        return false;
    }
    CloseHandle(writer);
    CloseHandle(child.hThread);
    const DWORD wait = WaitForSingleObject(child.hProcess, 5000);
    child_finished = wait == WAIT_OBJECT_0;
    DWORD exit_code{};
    const bool accepted = child_finished && GetExitCodeProcess(child.hProcess, &exit_code) &&
                          exit_code == 0;
    CloseHandle(child.hProcess);
    char bytes[512]{};
    DWORD read{};
    if (accepted && ReadFile(reader, bytes, sizeof(bytes), &read, nullptr)) output.assign(bytes, read);
    CloseHandle(reader);
    return accepted;
}
#endif
int RunDiagnosticTests() {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 2;
    const auto root = std::filesystem::path(temp) /
        (L"A0MarkerDiagnosticTest-" + std::to_wstring(GetCurrentProcessId()));
    if (!CreateDirectoryW(root.c_str(), nullptr)) return 2;
    DWORD sid{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sid)) return 2;
    const auto marker = root / (L"armed-session-" + std::to_wstring(sid) + L".marker");
    const auto other = root / L"armed-session-4294967295.marker";
    const auto write = [](const std::filesystem::path &path, const std::string &content) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written{};
        const bool ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                        written == content.size();
        CloseHandle(file);
        return ok;
    };
    const std::string header = "a0-dual-delegation-v2\nnonce=" + std::string(32, 'a') +
        "\nownerPid=";
    const std::string tail = "\nepoch=" + std::string(32, 'b') +
        "\nworkerAPid=4294967289\nworkerBPid=4294967288\n";
    const auto absent_root = root / L"not-created";
    Check(InspectDualDelegationMarkerReadOnly(absent_root).status == "marker_root_untrusted" &&
          !std::filesystem::exists(absent_root), "diagnostic must not create a missing root");
    Check(InspectDualDelegationMarkerReadOnly(root).status == "marker_missing", "missing marker is read-only");
    const auto valid = header + "4294967290" + tail;
    if (!write(marker, valid)) return 3;
    const auto timestamp = std::filesystem::last_write_time(marker);
    const auto accepted = InspectDualDelegationMarkerReadOnly(root);
    Check(accepted.status == "eligible_for_human_review" && accepted.anonymous_sha256.size() == 64 &&
          accepted.size == valid.size(), "valid marker only becomes a human-review candidate");
#ifdef A0_MARKER_DIAGNOSTIC_TEST_ROOT
    std::string cli_output;
    bool cli_finished{};
    Check(DiagnosticCliAcceptsTestRoot(root, cli_output, cli_finished) &&
          cli_output.find("\"status\":\"eligible_for_human_review\"") != std::string::npos &&
          cli_output.find(accepted.anonymous_sha256) != std::string::npos &&
          cli_output.find("nonce=") == std::string::npos &&
          cli_output.find(std::string(32, 'a')) == std::string::npos &&
          cli_output.find("ownerPid") == std::string::npos &&
          cli_output.find("4294967290") == std::string::npos,
          "read-only CLI must report only anonymous synthetic marker status");
    if (!cli_finished) return 3; // Never clean a fixture while a diagnostic child may still be alive.
#endif
    Check(GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES &&
          std::filesystem::file_size(marker) == valid.size() &&
          std::filesystem::last_write_time(marker) == timestamp, "diagnostic must retain marker unchanged");
    if (!write(other, valid)) return 3;
    Check(InspectDualDelegationMarkerReadOnly(root).status == "marker_ambiguous",
          "multiple session markers must stop");
    if (!DeleteFileW(other.c_str())) return 3;
    if (!write(marker, "invalid\n")) return 3;
    Check(InspectDualDelegationMarkerReadOnly(root).status == "marker_invalid",
          "invalid syntax must stop");
    if (!write(marker, std::string(256, 'x'))) return 3;
    Check(InspectDualDelegationMarkerReadOnly(root).status == "marker_invalid",
          "oversized marker must stop");
    if (!write(marker, header + std::to_string(GetCurrentProcessId()) + tail)) return 3;
    Check(InspectDualDelegationMarkerReadOnly(root).status == "process_active_or_unknown",
          "live owner PID must stop without asserting historical identity");
    if (!DeleteFileW(marker.c_str())) return 3;
    if (!CreateDirectoryW(marker.c_str(), nullptr)) return 3;
    Check(InspectDualDelegationMarkerReadOnly(root).status == "marker_invalid",
          "directory in place of marker must stop");
    if (!RemoveDirectoryW(marker.c_str()) || !RemoveDirectoryW(root.c_str())) return 3;
    std::cout << "{\"mode\":\"diagnostic-simulation\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
} // namespace
int RunMain(int argc, char **argv);
// Surface uncaught non-TransportError exceptions as a readable failure instead of
// an abort() with no output, so a CTest failure records the reason.
int main(int argc, char **argv) {
    try {
        return RunMain(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << "uncaught exception: " << e.what() << '\n';
        return 99;
    }
}
int RunMain(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--clean-exit") return 0;
    if (argc == 2 && std::string_view(argv[1]) == "--diagnostic") return RunDiagnosticTests();
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
    HANDLE clean[2]{};
    for (auto& handle : clean) {
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --clean-exit";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &child)) return 2;
        CloseHandle(child.hThread);
        handle = child.hProcess;
        if (WaitForSingleObject(handle, 5000) != WAIT_OBJECT_0) return 3;
    }
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
        DualDelegationCloseEvidence ok{{true, true, true, clean[0]}, {true, true, true, clean[1]}};
        x.RegisterDualWorkers(clean[0], clean[1]);
        x.DisarmDualDelegation(ok);
        Check(!x.DualDelegationArmed(), "OS-verified clean exits disarm");
    }
    const DualDelegationCloseEvidence complete{{true,true,true,clean[0]},{true,true,true,clean[1]}};
    for (int missing = 0; missing < 8; ++missing) {
        {
            HardwareProcessLease lease(name, std::chrono::milliseconds(0), root);
            lease.ArmDualDelegation();
            auto evidence = complete;
            lease.RegisterDualWorkers(clean[0], clean[1]);
            auto& body = missing < 4 ? evidence.camera_a : evidence.camera_b;
            switch (missing % 4) {
                case 0: body.live_view_off = false; break;
                case 1: body.source_closed = false; break;
                case 2: body.module_closed = false; break;
                case 3: body.worker_process = nullptr; break;
            }
            Check(Rejects([&] { lease.DisarmDualDelegation(evidence); }), "each missing receipt rejects");
            Check(Rejects([&] { lease.DisarmDualDelegation(complete); }), "failed disarm is terminal");
        }
        Check(Quarantined(name, root), "missing receipt preserves cross-instance quarantine");
        if (!DeleteFileW(marker.c_str())) return 4;
    }
    HANDLE event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!event) return 2;
    for (auto invalid : {GetCurrentProcess(), pi.hProcess, clean[1], event}) {
        {
            HardwareProcessLease lease(name, std::chrono::milliseconds(0), root);
            lease.ArmDualDelegation();
            auto evidence = complete;
            evidence.camera_a.worker_process = invalid;
            lease.RegisterDualWorkers(invalid == pi.hProcess ? pi.hProcess : clean[0], clean[1]);
            Check(Rejects([&] { lease.DisarmDualDelegation(evidence); }),
                  "running, crashed, duplicate, or non-process handle must reject");
            Check(Rejects([&] { lease.DisarmDualDelegation(complete); }), "OS failure cannot retry");
        }
        Check(Quarantined(name, root), "OS failure retains quarantine");
        if (!DeleteFileW(marker.c_str())) return 4;
    }
    CloseHandle(event);
    CloseHandle(pi.hProcess);
    for (int invalid = 0; invalid < 3; ++invalid) {
        {
            HardwareProcessLease lease(name, std::chrono::milliseconds(0), root);
            lease.ArmDualDelegation();
            if (invalid == 1) lease.RegisterDualWorkers(clean[1], clean[0]);
            if (invalid == 2) {
                lease.RegisterDualWorkers(clean[0], clean[1]);
                Check(Rejects([&] { lease.RegisterDualWorkers(clean[0], clean[1]); }),
                      "second registration rejects");
            }
            Check(Rejects([&] { lease.DisarmDualDelegation(complete); }),
                  "missing, swapped, or repeated registration cannot disarm");
        }
        Check(Quarantined(name, root), "registration failure retains quarantine");
        if (!DeleteFileW(marker.c_str())) return 4;
    }
    {
        HardwareProcessLease lease(name, std::chrono::milliseconds(0), root);
        lease.ArmDualDelegation();
        // Register before making the marker read-only: the v2 marker rewrite in
        // RegisterDualWorkers needs write access. This case targets the delete step.
        lease.RegisterDualWorkers(clean[0], clean[1]);
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
    for (auto handle : clean) CloseHandle(handle);
    std::cout << "{\"mode\":\"simulation\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
