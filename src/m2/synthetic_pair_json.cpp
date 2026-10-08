#include "a0/m2/synthetic_pair_json.hpp"

#include <charconv>
#include <cmath>
#include <stdexcept>

namespace a0::m2::synthetic {

namespace {

constexpr int kMaximumDepth = 32;

class Parser final {
public:
    explicit Parser(const std::string_view text) : text_(text) {}

    JsonValue ParseDocument() {
        SkipWhitespace();
        auto value = ParseValue(0);
        SkipWhitespace();
        if (position_ != text_.size()) Fail("unexpected content after the JSON value");
        return value;
    }

private:
    [[noreturn]] void Fail(const char* message) const {
        throw std::invalid_argument(
            std::string("JSON error at offset ") + std::to_string(position_) + ": " + message);
    }

    void SkipWhitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++position_;
        }
    }

    [[nodiscard]] bool Consume(const char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void ExpectLiteral(const std::string_view literal) {
        if (text_.substr(position_, literal.size()) != literal) Fail("invalid literal");
        position_ += literal.size();
    }

    JsonValue ParseValue(const int depth) {
        if (position_ >= text_.size()) Fail("unexpected end of input");
        const char c = text_[position_];
        JsonValue value;
        // Objects and arrays nest at most kMaximumDepth deep; depth 0 is the outermost.
        if ((c == '{' || c == '[') && depth >= kMaximumDepth) Fail("nesting is too deep");
        if (c == '{') {
            ParseObject(value, depth);
        } else if (c == '[') {
            ParseArray(value, depth);
        } else if (c == '"') {
            value.kind = JsonValue::Kind::string;
            value.text = ParseString();
        } else if (c == 't') {
            ExpectLiteral("true");
            value.kind = JsonValue::Kind::boolean;
            value.boolean = true;
        } else if (c == 'f') {
            ExpectLiteral("false");
            value.kind = JsonValue::Kind::boolean;
        } else if (c == 'n') {
            ExpectLiteral("null");
        } else {
            value.kind = JsonValue::Kind::number;
            value.text = ParseNumber();
        }
        return value;
    }

    void ParseObject(JsonValue& value, const int depth) {
        value.kind = JsonValue::Kind::object;
        ++position_;
        SkipWhitespace();
        if (Consume('}')) return;
        for (;;) {
            SkipWhitespace();
            if (position_ >= text_.size() || text_[position_] != '"') Fail("object key must be a string");
            auto key = ParseString();
            for (const auto& existing : value.keys) {
                if (existing == key) Fail("duplicate object key");
            }
            SkipWhitespace();
            if (!Consume(':')) Fail("expected ':' after object key");
            SkipWhitespace();
            auto member = ParseValue(depth + 1);
            value.keys.push_back(std::move(key));
            value.values.push_back(std::move(member));
            SkipWhitespace();
            if (Consume(',')) continue;
            if (Consume('}')) return;
            Fail("expected ',' or '}' in object");
        }
    }

    void ParseArray(JsonValue& value, const int depth) {
        value.kind = JsonValue::Kind::array;
        ++position_;
        SkipWhitespace();
        if (Consume(']')) return;
        for (;;) {
            SkipWhitespace();
            value.items.push_back(ParseValue(depth + 1));
            SkipWhitespace();
            if (Consume(',')) continue;
            if (Consume(']')) return;
            Fail("expected ',' or ']' in array");
        }
    }

    [[nodiscard]] unsigned ParseHex4() {
        if (position_ + 4U > text_.size()) Fail("truncated \\u escape");
        unsigned code = 0;
        for (int index = 0; index < 4; ++index) {
            const char c = text_[position_++];
            code <<= 4U;
            if (c >= '0' && c <= '9') code |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<unsigned>(c - 'A' + 10);
            else Fail("invalid \\u escape");
        }
        return code;
    }

    static void AppendUtf8(std::string& output, const unsigned code) {
        if (code < 0x80U) {
            output.push_back(static_cast<char>(code));
        } else if (code < 0x800U) {
            output.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xE0U | (code >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
    }

    std::string ParseString() {
        ++position_;
        std::string output;
        for (;;) {
            if (position_ >= text_.size()) Fail("unterminated string");
            const char c = text_[position_++];
            if (c == '"') return output;
            if (static_cast<unsigned char>(c) < 0x20U) Fail("control character in string");
            if (c != '\\') {
                output.push_back(c);
                continue;
            }
            if (position_ >= text_.size()) Fail("unterminated escape");
            const char escape = text_[position_++];
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                const auto code = ParseHex4();
                if (code >= 0xD800U && code <= 0xDFFFU) Fail("surrogate pairs are not supported");
                AppendUtf8(output, code);
                break;
            }
            default: Fail("invalid escape");
            }
        }
    }

    std::string ParseNumber() {
        const auto start = position_;
        (void)Consume('-');
        if (position_ >= text_.size()) Fail("invalid number");
        if (text_[position_] == '0') {
            ++position_;
        } else if (text_[position_] >= '1' && text_[position_] <= '9') {
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        } else {
            Fail("invalid number");
        }
        if (Consume('.')) {
            const auto digits = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (position_ == digits) Fail("invalid fraction");
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            const auto digits = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (position_ == digits) Fail("invalid exponent");
        }
        return std::string(text_.substr(start, position_ - start));
    }

    std::string_view text_;
    std::size_t position_{};
};

} // namespace

const JsonValue* JsonValue::Find(const std::string_view key) const noexcept {
    if (kind != Kind::object) return nullptr;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (keys[index] == key) return &values[index];
    }
    return nullptr;
}

double JsonValue::AsDouble() const {
    if (kind != Kind::number) throw std::invalid_argument("JSON value is not a number");
    double parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(parsed)) {
        throw std::invalid_argument("JSON number is not a finite double");
    }
    return parsed;
}

std::uint64_t JsonValue::AsUnsigned() const {
    if (kind != Kind::number) throw std::invalid_argument("JSON value is not a number");
    std::uint64_t parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::invalid_argument("JSON number is not an unsigned 64-bit integer");
    }
    return parsed;
}

const std::string& JsonValue::AsString() const {
    if (kind != Kind::string) throw std::invalid_argument("JSON value is not a string");
    return text;
}

JsonValue ParseJson(const std::string_view text) {
    return Parser(text).ParseDocument();
}

} // namespace a0::m2::synthetic
