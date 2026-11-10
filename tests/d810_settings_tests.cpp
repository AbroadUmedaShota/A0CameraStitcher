// Contracts of the read-only camera settings table, parser and gate (#416 stage A).
// Synthetic bodies only: in-process WPD value objects, no device, no camera.
#include "a0/phase0/experimental/wpd_settings_read.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace a0::phase0::experimental;
namespace {
int failures = 0, checks = 0;
void check(bool value, const char* what = "") {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "FAILED check " << checks << " " << what << "\n";
    }
}
using Bytes = std::vector<unsigned char>;
void Put(Bytes& out, std::int64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) out.push_back(static_cast<unsigned char>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xff));
}
std::size_t SizeOfType(unsigned type) { return type == 1 || type == 2 ? 1 : type == 3 || type == 4 ? 2 : 4; }
// form 0: none. form 1: values = {min, max, step}. form 2: values = the list.
Bytes Desc(unsigned code, unsigned type, unsigned get_set, std::int64_t def, std::int64_t current, unsigned form, const std::vector<std::int64_t>& values = {}) {
    Bytes out;
    const auto size = SizeOfType(type);
    Put(out, code, 2);
    Put(out, type, 2);
    Put(out, get_set, 1);
    Put(out, def, size);
    Put(out, current, size);
    Put(out, form, 1);
    if (form == 2) Put(out, static_cast<std::int64_t>(values.size()), 2);
    for (const auto value : values) Put(out, value, size);
    return out;
}
const D810SettingSpec& Spec(D810SettingId id) { return D810SettingOf(id); }
Bytes DescFor(const D810SettingSpec& spec, std::int64_t current, unsigned get_set = 1) {
    return Desc(spec.code, static_cast<unsigned>(spec.type), get_set, current, current, 2, {current, current});
}
bool HasIdentifierShape(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::regex guid("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-");
    return lower.find("\\\\?\\") != std::string::npos || lower.find("usb#") != std::string::npos || lower.find("vid_") != std::string::npos ||
        lower.find('{') != std::string::npos || std::regex_search(lower, guid);
}

// --- WPD helpers ------------------------------------------------------------------
WpdValues Reply(bool final_reply = false, DWORD mtp = 0x2001) {
    WpdValues result;
    CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&result));
    result->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT, S_OK);
    if (final_reply) result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, mtp);
    return result;
}
WpdLiveViewExchange BeginReply(const wchar_t* context, std::size_t total) {
    auto result = Reply();
    result->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, context);
    result->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, total);
    result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE, 64);
    return {S_OK, result};
}
WpdLiveViewExchange FinalReply(DWORD mtp = 0x2001) { return {S_OK, Reply(true, mtp)}; }
// The reply to a READ_DATA that delivered `count` bytes (at most 16).
WpdLiveViewExchange ReadReplyOf(DWORD count) {
    static const unsigned char bytes[16]{};
    auto result = Reply();
    result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, count);
    result->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, const_cast<unsigned char*>(bytes), count);
    return {S_OK, result};
}
// The dispatch succeeded and the command HRESULT failed.
WpdLiveViewExchange CommandFailedReply() {
    auto result = Reply();
    result->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT, E_FAIL);
    return {S_OK, result};
}
// A request of a given command kind with an operation and 32 bit arguments.
WpdValues Make(SettingsCommandKind kind, DWORD opcode, std::initializer_list<DWORD> arguments) {
    auto request = BuildSettingsExecute(kind == SettingsCommandKind::ExecuteWithDataToRead ? WpdLiveViewOperation::Frame : WpdLiveViewOperation::Start, opcode, arguments);
    if (kind == SettingsCommandKind::ExecuteWithDataToWrite)
        request->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE.pid);
    return request;
}
WpdValues WithCommand(WpdValues request, const PROPERTYKEY& command, const wchar_t* context) {
    request->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, command.fmtid);
    request->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, command.pid);
    request->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, context);
    return request;
}
WpdValues ReadRequest(const wchar_t* context) { return BuildWpdLiveViewRead(context, 16); }
WpdValues EndRequest(const wchar_t* context) { return BuildWpdLiveViewEndTransfer(context); }
WpdValues WriteDataRequest(const wchar_t* context) { return WithCommand(BuildWpdLiveViewRead(context, 16), WPD_COMMAND_MTP_EXT_WRITE_DATA, context); }

// --- A synthetic body ----------------------------------------------------------------
struct FakeBody {
    std::map<std::uint16_t, Bytes> descs;
    std::map<std::uint16_t, DWORD> reject;       // property code -> MTP response at the begin
    std::set<std::uint16_t> throw_on_read;       // property codes whose READ_DATA throws
    std::map<std::uint16_t, DWORD> end_response; // property code -> MTP response at the END (instead of OK)
    std::map<std::uint16_t, std::size_t> serve_limit; // property code -> bytes READ_DATA delivers in total
    std::set<std::uint16_t> unknown_size;        // begin reports TOTAL_DATA_SIZE 0xFFFFFFFF
    std::set<std::uint16_t> oversize;            // begin reports a size above the caller's cap
    std::set<std::uint16_t> begin_command_failed; // begin: dispatch succeeds, the command HRESULT fails
    std::set<std::uint16_t> end_command_failed;  // END: the command HRESULT fails
    std::set<std::uint16_t> end_without_code;    // END: no MTP response code
    std::set<std::uint16_t> throw_on_end;        // END throws
    unsigned live = 0;
    DWORD ready_response = 0x2001;
    int busy_replies = 0;
    std::vector<std::string> trace;
    int writes = 0, ends = 0, begins = 0, reads = 0;
    std::function<void(const std::string&)> after;
    // Everything the body sees, as text: "90c8", "1015(d1a2)", "1014(500d)", "read", "end".
    WpdLiveViewExchange operator()(IPortableDeviceValues* request) {
        const auto shape = InspectSettingsRequest(request);
        if (!shape || shape->kind == SettingsCommandKind::ExecuteWithDataToWrite || shape->kind == SettingsCommandKind::WriteData || shape->opcode == 0x1016) {
            ++writes;
            throw std::runtime_error("write-shaped request reached the body");
        }
        const auto note = [&](const std::string& entry) { trace.push_back(entry); if (after) after(entry); };
        char text[40];
        if (shape->kind == SettingsCommandKind::ExecuteWithoutData) {
            note("90c8");
            if (busy_replies > 0) { --busy_replies; return FinalReply(0x2019); }
            return FinalReply(ready_response);
        }
        if (shape->kind == SettingsCommandKind::ExecuteWithDataToRead) {
            ++begins;
            std::snprintf(text, sizeof text, "%04x(%04x)", shape->opcode, shape->arguments.empty() ? 0u : shape->arguments[0]);
            const auto code = static_cast<std::uint16_t>(shape->arguments[0]);
            if (shape->opcode == 0x1015) data_ = Bytes{static_cast<unsigned char>(live)};
            else {
                current_ = code;
                const auto rejected = reject.find(code);
                if (rejected != reject.end()) { note(text); return FinalReply(rejected->second); }
                if (begin_command_failed.count(code)) { note(text); return CommandFailedReply(); }
                const auto found = descs.find(code);
                if (found == descs.end()) { note(text); return FinalReply(0x200a); }
                data_ = found->second;
            }
            offset_ = 0;
            note(text);
            const std::uint64_t total = oversize.count(current_) ? 5000 : unknown_size.count(current_) ? 0xffffffffULL : data_.size();
            return BeginReply((L"fake-context-" + std::to_wstring(begins)).c_str(), total);
        }
        if (shape->kind == SettingsCommandKind::ReadData) {
            ++reads;
            note("read");
            if (throw_on_read.count(current_)) throw std::runtime_error("transport failure");
            DWORD requested{};
            request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ, &requested);
            auto count = std::min<std::size_t>(requested, data_.size() - offset_);
            const auto limit = serve_limit.find(current_);
            if (limit != serve_limit.end()) count = std::min<std::size_t>(count, offset_ < limit->second ? limit->second - offset_ : 0);
            static unsigned char none = 0;
            auto result = Reply();
            result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, static_cast<DWORD>(count));
            result->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, count ? data_.data() + offset_ : &none, static_cast<DWORD>(count));
            offset_ += count;
            return {S_OK, result};
        }
        ++ends;
        note("end");
        if (throw_on_end.count(current_)) throw std::runtime_error("transport failure at END");
        if (end_command_failed.count(current_)) return CommandFailedReply();
        if (end_without_code.count(current_)) return {S_OK, Reply()};
        const auto response = end_response.find(current_);
        return FinalReply(response != end_response.end() ? response->second : 0x2001);
    }
    std::size_t CountOf(const std::string& prefix) const {
        return static_cast<std::size_t>(std::count_if(trace.begin(), trace.end(), [&](const std::string& entry) { return entry.rfind(prefix, 0) == 0; }));
    }
private:
    Bytes data_;
    std::size_t offset_ = 0;
    std::uint16_t current_ = 0;
};
FakeBody HappyBody() {
    FakeBody body;
    for (const auto& spec : kD810SettingSpecs) {
        std::int64_t current = 1;
        if (spec.id == D810SettingId::ShutterSpeed) current = 1667;
        if (spec.id == D810SettingId::Aperture) current = 800;
        if (spec.id == D810SettingId::Iso) current = 64;
        if (spec.id == D810SettingId::IsoAuto || spec.id == D810SettingId::IsoAutoCandidate) current = 0;
        if (spec.id == D810SettingId::ExposureCompensation) current = -333;
        body.descs[spec.code] = DescFor(spec, current);
    }
    return body;
}
struct Rig {
    FakeBody body;
    bool go = true;
    bool cleanup = true;
    std::vector<std::string> log;
    std::unique_ptr<WpdSettingsReadSession> session;
    explicit Rig(FakeBody b): body(std::move(b)) {
        session = std::make_unique<WpdSettingsReadSession>(
            [this](IPortableDeviceValues* request) { return body(request); }, [this] { return go; }, [this] { return cleanup; });
    }
    D810CameraSettingsRead Run() { return session->ReadAll("CAM-B", [this](const std::string& line) { log.push_back(line); }); }
};

// --- Parser -----------------------------------------------------------------------------
void ParserBoundaries() {
    const auto& shutter = Spec(D810SettingId::ShutterSpeed);
    const auto good = Desc(0x500d, 6, 1, 1667, 5000, 2, {1667, 3333, 5000, 10000});
    auto parsed = ParseD810PropDesc(good, shutter);
    check(parsed.Ok() && parsed.desc.get_set == 1 && parsed.desc.form_flag == 2 && parsed.desc.current_value == 5000 &&
        parsed.desc.values.size() == 4 && parsed.desc.values.back() == 10000 && parsed.desc.Writable(), "enum shutter");
    // Every shorter prefix is refused, never accepted and never fatal.
    for (std::size_t n = 0; n < good.size(); ++n)
        check(!ParseD810PropDesc(std::span<const unsigned char>(good.data(), n), shutter).Ok(), "prefix refused");
    auto trailing = good;
    trailing.push_back(0);
    check(ParseD810PropDesc(trailing, shutter).error == D810PropDescError::TrailingBytes, "trailing");
    check(ParseD810PropDesc(Bytes{}, shutter).error == D810PropDescError::TooShort, "empty");
    check(ParseD810PropDesc(std::span<const unsigned char>(good.data(), 2), shutter).error == D810PropDescError::TooShort, "two bytes");
    // Property code and type must be what was asked for.
    check(ParseD810PropDesc(Desc(0x500e, 6, 1, 1, 1, 2, {1}), shutter).error == D810PropDescError::CodeMismatch, "code");
    check(ParseD810PropDesc(Desc(0x500d, 4, 1, 1, 1, 2, {1}), shutter).error == D810PropDescError::TypeMismatch, "uint16 for uint32");
    check(ParseD810PropDesc(Desc(0x500d, 5, 1, 1, 1, 2, {1}), shutter).error == D810PropDescError::TypeMismatch, "int32 for uint32");
    for (const unsigned type : {0u, 7u, 8u, 9u, 10u, 0x4004u, 0xffffu}) {
        Bytes raw;
        Put(raw, 0x500d, 2);
        Put(raw, type, 2);
        Put(raw, 1, 1);
        for (int i = 0; i < 12; ++i) raw.push_back(0);
        check(ParseD810PropDesc(raw, shutter).error == D810PropDescError::TypeUnsupported, "unsupported type");
    }
    // A string-typed description (type 0xFFFF) is refused even when its length fits.
    check(ParseD810PropDesc(Bytes{0x0d, 0x50, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00}, shutter).error == D810PropDescError::TypeUnsupported, "string");
    // Get/Set and form flag.
    for (const unsigned get_set : {2u, 0x80u, 0xffu}) check(ParseD810PropDesc(Desc(0x500d, 6, get_set, 1, 1, 0), shutter).error == D810PropDescError::BadGetSet, "get_set");
    for (const unsigned flag : {3u, 4u, 0xffu}) check(ParseD810PropDesc(Desc(0x500d, 6, 1, 1, 1, flag), shutter).error == D810PropDescError::BadFormFlag, "form flag");
    check(ParseD810PropDesc(Desc(0x500d, 6, 0, 1, 1, 0), shutter).Ok() && !ParseD810PropDesc(Desc(0x500d, 6, 0, 1, 1, 0), shutter).desc.Writable(), "get only");
    // Range: min <= max in the signed domain; step 0 parses but is not writable.
    const auto& comp = Spec(D810SettingId::ExposureCompensation);
    auto range = ParseD810PropDesc(Desc(0x5010, 3, 1, 0, -333, 1, {-3000, 3000, 333}), comp);
    check(range.Ok() && range.desc.range_min == -3000 && range.desc.range_max == 3000 && range.desc.range_step == 333 && range.desc.current_value == -333 && range.desc.Writable(), "signed range");
    check(ParseD810PropDesc(Desc(0x5010, 3, 1, 0, 0, 1, {-1, 1, 0}), comp).Ok() && !ParseD810PropDesc(Desc(0x5010, 3, 1, 0, 0, 1, {-1, 1, 0}), comp).desc.Writable(), "step zero");
    check(ParseD810PropDesc(Desc(0x5010, 3, 1, 0, 0, 1, {1, -1, 1}), comp).error == D810PropDescError::RangeInverted, "inverted signed");
    check(ParseD810PropDesc(Desc(0x5010, 3, 1, 0, 0, 1, {5, 4, 1}), comp).error == D810PropDescError::RangeInverted, "inverted");
    check(ParseD810PropDesc(Desc(0x5010, 3, 1, 0, 0, 1, {-1, 1}), comp).error == D810PropDescError::TooShort, "range short");
    // Enumeration counts: 1..1024, and the payload bound.
    const auto& auto_iso = Spec(D810SettingId::IsoAuto);
    check(ParseD810PropDesc(Desc(0xd054, 2, 1, 0, 0, 2, {}), auto_iso).error == D810PropDescError::EnumCountOutOfRange, "count 0");
    check(ParseD810PropDesc(Desc(0xd054, 2, 1, 0, 0, 2, std::vector<std::int64_t>(1024, 1)), auto_iso).Ok(), "count 1024");
    check(ParseD810PropDesc(Desc(0xd054, 2, 1, 0, 0, 2, std::vector<std::int64_t>(1025, 1)), auto_iso).error == D810PropDescError::EnumCountOutOfRange, "count 1025");
    auto liar = Desc(0xd054, 2, 1, 0, 0, 2, {0, 1});
    liar[liar.size() - 4] = 3;  // claims 3 values, carries 2
    check(ParseD810PropDesc(liar, auto_iso).error == D810PropDescError::TooShort, "count larger than data");
    liar[liar.size() - 4] = 1;  // claims 1 value, carries 2
    check(ParseD810PropDesc(liar, auto_iso).error == D810PropDescError::TrailingBytes, "count smaller than data");
    check(ParseD810PropDesc(Bytes(kD810PropDescMaxBytes + 1, 0), shutter).error == D810PropDescError::TooLarge, "too large");
    check(ParseD810PropDesc(Bytes(kD810PropDescMaxBytes, 0), shutter).error != D810PropDescError::TooLarge, "at the bound");
    // Every table entry parses its own type, and a full-size enumeration stays inside the bound.
    for (const auto& spec : kD810SettingSpecs) check(ParseD810PropDesc(DescFor(spec, 1), spec).Ok(), "own type");
    // No input aborts: deterministic noise over every table entry.
    std::uint32_t seed = 12345;
    const auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 24; };
    for (const auto& spec : kD810SettingSpecs)
        for (int round = 0; round < 3000; ++round) {
            Bytes noise(next() % 40);
            for (auto& byte : noise) byte = static_cast<unsigned char>(next());
            if (noise.size() >= 4 && round % 2) { noise[0] = static_cast<unsigned char>(spec.code); noise[1] = static_cast<unsigned char>(spec.code >> 8); noise[2] = static_cast<unsigned char>(spec.type); noise[3] = 0; }
            const auto result = ParseD810PropDesc(noise, spec);
            if (result.Ok()) check(result.desc.code == spec.code && result.desc.data_type == static_cast<unsigned>(spec.type) && result.desc.get_set <= 1 && result.desc.form_flag <= 2, "noise accepted consistently");
        }
}

void Formatting() {
    const auto text = [](D810SettingId id, std::int64_t value) { return FormatD810Setting(Spec(id), value); };
    check(text(D810SettingId::ExposureMode, 1) == "M" && text(D810SettingId::ExposureMode, 2) == "P" && text(D810SettingId::ExposureMode, 3) == "A" &&
        text(D810SettingId::ExposureMode, 4) == "S" && text(D810SettingId::ExposureMode, 0x8010) == "raw 0x8010", "exposure mode");
    check(text(D810SettingId::ShutterSpeed, 1667) == "1/6 s", "1/6");
    check(text(D810SettingId::ShutterSpeed, 5000) == "1/2 s", "1/2");
    check(text(D810SettingId::ShutterSpeed, 3333) == "1/3 s", "1/3");
    check(text(D810SettingId::ShutterSpeed, 80) == "1/125 s", "1/125");
    check(text(D810SettingId::ShutterSpeed, 3000) == "0.3 s", "0.3 s");
    check(text(D810SettingId::ShutterSpeed, 10000) == "1 s", "1 s");
    check(text(D810SettingId::ShutterSpeed, 25000) == "2.5 s", "2.5 s");
    check(text(D810SettingId::ShutterSpeed, 3) == "3/10000 s", "below resolution");
    check(text(D810SettingId::ShutterSpeed, 0) == "raw 0x00000000", "zero");
    check(text(D810SettingId::ShutterSpeed, 0xffffffffLL) == "Bulb" && text(D810SettingId::ShutterSpeed, 0xfffffffdLL) == "Time", "bulb time");
    check(text(D810SettingId::NikonShutterSpeed, 1667) == "1/6 s", "nikon shutter");
    check(text(D810SettingId::Aperture, 800) == "F8" && text(D810SettingId::Aperture, 560) == "F5.6" && text(D810SettingId::Aperture, 1120) == "F11.2", "aperture");
    check(text(D810SettingId::Iso, 64) == "64" && text(D810SettingId::Iso, 12800) == "12800", "iso");
    check(text(D810SettingId::IsoAuto, 0) == "OFF" && text(D810SettingId::IsoAuto, 1) == "ON" && text(D810SettingId::IsoAuto, 7) == "raw 0x07", "iso auto");
    check(text(D810SettingId::WhiteBalance, 2) == "Automatic" && text(D810SettingId::WhiteBalance, 0x8013) == "Preset" && text(D810SettingId::WhiteBalance, 0x1234) == "raw 0x1234", "wb");
    check(text(D810SettingId::Metering, 2) == "Center Weighted" && text(D810SettingId::Metering, 5) == "raw 0x0005", "metering");
    check(text(D810SettingId::ExposureCompensation, 0) == "+0.00 EV" && text(D810SettingId::ExposureCompensation, -333) == "-0.33 EV" && text(D810SettingId::ExposureCompensation, 1000) == "+1.00 EV", "comp");
    check(text(D810SettingId::PictureControl, 3) == "raw 0x03" && text(D810SettingId::ActiveDLighting, 2) == "raw 0x02", "unnamed values stay raw");
    // The log line of the issue's example.
    D810SettingReading reading;
    reading.id = D810SettingId::ShutterSpeed;
    reading.available = true;
    reading.mtp_response = 0x2001;
    reading.desc = ParseD810PropDesc(Desc(0x500d, 6, 1, 1667, 5000, 2, {1667, 3333, 5000, 10000}), Spec(D810SettingId::ShutterSpeed)).desc;
    check(FormatD810SettingLogLine("CAM-B", Spec(D810SettingId::ShutterSpeed), reading) ==
        "CAM-B settings_desc code=0x500d mtp=0x2001 type=0x0006 getset=1 form=2 count=4 current=0x00001388 first=0x00000683 last=0x00002710", "log line");
    auto missing = UnavailableD810Setting(D810SettingId::ShutterSpeed, "mtp_rejected", 0x200a);
    check(FormatD810SettingLogLine("CAM-A", Spec(D810SettingId::ShutterSpeed), missing) == "CAM-A settings_desc code=0x500d unavailable reason=mtp_rejected mtp=0x200a", "log unavailable");
}

void Comparison() {
    D810SettingsSnapshot a, b;
    for (const auto& spec : kD810SettingSpecs) {
        const auto make = [&](std::int64_t current) {
            D810SettingReading r;
            r.id = spec.id; r.available = true; r.desc.current_value = current; r.desc.data_type = static_cast<unsigned>(spec.type);
            return r;
        };
        a.push_back(make(1));
        b.push_back(spec.id == D810SettingId::ShutterSpeed || spec.id == D810SettingId::Iso ? make(2) : make(1));
    }
    // An unreadable item on one side is Unknown, never Same.
    b[static_cast<std::size_t>(D810SettingId::WhiteBalance)] = UnavailableD810Setting(D810SettingId::WhiteBalance, "type_mismatch");
    a[static_cast<std::size_t>(D810SettingId::Metering)] = UnavailableD810Setting(D810SettingId::Metering, "mtp_rejected", 0x200a);
    b[static_cast<std::size_t>(D810SettingId::Metering)] = UnavailableD810Setting(D810SettingId::Metering, "mtp_rejected", 0x200a);
    const auto rows = CompareD810Settings(a, b);
    check(rows.size() == 10, "table rows only");
    for (const auto& row : rows) {
        const bool differs = row.id == D810SettingId::ShutterSpeed || row.id == D810SettingId::Iso;
        const bool unknown = row.id == D810SettingId::WhiteBalance || row.id == D810SettingId::Metering;
        check(row.difference == (differs ? D810Difference::Different : unknown ? D810Difference::Unknown : D810Difference::Same), "difference");
        if (row.id == D810SettingId::Metering) check(row.a_text == "unavailable (mtp_rejected)", "unavailable text");
    }
    check(CountD810Differences(rows) == 2, "count");
    check(CompareD810Settings({}, {}).size() == 10 && CompareD810Settings({}, {})[0].difference == D810Difference::Unknown && CompareD810Settings({}, {})[0].a_text == "unavailable (not_read)", "empty snapshots");
    for (const auto& row : rows) check(row.name != "iso_auto_candidate" && row.name != "nikon_shutter_speed", "diagnostic entries are not rows");
}

// --- Gate ----------------------------------------------------------------------------------
// The independent statement of what is allowed.
bool ShouldAdmit(SettingsCommandKind kind, std::uint32_t opcode, const std::vector<std::uint32_t>& arguments) {
    if (kind == SettingsCommandKind::ExecuteWithoutData) return opcode == 0x90c8 && arguments.empty();
    if (kind != SettingsCommandKind::ExecuteWithDataToRead || arguments.size() != 1) return false;
    if (opcode == 0x1015) return arguments[0] == 0xd1a2;
    if (opcode == 0x1014) {
        static const std::set<std::uint32_t> codes{0x500e, 0x500d, 0x5007, 0x500f, 0xd054, 0x5005, 0x500b, 0x5010, 0xd200, 0xd14e, 0xd16a, 0xd100};
        return codes.count(arguments[0]) != 0;
    }
    return false;
}
void SetArguments(IPortableDeviceValues* request, const std::vector<std::uint32_t>& arguments, VARTYPE type = VT_UI4) {
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> collection;
    request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, &collection);
    collection->Clear();
    for (const auto argument : arguments) {
        PROPVARIANT value{};
        value.vt = type;
        value.uhVal.QuadPart = argument;
        collection->Add(&value);
    }
}
// The sweep from a gate in Idle: either a new one or one that came back to Idle
// after a transfer ended.
void SweepFromIdle(const D810SettingsReadGate& base, const std::vector<std::vector<std::uint32_t>>& argument_sets, std::size_t expected_admitted, bool table_codes) {
    std::size_t admitted = 0, probes = 0;
    for (const auto kind : {SettingsCommandKind::ExecuteWithoutData, SettingsCommandKind::ExecuteWithDataToRead, SettingsCommandKind::ExecuteWithDataToWrite}) {
        auto request = Make(kind, 0, {});
        check(InspectSettingsRequest(request.Get()) && InspectSettingsRequest(request.Get())->kind == kind, "kind detected");
        for (const auto& arguments : argument_sets) {
            SetArguments(request.Get(), arguments);
            for (std::uint32_t opcode = 0; opcode <= 0xffff; ++opcode) {
                request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, opcode);
                D810SettingsReadGate gate = base;
                const bool got = gate.Admit(request.Get());
                ++probes;
                admitted += got;
                if (got != ShouldAdmit(kind, opcode, arguments)) { check(false, "gate sweep mismatch"); return; }
            }
        }
    }
    // DeviceReady, the Live state, and the descriptions of 0x500d and 0xd16a.
    check(admitted == expected_admitted, "exactly the read-only operations are admitted");
    check(probes == 3 * argument_sets.size() * 65536, "swept everything");
    if (!table_codes) return;
    // Every other table code, over every operation number and the read form.
    for (const auto& spec : kD810SettingSpecs) {
        if (spec.code == 0x500d || spec.code == 0xd16a) continue;
        auto request = Make(SettingsCommandKind::ExecuteWithDataToRead, 0, {spec.code});
        std::size_t accepted = 0;
        for (std::uint32_t opcode = 0; opcode <= 0xffff; ++opcode) {
            request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, opcode);
            D810SettingsReadGate gate = base;
            if (gate.Admit(request.Get())) { ++accepted; check(opcode == 0x1014, "only GetDevicePropDesc"); }
        }
        check(accepted == 1, "one operation per table code");
    }
}
// A gate that has seen a transfer with no data end with an explicit MTP refusal.
D810SettingsReadGate ReopenedGate() {
    D810SettingsReadGate gate;
    gate.Admit(Make(SettingsCommandKind::ExecuteWithDataToRead, 0x1014, {0x500d}).Get());
    gate.Observe(BeginReply(L"c1", 8));
    gate.Admit(ReadRequest(L"c1").Get());
    gate.Observe(ReadReplyOf(0));
    gate.Admit(EndRequest(L"c1").Get());
    gate.Observe(FinalReply(0x200a));
    return gate;
}
void GateSweep() {
    const std::vector<std::vector<std::uint32_t>> argument_sets{{}, {0xd1a2}, {0x500d}, {0x1016}, {0xd1a2, 0xd1a2}, {0xffffffffu}, {0xd16a}};
    SweepFromIdle(D810SettingsReadGate{}, argument_sets, 4, true);
    // The gate that came back to Idle after a refused END admits exactly the same set.
    const auto reopened = ReopenedGate();
    check(!reopened.Closed() && !reopened.TransferOpen(), "reopened gate is idle");
    // (The Debug build is slow: a smaller set of argument lists, DeviceReady, one description and the write codes among them.)
    SweepFromIdle(reopened, {{}, {0x500d}, {0x1016}}, 2, false);
    // Bits above 16 in the operation or an argument change the value: refused.
    for (const auto high : {0x00010000u, 0x80000000u, 0xffff0000u, 0x10000000u}) {
        for (const auto& [kind, opcode, argument] : std::vector<std::tuple<SettingsCommandKind, DWORD, DWORD>>{
                {SettingsCommandKind::ExecuteWithDataToRead, 0x1015, 0xd1a2}, {SettingsCommandKind::ExecuteWithDataToRead, 0x1014, 0x500d}}) {
            D810SettingsReadGate gate;
            check(!gate.Admit(Make(kind, opcode | high, {argument}).Get()), "high opcode bits");
            check(!gate.Admit(Make(kind, opcode, {argument | high}).Get()), "high argument bits");
            check(gate.Admit(Make(kind, opcode, {argument}).Get()), "baseline admitted");
        }
        D810SettingsReadGate gate;
        check(!gate.Admit(Make(SettingsCommandKind::ExecuteWithoutData, 0x90c8 | high, {}).Get()), "high ready bits");
    }
    // Arguments of another variant type or in excess are refused.
    for (const auto type : {VT_I4, VT_UI2, VT_UI8}) {
        auto request = Make(SettingsCommandKind::ExecuteWithDataToRead, 0x1014, {0x500d});
        SetArguments(request.Get(), {0x500d}, static_cast<VARTYPE>(type));
        D810SettingsReadGate gate;
        check(!gate.Admit(request.Get()), "variant type");
    }
    {
        D810SettingsReadGate gate;
        auto many = Make(SettingsCommandKind::ExecuteWithDataToRead, 0x1014, {0x500d, 0x500d});
        check(!gate.Admit(many.Get()), "two arguments");
        check(!gate.Admit(nullptr), "null");
        auto bare = Make(SettingsCommandKind::ExecuteWithDataToRead, 0x1014, {0x500d});
        bare->RemoveValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS);
        check(!gate.Admit(bare.Get()), "no parameter collection");
    }
    // The named operations that must never pass, in every kind and with every argument set.
    for (const DWORD opcode : {0x1016u, 0x90c7u, 0x90c2u, 0x9207u, 0x90c0u, 0x9201u, 0x9202u, 0x9203u, 0x9204u, 0x1008u, 0x1009u, 0x100bu, 0x100eu, 0x100fu, 0x1017u})
        for (const auto kind : {SettingsCommandKind::ExecuteWithoutData, SettingsCommandKind::ExecuteWithDataToRead, SettingsCommandKind::ExecuteWithDataToWrite})
            for (const auto& arguments : argument_sets) {
                auto request = Make(kind, opcode, {});
                SetArguments(request.Get(), arguments);
                D810SettingsReadGate gate;
                check(!gate.Admit(request.Get()), "forbidden operation");
            }
}

void GateTransferOrder() {
    const auto begin = [] { return Make(SettingsCommandKind::ExecuteWithDataToRead, 0x1014, {0x500d}); };
    const auto ready = [] { return Make(SettingsCommandKind::ExecuteWithoutData, 0x90c8, {}); };
    {
        D810SettingsReadGate gate;
        check(!gate.Admit(ReadRequest(L"c1").Get()) && !gate.Admit(EndRequest(L"c1").Get()), "no read or end without a transfer");
        check(gate.Admit(begin().Get()), "begin");
        check(!gate.Admit(begin().Get()) && !gate.Admit(ready().Get()) && !gate.Admit(ReadRequest(L"c1").Get()), "nothing while the begin reply is unseen");
        gate.Observe(BeginReply(L"c1", 8));
        check(gate.TransferOpen(), "open");
        check(!gate.Admit(ReadRequest(L"c2").Get()) && !gate.Admit(EndRequest(L"c2").Get()) && !gate.Admit(WithCommand(ReadRequest(L"c1"), WPD_COMMAND_MTP_EXT_READ_DATA, L"").Get()), "context mismatch");
        check(!gate.Admit(begin().Get()) && !gate.Admit(ready().Get()), "no second begin while open");
        check(!gate.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x500d}).Get()) &&
            !gate.Admit(WriteDataRequest(L"c1").Get()) && !gate.Admit(Make(SettingsCommandKind::ExecuteWithoutData, 0x1016, {0x500d}).Get()), "no write while open");
        check(gate.Admit(ReadRequest(L"c1").Get()) && gate.Admit(ReadRequest(L"c1").Get()), "reads of the own context");
        check(gate.Admit(EndRequest(L"c1").Get()), "end");
        check(!gate.Admit(ReadRequest(L"c1").Get()) && !gate.Admit(EndRequest(L"c1").Get()) && !gate.Admit(begin().Get()), "after END and before its reply: nothing");
        gate.Observe(FinalReply());
        check(!gate.TransferOpen() && !gate.Closed(), "closed transfer");
        check(!gate.Admit(ReadRequest(L"c1").Get()) && !gate.Admit(EndRequest(L"c1").Get()), "no read after END");
        check(gate.Admit(ready().Get()) && gate.Admit(begin().Get()), "a new operation after END");
    }
    {   // A begin the body refuses opens nothing.
        D810SettingsReadGate gate;
        check(gate.Admit(begin().Get()));
        gate.Observe(FinalReply(0x200a));
        check(!gate.TransferOpen() && !gate.Admit(ReadRequest(L"c1").Get()) && gate.Admit(begin().Get()), "refused begin");
    }
    {   // A transport failure before any context is seen closes the gate for good.
        D810SettingsReadGate gate;
        check(gate.Admit(begin().Get()));
        gate.TransportFailed();
        check(gate.Closed() && !gate.Admit(ready().Get()) && !gate.Admit(begin().Get()) && !gate.Admit(EndRequest(L"c1").Get()), "failed begin");
    }
    {   // A failed READ_DATA leaves only the END of that transfer.
        D810SettingsReadGate gate;
        gate.Admit(begin().Get());
        gate.Observe(BeginReply(L"c1", 8));
        gate.Admit(ReadRequest(L"c1").Get());
        gate.TransportFailed();
        check(!gate.Admit(ReadRequest(L"c1").Get()) && !gate.Admit(begin().Get()) && !gate.Admit(EndRequest(L"x").Get()), "after failure");
        check(gate.Admit(EndRequest(L"c1").Get()), "END still allowed");
        gate.Observe(FinalReply());
        check(gate.Closed() && !gate.Admit(ready().Get()), "closed after the failed transfer ended");
    }
    // The END of a transfer. `reads` are the replies to the READ_DATA before it.
    const auto ended_with = [&](const std::vector<WpdLiveViewExchange>& reads, const WpdLiveViewExchange& end, bool transport_failed = false) {
        D810SettingsReadGate gate;
        gate.Admit(begin().Get());
        gate.Observe(BeginReply(L"c1", 8));
        for (const auto& reply : reads) { gate.Admit(ReadRequest(L"c1").Get()); gate.Observe(reply); }
        if (transport_failed) gate.TransportFailed();
        gate.Admit(EndRequest(L"c1").Get());
        gate.Observe(end);
        return gate;
    };
    {   // An explicit MTP refusal of the END of a transfer that delivered no data: back to Idle.
        for (const DWORD code : {0x200au, 0x2019u, 0xa00bu}) {
            auto gate = ended_with({ReadReplyOf(0)}, FinalReply(code));
            check(!gate.Closed() && !gate.TransferOpen(), "empty transfer, END refused: idle");
            check(!gate.Admit(ReadRequest(L"c1").Get()) && !gate.Admit(EndRequest(L"c1").Get()), "no read or END after the refused END");
            check(gate.Admit(begin().Get()), "a new operation after the refused END");
        }
        check(!ended_with({}, FinalReply(0x2019)).Closed(), "no READ at all, END refused: idle");
        check(!ended_with({ReadReplyOf(0), ReadReplyOf(0)}, FinalReply(0x200a)).Closed(), "two empty READs, END refused: idle");
    }
    {   // Everything else about a refused END keeps the gate closed.
        check(ended_with({ReadReplyOf(3)}, FinalReply(0x200a)).Closed(), "data received, END refused");
        check(ended_with({ReadReplyOf(0), ReadReplyOf(2)}, FinalReply(0x200a)).Closed(), "data after an empty READ, END refused");
        check(ended_with({CommandFailedReply()}, FinalReply(0x200a)).Closed(), "READ failed, END refused");
        check(ended_with({{E_FAIL, Reply()}}, FinalReply(0x200a)).Closed(), "READ dispatch failed, END refused");
        check(ended_with({ReadReplyOf(0)}, FinalReply(0x200a), true).Closed(), "transport failure, END refused");
        check(ended_with({ReadReplyOf(0)}, CommandFailedReply()).Closed(), "END command HRESULT failed");
        check(ended_with({ReadReplyOf(0)}, {E_FAIL, Reply(true, 0x200a)}).Closed(), "END dispatch failed");
        check(ended_with({ReadReplyOf(0)}, {S_OK, Reply()}).Closed(), "END without a response code");
        check(ended_with({ReadReplyOf(0)}, FinalReply(0)).Closed(), "END response code zero");
        check(!ended_with({ReadReplyOf(3)}, FinalReply(0x2001)).Closed(), "data received, END accepted: idle as before");
    }
    {   // The data flag does not carry over to the next transfer.
        auto gate = ended_with({ReadReplyOf(3)}, FinalReply(0x2001));
        gate.Admit(begin().Get());
        gate.Observe(BeginReply(L"c2", 8));
        gate.Admit(ReadRequest(L"c2").Get());
        gate.Observe(ReadReplyOf(0));
        gate.Admit(EndRequest(L"c2").Get());
        gate.Observe(FinalReply(0x200a));
        check(!gate.Closed(), "a clean transfer after a dirty one");
    }
    {   // WRITE_DATA and the write command are refused in every state.
        D810SettingsReadGate idle;
        check(!idle.Admit(WriteDataRequest(L"c1").Get()) && !idle.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0xd1a2}).Get()), "idle");
    }
}

// --- Reads through the whole stack ----------------------------------------------------------------
bool NoWrites(const Rig& rig) { return rig.body.writes == 0; }
void HappyRead() {
    Rig rig(HappyBody());
    const auto read = rig.Run();
    check(read.Complete() && read.readings.size() == kD810SettingSpecs.size() && read.live_state == 0u, "complete");
    for (const auto& reading : read.readings) check(reading.available && reading.mtp_response == 0x2001, "available");
    const auto shutter = FindD810Reading(read.readings, D810SettingId::ShutterSpeed);
    check(shutter && shutter->desc.current_value == 1667 && FormatD810Setting(Spec(D810SettingId::ShutterSpeed), shutter->desc.current_value) == "1/6 s", "shutter value");
    check(rig.body.trace.front() == "90c8" && rig.body.trace[1] == "1015(d1a2)", "ready then live state first");
    check(rig.body.CountOf("1014(") == 12 && rig.body.CountOf("1015(") == 1 && rig.body.CountOf("90c8") == 1, "operations");
    check(rig.body.ends == 13 && rig.body.begins == 13, "each transfer ended once");
    check(NoWrites(rig) && rig.session->Gate().Rejected() == 0, "no writes, nothing refused");
    std::set<std::string> allowed{"90c8", "1015(d1a2)", "read", "end"};
    for (const auto& spec : kD810SettingSpecs) {
        char text[24];
        std::snprintf(text, sizeof text, "1014(%04x)", spec.code);
        allowed.insert(text);
    }
    for (const auto& entry : rig.body.trace) check(allowed.count(entry) != 0, "only allowed requests reached the body");
    check(rig.log.size() == 1 + kD810SettingSpecs.size(), "log lines");
    for (const auto& line : rig.log) check(!HasIdentifierShape(line) && line.find("CAM-B") == 0, "no identifier shapes in the log");
    check(rig.log[1] == "CAM-B settings_desc code=0x500e mtp=0x2001 type=0x0004 getset=1 form=2 count=2 current=0x0001 first=0x0001 last=0x0001", "first line");
    std::string all;
    for (const auto& line : rig.log) all += line + "\n";
    check(all.find("fake-context") == std::string::npos, "no transfer context in the log");
}
void LiveViewOn() {
    auto body = HappyBody();
    body.live = 1;
    Rig rig(std::move(body));
    const auto read = rig.Run();
    check(!read.Complete() && read.stop_reason == "live_view_on" && read.live_state == 1u, "live on");
    check(rig.body.CountOf("1014(") == 0 && rig.body.ends == 1, "no description is requested");
    check(read.readings.size() == kD810SettingSpecs.size(), "every row present");
    for (const auto& reading : read.readings) check(!reading.available && reading.reason == "not_read", "not read");
    check(NoWrites(rig), "no writes");
    auto other = HappyBody();
    other.live = 7;
    Rig odd(std::move(other));
    check(odd.Run().stop_reason == "live_state_unexpected" && odd.body.CountOf("1014(") == 0, "unexpected live state");
}
void UnreadableItems() {
    auto body = HappyBody();
    body.reject[0xd200] = 0x200a;
    body.descs[0x500b] = Desc(0x500b, 4, 1, 1, 1, 2, {1});
    body.descs[0x5007] = Desc(0x5007, 2, 1, 8, 8, 2, {8});    // wrong type
    body.descs[0x500f] = Desc(0x500f, 4, 3, 64, 64, 0);       // bad get/set
    body.descs.erase(0xd16a);                                 // answered by an MTP error
    Rig rig(std::move(body));
    const auto read = rig.Run();
    check(read.Complete(), "a refused item does not stop the rest");
    const auto reason = [&](D810SettingId id) { return FindD810Reading(read.readings, id)->reason; };
    check(reason(D810SettingId::PictureControl) == "mtp_rejected" && FindD810Reading(read.readings, D810SettingId::PictureControl)->mtp_response == 0x200a, "mtp rejected");
    check(reason(D810SettingId::Aperture) == "type_mismatch" && reason(D810SettingId::Iso) == "bad_get_set", "parser reasons");
    check(reason(D810SettingId::IsoAutoCandidate) == "mtp_rejected", "missing code");
    check(FindD810Reading(read.readings, D810SettingId::Metering)->available, "valid item stays");
    check(FindD810Reading(read.readings, D810SettingId::NikonShutterSpeed)->available, "items after a refusal are read");
    check(rig.body.CountOf("1014(") == 12 && NoWrites(rig), "all twelve asked once");
    const auto rows = CompareD810Settings(read.readings, read.readings);
    for (const auto& row : rows)
        if (row.id == D810SettingId::Aperture) check(row.difference == D810Difference::Unknown && row.a_text == "unavailable (type_mismatch)", "table shows the reason");
    for (const auto& line : rig.log) check(!HasIdentifierShape(line), "log");
    // The type the body answered with stays in the line for the operator.
    bool typed = false;
    for (const auto& line : rig.log) typed = typed || (line.find("code=0x5007 unavailable reason=type_mismatch") != std::string::npos && line.find("type=0x0002") != std::string::npos);
    check(typed, "type kept in the unavailable line");
}
void TransportFailure() {
    auto body = HappyBody();
    body.throw_on_read.insert(0x500b);
    Rig rig(std::move(body));
    const auto read = rig.Run();
    check(!read.Complete() && read.stop_reason == "not_read_after_failure", "stops after a transport failure");
    check(rig.session->Poisoned(), "poisoned");
    const auto metering = FindD810Reading(read.readings, D810SettingId::Metering);
    check(metering && !metering->available && metering->reason.rfind("transfer_failed_", 0) == 0, "failed item");
    check(FindD810Reading(read.readings, D810SettingId::ExposureCompensation)->reason == "not_read", "later items are not asked");
    check(rig.body.trace.back() == "end" && rig.body.CountOf("1014(") == 7, "only the open transfer is ended");
    check(rig.body.ends == 8, "the open transfer was ended once");
    check(NoWrites(rig), "no writes");
}
void DeadlineAndStop() {
    {   // The stop arrives while a description is open: that transfer is ended, then nothing.
        Rig rig(HappyBody());
        rig.body.after = [&](const std::string& entry) { if (entry == "1014(5007)") rig.go = false; };
        const auto read = rig.Run();
        check(!read.Complete() && read.stop_reason == "interrupted", "stopped");
        check(rig.body.trace.back() == "end" && rig.body.CountOf("1014(") == 3, "no new request after the stop");
        const auto aperture = FindD810Reading(read.readings, D810SettingId::Aperture);
        check(aperture && !aperture->available && aperture->reason == "interrupted", "interrupted item");
        check(read.readings.size() == kD810SettingSpecs.size() && FindD810Reading(read.readings, D810SettingId::Iso)->reason == "not_read", "rest not read");
        check(rig.body.ends == rig.body.begins && NoWrites(rig), "every opened transfer ended");
    }
    {   // Expired before the first request.
        Rig rig(HappyBody());
        rig.go = false;
        const auto read = rig.Run();
        check(read.stop_reason == "not_started" && rig.body.trace.empty(), "nothing sent");
    }
    {   // Expired between DeviceReady and the Live state.
        Rig rig(HappyBody());
        rig.body.after = [&](const std::string& entry) { if (entry == "90c8") rig.go = false; };
        const auto read = rig.Run();
        check(read.stop_reason == "interrupted" && rig.body.trace.size() == 1, "stopped after ready");
    }
}
void DeviceReadiness() {
    {
        auto body = HappyBody();
        body.busy_replies = 2;
        Rig rig(std::move(body));
        const auto read = rig.Run();
        check(read.Complete() && rig.body.CountOf("90c8") == 3, "busy then ready");
    }
    {
        auto body = HappyBody();
        body.ready_response = 0xa002;
        Rig rig(std::move(body));
        const auto read = rig.Run();
        check(read.stop_reason == "device_not_ready" && rig.body.trace.size() == 1, "not ready");
    }
    {   // A deadline while busy.
        auto body = HappyBody();
        body.busy_replies = 1000;
        Rig rig(std::move(body));
        rig.body.after = [&](const std::string&) { if (rig.body.CountOf("90c8") >= 3) rig.go = false; };
        const auto read = rig.Run();
        check(read.stop_reason == "interrupted" && rig.body.CountOf("90c8") == 3, "deadline while busy");
    }
}
// --- Refusals the body gives at the END of a transfer that carried no data -------------------------
const D810SettingReading& Row(const D810CameraSettingsRead& read, D810SettingId id) { return *FindD810Reading(read.readings, id); }
bool LogHas(const Rig& rig, const std::string& text) {
    return std::any_of(rig.log.begin(), rig.log.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
}
void RejectedAtEnd() {
    {   // Size unknown, READ delivers nothing, END answers 0x200a: the item is unavailable and the rest is read.
        auto body = HappyBody();
        body.unknown_size.insert(0xd200);
        body.serve_limit[0xd200] = 0;
        body.end_response[0xd200] = 0x200a;
        Rig rig(std::move(body));
        const auto read = rig.Run();
        check(read.Complete() && !rig.session->Poisoned() && !rig.session->Gate().Closed(), "refused END of an empty transfer does not stop the read");
        const auto& row = Row(read, D810SettingId::PictureControl);
        check(!row.available && row.reason == "mtp_rejected_at_end" && row.mtp_response == 0x200a, "reason and END code kept");
        check(LogHas(rig, "CAM-B settings_desc code=0xd200 unavailable reason=mtp_rejected_at_end mtp=0x200a"), "the row carries mtp=0x200a");
        check(Row(read, D810SettingId::ActiveDLighting).available && Row(read, D810SettingId::IsoAutoCandidate).available &&
            Row(read, D810SettingId::NikonShutterSpeed).available, "items after it are read");
        check(rig.body.CountOf("1014(") == 12 && rig.body.ends == rig.body.begins && NoWrites(rig), "all twelve asked once, every transfer ended once");
        check(rig.session->Gate().Rejected() == 0, "nothing refused by the gate");
        const auto rows = CompareD810Settings(read.readings, read.readings);
        for (const auto& line : rows)
            if (line.id == D810SettingId::PictureControl) check(line.difference == D810Difference::Unknown && line.a_text == "unavailable (mtp_rejected_at_end)", "table shows the reason");
    }
    {   // Size known, READ delivers nothing (incomplete payload), END answers 0x2019.
        auto body = HappyBody();
        body.serve_limit[0x500b] = 0;
        body.end_response[0x500b] = 0x2019;
        Rig rig(std::move(body));
        const auto read = rig.Run();
        const auto& row = Row(read, D810SettingId::Metering);
        check(read.Complete() && !row.available && row.reason == "mtp_rejected_at_end" && row.mtp_response == 0x2019, "known size, empty READ");
        check(Row(read, D810SettingId::ExposureCompensation).available && rig.body.CountOf("1014(") == 12, "later items read");
    }
    {   // Two items in a row.
        auto body = HappyBody();
        for (const std::uint16_t code : {std::uint16_t{0x500b}, std::uint16_t{0x5010}}) { body.serve_limit[code] = 0; body.end_response[code] = 0x200a; }
        Rig rig(std::move(body));
        const auto read = rig.Run();
        check(read.Complete() && !Row(read, D810SettingId::Metering).available && !Row(read, D810SettingId::ExposureCompensation).available &&
            Row(read, D810SettingId::PictureControl).available && rig.body.CountOf("1014(") == 12, "two refusals in a row");
    }
}
// Cases in which the read stops at an item (Metering, the seventh entry of the table).
void StopsAfterUncleanEnd(FakeBody body, const char* what, bool gate_closed = true) {
    Rig rig(std::move(body));
    const auto read = rig.Run();
    check(!read.Complete() && read.stop_reason == "not_read_after_failure" && rig.session->Poisoned(), what);
    check(Row(read, D810SettingId::Metering).reason.rfind("transfer_failed_", 0) == 0, what);
    check(Row(read, D810SettingId::ExposureCompensation).reason == "not_read" && rig.body.CountOf("1014(") == 7, what);
    check(rig.session->Gate().Closed() == gate_closed && rig.body.ends == rig.body.begins && NoWrites(rig), what);
}
void RejectedAtEndStopsWhenNotExplicitOrNotEmpty() {
    {   // Data was received before the refusal.
        auto body = HappyBody();
        body.serve_limit[0x500b] = 3;
        body.end_response[0x500b] = 0x200a;
        StopsAfterUncleanEnd(std::move(body), "partial data, END refused");
    }
    {   // Size unknown and the data is complete, END refused: not an empty transfer.
        auto body = HappyBody();
        body.unknown_size.insert(0x500b);
        body.end_response[0x500b] = 0x200a;
        StopsAfterUncleanEnd(std::move(body), "complete data, END refused");
    }
    {   // Empty and the END is accepted: nothing was refused, the payload is just empty.
        auto body = HappyBody();
        body.unknown_size.insert(0x500b);
        body.serve_limit[0x500b] = 0;
        // The gate saw a clean END and is idle, as before; the session stops anyway.
        StopsAfterUncleanEnd(std::move(body), "empty, END accepted", false);
    }
    {   // Empty and the END fails at the command level (HRESULT), or has no code, or throws.
        for (int variant = 0; variant < 3; ++variant) {
            auto body = HappyBody();
            body.serve_limit[0x500b] = 0;
            if (variant == 0) body.end_command_failed.insert(0x500b);
            if (variant == 1) body.end_without_code.insert(0x500b);
            if (variant == 2) body.throw_on_end.insert(0x500b);
            Rig rig(std::move(body));
            const auto read = rig.Run();
            check(!read.Complete() && read.stop_reason == "not_read_after_failure" && rig.session->Poisoned(), "END not an explicit refusal");
            check(Row(read, D810SettingId::Metering).reason.rfind("transfer_failed_", 0) == 0 && Row(read, D810SettingId::ExposureCompensation).reason == "not_read", "END not an explicit refusal: reason and rest");
            check(rig.body.CountOf("1014(") == 7 && rig.session->Gate().Closed() && NoWrites(rig), "END not an explicit refusal: nothing more asked");
        }
    }
    {   // READ threw, the END was refused: the exception wins.
        auto body = HappyBody();
        body.throw_on_read.insert(0x500b);
        body.end_response[0x500b] = 0x200a;
        StopsAfterUncleanEnd(std::move(body), "READ threw, END refused");
    }
}

// --- Begin failures ----------------------------------------------------------------------------------
void BeginCommandFailureStops() {
    // A failed HRESULT of the command (a session that went away, for one) is not a refusal by the body.
    auto body = HappyBody();
    body.begin_command_failed.insert(0x500b);
    Rig rig(std::move(body));
    const auto read = rig.Run();
    check(!read.Complete() && read.stop_reason == "not_read_after_failure" && rig.session->Poisoned(), "command HRESULT failure stops the read");
    check(Row(read, D810SettingId::Metering).reason == "command_failed" && !Row(read, D810SettingId::Metering).available, "reason kept");
    check(Row(read, D810SettingId::ExposureCompensation).reason == "not_read" && rig.body.CountOf("1014(") == 7, "later items are not asked");
    check(rig.body.ends == rig.body.begins - 1 && NoWrites(rig), "no transfer opened, no END");
}
void BeginMtpRejectionContinues() {
    // The same position with an MTP refusal at the begin: the read goes on (unchanged).
    auto body = HappyBody();
    body.reject[0x500b] = 0x2002;
    Rig rig(std::move(body));
    const auto read = rig.Run();
    check(read.Complete() && Row(read, D810SettingId::Metering).reason == "mtp_rejected" && Row(read, D810SettingId::Metering).mtp_response == 0x2002 &&
        rig.body.CountOf("1014(") == 12, "MTP refusal at the begin continues");
}

// --- Paths of the transfer that had no test ----------------------------------------------------------
void OversizeThenEnd() {
    {   // Larger than the cap: not read, the transfer is ended, the item is too_large and the read goes on.
        auto body = HappyBody();
        body.oversize.insert(0x500b);
        Rig rig(std::move(body));
        const auto read = rig.Run();
        check(read.Complete() && !rig.session->Poisoned(), "too large does not stop the read");
        check(Row(read, D810SettingId::Metering).reason == "too_large" && !Row(read, D810SettingId::Metering).available, "too_large");
        check(Row(read, D810SettingId::ExposureCompensation).available && rig.body.CountOf("1014(") == 12, "items after it are read");
        check(rig.body.ends == rig.body.begins && NoWrites(rig), "the transfer was ended");
        const auto at = std::find(rig.body.trace.begin(), rig.body.trace.end(), "1014(500b)");
        check(at != rig.body.trace.end() && at + 1 != rig.body.trace.end() && *(at + 1) == "end", "no READ_DATA for the oversize item");
    }
    {   // The END of that transfer is refused: stop.
        auto body = HappyBody();
        body.oversize.insert(0x500b);
        body.end_response[0x500b] = 0x200a;
        Rig rig(std::move(body));
        const auto read = rig.Run();
        check(!read.Complete() && rig.session->Poisoned() && Row(read, D810SettingId::Metering).reason.rfind("transfer_failed_", 0) == 0 &&
            Row(read, D810SettingId::ExposureCompensation).reason == "not_read" && rig.body.CountOf("1014(") == 7, "oversize and END refused: stop");
    }
}
void CleanupNotAdmittedStops() {
    // The session is no longer ours after the item was opened: the END is not sent, and nothing else is.
    Rig rig(HappyBody());
    rig.body.after = [&](const std::string& entry) { if (entry == "1014(5007)") rig.cleanup = false; };
    const auto read = rig.Run();
    check(!read.Complete() && read.stop_reason == "not_read_after_failure" && rig.session->Poisoned(), "stopped");
    check(Row(read, D810SettingId::Aperture).reason.rfind("transfer_failed_", 0) == 0 && Row(read, D810SettingId::Iso).reason == "not_read", "reasons");
    check(rig.body.ends == rig.body.begins - 1 && rig.body.CountOf("1014(") == 3 && rig.body.trace.back() == "read", "no END after the cleanup lost admission");
    check(rig.session->Gate().TransferOpen() && NoWrites(rig), "the open transfer stays open, nothing more is sent");
}
void UnknownSizeEndsWithShortRead() {
    // TOTAL_DATA_SIZE 0xFFFFFFFF: the payload ends at the first READ that returns less than asked.
    auto body = HappyBody();
    body.unknown_size = {0x500d, 0x500e};
    Rig rig(std::move(body));
    const auto read = rig.Run();
    check(read.Complete() && rig.body.ends == rig.body.begins && NoWrites(rig), "complete");
    const auto& shutter = Row(read, D810SettingId::ShutterSpeed);
    check(shutter.available && shutter.desc.current_value == 1667 && Row(read, D810SettingId::ExposureMode).available, "unknown-size items parse");
    for (const auto& reading : read.readings) check(reading.available && reading.mtp_response == 0x2001, "available");
}

// --- Nothing write-shaped reaches a transport, whatever it is asked ----------------------------
struct RecordingBody {
    struct Seen { SettingsCommandKind kind; std::uint32_t opcode; std::vector<std::uint32_t> arguments; };
    std::vector<Seen> seen;
    std::size_t unreadable = 0;
    FakeBody inner;
    WpdLiveViewExchange operator()(IPortableDeviceValues* request) {
        const auto shape = InspectSettingsRequest(request);
        if (!shape) { ++unreadable; return FinalReply(); }
        seen.push_back({shape->kind, shape->opcode, shape->arguments});
        return inner(request);
    }
    std::size_t WriteShaped() const {
        std::size_t count = 0;
        for (const auto& entry : seen) {
            const bool executes = entry.kind == SettingsCommandKind::ExecuteWithoutData || entry.kind == SettingsCommandKind::ExecuteWithDataToRead;
            count += entry.kind == SettingsCommandKind::ExecuteWithDataToWrite || entry.kind == SettingsCommandKind::WriteData ||
                entry.kind == SettingsCommandKind::Other || (executes && (entry.opcode == 0x1016 || entry.opcode == 0x9207 || entry.opcode == 0x90c2));
        }
        return count;
    }
    bool OnlyReadOperations() const {
        for (const auto& entry : seen) {
            if (entry.kind == SettingsCommandKind::ReadData || entry.kind == SettingsCommandKind::EndDataTransfer) continue;
            if (!ShouldAdmit(entry.kind, entry.opcode, entry.arguments)) return false;
        }
        return true;
    }
};
void SessionNeverSendsWrites() {
    const auto run = [](FakeBody inner, std::span<const D810SettingSpec> specs, const char* what) {
        RecordingBody recorder{{}, 0, std::move(inner)};
        WpdSettingsReadSession session([&](IPortableDeviceValues* request) { return recorder(request); }, [] { return true; }, [] { return true; });
        const auto read = session.ReadAll("CAM-A", nullptr, specs);
        check(!recorder.seen.empty() && recorder.WriteShaped() == 0 && recorder.unreadable == 0 && recorder.inner.writes == 0, what);
        check(recorder.OnlyReadOperations(), what);
        check(read.readings.size() == specs.size(), what);
        return std::make_pair(std::move(recorder), session.Gate().Rejected());
    };
    const std::span<const D810SettingSpec> table(kD810SettingSpecs);
    {   // Every kind of body answer in one read: refusals, empty transfers, unknown sizes, oversize.
        auto body = HappyBody();
        body.reject[0x5007] = 0x2002;
        body.serve_limit[0xd200] = 0; body.end_response[0xd200] = 0x200a;
        body.unknown_size.insert(0x500d);
        body.oversize.insert(0x500f);
        const auto [recorder, rejected] = run(std::move(body), table, "mixed answers");
        check(rejected == 0 && recorder.seen.size() > 12, "nothing refused by the gate, the whole table was asked");
    }
    {   // After a stop (an exception at a READ_DATA) nothing more goes out.
        auto body = HappyBody();
        body.throw_on_read.insert(0x500b);
        const auto [recorder, rejected] = run(std::move(body), table, "after a failure");
        check(recorder.seen.back().kind == SettingsCommandKind::EndDataTransfer, "only the END of the open transfer follows the failure");
    }
    {   // Table entries whose codes name a write or a vendor action are never sent: the gate refuses them.
        std::array<D810SettingSpec, 3> bad{{
            {D810SettingId::ExposureMode, 0x1016, D810ValueType::Uint16, "x", true},
            {D810SettingId::ShutterSpeed, 0x9207, D810ValueType::Uint32, "y", true},
            {D810SettingId::Aperture, 0x90c2, D810ValueType::Uint16, "z", true}}};
        const auto [recorder, rejected] = run(HappyBody(), bad, "codes outside the table");
        const bool described = std::any_of(recorder.seen.begin(), recorder.seen.end(), [](const auto& entry) { return entry.opcode == 0x1014 && entry.kind == SettingsCommandKind::ExecuteWithDataToRead; });
        check(rejected >= 1 && !described && recorder.inner.CountOf("1014(") == 0, "no description request reached the body");
    }
}
}

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    const auto start = std::chrono::steady_clock::now();
    // A thrown exception is a failed contract with a name, not an abort.
    const auto run = [](const char* name, void (*test)()) {
        try { test(); }
        catch (const std::exception& error) { ++failures; std::cerr << name << " threw: " << error.what() << "\n"; }
        catch (...) { ++failures; std::cerr << name << " threw\n"; }
    };
    run("ParserBoundaries", ParserBoundaries);
    run("Formatting", Formatting);
    run("Comparison", Comparison);
    run("GateSweep", GateSweep);
    run("GateTransferOrder", GateTransferOrder);
    run("HappyRead", HappyRead);
    run("LiveViewOn", LiveViewOn);
    run("UnreadableItems", UnreadableItems);
    run("TransportFailure", TransportFailure);
    run("DeadlineAndStop", DeadlineAndStop);
    run("DeviceReadiness", DeviceReadiness);
    run("RejectedAtEnd", RejectedAtEnd);
    run("RejectedAtEndStopsWhenNotExplicitOrNotEmpty", RejectedAtEndStopsWhenNotExplicitOrNotEmpty);
    run("BeginCommandFailureStops", BeginCommandFailureStops);
    run("BeginMtpRejectionContinues", BeginMtpRejectionContinues);
    run("OversizeThenEnd", OversizeThenEnd);
    run("CleanupNotAdmittedStops", CleanupNotAdmittedStops);
    run("UnknownSizeEndsWithShortRead", UnknownSizeEndsWithShortRead);
    run("SessionNeverSendsWrites", SessionNeverSendsWrites);
    std::cout << checks << " checks, " << failures << " failures, "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << " ms\n";
    CoUninitialize();
    return failures ? 1 : 0;
}
