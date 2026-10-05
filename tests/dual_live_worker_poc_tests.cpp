#include "dual_live_test_ipc.hpp"
namespace {

int Worker(const std::string& pipe_name, const std::string& alias, const std::string& generation,
           const std::string& token, unsigned source, HANDLE parent) {
    if ((alias != "CAM-A" && alias != "CAM-B") || generation.empty() || token.empty() ||
        !parent || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT) return 1;
    const auto selected = SelectAssignedSourceBeforeOpen({71U, 83U}, source);
    if (!selected) return 2;
    const std::vector<unsigned> fake_open_trace{*selected};
    // Server exists before launch: no connection retry is needed.
    Handle pipe(CreateFileW(PipePath(pipe_name).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!pipe.valid()) return 3;
    bool first_read = true;
    for (;;) {
        if (WaitForSingleObject(parent, 0) != WAIT_TIMEOUT) return 7;
        std::string request;
        // The controller may still be spawning/connecting the sibling worker
        // before sending anything, so only the first wait gets the spawn
        // budget; every later request/response stays on the steady-state one.
        const auto read_timeout = first_read ? kSpawnTimeout : kTimeout;
        first_read = false;
        if (!ReadMessage(pipe.value, request, parent, read_timeout)) {
            // Closing the controller's pipe can precede its process handle
            // becoming signaled. Resolve that exit race without reconnecting.
            return WaitForSingleObject(parent, kTimeout) == WAIT_OBJECT_0 ? 7 : 4;
        }
        const auto fields = Split(request);
        if (fields.size() == 3 && fields[0] == generation && fields[1] == token && fields[2] == "STOP") {
            if (!WriteMessage(pipe.value, "STOPPED", parent)) return 5;
            return ReadMessage(pipe.value, request, parent) && request == "ACK" ? 0 : 6;
        }
        std::uint64_t sequence{};
        if (fields.size() != 3 || fields[0] != generation || fields[1] != token ||
            !Number(fields[2], sequence)) return 6;
        const auto response = alias + "|" + generation + "|" + token + "|" + fields[2] +
            "|" + std::to_string(fake_open_trace.at(0)) + "|simulated-frame-" + fields[2];
        if (!WriteMessage(pipe.value, response, parent)) return 5;
    }
}

struct Child {
    std::string name = Unique();
    std::string alias, token;
    unsigned source;
    Handle pipe;
    Handle process;
    Child(std::string a, std::string t, unsigned s, HANDLE parent)
        : alias(std::move(a)), token(std::move(t)), source(s), pipe(CreateServer(name)),
          process(pipe.valid() ? Start(L"--worker " + Wide(name) + L" " + Wide(alias) +
              L" g " + Wide(token) + L" " + std::to_wstring(source) + L" " +
              std::to_wstring(reinterpret_cast<std::uintptr_t>(parent)), parent) : nullptr) {
        if (!pipe.valid() || !process.valid() || !Connect(pipe.value, kSpawnTimeout)) {
            Reap(process.value);
            throw std::runtime_error("worker connection");
        }
    }
    ~Child() { Reap(process.value); }
    bool Alive() const { return WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT; }
    std::string Request(const std::string& sequence) {
        std::string response;
        if (!WriteMessage(pipe.value, "g|" + token + "|" + sequence) || !ReadMessage(pipe.value, response)) return {};
        return response;
    }
    bool Stop() {
        if (!Alive()) return true;
        const bool ack = Request("STOP") == "STOPPED" && WriteMessage(pipe.value, "ACK");
        return ack && WaitForSingleObject(process.value, kTimeout) == WAIT_OBJECT_0 && ExitCode(process.value) == 0;
    }
};
bool Accept(const std::string& wire, const Child& expected, DualLiveWorkerPocCoordinator& coordinator) {
    const auto fields = Split(wire);
    std::uint64_t sequence{};
    if (wire.size() > kMaxWire || fields.size() != 6 || fields[0] != expected.alias ||
        fields[1] != "g" || fields[2] != expected.token || !Number(fields[3], sequence) ||
        fields[4] != std::to_string(expected.source) || fields[5] != "simulated-frame-" + fields[3]) return false;
    return coordinator.AcceptFrame(fields[0], fields[1], fields[2], sequence, {fields[5].begin(), fields[5].end()});
}
struct Controller {
    Child& a; Child& b;
    DualLiveWorkerPocCoordinator state{"g", "a-token", "b-token"};
    bool ObserveChildren() {
        if (!a.Alive() || !b.Alive()) {
            state.ReportWorkerExited(!a.Alive() ? "CAM-A" : "CAM-B");
            if (a.Alive()) Check(a.Stop(), "stop surviving A after B fault");
            if (b.Alive()) Check(b.Stop(), "stop surviving B after A fault");
        }
        return state.CanBeginNewOperation();
    }
    bool Frame(Child& child, const std::string& sequence) {
        if (!ObserveChildren()) return false;
        if (!Accept(child.Request(sequence), child, state)) {
            state.ReportWorkerExited(child.alias);
            if (a.Alive()) a.Stop();
            if (b.Alive()) b.Stop();
            return false;
        }
        return ObserveChildren();
    }
};
int Helper(const std::string& report_name, HANDLE outer_parent) {
    Handle report(CreateFileW(PipePath(report_name).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!report.valid()) return 10;
    Handle self(OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId()));
    Child a("CAM-A", "a-token", 71, self.value), b("CAM-B", "b-token", 83, self.value);
    const auto pids = std::to_string(GetProcessId(a.process.value)) + "|" + std::to_string(GetProcessId(b.process.value));
    if (!WriteMessage(report.value, pids, outer_parent)) return 11;
    std::string unused;
    // Idles here until the outer test terminates this helper process (its
    // parent-death injection). Under load, giving this the steady-state
    // timeout risks a natural timeout racing ahead of that termination, so
    // this uses the spawn budget instead.
    ReadMessage(report.value, unused, outer_parent, kSpawnTimeout);
    return 12;
}
void TestNormalAndWorkerFault(HANDLE parent) {
    Child a("CAM-A", "a-token", 71, parent), b("CAM-B", "b-token", 83, parent);
    Controller controller{a, b};
    Require(a.Alive() && b.Alive(), "two simultaneous persistent workers");
    Check(controller.Frame(a, "1") && controller.Frame(b, "1") &&
          controller.Frame(a, "2") && controller.Frame(b, "2"), "both workers deliver repeated wire frames");
    Check(controller.state.LatestA() && controller.state.LatestA()->sequence == 2 &&
          controller.state.LatestB() && controller.state.LatestB()->sequence == 2, "latest-only slots");
    Check(!Accept(a.Request("1"), a, controller.state), "stale sequence rejected through wire decoder");
    Check(!Accept(b.Request("3"), a, controller.state), "wrong worker credential rejected");
    Check(!Accept("CAM-A|g|a-token|3x|71|simulated-frame-3x", a, controller.state), "strict response sequence");
    Check(!Accept("CAM-A|old|a-token|3|71|simulated-frame-3", a, controller.state), "old generation rejected");
    Require(TerminateProcess(b.process.value, 9) != FALSE, "inject fake worker fault");
    Require(WaitForSingleObject(b.process.value, kTimeout) == WAIT_OBJECT_0, "observe B failure");
    Check(ExitCode(b.process.value) == 9, "record injected cause");
    Check(!controller.ObserveChildren() && !a.Alive() && ExitCode(a.process.value) == 0,
          "real worker exit makes controller terminal and stops sibling");
    Check(!controller.Frame(a, "4") && !controller.state.CanBeginCaptureOrWpd(), "no next operation or hardware handoff");
}
void TestParentDeath(HANDLE parent) {
    const auto name = Unique();
    Handle report(CreateServer(name));
    Handle helper(Start(L"--helper " + Wide(name) + L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(parent)), parent));
    Require(Connect(report.value, kSpawnTimeout), "helper connection");
    std::string pids;
    // This wait spans two nested spawns: the helper process itself, then its
    // own two CAM-A/CAM-B children, each connecting before the helper reports
    // back. Budget for both.
    Require(ReadMessage(report.value, pids, nullptr, 2 * kSpawnTimeout), "helper reports actual worker identities");
    const auto ids = Split(pids);
    std::uint64_t a_pid{}, b_pid{};
    Require(ids.size() == 2 && Number(ids[0], a_pid) && Number(ids[1], b_pid), "worker pid envelope");
    Handle a(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(a_pid)));
    Handle b(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(b_pid)));
    Require(a.valid() && b.valid(), "monitor actual workers");
    Require(TerminateProcess(helper.value, 9) != FALSE, "inject controller death");
    Require(WaitForSingleObject(helper.value, kTimeout) == WAIT_OBJECT_0, "controller ended");
    Check(ExitCode(helper.value) == 9, "record controller termination cause");
    const auto a_wait = WaitForSingleObject(a.value, kTimeout * 2);
    const auto b_wait = WaitForSingleObject(b.value, kTimeout * 2);
    const auto a_exit = ExitCode(a.value), b_exit = ExitCode(b.value);
    if (a_wait != WAIT_OBJECT_0 || b_wait != WAIT_OBJECT_0 || a_exit != 7 || b_exit != 7)
        std::cerr << "parent-death waits=" << a_wait << ',' << b_wait << " exits=" << a_exit << ',' << b_exit << '\n';
    Check(a_wait == WAIT_OBJECT_0 && b_wait == WAIT_OBJECT_0 && a_exit == 7 && b_exit == 7,
          "same IPC workers stop on parent death, no orphan");
}
void TestCommunicationFault(HANDLE parent) {
    Child a("CAM-A", "a-token", 71, parent), b("CAM-B", "b-token", 83, parent);
    Controller controller{a, b};
    Require(controller.Frame(a, "1") && controller.Frame(b, "1"), "communication-fault baseline");
    // The worker rejects this request and closes the pipe without a response.
    // The controller must react to actual ReadMessage failure, not a manual event.
    Check(!controller.Frame(a, "invalid"), "controller rejects broken response path");
    Check(!controller.state.CanBeginNewOperation() && !b.Alive() && ExitCode(b.process.value) == 0,
          "IPC failure makes controller terminal and stops sibling");
    Check(WaitForSingleObject(a.process.value, kTimeout) == WAIT_OBJECT_0 && ExitCode(a.process.value) == 6,
          "rejected worker is reaped with cause");
}
void TestWireFailures(HANDLE parent) {
    {
        Child a("CAM-A", "a-token", 71, parent);
        auto header = Header(static_cast<std::uint32_t>(kMaxWire + 1));
        Check(Io(a.pipe.value, header.data(), 4, true), "inject over-limit header");
        Check(WaitForSingleObject(a.process.value, kTimeout * 2) == WAIT_OBJECT_0 && ExitCode(a.process.value) == 4,
              "oversized wire rejected before allocation");
    }
    {
        Child a("CAM-A", "a-token", 71, parent);
        Check(WriteMessage(a.pipe.value, "g|a-token|1x"), "inject malformed sequence");
        Check(WaitForSingleObject(a.process.value, kTimeout) == WAIT_OBJECT_0 && ExitCode(a.process.value) == 6,
              "malformed sequence rejected in worker");
    }
    {
        Child a("CAM-A", "a-token", 71, parent);
        Check(WriteMessage(a.pipe.value, "g|wrong-token|1"), "inject wrong token");
        Check(WaitForSingleObject(a.process.value, kTimeout) == WAIT_OBJECT_0 && ExitCode(a.process.value) == 6,
              "wrong request credential rejected");
    }
    {
        // No request is ever sent, so this exercises the first read. It uses
        // kSpawnTimeout, and on that read's own timeout the worker waits on
        // `parent` for up to another kTimeout before exiting (see Worker()
        // above), so the worst case before exit is kSpawnTimeout + kTimeout.
        // Doubling kSpawnTimeout clears that with real margin.
        Child a("CAM-A", "a-token", 71, parent);
        Check(WaitForSingleObject(a.process.value, 2 * kSpawnTimeout) == WAIT_OBJECT_0 && ExitCode(a.process.value) == 4,
              "idle IPC deadline on first read closes actual worker");
    }
    {
        // After one served request, every later read must use kTimeout. With
        // no further request the worker exits 4 within one kTimeout read plus
        // its kTimeout parent check. The bound below is shorter than
        // kSpawnTimeout, so a later read that wrongly kept the first-read
        // budget fails this check instead of passing slowly.
        static_assert(kTimeout * 3 < kSpawnTimeout, "idle bound must separate the two read budgets");
        Child a("CAM-A", "a-token", 71, parent);
        Check(a.Request("1") == "CAM-A|g|a-token|1|71|simulated-frame-1", "idle baseline request served");
        Check(WaitForSingleObject(a.process.value, kTimeout * 3) == WAIT_OBJECT_0 && ExitCode(a.process.value) == 4,
              "idle IPC deadline after a served request closes actual worker");
    }
    Check(Worker("unused", "CAM-A", "g", "token", 71, nullptr) == 1, "unowned startup rejected before pipe open");
    Handle unowned(Start(L"--worker unused CAM-A g token 71 0", parent));
    Check(WaitForSingleObject(unowned.value, kTimeout) == WAIT_OBJECT_0 && ExitCode(unowned.value) == 1,
          "command-line-only worker rejects in actual child process");
    Check(Worker("unused", "CAM-X", "g", "token", 71, parent) == 1, "invalid alias rejected before pipe open");
    Check(Worker("unused", "CAM-A", "g", "token", 99, parent) == 2, "missing assignment refuses worker fake Open");
    Check(!SelectAssignedSourceBeforeOpen({71, 83}, 99) && !SelectAssignedSourceBeforeOpen({83, 83}, 83),
          "missing/ambiguous source rejected before fake Open");
    DualLiveWorkerPocCoordinator invalid("g", "same", "same");
    Check(!invalid.CanBeginNewOperation(), "shared worker credentials fail closed");
    DualLiveWorkerPocCoordinator mailbox("g", "a", "b");
    Check(!mailbox.AcceptFrame("CAM-A", "g", "a", 0, {1}) &&
          !mailbox.AcceptFrame("CAM-A", "g", "a", 1,
              std::vector<std::uint8_t>(kDualLiveWorkerPocMaximumFrameBytes + 1, 1)),
          "mailbox zero sequence and oversized payload rejected");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 8 && std::string_view(argv[1]) == "--worker") {
            std::uint64_t source{}, parent{};
            if (!Number(argv[6], source) || source > UINT32_MAX || !Number(argv[7], parent)) return 1;
            return Worker(argv[2], argv[3], argv[4], argv[5], static_cast<unsigned>(source), reinterpret_cast<HANDLE>(parent));
        }
        if (argc == 4 && std::string_view(argv[1]) == "--helper") {
            std::uint64_t parent{};
            if (!Number(argv[3], parent)) return 1;
            return Helper(argv[2], reinterpret_cast<HANDLE>(parent));
        }
        if (argc != 1) return 1;
        Handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Require(job.valid() && SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                &limits, sizeof(limits)), "test-owned cleanup job");
        cleanup_job = job.value;
        Handle parent(OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId()));
        Require(parent.valid(), "inheritable controller process handle");
        TestNormalAndWorkerFault(parent.value);
        TestParentDeath(parent.value);
        TestCommunicationFault(parent.value);
        TestWireFailures(parent.value);
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        const auto reaped_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeout);
        do {
            Require(QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation,
                &accounting, sizeof(accounting), nullptr) != FALSE, "job process accounting");
            if (accounting.ActiveProcesses == 0) break;
            Sleep(10); // Bounded OS process-accounting convergence, not a test retry.
        } while (std::chrono::steady_clock::now() < reaped_deadline);
        Check(accounting.ActiveProcesses == 0, "no surviving test child processes");
        std::cout << (failures ? "FAIL" : "PASS") << " dual live worker POC (software-only)\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
