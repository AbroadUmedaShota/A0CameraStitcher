#include "a0/phase0/dual_binding_camera_agent.hpp"
#include "a0/phase0/phase0.hpp"
#ifdef A0_TEST_NIKON_SDK_STUB
#include "a0/phase0/nikon_sdk_transport.hpp"
#endif

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace a0::phase0;

int failures = 0;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// ---------------------------------------------------------------------------
// Request / response helpers
// ---------------------------------------------------------------------------

std::string Envelope(
    std::string_view request_id, std::string_view operation, std::string_view payload) {
    std::string json = R"({"schemaVersion":")";
    json += kDualBindingCameraAgentSchemaVersion;
    json += R"(","simulation":false,"marker":")";
    json += kDualBindingCameraAgentMarker;
    json += R"(","requestId":")";
    json += request_id;
    json += R"(","operation":")";
    json += operation;
    json += R"(","payload":)";
    json += payload;
    json += "}";
    return json;
}

std::string BeginBindingRequest(std::string_view request_id = "r-begin") {
    return Envelope(request_id, "begin-binding", R"({"cameraMode":"DualCamera"})");
}

std::string StartLiveViewRequest(std::string_view session, std::size_t ordinal) {
    return Envelope(
        "r-start", "start-candidate-live-view",
        R"({"sessionId":")" + std::string(session) + R"(","candidateOrdinal":)" +
            std::to_string(ordinal) + "}");
}

std::string FrameRequest(std::string_view session, std::size_t ordinal) {
    return Envelope(
        "r-frame", "get-candidate-live-view-frame",
        R"({"sessionId":")" + std::string(session) + R"(","candidateOrdinal":)" +
            std::to_string(ordinal) + "}");
}

std::string ConfirmAliasRequest(
    std::string_view session, std::size_t ordinal, std::string_view alias) {
    return Envelope(
        "r-confirm", "confirm-alias",
        R"({"sessionId":")" + std::string(session) + R"(","candidateOrdinal":)" +
            std::to_string(ordinal) + R"(,"cameraAlias":")" + std::string(alias) + "\"}");
}

std::string CompleteBindingRequest(
    std::string_view session, std::string_view confirmed_at = "2026-08-21T00:00:00Z") {
    return Envelope(
        "r-complete", "complete-binding",
        R"({"sessionId":")" + std::string(session) + R"(","confirmedAtUtc":")" +
            std::string(confirmed_at) + "\"}");
}

std::string ActivateCaptureRequest(std::string_view session) {
    return Envelope(
        "r-activate", "activate-capture",
        R"({"sessionId":")" + std::string(session) + "\"}");
}

std::string CancelBindingRequest(std::string_view session) {
    return Envelope(
        "r-cancel", "cancel-binding",
        R"({"sessionId":")" + std::string(session) + "\"}");
}

// Reads one quoted field out of a response. Deliberately a plain substring
// search rather than a JSON parse: the point of these tests is to pin the exact
// bytes the agent puts on the wire, so re-parsing them with the same parser the
// agent used would hide a serializer that agrees with itself and nobody else.
std::string StringFieldOf(std::string_view response, std::string_view field) {
    const std::string key = "\"" + std::string(field) + "\":\"";
    const std::size_t start = response.find(key);
    if (start == std::string_view::npos) return {};
    const std::size_t value_start = start + key.size();
    const std::size_t end = response.find('"', value_start);
    if (end == std::string_view::npos) return {};
    return std::string(response.substr(value_start, end - value_start));
}

bool Succeeded(std::string_view response) {
    return response.find("\"success\":true") != std::string_view::npos;
}

std::string ResultCode(std::string_view response) {
    return StringFieldOf(response, "resultCode");
}

struct Harness {
    std::shared_ptr<DualBindingFakeSdkAdapter> adapter;
    DualBindingCameraAgentDispatcher dispatcher;

    explicit Harness(DualBindingFakeSdkOptions options = {})
        : adapter(std::make_shared<DualBindingFakeSdkAdapter>(std::move(options))),
          dispatcher(adapter) {}

    std::string Begin() { return dispatcher.Handle(BeginBindingRequest()); }

    // Drives a session to the point where both aliases are confirmed and every
    // candidate is quiesced, so the tests that care about what happens after
    // that do not each repeat the ceremony.
    std::string BindBothAliases() {
        const std::string session = StringFieldOf(Begin(), "sessionId");
        (void)dispatcher.Handle(StartLiveViewRequest(session, 0));
        (void)dispatcher.Handle(FrameRequest(session, 0));
        (void)dispatcher.Handle(ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA));
        (void)dispatcher.Handle(StartLiveViewRequest(session, 1));
        (void)dispatcher.Handle(FrameRequest(session, 1));
        (void)dispatcher.Handle(ConfirmAliasRequest(session, 1, kDualIdentityCameraAliasB));
        return session;
    }
};

class FailingEnumerationAdapter final : public DualBindingSdkAdapter {
public:
    explicit FailingEnumerationAdapter(std::string category)
        : category_(std::move(category)) {}

    std::vector<std::string> EnumerateCandidates() override {
        throw TransportError(category_, "machine-specific SDK detail must not cross the boundary");
    }
    bool StartLiveView(std::size_t) override { return false; }
    bool StopLiveView(std::size_t) override { return false; }
    std::vector<std::uint8_t> ReadLiveViewFrame(std::size_t) override { return {}; }
    bool CloseCandidateSession(std::size_t) override { return false; }
    bool EndBindingSession(std::chrono::seconds) noexcept override { return true; }
    DualIdentityInvalidationReason PollInvalidation() override {
        return DualIdentityInvalidationReason::None;
    }

private:
    std::string category_;
};

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

void EnvelopeMustBeTheBindingProtocol() {
    Harness harness;

    // The stable v2 protocol must not be servable here, and this protocol must
    // not be servable by v2. The marker is what makes that true in both
    // directions without either dispatcher knowing the other exists.
    const std::string v2_marker = R"({"schemaVersion":"a0.camera-agent.hardware-dual-binding.v1",)"
        R"("simulation":false,"marker":"Hardware","requestId":"r-1",)"
        R"("operation":"begin-binding","payload":{"cameraMode":"DualCamera"}})";
    Check(
        ResultCode(harness.dispatcher.Handle(v2_marker)) == "DualBindingProtocolRequired",
        "a request carrying the Dual v2 marker is refused by the binding agent");

    const std::string v2_schema = R"({"schemaVersion":"a0.camera-agent.hardware-dual.v2",)"
        R"("simulation":false,"marker":"HardwareBinding","requestId":"r-1",)"
        R"("operation":"begin-binding","payload":{"cameraMode":"DualCamera"}})";
    Check(
        ResultCode(harness.dispatcher.Handle(v2_schema)) == "DualBindingProtocolRequired",
        "a request carrying the Dual v2 schema version is refused");

    const std::string simulated = R"({"schemaVersion":"a0.camera-agent.hardware-dual-binding.v1",)"
        R"("simulation":true,"marker":"HardwareBinding","requestId":"r-1",)"
        R"("operation":"begin-binding","payload":{"cameraMode":"DualCamera"}})";
    Check(
        ResultCode(harness.dispatcher.Handle(simulated)) == "DualBindingProtocolRequired",
        "a simulated request is refused by the hardware binding agent");

    Check(
        ResultCode(harness.dispatcher.Handle(
            Envelope("r-1", "stop-candidate-live-view", "{}"))) == "UnsupportedOperation",
        "an operation outside the five defined ones is refused");

    Check(
        ResultCode(harness.dispatcher.Handle(Envelope(
            "r-1", "begin-binding",
            R"({"cameraMode":"DualCamera","extra":1})"))) == "UnexpectedField",
        "an unexpected payload field is refused rather than ignored");

    Check(
        ResultCode(harness.dispatcher.Handle("{ not json")) == "MalformedEnvelope",
        "a malformed envelope is refused");
    Check(
        StringFieldOf(harness.dispatcher.Handle("{ not json"), "requestId") == "rejected",
        "a malformed envelope still produces a correlatable response");

    Check(
        ResultCode(harness.dispatcher.Handle(
            Envelope("r 1 with spaces", "begin-binding",
                     R"({"cameraMode":"DualCamera"})"))) == "InvalidRequestId",
        "an unsafe request ID is refused");

    Check(
        StringFieldOf(harness.dispatcher.Handle(BeginBindingRequest("r-echo")), "requestId") ==
            "r-echo",
        "a successful response echoes the request ID it correlates to");
}

void SessionIdShapeIsEnforcedBeforeItIsCompared() {
    Harness harness;
    (void)harness.Begin();

    Check(
        ResultCode(harness.dispatcher.Handle(Envelope(
            "r-1", "start-candidate-live-view",
            R"({"sessionId":"short","candidateOrdinal":0})"))) == "InvalidSessionId",
        "a malformed session ID is refused by the parser");
    Check(
        ResultCode(harness.dispatcher.Handle(Envelope(
            "r-1", "start-candidate-live-view",
            R"({"sessionId":"00000000000000000000000000000000",)"
            R"("candidateOrdinal":0})"))) == "InvalidSessionId",
        "an all-zero session ID is refused");
    Check(
        ResultCode(harness.dispatcher.Handle(Envelope(
            "r-1", "start-candidate-live-view",
            R"({"sessionId":"0123456789abcdef0123456789abcdef",)"
            R"("candidateOrdinal":1.5})"))) == "InvalidCandidateOrdinal",
        "a fractional candidate ordinal is refused rather than rounded");
    Check(
        ResultCode(harness.dispatcher.Handle(Envelope(
            "r-1", "start-candidate-live-view",
            R"({"sessionId":"0123456789abcdef0123456789abcdef",)"
            R"("candidateOrdinal":-1})"))) == "InvalidCandidateOrdinal",
        "a negative candidate ordinal is refused");
}

// ---------------------------------------------------------------------------
// Cardinality
// ---------------------------------------------------------------------------

void EveryCandidateCountExceptTwoIsRefused() {
    for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
        DualBindingFakeSdkOptions options;
        options.candidate_count = count;
        Harness harness(options);
        const std::string response = harness.Begin();
        Check(
            !Succeeded(response) && ResultCode(response) == "CandidateCountNotTwo",
            "candidate count " + std::to_string(count) + " is refused, not truncated");
        Check(
            StringFieldOf(response, "sessionId").empty(),
            "a refused begin-binding hands back no session to address");
    }

    Harness two;
    const std::string response = two.Begin();
    Check(
        Succeeded(response) && ResultCode(response) == "BindingSessionStarted",
        "exactly two candidates starts a binding session");
    Check(
        StringFieldOf(response, "sessionId").size() == 32,
        "a started session is addressed by a 32 character session ID");
    Check(
        response.find("\"candidateOrdinals\":[0,1]") != std::string::npos,
        "both session-local candidate ordinals are offered to the operator");
}

void DuplicateAndMissingSourceObjectsAreRefused() {
    DualBindingFakeSdkOptions duplicate;
    duplicate.duplicate_source_object_tokens = true;
    Harness duplicate_harness(duplicate);
    Check(
        ResultCode(duplicate_harness.Begin()) == "DuplicateCandidateSourceObject",
        "two candidates sharing one source object are refused");

    DualBindingFakeSdkOptions empty;
    empty.empty_source_object_tokens = true;
    Harness empty_harness(empty);
    Check(
        ResultCode(empty_harness.Begin()) == "CandidateSourceObjectMissing",
        "a candidate with no source object is refused at binding time");
}

void AnAgentWithNoSdkFailsClosed() {
    DualBindingCameraAgentDispatcher dispatcher;
    const std::string response = dispatcher.Handle(BeginBindingRequest());
    Check(
        !Succeeded(response) && ResultCode(response) == "SdkUnavailable",
        "an agent with no SDK adapter refuses to invent a binding session");
    Check(
        dispatcher.BindingState() == DualIdentitySessionBindingState::None,
        "a refused begin-binding leaves no binding behind");
}

void SdkEnumerationFailuresKeepOnlySafeTypedCategories() {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"session_busy", "SdkSessionBusy"},
        {"sdk_load_failed", "SdkUnavailable"},
        {"licensed_adapter_unavailable", "SdkUnavailable"},
        {"camera_count_mismatch", "CandidateCountNotTwo"},
        {"identity_collision", "DuplicateCandidateSourceObject"},
        {"dual_inventory_failed", "SdkOperationFailed"},
        {R"(C:\private\sdk\Type0014.md3)", "SdkOperationFailed"},
    };
    for (const auto& [category, expected_code] : cases) {
        auto adapter = std::make_shared<FailingEnumerationAdapter>(category);
        DualBindingCameraAgentDispatcher dispatcher(adapter);
        const std::string response = dispatcher.Handle(BeginBindingRequest());
        Check(!Succeeded(response) && ResultCode(response) == expected_code,
            "SDK enumeration failure keeps its safe typed category");
        Check(response.find("machine-specific SDK detail") == std::string::npos,
            "SDK enumeration failure does not expose machine-specific detail");
        Check(response.find(category) == std::string::npos,
            "SDK enumeration failure does not expose its unrestricted internal category");
        Check(dispatcher.BindingState() == DualIdentitySessionBindingState::None,
            "SDK enumeration failure leaves no binding session");
        const auto counters = dispatcher.SafetyCounters();
        Check(counters.binding_session_count == 0 && counters.live_view_start_count == 0,
            "SDK enumeration failure publishes no candidate session or Live View");
    }
}

#ifdef A0_TEST_NIKON_SDK_STUB
void SdklessConcreteAdapterFailsClosedAsUnavailable() {
    auto adapter = std::make_shared<NikonDualBindingSdkAdapter>();
    DualBindingCameraAgentDispatcher dispatcher(adapter);
    const std::string response = dispatcher.Handle(BeginBindingRequest());
    Check(!Succeeded(response) && ResultCode(response) == "SdkUnavailable",
        "the SDK-less concrete adapter is reported as unavailable");
    Check(dispatcher.BindingState() == DualIdentitySessionBindingState::None,
        "the SDK-less concrete adapter creates no binding session");
    const auto counters = dispatcher.SafetyCounters();
    Check(counters.binding_session_count == 0 && counters.live_view_start_count == 0,
        "the SDK-less concrete adapter publishes no candidates or Live View");
}
#endif

// ---------------------------------------------------------------------------
// The five operations end to end
// ---------------------------------------------------------------------------

void FiveOperationsBindTwoBodies() {
    Harness harness;
    const std::string begin = harness.Begin();
    const std::string session = StringFieldOf(begin, "sessionId");

    const std::string started = harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
    Check(
        Succeeded(started) && ResultCode(started) == "CandidateLiveViewStarted",
        "Live View starts for the first candidate");

    const std::string frame = harness.dispatcher.Handle(FrameRequest(session, 0));
    Check(
        Succeeded(frame) && ResultCode(frame) == "CandidateLiveViewFrame",
        "a Live View frame is returned for the running candidate");
    Check(
        frame.find("\"frameBytes\":4096") != std::string::npos,
        "the frame response declares its own byte count");
    Check(
        !StringFieldOf(frame, "frameBase64").empty(),
        "the frame response carries the transient preview itself");

    const std::string confirmed_a =
        harness.dispatcher.Handle(ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA));
    Check(
        Succeeded(confirmed_a) && ResultCode(confirmed_a) == "CandidateAliasConfirmed",
        "the operator assigns the first candidate to CAM-A");
    Check(
        StringFieldOf(confirmed_a, "state") == "CollectingCandidates",
        "one assignment is not enough to leave candidate collection");

    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 1));
    (void)harness.dispatcher.Handle(FrameRequest(session, 1));
    const std::string confirmed_b =
        harness.dispatcher.Handle(ConfirmAliasRequest(session, 1, kDualIdentityCameraAliasB));
    Check(
        StringFieldOf(confirmed_b, "state") == "AwaitingQuiesce",
        "both assignments move the binding to AwaitingQuiesce");

    const std::string completed = harness.dispatcher.Handle(CompleteBindingRequest(session));
    Check(
        Succeeded(completed) && ResultCode(completed) == "BindingCompleted",
        "a fully assigned and quiesced binding completes");
    Check(
        StringFieldOf(completed, "state") == "Ready",
        "a completed binding reports Ready");
    Check(
        harness.adapter->ClosedCandidateSessionCount() == 2,
        "every candidate's SDK session was closed before the binding completed");
    Check(
        harness.adapter->ActiveLiveViewCount() == 0,
        "no Live View is left running by a completed binding");
}

void CompletedEvidenceIsExactlyTheAllowlist() {
    Harness harness;
    const std::string session = harness.BindBothAliases();
    const std::string completed = harness.dispatcher.Handle(CompleteBindingRequest(session));

    for (const std::string_view allowed : {
             "cameraAlias", "providerId", "providerVersion", "confirmedAtUtc",
             "invalidationReason"}) {
        Check(
            completed.find("\"" + std::string(allowed) + "\":") != std::string::npos,
            "published evidence carries " + std::string(allowed));
    }
    // The three things ADR-0025 excludes are excluded because publishing any of
    // them would re-create the belief that the app knows which physical body it
    // is talking to.
    Check(
        completed.find("fake-source-object") == std::string::npos,
        "no source object token appears anywhere in a completed-binding response");
    Check(
        completed.find("\"candidateOrdinal\"") == std::string::npos,
        "no candidate ordinal is published as evidence");
    Check(
        completed.find("\"frameBase64\"") == std::string::npos,
        "no preview frame is published as evidence");
    Check(
        completed.find(kDualIdentitySessionBindingProviderId) != std::string::npos,
        "evidence names the session-binding provider it came from");
}

void NoResponseEverCarriesASourceObject() {
    Harness harness;
    const std::string begin = harness.Begin();
    const std::string session = StringFieldOf(begin, "sessionId");
    const std::vector<std::string> responses{
        begin,
        harness.dispatcher.Handle(StartLiveViewRequest(session, 0)),
        harness.dispatcher.Handle(FrameRequest(session, 0)),
        harness.dispatcher.Handle(ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA)),
        harness.dispatcher.Handle(StartLiveViewRequest(session, 1)),
        harness.dispatcher.Handle(FrameRequest(session, 1)),
        harness.dispatcher.Handle(ConfirmAliasRequest(session, 1, kDualIdentityCameraAliasB)),
        harness.dispatcher.Handle(CompleteBindingRequest(session)),
    };
    for (const std::string& response : responses) {
        Check(
            response.find("fake-source-object") == std::string::npos,
            "no operation's response leaks the SDK source object token");
    }
}

void CaptureReusesBoundObjectsWithoutReEnumerating() {
    Harness harness;
    const std::string session = harness.BindBothAliases();
    (void)harness.dispatcher.Handle(CompleteBindingRequest(session));

    const std::string bound_a =
        harness.dispatcher.BoundSourceObjectForCapture(kDualIdentityCameraAliasA);
    const std::string bound_b =
        harness.dispatcher.BoundSourceObjectForCapture(kDualIdentityCameraAliasB);
    Check(!bound_a.empty() && !bound_b.empty(), "capture can reach both bound source objects");
    Check(bound_a != bound_b, "the two aliases are bound to different source objects");
    Check(
        harness.adapter->EnumerationCount() == 1,
        "reaching a bound source object does not re-enumerate the SDK");
}

// ---------------------------------------------------------------------------
// One Live View at a time
// ---------------------------------------------------------------------------

void OnlyOneLiveViewRunsAtATime() {
    Harness harness;
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");

    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
    Check(harness.adapter->ActiveLiveViewCount() == 1, "the first Live View is running");

    // Switching bodies is a normal part of comparing them, so this succeeds --
    // but only ever with one running.
    const std::string switched = harness.dispatcher.Handle(StartLiveViewRequest(session, 1));
    Check(Succeeded(switched), "the operator can switch to the other candidate");
    Check(
        harness.adapter->ActiveLiveViewCount() == 1,
        "switching candidates leaves exactly one Live View running");
    Check(
        harness.adapter->ConcurrentLiveViewViolationCount() == 0,
        "the SDK never saw two Live Views asked for at once");

    Check(
        ResultCode(harness.dispatcher.Handle(FrameRequest(session, 0))) == "LiveViewNotActive",
        "a frame for the candidate that is no longer streaming is refused");

    Check(
        ResultCode(harness.dispatcher.Handle(StartLiveViewRequest(session, 7))) ==
            "UnknownCandidateOrdinal",
        "an ordinal outside this session's candidates is refused");
}

void ReadyBindingRequiresExplicitFreshActivation() {
    auto adapter = std::make_shared<DualBindingFakeSdkAdapter>();
    DualBindingCameraAgentDispatcher dispatcher(adapter, true);
    const std::string transition_session =
        StringFieldOf(dispatcher.Handle(BeginBindingRequest()), "sessionId");
    (void)dispatcher.Handle(StartLiveViewRequest(transition_session, 0));
    (void)dispatcher.Handle(FrameRequest(transition_session, 0));
    (void)dispatcher.Handle(
        ConfirmAliasRequest(transition_session, 0, kDualIdentityCameraAliasA));
    (void)dispatcher.Handle(StartLiveViewRequest(transition_session, 1));
    (void)dispatcher.Handle(FrameRequest(transition_session, 1));
    (void)dispatcher.Handle(
        ConfirmAliasRequest(transition_session, 1, kDualIdentityCameraAliasB));
    Check(Succeeded(dispatcher.Handle(CompleteBindingRequest(transition_session))),
        "the operator-confirmed binding must reach Ready");
    Check(!dispatcher.ShouldStop(),
        "Ready alone must leave the freshness/cancellation pipe available");
    Check(ResultCode(dispatcher.Handle(CompleteBindingRequest(transition_session))) ==
            "BindingAlreadyComplete",
        "a Ready binding remains addressable for the read-only freshness probe");

    const std::string activated =
        dispatcher.Handle(ActivateCaptureRequest(transition_session));
    Check(Succeeded(activated) && ResultCode(activated) == "CaptureHostActivated",
        "only explicit capture activation retires the binding pipe");
    Check(dispatcher.CaptureTransitionRequested() && dispatcher.ShouldStop(),
        "successful activation requests the same process to enter its capture pipe");
}

void CancellationEndsActiveBindingExactlyOnce() {
    Harness harness;
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));

    const std::string cancelled = harness.dispatcher.Handle(CancelBindingRequest(session));
    Check(Succeeded(cancelled) && ResultCode(cancelled) == "BindingCancelled",
        "cancel-binding acknowledges only confirmed full cleanup");
    Check(cancelled.find("\"sdkSessionEnded\":true") != std::string::npos &&
          cancelled.find("\"liveViewStopped\":true") != std::string::npos,
        "the cancellation response confirms Live View and SDK teardown");
    Check(harness.adapter->ActiveLiveViewCount() == 0 &&
          harness.adapter->ClosedCandidateSessionCount() == 2 &&
          harness.adapter->EndBindingSessionCount() == 1,
        "active Live View and both candidate sessions end in one cleanup attempt");
    Check(harness.dispatcher.CancellationRequested() &&
          harness.dispatcher.CancellationSucceeded() &&
          harness.dispatcher.ShouldStop(),
        "the binding host exits after delivering a successful cancellation");
}

void CancellationStillRunsAfterInvalidation() {
    Harness harness;
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
    harness.adapter->RaiseInvalidation(DualIdentityInvalidationReason::UsbReconnect);
    Check(ResultCode(harness.dispatcher.Handle(FrameRequest(session, 0))) ==
            "BindingInvalidated",
        "test precondition invalidates the active binding");

    const std::string cancelled = harness.dispatcher.Handle(CancelBindingRequest(session));
    Check(Succeeded(cancelled) && harness.adapter->EndBindingSessionCount() == 1,
        "an invalid binding can still release the SDK session");
    Check(harness.adapter->ActiveLiveViewCount() == 0 && harness.dispatcher.ShouldStop(),
        "invalidation does not strand Live View after cancellation");
}

void CancellationFailureIsTerminalAndNeverRetried() {
    DualBindingFakeSdkOptions options;
    options.fail_end_binding_session = true;
    Harness harness(options);
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));

    const std::string failed = harness.dispatcher.Handle(CancelBindingRequest(session));
    Check(!Succeeded(failed) && ResultCode(failed) == "BindingCleanupFailed",
        "unconfirmed SDK teardown is never reported as cancellation success");
    Check(harness.adapter->EndBindingSessionCount() == 1,
        "cleanup failure is recorded after exactly one attempt");
    Check(harness.dispatcher.CancellationRequested() &&
          !harness.dispatcher.CancellationSucceeded() &&
          harness.dispatcher.ShouldStop(),
        "a failed cleanup response is terminal instead of leaving a ten-minute host");
}

void AStaleSessionCannotCancelTheCurrentBinding() {
    Harness harness;
    const std::string old_session = StringFieldOf(harness.Begin(), "sessionId");
    const std::string current_session = StringFieldOf(harness.Begin(), "sessionId");

    Check(ResultCode(harness.dispatcher.Handle(CancelBindingRequest(old_session))) ==
            "SessionMismatch",
        "a stale session cannot cancel a newer binding");
    Check(harness.adapter->EndBindingSessionCount() == 0 &&
          !harness.dispatcher.ShouldStop(),
        "stale cancellation touches no SDK state and does not stop the current host");
    Check(Succeeded(harness.dispatcher.Handle(CancelBindingRequest(current_session))),
        "the exact current session remains cancellable");
}

void BothCandidatesCanBeComparedBeforeEitherAliasIsConfirmed() {
    Harness harness;
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");

    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
    (void)harness.dispatcher.Handle(FrameRequest(session, 0));
    const std::string switched =
        harness.dispatcher.Handle(StartLiveViewRequest(session, 1));

    Check(Succeeded(switched), "the second candidate can be viewed before assigning the first");
    Check(
        harness.adapter->ClosedCandidateSessionCount() == 1,
        "switching candidates closes the first SDK source while retaining the session");
    (void)harness.dispatcher.Handle(FrameRequest(session, 1));

    const std::string stale_confirmation =
        harness.dispatcher.Handle(ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA));
    Check(
        !Succeeded(stale_confirmation) && ResultCode(stale_confirmation) == "LiveViewFrameRequired",
        "a preview from before switching candidates cannot authorize an alias confirmation");

    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
    (void)harness.dispatcher.Handle(FrameRequest(session, 0));
    const std::string confirmed_a =
        harness.dispatcher.Handle(ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA));
    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 1));
    (void)harness.dispatcher.Handle(FrameRequest(session, 1));
    const std::string confirmed_b =
        harness.dispatcher.Handle(ConfirmAliasRequest(session, 1, kDualIdentityCameraAliasB));
    const std::string completed = harness.dispatcher.Handle(CompleteBindingRequest(session));

    Check(
        Succeeded(confirmed_a) && Succeeded(confirmed_b),
        "both aliases can be confirmed after each current preview is shown again");
    Check(
        Succeeded(completed) && ResultCode(completed) == "BindingCompleted",
        "the compare-before-assign flow reaches a fully quiesced binding");
}

void AFailedStopPreventsASecondLiveView() {
    // The body that is already streaming stops responding to stop requests.
    // Starting the other one anyway would leave two running.
    DualBindingFakeSdkOptions options;
    options.fail_stop_live_view = true;
    Harness harness(options);
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));

    const std::string refused = harness.dispatcher.Handle(StartLiveViewRequest(session, 1));
    Check(
        !Succeeded(refused) && ResultCode(refused) == "LiveViewStopFailed",
        "a Live View that will not stop blocks the next one from starting");
    Check(
        harness.adapter->ActiveLiveViewCount() == 1,
        "the refused switch left the original Live View running and no second one");
    Check(
        harness.dispatcher.BindingState() == DualIdentitySessionBindingState::Invalid,
        "an SDK that will not stop Live View invalidates the binding");
}

void AFailedSourceClosePreventsASecondLiveView() {
    DualBindingFakeSdkOptions options;
    options.fail_close_candidate_session = true;
    Harness harness(options);
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));

    const std::string refused = harness.dispatcher.Handle(StartLiveViewRequest(session, 1));
    Check(
        !Succeeded(refused) && ResultCode(refused) == "SdkSessionCloseFailed",
        "a candidate Source that will not close blocks the next Live View");
    Check(
        harness.adapter->ActiveLiveViewCount() == 0,
        "the failed Source close does not start a second Live View");
    Check(
        harness.dispatcher.BindingState() == DualIdentitySessionBindingState::Invalid,
        "an unclosed candidate Source invalidates the binding");
}

void ARefusedLiveViewStartIsNotRecordedAsRunning() {
    DualBindingFakeSdkOptions options;
    options.fail_start_live_view = true;
    Harness harness(options);
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");

    const std::string refused = harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
    Check(
        !Succeeded(refused) && ResultCode(refused) == "LiveViewStartFailed",
        "an SDK refusal to start Live View is reported, not retried");
    Check(
        ResultCode(harness.dispatcher.Handle(FrameRequest(session, 0))) == "LiveViewNotActive",
        "a Live View that never started cannot produce a frame");
}

// ---------------------------------------------------------------------------
// Bounded frames
// ---------------------------------------------------------------------------

void FramesAreBoundedAndNeverTruncated() {
    Harness missing_harness;
    const std::string missing_session =
        StringFieldOf(missing_harness.Begin(), "sessionId");
    (void)missing_harness.dispatcher.Handle(StartLiveViewRequest(missing_session, 0));
    Check(
        ResultCode(missing_harness.dispatcher.Handle(
            ConfirmAliasRequest(missing_session, 0, kDualIdentityCameraAliasA))) ==
            "LiveViewFrameRequired",
        "alias confirmation is refused before a current frame is returned");

    DualBindingFakeSdkOptions oversize;
    oversize.live_view_frame_bytes = kMaximumBindingLiveViewFrameBytes + 1U;
    Harness oversize_harness(oversize);
    const std::string oversize_session =
        StringFieldOf(oversize_harness.Begin(), "sessionId");
    (void)oversize_harness.dispatcher.Handle(StartLiveViewRequest(oversize_session, 0));
    const std::string refused =
        oversize_harness.dispatcher.Handle(FrameRequest(oversize_session, 0));
    Check(
        !Succeeded(refused) && ResultCode(refused) == "LiveViewFrameTooLarge",
        "a frame above the bound is refused");
    Check(
        refused.find("\"frameBase64\"") == std::string::npos,
        "a refused frame carries no partial preview");
    Check(
        ResultCode(oversize_harness.dispatcher.Handle(
            ConfirmAliasRequest(oversize_session, 0, kDualIdentityCameraAliasA))) ==
            "LiveViewFrameRequired",
        "an oversized refused frame cannot authorize alias confirmation");

    DualBindingFakeSdkOptions at_bound;
    at_bound.live_view_frame_bytes = kMaximumBindingLiveViewFrameBytes;
    Harness bound_harness(at_bound);
    const std::string bound_session = StringFieldOf(bound_harness.Begin(), "sessionId");
    (void)bound_harness.dispatcher.Handle(StartLiveViewRequest(bound_session, 0));
    const std::string accepted = bound_harness.dispatcher.Handle(FrameRequest(bound_session, 0));
    Check(Succeeded(accepted), "a frame exactly at the bound is accepted");
    // Base64 of the maximum frame plus the envelope has to stay inside the
    // 1 MiB pipe frame, or the bound would be decorative.
    Check(
        accepted.size() < 1024U * 1024U,
        "the largest allowed frame response still fits one pipe frame");
    Check(
        ResultCode(bound_harness.dispatcher.Handle(FrameRequest(bound_session, 1))) ==
            "LiveViewNotActive",
        "a failed refresh for another candidate is refused");
    Check(
        ResultCode(bound_harness.dispatcher.Handle(
            ConfirmAliasRequest(bound_session, 0, kDualIdentityCameraAliasA))) ==
            "LiveViewFrameRequired",
        "a failed refresh clears an older successful preview before confirmation");

    DualBindingFakeSdkOptions empty;
    empty.live_view_frame_bytes = 0;
    Harness empty_harness(empty);
    const std::string empty_session = StringFieldOf(empty_harness.Begin(), "sessionId");
    (void)empty_harness.dispatcher.Handle(StartLiveViewRequest(empty_session, 0));
    Check(
        ResultCode(empty_harness.dispatcher.Handle(FrameRequest(empty_session, 0))) ==
            "LiveViewFrameUnavailable",
        "an empty frame is reported as unavailable rather than as a preview");
    Check(
        ResultCode(empty_harness.dispatcher.Handle(
            ConfirmAliasRequest(empty_session, 0, kDualIdentityCameraAliasA))) ==
            "LiveViewFrameRequired",
        "an empty refused frame cannot authorize alias confirmation");
}

// The preview is the only binary payload this protocol carries, and the whole
// operator decision rests on seeing it correctly. Asserting the frame is
// "non-empty" would pass for an encoder that emits the wrong alphabet, drops the
// padding, or mangles the tail -- all of which produce an image the operator
// cannot read, from an agent reporting success. So the bytes are pinned against
// known values instead, including both partial-tail cases.
void ThePreviewEncodingIsPinnedToKnownBytes() {
    // The fake fills byte i of candidate 0's frame with i, so the expected
    // base64 is the standard encoding of 00 01 02 ... for each length.
    const std::pair<std::size_t, std::string_view> vectors[]{
        {3, "AAEC"},        // exact multiple of three, no padding
        {4, "AAECAw=="},    // one byte over, two padding characters
        {5, "AAECAwQ="},    // two bytes over, one padding character
    };
    for (const auto& [length, expected] : vectors) {
        DualBindingFakeSdkOptions options;
        options.live_view_frame_bytes = length;
        Harness harness(options);
        const std::string session = StringFieldOf(harness.Begin(), "sessionId");
        (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
        const std::string frame = harness.dispatcher.Handle(FrameRequest(session, 0));

        Check(
            StringFieldOf(frame, "frameBase64") == expected,
            "a " + std::to_string(length) + " byte preview encodes to its known base64");
        Check(
            frame.find("\"frameBytes\":" + std::to_string(length)) != std::string::npos,
            "the declared byte count is the raw length, not the encoded one");
    }
}

void SessionIdsAreShapedTheWayClientsValidateThem() {
    Harness harness;
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    Check(session.size() == 32, "a session ID is 32 characters");
    Check(
        session.find_first_not_of("0123456789abcdef") == std::string::npos,
        "a session ID is lowercase hex, which is what the parser accepts back");
    Check(
        session.find_first_not_of('0') != std::string::npos,
        "a session ID is never the all-zero value the parser refuses");

    // Handing the agent back its own session ID has to work, or the shape the
    // agent emits and the shape it accepts would have drifted apart.
    Check(
        Succeeded(harness.dispatcher.Handle(StartLiveViewRequest(session, 0))),
        "the agent accepts the session ID it issued");
}

// ---------------------------------------------------------------------------
// Alias assignment
// ---------------------------------------------------------------------------

void AnAliasAndACandidateAreEachAssignedOnce() {
    Harness harness;
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
    (void)harness.dispatcher.Handle(FrameRequest(session, 0));
    (void)harness.dispatcher.Handle(ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA));

    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 1));
    (void)harness.dispatcher.Handle(FrameRequest(session, 1));

    Check(
        ResultCode(harness.dispatcher.Handle(
            ConfirmAliasRequest(session, 1, kDualIdentityCameraAliasA))) ==
            "AliasAlreadyAssigned",
        "an operator who assigns CAM-A twice is told, not silently overridden");
    Check(
        ResultCode(harness.dispatcher.Handle(
            ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasB))) ==
            "CandidateAlreadyAssigned",
        "one candidate cannot be assigned to both aliases");
    Check(
        ResultCode(harness.dispatcher.Handle(ConfirmAliasRequest(session, 1, "CAM-C"))) ==
            "UnknownCameraAlias",
        "an alias outside the two the rig defines is refused");
    Check(
        ResultCode(harness.dispatcher.Handle(ConfirmAliasRequest(session, 1, ""))) ==
            "UnknownCameraAlias",
        "an empty alias is refused");
    Check(
        ResultCode(harness.dispatcher.Handle(
            ConfirmAliasRequest(session, 9, kDualIdentityCameraAliasB))) ==
            "UnknownCandidateOrdinal",
        "an alias cannot be assigned to a candidate this session does not have");
}

void CompletionNeedsBothAliasesAndBothQuiesced() {
    Harness harness;
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    Check(
        ResultCode(harness.dispatcher.Handle(CompleteBindingRequest(session))) ==
            "AliasAssignmentIncomplete",
        "completing with no assignment at all is refused");

    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
    (void)harness.dispatcher.Handle(FrameRequest(session, 0));
    (void)harness.dispatcher.Handle(ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA));
    Check(
        ResultCode(harness.dispatcher.Handle(CompleteBindingRequest(session))) ==
            "AliasAssignmentIncomplete",
        "completing with one alias missing is refused");

    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 1));
    (void)harness.dispatcher.Handle(FrameRequest(session, 1));
    (void)harness.dispatcher.Handle(ConfirmAliasRequest(session, 1, kDualIdentityCameraAliasB));
    Check(
        Succeeded(harness.dispatcher.Handle(CompleteBindingRequest(session))),
        "completing with both aliases assigned and both candidates quiesced succeeds");
    Check(
        ResultCode(harness.dispatcher.Handle(CompleteBindingRequest(session))) ==
            "BindingAlreadyComplete",
        "completing an already complete binding is refused rather than repeated");
}

// An SDK session that will not close is the case where everything else about
// the session stays healthy: both bodies can still be viewed and assigned, so
// the only thing standing between this binding and Ready is the quiesce the
// binding core was told about. That makes this the test that proves the core is
// the authority -- an agent that reported an optimistic "quiesced" here would
// complete the binding and let capture start against a session that is still
// open.
void AnUnclosableSdkSessionBlocksCompletion() {
    DualBindingFakeSdkOptions options;
    options.fail_close_candidate_session = true;
    Harness harness(options);
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
    (void)harness.dispatcher.Handle(FrameRequest(session, 0));

    const std::string confirmed =
        harness.dispatcher.Handle(ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA));
    Check(
        !Succeeded(confirmed) && ResultCode(confirmed) == "QuiesceIncomplete",
        "an SDK session that will not close is reported at confirm time");
    Check(
        confirmed.find("\"sdkSessionClosed\":false") != std::string::npos &&
            confirmed.find("\"liveViewStopped\":true") != std::string::npos,
        "the response says which half of the quiesce failed");

    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 1));
    (void)harness.dispatcher.Handle(FrameRequest(session, 1));
    (void)harness.dispatcher.Handle(ConfirmAliasRequest(session, 1, kDualIdentityCameraAliasB));
    Check(
        ResultCode(harness.dispatcher.Handle(CompleteBindingRequest(session))) ==
            "CandidateNotQuiesced",
        "an unclosed SDK session blocks completion even with both aliases assigned");
    Check(
        harness.dispatcher.BindingState() != DualIdentitySessionBindingState::Ready,
        "a binding with a body that may still be open never reaches Ready");
}

// A Live View that will not stop is the harsher case: the session cannot even
// get as far as assigning the second body, because switching to it would need
// the first one stopped.
void AnUnstoppableLiveViewEndsTheSession() {
    DualBindingFakeSdkOptions options;
    options.fail_stop_live_view = true;
    Harness harness(options);
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
    (void)harness.dispatcher.Handle(FrameRequest(session, 0));

    const std::string confirmed =
        harness.dispatcher.Handle(ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA));
    Check(
        !Succeeded(confirmed) && ResultCode(confirmed) == "QuiesceIncomplete",
        "a Live View that will not stop is reported at confirm time");
    Check(
        confirmed.find("\"liveViewStopped\":false") != std::string::npos,
        "the response says the Live View is the half that failed");

    Check(
        ResultCode(harness.dispatcher.Handle(StartLiveViewRequest(session, 1))) ==
            "LiveViewStopFailed",
        "the second body cannot be viewed while the first will not stop streaming");
    Check(
        ResultCode(harness.dispatcher.Handle(CompleteBindingRequest(session))) ==
            "BindingInvalidated",
        "a session that cannot guarantee one Live View at a time cannot complete");
}

void AnAssignedCandidateCannotBeViewedAgain() {
    Harness harness;
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    (void)harness.dispatcher.Handle(StartLiveViewRequest(session, 0));
    (void)harness.dispatcher.Handle(FrameRequest(session, 0));
    (void)harness.dispatcher.Handle(ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA));

    // Assigning the candidate closed its SDK session. Reopening Live View would
    // undo the quiesce the completed binding is about to rest on.
    Check(
        ResultCode(harness.dispatcher.Handle(StartLiveViewRequest(session, 0))) ==
            "CandidateAlreadyAssigned",
        "an already assigned candidate cannot have its Live View reopened");
    Check(
        harness.adapter->ActiveLiveViewCount() == 0,
        "the refused request left no Live View running");
}

// ---------------------------------------------------------------------------
// Session correlation and invalidation
// ---------------------------------------------------------------------------

void AnOldSessionIsRefusedAfterRebinding() {
    Harness harness;
    const std::string first = StringFieldOf(harness.Begin(), "sessionId");
    const std::string second = StringFieldOf(harness.Begin(), "sessionId");
    Check(first != second, "re-binding produces a session ID the old one cannot be mistaken for");

    const std::string stale = harness.dispatcher.Handle(StartLiveViewRequest(first, 0));
    Check(
        !Succeeded(stale) && ResultCode(stale) == "SessionMismatch",
        "the superseded session is refused");
    Check(
        Succeeded(harness.dispatcher.Handle(StartLiveViewRequest(second, 0))),
        "the current session still works");
    Check(
        harness.dispatcher.SafetyCounters().rejected_stale_session_count == 1,
        "the refused stale request is counted");
}

void ARestartedAgentHoldsNoSession() {
    Harness first;
    const std::string session = StringFieldOf(first.Begin(), "sessionId");

    // A restart is a new dispatcher: the binding lived in the memory of the
    // process that produced the candidates, so a restarted agent has nothing to
    // address, whatever session ID the client still remembers.
    Harness restarted;
    for (const std::string& request : {
             StartLiveViewRequest(session, 0),
             FrameRequest(session, 0),
             ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA),
             CompleteBindingRequest(session)}) {
        const std::string response = restarted.dispatcher.Handle(request);
        Check(
            !Succeeded(response) && ResultCode(response) == "SessionMismatch",
            "a restarted agent refuses every operation naming the pre-restart session");
    }
    Check(
        restarted.dispatcher.SafetyCounters().rejected_stale_session_count == 4,
        "every refused pre-restart request is counted");
}

void EveryTypedInvalidationStopsTheSession() {
    const std::pair<DualIdentityInvalidationReason, std::string_view> reasons[]{
        {DualIdentityInvalidationReason::AgentRestart, "AgentRestart"},
        {DualIdentityInvalidationReason::UsbReconnect, "UsbReconnect"},
        {DualIdentityInvalidationReason::CameraCountChanged, "CameraCountChanged"},
        {DualIdentityInvalidationReason::TopologyChanged, "TopologyChanged"},
        {DualIdentityInvalidationReason::SdkManagerRecreated, "SdkManagerRecreated"},
        {DualIdentityInvalidationReason::SdkError, "SdkError"},
    };
    for (const auto& [reason, name] : reasons) {
        Harness harness;
        const std::string session = harness.BindBothAliases();
        harness.adapter->RaiseInvalidation(reason);

        const std::string response = harness.dispatcher.Handle(CompleteBindingRequest(session));
        Check(
            !Succeeded(response) && ResultCode(response) == "BindingInvalidated",
            std::string(name) + " stops the session from completing");
        Check(
            StringFieldOf(response, "invalidationReason") == name,
            std::string(name) + " is reported back by name, not as a generic failure");
        Check(
            StringFieldOf(response, "state") == "Invalid",
            std::string(name) + " leaves the binding in Invalid");
    }
}

void InvalidationDuringCandidateCollectionRefusesTheNewSession() {
    DualBindingFakeSdkOptions options;
    options.invalidation_during_enumeration =
        DualIdentityInvalidationReason::TopologyChanged;
    Harness harness(options);

    const std::string response = harness.Begin();
    Check(
        !Succeeded(response) && ResultCode(response) == "BindingInvalidated",
        "an event observed while candidates were being collected refuses the session");
    Check(
        StringFieldOf(response, "sessionId").empty(),
        "the refused session is never handed to the caller to address");

    // The event has been consumed, so the operator's retry starts clean. If it
    // had not been, begin-binding would be unable to ever succeed again.
    Check(
        Succeeded(harness.Begin()),
        "the retry after a collection-time invalidation starts a usable session");
}

void AnEventFromTheDiscardedSessionDoesNotKillTheNewOne() {
    // begin-binding drains whatever the adapter was still holding, because that
    // event describes the session it just threw away. Acting on it would refuse
    // the replacement session for something that happened before it existed --
    // and since the drain is what clears it, the operator would be stuck in a
    // loop of begin-binding failing for a body they already unplugged.
    DualBindingFakeSdkOptions options;
    options.pending_invalidation = DualIdentityInvalidationReason::UsbReconnect;
    Harness harness(options);

    const std::string response = harness.Begin();
    Check(
        Succeeded(response) && ResultCode(response) == "BindingSessionStarted",
        "an event queued before begin-binding does not refuse the new session");
    Check(
        harness.dispatcher.BindingState() ==
            DualIdentitySessionBindingState::CollectingCandidates,
        "the new session is usable after the stale event was drained");
}

void AnInvalidatedSessionRefusesEveryOperation() {
    Harness harness;
    const std::string session = StringFieldOf(harness.Begin(), "sessionId");
    harness.adapter->RaiseInvalidation(DualIdentityInvalidationReason::UsbReconnect);
    // The first request consumes the event; the rest see an already invalid
    // binding. Both paths have to refuse.
    for (const std::string& request : {
             StartLiveViewRequest(session, 0),
             FrameRequest(session, 0),
             ConfirmAliasRequest(session, 0, kDualIdentityCameraAliasA),
             CompleteBindingRequest(session)}) {
        const std::string response = harness.dispatcher.Handle(request);
        Check(
            !Succeeded(response) && ResultCode(response) == "BindingInvalidated",
            "an invalidated session refuses every operation");
        Check(
            StringFieldOf(response, "invalidationReason") == "UsbReconnect",
            "the reason reported stays the one that broke the binding");
    }
    Check(
        harness.dispatcher.BindingState() == DualIdentitySessionBindingState::Invalid,
        "the invalidated binding does not recover on its own");
}

void AStaleSessionCannotConsumeTheCurrentSessionsEvent() {
    Harness harness;
    const std::string first = StringFieldOf(harness.Begin(), "sessionId");
    const std::string second = StringFieldOf(harness.Begin(), "sessionId");
    harness.adapter->RaiseInvalidation(DualIdentityInvalidationReason::SdkError);

    // A request naming the old session is refused before the adapter is polled.
    // Otherwise it would swallow the event and the live session's next request
    // would see a clean adapter and carry on with a binding that is no longer
    // trustworthy.
    Check(
        ResultCode(harness.dispatcher.Handle(StartLiveViewRequest(first, 0))) ==
            "SessionMismatch",
        "the stale request is refused on identity alone");
    const std::string current = harness.dispatcher.Handle(StartLiveViewRequest(second, 0));
    Check(
        ResultCode(current) == "BindingInvalidated" &&
            StringFieldOf(current, "invalidationReason") == "SdkError",
        "the live session still learns about the event the stale request did not consume");
}

void CountersReflectWhatActuallyHappened() {
    Harness harness;
    const std::string session = harness.BindBothAliases();
    (void)harness.dispatcher.Handle(FrameRequest(session, 1));

    const auto counters = harness.dispatcher.SafetyCounters();
    Check(counters.binding_session_count == 1, "one binding session was started");
    Check(counters.live_view_start_count == 2, "one Live View start per candidate");
    Check(counters.rejected_stale_session_count == 0, "no stale session was addressed");
    // The frame after both aliases are confirmed has no Live View behind it, so
    // it is refused and must not be counted as a frame that was handed out.
    Check(counters.live_view_frame_count == 2,
        "only the two successful current preview frames are counted");
}

} // namespace

int main() {
    EnvelopeMustBeTheBindingProtocol();
    SessionIdShapeIsEnforcedBeforeItIsCompared();
    EveryCandidateCountExceptTwoIsRefused();
    DuplicateAndMissingSourceObjectsAreRefused();
    AnAgentWithNoSdkFailsClosed();
    SdkEnumerationFailuresKeepOnlySafeTypedCategories();
#ifdef A0_TEST_NIKON_SDK_STUB
    SdklessConcreteAdapterFailsClosedAsUnavailable();
#endif
    FiveOperationsBindTwoBodies();
    CompletedEvidenceIsExactlyTheAllowlist();
    ReadyBindingRequiresExplicitFreshActivation();
    CancellationEndsActiveBindingExactlyOnce();
    CancellationStillRunsAfterInvalidation();
    CancellationFailureIsTerminalAndNeverRetried();
    AStaleSessionCannotCancelTheCurrentBinding();
    NoResponseEverCarriesASourceObject();
    CaptureReusesBoundObjectsWithoutReEnumerating();
    OnlyOneLiveViewRunsAtATime();
    BothCandidatesCanBeComparedBeforeEitherAliasIsConfirmed();
    AFailedStopPreventsASecondLiveView();
    AFailedSourceClosePreventsASecondLiveView();
    ARefusedLiveViewStartIsNotRecordedAsRunning();
    FramesAreBoundedAndNeverTruncated();
    ThePreviewEncodingIsPinnedToKnownBytes();
    SessionIdsAreShapedTheWayClientsValidateThem();
    AnAliasAndACandidateAreEachAssignedOnce();
    CompletionNeedsBothAliasesAndBothQuiesced();
    AnUnclosableSdkSessionBlocksCompletion();
    AnUnstoppableLiveViewEndsTheSession();
    AnAssignedCandidateCannotBeViewedAgain();
    AnOldSessionIsRefusedAfterRebinding();
    ARestartedAgentHoldsNoSession();
    EveryTypedInvalidationStopsTheSession();
    InvalidationDuringCandidateCollectionRefusesTheNewSession();
    AnEventFromTheDiscardedSessionDoesNotKillTheNewOne();
    AnInvalidatedSessionRefusesEveryOperation();
    AStaleSessionCannotConsumeTheCurrentSessionsEvent();
    CountersReflectWhatActuallyHappened();

    if (failures != 0) {
        std::cerr << failures << " dual binding Camera Agent contract failures\n";
        return 1;
    }
    std::cout << "dual binding Camera Agent contracts passed\n";
    return 0;
}
