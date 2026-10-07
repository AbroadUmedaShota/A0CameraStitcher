#pragma once

// GitHub Issue #231: access to the anonymized real-device fixtures in
// tests/fixtures/hardware-replay from the native contract tests. The fixture folder is
// passed in by CMake (A0_REPLAY_FIXTURE_DIR), so the tests do not depend on the working
// directory. See the README in that folder for where the files come from and which values were
// replaced.

#include "a0/common/protocol_json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef A0_REPLAY_FIXTURE_DIR
#error "A0_REPLAY_FIXTURE_DIR must name tests/fixtures/hardware-replay"
#endif

namespace a0::replay {

inline constexpr std::string_view kSingleTransactionId = "f231a0010000000000000000000000a1";
inline constexpr std::string_view kSingleRunId = "run-231001-1";
inline constexpr std::string_view kDualSucceededTransactionId = "f231b0020000000000000000000000b2";
inline constexpr std::string_view kDualFailedTransactionId = "f231b0030000000000000000000000b3";

struct JsonFailure {
    [[noreturn]] static void Fail(std::string code, std::string message) {
        throw std::runtime_error("replay fixture JSON " + code + ": " + message);
    }
};

using Json = ::a0::common::protocol_json::JsonValue;
using JsonKind = ::a0::common::protocol_json::JsonKind;

[[nodiscard]] inline std::filesystem::path FixtureDirectory() {
    return std::filesystem::path(A0_REPLAY_FIXTURE_DIR);
}

// Line endings are normalized: the fixtures are text files and a Windows checkout may convert
// them.
[[nodiscard]] inline std::string ReadFixtureText(std::string_view relative_path) {
    const auto path = FixtureDirectory() / std::filesystem::path(std::string(relative_path));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("replay fixture is missing: " + path.string());
    }
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') continue;
        normalized.push_back(text[index]);
    }
    return normalized;
}

[[nodiscard]] inline std::string TrimTrailingNewlines(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

[[nodiscard]] inline Json ParseJson(std::string_view text) {
    return ::a0::common::protocol_json::BasicJsonParser<JsonFailure>(text).Parse();
}

[[nodiscard]] inline const Json& Field(const Json& object, std::string_view name) {
    if (object.kind != JsonKind::object) throw std::runtime_error("replay fixture value is not an object");
    const auto found = object.object.find(std::string(name));
    if (found == object.object.end()) {
        throw std::runtime_error("replay fixture field is missing: " + std::string(name));
    }
    return found->second;
}

[[nodiscard]] inline bool HasField(const Json& object, std::string_view name) {
    return object.kind == JsonKind::object && object.object.contains(std::string(name));
}

[[nodiscard]] inline std::string HexDecode(std::string_view hex) {
    if (hex.size() % 2 != 0) throw std::runtime_error("replay fixture hex has an odd length");
    const auto digit = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        throw std::runtime_error("replay fixture hex has a non-hex character");
    };
    std::string output;
    output.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        output.push_back(static_cast<char>((digit(hex[index]) << 4) | digit(hex[index + 1])));
    }
    return output;
}

// The decoded terminal result of a real Agent pair journal (the journal stores it as hex).
[[nodiscard]] inline std::string DecodeTerminalResult(std::string_view journal_text) {
    const Json journal = ParseJson(journal_text);
    return HexDecode(Field(journal, "terminalResultHex").string);
}

// A dummy JPEG, stored as base64 text so that no image file ever sits in the repository.
[[nodiscard]] inline std::vector<unsigned char> ReadFixtureJpeg(std::string_view name) {
    const std::string text = ReadFixtureText("images/" + std::string(name) + ".jpg.b64");
    const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> output;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char character : text) {
        if (character == '=' || character == '\n' || character == '\r') continue;
        const auto position = alphabet.find(character);
        if (position == std::string::npos) throw std::runtime_error("replay fixture base64 is invalid");
        buffer = (buffer << 6U) | static_cast<std::uint32_t>(position);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<unsigned char>((buffer >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    return output;
}

// Non-empty lines of a JSON Lines fixture, parsed.
[[nodiscard]] inline std::vector<Json> ReadFixtureJsonLines(std::string_view relative_path) {
    const std::string text = ReadFixtureText(relative_path);
    std::vector<Json> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) lines.push_back(ParseJson(std::string_view(text).substr(start, end - start)));
        start = end + 1;
    }
    return lines;
}

} // namespace a0::replay

// The tests refer to the helpers as replay::....
namespace replay = ::a0::replay;
