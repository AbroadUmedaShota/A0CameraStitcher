#include "a0/phase0/dual_hardware_camera_agent.hpp"

#include "a0/common/protocol_json.hpp"
#include "a0/phase0/dual_hardware_camera_agent_store.hpp"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace a0::phase0 {
namespace {

// The JSON envelope handling that used to live here moved to protocol_json.hpp so
// the binding protocol (Issue #61) can share one parser. These aliases bind the
// shared templates to this protocol's error type, which is why every call site
// below still reads exactly as it did before the move.
using ::a0::common::protocol_json::JsonKind;
using ::a0::common::protocol_json::JsonValue;
using ::a0::common::protocol_json::kMaximumProtocolJsonBytes;
using ::a0::common::protocol_json::kMaximumProtocolJsonDepth;
using ::a0::common::protocol_json::JsonEscape;

[[noreturn]] void ProtocolFailure(std::string code, std::string message);

struct DualHardwareJsonFailure {
    [[noreturn]] static void Fail(std::string code, std::string message) {
        ProtocolFailure(std::move(code), std::move(message));
    }
};

using JsonParser = ::a0::common::protocol_json::BasicJsonParser<DualHardwareJsonFailure>;

[[nodiscard]] inline const JsonValue& RequireField(
    const JsonValue& object, std::string_view name, JsonKind kind) {
    return ::a0::common::protocol_json::RequireFieldWith<DualHardwareJsonFailure>(object, name, kind);
}

[[nodiscard]] inline std::string SerializeJson(const JsonValue& value) {
    return ::a0::common::protocol_json::SerializeJsonWith<DualHardwareJsonFailure>(value);
}

constexpr std::string_view kCameraMode = "DualCamera";
constexpr std::string_view kCameraAliasA = "CAM-A";
constexpr std::string_view kCameraAliasB = "CAM-B";


[[noreturn]] void ProtocolFailure(std::string code, std::string message) {
    throw DualHardwareCameraAgentProtocolError(
        std::move(code), std::move(message));
}



void RequireExactFields(
    const JsonValue& object,
    const std::set<std::string>& expected) {
    if (object.kind != JsonKind::object || object.object.size() != expected.size()) {
        ProtocolFailure(
            "UnexpectedField",
            "Dual hardware protocol object has missing or unexpected fields");
    }
    for (const auto& [name, ignored] : object.object) {
        (void)ignored;
        if (!expected.contains(name)) {
            ProtocolFailure(
                "UnexpectedField",
                "Dual hardware protocol object contains an unexpected field");
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

bool IsSafeTransactionId(std::string_view value) noexcept {
    return value.size() == 32 &&
        std::any_of(value.begin(), value.end(), [](char character) { return character != '0'; }) &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f') ||
                (character >= 'A' && character <= 'F');
        });
}

constexpr std::int64_t kHundredNanosecondsPerSecond = 10'000'000;
constexpr std::int64_t kHundredNanosecondsPerDay = 86'400 * kHundredNanosecondsPerSecond;

bool IsLeapYear(int year) noexcept { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
int DaysInMonth(int year, int month) noexcept {
    constexpr int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month - 1] + (month == 2 && IsLeapYear(year) ? 1 : 0);
}
std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    const unsigned doy = (153U * adjusted_month + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
}
int ParseFixedDigits(std::string_view value, std::size_t offset, std::size_t count) {
    int parsed = 0;
    if (offset + count > value.size()) ProtocolFailure("InvalidPairRequest", "invalid UTC timestamp");
    for (std::size_t index = offset; index < offset + count; ++index) {
        if (value[index] < '0' || value[index] > '9') ProtocolFailure("InvalidPairRequest", "invalid UTC timestamp");
        parsed = parsed * 10 + (value[index] - '0');
    }
    return parsed;
}
std::int64_t ParseUtc100ns(std::string_view value) {
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':') ProtocolFailure("InvalidPairRequest", "invalid UTC timestamp");
    const int year = ParseFixedDigits(value, 0, 4), month = ParseFixedDigits(value, 5, 2);
    const int day = ParseFixedDigits(value, 8, 2), hour = ParseFixedDigits(value, 11, 2);
    const int minute = ParseFixedDigits(value, 14, 2), second = ParseFixedDigits(value, 17, 2);
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > DaysInMonth(year, month) ||
        hour > 23 || minute > 59 || second > 59) ProtocolFailure("InvalidPairRequest", "invalid UTC timestamp");
    std::size_t position = 19;
    std::int64_t fraction = 0;
    int fraction_digits = 0;
    if (position < value.size() && value[position] == '.') {
        ++position;
        const std::size_t start = position;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
            if (++fraction_digits > 7) ProtocolFailure("InvalidPairRequest", "invalid UTC timestamp precision");
            fraction = fraction * 10 + (value[position++] - '0');
        }
        if (position == start) ProtocolFailure("InvalidPairRequest", "invalid UTC timestamp fraction");
    }
    while (fraction_digits++ < 7) fraction *= 10;
    if (position == value.size() - 1 && value[position] == 'Z') ++position;
    else if (position + 6 == value.size() && value.substr(position) == "+00:00") position += 6;
    else ProtocolFailure("InvalidPairRequest", "timestamp must have a zero UTC offset");
    return DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * kHundredNanosecondsPerDay +
        (static_cast<std::int64_t>(hour) * 3600 + minute * 60 + second) * kHundredNanosecondsPerSecond + fraction;
}
std::int64_t Clock100ns(const DualHardwareUtcClock& clock) {
    const auto value = clock ? clock() : std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count() / 100;
}
bool IsBoundedText(std::string_view value, std::size_t maximum) noexcept {
    return !value.empty() && value.size() <= maximum &&
        std::none_of(value.begin(), value.end(),
            [](unsigned char character) { return character < 0x20U || character == 0x7FU; }) &&
        std::any_of(value.begin(), value.end(),
            [](unsigned char character) { return !std::isspace(character); });
}
std::int64_t ParseInteger(const JsonValue& value) {
    if (value.kind != JsonKind::number || value.string.empty() || value.string.find_first_of(".eE") != std::string::npos)
        ProtocolFailure("InvalidPairRequest", "integer field is invalid");
    std::int64_t parsed{};
    const auto result = std::from_chars(value.string.data(), value.string.data() + value.string.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.string.data() + value.string.size())
        ProtocolFailure("InvalidPairRequest", "integer field is outside the supported range");
    return parsed;
}
double ParseFiniteNumber(const JsonValue& value) {
    if (value.kind != JsonKind::number || value.string.empty()) ProtocolFailure("InvalidPairRequest", "numeric field is invalid");
    double parsed{};
    const auto result = std::from_chars(value.string.data(), value.string.data() + value.string.size(), parsed,
        std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != value.string.data() + value.string.size() || !std::isfinite(parsed))
        ProtocolFailure("InvalidPairRequest", "numeric field is outside the supported range");
    return parsed;
}
void RequireStringValue(const JsonValue& object, std::string_view field, std::string_view expected) {
    if (RequireField(object, field, JsonKind::string).string != expected)
        ProtocolFailure("InvalidPairRequest", "frozen snapshot value is invalid");
}
void RequireTrue(const JsonValue& object, std::string_view field) {
    if (!RequireField(object, field, JsonKind::boolean).boolean)
        ProtocolFailure("InvalidPairRequest", "operator confirmation is missing");
}
std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        ProtocolFailure("InvalidPairRequest", "transaction directory encoding is invalid");
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) ProtocolFailure("InvalidPairRequest", "transaction directory encoding is invalid");
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), output.data(), required) != required)
        ProtocolFailure("InvalidPairRequest", "transaction directory encoding is invalid");
    return output;
}
void ValidateTransactionDirectory(std::string_view raw_path) {
    if (raw_path.size() < 4 || raw_path.size() > 1024 || !std::isalpha(static_cast<unsigned char>(raw_path[0])) ||
        raw_path[1] != ':' || (raw_path[2] != '\\' && raw_path[2] != '/') ||
        raw_path.find(':', 2) != std::string_view::npos ||
        std::any_of(raw_path.begin(), raw_path.end(), [](unsigned char character) { return character < 0x20U; }))
        ProtocolFailure("InvalidPairRequest", "transaction directory must be a fixed local path");
    std::string component;
    for (std::size_t index = 3; index <= raw_path.size(); ++index) {
        if (index == raw_path.size() || raw_path[index] == '\\' || raw_path[index] == '/') {
            if (component.empty() || component == "." || component == "..")
                ProtocolFailure("InvalidPairRequest", "transaction directory contains an unsafe component");
            component.clear();
        } else component.push_back(raw_path[index]);
    }
    const std::filesystem::path path(Utf8ToWide(raw_path));
    const std::wstring drive_root = path.root_name().wstring() + L"\\";
    if (GetDriveTypeW(drive_root.c_str()) != DRIVE_FIXED)
        ProtocolFailure("InvalidPairRequest", "transaction directory drive is not fixed");
    std::filesystem::path current = path.root_path();
    for (const auto& part : path.relative_path()) {
        current /= part;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) break;
            ProtocolFailure("InvalidPairRequest", "transaction directory cannot be inspected");
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            ProtocolFailure("InvalidPairRequest", "transaction directory contains a reparse point");
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            ProtocolFailure("InvalidPairRequest", "transaction directory names an existing file");
    }
}
void ValidateIdentity(const JsonValue& identity, std::int64_t started, std::int64_t now,
    DualHardwareCameraAgentRequest& request) {
    RequireExactFields(identity, {"status", "reasonCode", "observedAtUtc", "expiresAtUtc"});
    RequireStringValue(identity, "status", "Ready");
    if (!IsBoundedText(RequireField(identity, "reasonCode", JsonKind::string).string, 128))
        ProtocolFailure("InvalidPairRequest", "identity reason is invalid");
    const auto observed = ParseUtc100ns(RequireField(identity, "observedAtUtc", JsonKind::string).string);
    const auto expires = ParseUtc100ns(RequireField(identity, "expiresAtUtc", JsonKind::string).string);
    if (observed > started || started > now || observed >= expires || now >= expires)
        ProtocolFailure("InvalidPairRequest", "identity snapshot is stale or inconsistent");
    request.identity_observed_at_100ns = observed;
    request.identity_expires_at_100ns = expires;
}
void ValidateCaptureBody(const JsonValue& body, std::string_view alias) {
    RequireExactFields(body, {"alias","imageArea","fileFormat","jpegQuality","imageSize","exposureMode",
        "autoIsoEnabled","focusMode","whiteBalanceMode","vibrationReductionEnabled"});
    RequireStringValue(body, "alias", alias); RequireStringValue(body, "imageArea", "FX");
    RequireStringValue(body, "fileFormat", "JPEG"); RequireStringValue(body, "jpegQuality", "Fine");
    RequireStringValue(body, "imageSize", "L"); RequireStringValue(body, "exposureMode", "Manual");
    RequireStringValue(body, "focusMode", "Manual"); RequireStringValue(body, "whiteBalanceMode", "Fixed");
    if (RequireField(body, "autoIsoEnabled", JsonKind::boolean).boolean ||
        RequireField(body, "vibrationReductionEnabled", JsonKind::boolean).boolean)
        ProtocolFailure("InvalidPairRequest", "capture body settings are not frozen");
}
void ValidateCaptureProfile(const JsonValue& profile, std::int64_t started, std::int64_t now,
    DualHardwareCameraAgentRequest& request) {
    RequireExactFields(profile, {"profileId","version","schemaVersion","status","approvedAtUtc","validUntilUtc","bodies"});
    if (!IsBoundedText(RequireField(profile, "profileId", JsonKind::string).string, 128) ||
        !IsBoundedText(RequireField(profile, "version", JsonKind::string).string, 64))
        ProtocolFailure("InvalidPairRequest", "capture profile identity is invalid");
    RequireStringValue(profile, "schemaVersion", "a0.hardware-dual-capture-profile.v1");
    RequireStringValue(profile, "status", "Approved");
    const auto approved = ParseUtc100ns(RequireField(profile, "approvedAtUtc", JsonKind::string).string);
    const auto valid = ParseUtc100ns(RequireField(profile, "validUntilUtc", JsonKind::string).string);
    if (approved >= valid || approved > started || valid <= started || valid <= now)
        ProtocolFailure("InvalidPairRequest", "capture profile is stale or inconsistent");
    const auto& bodies = RequireField(profile, "bodies", JsonKind::array).array;
    if (bodies.size() != 2) ProtocolFailure("InvalidPairRequest", "capture profile requires two bodies");
    ValidateCaptureBody(bodies[0], kCameraAliasA); ValidateCaptureBody(bodies[1], kCameraAliasB);
    request.capture_profile_valid_until_100ns = valid;
}
void ValidateRigProfile(const JsonValue& profile, std::int64_t started, std::int64_t now,
    DualHardwareCameraAgentRequest& request) {
    RequireExactFields(profile, {"profileId","version","status","schemaVersion","provenance","measuredAtUtc",
        "validUntilUtc","assessedAtUtc","expectedInputWidth","expectedInputHeight","cameraBToCameraA","layout","crop","cameraAliases"});
    if (!IsBoundedText(RequireField(profile, "profileId", JsonKind::string).string, 128) ||
        !IsBoundedText(RequireField(profile, "version", JsonKind::string).string, 64) ||
        !IsBoundedText(RequireField(profile, "provenance", JsonKind::string).string, 256))
        ProtocolFailure("InvalidPairRequest", "rig profile identity is invalid");
    RequireStringValue(profile, "status", "Approved"); RequireStringValue(profile, "schemaVersion", "1.1.0");
    const auto& layout = RequireField(profile, "layout", JsonKind::string).string;
    if (layout != "camera-a-left-camera-b-right" &&
        layout != "camera-a-top-camera-b-bottom")
        ProtocolFailure("InvalidPairRequest", "rig layout is unsupported");
    const auto measured = ParseUtc100ns(RequireField(profile, "measuredAtUtc", JsonKind::string).string);
    const auto assessed = ParseUtc100ns(RequireField(profile, "assessedAtUtc", JsonKind::string).string);
    const auto valid = ParseUtc100ns(RequireField(profile, "validUntilUtc", JsonKind::string).string);
    if (measured > assessed || assessed > started || assessed >= valid || valid <= started || valid <= now)
        ProtocolFailure("InvalidPairRequest", "rig profile is stale or inconsistent");
    if (ParseInteger(RequireField(profile, "expectedInputWidth", JsonKind::number)) <= 0 ||
        ParseInteger(RequireField(profile, "expectedInputHeight", JsonKind::number)) <= 0)
        ProtocolFailure("InvalidPairRequest", "rig dimensions are invalid");
    const auto& matrix = RequireField(profile, "cameraBToCameraA", JsonKind::array).array;
    if (matrix.size() != 9) ProtocolFailure("InvalidPairRequest", "rig matrix must contain nine values");
    double values[9]{}; for (std::size_t index = 0; index < 9; ++index) values[index] = ParseFiniteNumber(matrix[index]);
    const double determinant = values[0]*(values[4]*values[8]-values[5]*values[7])-
        values[1]*(values[3]*values[8]-values[5]*values[6])+values[2]*(values[3]*values[7]-values[4]*values[6]);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
        ProtocolFailure("InvalidPairRequest", "rig matrix is singular");
    const auto& crop = RequireField(profile, "crop", JsonKind::array).array;
    if (crop.size() != 4) ProtocolFailure("InvalidPairRequest", "rig crop must contain four values");
    for (const auto& value : crop) if (ParseInteger(value) < 0) ProtocolFailure("InvalidPairRequest", "rig crop is invalid");
    const auto& aliases = RequireField(profile, "cameraAliases", JsonKind::array).array;
    if (aliases.size()!=2 || aliases[0].kind!=JsonKind::string || aliases[1].kind!=JsonKind::string ||
        aliases[0].string!=kCameraAliasA || aliases[1].string!=kCameraAliasB)
        ProtocolFailure("InvalidPairRequest", "rig aliases are invalid");
    request.rig_profile_valid_until_100ns = valid;
}
void ValidateConfirmations(const JsonValue& confirmations) {
    RequireExactFields(confirmations, {"identitySnapshotApproved","captureProfileFrozen","rigProfileFrozen",
        "liveViewStoppedAndClosed","bothCardsConfirmedEmpty"});
    RequireTrue(confirmations, "identitySnapshotApproved"); RequireTrue(confirmations, "captureProfileFrozen");
    RequireTrue(confirmations, "rigProfileFrozen"); RequireTrue(confirmations, "liveViewStoppedAndClosed");
    RequireTrue(confirmations, "bothCardsConfirmedEmpty");
}

void ValidateCameraMode(const JsonValue& payload) {
    if (RequireField(payload, "cameraMode", JsonKind::string).string != kCameraMode) {
        ProtocolFailure(
            "InvalidCameraMode", "Dual hardware operation requires DualCamera mode");
    }
}

void ValidateOrderedAliases(const JsonValue& payload) {
    const auto& aliases =
        RequireField(payload, "orderedRequiredAliases", JsonKind::array).array;
    if (aliases.size() != 2 || aliases[0].kind != JsonKind::string ||
        aliases[1].kind != JsonKind::string ||
        aliases[0].string != kCameraAliasA || aliases[1].string != kCameraAliasB) {
        ProtocolFailure(
            "InvalidAliasOrder",
            "Dual hardware aliases must be exactly CAM-A then CAM-B");
    }
}

std::string ValidateTransactionId(const JsonValue& object) {
    const auto& transaction_id =
        RequireField(object, "transactionId", JsonKind::string).string;
    if (!IsSafeTransactionId(transaction_id)) {
        ProtocolFailure(
            "InvalidTransactionId",
            "Dual hardware transactionId must contain exactly 32 hexadecimal characters");
    }
    return transaction_id;
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
    std::string_view request_id,
    bool success,
    std::string_view result_code) {
    const std::string safe_request_id = IsSafeRequestId(request_id)
        ? std::string(request_id)
        : "rejected";
    std::ostringstream output;
    output << "{\"schemaVersion\":\"" << kDualHardwareCameraAgentSchemaVersion
           << "\",\"simulation\":false,\"marker\":\""
           << kDualHardwareCameraAgentMarker << "\",\"requestId\":\""
           << safe_request_id << "\",\"success\":"
           << (success ? "true" : "false") << ",\"resultCode\":\""
           << result_code << "\",\"payload\":";
    return output.str();
}

std::string FormatUtc100ns(std::int64_t unix_100ns) {
    constexpr std::int64_t epoch = 116444736000000000LL;
    const std::uint64_t ticks = static_cast<std::uint64_t>(unix_100ns + epoch);
    FILETIME file_time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32U)};
    SYSTEMTIME system_time{};
    if (!FileTimeToSystemTime(&file_time, &system_time))
        ProtocolFailure("AgentFailure", "completion time could not be formatted");
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%07lldZ",
        system_time.wYear, system_time.wMonth, system_time.wDay,
        system_time.wHour, system_time.wMinute, system_time.wSecond,
        static_cast<long long>((unix_100ns % kHundredNanosecondsPerSecond +
            kHundredNanosecondsPerSecond) % kHundredNanosecondsPerSecond));
    return buffer;
}

bool IsRegularCanonicalOriginal(const std::filesystem::path& path) {
    ValidateTransactionDirectory(path.parent_path().generic_string());
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

std::string BuildTerminalResult(
    const JsonValue& transaction, std::string_view transaction_id,
    std::string_view terminal_state, std::string_view failure_code,
    const std::vector<std::pair<std::string, DualHardwareFakeCaptureOutcome>>& originals,
    const std::filesystem::path& transaction_directory,
    std::int64_t completed_at_100ns) {
    const auto& identity = RequireField(transaction, "identitySnapshot", JsonKind::object);
    const auto& capture = RequireField(transaction, "captureProfileSnapshot", JsonKind::object);
    const auto& rig = RequireField(transaction, "rigProfileSnapshot", JsonKind::object);
    std::string original_json = "[";
    for (std::size_t index = 0; index < originals.size(); ++index) {
        if (index != 0) original_json += ',';
        const auto& [alias, outcome] = originals[index];
        const auto canonical = (transaction_directory / alias / "original.jpg").generic_string();
        original_json += "{\"alias\":\"" + alias + "\",\"canonicalOriginalPath\":\"" +
            JsonEscape(canonical) + "\",\"exactRecoveredObjectDeleted\":" +
            (outcome.exact_recovered_object_deleted ? "true" : "false") +
            ",\"spoolEmptyAfterDelete\":" + (outcome.spool_empty_after_delete ? "true" : "false") + "}";
    }
    original_json += "]";
    const bool exact = std::all_of(originals.begin(), originals.end(), [](const auto& item) {
        return item.second.exact_recovered_object_deleted;
    });
    const bool empty = std::all_of(originals.begin(), originals.end(), [](const auto& item) {
        return item.second.spool_empty_after_delete;
    });
    return "{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"originals\":" + original_json + ",\"terminalState\":\"" +
        std::string(terminal_state) + "\",\"failureCode\":\"" + std::string(failure_code) +
        "\",\"evidence\":{\"terminalState\":\"" + std::string(terminal_state) +
        "\",\"identitySnapshot\":" + SerializeJson(identity) +
        ",\"captureProfileId\":\"" + JsonEscape(RequireField(capture, "profileId", JsonKind::string).string) +
        "\",\"captureProfileVersion\":\"" + JsonEscape(RequireField(capture, "version", JsonKind::string).string) +
        "\",\"profileId\":\"" + JsonEscape(RequireField(rig, "profileId", JsonKind::string).string) +
        "\",\"profileVersion\":\"" + JsonEscape(RequireField(rig, "version", JsonKind::string).string) +
        "\",\"watchdogStartedAtUtc\":\"" + RequireField(transaction, "startedAtUtc", JsonKind::string).string +
        "\",\"watchdogDeadlineUtc\":\"" + RequireField(transaction, "watchdogDeadlineUtc", JsonKind::string).string +
        "\",\"completedAtUtc\":\"" + FormatUtc100ns(completed_at_100ns) +
        "\",\"watchdogCompletedInTime\":" +
        (terminal_state == "WatchdogExpired" ? "false" : "true") +
        ",\"liveViewStopAndCloseConfirmed\":true,\"exactDeleteConfirmedForEveryRetainedOriginal\":" +
        (exact ? "true" : "false") + ",\"bothSpoolsEmptyAfter\":" +
        (empty ? "true" : "false") + ",\"automaticRetryCount\":0}}";
}

DualHardwarePairJournalState JournalStateFor(std::string_view terminal) {
    if (terminal == "Succeeded") return DualHardwarePairJournalState::succeeded;
    if (terminal == "Failed") return DualHardwarePairJournalState::failed;
    if (terminal == "FailedPartial") return DualHardwarePairJournalState::failed_partial;
    return DualHardwarePairJournalState::watchdog_expired;
}

std::string CapabilitiesResponse(std::string_view request_id) {
    return ResponsePrefix(request_id, true, "DualCapabilities") +
        "{\"cameraMode\":\"DualCamera\",\"protocolVersion\":2,"
        "\"orderedRequiredAliases\":[\"CAM-A\",\"CAM-B\"],"
        "\"supportedOperations\":[\"get-dual-capabilities\","
        "\"reserve-pair-transaction\",\"start-reserved-pair\","
        "\"get-pair-transaction-result\",\"close-reserved-pair-transaction\"],"
        "\"pairJournalDurable\":true,"
        "\"sameTransactionQueryOnly\":true,\"automaticRetryCount\":0}}";
}

std::string ReservationUnavailableResponse(
    std::string_view request_id,
    std::string_view transaction_id) {
    return ResponsePrefix(request_id, false, "PairStoreUnavailable") +
        "{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"accepted\":false}}";
}

std::string ReservationResponse(
    std::string_view request_id,
    std::string_view transaction_id,
    bool success,
    std::string_view result_code,
    bool accepted) {
    return ResponsePrefix(request_id, success, result_code) +
        "{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"accepted\":" + (accepted ? "true" : "false") + "}}";
}

std::string StartUnavailableResponse(
    std::string_view request_id,
    std::string_view transaction_id) {
    return ResponsePrefix(request_id, false, "PairDispatcherUnavailable") +
        "{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"dispatchStarted\":false}}";
}

std::string QueryUnavailableResponse(
    std::string_view request_id,
    std::string_view transaction_id) {
    return ResponsePrefix(request_id, false, "PairStoreUnavailable") +
        "{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"found\":false,\"result\":null}}";
}

std::string QueryResponse(
    std::string_view request_id,
    std::string_view transaction_id,
    std::string_view result_code,
    bool found) {
    return ResponsePrefix(request_id, false, result_code) +
        "{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"found\":" + (found ? "true" : "false") +
        ",\"result\":null}}";
}

std::string CloseResponse(
    std::string_view request_id,
    std::string_view transaction_id,
    bool success,
    std::string_view result_code,
    bool closed_before_dispatch) {
    return ResponsePrefix(request_id, success, result_code) +
        "{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"closedBeforeDispatch\":" +
        (closed_before_dispatch ? "true" : "false") + "}}";
}

std::string ProtocolRejection(
    std::string_view request_id,
    std::string_view result_code) {
    return ResponsePrefix(request_id, false, result_code) +
        "{\"rejected\":true}}";
}

} // namespace

DualHardwareCameraAgentProtocolError::DualHardwareCameraAgentProtocolError(
    std::string code,
    std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

const std::string& DualHardwareCameraAgentProtocolError::Code() const noexcept {
    return code_;
}

DualHardwareCameraAgentDispatcher::DualHardwareCameraAgentDispatcher(
    std::shared_ptr<DualHardwarePairJournalStore> pair_store) noexcept
    : pair_store_(std::move(pair_store)) {}

DualHardwareCameraAgentDispatcher::DualHardwareCameraAgentDispatcher(
    std::shared_ptr<DualHardwarePairJournalStore> pair_store, DualHardwareUtcClock utc_clock)
    : pair_store_(std::move(pair_store)), utc_clock_(std::move(utc_clock)) {
    if (!utc_clock_) throw std::invalid_argument("utc_clock is required");
}

DualHardwareCameraAgentDispatcher::DualHardwareCameraAgentDispatcher(
    std::shared_ptr<DualHardwarePairJournalStore> pair_store,
    DualHardwareUtcClock utc_clock,
    std::shared_ptr<DualHardwareFakePairCaptureBackend> fake_backend)
    : pair_store_(std::move(pair_store)), utc_clock_(std::move(utc_clock)),
      fake_backend_(std::move(fake_backend)) {
    if (!utc_clock_) throw std::invalid_argument("utc_clock is required");
}

DualHardwareCameraAgentRequest ParseDualHardwareCameraAgentRequest(
    std::string_view json) {
    const JsonValue root = JsonParser(json).Parse();
    RequireExactFields(root, {
        "schemaVersion", "simulation", "marker", "requestId", "operation", "payload",
    });

    const auto& schema =
        RequireField(root, "schemaVersion", JsonKind::string).string;
    const bool simulation =
        RequireField(root, "simulation", JsonKind::boolean).boolean;
    const auto& marker = RequireField(root, "marker", JsonKind::string).string;
    const auto& request_id =
        RequireField(root, "requestId", JsonKind::string).string;
    const auto& operation =
        RequireField(root, "operation", JsonKind::string).string;
    const auto& payload = RequireField(root, "payload", JsonKind::object);

    if (schema != kDualHardwareCameraAgentSchemaVersion || simulation ||
        marker != kDualHardwareCameraAgentMarker) {
        ProtocolFailure(
            "DualHardwareProtocolRequired",
            "request is not the required Dual hardware v2 protocol");
    }
    if (!IsSafeRequestId(request_id)) {
        ProtocolFailure(
            "InvalidRequestId", "a bounded safe request ID is required");
    }

    DualHardwareCameraAgentRequest request;
    request.request_id = request_id;
    if (operation == "get-dual-capabilities") {
        RequireExactFields(payload, {"cameraMode"});
        ValidateCameraMode(payload);
        request.operation =
            DualHardwareCameraAgentOperation::get_dual_capabilities;
        return request;
    }
    if (operation == "reserve-pair-transaction") {
        RequireExactFields(payload, {
            "transactionId", "cameraMode", "orderedRequiredAliases",
        });
        ValidateCameraMode(payload);
        ValidateOrderedAliases(payload);
        request.transaction_id = ValidateTransactionId(payload);
        request.operation =
            DualHardwareCameraAgentOperation::reserve_pair_transaction;
        return request;
    }
    if (operation == "start-reserved-pair") {
        RequireExactFields(payload, {
            "cameraMode", "orderedRequiredAliases", "transaction",
        });
        ValidateCameraMode(payload);
        ValidateOrderedAliases(payload);
        const auto& transaction =
            RequireField(payload, "transaction", JsonKind::object);
        RequireExactFields(transaction, {
            "transactionId",
            "transactionDirectory",
            "identitySnapshot",
            "captureProfileSnapshot",
            "rigProfileSnapshot",
            "operatorConfirmations",
            "startedAtUtc",
            "watchdogDeadlineUtc",
        });
        request.transaction_id = ValidateTransactionId(transaction);
        (void)RequireField(transaction, "transactionDirectory", JsonKind::string);
        (void)RequireField(transaction, "identitySnapshot", JsonKind::object);
        (void)RequireField(transaction, "captureProfileSnapshot", JsonKind::object);
        (void)RequireField(transaction, "rigProfileSnapshot", JsonKind::object);
        (void)RequireField(transaction, "operatorConfirmations", JsonKind::object);
        (void)RequireField(transaction, "startedAtUtc", JsonKind::string);
        (void)RequireField(transaction, "watchdogDeadlineUtc", JsonKind::string);
        request.operation = DualHardwareCameraAgentOperation::start_reserved_pair;
        return request;
    }
    if (operation == "get-pair-transaction-result") {
        RequireExactFields(payload, {"transactionId"});
        request.transaction_id = ValidateTransactionId(payload);
        request.operation =
            DualHardwareCameraAgentOperation::get_pair_transaction_result;
        return request;
    }
    if (operation == "close-reserved-pair-transaction") {
        RequireExactFields(payload, {"transactionId"});
        request.transaction_id = ValidateTransactionId(payload);
        request.operation =
            DualHardwareCameraAgentOperation::close_reserved_pair_transaction;
        return request;
    }
    ProtocolFailure(
        "UnsupportedOperation", "Dual hardware protocol operation is unsupported");
}

std::string DualHardwareCameraAgentDispatcher::Handle(
    std::string_view request_json) noexcept {
    const std::string extracted_request_id =
        TryExtractSafeRequestId(request_json);
    try {
        auto request =
            ParseDualHardwareCameraAgentRequest(request_json);
        switch (request.operation) {
        case DualHardwareCameraAgentOperation::get_dual_capabilities:
            return CapabilitiesResponse(request.request_id);
        case DualHardwareCameraAgentOperation::reserve_pair_transaction: {
            if (pair_store_ == nullptr) {
                return ReservationUnavailableResponse(
                    request.request_id, request.transaction_id);
            }
            try {
                (void)pair_store_->Reserve(request.transaction_id);
                return ReservationResponse(
                    request.request_id, request.transaction_id, true,
                    "PairTransactionReserved", true);
            } catch (const DualHardwarePairJournalStoreError& error) {
                if (error.Code() == "DuplicateTransactionId" ||
                    error.Code() == "ActiveTransactionExists") {
                    return ReservationResponse(
                        request.request_id, request.transaction_id, false,
                        error.Code(), false);
                }
                return ReservationResponse(
                    request.request_id, request.transaction_id, false,
                    "PairStoreFailure", false);
            }
        }
        case DualHardwareCameraAgentOperation::start_reserved_pair:
        {
            const JsonValue root = JsonParser(request_json).Parse();
            const auto& transaction = RequireField(RequireField(root, "payload", JsonKind::object), "transaction", JsonKind::object);
            const auto now = Clock100ns(utc_clock_);
            request.started_at_100ns = ParseUtc100ns(RequireField(transaction, "startedAtUtc", JsonKind::string).string);
            request.watchdog_deadline_100ns = ParseUtc100ns(RequireField(transaction, "watchdogDeadlineUtc", JsonKind::string).string);
            if (request.started_at_100ns > now || now >= request.watchdog_deadline_100ns ||
                request.watchdog_deadline_100ns - request.started_at_100ns != 180 * kHundredNanosecondsPerSecond)
                ProtocolFailure("InvalidPairRequest", "pair watchdog window is invalid");
            ValidateTransactionDirectory(RequireField(transaction, "transactionDirectory", JsonKind::string).string);
            ValidateIdentity(RequireField(transaction, "identitySnapshot", JsonKind::object), request.started_at_100ns, now, request);
            ValidateCaptureProfile(RequireField(transaction, "captureProfileSnapshot", JsonKind::object), request.started_at_100ns, now, request);
            ValidateRigProfile(RequireField(transaction, "rigProfileSnapshot", JsonKind::object), request.started_at_100ns, now, request);
            ValidateConfirmations(RequireField(transaction, "operatorConfirmations", JsonKind::object));
            if (pair_store_ == nullptr || fake_backend_ == nullptr) {
                return StartUnavailableResponse(
                    request.request_id, request.transaction_id);
            }
            bool dispatch_started = false;
            try {
                (void)pair_store_->BeginDispatch(request.transaction_id);
                dispatch_started = true;
                ++safety_counters_.pair_dispatch_count;
                const std::filesystem::path transaction_directory =
                    std::filesystem::path(RequireField(transaction,
                        "transactionDirectory", JsonKind::string).string);
                std::vector<std::pair<std::string, DualHardwareFakeCaptureOutcome>> originals;
                const auto terminalize = [&](std::string_view state,
                                             std::string_view failure,
                                             std::int64_t completed) {
                    const std::string result = BuildTerminalResult(
                        transaction, request.transaction_id, state, failure,
                        originals, transaction_directory, completed);
                    const auto persisted = pair_store_->CompleteTerminal(
                        request.transaction_id, JournalStateFor(state), result);
                    return ResponsePrefix(request.request_id, true, "PairDispatchAccepted") +
                        "{\"transactionId\":\"" + request.transaction_id +
                        "\",\"dispatchState\":\"Completed\",\"result\":" +
                        persisted.terminal_result_json + "}}";
                };
                const auto before_a = Clock100ns(utc_clock_);
                if (before_a >= request.watchdog_deadline_100ns)
                    return terminalize("WatchdogExpired", "WatchdogExpired", before_a);
                DualHardwareFakeCaptureOutcome a{};
                bool a_spool_invalid = false;
                try {
                    const auto path = transaction_directory / "CAM-A" / "original.jpg";
                    a = fake_backend_->Capture("CAM-A", path, request.watchdog_deadline_100ns);
                    a_spool_invalid = a.succeeded &&
                        (!a.exact_recovered_object_deleted || !a.spool_empty_after_delete);
                    if (a.succeeded && !IsRegularCanonicalOriginal(path)) a.succeeded = false;
                } catch (...) { a.succeeded = false; }
                const auto after_a = Clock100ns(utc_clock_);
                if (after_a >= request.watchdog_deadline_100ns)
                    return terminalize("WatchdogExpired", "WatchdogExpired", after_a);
                if (a_spool_invalid) return terminalize("Failed", "SpoolNotEmpty", after_a);
                if (!a.succeeded) return terminalize("Failed", "CaptureCameraA", after_a);
                originals.emplace_back("CAM-A", a);
                DualHardwareFakeCaptureOutcome b{};
                bool b_spool_invalid = false;
                try {
                    const auto path = transaction_directory / "CAM-B" / "original.jpg";
                    b = fake_backend_->Capture("CAM-B", path, request.watchdog_deadline_100ns);
                    b_spool_invalid = b.succeeded &&
                        (!b.exact_recovered_object_deleted || !b.spool_empty_after_delete);
                    if (b.succeeded && !IsRegularCanonicalOriginal(path)) b.succeeded = false;
                } catch (...) { b.succeeded = false; }
                const auto after_b = Clock100ns(utc_clock_);
                if (after_b >= request.watchdog_deadline_100ns)
                    return terminalize("WatchdogExpired", "WatchdogExpired", after_b);
                if (b_spool_invalid) return terminalize("FailedPartial", "SpoolNotEmpty", after_b);
                if (!b.succeeded) return terminalize("FailedPartial", "CaptureCameraB", after_b);
                originals.emplace_back("CAM-B", b);
                return terminalize("Succeeded", "None", after_b);
            } catch (const DualHardwarePairJournalStoreError&) {
                return ResponsePrefix(request.request_id, false, "PairStoreFailure") +
                    "{\"transactionId\":\"" + request.transaction_id +
                    "\",\"dispatchStarted\":" +
                    (dispatch_started ? "true" : "false") + "}}";
            }
        }
        case DualHardwareCameraAgentOperation::get_pair_transaction_result: {
            if (pair_store_ == nullptr) {
                return QueryUnavailableResponse(
                    request.request_id, request.transaction_id);
            }
            try {
                const auto record = pair_store_->Query(request.transaction_id);
                if (record && record->state ==
                    DualHardwarePairJournalState::closed_before_dispatch) {
                    return ResponsePrefix(
                               request.request_id, true,
                               "PairTransactionClosedBeforeDispatch") +
                        "{\"transactionId\":\"" + request.transaction_id +
                        "\",\"found\":true,\"result\":null}}";
                }
                if (record && !record->terminal_result_json.empty()) {
                    return ResponsePrefix(request.request_id, true, "PairTransactionFound") +
                        "{\"transactionId\":\"" + request.transaction_id +
                        "\",\"found\":true,\"result\":" +
                        record->terminal_result_json + "}}";
                }
                return QueryResponse(
                    request.request_id, request.transaction_id,
                    record ? "PairTransactionReserved" :
                        "PairTransactionNotFound",
                    record.has_value());
            } catch (const DualHardwarePairJournalStoreError&) {
                return QueryResponse(
                    request.request_id, request.transaction_id,
                    "PairStoreFailure", false);
            }
        }
        case DualHardwareCameraAgentOperation::close_reserved_pair_transaction: {
            if (pair_store_ == nullptr) {
                return CloseResponse(
                    request.request_id, request.transaction_id, false,
                    "PairStoreUnavailable", false);
            }
            try {
                const auto closed =
                    pair_store_->CloseReservedBeforeDispatch(
                        request.transaction_id);
                const bool confirmed =
                    closed.transaction_id == request.transaction_id &&
                    closed.state ==
                        DualHardwarePairJournalState::closed_before_dispatch &&
                    closed.automatic_retry_count == 0 &&
                    closed.terminal_result_json.empty();
                return CloseResponse(
                    request.request_id, request.transaction_id, confirmed,
                    confirmed ? "PairTransactionClosedBeforeDispatch" :
                        "PairCloseRejected",
                    confirmed);
            } catch (const DualHardwarePairJournalStoreError&) {
                return CloseResponse(
                    request.request_id, request.transaction_id, false,
                    "PairCloseRejected", false);
            }
        }
        }
    } catch (const DualHardwareCameraAgentProtocolError& error) {
        return ProtocolRejection(extracted_request_id, error.Code());
    } catch (...) {
        return ProtocolRejection(extracted_request_id, "AgentFailure");
    }
    return ProtocolRejection(extracted_request_id, "AgentFailure");
}

DualHardwareCameraAgentSafetyCounters
DualHardwareCameraAgentDispatcher::SafetyCounters() const noexcept {
    return safety_counters_;
}

void DualHardwareCameraAgentDispatcher::OnIdle() noexcept {
    // The Dual protocol has no continuous/idle-driven backend state (unlike
    // Single's Live View heartbeat), so there is nothing to poll here. This
    // method exists only to satisfy the shared named-pipe server loop's
    // dispatcher contract (see hardware_camera_agent_pipe.cpp).
}

bool DualHardwareCameraAgentDispatcher::ShouldStop() const noexcept {
    // The Dual protocol has no operation that asks the host to terminate
    // (no close-agent-session equivalent). The host's lifetime is bounded
    // solely by the named-pipe server loop's own deadline.
    return false;
}

} // namespace a0::phase0
