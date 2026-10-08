#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"
#include <Windows.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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
bool Succeeds(const std::string &n, const std::filesystem::path &r) {
    try {
        HardwareProcessLease x(n, std::chrono::milliseconds(0), r);
        return true;
    } catch (const TransportError &) {
        return false;
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
    // A canonical-looking marker that belongs to a different session, with
    // this session's own marker entirely absent, is exactly one candidate
    // (walk.count == 1) whose name does not match this session's expected
    // name. That must still be reported as ambiguous, not silently folded
    // into "nothing matched" -- otherwise the diagnostic would read a
    // stranded foreign-session marker as if the root were empty.
    if (!write(other, valid)) return 3;
    Check(InspectDualDelegationMarkerReadOnly(root).status == "marker_ambiguous",
          "a single foreign-session marker without this session's own marker must not read as missing");
    if (!DeleteFileW(other.c_str())) return 3;
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
bool WriteFixtureFile(const std::filesystem::path &path, const std::string &content) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{};
    const bool ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                    written == content.size();
    CloseHandle(file);
    return ok;
}
// Directory junctions need no elevated privilege on Windows (unlike
// symlinks), so a plain `mklink /J` child process is enough to fabricate a
// reparse point for the fail-closed guard test below.
bool CreateJunctionFixture(const std::filesystem::path &link, const std::filesystem::path &target) {
    // Resolve cmd.exe by absolute path rather than letting CreateProcessW's
    // implicit PATH search find a relative "cmd.exe" (CWE-427, uncontrolled
    // search path element). Note for callers: cmd.exe expands "%" inside a
    // double-quoted argument as an environment-variable reference, so link
    // and target paths containing "%" would not survive this command line
    // unchanged; test fixture paths built under GetTempPathW never contain one.
    wchar_t system_directory[MAX_PATH]{};
    if (!GetSystemDirectoryW(system_directory, MAX_PATH)) return false;
    const auto cmd_exe = std::filesystem::path(system_directory) / L"cmd.exe";
    std::wstring command =
        L"\"" + cmd_exe.wstring() + L"\" /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &child))
        return false;
    CloseHandle(child.hThread);
    const DWORD wait = WaitForSingleObject(child.hProcess, 5000);
    DWORD exit_code{};
    const bool ok = wait == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &exit_code) && exit_code == 0;
    CloseHandle(child.hProcess);
    return ok && GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES;
}
// PM brief B (fail-closed cross-session delegation guard): a committed
// delegation marker from a different Windows session ID (sign-out/sign-in,
// reboot) must not go unnoticed just because RejectMarker only ever looked at
// *this* session's own marker filename. Each case below is an isolated
// Given/When/Then against a synthetic fixture root -- never the production
// marker root, never a real camera/SDK/WPD call (the lease layer cannot reach
// those at all, so a quarantine here structurally rules them out).
int RunCrossSessionMarkerGuardTests() {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 2;
    const auto root = std::filesystem::path(temp) /
        (L"A0LeaseCrossSessionGuardTest-" + std::to_wstring(GetCurrentProcessId()));
    const auto name = "A0.Poc.TestLease.CrossSessionGuard." + std::to_string(GetCurrentProcessId());
    DWORD own_session{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &own_session)) return 2;
    const DWORD foreign_session_a = own_session + 1;
    const DWORD foreign_session_b = own_session + 2;
    const std::string fixture_body = "synthetic fixture; the lease guard never parses marker content\n";
    const auto reset_root = [&] {
        std::filesystem::remove_all(root);
    };

    // Given: no marker root exists at all. When: a durable-marker lease is
    // acquired against that path. Then: acquisition succeeds (root absence is
    // indistinguishable from "zero markers" once the lease creates it).
    reset_root();
    Check(Succeeds(name, root), "no marker root present must allow lease acquisition");
    reset_root();

    // Given: the marker root exists but holds zero matching entries. When:
    // the lease is acquired. Then: acquisition succeeds.
    if (!std::filesystem::create_directories(root)) return 2;
    Check(Succeeds(name, root), "existing empty marker root must allow lease acquisition");
    reset_root();

    // Given: a single regular-file marker from a different session ID (e.g.
    // this session's own ID plus one). When: the lease is acquired. Then:
    // acquisition is quarantined, proving a cross-session marker is no longer
    // invisible after a sign-out/sign-in or reboot changes the session ID.
    if (!std::filesystem::create_directories(root)) return 2;
    const auto foreign_marker = root / (L"armed-session-" + std::to_wstring(foreign_session_a) + L".marker");
    if (!WriteFixtureFile(foreign_marker, fixture_body)) return 3;
    Check(Quarantined(name, root), "a foreign session's marker must quarantine lease acquisition");
    reset_root();

    // Given: two foreign-session markers. When: the lease is acquired. Then:
    // acquisition is quarantined (multiple strangers are no safer than one).
    if (!std::filesystem::create_directories(root)) return 2;
    const auto foreign_marker_a = root / (L"armed-session-" + std::to_wstring(foreign_session_a) + L".marker");
    const auto foreign_marker_b = root / (L"armed-session-" + std::to_wstring(foreign_session_b) + L".marker");
    if (!WriteFixtureFile(foreign_marker_a, fixture_body) || !WriteFixtureFile(foreign_marker_b, fixture_body))
        return 3;
    Check(Quarantined(name, root), "multiple foreign-session markers must quarantine lease acquisition");
    reset_root();

    // Given: a directory (not a file) whose name matches the marker glob.
    // When: the lease is acquired. Then: acquisition is quarantined -- the
    // scan never assumes a glob match is a regular file.
    if (!std::filesystem::create_directories(root)) return 2;
    const auto marker_shaped_directory =
        root / (L"armed-session-" + std::to_wstring(own_session + 3) + L".marker");
    if (!CreateDirectoryW(marker_shaped_directory.c_str(), nullptr)) return 2;
    Check(Quarantined(name, root), "a directory shaped like a marker name must quarantine lease acquisition");
    if (!RemoveDirectoryW(marker_shaped_directory.c_str())) return 4;
    reset_root();

    // Given: a reparse point (directory junction) whose name matches the
    // marker glob. When: the lease is acquired. Then: acquisition is
    // quarantined -- the scan never follows or trusts a reparse point.
    if (!std::filesystem::create_directories(root)) return 2;
    const auto junction_target = root / L"junction-target";
    if (!std::filesystem::create_directories(junction_target)) return 2;
    const auto marker_shaped_junction =
        root / (L"armed-session-" + std::to_wstring(own_session + 4) + L".marker");
    if (!CreateJunctionFixture(marker_shaped_junction, junction_target)) return 2;
    Check(Quarantined(name, root), "a reparse point shaped like a marker name must quarantine lease acquisition");
    // Unlink the reparse point itself first; remove_all must never be asked
    // to resolve into (and delete) the junction's target through the link.
    if (!RemoveDirectoryW(marker_shaped_junction.c_str())) return 4;
    reset_root();

    // Given: a file whose name satisfies the armed-session-*.marker glob but
    // is not the canonical armed-session-<digits>.marker form. When: the
    // lease is acquired. Then: acquisition is quarantined -- any glob match
    // is treated as a candidate, canonical or not.
    if (!std::filesystem::create_directories(root)) return 2;
    const auto non_canonical_marker = root / L"armed-session-abc.marker";
    if (!WriteFixtureFile(non_canonical_marker, fixture_body)) return 3;
    Check(Quarantined(name, root),
          "a non-canonical name that still matches the glob must quarantine lease acquisition");
    reset_root();

    // Given: files present under root whose names do not satisfy the
    // armed-session-*.marker glob at all (wrong trailing extension, unrelated
    // name). When: the lease is acquired. Then: acquisition succeeds -- a
    // fail-closed guard still must not quarantine on files that were never a
    // candidate in the first place.
    if (!std::filesystem::create_directories(root)) return 2;
    if (!WriteFixtureFile(root / L"armed-session-2.marker.bak", fixture_body) ||
        !WriteFixtureFile(root / L"notes.txt", fixture_body))
        return 3;
    Check(Succeeds(name, root), "non-matching file names must not block lease acquisition");
    reset_root();

    // Given: a marker name that differs from the canonical form only by
    // letter case. When: the lease is acquired. Then: acquisition is
    // quarantined -- Win32 FindFirstFileW/NTFS name matching is
    // case-insensitive, so the guard built on it must treat a case-varied
    // name as the same candidate rather than as a non-match.
    if (!std::filesystem::create_directories(root)) return 2;
    const auto case_varied_marker =
        root / (L"ARMED-SESSION-" + std::to_wstring(own_session + 5) + L".MARKER");
    if (!WriteFixtureFile(case_varied_marker, fixture_body)) return 3;
    Check(Quarantined(name, root), "a case-varied marker name must still quarantine lease acquisition");
    reset_root();

    std::cout << "{\"mode\":\"cross-session-guard-simulation\",\"failures\":" << failures << "}\n";
    return 0;
}

bool LeaseRejectsAsMarkerFailure(const std::string &n, const std::filesystem::path &r) {
    try {
        HardwareProcessLease x(n, std::chrono::milliseconds(0), r);
        return false;
    } catch (const TransportError &e) {
        return e.Category() == "camera_control_marker_failed";
    }
}
std::filesystem::path LowerCased(const std::filesystem::path &path) {
    std::wstring text = path.wstring();
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return std::filesystem::path(text);
}
// First drive letter (from Z down) that is neither a logical drive nor a DOS
// device, or 0 when none is free.
wchar_t FreeDriveLetter() {
    const DWORD in_use = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
        if ((in_use & (1u << (letter - L'A'))) != 0) continue;
        const wchar_t device[3] = {letter, L':', 0};
        wchar_t target[MAX_PATH]{};
        if (QueryDosDeviceW(device, target, MAX_PATH) == 0) return letter;
    }
    return 0;
}
// A subst-style drive mapping, removed on every exit path.
class SubstDrive final {
  public:
    SubstDrive(wchar_t letter, const std::filesystem::path &target)
        : device_{letter, L':', 0}, target_(L"\\??\\" + target.wstring()) {
        defined_ = DefineDosDeviceW(DDD_RAW_TARGET_PATH, device_, target_.c_str()) != FALSE;
    }
    ~SubstDrive() {
        if (defined_)
            DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_RAW_TARGET_PATH | DDD_EXACT_MATCH_ON_REMOVE,
                             device_, target_.c_str());
    }
    SubstDrive(const SubstDrive &) = delete;
    SubstDrive &operator=(const SubstDrive &) = delete;
    [[nodiscard]] bool Defined() const noexcept { return defined_; }
  private:
    wchar_t device_[3];
    std::wstring target_;
    bool defined_{};
};
struct DirectoryStamp final {
    bool exists{};
    FILETIME last_write{};
    DWORD attributes{};
    [[nodiscard]] bool operator==(const DirectoryStamp &other) const noexcept {
        return exists == other.exists && attributes == other.attributes &&
               last_write.dwLowDateTime == other.last_write.dwLowDateTime &&
               last_write.dwHighDateTime == other.last_write.dwHighDateTime;
    }
};
// Reads directory metadata only (no open, no enumeration of the production tree).
DirectoryStamp StampDirectory(const std::filesystem::path &path) {
    DirectoryStamp stamp;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        stamp.exists = true;
        stamp.last_write = data.ftLastWriteTime;
        stamp.attributes = data.dwFileAttributes;
    }
    return stamp;
}
// Issue #246: the lease layer must refuse "test lease name + production marker
// root" (the reverse of the existing "production name + test root" refusal),
// including aliases of the production root. The predicate cases use a scratch
// reference root, so nothing under the production marker root is touched. The
// lease-layer cases that name the production root are expected to throw before
// any file system access; the production directory metadata is compared before
// and after to prove it.
int RunAliasHardeningTests() {
    wchar_t temp[MAX_PATH]{}, long_temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 2;
    const DWORD long_length = GetLongPathNameW(temp, long_temp, MAX_PATH);
    const std::filesystem::path base(long_length > 0 && long_length < MAX_PATH ? long_temp : temp);
    const auto scratch = base / (L"A0LeaseAliasHardeningTest-" + std::to_wstring(GetCurrentProcessId()));
    const auto name = "A0.Poc.TestLease.AliasHardening." + std::to_string(GetCurrentProcessId());
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
    const auto reference = scratch / L"Phase0Like" / L"DualDelegation";
    if (!std::filesystem::create_directories(reference)) return 2;
    const auto alias = [](const std::filesystem::path &candidate, const std::filesystem::path &ref) {
        return DualDelegationMarkerRootMayAlias(candidate, ref);
    };
    std::string notes;

    // Reference exists: spellings of the same directory.
    Check(alias(reference, reference), "identical root must alias");
    Check(alias(reference.wstring() + L"\\", reference), "trailing separator must alias");
    Check(alias(std::filesystem::path(reference.generic_wstring()), reference), "forward slashes must alias");
    Check(alias(LowerCased(reference), reference), "case variant must alias");
    Check(alias(scratch / L"not-created" / L".." / L"Phase0Like" / L"DualDelegation", reference),
          "dot-dot spelling must alias");
    Check(alias(reference / L".." / L"DualDelegation", reference), "dot-dot through an existing directory must alias");
    Check(alias(reference.wstring() + L".", reference), "trailing dot on an existing root must alias");
    Check(alias(reference.wstring() + L" ", reference), "trailing space on an existing root must alias");
    Check(alias(reference.wstring() + L"::$INDEX_ALLOCATION", reference), "stream selector on a root must alias");
    // Reference exists: things that are not the reference.
    const auto sibling = scratch / L"Phase0Like" / L"Sibling";
    if (!std::filesystem::create_directories(sibling)) return 2;
    Check(!alias(reference / L"nested", reference), "a child of the reference must not alias");
    Check(!alias(sibling, reference), "an existing sibling must not alias");
    Check(!alias(scratch / L"Phase0Like" / L"AbsentSibling", reference), "an absent sibling must not alias");
    Check(!alias(std::filesystem::path{}, reference), "an empty root is not a root and must not alias");
    // Fail-closed: not an absolute drive-letter path, or a drive that cannot be opened.
    Check(alias(std::filesystem::path(L"relative\\root"), reference), "a relative root must be unverifiable");
    Check(alias(std::filesystem::path(L"\\\\?\\" + reference.wstring()), reference),
          "an extended-length prefix must be unverifiable");
    if (const wchar_t letter = FreeDriveLetter(); letter != 0) {
        const std::wstring absent_drive = std::wstring(1, letter) + L":\\x\\y";
        Check(alias(absent_drive, reference), "a root on an absent drive must be unverifiable");
    } else {
        notes += "absent-drive case not run: no free drive letter; ";
    }

    // Reference does not exist yet: names the file system would rewrite.
    const auto absent_reference = scratch / L"Absent" / L"DualDelegation";
    Check(alias(absent_reference, absent_reference), "identical absent root must alias");
    Check(alias(absent_reference.wstring() + L".", absent_reference), "trailing dot on an absent root must alias");
    Check(alias(absent_reference.wstring() + L" ", absent_reference), "trailing space on an absent root must alias");
    Check(alias(scratch / L"ABSEN~1" / L"DualDelegation", absent_reference), "short-name syntax on an absent root must alias");
    Check(alias(scratch / L"Absent" / L"DUALDE~1", absent_reference), "short-name syntax in the leaf must alias");
    Check(alias(scratch / L"Absent:stream" / L"DualDelegation", absent_reference), "stream selector must alias");
    Check(!alias(scratch / L"Absent" / L"Other", absent_reference), "an absent sibling must not alias an absent root");
    Check(!alias(absent_reference / L"nested", absent_reference), "a child of an absent root must not alias");
    Check(!std::filesystem::exists(scratch / L"Absent"), "alias checks must not create directories");

    // 8.3 short names: generated only where the volume has them enabled.
    const auto long_dir = scratch / L"AliasLongDirectoryName";
    const auto long_reference = long_dir / L"Phase0Like2" / L"DualDelegation";
    if (!std::filesystem::create_directories(long_reference)) return 2;
    wchar_t short_buffer[MAX_PATH]{};
    const DWORD short_length = GetShortPathNameW(long_dir.c_str(), short_buffer, MAX_PATH);
    const bool short_name_generated = short_length > 0 && short_length < MAX_PATH &&
        std::wstring_view(short_buffer).find(L'~') != std::wstring_view::npos;
    if (short_name_generated) {
        const std::filesystem::path short_dir(short_buffer);
        const auto short_reference = short_dir / L"Phase0Like2" / L"DualDelegation";
        Check(alias(short_reference, long_reference), "8.3 short name of an existing root must alias");
        Check(alias(short_reference / L"nested", long_reference / L"nested"),
              "8.3 short name in front of an absent leaf must alias");
        Check(!alias(short_reference / L"other", long_reference / L"nested"),
              "8.3 short name in front of a different absent leaf must not alias");
        // An existing ancestor spelled with a short name is not itself a reason
        // to refuse (a short TEMP path is legitimate); only the absent tail is.
        Check(!alias(short_dir / L"NewRoot", reference), "a short-named existing ancestor must not alias by itself");
    } else {
        notes += "8.3 alias cases not run: GetShortPathNameW produced no short name for a 24-character directory "
                 "(8.3 generation is disabled on this volume); ";
    }

    // Junction: resolved through the final path and refused by the lease layer.
    const auto junction = scratch / L"junction-to-reference";
    if (CreateJunctionFixture(junction, reference)) {
        Check(alias(junction, reference), "a junction to the reference must alias");
        Check(!alias(junction / L"nested", reference), "a child of a junction to the reference must not alias");
        Check(LeaseRejectsAsMarkerFailure(name, junction),
              "a junction marker root must be refused by the lease layer");
        RemoveDirectoryW(junction.c_str());
    } else {
        notes += "junction case not run: mklink /J failed; ";
    }

    // subst-style drive mapped onto the reference's parent.
    if (const wchar_t letter = FreeDriveLetter(); letter != 0) {
        SubstDrive mapped(letter, scratch / L"Phase0Like");
        if (mapped.Defined()) {
            const std::filesystem::path drive(std::wstring(1, letter) + L":\\");
            Check(alias(drive / L"DualDelegation", reference), "a subst drive onto the parent must alias an existing root");
            Check(alias(drive / L"NotYet" / L"DualDelegation", scratch / L"Phase0Like" / L"NotYet" / L"DualDelegation"),
                  "a subst drive must alias an absent root through its existing ancestor");
            Check(!alias(drive / L"Sibling", reference), "a subst drive onto a sibling must not alias");
        } else {
            notes += "subst case not run: DefineDosDeviceW failed; ";
        }
    } else {
        notes += "subst case not run: no free drive letter; ";
    }

    // Lease layer, scratch roots: an alias-prone name that does not exist yet is
    // refused before anything is created, even though it is not the production root.
    const auto tilde_root = scratch / L"Lease~1";
    const auto dotted_root = scratch / L"leaseDotted.";
    Check(LeaseRejectsAsMarkerFailure(name, tilde_root) && !std::filesystem::exists(tilde_root),
          "short-name syntax must be refused by the lease before any directory is created");
    Check(LeaseRejectsAsMarkerFailure(name, dotted_root) && !std::filesystem::exists(scratch / L"leaseDotted"),
          "a trailing dot must be refused by the lease before any directory is created");
    Check(Succeeds(name, scratch / L"LeaseOrdinary"), "an ordinary absent scratch root must still be accepted");

    // Lease layer, production root and spellings of it. Nothing may be created
    // or changed there.
    wchar_t local_app_data[MAX_PATH]{};
    const DWORD local_app_data_length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    Check(local_app_data_length > 0 && local_app_data_length < MAX_PATH, "LOCALAPPDATA must be resolvable");
    if (local_app_data_length > 0 && local_app_data_length < MAX_PATH) {
        const auto production = std::filesystem::path(local_app_data) / L"A0CameraStitcher" / L"Phase0" / L"DualDelegation";
        const auto production_parent_stamp = StampDirectory(production.parent_path());
        const auto production_stamp = StampDirectory(production);
        std::vector<std::filesystem::path> spellings = {
            production,
            production.wstring() + L"\\",
            std::filesystem::path(production.generic_wstring()),
            LowerCased(production),
            production.wstring() + L".",
            production / L"x" / L"..",
            std::filesystem::path(local_app_data) / L"A0CameraStitcher" / L"Phase0" / L"..\\Phase0\\DualDelegation",
        };
        wchar_t production_short[MAX_PATH]{};
        const DWORD production_short_length = GetShortPathNameW(production.c_str(), production_short, MAX_PATH);
        if (production_short_length > 0 && production_short_length < MAX_PATH &&
            std::wstring_view(production_short).find(L'~') != std::wstring_view::npos)
            spellings.emplace_back(production_short);
        else
            notes += "production 8.3 spelling not run: the production root is absent or has no short name; ";
        for (const auto &spelling : spellings) {
            // Predicate first: it has no side effects, so a regression is reported
            // without the constructor ever being pointed at the production root.
            const bool predicate = IsProductionDualDelegationMarkerRoot(spelling);
            Check(predicate, "a production root spelling must be recognised");
            if (!predicate) continue;
            Check(LeaseRejectsAsMarkerFailure(name, spelling),
                  "a test lease name with the production marker root must be refused by the lease");
            Check(LeaseRejectsAsMarkerFailure("A0CameraStitcher.Phase0.Test.AliasHardening", spelling),
                  "the second test lease name prefix with the production marker root must be refused");
        }
        Check(StampDirectory(production) == production_stamp && StampDirectory(production.parent_path()) == production_parent_stamp,
              "refusing the production root must not create or change anything under it");
        Check(!IsProductionDualDelegationMarkerRoot(std::filesystem::path{}), "an empty root must not be the production root");
        Check(!IsProductionDualDelegationMarkerRoot(production / L"nested"), "a child of the production root must not match");
        Check(!IsProductionDualDelegationMarkerRoot(scratch / L"LeaseOrdinary"), "a scratch root must not match");
    }

    std::filesystem::remove_all(scratch, ignored);
    std::cout << "{\"mode\":\"alias-hardening-simulation\",\"failures\":" << failures
              << ",\"short_name_generated\":" << (short_name_generated ? "true" : "false")
              << ",\"notes\":\"" << notes << "\"}\n";
    return 0;
}
// Defense in depth (CWE-73, external control of file name/path) for the only
// entry point that lets argv reach HardwareProcessLease's constructor as a
// lease name and marker root: a production-shaped lease name paired with an
// empty root would otherwise fall through to the *production* marker root
// inside the constructor (test_marker_root.empty() there means "use
// production"), so this process never even calls that constructor unless the
// lease name is unambiguously test-shaped and the root is a non-empty
// absolute path.
bool IsAcceptableTestInvocation(std::string_view lease_name, const std::filesystem::path &root) {
    return lease_name.starts_with("A0.Poc.TestLease.") && !root.empty() && root.is_absolute();
}
} // namespace
int RunMain(int argc, char **argv);
// Surface uncaught non-TransportError exceptions as a readable failure instead of
// an abort() with no output, so a CTest failure records the reason. The
// exception text itself is not printed: std::filesystem failures embed the
// absolute path they operated on in what(), and a test fixture path is not
// meant to end up in captured CI output.
int main(int argc, char **argv) {
    try {
        return RunMain(argc, argv);
    } catch (const std::exception &) {
        std::cerr << "uncaught exception (see process return code)\n";
        return 99;
    }
}
int RunMain(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--clean-exit") return 0;
    if (argc == 2 && std::string_view(argv[1]) == "--diagnostic") return RunDiagnosticTests();
    if (argc == 4 && !IsAcceptableTestInvocation(argv[2], std::filesystem::path(argv[3])))
        return 2;
    const auto root = argc == 4 ? std::filesystem::path(argv[3]) : Root();
    const auto name = argc == 4 ? std::string(argv[2]) : Name();
    if (argc == 4 && std::string_view(argv[1]) == "--crash") {
        HardwareProcessLease x(name, std::chrono::milliseconds(0), root);
        x.ArmDualDelegation();
        ExitProcess(91);
    }
    // Reaching here means this is the top-level invocation (the --clean-exit,
    // --diagnostic, and --crash branches above all return/exit before this
    // point), so it is safe to run the fail-closed cross-session guard suite
    // once, against its own isolated fixture root.
    if (const auto guard_rc = RunCrossSessionMarkerGuardTests(); guard_rc != 0) return guard_rc;
    if (const auto alias_rc = RunAliasHardeningTests(); alias_rc != 0) return alias_rc;
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
