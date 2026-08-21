#include "a0/m2/stitch_metrics.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a0::m2 {
namespace {

constexpr std::size_t kMaximumDocumentBytes = 64U * 1024U;
constexpr std::size_t kMaximumDepth = 32;
constexpr int kMaximumDecimalPlaces = 9;
// JSON integer counters stay below 2^53 so their contract remains exactly
// representable if consumed by binary64 clients as well as this exact parser.
constexpr std::uint64_t kMaximumExactInteger = (1ULL << 53U) - 1U;

// This is a representation-safety boundary, not a product-quality limit.
// At |scaled| <= 2^39, binary64 has ULP <= 2^-13, so the 0.5 rounding
// boundary is separated by 4096 ULPs (and adjacent integer quanta by 8192
// ULPs).  This leaves twelve binary orders of headroom before ULP reaches
// 0.5 at 2^51, while also rejecting the historical 2^39+1 regression input.
constexpr std::uint64_t kExactSafeScaledMagnitude = 1ULL << 39U;
constexpr std::uint64_t kPowerOfTen[] = {
    1ULL, 10ULL, 100ULL, 1'000ULL, 10'000ULL, 100'000ULL,
    1'000'000ULL, 10'000'000ULL, 100'000'000ULL, 1'000'000'000ULL};

struct DecimalParts final {
    bool negative{};
    std::string digits;
    int scale{};
};

bool ParseDecimalParts(const std::string_view token, DecimalParts& parts) {
    if (token.empty()) return false;
    std::size_t position = 0;
    parts.negative = false;
    if (token[position] == '-') {
        parts.negative = true;
        ++position;
    }
    const std::size_t exponent_position = token.find_first_of("eE", position);
    const std::size_t mantissa_end = exponent_position == std::string_view::npos
        ? token.size() : exponent_position;
    if (position >= mantissa_end) return false;

    const std::size_t decimal_position = token.find('.', position);
    const std::size_t fraction_start = decimal_position == std::string_view::npos
        ? mantissa_end : decimal_position + 1U;
    const std::size_t fraction_digits = decimal_position == std::string_view::npos
        ? 0U : mantissa_end - fraction_start;

    parts.digits.clear();
    parts.digits.reserve(mantissa_end - position);
    for (std::size_t index = position; index < mantissa_end; ++index) {
        if (token[index] == '.') continue;
        if (token[index] < '0' || token[index] > '9') return false;
        parts.digits.push_back(token[index]);
    }
    if (parts.digits.empty()) return false;
    const auto first_nonzero = parts.digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) {
        parts.digits = "0";
        parts.scale = 0;
        parts.negative = false;
        return true;
    }
    parts.digits.erase(0, first_nonzero);

    int exponent = 0;
    if (exponent_position != std::string_view::npos) {
        std::size_t exponent_cursor = exponent_position + 1U;
        bool exponent_negative = false;
        if (exponent_cursor < token.size() &&
            (token[exponent_cursor] == '+' || token[exponent_cursor] == '-')) {
            exponent_negative = token[exponent_cursor] == '-';
            ++exponent_cursor;
        }
        if (exponent_cursor >= token.size()) return false;
        for (; exponent_cursor < token.size(); ++exponent_cursor) {
            const char digit = token[exponent_cursor];
            if (digit < '0' || digit > '9') return false;
            constexpr int kExponentLimit = std::numeric_limits<int>::max();
            const int numeric_digit = digit - '0';
            if (exponent > (kExponentLimit - numeric_digit) / 10) return false;
            exponent = exponent * 10 + numeric_digit;
        }
        if (exponent_negative) exponent = -exponent;
    }
    const auto scale = static_cast<std::int64_t>(fraction_digits) - exponent;
    if (scale < std::numeric_limits<int>::min() || scale > std::numeric_limits<int>::max()) {
        return false;
    }
    parts.scale = static_cast<int>(scale);
    return true;
}

bool IsDecimalTokenUnderflow(const std::string_view token) {
    DecimalParts parts;
    if (!ParseDecimalParts(token, parts) || parts.digits == "0") return false;
    const auto leading_decimal_exponent =
        static_cast<std::int64_t>(parts.digits.size()) - 1 - parts.scale;
    // A finite decimal below the smallest binary64 subnormal may be retained
    // lexically for exact rawValue rounding, but must not become generic zero.
    return leading_decimal_exponent <= -324;
}

struct JsonValue final {
    enum class Type { null_value, boolean, number, string, array, object };
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;

    Type type{Type::null_value};
    bool boolean{};
    double number{};
    bool number_underflow{};
    std::string string;
    Array array;
    Object object;
    // Retain the JSON number spelling so decimal half ties are classified
    // exactly instead of by an epsilon applied to a binary64 product.
    std::string number_text;
};

class JsonParser final {
public:
    explicit JsonParser(const std::string_view input) : input_(input) {
        if (input.empty() || input.size() > kMaximumDocumentBytes) {
            throw std::invalid_argument("metric JSON size is invalid");
        }
    }

    JsonValue Parse() {
        JsonValue value = ParseValue(0);
        SkipWhitespace();
        if (position_ != input_.size()) throw std::invalid_argument("metric JSON has trailing data");
        return value;
    }

private:
    void SkipWhitespace() {
        while (position_ < input_.size()) {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
            ++position_;
        }
    }

    char Take() {
        if (position_ >= input_.size()) throw std::invalid_argument("metric JSON ended unexpectedly");
        return input_[position_++];
    }

    bool Consume(const std::string_view token) {
        if (input_.substr(position_, token.size()) != token) return false;
        position_ += token.size();
        return true;
    }

    JsonValue ParseValue(const std::size_t depth) {
        if (depth > kMaximumDepth) throw std::invalid_argument("metric JSON nesting is too deep");
        SkipWhitespace();
        if (position_ >= input_.size()) throw std::invalid_argument("metric JSON value is missing");
        switch (input_[position_]) {
        case 'n':
            if (!Consume("null")) throw std::invalid_argument("invalid null token");
            return {};
        case 't':
            if (!Consume("true")) throw std::invalid_argument("invalid true token");
            return JsonValue{JsonValue::Type::boolean, true};
        case 'f':
            if (!Consume("false")) throw std::invalid_argument("invalid false token");
            return JsonValue{JsonValue::Type::boolean, false};
        case '"': {
            JsonValue value;
            value.type = JsonValue::Type::string;
            value.string = ParseString();
            return value;
        }
        case '[':
            return ParseArray(depth + 1);
        case '{':
            return ParseObject(depth + 1);
        default:
            return ParseNumber();
        }
    }

    static int HexValue(const char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    std::uint32_t ParseHexQuad() {
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = HexValue(Take());
            if (digit < 0) throw std::invalid_argument("invalid JSON unicode escape");
            value = value * 16U + static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    static void AppendUtf8(std::string& output, const std::uint32_t code_point) {
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

    std::string ParseString() {
        if (Take() != '"') throw std::invalid_argument("JSON string opening quote is missing");
        std::string output;
        while (true) {
            const unsigned char value = static_cast<unsigned char>(Take());
            if (value == '"') break;
            if (value < 0x20U) throw std::invalid_argument("JSON string contains a control character");
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            switch (Take()) {
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
                    if (Take() != '\\' || Take() != 'u') {
                        throw std::invalid_argument("JSON high surrogate is unpaired");
                    }
                    const std::uint32_t low = ParseHexQuad();
                    if (low < 0xDC00U || low > 0xDFFFU) {
                        throw std::invalid_argument("JSON surrogate pair is invalid");
                    }
                    code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
                } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
                    throw std::invalid_argument("JSON low surrogate is unpaired");
                }
                AppendUtf8(output, code_point);
                break;
            }
            default:
                throw std::invalid_argument("JSON string escape is invalid");
            }
            if (output.size() > kMaximumDocumentBytes) {
                throw std::invalid_argument("JSON string is too long");
            }
        }
        return output;
    }

    JsonValue ParseNumber() {
        const std::size_t start = position_;
        if (input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) throw std::invalid_argument("JSON number is incomplete");
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                throw std::invalid_argument("JSON number has a leading zero");
            }
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') {
                throw std::invalid_argument("JSON number integer part is invalid");
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t fraction_start = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (fraction_start == position_) throw std::invalid_argument("JSON number fraction is empty");
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const std::size_t exponent_start = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (exponent_start == position_) throw std::invalid_argument("JSON number exponent is empty");
        }
        double number = 0.0;
        const auto token = input_.substr(start, position_ - start);
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
        bool number_underflow = false;
        if (parsed.ec == std::errc::result_out_of_range && IsDecimalTokenUnderflow(token)) {
            number = 0.0;
            number_underflow = true;
        } else if (parsed.ec != std::errc{} ||
            parsed.ptr != token.data() + token.size() || !std::isfinite(number)) {
            throw std::invalid_argument("JSON number is not finite");
        }
        JsonValue value;
        value.type = JsonValue::Type::number;
        value.number = number;
        value.number_underflow = number_underflow;
        value.number_text = std::string(token);
        return value;
    }

    JsonValue ParseArray(const std::size_t depth) {
        (void)Take();
        JsonValue value;
        value.type = JsonValue::Type::array;
        SkipWhitespace();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            return value;
        }
        while (true) {
            value.array.push_back(ParseValue(depth));
            SkipWhitespace();
            const char separator = Take();
            if (separator == ']') break;
            if (separator != ',') throw std::invalid_argument("JSON array separator is invalid");
        }
        return value;
    }

    JsonValue ParseObject(const std::size_t depth) {
        (void)Take();
        JsonValue value;
        value.type = JsonValue::Type::object;
        SkipWhitespace();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            return value;
        }
        while (true) {
            SkipWhitespace();
            if (position_ >= input_.size() || input_[position_] != '"') {
                throw std::invalid_argument("JSON object key is not a string");
            }
            std::string key = ParseString();
            SkipWhitespace();
            if (Take() != ':') throw std::invalid_argument("JSON object colon is missing");
            auto [iterator, inserted] = value.object.emplace(std::move(key), ParseValue(depth));
            (void)iterator;
            if (!inserted) throw std::invalid_argument("JSON object contains a duplicate key");
            SkipWhitespace();
            const char separator = Take();
            if (separator == '}') break;
            if (separator != ',') throw std::invalid_argument("JSON object separator is invalid");
        }
        return value;
    }

    std::string_view input_;
    std::size_t position_{};
};

using Object = JsonValue::Object;

const JsonValue* Find(const Object& object, const std::string_view name) {
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

bool ExactFields(const Object& object, const std::initializer_list<std::string_view> fields) {
    if (object.size() != fields.size()) return false;
    return std::all_of(fields.begin(), fields.end(), [&](const auto field) {
        return object.contains(field);
    });
}

bool IsNull(const JsonValue* value) {
    return value != nullptr && value->type == JsonValue::Type::null_value;
}

bool IsNumber(const JsonValue* value) {
    return value != nullptr && value->type == JsonValue::Type::number &&
        !value->number_underflow && std::isfinite(value->number);
}

bool IsJsonNumber(const JsonValue* value) {
    return value != nullptr && value->type == JsonValue::Type::number &&
        !value->number_text.empty();
}

struct ExactInteger final {
    bool negative{};
    std::string digits{"0"};
    std::uint64_t trailing_zeros{};
};

bool TryExactInteger(const JsonValue* value, ExactInteger& integer) {
    if (!IsJsonNumber(value)) return false;
    DecimalParts parts;
    if (!ParseDecimalParts(value->number_text, parts)) return false;
    if (parts.digits == "0") {
        integer = {};
        return true;
    }
    std::uint64_t trailing_zeros = parts.scale < 0
        ? static_cast<std::uint64_t>(-(static_cast<std::int64_t>(parts.scale))) : 0U;
    if (parts.scale > 0) {
        const auto scale = static_cast<std::size_t>(parts.scale);
        if (scale >= parts.digits.size() ||
            !std::all_of(parts.digits.end() - scale, parts.digits.end(),
                [](const char digit) { return digit == '0'; })) {
            return false;
        }
        parts.digits.resize(parts.digits.size() - scale);
    }
    while (parts.digits.size() > 1U && parts.digits.back() == '0') {
        parts.digits.pop_back();
        if (trailing_zeros != std::numeric_limits<std::uint64_t>::max()) ++trailing_zeros;
    }
    integer.negative = parts.negative;
    integer.digits = std::move(parts.digits);
    integer.trailing_zeros = trailing_zeros;
    return true;
}

std::uint64_t ExactIntegerLength(const ExactInteger& integer) {
    const auto digits = static_cast<std::uint64_t>(integer.digits.size());
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return integer.trailing_zeros > maximum - digits
        ? maximum : digits + integer.trailing_zeros;
}

int CompareExactIntegerMagnitude(const ExactInteger& left, const ExactInteger& right) {
    const auto left_length = ExactIntegerLength(left);
    const auto right_length = ExactIntegerLength(right);
    if (left_length < right_length) return -1;
    if (left_length > right_length) return 1;
    const auto common_digits = std::max(left.digits.size(), right.digits.size());
    for (std::size_t index = 0; index < common_digits; ++index) {
        const char left_digit = index < left.digits.size() ? left.digits[index] : '0';
        const char right_digit = index < right.digits.size() ? right.digits[index] : '0';
        if (left_digit < right_digit) return -1;
        if (left_digit > right_digit) return 1;
    }
    return 0;
}

int CompareExactIntegers(const ExactInteger& left, const ExactInteger& right) {
    if (left.negative != right.negative) return left.negative ? -1 : 1;
    const int magnitude = CompareExactIntegerMagnitude(left, right);
    return left.negative ? -magnitude : magnitude;
}

bool IsExactInteger(const JsonValue* value, const std::uint64_t expected) {
    ExactInteger actual;
    if (!TryExactInteger(value, actual) || actual.negative) return false;
    const ExactInteger target{false, std::to_string(expected), 0U};
    return CompareExactIntegers(actual, target) == 0;
}

bool TryNonnegativeExactInteger(const JsonValue* value, ExactInteger& integer) {
    if (!TryExactInteger(value, integer) || integer.negative) return false;
    const ExactInteger maximum{false, std::to_string(kMaximumExactInteger), 0U};
    return CompareExactIntegers(integer, maximum) <= 0;
}

bool IsString(const JsonValue* value, const std::string_view expected) {
    return value != nullptr && value->type == JsonValue::Type::string && value->string == expected;
}

bool IsOneOf(const JsonValue* value, const std::initializer_list<std::string_view> choices) {
    if (value == nullptr || value->type != JsonValue::Type::string) return false;
    return std::find(choices.begin(), choices.end(), value->string) != choices.end();
}

bool IsMetricId(const JsonValue* value) {
    if (value == nullptr || value->type != JsonValue::Type::string ||
        value->string.empty() || value->string.size() > 64 ||
        value->string.front() < 'a' || value->string.front() > 'z') {
        return false;
    }
    return std::all_of(value->string.begin(), value->string.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '-';
    });
}

bool IsFailureCode(const JsonValue* value) {
    return IsMetricId(value);
}

struct RoundingSpec final {
    std::string_view mode;
    int decimal_places{};
};

bool ValidateRounding(const JsonValue* value, RoundingSpec& spec) {
    if (value == nullptr || value->type != JsonValue::Type::object ||
        !ExactFields(value->object, {"mode", "decimalPlaces", "order"}) ||
        !IsOneOf(Find(value->object, "mode"),
            {"HalfToEven", "HalfAwayFromZero", "TowardZero"}) ||
        !IsString(Find(value->object, "order"), "AggregateThenRound")) {
        return false;
    }
    const JsonValue* decimals = Find(value->object, "decimalPlaces");
    const JsonValue* mode = Find(value->object, "mode");
    ExactInteger decimal_value;
    const ExactInteger minimum_decimal{false, "0", 0U};
    const ExactInteger maximum_decimal{
        false, std::to_string(kMaximumDecimalPlaces), 0U};
    if (!TryExactInteger(decimals, decimal_value) || decimal_value.negative ||
        CompareExactIntegers(decimal_value, minimum_decimal) < 0 ||
        CompareExactIntegers(decimal_value, maximum_decimal) > 0 ||
        decimal_value.trailing_zeros != 0U || mode == nullptr) {
        return false;
    }
    spec = {mode->string, decimal_value.digits.front() - '0'};
    return true;
}

bool SameRounding(const JsonValue* left, const JsonValue* right) {
    RoundingSpec left_spec;
    RoundingSpec right_spec;
    return ValidateRounding(left, left_spec) && ValidateRounding(right, right_spec) &&
        left_spec.mode == right_spec.mode &&
        left_spec.decimal_places == right_spec.decimal_places;
}

bool ParseBoundedDigits(
    const std::string_view digits,
    const std::uint64_t maximum,
    std::uint64_t& value) {
    value = 0;
    for (const char digit : digits) {
        if (digit < '0' || digit > '9') return false;
        const auto numeric_digit = static_cast<std::uint64_t>(digit - '0');
        if (numeric_digit > maximum || value > (maximum - numeric_digit) / 10U) {
            return false;
        }
        value = value * 10U + numeric_digit;
    }
    return true;
}

enum class FractionRelation { LessThanHalf, EqualToHalf, GreaterThanHalf };

FractionRelation CompareFractionToHalf(
    const std::string_view fractional_digits,
    const std::size_t denominator_digits) {
    if (denominator_digits == 0U || fractional_digits.empty()) {
        return FractionRelation::LessThanHalf;
    }
    // The fraction is the suffix padded on the left to denominator_digits.
    // If it needs padding, its first digit is zero and is necessarily below
    // one half. Otherwise compare the decimal digits with 5000... exactly.
    if (fractional_digits.size() < denominator_digits) {
        return FractionRelation::LessThanHalf;
    }
    const char first = fractional_digits.front();
    if (first < '5') return FractionRelation::LessThanHalf;
    if (first > '5') return FractionRelation::GreaterThanHalf;
    for (std::size_t index = 1; index < denominator_digits; ++index) {
        if (fractional_digits[index] != '0') return FractionRelation::GreaterThanHalf;
    }
    return FractionRelation::EqualToHalf;
}

struct ExactScaledValue final {
    bool negative{};
    std::uint64_t integer_part{};
    bool fraction_nonzero{};
    FractionRelation fraction_relation{FractionRelation::LessThanHalf};
};

ExactScaledValue ParseExactScaledValue(
    const JsonValue& raw,
    const RoundingSpec& spec) {
    if (!IsJsonNumber(&raw)) {
        throw std::invalid_argument("raw metric is not a finite JSON number");
    }
    DecimalParts parts;
    if (!ParseDecimalParts(raw.number_text, parts)) {
        throw std::invalid_argument("raw metric decimal spelling is invalid");
    }
    if (parts.digits == "0") return {};

    const std::int64_t shift = static_cast<std::int64_t>(spec.decimal_places) - parts.scale;
    ExactScaledValue scaled;
    scaled.negative = parts.negative;
    if (shift >= 0) {
        if (shift > 18) {
            throw std::invalid_argument("scaled metric exceeds exact-safe range");
        }
        std::uint64_t multiplier = 1U;
        for (std::int64_t index = 0; index < shift; ++index) multiplier *= 10U;
        std::uint64_t coefficient = 0;
        if (!ParseBoundedDigits(
                parts.digits, kExactSafeScaledMagnitude / multiplier, coefficient)) {
            throw std::invalid_argument("scaled metric exceeds exact-safe range");
        }
        scaled.integer_part = coefficient * multiplier;
    } else {
        const auto denominator_digits = static_cast<std::size_t>(-shift);
        const std::size_t quotient_length = parts.digits.size() > denominator_digits
            ? parts.digits.size() - denominator_digits : 0U;
        const std::string_view quotient_digits(parts.digits.data(), quotient_length);
        if (!ParseBoundedDigits(quotient_digits, kExactSafeScaledMagnitude,
                scaled.integer_part)) {
            throw std::invalid_argument("scaled metric exceeds exact-safe range");
        }
        const std::string_view fraction_digits(
            parts.digits.data() + quotient_length,
            parts.digits.size() - quotient_length);
        scaled.fraction_nonzero = std::any_of(
            fraction_digits.begin(), fraction_digits.end(), [](const char digit) {
                return digit != '0';
            });
        scaled.fraction_relation = CompareFractionToHalf(fraction_digits, denominator_digits);
    }

    if (scaled.integer_part > kExactSafeScaledMagnitude ||
        (scaled.integer_part == kExactSafeScaledMagnitude && scaled.fraction_nonzero)) {
        throw std::invalid_argument("scaled metric exceeds exact-safe range");
    }
    return scaled;
}

double RoundFromRaw(const JsonValue& raw, const RoundingSpec& spec) {
    const ExactScaledValue scaled = ParseExactScaledValue(raw, spec);
    std::uint64_t rounded_absolute = scaled.integer_part;
    if (spec.mode == "HalfAwayFromZero") {
        if (scaled.fraction_relation != FractionRelation::LessThanHalf) ++rounded_absolute;
    } else if (spec.mode == "HalfToEven" &&
        (scaled.fraction_relation == FractionRelation::GreaterThanHalf ||
            (scaled.fraction_relation == FractionRelation::EqualToHalf &&
                (scaled.integer_part & 1U) != 0U))) {
        ++rounded_absolute;
    }
    if (rounded_absolute > kExactSafeScaledMagnitude) {
        throw std::invalid_argument("rounded metric exceeds exact-safe range");
    }
    const double factor = static_cast<double>(kPowerOfTen[spec.decimal_places]);
    const double integral = static_cast<double>(rounded_absolute);
    return std::copysign(integral / factor, scaled.negative ? -1.0 : 1.0);
}

bool MatchesRoundedValue(
    const JsonValue* raw,
    const JsonValue* rounded,
    const RoundingSpec& spec) {
    if (!IsJsonNumber(raw) || !IsNumber(rounded)) return false;
    double expected = 0.0;
    try {
        expected = RoundFromRaw(*raw, spec);
    } catch (...) {
        // A finite JSON number outside the exact-safe contract is a value
        // violation, not malformed JSON; the caller reports metric_value_invalid.
        return false;
    }
    // The rounded value is a contract value, not an approximate display.
    // Exact binary64 equality rejects a one-quantum mismatch at every scale.
    return std::isfinite(expected) && expected == rounded->number;
}

bool ValidateAggregation(const JsonValue* value) {
    if (value == nullptr || value->type != JsonValue::Type::object ||
        !ExactFields(value->object,
            {"method", "percentile", "algorithm", "percentileInterpolation", "medianEvenRule"})) {
        return false;
    }
    const JsonValue* method = Find(value->object, "method");
    const JsonValue* percentile = Find(value->object, "percentile");
    const JsonValue* algorithm = Find(value->object, "algorithm");
    const JsonValue* interpolation = Find(value->object, "percentileInterpolation");
    const JsonValue* median_rule = Find(value->object, "medianEvenRule");
    if (IsString(method, "Mean")) {
        return IsNull(percentile) && IsString(algorithm, "PairwiseArithmeticMean") &&
            IsNull(interpolation) && IsNull(median_rule);
    }
    if (IsString(method, "Median")) {
        return IsNull(percentile) && IsString(algorithm, "SortedMiddle") &&
            IsNull(interpolation) && IsString(median_rule, "MeanOfMiddlePair");
    }
    if (IsString(method, "Minimum")) {
        return IsNull(percentile) && IsString(algorithm, "SortedFirst") &&
            IsNull(interpolation) && IsNull(median_rule);
    }
    if (IsString(method, "Maximum")) {
        return IsNull(percentile) && IsString(algorithm, "SortedLast") &&
            IsNull(interpolation) && IsNull(median_rule);
    }
    return IsString(method, "Percentile") && IsNumber(percentile) &&
        percentile->number >= 0.0 && percentile->number <= 100.0 &&
        IsString(algorithm, "SortedOrderStatistic") &&
        IsString(interpolation, "LinearR7") && IsNull(median_rule);
}

bool SameAggregation(const JsonValue* left, const JsonValue* right) {
    if (!ValidateAggregation(left) || !ValidateAggregation(right)) return false;
    for (const auto field :
        {"method", "algorithm", "percentileInterpolation", "medianEvenRule"}) {
        const JsonValue* left_field = Find(left->object, field);
        const JsonValue* right_field = Find(right->object, field);
        if (left_field->type != right_field->type ||
            (left_field->type == JsonValue::Type::string && left_field->string != right_field->string)) {
            return false;
        }
    }
    const JsonValue* left_percentile = Find(left->object, "percentile");
    const JsonValue* right_percentile = Find(right->object, "percentile");
    return left_percentile->type == right_percentile->type &&
        (IsNull(left_percentile) || left_percentile->number == right_percentile->number);
}

bool ValidateDefinitionConfidence(const JsonValue* value) {
    if (value == nullptr || value->type != JsonValue::Type::object ||
        !ExactFields(value->object, {"method", "level"})) {
        return false;
    }
    const JsonValue* method = Find(value->object, "method");
    const JsonValue* level = Find(value->object, "level");
    if (!IsOneOf(method, {"None", "BootstrapPercentile", "AnalyticNormal"})) return false;
    return IsString(method, "None")
        ? IsNull(level)
        : IsNumber(level) && level->number > 0.0 && level->number < 1.0;
}

bool ValidateResultConfidence(const JsonValue* value, const JsonValue* measured_value) {
    if (value == nullptr || value->type != JsonValue::Type::object ||
        !ExactFields(value->object, {"method", "level", "lower", "upper"})) {
        return false;
    }
    const JsonValue* method = Find(value->object, "method");
    const JsonValue* level = Find(value->object, "level");
    const JsonValue* lower = Find(value->object, "lower");
    const JsonValue* upper = Find(value->object, "upper");
    if (!IsOneOf(method, {"None", "BootstrapPercentile", "AnalyticNormal"})) return false;
    if (IsString(method, "None")) return IsNull(level) && IsNull(lower) && IsNull(upper);
    return IsNumber(level) && level->number > 0.0 && level->number < 1.0 &&
        IsNumber(lower) && IsNumber(upper) && lower->number <= upper->number &&
        (!IsNumber(measured_value) ||
            (lower->number <= measured_value->number && measured_value->number <= upper->number));
}

bool ValidateDefinitionUncertainty(const JsonValue* value) {
    if (value == nullptr || value->type != JsonValue::Type::object ||
        !ExactFields(value->object, {"method", "unitMode"})) {
        return false;
    }
    return IsOneOf(Find(value->object, "method"),
               {"None", "StandardDeviation", "StandardError", "ConfidenceIntervalHalfWidth"}) &&
        IsOneOf(Find(value->object, "unitMode"), {"SameAsMetric", "Dimensionless"});
}

bool ValidateResultUncertainty(const JsonValue* value) {
    if (value == nullptr || value->type != JsonValue::Type::object ||
        !ExactFields(value->object, {"method", "value"})) {
        return false;
    }
    const JsonValue* method = Find(value->object, "method");
    const JsonValue* amount = Find(value->object, "value");
    if (!IsOneOf(method,
            {"None", "StandardDeviation", "StandardError", "ConfidenceIntervalHalfWidth"})) {
        return false;
    }
    return IsString(method, "None") ? IsNull(amount) : IsNumber(amount) && amount->number >= 0.0;
}

bool ValidateCommonDefinitionFields(const Object& object) {
    const JsonValue* version = Find(object, "definitionVersion");
    return IsExactInteger(version, 1U) && IsMetricId(Find(object, "metricId"));
}

bool IsKnownFailureCodeForOutcome(
    const std::string_view outcome,
    const std::string_view code) {
    if (outcome == "NoResult") {
        return code == "insufficient-valid-samples" || code == "zero-valid-results";
    }
    if (outcome == "NotApplicable") {
        return code == "not-applicable-for-metric-domain" || code == "metric-not-applicable";
    }
    if (outcome == "Invalid") {
        return code == "invalid-source-contract" || code == "definition-mismatch";
    }
    return false;
}

bool ValidateFailureCodeArray(
    const JsonValue* value,
    const std::string_view outcome,
    const bool must_be_empty,
    const bool must_be_nonempty) {
    if (value == nullptr || value->type != JsonValue::Type::array || value->array.size() > 8 ||
        (must_be_empty && !value->array.empty()) ||
        (must_be_nonempty && value->array.empty())) {
        return false;
    }
    std::vector<std::string_view> seen;
    for (const auto& item : value->array) {
        if (item.type != JsonValue::Type::string || !IsKnownFailureCodeForOutcome(outcome, item.string) ||
            std::find(seen.begin(), seen.end(), item.string) != seen.end()) {
            return false;
        }
        seen.emplace_back(item.string);
    }
    return true;
}

bool ValidateOutcomePolicy(const JsonValue* value, const bool resource_domain) {
    if (value == nullptr || value->type != JsonValue::Type::object ||
        !ExactFields(value->object, {"allowedFailureCodes", "noResultRule"}) ||
        !IsString(Find(value->object, "noResultRule"), "ZeroValidSamples")) {
        return false;
    }
    const JsonValue* codes = Find(value->object, "allowedFailureCodes");
    if (codes == nullptr || codes->type != JsonValue::Type::object ||
        !ExactFields(codes->object, {"NoResult", "NotApplicable", "Invalid"})) {
        return false;
    }
    return ValidateFailureCodeArray(Find(codes->object, "NoResult"), "NoResult", false, true) &&
        ValidateFailureCodeArray(Find(codes->object, "NotApplicable"), "NotApplicable",
            !resource_domain, resource_domain) &&
        ValidateFailureCodeArray(Find(codes->object, "Invalid"), "Invalid", false, true);
}

bool DefinitionAllowsFailureCode(
    const Object& definition,
    const std::string_view outcome,
    const JsonValue* failure_code) {
    if (!IsFailureCode(failure_code) || !IsKnownFailureCodeForOutcome(outcome, failure_code->string)) {
        return false;
    }
    const JsonValue* policy = Find(definition, "outcomePolicy");
    const JsonValue* codes = policy == nullptr ? nullptr : Find(policy->object, "allowedFailureCodes");
    const JsonValue* allowed = codes == nullptr ? nullptr : Find(codes->object, outcome);
    return allowed != nullptr && allowed->type == JsonValue::Type::array &&
        std::any_of(allowed->array.begin(), allowed->array.end(), [&](const JsonValue& item) {
            return item.type == JsonValue::Type::string && item.string == failure_code->string;
        });
}

StitchMetricValidation Valid() {
    return {true, {}};
}

StitchMetricValidation Invalid(const std::string_view code) {
    return {false, std::string(code)};
}

JsonValue ParseDocument(const std::string_view json) {
    return JsonParser(json).Parse();
}

StitchMetricValidation ValidateResultLike(
    const Object& definition,
    const Object& object,
    const bool statistical) {
    const JsonValue* outcome = Find(object, "outcome");
    if (!IsOneOf(outcome, {"Success", "NoResult", "NotApplicable", "Invalid"})) {
        return Invalid("metric_outcome_invalid");
    }
    if (!ValidateCommonDefinitionFields(object) ||
        !IsString(Find(object, "metricId"), Find(definition, "metricId")->string) ||
        !IsString(Find(object, "metricDomain"), Find(definition, "domain")->string) ||
        !SameRounding(Find(object, "rounding"), Find(definition, "rounding"))) {
        return Invalid("metric_definition_mismatch");
    }
    RoundingSpec rounding;
    if (!ValidateRounding(Find(object, "rounding"), rounding) ||
        !ValidateResultConfidence(Find(object, "confidence"), Find(object, "value")) ||
        !ValidateResultUncertainty(Find(object, "uncertainty"))) {
        return Invalid("metric_value_invalid");
    }
    const JsonValue* raw_value = Find(object, "rawValue");
    const JsonValue* value = Find(object, "value");
    const JsonValue* failure_code = Find(object, "failureCode");
    const bool success = IsString(outcome, "Success");
    if (success) {
        if (!MatchesRoundedValue(raw_value, value, rounding) || !IsNull(failure_code)) {
            return Invalid("metric_value_invalid");
        }
    } else {
        const JsonValue* confidence = Find(object, "confidence");
        const JsonValue* uncertainty = Find(object, "uncertainty");
        if (!IsNull(raw_value) || !IsNull(value) ||
            !DefinitionAllowsFailureCode(definition, outcome->string, failure_code) ||
            confidence == nullptr || uncertainty == nullptr ||
            !IsString(Find(confidence->object, "method"), "None") ||
            !IsString(Find(uncertainty->object, "method"), "None") ||
            (IsString(outcome, "NotApplicable") &&
                !IsString(Find(definition, "domain"), "Resource"))) {
            return Invalid("metric_outcome_invalid");
        }
    }
    if (statistical) {
        const JsonValue* population = Find(object, "populationCount");
        const JsonValue* valid = Find(object, "validResultCount");
        ExactInteger population_count;
        ExactInteger valid_count;
        if (!TryNonnegativeExactInteger(population, population_count) ||
            !TryNonnegativeExactInteger(valid, valid_count) ||
            CompareExactIntegers(valid_count, population_count) > 0 ||
            (success && (population_count.digits == "0" || valid_count.digits == "0")) ||
            (IsString(outcome, "NoResult") && valid_count.digits != "0")) {
            return Invalid("metric_value_invalid");
        }
    } else {
        const JsonValue* count = Find(object, "sampleCount");
        ExactInteger sample_count;
        if (!TryNonnegativeExactInteger(count, sample_count) ||
            (success && sample_count.digits == "0") ||
            (!success && sample_count.digits != "0")) {
            return Invalid("metric_value_invalid");
        }
    }
    return Valid();
}

} // namespace

bool IsStitchMetricJsonSyntaxValid(const std::string_view json) noexcept {
    try {
        (void)ParseDocument(json);
        return true;
    } catch (...) {
        return false;
    }
}

StitchMetricValidation ValidateStitchMetricDefinitionJson(const std::string_view json) noexcept {
    try {
        const JsonValue root = ParseDocument(json);
        if (root.type != JsonValue::Type::object) return Invalid("metric_shape_invalid");
        const Object& object = root.object;
        if (!ExactFields(object,
                {"schemaVersion", "definitionVersion", "metricId", "domain", "unit", "coordinateSystem",
                 "mask", "sampling", "aggregation", "rounding", "boundary", "confidence",
                 "uncertainty", "outcomePolicy"})) {
            return Invalid("metric_shape_invalid");
        }
        if (!IsString(Find(object, "schemaVersion"), kStitchMetricDefinitionSchemaVersion)) {
            return Invalid("metric_schema_version_mismatch");
        }
        const bool image_domain = IsString(Find(object, "domain"), "Image");
        const bool resource_domain = IsString(Find(object, "domain"), "Resource");
        const bool image_unit = IsOneOf(Find(object, "unit"),
            {"pixels", "degrees", "ratio", "percent", "ev", "delta-e-2000"});
        const bool resource_unit = IsOneOf(Find(object, "unit"),
            {"seconds", "milliseconds", "bytes", "MiB", "ratio", "percent"});
        if (!ValidateCommonDefinitionFields(object) ||
            (!image_domain && !resource_domain) ||
            (image_domain && !image_unit) || (resource_domain && !resource_unit) ||
            !ValidateOutcomePolicy(Find(object, "outcomePolicy"), resource_domain)) {
            return Invalid("metric_value_invalid");
        }
        const JsonValue* coordinate = Find(object, "coordinateSystem");
        const JsonValue* mask = Find(object, "mask");
        const JsonValue* sampling = Find(object, "sampling");
        const JsonValue* boundary = Find(object, "boundary");
        const bool image_contract_valid = coordinate != nullptr &&
            coordinate->type == JsonValue::Type::object &&
            ExactFields(coordinate->object, {"origin", "xAxis", "yAxis", "reference"}) &&
            IsOneOf(Find(coordinate->object, "origin"), {"TopLeftPixelCenter", "OpticalCenter"}) &&
            IsString(Find(coordinate->object, "xAxis"), "Right") &&
            IsString(Find(coordinate->object, "yAxis"), "Down") &&
            IsOneOf(Find(coordinate->object, "reference"), {"CAM-A", "CAM-B", "StitchedOutput"}) &&
            mask != nullptr && mask->type == JsonValue::Type::object &&
            ExactFields(mask->object, {"kind", "source", "includedValue", "excludedValue"}) &&
            IsString(Find(mask->object, "kind"), "BinaryValidity") &&
            IsOneOf(Find(mask->object, "source"),
                {"CAM-AValidSamples", "CAM-BValidSamples", "IntersectionOfValidSamples",
                 "UnionOfValidSamples", "StitchedOutputValidity"}) &&
            IsExactInteger(Find(mask->object, "includedValue"), 1U) &&
            IsExactInteger(Find(mask->object, "excludedValue"), 0U) &&
            sampling != nullptr && sampling->type == JsonValue::Type::object &&
            ExactFields(sampling->object, {"method", "outOfBounds", "invalidSample"}) &&
            IsOneOf(Find(sampling->object, "method"), {"Nearest", "Bilinear", "Area"}) &&
            IsString(Find(sampling->object, "outOfBounds"), "Exclude") &&
            IsString(Find(sampling->object, "invalidSample"), "Exclude") &&
            ValidateAggregation(Find(object, "aggregation")) &&
            boundary != nullptr && boundary->type == JsonValue::Type::object &&
            ExactFields(boundary->object, {"imageEdge", "maskEdge", "intervalClosure"}) &&
            IsOneOf(Find(boundary->object, "imageEdge"),
                {"ExcludeIncompleteKernel", "ClampToImage"}) &&
            IsOneOf(Find(boundary->object, "maskEdge"),
                {"RequireAllSamplesValid", "RequireCenterSampleValid"}) &&
            IsOneOf(Find(boundary->object, "intervalClosure"),
                {"LowerInclusiveUpperExclusive", "Closed"}) &&
            ValidateDefinitionConfidence(Find(object, "confidence")) &&
            ValidateDefinitionUncertainty(Find(object, "uncertainty"));
        const bool resource_contract_valid = IsNull(coordinate) && IsNull(mask) &&
            IsNull(sampling) && IsNull(boundary) && ValidateAggregation(Find(object, "aggregation")) &&
            ValidateDefinitionConfidence(Find(object, "confidence")) &&
            ValidateDefinitionUncertainty(Find(object, "uncertainty"));
        if ((image_domain && !image_contract_valid) ||
            (resource_domain && !resource_contract_valid)) {
            return Invalid("metric_value_invalid");
        }
        RoundingSpec rounding;
        if (!ValidateRounding(Find(object, "rounding"), rounding)) {
            return Invalid("metric_value_invalid");
        }
        return Valid();
    } catch (...) {
        return Invalid("metric_json_invalid");
    }
}

StitchMetricValidation ValidateStitchMetricResultJson(
    const std::string_view definition_json,
    const std::string_view result_json) noexcept {
    try {
        const StitchMetricValidation definition_validation =
            ValidateStitchMetricDefinitionJson(definition_json);
        if (!definition_validation.valid) return definition_validation;
        const JsonValue definition = ParseDocument(definition_json);
        const JsonValue root = ParseDocument(result_json);
        if (root.type != JsonValue::Type::object ||
            !ExactFields(root.object,
                {"schemaVersion", "definitionVersion", "metricId", "metricDomain", "outcome",
                 "rawValue", "value", "sampleCount", "rounding", "confidence", "uncertainty",
                 "failureCode"})) {
            return Invalid("metric_shape_invalid");
        }
        if (!IsString(Find(root.object, "schemaVersion"), kStitchMetricResultSchemaVersion)) {
            return Invalid("metric_schema_version_mismatch");
        }
        return ValidateResultLike(definition.object, root.object, false);
    } catch (...) {
        return Invalid("metric_json_invalid");
    }
}

StitchMetricValidation ValidateStitchStatisticalResultJson(
    const std::string_view definition_json,
    const std::string_view result_json) noexcept {
    try {
        const StitchMetricValidation definition_validation =
            ValidateStitchMetricDefinitionJson(definition_json);
        if (!definition_validation.valid) return definition_validation;
        const JsonValue definition = ParseDocument(definition_json);
        const JsonValue root = ParseDocument(result_json);
        if (root.type != JsonValue::Type::object ||
            !ExactFields(root.object,
                {"schemaVersion", "definitionVersion", "metricId", "metricDomain", "outcome",
                 "aggregation", "populationCount", "validResultCount", "rawValue", "value",
                 "rounding", "confidence", "uncertainty", "failureCode"})) {
            return Invalid("metric_shape_invalid");
        }
        if (!IsString(Find(root.object, "schemaVersion"),
                kStitchStatisticalResultSchemaVersion)) {
            return Invalid("metric_schema_version_mismatch");
        }
        if (!SameAggregation(Find(root.object, "aggregation"),
                Find(definition.object, "aggregation"))) {
            return Invalid("metric_definition_mismatch");
        }
        return ValidateResultLike(definition.object, root.object, true);
    } catch (...) {
        return Invalid("metric_json_invalid");
    }
}

} // namespace a0::m2
