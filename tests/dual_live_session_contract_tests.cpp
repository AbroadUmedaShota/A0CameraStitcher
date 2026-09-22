#include "a0/phase0/dual_live_session_contract.hpp"
#include <iostream>
#include <stdexcept>

using namespace a0::phase0::experimental;
namespace {
int failures{};
void Check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
struct Fixture {
    DualLiveSessionContract session{"controller-1", "epoch-1"};
    PreviewGrant a, b;
    Fixture() {
        if (!session.ObserveLease(LeaseObservation::HeldByController) ||
            !session.Bind({"CAM-A", "worker-a", "epoch-1", "receipt-a", "synthetic-body-a"}) ||
            !session.Bind({"CAM-B", "worker-b", "epoch-1", "receipt-b", "synthetic-body-b"}))
            throw std::runtime_error("fixture setup failed");
        const auto ga = session.Grant("CAM-A", "test-capability-a", 10, 100);
        const auto gb = session.Grant("CAM-B", "test-capability-b", 10, 100);
        if (!ga || !gb) throw std::runtime_error("fixture grants failed");
        a = *ga; b = *gb;
    }
};
void WorkerSelectionContracts() {
    WorkerLocalSelection a("worker-a", "epoch-1", {71, 83});
    WorkerLocalSelection b("worker-b", "epoch-1", {71, 83});
    std::vector<unsigned> a_open, b_open;
    Check(a.Select("worker-a", "epoch-1", 71) && b.Select("worker-b", "epoch-1", 71),
          "same numeric Source in separate workers is not a global collision");
    Check(a.OpenSelected("worker-a", "epoch-1", [&](unsigned id) { a_open.push_back(id); return true; }) &&
          b.OpenSelected("worker-b", "epoch-1", [&](unsigned id) { b_open.push_back(id); return true; }),
          "worker-selected fake transport seam opens exactly its assignment");
    Check(a_open == std::vector<unsigned>{71} && b_open == std::vector<unsigned>{71}, "nonassigned Source never opened");
    Check(!a.OpenSelected("worker-a", "epoch-1", [&](unsigned id) { a_open.push_back(id); return true; }) &&
          a_open.size() == 1, "repeated Open refused before callback");
    WorkerLocalSelection crossed("worker-a", "epoch-1", {71, 83});
    Check(!crossed.Select("worker-b", "epoch-1", 71), "cross-worker selection refused");
    Check(!crossed.Select("worker-a", "epoch-1", 71), "invalid selection cannot be retried");
    WorkerLocalSelection stale("worker-a", "epoch-2", {71});
    Check(!stale.Select("worker-a", "epoch-1", 71), "old generation selection refused");
    WorkerLocalSelection missing("worker-a", "epoch-1", {71});
    Check(!missing.Select("worker-a", "epoch-1", 83), "unknown selection refused");
    WorkerLocalSelection ambiguous("worker-a", "epoch-1", {71, 71});
    Check(!ambiguous.Select("worker-a", "epoch-1", 71), "ambiguous local selection refused");
    WorkerLocalSelection failed("worker-a", "epoch-1", {71});
    failed.Select("worker-a", "epoch-1", 71);
    unsigned calls{};
    Check(!failed.OpenSelected("worker-a", "epoch-1", [&](unsigned) { ++calls; return false; }), "failed fake Open recorded");
    Check(!failed.OpenSelected("worker-a", "epoch-1", [&](unsigned) { ++calls; return true; }) && calls == 1,
          "failed fake Open is never retried");
}
void BindingAndLeaseContracts() {
    DualLiveSessionContract no_owner("controller", "epoch-1");
    Check(!no_owner.Bind({"CAM-A", "a", "epoch-1", "r-a", "fake-a"}), "lease observation required before binding");
    DualLiveSessionContract duplicates("controller", "epoch-1");
    duplicates.ObserveLease(LeaseObservation::HeldByController);
    Check(duplicates.Bind({"CAM-A", "a", "epoch-1", "r-a", "same-fake-body"}), "first fake body receipt");
    Check(!duplicates.Bind({"CAM-B", "b", "epoch-1", "r-b", "same-fake-body"}), "two source identities cannot hide same synthetic physical body");
    DualLiveSessionContract partial("controller", "epoch-1");
    partial.ObserveLease(LeaseObservation::HeldByController);
    partial.Bind({"CAM-A", "a", "epoch-1", "r-a", "fake-a"});
    Check(!partial.Grant("CAM-A", "cap", 1, 5), "no delegation before both assignments");
    for (const auto loss : {LeaseObservation::Missing, LeaseObservation::Abandoned}) {
        Fixture f;
        Check(!f.session.Authorize(f.a, Operation::SimulatedPreview, 1, 20, loss), "lease loss/abandonment blocks delegation");
        Check(!f.session.Authorize(f.b, Operation::SimulatedPreview, 1, 21, LeaseObservation::HeldByController),
              "later lease availability cannot revive old grants");
    }
    Fixture borrowed;
    auto forged = borrowed.a;
    forged.instance = "worker-b";
    Check(!borrowed.session.Authorize(forged, Operation::SimulatedPreview, 1, 20, LeaseObservation::HeldByController),
          "worker cannot borrow sibling capability");
    Fixture replay;
    Check(replay.session.Authorize(replay.a, Operation::SimulatedPreview, 1, 20, LeaseObservation::HeldByController), "first delegated fake preview");
    Check(!replay.session.Authorize(replay.a, Operation::SimulatedPreview, 1, 21, LeaseObservation::HeldByController), "replayed operation refused");
    Fixture expired;
    Check(!expired.session.Authorize(expired.a, Operation::SimulatedPreview, 1, 100, LeaseObservation::HeldByController), "deadline is exclusive");
    Fixture preissue;
    Check(!preissue.session.Authorize(preissue.a, Operation::SimulatedPreview, 1, 9, LeaseObservation::HeldByController),
          "authority cannot be used before issue time");
    Fixture clock;
    Check(clock.session.Authorize(clock.a, Operation::SimulatedPreview, 1, 30, LeaseObservation::HeldByController) &&
          !clock.session.Authorize(clock.b, Operation::SimulatedPreview, 1, 29, LeaseObservation::HeldByController),
          "time regression across workers invalidates the controller session");
    Fixture epoch;
    auto old = epoch.a; old.epoch = "old-epoch";
    Check(!epoch.session.Authorize(old, Operation::SimulatedPreview, 1, 20, LeaseObservation::HeldByController), "old generation capability refused");
    Fixture topology;
    topology.session.TopologyChanged();
    Check(!topology.session.Authorize(topology.b, Operation::SimulatedPreview, 1, 20, LeaseObservation::HeldByController), "topology change revokes both assignments");
    Check(topology.session.Failure() == "topology_changed", "first invalidation cause retained");
}
void CloseAndHardwareContracts() {
    Fixture f;
    Check(f.session.Authorize(f.a, Operation::SimulatedPreview, 1, 20, LeaseObservation::HeldByController) &&
          f.session.Authorize(f.b, Operation::SimulatedPreview, 1, 20, LeaseObservation::HeldByController), "both simulated previews authorized");
    Check(f.session.BeginQuiesce(), "quiesce revokes further preview authority");
    Check(f.session.ConfirmClosed(f.a, true, true, true) && !f.session.AllCloseReceiptsObserved(), "one close is not enough");
    Check(f.session.ConfirmClosed(f.b, true, true, true) && f.session.AllCloseReceiptsObserved(), "both explicit close receipts");
    Check(!f.session.HardwareAllowed(), "even simulated closed receipts never permit hardware");
    Check(!f.session.Grant("CAM-A", "new-cap", 30, 100), "closed session cannot automatically reuse binding");
    Check(!f.session.ConfirmClosed(f.a, true, true, true) && f.session.AllCloseReceiptsObserved(),
          "late duplicate close cannot erase completed shutdown");
    f.session.WorkerExited("worker-a");
    Check(f.session.Stage() == SessionStage::Closed, "expected worker exit after proven close preserves closure");
    Fixture dying;
    dying.session.BeginQuiesce();
    dying.session.WorkerExited("worker-a");
    Check(!dying.session.AllCloseReceiptsObserved() && !dying.session.ConfirmClosed(dying.b, true, true, true),
          "worker exit cannot stand in for SDK teardown");
    Fixture incomplete;
    incomplete.session.BeginQuiesce();
    Check(!incomplete.session.ConfirmClosed(incomplete.a, true, true, false), "missing module close blocks clean quiescence");
    Fixture revoked;
    revoked.session.BeginQuiesce();
    Check(!revoked.session.Authorize(revoked.a, Operation::SimulatedPreview, 2, 30, LeaseObservation::HeldByController),
          "no frame operation after quiesce begins");
    for (const auto operation : {Operation::RealSdk, Operation::Wpd, Operation::Capture}) {
        Fixture forbidden;
        Check(!forbidden.session.Authorize(forbidden.a, operation, 1, 20, LeaseObservation::HeldByController),
              "preview capability cannot authorize SDK, WPD, or capture");
    }
}
void SimulatedDispatchContract() {
    Fixture f;
    WorkerLocalSelection a("worker-a", "epoch-1", {71, 83});
    WorkerLocalSelection b("worker-b", "epoch-1", {71, 83});
    unsigned opens{}, frames{};
    Check(a.Select("worker-a", "epoch-1", 71) && b.Select("worker-b", "epoch-1", 71), "select in each worker namespace");
    const auto dispatch_open = [&](WorkerLocalSelection& selection, const PreviewGrant& grant) {
        if (!f.session.Authorize(grant, Operation::SimulatedPreview, 1, 20, LeaseObservation::HeldByController)) return false;
        return selection.OpenSelected(grant.instance, grant.epoch, [&](unsigned id) { ++opens; return id == 71; });
    };
    Check(dispatch_open(a, f.a) && dispatch_open(b, f.b) && opens == 2, "delegated grant drives only worker-local fake Open");
    const auto dispatch_frame = [&](const PreviewGrant& grant, unsigned sequence) {
        if (!f.session.Authorize(grant, Operation::SimulatedPreview, sequence, 30, LeaseObservation::HeldByController)) return false;
        ++frames; return true;
    };
    Check(dispatch_frame(f.a, 2) && dispatch_frame(f.b, 2), "both fake workers dispatch under their own grants");
    f.session.ControllerExited();
    Check(!dispatch_frame(f.a, 3) && !dispatch_frame(f.b, 3) && frames == 2,
          "controller loss stops both transport callbacks, without regrant or retry");
}
}
int main() {
    try { WorkerSelectionContracts(); BindingAndLeaseContracts(); CloseAndHardwareContracts(); SimulatedDispatchContract(); }
    catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    std::cout << "{\"schema\":\"a0.dual-live-session-contract.v1\",\"mode\":\"simulation\",\"hardwareAllowed\":false,\"status\":\""
              << (failures ? "FAIL" : "PASS") << "\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
