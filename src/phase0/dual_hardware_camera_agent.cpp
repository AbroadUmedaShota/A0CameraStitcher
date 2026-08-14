#include "a0/phase0/dual_hardware_camera_agent.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace a0::phase0 {
namespace {

constexpr std::size_t kMaximumProtocolJsonBytes = 256U * 1024U;
constexpr int kMaximumProtocolJsonDepth = 32;
constexpr std::string_view kCameraMode = "DualCamera";
constexpr std::string_view kCameraAliasA = "CAM-A";
constexpr std::string_view kCameraAliasB = "CAM-B";

enum class JsonKind {
    object,
    array,
    string,
    boolean,
    number,
    null_value,
};

struct JsonValue {
    JsonKind kind{JsonKind::null_value};
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;
    std::string string;
    bool boolean{};
};

[[noreturn]] void ProtocolFailure(std::string code, std::string message) {
    throw DualHardwareCameraAgentProtocolError(
        std::move(code), std::move(message));
}

void AppendUtf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7FU) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

class JsonParser final {
public:
    explicit JsonParser(std::string_view input) : input_(input) {
        if (input.empty() || input.size() > kMaximumProtocolJsonBytes) {
            ProtocolFailure(
                "MalformedEnvelope",
                "Dual hardware protocol JSON length is outside the allowed range");
        }
    }

    [[nodiscard]] JsonValue Parse() {
        SkipWhitespace();
        JsonValue value = ParseValue(0);
        SkipWhitespace();
        if (position_ != input_.size()) {
            ProtocolFailure(
                "MalformedEnvelope",
                "unexpected data follows the Dual hardware protocol JSON value");
        }
        return value;
    }

private:
    [[nodiscard]] JsonValue ParseValue(int depth) {
        if (depth > kMaximumProtocolJsonDepth || position_ >= input_.size()) {
            ProtocolFailure(
                "MalformedEnvelope",
                "Dual hardware protocol JSON nesting is invalid");
        }
        const char current = input_[position_];
        if (current == '{') return ParseObject(depth + 1);
        if (current == '[') return ParseArray(depth + 1);
        if (current == '"') {
            JsonValue value;
            value.kind = JsonKind::string;
            value.string = ParseString();
            return value;
        }
        if (current == 't' || current == 'f') return ParseBoolean();
        if (current == 'n') return ParseNull();
        if (current == '-' || (current >= '0' && current <= '9')) {
            return ParseNumber();
        }
        ProtocolFailure(
            "MalformedEnvelope",
            "Dual hardware protocol JSON contains an unsupported value type");
    }

    [[nodiscard]] JsonValue ParseObject(int depth) {
        JsonValue value;
        value.kind = JsonKind::object;
        ++position_;
        SkipWhitespace();
        if (Consume('}')) return value;
        for (;;) {
            if (position_ >= input_.size() || input_[position_] != '"') {
                ProtocolFailure(
                    "MalformedEnvelope",
                    "Dual hardware protocol JSON object key is invalid");
            }
            std::string key = ParseString();
            SkipWhitespace();
            if (!Consume(':')) {
                ProtocolFailure(
                    "MalformedEnvelope",
                    "Dual hardware protocol JSON object is missing ':'");
            }
            SkipWhitespace();
            JsonValue child = ParseValue(depth);
            if (!value.object.emplace(std::move(key), std::move(child)).second) {
                ProtocolFailure(
                    "DuplicateField",
                    "Dual hardware protocol JSON contains a duplicate field");
            }
            SkipWhitespace();
            if (Consume('}')) break;
            if (!Consume(',')) {
                ProtocolFailure(
                    "MalformedEnvelope",
                    "Dual hardware protocol JSON object is missing ','");
            }
            SkipWhitespace();
        }
        return value;
    }

    [[nodiscard]] JsonValue ParseArray(int depth) {
        JsonValue value;
        value.kind = JsonKind::array;
        ++position_;
        SkipWhitespace();
        if (Consume(']')) return value;
        for (;;) {
            value.array.push_back(ParseValue(depth));
            SkipWhitespace();
            if (Consume(']')) break;
            if (!Consume(',')) {
                ProtocolFailure(
                    "MalformedEnvelope",
                    "Dual hardware protocol JSON array is missing ','");
            }
            SkipWhitespace();
        }
        return value;
    }

    [[nodiscard]] std::uint32_t ParseHexQuad() {
        if (input_.size() - position_ < 4) {
            ProtocolFailure("MalformedEnvelope", "incomplete JSON unicode escape");
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char character = input_[position_++];
            value <<= 4U;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                ProtocolFailure("MalformedEnvelope", "invalid JSON unicode escape");
            }
        }
        return value;
    }

    [[nodiscard]] std::string ParseString() {
        if (!Consume('"')) {
            ProtocolFailure(
                "MalformedEnvelope", "Dual hardware protocol JSON string is invalid");
        }
        std::string output;
        while (position_ < input_.size()) {
            const unsigned char character =
                static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return output;
            if (character < 0x20U) {
                ProtocolFailure(
                    "MalformedEnvelope",
                    "Dual hardware protocol JSON string contains a control character");
            }
            if (character != '\\') {
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= input_.size()) {
                ProtocolFailure("MalformedEnvelope", "incomplete JSON escape");
            }
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
                std::uint32_t code_point = ParseHexQuad();
                if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
                    if (input_.size() - position_ < 6 ||
                        input_[position_] != '\\' ||
                        input_[position_ + 1] != 'u') {
                        ProtocolFailure(
                            "MalformedEnvelope", "incomplete JSON surrogate pair");
                    }
                    position_ += 2;
                    const std::uint32_t low = ParseHexQuad();
                    if (low < 0xDC00U || low > 0xDFFFU) {
                        ProtocolFailure(
                            "MalformedEnvelope", "invalid JSON surrogate pair");
                    }
                    code_point = 0x10000U +
                        ((code_point - 0xD800U) << 10U) +
                        (low - 0xDC00U);
                } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
                    ProtocolFailure(
                        "MalformedEnvelope", "unexpected JSON low surrogate");
                }
                AppendUtf8(output, code_point);
                break;
            }
            default:
                ProtocolFailure("MalformedEnvelope", "unsupported JSON escape");
            }
        }
        ProtocolFailure(
            "MalformedEnvelope", "unterminated Dual hardware protocol JSON string");
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
        ProtocolFailure("MalformedEnvelope", "invalid JSON boolean");
    }

    [[nodiscard]] JsonValue ParseNumber() {
        const std::size_t start = position_;
        if (input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) {
            ProtocolFailure("MalformedEnvelope", "invalid JSON number");
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ProtocolFailure("MalformedEnvelope", "JSON number has a leading zero");
            }
        } else {
            if (!std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ProtocolFailure("MalformedEnvelope", "invalid JSON number");
            }
            while (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ProtocolFailure("MalformedEnvelope", "invalid JSON fraction");
            }
            while (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ProtocolFailure("MalformedEnvelope", "invalid JSON exponent");
            }
            while (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ == start) {
            ProtocolFailure("MalformedEnvelope", "invalid JSON number");
        }
        JsonValue value;
        value.kind = JsonKind::number;
        return value;
    }

    [[nodiscard]] JsonValue ParseNull() {
        if (input_.substr(position_, 4) != "null") {
            ProtocolFailure("MalformedEnvelope", "invalid JSON null");
        }
        position_ += 4;
        JsonValue value;
        value.kind = JsonKind::null_value;
        return value;
    }

    void SkipWhitespace() {
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (character != ' ' && character != '\t' &&
                character != '\r' && character != '\n') {
                break;
            }
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
};

const JsonValue& RequireField(
    const JsonValue& object,
    std::string_view name,
    JsonKind kind) {
    if (object.kind != JsonKind::object) {
        ProtocolFailure(
            "InvalidFieldType", "Dual hardware protocol value must be an object");
    }
    const auto found = object.object.find(std::string(name));
    if (found == object.object.end()) {
        ProtocolFailure(
            "UnexpectedField", "Dual hardware protocol object has a missing field");
    }
    if (found->second.kind != kind) {
        ProtocolFailure(
            "InvalidFieldType", "Dual hardware protocol field has an invalid type");
    }
    return found->second;
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
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f') ||
                (character >= 'A' && character <= 'F');
        });
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

std::string CapabilitiesResponse(std::string_view request_id) {
    return ResponsePrefix(request_id, true, "DualCapabilities") +
        "{\"cameraMode\":\"DualCamera\",\"protocolVersion\":2,"
        "\"orderedRequiredAliases\":[\"CAM-A\",\"CAM-B\"],"
        "\"supportedOperations\":[\"get-dual-capabilities\","
        "\"reserve-pair-transaction\",\"start-reserved-pair\","
        "\"get-pair-transaction-result\"],\"pairJournalDurable\":true,"
        "\"sameTransactionQueryOnly\":true,\"automaticRetryCount\":0}}";
}

std::string ReservationUnavailableResponse(
    std::string_view request_id,
    std::string_view transaction_id) {
    return ResponsePrefix(request_id, false, "PairStoreUnavailable") +
        "{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"accepted\":false}}";
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
    ProtocolFailure(
        "UnsupportedOperation", "Dual hardware protocol operation is unsupported");
}

std::string DualHardwareCameraAgentDispatcher::Handle(
    std::string_view request_json) noexcept {
    const std::string extracted_request_id =
        TryExtractSafeRequestId(request_json);
    try {
        const auto request =
            ParseDualHardwareCameraAgentRequest(request_json);
        switch (request.operation) {
        case DualHardwareCameraAgentOperation::get_dual_capabilities:
            return CapabilitiesResponse(request.request_id);
        case DualHardwareCameraAgentOperation::reserve_pair_transaction:
            return ReservationUnavailableResponse(
                request.request_id, request.transaction_id);
        case DualHardwareCameraAgentOperation::start_reserved_pair:
            return StartUnavailableResponse(
                request.request_id, request.transaction_id);
        case DualHardwareCameraAgentOperation::get_pair_transaction_result:
            return QueryUnavailableResponse(
                request.request_id, request.transaction_id);
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
    return {};
}

} // namespace a0::phase0
