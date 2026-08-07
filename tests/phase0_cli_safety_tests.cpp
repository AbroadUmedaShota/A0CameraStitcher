#include "a0/phase0/cli_safety.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"

#include <Windows.h>

#include <atomic>
#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace a0::phase0;

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void TestDirectCaptureCommandsAreFakeOnly() {
    for (const std::string command : {"capture-single", "capture-pair", "stability"}) {
        Check(!ValidateDirectCaptureSafety(command, "fake", false),
            "fake direct capture contract should remain available");
        Check(ValidateDirectCaptureSafety(command, "wpd", false).has_value(),
            "direct WPD capture must be rejected");
        Check(ValidateDirectCaptureSafety(command, "sdk", false).has_value(),
            "direct SDK capture must be rejected");
        Check(ValidateDirectCaptureSafety(command, "fake", true).has_value(),
            "legacy operator gate must not reopen a direct hardware path");
    }
    Check(!ValidateDirectCaptureSafety("hybrid-capture-single", "sdk", false),
        "the approved hybrid command should not be classified as legacy direct capture");
}

void TestHardwareCommandClassification() {
    Check(RequiresHardwareProcessLease("inventory", "sdk"), "real SDK inventory needs the process lease");
    Check(RequiresHardwareProcessLease("wpd-status", "wpd"), "real WPD status needs the process lease");
    Check(RequiresHardwareProcessLease("live-view", "sdk"), "Live View needs the process lease");
    Check(RequiresHardwareProcessLease("hybrid-capture-single", "sdk"),
        "hybrid capture needs the process lease");
    Check(!RequiresHardwareProcessLease("inventory", "fake"), "fake inventory must not need hardware lease");
    Check(!RequiresHardwareProcessLease("preflight", "wpd"), "preflight does not open a camera session");
    Check(!RequiresHardwareProcessLease("report", "wpd"), "report generation does not open a camera session");
    Check(!RequiresHardwareProcessLease("unknown", "wpd"), "unknown commands must not acquire hardware lease");
}

void TestProcessLeaseRejectsConcurrentOwner() {
    const std::string lease_name = "A0CameraStitcher.Phase0.Test." + std::to_string(GetCurrentProcessId());
    {
        HardwareProcessLease owner(lease_name);
        std::atomic<bool> rejected{false};
        std::thread contender([&] {
            try {
                HardwareProcessLease duplicate(lease_name);
            } catch (const TransportError& error) {
                rejected = error.Category() == "camera_control_busy";
            }
        });
        contender.join();
        Check(rejected.load(), "a concurrent thread must not acquire the process-wide hardware lease");
    }

    bool reacquired = false;
    try {
        HardwareProcessLease next_owner(lease_name);
        reacquired = true;
    } catch (...) {
    }
    Check(reacquired, "the hardware lease must be released when its owner exits scope");
}

std::wstring ToWide(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

int HoldLeaseForParent(int argc, char** argv) {
    if (argc != 5) return 2;

    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ToWide(argv[3]).c_str());
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, ToWide(argv[4]).c_str());
    if (ready == nullptr || stop == nullptr) {
        if (ready != nullptr) CloseHandle(ready);
        if (stop != nullptr) CloseHandle(stop);
        return 3;
    }

    try {
        HardwareProcessLease owner(argv[2]);
        const bool signaled = SetEvent(ready) != FALSE;
        const DWORD stopped = signaled ? WaitForSingleObject(stop, 10'000) : WAIT_FAILED;
        CloseHandle(ready);
        CloseHandle(stop);
        return stopped == WAIT_OBJECT_0 ? 0 : 4;
    } catch (...) {
        CloseHandle(ready);
        CloseHandle(stop);
        return 5;
    }
}

void TestProcessLeaseRejectsSeparateProcess() {
    const std::string suffix = std::to_string(GetCurrentProcessId());
    const std::string lease_name = "A0CameraStitcher.Phase0.ProcessTest." + suffix;
    const std::wstring ready_name = L"Local\\A0CameraStitcher.Phase0.ProcessTest.Ready." + ToWide(suffix);
    const std::wstring stop_name = L"Local\\A0CameraStitcher.Phase0.ProcessTest.Stop." + ToWide(suffix);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());
    Check(ready != nullptr && stop != nullptr, "cross-process test events must be created");
    if (ready == nullptr || stop == nullptr) {
        if (ready != nullptr) CloseHandle(ready);
        if (stop != nullptr) CloseHandle(stop);
        return;
    }

    std::array<wchar_t, MAX_PATH> executable{};
    const DWORD executable_length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Check(executable_length != 0 && executable_length < executable.size(),
        "test executable path must fit in MAX_PATH");
    if (executable_length == 0 || executable_length >= executable.size()) {
        CloseHandle(ready);
        CloseHandle(stop);
        return;
    }

    std::wstring command_line = L"\"" + std::wstring(executable.data(), executable_length) + L"\" --hold-lease " +
        ToWide(lease_name) + L" " + ready_name + L" " + stop_name;
    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    const BOOL created = CreateProcessW(
        nullptr,
        mutable_command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &child);
    Check(created != FALSE, "lease child process must start");
    if (created == FALSE) {
        CloseHandle(ready);
        CloseHandle(stop);
        return;
    }

    const DWORD ready_result = WaitForSingleObject(ready, 5'000);
    Check(ready_result == WAIT_OBJECT_0, "lease child must signal that it owns the mutex");
    bool rejected = false;
    if (ready_result == WAIT_OBJECT_0) {
        try {
            HardwareProcessLease duplicate(lease_name);
        } catch (const TransportError& error) {
            rejected = error.Category() == "camera_control_busy";
        }
    }
    Check(rejected, "a separate process must be rejected while the camera-control lease is held");

    Check(SetEvent(stop) != FALSE, "lease child must receive the stop signal");
    const DWORD child_result = WaitForSingleObject(child.hProcess, 5'000);
    Check(child_result == WAIT_OBJECT_0, "lease child must exit after the stop signal");
    if (child_result == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        Check(GetExitCodeProcess(child.hProcess, &exit_code) != FALSE && exit_code == 0,
            "lease child must exit successfully");
    } else {
        TerminateProcess(child.hProcess, 1);
    }

    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    CloseHandle(ready);
    CloseHandle(stop);

    bool reacquired = false;
    try {
        HardwareProcessLease next_owner(lease_name);
        reacquired = true;
    } catch (...) {
    }
    Check(reacquired, "the separate-process lease must be released when the child exits");
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--hold-lease") {
        return HoldLeaseForParent(argc, argv);
    }
    TestDirectCaptureCommandsAreFakeOnly();
    TestHardwareCommandClassification();
    TestProcessLeaseRejectsConcurrentOwner();
    TestProcessLeaseRejectsSeparateProcess();
    if (failures != 0) {
        std::cerr << failures << " CLI safety assertion(s) failed\n";
        return 1;
    }
    std::cout << "All Phase 0 CLI safety contracts passed\n";
    return 0;
}
