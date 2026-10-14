#include "a0/phase0/dual_binding_camera_agent.hpp"
#include "a0/phase0/dual_bound_live_view.hpp"
#include "a0/phase0/dual_hardware_camera_agent.hpp"
#include "a0/phase0/dual_identity_session_binding.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Contract tests for the Ready-time bound Live View
// (`a0.camera-agent.hardware-dual-bound-live-view.v1`, Issue #291), served on the
// binding pipe. Everything runs against the fake SDK adapter with injected
// clocks, so every stop and close in these tests is decided by the test and
// observed on the adapter, not assumed.

namespace {

using namespace a0::phase0;

int failures = 0;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr std::string_view kBindingMarker = "HardwareBinding";
constexpr std::string_view kCaptureMarker = "Hardware";
constexpr std::uint64_t kNoNumber = std::numeric_limits<std::uint64_t>::max();

// ---------------------------------------------------------------------------
// Request / response helpers
// ---------------------------------------------------------------------------

std::string BindingEnvelope(
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

std::string LiveEnvelope(
    std::string_view request_id,
    std::string_view marker,
    std::string_view operation,
    std::string_view payload,
    bool simulation = false) {
    std::string json = R"({"schemaVersion":")";
    json += kDualBoundLiveViewSchemaVersion;
    json += R"(","simulation":)";
    json += simulation ? "true" : "false";
    json += R"(,"marker":")";
    json += marker;
    json += R"(","requestId":")";
    json += request_id;
    json += R"(","operation":")";
    json += operation;
    json += R"(","payload":)";
    json += payload;
    json += "}";
    return json;
}

std::string SessionAliasPayload(std::string_view session, std::string_view alias) {
    return R"({"sessionId":")" + std::string(session) + R"(","cameraAlias":")" +
        std::string(alias) + "\"}";
}

std::string StartRequest(
    std::string_view session, std::string_view alias,
    std::string_view marker = kBindingMarker) {
    return LiveEnvelope(
        "r-live-start", marker, "start-bound-live-view",
        SessionAliasPayload(session, alias));
}

std::string FrameRequest(
    std::string_view session, std::string_view alias,
    std::string_view marker = kBindingMarker) {
    return LiveEnvelope(
        "r-live-frame", marker, "get-bound-live-view-frame",
        SessionAliasPayload(session, alias));
}

std::string StopRequest(
    std::string_view session, std::string_view marker = kBindingMarker) {
    return LiveEnvelope(
        "r-live-stop", marker, "stop-bound-live-view",
        R"({"sessionId":")" + std::string(session) + "\"}");
}

std::string BeginBindingRequest() {
    return BindingEnvelope("r-begin", "begin-binding", R"({"cameraMode":"DualCamera"})");
}

std::string CandidateStartRequest(std::string_view session, std::size_t ordinal) {
    return BindingEnvelope(
        "r-start", "start-candidate-live-view",
        R"({"sessionId":")" + std::string(session) + R"(","candidateOrdinal":)" +
            std::to_string(ordinal) + "}");
}

std::string CandidateFrameRequest(std::string_view session, std::size_t ordinal) {
    return BindingEnvelope(
        "r-frame", "get-candidate-live-view-frame",
        R"({"sessionId":")" + std::string(session) + R"(","candidateOrdinal":)" +
            std::to_string(ordinal) + "}");
}

std::string ConfirmAliasRequest(
    std::string_view session, std::size_t ordinal, std::string_view alias) {
    return BindingEnvelope(
        "r-confirm", "confirm-alias",
        R"({"sessionId":")" + std::string(session) + R"(","candidateOrdinal":)" +
            std::to_string(ordinal) + R"(,"cameraAlias":")" + std::string(alias) + "\"}");
}

std::string CompleteBindingRequest(std::string_view session) {
    return BindingEnvelope(
        "r-complete", "complete-binding",
        R"({"sessionId":")" + std::string(session) +
            R"(","confirmedAtUtc":"2026-10-14T00:00:00Z"})");
}

std::string ActivateCaptureRequest(std::string_view session) {
    return BindingEnvelope(
        "r-activate", "activate-capture", R"({"sessionId":")" + std::string(session) + "\"}");
}

std::string CancelBindingRequest(std::string_view session) {
    return BindingEnvelope(
        "r-cancel", "cancel-binding", R"({"sessionId":")" + std::string(session) + "\"}");
}

// Plain substring reads, not a JSON parse: these tests pin the exact bytes put on
// the wire (see the same note in dual_binding_camera_agent_tests.cpp).
std::string StringFieldOf(std::string_view response, std::string_view field) {
    const std::string key = "\"" + std::string(field) + "\":\"";
    const std::size_t start = response.find(key);
    if (start == std::string_view::npos) return {};
    const std::size_t value_start = start + key.size();
    const std::size_t end = response.find('"', value_start);
    if (end == std::string_view::npos) return {};
    return std::string(response.substr(value_start, end - value_start));
}

std::uint64_t NumberFieldOf(std::string_view response, std::string_view field) {
    const std::string key = "\"" + std::string(field) + "\":";
    const std::size_t start = response.find(key);
    if (start == std::string_view::npos) return kNoNumber;
    std::size_t position = start + key.size();
    std::uint64_t value = 0;
    bool any = false;
    while (position < response.size() &&
           std::isdigit(static_cast<unsigned char>(response[position])) != 0) {
        value = value * 10U + static_cast<std::uint64_t>(response[position] - '0');
        any = true;
        ++position;
    }
    return any ? value : kNoNumber;
}

bool BoolFieldIs(std::string_view response, std::string_view field, bool expected) {
    const std::string key =
        "\"" + std::string(field) + "\":" + (expected ? "true" : "false");
    return response.find(key) != std::string_view::npos;
}

bool Succeeded(std::string_view response) {
    return response.find("\"success\":true") != std::string_view::npos;
}

std::string ResultCode(std::string_view response) {
    return StringFieldOf(response, "resultCode");
}

std::string ReplaceAll(std::string text, std::string_view from, std::string_view to) {
    if (from.empty()) return text;
    std::size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

struct Fixture {
    std::shared_ptr<DualBindingFakeSdkAdapter> adapter;
    DualBindingCameraAgentDispatcher dispatcher;
    std::uint64_t host_remaining{600'000};
    std::uint64_t now{1'000};
    bool host_remaining_throws{};
    std::unique_ptr<DualBoundLiveViewController> live;
    std::string session;

    explicit Fixture(DualBindingFakeSdkOptions options = {}, bool attach = true)
        : adapter(std::make_shared<DualBindingFakeSdkAdapter>(std::move(options))),
          dispatcher(adapter, true) {
        if (!attach) return;
        DualBoundLiveViewOptions live_options;
        live_options.host_remaining_milliseconds = [this]() -> std::uint64_t {
            if (host_remaining_throws) throw std::runtime_error("injected clock failure");
            return host_remaining;
        };
        live_options.monotonic_milliseconds = [this] { return now; };
        live = std::make_unique<DualBoundLiveViewController>(
            *adapter, std::move(live_options));
        dispatcher.AttachBoundLiveView(live.get());
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::string Handle(const std::string& request) { return dispatcher.Handle(request); }

    // Drives begin -> assign -> quiesce -> complete. `cam_a_ordinal` is which
    // session-local candidate the operator calls CAM-A, so the tests can tell an
    // alias lookup from an accidental "alias A is candidate 0".
    std::vector<std::string> BindReady(std::size_t cam_a_ordinal = 0) {
        std::vector<std::string> responses;
        responses.push_back(Handle(BeginBindingRequest()));
        session = StringFieldOf(responses.back(), "sessionId");
        const std::size_t cam_b_ordinal = 1 - cam_a_ordinal;
        responses.push_back(Handle(CandidateStartRequest(session, cam_a_ordinal)));
        responses.push_back(Handle(CandidateFrameRequest(session, cam_a_ordinal)));
        responses.push_back(Handle(
            ConfirmAliasRequest(session, cam_a_ordinal, kDualIdentityCameraAliasA)));
        responses.push_back(Handle(CandidateStartRequest(session, cam_b_ordinal)));
        responses.push_back(Handle(CandidateFrameRequest(session, cam_b_ordinal)));
        responses.push_back(Handle(
            ConfirmAliasRequest(session, cam_b_ordinal, kDualIdentityCameraAliasB)));
        responses.push_back(Handle(CompleteBindingRequest(session)));
        return responses;
    }

    [[nodiscard]] std::size_t CallMark() const { return adapter->CallLog().size(); }

    [[nodiscard]] std::vector<std::string> CallsSince(std::size_t mark) const {
        const auto& log = adapter->CallLog();
        return std::vector<std::string>(
            log.begin() + static_cast<std::ptrdiff_t>(std::min(mark, log.size())),
            log.end());
    }

    [[nodiscard]] bool NoOverlapEver() const {
        return adapter->ConcurrentLiveViewViolationCount() == 0 &&
            adapter->ConcurrentSourceViolationCount() == 0;
    }
};

using Calls = std::vector<std::string>;

// ---------------------------------------------------------------------------
// Binding core: the ordinal seam
// ---------------------------------------------------------------------------

void TheCoreHandsOutCandidateOrdinalsOnlyWhileReady() {
    DualIdentitySessionBinding binding;
    bool refused_before_start = false;
    try { (void)binding.BoundCandidateOrdinalForLiveView("CAM-A"); }
    catch (const DualIdentitySessionBindingError& error) {
        refused_before_start = error.Code() == "BindingNotReady";
    }
    Check(refused_before_start, "no ordinal is handed out before a binding exists");

    binding.BeginBinding({{0, "token-0"}, {1, "token-1"}});
    binding.ConfirmAlias(1, "CAM-A");
    binding.ConfirmAlias(0, "CAM-B");
    bool refused_while_collecting = false;
    try { (void)binding.BoundCandidateOrdinalForLiveView("CAM-A"); }
    catch (const DualIdentitySessionBindingError& error) {
        refused_while_collecting = error.Code() == "BindingNotReady";
    }
    Check(refused_while_collecting, "no ordinal is handed out before the binding is Ready");

    binding.ConfirmCandidateQuiesced(0, true, true);
    binding.ConfirmCandidateQuiesced(1, true, true);
    binding.CompleteBinding("2026-10-14T00:00:00Z");
    Check(binding.BoundCandidateOrdinalForLiveView("CAM-A") == 1,
        "CAM-A resolves to the candidate the operator assigned it to");
    Check(binding.BoundCandidateOrdinalForLiveView("CAM-B") == 0,
        "CAM-B resolves to the candidate the operator assigned it to");
    Check(binding.SafetyCounters().source_object_reuse_count == 0,
        "reading an ordinal for Live View is not capture reusing a source object");

    bool unknown_refused = false;
    try { (void)binding.BoundCandidateOrdinalForLiveView("CAM-C"); }
    catch (const DualIdentitySessionBindingError& error) {
        unknown_refused = error.Code() == "UnknownCameraAlias";
    }
    Check(unknown_refused, "an alias the binding does not know has no ordinal");

    binding.Invalidate(DualIdentityInvalidationReason::UsbReconnect);
    bool refused_when_invalid = false;
    try { (void)binding.BoundCandidateOrdinalForLiveView("CAM-A"); }
    catch (const DualIdentitySessionBindingError& error) {
        refused_when_invalid = error.Code() == "BindingNotReady";
    }
    Check(refused_when_invalid, "an invalidated binding hands out no ordinal");
}

// ---------------------------------------------------------------------------
// Start / frame / stop
// ---------------------------------------------------------------------------

void ALiveViewOpensAfterReadyAndClosesBeforeActivation() {
    Fixture fixture;
    const auto ceremony = fixture.BindReady();
    Check(Succeeded(ceremony.back()) && ResultCode(ceremony.back()) == "BindingCompleted",
        "the fixture reaches a Ready binding");
    const std::size_t mark = fixture.CallMark();

    const std::string started = fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    Check(Succeeded(started) && ResultCode(started) == "BoundLiveViewStarted",
        "a Ready binding opens a Live View for CAM-A");
    Check(StringFieldOf(started, "schemaVersion") == kDualBoundLiveViewSchemaVersion &&
            StringFieldOf(started, "marker") == kBindingMarker,
        "the response carries the bound Live View schema and the binding host marker");
    Check(StringFieldOf(started, "state") == "Live" &&
            StringFieldOf(started, "cameraAlias") == "CAM-A",
        "the start response names the alias and says it is Live");
    Check(NumberFieldOf(started, "captureAdmissionRemainingMs") == 415'000,
        "a success reports what is left of the capture admission window (600 s - 185 s)");

    const std::string frame = fixture.Handle(FrameRequest(fixture.session, "CAM-A"));
    Check(Succeeded(frame) && ResultCode(frame) == "BoundLiveViewFrame" &&
            NumberFieldOf(frame, "frameBytes") == 4096,
        "a running Live View returns a bounded frame");
    Check(NumberFieldOf(frame, "captureAdmissionRemainingMs") == 415'000,
        "a frame response reports the admission window too");

    const std::string stopped = fixture.Handle(StopRequest(fixture.session));
    Check(Succeeded(stopped) && ResultCode(stopped) == "BoundLiveViewStopped" &&
            BoolFieldIs(stopped, "liveViewStopped", true) &&
            BoolFieldIs(stopped, "sdkSourceClosed", true) &&
            BoolFieldIs(stopped, "wasOpen", true),
        "stop reports that it closed a running Live View and the source behind it");
    Check(fixture.CallsSince(mark) == Calls({
              "StartLiveView:0", "ReadLiveViewFrame:0", "StopLiveView:0",
              "CloseCandidateSession:0"}),
        "the adapter saw exactly start, read, stop, then close, in that order");
    Check(fixture.adapter->InspectOpenState().IsClosed() &&
            fixture.adapter->OpenSourceCount() == 0,
        "nothing is left open on the adapter after stop");
    Check(fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Ready,
        "using Live View does not disturb a Ready binding");
    Check(fixture.live->IsFullyClosed(), "the controller reads back as fully closed");

    const std::string activated = fixture.Handle(ActivateCaptureRequest(fixture.session));
    Check(Succeeded(activated) && ResultCode(activated) == "CaptureHostActivated" &&
            fixture.dispatcher.CaptureTransitionRequested(),
        "capture activation is accepted once the Live View has been closed");
    const auto counters = fixture.live->Counters();
    Check(counters.start_count == 1 && counters.frame_count == 1 &&
            counters.stop_count == 1 && counters.idle_stop_count == 0,
        "the counters reflect exactly what happened");
    Check(fixture.NoOverlapEver(), "no two Live Views or camera sessions ever overlapped");
}

void TheAliasDecidesWhichCandidateIsOpened() {
    Fixture fixture;
    (void)fixture.BindReady(1);
    const std::size_t mark = fixture.CallMark();

    const std::string started = fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    Check(Succeeded(started), "CAM-A opens when it is the second candidate");
    Check(fixture.CallsSince(mark) == Calls({"StartLiveView:1"}),
        "CAM-A opened the candidate the operator assigned it to, not candidate 0");

    const std::string wrong_alias = fixture.Handle(FrameRequest(fixture.session, "CAM-B"));
    Check(!Succeeded(wrong_alias) && ResultCode(wrong_alias) == "BoundLiveViewNotActive",
        "a frame for the alias that is not Live is refused");
    Check(fixture.CallsSince(mark) == Calls({"StartLiveView:1"}),
        "the refused frame request did not reach the SDK");
    (void)fixture.Handle(StopRequest(fixture.session));
}

void ASecondSourceIsRefusedNotSwitchedTo() {
    Fixture fixture;
    (void)fixture.BindReady();
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    const std::size_t mark = fixture.CallMark();

    const std::string other = fixture.Handle(StartRequest(fixture.session, "CAM-B"));
    Check(!Succeeded(other) && ResultCode(other) == "BoundLiveViewAlreadyOpen",
        "starting the other camera while one is open is refused, not turned into a switch");
    const std::string same = fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    Check(!Succeeded(same) && ResultCode(same) == "BoundLiveViewAlreadyOpen",
        "starting the camera that is already open is refused too");
    Check(fixture.CallsSince(mark).empty(),
        "refusing a second source makes no SDK call at all");
    Check(fixture.live->State() == DualBoundLiveViewState::Live &&
            fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Ready,
        "the refusal leaves the first Live View running and the binding Ready");

    // The client switches by stopping first.
    Check(Succeeded(fixture.Handle(StopRequest(fixture.session))), "the first view stops");
    const std::string switched = fixture.Handle(StartRequest(fixture.session, "CAM-B"));
    Check(Succeeded(switched), "the other camera opens once the first one is closed");
    Check(fixture.CallsSince(mark) == Calls({
              "StopLiveView:0", "CloseCandidateSession:0", "StartLiveView:1"}),
        "a switch is stop, close, start: the first source is closed before the second opens");
    Check(fixture.adapter->OpenSourceCount() == 1 && fixture.NoOverlapEver(),
        "only one source is ever open");
    (void)fixture.Handle(StopRequest(fixture.session));
}

void StopWithNothingOpenIsAnIdempotentSuccess() {
    Fixture fixture;
    (void)fixture.BindReady();
    const std::size_t mark = fixture.CallMark();

    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::string stopped = fixture.Handle(StopRequest(fixture.session));
        Check(Succeeded(stopped) && ResultCode(stopped) == "BoundLiveViewStopped" &&
                BoolFieldIs(stopped, "liveViewStopped", true) &&
                BoolFieldIs(stopped, "sdkSourceClosed", true) &&
                BoolFieldIs(stopped, "wasOpen", false),
            "stop with nothing open reports true, true and wasOpen=false");
    }
    Check(fixture.CallsSince(mark).empty(),
        "stop with nothing open touches no SDK call");

    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    const std::string first = fixture.Handle(StopRequest(fixture.session));
    const std::string second = fixture.Handle(StopRequest(fixture.session));
    Check(BoolFieldIs(first, "wasOpen", true) && BoolFieldIs(second, "wasOpen", false) &&
            Succeeded(second),
        "a repeated stop is a success that says nothing was open");
    Check(fixture.live->Counters().stop_count == 1,
        "only the stop that closed something is counted");
}

void FramesAreBoundedAndAnEmptyFrameDoesNotStopTheLiveView() {
    Fixture fixture;
    // The binding ceremony needs real preview frames to confirm an alias, so the
    // camera only starts returning empty frames once the binding is Ready.
    (void)fixture.BindReady();
    fixture.adapter->MutableOptions().live_view_frame_bytes = 0;

    const std::string before_start = fixture.Handle(FrameRequest(fixture.session, "CAM-A"));
    Check(!Succeeded(before_start) && ResultCode(before_start) == "BoundLiveViewNotActive",
        "a frame before start is refused as not active");

    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    const std::size_t mark = fixture.CallMark();
    const std::string empty = fixture.Handle(FrameRequest(fixture.session, "CAM-A"));
    Check(!Succeeded(empty) && ResultCode(empty) == "LiveViewFrameUnavailable",
        "an empty frame is reported as unavailable");
    Check(fixture.CallsSince(mark) == Calls({"ReadLiveViewFrame:0"}),
        "an empty frame without an SDK error stops nothing and closes nothing");
    Check(fixture.live->State() == DualBoundLiveViewState::Live &&
            fixture.adapter->ActiveLiveViewCount() == 1 &&
            fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Ready,
        "the Live View stays open and the binding stays Ready after an empty frame");

    fixture.adapter->MutableOptions().live_view_frame_bytes = 3;
    const std::string small = fixture.Handle(FrameRequest(fixture.session, "CAM-A"));
    Check(Succeeded(small) && NumberFieldOf(small, "frameBytes") == 3 &&
            StringFieldOf(small, "frameBase64") == "AAEC",
        "the same Live View then returns a frame, and the base64 is the known bytes");

    fixture.adapter->MutableOptions().live_view_frame_bytes =
        kMaximumBindingLiveViewFrameBytes;
    Check(Succeeded(fixture.Handle(FrameRequest(fixture.session, "CAM-A"))),
        "a frame exactly at the cap is accepted");

    fixture.adapter->MutableOptions().live_view_frame_bytes =
        kMaximumBindingLiveViewFrameBytes + 1;
    const std::string too_large = fixture.Handle(FrameRequest(fixture.session, "CAM-A"));
    Check(!Succeeded(too_large) && ResultCode(too_large) == "LiveViewFrameTooLarge" &&
            too_large.find("frameBase64") == std::string::npos,
        "a frame over the cap is refused and none of it is sent");
    Check(fixture.live->State() == DualBoundLiveViewState::Live &&
            fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Ready,
        "an oversize frame leaves the Live View open and the binding Ready");
    Check(fixture.live->Counters().frame_count == 2,
        "only the two frames that were handed out are counted");

    // CAM-B's frames are different bytes: the frame comes from the alias asked for.
    (void)fixture.Handle(StopRequest(fixture.session));
    fixture.adapter->MutableOptions().live_view_frame_bytes = 3;
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-B"));
    const std::string cam_b = fixture.Handle(FrameRequest(fixture.session, "CAM-B"));
    Check(StringFieldOf(cam_b, "frameBase64") == "AQID",
        "CAM-B's frame comes from CAM-B's candidate");
    (void)fixture.Handle(StopRequest(fixture.session));
}

// ---------------------------------------------------------------------------
// Rejections that never reach the SDK
// ---------------------------------------------------------------------------

void MalformedAndMisaddressedRequestsAreRefusedBeforeTheSdk() {
    Fixture fixture;
    (void)fixture.BindReady();
    const std::size_t mark = fixture.CallMark();
    const std::string session = fixture.session;

    const auto code_of = [&fixture](const std::string& request) {
        return ResultCode(fixture.Handle(request));
    };

    Check(code_of(LiveEnvelope("r-1", "Hardware", "start-bound-live-view",
                               SessionAliasPayload(session, "CAM-A"))) ==
              "DualBoundLiveViewProtocolRequired",
        "the capture host's marker is refused by the binding host");
    Check(code_of(LiveEnvelope("r-1", "HardwareBinding", "start-bound-live-view",
                               SessionAliasPayload(session, "CAM-A"), true)) ==
              "DualBoundLiveViewProtocolRequired",
        "a simulated request is refused");
    Check(code_of(LiveEnvelope("r-1", "HardwareBinding", "start-bound-live-view",
                               SessionAliasPayload(session, "CAM-C"))) == "UnknownCameraAlias",
        "an alias outside CAM-A and CAM-B is refused");
    Check(code_of(LiveEnvelope("r-1", "HardwareBinding", "start-bound-live-view",
                               R"({"sessionId":")" + session + R"("})")) == "UnexpectedField",
        "a start with no alias is refused");
    Check(code_of(LiveEnvelope("r-1", "HardwareBinding", "stop-bound-live-view",
                               SessionAliasPayload(session, "CAM-A"))) == "UnexpectedField",
        "a stop carrying an alias is refused rather than ignored");
    Check(code_of(LiveEnvelope("r-1", "HardwareBinding", "start-bound-live-view",
                               R"({"sessionId":"short","cameraAlias":"CAM-A"})")) ==
              "InvalidSessionId",
        "a malformed session ID is refused by the parser");
    Check(code_of(LiveEnvelope("r-1", "HardwareBinding", "start-bound-live-view",
                               R"({"sessionId":"00000000000000000000000000000000",)"
                               R"("cameraAlias":"CAM-A"})")) == "InvalidSessionId",
        "an all-zero session ID is refused");
    Check(code_of(LiveEnvelope("r 1", "HardwareBinding", "stop-bound-live-view",
                               R"({"sessionId":")" + session + R"("})")) == "InvalidRequestId",
        "an unsafe request ID is refused");
    Check(code_of(LiveEnvelope("r-1", "HardwareBinding", "restart-bound-live-view",
                               R"({"sessionId":")" + session + R"("})")) ==
              "UnsupportedOperation",
        "an operation outside the three defined ones is refused");
    Check(code_of("{ not json") == "MalformedEnvelope",
        "text that is not JSON falls through to the binding protocol's own refusal");

    const std::string wrong_marker = fixture.Handle(LiveEnvelope(
        "r-echo", "Hardware", "stop-bound-live-view",
        R"({"sessionId":")" + session + R"("})"));
    Check(StringFieldOf(wrong_marker, "requestId") == "r-echo" &&
            StringFieldOf(wrong_marker, "schemaVersion") == kDualBoundLiveViewSchemaVersion &&
            StringFieldOf(wrong_marker, "marker") == kBindingMarker,
        "a refusal still echoes the request ID and answers in this schema with this host's marker");

    Check(fixture.CallsSince(mark).empty(),
        "none of those refusals reached the SDK");
    Check(fixture.live->State() == DualBoundLiveViewState::Closed &&
            fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Ready,
        "refused requests leave the binding Ready and nothing open");
}

void ALiveViewNeedsTheCurrentSessionAndAReadyBinding() {
    Fixture fixture;
    const std::string no_session = fixture.Handle(
        StartRequest("0123456789abcdef0123456789abcdef", "CAM-A"));
    Check(ResultCode(no_session) == "SessionMismatch",
        "before any binding there is no session to address");

    const std::string begun = fixture.Handle(BeginBindingRequest());
    const std::string session = StringFieldOf(begun, "sessionId");
    const std::size_t mark = fixture.CallMark();

    const std::string collecting = fixture.Handle(StartRequest(session, "CAM-A"));
    Check(!Succeeded(collecting) && ResultCode(collecting) == "BindingNotReady",
        "a binding that is still collecting candidates is not Ready");
    const std::string other = fixture.Handle(
        StartRequest("0123456789abcdef0123456789abcdef", "CAM-A"));
    Check(ResultCode(other) == "SessionMismatch",
        "another session's ID is refused");
    Check(fixture.CallsSince(mark).empty(),
        "neither refusal issued an SDK command");
    Check(fixture.adapter->InspectOpenState().IsClosed(),
        "nothing was opened by a request the binding was not ready for");

    // The candidate-phase protocol is unchanged: a bound Live View is not a way
    // around its "an assigned candidate cannot be viewed again" rule.
    (void)fixture.BindReady();
    const std::string candidate_after_ready = fixture.Handle(
        CandidateStartRequest(fixture.session, 0));
    Check(!Succeeded(candidate_after_ready) &&
            ResultCode(candidate_after_ready) == "BindingNotCollecting",
        "the binding protocol still refuses a candidate Live View once Ready");
}

void AMismatchedSessionCannotConsumeTheCurrentSessionsEvent() {
    Fixture fixture;
    (void)fixture.BindReady();
    fixture.adapter->RaiseInvalidation(DualIdentityInvalidationReason::UsbReconnect);

    const std::string stale = fixture.Handle(
        StartRequest("0123456789abcdef0123456789abcdef", "CAM-A"));
    Check(ResultCode(stale) == "SessionMismatch",
        "a request for another session is refused without polling");
    Check(fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Ready,
        "the refused request did not invalidate the current session");

    const std::string current = fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    Check(!Succeeded(current) && ResultCode(current) == "BindingInvalidated" &&
            StringFieldOf(current, "invalidationReason") == "UsbReconnect",
        "the current session still sees its own invalidation event");
}

// ---------------------------------------------------------------------------
// Host lifetime
// ---------------------------------------------------------------------------

void ALiveViewIsNotStartedWithTooLittleHostLifetime() {
    Fixture fixture;
    (void)fixture.BindReady();
    const std::size_t mark = fixture.CallMark();

    fixture.host_remaining = kBoundLiveViewMinimumHostRemainingMilliseconds - 1;
    const std::string refused = fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    Check(!Succeeded(refused) && ResultCode(refused) == "HostLifetimeInsufficient" &&
            NumberFieldOf(refused, "captureAdmissionRemainingMs") == 0,
        "one millisecond under the minimum lifetime refuses the start");
    Check(fixture.CallsSince(mark).empty(), "the refusal made no SDK call");
    Check(fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Ready,
        "running out of host lifetime does not invalidate the binding");

    fixture.host_remaining = kBoundLiveViewMinimumHostRemainingMilliseconds;
    Check(Succeeded(fixture.Handle(StartRequest(fixture.session, "CAM-A"))),
        "exactly the minimum lifetime is enough to start");

    // The reported window follows the capture host's own admission threshold.
    fixture.host_remaining = kBoundLiveViewCaptureAdmissionReserveMilliseconds + 1;
    Check(NumberFieldOf(fixture.Handle(FrameRequest(fixture.session, "CAM-A")),
                        "captureAdmissionRemainingMs") == 1,
        "one millisecond over the admission reserve leaves one millisecond");
    fixture.host_remaining = kBoundLiveViewCaptureAdmissionReserveMilliseconds;
    Check(NumberFieldOf(fixture.Handle(FrameRequest(fixture.session, "CAM-A")),
                        "captureAdmissionRemainingMs") == 0,
        "exactly the admission reserve leaves nothing");
    fixture.host_remaining = kBoundLiveViewCaptureAdmissionReserveMilliseconds - 1;
    Check(NumberFieldOf(fixture.Handle(FrameRequest(fixture.session, "CAM-A")),
                        "captureAdmissionRemainingMs") == 0,
        "under the admission reserve the window is zero, never negative");
    Check(kBoundLiveViewCaptureAdmissionReserveMilliseconds == 185'000,
        "the reserve is the capture host's 180 s watchdog + 3 s reply + 2 s cleanup");
    static_assert(kBoundLiveViewCaptureAdmissionReserveMilliseconds ==
            kDualMinimumHostAdmissionMilliseconds,
        "the reported window must equal the capture host's own admission threshold");

    // Lifetime only gates the start. A Live View already open keeps serving and
    // can always be stopped, even with no lifetime left.
    fixture.host_remaining = 0;
    Check(Succeeded(fixture.Handle(FrameRequest(fixture.session, "CAM-A"))),
        "an open Live View still serves frames when lifetime is nearly gone");
    const std::string stopped = fixture.Handle(StopRequest(fixture.session));
    Check(Succeeded(stopped) && NumberFieldOf(stopped, "captureAdmissionRemainingMs") == 0,
        "stop works with no lifetime left");

    fixture.host_remaining_throws = true;
    const std::string unknown = fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    Check(ResultCode(unknown) == "HostLifetimeInsufficient",
        "a lifetime that cannot be read is treated as none left");
}

void TheControllerCannotBeBuiltWithoutALifetimeSource() {
    DualBindingFakeSdkAdapter adapter;
    bool refused = false;
    try {
        DualBoundLiveViewController controller(adapter, DualBoundLiveViewOptions{});
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    Check(refused, "a controller with no host lifetime source is refused at construction");
}

// ---------------------------------------------------------------------------
// SDK failures and invalidation
// ---------------------------------------------------------------------------

void AssertTerminal(Fixture& fixture, std::string_view context) {
    const std::string label(context);
    Check(fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Invalid,
        label + ": the binding is invalid");
    Check(fixture.live->State() == DualBoundLiveViewState::Terminal,
        label + ": the controller is terminal");
    const std::size_t mark = fixture.CallMark();
    const std::string start = fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    const std::string frame = fixture.Handle(FrameRequest(fixture.session, "CAM-A"));
    const std::string stop = fixture.Handle(StopRequest(fixture.session));
    const std::string activate = fixture.Handle(ActivateCaptureRequest(fixture.session));
    Check(ResultCode(start) == "BindingInvalidated" &&
            ResultCode(frame) == "BindingInvalidated" &&
            ResultCode(stop) == "BindingInvalidated",
        label + ": start, frame and stop are all refused as invalidated");
    Check(ResultCode(activate) == "BindingInvalidated" &&
            !fixture.dispatcher.CaptureTransitionRequested(),
        label + ": capture cannot be activated");
    Check(fixture.CallsSince(mark).empty(),
        label + ": an invalidated binding is never reopened or retried automatically");
}

void AnSdkErrorWhileStartingInvalidatesTheBinding() {
    DualBindingFakeSdkOptions options;
    options.nikon_like_failure_semantics = true;
    Fixture fixture(options);
    (void)fixture.BindReady();
    // Switched on only now: the binding ceremony itself opens Live Views.
    fixture.adapter->MutableOptions().fail_start_live_view = true;

    const std::string failed = fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    Check(!Succeeded(failed) && ResultCode(failed) == "BindingInvalidated" &&
            StringFieldOf(failed, "state") == "Invalid" &&
            StringFieldOf(failed, "invalidationReason") == "SdkError",
        "an SDK error on start invalidates the binding and says why");
    Check(BoolFieldIs(failed, "liveViewStopped", true) &&
            BoolFieldIs(failed, "sdkSourceClosed", true),
        "the response carries what the adapter reports as closed");
    AssertTerminal(fixture, "start failure");
}

void AnSdkErrorWhileReadingAFrameInvalidatesTheBinding() {
    DualBindingFakeSdkOptions options;
    options.nikon_like_failure_semantics = true;
    Fixture fixture(options);
    (void)fixture.BindReady();
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-B"));
    fixture.adapter->MutableOptions().fail_read_live_view_frame = true;

    const std::string failed = fixture.Handle(FrameRequest(fixture.session, "CAM-B"));
    Check(!Succeeded(failed) && ResultCode(failed) == "BindingInvalidated" &&
            StringFieldOf(failed, "invalidationReason") == "SdkError" &&
            BoolFieldIs(failed, "liveViewStopped", true) &&
            BoolFieldIs(failed, "sdkSourceClosed", true),
        "an SDK error during a frame read invalidates the binding, unlike an empty frame");
    AssertTerminal(fixture, "frame failure");
}

void AFailedStopInvalidatesTheBindingAndSkipsTheClose() {
    Fixture fixture;
    (void)fixture.BindReady();
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    fixture.adapter->MutableOptions().fail_stop_live_view = true;
    const std::size_t mark = fixture.CallMark();

    const std::string failed = fixture.Handle(StopRequest(fixture.session));
    Check(!Succeeded(failed) && ResultCode(failed) == "BindingInvalidated" &&
            StringFieldOf(failed, "invalidationReason") == "SdkError",
        "a Live View that will not stop invalidates the binding");
    Check(BoolFieldIs(failed, "liveViewStopped", false) &&
            BoolFieldIs(failed, "sdkSourceClosed", false) &&
            BoolFieldIs(failed, "wasOpen", true),
        "the response reports that something was open and nothing was confirmed closed");
    Check(fixture.CallsSince(mark) == Calls({"StopLiveView:0"}),
        "the source is not closed under a Live View that did not stop, and nothing is retried");
    Check(fixture.adapter->ActiveLiveViewCount() == 1,
        "the facts in the response match the adapter: the Live View is still running");
    AssertTerminal(fixture, "stop failure");
}

void AFailedStopThatEndedTheSessionStillReportsTheStreamAsUnconfirmed() {
    DualBindingFakeSdkOptions options;
    options.nikon_like_failure_semantics = true;
    Fixture fixture(options);
    (void)fixture.BindReady();
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    fixture.adapter->MutableOptions().fail_stop_live_view = true;

    // Under Nikon-like semantics the failure ends the whole SDK session, so the
    // source reads as closed. The stop command itself never succeeded, so the
    // stream is not reported as stopped.
    const std::string failed = fixture.Handle(StopRequest(fixture.session));
    Check(!Succeeded(failed) && ResultCode(failed) == "BindingInvalidated" &&
            StringFieldOf(failed, "invalidationReason") == "SdkError",
        "a Live View that will not stop invalidates the binding");
    Check(BoolFieldIs(failed, "liveViewStopped", false) &&
            BoolFieldIs(failed, "sdkSourceClosed", true) &&
            BoolFieldIs(failed, "wasOpen", true),
        "the source is closed by the session end, but the stream is not reported stopped");
    Check(!fixture.adapter->InspectOpenState().IsClosed() &&
            fixture.adapter->InspectOpenState().live_view_open,
        "the adapter keeps reporting a Live View open until the session is explicitly ended");
    AssertTerminal(fixture, "stop failure that ended the session");

    const std::string cancelled = fixture.Handle(CancelBindingRequest(fixture.session));
    Check(Succeeded(cancelled) && fixture.adapter->InspectOpenState().IsClosed(),
        "cancel-binding ends the session and clears the unconfirmed stream");
}

void AFailedCloseInvalidatesTheBindingAndReportsWhatWasConfirmed() {
    Fixture fixture;
    (void)fixture.BindReady();
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    fixture.adapter->MutableOptions().fail_close_candidate_session = true;
    const std::size_t mark = fixture.CallMark();

    const std::string failed = fixture.Handle(StopRequest(fixture.session));
    Check(!Succeeded(failed) && ResultCode(failed) == "BindingInvalidated" &&
            BoolFieldIs(failed, "liveViewStopped", true) &&
            BoolFieldIs(failed, "sdkSourceClosed", false),
        "a source that will not close invalidates the binding; the stream is reported stopped");
    Check(fixture.CallsSince(mark) == Calls({"StopLiveView:0", "CloseCandidateSession:0"}),
        "stop then one close attempt and no retry");
    Check(fixture.adapter->OpenSourceCount() == 1,
        "the facts in the response match the adapter: the source is still open");
    // The unclosed source keeps capture out for good.
    const std::string activate = fixture.Handle(ActivateCaptureRequest(fixture.session));
    Check(!Succeeded(activate) && !fixture.dispatcher.CaptureTransitionRequested(),
        "capture cannot be activated over a source that would not close");
    AssertTerminal(fixture, "close failure");
}

void AnUsbEventWhileLiveInvalidatesAndCleansUpOnCancel() {
    DualBindingFakeSdkOptions options;
    options.nikon_like_failure_semantics = true;
    Fixture fixture(options);
    (void)fixture.BindReady();
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    const std::size_t mark = fixture.CallMark();

    fixture.adapter->RaiseInvalidation(DualIdentityInvalidationReason::UsbReconnect);
    const std::string response = fixture.Handle(FrameRequest(fixture.session, "CAM-A"));
    Check(!Succeeded(response) && ResultCode(response) == "BindingInvalidated" &&
            StringFieldOf(response, "invalidationReason") == "UsbReconnect",
        "a USB event while Live invalidates the binding with its typed reason");
    Check(BoolFieldIs(response, "liveViewStopped", true) &&
            BoolFieldIs(response, "sdkSourceClosed", true),
        "the response says nothing is left running, as the adapter reports");
    Check(fixture.CallsSince(mark).empty(),
        "the agent issued no SDK command of its own for an event that ended the session");
    AssertTerminal(fixture, "usb event");

    // Cleanup is the existing terminal cancel path, and it is allowed after an
    // invalidation.
    const std::string cancelled = fixture.Handle(CancelBindingRequest(fixture.session));
    Check(Succeeded(cancelled) && ResultCode(cancelled) == "BindingCancelled" &&
            fixture.adapter->EndBindingSessionCount() == 1,
        "cancel-binding ends the session exactly once");
}

void ARefusedStartThatLeavesNothingOpenKeepsTheBindingReady() {
    Fixture fixture;
    (void)fixture.BindReady();
    fixture.adapter->MutableOptions().fail_start_live_view = true;
    const std::size_t mark = fixture.CallMark();

    const std::string refused = fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    Check(!Succeeded(refused) && ResultCode(refused) == "LiveViewStartFailed",
        "an SDK refusal with no error event and nothing left open is a plain refusal");
    Check(fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Ready &&
            fixture.live->State() == DualBoundLiveViewState::Closed &&
            fixture.adapter->InspectOpenState().IsClosed(),
        "the binding stays Ready and nothing is recorded as running");
    // Nothing retries on its own; a person asking again is a new request.
    Check(fixture.CallsSince(mark) == Calls({"StartLiveView:0"}),
        "exactly one start attempt was made");

    fixture.adapter->MutableOptions().fail_start_live_view = false;
    Check(Succeeded(fixture.Handle(StartRequest(fixture.session, "CAM-A"))),
        "a later explicit request can start it");
    (void)fixture.Handle(StopRequest(fixture.session));
}

// ---------------------------------------------------------------------------
// Capture activation
// ---------------------------------------------------------------------------

void ActivateCaptureIsRefusedWhileALiveViewIsOpen() {
    Fixture fixture;
    (void)fixture.BindReady();
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    const std::size_t mark = fixture.CallMark();

    const std::string refused = fixture.Handle(ActivateCaptureRequest(fixture.session));
    Check(!Succeeded(refused) && ResultCode(refused) == "BoundLiveViewOpen",
        "activate-capture is refused while a Live View is open");
    Check(!fixture.dispatcher.CaptureTransitionRequested() && !fixture.dispatcher.ShouldStop(),
        "the host does not move toward the capture pipe");
    Check(fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Ready,
        "the refusal leaves the binding Ready so the client can stop the view and ask again");
    Check(fixture.CallsSince(mark).empty(),
        "the refusal makes no SDK call and does not stop the Live View for the client");
    Check(fixture.adapter->ActiveLiveViewCount() == 1, "the Live View is still running");

    (void)fixture.Handle(StopRequest(fixture.session));
    const std::string accepted = fixture.Handle(ActivateCaptureRequest(fixture.session));
    Check(Succeeded(accepted) && fixture.dispatcher.CaptureTransitionRequested(),
        "after the stop the same request is accepted");
}

void TheOpenCheckRestsOnTheAdapterNotOnTheControllersFlag() {
    Fixture fixture;
    (void)fixture.BindReady();
    // Something opens a source behind the controller's back. Its own bookkeeping
    // still says Closed.
    Check(fixture.adapter->StartLiveView(0), "test setup: a stray source opens");
    Check(fixture.live->State() == DualBoundLiveViewState::Closed,
        "the controller's own state still says Closed");
    Check(!fixture.live->IsFullyClosed(), "but it does not read back as fully closed");

    const std::string activate = fixture.Handle(ActivateCaptureRequest(fixture.session));
    Check(!Succeeded(activate) && ResultCode(activate) == "BoundLiveViewOpen" &&
            !fixture.dispatcher.CaptureTransitionRequested(),
        "capture is not activated over a source the adapter reports open");
    const std::string start = fixture.Handle(StartRequest(fixture.session, "CAM-B"));
    Check(!Succeeded(start) && ResultCode(start) == "BoundLiveViewAlreadyOpen",
        "nor is a second source started");
    const std::string stop = fixture.Handle(StopRequest(fixture.session));
    Check(!Succeeded(stop) && ResultCode(stop) == "BindingInvalidated" &&
            fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Invalid,
        "a stop cannot name what it did not open, so it invalidates instead of guessing");
}

// ---------------------------------------------------------------------------
// OnIdle
// ---------------------------------------------------------------------------

void AQuietClientHasItsLiveViewStoppedAndClosed() {
    Fixture fixture;
    (void)fixture.BindReady();
    fixture.now = 1'000;
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    const std::size_t mark = fixture.CallMark();

    fixture.now = 1'000 + kBoundLiveViewIdleStopMilliseconds - 1;
    fixture.dispatcher.OnIdle();
    Check(fixture.live->State() == DualBoundLiveViewState::Live &&
            fixture.CallsSince(mark).empty(),
        "one millisecond short of the idle limit nothing happens");

    fixture.now = 1'000 + kBoundLiveViewIdleStopMilliseconds;
    fixture.dispatcher.OnIdle();
    Check(fixture.CallsSince(mark) == Calls({"StopLiveView:0", "CloseCandidateSession:0"}),
        "at the idle limit the host stops the Live View and then closes the source");
    Check(fixture.live->State() == DualBoundLiveViewState::Closed &&
            fixture.adapter->InspectOpenState().IsClosed() &&
            fixture.live->Counters().idle_stop_count == 1,
        "the controller and the adapter both read back as closed, and the close is counted");
    Check(fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Ready,
        "an idle close does not invalidate the binding");

    const std::size_t after = fixture.CallMark();
    fixture.now += 100'000;
    fixture.dispatcher.OnIdle();
    Check(fixture.CallsSince(after).empty(), "with nothing open OnIdle touches nothing");

    const std::string frame = fixture.Handle(FrameRequest(fixture.session, "CAM-A"));
    Check(ResultCode(frame) == "BoundLiveViewNotActive",
        "the client finds out the view is gone from the next frame request");
    Check(Succeeded(fixture.Handle(StartRequest(fixture.session, "CAM-A"))),
        "and may start it again");
    Check(fixture.NoOverlapEver(), "the idle close never left two sources open");
}

void AnyRequestForTheSessionKeepsTheLiveViewAlive() {
    Fixture fixture;
    (void)fixture.BindReady();
    fixture.now = 1'000;
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));

    fixture.now = 10'000;
    (void)fixture.Handle(FrameRequest(fixture.session, "CAM-A"));
    fixture.now = 1'000 + kBoundLiveViewIdleStopMilliseconds + 8'999;
    fixture.dispatcher.OnIdle();
    Check(fixture.live->State() == DualBoundLiveViewState::Live,
        "a frame request restarts the idle timer");

    // A request for another session proves nothing about the client we serve.
    fixture.now = 19'999;
    (void)fixture.Handle(FrameRequest("0123456789abcdef0123456789abcdef", "CAM-A"));
    fixture.now = 10'000 + kBoundLiveViewIdleStopMilliseconds;
    fixture.dispatcher.OnIdle();
    Check(fixture.live->State() == DualBoundLiveViewState::Closed,
        "a request for another session does not keep the Live View alive");
}

void AnIdleCloseThatFailsInvalidatesTheBinding() {
    Fixture fixture;
    (void)fixture.BindReady();
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    fixture.adapter->MutableOptions().fail_close_candidate_session = true;
    const std::size_t mark = fixture.CallMark();

    fixture.now += kBoundLiveViewIdleStopMilliseconds;
    fixture.dispatcher.OnIdle();
    Check(fixture.CallsSince(mark) == Calls({"StopLiveView:0", "CloseCandidateSession:0"}),
        "one attempt at each step");
    Check(fixture.dispatcher.BindingState() == DualIdentitySessionBindingState::Invalid &&
            fixture.live->State() == DualBoundLiveViewState::Terminal,
        "a close the host cannot confirm invalidates the binding");
    const std::size_t after = fixture.CallMark();
    fixture.now += 100'000;
    fixture.dispatcher.OnIdle();
    Check(fixture.CallsSince(after).empty(), "and is not retried on the next idle");
}

// ---------------------------------------------------------------------------
// Session boundaries
// ---------------------------------------------------------------------------

void ANewBindingDiscardsTheOldLiveView() {
    Fixture fixture;
    (void)fixture.BindReady();
    const std::string old_session = fixture.session;
    (void)fixture.Handle(StartRequest(old_session, "CAM-A"));

    (void)fixture.BindReady();
    Check(fixture.session != old_session, "the second binding has its own session");
    Check(fixture.live->State() == DualBoundLiveViewState::Closed &&
            fixture.live->IsFullyClosed(),
        "rebinding leaves nothing open and the controller Closed");
    const std::string stale = fixture.Handle(StartRequest(old_session, "CAM-A"));
    Check(ResultCode(stale) == "SessionMismatch", "the old session cannot start a view");
    Check(Succeeded(fixture.Handle(StartRequest(fixture.session, "CAM-A"))),
        "the new session can");
    (void)fixture.Handle(StopRequest(fixture.session));
    Check(fixture.NoOverlapEver(), "rebinding never left two sources open");
}

void CancelBindingClosesTheLiveViewThroughTheTerminalPath() {
    Fixture fixture;
    (void)fixture.BindReady();
    (void)fixture.Handle(StartRequest(fixture.session, "CAM-A"));

    const std::string cancelled = fixture.Handle(CancelBindingRequest(fixture.session));
    Check(Succeeded(cancelled) && ResultCode(cancelled) == "BindingCancelled" &&
            fixture.adapter->EndBindingSessionCount() == 1,
        "cancel-binding ends the SDK session once");
    Check(fixture.adapter->InspectOpenState().IsClosed() &&
            fixture.live->State() == DualBoundLiveViewState::Closed,
        "the Live View is gone and the controller is Closed");
    Check(fixture.dispatcher.ShouldStop(), "the host stops after a cancellation");
}

// ---------------------------------------------------------------------------
// Two hosts, one schema
// ---------------------------------------------------------------------------

class TestPort final : public DualBoundLiveViewBindingPort {
public:
    std::string session_id{"0123456789abcdef0123456789abcdef"};
    DualIdentitySessionBindingState state{DualIdentitySessionBindingState::Ready};
    DualIdentityInvalidationReason reason{DualIdentityInvalidationReason::None};

    std::string BoundLiveViewSessionId() const override { return session_id; }
    DualIdentitySessionBindingState BoundLiveViewBindingState() const noexcept override {
        return state;
    }
    DualIdentityInvalidationReason BoundLiveViewInvalidationReason()
        const noexcept override {
        return reason;
    }
    std::size_t BoundLiveViewCandidateOrdinal(std::string_view alias) override {
        return alias == "CAM-A" ? 0 : 1;
    }
    void InvalidateBoundLiveViewBinding(DualIdentityInvalidationReason r) noexcept override {
        state = DualIdentitySessionBindingState::Invalid;
        if (reason == DualIdentityInvalidationReason::None) reason = r;
    }
};

void EachHostAcceptsOnlyItsOwnMarker() {
    DualBindingFakeSdkAdapter adapter;
    (void)adapter.EnumerateCandidates();
    DualBoundLiveViewOptions options;
    options.host_remaining_milliseconds = [] { return std::uint64_t{600'000}; };
    DualBoundLiveViewController controller(adapter, std::move(options));
    TestPort port;

    // As the capture host would call it.
    const std::string served = controller.Handle(
        StartRequest(port.session_id, "CAM-A", kCaptureMarker), kCaptureMarker, port);
    Check(Succeeded(served) && StringFieldOf(served, "marker") == kCaptureMarker,
        "a host that expects the capture marker serves a request that carries it");
    const std::string refused = controller.Handle(
        StartRequest(port.session_id, "CAM-A", kBindingMarker), kCaptureMarker, port);
    Check(!Succeeded(refused) && ResultCode(refused) == "DualBoundLiveViewProtocolRequired" &&
            StringFieldOf(refused, "marker") == kCaptureMarker,
        "that host refuses the binding host's marker");
    Check(Succeeded(controller.Handle(
              StopRequest(port.session_id, kCaptureMarker), kCaptureMarker, port)),
        "and stops through the same code path");

    // The v2 capture-host dispatcher does not know this schema in either marker.
    DualHardwareCameraAgentDispatcher v2;
    for (const std::string_view marker : {kBindingMarker, kCaptureMarker}) {
        const std::string response = v2.Handle(StartRequest(port.session_id, "CAM-A", marker));
        Check(!Succeeded(response) && ResultCode(response) == "DualHardwareProtocolRequired",
            "the Dual v2 dispatcher refuses the bound Live View schema");
    }
    Check(adapter.ConcurrentSourceViolationCount() == 0 &&
            adapter.ConcurrentLiveViewViolationCount() == 0,
        "no overlap in the shared-controller path");
}

void WithoutAControllerTheSchemaIsUnavailableAndNothingElseChanges() {
    Fixture fixture(DualBindingFakeSdkOptions{}, false);
    (void)fixture.BindReady();
    const std::size_t mark = fixture.CallMark();

    const std::string response = fixture.Handle(StartRequest(fixture.session, "CAM-A"));
    Check(!Succeeded(response) && ResultCode(response) == "BoundLiveViewUnavailable" &&
            StringFieldOf(response, "schemaVersion") == kDualBoundLiveViewSchemaVersion &&
            StringFieldOf(response, "requestId") == "r-live-start",
        "a host with no controller says the schema is unavailable, in the schema's own terms");
    Check(fixture.CallsSince(mark).empty(), "and makes no SDK call");
    Check(Succeeded(fixture.Handle(ActivateCaptureRequest(fixture.session))),
        "activation behaves exactly as before without a controller");
}

// ---------------------------------------------------------------------------
// Not using Live View changes nothing
// ---------------------------------------------------------------------------

std::vector<std::string> RunAcceptedPath(Fixture& fixture) {
    std::vector<std::string> responses = fixture.BindReady();
    fixture.dispatcher.OnIdle();
    responses.push_back(fixture.Handle(ActivateCaptureRequest(fixture.session)));
    fixture.dispatcher.OnIdle();
    for (auto& response : responses) {
        response = ReplaceAll(response, fixture.session, "<session>");
    }
    return responses;
}

void WithoutLiveViewTheCommandSequenceToTheAgentIsUnchanged() {
    Fixture without(DualBindingFakeSdkOptions{}, false);
    Fixture with_controller(DualBindingFakeSdkOptions{}, true);

    const auto baseline = RunAcceptedPath(without);
    const auto attached = RunAcceptedPath(with_controller);

    Check(baseline == attached,
        "every response on the accepted path is byte-identical with a controller attached");
    Check(without.adapter->CallLog() == with_controller.adapter->CallLog(),
        "the SDK call sequence is identical with and without a controller attached");
    Check(without.adapter->CallLog().size() >= 8,
        "the comparison covered a real binding ceremony, not an empty log");
    Check(with_controller.dispatcher.CaptureTransitionRequested() &&
            without.dispatcher.CaptureTransitionRequested(),
        "both reached the capture activation");
    const auto counters = with_controller.live->Counters();
    Check(counters.start_count == 0 && counters.frame_count == 0 &&
            counters.stop_count == 0 && counters.idle_stop_count == 0,
        "the controller did nothing");
}

void UsingLiveViewAddsOnlyItsOwnCalls() {
    Fixture without(DualBindingFakeSdkOptions{}, false);
    Fixture with_controller(DualBindingFakeSdkOptions{}, true);
    (void)RunAcceptedPath(without);

    // Same ceremony, with one view opened and closed before activation.
    (void)with_controller.BindReady();
    (void)with_controller.Handle(StartRequest(with_controller.session, "CAM-A"));
    (void)with_controller.Handle(FrameRequest(with_controller.session, "CAM-A"));
    (void)with_controller.Handle(StopRequest(with_controller.session));
    (void)with_controller.Handle(ActivateCaptureRequest(with_controller.session));

    Calls expected = without.adapter->CallLog();
    // Everything up to and including the binding ceremony is shared.
    const std::size_t ceremony_calls = expected.size();
    Calls actual = with_controller.adapter->CallLog();
    Check(actual.size() == ceremony_calls + 4,
        "using a Live View adds exactly start, read, stop and close");
    Check(Calls(actual.begin(), actual.begin() + static_cast<std::ptrdiff_t>(ceremony_calls)) ==
              expected,
        "and the calls around it are the accepted path's calls, in the same order");
}

} // namespace

int main() {
    TheCoreHandsOutCandidateOrdinalsOnlyWhileReady();
    ALiveViewOpensAfterReadyAndClosesBeforeActivation();
    TheAliasDecidesWhichCandidateIsOpened();
    ASecondSourceIsRefusedNotSwitchedTo();
    StopWithNothingOpenIsAnIdempotentSuccess();
    FramesAreBoundedAndAnEmptyFrameDoesNotStopTheLiveView();
    MalformedAndMisaddressedRequestsAreRefusedBeforeTheSdk();
    ALiveViewNeedsTheCurrentSessionAndAReadyBinding();
    AMismatchedSessionCannotConsumeTheCurrentSessionsEvent();
    ALiveViewIsNotStartedWithTooLittleHostLifetime();
    TheControllerCannotBeBuiltWithoutALifetimeSource();
    AnSdkErrorWhileStartingInvalidatesTheBinding();
    AnSdkErrorWhileReadingAFrameInvalidatesTheBinding();
    AFailedStopInvalidatesTheBindingAndSkipsTheClose();
    AFailedStopThatEndedTheSessionStillReportsTheStreamAsUnconfirmed();
    AFailedCloseInvalidatesTheBindingAndReportsWhatWasConfirmed();
    AnUsbEventWhileLiveInvalidatesAndCleansUpOnCancel();
    ARefusedStartThatLeavesNothingOpenKeepsTheBindingReady();
    ActivateCaptureIsRefusedWhileALiveViewIsOpen();
    TheOpenCheckRestsOnTheAdapterNotOnTheControllersFlag();
    AQuietClientHasItsLiveViewStoppedAndClosed();
    AnyRequestForTheSessionKeepsTheLiveViewAlive();
    AnIdleCloseThatFailsInvalidatesTheBinding();
    ANewBindingDiscardsTheOldLiveView();
    CancelBindingClosesTheLiveViewThroughTheTerminalPath();
    EachHostAcceptsOnlyItsOwnMarker();
    WithoutAControllerTheSchemaIsUnavailableAndNothingElseChanges();
    WithoutLiveViewTheCommandSequenceToTheAgentIsUnchanged();
    UsingLiveViewAddsOnlyItsOwnCalls();

    if (failures != 0) {
        std::cerr << failures << " dual bound Live View contract failures\n";
        return 1;
    }
    std::cout << "dual bound Live View contracts passed\n";
    return 0;
}
