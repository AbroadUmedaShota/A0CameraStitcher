#include "a0/phase0/preview_commissioning.hpp"
#include "a0/phase0/worker_preview_dispatcher.hpp"
#include "a0/phase0/preview_worker_reply.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include <iostream>
using namespace a0::phase0;
using namespace a0::phase0::experimental;
namespace json = a0::common::protocol_json;
namespace {
int failures{};
void Check(bool ok, const char* message) { if (!ok) { ++failures; std::cerr << message << '\n'; } }
struct Failure { [[noreturn]] static void Fail(std::string_view, std::string_view) { throw std::runtime_error("JSON rejected"); } };
struct Fake {
    unsigned opens{}, starts{}, frames{}, stops{}, resumes{};
    unsigned char pixel{};
    std::string fail;
    std::vector<std::string> BeginWorkerPreviewSelection(std::chrono::seconds) { return {"same-local-token", "other"}; }
    void OpenWorkerPreviewCandidate(std::string_view, std::chrono::seconds) { ++opens; }
    void StartSelectedWorkerLiveView(std::chrono::seconds) { ++starts; }
    std::vector<unsigned char> ReadLiveViewFrame(std::chrono::seconds) { ++frames; return std::vector<unsigned char>(256U * 1024U, pixel); }
    void StopLiveView(std::chrono::seconds) { ++stops; }
    void SuspendSelectedWorkerPreview(std::chrono::seconds) {}
    void ResumeSelectedWorkerPreview(std::string_view candidate, std::chrono::seconds) {
        ++resumes;
        if (fail == "resume" || candidate != "same-local-token") throw TransportError("injected", "resume");
    }
    void Close(std::chrono::seconds) {}
    INikonDualSessionTransport::ExitState InspectDualSessionExitState() { return {}; }
    PreviewTopologyDiag WorkerTopologyDiagnostics() const noexcept { return {}; }
    void MarkWorkerTopologyOperation(PreviewTopologyOperation) noexcept {}
};
struct Fixture {
    Fake a, b;
    WorkerPreviewDispatcher<Fake> da{a,"epoch","secret",10,11,[] { return true; }};
    WorkerPreviewDispatcher<Fake> db{b,"epoch","secret",10,12,[] { return true; }};
    std::array<unsigned,2> sequence{};
    PreviewCommissioning flow{[this](std::size_t worker, std::string_view op, std::string_view candidate) {
        const auto wire = "{\"schema\":\"a0.preview-worker.v2\",\"epoch\":\"epoch\",\"capability\":\"secret\",\"sequence\":" +
            std::to_string(++sequence[worker]) + ",\"operation\":\"" + std::string(op) + "\",\"candidate\":\"" + json::JsonEscape(candidate) + "\"}";
        const auto reply = (worker == 0 ? da : db).Handle(wire);
        auto parsed = ParsePreviewWorkerReply(reply, "epoch", worker == 0 ? 11 : 12,
                                              sequence[worker], op);
        if (parsed.status != PreviewWorkerReplyStatus::ok) throw std::runtime_error("worker failed");
        return std::move(parsed.payload);
    }};
    Fixture() { a.pixel = 0xab; b.pixel = 0xcd; }
    void Observe(std::size_t worker, ObservedPreviewBody body) {
        const auto candidates = flow.Enumerate(worker);
        const auto bytes = flow.Preview(worker, candidates[0]);
        Check(bytes.size() == 256U * 1024U && bytes.front() == (worker == 0 ? 0xab : 0xcd), "full bounded preview survives JSON hex round trip");
        flow.ConfirmAndSuspend(worker, body);
    }
    void Ready() { Observe(0,ObservedPreviewBody::CameraB); Observe(1,ObservedPreviewBody::CameraA); }
};
template<class Action> void Reject(Action action, const char* message) {
    bool rejected{}; try { action(); } catch (...) { rejected = true; } Check(rejected,message);
}
}
int main() {
    Fixture ok; ok.Ready();
    Check(ok.a.stops == 1 && ok.b.stops == 1, "both commissioned sources stopped before concurrent grant");
    ok.flow.StartBoth();
    Check(ok.a.starts == 2 && ok.b.starts == 2 && ok.a.resumes == 1 && ok.b.resumes == 1, "both sources resume exactly once");
    Check(ok.flow.Read(ObservedPreviewBody::CameraA).front() == 0xcd &&
          ok.flow.Read(ObservedPreviewBody::CameraB).front() == 0xab, "aliases follow explicit observation, not process order or same token text");
    Reject([&] { ok.flow.StartBoth(); }, "concurrent grant cannot be repeated");
    const auto calls = ok.sequence;
    Reject([&] { ok.flow.Read(ObservedPreviewBody::CameraA); }, "terminal workflow cannot send again");
    Check(ok.sequence == calls, "no sends after terminal failure");
    Fixture early;
    Reject([&] { early.flow.Enumerate(1); }, "worker B cannot enumerate before A is suspended");
    Check(early.sequence[0] == 0 && early.sequence[1] == 0, "early B rejected before transport");
    Fixture unbound; unbound.Observe(0,ObservedPreviewBody::CameraA);
    Reject([&] { unbound.flow.StartBoth(); }, "one body confirmation is insufficient");
    Check(unbound.a.resumes == 0 && unbound.b.starts == 0, "no premature concurrent source open");
    Fixture duplicate; duplicate.Observe(0,ObservedPreviewBody::CameraA);
    duplicate.flow.Enumerate(1); duplicate.flow.Preview(1,"same-local-token");
    Reject([&] { duplicate.flow.ConfirmAndSuspend(1,ObservedPreviewBody::CameraA); }, "same physical body assignment rejected");
    Reject([&] { duplicate.flow.StartBoth(); }, "duplicate observation cannot grant concurrent operation");
    Fixture candidate; candidate.flow.Enumerate(0);
    Reject([&] { candidate.flow.Preview(0,"not-enumerated"); }, "unknown candidate rejected");
    Check(candidate.a.opens == 0, "unknown candidate never opens SDK source");
    Fixture mid; mid.Ready(); mid.b.fail = "resume";
    Reject([&] { mid.flow.StartBoth(); }, "second resume failure ends session");
    Reject([&] { mid.flow.StartBoth(); }, "failed resume not retried");
    Check(mid.a.resumes == 1 && mid.b.resumes == 1 && mid.b.starts == 1, "failure never retries or starts failed source");
    // v2 replies carry the 18 topology counters; all zero here.
    const std::string kZeroDiag =
        R"({"openAdd":0,"openRemove":0,"inventoryAdd":0,"inventoryRemove":0,"inventoryPumps":0,)"
        R"("snapshotChildren":0,"snapshotEventIds":0,"snapshotMs":0,"postAddKnown":0,"postAddUnknown":0,)"
        R"("postRemoveKnown":0,"postRemoveUnknown":0,"postAddKnownDistinct":0,"postFirstEventOp":0,)"
        R"("postFirstEventMs":0,"checkValidBeforePump":0,"checkSetEqual":0,"checkCurrentCount":0})";
    // The default parser limit remains unchanged for all existing protocols.
    const std::string large = "\"" + std::string(300U * 1024U, 'a') + "\"";
    Reject([&] { json::BasicJsonParser<Failure>(large).Parse(); }, "legacy parser size limit preserved");
    Check(json::BasicJsonParser<Failure, 512U * 1024U + 4096>(large).Parse().string.size() == 300U * 1024U,
          "explicit preview parser accepts bounded large replies");
    const std::string failed_reply =
        R"({"schema":"a0.preview-worker.v2","epoch":"epoch","workerPid":11,"sequence":2,"status":"failed","payload":{"error":"open_failed","close":{"liveViewOff":true,"sourceClosed":true,"moduleClosed":true,"processClaimReleased":true,"safeToExit":true}},"diag":)" + kZeroDiag + "}";
    const auto failed = ParsePreviewWorkerReply(failed_reply, "epoch", 11, 2, "select");
    Check(failed.status == PreviewWorkerReplyStatus::failed && failed.error_category == "open_failed" &&
          failed.close_receipt.has_value() && failed.close_receipt->safe_to_exit,
          "validated failure retains bounded category and reported close receipt");
    const std::string closed_reply =
        R"({"schema":"a0.preview-worker.v2","epoch":"epoch","workerPid":11,"sequence":3,"status":"closed","payload":{"liveViewOff":true,"sourceClosed":true,"moduleClosed":true,"processClaimReleased":true,"safeToExit":true},"diag":)" + kZeroDiag + "}";
    Check(ParsePreviewWorkerReply(closed_reply, "epoch", 11, 3, "close").close_receipt->Complete(),
          "explicit close requires complete reported receipt");
    auto incomplete_close = closed_reply;
    incomplete_close.replace(incomplete_close.find("\"sourceClosed\":true"), sizeof("\"sourceClosed\":true") - 1, "\"sourceClosed\":false");
    Reject([&] { (void)ParsePreviewWorkerReply(incomplete_close, "epoch", 11, 3, "close"); },
           "incomplete explicit close remains unconfirmed");
    incomplete_close.replace(incomplete_close.find("\"closed\""), sizeof("\"closed\"") - 1, "\"quarantined\"");
    Check(ParsePreviewWorkerReply(incomplete_close, "epoch", 11, 3, "close").status ==
              PreviewWorkerReplyStatus::quarantined,
          "quarantined close preserves incomplete reported receipt for diagnosis");
    auto missing_close = failed_reply;
    missing_close.replace(missing_close.find("\"safeToExit\":true"), sizeof("\"safeToExit\":true") - 1, "\"safeToExit\":null");
    Reject([&] { (void)ParsePreviewWorkerReply(missing_close, "epoch", 11, 2, "select"); },
           "missing or non-boolean close evidence is rejected");
    auto bad_category = failed_reply;
    bad_category.replace(bad_category.find("open_failed"), sizeof("open_failed") - 1, "C:/private/device");
    Reject([&] { (void)ParsePreviewWorkerReply(bad_category, "epoch", 11, 2, "select"); },
           "free-form worker error text cannot cross the diagnostic boundary");
    Reject([&] { (void)ParsePreviewWorkerReply(failed_reply, "other-epoch", 11, 2, "select"); },
           "failure evidence from another epoch is rejected");
    Reject([&] { (void)ParsePreviewWorkerReply(failed_reply, "epoch", 12, 2, "select"); },
           "failure evidence from another PID is rejected");
    Reject([&] { (void)ParsePreviewWorkerReply(failed_reply, "epoch", 11, 3, "select"); },
           "failure evidence from another sequence is rejected");
    std::cout << "{\"mode\":\"fake-commissioning\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
