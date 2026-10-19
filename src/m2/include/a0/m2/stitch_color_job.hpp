#pragma once
#include "a0/m2/color_correction.hpp"
#include <string>
#include <utility>

namespace a0::m2::correction {
inline constexpr std::size_t kMaximumColorJobBytes=65536;
inline constexpr std::size_t kMaximumColorReportBytes=4*1024*1024;
struct StitchColorJob {
    const ColorCorrectionPolicy policy;
    const std::string source_json;
    StitchColorJob(const StitchColorJob&)=default;
private:
    StitchColorJob(ColorCorrectionPolicy p,std::string text):policy(p),source_json(std::move(text)){}
    friend StitchColorJob ParseStitchColorJob(std::string_view);
};
[[nodiscard]] StitchColorJob ParseStitchColorJob(std::string_view json);
struct ColorInputBinding { std::string sha256;std::uint64_t encoded_size_bytes; };
struct StitchColorReportBindings {
    std::array<ColorInputBinding,2> inputs;
    std::string profile_file_sha256,profile_fingerprint_sha256,job_sha256,geometry_report_sha256;
    render::OutputRaster raster;
};
// Pure diagnostic only. Even estimated requires a matching final eval manifest.
[[nodiscard]] std::string StitchColorReport(const StitchColorJob& job,
    const ImageColorCorrectionResult& result,const StitchColorReportBindings& bindings);
} // namespace a0::m2::correction
