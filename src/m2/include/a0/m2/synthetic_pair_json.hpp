#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::synthetic {

// Minimal JSON document model for the synthetic pair generator. It reads the
// generator spec and the ground truth file. Numbers are kept as their literal
// text so a 64-bit seed is not rounded through a double.
class JsonValue {
public:
    enum class Kind { null_value, boolean, number, string, array, object };

    Kind kind{Kind::null_value};
    bool boolean{};
    std::string text;
    std::vector<JsonValue> items;
    std::vector<std::string> keys;
    std::vector<JsonValue> values;

    [[nodiscard]] const JsonValue* Find(std::string_view key) const noexcept;
    // Each accessor throws std::invalid_argument when the value has another
    // kind or does not fit the requested type.
    [[nodiscard]] double AsDouble() const;
    [[nodiscard]] std::uint64_t AsUnsigned() const;
    [[nodiscard]] const std::string& AsString() const;
};

// Throws std::invalid_argument for malformed input, duplicate object keys,
// objects and arrays nested more than 32 levels deep (a scalar inside 32 levels
// is accepted), and trailing content.
[[nodiscard]] JsonValue ParseJson(std::string_view text);

} // namespace a0::m2::synthetic
