#include "a0/phase0/hardware_process_lease.hpp"

#include "a0/phase0/phase0.hpp"

#include <ShlObj.h>
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
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
// True for a name that ends in a dot or a space, or that contains a colon. Such
// a name is refused whether or not it exists.
//
// Win32 rewrites a trailing dot or space differently by the position of the name
// in a path (measured on Windows 11):
//   - the last name loses every trailing dot and space ("foo ." becomes "foo");
//   - an inner name loses one trailing dot only and keeps a trailing space and
//     the second and later dots ("foo " and "foo.." are kept as written, and
//     "foo ." becomes "foo ").
// A directory is opened as the last name, but every file or child below it is
// reached through the same name as an inner name, so one spelling can designate
// two different directories ("foo" and "foo "). The rule therefore refuses every
// name that ends in a dot or a space, a single dot included: relaxing it to allow
// one trailing dot reopens the gap through "foo .". A colon selects an alternate
// data stream.
bool HasTrimmedOrStreamName(const std::wstring &name) noexcept {
    return !name.empty() &&
           (name.back() == L'.' || name.back() == L' ' || name.find(L':') != std::wstring::npos);
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
        // Defense in depth: every caller already passes a root that was checked
        // by CheckMarkerRootOverlap or is the production root. The name is
        // refused before the directory is created.
        if (HasTrimmedOrStreamName(part.wstring()))
            throw TransportError("camera_control_marker_failed", "marker path has a name the file system may rewrite");
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
// Per-user directory that holds every record of the product
// (%LOCALAPPDATA%\A0CameraStitcher), including the marker root below.
std::filesystem::path ProductionDataRoot() {
    PWSTR raw{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)) || raw == nullptr)
        throw TransportError("camera_control_marker_failed", "could not resolve LocalAppData");
    std::filesystem::path root = std::filesystem::path(raw) / L"A0CameraStitcher";
    CoTaskMemFree(raw);
    return root;
}
std::filesystem::path ProductionMarkerRoot() {
    return ProductionDataRoot() / L"Phase0" / L"DualDelegation";
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

bool EqualsIgnoreCase(const std::wstring &a, const std::wstring &b) noexcept {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}
// True when `path` is `reference` or lies beneath it, compared ignoring case and
// only on whole path components ("C:\ab" is not beneath "C:\a").
bool IsSameOrBeneath(const std::wstring &path, const std::wstring &reference) noexcept {
    if (reference.empty() || path.size() < reference.size())
        return false;
    const int prefix = static_cast<int>(reference.size());
    if (CompareStringOrdinal(path.c_str(), prefix, reference.c_str(), prefix, TRUE) != CSTR_EQUAL)
        return false;
    return path.size() == reference.size() || reference.back() == L'\\' || path[reference.size()] == L'\\';
}
bool IsAsciiLetter(wchar_t character) noexcept {
    return (character >= L'A' && character <= L'Z') || (character >= L'a' && character <= L'z');
}
struct ScopedHandle final {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit ScopedHandle(HANDLE handle) noexcept : value(handle) {}
    ~ScopedHandle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr)
            CloseHandle(value);
    }
    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;
};
bool IsNotFoundError(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
// A name that does not exist yet cannot be resolved through the file system, so
// a name that the file system would rewrite when it is created or opened (8.3
// short-name syntax, an alternate data stream selector, a trailing dot or
// space) is treated as a possible alias instead of a new directory.
bool IsAliasProneName(const std::wstring &name) noexcept {
    return name.empty() || name.find_first_of(L"~:") != std::wstring::npos || name.back() == L'.' ||
           name.back() == L' ';
}

// Lexically normalized absolute drive-letter path without a trailing
// separator, or nullopt for anything else (relative, drive-relative, UNC,
// device or extended-length prefixes), which the caller treats as unverifiable.
std::optional<std::filesystem::path> NormalizedDrivePath(const std::filesystem::path &path) {
    std::wstring text = path.lexically_normal().wstring();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    while (!text.empty() && text.back() == L'\\')
        text.pop_back();
    if (text.size() == 2 && text[1] == L':')
        text.push_back(L'\\');
    std::filesystem::path result(text);
    const std::wstring drive = result.root_name().wstring();
    if (drive.size() != 2 || drive[1] != L':' || !IsAsciiLetter(drive[0]) || !result.has_root_directory())
        return std::nullopt;
    return result;
}

struct DirectoryIdentity final {
    bool extended{};
    ULONGLONG volume{};
    std::array<BYTE, 16> id{};
    bool operator==(const DirectoryIdentity &) const = default;
};
bool QueryDirectoryIdentity(HANDLE handle, DirectoryIdentity &identity) noexcept {
    FILE_ID_INFO extended{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &extended, sizeof(extended))) {
        identity.extended = true;
        identity.volume = extended.VolumeSerialNumber;
        std::copy(std::begin(extended.FileId.Identifier), std::end(extended.FileId.Identifier),
                  identity.id.begin());
        return true;
    }
    BY_HANDLE_FILE_INFORMATION basic{};
    if (GetFileInformationByHandle(handle, &basic)) {
        identity.extended = false;
        identity.volume = basic.dwVolumeSerialNumber;
        identity.id = {};
        std::memcpy(identity.id.data(), &basic.nFileIndexHigh, sizeof(basic.nFileIndexHigh));
        std::memcpy(identity.id.data() + sizeof(basic.nFileIndexHigh), &basic.nFileIndexLow,
                    sizeof(basic.nFileIndexLow));
        return true;
    }
    return false;
}
// Final path of an open handle with 8.3 names expanded and subst drives and
// junctions resolved, without the `\\?\` prefix and without a trailing
// separator.
bool QueryFinalPath(HANDLE handle, std::wstring &out) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    std::wstring buffer(512, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (length >= buffer.size()) {
        buffer.assign(static_cast<std::size_t>(length) + 1, L'\0');
        length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    }
    if (length == 0 || length >= buffer.size())
        return false;
    buffer.resize(length);
    constexpr std::wstring_view unc_prefix = L"\\\\?\\UNC\\";
    constexpr std::wstring_view device_prefix = L"\\\\?\\";
    const std::wstring_view view(buffer);
    if (view.starts_with(unc_prefix))
        out = L"\\\\" + std::wstring(view.substr(unc_prefix.size()));
    else if (view.starts_with(device_prefix) && view.size() >= 6 && view[5] == L':')
        out = std::wstring(view.substr(device_prefix.size()));
    else
        out = buffer;
    while (!out.empty() && out.back() == L'\\')
        out.pop_back();
    return !out.empty();
}

struct ResolvedMarkerRoot final {
    // Final path of the longest existing prefix plus the names that do not exist.
    std::wstring resolved;
    std::vector<std::wstring> missing;
    // Set only when the whole path exists.
    std::optional<DirectoryIdentity> identity;
    // Win32 error behind a false return.
    DWORD error{};
};
// Opens (never creates) the longest existing prefix of `absolute`. Returns
// false when that cannot be established, e.g. the drive is absent or an
// ancestor cannot be opened for a reason other than "not found"; the caller
// treats that as unverifiable and fails closed.
bool ResolveMarkerRoot(const std::filesystem::path &absolute, ResolvedMarkerRoot &out) {
    std::vector<std::wstring> parts;
    for (const auto &part : absolute.relative_path())
        parts.push_back(part.wstring());
    for (std::size_t keep = parts.size() + 1; keep-- > 0;) {
        std::filesystem::path prefix = absolute.root_path();
        for (std::size_t index = 0; index < keep; ++index)
            prefix /= parts[index];
        const ScopedHandle handle(CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (handle.value == INVALID_HANDLE_VALUE) {
            out.error = GetLastError();
            if (keep > 0 && IsNotFoundError(out.error))
                continue;
            return false;
        }
        std::wstring final_path;
        DirectoryIdentity identity;
        const bool path_ok = QueryFinalPath(handle.value, final_path);
        if (!path_ok) {
            out.error = GetLastError();
            if (out.error == 0)
                out.error = ERROR_BAD_PATHNAME;
            return false;
        }
        const bool identity_ok = QueryDirectoryIdentity(handle.value, identity);
        out.missing.assign(parts.begin() + static_cast<std::ptrdiff_t>(keep), parts.end());
        out.resolved = final_path;
        for (const auto &name : out.missing)
            out.resolved += L"\\" + name;
        out.identity.reset();
        if (keep == parts.size() && identity_ok)
            out.identity = identity;
        return true;
    }
    return false;
}

enum class RootOverlap { none, within_reference, alias_prone_name, unverifiable };
struct RootOverlapResult final {
    RootOverlap kind{RootOverlap::none};
    // Win32 error behind `unverifiable`, 0 when there is none.
    DWORD error{};
};
RootOverlapResult CheckMarkerRootOverlap(const std::filesystem::path &candidate,
                                         const std::filesystem::path &reference) {
    if (candidate.empty())
        return {};
    const auto candidate_path = NormalizedDrivePath(candidate);
    const auto reference_path = NormalizedDrivePath(reference);
    if (!candidate_path || !reference_path)
        return {RootOverlap::unverifiable, ERROR_BAD_PATHNAME};
    if (IsSameOrBeneath(candidate_path->wstring(), reference_path->wstring()))
        return {RootOverlap::within_reference, 0};
    // Win32 trims the end of the last name of a path more than the end of an inner
    // name (see HasTrimmedOrStreamName). The identity check below opens the root
    // as the last name ("foo"), but every later use appends a file or a child
    // name, which turns the root name into an inner name ("foo "). The two can be
    // different directories, so a name that ends in a dot or a space, a single dot
    // included, is refused whether or not it exists.
    for (const auto &part : candidate_path->relative_path()) {
        if (HasTrimmedOrStreamName(part.wstring()))
            return {RootOverlap::alias_prone_name, 0};
    }
    ResolvedMarkerRoot resolved_candidate, resolved_reference;
    if (!ResolveMarkerRoot(*candidate_path, resolved_candidate))
        return {RootOverlap::unverifiable, resolved_candidate.error};
    if (!ResolveMarkerRoot(*reference_path, resolved_reference))
        return {RootOverlap::unverifiable, resolved_reference.error};
    if (std::any_of(resolved_candidate.missing.begin(), resolved_candidate.missing.end(), IsAliasProneName))
        return {RootOverlap::alias_prone_name, 0};
    if (resolved_candidate.identity && resolved_reference.identity &&
        *resolved_candidate.identity == *resolved_reference.identity)
        return {RootOverlap::within_reference, 0};
    if (IsSameOrBeneath(resolved_candidate.resolved, resolved_reference.resolved))
        return {RootOverlap::within_reference, 0};
    return {};
}

// Marker roots handed to the lease for tests must never designate the
// production data root or anything inside it, by name or by alias. Called only
// when a test root is supplied; the default production path never reaches it.
// The reason is carried in the message so a rejected caller can tell a wrong
// location from an unusual name from a root that could not be checked.
void RejectProductionDataTestRoot(const std::filesystem::path &test_marker_root) {
    RootOverlapResult result{RootOverlap::unverifiable, ERROR_OUTOFMEMORY};
    try {
        result = CheckMarkerRootOverlap(test_marker_root, ProductionDataRoot());
    } catch (const TransportError &) {
        throw;
    } catch (const std::exception &) {
    }
    switch (result.kind) {
    case RootOverlap::none:
        return;
    case RootOverlap::within_reference:
        throw TransportError("camera_control_marker_failed",
                             "test marker root must not be the production data root or inside it");
    case RootOverlap::alias_prone_name:
        throw TransportError("camera_control_marker_failed",
                             "test marker root has a name that may be an alias of another directory");
    case RootOverlap::unverifiable:
        break;
    }
    throw TransportError("camera_control_marker_failed",
                         "test marker root could not be verified: " +
                             WindowsError("root verification", result.error));
}

} // namespace

bool IsHardwareProcessTestLeaseName(std::string_view lease_name) noexcept {
    return IsTestLeaseName(lease_name);
}

bool MarkerRootMayOverlap(const std::filesystem::path &candidate, const std::filesystem::path &reference) {
    return CheckMarkerRootOverlap(candidate, reference).kind != RootOverlap::none;
}

bool MarkerRootMayTouchProductionData(const std::filesystem::path &marker_root) {
    if (marker_root.empty())
        return false;
    return MarkerRootMayOverlap(marker_root, ProductionDataRoot());
}
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
    // The reverse pairing -- a test lease name with the production data root
    // (which holds the production marker root), anything inside it, or an alias
    // of either -- is refused here, before any directory is created or scanned.
    // Only a supplied test root reaches this check; the production default
    // (empty root) is unchanged.
    const bool test_root_supplied = !test_marker_root.empty();
    if (test_root_supplied)
        RejectProductionDataTestRoot(test_marker_root);
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
                if (test_root_supplied)
                    RejectProductionDataTestRoot(root);
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
        // Defense in depth: a name the file system rewrites when it is the last
        // name of a path is not a trusted ancestor (see HasTrimmedOrStreamName).
        if (HasTrimmedOrStreamName(part.wstring())) return false;
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
        // A test lease name with the production data root, anything inside it,
        // an alias of either, or a root that cannot be verified is refused; a
        // throw lands in the catch below and fails closed. This is after the SDK
        // build's refusal of every test root so that build does no file I/O here.
        if (!test_marker_root.empty() && MarkerRootMayTouchProductionData(test_marker_root))
            return false;
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

} // namespace a0::phase0
