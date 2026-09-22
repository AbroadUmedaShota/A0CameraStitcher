#include "a0/phase0/hardware_process_lease.hpp"

#include "a0/phase0/phase0.hpp"

#include <ShlObj.h>
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>

namespace a0::phase0 {
namespace {

std::string WindowsError(std::string_view operation, DWORD error);

bool IsSafeLeaseName(std::string_view value) noexcept {
    if (value.empty() || value.size() > 120)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' || character == '-' || character == '_';
    });
}
constexpr std::string_view kProductionLeaseName = "A0CameraStitcher.Phase0.CameraControl.v1";
bool IsTestLeaseName(std::string_view value) noexcept {
    return value.starts_with("A0.Poc.TestLease.") || value.starts_with("A0CameraStitcher.Phase0.Test.");
}
std::wstring ToWide(std::string_view value) {
    return {value.begin(), value.end()};
}
void RequireSafeDirectoryTree(const std::filesystem::path &root) {
    const auto drive = root.root_name().wstring();
    if (!root.is_absolute() || drive.size() != 2 || drive[1] != L':' ||
        GetDriveTypeW(root.root_path().c_str()) != DRIVE_FIXED)
        throw TransportError("camera_control_marker_failed", "marker root must be on a fixed local drive");
    std::filesystem::path current = root.root_path();
    const DWORD root_attributes = GetFileAttributesW(current.c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES ||
        (root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        throw TransportError("camera_control_marker_failed", "marker drive root is not trusted");
    for (const auto &part : root.relative_path()) {
        if (part == L".." || part == L".")
            throw TransportError("camera_control_marker_failed", "marker path must be normalized");
        current /= part;
        DWORD a = GetFileAttributesW(current.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                throw TransportError("camera_control_marker_failed",
                                     WindowsError("CreateDirectoryW", GetLastError()));
            a = GetFileAttributesW(current.c_str());
        }
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            throw TransportError("camera_control_marker_failed",
                                 "marker ancestor is not a trusted directory");
    }
}
std::filesystem::path ProductionMarkerRoot() {
    PWSTR raw{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)) || raw == nullptr)
        throw TransportError("camera_control_marker_failed", "could not resolve LocalAppData");
    std::filesystem::path root =
        std::filesystem::path(raw) / L"A0CameraStitcher" / L"Phase0" / L"DualDelegation";
    CoTaskMemFree(raw);
    return root;
}
std::wstring MarkerPath(const std::filesystem::path &root) {
    DWORD sid{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sid))
        throw TransportError("camera_control_marker_failed",
                             WindowsError("ProcessIdToSessionId", GetLastError()));
    return (root / (L"armed-session-" + std::to_wstring(sid) + L".marker")).wstring();
}
std::string ReadMarker(const std::wstring &path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT))
        throw TransportError("camera_control_marker_failed", "marker reread is not a regular file");
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        throw TransportError("camera_control_marker_failed", WindowsError("CreateFileW", GetLastError()));
    if (GetFileType(f) != FILE_TYPE_DISK) {
        CloseHandle(f);
        throw TransportError("camera_control_marker_failed", "marker reread is not a regular disk file");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 1 || size.QuadPart > 255) {
        CloseHandle(f);
        throw TransportError("camera_control_marker_failed", "marker size is invalid");
    }
    std::array<char, 256> b{};
    DWORD n{};
    BOOL ok = ReadFile(f, b.data(), static_cast<DWORD>(b.size()), &n, nullptr);
    CloseHandle(f);
    if (!ok)
        throw TransportError("camera_control_marker_failed", WindowsError("ReadFile", GetLastError()));
    return {b.data(), n};
}
void RejectMarker(const std::wstring &path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return;
        throw TransportError("camera_control_delegation_quarantined", WindowsError("GetFileAttributesW", e));
    }
    if ((a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT))
        throw TransportError("camera_control_delegation_quarantined",
                             "delegation marker is not a regular file");
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        throw TransportError("camera_control_delegation_quarantined",
                             WindowsError("CreateFileW", GetLastError()));
    DWORD t = GetFileType(f);
    CloseHandle(f);
    if (t != FILE_TYPE_DISK)
        throw TransportError("camera_control_delegation_quarantined",
                             "delegation marker is not a regular disk file");
    throw TransportError("camera_control_delegation_quarantined",
                         "dual delegation marker is armed; human recovery required");
}

std::string WindowsError(std::string_view operation, DWORD error) {
    std::ostringstream message;
    message << operation << " failed with Windows error " << error;
    return message.str();
}

} // namespace

HardwareProcessLease::HardwareProcessLease(std::string_view lease_name, std::chrono::milliseconds wait)
    : HardwareProcessLease(lease_name, wait, {}) {
}
HardwareProcessLease::HardwareProcessLease(std::string_view lease_name, std::chrono::milliseconds wait,
                                           const std::filesystem::path &test_marker_root) {
    if (!IsSafeLeaseName(lease_name)) {
        throw TransportError("camera_control_lock_failed", "camera-control lease name is invalid");
    }
    if (wait < std::chrono::milliseconds::zero()) {
        throw TransportError("camera_control_lock_failed", "camera-control lease wait must not be negative");
    }

    if (!test_marker_root.empty() && (lease_name == kProductionLeaseName || !IsTestLeaseName(lease_name)))
        throw TransportError("camera_control_marker_failed", "test marker root requires test lease name");
    durable_marker_enabled_ = lease_name == kProductionLeaseName || !test_marker_root.empty();
    if (durable_marker_enabled_) {
        const auto root = test_marker_root.empty() ? ProductionMarkerRoot() : test_marker_root;
        RequireSafeDirectoryTree(root);
        marker_path_ = MarkerPath(root);
    }
    const std::wstring wide_name = ToWide(lease_name);
    const std::wstring mutex_name = L"Local\\" + wide_name;
    HANDLE handle = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (handle == nullptr) {
        throw TransportError("camera_control_lock_failed", WindowsError("CreateMutexW", GetLastError()));
    }
    handle_ = handle;

    const auto bounded_wait =
        std::min<std::uint64_t>(static_cast<std::uint64_t>(wait.count()),
                                static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max() - 1U));
    const DWORD result = WaitForSingleObject(handle, static_cast<DWORD>(bounded_wait));
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        owned_ = true;
        owner_thread_id_ = GetCurrentThreadId();
        recovered_abandoned_owner_ = result == WAIT_ABANDONED;
        if (durable_marker_enabled_) {
            try {
                RejectMarker(marker_path_);
            } catch (...) {
                ReleaseMutex(handle);
                owned_ = false;
                CloseHandle(handle);
                handle_ = nullptr;
                throw;
            }
        }
        return;
    }

    CloseHandle(handle);
    handle_ = nullptr;
    if (result == WAIT_TIMEOUT) {
        throw TransportError("camera_control_busy", "another Phase 0 process owns the camera-control lease");
    }
    throw TransportError("camera_control_lock_failed", WindowsError("WaitForSingleObject", GetLastError()));
}

HardwareProcessLease::~HardwareProcessLease() {
    HANDLE handle = static_cast<HANDLE>(handle_);
    if (handle == nullptr)
        return;
    if (owned_)
        ReleaseMutex(handle);
    CloseHandle(handle);
}

bool HardwareProcessLease::RecoveredAbandonedOwner() const noexcept {
    return recovered_abandoned_owner_;
}
void HardwareProcessLease::ArmDualDelegation() {
    if (!durable_marker_enabled_ || !owned_ || owner_thread_id_ != GetCurrentThreadId() ||
        delegation_ever_armed_ || delegation_disarm_failed_)
        throw TransportError("camera_control_marker_failed", "delegation cannot arm");
    delegation_ever_armed_ = true;
    RequireSafeDirectoryTree(std::filesystem::path(marker_path_).parent_path());
    std::array<unsigned char, 16> nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw TransportError("camera_control_marker_failed", "nonce generation failed");
    static constexpr char hex[] = "0123456789abcdef";
    marker_contents_ = "a0-dual-delegation-v1\nnonce=";
    for (auto b : nonce) {
        marker_contents_ += hex[b >> 4];
        marker_contents_ += hex[b & 15];
    }
    marker_contents_ += "\n";
    HANDLE f = CreateFileW(marker_path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        throw TransportError("camera_control_marker_failed", WindowsError("CreateFileW", GetLastError()));
    DWORD n{};
    BOOL ok =
        WriteFile(f, marker_contents_.data(), static_cast<DWORD>(marker_contents_.size()), &n, nullptr) &&
        n == marker_contents_.size() && FlushFileBuffers(f);
    DWORD e = GetLastError();
    CloseHandle(f);
    if (!ok)
        throw TransportError("camera_control_marker_failed", WindowsError("WriteFile/FlushFileBuffers", e));
    RequireSafeDirectoryTree(std::filesystem::path(marker_path_).parent_path());
    if (ReadMarker(marker_path_) != marker_contents_)
        throw TransportError("camera_control_marker_failed", "marker reread mismatch");
    delegation_armed_ = true;
}
void HardwareProcessLease::DisarmDualDelegation(const DualDelegationCloseEvidence &evidence) {
    try {
        if (!durable_marker_enabled_ || !owned_ || owner_thread_id_ != GetCurrentThreadId() ||
            !delegation_armed_ || delegation_disarm_failed_ || !evidence.Complete())
            throw TransportError("camera_control_marker_failed",
                                 "complete typed close evidence required; marker remains armed");
        RequireSafeDirectoryTree(std::filesystem::path(marker_path_).parent_path());
        if (ReadMarker(marker_path_) != marker_contents_)
            throw TransportError("camera_control_marker_failed",
                                 "marker instance mismatch; marker remains armed");
        if (!DeleteFileW(marker_path_.c_str()))
            throw TransportError("camera_control_marker_failed", WindowsError("DeleteFileW", GetLastError()));
        delegation_armed_ = false;
    } catch (...) {
        delegation_disarm_failed_ = true;
        throw;
    }
}
bool HardwareProcessLease::DualDelegationArmed() const noexcept {
    return delegation_armed_;
}

} // namespace a0::phase0
