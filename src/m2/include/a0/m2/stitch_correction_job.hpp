#pragma once
#include "a0/m2/image_patch_seed_selection.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace a0::m2::correction {
inline constexpr std::size_t kMaximumCorrectionJobBytes = 65536;
inline constexpr std::size_t kMaximumCorrectionReportBytes = 4*1024*1024;
struct StitchCorrectionJob {
    const PatchSeedSelectionPolicy selection;
    const ImagePatchPolicy matching;
    const std::optional<PatchRefinementPolicy> refinement; // v1 absent, v2 explicit
    const RawInversePolicy inverse;
    const DocumentCorrectionPolicy correction;
    const std::string source_json; // validated exact input, including whitespace
    StitchCorrectionJob(const StitchCorrectionJob&) = default;
private:
    StitchCorrectionJob(PatchSeedSelectionPolicy s, ImagePatchPolicy m, std::optional<PatchRefinementPolicy> r,
        RawInversePolicy i, DocumentCorrectionPolicy c, std::string text)
        : selection(s), matching(m), refinement(r), inverse(i), correction(c), source_json(std::move(text)) {}
    friend StitchCorrectionJob ParseStitchCorrectionJob(std::string_view);
};
[[nodiscard]] StitchCorrectionJob ParseStitchCorrectionJob(std::string_view json);
struct CorrectionInputBinding { std::string sha256; std::uint64_t encoded_size_bytes; };
struct CorrectionReportBindings {
    std::array<CorrectionInputBinding,2> inputs;
    std::string profile_file_sha256, profile_fingerprint_sha256, job_sha256;
    render::OutputRaster raster;
};
// Pure detailed diagnostic. Does not publish/approve an image or profile.
// A report with an estimate still requires the final eval manifest for preview.
[[nodiscard]] std::string StitchCorrectionReport(const StitchCorrectionJob& job,
    const AutoImagePatchCorrectionResult& result, const CorrectionReportBindings& bindings);
} // namespace a0::m2::correction
