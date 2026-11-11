#pragma once
// Camera settings write channel (#416 stage B, ADR-0038 decision 9).
//
// A gate of its own. AllowedD810PreviewRequest, the still capture gates and the
// read-only D810SettingsReadGate are not widened and still refuse 0x1016,
// WITH_DATA_TO_WRITE and WRITE_DATA. Only D810SettingsWriteGate admits a write,
// and only:
//   - SetDevicePropValue (0x1016) as WITH_DATA_TO_WRITE with one argument, the
//     code armed from the confirmed plan, and a total size equal to the type;
//   - after Arm, which needs the plan's exact (code, value), a Live state read
//     of 0 and a fresh description of the code in the same generation (and, for
//     the exposure values, manual mode and ISO auto off read the same way);
//   - once per code per session;
//   - followed by exactly one WRITE_DATA with that context whose byte count,
//     element count and bytes equal the value's encoding, and one END.
// Arm is consumed and the generation advances before the begin reaches the
// transport; the END of a write advances it again, so the next write needs new
// reads. Any refusal, missing reply, exception, timeout or cancel during a write
// retires the gate: nothing more is sent on that session except the END of a
// transfer that is open, and after the I/O watchdog's cancel not even that
// (D810CancelFlag). The read-only gate's "an explicit refusal returns to
// Idle" rule is not used here, and the read gate's refusal test is not shared.
//
// Only targets built with A0_ENABLE_WPD_SETTINGS_WRITE (the CMake option
// A0_BUILD_WPD_SETTINGS_WRITE, OFF by default, and the contract tests) can
// include this header. There is no command-line entry to it.
#if !defined(A0_ENABLE_WPD_SETTINGS_WRITE)
#error "wpd_settings_write.hpp is only for targets built with A0_BUILD_WPD_SETTINGS_WRITE"
#endif
#include "a0/phase0/experimental/d810_settings_plan_chosen.hpp"
#include "a0/phase0/experimental/d810_settings_write.hpp"
#include "a0/phase0/experimental/wpd_settings_read.hpp"
#include <ShlObj.h>
#include <KnownFolders.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace a0::phase0::experimental {
inline constexpr DWORD kSetDevicePropValue = 0x1016;
inline constexpr std::size_t kD810WriteReadMaxBytes = kD810PropDescMaxBytes;

// --- Requests ----------------------------------------------------------------------
inline WpdValues BuildSettingsWriteBegin(std::uint16_t code, std::uint64_t total_bytes) {
    auto request = BuildSettingsExecute(WpdLiveViewOperation::Start, kSetDevicePropValue, {code});
    if (request->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE.fmtid) != S_OK ||
        request->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE.pid) != S_OK ||
        request->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, total_bytes) != S_OK)
        throw std::runtime_error("settings write begin");
    return request;
}
inline WpdValues BuildSettingsWriteData(std::wstring_view context, std::span<const unsigned char> bytes) {
    if (context.empty() || context.size() > WpdLiveViewMaxContextCharacters || bytes.empty() || bytes.size() > 4)
        throw std::invalid_argument("settings write data");
    WpdValues request;
    const std::wstring owned(context);
    std::vector<BYTE> buffer(bytes.begin(), bytes.end());
    if (CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&request)) != S_OK ||
        request->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, WPD_COMMAND_MTP_EXT_WRITE_DATA.fmtid) != S_OK ||
        request->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, WPD_COMMAND_MTP_EXT_WRITE_DATA.pid) != S_OK ||
        request->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, owned.c_str()) != S_OK ||
        request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_WRITE, static_cast<DWORD>(buffer.size())) != S_OK ||
        request->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, buffer.data(), static_cast<DWORD>(buffer.size())) != S_OK)
        throw std::runtime_error("settings write data");
    return request;
}
// The total size a write begin declares, or nothing when absent or mistyped.
inline std::optional<std::uint64_t> SettingsWriteTotalBytes(IPortableDeviceValues* request) {
    PROPVARIANT value{};
    std::optional<std::uint64_t> out;
    if (request && request->GetValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, &value) == S_OK && value.vt == VT_UI8) out = value.uhVal.QuadPart;
    PropVariantClear(&value);
    return out;
}
struct SettingsWriteDataShape {
    DWORD declared_bytes{};
    std::vector<unsigned char> bytes;
};
inline std::optional<SettingsWriteDataShape> InspectSettingsWriteData(IPortableDeviceValues* request) {
    if (!request) return {};
    SettingsWriteDataShape shape;
    PROPVARIANT count{}, data{};
    const bool counted = request->GetValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_WRITE, &count) == S_OK && count.vt == VT_UI4;
    if (counted) shape.declared_bytes = count.ulVal;
    const bool buffered = request->GetValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, &data) == S_OK && data.vt == (VT_VECTOR | VT_UI1) &&
        data.caub.cElems <= 16 && (data.caub.cElems == 0 || data.caub.pElems);
    if (buffered && data.caub.cElems) shape.bytes.assign(data.caub.pElems, data.caub.pElems + data.caub.cElems);
    PropVariantClear(&count);
    PropVariantClear(&data);
    if (!counted || !buffered) return {};
    return shape;
}
// The context of a write begin's reply: command and HRESULT good, a context of
// 1..4096 characters. Empty otherwise.
inline std::wstring SettingsWriteBeginContext(const WpdLiveViewExchange& reply) {
    if (!InspectWpdLiveViewCommand(reply.status, reply.results.Get(), false).Ok()) return {};
    PROPVARIANT value{};
    std::wstring context;
    if (reply.results->GetValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, &value) == S_OK && value.vt == VT_LPWSTR && value.pwszVal) {
        const auto length = wcsnlen_s(value.pwszVal, WpdLiveViewMaxContextCharacters + 1);
        if (length > 0 && length <= WpdLiveViewMaxContextCharacters) context.assign(value.pwszVal, length);
    }
    PropVariantClear(&value);
    return context;
}
// Any context a reply carries, valid or not, so an open transfer can be ended once.
inline std::wstring SettingsAnyContext(const WpdLiveViewExchange& reply) {
    if (!reply.results) return {};
    PROPVARIANT value{};
    std::wstring context;
    if (reply.results->GetValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, &value) == S_OK && value.vt == VT_LPWSTR && value.pwszVal) {
        const auto length = wcsnlen_s(value.pwszVal, WpdLiveViewMaxContextCharacters + 1);
        if (length > 0 && length <= WpdLiveViewMaxContextCharacters) context.assign(value.pwszVal, length);
    }
    PropVariantClear(&value);
    return context;
}
// The reply to WRITE_DATA: command good and exactly `expected` bytes written.
inline bool SettingsWriteDataAccepted(const WpdLiveViewExchange& reply, DWORD expected) {
    if (!InspectWpdLiveViewCommand(reply.status, reply.results.Get(), false).Ok()) return false;
    PROPVARIANT value{};
    const bool ok = reply.results->GetValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_WRITTEN, &value) == S_OK && value.vt == VT_UI4 && value.ulVal == expected;
    PropVariantClear(&value);
    return ok;
}

// --- The gate ------------------------------------------------------------------------
struct D810WriteGateEntry {
    std::uint16_t code{};
    std::int64_t value{};
};
// One latch per operation, shared by the sessions of both cameras. Any failure,
// unknown result or mismatch trips it; once tripped, no session of that
// operation can arm, and the other camera is not opened for writing.
class D810OperationLatch {
public:
    void Trip() { tripped_.store(true); }
    bool Tripped() const { return tripped_.load(); }
private:
    std::atomic<bool> tripped_{false};
};
// What one camera's session may write: the destination, the confirmed plan's
// steps for that camera, and the confirmed ISO auto code (read for the
// precondition).
//
// `cam_a_allowed` is the key to a CAM-A session (ADR-0041), closed by default: a
// configuration for CAM-A without it is invalid and the gate is retired at once.
// The gate does not know the table; the run sets the key only for a chosen plan
// under a table whose chosen-values switch is on, so a CAM-A session cannot be
// built from a copy plan or from a table that does not allow it, whatever the
// plan says. CAM-B needs no key.
struct D810WriteGateConfig {
    D810Camera camera{D810Camera::B};
    std::vector<D810WriteGateEntry> entries;
    std::uint16_t iso_auto_code{};
    D810OperationLatch* latch{};
    bool cam_a_allowed{};
};
// States. In the words of ADR-0038 decision 9: ReadBegun, ReadOpen and
// ReadEnding are "ReadOpen"; WriteBegun and WriteOpen are "WriteBegun";
// WriteSent and WriteEnding are "WriteSent". ReadyPending waits for the reply
// to DeviceReady (0x90C8): OK or 0x2019 (DeviceBusy, "not yet") returns to
// Idle, anything else retires.
enum class D810WriteGateState : std::uint8_t {
    Idle, ReadyPending, ReadBegun, ReadOpen, ReadEnding, WriteBegun, WriteOpen, WriteSent, WriteEnding, Retired,
};
inline const char* D810WriteGateStateToken(D810WriteGateState state) {
    switch (state) {
    case D810WriteGateState::Idle: return "idle";
    case D810WriteGateState::ReadyPending: return "ready_pending";
    case D810WriteGateState::ReadBegun: return "read_begun";
    case D810WriteGateState::ReadOpen: return "read_open";
    case D810WriteGateState::ReadEnding: return "read_ending";
    case D810WriteGateState::WriteBegun: return "write_begun";
    case D810WriteGateState::WriteOpen: return "write_open";
    case D810WriteGateState::WriteSent: return "write_sent";
    case D810WriteGateState::WriteEnding: return "write_ending";
    case D810WriteGateState::Retired: return "retired";
    }
    return "retired";
}

class D810SettingsWriteGate {
public:
    explicit D810SettingsWriteGate(D810WriteGateConfig config = {}): config_(std::move(config)) {
        if (!ValidConfig()) Retire("invalid_config");
    }
    // Arm the next write, keyed by (destination, code, value). Returns the
    // refusal, or nullptr when armed.
    const char* Arm(D810Camera camera, std::uint16_t code, std::int64_t value) {
        const auto* refusal = camera != config_.camera ? "other_destination" : ArmRefusal(code, value);
        if (refusal) return last_refusal_ = refusal;
        armed_ = ArmedWrite{code, value, generation_};
        return nullptr;
    }
    D810Camera Destination() const { return config_.camera; }
    bool Admit(IPortableDeviceValues* request) {
        const bool admitted = Decide(request);
        ++(admitted ? admitted_ : rejected_);
        return admitted;
    }
    // The reply to the request Admit let through.
    void Observe(const WpdLiveViewExchange& reply) {
        switch (state_) {
        case D810WriteGateState::ReadyPending: {
            const auto ready = InspectWpdLiveViewCommand(reply.status, reply.results.Get(), true);
            // 0x2019 (DeviceBusy) is a "not yet"; anything else but OK retires.
            if (ready.Ok() || (ready.error == WpdLiveViewResultError::MtpRejected && ready.mtp_response == 0x2019)) state_ = D810WriteGateState::Idle;
            else Retire("ready_failed");
            return;
        }
        case D810WriteGateState::ReadBegun: {
            const auto begin = InspectWpdLiveViewTransferBegin(reply.status, reply.results.Get(), kD810WriteReadMaxBytes);
            if (begin.result.Ok() && !begin.context.empty()) {
                context_ = begin.context;
                read_bytes_.clear();
                read_dirty_ = false;
                state_ = D810WriteGateState::ReadOpen;
            } else {
                const auto context = begin.context.empty() ? SettingsAnyContext(reply) : begin.context;
                Retire("read_begin_failed", context);
            }
            return;
        }
        case D810WriteGateState::ReadOpen: {
            const auto chunk = InspectWpdLiveViewReadChunk(reply.status, reply.results.Get(), static_cast<DWORD>(kD810WriteReadMaxBytes));
            if (!chunk.result.Ok() || read_bytes_.size() + chunk.bytes.size() > kD810WriteReadMaxBytes) read_dirty_ = true;
            else read_bytes_.insert(read_bytes_.end(), chunk.bytes.begin(), chunk.bytes.end());
            return;
        }
        case D810WriteGateState::ReadEnding: {
            context_.clear();
            const auto end = InspectWpdLiveViewCommand(reply.status, reply.results.Get(), true);
            if (!end.Ok() || read_dirty_ || !Record()) { Retire("read_end_failed"); return; }
            state_ = D810WriteGateState::Idle;
            return;
        }
        case D810WriteGateState::WriteBegun: {
            const auto context = SettingsWriteBeginContext(reply);
            if (context.empty()) { Retire("write_begin_failed", SettingsAnyContext(reply)); return; }
            context_ = context;
            state_ = D810WriteGateState::WriteOpen;
            return;
        }
        case D810WriteGateState::WriteSent:
            data_replied_ = true;
            if (!SettingsWriteDataAccepted(reply, static_cast<DWORD>(write_bytes_.size()))) write_failed_ = true;
            return;
        case D810WriteGateState::WriteEnding: {
            context_.clear();
            const auto end = InspectWpdLiveViewCommand(reply.status, reply.results.Get(), true);
            // After data was sent, any refusal of the END closes the gate,
            // whatever the response code.
            if (!end.Ok() || write_failed_) { Retire("write_end_failed"); return; }
            ++generation_;
            ++writes_ended_;
            state_ = D810WriteGateState::Idle;
            return;
        }
        case D810WriteGateState::Retired:
            cleanup_pending_ = false;
            return;
        default:
            Retire("unexpected_reply");
            return;
        }
    }
    // The transport threw: the request may or may not have reached the body.
    // Only the END of a transfer that is open stays possible.
    void TransportFailed() {
        const bool open = state_ == D810WriteGateState::ReadOpen || state_ == D810WriteGateState::WriteSent;
        Retire("transport_failed", open ? context_ : std::wstring());
    }
    // The I/O watchdog cancelled the device: nothing more, not even an END.
    // Called on the I/O thread only (the session does it when it sees the
    // watchdog's flag); the gate is never touched from another thread.
    void Cancelled() {
        Retire("cancelled");
        cleanup_context_.clear();
    }
    D810WriteGateState State() const { return state_; }
    bool Retired() const { return state_ == D810WriteGateState::Retired; }
    // Write begins handed to the transport ("sent", ADR-0038 decision 8).
    std::size_t WritesSent() const { return writes_sent_; }
    std::size_t WriteDataSent() const { return write_data_sent_; }
    std::size_t WritesEnded() const { return writes_ended_; }
    std::uint64_t Generation() const { return generation_; }
    std::size_t Admitted() const { return admitted_; }
    std::size_t Rejected() const { return rejected_; }
    bool Armed() const { return armed_.has_value(); }
    const char* LastRefusal() const { return last_refusal_; }
    const std::wstring& OpenContext() const { return context_; }
    // The description read in this generation, if any.
    std::optional<D810PropDesc> FreshDesc(std::uint16_t code) const {
        const auto found = descs_.find(code);
        if (found == descs_.end() || found->second.generation != generation_) return {};
        return found->second.desc;
    }
    std::optional<unsigned> FreshLiveState() const {
        if (!live_ || live_->generation != generation_) return {};
        return live_->value;
    }

private:
    struct ArmedWrite { std::uint16_t code; std::int64_t value; std::uint64_t generation; };
    struct SeenDesc { std::uint64_t generation; D810PropDesc desc; };
    struct SeenLive { std::uint64_t generation; unsigned value; };
    bool ValidConfig() const {
        std::set<std::uint16_t> codes;
        if (config_.camera == D810Camera::A && !config_.cam_a_allowed) return false;
        if (!D810WriteItemHasCode(D810WriteItemOf(D810WriteItem::IsoAuto), config_.iso_auto_code)) return false;
        for (const auto& entry : config_.entries) {
            const auto* spec = FindD810WriteItemByCode(entry.code);
            // 0xD100 has no public source for its encoding; the table refuses it too.
            if (!spec || entry.code == 0xd100 || !codes.insert(entry.code).second || !D810ValueFits(spec->type, entry.value)) return false;
            if (spec->fixed && entry.value != spec->fixed_value) return false;
            if (spec->item == D810WriteItem::IsoAuto && entry.code != config_.iso_auto_code) return false;
        }
        return true;
    }
    bool ReadCode(std::uint32_t code) const {
        if (code == kD810ExposureModeCode || code == config_.iso_auto_code) return true;
        for (const auto& entry : config_.entries)
            if (entry.code == code) return true;
        return false;
    }
    const char* ArmRefusal(std::uint16_t code, std::int64_t value) const {
        if (state_ == D810WriteGateState::Retired) return "retired";
        if (state_ != D810WriteGateState::Idle) return "not_idle";
        if (config_.latch && config_.latch->Tripped()) return "operation_latched";
        if (armed_) return "already_armed";
        const auto entry = std::find_if(config_.entries.begin(), config_.entries.end(), [&](const auto& e) { return e.code == code && e.value == value; });
        if (entry == config_.entries.end()) return "not_in_plan";
        if (written_.count(code)) return "already_written";
        const auto live = FreshLiveState();
        if (!live || *live != 0) return "live_not_confirmed_off";
        const auto* spec = FindD810WriteItemByCode(code);
        const auto desc = FreshDesc(code);
        if (!spec || !desc) return "desc_not_reread";
        if (const auto refusal = D810WriteValueRefusal(*spec, *desc, value)) return refusal;
        if (spec->precondition != D810WritePrecondition::LiveOff) {
            const auto mode = FreshDesc(kD810ExposureModeCode);
            if (!mode || mode->current_value != kD810ExposureModeManual) return "mode_not_confirmed_manual";
        }
        if (spec->precondition == D810WritePrecondition::ManualModeIsoAutoOff) {
            const auto iso_auto = FreshDesc(config_.iso_auto_code);
            if (!iso_auto || iso_auto->current_value != kD810IsoAutoOff) return "iso_auto_not_confirmed_off";
        }
        return nullptr;
    }
    bool Decide(IPortableDeviceValues* request) {
        if (state_ == D810WriteGateState::Retired) {
            // The single END of a transfer that was open when the gate retired.
            const auto shape = InspectSettingsRequest(request);
            if (cleanup_context_.empty() || !shape || shape->kind != SettingsCommandKind::EndDataTransfer || shape->context != cleanup_context_) return false;
            cleanup_context_.clear();
            cleanup_pending_ = true;
            return true;
        }
        const auto shape = InspectSettingsRequest(request);
        if (!shape) return Refuse("unreadable_request");
        switch (state_) {
        case D810WriteGateState::Idle: return DecideIdle(request, *shape);
        case D810WriteGateState::ReadOpen:
            if (shape->kind == SettingsCommandKind::ReadData && shape->context == context_) return true;
            if (shape->kind == SettingsCommandKind::EndDataTransfer && shape->context == context_) { state_ = D810WriteGateState::ReadEnding; return true; }
            return Refuse("not_this_read");
        case D810WriteGateState::WriteOpen: {
            // The one WRITE_DATA of this transfer, with the planned bytes.
            if (shape->kind != SettingsCommandKind::WriteData || shape->context != context_) return Refuse("not_this_write");
            const auto data = InspectSettingsWriteData(request);
            if (!data || data->declared_bytes != write_bytes_.size() || data->bytes != write_bytes_) return Refuse("write_data_mismatch");
            ++write_data_sent_;
            state_ = D810WriteGateState::WriteSent;
            return true;
        }
        case D810WriteGateState::WriteSent:
            if (data_replied_ && shape->kind == SettingsCommandKind::EndDataTransfer && shape->context == context_) {
                state_ = D810WriteGateState::WriteEnding;
                return true;
            }
            return Refuse("not_this_end");
        default: return Refuse("reply_pending");
        }
    }
    bool DecideIdle(IPortableDeviceValues* request, const SettingsRequestShape& shape) {
        if (shape.kind == SettingsCommandKind::ExecuteWithoutData) {
            if (shape.opcode != 0x90c8 || !shape.arguments.empty()) return Refuse("operation");
            state_ = D810WriteGateState::ReadyPending;
            return true;
        }
        if (shape.kind == SettingsCommandKind::ExecuteWithDataToRead) {
            if (shape.arguments.size() != 1) return Refuse("operation");
            if (shape.opcode == 0x1015 && shape.arguments[0] == kD810LiveStateCode) read_code_ = 0;
            else if (shape.opcode == 0x1014 && ReadCode(shape.arguments[0])) read_code_ = static_cast<std::uint16_t>(shape.arguments[0]);
            else return Refuse("operation");
            state_ = D810WriteGateState::ReadBegun;
            return true;
        }
        if (shape.kind == SettingsCommandKind::ExecuteWithDataToWrite) {
            if (shape.opcode != kSetDevicePropValue || shape.arguments.size() != 1) return Refuse("operation");
            if (!armed_ || armed_->generation != generation_ || shape.arguments[0] != armed_->code) return Refuse("not_armed");
            const auto* spec = FindD810WriteItemByCode(armed_->code);
            const auto bytes = EncodeD810Value(spec->type, armed_->value);
            if (SettingsWriteTotalBytes(request) != bytes.size()) return Refuse("write_size_mismatch");
            // Consume the arm and advance the generation before the begin
            // reaches the transport.
            written_.insert(armed_->code);
            armed_.reset();
            ++generation_;
            write_bytes_ = bytes;
            write_failed_ = data_replied_ = false;
            ++writes_sent_;
            state_ = D810WriteGateState::WriteBegun;
            return true;
        }
        return Refuse("operation");
    }
    // A refusal while a transfer or a reply is pending retires the gate; in
    // Idle nothing was sent, so the gate stays as it is (an arm is dropped).
    bool Refuse(const char* reason) {
        last_refusal_ = reason;
        if (state_ == D810WriteGateState::Idle) armed_.reset();
        else {
            const bool open = state_ == D810WriteGateState::ReadOpen || state_ == D810WriteGateState::WriteOpen || state_ == D810WriteGateState::WriteSent;
            Retire(reason, open ? context_ : std::wstring());
        }
        return false;
    }
    void Retire(const char* reason, std::wstring cleanup = {}) {
        if (state_ != D810WriteGateState::Retired) cleanup_context_ = std::move(cleanup);
        last_refusal_ = reason;
        armed_.reset();
        state_ = D810WriteGateState::Retired;
        if (config_.latch) config_.latch->Trip();
    }
    // Keep what a clean read said, in this generation.
    bool Record() {
        if (read_code_ == 0) {
            if (read_bytes_.size() != 1) return false;
            live_ = SeenLive{generation_, read_bytes_[0]};
            return true;
        }
        const auto* spec = FindD810Setting(read_code_);
        if (!spec) return false;
        const auto parsed = ParseD810PropDesc(read_bytes_, *spec);
        if (!parsed.Ok()) return false;
        descs_[read_code_] = SeenDesc{generation_, parsed.desc};
        return true;
    }
    D810WriteGateConfig config_;
    D810WriteGateState state_{D810WriteGateState::Idle};
    std::optional<ArmedWrite> armed_;
    std::set<std::uint16_t> written_;
    std::map<std::uint16_t, SeenDesc> descs_;
    std::optional<SeenLive> live_;
    std::uint64_t generation_{};
    std::uint16_t read_code_{};
    std::vector<unsigned char> read_bytes_, write_bytes_;
    bool read_dirty_{}, write_failed_{}, data_replied_{}, cleanup_pending_{};
    std::wstring context_, cleanup_context_;
    std::size_t admitted_{}, rejected_{}, writes_sent_{}, write_data_sent_{}, writes_ended_{};
    const char* last_refusal_{""};
};

// --- One camera's write session ----------------------------------------------------------
struct D810WriteAttempt {
    // Set when nothing was handed to the transport.
    const char* refused{};
    // The begin was handed to the transport (also when that threw).
    bool sent{};
    // Begin, data and END all completed cleanly. Whether the body applied the
    // value is decided by reading it back, not here.
    bool ended{};
};
// The I/O watchdog's signal (ADR-0038 decision 10, M3). The watchdog thread
// only sets the flag (and cancels the device); it never calls the session or
// the gate. The session reads the flag on the I/O thread before and after each
// request, and on seeing it retires the gate with no cleanup: after a cancel
// nothing more is sent on that session, not even the END of an open transfer,
// and a reply that comes back after the cancel is not trusted.
using D810CancelFlag = std::shared_ptr<const std::atomic<bool>>;
inline bool D810CancelRaised(const D810CancelFlag& flag) { return flag && flag->load(); }

class WpdSettingsWriteSession {
public:
    using Transport = WpdSettingsReadSession::Transport;
    WpdSettingsWriteSession(Transport transport, std::function<bool()> may_continue, std::function<bool()> may_cleanup, D810WriteGateConfig config,
        D810CancelFlag cancelled = nullptr):
        transport_(std::move(transport)), may_continue_(std::move(may_continue)), may_cleanup_(std::move(may_cleanup)), gate_(std::move(config)),
        cancelled_(std::move(cancelled)) {
        if (!transport_ || !may_continue_ || !may_cleanup_) throw std::invalid_argument("missing settings write admission");
    }
    WpdSettingsWriteSession(const WpdSettingsWriteSession&) = delete;
    WpdSettingsWriteSession& operator=(const WpdSettingsWriteSession&) = delete;
    const D810SettingsWriteGate& Gate() const { return gate_; }
    bool Poisoned() const { return poisoned_ || gate_.Retired() || D810CancelRaised(cancelled_); }

    bool Ready() {
        for (;;) {
            if (Poisoned() || !may_continue_()) return false;
            WpdLiveViewExchange reply;
            try { reply = Send(BuildSettingsDeviceReady().Get()); }
            catch (...) { poisoned_ = true; return false; }
            const auto result = InspectWpdLiveViewCommand(reply.status, reply.results.Get(), true);
            if (result.Ok()) return true;
            if (gate_.Retired()) { poisoned_ = true; return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    std::optional<unsigned> ReadLiveState() {
        const auto payload = Read([] { return BuildSettingsLiveState(); }, 16);
        if (!payload || payload->size() != 1) { poisoned_ = true; return {}; }
        return (*payload)[0];
    }
    std::optional<D810PropDesc> ReadDesc(std::uint16_t code) {
        const auto* spec = FindD810Setting(code);
        if (!spec) { poisoned_ = true; return {}; }
        const auto payload = Read([code] { return BuildSettingsPropDesc(code); }, kD810PropDescMaxBytes);
        if (!payload) { poisoned_ = true; return {}; }
        const auto parsed = ParseD810PropDesc(*payload, *spec);
        if (!parsed.Ok()) { poisoned_ = true; return {}; }
        return parsed.desc;
    }
    // One write, once. Never retried, never followed by another attempt at the
    // same code on this session.
    D810WriteAttempt Write(std::uint16_t code, std::int64_t value) {
        D810WriteAttempt attempt;
        if (Poisoned()) { attempt.refused = "session_poisoned"; return attempt; }
        if (!may_continue_()) { attempt.refused = "interrupted"; return attempt; }
        if (const auto refusal = gate_.Arm(gate_.Destination(), code, value)) { attempt.refused = refusal; return attempt; }
        const auto* spec = FindD810WriteItemByCode(code);
        const auto bytes = EncodeD810Value(spec->type, value);
        const auto sent_before = gate_.WritesSent();
        WpdLiveViewExchange reply;
        try {
            auto request = BuildSettingsWriteBegin(code, bytes.size());
            reply = Send(request.Get());
        } catch (...) {
            attempt.sent = gate_.WritesSent() > sent_before;
            if (!attempt.sent) attempt.refused = "begin_not_admitted";
            poisoned_ = true;
            return attempt;
        }
        attempt.sent = true;
        if (gate_.State() != D810WriteGateState::WriteOpen) { CleanupEnd(SettingsAnyContext(reply)); poisoned_ = true; return attempt; }
        const auto context = gate_.OpenContext();
        // After the begin the data phase is bounded by the I/O watchdog only,
        // not by the operation deadline (ADR-0038 decision 10, M-6).
        try { Send(BuildSettingsWriteData(context, bytes).Get()); }
        catch (...) { CleanupEnd(context); poisoned_ = true; return attempt; }
        try { Send(BuildWpdLiveViewEndTransfer(context).Get()); }
        catch (...) { poisoned_ = true; return attempt; }
        attempt.ended = gate_.State() == D810WriteGateState::Idle;
        if (!attempt.ended) poisoned_ = true;
        return attempt;
    }

private:
    class TransferChannel final: public WpdLiveViewCommandChannel {
    public:
        explicit TransferChannel(WpdSettingsWriteSession& session): session_(session) {}
    protected:
        WpdLiveViewExchange Dispatch(IPortableDeviceValues* request) override { return session_.Send(request); }
    private:
        WpdSettingsWriteSession& session_;
    };
    // The watchdog's flag, seen on this (the I/O) thread: the gate retires
    // with no cleanup left.
    bool CancelSeen() {
        if (!D810CancelRaised(cancelled_)) return false;
        gate_.Cancelled();
        poisoned_ = true;
        return true;
    }
    WpdLiveViewExchange Send(IPortableDeviceValues* request) {
        if (CancelSeen()) throw std::runtime_error("settings write cancelled");
        if (!gate_.Admit(request)) throw std::runtime_error("settings write request not admitted");
        WpdLiveViewExchange reply;
        try { reply = transport_(request); }
        catch (...) {
            if (!CancelSeen()) gate_.TransportFailed();
            poisoned_ = true;
            throw;
        }
        if (CancelSeen()) throw std::runtime_error("settings write cancelled");
        gate_.Observe(reply);
        return reply;
    }
    // The END of a transfer the gate retired on, once, when still ours.
    void CleanupEnd(const std::wstring& context) {
        if (context.empty() || !may_cleanup_()) return;
        try { Send(BuildWpdLiveViewEndTransfer(context).Get()); }
        catch (...) {}
    }
    std::optional<std::vector<unsigned char>> Read(std::function<WpdValues()> begin, std::uint64_t max_payload) {
        if (Poisoned() || !may_continue_()) return {};
        TransferChannel channel(*this);
        WpdLiveViewTransferOptions options;
        options.may_continue = may_continue_;
        options.may_cleanup = may_cleanup_;
        options.begin_request = std::move(begin);
        options.max_payload_bytes = max_payload;
        options.max_chunk_bytes = static_cast<DWORD>(std::min<std::uint64_t>(max_payload, 4096));
        options.max_read_commands = 8;
        auto result = ReadWpdLiveViewPayload(channel, options);
        if (!result.Ok() || gate_.State() != D810WriteGateState::Idle) return {};
        return std::move(result.payload);
    }
    Transport transport_;
    std::function<bool()> may_continue_, may_cleanup_;
    D810SettingsWriteGate gate_;
    D810CancelFlag cancelled_;
    bool poisoned_{};
};

// --- Ownership of the cameras (ADR-0038 decision 6, M-4) -------------------------------------
enum class D810CameraUse : int { None = 0, Live = 1, Capture = 2, Settings = 3 };
// One atomic for who uses the cameras. Moves are compare-and-swap only, so a
// request that does not find the expected owner sends nothing.
class D810CameraOwnership {
public:
    bool TryAcquire(D810CameraUse use) {
        int expected = static_cast<int>(D810CameraUse::None);
        return use != D810CameraUse::None && value_.compare_exchange_strong(expected, static_cast<int>(use));
    }
    bool Release(D810CameraUse use) {
        int expected = static_cast<int>(use);
        return value_.compare_exchange_strong(expected, static_cast<int>(D810CameraUse::None));
    }
    D810CameraUse Current() const { return static_cast<D810CameraUse>(value_.load()); }
private:
    std::atomic<int> value_{static_cast<int>(D810CameraUse::None)};
};

// --- Files ------------------------------------------------------------------------------------
// The record lives in %LOCALAPPDATA%\A0CameraStitcher\settings-write, resolved
// with SHGetKnownFolderPath. When that fails there is no fallback (no current
// directory, no temporary folder): the caller refuses to write.
inline std::optional<std::filesystem::path> ProductD810SettingsWriteFolder() {
    PWSTR raw{};
    const bool ok = SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)) && raw;
    std::optional<std::filesystem::path> folder;
    if (ok) folder = std::filesystem::path(raw) / L"A0CameraStitcher" / L"settings-write";
    CoTaskMemFree(raw);
    return folder;
}
enum class D810FolderState { Absent, Trusted, Untrusted };
// Every component of an absolute, normalized path must be a directory and not a
// reparse point (as for the camera control marker). With `create`, missing
// components are made first.
inline D810FolderState CheckD810Folder(const std::filesystem::path& folder, bool create) {
    if (!folder.is_absolute()) return D810FolderState::Untrusted;
    std::filesystem::path current = folder.root_path();
    for (const auto& part : folder.relative_path()) {
        if (part == L"." || part == L"..") return D810FolderState::Untrusted;
        current /= part;
        DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return D810FolderState::Untrusted;
            if (!create) return D810FolderState::Absent;
            if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return D810FolderState::Untrusted;
            attributes = GetFileAttributesW(current.c_str());
        }
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return D810FolderState::Untrusted;
    }
    return D810FolderState::Trusted;
}
struct D810TextFile {
    enum class Kind { Absent, Read, Error } kind{Kind::Error};
    std::string text;
};
// A regular disk file (not a directory, not a reparse point) of at most `max_bytes`.
inline D810TextFile ReadD810TextFile(const std::filesystem::path& path, std::size_t max_bytes) {
    D810TextFile out;
    const auto folder = CheckD810Folder(path.parent_path(), false);
    if (folder != D810FolderState::Trusted) {
        out.kind = folder == D810FolderState::Absent ? D810TextFile::Kind::Absent : D810TextFile::Kind::Error;
        return out;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        out.kind = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? D810TextFile::Kind::Absent : D810TextFile::Kind::Error;
        return out;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return out;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    bool ok = GetFileType(file) == FILE_TYPE_DISK && GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
        static_cast<std::uint64_t>(size.QuadPart) <= max_bytes;
    if (ok) {
        out.text.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.text.empty() || (ReadFile(file, out.text.data(), static_cast<DWORD>(out.text.size()), &read, nullptr) != FALSE && read == out.text.size());
    }
    CloseHandle(file);
    out.kind = ok ? D810TextFile::Kind::Read : D810TextFile::Kind::Error;
    if (!ok) out.text.clear();
    return out;
}
inline constexpr std::size_t kD810JournalMaxBytes = 16 * 1024 * 1024;
// The operation record. Append returns true only after the line reached the
// disk (write-through and FlushFileBuffers).
class D810SettingsJournal {
public:
    virtual ~D810SettingsJournal() = default;
    virtual bool Append(const std::string& line) = 0;
    virtual D810TextFile Read() const = 0;
};
class D810SettingsFileJournal final: public D810SettingsJournal {
public:
    // `folder` is ProductD810SettingsWriteFolder() in the product and a
    // temporary folder in the tests.
    explicit D810SettingsFileJournal(std::filesystem::path folder): folder_(std::move(folder)), path_(folder_ / L"journal.txt") {}
    const std::filesystem::path& Path() const { return path_; }
    bool Append(const std::string& line) override {
        if (line.empty() || line.find('\n') != std::string::npos || line.size() > 4096) return false;
        if (CheckD810Folder(folder_, true) != D810FolderState::Trusted) return false;
        const DWORD attributes = GetFileAttributesW(path_.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && ((attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))) return false;
        const HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ | FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        // A line torn by an earlier crash is closed first, so the new line
        // stays a line of its own (the torn one then reads as malformed).
        std::string text;
        LARGE_INTEGER size{};
        bool ok = GetFileType(file) == FILE_TYPE_DISK && GetFileSizeEx(file, &size) != FALSE &&
            size.QuadPart + static_cast<LONGLONG>(line.size()) + 2 <= static_cast<LONGLONG>(kD810JournalMaxBytes);
        if (ok && size.QuadPart > 0) {
            LARGE_INTEGER at{};
            at.QuadPart = size.QuadPart - 1;
            char last = 0;
            DWORD read = 0;
            ok = SetFilePointerEx(file, at, nullptr, FILE_BEGIN) != FALSE && ReadFile(file, &last, 1, &read, nullptr) != FALSE && read == 1;
            if (ok && last != '\n') text += '\n';
        }
        text += line;
        text += '\n';
        DWORD written = 0;
        ok = ok && WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE && written == text.size() &&
            FlushFileBuffers(file) != FALSE;
        ok = CloseHandle(file) != FALSE && ok;
        return ok;
    }
    D810TextFile Read() const override { return ReadD810TextFile(path_, kD810JournalMaxBytes); }
private:
    std::filesystem::path folder_, path_;
};
// The product's record, or nothing when the folder cannot be resolved (then
// nothing may be written).
inline std::unique_ptr<D810SettingsFileJournal> OpenProductD810SettingsJournal() {
    const auto folder = ProductD810SettingsWriteFolder();
    if (!folder) return nullptr;
    return std::make_unique<D810SettingsFileJournal>(*folder);
}
inline D810CaptureBlock D810JournalBlock(const D810TextFile& file) {
    if (file.kind == D810TextFile::Kind::Absent) return D810CaptureBlock::None;
    if (file.kind == D810TextFile::Kind::Error) return D810CaptureBlock::Unreadable;
    return ParseD810Journal(file.text).block;
}

// --- Camera access ---------------------------------------------------------------------------
// An opened camera: its transport, the model and firmware version its identity
// reports (compared with the fixed table, never logged or recorded), and the
// flag its I/O watchdog sets when it cancels the device (empty: no watchdog).
// No device object: the controller and the sessions see only these.
struct D810OpenedCamera {
    WpdSettingsWriteSession::Transport transport;
    std::string model;
    std::string firmware;
    D810CancelFlag cancelled;
};
// Opens the registered camera (the implementation re-checks its identity
// against the registration) or returns an empty transport. One camera at a
// time: Close comes before the next Open. After the watchdog's flag is raised
// the transport sends nothing more.
class D810SettingsCameraAccess {
public:
    virtual ~D810SettingsCameraAccess() = default;
    virtual D810OpenedCamera Open(D810Camera camera) = 0;
    // True when the session closed cleanly.
    virtual bool Close(D810Camera camera) = 0;
};

// --- Controller: the entry points for the screen ------------------------------------------------
struct D810WriteRunOptions {
    // Deadline, the operation's stop flag and the watchdog, checked before every request.
    std::function<bool()> may_continue;
    // The session is still ours (END and Close after a deadline).
    std::function<bool()> may_cleanup;
    // Time left in the write phase. Required by Execute: without it, or with a
    // `step_budget` that is not positive, nothing is sent. A begin is passed
    // only when at least `step_budget` per remaining step is left, checked
    // before the reads of a step and again right before its begin (both values
    // provisional).
    std::function<std::chrono::milliseconds()> remaining;
    std::chrono::milliseconds step_budget{5000};
    std::function<void(const std::string&)> log;
    // UTC time for the record; the system clock when empty.
    std::function<std::string()> now;
};
struct D810StepOutcome {
    D810WriteStep step;
    bool sent{};
    std::optional<std::int64_t> readback;
    D810WriteVerdict verdict{D810WriteVerdict::NotSent};
};
struct D810WriteRun {
    D810WriteVerdict verdict{D810WriteVerdict::NotSent};
    std::size_t sent{};
    // ASCII token: why the run stopped or did not start. Empty after a full match.
    std::string reason;
    std::vector<D810StepOutcome> steps;
    D810CaptureBlock block{D810CaptureBlock::None};
};
struct D810RereadResult {
    bool complete{};
    D810ReleaseCheck check;
    D810CaptureBlock block{D810CaptureBlock::None};
    std::string reason;
};
// The read step of a copy (ADR-0038 decision 5): both cameras, read-only.
struct D810PlanRead {
    // The number of this read, given by the controller that made it (0: not a read of a controller). A plan of picked
    // values is made only from the last complete read that controller made, which it keeps itself; from the read it is
    // handed it takes this number and nothing else.
    std::uint64_t id{};
    bool complete{};
    std::array<D810SettingsSnapshot, 2> snapshots; // CAM-A, CAM-B
    // Which body reported the model and firmware the table names for it (indexed like `snapshots`; booleans only).
    // CAM-B's is always true in a complete read (a CAM-B that does not match ends the read); CAM-A's is what a plan
    // that writes CAM-A needs, and stays false while the table names no firmware for CAM-A.
    D810BodyFlags verified{};
    std::string reason;
};
struct D810WriteStatus {
    D810CaptureBlock block{D810CaptureBlock::Unreadable};
    bool capture_allowed{};
    // A new plan may be made and run: the table enables a row, and there is no
    // block or only a known mismatch.
    bool write_allowed{};
    bool plan_pending{};
    bool plan_confirmed{};
    // The history since the capture was last free of a block holds a plan of picked values (a copy only otherwise).
    bool chosen_history{};
    // Part of the record since then cannot be read (a damaged line, or steps that do not match their plan line).
    bool history_damaged{};
    // Rows the fixed table enables.
    std::size_t enabled_rows{};
    // The items a copy may write to CAM-B with this table, in table order (the screen names them).
    std::vector<D810WriteItem> writable_items;
};

// --- What the write path reads --------------------------------------------------------
// The write path reads only the codes of the fixed table of read settings (`in_table`), not the two candidate codes
// (0xD16A, 0xD100) that the stage A listing also reads. On a real body 0xD16A was answered with a failed begin and an
// END refused with 0x200A (the size of 0 is an inference), which stops the read session; 0xD100 has not been read on a
// body yet (unconfirmed). Neither is read here, so a plan, the check before a send and the reread cannot be stopped
// by them. The stage A read (gate and session) is untouched and still reads all of its entries.
constexpr std::size_t D810WriteReadSpecCount() {
    std::size_t count = 0;
    for (const auto& spec : kD810SettingSpecs)
        if (spec.in_table) ++count;
    return count;
}
inline constexpr std::array<D810SettingSpec, D810WriteReadSpecCount()> kD810WriteReadSpecs = [] {
    std::array<D810SettingSpec, D810WriteReadSpecCount()> out{};
    std::size_t next = 0;
    for (const auto& spec : kD810SettingSpecs)
        if (spec.in_table) out[next++] = spec;
    return out;
}();
constexpr bool D810WriteReadSetHas(std::uint32_t code) {
    for (const auto& spec : kD810WriteReadSpecs)
        if (spec.code == code) return true;
    return false;
}
// Every code a table confirms is in the read set (otherwise a plan on that code could not be read back).
constexpr bool D810WriteReadSetCovers(const D810WriteTable& table) {
    for (const auto& row : table.rows)
        if (row.code != 0 && !D810WriteReadSetHas(row.code)) return false;
    return true;
}
static_assert(D810WriteReadSpecCount() == 10 && !D810WriteReadSetHas(0xd16a) && !D810WriteReadSetHas(0xd100),
    "the write path reads the table's ten codes and not the two candidate codes");
static_assert(D810WriteReadSetCovers(kD810ProductWriteTable), "every code the product table confirms is read by the write path");

// Which tables a controller may be built with. Only the contract test target
// defines A0_SETTINGS_WRITE_TEST_TABLES; there the controller also accepts the
// tables the tests build (the ADR-0041 code paths included). Everywhere else
// it is built from kD810ProductWriteTable alone, and that table must also pass
// the product rules (CAM-B and the five ADR-0038 items only, no chosen values).
#if defined(A0_SETTINGS_WRITE_TEST_TABLES)
inline constexpr bool kD810ControllerTestTables = true;
#else
inline constexpr bool kD810ControllerTestTables = false;
#endif
inline const char* D810ControllerTableProblem(const D810WriteTable& table, bool test_tables) {
    if (const auto problem = D810WriteTableProblem(table)) return problem;
    return test_tables ? nullptr : D810ProductTableProblem(table);
}

// The entry points for the screen: plan, confirm, run, reread, status. There is
// no other way in (no command line, no automation), no "undo" and no write of
// an earlier value (ADR-0038 decision 7).
//
// Not thread-safe (the plans and the in-memory block are not locked): calls
// never overlap. They come from one thread at a time, and a call handed to a
// worker thread (the read for a plan, Execute, Reread) is the only call until
// its result has been taken back through a future or a join, which orders the
// worker's changes before the next call. The screen (D810SettingsWriteScreen)
// keeps to this; the I/O watchdog never calls the controller.
class D810SettingsWriteController {
    struct AnyTable {};
public:
    // The product entry point: the fixed product table, nothing else.
    D810SettingsWriteController(D810SettingsJournal& journal, D810CameraOwnership& owner):
        D810SettingsWriteController(AnyTable{}, kD810ProductWriteTable, journal, owner) {}
#if defined(A0_SETTINGS_WRITE_TEST_TABLES)
    // Contract tests only: a table of their own.
    D810SettingsWriteController(const D810WriteTable& table, D810SettingsJournal& journal, D810CameraOwnership& owner):
        D810SettingsWriteController(AnyTable{}, table, journal, owner) {}
#endif

    // The table this controller decides every row by (the screen builds its panel from it).
    const D810WriteTable& Table() const { return table_; }
    // The record on disk is the authority; the in-memory block only adds to it.
    D810CaptureBlock Block() const { return StrongerD810Block(D810JournalBlock(journal_.Read()), memory_block_); }
    bool CaptureAllowed() const { return Block() == D810CaptureBlock::None; }
    D810WriteStatus Status() const {
        D810WriteStatus status;
        status.block = Block();
        status.capture_allowed = status.block == D810CaptureBlock::None;
        status.enabled_rows = D810EnabledRowCount(table_);
        status.writable_items = D810CopyWritableItems(table_);
        status.write_allowed = table_valid_ && status.enabled_rows > 0 && WriteAllowed(status.block) && owner_.Current() == D810CameraUse::None;
        status.plan_pending = pending_.has_value();
        status.plan_confirmed = confirmed_.has_value();
        // What the record says about the history since the capture was last free of a block. The block is told in terms of
        // the values written only when the controller would release it by them (a plan of picked values with planned values
        // to check, and no part of the record damaged); a damaged part of the record sends it back to the comparison of the
        // two cameras, and the screen then says that the record is damaged.
        if (const auto file = journal_.Read(); file.kind == D810TextFile::Kind::Read) {
            const auto state = ParseD810Journal(file.text);
            status.history_damaged = state.history_damaged;
            status.chosen_history = state.chosen_unresolved && !state.history_damaged && !state.expected.empty();
        }
        return status;
    }
    // The read step of a copy (ADR-0038 decision 5): CAM-A, then CAM-B, each
    // opened, read with the read-only session and closed. Sends no write and
    // keeps no session open: the plan is then built with PlanCopy from these
    // snapshots, and nothing is open while the operator decides.
    D810PlanRead ReadForPlan(D810SettingsCameraAccess& access, const D810WriteRunOptions& options) {
        last_read_.reset(); // a plan of picked values is made from the last complete read only, so an earlier read is gone whatever this one does
        D810PlanRead out;
        if (!options.may_continue || !options.may_cleanup) { out.reason = "invalid_options"; return out; }
        if (!owner_.TryAcquire(D810CameraUse::Settings)) { out.reason = "cameras_busy"; return out; }
        out.id = NextReadId();
        struct Release { D810CameraOwnership& owner; ~Release() { owner.Release(D810CameraUse::Settings); } } release{owner_};
        for (const auto camera : {D810Camera::A, D810Camera::B}) {
            // CAM-B is the body that would be written: when it is not the model and firmware the table names, no
            // plan is made and nothing is shown to confirm. CAM-A is only read, so its version does not matter.
            const bool target = camera == D810Camera::B;
            auto read = ReadCamera(access, camera, options, target);
            out.verified[static_cast<std::size_t>(camera)] = read.opened && read.verified;
            // CAM-A's comparison is reported (booleans only) only while the table enables a CAM-A row, and never stops
            // the read: it tells whether a plan for CAM-A would pass the check before any send.
            if (read.opened && (target || (camera == D810Camera::A && D810AnyCamARowEnabled(table_))) && options.log)
                options.log(std::string(D810CameraAlias(camera)) + " settings_write_body_check model_match=" + (read.body.model_match ? "1" : "0") +
                    " firmware_match=" + (read.body.firmware_match ? "1" : "0") + " setting_writes=0");
            if (target && read.opened && !read.verified) { out.reason = std::string("body_not_verified:") + D810CameraAlias(camera); return out; }
            if (!read.ok) { out.reason = std::string("read_incomplete:") + D810CameraAlias(camera); return out; }
            out.snapshots[static_cast<std::size_t>(camera)] = std::move(read.read.readings);
        }
        out.complete = true;
        last_read_ = out;
        return out;
    }
    // Build a plan from two fresh stage A reads. It replaces any plan not yet
    // run. `only`: the items the operator keeps (the rest are left out, their
    // values never replaced).
    D810PlanResult PlanCopy(const D810SettingsSnapshot& a, const D810SettingsSnapshot& b, const std::optional<std::vector<D810WriteItem>>& only = std::nullopt) {
        if (const auto refusal = PlanRefusal()) return Keep(Refused(refusal));
        return Keep(BuildD810CopyPlan(a, b, table_, only));
    }
    // Build a plan of the values picked for each camera (D810PanelDraft::Choices()) from the last complete read this
    // controller made (ReadForPlan). `read` is only the token of that read: its number must be the last read's, and the
    // snapshots and the flags of the bodies (which of them the table names) are the ones the controller kept, not what
    // the caller holds. A read that is not the last one (an older read, one that was not complete, one from elsewhere),
    // or one that a run or a reread has made old, is refused whole ("read_not_current"). Refused whole too while the
    // table's chosen-values switch is off (the product table). It replaces any plan not yet run.
    D810PlanResult PlanChosen(const D810PlanRead& read, const std::vector<D810CameraChoice>& choices) {
        if (const auto refusal = PlanRefusal()) return Keep(Refused(refusal));
        if (!last_read_ || !last_read_->complete || read.id == 0 || read.id != last_read_->id) return Keep(Refused("read_not_current"));
        return Keep(BuildD810PerCameraPlan(last_read_->snapshots[0], last_read_->snapshots[1], last_read_->verified, choices, table_));
    }
#if defined(A0_SETTINGS_WRITE_TEST_TABLES)
    // Contract tests only: a read made without a camera becomes the last read (numbered here), as ReadForPlan would leave it.
    void AcceptReadForTest(D810PlanRead& read) {
        read.id = NextReadId();
        last_read_ = read;
    }
#endif
    // The operator confirmed the plan shown. Returns the refusal, or nullptr.
    const char* Confirm(std::uint64_t plan_id) {
        if (!pending_ || pending_->id != plan_id) return "plan_not_pending";
        if (pending_->steps.empty()) return "nothing_to_write";
        confirmed_ = std::move(pending_);
        pending_.reset();
        return nullptr;
    }
    void Discard() { pending_.reset(); confirmed_.reset(); }
    // Run the confirmed plan once. The plan is consumed whatever happens.
    D810WriteRun Execute(std::uint64_t plan_id, D810SettingsCameraAccess& access, const D810WriteRunOptions& options) {
        D810WriteRun run;
        if (!confirmed_ || confirmed_->id != plan_id) { run.reason = "plan_not_confirmed"; return run; }
        const D810WritePlan plan = std::move(*confirmed_);
        confirmed_.reset();
        pending_.reset();
        last_read_.reset(); // the cameras are about to change: no plan is made from a read before this run
        if (!options.may_continue || !options.may_cleanup || !options.remaining || options.step_budget <= std::chrono::milliseconds::zero()) {
            run.reason = "invalid_options";
            return run;
        }
        if (const auto refusal = PlanRefusal()) { run.reason = refusal; return run; }
        // The order of the steps (CAM-B first, CAM-A last, no item twice), and the table again: this run's time
        // margin and its latch rely on that order.
        if (const auto problem = D810PlanProblem(plan, table_); !problem.empty()) { run.reason = problem; return run; }
        if (!owner_.TryAcquire(D810CameraUse::Settings)) { run.reason = "cameras_busy"; return run; }
        struct Release { D810CameraOwnership& owner; ~Release() { owner.Release(D810CameraUse::Settings); } } release{owner_};
        D810OperationLatch latch;
        Runner runner{*this, plan, access, options, NewId(), run, latch};
        runner.Go();
        memory_block_ = StrongerD810Block(memory_block_, run.block);
        return run;
    }
    // Read both cameras with the read-only session and release a block. For a
    // history of copies only, when every block item reads the same on both; for a
    // history that holds a chosen plan (read from the record, so also after a
    // restart), when every item the unfinished operations planned reads its
    // planned value on its own camera, with no comparison of the two cameras.
    // Sends no write.
    D810RereadResult Reread(D810SettingsCameraAccess& access, const D810WriteRunOptions& options) {
        D810RereadResult result;
        if (!options.may_continue || !options.may_cleanup) { result.reason = "invalid_options"; result.block = Block(); return result; }
        if (!owner_.TryAcquire(D810CameraUse::Settings)) { result.reason = "cameras_busy"; result.block = Block(); return result; }
        struct Release { D810CameraOwnership& owner; ~Release() { owner.Release(D810CameraUse::Settings); } } release{owner_};
        last_read_.reset(); // a reread follows a change of the cameras
        const auto a = ReadCamera(access, D810Camera::A, options);
        const auto b = ReadCamera(access, D810Camera::B, options);
        result.complete = a.ok && b.ok;
        if (!result.complete) { result.reason = "reread_incomplete"; result.block = Block(); return result; }
        result.check = ReleaseCheck(a.read.readings, b.read.readings);
        if (!journal_.Append(FormatD810JournalReread(Now(options), result.check.released))) {
            result.reason = "record_unwritable";
            result.block = Block();
            return result;
        }
        if (result.check.released) memory_block_ = D810CaptureBlock::None;
        else if (memory_block_ != D810CaptureBlock::None) memory_block_ = D810CaptureBlock::Mismatch;
        result.block = Block();
        if (!result.check.released) result.reason = "settings_differ";
        return result;
    }

private:
    D810SettingsWriteController(AnyTable, const D810WriteTable& table, D810SettingsJournal& journal, D810CameraOwnership& owner):
        table_(table), table_valid_(D810ControllerTableProblem(table_, kD810ControllerTestTables) == nullptr), journal_(journal), owner_(owner) {}
    // Read numbers are unique over the process, so a read of another controller is never taken for this one's.
    static std::uint64_t NextReadId() {
        static std::atomic<std::uint64_t> counter{0};
        return ++counter;
    }
    static bool WriteAllowed(D810CaptureBlock block) { return block == D810CaptureBlock::None || block == D810CaptureBlock::Mismatch; }
    // Which comparison releases the block is decided by the record on disk, the authority: a history with a chosen
    // plan and planned values to check uses them; anything else (copies only, no record, a chosen history whose
    // step lines gave no value, a chosen history of which part of the record is damaged) compares the two cameras,
    // the stricter rule. A record that exists but cannot be read releases nothing: which rule applies is not known.
    D810ReleaseCheck ReleaseCheck(const D810SettingsSnapshot& a, const D810SettingsSnapshot& b) const {
        const auto file = journal_.Read();
        if (file.kind == D810TextFile::Kind::Error) {
            D810ReleaseCheck unreadable;
            unreadable.differences.push_back("record_unreadable");
            return unreadable;
        }
        if (file.kind == D810TextFile::Kind::Read) {
            const auto state = ParseD810Journal(file.text);
            if (state.chosen_unresolved && !state.history_damaged && !state.expected.empty()) return CheckD810ReleasePlanned(a, b, state.expected, table_);
        }
        return CheckD810Release(a, b, table_);
    }
    const char* PlanRefusal() const {
        if (!table_valid_) return "invalid_table";
        if (!WriteAllowed(Block())) return "reread_first";
        return nullptr;
    }
    static D810PlanResult Refused(const char* reason) {
        D810PlanResult result;
        result.reasons.push_back(reason);
        return result;
    }
    D810PlanResult Keep(D810PlanResult result) {
        pending_.reset();
        confirmed_.reset();
        if (result.plan) {
            result.plan->id = NewId();
            pending_ = *result.plan;
        }
        return result;
    }
    std::uint64_t NewId() {
        std::random_device device;
        std::uint64_t id = (static_cast<std::uint64_t>(device()) << 32) ^ device() ^ (++counter_ << 8);
        return id ? id : 1;
    }
    static std::string Now(const D810WriteRunOptions& options) {
        if (options.now) return options.now();
        const auto seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
        gmtime_s(&utc, &seconds);
        char text[32]{};
        std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
        return text;
    }
    struct CameraRead {
        D810CameraSettingsRead read;
        bool ok{};
        bool opened{};
        bool verified{};
        D810BodyCheck body;
    };
    // Open, read every table code with the read-only session, Close. After the
    // watchdog's flag nothing more reaches the transport, not even an END, and
    // what was answered before it is not trusted: the read is not complete.
    // `only_if_verified`: a body that is not the model and firmware the table names
    // is closed without a request (`opened` and `body` say what was found, `ok` is false).
    CameraRead ReadCamera(D810SettingsCameraAccess& access, D810Camera camera, const D810WriteRunOptions& options, bool only_if_verified = false) const {
        CameraRead out;
        auto opened = access.Open(camera);
        if (!opened.transport) return out;
        out.opened = true;
        out.body = D810CheckBody(table_, camera, opened.model, opened.firmware);
        out.verified = out.body.Verified();
        if (only_if_verified && !out.verified) {
            access.Close(camera);
            return out;
        }
        const auto cancelled = opened.cancelled;
        bool complete = false;
        try {
            auto transport = [inner = std::move(opened.transport), cancelled](IPortableDeviceValues* request) {
                if (D810CancelRaised(cancelled)) throw std::runtime_error("settings read cancelled");
                return inner(request);
            };
            WpdSettingsReadSession session(std::move(transport), [&options, cancelled] { return !D810CancelRaised(cancelled) && options.may_continue(); },
                [&options, cancelled] { return !D810CancelRaised(cancelled) && options.may_cleanup(); });
            out.read = session.ReadAll(D810CameraAlias(camera), options.log, std::span<const D810SettingSpec>(kD810WriteReadSpecs));
            // A stop on the very last row leaves no row to report as not read: the spent session says it.
            complete = out.read.Complete() && !session.Poisoned();
        } catch (...) {}
        const bool closed = access.Close(camera);
        out.ok = complete && closed && !D810CancelRaised(cancelled);
        return out;
    }

    // One run of one confirmed plan.
    struct Runner {
        D810SettingsWriteController& self;
        const D810WritePlan& plan;
        D810SettingsCameraAccess& access;
        const D810WriteRunOptions& options;
        std::uint64_t op;
        D810WriteRun& run;
        D810OperationLatch& latch;
        bool unknown{}, mismatch{}, record_failed{};

        void Say(const std::string& line) const { if (options.log) options.log(line); }
        // Enough time for step `i` and every step after it, on both cameras
        // (ADR-0038 decision 10). `remaining` is checked present and
        // `step_budget` positive by Execute. Divided, not multiplied: a large
        // budget cannot wrap round into "enough time" (for integers and a
        // positive count, remaining / left >= budget is remaining >= budget * left).
        bool MarginLeft(std::size_t i) const {
            const auto left = static_cast<long long>(plan.steps.size() - i);
            return options.remaining().count() / left >= options.step_budget.count();
        }
        bool Append(const std::string& line) {
            if (self.journal_.Append(line)) return true;
            record_failed = true;
            return false;
        }
        void Go() {
            for (const auto& step : plan.steps) run.steps.push_back({step});
            // The plan and the value before of every step, on every target
            // camera, are on disk before anything is sent.
            bool recorded = Append(FormatD810JournalPlan(op, Now(options), plan));
            for (std::size_t i = 0; recorded && i < plan.steps.size(); ++i) recorded = Append(FormatD810JournalStep(op, i, plan.steps[i]));
            if (!recorded) { Finish("record_unwritable"); return; }
            if (const auto change = Verify(); !change.empty()) { Finish(change); return; }
            for (const auto camera : plan.Cameras()) {
                // The other camera is not opened for writing once anything failed.
                if (latch.Tripped()) { Finish("operation_latched"); return; }
                if (const auto stop = WriteCamera(camera); !stop.empty()) { latch.Trip(); Finish(stop); return; }
            }
            Finish("");
        }
        // Every camera the plan relies on is read again with the read-only
        // session; any difference from the plan, or a target body that is not
        // the model and firmware the table names, stops the run before a send.
        std::string Verify() {
            const auto targets = plan.Cameras();
            for (const auto camera : {D810Camera::A, D810Camera::B}) {
                const bool target = std::find(targets.begin(), targets.end(), camera) != targets.end();
                const bool observed = std::any_of(plan.observed.begin(), plan.observed.end(), [&](const auto& seen) { return seen.camera == camera; });
                if (!target && !observed) continue;
                const auto read = self.ReadCamera(access, camera, options);
                if (!read.ok) return std::string("reread_incomplete:") + D810CameraAlias(camera);
                if (target && !read.verified) return std::string("body_not_verified:") + D810CameraAlias(camera);
                if (const auto change = D810PlanChange(plan, camera, read.read.readings); !change.empty())
                    return std::string("plan_changed:") + D810CameraAlias(camera) + ":" + change;
            }
            return {};
        }
        std::string WriteCamera(D810Camera camera) {
            D810WriteGateConfig config;
            config.camera = camera;
            config.iso_auto_code = plan.iso_auto_code;
            config.latch = &latch;
            // The key to a CAM-A session: only a chosen plan under a table that switches chosen values on.
            config.cam_a_allowed = camera == D810Camera::A && D810CamAKeyOpen(plan, self.table_);
            for (const auto& step : plan.steps)
                if (step.camera == camera) config.entries.push_back({step.code, step.value});
            auto opened = access.Open(camera);
            if (!opened.transport) return std::string("open_failed:") + D810CameraAlias(camera);
            std::string stop;
            if (!D810BodyVerified(self.table_, camera, opened.model, opened.firmware)) stop = std::string("body_not_verified:") + D810CameraAlias(camera);
            else {
                try {
                    WpdSettingsWriteSession session(std::move(opened.transport), options.may_continue, options.may_cleanup, std::move(config), opened.cancelled);
                    stop = session.Ready() ? WriteSteps(session, camera) : "device_not_ready";
                } catch (...) {
                    stop = "exception";
                }
            }
            if (!access.Close(camera)) {
                if (run.sent) unknown = true;
                if (stop.empty()) stop = std::string("close_failed:") + D810CameraAlias(camera);
            }
            return stop;
        }
        std::string WriteSteps(WpdSettingsWriteSession& session, D810Camera camera) {
            for (std::size_t i = 0; i < plan.steps.size(); ++i) {
                const auto& step = plan.steps[i];
                if (step.camera != camera) continue;
                auto& outcome = run.steps[i];
                if (!options.may_continue()) return "interrupted";
                if (!MarginLeft(i)) return "deadline_margin";
                // Reads the gate requires in this generation: Live off, the
                // preconditions, and the code itself (type, GetSet, form and
                // value are checked from this description, not from the plan).
                const auto live = session.ReadLiveState();
                if (!live) return "live_state_unreadable";
                if (*live != 0) return "live_view_on";
                const auto pre = D810WriteItemOf(step.item).precondition;
                if (pre != D810WritePrecondition::LiveOff && !session.ReadDesc(kD810ExposureModeCode)) return "desc_unreadable";
                if (pre == D810WritePrecondition::ManualModeIsoAutoOff && !session.ReadDesc(plan.iso_auto_code)) return "desc_unreadable";
                const auto desc = session.ReadDesc(step.code);
                if (!desc) return "desc_unreadable";
                if (desc->current_value != step.before) return "changed_before_write";
                // The reads take time: the margin is checked again right before the begin.
                if (!MarginLeft(i)) return "deadline_margin";
                const auto attempt = session.Write(step.code, step.value);
                if (attempt.refused) return std::string("refused_") + attempt.refused;
                outcome.sent = true;
                ++run.sent;
                Say(std::string(D810CameraAlias(camera)) + " settings_write code=" + D810Hex(step.code, 4) + " ended=" + (attempt.ended ? "1" : "0"));
                if (attempt.ended) {
                    if (const auto readback = session.ReadDesc(step.code)) {
                        outcome.readback = readback->current_value;
                        outcome.verdict = readback->current_value == step.value ? D810WriteVerdict::Match : D810WriteVerdict::Mismatch;
                    } else outcome.verdict = D810WriteVerdict::Unknown;
                } else outcome.verdict = D810WriteVerdict::Unknown;
                if (!Append(FormatD810JournalResult(op, i, step, outcome.readback, outcome.verdict))) {
                    unknown = true;
                    return "record_unwritable";
                }
                if (outcome.verdict == D810WriteVerdict::Unknown) { unknown = true; return "result_unknown"; }
                if (outcome.verdict == D810WriteVerdict::Mismatch) { mismatch = true; return "readback_mismatch"; }
            }
            return {};
        }
        void Finish(const std::string& reason) {
            run.reason = reason;
            if (run.sent == 0) run.verdict = D810WriteVerdict::NotSent;
            else if (unknown || record_failed) run.verdict = D810WriteVerdict::Unknown;
            // A stop after some matches leaves the plan half done: a mismatch.
            else if (mismatch || !reason.empty()) run.verdict = D810WriteVerdict::Mismatch;
            else run.verdict = D810WriteVerdict::Match;
            const bool ended = self.journal_.Append(FormatD810JournalEnd(op, Now(options), run.verdict, run.sent, reason));
            if (run.verdict == D810WriteVerdict::Mismatch) run.block = D810CaptureBlock::Mismatch;
            if (run.verdict == D810WriteVerdict::Unknown) run.block = D810CaptureBlock::Unknown;
            if (!ended && run.sent) {
                run.verdict = D810WriteVerdict::Unknown;
                run.block = D810CaptureBlock::Unconfirmed;
                if (run.reason.empty()) run.reason = "record_unwritable";
            }
        }
    };

    // Held by value: a table passed as a temporary cannot dangle.
    const D810WriteTable table_;
    const bool table_valid_;
    D810SettingsJournal& journal_;
    D810CameraOwnership& owner_;
    std::optional<D810WritePlan> pending_, confirmed_;
    // The last complete read for a plan (ReadForPlan), kept here: a plan of picked values is made from this, never from what a caller holds.
    std::optional<D810PlanRead> last_read_;
    D810CaptureBlock memory_block_{D810CaptureBlock::None};
    std::uint64_t counter_{};
};
static_assert(std::is_constructible_v<D810SettingsWriteController, D810SettingsJournal&, D810CameraOwnership&>, "the product entry point");
#if !defined(A0_SETTINGS_WRITE_TEST_TABLES)
static_assert(!std::is_constructible_v<D810SettingsWriteController, const D810WriteTable&, D810SettingsJournal&, D810CameraOwnership&>,
    "outside the contract tests a controller takes no table");
#endif
} // namespace a0::phase0::experimental
