#pragma once
// Read-only camera settings channel for the development preview (#416 stage A).
//
// This is a separate gate from AllowedD810PreviewRequest and from the still
// capture gates; none of them is widened. The gate here is built so that it
// cannot write: SetDevicePropValue (0x1016), the write command kinds and
// WRITE_DATA are refused by shape in every state, and no write branch exists.
// It admits only DeviceReady (0x90C8), GetDevicePropValue of the Live state
// (0x1015 with 0xD1A2), GetDevicePropDesc (0x1014) for a code of the settings
// table, and the READ_DATA / END_DATA_TRANSFER of the transfer it opened.
#include "a0/phase0/experimental/d810_settings.hpp"
#include "a0/phase0/experimental/wpd_live_view_transfer.hpp"
#include <PortableDevice.h>
#include <WpdMtpExtensions.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace a0::phase0::experimental {
// Provisional limits (unverified on a body): one operation of the transfer function is bounded by
// 8 reads of at most 4 KiB.
inline constexpr std::size_t kSettingsMaxArguments = 8;

// An execute-style request: `form` selects the command kind (Start: without a
// data phase, Frame: data phase to read), the rest is the operation and its
// 32 bit arguments.
inline WpdValues BuildSettingsExecute(WpdLiveViewOperation form, DWORD opcode, std::initializer_list<DWORD> arguments) {
    auto request = BuildWpdLiveViewOperation(form);
    if (request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, opcode) != S_OK) throw std::runtime_error("settings opcode");
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> parameters;
    if (request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, &parameters) != S_OK)
        throw std::runtime_error("settings params");
    for (const auto argument : arguments) {
        PROPVARIANT value{};
        value.vt = VT_UI4;
        value.ulVal = argument;
        if (parameters->Add(&value) != S_OK) throw std::runtime_error("settings parameter");
    }
    return request;
}
inline WpdValues BuildSettingsDeviceReady() { return BuildSettingsExecute(WpdLiveViewOperation::Start, 0x90c8, {}); }
inline WpdValues BuildSettingsLiveState() { return BuildSettingsExecute(WpdLiveViewOperation::Frame, 0x1015, {kD810LiveStateCode}); }
inline WpdValues BuildSettingsPropDesc(std::uint16_t code) { return BuildSettingsExecute(WpdLiveViewOperation::Frame, 0x1014, {code}); }

// --- Shape of a request ---------------------------------------------------------
struct SettingsRequestShape {
    SettingsCommandKind kind{SettingsCommandKind::Other};
    std::uint32_t opcode{};
    std::vector<std::uint32_t> arguments;
    std::wstring context;
};
inline SettingsCommandKind SettingsKindOf(IPortableDeviceValues* request) {
    GUID category{};
    DWORD id{};
    if (request->GetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, &category) != S_OK ||
        request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, &id) != S_OK) return SettingsCommandKind::Other;
    const PROPERTYKEY command{category, id};
    if (IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE)) return SettingsCommandKind::ExecuteWithoutData;
    if (IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ)) return SettingsCommandKind::ExecuteWithDataToRead;
    if (IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE)) return SettingsCommandKind::ExecuteWithDataToWrite;
    if (IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_READ_DATA)) return SettingsCommandKind::ReadData;
    if (IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_WRITE_DATA)) return SettingsCommandKind::WriteData;
    if (IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER)) return SettingsCommandKind::EndDataTransfer;
    return SettingsCommandKind::Other;
}
// Nothing when the request cannot be read in full (missing or mistyped field).
inline std::optional<SettingsRequestShape> InspectSettingsRequest(IPortableDeviceValues* request) {
    if (!request) return {};
    SettingsRequestShape shape;
    shape.kind = SettingsKindOf(request);
    switch (shape.kind) {
    case SettingsCommandKind::ExecuteWithoutData:
    case SettingsCommandKind::ExecuteWithDataToRead:
    case SettingsCommandKind::ExecuteWithDataToWrite: {
        DWORD opcode{};
        Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> parameters;
        DWORD count{};
        if (request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, &opcode) != S_OK ||
            request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, &parameters) != S_OK ||
            !parameters || parameters->GetCount(&count) != S_OK || count > kSettingsMaxArguments) return {};
        shape.opcode = opcode;
        for (DWORD i = 0; i < count; ++i) {
            PROPVARIANT value{};
            const bool typed = parameters->GetAt(i, &value) == S_OK && value.vt == VT_UI4;
            if (typed) shape.arguments.push_back(value.ulVal);
            PropVariantClear(&value);
            if (!typed) return {};
        }
        return shape;
    }
    case SettingsCommandKind::ReadData:
    case SettingsCommandKind::WriteData:
    case SettingsCommandKind::EndDataTransfer: {
        PWSTR raw{};
        if (request->GetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, &raw) != S_OK || !raw) return {};
        shape.context = raw;
        CoTaskMemFree(raw);
        return shape;
    }
    default: return shape;
    }
}

// A response code the body uses to refuse a request: a 16 bit code in the
// standard error range 0x2002..0x2FFF or the vendor range 0xA000..0xAFFF.
// 0x2019 (DeviceBusy) is left out: it says the body could not take the request
// now, not that a transfer is over.
inline bool IsMtpRefusalCode(DWORD code) {
    if (code == 0x2019) return false;
    return (code >= 0x2002 && code <= 0x2fff) || (code >= 0xa000 && code <= 0xafff);
}
// The body answered with an MTP refusal code: both the dispatch and the command
// succeeded and the code is present and in the range above. A failed HRESULT, a
// missing code, a code outside the range (0, 0xFFFFFFFF, more than 16 bits), 0x2019
// or an exception is not an explicit refusal.
inline bool IsExplicitMtpRejection(const WpdLiveViewCommandResult& result) {
    return result.error == WpdLiveViewResultError::MtpRejected && result.dispatch_status == S_OK &&
        result.command_status == S_OK && IsMtpRefusalCode(result.mtp_response);
}

// --- The gate ----------------------------------------------------------------------
// One instance belongs to one open session. States: Idle, BeginPending (a read
// operation was admitted, its reply not yet seen), ReadOpen (a transfer is open
// and the gate holds its context, taken from the begin reply), EndPending, and
// Closed (after an ambiguous transport failure or an END that did not close the
// transfer cleanly: nothing more). An END the body explicitly refused with an MTP
// refusal code (IsExplicitMtpRejection), for a transfer that delivered no data, is
// taken to mean the transfer is over and the gate returns to Idle. That is an
// inference (unverified on a body), kept to the narrow code range above.
class D810SettingsReadGate {
public:
    bool Admit(IPortableDeviceValues* request) {
        const bool admitted = Decide(request);
        ++(admitted ? admitted_ : rejected_);
        return admitted;
    }
    // The reply to the request that Admit let through.
    void Observe(const WpdLiveViewExchange& reply) {
        if (state_ == State::BeginPending) {
            const auto begin = InspectWpdLiveViewTransferBegin(reply.status, reply.results.Get());
            // A context is kept even when the size hint was invalid, so the
            // transfer that exists can still be ended exactly once.
            transfer_dirty_ = false;
            if (begin.context.empty()) state_ = State::Idle;
            else { context_ = begin.context; state_ = State::ReadOpen; }
        } else if (state_ == State::ReadOpen) {
            // The reply to a READ_DATA of the open transfer. Any data, or a reply
            // that cannot be read, makes the transfer unclean.
            const auto chunk = InspectWpdLiveViewReadChunk(reply.status, reply.results.Get(), WpdLiveViewMaxChunkBytes);
            if (!chunk.result.Ok() || !chunk.bytes.empty()) transfer_dirty_ = true;
        } else if (state_ == State::EndPending) {
            context_.clear();
            const auto end = InspectWpdLiveViewCommand(reply.status, reply.results.Get(), true);
            // A clean END reopens the gate. So does an explicit MTP refusal of the
            // END of a transfer that delivered nothing (a body that does not know
            // the property answers so; that this closes the transfer is inferred).
            // Anything else stays closed.
            const bool reopen = !failed_ && (end.Ok() || (!transfer_dirty_ && IsExplicitMtpRejection(end)));
            state_ = reopen ? State::Idle : State::Closed;
            transfer_dirty_ = false;
        }
    }
    // The transport threw: the request may or may not have reached the body.
    // Nothing more is admitted, except the END of a transfer that is already
    // open (the transfer function sends it once to close that transfer).
    void TransportFailed() {
        failed_ = true;
        if (state_ != State::ReadOpen) state_ = State::Closed;
    }
    bool Closed() const { return state_ == State::Closed; }
    bool TransferOpen() const { return state_ == State::ReadOpen || state_ == State::BeginPending || state_ == State::EndPending; }
    std::size_t Admitted() const { return admitted_; }
    std::size_t Rejected() const { return rejected_; }
private:
    enum class State { Idle, BeginPending, ReadOpen, EndPending, Closed };
    bool Decide(IPortableDeviceValues* request) {
        if (state_ == State::Closed) return false;
        const auto shape = InspectSettingsRequest(request);
        if (!shape) return false;
        switch (state_) {
        case State::Idle: {
            const auto operation = ClassifySettingsExecute(shape->kind, shape->opcode, shape->arguments);
            if (!operation) return false;
            if (*operation != SettingsOperation::DeviceReady) state_ = State::BeginPending;
            return true;
        }
        case State::ReadOpen:
            // The next step of this transfer, nothing else.
            if (shape->kind == SettingsCommandKind::ReadData && !failed_ && shape->context == context_) return true;
            if (shape->kind == SettingsCommandKind::EndDataTransfer && shape->context == context_) { state_ = State::EndPending; return true; }
            return false;
        default: return false;
        }
    }
    State state_{State::Idle};
    std::wstring context_;
    std::size_t admitted_{}, rejected_{};
    bool failed_{};
    bool transfer_dirty_{};
};

// --- One camera's read -----------------------------------------------------------------
struct D810CameraSettingsRead {
    std::string alias;
    D810SettingsSnapshot readings;
    // Empty when every table entry was attempted. Otherwise why reading stopped
    // (the caller only closes the session after that).
    std::string stop_reason;
    std::optional<unsigned> live_state;
    bool Complete() const { return stop_reason.empty(); }
};

class WpdSettingsReadSession {
public:
    using Transport = std::function<WpdLiveViewExchange(IPortableDeviceValues*)>;
    using Log = std::function<void(const std::string&)>;
    // may_continue: deadline, stop request and cancellation, checked before every
    // request. may_cleanup: only that the session is still ours, so an END can
    // still be sent after a deadline.
    WpdSettingsReadSession(Transport transport, std::function<bool()> may_continue, std::function<bool()> may_cleanup):
        transport_(std::move(transport)), may_continue_(std::move(may_continue)), may_cleanup_(std::move(may_cleanup)) {
        if (!transport_ || !may_continue_ || !may_cleanup_) throw std::invalid_argument("missing settings read admission");
    }
    WpdSettingsReadSession(const WpdSettingsReadSession&) = delete;
    WpdSettingsReadSession& operator=(const WpdSettingsReadSession&) = delete;
    const D810SettingsReadGate& Gate() const { return gate_; }
    bool Poisoned() const { return poisoned_; }

    // DeviceReady, then the Live state (must be off), then one description per
    // table entry. Never sends a write, never retries an item. One call per
    // session: a second call sends nothing and reports every row as not_read.
    D810CameraSettingsRead ReadAll(const std::string& alias, const Log& log, std::span<const D810SettingSpec> specs = kD810SettingSpecs) {
        D810CameraSettingsRead out;
        out.alias = alias;
        const auto say = [&](const std::string& line) { if (log) log(line); };
        const auto stop = [&](const char* reason, std::size_t next) {
            out.stop_reason = reason;
            for (std::size_t i = next; i < specs.size(); ++i) out.readings.push_back(UnavailableD810Setting(specs[i].id, "not_read"));
            say(alias + " settings_read_stopped reason=" + reason);
            return out;
        };
        if (read_all_called_) return stop("already_read", 0);
        read_all_called_ = true;
        if (!may_continue_()) return stop("not_started", 0);
        const auto ready = DeviceReady();
        if (ready != Ready::Ok) return stop(ready == Ready::Interrupted ? "interrupted" : "device_not_ready", 0);
        const auto live = LiveState();
        if (!live.value) return stop(live.interrupted ? "interrupted" : "live_state_unreadable", 0);
        out.live_state = *live.value;
        say(alias + " settings_live_state value=" + std::to_string(*live.value));
        if (*live.value != 0) return stop(*live.value == 1 ? "live_view_on" : "live_state_unexpected", 0);
        for (std::size_t i = 0; i < specs.size(); ++i) {
            if (poisoned_ || !may_continue_()) return stop(poisoned_ ? "not_read_after_failure" : "interrupted", i);
            out.readings.push_back(ReadSetting(specs[i]));
            say(FormatD810SettingLogLine(alias, specs[i], out.readings.back()));
            if (!stop_detail_.empty()) {
                char head[48]{};
                std::snprintf(head, sizeof head, " settings_stop code=0x%04x", static_cast<unsigned>(specs[i].code));
                say(alias + head + stop_detail_);
            }
            if (interrupted_) return stop("interrupted", i + 1);
        }
        return out;
    }

private:
    enum class Ready { Ok, NotReady, Interrupted };
    struct Live { std::optional<unsigned> value; bool interrupted{}; };
    class TransferChannel final: public WpdLiveViewCommandChannel {
    public:
        explicit TransferChannel(WpdSettingsReadSession& session): session_(session) {}
    protected:
        WpdLiveViewExchange Dispatch(IPortableDeviceValues* request) override { return session_.Send(request); }
    private:
        WpdSettingsReadSession& session_;
    };
    WpdLiveViewExchange Send(IPortableDeviceValues* request) {
        if (!gate_.Admit(request)) throw std::runtime_error("settings request not admitted");
        WpdLiveViewExchange reply;
        try { reply = transport_(request); }
        catch (...) { gate_.TransportFailed(); poisoned_ = true; throw; }
        gate_.Observe(reply);
        return reply;
    }
    Ready DeviceReady() {
        for (;;) {
            if (!may_continue_()) return Ready::Interrupted;
            WpdLiveViewExchange reply;
            try { reply = Send(BuildSettingsDeviceReady().Get()); }
            catch (...) { poisoned_ = true; return Ready::NotReady; }
            const auto result = InspectWpdLiveViewCommand(reply.status, reply.results.Get(), true);
            if (result.Ok()) return Ready::Ok;
            // 0x2019: DeviceBusy. Poll again until the deadline, as the Live and
            // still paths do; any other answer is not readiness.
            if (result.error != WpdLiveViewResultError::MtpRejected || result.mtp_response != 0x2019) { poisoned_ = true; return Ready::NotReady; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    WpdLiveViewTransferResult Transfer(std::function<WpdValues()> begin, std::uint64_t max_payload) {
        TransferChannel channel(*this);
        WpdLiveViewTransferOptions options;
        options.may_continue = may_continue_;
        options.may_cleanup = may_cleanup_;
        options.begin_request = std::move(begin);
        options.max_payload_bytes = max_payload;
        options.max_chunk_bytes = static_cast<DWORD>(std::min<std::uint64_t>(max_payload, 4096));
        options.max_read_commands = 8;
        return ReadWpdLiveViewPayload(channel, options);
    }
    Live LiveState() {
        const auto result = Transfer([] { return BuildSettingsLiveState(); }, 16);
        if (!result.Ok()) {
            poisoned_ = true;
            return {{}, result.error == WpdLiveViewTransferError::NotAdmitted || result.error == WpdLiveViewTransferError::Interrupted};
        }
        if (result.payload.size() != 1) { poisoned_ = true; return {}; }
        return {static_cast<unsigned>(result.payload[0]), false};
    }
    // Begin succeeded, no byte was received, the END was sent and the body
    // answered it with an explicit MTP refusal.
    static bool RejectedAtEnd(const WpdLiveViewTransferResult& result) {
        return (result.error == WpdLiveViewTransferError::EmptyPayload || result.error == WpdLiveViewTransferError::IncompletePayload) &&
            result.begin_dispatched && result.received_bytes == 0 && result.end_attempted && result.end_result &&
            IsExplicitMtpRejection(*result.end_result);
    }
    // What the END of a transfer that stopped the read looked like, in the words of
    // the Live log (end_dispatch / end_command / end_mtp), plus the END error
    // number. Empty when no END was attempted.
    static std::string DescribeEnd(const WpdLiveViewTransferResult& result) {
        std::string text;
        char part[96]{};
        if (result.end_result) {
            std::snprintf(part, sizeof part, " end_dispatch=0x%lx end_command=0x%lx end_mtp=0x%lx",
                static_cast<unsigned long>(result.end_result->dispatch_status), static_cast<unsigned long>(result.end_result->command_status),
                static_cast<unsigned long>(result.end_result->mtp_response));
            text += part;
        }
        if (result.end_error != WpdLiveViewTransferError::None) text += " end_error=" + std::to_string(static_cast<int>(result.end_error));
        return text;
    }
    D810SettingReading ReadSetting(const D810SettingSpec& spec) {
        stop_detail_.clear();
        const auto code = spec.code;
        const auto result = Transfer([code] { return BuildSettingsPropDesc(code); }, kD810PropDescMaxBytes);
        if (!result.Ok()) {
            const auto mtp = result.command_failure ? result.command_failure->mtp_response : 0u;
            if (result.error == WpdLiveViewTransferError::NotAdmitted || result.error == WpdLiveViewTransferError::Interrupted) {
                interrupted_ = true;
                poisoned_ = true;
                return UnavailableD810Setting(spec.id, "interrupted", mtp);
            }
            // The body refused the begin with an MTP code and no transfer exists:
            // later items can still be asked.
            if (result.error == WpdLiveViewTransferError::BeginRejected && !result.end_attempted && result.command_failure &&
                IsExplicitMtpRejection(*result.command_failure))
                return UnavailableD810Setting(spec.id, "mtp_rejected", mtp);
            // The body opened a transfer, delivered no data and refused its END
            // with an MTP code (it does not know the property): the transfer is
            // over, and the END code is kept in the row.
            if (RejectedAtEnd(result))
                return UnavailableD810Setting(spec.id, "mtp_rejected_at_end", result.end_result->mtp_response);
            if (result.error == WpdLiveViewTransferError::PayloadLimitExceeded && result.end_confirmed)
                return UnavailableD810Setting(spec.id, "too_large");
            // The body opened a transfer, sent nothing (a short READ of an unknown
            // size) and accepted the END: an empty answer, nothing is left open.
            if (result.error == WpdLiveViewTransferError::EmptyPayload && result.received_bytes == 0 && result.end_confirmed && result.end_result)
                return UnavailableD810Setting(spec.id, "empty_payload", result.end_result->mtp_response);
            // Anything else (an HRESULT failure, an exception, partial data, an END
            // that is not an explicit refusal) may have left a data phase in an
            // unknown state, so nothing more is sent on this session. The row keeps
            // the MTP code of the failed command, or else that of the END, and the
            // log line that follows it carries what the END answered.
            poisoned_ = true;
            stop_detail_ = DescribeEnd(result);
            const auto stop_mtp = result.command_failure ? result.command_failure->mtp_response : result.end_result ? result.end_result->mtp_response : 0u;
            if (result.error == WpdLiveViewTransferError::BeginRejected && result.command_failure &&
                result.command_failure->error == WpdLiveViewResultError::CommandFailed)
                return UnavailableD810Setting(spec.id, "command_failed", stop_mtp);
            return UnavailableD810Setting(spec.id, "transfer_failed_" + std::to_string(static_cast<int>(result.error)), stop_mtp);
        }
        const auto parsed = ParseD810PropDesc(result.payload, spec);
        D810SettingReading reading;
        reading.id = spec.id;
        reading.mtp_response = result.end_result ? result.end_result->mtp_response : 0;
        reading.desc = parsed.desc;
        reading.available = parsed.Ok();
        if (!parsed.Ok()) reading.reason = D810PropDescErrorToken(parsed.error);
        return reading;
    }
    Transport transport_;
    std::function<bool()> may_continue_, may_cleanup_;
    D810SettingsReadGate gate_;
    bool poisoned_{}, interrupted_{}, read_all_called_{};
    std::string stop_detail_;
};
}
