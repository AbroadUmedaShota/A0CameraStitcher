#include "a0/phase0/worker_preview_dispatcher.hpp"
#include "a0/phase0/preview_worker_timing.hpp"
#include "a0/phase0/agent_pipe_timing.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>
using namespace a0::phase0;
using namespace a0::phase0::experimental;
namespace {
int failures{};
void Check(bool value, const char* message) { if (!value) { ++failures; std::cerr << message << '\n'; } }
struct Fake {
    unsigned enumerations{}, opens{}, starts{}, frames{}, stops{}, suspends{}, resumes{}, closes{};
    std::string fail;
    // Every timeout argument the dispatcher passed, in call order.
    std::vector<std::pair<std::string, std::chrono::seconds>> timeouts;
    void Fault(const char* step) { if (fail == step) throw TransportError("injected", step); }
    void Note(const char* call, std::chrono::seconds timeout) { timeouts.emplace_back(call, timeout); }
    std::vector<std::string> BeginWorkerPreviewSelection(std::chrono::seconds t) { Note("enumerate", t); ++enumerations; Fault("enumerate"); return {"x","y"}; }
    void OpenWorkerPreviewCandidate(std::string_view, std::chrono::seconds t) { Note("select", t); ++opens; Fault("select"); }
    void StartSelectedWorkerLiveView(std::chrono::seconds t) { Note("start", t); ++starts; Fault("start"); }
    std::vector<unsigned char> ReadLiveViewFrame(std::chrono::seconds t) { Note("frame", t); ++frames; Fault("frame"); return {0xff,0xd8,0xff,0xd9}; }
    void StopLiveView(std::chrono::seconds t) { Note("stop", t); ++stops; Fault("stop"); }
    void SuspendSelectedWorkerPreview(std::chrono::seconds t) { Note("suspend", t); ++suspends; Fault("suspend"); }
    void ResumeSelectedWorkerPreview(std::string_view, std::chrono::seconds t) { Note("resume", t); ++resumes; Fault("resume"); }
    void Close(std::chrono::seconds t) { Note("close", t); ++closes; Fault("close"); }
    INikonDualSessionTransport::ExitState InspectDualSessionExitState() { return {false,fail == "retained",false}; }
};
std::string Request(unsigned sequence, std::string_view operation, std::string_view candidate = "", std::string_view capability = "secret") {
    return "{\"schema\":\"a0.preview-worker.v1\",\"epoch\":\"epoch\",\"capability\":\"" + std::string(capability) +
        "\",\"sequence\":" + std::to_string(sequence) + ",\"operation\":\"" + std::string(operation) +
        "\",\"candidate\":\"" + std::string(candidate) + "\"}";
}
bool Ok(const std::string& reply) { return reply.find("\"status\":\"ok\"") != std::string::npos; }
void Start(WorkerPreviewDispatcher<Fake>& d) {
    Check(Ok(d.Handle(Request(1,"enumerate"))), "enumerate");
    Check(Ok(d.Handle(Request(2,"select","x"))), "select");
    Check(Ok(d.Handle(Request(3,"start"))), "start");
}
bool Has(const std::string& reply, std::string_view text) { return reply.find(text) != std::string::npos; }

// WU1: the budget table, checked against an independent list of the commands
// the parent sends. Removing one command from the table, renaming it, or
// changing a value without updating this list fails here.
void BudgetTable() {
    namespace timing = preview_worker_timing;
    using std::chrono::milliseconds;
    using std::chrono::seconds;
    struct Expected { const char* operation; milliseconds worst; milliseconds deadline; };
    // W = SDK budget + failure cleanup + n x G (G = 1 s); D = W + M (M = 5 s).
    static constexpr Expected expected[] = {
        {"enumerate", seconds{22}, seconds{27}}, // 10 + Close 10 + 2G
        {"select", seconds{22}, seconds{27}},    // 10 + Close 10 + 2G
        {"start", seconds{42}, seconds{47}},     // 10 + 10 + 10 + Close 10 + 2G
        {"frame", seconds{29}, seconds{34}},     // 3 + 3 + Stop 10 + Close 10 + 3G
        {"suspend", seconds{33}, seconds{38}},   // Stop 10 + 10 + Close 10 + 3G
        {"resume", seconds{22}, seconds{27}},    // 10 + Close 10 + 2G
        {"close", seconds{22}, seconds{27}},     // Stop 10 + Close 10 + 2G
    };
    Check(timing::kWorkerOperationBudgets.size() == std::size(expected), "budget table has one entry per command");
    Check(timing::CoversEveryWorkerOperation(timing::kWorkerOperationBudgets), "budget table covers every command once");
    for (const auto& entry : expected) {
        const auto* budget = timing::FindWorkerOperationBudget(timing::kWorkerOperationBudgets, entry.operation);
        if (!budget) {
            Check(false, "budget table is missing a command");
            std::cerr << "  missing: " << entry.operation << '\n';
            continue;
        }
        Check(std::count(timing::kWorkerOperations.begin(), timing::kWorkerOperations.end(),
                         std::string_view(entry.operation)) == 1, "command list names the command once");
        const auto worst = timing::WorkerReplyWorst(*budget);
        const auto deadline = timing::ParentExchangeDeadline(*budget);
        Check(deadline >= worst + timing::kIpcMargin, "parent exchange deadline covers worker worst reply plus M");
        Check(worst == entry.worst && deadline == entry.deadline, "W and D match the reviewed budget");
        Check(timing::WorkerReplyWorst(entry.operation) == worst &&
              timing::ParentExchangeDeadline(entry.operation) == deadline, "lookup by name agrees with the entry");
        if (worst != entry.worst || deadline != entry.deadline)
            std::cerr << "  " << entry.operation << " W=" << worst.count() << " D=" << deadline.count() << '\n';
    }
    Check(!timing::ParentExchangeDeadline("capture") && !timing::WorkerReplyWorst(""), "unknown commands have no budget");
    // A table with one command renamed away no longer covers every command,
    // and the serving check rejects it.
    auto missing = timing::kWorkerOperationBudgets;
    missing[2].operation = "begin";
    Check(!timing::CoversEveryWorkerOperation(missing), "coverage check fails when one command is missing");
    Check(!timing::ServingLifetimeFits(timing::kDefaultOperationDeadline, timing::kDefaultServingLifetime, missing),
          "serving check rejects a table that misses a command");
    // Close step values derived from the same table.
    Check(timing::kExitWindowAfterSafeReceipt == seconds{5}, "E_ok is 5 s");
    Check(timing::ExitWindowWithoutReceipt(timing::kWorkerOperationBudgets) == seconds{30},
          "E_fail is close W 22 s + reply stages 3 s + M 5 s");
    Check(timing::LongestCommandExchange(timing::kWorkerOperationBudgets) == seconds{47}, "longest command is start");
    Check(timing::CloseSendWorst(timing::kWorkerOperationBudgets) == seconds{54}, "two close sends take at most 54 s");
    Check(timing::kPreviewSessionLimit == seconds{180} && timing::kDefaultOperationDeadline == seconds{60} &&
          timing::kDefaultServingLifetime == seconds{170}, "lifetimes: 60 s operation, 170 s serving, 180 s limit");
    Check(timing::ServingLifetimeFits(seconds{60}, seconds{161}, timing::kWorkerOperationBudgets) &&
          !timing::ServingLifetimeFits(seconds{60}, seconds{160}, timing::kWorkerOperationBudgets),
          "60 s + 47 s + 54 s = 161 s is the smallest serving lifetime for the default table");
    Check(!timing::ServingLifetimeFits(seconds{60}, seconds{181}, timing::kWorkerOperationBudgets) &&
          !timing::ServingLifetimeFits(seconds{60}, seconds{60}, timing::kWorkerOperationBudgets) &&
          !timing::ServingLifetimeFits(seconds{0}, seconds{170}, timing::kWorkerOperationBudgets),
          "serving lifetime above 180 s, equal to the operation deadline, or a zero deadline is rejected");
    auto raised = timing::kWorkerOperationBudgets;
    raised[6].sdk += seconds{5};
    Check(!timing::ServingLifetimeFits(timing::kDefaultOperationDeadline, timing::kDefaultServingLifetime, raised),
          "raising one budget value past the slack breaks the serving check");
    // M and the extra reply stages of E_fail come from the pipe host's stage
    // timeout, not from literals of their own.
    Check(agent_pipe_timing::kResponseWriteTimeout == milliseconds{1000} &&
          timing::kWorkerReplyStages == 3 * agent_pipe_timing::kResponseWriteTimeout &&
          timing::kWorkerReplyStages == seconds{3} && timing::kIpcMargin == seconds{5},
          "reply stages are 3 x the 1 s response stage, and M is those 3 s + 2 s");
    // S: added to D only for the parent's first exchange with a worker.
    Check(timing::kWorkerStartupAllowance == seconds{9}, "S is 9 s");
    for (const auto& entry : expected) {
        const auto* budget = timing::FindWorkerOperationBudget(timing::kWorkerOperationBudgets, entry.operation);
        if (!budget) continue; // Reported above.
        Check(timing::ParentExchangeDeadline(*budget, false) == entry.deadline &&
              timing::ParentExchangeDeadline(*budget, true) == entry.deadline + seconds{9},
              "first exchange waits D + S, every later exchange waits D");
    }
    const auto* enumerate = timing::FindWorkerOperationBudget(timing::kWorkerOperationBudgets, "enumerate");
    const auto* close = timing::FindWorkerOperationBudget(timing::kWorkerOperationBudgets, "close");
    Check(enumerate && timing::ParentExchangeDeadline(*enumerate, true) == seconds{36} &&
          timing::ParentExchangeDeadline(*enumerate, true) <= timing::LongestCommandExchange(timing::kWorkerOperationBudgets),
          "enumerate, the first command of each worker, waits 27 s + 9 s = 36 s, within the longest command (47 s)");
    Check(close && timing::ParentExchangeDeadline(*close, true) == seconds{36} &&
          timing::kDefaultOperationDeadline + timing::LongestCommandExchange(timing::kWorkerOperationBudgets) +
                  timing::CloseSendWorst(timing::kWorkerOperationBudgets) + timing::kWorkerStartupAllowance ==
              timing::kDefaultServingLifetime,
          "a close that is a worker's first exchange fits the default slack: 60 + 47 + 27 + 36 = 170 s");
    Check(timing::ServingLifetimeFits(seconds{60}, seconds{161}, timing::kWorkerOperationBudgets),
          "S leaves the serving check unchanged: 161 s still fits the default table");
}

// WU1: the dispatcher passes the same timeouts as before the table (10 s, 3 s).
void TransportTimeouts() {
    using std::chrono::seconds;
    Fake f;
    WorkerPreviewDispatcher d(f, "epoch", "secret", 10, 11, [] { return true; });
    Start(d);
    Check(Ok(d.Handle(Request(4, "frame"))), "timeout fixture frame");
    Check(Ok(d.Handle(Request(5, "suspend"))), "timeout fixture suspend");
    Check(Ok(d.Handle(Request(6, "resume", "x"))), "timeout fixture resume");
    Check(Ok(d.Handle(Request(7, "start"))), "timeout fixture second start");
    (void)d.Handle(Request(8, "close"));
    const std::vector<std::pair<std::string, seconds>> expected{
        {"enumerate", seconds{10}}, {"select", seconds{10}}, {"start", seconds{10}}, {"frame", seconds{3}},
        {"stop", seconds{10}}, {"suspend", seconds{10}}, {"resume", seconds{10}}, {"start", seconds{10}},
        {"stop", seconds{10}}, {"close", seconds{10}}};
    Check(f.timeouts == expected, "fake transport receives 10 s / 3 s exactly as before the budget table");
    if (f.timeouts != expected)
        for (const auto& [call, timeout] : f.timeouts) std::cerr << "  " << call << '=' << timeout.count() << '\n';
    // Failure cleanup after a frame failure: StopLiveView then Close, 10 s each.
    Fake fault;
    WorkerPreviewDispatcher x(fault, "epoch", "secret", 10, 11, [] { return true; });
    Start(x);
    fault.fail = "frame";
    (void)x.Handle(Request(4, "frame"));
    Check(fault.timeouts.size() == 6 && fault.timeouts[3] == std::pair<std::string, seconds>{"frame", seconds{3}} &&
          fault.timeouts[4] == std::pair<std::string, seconds>{"stop", seconds{10}} &&
          fault.timeouts[5] == std::pair<std::string, seconds>{"close", seconds{10}},
          "failure cleanup passes StopLiveView 10 s and Close 10 s");
}

// WU3: the operation deadline gates commands; only serving authority loss
// makes the dispatcher close on its own.
void OperationDeadline() {
    {
        Fake f;
        bool serving = true, operations = true;
        WorkerPreviewDispatcher d(f, "epoch", "secret", 10, 11, [&] { return serving; }, [&] { return operations; });
        Start(d);
        operations = false;
        d.OnIdle();
        Check(!d.ShouldStop() && f.closes == 0, "operation deadline alone does not make OnIdle close the session");
        const auto frame = d.Handle(Request(4, "frame"));
        Check(Has(frame, "\"status\":\"failed\"") && Has(frame, "\"error\":\"worker_authority_expired\"") &&
              f.frames == 0, "a command after the operation deadline is rejected before the SDK");
        Check(d.ShouldStop() && f.stops == 1 && f.closes == 1 && d.SafeToExit() && !d.Completed(),
              "the rejected command closes the session once, without claiming an explicit close");
    }
    {
        Fake f;
        bool serving = true, operations = true;
        WorkerPreviewDispatcher d(f, "epoch", "secret", 10, 11, [&] { return serving; }, [&] { return operations; });
        Start(d);
        operations = false;
        d.OnIdle();
        const auto closed = d.Handle(Request(4, "close"));
        Check(Has(closed, "\"status\":\"closed\"") && d.Completed() && f.stops == 1 && f.closes == 1,
              "close after the operation deadline is still accepted while serving");
    }
    {
        Fake f;
        bool serving = true, operations = true;
        WorkerPreviewDispatcher d(f, "epoch", "secret", 10, 11, [&] { return serving; }, [&] { return operations; });
        Start(d);
        operations = false;
        serving = false;
        d.OnIdle();
        Check(d.ShouldStop() && f.closes == 1 && d.SafeToExit() && !d.Completed(),
              "serving lifetime end makes OnIdle close the session once");
    }
}
}
int main() {
    BudgetTable();
    TransportTimeouts();
    OperationDeadline();
    Fake f; bool live = true;
    WorkerPreviewDispatcher d(f,"epoch","secret",10,11,[&] { return live; });
    Check(d.PeerAllowed(10) && !d.PeerAllowed(11), "only parent PID allowed");
    Start(d);
    const auto frame = d.Handle(Request(4,"frame"));
    Check(Ok(frame) && frame.find("ffd8ffd9") != std::string::npos, "actual frame bytes returned as bounded hex");
    Check(Ok(d.Handle(Request(5,"suspend"))), "suspend");
    Check(Ok(d.Handle(Request(6,"resume","x"))), "resume");
    Check(Ok(d.Handle(Request(7,"start"))), "second phase start");
    const auto closed = d.Handle(Request(8,"close"));
    Check(d.SafeToExit() && d.ShouldStop() && f.stops == 2 && f.closes == 1, "both phases close once");
    Check(d.Completed(), "only acknowledged explicit close can complete");
    Check(closed.find("\"workerPid\":11") != std::string::npos && closed.find("\"moduleClosed\":true") != std::string::npos,
          "bound close reply contains worker PID and SDK receipt");
    Check(closed.find("secret") == std::string::npos, "capability is not echoed");
    d.Handle(Request(9,"enumerate"));
    Check(f.enumerations == 1 && f.closes == 1, "terminal has no SDK retry");
    for (auto operation : {"capture", "wpd", "settings", "resume", "frame"}) {
        Fake denied; WorkerPreviewDispatcher x(denied,"epoch","secret",10,11,[] { return true; });
        Check(!Ok(x.Handle(Request(1,operation))) && denied.enumerations == 0 && denied.closes == 0,
              "unavailable operations do not load SDK");
    }
    Fake select_fault;
    WorkerPreviewDispatcher selection(select_fault,"epoch","secret",10,11,[] { return true; });
    Check(Ok(selection.Handle(Request(1,"enumerate"))), "fault fixture enumerates before selection");
    select_fault.fail = "select";
    const auto selection_reply = selection.Handle(Request(2,"select","x"));
    Check(selection_reply.find("\"status\":\"failed\"") != std::string::npos &&
          selection_reply.find("\"error\":\"injected\"") != std::string::npos &&
          selection_reply.find("\"safeToExit\":true") != std::string::npos,
          "selection failure reply retains fixed category and close receipt");
    Check(selection.SafeToExit() && !selection.Completed() && select_fault.closes == 1 &&
          select_fault.starts == 0, "selection failure closes once without claiming explicit success");
    for (auto failure : {"frame","stop","close","retained","suspend"}) {
        Fake fault; WorkerPreviewDispatcher x(fault,"epoch","secret",10,11,[] { return true; }); Start(x);
        fault.fail = failure;
        x.Handle(Request(4, fault.fail == "frame" ? "frame" : fault.fail == "suspend" ? "suspend" : "close"));
        Check(x.ShouldStop() && fault.closes == 1 && fault.stops == 1, "fault closes once without retry");
        Check(x.SafeToExit() == (fault.fail == "frame"), "unknown shutdown is quarantined");
    }
    Fake replay; WorkerPreviewDispatcher r(replay,"epoch","secret",10,11,[] { return true; });
    r.Handle(Request(1,"enumerate")); r.Handle(Request(1,"enumerate"));
    Check(r.ShouldStop() && replay.enumerations == 1 && replay.closes == 1, "duplicate command is terminal");
    Fake unauthorized; WorkerPreviewDispatcher u(unauthorized,"epoch","secret",10,11,[] { return true; });
    u.Handle(Request(1,"enumerate","","wrong"));
    Check(u.ShouldStop() && unauthorized.enumerations == 0, "wrong capability rejected before SDK");
    Fake expired; WorkerPreviewDispatcher e(expired,"epoch","secret",10,11,[&] { return live; }); Start(e);
    live = false; e.OnIdle();
    Check(e.SafeToExit() && expired.stops == 1 && expired.closes == 1, "parent loss or expiry drains once");
    Check(!e.Completed(), "automatic cleanup is not a successful session");
    Fake delivery; WorkerPreviewDispatcher t(delivery,"epoch","secret",10,11,[] { return true; }); Start(t);
    t.TransportFailed(); t.TransportFailed();
    Check(delivery.stops == 1 && delivery.closes == 1 && t.SafeToExit(), "delivery failure cannot repeat cleanup");
    std::cout << "{\"mode\":\"simulation\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
