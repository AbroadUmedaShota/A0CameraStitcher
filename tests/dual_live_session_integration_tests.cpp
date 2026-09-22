// Software-only. Only a test-named OS lease uses production code.
#include "dual_live_test_ipc.hpp"
#include "a0/phase0/dual_live_session_contract.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"
using namespace a0::phase0::experimental;
namespace {
constexpr std::string_view kEpoch = "test-epoch", kController = "test-controller";
std::string Instance(const std::string& a) { return "worker-" + a; }
std::string Receipt(const std::string& a) { return "receipt-" + a; }
std::string Capability(const std::string& a) { return "cap-" + a; }
std::string GrantWire(const PreviewGrant& g) {
    return g.controller + "|" + g.epoch + "|" + g.alias + "|" + g.instance + "|" + g.receipt + "|" +
        g.capability + "|" + std::to_string(g.issued_at_ms) + "|" + std::to_string(g.deadline_ms);
}
std::optional<PreviewGrant> ParseGrant(const std::string& wire) {
    const auto f = Split(wire); std::uint64_t issued{}, deadline{};
    if (f.size() != 8 || !Number(f[6], issued) || !Number(f[7], deadline)) return {};
    return PreviewGrant{f[0], f[1], f[2], f[3], f[4], f[5], issued, deadline};
}
struct FakeTransport {
    unsigned opens{}, frames{};
    bool module{}, source{}, preview{};
    bool Open(std::uint32_t id) {
        if (id != 71 || opens) return false;
        ++opens; module = source = preview = true; return true;
    }
    bool Frame() { if (!preview || !source || !module) return false; ++frames; return true; }
    void Close() { preview = false; source = false; module = false; }
};
int GrantWorker(const std::string& name, const std::string& alias, HANDLE parent) {
    if ((alias != "CAM-A" && alias != "CAM-B") || !parent ||
        WaitForSingleObject(parent, 0) != WAIT_TIMEOUT) return 1;
    Handle pipe(CreateFileW(PipePath(name).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!pipe.valid()) return 2;
    WorkerLocalSelection selection(Instance(alias), std::string(kEpoch), {71, 83});
    if (!selection.Select(Instance(alias), kEpoch, 71)) return 3;
    // Identical local numbers are not global identity. Only fake receipts cross IPC.
    if (!WriteMessage(pipe.value, alias + "|" + Instance(alias) + "|" + std::string(kEpoch) +
        "|" + Receipt(alias) + "|body-" + alias, parent)) return 4;
    FakeTransport transport;
    auto finish = [&](const std::string& message) {
        transport.Close(); selection.Invalidate();
        if (!WriteMessage(pipe.value, message, parent)) return 5;
        std::string ack;
        return ReadMessage(pipe.value, ack, parent) && ack == "ACK" ? 0 : 6;
    };
    auto deny = [&] { return finish("DENIED|" + std::to_string(transport.opens) + "|" + std::to_string(transport.frames)); };
    std::string wire;
    if (!ReadMessage(pipe.value, wire, parent)) return 7;
    const auto grant = ParseGrant(wire); const auto now = GetTickCount64();
    if (!grant || grant->controller != kController || grant->epoch != kEpoch || grant->alias != alias ||
        grant->instance != Instance(alias) || grant->receipt != Receipt(alias) || grant->capability != Capability(alias) ||
        grant->issued_at_ms > now || grant->deadline_ms <= now || grant->deadline_ms <= grant->issued_at_ms) return deny();
    if (WaitForSingleObject(parent, 0) != WAIT_TIMEOUT ||
        !selection.OpenSelected(Instance(alias), kEpoch, [&](auto id) { return transport.Open(id); })) return deny();
    if (!WriteMessage(pipe.value, "READY", parent)) return 8;
    std::uint64_t sequence{}, last_time = now;
    for (;;) {
        if (!ReadMessage(pipe.value, wire, parent)) { transport.Close(); selection.Invalidate(); return 9; }
        const auto f = Split(wire);
        // Close is allowed after expiry; it cannot generate a frame.
        if (f.size() == 3 && f[0] == "STOP" && f[1] == grant->capability && f[2] == grant->epoch) {
            transport.Close();
            return finish("CLOSE|" + grant->capability + "|" + grant->epoch + "|" +
                (transport.preview ? "0" : "1") + "|" + (transport.source ? "0" : "1") + "|" + (transport.module ? "0" : "1"));
        }
        std::uint64_t incoming{}; const auto tick = GetTickCount64();
        if (f.size() != 4 || f[0] != "PREVIEW" || f[1] != grant->capability || f[2] != grant->epoch ||
            !Number(f[3], incoming) || incoming <= sequence || tick < last_time || tick < grant->issued_at_ms ||
            tick >= grant->deadline_ms || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT) return deny();
        sequence = incoming; last_time = tick;
        if (!transport.Frame()) return deny();
        if (!WriteMessage(pipe.value, "FRAME|" + grant->capability + "|" + grant->epoch + "|" +
            std::to_string(sequence) + "|simulated-" + alias, parent)) return 10;
    }
}
struct Child {
    std::string alias, name;
    Handle pipe, process;
    Child(std::string a, HANDLE parent) : alias(std::move(a)), name(Unique()), pipe(CreateServer(name)),
        process(Start(L"--grant-worker " + Wide(name) + L" " + Wide(alias) + L" " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(parent)), parent)) {
        Require(pipe.valid() && Connect(pipe.value), "worker pipe connect");
    }
    ~Child() { Reap(process.value); }
    std::string Read() { std::string wire; Require(ReadMessage(pipe.value, wire), "worker reply"); return wire; }
    void Send(const std::string& wire) { Require(WriteMessage(pipe.value, wire), "worker request"); }
    void AckExit() {
        Send("ACK");
        Require(WaitForSingleObject(process.value, kTimeout) == WAIT_OBJECT_0 && ExitCode(process.value) == 0, "worker acknowledged exit");
    }
    SimulatedBindingReceipt Bind() {
        const auto f = Split(Read());
        Require(f.size() == 5 && f[0] == alias && f[1] == Instance(alias) && f[2] == kEpoch &&
            f[3] == Receipt(alias) && f[4] == "body-" + alias, "binding receipt identity");
        return {f[0], f[1], f[2], f[3], f[4]};
    }
    bool Alive() const { return WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT; }
    bool Close(const PreviewGrant& g) {
        Send("STOP|" + g.capability + "|" + g.epoch);
        const bool closed = Read() == "CLOSE|" + g.capability + "|" + g.epoch + "|1|1|1";
        AckExit(); return closed;
    }
};
int LeaseProbe(const std::string& name) {
    if (!name.starts_with("A0.Poc.TestLease.")) return 22;
    try { HardwareProcessLease lease(name); return lease.RecoveredAbandonedOwner() ? 23 : 0; }
    catch (const TransportError& e) { return e.Category() == "camera_control_busy" ? 20 : 21; }
}
void Probe(const std::string& name, HANDLE parent, DWORD expected) {
    Handle process(Start(L"--lease-probe " + Wide(name), parent)); Reap(process.value);
    Check(ExitCode(process.value) == expected, "separate process observes exact lease result");
}
struct Pair {
    // Reverse destruction order keeps the lease through child cleanup.
    HardwareProcessLease lease;
    DualLiveSessionContract session{std::string(kController), std::string(kEpoch)};
    Child a, b;
    PreviewGrant ga, gb;
    Pair(const std::string& name, HANDLE parent) : lease(name), a("CAM-A", parent), b("CAM-B", parent) {
        Require(!lease.RecoveredAbandonedOwner(), "fresh lease");
        Require(session.ObserveLease(LeaseObservation::HeldByController), "owned lease observation");
        Require(session.Bind(a.Bind()) && session.Bind(b.Bind()), "bind received receipts");
        const auto now = GetTickCount64();
        const auto x = session.Grant(a.alias, Capability(a.alias), now, now + 10000);
        const auto y = session.Grant(b.alias, Capability(b.alias), now, now + 10000);
        Require(x && y, "issue scoped grants"); ga = *x; gb = *y;
    }
    void StartBoth() {
        a.Send(GrantWire(ga)); b.Send(GrantWire(gb));
        Require(Receive(a) == "READY" && Receive(b) == "READY", "both grants independently accepted");
    }
    std::string Receive(Child& child) {
        auto& sibling = &child == &a ? b : a;
        const auto& sibling_grant = &child == &a ? gb : ga;
        std::string response;
        try { response = child.Read(); }
        catch (...) {
            session.WorkerExited(Instance(child.alias));
            if (sibling.Alive()) sibling.Close(sibling_grant);
            throw;
        }
        if (response.starts_with("DENIED|")) {
            // The controller's receive path revokes both grants on wire denial;
            // callers do not manually invalidate or stop the other worker.
            session.WorkerExited(Instance(child.alias));
            child.AckExit();
            if (sibling.Alive()) Require(sibling.Close(sibling_grant), "denial stops sibling");
        }
        return response;
    }
    void Frame(Child& child, const PreviewGrant& g, std::uint64_t sequence) {
        Require(a.Alive() && b.Alive(), "both workers alive before dispatch");
        Require(session.Authorize(g, Operation::SimulatedPreview, sequence, GetTickCount64(),
            LeaseObservation::HeldByController), "controller authorizes before dispatch");
        child.Send("PREVIEW|" + g.capability + "|" + g.epoch + "|" + std::to_string(sequence));
        Check(Receive(child) == "FRAME|" + g.capability + "|" + g.epoch + "|" + std::to_string(sequence) +
            "|simulated-" + child.alias, "scoped wire frame");
    }
};
std::string LeaseName() { return "A0.Poc.TestLease." + Unique(); }
void Normal(HANDLE parent) {
    const auto name = LeaseName();
    {
        Pair p(name, parent); Probe(name, parent, 20); p.StartBoth();
        p.Frame(p.a, p.ga, 1); p.Frame(p.b, p.gb, 1); p.Frame(p.a, p.ga, 2); p.Frame(p.b, p.gb, 2);
        Require(p.session.BeginQuiesce(), "begin quiescence");
        Require(p.a.Close(p.ga), "A mock close");
        Check(p.session.ConfirmClosed(p.ga, true, true, true) && !p.session.AllCloseReceiptsObserved(), "one receipt insufficient");
        Probe(name, parent, 20); Require(p.b.Close(p.gb), "B mock close");
        Check(p.session.ConfirmClosed(p.gb, true, true, true) && p.session.AllCloseReceiptsObserved(), "both mock receipts close session");
        Check(!p.session.HardwareAllowed(), "closed never enables hardware");
    }
    Probe(name, parent, 0);
}
void RejectedGrant(HANDLE parent, int mutation) {
    Pair p(LeaseName(), parent); auto bad = p.ga;
    if (mutation == 0) bad.epoch = "old-epoch";
    if (mutation == 1) bad = p.gb;
    if (mutation == 2) bad.deadline_ms = bad.issued_at_ms;
    if (mutation == 3) bad.controller = "other-controller";
    if (mutation == 4) bad.instance = "other-worker";
    if (mutation == 5) bad.receipt = "other-binding";
    if (mutation == 6) bad.capability = "other-capability";
    if (mutation == 7) bad.issued_at_ms = bad.deadline_ms + 1;
    p.b.Send(GrantWire(p.gb)); Require(p.b.Read() == "READY", "sibling grant");
    p.a.Send(GrantWire(bad)); Check(p.Receive(p.a) == "DENIED|0|0", "invalid grant calls no fake transport");
    Check(p.session.Stage() == SessionStage::Invalid && !p.session.AllCloseReceiptsObserved(), "exit invalidates both; no close proof");
    Check(!p.a.Alive() && !p.b.Alive(), "denial receive path stopped both workers");
    Check(!p.session.Authorize(p.gb, Operation::SimulatedPreview, 1, GetTickCount64(),
        LeaseObservation::HeldByController), "old grant never revives");
}
void RejectedOperation(HANDLE parent, const std::string& operation) {
    Pair p(LeaseName(), parent); p.StartBoth(); p.Frame(p.a, p.ga, 1);
    // Bypass controller dispatch only to exercise the worker's independent gate.
    p.a.Send(operation + "|" + p.ga.capability + "|" + p.ga.epoch + "|1");
    Check(p.Receive(p.a) == "DENIED|1|1", "forbidden/replayed operation adds no callback");
    Check(!p.a.Alive() && !p.b.Alive(), "operation denial stops both workers");
    Check(!p.session.AllCloseReceiptsObserved() && !p.session.HardwareAllowed(), "failure cannot unlock hardware");
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--grant-worker") {
            std::uint64_t parent{}; if (!Number(argv[4], parent)) return 1;
            return GrantWorker(argv[2], argv[3], reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(parent)));
        }
        if (argc == 3 && std::string_view(argv[1]) == "--lease-probe") return LeaseProbe(argv[2]);
        if (argc != 1) return 1;
        Handle job(CreateJobObjectW(nullptr, nullptr)); Require(job.valid(), "cleanup job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "job policy");
        cleanup_job = job.value;
        Handle parent(OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId())); Require(parent.valid(), "parent monitor");
        Normal(parent.value);
        for (int mutation = 0; mutation < 8; ++mutation) RejectedGrant(parent.value, mutation);
        for (const auto* operation : {"SDK", "WPD", "CAPTURE", "PREVIEW"}) RejectedOperation(parent.value, operation);
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{}; const auto deadline = GetTickCount64() + kTimeout;
        do {
            Require(QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting,
                sizeof(accounting), nullptr), "query child cleanup");
            if (!accounting.ActiveProcesses) break;
            Sleep(10);
        } while (GetTickCount64() < deadline);
        Check(accounting.ActiveProcesses == 0, "no remaining children");
        std::cout << "{\"mode\":\"simulation\",\"hardwareAllowed\":false,\"status\":\""
                  << (failures ? "FAIL" : "PASS") << "\",\"failures\":" << failures << "}\n";
        cleanup_job = nullptr; return failures ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
