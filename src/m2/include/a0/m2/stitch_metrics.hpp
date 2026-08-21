#pragma once

#include <string>
#include <string_view>

namespace a0::m2 {

inline constexpr std::string_view kStitchMetricDefinitionSchemaVersion =
    "a0.stitch-metric-definition.v1";
inline constexpr std::string_view kStitchMetricResultSchemaVersion =
    "a0.stitch-metric-result.v1";
inline constexpr std::string_view kStitchStatisticalResultSchemaVersion =
    "a0.stitch-statistical-result.v1";

struct StitchMetricValidation final {
    bool valid{};
    std::string failure_code;
};

// Syntax-only parsing supports checked-in schema integrity tests. Contract tests
// also apply positive and negative fixtures directly to those schemas.
[[nodiscard]] bool IsStitchMetricJsonSyntaxValid(std::string_view json) noexcept;

[[nodiscard]] StitchMetricValidation ValidateStitchMetricDefinitionJson(
    std::string_view json) noexcept;
[[nodiscard]] StitchMetricValidation ValidateStitchMetricResultJson(
    std::string_view definition_json,
    std::string_view result_json) noexcept;
[[nodiscard]] StitchMetricValidation ValidateStitchStatisticalResultJson(
    std::string_view definition_json,
    std::string_view result_json) noexcept;

} // namespace a0::m2
