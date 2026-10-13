#pragma once

#include <cstddef>
#include <string_view>

// Internal header of the calibration observation session reader. It is not
// part of the public API; the contract test includes it to test the count on
// small inputs.
namespace a0::m2::calibration::detail {

// Upper bound on the number of values (the nodes of the parsed tree) of a JSON
// text, counted in one pass over the bytes without building anything.
//
// For a well-formed text, the values are the root plus every member or element
// of every container. A container that holds m >= 1 members contains m - 1
// commas, so
//     values = 1 + (number of commas) + (number of non-empty containers)
//            <= 1 + (number of commas) + (number of '[' and '{').
// The bound counts an empty container once more than it deserves; for a text
// that is not well-formed the number is only a count, and the parser rejects
// the text afterwards. Characters inside a string literal are skipped, and a
// backslash skips the character after it, so a quote, comma or bracket inside a
// string is not counted. An unterminated string ends the scan.
[[nodiscard]] inline std::size_t JsonValueCountUpperBound(const std::string_view text) noexcept {
    std::size_t count = 1;
    bool in_string = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (in_string) {
            if (c == '\\') ++index;
            else if (c == '"') in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == ',' || c == '[' || c == '{') {
            ++count;
        }
    }
    return count;
}

} // namespace a0::m2::calibration::detail
