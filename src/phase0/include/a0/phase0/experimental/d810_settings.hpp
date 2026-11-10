#pragma once
// Read-only camera setting table for the development preview (#416 stage A).
// Pure functions only: no Windows headers, no device access. A setting is a
// property code, a scalar type and a display rule. Property codes and scalar
// types are cross-checked against the public libgphoto2 sources at the fixed
// commit listed in third_party/PTP_PROTOCOL_REFERENCES.md. Nothing here
// builds a request that changes the camera.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace a0::phase0::experimental {
enum class D810ValueType : std::uint16_t { Int8 = 1, Uint8 = 2, Int16 = 3, Uint16 = 4, Int32 = 5, Uint32 = 6 };
inline constexpr std::size_t D810ValueBytes(D810ValueType type) {
    switch (type) {
    case D810ValueType::Int8: case D810ValueType::Uint8: return 1;
    case D810ValueType::Int16: case D810ValueType::Uint16: return 2;
    default: return 4;
    }
}
inline constexpr bool D810ValueSigned(D810ValueType type) {
    return type == D810ValueType::Int8 || type == D810ValueType::Int16 || type == D810ValueType::Int32;
}

enum class D810SettingId {
    ExposureMode, ShutterSpeed, Aperture, Iso, IsoAuto, WhiteBalance, Metering, ExposureCompensation,
    PictureControl, ActiveDLighting, IsoAutoCandidate, NikonShutterSpeed,
};
// in_table: shown in the side-by-side table. The rest is read for the
// diagnostic line only (which of two candidate codes the body answers to).
struct D810SettingSpec {
    D810SettingId id;
    std::uint16_t code;
    D810ValueType type;
    const char* name;
    bool in_table;
};
// String-typed properties are deliberately absent: the parser refuses them.
// Picture Control (0xD200) has no entry in the upstream configuration table, so
// its scalar type is an assumption (not cross-checked); a different type is reported as
// unavailable with the type the body returned, never coerced.
inline constexpr std::array<D810SettingSpec, 12> kD810SettingSpecs{{
    {D810SettingId::ExposureMode, 0x500e, D810ValueType::Uint16, "exposure_mode", true},
    {D810SettingId::ShutterSpeed, 0x500d, D810ValueType::Uint32, "shutter_speed", true},
    {D810SettingId::Aperture, 0x5007, D810ValueType::Uint16, "aperture", true},
    {D810SettingId::Iso, 0x500f, D810ValueType::Uint16, "iso", true},
    {D810SettingId::IsoAuto, 0xd054, D810ValueType::Uint8, "iso_auto", true},
    {D810SettingId::WhiteBalance, 0x5005, D810ValueType::Uint16, "white_balance", true},
    {D810SettingId::Metering, 0x500b, D810ValueType::Uint16, "metering", true},
    {D810SettingId::ExposureCompensation, 0x5010, D810ValueType::Int16, "exposure_compensation", true},
    {D810SettingId::PictureControl, 0xd200, D810ValueType::Uint8, "picture_control", true},
    {D810SettingId::ActiveDLighting, 0xd14e, D810ValueType::Uint8, "active_d_lighting", true},
    {D810SettingId::IsoAutoCandidate, 0xd16a, D810ValueType::Uint8, "iso_auto_candidate", false},
    {D810SettingId::NikonShutterSpeed, 0xd100, D810ValueType::Uint32, "nikon_shutter_speed", false},
}};
inline constexpr std::uint16_t kD810LiveStateCode = 0xd1a2;
// Upper bounds of one property description. The device is not trusted: a
// longer payload is refused before it is parsed. Both are provisional.
inline constexpr std::size_t kD810PropDescMaxBytes = 4096;
inline constexpr std::size_t kD810EnumMaxValues = 1024;

inline const D810SettingSpec* FindD810Setting(std::uint32_t code) {
    for (const auto& spec : kD810SettingSpecs)
        if (spec.code == code) return &spec;
    return nullptr;
}
inline const D810SettingSpec& D810SettingOf(D810SettingId id) {
    for (const auto& spec : kD810SettingSpecs)
        if (spec.id == id) return spec;
    return kD810SettingSpecs[0];
}

// --- DevicePropDesc (PTP standard layout, little endian) -------------------
// code u16 | data type u16 | get/set u8 | default | current | form flag u8 |
// form (none | range: min max step | enum: count u16, values)
enum class D810PropDescError {
    None, TooLarge, TooShort, TrailingBytes, CodeMismatch, TypeUnsupported, TypeMismatch,
    BadGetSet, BadFormFlag, RangeInverted, EnumCountOutOfRange,
};
inline const char* D810PropDescErrorToken(D810PropDescError error) {
    switch (error) {
    case D810PropDescError::None: return "none";
    case D810PropDescError::TooLarge: return "too_large";
    case D810PropDescError::TooShort: return "too_short";
    case D810PropDescError::TrailingBytes: return "trailing_bytes";
    case D810PropDescError::CodeMismatch: return "code_mismatch";
    case D810PropDescError::TypeUnsupported: return "type_unsupported";
    case D810PropDescError::TypeMismatch: return "type_mismatch";
    case D810PropDescError::BadGetSet: return "bad_get_set";
    case D810PropDescError::BadFormFlag: return "bad_form_flag";
    case D810PropDescError::RangeInverted: return "range_inverted";
    case D810PropDescError::EnumCountOutOfRange: return "enum_count_out_of_range";
    }
    return "unknown";
}
// Values are kept as 64-bit integers: sign-extended for signed types, zero
// extended for unsigned ones, so range and equality tests need no type switch.
struct D810PropDesc {
    std::uint16_t code{};
    std::uint16_t data_type{};
    std::uint8_t get_set{};
    std::uint8_t form_flag{};
    std::int64_t default_value{}, current_value{};
    std::int64_t range_min{}, range_max{}, range_step{};
    std::vector<std::int64_t> values;
    // GetSet 1 only. A range with step 0 offers no value to choose (stage B).
    bool Writable() const { return get_set == 1 && !(form_flag == 1 && range_step == 0); }
};
struct D810PropDescParse {
    D810PropDescError error{D810PropDescError::TooShort};
    // Header fields read before an error stopped the parse. Diagnostic only.
    D810PropDesc desc;
    bool Ok() const { return error == D810PropDescError::None; }
};
inline std::int64_t D810DecodeValue(D810ValueType type, std::span<const unsigned char> data, std::size_t offset) {
    std::uint32_t raw{};
    const auto size = D810ValueBytes(type);
    for (std::size_t i = 0; i < size; ++i) raw |= static_cast<std::uint32_t>(data[offset + i]) << (8 * i);
    switch (type) {
    case D810ValueType::Int8: return static_cast<std::int8_t>(raw);
    case D810ValueType::Int16: return static_cast<std::int16_t>(raw);
    case D810ValueType::Int32: return static_cast<std::int32_t>(raw);
    default: return raw;
    }
}
inline D810PropDescParse ParseD810PropDesc(std::span<const unsigned char> data, const D810SettingSpec& spec) {
    D810PropDescParse out;
    const auto fail = [&](D810PropDescError error) { out.error = error; return out; };
    if (data.size() > kD810PropDescMaxBytes) return fail(D810PropDescError::TooLarge);
    if (data.size() < 5) return fail(D810PropDescError::TooShort);
    const auto u16 = [&](std::size_t at) { return static_cast<std::uint16_t>(data[at] | (data[at + 1] << 8)); };
    out.desc.code = u16(0);
    if (out.desc.code != spec.code) return fail(D810PropDescError::CodeMismatch);
    out.desc.data_type = u16(2);
    // Scalar integers up to 32 bit only. Strings, arrays and 64/128 bit
    // integers are refused, not skipped.
    if (out.desc.data_type < 1 || out.desc.data_type > 6) return fail(D810PropDescError::TypeUnsupported);
    if (out.desc.data_type != static_cast<std::uint16_t>(spec.type)) return fail(D810PropDescError::TypeMismatch);
    const auto size = D810ValueBytes(spec.type);
    const std::size_t form_at = 5 + 2 * size;
    if (data.size() < form_at + 1) return fail(D810PropDescError::TooShort);
    out.desc.get_set = data[4];
    if (out.desc.get_set > 1) return fail(D810PropDescError::BadGetSet);
    out.desc.default_value = D810DecodeValue(spec.type, data, 5);
    out.desc.current_value = D810DecodeValue(spec.type, data, 5 + size);
    out.desc.form_flag = data[form_at];
    if (out.desc.form_flag > 2) return fail(D810PropDescError::BadFormFlag);
    std::size_t expected = form_at + 1;
    std::size_t count = 0;
    if (out.desc.form_flag == 1) expected += 3 * size;
    else if (out.desc.form_flag == 2) {
        if (data.size() < expected + 2) return fail(D810PropDescError::TooShort);
        count = u16(expected);
        if (count < 1 || count > kD810EnumMaxValues) return fail(D810PropDescError::EnumCountOutOfRange);
        expected += 2 + count * size;
    }
    if (data.size() < expected) return fail(D810PropDescError::TooShort);
    if (data.size() > expected) return fail(D810PropDescError::TrailingBytes);
    if (out.desc.form_flag == 1) {
        out.desc.range_min = D810DecodeValue(spec.type, data, form_at + 1);
        out.desc.range_max = D810DecodeValue(spec.type, data, form_at + 1 + size);
        out.desc.range_step = D810DecodeValue(spec.type, data, form_at + 1 + 2 * size);
        if (out.desc.range_min > out.desc.range_max) return fail(D810PropDescError::RangeInverted);
    } else if (out.desc.form_flag == 2) {
        for (std::size_t i = 0; i < count; ++i) out.desc.values.push_back(D810DecodeValue(spec.type, data, form_at + 3 + i * size));
    }
    out.error = D810PropDescError::None;
    return out;
}

// --- Request shape (what the read-only gate may ever admit) ------------------
enum class SettingsCommandKind {
    Other, ExecuteWithoutData, ExecuteWithDataToRead, ExecuteWithDataToWrite, ReadData, WriteData, EndDataTransfer,
};
enum class SettingsOperation { DeviceReady, LiveState, PropDesc };
// The operation a start-of-exchange request asks for, or nothing when it is not
// one of the three read-only ones. Operation numbers and arguments are compared
// as 32 bit values; each operation is tied to one command kind.
inline std::optional<SettingsOperation> ClassifySettingsExecute(SettingsCommandKind kind, std::uint32_t opcode,
    std::span<const std::uint32_t> arguments) {
    if (kind == SettingsCommandKind::ExecuteWithoutData && opcode == 0x90c8 && arguments.empty())
        return SettingsOperation::DeviceReady;
    if (kind != SettingsCommandKind::ExecuteWithDataToRead || arguments.size() != 1) return {};
    if (opcode == 0x1015 && arguments[0] == kD810LiveStateCode) return SettingsOperation::LiveState;
    if (opcode == 0x1014 && FindD810Setting(arguments[0])) return SettingsOperation::PropDesc;
    return {};
}

// --- Display ------------------------------------------------------------------
inline std::string D810Printf(const char* format, double number) {
    char text[48]{};
    std::snprintf(text, sizeof text, format, number);
    return text;
}
inline std::string D810HexValue(const D810SettingSpec& spec, std::int64_t value) {
    char text[24]{};
    const auto bytes = D810ValueBytes(spec.type);
    std::snprintf(text, sizeof text, "0x%0*llx", static_cast<int>(2 * bytes),
        static_cast<unsigned long long>(static_cast<std::uint64_t>(value) & (bytes == 4 ? 0xffffffffULL : (1ULL << (8 * bytes)) - 1)));
    return text;
}
inline std::string D810RawText(const D810SettingSpec& spec, std::int64_t value) { return "raw " + D810HexValue(spec, value); }
// Shutter speed: the body reports 1/10000 s units. The fraction shown is
// derived and rounded; below 10 units the resolution is too coarse to name a
// fraction, so the units are shown as they are.
inline std::string FormatD810ShutterSpeed(const D810SettingSpec& spec, std::int64_t value) {
    const auto raw = static_cast<std::uint32_t>(value);
    if (raw == 0xffffffffu) return "Bulb";
    if (raw == 0xfffffffdu) return "Time";
    if (raw == 0) return D810RawText(spec, value);
    if (raw < 10) return std::to_string(raw) + "/10000 s";
    if (raw >= 10000) return D810Printf("%.4g", raw / 10000.0) + " s";
    const double divisor = 10000.0 / raw, rounded = std::round(divisor);
    if (std::fabs(divisor - rounded) <= 0.005 * divisor) return "1/" + std::to_string(static_cast<long long>(rounded)) + " s";
    return D810Printf("%.4g", raw / 10000.0) + " s";
}
inline std::string FormatD810Setting(const D810SettingSpec& spec, std::int64_t value) {
    switch (spec.id) {
    case D810SettingId::ExposureMode:
        switch (value) {
        case 1: return "M";
        case 2: return "P";
        case 3: return "A";
        case 4: return "S";
        default: return D810RawText(spec, value);
        }
    case D810SettingId::ShutterSpeed:
    case D810SettingId::NikonShutterSpeed: return FormatD810ShutterSpeed(spec, value);
    case D810SettingId::Aperture: return "F" + D810Printf("%g", value / 100.0);
    case D810SettingId::Iso: return std::to_string(value);
    case D810SettingId::IsoAuto:
    case D810SettingId::IsoAutoCandidate:
        return value == 0 ? "OFF" : value == 1 ? "ON" : D810RawText(spec, value);
    case D810SettingId::WhiteBalance:
        switch (value) {
        case 1: return "Manual";
        case 2: return "Automatic";
        case 3: return "One-push Automatic";
        case 4: return "Daylight";
        case 5: return "Fluorescent";
        case 6: return "Tungsten";
        case 7: return "Flash";
        case 0x8010: return "Cloudy";
        case 0x8011: return "Shade";
        case 0x8012: return "Color Temperature";
        case 0x8013: return "Preset";
        default: return D810RawText(spec, value);
        }
    case D810SettingId::Metering:
        switch (value) {
        case 1: return "Average";
        case 2: return "Center Weighted";
        case 3: return "Multi Spot";
        case 4: return "Center Spot";
        default: return D810RawText(spec, value);
        }
    case D810SettingId::ExposureCompensation: return D810Printf("%+.2f", value / 1000.0) + " EV";
    case D810SettingId::PictureControl:
    case D810SettingId::ActiveDLighting: return D810RawText(spec, value);
    }
    return D810RawText(spec, value);
}

// --- Readings and the side-by-side comparison --------------------------------
struct D810SettingReading {
    D810SettingId id{};
    bool available{};
    // Short ASCII token when not available (see D810PropDescErrorToken and the
    // reader's transport reasons). Never a device string.
    std::string reason;
    std::uint32_t mtp_response{};
    D810PropDesc desc;
};
inline D810SettingReading UnavailableD810Setting(D810SettingId id, std::string reason, std::uint32_t mtp_response = 0) {
    D810SettingReading reading;
    reading.id = id;
    reading.reason = std::move(reason);
    reading.mtp_response = mtp_response;
    return reading;
}
// One camera's readings, one per entry of kD810SettingSpecs in that order.
using D810SettingsSnapshot = std::vector<D810SettingReading>;

inline const D810SettingReading* FindD810Reading(const D810SettingsSnapshot& snapshot, D810SettingId id) {
    for (const auto& reading : snapshot)
        if (reading.id == id) return &reading;
    return nullptr;
}
inline std::string D810ReadingText(const D810SettingsSnapshot& snapshot, const D810SettingSpec& spec) {
    const auto* reading = FindD810Reading(snapshot, spec.id);
    if (!reading) return "unavailable (not_read)";
    if (!reading->available) return "unavailable (" + (reading->reason.empty() ? std::string("unknown") : reading->reason) + ")";
    return FormatD810Setting(spec, reading->desc.current_value);
}
enum class D810Difference { Same, Different, Unknown };
struct D810ComparisonRow {
    D810SettingId id{};
    std::string name, a_text, b_text;
    D810Difference difference{D810Difference::Unknown};
};
// Table rows only. Unknown: at least one side could not be read, so no claim
// is made about equality.
inline std::vector<D810ComparisonRow> CompareD810Settings(const D810SettingsSnapshot& a, const D810SettingsSnapshot& b) {
    std::vector<D810ComparisonRow> rows;
    for (const auto& spec : kD810SettingSpecs) {
        if (!spec.in_table) continue;
        D810ComparisonRow row;
        row.id = spec.id;
        row.name = spec.name;
        row.a_text = D810ReadingText(a, spec);
        row.b_text = D810ReadingText(b, spec);
        const auto* left = FindD810Reading(a, spec.id);
        const auto* right = FindD810Reading(b, spec.id);
        if (left && right && left->available && right->available)
            row.difference = left->desc.current_value == right->desc.current_value ? D810Difference::Same : D810Difference::Different;
        rows.push_back(std::move(row));
    }
    return rows;
}
inline const char* D810DifferenceToken(D810Difference difference) {
    return difference == D810Difference::Same ? "same" : difference == D810Difference::Different ? "different" : "unknown";
}
inline std::size_t CountD810Differences(const std::vector<D810ComparisonRow>& rows) {
    std::size_t count{};
    for (const auto& row : rows)
        if (row.difference == D810Difference::Different) ++count;
    return count;
}

// --- Log line ----------------------------------------------------------------
// `alias` is CAM-A or CAM-B. The line carries property numbers and values only:
// no device path, serial, transfer context or raw bytes.
inline std::string FormatD810SettingLogLine(const std::string& alias, const D810SettingSpec& spec, const D810SettingReading& reading) {
    char head[96]{};
    std::snprintf(head, sizeof head, "%s settings_desc code=0x%04x", alias.c_str(), static_cast<unsigned>(spec.code));
    std::string line = head;
    char text[96]{};
    if (!reading.available) {
        std::snprintf(text, sizeof text, " unavailable reason=%s mtp=0x%04x", reading.reason.empty() ? "unknown" : reading.reason.c_str(),
            static_cast<unsigned>(reading.mtp_response));
        line += text;
        if (reading.desc.data_type) {
            std::snprintf(text, sizeof text, " type=0x%04x", static_cast<unsigned>(reading.desc.data_type));
            line += text;
        }
        return line;
    }
    const auto& desc = reading.desc;
    std::snprintf(text, sizeof text, " mtp=0x%04x type=0x%04x getset=%u form=%u", static_cast<unsigned>(reading.mtp_response),
        static_cast<unsigned>(desc.data_type), static_cast<unsigned>(desc.get_set), static_cast<unsigned>(desc.form_flag));
    line += text;
    std::string first = "none", last = "none";
    std::size_t count = 0;
    if (desc.form_flag == 2 && !desc.values.empty()) {
        count = desc.values.size();
        first = D810HexValue(spec, desc.values.front());
        last = D810HexValue(spec, desc.values.back());
    } else if (desc.form_flag == 1) {
        first = D810HexValue(spec, desc.range_min);
        last = D810HexValue(spec, desc.range_max);
    }
    line += " count=" + std::to_string(count) + " current=" + D810HexValue(spec, desc.current_value) + " first=" + first + " last=" + last;
    if (desc.form_flag == 1) line += " step=" + D810HexValue(spec, desc.range_step);
    return line;
}
}
