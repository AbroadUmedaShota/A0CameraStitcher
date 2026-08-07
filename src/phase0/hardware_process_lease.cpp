#include "a0/phase0/hardware_process_lease.hpp"

#include "a0/phase0/phase0.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <string>

namespace a0::phase0 {
namespace {

bool IsSafeLeaseName(std::string_view value) noexcept {
    if (value.empty() || value.size() > 120) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' || character == '-' || character == '_';
    });
}

std::string WindowsError(std::string_view operation, DWORD error) {
    std::ostringstream message;
    message << operation << " failed with Windows error " << error;
    return message.str();
}

} // namespace

HardwareProcessLease::HardwareProcessLease(
    std::string_view lease_name,
    std::chrono::milliseconds wait) {
    if (!IsSafeLeaseName(lease_name)) {
        throw TransportError("camera_control_lock_failed", "camera-control lease name is invalid");
    }
    if (wait < std::chrono::milliseconds::zero()) {
        throw TransportError("camera_control_lock_failed", "camera-control lease wait must not be negative");
    }

    const std::wstring wide_name(lease_name.begin(), lease_name.end());
    const std::wstring mutex_name = L"Local\\" + wide_name;
    HANDLE handle = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (handle == nullptr) {
        throw TransportError(
            "camera_control_lock_failed",
            WindowsError("CreateMutexW", GetLastError()));
    }
    handle_ = handle;

    const auto bounded_wait = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(wait.count()),
        static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max() - 1U));
    const DWORD result = WaitForSingleObject(handle, static_cast<DWORD>(bounded_wait));
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        owned_ = true;
        recovered_abandoned_owner_ = result == WAIT_ABANDONED;
        return;
    }

    CloseHandle(handle);
    handle_ = nullptr;
    if (result == WAIT_TIMEOUT) {
        throw TransportError(
            "camera_control_busy",
            "another Phase 0 process owns the camera-control lease");
    }
    throw TransportError(
        "camera_control_lock_failed",
        WindowsError("WaitForSingleObject", GetLastError()));
}

HardwareProcessLease::~HardwareProcessLease() {
    HANDLE handle = static_cast<HANDLE>(handle_);
    if (handle == nullptr) return;
    if (owned_) ReleaseMutex(handle);
    CloseHandle(handle);
}

bool HardwareProcessLease::RecoveredAbandonedOwner() const noexcept {
    return recovered_abandoned_owner_;
}

} // namespace a0::phase0
