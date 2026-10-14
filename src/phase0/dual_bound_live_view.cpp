#include "a0/phase0/dual_bound_live_view.hpp"

#include "a0/common/protocol_json.hpp"
#include "a0/phase0/dual_binding_camera_agent.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace a0::phase0 {
namespace {

using ::a0::common::protocol_json::JsonKind;
using ::a0::common::protocol_json::JsonValue;

[[noreturn]] void ProtocolFailure(std::string code, std::string message);

struct BoundLiveViewJsonFailure {
    [[noreturn]] static void Fail(std::string code, std::string message) {
        ProtocolFailure(std::move(code), std::move(message));
    }
};

using JsonParser = ::a0::common::protocol_json::BasicJsonParser<BoundLiveViewJsonFailure>;

[[noreturn]] void ProtocolFailure(std::string code, std::string message) {
    throw DualBoundLiveViewProtocolError(std::move(code), std::move(message));
}

[[nodiscard]] const JsonValue& RequireField(
    const JsonValue& object, std::string_view name, JsonKind kind) {
    return ::a0::common::protocol_json::RequireFieldWith<BoundLiveViewJsonFailure>(
        object, name, kind);
}

void RequireExactFields(const JsonValue& object, const std::set<std::string>& expected) {
    if (object.kind != JsonKind::object || object.object.size() != expected.size()) {
        ProtocolFailure(
            "UnexpectedField",
            "bound Live View protocol object has missing or unexpected fields");
    }
    for (const auto& [name, ignored] : object.object) {
        (void)ignored;
        if (!expected.contains(name)) {
            ProtocolFailure(
                "UnexpectedField",
                "bound Live View protocol object contains an unexpected field");
        }
    }
}

// The same request ID and session ID shapes as the binding protocol, so a
// client builds both kinds of request the same way.
bool IsSafeRequestId(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '.' || character == '-' || character == '_';
    });
}

bool IsSafeSessionId(std::string_view value) noexcept {
    return value.size() == 32 &&
        std::any_of(value.begin(), value.end(),
                    [](char character) { return character != '0'; }) &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f') ||
                (character >= 'A' && character <= 'F');
        });
}

std::string TryExtractSafeRequestId(std::string_view json) noexcept {
    constexpr std::string_view key = "\"requestId\"";
    const std::size_t key_position = json.find(key);
    if (key_position == std::string_view::npos) return "rejected";
    const std::size_t colon = json.find(':', key_position + key.size());
    if (colon == std::string_view::npos) return "rejected";
    const std::size_t opening_quote = json.find('"', colon + 1);
    if (opening_quote == std::string_view::npos) return "rejected";
    const std::size_t closing_quote = json.find('"', opening_quote + 1);
    if (closing_quote == std::string_view::npos) return "rejected";
    const std::string_view value =
        json.substr(opening_quote + 1, closing_quote - opening_quote - 1);
    return IsSafeRequestId(value) ? std::string(value) : "rejected";
}

std::string EncodeBase64(const std::vector<std::uint8_t>& bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
    std::size_t index = 0;
    while (index + 3U <= bytes.size()) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (static_cast<std::uint32_t>(bytes[index + 1U]) << 8U) |
            static_cast<std::uint32_t>(bytes[index + 2U]);
        encoded.push_back(alphabet[(triple >> 18U) & 0x3FU]);
        encoded.push_back(alphabet[(triple >> 12U) & 0x3FU]);
        encoded.push_back(alphabet[(triple >> 6U) & 0x3FU]);
        encoded.push_back(alphabet[triple & 0x3FU]);
        index += 3U;
    }
    const std::size_t remaining = bytes.size() - index;
    if (remaining == 1U) {
        const std::uint32_t triple = static_cast<std::uint32_t>(bytes[index]) << 16U;
        encoded.push_back(alphabet[(triple >> 18U) & 0x3FU]);
        encoded.push_back(alphabet[(triple >> 12U) & 0x3FU]);
        encoded.push_back('=');
        encoded.push_back('=');
    } else if (remaining == 2U) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (static_cast<std::uint32_t>(bytes[index + 1U]) << 8U);
        encoded.push_back(alphabet[(triple >> 18U) & 0x3FU]);
        encoded.push_back(alphabet[(triple >> 12U) & 0x3FU]);
        encoded.push_back(alphabet[(triple >> 6U) & 0x3FU]);
        encoded.push_back('=');
    }
    return encoded;
}

std::string ResponsePrefix(
    std::string_view marker,
    std::string_view request_id,
    bool success,
    std::string_view result_code) {
    const std::string safe_request_id =
        IsSafeRequestId(request_id) ? std::string(request_id) : "rejected";
    std::ostringstream output;
    output << "{\"schemaVersion\":\"" << kDualBoundLiveViewSchemaVersion
           << "\",\"simulation\":false,\"marker\":\""
           << ::a0::common::protocol_json::JsonEscape(marker)
           << "\",\"requestId\":\"" << safe_request_id << "\",\"success\":"
           << (success ? "true" : "false") << ",\"resultCode\":\"" << result_code
           << "\",\"payload\":";
    return output.str();
}

// `extra_fields` is pre-formatted JSON members, each starting with a comma.
std::string Rejection(
    std::string_view marker,
    std::string_view request_id,
    std::string_view code,
    std::string_view message,
    std::string_view extra_fields = {}) {
    std::ostringstream output;
    output << ResponsePrefix(marker, request_id, false, code) << "{\"detail\":\""
           << ::a0::common::protocol_json::JsonEscape(message) << '"' << extra_fields
           << "}}";
    return output.str();
}

bool IsKnownAlias(std::string_view alias) noexcept {
    return alias == kDualIdentityCameraAliasA || alias == kDualIdentityCameraAliasB;
}

const char* JsonBool(bool value) noexcept { return value ? "true" : "false"; }

} // namespace

DualBoundLiveViewProtocolError::DualBoundLiveViewProtocolError(
    std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

const std::string& DualBoundLiveViewProtocolError::Code() const noexcept {
    return code_;
}

// ---------------------------------------------------------------------------
// Request parsing
// ---------------------------------------------------------------------------

DualBoundLiveViewRequest ParseDualBoundLiveViewRequest(
    std::string_view json, std::string_view expected_marker) {
    const JsonValue root = JsonParser(json).Parse();
    RequireExactFields(root, {
        "schemaVersion", "simulation", "marker", "requestId", "operation", "payload",
    });

    const auto& schema = RequireField(root, "schemaVersion", JsonKind::string).string;
    const bool simulation = RequireField(root, "simulation", JsonKind::boolean).boolean;
    const auto& marker = RequireField(root, "marker", JsonKind::string).string;
    const auto& request_id = RequireField(root, "requestId", JsonKind::string).string;
    const auto& operation = RequireField(root, "operation", JsonKind::string).string;
    const auto& payload = RequireField(root, "payload", JsonKind::object);

    if (schema != kDualBoundLiveViewSchemaVersion || simulation ||
        marker != expected_marker) {
        ProtocolFailure(
            "DualBoundLiveViewProtocolRequired",
            "request is not the bound Live View v1 protocol for this host");
    }
    if (!IsSafeRequestId(request_id)) {
        ProtocolFailure("InvalidRequestId", "a bounded safe request ID is required");
    }

    DualBoundLiveViewRequest request;
    request.request_id = request_id;

    const auto require_session_id = [&payload]() {
        const auto& value = RequireField(payload, "sessionId", JsonKind::string).string;
        if (!IsSafeSessionId(value)) {
            ProtocolFailure("InvalidSessionId", "a 32 hex character session ID is required");
        }
        return value;
    };
    const auto require_alias = [&payload]() {
        const auto& value = RequireField(payload, "cameraAlias", JsonKind::string).string;
        if (!IsKnownAlias(value)) {
            ProtocolFailure("UnknownCameraAlias", "the rig defines only CAM-A and CAM-B");
        }
        return value;
    };

    if (operation == "start-bound-live-view" || operation == "get-bound-live-view-frame") {
        RequireExactFields(payload, {"sessionId", "cameraAlias"});
        request.session_id = require_session_id();
        request.camera_alias = require_alias();
        request.operation = operation == "start-bound-live-view"
            ? DualBoundLiveViewOperation::start
            : DualBoundLiveViewOperation::get_frame;
        return request;
    }
    if (operation == "stop-bound-live-view") {
        RequireExactFields(payload, {"sessionId"});
        request.session_id = require_session_id();
        request.operation = DualBoundLiveViewOperation::stop;
        return request;
    }
    ProtocolFailure("UnsupportedOperation", "bound Live View operation is unsupported");
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

struct DualBoundLiveViewController::CloseOutcome {
    // True when the stop was commanded and the adapter then read back as fully
    // closed. Anything less keeps the binding out of use.
    bool verified{};
    // The Live View stream is stopped (commanded, or the adapter no longer
    // holds a Live View at all).
    bool live_view_stopped{};
    // The adapter reports no SDK source open.
    bool sdk_source_closed{};
};

DualBoundLiveViewController::DualBoundLiveViewController(
    DualBindingSdkAdapter& adapter, DualBoundLiveViewOptions options)
    : adapter_(adapter), options_(std::move(options)) {
    if (!options_.host_remaining_milliseconds) {
        throw std::invalid_argument(
            "bound Live View requires the host's remaining lifetime");
    }
}

bool DualBoundLiveViewController::IsBoundLiveViewRequest(
    std::string_view request_json) noexcept {
    try {
        const JsonValue root = JsonParser(request_json).Parse();
        if (root.kind != JsonKind::object) return false;
        const auto found = root.object.find("schemaVersion");
        return found != root.object.end() && found->second.kind == JsonKind::string &&
            found->second.string == kDualBoundLiveViewSchemaVersion;
    } catch (...) {
        return false;
    }
}

std::string DualBoundLiveViewController::UnavailableResponse(
    std::string_view request_json, std::string_view marker) noexcept {
    try {
        return Rejection(
            marker, TryExtractSafeRequestId(request_json), "BoundLiveViewUnavailable",
            "this host is not configured to serve a bound Live View");
    } catch (...) {
        return {};
    }
}

std::uint64_t DualBoundLiveViewController::HostRemainingMilliseconds() const noexcept {
    // A provider that throws is a host whose lifetime is unknown. Unknown is
    // treated as none left, never as plenty.
    try {
        return options_.host_remaining_milliseconds();
    } catch (...) {
        return 0;
    }
}

std::uint64_t DualBoundLiveViewController::CaptureAdmissionRemainingMilliseconds()
    const noexcept {
    const std::uint64_t remaining = HostRemainingMilliseconds();
    return remaining > kBoundLiveViewCaptureAdmissionReserveMilliseconds
        ? remaining - kBoundLiveViewCaptureAdmissionReserveMilliseconds
        : 0;
}

std::uint64_t DualBoundLiveViewController::NowMilliseconds() const noexcept {
    try {
        if (options_.monotonic_milliseconds) return options_.monotonic_milliseconds();
    } catch (...) {
        // Falls through to the real clock below: a failing test clock must not
        // silence the idle timeout.
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

DualBoundLiveViewController::CloseOutcome DualBoundLiveViewController::StopAndClose()
    noexcept {
    // One attempt at each step, in this order, with no retry. Close is not
    // attempted after a failed stop: closing a source that may still be
    // streaming is the thing the stop-before-close order exists to prevent. The
    // binding is invalidated instead, and the owner's cancel-binding is the one
    // terminal cleanup.
    bool stop_ok = false;
    bool close_ok = false;
    try { stop_ok = adapter_.StopLiveView(active_ordinal_); } catch (...) {}
    if (stop_ok) {
        try { close_ok = adapter_.CloseCandidateSession(active_ordinal_); } catch (...) {}
    }
    const DualBindingSdkOpenState after = adapter_.InspectOpenState();
    CloseOutcome outcome;
    outcome.live_view_stopped = stop_ok || !after.live_view_open;
    outcome.sdk_source_closed = !after.source_open;
    outcome.verified = stop_ok && close_ok && after.IsClosed();
    return outcome;
}

std::string DualBoundLiveViewController::InvalidatedResponse(
    std::string_view marker,
    std::string_view request_id,
    const DualBoundLiveViewBindingPort& port,
    const CloseOutcome* stop_attempt) const noexcept {
    try {
        const DualBindingSdkOpenState open = adapter_.InspectOpenState();
        const bool live_view_stopped =
            stop_attempt != nullptr ? stop_attempt->live_view_stopped : !open.live_view_open;
        const bool sdk_source_closed =
            stop_attempt != nullptr ? stop_attempt->sdk_source_closed : !open.source_open;
        std::ostringstream output;
        output << ResponsePrefix(marker, request_id, false, "BindingInvalidated")
               << "{\"sessionId\":\""
               << ::a0::common::protocol_json::JsonEscape(port.BoundLiveViewSessionId())
               << "\",\"state\":\"Invalid\",\"invalidationReason\":\""
               << DualIdentityInvalidationReasonName(port.BoundLiveViewInvalidationReason())
               << "\",\"liveViewStopped\":" << JsonBool(live_view_stopped)
               << ",\"sdkSourceClosed\":" << JsonBool(sdk_source_closed);
        // A close was attempted, so something was open when it began.
        if (stop_attempt != nullptr) output << ",\"wasOpen\":true";
        output << "}}";
        return output.str();
    } catch (...) {
        return {};
    }
}

std::string DualBoundLiveViewController::FailClosed(
    std::string_view marker,
    std::string_view request_id,
    DualBoundLiveViewBindingPort& port,
    DualIdentityInvalidationReason reason,
    const CloseOutcome* stop_attempt) noexcept {
    port.InvalidateBoundLiveViewBinding(reason);
    return InvalidatedResponse(marker, request_id, port, stop_attempt);
}

std::string DualBoundLiveViewController::Handle(
    std::string_view request_json,
    std::string_view marker,
    DualBoundLiveViewBindingPort& port) noexcept {
    const std::string extracted_request_id = TryExtractSafeRequestId(request_json);
    try {
        const DualBoundLiveViewRequest request =
            ParseDualBoundLiveViewRequest(request_json, marker);

        // The session is compared before the adapter is polled, so a request
        // naming some other session can neither learn this one's state nor
        // consume the invalidation event that this one's next request needs.
        const std::string session_id = port.BoundLiveViewSessionId();
        if (session_id.empty() || request.session_id != session_id) {
            return Rejection(
                marker, request.request_id, "SessionMismatch",
                "the named binding session is not the one this agent is serving");
        }
        // Any request in this schema for the live session shows the client is
        // still there, whatever the request is answered with.
        last_request_milliseconds_ = NowMilliseconds();

        const DualIdentityInvalidationReason observed = adapter_.PollInvalidation();
        if (observed != DualIdentityInvalidationReason::None) {
            port.InvalidateBoundLiveViewBinding(observed);
        }
        const DualIdentitySessionBindingState binding_state = port.BoundLiveViewBindingState();
        if (binding_state == DualIdentitySessionBindingState::Invalid) {
            return InvalidatedResponse(marker, request.request_id, port);
        }
        if (binding_state != DualIdentitySessionBindingState::Ready) {
            return Rejection(
                marker, request.request_id, "BindingNotReady",
                "a bound Live View requires a completed Ready binding");
        }

        switch (request.operation) {
            case DualBoundLiveViewOperation::start:
                return HandleStart(request, marker, port);
            case DualBoundLiveViewOperation::get_frame:
                return HandleGetFrame(request, marker, port);
            case DualBoundLiveViewOperation::stop:
                return HandleStop(request, marker, port);
        }
        return Rejection(
            marker, request.request_id, "UnsupportedOperation",
            "bound Live View operation is unsupported");
    } catch (const DualBoundLiveViewProtocolError& error) {
        return Rejection(marker, extracted_request_id, error.Code(), error.what());
    } catch (const DualIdentitySessionBindingError& error) {
        // The binding core's vocabulary is reported unchanged, as in the binding
        // protocol.
        return Rejection(marker, extracted_request_id, error.Code(), error.what());
    } catch (...) {
        try {
            return Rejection(
                marker, extracted_request_id, "AgentFailure",
                "bound Live View failed to handle the request");
        } catch (...) {
            return {};
        }
    }
}

std::string DualBoundLiveViewController::HandleStart(
    const DualBoundLiveViewRequest& request,
    std::string_view marker,
    DualBoundLiveViewBindingPort& port) {
    // Throws the core's BindingNotReady / UnknownCameraAlias; no SDK is touched.
    const std::size_t ordinal = port.BoundLiveViewCandidateOrdinal(request.camera_alias);

    if (HostRemainingMilliseconds() < kBoundLiveViewMinimumHostRemainingMilliseconds) {
        return Rejection(
            marker, request.request_id, "HostLifetimeInsufficient",
            "the host has too little lifetime left to start a Live View",
            ",\"captureAdmissionRemainingMs\":" +
                std::to_string(CaptureAdmissionRemainingMilliseconds()));
    }

    // Both our own bookkeeping and the adapter must say nothing is open. A
    // second request is refused rather than treated as a switch: stopping the
    // first is the client's call, so an exchange that only works because the
    // agent guessed what the client meant cannot hide a wrong order.
    if (state_ != DualBoundLiveViewState::Closed ||
        !adapter_.InspectOpenState().IsClosed()) {
        return Rejection(
            marker, request.request_id, "BoundLiveViewAlreadyOpen",
            "an SDK source or Live View is already open; stop it first");
    }

    bool started = false;
    try {
        started = adapter_.StartLiveView(ordinal);
    } catch (...) {
        return FailClosed(
            marker, request.request_id, port, DualIdentityInvalidationReason::SdkError);
    }
    if (!started) {
        // The adapter reports an SDK exception by queueing an invalidation, so a
        // refusal that comes with an event is a failure of the binding itself.
        const DualIdentityInvalidationReason observed = adapter_.PollInvalidation();
        if (observed != DualIdentityInvalidationReason::None) {
            return FailClosed(marker, request.request_id, port, observed);
        }
        // A refusal with nothing queued is only trusted if nothing was left open.
        if (!adapter_.InspectOpenState().IsClosed()) {
            return FailClosed(
                marker, request.request_id, port, DualIdentityInvalidationReason::SdkError);
        }
        return Rejection(
            marker, request.request_id, "LiveViewStartFailed",
            "the SDK refused to start Live View for this camera");
    }

    state_ = DualBoundLiveViewState::Live;
    active_alias_ = request.camera_alias;
    active_ordinal_ = ordinal;
    ++counters_.start_count;

    std::ostringstream output;
    output << ResponsePrefix(marker, request.request_id, true, "BoundLiveViewStarted")
           << "{\"sessionId\":\""
           << ::a0::common::protocol_json::JsonEscape(request.session_id)
           << "\",\"cameraAlias\":\""
           << ::a0::common::protocol_json::JsonEscape(request.camera_alias)
           << "\",\"state\":\"Live\",\"captureAdmissionRemainingMs\":"
           << CaptureAdmissionRemainingMilliseconds() << "}}";
    return output.str();
}

std::string DualBoundLiveViewController::HandleGetFrame(
    const DualBoundLiveViewRequest& request,
    std::string_view marker,
    DualBoundLiveViewBindingPort& port) {
    if (state_ != DualBoundLiveViewState::Live || request.camera_alias != active_alias_) {
        return Rejection(
            marker, request.request_id, "BoundLiveViewNotActive",
            "no bound Live View is running for this camera");
    }

    std::vector<std::uint8_t> frame;
    try {
        frame = adapter_.ReadLiveViewFrame(active_ordinal_);
    } catch (...) {
        return FailClosed(
            marker, request.request_id, port, DualIdentityInvalidationReason::SdkError);
    }
    if (frame.empty()) {
        // Nothing yet is not an error: a camera that has only just started has no
        // frame to give, and stopping on that would make every start flaky. An
        // SDK failure reads as an empty frame too, but it queues an invalidation,
        // which is what tells the two apart.
        const DualIdentityInvalidationReason observed = adapter_.PollInvalidation();
        if (observed != DualIdentityInvalidationReason::None) {
            return FailClosed(marker, request.request_id, port, observed);
        }
        return Rejection(
            marker, request.request_id, "LiveViewFrameUnavailable",
            "the SDK returned no Live View frame");
    }
    if (frame.size() > kMaximumBindingLiveViewFrameBytes) {
        // Refused, never truncated. The Live View stays open.
        return Rejection(
            marker, request.request_id, "LiveViewFrameTooLarge",
            "the Live View frame exceeds the bounded preview size");
    }
    ++counters_.frame_count;

    // The frame exists only in this string. It is not logged, journaled or kept.
    std::ostringstream output;
    output << ResponsePrefix(marker, request.request_id, true, "BoundLiveViewFrame")
           << "{\"sessionId\":\""
           << ::a0::common::protocol_json::JsonEscape(request.session_id)
           << "\",\"cameraAlias\":\""
           << ::a0::common::protocol_json::JsonEscape(request.camera_alias)
           << "\",\"frameBytes\":" << frame.size() << ",\"frameBase64\":\""
           << EncodeBase64(frame) << "\",\"captureAdmissionRemainingMs\":"
           << CaptureAdmissionRemainingMilliseconds() << "}}";
    return output.str();
}

std::string DualBoundLiveViewController::HandleStop(
    const DualBoundLiveViewRequest& request,
    std::string_view marker,
    DualBoundLiveViewBindingPort& port) {
    bool was_open = false;
    if (state_ == DualBoundLiveViewState::Live) {
        was_open = true;
        const CloseOutcome outcome = StopAndClose();
        if (!outcome.verified) {
            return FailClosed(
                marker, request.request_id, port, DualIdentityInvalidationReason::SdkError,
                &outcome);
        }
        state_ = DualBoundLiveViewState::Closed;
        active_alias_.clear();
        ++counters_.stop_count;
    } else if (!adapter_.InspectOpenState().IsClosed()) {
        // Something is open that this controller did not open, so it cannot name
        // which candidate to close. Guessing could close the wrong body or leave
        // the right one open; the binding is invalidated and cancelled instead.
        return FailClosed(
            marker, request.request_id, port, DualIdentityInvalidationReason::SdkError);
    }

    // Idempotent: with nothing open this is a success that says so.
    std::ostringstream output;
    output << ResponsePrefix(marker, request.request_id, true, "BoundLiveViewStopped")
           << "{\"sessionId\":\""
           << ::a0::common::protocol_json::JsonEscape(request.session_id)
           << "\",\"state\":\"Closed\",\"liveViewStopped\":true,\"sdkSourceClosed\":true"
              ",\"wasOpen\":" << JsonBool(was_open) << ",\"captureAdmissionRemainingMs\":"
           << CaptureAdmissionRemainingMilliseconds() << "}}";
    return output.str();
}

void DualBoundLiveViewController::OnIdle(DualBoundLiveViewBindingPort& port) noexcept {
    try {
        if (state_ != DualBoundLiveViewState::Live) return;
        const std::uint64_t now = NowMilliseconds();
        if (now < last_request_milliseconds_ ||
            now - last_request_milliseconds_ < kBoundLiveViewIdleStopMilliseconds) {
            return;
        }
        if (port.BoundLiveViewBindingState() != DualIdentitySessionBindingState::Ready) {
            return;
        }
        const CloseOutcome outcome = StopAndClose();
        if (outcome.verified) {
            state_ = DualBoundLiveViewState::Closed;
            active_alias_.clear();
            ++counters_.idle_stop_count;
            return;
        }
        port.InvalidateBoundLiveViewBinding(DualIdentityInvalidationReason::SdkError);
    } catch (...) {
        // Nothing to report to: OnIdle has no response. A failure here leaves the
        // state as it was, and the next request or the host's end deals with it.
    }
}

bool DualBoundLiveViewController::IsFullyClosed() const noexcept {
    return state_ == DualBoundLiveViewState::Closed &&
        adapter_.InspectOpenState().IsClosed();
}

void DualBoundLiveViewController::NotifyBindingReset() noexcept {
    state_ = DualBoundLiveViewState::Closed;
    active_alias_.clear();
}

void DualBoundLiveViewController::NotifyBindingInvalidated() noexcept {
    state_ = DualBoundLiveViewState::Terminal;
    active_alias_.clear();
}

DualBoundLiveViewState DualBoundLiveViewController::State() const noexcept {
    return state_;
}

DualBoundLiveViewCounters DualBoundLiveViewController::Counters() const noexcept {
    return counters_;
}

} // namespace a0::phase0
