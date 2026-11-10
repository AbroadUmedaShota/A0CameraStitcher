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
// transfer that is open. The read-only gate's "an explicit refusal returns to
// Idle" rule is not used here, and the read gate's refusal test is not shared.
//
// Only targets built with A0_ENABLE_WPD_SETTINGS_WRITE (the CMake option
// A0_BUILD_WPD_SETTINGS_WRITE, OFF by default, and the contract tests) can
// include this header. There is no command-line entry to it.
#if !defined(A0_ENABLE_WPD_SETTINGS_WRITE)
#error "wpd_settings_write.hpp is only for targets built with A0_BUILD_WPD_SETTINGS_WRITE"
#endif
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
struct D810WriteGateConfig {
    D810Camera camera{D810Camera::B};
    std::vector<D810WriteGateEntry> entries;
    std::uint16_t iso_auto_code{};
    D810OperationLatch* latch{};
};
// States. In the words of ADR-0038 decision 9: ReadBegun, ReadOpen and
// ReadEnding are "ReadOpen"; WriteBegun and WriteOpen are "WriteBegun";
// WriteSent and WriteEnding are "WriteSent".
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
        if (!D810WriteItemHasCode(D810WriteItemOf(D810WriteItem::IsoAuto), config_.iso_auto_code)) return false;
        for (const auto& entry : config_.entries) {
            const auto* spec = FindD810WriteItemByCode(entry.code);
            if (!spec || !codes.insert(entry.code).second || !D810ValueFits(spec->type, entry.value)) return false;
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
class WpdSettingsWriteSession {
public:
    using Transport = WpdSettingsReadSession::Transport;
    WpdSettingsWriteSession(Transport transport, std::function<bool()> may_continue, std::function<bool()> may_cleanup, D810WriteGateConfig config):
        transport_(std::move(transport)), may_continue_(std::move(may_continue)), may_cleanup_(std::move(may_cleanup)), gate_(std::move(config)) {
        if (!transport_ || !may_continue_ || !may_cleanup_) throw std::invalid_argument("missing settings write admission");
    }
    WpdSettingsWriteSession(const WpdSettingsWriteSession&) = delete;
    WpdSettingsWriteSession& operator=(const WpdSettingsWriteSession&) = delete;
    const D810SettingsWriteGate& Gate() const { return gate_; }
    bool Poisoned() const { return poisoned_ || gate_.Retired(); }
    // Called by the I/O watchdog's owner after it cancelled the device.
    void Cancelled() { gate_.Cancelled(); poisoned_ = true; }

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
    WpdLiveViewExchange Send(IPortableDeviceValues* request) {
        if (!gate_.Admit(request)) throw std::runtime_error("settings write request not admitted");
        WpdLiveViewExchange reply;
        try { reply = transport_(request); }
        catch (...) { gate_.TransportFailed(); poisoned_ = true; throw; }
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
    bool poisoned_{};
};

} // namespace a0::phase0::experimental
