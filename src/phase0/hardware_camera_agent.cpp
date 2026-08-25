#include "a0/phase0/hardware_camera_agent.hpp"

#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include "a0/phase0/wpd_transport.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace a0::phase0 {
namespace {

constexpr std::size_t kMaximumProtocolJsonBytes = 64U * 1024U;
constexpr std::uintmax_t kMaximumVerifiedJpegBytes = 256U * 1024U * 1024U;
constexpr std::uint16_t kSingleOriginalWidth = 7360U;
constexpr std::uint16_t kSingleOriginalHeight = 4912U;
constexpr std::string_view kTransactionJournalSchema =
    "a0.camera-agent.transaction.v1";

bool HasExpectedSingleOriginalDimensions(
    const std::vector<unsigned char>& bytes) {
    if (!IsValidJpeg(bytes)) return false;
    std::size_t offset = 2;
    while (offset + 1 < bytes.size()) {
        if (bytes[offset++] != 0xFFU) return false;
        while (offset < bytes.size() && bytes[offset] == 0xFFU) ++offset;
        if (offset >= bytes.size()) return false;
        const unsigned char marker = bytes[offset++];
        if (marker == 0x00U) return false;
        if (marker == 0xD9U || marker == 0xDAU) return false;
        if (marker == 0x01U || (marker >= 0xD0U && marker <= 0xD7U)) {
            continue;
        }
        if (offset + 2 > bytes.size()) return false;
        const std::size_t segment_length =
            (static_cast<std::size_t>(bytes[offset]) << 8U) |
            static_cast<std::size_t>(bytes[offset + 1]);
        if (segment_length < 2 || segment_length > bytes.size() - offset) {
            return false;
        }
        const bool is_start_of_frame =
            marker >= 0xC0U && marker <= 0xCFU &&
            marker != 0xC4U && marker != 0xC8U && marker != 0xCCU;
        if (is_start_of_frame) {
            if (segment_length < 7) return false;
            const std::uint16_t height = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(bytes[offset + 3]) << 8U) |
                bytes[offset + 4]);
            const std::uint16_t width = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(bytes[offset + 5]) << 8U) |
                bytes[offset + 6]);
            return width == kSingleOriginalWidth &&
                height == kSingleOriginalHeight;
        }
        offset += segment_length;
    }
    return false;
}

enum class JsonKind {
    object,
    string,
    boolean,
    integer,
    null_value,
};

struct JsonValue {
    JsonKind kind{JsonKind::object};
    std::map<std::string, JsonValue> object;
    std::string string;
    bool boolean{};
    std::int64_t integer{};
};

[[noreturn]] void ProtocolFailure(std::string code, std::string message) {
    throw HardwareCameraAgentProtocolError(std::move(code), std::move(message));
}

void AppendUtf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7FU) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
        ProtocolFailure("MalformedEnvelope", "surrogate escapes are not accepted in hardware protocol fields");
    } else {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

class JsonParser final {
public:
    explicit JsonParser(
        std::string_view input,
        bool reject_duplicate_fields = true)
        : input_(input), reject_duplicate_fields_(reject_duplicate_fields) {
        if (input.empty() || input.size() > kMaximumProtocolJsonBytes) {
            ProtocolFailure("MalformedEnvelope", "hardware protocol JSON length is outside the allowed range");
        }
    }

    [[nodiscard]] JsonValue Parse() {
        SkipWhitespace();
        JsonValue value = ParseValue(0);
        SkipWhitespace();
        if (position_ != input_.size()) {
            ProtocolFailure("MalformedEnvelope", "unexpected data follows the hardware protocol JSON object");
        }
        return value;
    }

private:
    [[nodiscard]] JsonValue ParseValue(int depth) {
        if (depth > 4 || position_ >= input_.size()) {
            ProtocolFailure("MalformedEnvelope", "hardware protocol JSON nesting is invalid");
        }
        const char current = input_[position_];
        if (current == '{') return ParseObject(depth + 1);
        if (current == '"') {
            JsonValue value;
            value.kind = JsonKind::string;
            value.string = ParseString();
            return value;
        }
        if (current == 't' || current == 'f') return ParseBoolean();
        if (current == 'n') return ParseNull();
        if (current == '-' || (current >= '0' && current <= '9')) return ParseInteger();
        ProtocolFailure("MalformedEnvelope", "hardware protocol JSON contains an unsupported value type");
    }

    [[nodiscard]] JsonValue ParseObject(int depth) {
        JsonValue value;
        value.kind = JsonKind::object;
        ++position_;
        SkipWhitespace();
        if (Consume('}')) return value;
        for (;;) {
            if (position_ >= input_.size() || input_[position_] != '"') {
                ProtocolFailure("MalformedEnvelope", "hardware protocol JSON object key is invalid");
            }
            std::string key = ParseString();
            SkipWhitespace();
            if (!Consume(':')) ProtocolFailure("MalformedEnvelope", "hardware protocol JSON object is missing ':'");
            SkipWhitespace();
            JsonValue child = ParseValue(depth);
            if (!value.object.emplace(std::move(key), std::move(child)).second &&
                reject_duplicate_fields_) {
                ProtocolFailure("MalformedEnvelope", "duplicate hardware protocol JSON field");
            }
            SkipWhitespace();
            if (Consume('}')) break;
            if (!Consume(',')) ProtocolFailure("MalformedEnvelope", "hardware protocol JSON object is missing ','");
            SkipWhitespace();
        }
        return value;
    }

    [[nodiscard]] std::string ParseString() {
        if (!Consume('"')) ProtocolFailure("MalformedEnvelope", "hardware protocol JSON string is invalid");
        std::string output;
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return output;
            if (character < 0x20U) {
                ProtocolFailure("MalformedEnvelope", "hardware protocol JSON string contains a control character");
            }
            if (character != '\\') {
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= input_.size()) ProtocolFailure("MalformedEnvelope", "incomplete JSON escape");
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                if (input_.size() - position_ < 4) ProtocolFailure("MalformedEnvelope", "incomplete JSON unicode escape");
                std::uint32_t code_point = 0;
                for (int index = 0; index < 4; ++index) {
                    const char hex = input_[position_++];
                    code_point <<= 4U;
                    if (hex >= '0' && hex <= '9') code_point |= static_cast<std::uint32_t>(hex - '0');
                    else if (hex >= 'a' && hex <= 'f') code_point |= static_cast<std::uint32_t>(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F') code_point |= static_cast<std::uint32_t>(hex - 'A' + 10);
                    else ProtocolFailure("MalformedEnvelope", "invalid JSON unicode escape");
                }
                AppendUtf8(output, code_point);
                break;
            }
            default:
                ProtocolFailure("MalformedEnvelope", "unsupported JSON escape");
            }
        }
        ProtocolFailure("MalformedEnvelope", "unterminated hardware protocol JSON string");
    }

    [[nodiscard]] JsonValue ParseBoolean() {
        JsonValue value;
        value.kind = JsonKind::boolean;
        if (input_.substr(position_, 4) == "true") {
            position_ += 4;
            value.boolean = true;
            return value;
        }
        if (input_.substr(position_, 5) == "false") {
            position_ += 5;
            value.boolean = false;
            return value;
        }
        ProtocolFailure("MalformedEnvelope", "invalid hardware protocol JSON boolean");
    }

    [[nodiscard]] JsonValue ParseInteger() {
        const std::size_t start = position_;
        if (input_[position_] == '-') ++position_;
        if (position_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ProtocolFailure("MalformedEnvelope", "invalid hardware protocol JSON integer");
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ProtocolFailure("MalformedEnvelope", "hardware protocol JSON integer has a leading zero");
            }
        } else {
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        if (position_ < input_.size() &&
            (input_[position_] == '.' || input_[position_] == 'e' || input_[position_] == 'E')) {
            ProtocolFailure("MalformedEnvelope", "hardware protocol accepts integers only");
        }
        JsonValue value;
        value.kind = JsonKind::integer;
        try {
            value.integer = std::stoll(std::string(input_.substr(start, position_ - start)));
        } catch (...) {
            ProtocolFailure("MalformedEnvelope", "hardware protocol JSON integer is outside the allowed range");
        }
        return value;
    }

    [[nodiscard]] JsonValue ParseNull() {
        if (input_.substr(position_, 4) != "null") {
            ProtocolFailure("MalformedEnvelope", "invalid hardware protocol JSON null");
        }
        position_ += 4;
        JsonValue value;
        value.kind = JsonKind::null_value;
        return value;
    }

    void SkipWhitespace() {
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n') break;
            ++position_;
        }
    }

    bool Consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    std::string_view input_;
    std::size_t position_{};
    bool reject_duplicate_fields_{};
};

std::optional<std::string> TryParseResponseSchemaVersion(
    std::string_view json) noexcept {
    try {
        const JsonValue root = JsonParser(json, false).Parse();
        if (root.kind != JsonKind::object) return std::nullopt;
        const auto found = root.object.find("schemaVersion");
        if (found == root.object.end() || found->second.kind != JsonKind::string) {
            return std::nullopt;
        }
        if (found->second.string == kHardwareCameraAgentSchemaVersion ||
            found->second.string == kHardwareCameraAgentLiveViewSchemaVersion) {
            return found->second.string;
        }
    } catch (...) {
    }
    return std::nullopt;
}

const JsonValue& RequireField(const JsonValue& object, std::string_view name, JsonKind kind) {
    if (object.kind != JsonKind::object) ProtocolFailure("MalformedEnvelope", "hardware protocol envelope must be an object");
    const auto found = object.object.find(std::string(name));
    if (found == object.object.end()) ProtocolFailure("MissingField", "hardware protocol envelope is missing a required field");
    if (found->second.kind != kind) ProtocolFailure("InvalidFieldType", "hardware protocol field has an invalid type");
    return found->second;
}

void RequireExactFields(const JsonValue& object, const std::set<std::string>& expected) {
    if (object.kind != JsonKind::object || object.object.size() != expected.size()) {
        ProtocolFailure("UnexpectedField", "hardware protocol object has missing or unexpected fields");
    }
    for (const auto& [name, ignored] : object.object) {
        (void)ignored;
        if (!expected.contains(name)) ProtocolFailure("UnexpectedField", "hardware protocol object contains an unexpected field");
    }
}

const JsonValue& RequireAnyField(const JsonValue& object, std::string_view name) {
    if (object.kind != JsonKind::object) {
        ProtocolFailure("MalformedEnvelope", "hardware protocol value must be an object");
    }
    const auto found = object.object.find(std::string(name));
    if (found == object.object.end()) {
        ProtocolFailure("MissingField", "hardware protocol object is missing a required field");
    }
    return found->second;
}

bool IsSafeRequestId(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' || character == '-' || character == '_';
    });
}

bool IsSafeTransactionId(std::string_view value) noexcept {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

bool IsLowerHex(std::string_view value, std::size_t expected_size) noexcept {
    return value.size() == expected_size &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

bool IsSafeRunId(std::string_view value) noexcept {
    if (value.size() < 7 || value.size() > 64 || !value.starts_with("run-")) return false;
    const std::size_t separator = value.find('-', 4);
    if (separator == std::string_view::npos || separator == 4 || separator + 1 == value.size()) {
        return false;
    }
    return std::all_of(value.begin() + 4, value.begin() + separator, [](unsigned char character) {
               return std::isdigit(character) != 0;
           }) &&
        std::all_of(value.begin() + separator + 1, value.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        });
}

std::string TransactionLeaseName(std::string_view transaction_id) {
    if (!IsSafeTransactionId(transaction_id)) {
        throw HardwareCameraAgentProtocolError(
            "InvalidTransactionId", "transactionId must contain exactly 32 hexadecimal characters");
    }
    return "A0CameraStitcher.CameraAgent.Transaction.v1." + std::string(transaction_id);
}

void ValidateAlias(std::string_view alias) {
    if (alias != "CAM-A" && alias != "CAM-B") {
        ProtocolFailure("InvalidAlias", "hardware request alias must be CAM-A or CAM-B");
    }
}

ObservedCameraSetting ToObservedSetting(
    const SdkCameraStatus::SettingCapability& source) {
    ObservedCameraSetting result;
    result.available = source.available;
    result.cap_type = source.cap_type;
    result.probe_state = source.probe_state;
    result.value_type = source.value_type;
    result.current_value = source.current_value;
    result.current_index = source.current_index;
    result.current_label = source.current_label;
    return result;
}

ObservedCameraSettings ToObservedSettings(const SdkCameraStatus& source) {
    ObservedCameraSettings result;
    result.file_type = ToObservedSetting(source.file_type);
    result.compression_level = ToObservedSetting(source.compression_level);
    result.image_size = ToObservedSetting(source.image_size);
    result.exposure_mode = ToObservedSetting(source.exposure_mode);
    result.shutter_speed = ToObservedSetting(source.shutter_speed);
    result.aperture = ToObservedSetting(source.aperture);
    result.sensitivity = ToObservedSetting(source.sensitivity);
    result.white_balance_mode = ToObservedSetting(source.wb_mode);
    result.focus_mode = ToObservedSetting(source.focus_mode);
    return result;
}

std::string JsonEscape(std::string_view value);
std::string Bool(bool value);

void AppendObservedSettingJson(
    std::ostringstream& output,
    const ObservedCameraSetting& setting) {
    output << "{\"available\":" << Bool(setting.available)
           << ",\"capType\":\"" << JsonEscape(setting.cap_type)
           << "\",\"probeState\":\"" << JsonEscape(setting.probe_state)
           << "\",\"valueType\":\"" << JsonEscape(setting.value_type)
           << "\",\"currentValue\":";
    if (setting.current_value) output << *setting.current_value;
    else output << "null";
    output << ",\"currentIndex\":";
    if (setting.current_index) output << *setting.current_index;
    else output << "null";
    output << ",\"currentLabel\":";
    if (setting.current_label) output << '"' << JsonEscape(*setting.current_label) << '"';
    else output << "null";
    output << '}';
}

void AppendObservedSettingsJson(
    std::ostringstream& output,
    const ObservedCameraSettings& settings) {
    output << "{\"fileType\":";
    AppendObservedSettingJson(output, settings.file_type);
    output << ",\"compressionLevel\":";
    AppendObservedSettingJson(output, settings.compression_level);
    output << ",\"imageSize\":";
    AppendObservedSettingJson(output, settings.image_size);
    output << ",\"exposureMode\":";
    AppendObservedSettingJson(output, settings.exposure_mode);
    output << ",\"shutterSpeed\":";
    AppendObservedSettingJson(output, settings.shutter_speed);
    output << ",\"aperture\":";
    AppendObservedSettingJson(output, settings.aperture);
    output << ",\"sensitivity\":";
    AppendObservedSettingJson(output, settings.sensitivity);
    output << ",\"whiteBalanceMode\":";
    AppendObservedSettingJson(output, settings.white_balance_mode);
    output << ",\"focusMode\":";
    AppendObservedSettingJson(output, settings.focus_mode);
    output << '}';
}

struct SettingExpectation {
    std::optional<bool> available;
    std::optional<std::string> cap_type;
    std::optional<std::string> probe_state;
    std::optional<std::string> value_type;
    bool current_value_specified{};
    std::optional<std::uint32_t> current_value;
    bool current_index_specified{};
    std::optional<std::uint32_t> current_index;
    bool current_label_specified{};
    std::optional<std::string> current_label;
};

struct ApprovedCaptureProfile {
    std::string profile_id;
    std::uint32_t profile_version{};
    std::string selected_alias;
    std::string sha256;
    std::string expires_at_utc;
    FILETIME expires_at{};
    std::map<std::string, SettingExpectation> expected_settings;
};

FILETIME ParseProfileUtc(std::string_view value);

bool ProfileIsCurrentlyValid(const ApprovedCaptureProfile& profile) noexcept {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return CompareFileTime(&profile.expires_at, &now) > 0;
}

bool IsValidProfileTimestamp(std::string_view value) noexcept {
    try {
        (void)ParseProfileUtc(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool IsFutureProfileTimestamp(std::string_view value) noexcept {
    try {
        const FILETIME timestamp = ParseProfileUtc(value);
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        return CompareFileTime(&timestamp, &now) > 0;
    } catch (...) {
        return false;
    }
}

bool IsBoundedProfileText(std::string_view value, std::size_t maximum) noexcept {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= 0x20U && character != 0x7FU;
        });
}

FILETIME ParseProfileUtc(std::string_view value) {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
        throw std::runtime_error("capture profile UTC timestamp format is invalid");
    }
    const auto number = [&](std::size_t offset, std::size_t count) -> WORD {
        unsigned int result = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const unsigned char character = static_cast<unsigned char>(value[offset + index]);
            if (!std::isdigit(character)) {
                throw std::runtime_error("capture profile UTC timestamp contains a non-digit");
            }
            result = result * 10U + static_cast<unsigned int>(character - '0');
        }
        return static_cast<WORD>(result);
    };
    SYSTEMTIME time{};
    time.wYear = number(0, 4);
    time.wMonth = number(5, 2);
    time.wDay = number(8, 2);
    time.wHour = number(11, 2);
    time.wMinute = number(14, 2);
    time.wSecond = number(17, 2);
    FILETIME file_time{};
    if (!SystemTimeToFileTime(&time, &file_time)) {
        throw std::runtime_error("capture profile UTC timestamp is not a valid date/time");
    }
    return file_time;
}

SettingExpectation ParseSettingExpectation(const JsonValue& value) {
    static const std::set<std::string> allowed{
        "available", "capType", "probeState", "valueType",
        "currentValue", "currentIndex", "currentLabel",
    };
    if (value.kind != JsonKind::object || value.object.empty()) {
        throw std::runtime_error("capture profile setting expectation must be a non-empty object");
    }
    for (const auto& [name, ignored] : value.object) {
        (void)ignored;
        if (!allowed.contains(name)) {
            throw std::runtime_error("capture profile setting expectation has an unknown field");
        }
    }
    SettingExpectation result;
    const auto string_field = [&](std::string_view name) -> std::optional<std::string> {
        const auto found = value.object.find(std::string(name));
        if (found == value.object.end()) return std::nullopt;
        if (found->second.kind != JsonKind::string ||
            !IsBoundedProfileText(found->second.string, 256)) {
            throw std::runtime_error("capture profile setting string expectation is invalid");
        }
        return found->second.string;
    };
    if (const auto found = value.object.find("available"); found != value.object.end()) {
        if (found->second.kind != JsonKind::boolean) {
            throw std::runtime_error("capture profile available expectation must be boolean");
        }
        result.available = found->second.boolean;
    }
    result.cap_type = string_field("capType");
    result.probe_state = string_field("probeState");
    result.value_type = string_field("valueType");
    const auto nullable_uint = [&](std::string_view name, bool& specified)
        -> std::optional<std::uint32_t> {
        const auto found = value.object.find(std::string(name));
        if (found == value.object.end()) return std::nullopt;
        specified = true;
        if (found->second.kind == JsonKind::null_value) return std::nullopt;
        if (found->second.kind != JsonKind::integer || found->second.integer < 0 ||
            static_cast<std::uint64_t>(found->second.integer) >
                std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("capture profile numeric expectation is invalid");
        }
        return static_cast<std::uint32_t>(found->second.integer);
    };
    result.current_value = nullable_uint("currentValue", result.current_value_specified);
    result.current_index = nullable_uint("currentIndex", result.current_index_specified);
    if (const auto found = value.object.find("currentLabel"); found != value.object.end()) {
        result.current_label_specified = true;
        if (found->second.kind == JsonKind::string) {
            if (!IsBoundedProfileText(found->second.string, 256)) {
                throw std::runtime_error("capture profile label expectation is invalid");
            }
            result.current_label = found->second.string;
        } else if (found->second.kind != JsonKind::null_value) {
            throw std::runtime_error("capture profile label expectation must be string or null");
        }
    }
    return result;
}

const ObservedCameraSetting& ObservedSettingByName(
    const ObservedCameraSettings& settings,
    std::string_view name) {
    if (name == "fileType") return settings.file_type;
    if (name == "compressionLevel") return settings.compression_level;
    if (name == "imageSize") return settings.image_size;
    if (name == "exposureMode") return settings.exposure_mode;
    if (name == "shutterSpeed") return settings.shutter_speed;
    if (name == "aperture") return settings.aperture;
    if (name == "sensitivity") return settings.sensitivity;
    if (name == "whiteBalanceMode") return settings.white_balance_mode;
    if (name == "focusMode") return settings.focus_mode;
    throw std::runtime_error("capture profile names an unsupported setting");
}

bool MatchesExpectation(
    const ObservedCameraSetting& observed,
    const SettingExpectation& expected) noexcept {
    return (!expected.available || observed.available == *expected.available) &&
        (!expected.cap_type || observed.cap_type == *expected.cap_type) &&
        (!expected.probe_state || observed.probe_state == *expected.probe_state) &&
        (!expected.value_type || observed.value_type == *expected.value_type) &&
        (!expected.current_value_specified || observed.current_value == expected.current_value) &&
        (!expected.current_index_specified || observed.current_index == expected.current_index) &&
        (!expected.current_label_specified || observed.current_label == expected.current_label);
}

bool MatchesApprovedProfile(
    const ObservedCameraSettings& observed,
    const ApprovedCaptureProfile& profile) {
    for (const auto& [name, expectation] : profile.expected_settings) {
        if (!MatchesExpectation(ObservedSettingByName(observed, name), expectation)) return false;
    }
    return true;
}

bool LiveViewIsConfirmedOff(const SdkCameraStatus& status) noexcept {
    return status.live_view_status_available && status.live_view_status == "off";
}

void SetLiveViewPreflightFailure(
    const SdkCameraStatus& status,
    SingleCameraCaptureResult& result,
    std::string_view detail_suffix = {}) {
    result.succeeded = false;
    result.terminal_state = "Blocked";
    if (!status.live_view_status_available) {
        result.error_category = "live_view_status_unavailable";
        result.error_detail =
            "Live View OFF could not be confirmed before the WPD boundary";
    } else {
        result.error_category = "live_view_not_off";
        result.error_detail =
            "Live View was not OFF before the WPD boundary";
    }
    if (!detail_suffix.empty()) {
        result.error_detail += "; ";
        result.error_detail += detail_suffix;
    }
}

ApprovedCaptureProfile ParseApprovedCaptureProfile(std::string_view json) {
    const JsonValue root = JsonParser(json).Parse();
    RequireExactFields(root, {
        "schemaVersion", "profileId", "profileVersion", "selectedAlias", "cameraMode", "approved", "approvedBy",
        "approvalReference", "approvedAtUtc", "expiresAtUtc", "expectedSettings",
    });
    if (RequireField(root, "schemaVersion", JsonKind::string).string !=
            "a0.camera-agent.capture-profile.v1" ||
        RequireField(root, "cameraMode", JsonKind::string).string != "SingleCamera" ||
        !RequireField(root, "approved", JsonKind::boolean).boolean) {
        throw std::runtime_error("capture profile is not an approved Single profile");
    }
    ApprovedCaptureProfile profile;
    profile.sha256 = Sha256Hex(
        std::vector<unsigned char>(json.begin(), json.end()));
    profile.profile_id = RequireField(root, "profileId", JsonKind::string).string;
    const auto profile_version =
        RequireField(root, "profileVersion", JsonKind::integer).integer;
    if (profile_version < 1 ||
        static_cast<std::uint64_t>(profile_version) >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("capture profile version is invalid");
    }
    profile.profile_version = static_cast<std::uint32_t>(profile_version);
    profile.selected_alias =
        RequireField(root, "selectedAlias", JsonKind::string).string;
    if (profile.selected_alias != "CAM-A" && profile.selected_alias != "CAM-B") {
        throw std::runtime_error("capture profile selectedAlias is invalid");
    }
    if (!IsSafeRequestId(profile.profile_id) ||
        !IsBoundedProfileText(RequireField(root, "approvedBy", JsonKind::string).string, 128) ||
        !IsBoundedProfileText(
            RequireField(root, "approvalReference", JsonKind::string).string, 256)) {
        throw std::runtime_error("capture profile identity or approval provenance is invalid");
    }
    const FILETIME approved_at = ParseProfileUtc(
        RequireField(root, "approvedAtUtc", JsonKind::string).string);
    profile.expires_at_utc =
        RequireField(root, "expiresAtUtc", JsonKind::string).string;
    const FILETIME expires_at = ParseProfileUtc(profile.expires_at_utc);
    profile.expires_at = expires_at;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    if (CompareFileTime(&approved_at, &now) > 0 ||
        CompareFileTime(&approved_at, &expires_at) >= 0) {
        throw std::runtime_error("capture profile approval time window is invalid");
    }
    static const std::set<std::string> supported_settings{
        "fileType", "compressionLevel", "imageSize", "exposureMode", "shutterSpeed",
        "aperture", "sensitivity", "whiteBalanceMode", "focusMode",
    };
    const JsonValue& settings = RequireField(root, "expectedSettings", JsonKind::object);
    if (settings.object.empty()) {
        throw std::runtime_error("approved capture profile must choose at least one setting expectation");
    }
    for (const auto& [name, value] : settings.object) {
        if (!supported_settings.contains(name)) {
            throw std::runtime_error("approved capture profile names an unsupported setting");
        }
        profile.expected_settings.emplace(name, ParseSettingExpectation(value));
    }
    return profile;
}

std::string JsonEscape(std::string_view value) {
    std::ostringstream output;
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u00" << kHex[(character >> 4U) & 0x0FU] << kHex[character & 0x0FU];
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    return output.str();
}

std::string SafeErrorDetail(std::string_view value) {
    std::string output;
    output.reserve(std::min<std::size_t>(value.size(), 512));
    for (const unsigned char character : value) {
        if (output.size() >= 512) break;
        output.push_back(character >= 0x20U && character < 0x7FU ? static_cast<char>(character) : '?');
    }
    return output;
}

std::string Bool(bool value) {
    return value ? "true" : "false";
}

std::string PathForJson(const fs::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(utf8.data()),
        utf8.size());
}

fs::path PathFromUtf8(std::string_view value) {
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const unsigned char character : value) {
        utf8.push_back(static_cast<char8_t>(character));
    }
    return fs::path(utf8);
}

std::vector<unsigned char> ReadBoundedFile(
    const fs::path& path,
    std::uintmax_t maximum_bytes = kMaximumVerifiedJpegBytes) {
    std::error_code size_error;
    const std::uintmax_t size = fs::file_size(path, size_error);
    if (size_error || size == 0 || size > maximum_bytes ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("file size is outside the verified JPEG boundary");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("verified JPEG could not be opened");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw std::runtime_error("verified JPEG could not be read completely");
    }
    return bytes;
}

void WriteFileExclusiveAndFlush(const fs::path& path, const std::string& contents) {
    const HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "exclusive journal write failed");
    }
    bool succeeded = false;
    try {
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                contents.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD written = 0;
            if (!WriteFile(handle, contents.data() + offset, chunk, &written, nullptr) || written == 0) {
                throw std::system_error(
                    static_cast<int>(GetLastError()),
                    std::system_category(),
                    "journal write failed");
            }
            offset += written;
        }
        if (!FlushFileBuffers(handle)) {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "journal flush failed");
        }
        succeeded = true;
    } catch (...) {
        CloseHandle(handle);
        throw;
    }
    CloseHandle(handle);
    if (!succeeded) throw std::runtime_error("journal write failed");
}

void WriteBytesExclusiveAndFlush(
    const fs::path& path,
    const std::vector<unsigned char>& contents) {
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(), "exclusive JPEG write failed");
    }
    try {
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                contents.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD written = 0;
            if (!WriteFile(handle, contents.data() + offset, chunk, &written, nullptr) || written == 0) {
                throw std::system_error(
                    static_cast<int>(GetLastError()), std::system_category(), "JPEG write failed");
            }
            offset += written;
        }
        if (!FlushFileBuffers(handle)) {
            throw std::system_error(
                static_cast<int>(GetLastError()), std::system_category(), "JPEG flush failed");
        }
    } catch (...) {
        CloseHandle(handle);
        throw;
    }
    CloseHandle(handle);
}

void AtomicReplaceText(
    const fs::path& final,
    const std::string& contents,
    const std::function<void()>& before_replace = {}) {
    if (before_replace) before_replace();
    else fs::create_directories(final.parent_path());
    fs::path partial = final;
    partial += "." + NewRunId() + ".partial";
    WriteFileExclusiveAndFlush(partial, contents);
    if (before_replace) before_replace();
    if (!MoveFileExW(
            partial.c_str(),
            final.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(), "atomic journal rename failed");
    }
}

void ValidateArtifactRunNoReparse(const fs::path& run_root);
void ValidateTransactionJournalScope(
    const fs::path& transaction_root,
    std::string_view transaction_id);

PreviewJpegRecord PersistPreviewJpeg(
    const fs::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const std::vector<unsigned char>& bytes) {
    if (!IsValidJpeg(bytes) || bytes.empty() || bytes.size() > kMaximumVerifiedJpegBytes) {
        throw std::runtime_error("Live View preview is not a bounded valid JPEG");
    }
    const fs::path run_root = fs::absolute(artifacts_root) / std::string(run_id);
    ValidateArtifactRunNoReparse(run_root);
    const fs::path directory =
        run_root / "live-view" / std::string(camera_alias);
    fs::create_directories(directory);
    ValidateArtifactRunNoReparse(run_root);
    const fs::path partial = directory / "preview.jpg.partial";
    const fs::path final = directory / "preview.jpg";
    if (fs::exists(partial) || fs::exists(final)) {
        throw std::runtime_error("refusing to overwrite an existing Live View preview");
    }
    const std::string expected_sha256 = Sha256Hex(bytes);
    WriteBytesExclusiveAndFlush(partial, bytes);
    const auto partial_bytes = ReadBoundedFile(partial);
    if (partial_bytes.size() != bytes.size() || !IsValidJpeg(partial_bytes) ||
        Sha256Hex(partial_bytes) != expected_sha256) {
        throw std::runtime_error("Live View preview partial verification failed");
    }
    if (!MoveFileExW(partial.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(), "atomic preview rename failed");
    }
    const auto persisted = ReadBoundedFile(final);
    if (persisted.size() != bytes.size() || !IsValidJpeg(persisted) ||
        Sha256Hex(persisted) != expected_sha256) {
        throw std::runtime_error("Live View preview reread verification failed");
    }
    ValidateArtifactRunNoReparse(run_root);
    return PreviewJpegRecord{fs::absolute(final), persisted.size(), expected_sha256};
}

std::string SerializeTransactionJournal(const SingleCameraCaptureResult& result) {
    std::ostringstream output;
    output << "{\"schemaVersion\":\"" << kTransactionJournalSchema
           << "\",\"cameraMode\":\"SingleCamera\",\"requiredCameraAlias\":\""
           << JsonEscape(result.camera_alias)
           << "\",\"transactionId\":\"" << JsonEscape(result.transaction_id)
           << "\",\"captureProfileId\":\"" << JsonEscape(result.capture_profile_id)
           << "\",\"captureProfileVersion\":" << result.capture_profile_version
           << ",\"captureProfileSha256\":\""
           << JsonEscape(result.capture_profile_sha256)
           << "\",\"captureProfileCameraAlias\":\""
           << JsonEscape(result.capture_profile_camera_alias)
           << "\",\"profileExpiresAtUtc\":\""
           << JsonEscape(result.profile_expires_at_utc)
           << "\",\"liveViewHandoffRequested\":" << Bool(result.live_view_handoff_requested)
           << ",\"liveViewStoppedBeforeCapture\":"
           << Bool(result.live_view_stopped_before_capture)
           << ",\"liveViewSdkSessionClosedBeforeCapture\":"
           << Bool(result.live_view_sdk_session_closed_before_capture)
           << ",\"postCaptureLiveViewProbeAttempted\":" << Bool(result.live_view_resume_attempted)
           << ",\"postCaptureLiveViewProbeSucceeded\":" << Bool(result.live_view_resumed)
           << ",\"postCapturePreviewPresent\":" << Bool(result.resumed_preview.has_value())
           << ",\"postCapturePreviewPath\":\""
           << JsonEscape(result.resumed_preview ? PathForJson(result.resumed_preview->path) : "")
           << "\",\"postCapturePreviewSizeBytes\":"
           << (result.resumed_preview ? result.resumed_preview->size : 0)
           << ",\"postCapturePreviewSha256\":\""
           << JsonEscape(result.resumed_preview ? result.resumed_preview->sha256 : "")
           << "\",\"cameraAlias\":\"" << JsonEscape(result.camera_alias)
           << "\",\"runId\":\"" << JsonEscape(result.run_id)
           << "\",\"succeeded\":" << Bool(result.succeeded)
           << ",\"terminalState\":\"" << JsonEscape(result.terminal_state)
           << "\",\"errorCategory\":\"" << JsonEscape(result.error_category)
           << "\",\"errorDetail\":\"" << JsonEscape(SafeErrorDetail(result.error_detail))
           << "\",\"originalPresent\":" << Bool(result.retained_original.has_value())
           << ",\"originalCameraAlias\":\""
           << JsonEscape(result.retained_original ? result.retained_original->camera_alias : "")
           << "\",\"originalPath\":\""
           << JsonEscape(result.retained_original ? PathForJson(result.retained_original->path) : "")
           << "\",\"originalSizeBytes\":"
           << (result.retained_original ? result.retained_original->size : 0)
           << ",\"originalSha256\":\""
           << JsonEscape(result.retained_original ? result.retained_original->sha256 : "")
           << "\",\"spoolEmptyBeforeCapture\":" << Bool(result.spool_empty_before_capture)
           << ",\"cameraObjectDeleteAttempted\":" << Bool(result.camera_object_delete_attempted)
           << ",\"cameraObjectDeleteSucceeded\":" << Bool(result.camera_object_delete_succeeded)
           << ",\"spoolEmptyAfterCleanup\":" << Bool(result.spool_empty_after_cleanup)
           << ",\"automaticRetryCount\":" << result.automatic_retry_count
           << ",\"transactionWatchdogSeconds\":" << result.transaction_watchdog_seconds
           << ",\"realIdentifiersIncluded\":" << Bool(result.real_identifiers_included) << '}';
    return output.str();
}

SingleCameraCaptureResult ParseTransactionJournal(
    const fs::path& path,
    std::string_view expected_transaction_id) {
    const auto bytes = ReadBoundedFile(path, kMaximumProtocolJsonBytes);
    const std::string json(bytes.begin(), bytes.end());
    const JsonValue root = JsonParser(json).Parse();
    RequireExactFields(root, {
        "schemaVersion", "cameraMode", "requiredCameraAlias", "transactionId",
        "cameraAlias", "runId", "captureProfileId",
        "captureProfileVersion", "captureProfileSha256", "captureProfileCameraAlias",
        "profileExpiresAtUtc",
        "liveViewHandoffRequested",
        "liveViewStoppedBeforeCapture", "liveViewSdkSessionClosedBeforeCapture",
        "postCaptureLiveViewProbeAttempted", "postCaptureLiveViewProbeSucceeded",
        "postCapturePreviewPresent", "postCapturePreviewPath",
        "postCapturePreviewSizeBytes", "postCapturePreviewSha256", "succeeded",
        "terminalState", "errorCategory", "errorDetail", "originalPresent",
        "originalCameraAlias", "originalPath", "originalSizeBytes", "originalSha256",
        "spoolEmptyBeforeCapture", "cameraObjectDeleteAttempted",
        "cameraObjectDeleteSucceeded", "spoolEmptyAfterCleanup", "automaticRetryCount",
        "transactionWatchdogSeconds", "realIdentifiersIncluded",
    });
    if (RequireField(root, "schemaVersion", JsonKind::string).string != kTransactionJournalSchema) {
        throw std::runtime_error("transaction journal schema is unsupported");
    }
    if (RequireField(root, "cameraMode", JsonKind::string).string != "SingleCamera") {
        throw std::runtime_error("transaction journal camera mode is invalid");
    }
    SingleCameraCaptureResult result;
    result.transaction_id = RequireField(root, "transactionId", JsonKind::string).string;
    if (result.transaction_id != expected_transaction_id || !IsSafeTransactionId(result.transaction_id)) {
        throw std::runtime_error("transaction journal ID does not match its path");
    }
    result.camera_alias = RequireField(root, "cameraAlias", JsonKind::string).string;
    ValidateAlias(result.camera_alias);
    if (RequireField(root, "requiredCameraAlias", JsonKind::string).string !=
        result.camera_alias) {
        throw std::runtime_error("transaction journal required camera alias is inconsistent");
    }
    result.run_id = RequireField(root, "runId", JsonKind::string).string;
    if (!IsSafeRunId(result.run_id)) {
        throw std::runtime_error("transaction journal run ID is invalid");
    }
    result.capture_profile_id =
        RequireField(root, "captureProfileId", JsonKind::string).string;
    const auto capture_profile_version =
        RequireField(root, "captureProfileVersion", JsonKind::integer).integer;
    if (capture_profile_version < 0 ||
        static_cast<std::uint64_t>(capture_profile_version) >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("transaction journal capture profile version is invalid");
    }
    result.capture_profile_version =
        static_cast<std::uint32_t>(capture_profile_version);
    result.capture_profile_sha256 =
        RequireField(root, "captureProfileSha256", JsonKind::string).string;
    result.capture_profile_camera_alias =
        RequireField(root, "captureProfileCameraAlias", JsonKind::string).string;
    result.profile_expires_at_utc =
        RequireField(root, "profileExpiresAtUtc", JsonKind::string).string;
    if ((!result.capture_profile_id.empty() && !IsSafeRequestId(result.capture_profile_id)) ||
        (!result.capture_profile_sha256.empty() &&
         !IsLowerHex(result.capture_profile_sha256, 64)) ||
        (result.capture_profile_id.empty() != result.capture_profile_sha256.empty()) ||
        (result.capture_profile_id.empty() != (result.capture_profile_version == 0)) ||
        (result.capture_profile_id.empty() != result.capture_profile_camera_alias.empty()) ||
        (result.capture_profile_id.empty() != result.profile_expires_at_utc.empty()) ||
        (!result.capture_profile_camera_alias.empty() &&
         result.capture_profile_camera_alias != "CAM-A" &&
         result.capture_profile_camera_alias != "CAM-B")) {
        throw std::runtime_error("transaction journal capture profile identity is invalid");
    }
    if (!result.profile_expires_at_utc.empty()) {
        (void)ParseProfileUtc(result.profile_expires_at_utc);
    }
    result.live_view_handoff_requested =
        RequireField(root, "liveViewHandoffRequested", JsonKind::boolean).boolean;
    result.live_view_stopped_before_capture =
        RequireField(root, "liveViewStoppedBeforeCapture", JsonKind::boolean).boolean;
    result.live_view_sdk_session_closed_before_capture =
        RequireField(root, "liveViewSdkSessionClosedBeforeCapture", JsonKind::boolean).boolean;
    result.live_view_resume_attempted =
        RequireField(root, "postCaptureLiveViewProbeAttempted", JsonKind::boolean).boolean;
    result.live_view_resumed =
        RequireField(root, "postCaptureLiveViewProbeSucceeded", JsonKind::boolean).boolean;
    const bool resumed_preview_present =
        RequireField(root, "postCapturePreviewPresent", JsonKind::boolean).boolean;
    const std::string resumed_preview_path =
        RequireField(root, "postCapturePreviewPath", JsonKind::string).string;
    const auto resumed_preview_size =
        RequireField(root, "postCapturePreviewSizeBytes", JsonKind::integer).integer;
    const std::string resumed_preview_sha256 =
        RequireField(root, "postCapturePreviewSha256", JsonKind::string).string;
    if (resumed_preview_size < 0 ||
        static_cast<std::uint64_t>(resumed_preview_size) >
            std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("transaction journal resumed preview size is invalid");
    }
    if (resumed_preview_present) {
        result.resumed_preview = PreviewJpegRecord{
            PathFromUtf8(resumed_preview_path),
            static_cast<std::size_t>(resumed_preview_size),
            resumed_preview_sha256,
        };
        if (!result.resumed_preview->path.is_absolute() || result.resumed_preview->size == 0 ||
            !IsLowerHex(result.resumed_preview->sha256, 64)) {
            throw std::runtime_error("transaction journal resumed preview metadata is invalid");
        }
    } else if (!resumed_preview_path.empty() || resumed_preview_size != 0 ||
               !resumed_preview_sha256.empty()) {
        throw std::runtime_error("transaction journal hides resumed preview metadata");
    }
    result.succeeded = RequireField(root, "succeeded", JsonKind::boolean).boolean;
    result.terminal_state = RequireField(root, "terminalState", JsonKind::string).string;
    if (result.terminal_state != "Reserved" && result.terminal_state != "InProgress" &&
        result.terminal_state != "Complete" &&
        result.terminal_state != "Blocked" && result.terminal_state != "FailedPartial") {
        throw std::runtime_error("transaction journal terminal state is invalid");
    }
    result.error_category = RequireField(root, "errorCategory", JsonKind::string).string;
    result.error_detail = RequireField(root, "errorDetail", JsonKind::string).string;
    const bool original_present = RequireField(root, "originalPresent", JsonKind::boolean).boolean;
    const std::string original_alias =
        RequireField(root, "originalCameraAlias", JsonKind::string).string;
    const std::string original_path =
        RequireField(root, "originalPath", JsonKind::string).string;
    const std::string original_sha256 =
        RequireField(root, "originalSha256", JsonKind::string).string;
    const auto size = RequireField(root, "originalSizeBytes", JsonKind::integer).integer;
    if (size < 0 || static_cast<std::uint64_t>(size) > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("transaction journal original size is invalid");
    }
    if (original_present) {
        result.retained_original = RetainedOriginalRecord{
            original_alias,
            PathFromUtf8(original_path),
            static_cast<std::size_t>(size),
            original_sha256,
        };
        if (result.retained_original->camera_alias != result.camera_alias ||
            !result.retained_original->path.is_absolute() ||
            result.retained_original->size == 0 ||
            !IsLowerHex(result.retained_original->sha256, 64)) {
            throw std::runtime_error("transaction journal original metadata is invalid");
        }
    } else if (!original_alias.empty() || !original_path.empty() || size != 0 ||
               !original_sha256.empty()) {
        throw std::runtime_error("transaction journal hides original metadata while originalPresent is false");
    }
    result.spool_empty_before_capture =
        RequireField(root, "spoolEmptyBeforeCapture", JsonKind::boolean).boolean;
    result.camera_object_delete_attempted =
        RequireField(root, "cameraObjectDeleteAttempted", JsonKind::boolean).boolean;
    result.camera_object_delete_succeeded =
        RequireField(root, "cameraObjectDeleteSucceeded", JsonKind::boolean).boolean;
    result.spool_empty_after_cleanup =
        RequireField(root, "spoolEmptyAfterCleanup", JsonKind::boolean).boolean;
    const auto retry_count = RequireField(root, "automaticRetryCount", JsonKind::integer).integer;
    const auto watchdog = RequireField(root, "transactionWatchdogSeconds", JsonKind::integer).integer;
    if (retry_count != 0 || watchdog != 180) {
        throw std::runtime_error("transaction journal retry/watchdog metadata is invalid");
    }
    result.automatic_retry_count = 0;
    result.transaction_watchdog_seconds = static_cast<int>(watchdog);
    result.real_identifiers_included =
        RequireField(root, "realIdentifiersIncluded", JsonKind::boolean).boolean;
    if (result.real_identifiers_included) {
        throw std::runtime_error("transaction journal unexpectedly contains real identifiers");
    }
    if ((!result.live_view_handoff_requested &&
         (result.live_view_stopped_before_capture ||
          result.live_view_sdk_session_closed_before_capture ||
          result.live_view_resume_attempted || result.live_view_resumed ||
          result.resumed_preview)) ||
        (result.live_view_stopped_before_capture !=
         result.live_view_sdk_session_closed_before_capture) ||
        (result.live_view_resumed &&
         (!result.live_view_resume_attempted || !result.resumed_preview)) ||
        (result.resumed_preview && !result.live_view_resumed)) {
        throw std::runtime_error("transaction journal Live View handoff invariants are invalid");
    }
    const bool complete_success =
        result.terminal_state == "Complete" && result.retained_original.has_value() &&
        result.spool_empty_before_capture && result.camera_object_delete_attempted &&
        result.camera_object_delete_succeeded && result.spool_empty_after_cleanup &&
        result.error_category.empty() && result.error_detail.empty() &&
        !result.capture_profile_id.empty() &&
        result.capture_profile_version > 0 &&
        IsLowerHex(result.capture_profile_sha256, 64) &&
        result.capture_profile_camera_alias == result.camera_alias &&
        IsValidProfileTimestamp(result.profile_expires_at_utc) &&
        (!result.live_view_handoff_requested ||
         (result.live_view_stopped_before_capture &&
          result.live_view_sdk_session_closed_before_capture &&
          result.live_view_resume_attempted && result.live_view_resumed &&
          result.resumed_preview.has_value()));
    if (result.succeeded != complete_success ||
        (result.camera_object_delete_succeeded && !result.camera_object_delete_attempted)) {
        throw std::runtime_error("transaction journal success invariants are inconsistent");
    }
    if ((result.terminal_state == "Reserved" || result.terminal_state == "InProgress") &&
        (result.retained_original || result.spool_empty_before_capture ||
         result.camera_object_delete_attempted || result.camera_object_delete_succeeded ||
         result.spool_empty_after_cleanup || !result.error_category.empty() ||
         !result.error_detail.empty())) {
        throw std::runtime_error("active transaction journal contains terminal metadata");
    }
    if ((result.terminal_state == "Blocked" || result.terminal_state == "FailedPartial") &&
        result.error_category.empty()) {
        throw std::runtime_error("failed transaction journal has no error category");
    }
    return result;
}

fs::path StrictFixedLocalPath(
    const fs::path& path,
    std::string_view purpose);
void ValidateDistinctProductIdentityMaps(
    const IdentityMap& sdk_map,
    const IdentityMap& wpd_map);

fs::path TransactionJournalPath(const fs::path& root, std::string_view transaction_id) {
    if (!IsSafeTransactionId(transaction_id)) {
        throw HardwareCameraAgentProtocolError(
            "InvalidTransactionId", "transactionId must contain exactly 32 hexadecimal characters");
    }
    return StrictFixedLocalPath(root, "transaction-state root") /
        std::string(transaction_id) / "transaction.json";
}

bool IsReparsePoint(const fs::path& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool EqualPathComponent(const fs::path& left, const fs::path& right) noexcept {
    const std::wstring left_text = left.native();
    const std::wstring right_text = right.native();
    return CompareStringOrdinal(
        left_text.c_str(), static_cast<int>(left_text.size()),
        right_text.c_str(), static_cast<int>(right_text.size()), TRUE) == CSTR_EQUAL;
}

bool PathIsWithin(const fs::path& root, const fs::path& candidate) {
    std::error_code root_error;
    std::error_code candidate_error;
    const fs::path canonical_root = fs::weakly_canonical(root, root_error);
    const fs::path canonical_candidate = fs::weakly_canonical(candidate, candidate_error);
    if (root_error || candidate_error) return false;
    auto root_component = canonical_root.begin();
    auto candidate_component = canonical_candidate.begin();
    for (; root_component != canonical_root.end(); ++root_component, ++candidate_component) {
        if (candidate_component == canonical_candidate.end() ||
            !EqualPathComponent(*root_component, *candidate_component)) {
            return false;
        }
    }
    return true;
}

bool PathChainHasReparsePoint(const fs::path& root, const fs::path& candidate) {
    fs::path current = fs::absolute(root).lexically_normal();
    if (IsReparsePoint(current)) return true;
    const fs::path relative = fs::absolute(candidate).lexically_normal().lexically_relative(current);
    if (relative.empty()) return true;
    for (const auto& component : relative) {
        if (component == "..") return true;
        current /= component;
        if (IsReparsePoint(current)) return true;
    }
    return false;
}

bool IsMissingFilesystemError(const std::error_code& error) noexcept {
    return error == std::errc::no_such_file_or_directory ||
        error.value() == ERROR_FILE_NOT_FOUND ||
        error.value() == ERROR_PATH_NOT_FOUND;
}

fs::path StrictFixedLocalPath(
    const fs::path& path,
    std::string_view purpose) {
    const std::wstring input = path.native();
    const auto is_drive_letter = [](wchar_t value) noexcept {
        return (value >= L'A' && value <= L'Z') ||
            (value >= L'a' && value <= L'z');
    };
    const bool drive_qualified =
        input.size() >= 3 && is_drive_letter(input[0]) && input[1] == L':' &&
        (input[2] == L'\\' || input[2] == L'/');
    if (!path.is_absolute() || !path.has_root_name() ||
        !path.has_root_directory() || !drive_qualified ||
        input.find(L'\0') != std::wstring::npos ||
        input.find(L':', 2) != std::wstring::npos ||
        input.starts_with(L"\\\\") || input.starts_with(L"\\??\\")) {
        throw std::invalid_argument(
            std::string(purpose) +
            " must be an absolute drive-qualified local path without UNC, device, or ADS syntax");
    }

    const DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        throw std::invalid_argument(
            std::string(purpose) + " could not be normalized as a local path");
    }
    std::wstring buffer(static_cast<std::size_t>(required), L'\0');
    const DWORD written = GetFullPathNameW(
        input.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) {
        throw std::invalid_argument(
            std::string(purpose) + " could not be normalized as a local path");
    }
    buffer.resize(written);
    const fs::path normalized = fs::path(buffer).lexically_normal();
    const std::wstring normalized_native = normalized.native();
    if (normalized_native.size() < 3 ||
        !is_drive_letter(normalized_native[0]) || normalized_native[1] != L':' ||
        normalized_native.find(L':', 2) != std::wstring::npos) {
        throw std::invalid_argument(
            std::string(purpose) + " normalized outside the drive-qualified path policy");
    }

    std::wstring drive_root{
        normalized_native[0], L':', L'\\'};
    if (GetDriveTypeW(drive_root.c_str()) != DRIVE_FIXED) {
        throw std::invalid_argument(
            std::string(purpose) +
            " must reside on a fixed local drive; remote, mapped, removable, optical, and RAM drives are rejected");
    }
    if (PathChainHasReparsePoint(normalized.root_path(), normalized)) {
        throw std::invalid_argument(
            std::string(purpose) + " path chain must be reparse-free");
    }
    return normalized;
}

IdentityMap LoadStrictProductIdentityMap(const fs::path& path) {
    try {
        const fs::path absolute =
            StrictFixedLocalPath(path, "identity map");
        std::error_code type_error;
        if (!fs::is_regular_file(absolute, type_error) || type_error ||
            IsReparsePoint(absolute)) {
            throw std::runtime_error("identity map is not a regular reparse-free local file");
        }
        const auto size = fs::file_size(absolute, type_error);
        if (type_error || size == 0 || size > 64U * 1024U) {
            throw std::runtime_error("identity map size is invalid");
        }
        std::ifstream input(absolute, std::ios::binary);
        if (!input) throw std::runtime_error("identity map cannot be opened");
        const std::string body{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad() || body.size() != size) {
            throw std::runtime_error("identity map cannot be read completely");
        }
        const JsonValue root = JsonParser(body).Parse();
        RequireExactFields(root, {"CAM-A", "CAM-B"});
        const auto parse_binding = [&](std::string_view name) -> std::optional<std::string> {
            const JsonValue& value = RequireAnyField(root, name);
            if (value.kind == JsonKind::null_value) return std::nullopt;
            if (value.kind != JsonKind::string || value.string.empty() ||
                value.string.size() > 1024 ||
                !std::all_of(value.string.begin(), value.string.end(), [](unsigned char c) {
                    return c >= 0x20U && c != 0x7FU;
                })) {
                throw std::runtime_error("identity map binding is invalid");
            }
            return value.string;
        };
        auto cam_a = parse_binding("CAM-A");
        auto cam_b = parse_binding("CAM-B");
        if (cam_a && cam_b && *cam_a == *cam_b) {
            throw std::runtime_error("identity map aliases must not share one stable identity");
        }
        return IdentityMap(absolute, std::move(cam_a), std::move(cam_b));
    } catch (const TransportError&) {
        throw;
    } catch (const std::exception&) {
        throw TransportError(
            "identity_map_invalid",
            "product Camera Agent identity map is malformed or outside its trusted local scope");
    }
}

std::string Base64Encode(const std::vector<unsigned char>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3U) {
        const std::uint32_t first = bytes[offset];
        const std::uint32_t second = offset + 1U < bytes.size() ? bytes[offset + 1U] : 0U;
        const std::uint32_t third = offset + 2U < bytes.size() ? bytes[offset + 2U] : 0U;
        const std::uint32_t combined = (first << 16U) | (second << 8U) | third;
        output.push_back(alphabet[(combined >> 18U) & 0x3FU]);
        output.push_back(alphabet[(combined >> 12U) & 0x3FU]);
        output.push_back(offset + 1U < bytes.size()
            ? alphabet[(combined >> 6U) & 0x3FU] : '=');
        output.push_back(offset + 2U < bytes.size()
            ? alphabet[combined & 0x3FU] : '=');
    }
    return output;
}

SingleCameraIdentityV3 LoadStrictSingleIdentityV3(const fs::path& path) {
    try {
        const fs::path absolute =
            StrictFixedLocalPath(path, "SingleCamera identity-v3");
        std::error_code type_error;
        if (!fs::is_regular_file(absolute, type_error) || type_error ||
            IsReparsePoint(absolute)) {
            throw std::runtime_error("identity-v3 is not a regular local file");
        }
        const auto size = fs::file_size(absolute, type_error);
        if (type_error || size == 0 || size > 16U * 1024U) {
            throw std::runtime_error("identity-v3 size is invalid");
        }
        std::ifstream input(absolute, std::ios::binary);
        const std::string body{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (!input || input.bad() || body.size() != size) {
            throw std::runtime_error("identity-v3 could not be read completely");
        }
        return ParseSingleCameraIdentityV3(body);
    } catch (const TransportError&) {
        throw;
    } catch (const std::exception&) {
        throw TransportError(
            "single_identity_v3_invalid",
            "SingleCamera identity-v3 is missing, malformed, or outside trusted local storage");
    }
}

std::pair<IdentityMap, IdentityMap> LoadProductSingleIdentityMaps(
    const ProductionHardwareCameraAgentConfig& config,
    const std::vector<CameraInfo>& sdk_cameras) {
    if (config.single_identity_v3.empty()) {
        IdentityMap sdk_map = LoadStrictProductIdentityMap(config.sdk_identity_map);
        IdentityMap wpd_map = LoadStrictProductIdentityMap(config.wpd_identity_map);
        ValidateDistinctProductIdentityMaps(sdk_map, wpd_map);
        return {std::move(sdk_map), std::move(wpd_map)};
    }

    const SingleCameraIdentityV3 identity =
        LoadStrictSingleIdentityV3(config.single_identity_v3);
    const std::optional<std::string> session_sdk_identity =
        sdk_cameras.size() == 1
            ? std::optional<std::string>(sdk_cameras.front().stable_identity)
            : std::nullopt;
    return {
        IdentityMap(
            config.sdk_identity_map,
            identity.camera_alias == "CAM-A" ? session_sdk_identity : std::nullopt,
            identity.camera_alias == "CAM-B" ? session_sdk_identity : std::nullopt),
        IdentityMap(
            config.single_identity_v3,
            identity.camera_alias == "CAM-A"
                ? std::optional<std::string>(identity.wpd_stable_identity_sha256)
                : std::nullopt,
            identity.camera_alias == "CAM-B"
                ? std::optional<std::string>(identity.wpd_stable_identity_sha256)
                : std::nullopt),
    };
}

void ValidateProductSingleIdentityConfiguration(
    const ProductionHardwareCameraAgentConfig& config) {
    if (!config.single_identity_v3.empty()) {
        (void)LoadStrictSingleIdentityV3(config.single_identity_v3);
        return;
    }
    const IdentityMap sdk_map =
        LoadStrictProductIdentityMap(config.sdk_identity_map);
    const IdentityMap wpd_map =
        LoadStrictProductIdentityMap(config.wpd_identity_map);
    ValidateDistinctProductIdentityMaps(sdk_map, wpd_map);
}

ApprovedCaptureProfile LoadStrictApprovedCaptureProfile(const fs::path& path) {
    try {
        const fs::path absolute =
            StrictFixedLocalPath(path, "approved capture profile");
        std::error_code type_error;
        if (!fs::is_regular_file(absolute, type_error) || type_error ||
            IsReparsePoint(absolute)) {
            throw std::runtime_error(
                "approved capture profile is not a regular reparse-free local file");
        }
        const auto size = fs::file_size(absolute, type_error);
        if (type_error || size == 0 || size > kMaximumProtocolJsonBytes) {
            throw std::runtime_error("approved capture profile size is invalid");
        }
        std::ifstream input(absolute, std::ios::binary);
        if (!input) throw std::runtime_error("approved capture profile cannot be opened");
        const std::string body{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad() || body.size() != size) {
            throw std::runtime_error(
                "approved capture profile cannot be read completely");
        }
        return ParseApprovedCaptureProfile(body);
    } catch (const std::exception&) {
        throw std::invalid_argument(
            "approved capture profile is malformed or outside its trusted local scope");
    }
}

std::optional<ApprovedCaptureProfile> LoadConfiguredApprovedCaptureProfile(
    const fs::path& path) {
    if (path.empty()) return std::nullopt;
    const fs::path strict_path =
        StrictFixedLocalPath(path, "approved capture profile");
    std::error_code exists_error;
    const bool exists = fs::exists(strict_path, exists_error);
    if (exists_error) {
        throw std::invalid_argument(
            "approved capture profile location could not be inspected");
    }
    if (!exists) return std::nullopt;
    return LoadStrictApprovedCaptureProfile(strict_path);
}

void ValidateDistinctProductIdentityMaps(
    const IdentityMap& sdk_map,
    const IdentityMap& wpd_map) {
    std::error_code equivalent_error;
    const bool equivalent =
        fs::equivalent(sdk_map.Path(), wpd_map.Path(), equivalent_error);
    if (equivalent_error || equivalent ||
        EqualPathComponent(sdk_map.Path(), wpd_map.Path())) {
        throw TransportError(
            "identity_map_invalid",
            "SDK and WPD product identity maps must be distinct local files");
    }
}

bool IsCanonicalOriginalLocation(
    const fs::path& run_root,
    const fs::path& candidate,
    std::string_view camera_alias) {
    const fs::path relative =
        fs::absolute(candidate).lexically_normal().lexically_relative(
            fs::absolute(run_root).lexically_normal());
    std::vector<fs::path> components(relative.begin(), relative.end());
    if (components.size() != 3 || components[0].empty() ||
        components[0] == "." || components[0] == "..") {
        return false;
    }
    return components[1].native() == std::wstring(camera_alias.begin(), camera_alias.end()) &&
        components[2] == "original.jpg";
}

void ValidateArtifactRunNoReparse(const fs::path& run_root) {
    const fs::path absolute_run = fs::absolute(run_root).lexically_normal();
    const fs::path artifacts_root = StrictFixedLocalPath(
        absolute_run.parent_path(), "artifact root");
    std::error_code directory_error;
    if (!fs::is_directory(absolute_run, directory_error) || directory_error ||
        !PathIsWithin(artifacts_root, absolute_run) ||
        PathChainHasReparsePoint(artifacts_root.root_path(), artifacts_root) ||
        PathChainHasReparsePoint(artifacts_root, absolute_run)) {
        throw TransportError(
            "artifact_scope_invalid",
            "hardware Camera Agent artifact run is not a reparse-free child directory");
    }
    for (fs::recursive_directory_iterator iterator(absolute_run), end;
         iterator != end; ++iterator) {
        if (IsReparsePoint(iterator->path()) || !PathIsWithin(absolute_run, iterator->path())) {
            throw TransportError(
                "artifact_scope_invalid",
                "hardware Camera Agent artifact tree contains a reparse point or escaped path");
        }
    }
}

void ValidateTransactionJournalScope(
    const fs::path& transaction_root,
    std::string_view transaction_id) {
    const fs::path absolute_root = StrictFixedLocalPath(
        transaction_root, "transaction-state root");
    const fs::path directory = absolute_root / std::string(transaction_id);
    std::error_code directory_error;
    if (!IsSafeTransactionId(transaction_id) ||
        !fs::is_directory(directory, directory_error) || directory_error ||
        !PathIsWithin(absolute_root, directory) ||
        PathChainHasReparsePoint(absolute_root.root_path(), absolute_root) ||
        PathChainHasReparsePoint(absolute_root, directory)) {
        throw TransportError(
            "transaction_state_scope_invalid",
            "hardware Camera Agent transaction directory is not a reparse-free child");
    }
    const fs::path journal = directory / "transaction.json";
    if (fs::exists(journal) && IsReparsePoint(journal)) {
        throw TransportError(
            "transaction_state_scope_invalid",
            "hardware Camera Agent transaction journal is a reparse point");
    }
}

fs::path PrepareExclusiveArtifactRun(
    const fs::path& artifacts_root,
    std::string_view run_id) {
    if (!IsSafeRunId(run_id)) {
        throw TransportError("artifact_scope_invalid", "hardware Camera Agent run ID is invalid");
    }
    const fs::path absolute_root =
        StrictFixedLocalPath(artifacts_root, "artifact root");
    fs::create_directories(absolute_root);
    if (PathChainHasReparsePoint(absolute_root.root_path(), absolute_root)) {
        throw TransportError(
            "artifact_scope_invalid",
            "hardware Camera Agent artifact root contains a reparse point");
    }
    const fs::path run_root = absolute_root / std::string(run_id);
    std::error_code create_error;
    const bool created = fs::create_directory(run_root, create_error);
    if (!created || create_error) {
        throw TransportError(
            "artifact_run_exists",
            "hardware Camera Agent requires an exclusively created artifact run directory");
    }
    ValidateArtifactRunNoReparse(run_root);
    return run_root;
}

class LockedVerifiedOriginal final {
public:
    explicit LockedVerifiedOriginal(HANDLE handle) noexcept : handle_(handle) {}
    ~LockedVerifiedOriginal() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    LockedVerifiedOriginal(const LockedVerifiedOriginal&) = delete;
    LockedVerifiedOriginal& operator=(const LockedVerifiedOriginal&) = delete;
    LockedVerifiedOriginal(LockedVerifiedOriginal&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    LockedVerifiedOriginal& operator=(LockedVerifiedOriginal&& other) noexcept {
        if (this != &other) {
            if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

LockedVerifiedOriginal ValidateAndLockOriginalBeforeCameraDelete(
    const fs::path& run_root,
    std::string_view camera_alias,
    const FrameEvidence& frame) {
    ValidateArtifactRunNoReparse(run_root);
    if (!frame.success || frame.camera_alias != camera_alias ||
        !PathIsWithin(run_root, frame.path) ||
        PathChainHasReparsePoint(run_root, frame.path) ||
        !IsCanonicalOriginalLocation(run_root, frame.path, camera_alias)) {
        throw TransportError(
            "pc_original_scope_invalid",
            "canonical PC original escaped the approved reparse-free artifact run");
    }
    const HANDLE handle = CreateFileW(
        frame.path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw TransportError(
            "pc_original_verification_failed",
            "canonical PC original could not be locked against write/delete before camera cleanup");
    }
    LockedVerifiedOriginal locked(handle);
    try {
        BY_HANDLE_FILE_INFORMATION information{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(handle, &information) ||
            !GetFileSizeEx(handle, &size) || size.QuadPart <= 0 ||
            static_cast<std::uint64_t>(size.QuadPart) >
                kMaximumVerifiedJpegBytes ||
            (information.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            throw TransportError(
                "pc_original_verification_failed",
                "locked canonical PC original has invalid type or size");
        }
        std::vector<unsigned char> bytes(
            static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                bytes.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD read = 0;
            if (!ReadFile(handle, bytes.data() + offset, chunk, &read, nullptr) ||
                read == 0) {
                throw TransportError(
                    "pc_original_verification_failed",
                    "locked canonical PC original could not be read completely");
            }
            offset += read;
        }
        if (bytes.size() != frame.bytes ||
            !HasExpectedSingleOriginalDimensions(bytes) ||
            Sha256Hex(bytes) != frame.sha256) {
            throw TransportError(
                "pc_original_verification_failed",
                "canonical PC original changed before exact camera-object delete");
        }
        ValidateArtifactRunNoReparse(run_root);
        return locked;
    } catch (const TransportError&) {
        throw;
    } catch (const std::exception&) {
        throw TransportError(
            "pc_original_verification_failed",
            "canonical PC original could not be reread before exact camera-object delete");
    }
}

bool VerifyJournalOriginal(
    const fs::path& artifacts_root,
    const SingleCameraCaptureResult& result) {
    if (!result.retained_original || !IsSafeRunId(result.run_id)) return false;
    const fs::path run_root = fs::absolute(artifacts_root) / result.run_id;
    const fs::path path = result.retained_original->path;
    if (!PathIsWithin(run_root, path) || PathChainHasReparsePoint(run_root, path) ||
        !IsCanonicalOriginalLocation(run_root, path, result.camera_alias)) {
        return false;
    }
    try {
        const auto bytes = ReadBoundedFile(path);
        return bytes.size() == result.retained_original->size &&
            HasExpectedSingleOriginalDimensions(bytes) &&
            Sha256Hex(bytes) == result.retained_original->sha256;
    } catch (...) {
        return false;
    }
}

bool VerifyJournalResumedPreview(
    const fs::path& artifacts_root,
    const SingleCameraCaptureResult& result) {
    if (!result.resumed_preview || !IsSafeRunId(result.run_id)) return false;
    const fs::path run_root = fs::absolute(artifacts_root) / result.run_id;
    const fs::path path = result.resumed_preview->path;
    const fs::path relative =
        fs::absolute(path).lexically_normal().lexically_relative(
            fs::absolute(run_root).lexically_normal());
    const std::vector<fs::path> components(relative.begin(), relative.end());
    if (components.size() != 3 || components[0] != "live-view" ||
        components[1].native() !=
            std::wstring(result.camera_alias.begin(), result.camera_alias.end()) ||
        components[2] != "preview.jpg" || !PathIsWithin(run_root, path) ||
        PathChainHasReparsePoint(run_root, path)) {
        return false;
    }
    try {
        const auto bytes = ReadBoundedFile(path);
        return bytes.size() == result.resumed_preview->size && IsValidJpeg(bytes) &&
            Sha256Hex(bytes) == result.resumed_preview->sha256;
    } catch (...) {
        return false;
    }
}

void ReserveTransaction(
    const fs::path& root,
    const SingleCameraCaptureResult& initial,
    const std::function<void()>& after_directory_created) {
    const fs::path absolute_root =
        StrictFixedLocalPath(root, "transaction-state root");
    fs::create_directories(absolute_root);
    if (PathChainHasReparsePoint(absolute_root.root_path(), absolute_root)) {
        throw TransportError(
            "transaction_state_scope_invalid",
            "hardware Camera Agent transaction root contains a reparse point");
    }
    const fs::path directory = TransactionJournalPath(root, initial.transaction_id).parent_path();
    std::error_code create_error;
    const bool created = fs::create_directory(directory, create_error);
    if (!created) {
        if (create_error) {
            throw TransportError(
                "transaction_state_scope_invalid",
                "hardware Camera Agent could not create the transaction directory: " +
                    create_error.message());
        }
        throw HardwareCameraAgentProtocolError(
            "DuplicateTransactionId",
            "transactionId is already reserved and capture will not be repeated");
    }
    if (after_directory_created) after_directory_created();
    ValidateTransactionJournalScope(root, initial.transaction_id);
    AtomicReplaceText(
        directory / "transaction.json",
        SerializeTransactionJournal(initial),
        [&] { ValidateTransactionJournalScope(root, initial.transaction_id); });
}

std::optional<RetainedOriginalRecord> RecoverRetainedOriginal(
    const fs::path& artifacts_root,
    const SingleCameraCaptureResult& journal) {
    if (journal.run_id.empty()) return std::nullopt;
    const fs::path run_root = fs::absolute(artifacts_root) / journal.run_id;
    if (!IsSafeRunId(journal.run_id) || !PathIsWithin(fs::absolute(artifacts_root), run_root) ||
        PathChainHasReparsePoint(fs::absolute(artifacts_root), run_root)) {
        return std::nullopt;
    }
    std::error_code status_error;
    if (!fs::is_directory(run_root, status_error) || status_error) return std::nullopt;
    std::vector<fs::path> candidates;
    for (fs::recursive_directory_iterator iterator(
             run_root, fs::directory_options::skip_permission_denied), end;
         iterator != end; ++iterator) {
        std::error_code directory_error;
        if (iterator->is_directory(directory_error) && !directory_error &&
            IsReparsePoint(iterator->path())) {
            iterator.disable_recursion_pending();
            continue;
        }
        std::error_code type_error;
        if (!iterator->is_regular_file(type_error) || type_error || IsReparsePoint(iterator->path())) continue;
        const fs::path path = iterator->path();
        if (path.filename() == "original.jpg" && path.parent_path().filename() == journal.camera_alias &&
            PathIsWithin(run_root, path) && !PathChainHasReparsePoint(run_root, path) &&
            IsCanonicalOriginalLocation(run_root, path, journal.camera_alias)) {
            candidates.push_back(path);
        }
    }
    if (candidates.size() != 1) return std::nullopt;
    const auto bytes = ReadBoundedFile(candidates.front());
    if (!HasExpectedSingleOriginalDimensions(bytes)) return std::nullopt;
    return RetainedOriginalRecord{
        journal.camera_alias,
        fs::absolute(candidates.front()),
        bytes.size(),
        Sha256Hex(bytes),
    };
}

std::string TryExtractRequestId(std::string_view json) noexcept {
    try {
        const auto root = JsonParser(json).Parse();
        if (root.kind != JsonKind::object) return "rejected";
        const auto found = root.object.find("requestId");
        if (found == root.object.end() || found->second.kind != JsonKind::string ||
            !IsSafeRequestId(found->second.string)) {
            return "rejected";
        }
        return found->second.string;
    } catch (...) {
        return "rejected";
    }
}

fs::path WpdIdentityMapPath(fs::path sdk_identity_map) {
    sdk_identity_map.replace_filename(
        sdk_identity_map.stem().string() + "-wpd" + sdk_identity_map.extension().string());
    return sdk_identity_map;
}

void CopyBindingResult(
    const SingleCameraBindingResolution& binding,
    SingleCameraReadinessResult& result) {
    result.ready = binding.ready;
    result.camera_alias = binding.camera_alias;
    result.sdk_camera_count = binding.sdk_camera_count;
    result.wpd_camera_count = binding.wpd_camera_count;
    result.sdk_identity_bound = binding.sdk_identity_bound;
    result.wpd_identity_bound = binding.wpd_identity_bound;
    result.sdk_alias_matches = binding.sdk_alias_matches;
    result.wpd_alias_matches = binding.wpd_alias_matches;
    result.failure_category = binding.failure_category;
    result.failure_detail = binding.failure_detail;
}

HardwareCameraAgentResponse ProtocolRejection(
    std::string request_id,
    std::string code,
    std::string detail) {
    HardwareCameraAgentResponse response;
    response.request_id = IsSafeRequestId(request_id) ? std::move(request_id) : "rejected";
    response.success = false;
    response.result_code = "ProtocolRejected";
    response.rejection_code = std::move(code);
    response.error_detail = SafeErrorDetail(detail);
    return response;
}

HardwareCameraAgentResponse AgentFailure(
    std::string request_id,
    std::string code,
    std::string detail) {
    HardwareCameraAgentResponse response;
    response.request_id = IsSafeRequestId(request_id) ? std::move(request_id) : "rejected";
    response.success = false;
    response.result_code = std::move(code);
    response.error_detail = SafeErrorDetail(detail);
    return response;
}

HybridCaptureRunSummary ToHybridSummary(
    const HardwareCameraAgentRequest& request,
    const SingleCameraCaptureResult& result) {
    HybridCaptureRunSummary summary;
    summary.requested = 1;
    summary.attempted = result.terminal_state == "Blocked" ? 0 : 1;
    summary.completed = result.succeeded ? 1 : 0;
    summary.failures = result.succeeded ? 0 : 1;
    summary.terminal_state = result.terminal_state;
    summary.last_state = result.terminal_state;
    summary.pc_original_canonical = result.retained_original.has_value();
    summary.spool_empty_before_count = result.spool_empty_before_capture ? 1 : 0;
    summary.camera_card_delete_attempted_count = result.camera_object_delete_attempted ? 1 : 0;
    summary.camera_card_delete_succeeded_count = result.camera_object_delete_succeeded ? 1 : 0;
    summary.spool_empty_after_count = result.spool_empty_after_cleanup ? 1 : 0;
    summary.exclusive_camera_control_confirmed = request.exclusive_camera_control_confirmed;
    summary.dedicated_spool_scope_confirmed = request.dedicated_spool_scope_confirmed;
    summary.exact_object_delete_confirmed = request.exact_object_delete_confirmed;
    return summary;
}

bool IsCaptureResultStructurallyValid(
    const SingleCameraCaptureResult& result,
    std::string_view expected_transaction_id,
    bool allow_not_found,
    const HardwareCameraAgentRequest* expected_request = nullptr) noexcept {
    if (result.transaction_id != expected_transaction_id ||
        !IsSafeTransactionId(result.transaction_id) ||
        result.automatic_retry_count != 0 ||
        result.transaction_watchdog_seconds != 180 ||
        result.real_identifiers_included ||
        (result.camera_object_delete_succeeded && !result.camera_object_delete_attempted)) {
        return false;
    }
    if (expected_request) {
        if (result.camera_alias != expected_request->camera_alias ||
            result.live_view_handoff_requested !=
                expected_request->live_view_handoff_requested) {
            return false;
        }
        const bool profile_matches_request =
            result.capture_profile_id == expected_request->expected_capture_profile_id &&
            result.capture_profile_version ==
                expected_request->expected_capture_profile_version &&
            result.capture_profile_sha256 ==
                expected_request->expected_capture_profile_sha256 &&
            result.profile_expires_at_utc ==
                expected_request->expected_capture_profile_expires_at_utc;
        if ((result.succeeded && !profile_matches_request) ||
            (result.error_category == "capture_profile_snapshot_mismatch" &&
             profile_matches_request)) {
            return false;
        }
    }
    const bool profile_present = !result.capture_profile_id.empty();
    if (profile_present != (result.capture_profile_version > 0) ||
        profile_present != !result.capture_profile_sha256.empty() ||
        profile_present != !result.capture_profile_camera_alias.empty() ||
        profile_present != !result.profile_expires_at_utc.empty() ||
        (profile_present &&
         (!IsSafeRequestId(result.capture_profile_id) ||
          !IsLowerHex(result.capture_profile_sha256, 64) ||
          !IsValidProfileTimestamp(result.profile_expires_at_utc) ||
          (result.capture_profile_camera_alias != "CAM-A" &&
           result.capture_profile_camera_alias != "CAM-B")))) {
        return false;
    }
    if ((!result.live_view_handoff_requested &&
         (result.live_view_stopped_before_capture ||
          result.live_view_sdk_session_closed_before_capture ||
          result.live_view_resume_attempted || result.live_view_resumed ||
          result.resumed_preview)) ||
        (result.live_view_stopped_before_capture !=
         result.live_view_sdk_session_closed_before_capture) ||
        (result.live_view_resumed &&
         (!result.live_view_resume_attempted || !result.resumed_preview)) ||
        (result.resumed_preview &&
         (!result.live_view_resumed || !result.resumed_preview->path.is_absolute() ||
          result.resumed_preview->size == 0 ||
          !IsLowerHex(result.resumed_preview->sha256, 64)))) {
        return false;
    }
    if (allow_not_found && result.error_category == "transaction_not_found") {
        return !result.succeeded && result.terminal_state == "Blocked" &&
            result.camera_alias.empty() && result.run_id.empty() &&
            !result.retained_original && !result.spool_empty_before_capture &&
            !result.camera_object_delete_attempted &&
            !result.camera_object_delete_succeeded &&
            !result.spool_empty_after_cleanup;
    }
    if (allow_not_found && result.error_category == "transaction_reservation_incomplete") {
        return !result.succeeded && result.terminal_state == "FailedPartial" &&
            result.camera_alias.empty() && result.run_id.empty() &&
            !result.retained_original && !result.spool_empty_before_capture &&
            !result.camera_object_delete_attempted &&
            !result.camera_object_delete_succeeded &&
            !result.spool_empty_after_cleanup;
    }
    if (allow_not_found && result.error_category == "transaction_reserved" &&
        result.camera_alias.empty() && result.run_id.empty()) {
        return !result.succeeded && result.terminal_state == "Reserved" &&
            !result.retained_original && !result.spool_empty_before_capture &&
            !result.camera_object_delete_attempted &&
            !result.camera_object_delete_succeeded &&
            !result.spool_empty_after_cleanup;
    }
    if ((result.camera_alias != "CAM-A" && result.camera_alias != "CAM-B") ||
        !IsSafeRunId(result.run_id)) {
        return false;
    }
    if (result.retained_original &&
        (result.retained_original->camera_alias != result.camera_alias ||
         !result.retained_original->path.is_absolute() ||
         result.retained_original->size == 0 ||
         !IsLowerHex(result.retained_original->sha256, 64))) {
        return false;
    }
    const bool complete_success =
        result.terminal_state == "Complete" && result.retained_original.has_value() &&
        result.spool_empty_before_capture && result.camera_object_delete_attempted &&
        result.camera_object_delete_succeeded && result.spool_empty_after_cleanup &&
        result.error_category.empty() && result.error_detail.empty() &&
        !result.capture_profile_id.empty() &&
        result.capture_profile_version > 0 &&
        IsLowerHex(result.capture_profile_sha256, 64) &&
        result.capture_profile_camera_alias == result.camera_alias &&
        IsValidProfileTimestamp(result.profile_expires_at_utc) &&
        (!result.live_view_handoff_requested ||
         (result.live_view_stopped_before_capture &&
          result.live_view_sdk_session_closed_before_capture &&
          result.live_view_resume_attempted && result.live_view_resumed &&
          result.resumed_preview.has_value()));
    if (result.succeeded != complete_success) return false;
    if (result.terminal_state == "Complete") return complete_success;
    if (result.terminal_state != "Reserved" && result.terminal_state != "InProgress" &&
        result.terminal_state != "Blocked" &&
        result.terminal_state != "FailedPartial") {
        return false;
    }
    if (result.terminal_state == "Reserved" || result.terminal_state == "InProgress") {
        return !result.retained_original && !result.spool_empty_before_capture &&
            !result.camera_object_delete_attempted &&
            !result.camera_object_delete_succeeded && !result.spool_empty_after_cleanup &&
            (result.error_category.empty() || result.error_category == "transaction_in_progress" ||
             result.error_category == "transaction_reserved");
    }
    return !result.error_category.empty();
}

bool IsReadinessResultStructurallyValid(
    const SingleCameraReadinessResult& result,
    std::string_view expected_alias) noexcept {
    const auto setting_valid = [](const ObservedCameraSetting& setting) {
        const auto bounded = [](std::string_view value, std::size_t maximum) {
            return !value.empty() && value.size() <= maximum &&
                std::all_of(value.begin(), value.end(), [](unsigned char character) {
                    return character >= 0x20U && character != 0x7FU;
                });
        };
        const bool has_current = setting.current_value.has_value() ||
            setting.current_index.has_value() || setting.current_label.has_value();
        return bounded(setting.cap_type, 128) &&
            bounded(setting.probe_state, 128) &&
            bounded(setting.value_type, 128) &&
            (!setting.current_label || bounded(*setting.current_label, 256)) &&
            (setting.available ? has_current : !has_current);
    };
    const auto& settings = result.observed_settings;
    if (result.camera_alias != expected_alias || result.real_identifiers_included ||
        !result.read_only || result.capture_command_sent ||
        result.camera_object_delete_attempted || result.camera_settings_changed ||
        !setting_valid(settings.file_type) ||
        !setting_valid(settings.compression_level) ||
        !setting_valid(settings.image_size) ||
        !setting_valid(settings.exposure_mode) ||
        !setting_valid(settings.shutter_speed) ||
        !setting_valid(settings.aperture) ||
        !setting_valid(settings.sensitivity) ||
        !setting_valid(settings.white_balance_mode) ||
        !setting_valid(settings.focus_mode)) {
        return false;
    }
    if (result.capture_profile_approved) {
        if (!IsSafeRequestId(result.capture_profile_id) ||
            result.capture_profile_version == 0 ||
            !IsLowerHex(result.capture_profile_sha256, 64) ||
            !IsValidProfileTimestamp(result.profile_expires_at_utc) ||
            (result.capture_profile_camera_alias != "CAM-A" &&
             result.capture_profile_camera_alias != "CAM-B") ||
            result.capture_profile_alias_matches !=
                (result.capture_profile_camera_alias == expected_alias)) {
            return false;
        }
    } else if (!result.capture_profile_id.empty() || result.capture_profile_version != 0 ||
               !result.capture_profile_sha256.empty() ||
               !result.capture_profile_camera_alias.empty() ||
               !result.profile_expires_at_utc.empty() ||
               result.capture_profile_alias_matches) {
        return false;
    }
    if (!result.ready) return !result.failure_category.empty() || !result.spool_known_empty;
    return result.sdk_camera_count == 1 && result.wpd_camera_count == 1 &&
        result.sdk_identity_bound && result.wpd_identity_bound &&
        result.sdk_alias_matches && result.wpd_alias_matches &&
        result.sdk_status_probed && result.spool_inspected && result.spool_known_empty &&
        result.live_view_status_available && result.live_view_status == "off" &&
        result.capture_profile_approved && result.settings_match_approved_profile &&
        result.capture_profile_alias_matches &&
        IsFutureProfileTimestamp(result.profile_expires_at_utc) &&
        result.failure_category.empty() && result.failure_detail.empty();
}

bool IsLiveViewResultStructurallyValid(
    const SingleCameraLiveViewProbeResult& result,
    const HardwareCameraAgentRequest& request) noexcept {
    if (result.camera_alias != request.camera_alias || !IsSafeRunId(result.run_id) ||
        result.preview_is_original || result.preview_is_stitch_input ||
        result.real_identifiers_included) {
        return false;
    }
    if (!result.succeeded) {
        return !result.preview_persisted && !result.preview && !result.error_category.empty();
    }
    return result.frames == request.live_view_frames && result.frames >= 1 &&
        result.last_frame_bytes > 0 && IsLowerHex(result.last_frame_sha256, 64) &&
        result.duration_ms >= 0 && result.preview_persisted && result.preview &&
        result.preview->path.is_absolute() &&
        result.preview->size == result.last_frame_bytes &&
        result.preview->sha256 == result.last_frame_sha256 &&
        result.live_view_stopped && result.sdk_session_closed &&
        result.error_category.empty() && result.error_detail.empty();
}

} // namespace

std::string ComputeDualIdentityBindingProofPayloadSha256(
    const DualIdentityBindingProof& proof) {
    std::ostringstream payload;
    const auto append = [&](std::string_view name, std::string_view value) {
        payload << name << ':' << value.size() << ':' << value << '\n';
    };
    append("schemaVersion", "a0.camera-agent.dual-identity-binding-proof.v1");
    append("bindingVersion", "1");
    append("cameraMode", "DualCamera");
    append("selectedAlias", proof.camera_alias);
    append("providerId", proof.provider_id);
    append("providerVersion", std::to_string(proof.provider_version));
    append("sdkIdentitySha256", proof.sdk_identity_sha256);
    append("wpdIdentitySha256", proof.wpd_identity_sha256);
    append("createdAtUtc", proof.created_at_utc);
    append("expiresAtUtc", proof.expires_at_utc);
    append("singleCameraConnectedConfirmed",
        proof.single_camera_connected_confirmed ? "true" : "false");
    append("documentedCorrelationConfirmed",
        proof.documented_correlation_confirmed ? "true" : "false");
    const std::string bytes = payload.str();
    return Sha256Hex(std::vector<unsigned char>(bytes.begin(), bytes.end()));
}

std::string SerializeDualIdentityBindingProof(
    const DualIdentityBindingProof& proof) {
    std::ostringstream output;
    output << "{\n"
           << "  \"schemaVersion\": \"a0.camera-agent.dual-identity-binding-proof.v1\",\n"
           << "  \"bindingVersion\": 1,\n"
           << "  \"cameraMode\": \"DualCamera\",\n"
           << "  \"selectedAlias\": \"" << JsonEscape(proof.camera_alias) << "\",\n"
           << "  \"providerId\": \"" << JsonEscape(proof.provider_id) << "\",\n"
           << "  \"providerVersion\": " << proof.provider_version << ",\n"
           << "  \"sdkIdentitySha256\": \"" << proof.sdk_identity_sha256 << "\",\n"
           << "  \"wpdIdentitySha256\": \"" << proof.wpd_identity_sha256 << "\",\n"
           << "  \"createdAtUtc\": \"" << proof.created_at_utc << "\",\n"
           << "  \"expiresAtUtc\": \"" << proof.expires_at_utc << "\",\n"
           << "  \"singleCameraConnectedConfirmed\": "
           << (proof.single_camera_connected_confirmed ? "true" : "false") << ",\n"
           << "  \"documentedCorrelationConfirmed\": "
           << (proof.documented_correlation_confirmed ? "true" : "false") << ",\n"
           << "  \"proofPayloadSha256\": \"" << proof.proof_payload_sha256 << "\"\n"
           << "}\n";
    return output.str();
}

DualIdentityCorrelationProvider ParseDualIdentityCorrelationProvider(
    std::string_view json) {
    const JsonValue root = JsonParser(json).Parse();
    RequireExactFields(root, {
        "schemaVersion", "providerId", "providerVersion",
        "documentedStablePerBodyCorrelation"});
    const auto version =
        RequireField(root, "providerVersion", JsonKind::integer).integer;
    if (RequireField(root, "schemaVersion", JsonKind::string).string !=
            "a0.camera-agent.dual-correlation-provider.v1" ||
        version < 1 || version > std::numeric_limits<std::uint32_t>::max()) {
        throw HardwareCameraAgentProtocolError(
            "InvalidDualIdentityProvider",
            "dual identity provider schema or version is invalid");
    }
    DualIdentityCorrelationProvider provider;
    provider.provider_id =
        RequireField(root, "providerId", JsonKind::string).string;
    provider.provider_version = static_cast<std::uint32_t>(version);
    provider.documented_stable_per_body_correlation = RequireField(
        root, "documentedStablePerBodyCorrelation", JsonKind::boolean).boolean;
    if (!IsSafeRequestId(provider.provider_id)) {
        throw HardwareCameraAgentProtocolError(
            "InvalidDualIdentityProvider",
            "dual identity provider ID is invalid");
    }
    return provider;
}

DualIdentityCorrelationProvider LoadDualIdentityCorrelationProvider(
    const fs::path& path) {
    const fs::path absolute = StrictFixedLocalPath(
        path, "DualCamera identity provider config");
    std::error_code error;
    if (!fs::is_regular_file(absolute, error) || error || IsReparsePoint(absolute)) {
        throw std::runtime_error(
            "DualCamera identity provider config must be a regular reparse-free local file");
    }
    const auto size = fs::file_size(absolute, error);
    if (error || size == 0 || size > 16U * 1024U) {
        throw std::runtime_error("DualCamera identity provider config size is invalid");
    }
    std::ifstream input(absolute, std::ios::binary);
    const std::string body{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (!input || input.bad() || body.size() != size) {
        throw std::runtime_error(
            "DualCamera identity provider config could not be read completely");
    }
    return ParseDualIdentityCorrelationProvider(body);
}

DualIdentityBindingProof ParseDualIdentityBindingProof(std::string_view json) {
    const JsonValue root = JsonParser(json).Parse();
    RequireExactFields(root, {
        "schemaVersion", "bindingVersion", "cameraMode", "selectedAlias",
        "providerId", "providerVersion", "sdkIdentitySha256",
        "wpdIdentitySha256", "createdAtUtc", "expiresAtUtc",
        "singleCameraConnectedConfirmed", "documentedCorrelationConfirmed",
        "proofPayloadSha256"});
    const auto binding_version =
        RequireField(root, "bindingVersion", JsonKind::integer).integer;
    const auto provider_version =
        RequireField(root, "providerVersion", JsonKind::integer).integer;
    if (RequireField(root, "schemaVersion", JsonKind::string).string !=
            "a0.camera-agent.dual-identity-binding-proof.v1" ||
        binding_version != 1 ||
        RequireField(root, "cameraMode", JsonKind::string).string != "DualCamera" ||
        provider_version < 1 ||
        provider_version > std::numeric_limits<std::uint32_t>::max()) {
        throw HardwareCameraAgentProtocolError(
            "InvalidDualIdentityProof", "dual identity proof schema or version is invalid");
    }

    DualIdentityBindingProof proof;
    proof.camera_alias =
        RequireField(root, "selectedAlias", JsonKind::string).string;
    proof.provider_id = RequireField(root, "providerId", JsonKind::string).string;
    proof.provider_version = static_cast<std::uint32_t>(provider_version);
    proof.sdk_identity_sha256 =
        RequireField(root, "sdkIdentitySha256", JsonKind::string).string;
    proof.wpd_identity_sha256 =
        RequireField(root, "wpdIdentitySha256", JsonKind::string).string;
    proof.created_at_utc =
        RequireField(root, "createdAtUtc", JsonKind::string).string;
    proof.expires_at_utc =
        RequireField(root, "expiresAtUtc", JsonKind::string).string;
    proof.single_camera_connected_confirmed =
        RequireField(root, "singleCameraConnectedConfirmed", JsonKind::boolean).boolean;
    proof.documented_correlation_confirmed =
        RequireField(root, "documentedCorrelationConfirmed", JsonKind::boolean).boolean;
    proof.proof_payload_sha256 =
        RequireField(root, "proofPayloadSha256", JsonKind::string).string;

    if ((proof.camera_alias != "CAM-A" && proof.camera_alias != "CAM-B") ||
        !IsSafeRequestId(proof.provider_id) ||
        !IsLowerHex(proof.sdk_identity_sha256, 64) ||
        !IsLowerHex(proof.wpd_identity_sha256, 64) ||
        !IsLowerHex(proof.proof_payload_sha256, 64)) {
        throw HardwareCameraAgentProtocolError(
            "InvalidDualIdentityProof", "dual identity proof contains an invalid value");
    }
    const FILETIME created = ParseProfileUtc(proof.created_at_utc);
    const FILETIME expires = ParseProfileUtc(proof.expires_at_utc);
    if (CompareFileTime(&created, &expires) >= 0) {
        throw HardwareCameraAgentProtocolError(
            "InvalidDualIdentityProof", "dual identity proof validity window is invalid");
    }
    if (proof.proof_payload_sha256 !=
        ComputeDualIdentityBindingProofPayloadSha256(proof)) {
        throw HardwareCameraAgentProtocolError(
            "DualIdentityProofTampered", "dual identity proof payload digest does not match");
    }
    return proof;
}

DualIdentityBindingProof LoadDualIdentityBindingProof(const fs::path& path) {
    const fs::path absolute = StrictFixedLocalPath(path, "DualCamera identity proof");
    std::error_code error;
    if (!fs::is_regular_file(absolute, error) || error || IsReparsePoint(absolute)) {
        throw std::runtime_error("DualCamera identity proof must be a regular reparse-free local file");
    }
    const auto size = fs::file_size(absolute, error);
    if (error || size == 0 || size > 16U * 1024U) {
        throw std::runtime_error("DualCamera identity proof size is invalid");
    }
    std::ifstream input(absolute, std::ios::binary);
    const std::string body{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (!input || input.bad() || body.size() != size) {
        throw std::runtime_error("DualCamera identity proof could not be read completely");
    }
    return ParseDualIdentityBindingProof(body);
}

DualIdentityResult VerifyDualIdentitySoftwareContract(
    const std::optional<DualIdentityCorrelationProvider>& provider,
    const std::vector<fs::path>& proof_paths,
    const std::vector<DualIdentityInventoryProjection>& sdk_inventory,
    const std::vector<DualIdentityInventoryProjection>& wpd_inventory,
    std::string_view observed_at_utc,
    bool legacy_map_fallback_requested) {
    const auto blocked = [](DualIdentityBlockReason reason) -> DualIdentityResult {
        return DualIdentityBlocked{reason, {}};
    };
    if (legacy_map_fallback_requested) {
        return blocked(DualIdentityBlockReason::legacy_map_fallback_prohibited);
    }
    if (!provider || !provider->documented_stable_per_body_correlation ||
        !IsSafeRequestId(provider->provider_id) || provider->provider_version == 0) {
        return blocked(DualIdentityBlockReason::identity_strategy_unresolved);
    }
    if (proof_paths.size() != 2) {
        return blocked(DualIdentityBlockReason::proof_count_mismatch);
    }

    std::vector<DualIdentityBindingProof> proofs;
    proofs.reserve(2);
    try {
        for (const auto& path : proof_paths) {
            proofs.push_back(LoadDualIdentityBindingProof(path));
        }
    } catch (const HardwareCameraAgentProtocolError& error) {
        return blocked(error.Code() == "DualIdentityProofTampered"
            ? DualIdentityBlockReason::proof_tampered
            : DualIdentityBlockReason::proof_invalid);
    } catch (const std::exception&) {
        return blocked(DualIdentityBlockReason::proof_invalid);
    }

    FILETIME observed{};
    try {
        observed = ParseProfileUtc(observed_at_utc);
    } catch (const std::exception&) {
        return blocked(DualIdentityBlockReason::proof_invalid);
    }
    std::size_t proof_cam_a = 0;
    std::size_t proof_cam_b = 0;
    for (const auto& proof : proofs) {
        proof_cam_a += proof.camera_alias == "CAM-A" ? 1U : 0U;
        proof_cam_b += proof.camera_alias == "CAM-B" ? 1U : 0U;
        if (proof.provider_id != provider->provider_id ||
            proof.provider_version != provider->provider_version) {
            return blocked(DualIdentityBlockReason::provider_mismatch);
        }
        if (!proof.single_camera_connected_confirmed ||
            !proof.documented_correlation_confirmed) {
            return blocked(DualIdentityBlockReason::confirmation_mismatch);
        }
        const FILETIME created = ParseProfileUtc(proof.created_at_utc);
        const FILETIME expires = ParseProfileUtc(proof.expires_at_utc);
        if (CompareFileTime(&created, &observed) > 0 ||
            CompareFileTime(&expires, &observed) <= 0) {
            return blocked(DualIdentityBlockReason::proof_stale);
        }
    }
    if (proof_cam_a != 1 || proof_cam_b != 1) {
        return blocked(DualIdentityBlockReason::alias_cardinality_mismatch);
    }
    if (proofs[0].sdk_identity_sha256 == proofs[1].sdk_identity_sha256 ||
        proofs[0].wpd_identity_sha256 == proofs[1].wpd_identity_sha256) {
        return blocked(DualIdentityBlockReason::identity_collision);
    }
    if (sdk_inventory.size() != 2 || wpd_inventory.size() != 2) {
        return blocked(DualIdentityBlockReason::camera_count_mismatch);
    }

    const auto validate_inventory = [&](const std::vector<DualIdentityInventoryProjection>& inventory,
                                        DualIdentityTransport transport) ->
        std::optional<DualIdentityBlockReason> {
        std::set<std::string> identities;
        for (const auto& camera : inventory) {
            if (camera.transport != transport) {
                return DualIdentityBlockReason::mismatched_transport;
            }
            if (camera.model != "Nikon D810" ||
                !IsLowerHex(camera.identity_sha256, 64)) {
                return DualIdentityBlockReason::missing_identity;
            }
            if (camera.provider_id != provider->provider_id ||
                camera.provider_version != provider->provider_version) {
                return DualIdentityBlockReason::provider_mismatch;
            }
            if (!identities.insert(camera.identity_sha256).second) {
                return DualIdentityBlockReason::duplicate_identity;
            }
        }
        return std::nullopt;
    };
    if (const auto reason = validate_inventory(sdk_inventory, DualIdentityTransport::sdk)) {
        return blocked(*reason);
    }
    if (const auto reason = validate_inventory(wpd_inventory, DualIdentityTransport::wpd)) {
        return blocked(*reason);
    }

    DualIdentityReady ready;
    const auto count_matches = [&](const std::vector<DualIdentityInventoryProjection>& inventory,
                                   DualIdentityTransport transport,
                                   std::size_t& cam_a,
                                   std::size_t& cam_b,
                                   std::size_t& unbound) ->
        std::optional<DualIdentityBlockReason> {
        for (const auto& camera : inventory) {
            bool matched = false;
            for (const auto& proof : proofs) {
                const auto& expected = transport == DualIdentityTransport::sdk
                    ? proof.sdk_identity_sha256 : proof.wpd_identity_sha256;
                const auto& opposite = transport == DualIdentityTransport::sdk
                    ? proof.wpd_identity_sha256 : proof.sdk_identity_sha256;
                if (camera.identity_sha256 == opposite) {
                    return DualIdentityBlockReason::mismatched_transport;
                }
                if (camera.identity_sha256 == expected) {
                    matched = true;
                    if (proof.camera_alias == "CAM-A") ++cam_a;
                    else ++cam_b;
                }
            }
            if (!matched) ++unbound;
        }
        return std::nullopt;
    };
    if (const auto reason = count_matches(
            sdk_inventory, DualIdentityTransport::sdk,
            ready.sdk_cam_a_count, ready.sdk_cam_b_count,
            ready.sdk_unbound_count)) {
        return blocked(*reason);
    }
    if (const auto reason = count_matches(
            wpd_inventory, DualIdentityTransport::wpd,
            ready.wpd_cam_a_count, ready.wpd_cam_b_count,
            ready.wpd_unbound_count)) {
        return blocked(*reason);
    }
    if (ready.sdk_unbound_count != 0 || ready.wpd_unbound_count != 0) {
        return blocked(DualIdentityBlockReason::unbound_identity);
    }
    if (ready.sdk_cam_a_count != 1 || ready.sdk_cam_b_count != 1 ||
        ready.wpd_cam_a_count != 1 || ready.wpd_cam_b_count != 1) {
        return blocked(DualIdentityBlockReason::alias_cardinality_mismatch);
    }
    return ready;
}

std::string_view DualIdentityBlockReasonName(
    DualIdentityBlockReason reason) noexcept {
    switch (reason) {
    case DualIdentityBlockReason::identity_strategy_unresolved:
        return "identity_strategy_unresolved";
    case DualIdentityBlockReason::provider_config_invalid:
        return "provider_config_invalid";
    case DualIdentityBlockReason::legacy_map_fallback_prohibited:
        return "legacy_map_fallback_prohibited";
    case DualIdentityBlockReason::proof_count_mismatch:
        return "proof_count_mismatch";
    case DualIdentityBlockReason::proof_invalid:
        return "proof_invalid";
    case DualIdentityBlockReason::proof_tampered:
        return "proof_tampered";
    case DualIdentityBlockReason::proof_stale:
        return "proof_stale";
    case DualIdentityBlockReason::confirmation_mismatch:
        return "confirmation_mismatch";
    case DualIdentityBlockReason::provider_mismatch:
        return "provider_mismatch";
    case DualIdentityBlockReason::camera_count_mismatch:
        return "camera_count_mismatch";
    case DualIdentityBlockReason::missing_identity:
        return "missing_identity";
    case DualIdentityBlockReason::duplicate_identity:
        return "duplicate_identity";
    case DualIdentityBlockReason::identity_collision:
        return "identity_collision";
    case DualIdentityBlockReason::mismatched_transport:
        return "mismatched_transport";
    case DualIdentityBlockReason::unbound_identity:
        return "unbound_identity";
    case DualIdentityBlockReason::alias_cardinality_mismatch:
        return "alias_cardinality_mismatch";
    }
    return "identity_strategy_unresolved";
}

DualIdentityResult RunProductionDualIdentityPreflight(
    const ProductionDualIdentityPreflightRequest& request) {
    std::optional<DualIdentityCorrelationProvider> provider;
    if (request.provider_config_path) {
        try {
            provider = LoadDualIdentityCorrelationProvider(
                *request.provider_config_path);
        } catch (const std::exception&) {
            return DualIdentityBlocked{
                DualIdentityBlockReason::provider_config_invalid, {}};
        }
    }
    return VerifyDualIdentitySoftwareContract(
        provider,
        request.proof_paths,
        request.sdk_inventory,
        request.wpd_inventory,
        request.observed_at_utc,
        request.legacy_map_fallback_requested);
}

SingleCameraIdentityV3 ParseSingleCameraIdentityV3(std::string_view json) {
    const JsonValue root = JsonParser(json).Parse();
    RequireExactFields(root, {
        "schemaVersion", "cameraMode", "selectedAlias",
        "wpdStableIdentitySha256", "sdkSelectionPolicy"});
    if (RequireField(root, "schemaVersion", JsonKind::string).string !=
            "a0.camera-agent.single-identity.v3" ||
        RequireField(root, "cameraMode", JsonKind::string).string !=
            "SingleCamera") {
        throw HardwareCameraAgentProtocolError(
            "InvalidSingleIdentityV3", "identity-v3 schema or mode is invalid");
    }
    SingleCameraIdentityV3 identity;
    identity.camera_alias =
        RequireField(root, "selectedAlias", JsonKind::string).string;
    identity.wpd_stable_identity_sha256 =
        RequireField(root, "wpdStableIdentitySha256", JsonKind::string).string;
    identity.sdk_selection_policy =
        RequireField(root, "sdkSelectionPolicy", JsonKind::string).string;
    if (identity.camera_alias != "CAM-A" ||
        !IsLowerHex(identity.wpd_stable_identity_sha256, 64) ||
        identity.sdk_selection_policy != "exactly-one-current-session") {
        throw HardwareCameraAgentProtocolError(
            "InvalidSingleIdentityV3", "identity-v3 values are invalid");
    }
    return identity;
}

SingleCameraIdentityV3 LoadSingleCameraIdentityV3(const fs::path& path) {
    return LoadStrictSingleIdentityV3(path);
}

namespace {

void ValidateSingleCameraSdkStatusIdentityPolicy(
    const SingleCameraIdentityV3& identity,
    std::string_view requested_alias) {
    if (requested_alias != "CAM-A" || identity.camera_alias != "CAM-A" ||
        identity.sdk_selection_policy != "exactly-one-current-session") {
        throw TransportError(
            "single_identity_v3_invalid",
            "SingleCamera sdk-status requires the CAM-A identity-v3 exact-one policy");
    }
}

void ValidateSingleCameraSdkStatusWpdIdentity(
    const SingleCameraIdentityV3& identity,
    const std::vector<CameraInfo>& wpd_cameras) {
    if (wpd_cameras.size() != 1 ||
        wpd_cameras.front().model != "Nikon D810") {
        throw TransportError(
            "single_camera_count_mismatch",
            "SingleCamera sdk-status requires exactly one D810 in the WPD inventory");
    }
    if (wpd_cameras.front().stable_identity !=
        identity.wpd_stable_identity_sha256) {
        throw TransportError(
            "single_identity_v3_mismatch",
            "the current WPD body does not match the registered SingleCamera identity-v3");
    }
}

CameraInfo ResolveSingleCameraSdkStatusSdkProjection(
    const std::vector<CameraInfo>& sdk_cameras) {
    if (sdk_cameras.size() != 1 ||
        sdk_cameras.front().model != "Nikon D810") {
        throw TransportError(
            "single_camera_count_mismatch",
            "SingleCamera sdk-status requires exactly one D810 in the SDK inventory");
    }
    if (sdk_cameras.front().stable_identity.empty()) {
        throw TransportError(
            "single_identity_v3_invalid",
            "the exact-one current SDK projection has no selectable identity");
    }
    return sdk_cameras.front();
}

} // namespace

CameraInfo ResolveSingleCameraSdkStatusCamera(
    const SingleCameraIdentityV3& identity,
    std::string_view requested_alias,
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras) {
    ValidateSingleCameraSdkStatusIdentityPolicy(identity, requested_alias);
    ValidateSingleCameraSdkStatusWpdIdentity(identity, wpd_cameras);
    return ResolveSingleCameraSdkStatusSdkProjection(sdk_cameras);
}

SingleIdentityV3SdkStatusExecution ExecuteSingleIdentityV3SdkStatus(
    const fs::path& identity_path,
    std::string_view requested_alias,
    const std::function<std::unique_ptr<ISingleIdentityV3WpdEnumerator>()>&
        wpd_factory,
    const std::function<std::unique_ptr<ISdkStatusExecutor>()>& sdk_factory,
    std::chrono::seconds timeout) {
    const auto identity = LoadSingleCameraIdentityV3(identity_path);
    ValidateSingleCameraSdkStatusIdentityPolicy(identity, requested_alias);

    SingleIdentityV3SdkStatusExecution execution;
    execution.routing.sdk_status_executor_selected = true;
    execution.routing.single_identity_v3_selected = true;
    auto wpd = wpd_factory();
    if (!wpd) {
        throw TransportError(
            "wpd_identity_enumerator_unavailable",
            "SingleCamera identity-v3 WPD enumerator factory returned no enumerator");
    }
    const auto wpd_cameras = wpd->Enumerate();
    ++execution.routing.wpd_identity_enumeration_count;
    ValidateSingleCameraSdkStatusWpdIdentity(identity, wpd_cameras);

    auto sdk = sdk_factory();
    if (!sdk) {
        throw TransportError(
            "sdk_status_executor_unavailable",
            "SingleCamera sdk-status executor factory returned no executor");
    }
    sdk->RequireExactlyOneD810ForSingleStatus();
    const auto sdk_cameras = sdk->Enumerate();
    ++execution.routing.sdk_enumeration_count;
    execution.camera = ResolveSingleCameraSdkStatusSdkProjection(sdk_cameras);
    execution.status = sdk->ProbeSdkStatus(
        execution.camera.stable_identity, timeout);
    ++execution.routing.sdk_status_probe_count;
    if (const auto failure = ValidateSdkStatusProcessRouting(execution.routing)) {
        throw TransportError(
            "sdk_status_routing_invalid",
            "SingleCamera sdk-status routing failed: " + std::string(*failure));
    }
    execution.sdk_version = sdk->SdkVersion();
    return execution;
}

bool IsContinuousLiveViewResultStructurallyValid(
    const ContinuousLiveViewResult& result,
    const HardwareCameraAgentRequest& request) noexcept {
    if (result.camera_alias != "CAM-A" ||
        result.session_id != request.session_id ||
        result.preview_is_original || result.preview_is_stitch_input ||
        result.real_identifiers_included ||
        result.heartbeat_timeout_seconds != 20 ||
        result.maximum_session_seconds != 600) {
        return false;
    }
    if (!result.succeeded) {
        return !result.error_category.empty() &&
            result.frame_jpeg_base64.empty() && result.frame_size == 0 &&
            !result.sdk_session_open && !result.live_view_running;
    }
    if (!result.error_category.empty() || !result.error_detail.empty()) return false;
    if (request.operation == HardwareCameraAgentOperation::start_live_view) {
        return result.state == "Started" && result.sdk_session_open &&
            result.live_view_running && result.frame_number == 0 &&
            result.frame_size == 0 && result.frame_jpeg_base64.empty();
    }
    if (request.operation == HardwareCameraAgentOperation::read_live_view_frame) {
        return result.state == "Frame" && result.sdk_session_open &&
            result.live_view_running && result.frame_number > 0 &&
            result.frame_size >= 4 && result.frame_size <= 512U * 1024U &&
            IsLowerHex(result.frame_sha256, 64) && !result.frame_jpeg_base64.empty();
    }
    if (request.operation == HardwareCameraAgentOperation::live_view_heartbeat) {
        return result.state == "Heartbeat" && result.sdk_session_open &&
            result.live_view_running && result.frame_size == 0 &&
            result.frame_jpeg_base64.empty();
    }
    return (result.state == "Stopped" || result.state == "Closed") &&
        !result.sdk_session_open && !result.live_view_running &&
        result.frame_size == 0 && result.frame_jpeg_base64.empty();
}

HardwareCameraAgentProtocolError::HardwareCameraAgentProtocolError(
    std::string code,
    std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

const std::string& HardwareCameraAgentProtocolError::Code() const noexcept {
    return code_;
}

HardwareCameraAgentRequest ParseHardwareCameraAgentRequest(std::string_view json) {
    const JsonValue root = JsonParser(json).Parse();
    RequireExactFields(root, {"schemaVersion", "simulation", "marker", "requestId", "operation", "payload"});
    const auto& schema = RequireField(root, "schemaVersion", JsonKind::string).string;
    const bool live_view_v2 = schema == kHardwareCameraAgentLiveViewSchemaVersion;
    if (schema != kHardwareCameraAgentSchemaVersion && !live_view_v2) {
        ProtocolFailure("UnsupportedSchemaVersion", "hardware protocol schema version is not supported");
    }
    if (RequireField(root, "simulation", JsonKind::boolean).boolean) {
        ProtocolFailure("HardwareMarkerRequired", "simulation=false is required by the hardware protocol");
    }
    if (RequireField(root, "marker", JsonKind::string).string != kHardwareCameraAgentMarker) {
        ProtocolFailure("HardwareMarkerRequired", "the Hardware marker is required");
    }
    const auto& request_id = RequireField(root, "requestId", JsonKind::string).string;
    if (!IsSafeRequestId(request_id)) ProtocolFailure("InvalidRequestId", "a bounded safe request ID is required");
    const auto& operation_name = RequireField(root, "operation", JsonKind::string).string;
    const auto& payload = RequireField(root, "payload", JsonKind::object);

    HardwareCameraAgentRequest request;
    request.schema_version = schema;
    request.request_id = request_id;
    if (live_view_v2) {
        if (operation_name == "start-live-view") {
            request.operation = HardwareCameraAgentOperation::start_live_view;
            RequireExactFields(payload, {
                "cameraAlias", "sessionId", "exclusiveCameraControlConfirmed"});
            request.camera_alias =
                RequireField(payload, "cameraAlias", JsonKind::string).string;
            ValidateAlias(request.camera_alias);
            if (request.camera_alias != "CAM-A") {
                ProtocolFailure(
                    "InvalidLiveViewRequest", "initial SingleCamera Live View is CAM-A only");
            }
            request.session_id =
                RequireField(payload, "sessionId", JsonKind::string).string;
            request.exclusive_camera_control_confirmed = RequireField(
                payload, "exclusiveCameraControlConfirmed", JsonKind::boolean).boolean;
            if (!IsSafeTransactionId(request.session_id) ||
                !request.exclusive_camera_control_confirmed) {
                ProtocolFailure(
                    "InvalidLiveViewRequest",
                    "start-live-view requires a 32-hex session and exclusive confirmation");
            }
            return request;
        }
        if (operation_name == "read-live-view-frame") {
            request.operation = HardwareCameraAgentOperation::read_live_view_frame;
        } else if (operation_name == "live-view-heartbeat") {
            request.operation = HardwareCameraAgentOperation::live_view_heartbeat;
        } else if (operation_name == "stop-live-view") {
            request.operation = HardwareCameraAgentOperation::stop_live_view;
        } else if (operation_name == "close-agent-session") {
            request.operation = HardwareCameraAgentOperation::close_agent_session;
        } else {
            ProtocolFailure(
                "UnsupportedOperation", "hardware Live View v2 operation is not supported");
        }
        RequireExactFields(payload, {"sessionId"});
        request.session_id =
            RequireField(payload, "sessionId", JsonKind::string).string;
        if (!IsSafeTransactionId(request.session_id)) {
            ProtocolFailure(
                "InvalidLiveViewRequest", "Live View sessionId must be 32 hexadecimal characters");
        }
        return request;
    }
    if (operation_name == "get-single-readiness") {
        request.operation = HardwareCameraAgentOperation::get_single_readiness;
        RequireExactFields(payload, {"cameraAlias"});
    } else if (operation_name == "capture-single") {
        request.operation = HardwareCameraAgentOperation::capture_single;
        RequireExactFields(payload, {
            "transactionId",
            "cameraAlias",
            "expectedCaptureProfileId",
            "expectedCaptureProfileVersion",
            "expectedCaptureProfileSha256",
            "expectedCaptureProfileExpiresAtUtc",
            "exclusiveCameraControlConfirmed",
            "dedicatedSpoolScopeConfirmed",
            "exactObjectDeleteConfirmed",
            "liveViewHandoffRequested",
        });
        request.transaction_id = RequireField(payload, "transactionId", JsonKind::string).string;
        if (!IsSafeTransactionId(request.transaction_id)) {
            ProtocolFailure("InvalidTransactionId", "capture transactionId must contain exactly 32 hexadecimal characters");
        }
        request.expected_capture_profile_id =
            RequireField(payload, "expectedCaptureProfileId", JsonKind::string).string;
        const auto expected_profile_version =
            RequireField(payload, "expectedCaptureProfileVersion", JsonKind::integer).integer;
        request.expected_capture_profile_sha256 =
            RequireField(payload, "expectedCaptureProfileSha256", JsonKind::string).string;
        request.expected_capture_profile_expires_at_utc =
            RequireField(
                payload, "expectedCaptureProfileExpiresAtUtc", JsonKind::string).string;
        if (!IsSafeRequestId(request.expected_capture_profile_id) ||
            expected_profile_version < 1 ||
            static_cast<std::uint64_t>(expected_profile_version) >
                std::numeric_limits<std::uint32_t>::max() ||
            !IsLowerHex(request.expected_capture_profile_sha256, 64) ||
            !IsValidProfileTimestamp(
                request.expected_capture_profile_expires_at_utc)) {
            ProtocolFailure(
                "InvalidCaptureProfileSnapshot",
                "capture request requires a valid readiness-approved profile ID/version/SHA-256/expiry");
        }
        request.expected_capture_profile_version =
            static_cast<std::uint32_t>(expected_profile_version);
        request.exclusive_camera_control_confirmed =
            RequireField(payload, "exclusiveCameraControlConfirmed", JsonKind::boolean).boolean;
        request.dedicated_spool_scope_confirmed =
            RequireField(payload, "dedicatedSpoolScopeConfirmed", JsonKind::boolean).boolean;
        request.exact_object_delete_confirmed =
            RequireField(payload, "exactObjectDeleteConfirmed", JsonKind::boolean).boolean;
        request.live_view_handoff_requested =
            RequireField(payload, "liveViewHandoffRequested", JsonKind::boolean).boolean;
    } else if (operation_name == "live-view-probe") {
        request.operation = HardwareCameraAgentOperation::live_view_probe;
        RequireExactFields(payload, {"cameraAlias", "exclusiveCameraControlConfirmed", "liveViewFrames", "liveViewIntervalMs"});
        request.exclusive_camera_control_confirmed =
            RequireField(payload, "exclusiveCameraControlConfirmed", JsonKind::boolean).boolean;
        if (!request.exclusive_camera_control_confirmed) {
            ProtocolFailure("SafetyConfirmationRequired", "live-view-probe requires exclusive camera control confirmation");
        }
        const auto frames = RequireField(payload, "liveViewFrames", JsonKind::integer).integer;
        const auto interval = RequireField(payload, "liveViewIntervalMs", JsonKind::integer).integer;
        if (frames < 1 || frames > 30) ProtocolFailure("InvalidLiveViewRequest", "live view frames must be between 1 and 30");
        if (interval < 0 || interval > 1000) ProtocolFailure("InvalidLiveViewRequest", "live view intervalMs must be between 0 and 1000");
        request.live_view_frames = static_cast<int>(frames);
        request.live_view_interval_ms = static_cast<int>(interval);
    } else if (operation_name == "get-transaction-result") {
        request.operation = HardwareCameraAgentOperation::get_transaction_result;
        RequireExactFields(payload, {"transactionId"});
        request.transaction_id = RequireField(payload, "transactionId", JsonKind::string).string;
        if (!IsSafeTransactionId(request.transaction_id)) {
            ProtocolFailure("InvalidTransactionId", "transactionId must contain exactly 32 hexadecimal characters");
        }
        return request;
    } else {
        ProtocolFailure("UnsupportedOperation", "hardware protocol operation is not supported");
    }
    request.camera_alias = RequireField(payload, "cameraAlias", JsonKind::string).string;
    ValidateAlias(request.camera_alias);
    return request;
}

std::string SerializeHardwareCameraAgentResponse(const HardwareCameraAgentResponse& response) {
    const std::string request_id = IsSafeRequestId(response.request_id) ? response.request_id : "rejected";
    std::ostringstream output;
    const std::string_view schema = response.schema_version == kHardwareCameraAgentLiveViewSchemaVersion
        ? kHardwareCameraAgentLiveViewSchemaVersion
        : kHardwareCameraAgentSchemaVersion;
    output << "{\"schemaVersion\":\"" << schema
           << "\",\"simulation\":false,\"marker\":\"" << kHardwareCameraAgentMarker
           << "\",\"requestId\":\"" << JsonEscape(request_id)
           << "\",\"success\":" << Bool(response.success)
           << ",\"resultCode\":\"" << JsonEscape(response.result_code) << "\",\"payload\":";

    if (response.readiness) {
        const auto& result = *response.readiness;
        output << "{\"cameraMode\":\"SingleCamera\",\"cameraAlias\":\"" << JsonEscape(result.camera_alias)
               << "\",\"ready\":" << Bool(result.ready)
               << ",\"sdkCameraCount\":" << result.sdk_camera_count
               << ",\"wpdCameraCount\":" << result.wpd_camera_count
               << ",\"sdkIdentityBound\":" << Bool(result.sdk_identity_bound)
               << ",\"wpdIdentityBound\":" << Bool(result.wpd_identity_bound)
               << ",\"sdkAliasMatches\":" << Bool(result.sdk_alias_matches)
               << ",\"wpdAliasMatches\":" << Bool(result.wpd_alias_matches)
               << ",\"sdkStatusProbed\":" << Bool(result.sdk_status_probed)
               << ",\"spoolInspected\":" << Bool(result.spool_inspected)
               << ",\"spoolPayloadObjectCount\":" << result.spool_payload_object_count
               << ",\"spoolKnownEmpty\":" << Bool(result.spool_known_empty)
               << ",\"firmware\":\"" << JsonEscape(result.firmware)
               << "\",\"liveViewStatus\":\"" << JsonEscape(result.live_view_status)
               << "\",\"liveViewStatusAvailable\":" << Bool(result.live_view_status_available)
               << ",\"captureProfileApproved\":" << Bool(result.capture_profile_approved)
               << ",\"captureProfileId\":\"" << JsonEscape(result.capture_profile_id)
               << "\",\"captureProfileVersion\":" << result.capture_profile_version
               << ",\"captureProfileSha256\":\""
               << JsonEscape(result.capture_profile_sha256)
               << "\",\"captureProfileCameraAlias\":\""
               << JsonEscape(result.capture_profile_camera_alias)
               << "\",\"profileExpiresAtUtc\":\""
               << JsonEscape(result.profile_expires_at_utc)
               << "\",\"captureProfileAliasMatches\":"
               << Bool(result.capture_profile_alias_matches)
               << ",\"settingsMatchApprovedProfile\":"
               << Bool(result.settings_match_approved_profile)
               << ",\"observedSettings\":";
        AppendObservedSettingsJson(output, result.observed_settings);
        output << ",\"readOnly\":" << Bool(result.read_only)
               << ",\"captureCommandSent\":" << Bool(result.capture_command_sent)
               << ",\"cameraObjectDeleteAttempted\":" << Bool(result.camera_object_delete_attempted)
               << ",\"cameraSettingsChanged\":" << Bool(result.camera_settings_changed)
               << ",\"realIdentifiersIncluded\":" << Bool(result.real_identifiers_included)
               << ",\"failureCategory\":\"" << JsonEscape(result.failure_category)
               << "\",\"failureDetail\":\"" << JsonEscape(result.failure_detail) << "\"}";
    } else if (response.capture) {
        const auto& result = *response.capture;
        output << "{\"cameraMode\":\"SingleCamera\",\"cameraAlias\":\"" << JsonEscape(result.camera_alias)
               << "\",\"requiredCameraAlias\":\"" << JsonEscape(result.camera_alias)
               << "\",\"runId\":\"" << JsonEscape(result.run_id)
               << "\",\"transactionId\":\"" << JsonEscape(result.transaction_id)
               << "\",\"captureProfileId\":\"" << JsonEscape(result.capture_profile_id)
               << "\",\"captureProfileVersion\":" << result.capture_profile_version
               << ",\"captureProfileSha256\":\""
               << JsonEscape(result.capture_profile_sha256)
               << "\",\"captureProfileCameraAlias\":\""
               << JsonEscape(result.capture_profile_camera_alias)
               << "\",\"profileExpiresAtUtc\":\""
               << JsonEscape(result.profile_expires_at_utc)
               << "\",\"terminalState\":\"" << JsonEscape(result.terminal_state)
               << "\",\"errorCategory\":\"" << JsonEscape(result.error_category)
               << "\",\"errorDetail\":\"" << JsonEscape(result.error_detail)
               << "\",\"retainedOriginal\":";
        if (result.retained_original) {
            output << "{\"cameraAlias\":\"" << JsonEscape(result.retained_original->camera_alias)
                   << "\",\"path\":\"" << JsonEscape(PathForJson(result.retained_original->path))
                   << "\",\"sizeBytes\":" << result.retained_original->size
                   << ",\"sha256\":\"" << JsonEscape(result.retained_original->sha256) << "\"}";
        } else {
            output << "null";
        }
        output << ",\"liveViewHandoffRequested\":" << Bool(result.live_view_handoff_requested)
               << ",\"liveViewStoppedBeforeCapture\":"
               << Bool(result.live_view_stopped_before_capture)
               << ",\"liveViewSdkSessionClosedBeforeCapture\":"
               << Bool(result.live_view_sdk_session_closed_before_capture)
               << ",\"postCaptureLiveViewProbeAttempted\":" << Bool(result.live_view_resume_attempted)
               << ",\"postCaptureLiveViewProbeSucceeded\":" << Bool(result.live_view_resumed)
               << ",\"postCapturePreview\":";
        if (result.resumed_preview) {
            output << "{\"path\":\"" << JsonEscape(PathForJson(result.resumed_preview->path))
                   << "\",\"sizeBytes\":" << result.resumed_preview->size
                   << ",\"sha256\":\"" << JsonEscape(result.resumed_preview->sha256) << "\"}";
        } else {
            output << "null";
        }
        output << ",\"spoolEmptyBeforeCapture\":" << Bool(result.spool_empty_before_capture)
               << ",\"cameraObjectDeleteAttempted\":" << Bool(result.camera_object_delete_attempted)
               << ",\"cameraObjectDeleteSucceeded\":" << Bool(result.camera_object_delete_succeeded)
               << ",\"spoolEmptyAfterCleanup\":" << Bool(result.spool_empty_after_cleanup)
               << ",\"automaticRetryCount\":" << result.automatic_retry_count
               << ",\"transactionWatchdogSeconds\":" << result.transaction_watchdog_seconds
               << ",\"realIdentifiersIncluded\":" << Bool(result.real_identifiers_included) << "}";
    } else if (response.live_view) {
        const auto& result = *response.live_view;
        output << "{\"cameraMode\":\"SingleCamera\",\"cameraAlias\":\"" << JsonEscape(result.camera_alias)
               << "\",\"runId\":\"" << JsonEscape(result.run_id)
               << "\",\"frames\":" << result.frames
               << ",\"lastFrameBytes\":" << result.last_frame_bytes
               << ",\"lastFrameSha256\":\"" << JsonEscape(result.last_frame_sha256)
               << "\",\"durationMs\":" << result.duration_ms
               << ",\"previewPersisted\":" << Bool(result.preview_persisted)
               << ",\"preview\":";
        if (result.preview) {
            output << "{\"path\":\"" << JsonEscape(PathForJson(result.preview->path))
                   << "\",\"sizeBytes\":" << result.preview->size
                   << ",\"sha256\":\"" << JsonEscape(result.preview->sha256) << "\"}";
        } else {
            output << "null";
        }
        output << ",\"previewIsOriginal\":" << Bool(result.preview_is_original)
               << ",\"previewIsStitchInput\":" << Bool(result.preview_is_stitch_input)
               << ",\"liveViewStopped\":" << Bool(result.live_view_stopped)
               << ",\"sdkSessionClosed\":" << Bool(result.sdk_session_closed)
               << ",\"realIdentifiersIncluded\":" << Bool(result.real_identifiers_included)
               << ",\"errorCategory\":\"" << JsonEscape(result.error_category)
               << "\",\"errorDetail\":\"" << JsonEscape(result.error_detail) << "\"}";
    } else if (response.continuous_live_view) {
        const auto& result = *response.continuous_live_view;
        output << "{\"cameraMode\":\"SingleCamera\",\"cameraAlias\":\""
               << JsonEscape(result.camera_alias)
               << "\",\"sessionId\":\"" << JsonEscape(result.session_id)
               << "\",\"state\":\"" << JsonEscape(result.state)
               << "\",\"frameNumber\":" << result.frame_number
               << ",\"frameSize\":" << result.frame_size
               << ",\"frameSha256\":\"" << JsonEscape(result.frame_sha256)
               << "\",\"frameJpegBase64\":\"" << result.frame_jpeg_base64
               << "\",\"previewIsOriginal\":" << Bool(result.preview_is_original)
               << ",\"previewIsStitchInput\":" << Bool(result.preview_is_stitch_input)
               << ",\"sdkSessionOpen\":" << Bool(result.sdk_session_open)
               << ",\"liveViewRunning\":" << Bool(result.live_view_running)
               << ",\"heartbeatTimeoutSeconds\":" << result.heartbeat_timeout_seconds
               << ",\"maximumSessionSeconds\":" << result.maximum_session_seconds
               << ",\"realIdentifiersIncluded\":" << Bool(result.real_identifiers_included)
               << ",\"errorCategory\":\"" << JsonEscape(result.error_category)
               << "\",\"errorDetail\":\"" << JsonEscape(result.error_detail) << "\"}";
    } else {
        output << "{\"cameraAccess\":\"None\",\"rejectionCode\":\""
               << JsonEscape(response.rejection_code)
               << "\",\"errorDetail\":\"" << JsonEscape(response.error_detail)
               << "\",\"realIdentifiersIncluded\":false}";
    }
    output << '}';
    return output.str();
}

SingleCameraBindingResolution ResolveExactlyOneBoundCamera(
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras,
    const IdentityMap& sdk_identity_map,
    const IdentityMap& wpd_identity_map,
    std::string_view camera_alias) {
    if (camera_alias != "CAM-A" && camera_alias != "CAM-B") {
        throw std::invalid_argument("single-camera alias must be CAM-A or CAM-B");
    }

    SingleCameraBindingResolution result;
    result.camera_alias = std::string(camera_alias);
    result.sdk_camera_count = sdk_cameras.size();
    result.wpd_camera_count = wpd_cameras.size();
    if (sdk_cameras.size() != 1 || wpd_cameras.size() != 1) {
        result.failure_category = "camera_count_mismatch";
        result.failure_detail = "single-camera operation requires exactly one SDK and one WPD D810 projection";
        return result;
    }

    const auto sdk_alias = sdk_identity_map.FindAlias(sdk_cameras.front().stable_identity);
    const auto wpd_alias = wpd_identity_map.FindAlias(wpd_cameras.front().stable_identity);
    result.sdk_identity_bound = sdk_alias.has_value();
    result.wpd_identity_bound = wpd_alias.has_value();
    result.sdk_alias_matches = sdk_alias && *sdk_alias == camera_alias;
    result.wpd_alias_matches = wpd_alias && *wpd_alias == camera_alias;
    if (!result.sdk_identity_bound || !result.wpd_identity_bound) {
        result.failure_category = "identity_unbound";
        result.failure_detail = "the connected D810 must be explicitly bound in both SDK and WPD identity maps";
        return result;
    }
    if (!result.sdk_alias_matches || !result.wpd_alias_matches) {
        result.failure_category = "cross_transport_alias_mismatch";
        result.failure_detail = "SDK and WPD must both resolve the connected D810 to the requested alias";
        return result;
    }

    result.sdk_camera = sdk_cameras.front();
    result.wpd_camera = wpd_cameras.front();
    result.ready = true;
    return result;
}

SingleCameraCaptureResult ExecuteBoundSingleCapture(
    const HardwareCameraAgentRequest& request,
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras,
    const IdentityMap& sdk_identity_map,
    const IdentityMap& wpd_identity_map,
    ICameraTransport& wpd_session,
    IPostCardObservationTransport& wpd,
    ICameraTransport& sdk_session,
    ICardCaptureTransport& sdk,
    EvidenceWriter& evidence,
    const SdkCameraStatus& pre_wpd_sdk_status,
    const std::function<SdkCameraStatus()>& shutter_session_status_probe,
    Timeouts timeouts,
    std::optional<std::chrono::steady_clock::time_point> transaction_deadline,
    const std::function<void()>& before_camera_object_delete,
    const std::function<void(const SdkCameraStatus&)>&
        validate_shutter_session_status) {
    if (request.operation != HardwareCameraAgentOperation::capture_single ||
        !request.exclusive_camera_control_confirmed ||
        !request.dedicated_spool_scope_confirmed ||
        !request.exact_object_delete_confirmed) {
        throw std::invalid_argument("capture-single requires all hardware safety confirmations");
    }

    SingleCameraCaptureResult result;
    result.camera_alias = request.camera_alias;
    result.run_id = evidence.RunId();
    result.transaction_id = request.transaction_id;
    result.transaction_watchdog_seconds = static_cast<int>(timeouts.transaction_watchdog.count());
    try {
        ValidateArtifactRunNoReparse(evidence.RunRoot());
    } catch (const TransportError& error) {
        result.terminal_state = "FailedPartial";
        result.error_category = error.Category();
        result.error_detail = SafeErrorDetail(error.what());
        return result;
    }
    if (transaction_deadline && std::chrono::steady_clock::now() >= *transaction_deadline) {
        result.terminal_state = "FailedPartial";
        result.error_category = "transaction_watchdog";
        result.error_detail =
            "single-camera transaction watchdog expired during preflight; camera access was not started";
        evidence.RecordState(
            "preflight", "HardwareAgentSingleCaptureWatchdogExpired", request.camera_alias,
            result.error_detail);
        return result;
    }
    const auto binding = ResolveExactlyOneBoundCamera(
        sdk_cameras, wpd_cameras, sdk_identity_map, wpd_identity_map, request.camera_alias);
    if (!binding.ready) {
        result.terminal_state = "Blocked";
        result.error_category = binding.failure_category;
        result.error_detail = binding.failure_detail;
        evidence.RecordState("preflight", "HardwareAgentSingleCaptureBlocked", request.camera_alias, result.error_detail);
        return result;
    }
    if (transaction_deadline && std::chrono::steady_clock::now() >= *transaction_deadline) {
        result.terminal_state = "FailedPartial";
        result.error_category = "transaction_watchdog";
        result.error_detail =
            "single-camera transaction watchdog expired after binding; capture was not started";
        evidence.RecordState(
            "preflight", "HardwareAgentSingleCaptureWatchdogExpired", request.camera_alias,
            result.error_detail);
        return result;
    }
    if (!LiveViewIsConfirmedOff(pre_wpd_sdk_status)) {
        SetLiveViewPreflightFailure(
            pre_wpd_sdk_status,
            result,
            "capture was blocked before opening WPD and no shutter command was sent");
        evidence.RecordState(
            "preflight",
            "HardwareAgentLiveViewNotConfirmedOff",
            request.camera_alias,
            result.error_detail);
        return result;
    }

    std::optional<LockedVerifiedOriginal> locked_original;
    const TransactionResult transaction = ExecuteHybridCaptureOnce(
        wpd_session,
        wpd,
        sdk_session,
        sdk,
        evidence,
        request.camera_alias,
        binding.wpd_camera->stable_identity,
        binding.sdk_camera->stable_identity,
        timeouts,
        {},
        [&] { ValidateArtifactRunNoReparse(evidence.RunRoot()); },
        transaction_deadline,
        [&](const FrameEvidence& frame) {
            locked_original = ValidateAndLockOriginalBeforeCameraDelete(
                evidence.RunRoot(), request.camera_alias, frame);
            if (before_camera_object_delete) before_camera_object_delete();
        },
        [&] {
            if (!shutter_session_status_probe) {
                throw TransportError(
                    "live_view_status_unavailable",
                    "the hardware Camera Agent has no open-session Live View status probe");
            }
            const SdkCameraStatus status = shutter_session_status_probe();
            if (!LiveViewIsConfirmedOff(status)) {
                throw TransportError(
                    status.live_view_status_available
                        ? "live_view_not_off"
                        : "live_view_status_unavailable",
                    "Live View was not confirmed OFF in the open SDK capture session immediately before shutter");
            }
            if (validate_shutter_session_status) {
                validate_shutter_session_status(status);
            }
        });
    result.terminal_state = transaction.terminal_state;
    result.error_category = transaction.error_category;
    result.error_detail = transaction.error_detail;
    result.spool_empty_before_capture = transaction.spool_empty_before_capture;
    result.camera_object_delete_attempted = transaction.camera_card_delete_attempted;
    result.camera_object_delete_succeeded = transaction.camera_card_delete_succeeded;
    result.spool_empty_after_cleanup = transaction.spool_empty_after_cleanup;
    if (transaction.frames.size() == 1 && transaction.frames.front().success) {
        const auto& frame = transaction.frames.front();
        result.retained_original = RetainedOriginalRecord{
            frame.camera_alias,
            fs::absolute(frame.path),
            frame.bytes,
            frame.sha256,
        };
    }
    result.succeeded = transaction.terminal_state == "Complete" &&
        result.retained_original.has_value() &&
        result.spool_empty_before_capture &&
        result.camera_object_delete_succeeded &&
        result.spool_empty_after_cleanup;
    return result;
}

ProductionHardwareCameraAgentConfig ProductionHardwareCameraAgentConfig::Defaults() {
    ProductionHardwareCameraAgentConfig config;
    config.sdk_identity_map = DefaultIdentityMapPath();
    config.wpd_identity_map = WpdIdentityMapPath(config.sdk_identity_map);
    config.single_identity_v3 =
        config.sdk_identity_map.parent_path() / "single-identity-v3.json";
    const fs::path root = config.sdk_identity_map.parent_path() / "camera-agent";
    config.artifacts_root = root / "artifacts";
    config.reports_root = root / "reports";
    config.transaction_state_root = root / "transactions";
    config.approved_capture_profile = root / "approved-single-capture-profile.json";
    return config;
}

class NikonContinuousLiveViewSdkTransport final
    : public IContinuousLiveViewSdkTransport {
public:
    NikonContinuousLiveViewSdkTransport() {
        sdk_.RequireExactlyOneD810ForProductAgent();
    }

    std::vector<CameraInfo> Enumerate() override { return sdk_.Enumerate(); }

    SdkCameraStatus ProbeSdkStatus(
        std::string_view stable_identity,
        std::chrono::seconds timeout) override {
        return sdk_.ProbeSdkStatus(stable_identity, timeout);
    }

    void OpenLiveView(
        std::string_view stable_identity,
        std::chrono::seconds timeout) override {
        sdk_.OpenLiveView(stable_identity, timeout);
    }

    void StartLiveView(std::chrono::seconds timeout) override {
        sdk_.StartLiveView(timeout);
    }

    std::vector<unsigned char> ReadLiveViewFrame(
        std::chrono::seconds timeout) override {
        return sdk_.ReadLiveViewFrame(timeout);
    }

    void StopLiveView(std::chrono::seconds timeout) override {
        sdk_.StopLiveView(timeout);
    }

    void Close(std::chrono::seconds timeout) override { sdk_.Close(timeout); }

private:
    NikonSdkTransport sdk_;
};

class ProductionHardwareCameraAgentBackend::Impl final {
public:
    explicit Impl(ProductionHardwareCameraAgentConfig config) : config_(std::move(config)) {
        if (config_.artifacts_root.empty() || config_.reports_root.empty() ||
            config_.transaction_state_root.empty()) {
            throw std::invalid_argument(
                "hardware Camera Agent durable roots must not be empty");
        }
        config_.artifacts_root =
            StrictFixedLocalPath(config_.artifacts_root, "artifact root");
        config_.reports_root =
            StrictFixedLocalPath(config_.reports_root, "report root");
        config_.transaction_state_root = StrictFixedLocalPath(
            config_.transaction_state_root, "transaction-state root");
        if (config_.timeouts.transaction_watchdog != std::chrono::seconds(180)) {
            throw std::invalid_argument("hardware Camera Agent transaction watchdog must be 180 seconds");
        }
        if (config_.timeouts.live_view_frame <= std::chrono::seconds(0) ||
            config_.timeouts.live_view_frame > config_.timeouts.open) {
            throw std::invalid_argument(
                "continuous Live View frame budget must be positive and not exceed the open budget");
        }
        if (static_cast<bool>(config_.continuous_live_view_sdk_factory_for_testing) !=
            static_cast<bool>(
                config_.continuous_live_view_identity_resolver_for_testing)) {
            throw std::invalid_argument(
                "continuous Live View test SDK factory and identity resolver must be configured together");
        }
    }

    SingleCameraReadinessResult GetSingleReadiness(std::string_view camera_alias) {
        SingleCameraReadinessResult result;
        result.camera_alias = std::string(camera_alias);
        result.read_only = true;
        try {
            approved_profile_ =
                LoadConfiguredApprovedCaptureProfile(
                    config_.approved_capture_profile);
        } catch (const std::exception&) {
            approved_profile_.reset();
            result.failure_category = "capture_profile_invalid";
            result.failure_detail =
                "the configured approved Single profile is malformed or outside fixed local storage";
            return result;
        }
        try {
            ValidateProductSingleIdentityConfiguration(config_);
            HardwareProcessLease lease;
            NikonSdkTransport sdk;
            WpdTransport wpd;
            sdk.RequireExactlyOneD810ForProductAgent();
            wpd.RequireExactlyOneD810ForProductAgent();
            const auto sdk_cameras = sdk.Enumerate();
            const auto wpd_cameras = wpd.Enumerate();
            const auto [sdk_map, wpd_map] =
                LoadProductSingleIdentityMaps(config_, sdk_cameras);
            const auto binding = ResolveExactlyOneBoundCamera(
                sdk_cameras, wpd_cameras, sdk_map, wpd_map, camera_alias);
            CopyBindingResult(binding, result);
            result.read_only = true;
            if (!binding.ready) return result;

            const auto sdk_status = sdk.ProbeSdkStatus(
                binding.sdk_camera->stable_identity, config_.timeouts.open);
            result.sdk_status_probed = true;
            result.firmware = sdk_status.firmware == "unknown"
                ? binding.sdk_camera->firmware
                : sdk_status.firmware;
            result.live_view_status = sdk_status.live_view_status;
            result.live_view_status_available = sdk_status.live_view_status_available;
            result.observed_settings = ToObservedSettings(sdk_status);
            result.capture_profile_approved = approved_profile_.has_value();
            if (approved_profile_) {
                result.capture_profile_id = approved_profile_->profile_id;
                result.capture_profile_version = approved_profile_->profile_version;
                result.capture_profile_sha256 = approved_profile_->sha256;
                result.capture_profile_camera_alias = approved_profile_->selected_alias;
                result.profile_expires_at_utc = approved_profile_->expires_at_utc;
                result.capture_profile_alias_matches =
                    approved_profile_->selected_alias == camera_alias;
            }
            result.settings_match_approved_profile = approved_profile_ &&
                result.capture_profile_alias_matches &&
                MatchesApprovedProfile(result.observed_settings, *approved_profile_);
            if (!LiveViewIsConfirmedOff(sdk_status)) {
                result.ready = false;
                result.failure_category = sdk_status.live_view_status_available
                    ? "live_view_not_off"
                    : "live_view_status_unavailable";
                result.failure_detail = sdk_status.live_view_status_available
                    ? "Live View must be confirmed OFF before inspecting the WPD spool"
                    : "Live View status must be available and confirmed OFF before inspecting the WPD spool";
                return result;
            }
            result.spool_payload_object_count = wpd.InspectSpoolPayloadCount(
                binding.wpd_camera->stable_identity, config_.timeouts.open);
            result.spool_inspected = true;
            result.spool_known_empty = result.spool_payload_object_count == 0;
            const bool profile_unexpired = approved_profile_ &&
                ProfileIsCurrentlyValid(*approved_profile_);
            result.ready = result.spool_known_empty && result.capture_profile_approved &&
                profile_unexpired &&
                result.capture_profile_alias_matches &&
                result.settings_match_approved_profile &&
                LiveViewIsConfirmedOff(sdk_status);
            if (!result.spool_known_empty) {
                result.failure_category = "spool_not_empty";
                result.failure_detail = "dedicated camera spool contains payload objects";
            } else if (!result.capture_profile_approved) {
                result.failure_category = "capture_profile_not_approved";
                result.failure_detail =
                    "no human-approved Single capture profile is configured";
            } else if (!profile_unexpired) {
                result.failure_category = "capture_profile_expired";
                result.failure_detail =
                    "the human-approved Single capture profile has expired";
            } else if (!result.capture_profile_alias_matches) {
                result.failure_category = "capture_profile_alias_mismatch";
                result.failure_detail =
                    "approved Single profile does not apply to the requested camera alias";
            } else if (!result.settings_match_approved_profile) {
                result.failure_category = "capture_settings_mismatch";
                result.failure_detail =
                    "read-only observed settings do not match the approved Single profile";
            }
        } catch (const TransportError& error) {
            result.ready = false;
            result.failure_category = error.Category();
            result.failure_detail = SafeErrorDetail(error.what());
        } catch (const std::exception&) {
            result.ready = false;
            result.failure_category = "readiness_exception";
            result.failure_detail = "single-camera readiness inspection failed closed";
        }
        return result;
    }

    SingleCameraCaptureResult CaptureSingle(const HardwareCameraAgentRequest& request) {
        if (!request.exclusive_camera_control_confirmed ||
            !request.dedicated_spool_scope_confirmed ||
            !request.exact_object_delete_confirmed) {
            throw HardwareCameraAgentProtocolError(
                "SafetyConfirmationRequired",
                "capture-single requires every hardware safety confirmation");
        }

        bool live_view_blocks_capture = false;
        {
            const std::scoped_lock lock(continuous_live_view_mutex_);
            live_view_blocks_capture =
                static_cast<bool>(live_view_sdk_) || residual_live_view_unsafe_;
        }
        if (live_view_blocks_capture) {
            SingleCameraCaptureResult blocked;
            blocked.camera_alias = request.camera_alias;
            // Blocked も他の終端結果と同じく有効な run_id を持たねば
            // IsCaptureResultStructurallyValid を通らず、本来の
            // continuous_live_view_active が InvalidBackendResult に化ける。
            blocked.run_id = NewRunId();
            blocked.transaction_id = request.transaction_id;
            blocked.capture_profile_id = request.expected_capture_profile_id;
            blocked.capture_profile_version = request.expected_capture_profile_version;
            blocked.capture_profile_sha256 = request.expected_capture_profile_sha256;
            blocked.capture_profile_camera_alias = request.camera_alias;
            blocked.profile_expires_at_utc =
                request.expected_capture_profile_expires_at_utc;
            blocked.live_view_handoff_requested =
                request.live_view_handoff_requested;
            blocked.terminal_state = "Blocked";
            blocked.error_category = "continuous_live_view_active";
            blocked.error_detail =
                "stop the owned continuous Live View session before capture";
            return blocked;
        }

        const auto transaction_deadline =
            std::chrono::steady_clock::now() + config_.timeouts.transaction_watchdog;
        const auto ensure_active = [&] {
            if (std::chrono::steady_clock::now() >= transaction_deadline) {
                throw TransportError(
                    "transaction_watchdog",
                    "single-camera transaction watchdog expired during agent preflight");
            }
        };
        bool capture_profile_invalid = false;
        try {
            approved_profile_ =
                LoadConfiguredApprovedCaptureProfile(
                    config_.approved_capture_profile);
        } catch (const std::exception&) {
            approved_profile_.reset();
            capture_profile_invalid = true;
        }
        std::unique_ptr<HardwareProcessLease> transaction_lease;
        try {
            transaction_lease = std::make_unique<HardwareProcessLease>(
                TransactionLeaseName(request.transaction_id));
        } catch (const TransportError& error) {
            if (error.Category() == "camera_control_busy") {
                throw HardwareCameraAgentProtocolError(
                    "DuplicateTransactionId",
                    "transactionId is already active and capture will not be repeated");
            }
            throw;
        }
        const std::string run_id = NewRunId();
        SingleCameraCaptureResult result;
        result.camera_alias = request.camera_alias;
        result.run_id = run_id;
        result.transaction_id = request.transaction_id;
        result.live_view_handoff_requested = request.live_view_handoff_requested;
        if (approved_profile_) {
            result.capture_profile_id = approved_profile_->profile_id;
            result.capture_profile_version = approved_profile_->profile_version;
            result.capture_profile_sha256 = approved_profile_->sha256;
            result.capture_profile_camera_alias = approved_profile_->selected_alias;
            result.profile_expires_at_utc = approved_profile_->expires_at_utc;
        }
        result.terminal_state = "Reserved";
        result.transaction_watchdog_seconds =
            static_cast<int>(config_.timeouts.transaction_watchdog.count());
        ReserveTransaction(
            config_.transaction_state_root,
            result,
            config_.after_transaction_reservation_directory_created_for_testing);

        std::string pre_camera_block_category;
        std::string pre_camera_block_detail;
        if (capture_profile_invalid) {
            pre_camera_block_category = "capture_profile_invalid";
            pre_camera_block_detail =
                "the configured approved Single profile is malformed or outside fixed local storage; camera access was not started";
        } else if (!approved_profile_) {
            pre_camera_block_category = "capture_profile_not_approved";
            pre_camera_block_detail =
                "no human-approved Single capture profile is configured; camera access was not started";
        } else if (!ProfileIsCurrentlyValid(*approved_profile_)) {
            pre_camera_block_category = "capture_profile_expired";
            pre_camera_block_detail =
                "the readiness-approved Single capture profile has expired; camera access was not started";
        } else if (approved_profile_->selected_alias != request.camera_alias) {
            pre_camera_block_category = "capture_profile_alias_mismatch";
            pre_camera_block_detail =
                "the approved Single profile does not apply to this camera alias; camera access was not started";
        } else if (request.expected_capture_profile_id != approved_profile_->profile_id ||
                   request.expected_capture_profile_version != approved_profile_->profile_version ||
                   request.expected_capture_profile_sha256 != approved_profile_->sha256 ||
                   request.expected_capture_profile_expires_at_utc !=
                       approved_profile_->expires_at_utc) {
            pre_camera_block_category = "capture_profile_snapshot_mismatch";
            pre_camera_block_detail =
                "the capture profile differs from the last readiness snapshot; camera access was not started";
        }
        if (!pre_camera_block_category.empty()) {
            result.succeeded = false;
            result.terminal_state = "Blocked";
            result.error_category = std::move(pre_camera_block_category);
            result.error_detail = std::move(pre_camera_block_detail);
            AtomicReplaceText(
                TransactionJournalPath(config_.transaction_state_root, request.transaction_id),
                SerializeTransactionJournal(result),
                [&] {
                    ValidateTransactionJournalScope(
                        config_.transaction_state_root, request.transaction_id);
                });
            transaction_lease.reset();
            return result;
        }

        std::unique_ptr<HardwareProcessLease> lease;
        bool live_view_stopped_before_capture = false;
        bool live_view_sdk_closed_before_capture = false;
        bool live_view_resume_attempted = false;
        bool live_view_resumed = false;
        std::optional<PreviewJpegRecord> resumed_preview;
        try {
            ValidateProductSingleIdentityConfiguration(config_);
            (void)PrepareExclusiveArtifactRun(config_.artifacts_root, run_id);
            ensure_active();
            lease = std::make_unique<HardwareProcessLease>();
            ensure_active();
            result.terminal_state = "InProgress";
            AtomicReplaceText(
                TransactionJournalPath(config_.transaction_state_root, request.transaction_id),
                SerializeTransactionJournal(result),
                [&] {
                    ValidateTransactionJournalScope(
                        config_.transaction_state_root, request.transaction_id);
                });
            ensure_active();
            NikonSdkTransport sdk;
            WpdTransport wpd;
            sdk.RequireExactlyOneD810ForProductAgent();
            wpd.RequireExactlyOneD810ForProductAgent();
            ensure_active();
            const auto sdk_cameras = sdk.Enumerate();
            ensure_active();
            const auto wpd_cameras = wpd.Enumerate();
            ensure_active();
            const auto [sdk_map, wpd_map] =
                LoadProductSingleIdentityMaps(config_, sdk_cameras);
            ensure_active();
            EvidenceWriter evidence(
                config_.artifacts_root,
                run_id,
                sdk.SdkVersion() + "+" + wpd.SdkVersion());
            if (sdk_cameras.size() == 1) {
                evidence.RecordCamera(request.camera_alias, sdk_cameras.front().firmware);
            }
            ensure_active();
            ValidateTransactionJournalScope(
                config_.transaction_state_root, request.transaction_id);

            const auto binding = ResolveExactlyOneBoundCamera(
                sdk_cameras,
                wpd_cameras,
                sdk_map,
                wpd_map,
                request.camera_alias);
            if (!binding.ready) {
                result = ExecuteBoundSingleCapture(
                    request,
                    sdk_cameras,
                    wpd_cameras,
                    sdk_map,
                    wpd_map,
                    wpd,
                    wpd,
                    sdk,
                    sdk,
                    evidence,
                    SdkCameraStatus{},
                    [] { return SdkCameraStatus{}; },
                    config_.timeouts,
                    transaction_deadline);
            } else if (!approved_profile_) {
                result.succeeded = false;
                result.terminal_state = "Blocked";
                result.error_category = "capture_profile_not_approved";
                result.error_detail =
                    "no human-approved Single capture profile is configured; shutter was not sent";
                evidence.RecordState(
                    "preflight", "HardwareAgentCaptureProfileNotApproved",
                    request.camera_alias, result.error_detail);
            } else if (approved_profile_->selected_alias != request.camera_alias) {
                result.succeeded = false;
                result.terminal_state = "Blocked";
                result.error_category = "capture_profile_alias_mismatch";
                result.error_detail =
                    "approved Single profile does not apply to the requested camera alias; shutter was not sent";
                evidence.RecordState(
                    "preflight", "HardwareAgentCaptureProfileAliasMismatch",
                    request.camera_alias, result.error_detail);
            } else {
                evidence.RecordState(
                    "preflight", "HardwareAgentCaptureProfileFrozen",
                    request.camera_alias,
                    approved_profile_->profile_id + ":v" +
                        std::to_string(approved_profile_->profile_version) + ":" +
                        approved_profile_->sha256);
                ensure_active();
                const auto sdk_status = sdk.ProbeSdkStatus(
                    binding.sdk_camera->stable_identity, config_.timeouts.open);
                ensure_active();
                const auto observed = ToObservedSettings(sdk_status);
                SdkCameraStatus pre_wpd_sdk_status = sdk_status;
                if (!request.live_view_handoff_requested &&
                    !LiveViewIsConfirmedOff(sdk_status)) {
                    SetLiveViewPreflightFailure(
                        sdk_status,
                        result,
                        "capture did not request a Live View handoff, so no automatic stop was attempted");
                    evidence.RecordState(
                        "preflight", "HardwareAgentLiveViewNotConfirmedOff",
                        request.camera_alias, result.error_detail);
                } else if (!MatchesApprovedProfile(observed, *approved_profile_)) {
                    result.succeeded = false;
                    result.terminal_state = "Blocked";
                    result.error_category = "capture_settings_mismatch";
                    result.error_detail =
                        "read-only observed settings do not match the approved Single profile; shutter was not sent";
                    evidence.RecordState(
                        "preflight", "HardwareAgentCaptureSettingsMismatch",
                        request.camera_alias, result.error_detail);
                } else {
                    bool handoff_ready = true;
                    if (request.live_view_handoff_requested) {
                        ensure_active();
                        (void)AcquireLiveViewFrames(
                            sdk,
                            binding.sdk_camera->stable_identity,
                            1,
                            0);
                        live_view_stopped_before_capture = true;
                        live_view_sdk_closed_before_capture = true;
                        ensure_active();
                        const auto immediate_status = sdk.ProbeSdkStatus(
                            binding.sdk_camera->stable_identity,
                            config_.timeouts.open);
                        ensure_active();
                        pre_wpd_sdk_status = immediate_status;
                        if (!LiveViewIsConfirmedOff(immediate_status)) {
                            handoff_ready = false;
                            SetLiveViewPreflightFailure(
                                immediate_status,
                                result,
                                "the requested finite handoff did not leave Live View confirmed OFF");
                            evidence.RecordState(
                                "preflight", "HardwareAgentLiveViewHandoffNotConfirmedOff",
                                request.camera_alias, result.error_detail);
                        } else if (!MatchesApprovedProfile(
                                ToObservedSettings(immediate_status), *approved_profile_)) {
                            handoff_ready = false;
                            result.succeeded = false;
                            result.terminal_state = "Blocked";
                            result.error_category = "capture_settings_mismatch";
                            result.error_detail =
                                "settings changed during Live View handoff; shutter was not sent";
                            evidence.RecordState(
                                "preflight", "HardwareAgentCaptureSettingsChangedDuringHandoff",
                                request.camera_alias, result.error_detail);
                        }
                    }
                    if (handoff_ready) {
                        result = ExecuteBoundSingleCapture(
                            request,
                            sdk_cameras,
                            wpd_cameras,
                            sdk_map,
                            wpd_map,
                            wpd,
                            wpd,
                            sdk,
                            sdk,
                            evidence,
                            pre_wpd_sdk_status,
                            [&] {
                                return sdk.ProbeOpenCaptureSessionStatus(
                                    config_.timeouts.open);
                            },
                            config_.timeouts,
                            transaction_deadline,
                            [&] {
                                ValidateTransactionJournalScope(
                                    config_.transaction_state_root, request.transaction_id);
                            },
                            [&](const SdkCameraStatus& shutter_status) {
                                if (!ProfileIsCurrentlyValid(*approved_profile_)) {
                                    throw TransportError(
                                        "capture_profile_expired",
                                        "approved Single profile expired immediately before shutter; capture was blocked");
                                }
                                if (!MatchesApprovedProfile(
                                        ToObservedSettings(shutter_status), *approved_profile_)) {
                                    throw TransportError(
                                        "capture_settings_mismatch",
                                        "settings changed immediately before shutter in the open capture session");
                                }
                                evidence.RecordState(
                                    "preflight",
                                    "HardwareAgentCaptureProfileRevalidatedInShutterSession",
                                    request.camera_alias,
                                    approved_profile_->profile_id + ":v" +
                                        std::to_string(approved_profile_->profile_version) + ":" +
                                        approved_profile_->sha256);
                                // This is deliberately the last side-effect-free
                                // gate before ExecuteHybridCaptureOnce issues the
                                // shutter command. The earlier check prevents a
                                // known-expired profile from starting the probe;
                                // this second check closes expiry during a slow
                                // read-only SDK settings probe/evidence write.
                                if (!ProfileIsCurrentlyValid(*approved_profile_)) {
                                    throw TransportError(
                                        "capture_profile_expired",
                                        "approved Single profile expired during final shutter-session validation; capture was blocked");
                                }
                            });
                        if (result.succeeded && request.live_view_handoff_requested) {
                            live_view_resume_attempted = true;
                            ensure_active();
                            const auto resumed = AcquireLiveViewFrames(
                                sdk,
                                binding.sdk_camera->stable_identity,
                                1,
                                0);
                            ensure_active();
                            resumed_preview = PersistPreviewJpeg(
                                config_.artifacts_root,
                                run_id,
                                request.camera_alias,
                                resumed.last_frame);
                            live_view_resumed = true;
                        }
                    }
                }
            }
            result.transaction_id = request.transaction_id;
            (void)PersistHybridCaptureSummary(
                config_.artifacts_root,
                run_id,
                request.camera_alias,
                ToHybridSummary(request, result));
            try {
                (void)StrictFixedLocalPath(
                    config_.reports_root, "report root");
                evidence.GenerateRedactedReport(config_.reports_root);
            } catch (const std::exception&) {
                result.succeeded = false;
                result.terminal_state = "FailedPartial";
                result.error_category = "evidence_report_failed";
                result.error_detail =
                    "capture evidence could not be exported; retained PC original remains unchanged";
            }
            if (result.succeeded && std::chrono::steady_clock::now() >= transaction_deadline) {
                result.succeeded = false;
                result.terminal_state = "FailedPartial";
                result.error_category = "transaction_watchdog";
                result.error_detail =
                    "single-camera transaction exceeded 180 seconds before durable terminal commit; retained PC original remains unchanged";
            }
        } catch (const TransportError& error) {
            result.succeeded = false;
            result.terminal_state = error.Category() == "camera_control_busy"
                ? "Blocked"
                : "FailedPartial";
            result.error_category = error.Category();
            result.error_detail = SafeErrorDetail(error.what());
        } catch (const std::exception&) {
            result.succeeded = false;
            result.terminal_state = "FailedPartial";
            result.error_category = "capture_exception";
            result.error_detail =
                "single-camera capture failed closed; no automatic retry is permitted";
        }
        result.transaction_id = request.transaction_id;
        result.run_id = run_id;
        result.capture_profile_id = approved_profile_ ? approved_profile_->profile_id : "";
        result.capture_profile_version =
            approved_profile_ ? approved_profile_->profile_version : 0;
        result.capture_profile_sha256 = approved_profile_ ? approved_profile_->sha256 : "";
        result.capture_profile_camera_alias =
            approved_profile_ ? approved_profile_->selected_alias : "";
        result.profile_expires_at_utc =
            approved_profile_ ? approved_profile_->expires_at_utc : "";
        result.live_view_handoff_requested = request.live_view_handoff_requested;
        result.live_view_stopped_before_capture = live_view_stopped_before_capture;
        result.live_view_sdk_session_closed_before_capture =
            live_view_sdk_closed_before_capture;
        result.live_view_resume_attempted = live_view_resume_attempted;
        result.live_view_resumed = live_view_resumed;
        result.resumed_preview = resumed_preview;
        if (!result.retained_original) {
            try {
                result.retained_original =
                    RecoverRetainedOriginal(config_.artifacts_root, result);
            } catch (const std::exception&) {
                // The original remains unclaimed unless its exact canonical
                // location, JPEG bytes, size, and SHA-256 can all be rebuilt.
                result.retained_original.reset();
            }
        }
        AtomicReplaceText(
            TransactionJournalPath(config_.transaction_state_root, request.transaction_id),
            SerializeTransactionJournal(result),
            [&] {
                ValidateTransactionJournalScope(
                    config_.transaction_state_root, request.transaction_id);
            });
        // The camera-control lease intentionally remains held through the
        // atomic terminal journal replacement. A concurrent result query can
        // therefore never downgrade an active capture to process_interrupted
        // between camera cleanup and the durable terminal commit.
        lease.reset();
        transaction_lease.reset();
        return result;
    }

    SingleCameraLiveViewProbeResult ProbeLiveView(const HardwareCameraAgentRequest& request) {
        SingleCameraLiveViewProbeResult result;
        result.camera_alias = request.camera_alias;
        result.run_id = NewRunId();
        result.preview_is_original = false;
        result.preview_is_stitch_input = false;
        try {
            ValidateProductSingleIdentityConfiguration(config_);
            (void)PrepareExclusiveArtifactRun(config_.artifacts_root, result.run_id);
            HardwareProcessLease lease;
            NikonSdkTransport sdk;
            WpdTransport wpd;
            sdk.RequireExactlyOneD810ForProductAgent();
            wpd.RequireExactlyOneD810ForProductAgent();
            const auto sdk_cameras = sdk.Enumerate();
            const auto wpd_cameras = wpd.Enumerate();
            const auto [sdk_map, wpd_map] =
                LoadProductSingleIdentityMaps(config_, sdk_cameras);
            const auto binding = ResolveExactlyOneBoundCamera(
                sdk_cameras, wpd_cameras, sdk_map, wpd_map, request.camera_alias);
            if (!binding.ready) {
                result.error_category = binding.failure_category;
                result.error_detail = binding.failure_detail;
                return result;
            }
            const auto probe = AcquireLiveViewFrames(
                sdk,
                binding.sdk_camera->stable_identity,
                request.live_view_frames,
                request.live_view_interval_ms);
            result.frames = probe.frames;
            result.last_frame_bytes = probe.last_frame.size();
            result.last_frame_sha256 = Sha256Hex(probe.last_frame);
            result.duration_ms = probe.duration.count();
            result.live_view_stopped = true;
            result.sdk_session_closed = true;
            result.preview = PersistPreviewJpeg(
                config_.artifacts_root,
                result.run_id,
                request.camera_alias,
                probe.last_frame);
            result.preview_persisted = true;
            result.succeeded = true;
        } catch (const TransportError& error) {
            result.succeeded = false;
            result.error_category = error.Category();
            result.error_detail = SafeErrorDetail(error.what());
        } catch (const std::exception&) {
            result.succeeded = false;
            result.error_category = "live_view_exception";
            result.error_detail = "finite Live View probe failed closed";
        }
        return result;
    }

    ContinuousLiveViewResult StartContinuousLiveView(
        const HardwareCameraAgentRequest& request) {
        const std::scoped_lock lock(continuous_live_view_mutex_);
        ContinuousLiveViewResult result;
        result.camera_alias = request.camera_alias;
        result.session_id = request.session_id;
        if (live_view_sdk_ || residual_live_view_unsafe_) {
            result.error_category = "live_view_session_active";
            result.error_detail = "another continuous Live View session is already active";
            return result;
        }
        try {
            live_view_lease_ = std::make_unique<HardwareProcessLease>();
            live_view_sdk_ = config_.continuous_live_view_sdk_factory_for_testing
                ? config_.continuous_live_view_sdk_factory_for_testing()
                : std::make_unique<NikonContinuousLiveViewSdkTransport>();
            if (!live_view_sdk_) {
                throw std::runtime_error(
                    "continuous Live View SDK factory returned no transport");
            }
            const auto sdk_cameras = live_view_sdk_->Enumerate();
            std::string sdk_identity;
            if (config_.continuous_live_view_identity_resolver_for_testing) {
                sdk_identity =
                    config_.continuous_live_view_identity_resolver_for_testing(
                        request.camera_alias, sdk_cameras);
                if (sdk_identity.empty()) {
                    throw TransportError(
                        "single_identity_not_ready",
                        "the injected continuous Live View identity was empty");
                }
            } else {
                ValidateProductSingleIdentityConfiguration(config_);
                WpdTransport wpd;
                wpd.RequireExactlyOneD810ForProductAgent();
                const auto wpd_cameras = wpd.Enumerate();
                const auto [sdk_map, wpd_map] =
                    LoadProductSingleIdentityMaps(config_, sdk_cameras);
                const auto binding = ResolveExactlyOneBoundCamera(
                    sdk_cameras,
                    wpd_cameras,
                    sdk_map,
                    wpd_map,
                    request.camera_alias);
                if (!binding.ready) {
                    throw TransportError(
                        binding.failure_category.empty()
                            ? "single_identity_not_ready"
                            : binding.failure_category,
                        "CAM-A identity-v3 was not ready for continuous Live View");
                }
                sdk_identity = binding.sdk_camera->stable_identity;
            }
            const auto status = live_view_sdk_->ProbeSdkStatus(
                sdk_identity, config_.timeouts.open);
            if (!LiveViewIsConfirmedOff(status)) {
                throw TransportError(
                    status.live_view_status_available
                        ? "live_view_not_off" : "live_view_status_unavailable",
                    "continuous Live View requires a confirmed OFF baseline");
            }
            live_view_sdk_->OpenLiveView(
                sdk_identity, config_.timeouts.open);
            live_view_sdk_->StartLiveView(config_.timeouts.open);
            live_view_session_id_ = request.session_id;
            live_view_camera_alias_ = request.camera_alias;
            live_view_frame_number_ = 0;
            live_view_started_at_ = ContinuousLiveViewNow();
            live_view_last_activity_ = live_view_started_at_;
            result.succeeded = true;
            result.state = "Started";
            result.sdk_session_open = true;
            result.live_view_running = true;
        } catch (const TransportError& error) {
            StopContinuousLiveViewNoThrow();
            result.error_category = error.Category();
            result.error_detail = SafeErrorDetail(error.what());
        } catch (const std::exception&) {
            StopContinuousLiveViewNoThrow();
            result.error_category = "continuous_live_view_start_failed";
            result.error_detail = "continuous Live View failed closed before the first frame";
        }
        return result;
    }

    ContinuousLiveViewResult ReadContinuousLiveViewFrame(
        const HardwareCameraAgentRequest& request) {
        const std::scoped_lock lock(continuous_live_view_mutex_);
        ContinuousLiveViewResult result = ContinuousResultFor(request, "Frame");
        if (!ValidateContinuousSession(request, result)) return result;
        try {
            const auto frame =
                live_view_sdk_->ReadLiveViewFrame(config_.timeouts.live_view_frame);
            if (!IsValidJpeg(frame) || frame.size() > 512U * 1024U) {
                throw std::runtime_error("Live View frame is not a bounded JPEG");
            }
            live_view_last_activity_ = ContinuousLiveViewNow();
            result.succeeded = true;
            result.frame_number = ++live_view_frame_number_;
            result.frame_size = frame.size();
            result.frame_sha256 = Sha256Hex(frame);
            result.frame_jpeg_base64 = Base64Encode(frame);
            result.sdk_session_open = true;
            result.live_view_running = true;
        } catch (const std::exception&) {
            StopContinuousLiveViewNoThrow();
            result.error_category = "continuous_live_view_frame_failed";
            result.error_detail =
                "continuous Live View frame acquisition failed and the SDK session was closed";
        }
        return result;
    }

    ContinuousLiveViewResult HeartbeatContinuousLiveView(
        const HardwareCameraAgentRequest& request) {
        const std::scoped_lock lock(continuous_live_view_mutex_);
        ContinuousLiveViewResult result = ContinuousResultFor(request, "Heartbeat");
        if (!ValidateContinuousSession(request, result)) return result;
        live_view_last_activity_ = ContinuousLiveViewNow();
        result.succeeded = true;
        result.frame_number = live_view_frame_number_;
        result.sdk_session_open = true;
        result.live_view_running = true;
        return result;
    }

    ContinuousLiveViewResult StopContinuousLiveView(
        const HardwareCameraAgentRequest& request) {
        const std::scoped_lock lock(continuous_live_view_mutex_);
        ContinuousLiveViewResult result = ContinuousResultFor(request, "Stopped");
        if (!ValidateContinuousSession(request, result)) return result;
        if (TryStopAndCloseContinuousLiveView(config_.timeouts.close)) {
            result.succeeded = true;
            result.frame_number = live_view_frame_number_;
        } else {
            result.error_category = "continuous_live_view_stop_failed";
            result.error_detail = "Live View stop or SDK close failed; the session is unusable";
        }
        return result;
    }

    ContinuousLiveViewResult CloseAgentSession(
        const HardwareCameraAgentRequest& request) {
        const std::scoped_lock lock(continuous_live_view_mutex_);
        ContinuousLiveViewResult result = ContinuousResultFor(request, "Closed");
        if (live_view_sdk_ && request.session_id != live_view_session_id_) {
            result.error_category = "live_view_session_mismatch";
            result.error_detail = "the close request does not own the active Live View session";
            return result;
        }
        if (!live_view_session_id_.empty() && request.session_id != live_view_session_id_) {
            result.error_category = "live_view_session_mismatch";
            result.error_detail = "the close request does not own this agent session";
            return result;
        }
        if (TryStopAndCloseContinuousLiveView(config_.timeouts.close)) {
            result.succeeded = true;
            result.frame_number = live_view_frame_number_;
        } else {
            result.error_category = "continuous_live_view_close_failed";
            result.error_detail =
                "Live View stop or SDK close failed; capture remains blocked";
        }
        return result;
    }

    void OnAgentIdle() noexcept {
        try {
            const std::scoped_lock lock(continuous_live_view_mutex_);
            if (live_view_sdk_ &&
                (residual_live_view_unsafe_ || ContinuousSessionExpired())) {
                StopContinuousLiveViewNoThrow();
            }
        } catch (...) {
        }
    }

    SingleCameraCaptureResult GetTransactionResult(std::string_view transaction_id) {
        SingleCameraCaptureResult result;
        result.transaction_id = std::string(transaction_id);
        result.terminal_state = "Blocked";
        const fs::path journal_path =
            TransactionJournalPath(config_.transaction_state_root, transaction_id);
        const fs::path reservation_directory = journal_path.parent_path();
        std::error_code directory_exists_error;
        const bool reservation_exists = fs::exists(reservation_directory, directory_exists_error);
        if (directory_exists_error) {
            result.terminal_state = "FailedPartial";
            result.error_category = "transaction_journal_invalid";
            result.error_detail = "durable Camera Agent transaction scope could not be inspected";
            return result;
        }
        if (reservation_exists) {
            try {
                ValidateTransactionJournalScope(config_.transaction_state_root, transaction_id);
            } catch (const std::exception&) {
                result.terminal_state = "FailedPartial";
                result.error_category = "transaction_journal_invalid";
                result.error_detail =
                    "durable Camera Agent transaction scope is not reparse-free";
                return result;
            }
        } else {
            const fs::path transaction_root =
                fs::absolute(config_.transaction_state_root).lexically_normal();
            if (fs::exists(transaction_root) &&
                PathChainHasReparsePoint(transaction_root.root_path(), transaction_root)) {
                result.terminal_state = "FailedPartial";
                result.error_category = "transaction_journal_invalid";
                result.error_detail = "durable Camera Agent transaction root is not reparse-free";
                return result;
            }
        }
        std::unique_ptr<HardwareProcessLease> pre_acquired_transaction_lease;
        std::error_code exists_error;
        bool journal_is_regular =
            fs::is_regular_file(journal_path, exists_error);
        if (IsMissingFilesystemError(exists_error)) {
            exists_error.clear();
            journal_is_regular = false;
        }
        if (exists_error) {
            result.terminal_state = "FailedPartial";
            result.error_category = "transaction_journal_invalid";
            result.error_detail =
                "durable Camera Agent transaction journal could not be inspected";
            return result;
        }
        if (!journal_is_regular) {
            std::error_code directory_error;
            bool directory_is_valid =
                fs::is_directory(reservation_directory, directory_error);
            if (IsMissingFilesystemError(directory_error)) {
                directory_error.clear();
                directory_is_valid = false;
            }
            directory_is_valid = directory_is_valid && !directory_error &&
                !IsReparsePoint(reservation_directory);
            if (!directory_is_valid) {
                if (directory_error || IsReparsePoint(reservation_directory)) {
                    result.terminal_state = "FailedPartial";
                    result.error_category = "transaction_journal_invalid";
                    result.error_detail =
                        "durable Camera Agent transaction reservation is invalid";
                } else {
                    result.error_category = "transaction_not_found";
                    result.error_detail =
                        "no durable Camera Agent transaction exists for transactionId";
                }
                return result;
            }

            try {
                pre_acquired_transaction_lease =
                    std::make_unique<HardwareProcessLease>(
                        TransactionLeaseName(transaction_id));
            } catch (const TransportError& error) {
                if (error.Category() != "camera_control_busy") throw;
                result.succeeded = false;
                result.terminal_state = "Reserved";
                result.error_category = "transaction_reserved";
                result.error_detail =
                    "transaction owner is committing its initial durable reservation; query again without resubmitting capture";
                return result;
            }

            exists_error.clear();
            journal_is_regular =
                fs::is_regular_file(journal_path, exists_error);
            if (IsMissingFilesystemError(exists_error)) {
                exists_error.clear();
                journal_is_regular = false;
            }
            if (exists_error) {
                result.terminal_state = "FailedPartial";
                result.error_category = "transaction_journal_invalid";
                result.error_detail =
                    "durable Camera Agent transaction journal could not be reinspected";
                return result;
            }
            if (!journal_is_regular) {
                result.terminal_state = "FailedPartial";
                result.error_category = "transaction_reservation_incomplete";
                result.error_detail =
                    "transactionId was reserved but its owner ended before the initial journal commit; no capture retry is permitted";
                return result;
            }
        }
        try {
            result = ParseTransactionJournal(journal_path, transaction_id);
        } catch (const std::exception&) {
            result.transaction_id = std::string(transaction_id);
            result.terminal_state = "FailedPartial";
            result.error_category = "transaction_journal_invalid";
            result.error_detail = "durable Camera Agent transaction state is invalid";
            return result;
        }
        if (result.retained_original && !VerifyJournalOriginal(config_.artifacts_root, result)) {
            result.succeeded = false;
            result.terminal_state = "FailedPartial";
            result.retained_original.reset();
            result.error_category = "transaction_original_invalid";
            result.error_detail = "durable retained-original metadata failed path or content verification";
            return result;
        }
        if (result.resumed_preview &&
            !VerifyJournalResumedPreview(config_.artifacts_root, result)) {
            result.succeeded = false;
            result.terminal_state = "FailedPartial";
            result.live_view_resumed = false;
            result.resumed_preview.reset();
            result.error_category = "transaction_preview_invalid";
            result.error_detail =
                "durable resumed-preview metadata failed path or content verification";
            return result;
        }
        if (result.terminal_state != "Reserved" && result.terminal_state != "InProgress") {
            if (result.terminal_state == "FailedPartial" && !result.retained_original) {
                try {
                    result.retained_original =
                        RecoverRetainedOriginal(config_.artifacts_root, result);
                } catch (const std::exception&) {
                    result.retained_original.reset();
                }
            }
            return result;
        }
        if (config_.after_initial_active_journal_read_for_testing) {
            config_.after_initial_active_journal_read_for_testing();
        }

        try {
            HardwareProcessLease transaction_lease(TransactionLeaseName(transaction_id));
            // The initial active-state read may be stale: capture commits its
            // terminal journal before releasing this transaction lease.
            try {
                result = ParseTransactionJournal(journal_path, transaction_id);
            } catch (const std::exception&) {
                result.transaction_id = std::string(transaction_id);
                result.terminal_state = "FailedPartial";
                result.error_category = "transaction_journal_invalid";
                result.error_detail = "durable Camera Agent transaction state is invalid";
                return result;
            }
            if (result.retained_original && !VerifyJournalOriginal(config_.artifacts_root, result)) {
                result.succeeded = false;
                result.terminal_state = "FailedPartial";
                result.retained_original.reset();
                result.error_category = "transaction_original_invalid";
                result.error_detail =
                    "durable retained-original metadata failed path or content verification";
                return result;
            }
            if (result.resumed_preview &&
                !VerifyJournalResumedPreview(config_.artifacts_root, result)) {
                result.succeeded = false;
                result.terminal_state = "FailedPartial";
                result.live_view_resumed = false;
                result.resumed_preview.reset();
                result.error_category = "transaction_preview_invalid";
                result.error_detail =
                    "durable resumed-preview metadata failed path or content verification";
                return result;
            }
            if (result.terminal_state != "Reserved" && result.terminal_state != "InProgress") {
                if (result.terminal_state == "FailedPartial" && !result.retained_original) {
                    try {
                        result.retained_original =
                            RecoverRetainedOriginal(config_.artifacts_root, result);
                    } catch (const std::exception&) {
                        result.retained_original.reset();
                    }
                }
                return result;
            }
            const bool was_reserved = result.terminal_state == "Reserved";
            result.succeeded = false;
            result.terminal_state = "FailedPartial";
            if (was_reserved) {
                result.error_category = "transaction_abandoned_before_camera";
                result.error_detail =
                    "Camera Agent ended after reservation but before camera access; capture will not be retried";
            } else {
                try {
                    result.retained_original =
                        RecoverRetainedOriginal(config_.artifacts_root, result);
                } catch (const std::exception&) {
                    result.retained_original.reset();
                }
                result.error_category = "process_interrupted";
                result.error_detail = result.retained_original
                    ? "Camera Agent ended before terminal journal commit; one verified PC original was retained and capture will not be retried"
                    : "Camera Agent ended before terminal journal commit; capture will not be retried";
            }
            AtomicReplaceText(
                journal_path,
                SerializeTransactionJournal(result),
                [&] {
                    ValidateTransactionJournalScope(
                        config_.transaction_state_root, transaction_id);
                });
        } catch (const TransportError& error) {
            if (error.Category() != "camera_control_busy") throw;
            result.succeeded = false;
            result.error_category = result.terminal_state == "Reserved"
                ? "transaction_reserved"
                : "transaction_in_progress";
            result.error_detail =
                "transaction owner is active; query again without resubmitting capture";
        }
        return result;
    }

private:
    ContinuousLiveViewResult ContinuousResultFor(
        const HardwareCameraAgentRequest& request,
        std::string state) const {
        ContinuousLiveViewResult result;
        result.camera_alias = live_view_camera_alias_.empty()
            ? "CAM-A" : live_view_camera_alias_;
        result.session_id = request.session_id;
        result.state = std::move(state);
        return result;
    }

    std::chrono::steady_clock::time_point ContinuousLiveViewNow() const {
        return config_.continuous_live_view_clock_for_testing
            ? config_.continuous_live_view_clock_for_testing()
            : std::chrono::steady_clock::now();
    }

    bool ContinuousSessionExpired() const {
        if (!live_view_sdk_) return false;
        const auto now = ContinuousLiveViewNow();
        return now - live_view_started_at_ >= std::chrono::seconds(600) ||
            now - live_view_last_activity_ >= std::chrono::seconds(20);
    }

    bool ValidateContinuousSession(
        const HardwareCameraAgentRequest& request,
        ContinuousLiveViewResult& result) {
        if (!live_view_sdk_ || request.session_id != live_view_session_id_) {
            result.error_category = "live_view_session_not_found";
            result.error_detail = "the requested continuous Live View session is not active";
            return false;
        }
        if (ContinuousSessionExpired()) {
            StopContinuousLiveViewNoThrow();
            result.error_category = "live_view_session_expired";
            result.error_detail = "the continuous Live View heartbeat or maximum lifetime expired";
            return false;
        }
        return true;
    }

    bool TryStopAndCloseContinuousLiveView(
        std::chrono::seconds timeout) noexcept {
        if (!live_view_sdk_) {
            // GitHub Issue #97: SDK 生成失敗(factory が nullptr / コンストラクタ throw)時、
            // 直前の :StartContinuousLiveView で取得済みの lease が解放されず、カメラ制御
            // mutex を次の Start まで(最大プロセス寿命まで)保持して他 Phase0 プロセスを
            // camera_control_busy にしていた。lease は SDK 生成の直前でしか取得しないため、
            // sdk が無いのに lease が残っているのは「失敗した Start」だけであり、ここで
            // 解放するのは安全(保持していなければ no-op)。
            live_view_lease_.reset();
            return !residual_live_view_unsafe_;
        }
        bool stopped = true;
        bool closed = true;
        try {
            live_view_sdk_->StopLiveView(timeout);
        } catch (...) {
            stopped = false;
        }
        try {
            live_view_sdk_->Close(timeout);
        } catch (...) {
            closed = false;
        }
        if (stopped && closed) {
            live_view_sdk_.reset();
            live_view_lease_.reset();
            residual_live_view_unsafe_ = false;
            return true;
        }
        residual_live_view_unsafe_ = true;
        return false;
    }

    void StopContinuousLiveViewNoThrow() noexcept {
        (void)TryStopAndCloseContinuousLiveView(std::chrono::seconds(3));
    }

    ProductionHardwareCameraAgentConfig config_;
    std::optional<ApprovedCaptureProfile> approved_profile_;
    std::unique_ptr<HardwareProcessLease> live_view_lease_;
    std::unique_ptr<IContinuousLiveViewSdkTransport> live_view_sdk_;
    mutable std::mutex continuous_live_view_mutex_;
    bool residual_live_view_unsafe_{};
    std::string live_view_session_id_;
    std::string live_view_camera_alias_;
    std::uint64_t live_view_frame_number_{};
    std::chrono::steady_clock::time_point live_view_started_at_{};
    std::chrono::steady_clock::time_point live_view_last_activity_{};
};

ContinuousLiveViewResult IHardwareCameraAgentBackend::StartContinuousLiveView(
    const HardwareCameraAgentRequest&) {
    throw std::runtime_error("continuous Live View backend is not implemented");
}

ContinuousLiveViewResult IHardwareCameraAgentBackend::ReadContinuousLiveViewFrame(
    const HardwareCameraAgentRequest&) {
    throw std::runtime_error("continuous Live View backend is not implemented");
}

ContinuousLiveViewResult IHardwareCameraAgentBackend::HeartbeatContinuousLiveView(
    const HardwareCameraAgentRequest&) {
    throw std::runtime_error("continuous Live View backend is not implemented");
}

ContinuousLiveViewResult IHardwareCameraAgentBackend::StopContinuousLiveView(
    const HardwareCameraAgentRequest&) {
    throw std::runtime_error("continuous Live View backend is not implemented");
}

ContinuousLiveViewResult IHardwareCameraAgentBackend::CloseAgentSession(
    const HardwareCameraAgentRequest&) {
    throw std::runtime_error("continuous Live View backend is not implemented");
}

void IHardwareCameraAgentBackend::OnAgentIdle() noexcept {}

ProductionHardwareCameraAgentBackend::ProductionHardwareCameraAgentBackend(
    ProductionHardwareCameraAgentConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

ProductionHardwareCameraAgentBackend::~ProductionHardwareCameraAgentBackend() = default;

SingleCameraReadinessResult ProductionHardwareCameraAgentBackend::GetSingleReadiness(
    std::string_view camera_alias) {
    return impl_->GetSingleReadiness(camera_alias);
}

SingleCameraCaptureResult ProductionHardwareCameraAgentBackend::CaptureSingle(
    const HardwareCameraAgentRequest& request) {
    return impl_->CaptureSingle(request);
}

SingleCameraLiveViewProbeResult ProductionHardwareCameraAgentBackend::ProbeLiveView(
    const HardwareCameraAgentRequest& request) {
    return impl_->ProbeLiveView(request);
}

SingleCameraCaptureResult ProductionHardwareCameraAgentBackend::GetTransactionResult(
    std::string_view transaction_id) {
    return impl_->GetTransactionResult(transaction_id);
}

ContinuousLiveViewResult ProductionHardwareCameraAgentBackend::StartContinuousLiveView(
    const HardwareCameraAgentRequest& request) {
    return impl_->StartContinuousLiveView(request);
}

ContinuousLiveViewResult ProductionHardwareCameraAgentBackend::ReadContinuousLiveViewFrame(
    const HardwareCameraAgentRequest& request) {
    return impl_->ReadContinuousLiveViewFrame(request);
}

ContinuousLiveViewResult ProductionHardwareCameraAgentBackend::HeartbeatContinuousLiveView(
    const HardwareCameraAgentRequest& request) {
    return impl_->HeartbeatContinuousLiveView(request);
}

ContinuousLiveViewResult ProductionHardwareCameraAgentBackend::StopContinuousLiveView(
    const HardwareCameraAgentRequest& request) {
    return impl_->StopContinuousLiveView(request);
}

ContinuousLiveViewResult ProductionHardwareCameraAgentBackend::CloseAgentSession(
    const HardwareCameraAgentRequest& request) {
    return impl_->CloseAgentSession(request);
}

void ProductionHardwareCameraAgentBackend::OnAgentIdle() noexcept {
    impl_->OnAgentIdle();
}

HardwareCameraAgentDispatcher::HardwareCameraAgentDispatcher(IHardwareCameraAgentBackend& backend)
    : backend_(backend) {}

std::string HardwareCameraAgentDispatcher::Handle(std::string_view request_json) noexcept {
    const std::string extracted_request_id = TryExtractRequestId(request_json);
    const std::string response_schema = TryParseResponseSchemaVersion(request_json)
        .value_or(std::string(kHardwareCameraAgentSchemaVersion));
    try {
        const HardwareCameraAgentRequest request = ParseHardwareCameraAgentRequest(request_json);
        HardwareCameraAgentResponse response;
        response.schema_version = request.schema_version;
        response.request_id = request.request_id;
        if (request.operation == HardwareCameraAgentOperation::get_single_readiness) {
            response.readiness = backend_.GetSingleReadiness(request.camera_alias);
            if (!IsReadinessResultStructurallyValid(*response.readiness, request.camera_alias)) {
                return SerializeHardwareCameraAgentResponse(AgentFailure(
                    request.request_id,
                    "InvalidBackendResult",
                    "readiness backend returned an inconsistent hardware result"));
            }
            response.success = true;
            response.result_code = response.readiness->ready ? "SingleReady" : "SingleNotReady";
        } else if (request.operation == HardwareCameraAgentOperation::capture_single) {
            if (!request.exclusive_camera_control_confirmed ||
                !request.dedicated_spool_scope_confirmed ||
                !request.exact_object_delete_confirmed) {
                return SerializeHardwareCameraAgentResponse(ProtocolRejection(
                    request.request_id,
                    "SafetyConfirmationRequired",
                    "capture-single requires every hardware safety confirmation"));
            }
            response.capture = backend_.CaptureSingle(request);
            if (!IsCaptureResultStructurallyValid(
                    *response.capture, request.transaction_id, false, &request)) {
                return SerializeHardwareCameraAgentResponse(AgentFailure(
                    request.request_id,
                    "InvalidBackendResult",
                    "capture backend returned an inconsistent hardware result"));
            }
            response.success = response.capture->succeeded;
            response.result_code = response.capture->succeeded
                ? "CaptureComplete"
                : (response.capture->error_category.empty() ? "CaptureFailed" : response.capture->error_category);
        } else if (request.operation == HardwareCameraAgentOperation::live_view_probe) {
            response.live_view = backend_.ProbeLiveView(request);
            if (!IsLiveViewResultStructurallyValid(*response.live_view, request)) {
                return SerializeHardwareCameraAgentResponse(AgentFailure(
                    request.request_id,
                    "InvalidBackendResult",
                    "Live View backend returned an inconsistent hardware result"));
            }
            response.success = response.live_view->succeeded;
            response.result_code = response.live_view->succeeded
                ? "LiveViewProbeComplete"
                : (response.live_view->error_category.empty() ? "LiveViewProbeFailed" : response.live_view->error_category);
        } else if (request.operation == HardwareCameraAgentOperation::get_transaction_result) {
            response.capture = backend_.GetTransactionResult(request.transaction_id);
            if (!IsCaptureResultStructurallyValid(
                    *response.capture, request.transaction_id, true)) {
                return SerializeHardwareCameraAgentResponse(AgentFailure(
                    request.request_id,
                    "InvalidBackendResult",
                    "transaction backend returned an inconsistent hardware result"));
            }
            response.success = response.capture->succeeded;
            if (response.capture->terminal_state == "Reserved") {
                response.result_code = "TransactionReserved";
            } else if (response.capture->terminal_state == "InProgress") {
                response.result_code = "TransactionInProgress";
            } else if (response.capture->error_category == "transaction_not_found") {
                response.result_code = "TransactionNotFound";
            } else if (response.capture->succeeded) {
                response.result_code = "CaptureComplete";
            } else {
                response.result_code = response.capture->error_category.empty()
                    ? "CaptureFailed"
                    : response.capture->error_category;
            }
        } else {
            if (request.operation == HardwareCameraAgentOperation::start_live_view) {
                response.continuous_live_view = backend_.StartContinuousLiveView(request);
            } else if (request.operation == HardwareCameraAgentOperation::read_live_view_frame) {
                response.continuous_live_view = backend_.ReadContinuousLiveViewFrame(request);
            } else if (request.operation == HardwareCameraAgentOperation::live_view_heartbeat) {
                response.continuous_live_view = backend_.HeartbeatContinuousLiveView(request);
            } else if (request.operation == HardwareCameraAgentOperation::stop_live_view) {
                response.continuous_live_view = backend_.StopContinuousLiveView(request);
            } else {
                response.continuous_live_view = backend_.CloseAgentSession(request);
                if (response.continuous_live_view->succeeded) should_stop_ = true;
            }
            if (!IsContinuousLiveViewResultStructurallyValid(
                    *response.continuous_live_view, request)) {
                HardwareCameraAgentResponse invalid = AgentFailure(
                    request.request_id,
                    "InvalidBackendResult",
                    "continuous Live View backend returned an inconsistent result");
                invalid.schema_version = request.schema_version;
                return SerializeHardwareCameraAgentResponse(invalid);
            }
            response.success = response.continuous_live_view->succeeded;
            response.result_code = response.continuous_live_view->succeeded
                ? response.continuous_live_view->state
                : response.continuous_live_view->error_category;
        }
        return SerializeHardwareCameraAgentResponse(response);
    } catch (const HardwareCameraAgentProtocolError& error) {
        auto response = ProtocolRejection(extracted_request_id, error.Code(), error.what());
        response.schema_version = response_schema;
        return SerializeHardwareCameraAgentResponse(response);
    } catch (const TransportError& error) {
        auto response = AgentFailure(extracted_request_id, error.Category(), error.what());
        response.schema_version = response_schema;
        return SerializeHardwareCameraAgentResponse(response);
    } catch (const std::exception&) {
        auto response = AgentFailure(
            extracted_request_id, "AgentFailure", "hardware Camera Agent operation failed closed");
        response.schema_version = response_schema;
        return SerializeHardwareCameraAgentResponse(response);
    }
}

bool HardwareCameraAgentDispatcher::ShouldStop() const noexcept {
    return should_stop_;
}

void HardwareCameraAgentDispatcher::OnIdle() noexcept {
    backend_.OnAgentIdle();
}

} // namespace a0::phase0
