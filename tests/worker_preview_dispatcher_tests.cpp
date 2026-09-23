#include "a0/phase0/worker_preview_dispatcher.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include <iostream>
using namespace a0::phase0;
using namespace a0::phase0::experimental;
namespace {
int failures{};
void Check(bool value, const char* message) { if (!value) { ++failures; std::cerr << message << '\n'; } }
struct Fake {
    unsigned enumerations{}, opens{}, starts{}, frames{}, stops{}, suspends{}, resumes{}, closes{};
    std::string fail;
    void Fault(const char* step) { if (fail == step) throw TransportError("injected", step); }
    std::vector<std::string> BeginWorkerPreviewSelection(std::chrono::seconds) { ++enumerations; Fault("enumerate"); return {"x","y"}; }
    void OpenWorkerPreviewCandidate(std::string_view, std::chrono::seconds) { ++opens; Fault("select"); }
    void StartSelectedWorkerLiveView(std::chrono::seconds) { ++starts; Fault("start"); }
    std::vector<unsigned char> ReadLiveViewFrame(std::chrono::seconds) { ++frames; Fault("frame"); return {0xff,0xd8,0xff,0xd9}; }
    void StopLiveView(std::chrono::seconds) { ++stops; Fault("stop"); }
    void SuspendSelectedWorkerPreview(std::chrono::seconds) { ++suspends; Fault("suspend"); }
    void ResumeSelectedWorkerPreview(std::string_view, std::chrono::seconds) { ++resumes; Fault("resume"); }
    void Close(std::chrono::seconds) { ++closes; Fault("close"); }
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
}
int main() {
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
