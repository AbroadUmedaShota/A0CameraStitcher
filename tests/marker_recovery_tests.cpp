// Audited marker recovery contracts (docs/design/dual-preview-topology-diag.md
// section 9, acceptance T-h). Every case runs against a synthetic v2 marker
// under a per-process test root with a test lease name; the production marker
// root, the production camera-control mutex and real cameras are never used.
// PnP and A0-process probes are injected except where a case says otherwise.
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace a0::phase0;
namespace fs = std::filesystem;
using Probe = DualDelegationRecoveryProbe;
using Point = DualDelegationRecoveryTestPoint;

namespace {

int failures{};
int checks{};
std::vector<std::string> leak_corpus;

void Check(bool ok, const std::string &message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr DWORD kOwnerPid = 4000000000UL;
constexpr DWORD kWorkerAPid = 4000000004UL;
constexpr DWORD kWorkerBPid = 4000000008UL;
constexpr DWORD kForeignSession = 3999999999UL;
constexpr std::string_view kNonceStem = "5eed7777";
constexpr std::string_view kEpochStem = "e90c3333";

std::wstring Wide(std::string_view value) {
    return {value.begin(), value.end()};
}
DWORD OwnSession() {
    DWORD session{};
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return session;
}
fs::path OwnExe() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path);
}
fs::path TestBase() {
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    wchar_t long_path[MAX_PATH]{};
    const DWORD long_length = length > 0 ? GetLongPathNameW(temp, long_path, MAX_PATH) : 0;
    const fs::path base = long_length > 0 && long_length < MAX_PATH ? fs::path(long_path) : fs::path(temp);
    return (base / (L"A0MarkerRecoveryTest-" + std::to_wstring(GetCurrentProcessId()))).lexically_normal();
}
std::string Hex8(unsigned value) {
    char text[9]{};
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
}
std::string Sha(const std::string &text) {
    return Sha256Hex(std::vector<unsigned char>(text.begin(), text.end()));
}

struct Fixture final {
    std::string name;
    fs::path base, root, audit_dir, audit_file;
    std::string lease;
    std::string nonce, epoch;
    std::chrono::system_clock::time_point now{std::chrono::system_clock::now()};
    Probe camera{Probe::absent};
    Probe a0{Probe::absent};
    bool real_camera_probe{};
    bool real_a0_probe{};
    std::function<void(Point)> hook;

    DualDelegationMarkerRecoveryOptions Options() {
        DualDelegationMarkerRecoveryOptions options;
        options.test_marker_root = root;
        options.test_lease_name = lease;
        if (!real_camera_probe) options.test_camera_probe = [this] { return camera; };
        if (!real_a0_probe) options.test_a0_process_probe = [this] { return a0; };
        options.test_now = [this] { return now; };
        options.test_hook = [this](Point point) {
            if (hook) hook(point);
        };
        return options;
    }
    std::string Contents(DWORD owner = kOwnerPid, DWORD worker_a = kWorkerAPid, DWORD worker_b = kWorkerBPid) const {
        return "a0-dual-delegation-v2\nnonce=" + nonce + "\nownerPid=" + std::to_string(owner) + "\nepoch=" + epoch +
            "\nworkerAPid=" + std::to_string(worker_a) + "\nworkerBPid=" + std::to_string(worker_b) + "\n";
    }
    fs::path Marker(DWORD session) const {
        return root / (L"armed-session-" + std::to_wstring(session) + L".marker");
    }
    std::wstring MutexName() const {
        return L"Local\\" + Wide(lease);
    }
};

std::unique_ptr<Fixture> MakeFixture(std::string_view name) {
    static unsigned counter;
    ++counter;
    auto fixture = std::make_unique<Fixture>();
    fixture->name = std::string(name);
    fixture->base = TestBase() / Wide(name);
    std::error_code error;
    fs::remove_all(fixture->base, error);
    fixture->root = fixture->base / L"DualDelegation";
    fixture->audit_dir = fixture->base / L"DualDelegationRecovery";
    fixture->audit_file = fixture->audit_dir / L"audit.jsonl";
    fs::create_directories(fixture->root, error);
    fixture->lease = "A0.Poc.TestLease.Recovery." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(counter);
    fixture->nonce = std::string(kNonceStem) + std::string(16, '7') + Hex8(counter);
    fixture->epoch = std::string(kEpochStem) + std::string(16, '3') + Hex8(counter);
    return fixture;
}

bool WriteText(const fs::path &path, const std::string &content) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{};
    const bool ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                    written == content.size();
    CloseHandle(file);
    return ok;
}
std::string ReadText(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct Identity final {
    bool ok{};
    std::string contents;
    FILETIME write{};
    DWORD volume{}, index_high{}, index_low{}, links{};
};
Identity Capture(const fs::path &path) {
    Identity identity;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return identity;
    BY_HANDLE_FILE_INFORMATION info{};
    char buffer[512]{};
    DWORD read{};
    if (GetFileInformationByHandle(file, &info) && ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
        identity.ok = true;
        identity.contents.assign(buffer, read);
        identity.write = info.ftLastWriteTime;
        identity.volume = info.dwVolumeSerialNumber;
        identity.index_high = info.nFileIndexHigh;
        identity.index_low = info.nFileIndexLow;
        identity.links = info.nNumberOfLinks;
    }
    CloseHandle(file);
    return identity;
}
bool SameIdentity(const Identity &a, const Identity &b) {
    return a.ok && b.ok && a.contents == b.contents && CompareFileTime(&a.write, &b.write) == 0 &&
           a.volume == b.volume && a.index_high == b.index_high && a.index_low == b.index_low;
}

std::vector<std::string> AuditLines(const Fixture &fixture) {
    std::vector<std::string> lines;
    const auto text = ReadText(fixture.audit_file);
    std::size_t start{};
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        if (end == std::string::npos) break;
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}
std::string Field(const std::string &line, std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\":";
    const auto at = line.find(quoted);
    if (at == std::string::npos) return {};
    auto begin = at + quoted.size();
    if (begin < line.size() && line[begin] == '"') {
        const auto end = line.find('"', begin + 1);
        return end == std::string::npos ? std::string{} : line.substr(begin + 1, end - begin - 1);
    }
    auto end = line.find_first_of(",}", begin);
    return line.substr(begin, end - begin);
}
bool AuditHasRecord(const Fixture &fixture, std::string_view record) {
    for (const auto &line : AuditLines(fixture))
        if (Field(line, "record") == record) return true;
    return false;
}
std::vector<std::wstring> RootEntries(const Fixture &fixture) {
    std::vector<std::wstring> entries;
    WIN32_FIND_DATAW entry{};
    HANDLE found = FindFirstFileW((fixture.root / L"*").c_str(), &entry);
    if (found == INVALID_HANDLE_VALUE) return entries;
    do {
        const std::wstring_view name(entry.cFileName);
        if (name != L"." && name != L"..") entries.emplace_back(name);
    } while (FindNextFileW(found, &entry));
    FindClose(found);
    return entries;
}
bool AnyMarker(const Fixture &fixture) {
    WIN32_FIND_DATAW entry{};
    HANDLE found = FindFirstFileW((fixture.root / L"armed-session-*.marker").c_str(), &entry);
    if (found == INVALID_HANDLE_VALUE) return GetLastError() != ERROR_FILE_NOT_FOUND;
    FindClose(found);
    return true;
}
std::string Json(const DualDelegationMarkerRecoveryResult &result) {
    return "{\"status\":\"" + result.status + "\",\"anonymousSha256\":\"" + result.anonymous_sha256 +
        "\",\"size\":" + std::to_string(result.size) + ",\"sessionMatch\":" + (result.session_match ? "1" : "0") + "}";
}
void Collect(const DualDelegationMarkerRecoveryResult &result) {
    leak_corpus.push_back(Json(result));
}
void CollectAudit(const Fixture &fixture) {
    leak_corpus.push_back(ReadText(fixture.audit_file));
}

bool Quarantined(const Fixture &fixture) {
    try {
        HardwareProcessLease lease(fixture.lease, std::chrono::milliseconds(0), fixture.root);
        return false;
    } catch (const TransportError &error) {
        return error.Category() == "camera_control_delegation_quarantined";
    }
}
bool LeaseSucceeds(const Fixture &fixture) {
    try {
        HardwareProcessLease lease(fixture.lease, std::chrono::milliseconds(0), fixture.root);
        return true;
    } catch (const TransportError &) {
        return false;
    }
}

class ChildProcess final {
  public:
    ChildProcess() = default;
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;
    ~ChildProcess() { Stop(); }
    bool StartSuspended(const fs::path &exe) {
        std::wstring command = L"\"" + exe.wstring() + L"\" --suspended-child";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        started_ = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                                  CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info_) != FALSE;
        return started_;
    }
    [[nodiscard]] DWORD Pid() const { return started_ ? info_.dwProcessId : 0; }
    void Stop() {
        if (!started_) return;
        TerminateProcess(info_.hProcess, 0);
        WaitForSingleObject(info_.hProcess, 10000);
        CloseHandle(info_.hThread);
        CloseHandle(info_.hProcess);
        started_ = false;
    }

  private:
    PROCESS_INFORMATION info_{};
    bool started_{};
};

// Directory junctions need no elevation; cmd.exe is resolved by absolute path.
bool CreateJunction(const fs::path &link, const fs::path &target) {
    wchar_t system_directory[MAX_PATH]{};
    if (!GetSystemDirectoryW(system_directory, MAX_PATH)) return false;
    const auto cmd_exe = fs::path(system_directory) / L"cmd.exe";
    std::wstring command =
        L"\"" + cmd_exe.wstring() + L"\" /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &child))
        return false;
    CloseHandle(child.hThread);
    const DWORD wait = WaitForSingleObject(child.hProcess, 10000);
    DWORD exit_code{};
    const bool ok = wait == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &exit_code) && exit_code == 0;
    CloseHandle(child.hProcess);
    const DWORD attributes = GetFileAttributesW(link.c_str());
    return ok && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

struct CliRun final {
    bool started{};
    bool finished{};
    DWORD exit_code{0xFFFFFFFFUL};
    std::string out;
    std::string err;
};
std::string Drain(HANDLE reader) {
    std::string text;
    char buffer[1024];
    DWORD read{};
    while (ReadFile(reader, buffer, sizeof(buffer), &read, nullptr) && read > 0) text.append(buffer, read);
    return text;
}
CliRun RunCli(const std::vector<std::wstring> &arguments) {
    CliRun run;
    const auto cli = OwnExe().parent_path() / L"A0CameraStitcher.MarkerRecovery.exe";
    std::wstring command = L"\"" + cli.wstring() + L"\"";
    for (const auto &argument : arguments) command += L" \"" + argument + L"\"";
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE out_read{}, out_write{}, err_read{}, err_write{};
    if (!CreatePipe(&out_read, &out_write, &security, 0)) return run;
    if (!CreatePipe(&err_read, &err_write, &security, 0)) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        return run;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out_write;
    startup.hStdError = err_write;
    PROCESS_INFORMATION child{};
    run.started = CreateProcessW(cli.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &startup, &child) != FALSE;
    CloseHandle(out_write);
    CloseHandle(err_write);
    if (run.started) {
        run.finished = WaitForSingleObject(child.hProcess, 60000) == WAIT_OBJECT_0;
        if (!run.finished) {
            TerminateProcess(child.hProcess, 99);
            WaitForSingleObject(child.hProcess, 10000);
        }
        GetExitCodeProcess(child.hProcess, &run.exit_code);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        run.out = Drain(out_read);
        run.err = Drain(err_read);
    }
    CloseHandle(out_read);
    CloseHandle(err_read);
    return run;
}
std::vector<std::wstring> TestArgs(const Fixture &fixture, std::vector<std::wstring> arguments) {
    arguments.push_back(L"--test-root");
    arguments.push_back(fixture.root.wstring());
    arguments.push_back(L"--test-lease");
    arguments.push_back(Wide(fixture.lease));
    return arguments;
}

DualDelegationMarkerRecoveryResult DryRun(Fixture &fixture, std::string_view expected = {}) {
    auto result = DryRunDualDelegationMarkerRecovery(fixture.Options(), expected);
    Collect(result);
    return result;
}
DualDelegationMarkerRecoveryResult Execute(Fixture &fixture, std::string_view expected, bool attested = true,
                                           bool foreign = false) {
    auto result = ExecuteDualDelegationMarkerRecovery(fixture.Options(), expected, attested, foreign);
    Collect(result);
    return result;
}

// h1: single canonical marker for this session; dry run is read-only apart
// from exactly one audit line outside the marker root.
void CaseH1() {
    auto f = MakeFixture("h1");
    const auto contents = f->Contents();
    const auto marker = f->Marker(OwnSession());
    Check(WriteText(marker, contents), "h1 fixture");
    const auto before = Capture(marker);
    const auto result = DryRun(*f);
    Check(result.status == "recovery_eligible", "h1 status " + result.status);
    Check(result.anonymous_sha256 == Sha(contents) && result.size == contents.size(), "h1 hash/size");
    Check(result.session_match, "h1 sessionMatch must be 1");
    Check(SameIdentity(before, Capture(marker)), "h1 marker contents/time/index unchanged");
    const auto lines = AuditLines(*f);
    Check(lines.size() == 1 && Field(lines[0], "record") == "dry_run" &&
              Field(lines[0], "status") == "recovery_eligible" &&
              Field(lines[0], "anonymousSha256") == result.anonymous_sha256 && Field(lines[0], "sessionMatch") == "1" &&
              Field(lines[0], "operatorAttested") == "0" && Field(lines[0], "toolSha256").size() == 64,
          "h1 one dry_run audit line");
    Check(RootEntries(*f).size() == 1, "h1 marker root gains no file");
    CollectAudit(*f);
}

// h2: canonical marker of another session is eligible with sessionMatch 0.
void CaseH2() {
    auto f = MakeFixture("h2");
    Check(WriteText(f->Marker(kForeignSession), f->Contents()), "h2 fixture");
    const auto result = DryRun(*f);
    Check(result.status == "recovery_eligible" && !result.session_match, "h2 foreign session " + result.status);
    CollectAudit(*f);
}

// h3: names that satisfy the glob but not the canonical form stop recovery.
void CaseH3() {
    for (const std::wstring name : {L"armed-session-01.marker", L"armed-session-x.marker",
                                    L"armed-session-4294967296.marker", L"ARMED-SESSION-1.MARKER"}) {
        auto f = MakeFixture("h3-" + std::to_string(name.size()) + "-" + std::to_string(name[0]));
        const auto marker = f->root / name;
        Check(WriteText(marker, f->Contents()), "h3 fixture");
        const auto before = Capture(marker);
        const auto result = DryRun(*f);
        Check(result.status == "marker_name_noncanonical", "h3 noncanonical name " + result.status);
        Check(SameIdentity(before, Capture(marker)) && !fs::exists(f->audit_dir), "h3 untouched");
    }
}

// h4 plus brief: two markers, canonical directory, oversize, bad syntax and
// junctions (canonical and non-canonical names).
void CaseH4() {
    {
        auto f = MakeFixture("h4-two");
        Check(WriteText(f->Marker(OwnSession()), f->Contents()) && WriteText(f->Marker(kForeignSession), f->Contents()),
              "h4 two fixture");
        Check(DryRun(*f).status == "marker_ambiguous", "h4 two markers must be ambiguous");
    }
    {
        auto f = MakeFixture("h4-dir");
        Check(CreateDirectoryW(f->Marker(OwnSession()).c_str(), nullptr) != FALSE, "h4 dir fixture");
        Check(DryRun(*f).status == "marker_invalid", "h4 directory with the marker name must be invalid");
        Check(fs::is_directory(f->Marker(OwnSession())), "h4 directory retained");
    }
    {
        auto f = MakeFixture("h4-size");
        Check(WriteText(f->Marker(OwnSession()), std::string(256, 'x')), "h4 size fixture");
        Check(DryRun(*f).status == "marker_invalid", "h4 256-byte marker must be invalid");
    }
    {
        auto f = MakeFixture("h4-syntax");
        Check(WriteText(f->Marker(OwnSession()), "a0-dual-delegation-v2\nnonce=zz\n"), "h4 syntax fixture");
        Check(DryRun(*f).status == "marker_invalid", "h4 bad v2 syntax must be invalid");
    }
    {
        auto f = MakeFixture("h4-junction");
        const auto target = f->base / L"junction-target";
        fs::create_directories(target);
        Check(CreateJunction(f->Marker(OwnSession()), target), "h4 canonical junction fixture");
        Check(DryRun(*f).status == "marker_invalid", "h4 canonical-name junction must be invalid");
        RemoveDirectoryW(f->Marker(OwnSession()).c_str());
        Check(CreateJunction(f->root / L"armed-session-x.marker", target), "h4 noncanonical junction fixture");
        Check(DryRun(*f).status == "marker_name_noncanonical", "h4 noncanonical junction must stop");
        RemoveDirectoryW((f->root / L"armed-session-x.marker").c_str());
        Check(fs::is_directory(target), "h4 junction target retained");
    }
}

// h5: process list and PnP probes, injected; execute re-takes them under the hold.
void CaseH5() {
    auto f = MakeFixture("h5");
    const auto marker = f->Marker(OwnSession());
    Check(WriteText(marker, f->Contents()), "h5 fixture");
    f->a0 = Probe::present;
    Check(DryRun(*f).status == "a0_process_present", "h5 a0 process present");
    f->a0 = Probe::unavailable;
    Check(DryRun(*f).status == "process_list_unavailable", "h5 process list unavailable");
    f->a0 = Probe::absent;
    f->camera = Probe::present;
    Check(DryRun(*f).status == "camera_present", "h5 camera present");
    f->camera = Probe::unavailable;
    Check(DryRun(*f).status == "pnp_unavailable", "h5 pnp unavailable");
    Check(!fs::exists(f->audit_dir), "h5 failed dry runs write no audit");
    f->camera = Probe::absent;
    const auto eligible = DryRun(*f);
    Check(eligible.status == "recovery_eligible", "h5 eligible dry run");
    const auto before = Capture(marker);
    f->camera = Probe::present;
    Check(Execute(*f, eligible.anonymous_sha256).status == "camera_present", "h5 execute re-checks PnP under the hold");
    Check(SameIdentity(before, Capture(marker)) && !AuditHasRecord(*f, "execute_intent"), "h5 marker kept, no intent");
}

// Brief: a real process named A0CameraStitcher.* (suspended copy of this exe)
// stops recovery through the real Toolhelp probe.
void CaseRealA0Process() {
    auto f = MakeFixture("real-a0");
    Check(WriteText(f->Marker(OwnSession()), f->Contents()), "real-a0 fixture");
    const auto copy = f->base / L"A0CameraStitcher.RecoveryTestChild.exe";
    Check(CopyFileW(OwnExe().c_str(), copy.c_str(), FALSE) != FALSE, "real-a0 copy exe");
    ChildProcess child;
    Check(child.StartSuspended(copy), "real-a0 start suspended child");
    f->real_a0_probe = true;
    Check(DryRun(*f).status == "a0_process_present", "real-a0 live A0CameraStitcher.* process must stop");
    child.Stop();
    DeleteFileW(copy.c_str());
}

// h6 plus brief: a live recorded PID stops recovery (own PID as owner, live
// child as worker A).
void CaseH6() {
    {
        auto f = MakeFixture("h6-owner");
        Check(WriteText(f->Marker(OwnSession()), f->Contents(GetCurrentProcessId())), "h6 owner fixture");
        Check(DryRun(*f).status == "process_active_or_unknown", "h6 live owner PID");
    }
    {
        auto f = MakeFixture("h6-worker");
        ChildProcess child;
        Check(child.StartSuspended(OwnExe()), "h6 child");
        Check(WriteText(f->Marker(OwnSession()), f->Contents(kOwnerPid, child.Pid())), "h6 worker fixture");
        Check(DryRun(*f).status == "process_active_or_unknown", "h6 live worker A PID");
    }
}

// h7: the test mutex held by another thread, then abandoned by a thread.
void CaseH7() {
    auto f = MakeFixture("h7");
    const auto marker = f->Marker(OwnSession());
    Check(WriteText(marker, f->Contents()), "h7 fixture");
    const auto eligible = DryRun(*f);
    Check(eligible.status == "recovery_eligible", "h7 eligible");
    const auto before = Capture(marker);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto mutex_name = f->MutexName();
    std::thread holder([&] {
        HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
        SetEvent(ready);
        WaitForSingleObject(release, 30000);
        if (mutex) {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
    });
    WaitForSingleObject(ready, 10000);
    Check(DryRun(*f).status == "camera_control_busy", "h7 dry run busy");
    Check(Execute(*f, eligible.anonymous_sha256).status == "camera_control_busy", "h7 execute busy");
    SetEvent(release);
    holder.join();
    CloseHandle(ready);
    CloseHandle(release);
    Check(SameIdentity(before, Capture(marker)) && !AuditHasRecord(*f, "execute_intent"), "h7 busy keeps marker");

    auto g = MakeFixture("h7-abandoned");
    Check(WriteText(g->Marker(OwnSession()), g->Contents()), "h7 abandoned fixture");
    HANDLE kept{};
    const auto abandoned_name = g->MutexName();
    std::thread abandoner([&] { kept = CreateMutexW(nullptr, TRUE, abandoned_name.c_str()); });
    abandoner.join();
    Check(kept != nullptr, "h7 abandoned mutex created");
    Check(DryRun(*g).status == "camera_control_abandoned", "h7 abandoned mutex stops this round");
    Check(DryRun(*g).status == "recovery_eligible", "h7 next round after release is eligible");
    if (kept) CloseHandle(kept);
}

// h8: execute preconditions before the mutex (P10, P11).
void CaseH8() {
    auto f = MakeFixture("h8");
    const auto contents = f->Contents();
    const auto marker = f->Marker(OwnSession());
    Check(WriteText(marker, contents), "h8 fixture");
    const auto before = Capture(marker);
    Check(Execute(*f, Sha(contents)).status == "dry_run_record_missing", "h8 execute without dry run");
    const auto eligible = DryRun(*f);
    Check(eligible.status == "recovery_eligible", "h8 eligible");
    // Design T-h lists hash_mismatch here; step 2 (P11) runs before step 4 (P5),
    // so an unapproved hash stops as dry_run_record_missing. hash_mismatch is
    // covered by h10 (marker rewritten after the dry run).
    Check(Execute(*f, Sha("other\n")).status == "dry_run_record_missing", "h8 execute with an unapproved hash");
    Check(Execute(*f, eligible.anonymous_sha256, false).status == "operator_attestation_missing",
          "h8 missing camera attestation");
    f->now += std::chrono::minutes(31);
    Check(Execute(*f, eligible.anonymous_sha256).status == "dry_run_record_missing", "h8 dry run older than 30 min");
    Check(SameIdentity(before, Capture(marker)) && AuditLines(*f).size() == 1, "h8 marker kept, audit has dry_run only");
}

// h9: another handle on the marker makes the share-none open fail.
void CaseH9() {
    auto f = MakeFixture("h9");
    const auto marker = f->Marker(OwnSession());
    Check(WriteText(marker, f->Contents()), "h9 fixture");
    const auto eligible = DryRun(*f);
    HANDLE held = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "h9 test handle");
    Check(Execute(*f, eligible.anonymous_sha256).status == "marker_open_failed", "h9 shared open must fail");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    Check(fs::exists(marker) && !AuditHasRecord(*f, "execute_intent"), "h9 marker kept, no intent");
}

// h10 plus brief "content changes between reads": rewrite, hard link, and
// changes injected between the inspection and the exclusive reread.
void CaseH10() {
    {
        auto f = MakeFixture("h10-rewrite");
        const auto marker = f->Marker(OwnSession());
        Check(WriteText(marker, f->Contents()), "h10a fixture");
        const auto eligible = DryRun(*f);
        f->nonce = std::string(kNonceStem) + std::string(16, '9') + "0000000a";
        Check(WriteText(marker, f->Contents()), "h10a rewrite");
        Check(Execute(*f, eligible.anonymous_sha256).status == "hash_mismatch", "h10a new nonce must mismatch");
        Check(ReadText(marker) == f->Contents() && !AuditHasRecord(*f, "execute_intent"), "h10a marker kept");
    }
    {
        auto f = MakeFixture("h10-link");
        const auto marker = f->Marker(OwnSession());
        Check(WriteText(marker, f->Contents()), "h10b fixture");
        const auto eligible = DryRun(*f);
        const auto elsewhere = f->base / L"elsewhere";
        fs::create_directories(elsewhere);
        const auto link = elsewhere / L"link.bin";
        Check(CreateHardLinkW(link.c_str(), marker.c_str(), nullptr) != FALSE, "h10b hard link");
        const auto before = Capture(marker);
        Check(Execute(*f, eligible.anonymous_sha256).status == "marker_identity_changed", "h10b extra link must stop");
        Check(SameIdentity(before, Capture(marker)) && !AuditHasRecord(*f, "execute_intent"), "h10b marker kept");
        DeleteFileW(link.c_str());
    }
    {
        auto f = MakeFixture("h10-replace");
        const auto marker = f->Marker(OwnSession());
        const auto contents = f->Contents();
        Check(WriteText(marker, contents), "h10c fixture");
        const auto eligible = DryRun(*f);
        f->hook = [&](Point point) {
            if (point == Point::before_exclusive_open) {
                DeleteFileW(marker.c_str());
                WriteText(marker, contents);
            }
        };
        Check(Execute(*f, eligible.anonymous_sha256).status == "marker_identity_changed",
              "h10c file replaced before the exclusive open must stop");
        f->hook = nullptr;
        Check(ReadText(marker) == contents && !AuditHasRecord(*f, "execute_intent"), "h10c marker kept");
    }
    {
        auto f = MakeFixture("h10-reread");
        const auto marker = f->Marker(OwnSession());
        Check(WriteText(marker, f->Contents()), "h10d fixture");
        const auto eligible = DryRun(*f);
        f->hook = [&](Point point) {
            if (point == Point::before_exclusive_open) {
                f->nonce = std::string(kNonceStem) + std::string(16, '8') + "0000000b";
                WriteText(marker, f->Contents());
            }
        };
        Check(Execute(*f, eligible.anonymous_sha256).status == "hash_mismatch",
              "h10d content changed before the exclusive reread must mismatch");
        f->hook = nullptr;
        Check(ReadText(marker) == f->Contents() && !AuditHasRecord(*f, "execute_intent"), "h10d marker kept");
    }
    {
        auto f = MakeFixture("h10-snapshots");
        const auto marker = f->Marker(OwnSession());
        Check(WriteText(marker, f->Contents()), "h10e fixture");
        f->hook = [&](Point point) {
            if (point == Point::between_marker_snapshots) {
                f->nonce = std::string(kNonceStem) + std::string(16, '6') + "0000000c";
                WriteText(marker, f->Contents());
            }
        };
        Check(DryRun(*f).status == "marker_changed", "h10e change between the two P4 reads");
        f->hook = nullptr;
        Check(!fs::exists(f->audit_dir), "h10e no audit");
    }
}

// h11: audit unavailable (directory in its place, read-only file, garbage).
void CaseH11() {
    {
        auto f = MakeFixture("h11-dir");
        const auto marker = f->Marker(OwnSession());
        Check(WriteText(marker, f->Contents()), "h11a fixture");
        fs::create_directories(f->audit_file);
        Check(DryRun(*f).status == "audit_unavailable", "h11a directory at the audit path (dry run)");
        Check(Execute(*f, Sha(f->Contents())).status == "audit_unavailable", "h11a directory at the audit path (execute)");
        Check(fs::exists(marker), "h11a marker kept");
    }
    {
        auto f = MakeFixture("h11-readonly-dry");
        Check(WriteText(f->Marker(OwnSession()), f->Contents()), "h11b fixture");
        fs::create_directories(f->audit_dir);
        Check(WriteText(f->audit_file, ""), "h11b audit fixture");
        SetFileAttributesW(f->audit_file.c_str(), FILE_ATTRIBUTE_READONLY);
        Check(DryRun(*f).status == "audit_unavailable", "h11b read-only audit (dry run)");
        SetFileAttributesW(f->audit_file.c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    {
        auto f = MakeFixture("h11-readonly-exec");
        const auto marker = f->Marker(OwnSession());
        const auto contents = f->Contents();
        Check(WriteText(marker, contents), "h11c fixture");
        const auto eligible = DryRun(*f);
        SetFileAttributesW(f->audit_file.c_str(), FILE_ATTRIBUTE_READONLY);
        const auto before = Capture(marker);
        Check(Execute(*f, eligible.anonymous_sha256).status == "audit_unavailable",
              "h11c read-only audit stops before deletion");
        Check(SameIdentity(before, Capture(marker)) && AuditLines(*f).size() == 1, "h11c marker kept, no intent");
        SetFileAttributesW(f->audit_file.c_str(), FILE_ATTRIBUTE_NORMAL);
        // Restart from a fresh dry run: the identical evidence copy left by the
        // stopped attempt is accepted.
        const auto again = DryRun(*f);
        Check(again.status == "recovery_eligible", "h11c fresh dry run");
        Check(Execute(*f, again.anonymous_sha256).status == "executed_marker_absent",
              "h11c restart after the audit is repaired");
    }
    {
        auto f = MakeFixture("h11-garbage");
        const auto marker = f->Marker(OwnSession());
        Check(WriteText(marker, f->Contents()), "h11d fixture");
        const auto eligible = DryRun(*f);
        std::ofstream(f->audit_file, std::ios::binary | std::ios::app) << "{\"schema\":\"something-else\"}\n";
        Check(Execute(*f, eligible.anonymous_sha256).status == "audit_unavailable", "h11d unparseable audit line");
        Check(fs::exists(marker), "h11d marker kept");
    }
}

// h12: dry run -> execute end to end; audit order; evidence; once only;
// the B guard quarantines before and allows the lease after.
void CaseH12() {
    auto f = MakeFixture("h12");
    const auto contents = f->Contents();
    const auto marker = f->Marker(OwnSession());
    Check(WriteText(marker, contents), "h12 fixture");
    Check(Quarantined(*f), "h12 lease is quarantined before recovery");
    const auto eligible = DryRun(*f);
    Check(eligible.status == "recovery_eligible", "h12 eligible");
    bool exclusive = false;
    f->hook = [&](Point point) {
        if (point != Point::before_disposition) return;
        HANDLE other = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        exclusive = other == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
        if (other != INVALID_HANDLE_VALUE) CloseHandle(other);
    };
    const auto executed = Execute(*f, eligible.anonymous_sha256);
    f->hook = nullptr;
    Check(executed.status == "executed_marker_absent", "h12 execute " + executed.status);
    Check(exclusive, "h12 marker is unopenable by others while the delete handle is held");
    Check(!AnyMarker(*f) && RootEntries(*f).empty(), "h12 marker root has no marker");
    const auto lines = AuditLines(*f);
    Check(lines.size() == 3 && Field(lines[0], "record") == "dry_run" && Field(lines[1], "record") == "execute_intent" &&
              Field(lines[2], "record") == "execute_result" && Field(lines[2], "status") == "executed_marker_absent" &&
              Field(lines[0], "anonymousSha256") == eligible.anonymous_sha256 &&
              Field(lines[1], "anonymousSha256") == eligible.anonymous_sha256 &&
              Field(lines[2], "anonymousSha256") == eligible.anonymous_sha256 &&
              Field(lines[1], "operatorAttested") == "1" && Field(lines[2], "operatorAttested") == "1",
          "h12 audit dry_run -> execute_intent -> execute_result with one hash");
    const auto evidence = f->audit_dir / (L"evidence-" + Wide(eligible.anonymous_sha256.substr(0, 16)) + L".bak");
    Check(ReadText(evidence) == contents, "h12 evidence copy matches the original outside the marker root");
    Check(Execute(*f, eligible.anonymous_sha256).status == "already_executed", "h12 second execute");
    Check(LeaseSucceeds(*f), "h12 lease is available after recovery");
    CollectAudit(*f);
}

// Addendum: a foreign-session marker needs --confirm-foreign-session, and the
// flag on a same-session marker is a mix-up.
void CaseForeignSession() {
    {
        auto f = MakeFixture("foreign");
        const auto marker = f->Marker(kForeignSession);
        Check(WriteText(marker, f->Contents()), "foreign fixture");
        const auto eligible = DryRun(*f);
        Check(eligible.status == "recovery_eligible" && !eligible.session_match, "foreign eligible");
        const auto before = Capture(marker);
        Check(Execute(*f, eligible.anonymous_sha256, true, false).status == "session_confirmation_mismatch",
              "foreign marker without confirmation");
        Check(SameIdentity(before, Capture(marker)) && !AuditHasRecord(*f, "execute_intent"), "foreign marker kept");
        Check(Execute(*f, eligible.anonymous_sha256, true, true).status == "executed_marker_absent",
              "foreign marker with confirmation");
        Check(!AnyMarker(*f), "foreign marker removed");
        CollectAudit(*f);
    }
    {
        auto f = MakeFixture("same-confirmed");
        const auto marker = f->Marker(OwnSession());
        Check(WriteText(marker, f->Contents()), "same-confirmed fixture");
        const auto eligible = DryRun(*f);
        Check(Execute(*f, eligible.anonymous_sha256, true, true).status == "session_confirmation_mismatch",
              "same-session marker with the foreign flag");
        Check(fs::exists(marker), "same-confirmed marker kept");
    }
}

// Step 9 failure: a read-only marker cannot take the delete disposition.
void CaseDeleteFailed() {
    auto f = MakeFixture("delete-failed");
    const auto marker = f->Marker(OwnSession());
    Check(WriteText(marker, f->Contents()), "delete-failed fixture");
    SetFileAttributesW(marker.c_str(), FILE_ATTRIBUTE_READONLY);
    const auto eligible = DryRun(*f);
    Check(eligible.status == "recovery_eligible", "delete-failed eligible");
    Check(Execute(*f, eligible.anonymous_sha256).status == "delete_failed", "delete-failed status");
    Check(fs::exists(marker), "delete-failed marker kept");
    const auto lines = AuditLines(*f);
    Check(lines.size() == 3 && Field(lines[1], "record") == "execute_intent" &&
              Field(lines[2], "status") == "delete_failed",
          "delete-failed audit records intent and result");
    Check(DryRun(*f).status == "recovery_eligible", "delete-failed fresh dry run");
    Check(Execute(*f, eligible.anonymous_sha256).status == "already_executed", "delete-failed never retried");
    SetFileAttributesW(marker.c_str(), FILE_ATTRIBUTE_NORMAL);
}

// Step 11: a marker that appears during the deletion is reported, not hidden.
void CasePostDeleteNotEmpty() {
    auto f = MakeFixture("post-delete");
    Check(WriteText(f->Marker(OwnSession()), f->Contents()), "post-delete fixture");
    const auto eligible = DryRun(*f);
    f->hook = [&](Point point) {
        if (point == Point::before_disposition) WriteText(f->Marker(77), f->Contents());
    };
    Check(Execute(*f, eligible.anonymous_sha256).status == "post_delete_root_not_empty", "post-delete status");
    f->hook = nullptr;
    const auto lines = AuditLines(*f);
    Check(!lines.empty() && Field(lines.back(), "status") == "post_delete_root_not_empty" && fs::exists(f->Marker(77)),
          "post-delete audit and the new marker kept");
}

// Step 7: a conflicting evidence copy stops before the intent record.
void CaseEvidenceConflict() {
    auto f = MakeFixture("evidence");
    const auto marker = f->Marker(OwnSession());
    Check(WriteText(marker, f->Contents()), "evidence fixture");
    const auto eligible = DryRun(*f);
    const auto evidence = f->audit_dir / (L"evidence-" + Wide(eligible.anonymous_sha256.substr(0, 16)) + L".bak");
    Check(WriteText(evidence, "different\n"), "evidence conflict fixture");
    Check(Execute(*f, eligible.anonymous_sha256).status == "evidence_copy_failed", "evidence conflict status");
    Check(fs::exists(marker) && !AuditHasRecord(*f, "execute_intent"), "evidence conflict keeps marker");
}

// Option validation never falls back to the production root or mutex.
void CaseOptions() {
    auto f = MakeFixture("options");
    Check(WriteText(f->Marker(OwnSession()), f->Contents()), "options fixture");
    auto options = f->Options();
    options.test_lease_name = "A0.Other.Lease";
    Check(DryRunDualDelegationMarkerRecovery(options).status == "marker_root_untrusted", "non-test lease name");
    options = f->Options();
    options.test_marker_root = L"relative\\DualDelegation";
    Check(DryRunDualDelegationMarkerRecovery(options).status == "marker_root_untrusted", "relative test root");
    options = f->Options();
    options.test_marker_root = f->root.wstring() + L"\\";
    Check(DryRunDualDelegationMarkerRecovery(options).status == "marker_root_untrusted", "trailing separator root");
    options = f->Options();
    options.test_marker_root = f->base / L"absent";
    Check(DryRunDualDelegationMarkerRecovery(options).status == "marker_root_untrusted" &&
              !fs::exists(f->base / L"absent"),
          "missing root is not created");
    Check(!fs::exists(f->audit_dir), "option failures write no audit");
}

// h14: CLI argument contract. Every run carries the test root, except the
// bare no-argument run, which exits before any library call.
void CaseCliUsage() {
    auto f = MakeFixture("cli-usage");
    const std::string upper(64, 'A');
    const std::string short_hash(63, 'a');
    const auto usage = [&](const std::vector<std::wstring> &arguments, const char *label) {
        const auto run = RunCli(arguments);
        Check(run.started && run.finished && run.exit_code == 2 && run.out.empty() &&
                  run.err.find("usage:") != std::string::npos,
              std::string("h14 usage: ") + label + " exit=" + std::to_string(run.exit_code));
    };
    usage({}, "no arguments");
    usage(TestArgs(*f, {L"--dry-run", L"--bogus"}), "unknown argument");
    usage(TestArgs(*f, {L"--execute"}), "--execute only");
    usage(TestArgs(*f, {L"--dry-run", L"--expect-sha256", Wide(upper)}), "upper-case hash");
    usage(TestArgs(*f, {L"--dry-run", L"--expect-sha256", Wide(short_hash)}), "63-character hash");
    usage(TestArgs(*f, {L"--dry-run", L"--confirm-cameras-disconnected"}), "dry run with execute flag");
    usage({L"--dry-run", L"--test-root", f->root.wstring()}, "test root without test lease");
    Check(RootEntries(*f).empty() && !fs::exists(f->audit_dir), "h14 usage errors touch nothing");
    const auto missing = RunCli(TestArgs(*f, {L"--dry-run"}));
    Check(missing.exit_code == 2 && missing.out.find("\"status\":\"marker_missing\"") != std::string::npos,
          "h14 empty test root is marker_missing: " + missing.out);
    leak_corpus.push_back(missing.out);
    Check(WriteText(f->root / L"armed-session-01.marker", f->Contents()), "h14 noncanonical fixture");
    const auto noncanonical = RunCli(TestArgs(*f, {L"--dry-run"}));
    Check(noncanonical.exit_code == 2 &&
              noncanonical.out.find("\"status\":\"marker_name_noncanonical\"") != std::string::npos,
          "h14 CLI noncanonical: " + noncanonical.out);
}

// CLI end to end with the real PnP and process probes (read-only queries; no
// camera is opened). If this PC has a Nikon device or another A0 process the
// dry run stops there, which is reported as a skip rather than a failure.
std::string CaseCliEndToEnd() {
    auto f = MakeFixture("cli-e2e");
    const auto contents = f->Contents();
    const auto marker = f->Marker(OwnSession());
    Check(WriteText(marker, contents), "e2e fixture");
    // A dry run recorded by another tool (this test exe) does not authorize the CLI.
    const auto library_dry_run = DryRun(*f);
    Check(library_dry_run.status == "recovery_eligible", "e2e library dry run");
    const auto foreign_tool = RunCli(TestArgs(*f, {L"--execute", L"--expect-sha256", Wide(Sha(contents)),
                                                   L"--confirm-cameras-disconnected"}));
    Check(foreign_tool.exit_code == 2 && foreign_tool.out.find("\"status\":\"dry_run_record_missing\"") != std::string::npos,
          "e2e toolSha256 binding: " + foreign_tool.out);
    const auto unattested = RunCli(TestArgs(*f, {L"--execute", L"--expect-sha256", Wide(Sha(contents))}));
    Check(unattested.exit_code == 2 &&
              unattested.out.find("\"status\":\"operator_attestation_missing\"") != std::string::npos,
          "e2e missing attestation: " + unattested.out);
    const auto dry = RunCli(TestArgs(*f, {L"--dry-run"}));
    leak_corpus.push_back(dry.out);
    const std::string status = Field(dry.out, "status");
    if (status == "camera_present" || status == "a0_process_present") {
        Check(dry.exit_code == 2 && fs::exists(marker), "e2e environment stop keeps marker");
        return "skipped:" + status;
    }
    Check(dry.exit_code == 0 && status == "recovery_eligible" && Field(dry.out, "anonymousSha256") == Sha(contents) &&
              Field(dry.out, "size") == std::to_string(contents.size()) && Field(dry.out, "sessionMatch") == "1",
          "e2e CLI dry run: " + dry.out);
    const auto executed = RunCli(TestArgs(*f, {L"--execute", L"--expect-sha256", Wide(Sha(contents)),
                                               L"--confirm-cameras-disconnected"}));
    leak_corpus.push_back(executed.out);
    Check(executed.exit_code == 0 && Field(executed.out, "status") == "executed_marker_absent" && !AnyMarker(*f),
          "e2e CLI execute: " + executed.out);
    const auto lines = AuditLines(*f);
    Check(lines.size() == 4 && Field(lines[3], "status") == "executed_marker_absent" &&
              Field(lines[1], "toolSha256") == Field(lines[3], "toolSha256") &&
              Field(lines[0], "toolSha256") != Field(lines[1], "toolSha256"),
          "e2e audit lines carry the CLI tool hash");
    CollectAudit(*f);
    return "executed";
}

// h13: nothing synthetic or identifying leaks to results, CLI output or audit.
void CaseNoLeak() {
    const std::vector<std::string> forbidden = {std::string(kNonceStem), std::string(kEpochStem), "4000000000",
                                                "4000000004", "4000000008", "3999999999", "A0MarkerRecoveryTest",
                                                "DualDelegation", "armed-session", "nonce", "epoch", "Pid", "\\"};
    for (const auto &text : leak_corpus)
        for (const auto &token : forbidden)
            Check(text.find(token) == std::string::npos, "h13 leaked token " + token + " in " + text.substr(0, 120));
    Check(leak_corpus.size() > 20, "h13 corpus collected");
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--suspended-child") return 0;
    if (argc != 1) {
        std::cerr << "usage: a0_marker_recovery_tests\n";
        return 2;
    }
    if (OwnSession() == kForeignSession) return 2;
    std::error_code error;
    fs::remove_all(TestBase(), error);
    CaseH1();
    CaseH2();
    CaseH3();
    CaseH4();
    CaseH5();
    CaseRealA0Process();
    CaseH6();
    CaseH7();
    CaseH8();
    CaseH9();
    CaseH10();
    CaseH11();
    CaseH12();
    CaseForeignSession();
    CaseDeleteFailed();
    CasePostDeleteNotEmpty();
    CaseEvidenceConflict();
    CaseOptions();
    CaseCliUsage();
    const auto end_to_end = CaseCliEndToEnd();
    CaseNoLeak();
    fs::remove_all(TestBase(), error);
    std::cout << "{\"mode\":\"marker-recovery\",\"checks\":" << checks << ",\"failures\":" << failures
              << ",\"cliEndToEnd\":\"" << end_to_end << "\"}\n";
    return failures ? 1 : 0;
}
