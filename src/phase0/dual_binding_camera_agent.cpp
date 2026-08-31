#include "a0/phase0/dual_binding_camera_agent.hpp"

#include "a0/common/protocol_json.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <set>
#include <sstream>
#include <utility>

namespace a0::phase0 {
namespace {

using ::a0::common::protocol_json::JsonKind;
using ::a0::common::protocol_json::JsonValue;

[[noreturn]] void ProtocolFailure(std::string code, std::string message);

struct DualBindingJsonFailure {
    [[noreturn]] static void Fail(std::string code, std::string message) {
        ProtocolFailure(std::move(code), std::move(message));
    }
};

using JsonParser = ::a0::common::protocol_json::BasicJsonParser<DualBindingJsonFailure>;

[[nodiscard]] const JsonValue& RequireField(
    const JsonValue& object, std::string_view name, JsonKind kind) {
    return ::a0::common::protocol_json::RequireFieldWith<DualBindingJsonFailure>(object, name, kind);
}

[[noreturn]] void ProtocolFailure(std::string code, std::string message) {
    throw DualBindingCameraAgentProtocolError(std::move(code), std::move(message));
}

void RequireExactFields(const JsonValue& object, const std::set<std::string>& expected) {
    if (object.kind != JsonKind::object || object.object.size() != expected.size()) {
        ProtocolFailure(
            "UnexpectedField",
            "binding protocol object has missing or unexpected fields");
    }
    for (const auto& [name, ignored] : object.object) {
        (void)ignored;
        if (!expected.contains(name)) {
            ProtocolFailure(
                "UnexpectedField",
                "binding protocol object contains an unexpected field");
        }
    }
}

bool IsSafeRequestId(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '.' || character == '-' || character == '_';
    });
}

// Same shape as the v2 transaction id: 32 lowercase-or-uppercase hex characters
// that are not all zero. Fixing the shape means a malformed or truncated id is
// refused by the parser, before it can be compared against a live session.
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

std::size_t ParseCandidateOrdinal(const JsonValue& payload) {
    // The parser keeps a number as its raw lexeme, so "0" and "0.0" and "1e0"
    // are distinguishable here. An ordinal is an index into this session's
    // candidate list: a fractional or exponent form is a malformed request, not
    // something to round.
    const auto& value = RequireField(payload, "candidateOrdinal", JsonKind::number);
    if (value.string.empty() ||
        value.string.find_first_of(".eE-+") != std::string::npos) {
        ProtocolFailure("InvalidCandidateOrdinal", "candidate ordinal is not a whole index");
    }
    std::uint64_t parsed{};
    const auto result = std::from_chars(
        value.string.data(), value.string.data() + value.string.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.string.data() + value.string.size() || parsed > 4096U) {
        ProtocolFailure("InvalidCandidateOrdinal", "candidate ordinal is not a bounded index");
    }
    return static_cast<std::size_t>(parsed);
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

std::string ResponsePrefix(
    std::string_view request_id, bool success, std::string_view result_code) {
    const std::string safe_request_id =
        IsSafeRequestId(request_id) ? std::string(request_id) : "rejected";
    std::ostringstream output;
    output << "{\"schemaVersion\":\"" << kDualBindingCameraAgentSchemaVersion
           << "\",\"simulation\":false,\"marker\":\"" << kDualBindingCameraAgentMarker
           << "\",\"requestId\":\"" << safe_request_id << "\",\"success\":"
           << (success ? "true" : "false") << ",\"resultCode\":\"" << result_code
           << "\",\"payload\":";
    return output.str();
}

std::string_view StateName(DualIdentitySessionBindingState state) noexcept {
    switch (state) {
        case DualIdentitySessionBindingState::None: return "None";
        case DualIdentitySessionBindingState::CollectingCandidates:
            return "CollectingCandidates";
        case DualIdentitySessionBindingState::AwaitingQuiesce: return "AwaitingQuiesce";
        case DualIdentitySessionBindingState::Ready: return "Ready";
        case DualIdentitySessionBindingState::Invalid: return "Invalid";
    }
    return "Invalid";
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

std::string HexOf(const std::uint8_t* bytes, std::size_t count) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(count * 2U);
    for (std::size_t index = 0; index < count; ++index) {
        hex.push_back(digits[(bytes[index] >> 4U) & 0x0FU]);
        hex.push_back(digits[bytes[index] & 0x0FU]);
    }
    return hex;
}

// 12 random bytes fixed per dispatcher, so two sessions from two processes can
// never collide, plus a per-dispatcher counter so two sessions from one process
// never do either. The randomness is not a secret -- the session id is a
// correlation token, not a capability -- it is there so a client cannot guess
// the next id and so a restarted agent never reproduces an old one.
std::string SessionIdSeed() {
    std::array<std::uint8_t, 12> bytes{};
    const NTSTATUS status = BCryptGenRandom(
        nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        ProtocolFailure("AgentFailure", "binding session identifier source is unavailable");
    }
    return HexOf(bytes.data(), bytes.size());
}

std::string ProtocolRejection(
    std::string_view request_id, std::string_view code, std::string_view message) {
    std::ostringstream output;
    output << ResponsePrefix(request_id, false, code) << "{\"detail\":\""
           << ::a0::common::protocol_json::JsonEscape(message) << "\"}}";
    return output.str();
}

} // namespace

DualBindingCameraAgentProtocolError::DualBindingCameraAgentProtocolError(
    std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

const std::string& DualBindingCameraAgentProtocolError::Code() const noexcept {
    return code_;
}

// ---------------------------------------------------------------------------
// Fake SDK candidate / Live View lifecycle
// ---------------------------------------------------------------------------

DualBindingFakeSdkAdapter::DualBindingFakeSdkAdapter(
    DualBindingFakeSdkOptions options) noexcept
    : options_(std::move(options)) {}

std::vector<std::string> DualBindingFakeSdkAdapter::EnumerateCandidates() {
    ++enumeration_count_;
    live_view_active_.assign(options_.candidate_count, false);
    session_closed_.assign(options_.candidate_count, false);
    if (options_.invalidation_during_enumeration !=
        DualIdentityInvalidationReason::None) {
        options_.pending_invalidation = options_.invalidation_during_enumeration;
        // One-shot: the operator's retry has to be able to succeed, or the
        // rejection would be permanent rather than a fail-closed round trip.
        options_.invalidation_during_enumeration = DualIdentityInvalidationReason::None;
    }

    std::vector<std::string> tokens;
    tokens.reserve(options_.candidate_count);
    for (std::size_t ordinal = 0; ordinal < options_.candidate_count; ++ordinal) {
        if (options_.empty_source_object_tokens) {
            tokens.emplace_back();
            continue;
        }
        if (options_.duplicate_source_object_tokens) {
            tokens.emplace_back("fake-source-object-shared");
            continue;
        }
        // The enumeration count is part of the token on purpose: re-enumerating
        // yields different tokens for the same bodies, which is what makes
        // "enumeration order is not identity" visible to anything that tried to
        // treat a token as a stable name.
        tokens.push_back(
            "fake-source-object-" + std::to_string(enumeration_count_) + "-" +
            std::to_string(ordinal));
    }
    return tokens;
}

bool DualBindingFakeSdkAdapter::StartLiveView(std::size_t ordinal) {
    if (ordinal >= live_view_active_.size()) return false;
    if (options_.fail_start_live_view) return false;
    const std::size_t already_active = ActiveLiveViewCount();
    if (already_active > 0 && !live_view_active_[ordinal]) {
        // The protocol layer is supposed to have stopped the other one first.
        // Record it rather than quietly allowing the overlap, so a test can
        // prove the rule is enforced above rather than merely intended.
        ++concurrent_live_view_violations_;
        return false;
    }
    live_view_active_[ordinal] = true;
    return true;
}

bool DualBindingFakeSdkAdapter::StopLiveView(std::size_t ordinal) {
    if (ordinal >= live_view_active_.size()) return false;
    if (options_.fail_stop_live_view) return false;
    // Stopping an already-stopped Live View answers the same question -- "is it
    // stopped?" -- so it succeeds rather than erroring.
    live_view_active_[ordinal] = false;
    return true;
}

std::vector<std::uint8_t> DualBindingFakeSdkAdapter::ReadLiveViewFrame(
    std::size_t ordinal) {
    if (ordinal >= live_view_active_.size() || !live_view_active_[ordinal]) return {};
    std::vector<std::uint8_t> frame(options_.live_view_frame_bytes, 0);
    for (std::size_t index = 0; index < frame.size(); ++index) {
        frame[index] = static_cast<std::uint8_t>((index + ordinal) & 0xFFU);
    }
    return frame;
}

bool DualBindingFakeSdkAdapter::CloseCandidateSession(std::size_t ordinal) {
    if (ordinal >= session_closed_.size()) return false;
    if (options_.fail_close_candidate_session) return false;
    session_closed_[ordinal] = true;
    return true;
}

bool DualBindingFakeSdkAdapter::EndBindingSession(
    std::chrono::seconds) noexcept {
    ++end_binding_session_count_;
    if (options_.fail_end_binding_session) return false;
    std::fill(live_view_active_.begin(), live_view_active_.end(), false);
    std::fill(session_closed_.begin(), session_closed_.end(), true);
    return true;
}

DualIdentityInvalidationReason DualBindingFakeSdkAdapter::PollInvalidation() {
    const DualIdentityInvalidationReason reason = options_.pending_invalidation;
    options_.pending_invalidation = DualIdentityInvalidationReason::None;
    return reason;
}

void DualBindingFakeSdkAdapter::RaiseInvalidation(
    DualIdentityInvalidationReason reason) noexcept {
    options_.pending_invalidation = reason;
}

std::size_t DualBindingFakeSdkAdapter::EnumerationCount() const noexcept {
    return enumeration_count_;
}

std::size_t DualBindingFakeSdkAdapter::ActiveLiveViewCount() const noexcept {
    return static_cast<std::size_t>(
        std::count(live_view_active_.begin(), live_view_active_.end(), true));
}

std::size_t DualBindingFakeSdkAdapter::ConcurrentLiveViewViolationCount()
    const noexcept {
    return concurrent_live_view_violations_;
}

std::size_t DualBindingFakeSdkAdapter::ClosedCandidateSessionCount() const noexcept {
    return static_cast<std::size_t>(
        std::count(session_closed_.begin(), session_closed_.end(), true));
}

std::size_t DualBindingFakeSdkAdapter::EndBindingSessionCount() const noexcept {
    return end_binding_session_count_;
}

// ---------------------------------------------------------------------------
// Request parsing
// ---------------------------------------------------------------------------

DualBindingCameraAgentRequest ParseDualBindingCameraAgentRequest(
    std::string_view json) {
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

    if (schema != kDualBindingCameraAgentSchemaVersion || simulation ||
        marker != kDualBindingCameraAgentMarker) {
        ProtocolFailure(
            "DualBindingProtocolRequired",
            "request is not the required Dual binding v1 protocol");
    }
    if (!IsSafeRequestId(request_id)) {
        ProtocolFailure("InvalidRequestId", "a bounded safe request ID is required");
    }

    DualBindingCameraAgentRequest request;
    request.request_id = request_id;

    const auto require_session_id = [&payload]() {
        const auto& value = RequireField(payload, "sessionId", JsonKind::string).string;
        if (!IsSafeSessionId(value)) {
            ProtocolFailure("InvalidSessionId", "a 32 hex character session ID is required");
        }
        return value;
    };

    if (operation == "begin-binding") {
        RequireExactFields(payload, {"cameraMode"});
        const auto& mode = RequireField(payload, "cameraMode", JsonKind::string).string;
        if (mode != "DualCamera") {
            ProtocolFailure("UnsupportedCameraMode", "binding is defined for DualCamera only");
        }
        request.operation = DualBindingCameraAgentOperation::begin_binding;
        return request;
    }
    if (operation == "start-candidate-live-view") {
        RequireExactFields(payload, {"sessionId", "candidateOrdinal"});
        request.session_id = require_session_id();
        request.candidate_ordinal = ParseCandidateOrdinal(payload);
        request.operation = DualBindingCameraAgentOperation::start_candidate_live_view;
        return request;
    }
    if (operation == "get-candidate-live-view-frame") {
        RequireExactFields(payload, {"sessionId", "candidateOrdinal"});
        request.session_id = require_session_id();
        request.candidate_ordinal = ParseCandidateOrdinal(payload);
        request.operation = DualBindingCameraAgentOperation::get_candidate_live_view_frame;
        return request;
    }
    if (operation == "confirm-alias") {
        RequireExactFields(payload, {"sessionId", "candidateOrdinal", "cameraAlias"});
        request.session_id = require_session_id();
        request.candidate_ordinal = ParseCandidateOrdinal(payload);
        const auto& alias = RequireField(payload, "cameraAlias", JsonKind::string).string;
        // Bounded here; which aliases are legal is the binding core's call, so
        // both protocols report the same UnknownCameraAlias for a wrong one.
        if (alias.empty() || alias.size() > 32) {
            ProtocolFailure("UnknownCameraAlias", "camera alias is missing or unbounded");
        }
        request.camera_alias = alias;
        request.operation = DualBindingCameraAgentOperation::confirm_alias;
        return request;
    }
    if (operation == "complete-binding") {
        RequireExactFields(payload, {"sessionId", "confirmedAtUtc"});
        request.session_id = require_session_id();
        const auto& confirmed =
            RequireField(payload, "confirmedAtUtc", JsonKind::string).string;
        if (confirmed.empty() || confirmed.size() > 64) {
            ProtocolFailure("ConfirmedAtMissing", "a bounded confirmedAtUtc is required");
        }
        request.confirmed_at_utc = confirmed;
        request.operation = DualBindingCameraAgentOperation::complete_binding;
        return request;
    }
    if (operation == "activate-capture") {
        RequireExactFields(payload, {"sessionId"});
        request.session_id = require_session_id();
        request.operation = DualBindingCameraAgentOperation::activate_capture;
        return request;
    }
    if (operation == "cancel-binding") {
        RequireExactFields(payload, {"sessionId"});
        request.session_id = require_session_id();
        request.operation = DualBindingCameraAgentOperation::cancel_binding;
        return request;
    }
    ProtocolFailure("UnsupportedOperation", "binding protocol operation is unsupported");
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

DualBindingCameraAgentDispatcher::DualBindingCameraAgentDispatcher(
    std::shared_ptr<DualBindingSdkAdapter> adapter,
    bool capture_transition_enabled) noexcept
    : adapter_(std::move(adapter)),
      capture_transition_enabled_(capture_transition_enabled) {}

std::string DualBindingCameraAgentDispatcher::Handle(
    std::string_view request_json) noexcept {
    const std::string extracted_request_id = TryExtractSafeRequestId(request_json);
    try {
        const DualBindingCameraAgentRequest request =
            ParseDualBindingCameraAgentRequest(request_json);
        if (adapter_ == nullptr) {
            // Fail closed rather than inventing an empty candidate list: an
            // operator must never be shown a binding session that no SDK is
            // behind.
            return ProtocolRejection(
                request.request_id, "SdkUnavailable",
                "no SDK adapter is attached to this binding agent");
        }
        if (request.operation == DualBindingCameraAgentOperation::begin_binding) {
            return HandleBeginBinding(request);
        }

        // Session-scoped operations. The identifier is compared before the
        // adapter is polled, so a request naming some other session can neither
        // learn this one's state nor consume the invalidation event that this
        // one's next request needs to see.
        if (session_id_.empty() || request.session_id != session_id_) {
            ++safety_counters_.rejected_stale_session_count;
            return ProtocolRejection(
                request.request_id, "SessionMismatch",
                "the named binding session is not the one this agent is serving");
        }

        // Cancellation is allowed even after a typed invalidation. Its job is
        // to release resources owned by this exact session; refusing cleanup
        // because the binding is already untrustworthy could leave Live View
        // and the SDK module open until the host lifetime expires.
        if (request.operation == DualBindingCameraAgentOperation::cancel_binding) {
            return HandleCancelBinding(request);
        }
        const DualIdentityInvalidationReason observed = adapter_->PollInvalidation();
        if (observed != DualIdentityInvalidationReason::None) {
            InvalidateSession(observed);
        }
        if (binding_.State() == DualIdentitySessionBindingState::Invalid) {
            return InvalidatedRejection(request.request_id);
        }

        switch (request.operation) {
            case DualBindingCameraAgentOperation::start_candidate_live_view:
                return HandleStartCandidateLiveView(request);
            case DualBindingCameraAgentOperation::get_candidate_live_view_frame:
                return HandleGetCandidateLiveViewFrame(request);
            case DualBindingCameraAgentOperation::confirm_alias:
                return HandleConfirmAlias(request);
            case DualBindingCameraAgentOperation::complete_binding:
                return HandleCompleteBinding(request);
            case DualBindingCameraAgentOperation::activate_capture:
                return HandleActivateCapture(request);
            case DualBindingCameraAgentOperation::cancel_binding:
                break;
            case DualBindingCameraAgentOperation::begin_binding:
                break;
        }
        return ProtocolRejection(
            request.request_id, "UnsupportedOperation",
            "binding protocol operation is unsupported");
    } catch (const DualBindingCameraAgentProtocolError& error) {
        return ProtocolRejection(extracted_request_id, error.Code(), error.what());
    } catch (const DualIdentitySessionBindingError& error) {
        // The binding core's vocabulary is reported unchanged. Remapping it
        // would give the same refusal two names depending on which side of the
        // seam noticed it.
        return ProtocolRejection(extracted_request_id, error.Code(), error.what());
    } catch (const std::exception&) {
        return ProtocolRejection(
            extracted_request_id, "AgentFailure", "binding agent failed to handle the request");
    } catch (...) {
        // Handle() is noexcept: anything not derived from std::exception
        // (a third-party or SDK-thrown non-standard exception) must still be
        // turned into a protocol rejection here, matching the sibling
        // catch(...) in dual_hardware_camera_agent.cpp, instead of escaping
        // this function and calling std::terminate().
        return ProtocolRejection(
            extracted_request_id, "AgentFailure", "binding agent failed to handle the request");
    }
}

void DualBindingCameraAgentDispatcher::InvalidateSession(
    DualIdentityInvalidationReason reason) noexcept {
    binding_.Invalidate(reason);
    if (invalidation_reason_ == DualIdentityInvalidationReason::None) {
        // First reason wins, matching the binding core: it is the one that
        // explains why the binding stopped being trustworthy.
        invalidation_reason_ = reason;
    }
    active_live_view_ordinal_.reset();
    quiesced_ordinals_.clear();
}

std::string DualBindingCameraAgentDispatcher::InvalidatedRejection(
    std::string_view request_id) const {
    std::ostringstream output;
    output << ResponsePrefix(request_id, false, "BindingInvalidated")
           << "{\"sessionId\":\"" << session_id_ << "\",\"state\":\"Invalid\""
           << ",\"invalidationReason\":\""
           << DualIdentityInvalidationReasonName(invalidation_reason_) << "\"}}";
    return output.str();
}

std::string DualBindingCameraAgentDispatcher::HandleBeginBinding(
    const DualBindingCameraAgentRequest& request) {
    // Discard the previous session before anything can fail, so a refused
    // begin-binding cannot leave an older session addressable. The binding core
    // does the same for its own state; this is the wire-level half of it.
    session_id_.clear();
    active_live_view_ordinal_.reset();
    quiesced_ordinals_.clear();
    assigned_ordinals_.clear();
    invalidation_reason_ = DualIdentityInvalidationReason::None;
    capture_transition_requested_ = false;

    // Drain any event left over from the session just discarded. It describes a
    // session nobody can name any more, so acting on it would invalidate the new
    // one for something that happened before it existed.
    (void)adapter_->PollInvalidation();

    const std::vector<std::string> tokens = adapter_->EnumerateCandidates();
    std::vector<DualIdentityCandidate> candidates;
    candidates.reserve(tokens.size());
    for (std::size_t ordinal = 0; ordinal < tokens.size(); ++ordinal) {
        candidates.push_back(DualIdentityCandidate{ordinal, tokens[ordinal]});
    }
    binding_.BeginBinding(candidates);

    // Anything the adapter reports now happened during or after enumeration, so
    // the candidates just collected may already be stale. Fail closed: the
    // operator re-runs begin-binding and the second attempt starts clean.
    const DualIdentityInvalidationReason observed = adapter_->PollInvalidation();
    if (observed != DualIdentityInvalidationReason::None) {
        InvalidateSession(observed);
        return ProtocolRejection(
            request.request_id, "BindingInvalidated",
            "the SDK reported " +
                std::string(DualIdentityInvalidationReasonName(observed)) +
                " while candidates were being collected");
    }

    if (session_id_seed_.empty()) {
        session_id_seed_ = SessionIdSeed();
    }
    ++session_counter_;
    std::ostringstream counter;
    counter.width(8);
    counter.fill('0');
    counter << std::hex << (session_counter_ & 0xFFFFFFFFULL);
    session_id_ = session_id_seed_ + counter.str();
    ++safety_counters_.binding_session_count;

    std::ostringstream output;
    output << ResponsePrefix(request.request_id, true, "BindingSessionStarted")
           << "{\"sessionId\":\"" << session_id_ << "\",\"state\":\""
           << StateName(binding_.State()) << "\",\"candidateOrdinals\":[";
    for (std::size_t ordinal = 0; ordinal < candidates.size(); ++ordinal) {
        if (ordinal > 0) output << ',';
        output << ordinal;
    }
    output << "]}}";
    return output.str();
}

std::string DualBindingCameraAgentDispatcher::HandleStartCandidateLiveView(
    const DualBindingCameraAgentRequest& request) {
    if (binding_.State() != DualIdentitySessionBindingState::CollectingCandidates) {
        return ProtocolRejection(
            request.request_id, "BindingNotCollecting",
            "Live View is only offered while candidates are still being assigned");
    }
    if (request.candidate_ordinal >= kDualIdentityRequiredCandidateCount) {
        return ProtocolRejection(
            request.request_id, "UnknownCandidateOrdinal",
            "the candidate ordinal is not part of this binding session");
    }
    if (std::find(assigned_ordinals_.begin(), assigned_ordinals_.end(),
                  request.candidate_ordinal) != assigned_ordinals_.end()) {
        // This candidate's Live View was stopped and its SDK session closed when
        // the operator assigned it. Reopening either would undo the quiesce the
        // completed binding rests on, so the answer is no rather than a silent
        // re-open.
        return ProtocolRejection(
            request.request_id, "CandidateAlreadyAssigned",
            "this candidate was already assigned and its session closed");
    }
    if (active_live_view_ordinal_.has_value() &&
        *active_live_view_ordinal_ != request.candidate_ordinal) {
        // Switching bodies is a normal part of comparison. Nikon keeps the
        // Source open after Live View stops, so both the stream and Source must
        // quiesce before another candidate is opened under the retained Module.
        const std::size_t previous_ordinal = *active_live_view_ordinal_;
        if (!adapter_->StopLiveView(previous_ordinal)) {
            InvalidateSession(DualIdentityInvalidationReason::SdkError);
            return ProtocolRejection(
                request.request_id, "LiveViewStopFailed",
                "the previously running Live View could not be stopped");
        }
        active_live_view_ordinal_.reset();
        if (!adapter_->CloseCandidateSession(previous_ordinal)) {
            InvalidateSession(DualIdentityInvalidationReason::SdkError);
            return ProtocolRejection(
                request.request_id, "SdkSessionCloseFailed",
                "the previously viewed candidate SDK session could not be closed");
        }
        if (std::find(quiesced_ordinals_.begin(), quiesced_ordinals_.end(),
                      previous_ordinal) == quiesced_ordinals_.end()) {
            quiesced_ordinals_.push_back(previous_ordinal);
        }
    }
    if (!active_live_view_ordinal_.has_value()) {
        if (!adapter_->StartLiveView(request.candidate_ordinal)) {
            return ProtocolRejection(
                request.request_id, "LiveViewStartFailed",
                "the SDK refused to start Live View for this candidate");
        }
        active_live_view_ordinal_ = request.candidate_ordinal;
        quiesced_ordinals_.erase(
            std::remove(quiesced_ordinals_.begin(), quiesced_ordinals_.end(),
                        request.candidate_ordinal),
            quiesced_ordinals_.end());
        ++safety_counters_.live_view_start_count;
    }

    std::ostringstream output;
    output << ResponsePrefix(request.request_id, true, "CandidateLiveViewStarted")
           << "{\"sessionId\":\"" << session_id_ << "\",\"candidateOrdinal\":"
           << request.candidate_ordinal << ",\"liveViewActive\":true}}";
    return output.str();
}

std::string DualBindingCameraAgentDispatcher::HandleGetCandidateLiveViewFrame(
    const DualBindingCameraAgentRequest& request) {
    if (!active_live_view_ordinal_.has_value() ||
        *active_live_view_ordinal_ != request.candidate_ordinal) {
        return ProtocolRejection(
            request.request_id, "LiveViewNotActive",
            "no Live View is running for this candidate");
    }
    const std::vector<std::uint8_t> frame =
        adapter_->ReadLiveViewFrame(request.candidate_ordinal);
    if (frame.empty()) {
        return ProtocolRejection(
            request.request_id, "LiveViewFrameUnavailable",
            "the SDK returned no Live View frame");
    }
    if (frame.size() > kMaximumBindingLiveViewFrameBytes) {
        // Refused, never truncated: the operator decides which body they are
        // looking at from this image, and half an image is a wrong answer
        // waiting to happen.
        return ProtocolRejection(
            request.request_id, "LiveViewFrameTooLarge",
            "the Live View frame exceeds the bounded preview size");
    }
    ++safety_counters_.live_view_frame_count;

    std::ostringstream output;
    output << ResponsePrefix(request.request_id, true, "CandidateLiveViewFrame")
           << "{\"sessionId\":\"" << session_id_ << "\",\"candidateOrdinal\":"
           << request.candidate_ordinal << ",\"frameBytes\":" << frame.size()
           << ",\"frameBase64\":\"" << EncodeBase64(frame) << "\"}}";
    return output.str();
}

std::string DualBindingCameraAgentDispatcher::HandleConfirmAlias(
    const DualBindingCameraAgentRequest& request) {
    // Throws on a duplicate candidate, a duplicate alias, an unknown ordinal or
    // an unknown alias; those are the binding core's decisions and are reported
    // with its codes.
    binding_.ConfirmAlias(request.candidate_ordinal, request.camera_alias);

    // A candidate may already be quiesced because the operator compared both
    // previews before deciding their aliases. Reuse that observed result rather
    // than reopening the Source solely to close it again.
    const bool already_quiesced =
        std::find(quiesced_ordinals_.begin(), quiesced_ordinals_.end(),
                  request.candidate_ordinal) != quiesced_ordinals_.end();
    const bool live_view_stopped =
        already_quiesced || adapter_->StopLiveView(request.candidate_ordinal);
    const bool sdk_session_closed =
        already_quiesced || adapter_->CloseCandidateSession(request.candidate_ordinal);
    if (live_view_stopped && active_live_view_ordinal_.has_value() &&
        *active_live_view_ordinal_ == request.candidate_ordinal) {
        active_live_view_ordinal_.reset();
    }
    // What the SDK actually reported, not what this operation hoped for. This
    // is the only thing that can keep a still-streaming body out of a completed
    // binding: the core refuses CompleteBinding for any candidate it was not
    // told is quiesced, so passing an optimistic true here would let capture
    // start against a Live View that never stopped.
    binding_.ConfirmCandidateQuiesced(
        request.candidate_ordinal, live_view_stopped, sdk_session_closed);
    if (live_view_stopped && sdk_session_closed && !already_quiesced) {
        quiesced_ordinals_.push_back(request.candidate_ordinal);
    }
    assigned_ordinals_.push_back(request.candidate_ordinal);

    if (!live_view_stopped || !sdk_session_closed) {
        // Reported immediately rather than only at complete-binding, so the
        // operator finds out while they are still looking at the body that did
        // not quiesce. The session is deliberately not invalidated here: the
        // core already holds the fact that this candidate is not quiesced and
        // will refuse completion on its own, and inventing a second mechanism
        // for the same rule would leave one of them untested.
        std::ostringstream output;
        output << ResponsePrefix(request.request_id, false, "QuiesceIncomplete")
               << "{\"sessionId\":\"" << session_id_ << "\",\"candidateOrdinal\":"
               << request.candidate_ordinal << ",\"liveViewStopped\":"
               << (live_view_stopped ? "true" : "false") << ",\"sdkSessionClosed\":"
               << (sdk_session_closed ? "true" : "false") << ",\"state\":\""
               << StateName(binding_.State()) << "\"}}";
        return output.str();
    }

    std::ostringstream output;
    output << ResponsePrefix(request.request_id, true, "CandidateAliasConfirmed")
           << "{\"sessionId\":\"" << session_id_ << "\",\"candidateOrdinal\":"
           << request.candidate_ordinal << ",\"cameraAlias\":\""
           << ::a0::common::protocol_json::JsonEscape(request.camera_alias) << "\",\"state\":\""
           << StateName(binding_.State()) << "\"}}";
    return output.str();
}

std::string DualBindingCameraAgentDispatcher::HandleCompleteBinding(
    const DualBindingCameraAgentRequest& request) {
    // Refused unless both aliases are assigned and every candidate is quiesced.
    binding_.CompleteBinding(request.confirmed_at_utc);

    std::ostringstream output;
    output << ResponsePrefix(request.request_id, true, "BindingCompleted")
           << "{\"sessionId\":\"" << session_id_ << "\",\"state\":\""
           << StateName(binding_.State()) << "\",\"evidence\":[";
    // Exactly the ADR-0025 allowlist, straight from the core. No source object,
    // no candidate ordinal, no preview, no serial.
    const auto evidence = binding_.PublishableEvidence();
    for (std::size_t index = 0; index < evidence.size(); ++index) {
        if (index > 0) output << ',';
        output << "{\"cameraAlias\":\"" << ::a0::common::protocol_json::JsonEscape(evidence[index].camera_alias)
               << "\",\"providerId\":\"" << ::a0::common::protocol_json::JsonEscape(evidence[index].provider_id)
               << "\",\"providerVersion\":" << evidence[index].provider_version
               << ",\"confirmedAtUtc\":\""
               << ::a0::common::protocol_json::JsonEscape(evidence[index].confirmed_at_utc)
               << "\",\"invalidationReason\":\""
               << ::a0::common::protocol_json::JsonEscape(evidence[index].invalidation_reason) << "\"}";
    }
    output << "]}}";
    return output.str();
}

std::string DualBindingCameraAgentDispatcher::HandleActivateCapture(
    const DualBindingCameraAgentRequest& request) {
    if (!capture_transition_enabled_) {
        return ProtocolRejection(
            request.request_id, "CaptureTransitionUnavailable",
            "this binding host is not configured to enter a capture host");
    }
    if (binding_.State() != DualIdentitySessionBindingState::Ready) {
        return ProtocolRejection(
            request.request_id, "BindingNotReady",
            "capture activation requires a completed Ready binding");
    }

    capture_transition_requested_ = true;
    std::ostringstream output;
    output << ResponsePrefix(request.request_id, true, "CaptureHostActivated")
           << "{\"sessionId\":\"" << session_id_
           << "\",\"state\":\"Ready\",\"captureHostActivated\":true}}";
    return output.str();
}

std::string DualBindingCameraAgentDispatcher::HandleCancelBinding(
    const DualBindingCameraAgentRequest& request) {
    cancellation_requested_ = true;
    const std::string cancelled_session = session_id_;
    cancellation_succeeded_ = adapter_->EndBindingSession(std::chrono::seconds(10));

    active_live_view_ordinal_.reset();
    quiesced_ordinals_.clear();
    assigned_ordinals_.clear();
    if (cancellation_succeeded_) {
        binding_ = DualIdentitySessionBinding{};
        session_id_.clear();
        invalidation_reason_ = DualIdentityInvalidationReason::None;
        std::ostringstream output;
        output << ResponsePrefix(request.request_id, true, "BindingCancelled")
               << "{\"sessionId\":\"" << cancelled_session
               << "\",\"state\":\"None\",\"liveViewStopped\":true"
                  ",\"sdkSessionClosed\":true,\"sdkSessionEnded\":true}}";
        return output.str();
    }

    InvalidateSession(DualIdentityInvalidationReason::SdkError);
    std::ostringstream output;
    output << ResponsePrefix(request.request_id, false, "BindingCleanupFailed")
           << "{\"sessionId\":\"" << cancelled_session
           << "\",\"state\":\"Invalid\",\"invalidationReason\":\""
           << DualIdentityInvalidationReasonName(invalidation_reason_)
           << "\",\"detail\":\"binding SDK cleanup could not be confirmed\"}}";
    return output.str();
}

std::string DualBindingCameraAgentDispatcher::BoundSourceObjectForCapture(
    std::string_view camera_alias) {
    return binding_.BoundSourceObjectForCapture(camera_alias);
}

DualIdentitySessionBindingState DualBindingCameraAgentDispatcher::BindingState()
    const noexcept {
    return binding_.State();
}

void DualBindingCameraAgentDispatcher::InvalidateCaptureBinding(
    DualIdentityInvalidationReason reason) noexcept {
    if (reason == DualIdentityInvalidationReason::None) return;
    InvalidateSession(reason);
}

bool DualBindingCameraAgentDispatcher::CaptureTransitionRequested() const noexcept {
    return capture_transition_requested_;
}

bool DualBindingCameraAgentDispatcher::CancellationRequested() const noexcept {
    return cancellation_requested_;
}

bool DualBindingCameraAgentDispatcher::CancellationSucceeded() const noexcept {
    return cancellation_succeeded_;
}

DualBindingCameraAgentSafetyCounters
DualBindingCameraAgentDispatcher::SafetyCounters() const noexcept {
    return safety_counters_;
}

void DualBindingCameraAgentDispatcher::OnIdle() noexcept {}

bool DualBindingCameraAgentDispatcher::ShouldStop() const noexcept {
    return capture_transition_requested_ || cancellation_requested_;
}

} // namespace a0::phase0
