#include "a0/phase0/hardware_process_lease.hpp"

#include "a0/phase0/phase0.hpp"

#include <ShlObj.h>
#include <Windows.h>
#include <SetupAPI.h>
#include <TlHelp32.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <ctime>
#include <filesystem>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

struct MarkerCandidateWalk final {
    unsigned count{};
    std::wstring first_name;
    bool enumeration_not_found{};
    bool enumeration_clean{};
};
// Single owner of the Win32 FindFirstFileW/FindNextFileW walk over
// `<root>/armed-session-*.marker`, shared by the read-only diagnostic (which
// needs the exact-match detail for its single candidate) and the lease's
// fail-closed cross-session guard below (which only needs "did anything
// match at all"), so the two call sites cannot drift apart on what counts as
// a match. Stops once `max_entries` matches have been observed.
MarkerCandidateWalk WalkMarkerCandidates(const std::filesystem::path &root, unsigned max_entries) {
    MarkerCandidateWalk walk;
    WIN32_FIND_DATAW entry{};
    const std::wstring pattern = (root / L"armed-session-*.marker").wstring();
    HANDLE found = FindFirstFileW(pattern.c_str(), &entry);
    if (found == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        walk.enumeration_not_found = error == ERROR_FILE_NOT_FOUND;
        walk.enumeration_clean = walk.enumeration_not_found;
        return walk;
    }
    do {
        if (walk.count == 0) walk.first_name = entry.cFileName;
        ++walk.count;
    } while (walk.count < max_entries && FindNextFileW(found, &entry));
    const DWORD enumeration_error = GetLastError();
    FindClose(found);
    walk.enumeration_clean = walk.count >= max_entries || enumeration_error == ERROR_NO_MORE_FILES;
    return walk;
}
// Fail-closed cross-session guard for lease acquisition: a Windows session ID
// change (sign-out/sign-in, reboot) must never let a stranded delegation
// marker go unnoticed just because it no longer matches *this* session's
// marker filename. Any armed-session-*.marker candidate under root -- another
// session's marker, this session's own leftover from a crashed owner, a
// directory or reparse point placed at a matching name, or a name that only
// satisfies the glob without being the canonical armed-session-<digits>.marker
// form -- blocks lease acquisition the same way: none of these are silently
// skipped. An enumeration error other than "nothing matched" is treated the
// same way, since it could be hiding a marker this process cannot otherwise
// observe. Different Windows user profiles remain out of the Phase 0 lease's
// contract; this guard only covers sessions sharing the current user's
// marker root.
void RejectAnyArmedSessionMarker(const std::filesystem::path &root) {
    const auto walk = WalkMarkerCandidates(root, 1);
    if (walk.count == 0 && walk.enumeration_not_found)
        return;
    if (walk.count == 0) {
        throw TransportError("camera_control_delegation_quarantined",
                             "could not enumerate the delegation marker root; treating as quarantined");
    }
    throw TransportError("camera_control_delegation_quarantined",
                         "a dual-delegation marker is present in the marker root; human recovery required");
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
                // Re-validate the root immediately before the fail-closed
                // cross-session scan (defense in depth against the gap since
                // the root was first validated, before the mutex wait above),
                // then reject on ANY matching marker from ANY session before
                // falling back to the original same-session-only check.
                const auto root = std::filesystem::path(marker_path_).parent_path();
                RequireSafeDirectoryTree(root);
                RejectAnyArmedSessionMarker(root);
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
    if (worker_a_) CloseHandle(static_cast<HANDLE>(worker_a_));
    if (worker_b_) CloseHandle(static_cast<HANDLE>(worker_b_));
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
    std::array<unsigned char, 16> nonce_bytes{};
    if (BCryptGenRandom(nullptr, nonce_bytes.data(), static_cast<ULONG>(nonce_bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw TransportError("camera_control_marker_failed", "nonce generation failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string nonce;
    nonce.reserve(nonce_bytes.size() * 2);
    for (const auto byte : nonce_bytes) {
        nonce += hex[byte >> 4];
        nonce += hex[byte & 15];
    }
    // The durable record starts incomplete. It is deliberately a quarantine
    // record until RegisterDualWorkers atomically proves both child identities.
    owner_process_id_ = GetCurrentProcessId();
    marker_contents_ = "a0-dual-delegation-v2\nnonce=" + nonce + "\nownerPid=" +
        std::to_string(owner_process_id_) + "\nepoch=" + std::string(32, '0') +
        "\nworkerAPid=1\nworkerBPid=2\n";
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
bool IsLowerHex(std::string_view value, std::size_t exact_size) noexcept {
    return value.size() == exact_size && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}
bool ExistingSafeDirectoryTree(const std::filesystem::path &root) {
    const auto drive = root.root_name().wstring();
    if (!root.is_absolute() || root.lexically_normal() != root || drive.size() != 2 || drive[1] != L':' ||
        GetDriveTypeW(root.root_path().c_str()) != DRIVE_FIXED)
        return false;
    std::filesystem::path current = root.root_path();
    const auto safe_directory = [](const std::filesystem::path &path) {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
               (a & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    };
    if (!safe_directory(current)) return false;
    for (const auto &part : root.relative_path()) {
        if (part == L".." || part == L".") return false;
        current /= part;
        if (!safe_directory(current)) return false;
    }
    return true;
}
bool ParseUnsigned(std::string_view value, DWORD &out) noexcept {
    unsigned long long parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !parsed || parsed > std::numeric_limits<DWORD>::max())
        return false;
    out = static_cast<DWORD>(parsed);
    return true;
}
struct DelegationMarker final {
    std::string nonce;
    DWORD owner_pid{};
    std::string epoch;
    DWORD worker_a_pid{};
    DWORD worker_b_pid{};
};
bool ParseDelegationMarker(std::string_view wire, DelegationMarker &marker) {
    // Keep this deliberately rigid: marker content is a small safety record,
    // not a forward-compatible configuration file.
    constexpr std::string_view header = "a0-dual-delegation-v2\n";
    if (!wire.starts_with(header) || !wire.ends_with('\n')) return false;
    const auto fields = wire.substr(header.size());
    const auto line = [&](std::string_view key, std::size_t &cursor, std::string_view &value) -> bool {
        if (!fields.substr(cursor).starts_with(key)) return false;
        cursor += key.size();
        const auto end = fields.find('\n', cursor);
        if (end == std::string_view::npos) return false;
        value = fields.substr(cursor, end - cursor);
        cursor = end + 1;
        return true;
    };
    std::size_t cursor{};
    std::string_view nonce, owner, epoch, worker_a, worker_b;
    if (!line("nonce=", cursor, nonce) || !line("ownerPid=", cursor, owner) ||
        !line("epoch=", cursor, epoch) || !line("workerAPid=", cursor, worker_a) ||
        !line("workerBPid=", cursor, worker_b) || cursor != fields.size() ||
        !IsLowerHex(nonce, 32) || !IsLowerHex(epoch, 32) || !ParseUnsigned(owner, marker.owner_pid) ||
        !ParseUnsigned(worker_a, marker.worker_a_pid) || !ParseUnsigned(worker_b, marker.worker_b_pid) ||
        marker.worker_a_pid == marker.worker_b_pid || marker.owner_pid == marker.worker_a_pid ||
        marker.owner_pid == marker.worker_b_pid)
        return false;
    marker.nonce.assign(nonce);
    marker.epoch.assign(epoch);
    return true;
}
struct DiagnosticSnapshot final {
    std::string contents;
    BY_HANDLE_FILE_INFORMATION info{};
};
bool ReadDiagnosticSnapshot(const std::wstring &path, DiagnosticSnapshot &snapshot) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool regular = GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &info) &&
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        info.nFileSizeHigh == 0 && info.nFileSizeLow >= 1 && info.nFileSizeLow <= 255;
    if (!regular) {
        CloseHandle(file);
        return false;
    }
    std::array<char, 256> bytes{};
    DWORD read{};
    const bool ok = ReadFile(file, bytes.data(), info.nFileSizeLow, &read, nullptr) &&
                    read == info.nFileSizeLow;
    CloseHandle(file);
    if (!ok) return false;
    snapshot.contents.assign(bytes.data(), read);
    snapshot.info = info;
    return true;
}
bool SameDiagnosticSnapshot(const DiagnosticSnapshot &a, const DiagnosticSnapshot &b) {
    const auto &x = a.info;
    const auto &y = b.info;
    return a.contents == b.contents && x.dwFileAttributes == y.dwFileAttributes &&
           x.nFileSizeHigh == y.nFileSizeHigh && x.nFileSizeLow == y.nFileSizeLow &&
           x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh &&
           x.nFileIndexLow == y.nFileIndexLow &&
           CompareFileTime(&x.ftLastWriteTime, &y.ftLastWriteTime) == 0;
}
bool ProcessAbsent(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) return GetLastError() == ERROR_INVALID_PARAMETER;
    const DWORD state = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return state == WAIT_OBJECT_0;
}
DualDelegationMarkerDiagnostic InspectDualDelegationMarkerReadOnly(const std::filesystem::path &test_marker_root) {
    // No lease, directory creation, worker start, SDK/WPD call, or marker write.
    const auto root = test_marker_root.empty() ? ProductionMarkerRoot() : test_marker_root;
    if (!ExistingSafeDirectoryTree(root)) return {"marker_root_untrusted"};
    const std::wstring expected = std::filesystem::path(MarkerPath(root)).filename().wstring();
    const auto walk = WalkMarkerCandidates(root, 2);
    if (walk.count == 0) {
        return walk.enumeration_not_found ? DualDelegationMarkerDiagnostic{"marker_missing"}
                                          : DualDelegationMarkerDiagnostic{"marker_unavailable"};
    }
    if (walk.count != 1 || walk.first_name != expected) return {"marker_ambiguous"};
    if (!walk.enumeration_clean) return {"marker_unavailable"};
    const std::wstring path = (root / expected).wstring();
    DiagnosticSnapshot first{}, second{};
    if (!ReadDiagnosticSnapshot(path, first)) return {"marker_invalid"};
    DelegationMarker parsed{};
    if (!ParseDelegationMarker(first.contents, parsed)) return {"marker_invalid"};
    if (!ExistingSafeDirectoryTree(root) || !ReadDiagnosticSnapshot(path, second) ||
        !SameDiagnosticSnapshot(first, second)) return {"marker_changed"};
    if (!ProcessAbsent(parsed.owner_pid) || !ProcessAbsent(parsed.worker_a_pid) ||
        !ProcessAbsent(parsed.worker_b_pid)) return {"process_active_or_unknown"};
    const std::vector<unsigned char> bytes(second.contents.begin(), second.contents.end());
    return {"eligible_for_human_review", Sha256Hex(bytes), second.contents.size()};
}
std::string MakeHexRandom(std::array<unsigned char, 16> &bytes) {
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw TransportError("camera_control_marker_failed", "nonce generation failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
std::string MakeMarkerContents(std::string_view nonce, DWORD owner_pid, std::string_view epoch,
                               DWORD worker_a_pid, DWORD worker_b_pid) {
    return "a0-dual-delegation-v2\nnonce=" + std::string(nonce) + "\nownerPid=" +
        std::to_string(owner_pid) + "\nepoch=" + std::string(epoch) + "\nworkerAPid=" +
        std::to_string(worker_a_pid) + "\nworkerBPid=" + std::to_string(worker_b_pid) + "\n";
}
void RewriteMarker(const std::wstring &path, std::string_view expected, std::string_view replacement) {
    RequireSafeDirectoryTree(std::filesystem::path(path).parent_path());
    if (ReadMarker(path) != expected)
        throw TransportError("camera_control_marker_failed", "marker instance mismatch");
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        throw TransportError("camera_control_marker_failed", WindowsError("CreateFileW", GetLastError()));
    LARGE_INTEGER start{};
    DWORD n{};
    const BOOL ok = SetFilePointerEx(f, start, nullptr, FILE_BEGIN) && SetEndOfFile(f) &&
        WriteFile(f, replacement.data(), static_cast<DWORD>(replacement.size()), &n, nullptr) &&
        n == replacement.size() && FlushFileBuffers(f);
    const DWORD error = GetLastError();
    CloseHandle(f);
    if (!ok)
        throw TransportError("camera_control_marker_failed", WindowsError("SetEndOfFile/WriteFile", error));
    RequireSafeDirectoryTree(std::filesystem::path(path).parent_path());
    if (ReadMarker(path) != replacement)
        throw TransportError("camera_control_marker_failed", "marker reread mismatch");
}
void HardwareProcessLease::RegisterDualWorkers(void *camera_a_process, void *camera_b_process) {
    HANDLE a{}, b{};
    try {
        if (!owned_ || owner_thread_id_ != GetCurrentThreadId() || !delegation_armed_ ||
            delegation_disarm_failed_ || worker_a_ || worker_b_)
            throw TransportError("camera_control_marker_failed", "worker registration unavailable");
        const auto self = GetCurrentProcess();
        const DWORD access = SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION;
        if (!DuplicateHandle(self, static_cast<HANDLE>(camera_a_process), self, &a, access, FALSE, 0) ||
            !DuplicateHandle(self, static_cast<HANDLE>(camera_b_process), self, &b, access, FALSE, 0))
            throw TransportError("camera_control_marker_failed", "worker handle retention failed");
        const auto pid_a = GetProcessId(a), pid_b = GetProcessId(b);
        if (!pid_a || !pid_b || pid_a == pid_b || pid_a == GetCurrentProcessId() ||
            pid_b == GetCurrentProcessId())
            throw TransportError("camera_control_marker_failed", "distinct child workers required");
        std::array<unsigned char, 16> epoch_bytes{};
        const auto epoch = MakeHexRandom(epoch_bytes);
        DelegationMarker armed_marker;
        if (!ParseDelegationMarker(marker_contents_, armed_marker) || armed_marker.owner_pid != GetCurrentProcessId() ||
            armed_marker.epoch != std::string(32, '0') || armed_marker.worker_a_pid != 1 ||
            armed_marker.worker_b_pid != 2)
            throw TransportError("camera_control_marker_failed", "armed marker is malformed");
        const auto replacement = MakeMarkerContents(armed_marker.nonce, GetCurrentProcessId(), epoch, pid_a, pid_b);
        RewriteMarker(marker_path_, marker_contents_, replacement);
        marker_contents_ = replacement;
        delegation_epoch_ = epoch;
        worker_a_ = a;
        worker_b_ = b;
    } catch (...) {
        if (a) CloseHandle(a);
        if (b) CloseHandle(b);
        delegation_disarm_failed_ = true;
        throw;
    }
}
const std::string &HardwareProcessLease::DelegationEpoch() const {
    if (!owned_ || owner_thread_id_ != GetCurrentThreadId() || !delegation_armed_ || delegation_disarm_failed_ || !worker_a_ || !worker_b_ ||
        delegation_epoch_.empty())
        throw TransportError("camera_control_marker_failed", "delegation epoch unavailable");
    return delegation_epoch_;
}
bool HardwareProcessLease::ValidateWorkerDelegation(void *inherited_parent_process, std::string_view epoch,
                                                    std::string_view lease_name,
                                                    const std::filesystem::path &test_marker_root) {
    try {
        if (!IsSafeLeaseName(lease_name) || epoch.empty() || !IsLowerHex(epoch, 32) ||
            (!test_marker_root.empty() && (lease_name == kProductionLeaseName || !IsTestLeaseName(lease_name))))
            return false;
#if defined(A0_NIKON_SDK_AVAILABLE)
        if (lease_name != kProductionLeaseName || !test_marker_root.empty()) return false;
#endif
        const HANDLE parent = static_cast<HANDLE>(inherited_parent_process);
        const DWORD parent_pid = GetProcessId(parent);
        if (!parent_pid || parent_pid == GetCurrentProcessId() || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT)
            return false;
        DWORD parent_session{}, worker_session{};
        if (!ProcessIdToSessionId(parent_pid, &parent_session) ||
            !ProcessIdToSessionId(GetCurrentProcessId(), &worker_session) || parent_session != worker_session)
            return false;
        const auto root = test_marker_root.empty() ? ProductionMarkerRoot() : test_marker_root;
        RequireSafeDirectoryTree(root);
        const auto marker_wire = ReadMarker(MarkerPath(root));
        DelegationMarker marker;
        if (!ParseDelegationMarker(marker_wire, marker) || marker.owner_pid != parent_pid || marker.epoch != epoch ||
            (GetCurrentProcessId() != marker.worker_a_pid && GetCurrentProcessId() != marker.worker_b_pid))
            return false;

        // Do not acquire or create this mutex. WAIT_TIMEOUT proves an existing
        // lease is occupied; trusted registration binds that owner to parent_pid.
        // A missing or abandoned mutex is fail-closed even with a valid marker.
        const std::wstring mutex_name = L"Local\\" + ToWide(lease_name);
        HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutex_name.c_str());
        if (!mutex) return false;
        const DWORD wait = WaitForSingleObject(mutex, 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED)
            ReleaseMutex(mutex);
        CloseHandle(mutex);
        return wait == WAIT_TIMEOUT;
    } catch (...) {
        return false;
    }
}
void HardwareProcessLease::DisarmDualDelegation(const DualDelegationCloseEvidence &evidence) {
    try {
        if (!durable_marker_enabled_ || !owned_ || owner_thread_id_ != GetCurrentThreadId() ||
            !delegation_armed_ || delegation_disarm_failed_ || !worker_a_ || !worker_b_ || !evidence.Complete())
            throw TransportError("camera_control_marker_failed",
                                 "complete typed close evidence required; marker remains armed");
        const auto process_a = static_cast<HANDLE>(evidence.camera_a.worker_process);
        const auto process_b = static_cast<HANDLE>(evidence.camera_b.worker_process);
        const auto pid_a = GetProcessId(process_a);
        const auto pid_b = GetProcessId(process_b);
        if (!pid_a || !pid_b || pid_a == pid_b)
            throw TransportError("camera_control_marker_failed", "two distinct worker processes required");
        if (pid_a != GetProcessId(static_cast<HANDLE>(worker_a_)) ||
            pid_b != GetProcessId(static_cast<HANDLE>(worker_b_)))
            throw TransportError("camera_control_marker_failed", "close evidence worker binding mismatch");
        for (const auto process : {static_cast<HANDLE>(worker_a_), static_cast<HANDLE>(worker_b_)}) {
            DWORD exit_code{};
            if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0 ||
                !GetExitCodeProcess(process, &exit_code) || exit_code != 0)
                throw TransportError("camera_control_marker_failed",
                                     "worker clean exit unconfirmed; marker remains armed");
        }
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

// Audited one-shot recovery of a stranded dual-delegation marker
// (docs/design/dual-preview-topology-diag.md section 9). Everything below
// reuses the marker rules above (WalkMarkerCandidates, ReadDiagnosticSnapshot,
// ParseDelegationMarker, ProcessAbsent, ExistingSafeDirectoryTree) so the
// recovery path cannot drift from the lease guard or the read-only diagnostic.
namespace {

constexpr std::string_view kRecoveryAuditSchema = "a0.marker-recovery-audit.v1";
// Design 9.7 / P11: both limits are provisional values pending owner review.
constexpr unsigned long long kRecoveryAuditMaxBytes = 1024ULL * 1024ULL;
constexpr auto kRecoveryDryRunValidity = std::chrono::minutes(30);

struct RecoveryContext final {
    std::filesystem::path root;
    std::filesystem::path audit_dir;
    std::wstring mutex_name;
    std::function<DualDelegationRecoveryProbe()> camera_probe;
    std::function<DualDelegationRecoveryProbe()> a0_process_probe;
    std::function<std::chrono::system_clock::time_point()> now;
    std::function<void(DualDelegationRecoveryTestPoint)> hook;
    void Hook(DualDelegationRecoveryTestPoint point) const {
        if (hook) hook(point);
    }
};

class UniqueFileHandle final {
  public:
    explicit UniqueFileHandle(HANDLE handle) noexcept : handle_(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
    ~UniqueFileHandle() { reset(); }
    UniqueFileHandle(const UniqueFileHandle &) = delete;
    UniqueFileHandle &operator=(const UniqueFileHandle &) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
    void reset() noexcept {
        if (handle_ != nullptr) CloseHandle(handle_);
        handle_ = nullptr;
    }

  private:
    HANDLE handle_{};
};

// Camera-control exclusion for the recovery functions only. This is not a
// lease: it cannot arm, register workers, or hand out a delegation epoch, and
// no SDK/WPD/worker API accepts it (they require HardwareProcessLease&). It is
// created and destroyed inside one recovery call on one thread.
class RecoveryCameraControlHold final {
  public:
    explicit RecoveryCameraControlHold(const std::wstring &mutex_name) {
        handle_ = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
        if (handle_ == nullptr) {
            failure_ = "camera_control_busy";
            return;
        }
        const DWORD wait = WaitForSingleObject(handle_, 0);
        if (wait == WAIT_OBJECT_0) {
            owned_ = true;
        } else if (wait == WAIT_ABANDONED) {
            // The previous owner died while holding the mutex: stop this round.
            owned_ = true;
            failure_ = "camera_control_abandoned";
        } else {
            failure_ = "camera_control_busy";
        }
    }
    ~RecoveryCameraControlHold() {
        if (owned_) ReleaseMutex(handle_);
        if (handle_ != nullptr) CloseHandle(handle_);
    }
    RecoveryCameraControlHold(const RecoveryCameraControlHold &) = delete;
    RecoveryCameraControlHold &operator=(const RecoveryCameraControlHold &) = delete;
    [[nodiscard]] std::string_view Failure() const noexcept { return failure_; }

  private:
    HANDLE handle_{};
    bool owned_{};
    std::string_view failure_;
};

wchar_t UpperAscii(wchar_t character) noexcept {
    return character >= L'a' && character <= L'z' ? static_cast<wchar_t>(character - L'a' + L'A') : character;
}
bool ContainsIgnoringAsciiCase(std::wstring_view haystack, std::wstring_view upper_needle) noexcept {
    if (upper_needle.empty() || upper_needle.size() > haystack.size()) return false;
    for (std::size_t start = 0; start + upper_needle.size() <= haystack.size(); ++start) {
        std::size_t matched = 0;
        while (matched < upper_needle.size() && UpperAscii(haystack[start + matched]) == upper_needle[matched])
            ++matched;
        if (matched == upper_needle.size()) return true;
    }
    return false;
}
bool StartsWithIgnoringAsciiCase(std::wstring_view value, std::wstring_view upper_prefix) noexcept {
    return value.size() >= upper_prefix.size() &&
           ContainsIgnoringAsciiCase(value.substr(0, upper_prefix.size()), upper_prefix);
}

enum class DeviceText : unsigned char { absent, read, failed };
DeviceText ReadDeviceRegistryText(HDEVINFO devices, SP_DEVINFO_DATA &device, DWORD property, std::wstring &text) {
    text.clear();
    std::vector<BYTE> buffer(512);
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD type{}, required{};
        if (SetupDiGetDeviceRegistryPropertyW(devices, &device, property, &type, buffer.data(),
                                              static_cast<DWORD>(buffer.size()), &required)) {
            if (type != REG_SZ && type != REG_MULTI_SZ && type != REG_EXPAND_SZ) return DeviceText::failed;
            const std::size_t bytes = std::min<std::size_t>(required, buffer.size());
            text.assign(reinterpret_cast<const wchar_t *>(buffer.data()), bytes / sizeof(wchar_t));
            std::replace(text.begin(), text.end(), L'\0', L' ');
            return DeviceText::read;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_DATA) return DeviceText::absent;
        if (error != ERROR_INSUFFICIENT_BUFFER || required <= buffer.size() || required > 65536U)
            return DeviceText::failed;
        buffer.resize(required);
    }
    return DeviceText::failed;
}
bool ReadDeviceInstanceId(HDEVINFO devices, SP_DEVINFO_DATA &device, std::wstring &instance) {
    std::vector<wchar_t> buffer(256);
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD required{};
        if (SetupDiGetDeviceInstanceIdW(devices, &device, buffer.data(), static_cast<DWORD>(buffer.size()),
                                        &required)) {
            instance.assign(buffer.data());
            return true;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required <= buffer.size() || required > 32768U)
            return false;
        buffer.resize(required);
    }
    return false;
}
// P8: any present device node whose instance ID or hardware IDs carry the
// Nikon USB vendor ID, or whose hardware IDs, friendly name or description
// mention D810. Deliberately broad: every Nikon USB device stops recovery.
DualDelegationRecoveryProbe ProbeNikonCameraPresent() {
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return DualDelegationRecoveryProbe::unavailable;
    constexpr DWORD kTextProperties[] = {SPDRP_HARDWAREID, SPDRP_FRIENDLYNAME, SPDRP_DEVICEDESC};
    auto result = DualDelegationRecoveryProbe::absent;
    for (DWORD index = 0; result == DualDelegationRecoveryProbe::absent; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) result = DualDelegationRecoveryProbe::unavailable;
            break;
        }
        std::wstring instance;
        if (!ReadDeviceInstanceId(devices, device, instance)) {
            result = DualDelegationRecoveryProbe::unavailable;
            break;
        }
        bool match = ContainsIgnoringAsciiCase(instance, L"VID_04B0");
        for (const DWORD property : kTextProperties) {
            std::wstring text;
            const auto read = ReadDeviceRegistryText(devices, device, property, text);
            if (read == DeviceText::failed) {
                result = DualDelegationRecoveryProbe::unavailable;
                break;
            }
            match = match || ContainsIgnoringAsciiCase(text, L"VID_04B0") || ContainsIgnoringAsciiCase(text, L"D810");
        }
        if (result == DualDelegationRecoveryProbe::absent && match) result = DualDelegationRecoveryProbe::present;
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}
// P7: any process in any session, other than this one, whose image name
// starts with "A0CameraStitcher." (case-insensitive).
DualDelegationRecoveryProbe ProbeOtherA0Processes() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return DualDelegationRecoveryProbe::unavailable;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    auto result = DualDelegationRecoveryProbe::unavailable;
    if (Process32FirstW(snapshot, &entry)) {
        const DWORD self = GetCurrentProcessId();
        result = DualDelegationRecoveryProbe::absent;
        do {
            if (entry.th32ProcessID != self && StartsWithIgnoringAsciiCase(entry.szExeFile, L"A0CAMERASTITCHER.")) {
                result = DualDelegationRecoveryProbe::present;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
        if (result == DualDelegationRecoveryProbe::absent && GetLastError() != ERROR_NO_MORE_FILES)
            result = DualDelegationRecoveryProbe::unavailable;
    }
    CloseHandle(snapshot);
    return result;
}

bool ResolveRecoveryContext(const DualDelegationMarkerRecoveryOptions &options, RecoveryContext &context) {
    const bool any_test_seam = !options.test_marker_root.empty() || !options.test_lease_name.empty() ||
        static_cast<bool>(options.test_camera_probe) || static_cast<bool>(options.test_a0_process_probe) ||
        static_cast<bool>(options.test_now) || static_cast<bool>(options.test_hook);
    std::string_view lease_name = kProductionLeaseName;
#if defined(A0_NIKON_SDK_AVAILABLE)
    // I-14: an SDK-enabled library has no test root, test lease or probe override.
    if (any_test_seam) return false;
    context.root = ProductionMarkerRoot();
#else
    if (options.test_marker_root.empty()) {
        // Probe/time/hook overrides are honoured only together with a test root.
        if (any_test_seam) return false;
        context.root = ProductionMarkerRoot();
    } else {
        lease_name = options.test_lease_name;
        if (!IsSafeLeaseName(lease_name) || lease_name == kProductionLeaseName || !IsTestLeaseName(lease_name) ||
            !options.test_marker_root.is_absolute())
            return false;
        context.root = options.test_marker_root;
        context.camera_probe = options.test_camera_probe;
        context.a0_process_probe = options.test_a0_process_probe;
        context.now = options.test_now;
        context.hook = options.test_hook;
    }
#endif
    // The audit directory is the root's sibling, so the root must be a plain,
    // already-normal path with a final component (no trailing separator).
    if (!context.root.has_filename() || context.root.lexically_normal().native() != context.root.native() ||
        context.root.parent_path() == context.root)
        return false;
    context.audit_dir = context.root.parent_path() / L"DualDelegationRecovery";
    context.mutex_name = L"Local\\" + ToWide(lease_name);
    if (!context.camera_probe) context.camera_probe = ProbeNikonCameraPresent;
    if (!context.a0_process_probe) context.a0_process_probe = ProbeOtherA0Processes;
    if (!context.now) context.now = [] { return std::chrono::system_clock::now(); };
    return true;
}

// P3: armed-session-<digits>.marker, 1-10 digits, no leading zero except "0",
// value <= 4294967295, exact lower-case spelling.
bool CanonicalMarkerName(std::wstring_view name, DWORD &session) noexcept {
    constexpr std::wstring_view prefix = L"armed-session-";
    constexpr std::wstring_view suffix = L".marker";
    if (name.size() <= prefix.size() + suffix.size() || name.substr(0, prefix.size()) != prefix ||
        name.substr(name.size() - suffix.size()) != suffix)
        return false;
    const auto digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (digits.size() > 10 || (digits.size() > 1 && digits.front() == L'0')) return false;
    unsigned long long value{};
    for (const wchar_t character : digits) {
        if (character < L'0' || character > L'9') return false;
        value = value * 10ULL + static_cast<unsigned long long>(character - L'0');
    }
    if (value > std::numeric_limits<DWORD>::max()) return false;
    session = static_cast<DWORD>(value);
    return true;
}

std::string Sha256OfText(const std::string &text) {
    return Sha256Hex(std::vector<unsigned char>(text.begin(), text.end()));
}

struct RecoveryInspection final {
    std::string status;
    std::wstring path;
    DiagnosticSnapshot snapshot;
    std::string sha256;
    bool session_match{};
};
// P1-P8 in the design order. `foreign_confirmation` is null for a dry run.
RecoveryInspection InspectRecoveryCandidate(const RecoveryContext &context, std::string_view expected_sha256,
                                            const bool *foreign_confirmation) {
    RecoveryInspection inspection;
    const auto stop = [&](const char *status) {
        inspection.status = status;
        return inspection;
    };
    if (!ExistingSafeDirectoryTree(context.root)) return stop("marker_root_untrusted");
    const auto walk = WalkMarkerCandidates(context.root, 2);
    if (walk.count == 0) return stop(walk.enumeration_not_found ? "marker_missing" : "marker_unavailable");
    if (walk.count > 1) return stop("marker_ambiguous");
    if (!walk.enumeration_clean) return stop("marker_unavailable");
    DWORD marker_session{};
    if (!CanonicalMarkerName(walk.first_name, marker_session)) return stop("marker_name_noncanonical");
    DWORD current_session{};
    inspection.session_match =
        ProcessIdToSessionId(GetCurrentProcessId(), &current_session) && current_session == marker_session;
    inspection.path = (context.root / walk.first_name).wstring();
    DiagnosticSnapshot first{}, second{};
    DelegationMarker parsed{};
    if (!ReadDiagnosticSnapshot(inspection.path, first) || !ParseDelegationMarker(first.contents, parsed))
        return stop("marker_invalid");
    context.Hook(DualDelegationRecoveryTestPoint::between_marker_snapshots);
    if (!ExistingSafeDirectoryTree(context.root) || !ReadDiagnosticSnapshot(inspection.path, second) ||
        !SameDiagnosticSnapshot(first, second))
        return stop("marker_changed");
    inspection.snapshot = second;
    inspection.sha256 = Sha256OfText(second.contents);
    if (!expected_sha256.empty() && inspection.sha256 != expected_sha256) return stop("hash_mismatch");
    // Addendum: a foreign-session marker needs an explicit operator
    // confirmation, and a confirmation for a same-session marker is a mix-up.
    if (foreign_confirmation != nullptr && *foreign_confirmation == inspection.session_match)
        return stop("session_confirmation_mismatch");
    if (!ProcessAbsent(parsed.owner_pid) || !ProcessAbsent(parsed.worker_a_pid) ||
        !ProcessAbsent(parsed.worker_b_pid))
        return stop("process_active_or_unknown");
    switch (context.a0_process_probe()) {
    case DualDelegationRecoveryProbe::absent: break;
    case DualDelegationRecoveryProbe::present: return stop("a0_process_present");
    default: return stop("process_list_unavailable");
    }
    switch (context.camera_probe()) {
    case DualDelegationRecoveryProbe::absent: break;
    case DualDelegationRecoveryProbe::present: return stop("camera_present");
    default: return stop("pnp_unavailable");
    }
    return inspection;
}

long long DaysFromCivil(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned shifted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<long long>(day_of_era) - 719468;
}
std::string FormatUtc(std::chrono::system_clock::time_point point) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(point);
    std::tm utc{};
    if (gmtime_s(&utc, &seconds) != 0) return {};
    char text[32]{};
    if (std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc) != 20) return {};
    return text;
}
bool ParseUtc(std::string_view text, std::chrono::system_clock::time_point &point) {
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':' || text[19] != 'Z')
        return false;
    const auto field = [&](std::size_t offset, std::size_t length, unsigned &value) {
        value = 0;
        for (std::size_t index = offset; index < offset + length; ++index) {
            if (text[index] < '0' || text[index] > '9') return false;
            value = value * 10U + static_cast<unsigned>(text[index] - '0');
        }
        return true;
    };
    unsigned year{}, month{}, day{}, hour{}, minute{}, second{};
    if (!field(0, 4, year) || !field(5, 2, month) || !field(8, 2, day) || !field(11, 2, hour) ||
        !field(14, 2, minute) || !field(17, 2, second) || year < 1970 || month < 1 || month > 12 || day < 1 ||
        day > 31 || hour > 23 || minute > 59 || second > 59)
        return false;
    const long long total = DaysFromCivil(static_cast<int>(year), month, day) * 86400LL +
        static_cast<long long>(hour) * 3600LL + static_cast<long long>(minute) * 60LL + second;
    point = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(total)));
    return FormatUtc(point) == text; // rejects impossible dates such as 02-31
}

struct RecoveryAuditRecord final {
    std::string record;
    std::string status;
    std::string anonymous_sha256;
    std::string tool_sha256;
    unsigned long long size{};
    bool session_match{};
    bool operator_attested{};
    std::string utc;
};
std::string FormatRecoveryAuditRecord(const RecoveryAuditRecord &record) {
    // Fixed fields only: no path, PID, nonce, epoch, session number, user or camera identity.
    return "{\"schema\":\"" + std::string(kRecoveryAuditSchema) + "\",\"record\":\"" + record.record +
        "\",\"utc\":\"" + record.utc + "\",\"status\":\"" + record.status + "\",\"anonymousSha256\":\"" +
        record.anonymous_sha256 + "\",\"size\":" + std::to_string(record.size) +
        ",\"sessionMatch\":" + (record.session_match ? "1" : "0") +
        ",\"operatorAttested\":" + (record.operator_attested ? "1" : "0") + ",\"toolSha256\":\"" +
        record.tool_sha256 + "\"}\n";
}
bool IsStatusToken(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= 'a' && character <= 'z') || character == '_';
           });
}
bool ParseRecoveryAuditLine(std::string_view line, RecoveryAuditRecord &record) {
    std::size_t cursor{};
    const auto literal = [&](std::string_view text) {
        if (line.substr(cursor, text.size()) != text) return false;
        cursor += text.size();
        return true;
    };
    const auto quoted = [&](std::string &value) {
        const auto end = line.find('"', cursor);
        if (end == std::string_view::npos) return false;
        value.assign(line.substr(cursor, end - cursor));
        cursor = end;
        return true;
    };
    const auto flag = [&](bool &value) {
        if (cursor >= line.size() || (line[cursor] != '0' && line[cursor] != '1')) return false;
        value = line[cursor++] == '1';
        return true;
    };
    const auto size_field = [&]() {
        std::size_t end = cursor;
        while (end < line.size() && end - cursor < 4 && line[end] >= '0' && line[end] <= '9') ++end;
        if (end == cursor || end - cursor > 3 || (end - cursor > 1 && line[cursor] == '0')) return false;
        unsigned long long value{};
        for (auto index = cursor; index < end; ++index)
            value = value * 10ULL + static_cast<unsigned long long>(line[index] - '0');
        record.size = value;
        cursor = end;
        return true;
    };
    std::chrono::system_clock::time_point ignored{};
    return literal("{\"schema\":\"") && literal(kRecoveryAuditSchema) && literal("\",\"record\":\"") &&
           quoted(record.record) && literal("\",\"utc\":\"") && quoted(record.utc) &&
           literal("\",\"status\":\"") && quoted(record.status) && literal("\",\"anonymousSha256\":\"") &&
           quoted(record.anonymous_sha256) && literal("\",\"size\":") && size_field() &&
           literal(",\"sessionMatch\":") && flag(record.session_match) && literal(",\"operatorAttested\":") &&
           flag(record.operator_attested) && literal(",\"toolSha256\":\"") && quoted(record.tool_sha256) &&
           literal("\"}") && cursor == line.size() &&
           (record.record == "dry_run" || record.record == "execute_intent" || record.record == "execute_result") &&
           IsStatusToken(record.status) && IsLowerHex(record.anonymous_sha256, 64) &&
           IsLowerHex(record.tool_sha256, 64) && record.size >= 1 && record.size <= 255 &&
           ParseUtc(record.utc, ignored);
}

// Returns false when the audit exists but cannot be trusted or parsed.
bool LoadRecoveryAudit(const RecoveryContext &context, std::vector<RecoveryAuditRecord> &records) {
    records.clear();
    const DWORD directory_attributes = GetFileAttributesW(context.audit_dir.c_str());
    if (directory_attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if (!ExistingSafeDirectoryTree(context.audit_dir)) return false;
    const auto file_path = context.audit_dir / L"audit.jsonl";
    const DWORD attributes = GetFileAttributesW(file_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) return false;
    UniqueFileHandle file(CreateFileW(file_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!file || GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        info.nFileSizeHigh != 0 || info.nFileSizeLow > kRecoveryAuditMaxBytes)
        return false;
    std::string contents(info.nFileSizeLow, '\0');
    DWORD read{};
    if (!contents.empty() &&
        (!ReadFile(file.get(), contents.data(), info.nFileSizeLow, &read, nullptr) || read != info.nFileSizeLow))
        return false;
    if (!contents.empty() && contents.back() != '\n') return false;
    std::size_t start{};
    while (start < contents.size()) {
        const auto end = contents.find('\n', start);
        RecoveryAuditRecord record;
        if (!ParseRecoveryAuditLine(std::string_view(contents).substr(start, end - start), record)) return false;
        records.push_back(std::move(record));
        start = end + 1;
    }
    return true;
}

bool AppendRecoveryAudit(const RecoveryContext &context, RecoveryAuditRecord record) noexcept {
    try {
        record.utc = FormatUtc(context.now());
        if (record.utc.empty()) return false;
        const auto line = FormatRecoveryAuditRecord(record);
        RequireSafeDirectoryTree(context.audit_dir);
        const auto file_path = context.audit_dir / L"audit.jsonl";
        // Append-only handle (no FILE_WRITE_DATA), write-through, flushed per line.
        UniqueFileHandle file(CreateFileW(
            file_path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        DWORD written{};
        return file && GetFileType(file.get()) == FILE_TYPE_DISK && GetFileInformationByHandle(file.get(), &info) &&
               (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
               info.nNumberOfLinks == 1 && info.nFileSizeHigh == 0 &&
               info.nFileSizeLow + line.size() <= kRecoveryAuditMaxBytes &&
               WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) &&
               written == line.size() && FlushFileBuffers(file.get());
    } catch (...) {
        return false;
    }
}

std::string RunningToolSha256() {
    std::wstring module_path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD length =
            GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
        if (length == 0) return {};
        if (length < module_path.size()) {
            module_path.resize(length);
            break;
        }
        if (module_path.size() >= 32768) return {};
        module_path.resize(module_path.size() * 2);
    }
    UniqueFileHandle file(CreateFileW(module_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (!file || !GetFileSizeEx(file.get(), &size) || size.QuadPart < 1 || size.QuadPart > (256LL << 20))
        return {};
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1U << 20));
        DWORD read{};
        if (!ReadFile(file.get(), bytes.data() + offset, chunk, &read, nullptr) || read == 0) return {};
        offset += read;
    }
    return Sha256Hex(bytes);
}

// P11: a matching recovery_eligible dry run from this exact tool within the
// validity window, and no execute record for the hash anywhere.
std::string CheckRecoveryDryRunRecord(const RecoveryContext &context, std::string_view expected_sha256,
                                      const std::string &tool_sha256, const RecoveryInspection *inspection) {
    std::vector<RecoveryAuditRecord> records;
    if (!LoadRecoveryAudit(context, records)) return "audit_unavailable";
    for (const auto &record : records) {
        if (record.record != "dry_run" && record.anonymous_sha256 == expected_sha256) return "already_executed";
    }
    const auto now = context.now();
    for (const auto &record : records) {
        if (record.record != "dry_run" || record.status != "recovery_eligible" ||
            record.anonymous_sha256 != expected_sha256 || record.tool_sha256 != tool_sha256)
            continue;
        if (inspection != nullptr && (record.size != inspection->snapshot.contents.size() ||
                                      record.session_match != inspection->session_match))
            continue;
        std::chrono::system_clock::time_point recorded{};
        if (ParseUtc(record.utc, recorded) && recorded <= now && now - recorded <= kRecoveryDryRunValidity)
            return {};
    }
    return "dry_run_record_missing";
}

bool SameFinalPath(HANDLE file, const std::wstring &expected) {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= buffer.size() && length < 32768U) {
        buffer.assign(static_cast<std::size_t>(length) + 1, L'\0');
        length = GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    if (length == 0 || length >= buffer.size()) return false;
    std::wstring_view final_path(buffer.data(), length);
    if (final_path.substr(0, 8) == L"\\\\?\\UNC\\") return false;
    if (final_path.substr(0, 4) == L"\\\\?\\") final_path.remove_prefix(4);
    return CompareStringOrdinal(final_path.data(), static_cast<int>(final_path.size()), expected.data(),
                                static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

// Step 6: re-verify the exact object behind the exclusive handle.
std::string VerifyExclusiveMarker(HANDLE marker, const RecoveryInspection &inspection,
                                  std::string_view expected_sha256, std::string &contents) {
    constexpr const char *changed = "marker_identity_changed";
    if (!SameFinalPath(marker, inspection.path) || GetFileType(marker) != FILE_TYPE_DISK) return changed;
    BY_HANDLE_FILE_INFORMATION info{};
    const auto &before = inspection.snapshot.info;
    if (!GetFileInformationByHandle(marker, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        info.nNumberOfLinks != 1 || info.nFileSizeHigh != 0 || info.nFileSizeLow < 1 || info.nFileSizeLow > 255 ||
        info.dwVolumeSerialNumber != before.dwVolumeSerialNumber || info.nFileIndexHigh != before.nFileIndexHigh ||
        info.nFileIndexLow != before.nFileIndexLow)
        return changed;
    LARGE_INTEGER origin{};
    std::array<char, 256> bytes{};
    DWORD read{};
    if (!SetFilePointerEx(marker, origin, nullptr, FILE_BEGIN) ||
        !ReadFile(marker, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
        read != info.nFileSizeLow)
        return changed;
    contents.assign(bytes.data(), read);
    DelegationMarker parsed{};
    if (!ParseDelegationMarker(contents, parsed)) return changed;
    if (Sha256OfText(contents) != expected_sha256) return "hash_mismatch";
    if (!ProcessAbsent(parsed.owner_pid) || !ProcessAbsent(parsed.worker_a_pid) ||
        !ProcessAbsent(parsed.worker_b_pid))
        return changed;
    return {};
}

// Step 7: local evidence copy outside the marker root (it holds the nonce,
// epoch and PIDs, so it is never committed or exported). An identical copy
// left by an earlier attempt that stopped before deletion is accepted, so the
// operator can restart from a fresh dry run.
bool WriteRecoveryEvidence(const RecoveryContext &context, const std::string &sha256,
                           const std::string &contents) noexcept {
    try {
        RequireSafeDirectoryTree(context.audit_dir);
        const auto path =
            context.audit_dir / (L"evidence-" + ToWide(std::string_view(sha256).substr(0, 16)) + L".bak");
        HANDLE created = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (created != INVALID_HANDLE_VALUE) {
            DWORD written{};
            const bool ok =
                WriteFile(created, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
                written == contents.size() && FlushFileBuffers(created);
            if (!ok) {
                // Discard only the partial copy this call just created.
                FILE_DISPOSITION_INFO discard{TRUE};
                SetFileInformationByHandle(created, FileDispositionInfo, &discard, sizeof(discard));
            }
            CloseHandle(created);
            if (!ok) return false;
        } else if (GetLastError() != ERROR_FILE_EXISTS) {
            return false;
        }
        DiagnosticSnapshot copy{};
        return ReadDiagnosticSnapshot(path.wstring(), copy) && copy.contents == contents;
    } catch (...) {
        return false;
    }
}

void CopyInspection(const RecoveryInspection &inspection, DualDelegationMarkerRecoveryResult &result) {
    result.anonymous_sha256 = inspection.sha256;
    result.size = inspection.sha256.empty() ? 0 : inspection.snapshot.contents.size();
    result.session_match = inspection.session_match;
}

} // namespace

DualDelegationMarkerRecoveryResult DryRunDualDelegationMarkerRecovery(
    const DualDelegationMarkerRecoveryOptions &options, std::string_view expected_sha256) {
    DualDelegationMarkerRecoveryResult result;
    try {
        RecoveryContext context;
        if (!ResolveRecoveryContext(options, context)) {
            result.status = "marker_root_untrusted";
            return result;
        }
        if (!expected_sha256.empty() && !IsLowerHex(expected_sha256, 64)) {
            result.status = "hash_mismatch";
            return result;
        }
        const auto inspection = InspectRecoveryCandidate(context, expected_sha256, nullptr);
        CopyInspection(inspection, result);
        if (!inspection.status.empty()) {
            result.status = inspection.status;
            return result;
        }
        {
            // P9: taken and released at once; a dry run never keeps the mutex.
            const RecoveryCameraControlHold hold(context.mutex_name);
            if (!hold.Failure().empty()) {
                result.status = std::string(hold.Failure());
                return result;
            }
        }
        RecoveryAuditRecord record;
        record.record = "dry_run";
        record.status = "recovery_eligible";
        record.anonymous_sha256 = result.anonymous_sha256;
        record.tool_sha256 = RunningToolSha256();
        record.size = result.size;
        record.session_match = result.session_match;
        if (record.tool_sha256.empty() || !AppendRecoveryAudit(context, record)) {
            result.status = "audit_unavailable";
            return result;
        }
        result.status = "recovery_eligible";
    } catch (...) {
        result.status = "marker_unavailable";
    }
    return result;
}

DualDelegationMarkerRecoveryResult ExecuteDualDelegationMarkerRecovery(
    const DualDelegationMarkerRecoveryOptions &options, std::string_view expected_sha256,
    bool operator_attested_cameras_disconnected, bool operator_confirmed_foreign_session) {
    DualDelegationMarkerRecoveryResult result;
    bool disposition_attempted = false;
    const auto stop = [&](std::string_view status) {
        result.status = std::string(status);
        return result;
    };
    try {
        RecoveryContext context;
        if (!ResolveRecoveryContext(options, context)) return stop("marker_root_untrusted");
        if (!IsLowerHex(expected_sha256, 64)) return stop("hash_mismatch");
        if (!operator_attested_cameras_disconnected) return stop("operator_attestation_missing"); // 1 (P10)
        const auto tool_sha256 = RunningToolSha256();
        if (tool_sha256.empty()) return stop("audit_unavailable");
        if (const auto status = CheckRecoveryDryRunRecord(context, expected_sha256, tool_sha256, nullptr);
            !status.empty())
            return stop(status); // 2 (P11)
        const RecoveryCameraControlHold hold(context.mutex_name); // 3 (P9), held until return
        if (!hold.Failure().empty()) return stop(hold.Failure());
        // 4: every precondition is taken again under the hold, including P11.
        const auto inspection =
            InspectRecoveryCandidate(context, expected_sha256, &operator_confirmed_foreign_session);
        CopyInspection(inspection, result);
        if (!inspection.status.empty()) return stop(inspection.status);
        if (const auto status = CheckRecoveryDryRunRecord(context, expected_sha256, tool_sha256, &inspection);
            !status.empty())
            return stop(status);
        context.Hook(DualDelegationRecoveryTestPoint::before_exclusive_open);
        // 5: no sharing at all; a reparse point swapped in is opened as itself.
        UniqueFileHandle marker(CreateFileW(inspection.path.c_str(), GENERIC_READ | DELETE, 0, nullptr,
                                            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!marker) return stop("marker_open_failed");
        std::string contents;
        if (const auto status = VerifyExclusiveMarker(marker.get(), inspection, expected_sha256, contents);
            !status.empty())
            return stop(status); // 6
        if (!WriteRecoveryEvidence(context, inspection.sha256, contents)) return stop("evidence_copy_failed"); // 7
        RecoveryAuditRecord record;
        record.record = "execute_intent";
        record.status = "recovery_eligible";
        record.anonymous_sha256 = inspection.sha256;
        record.tool_sha256 = tool_sha256;
        record.size = contents.size();
        record.session_match = inspection.session_match;
        record.operator_attested = true;
        if (!AppendRecoveryAudit(context, record)) return stop("audit_unavailable"); // 8
        context.Hook(DualDelegationRecoveryTestPoint::before_disposition);
        // 9: delete through the verified handle only; never by path, never retried.
        FILE_DISPOSITION_INFO disposition{TRUE};
        disposition_attempted = true;
        record.record = "execute_result";
        if (!SetFileInformationByHandle(marker.get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
            record.status = "delete_failed";
            (void)AppendRecoveryAudit(context, record);
            return stop("delete_failed");
        }
        marker.reset(); // 10: the only handle closes, so the deletion completes here.
        const auto walk = WalkMarkerCandidates(context.root, 1); // 11
        record.status = walk.count == 0 && walk.enumeration_not_found ? "executed_marker_absent"
                                                                      : "post_delete_root_not_empty";
        result.status = record.status;
        if (!AppendRecoveryAudit(context, record) && result.status == "executed_marker_absent") // 12
            result.status = "executed_audit_incomplete";
        return result; // 13: the hold is released on return.
    } catch (...) {
        result.status = disposition_attempted ? "executed_audit_incomplete" : "marker_unavailable";
    }
    return result;
}

} // namespace a0::phase0
