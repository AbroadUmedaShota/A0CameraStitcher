#pragma once

// Shared JSON envelope handling for the Camera Agent protocols.
//
// This lived privately inside dual_hardware_camera_agent.cpp until the binding
// protocol (`a0.camera-agent.hardware-dual-binding.v1`, GitHub Issue #61) needed
// the same parser. Two hand-written JSON parsers in one codebase is how the two
// protocols end up disagreeing about what a malformed envelope is, so the code
// moved here unchanged rather than being copied.
//
// Failures are reported through a policy type instead of a fixed exception,
// because each protocol has its own error type and its own dispatcher catch.
// Passing the policy as a template argument keeps every existing call site
// byte-for-byte identical: the v2 translation unit aliases these templates with
// its own policy and nothing else about it changes.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a0::phase0::protocol_json {

constexpr std::size_t kMaximumProtocolJsonBytes = 256U * 1024U;
constexpr int kMaximumProtocolJsonDepth = 32;

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
    // For JsonKind::number this holds the raw lexeme, not a converted value.
    // Callers convert it themselves, which is what lets them refuse "1.0" or
    // "1e0" where only a whole number is meaningful -- a distinction a double
    // would have already thrown away.
    std::string string;
    bool boolean{};
};

inline void AppendUtf8(std::string& output, std::uint32_t code_point) {
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


template <typename Failure>
class BasicJsonParser final {
public:
    explicit BasicJsonParser(std::string_view input) : input_(input) {
        if (input.empty() || input.size() > kMaximumProtocolJsonBytes) {
            Failure::Fail(
                "MalformedEnvelope",
                "Dual hardware protocol JSON length is outside the allowed range");
        }
    }

    [[nodiscard]] JsonValue Parse() {
        SkipWhitespace();
        JsonValue value = ParseValue(0);
        SkipWhitespace();
        if (position_ != input_.size()) {
            Failure::Fail(
                "MalformedEnvelope",
                "unexpected data follows the Dual hardware protocol JSON value");
        }
        return value;
    }

private:
    [[nodiscard]] JsonValue ParseValue(int depth) {
        if (depth > kMaximumProtocolJsonDepth || position_ >= input_.size()) {
            Failure::Fail(
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
        Failure::Fail(
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
                Failure::Fail(
                    "MalformedEnvelope",
                    "Dual hardware protocol JSON object key is invalid");
            }
            std::string key = ParseString();
            SkipWhitespace();
            if (!Consume(':')) {
                Failure::Fail(
                    "MalformedEnvelope",
                    "Dual hardware protocol JSON object is missing ':'");
            }
            SkipWhitespace();
            JsonValue child = ParseValue(depth);
            if (!value.object.emplace(std::move(key), std::move(child)).second) {
                Failure::Fail(
                    "DuplicateField",
                    "Dual hardware protocol JSON contains a duplicate field");
            }
            SkipWhitespace();
            if (Consume('}')) break;
            if (!Consume(',')) {
                Failure::Fail(
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
                Failure::Fail(
                    "MalformedEnvelope",
                    "Dual hardware protocol JSON array is missing ','");
            }
            SkipWhitespace();
        }
        return value;
    }

    [[nodiscard]] std::uint32_t ParseHexQuad() {
        if (input_.size() - position_ < 4) {
            Failure::Fail("MalformedEnvelope", "incomplete JSON unicode escape");
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
                Failure::Fail("MalformedEnvelope", "invalid JSON unicode escape");
            }
        }
        return value;
    }

    [[nodiscard]] std::string ParseString() {
        if (!Consume('"')) {
            Failure::Fail(
                "MalformedEnvelope", "Dual hardware protocol JSON string is invalid");
        }
        std::string output;
        while (position_ < input_.size()) {
            const unsigned char character =
                static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return output;
            if (character < 0x20U) {
                Failure::Fail(
                    "MalformedEnvelope",
                    "Dual hardware protocol JSON string contains a control character");
            }
            if (character != '\\') {
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= input_.size()) {
                Failure::Fail("MalformedEnvelope", "incomplete JSON escape");
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
                        Failure::Fail(
                            "MalformedEnvelope", "incomplete JSON surrogate pair");
                    }
                    position_ += 2;
                    const std::uint32_t low = ParseHexQuad();
                    if (low < 0xDC00U || low > 0xDFFFU) {
                        Failure::Fail(
                            "MalformedEnvelope", "invalid JSON surrogate pair");
                    }
                    code_point = 0x10000U +
                        ((code_point - 0xD800U) << 10U) +
                        (low - 0xDC00U);
                } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
                    Failure::Fail(
                        "MalformedEnvelope", "unexpected JSON low surrogate");
                }
                AppendUtf8(output, code_point);
                break;
            }
            default:
                Failure::Fail("MalformedEnvelope", "unsupported JSON escape");
            }
        }
        Failure::Fail(
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
        Failure::Fail("MalformedEnvelope", "invalid JSON boolean");
    }

    [[nodiscard]] JsonValue ParseNumber() {
        const std::size_t start = position_;
        if (input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) {
            Failure::Fail("MalformedEnvelope", "invalid JSON number");
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                Failure::Fail("MalformedEnvelope", "JSON number has a leading zero");
            }
        } else {
            if (!std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                Failure::Fail("MalformedEnvelope", "invalid JSON number");
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
                Failure::Fail("MalformedEnvelope", "invalid JSON fraction");
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
                Failure::Fail("MalformedEnvelope", "invalid JSON exponent");
            }
            while (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ == start) {
            Failure::Fail("MalformedEnvelope", "invalid JSON number");
        }
        JsonValue value;
        value.kind = JsonKind::number;
        value.string = std::string(input_.substr(start, position_ - start));
        return value;
    }

    [[nodiscard]] JsonValue ParseNull() {
        if (input_.substr(position_, 4) != "null") {
            Failure::Fail("MalformedEnvelope", "invalid JSON null");
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

template <typename Failure>
const JsonValue& RequireFieldWith(
    const JsonValue& object,
    std::string_view name,
    JsonKind kind) {
    if (object.kind != JsonKind::object) {
        Failure::Fail(
            "InvalidFieldType", "Dual hardware protocol value must be an object");
    }
    const auto found = object.object.find(std::string(name));
    if (found == object.object.end()) {
        Failure::Fail(
            "UnexpectedField", "Dual hardware protocol object has a missing field");
    }
    if (found->second.kind != kind) {
        Failure::Fail(
            "InvalidFieldType", "Dual hardware protocol field has an invalid type");
    }
    return found->second;
}

// Not a template, unlike everything around it: escaping has no failure mode to
// route through a policy. It needs `inline` because more than one translation
// unit includes this header.
inline std::string JsonEscape(std::string_view value) {
    std::ostringstream output;
    for (unsigned char character : value) {
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
                constexpr char digits[] = "0123456789abcdef";
                output << "\\u00" << digits[character >> 4] << digits[character & 15];
            } else output << static_cast<char>(character);
        }
    }
    return output.str();
}


template <typename Failure>
std::string SerializeJsonWith(const JsonValue& value) {
    switch (value.kind) {
    case JsonKind::string: return "\"" + JsonEscape(value.string) + "\"";
    case JsonKind::boolean: return value.boolean ? "true" : "false";
    case JsonKind::number: return value.string;
    case JsonKind::null_value: return "null";
    case JsonKind::array: {
        std::string result = "[";
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            if (index != 0) result += ',';
            result += SerializeJsonWith<Failure>(value.array[index]);
        }
        return result + "]";
    }
    case JsonKind::object: {
        std::string result = "{"; bool first = true;
        for (const auto& [key, child] : value.object) {
            if (!first) result += ','; first = false;
            result += "\"" + JsonEscape(key) + "\":" + SerializeJsonWith<Failure>(child);
        }
        return result + "}";
    }
    }
    Failure::Fail("AgentFailure", "JSON value kind is unsupported");
}


} // namespace a0::phase0::protocol_json
