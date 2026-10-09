#pragma once

#include "a0/m2/render.hpp"
#include "a0/m2/document_render.hpp"
#include "a0/m2/setup_assessment.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace a0::m2 {

inline constexpr std::uint64_t kMaximumCompressedJpegBytes = 64ULL * 1024ULL * 1024ULL;

// CAM-A is the output coordinate reference. This matrix maps CAM-B pixel
// coordinates into that same coordinate system. It is calibration data only;
// the stitcher never estimates or changes it.
struct FixedRigStitchProfile {
    std::string profile_id;
    ProfileTrust trust;
    std::uint32_t expected_input_width;
    std::uint32_t expected_input_height;
    std::array<double, 9> camera_b_to_camera_a;
    StitchLayout layout;
    StitchCropPixels crop;
};

// Identity of what produced a stitched output. Recorded in the StitchJob
// manifest so two results with identical inputs and profile but different
// engines can never be mistaken for the same result.
inline constexpr std::string_view kOfflineStitcherEngineId = "a0.m2.offline-stitcher";
inline constexpr std::string_view kOfflineStitcherEngineVersion = "1.0.0";

struct OfflineStitchRequest {
    std::filesystem::path camera_a_original;
    std::filesystem::path camera_b_original;
    std::filesystem::path output_job_directory;
    FixedRigStitchProfile profile;

    // Immutable identity of this attempt, and of the CaptureTransaction whose
    // retained originals it consumes. Both are required: a stitched file with no
    // recorded identity is exactly the "file exists, therefore success" state the
    // manifest exists to abolish, so there is no path that produces one.
    // A restitch supplies a new stitch_job_id and its own job directory.
    std::string stitch_job_id;
    std::string capture_transaction_id;

    // When the caller considers this job complete, as yyyy-mm-ddThh:mm:ssZ.
    // Supplied rather than read from a clock here so the same inputs produce the
    // same manifest, which is what makes the record checkable.
    std::string completed_at_utc;
};

struct OfflineStitchResult {
    std::filesystem::path stitched_jpeg;
    std::uint32_t width;
    std::uint32_t height;
    std::string profile_id;

    // The published manifest. Its existence, verified by re-reading it, is what
    // makes this result terminal-success; the JPEG beside it proves nothing on
    // its own.
    std::filesystem::path manifest_path;
    std::string stitch_job_id;
};

// Product v2 requires an approved file, a cross-language fingerprint guard and
// an explicit kernel. No evaluation/DRAFT entry or implicit kernel exists.
struct OfflineStitchV2Request {
    std::filesystem::path camera_a_original, camera_b_original, output_job_directory;
    std::filesystem::path profile_path;
    std::string assessed_at_utc, expected_profile_sha256;
    render::Resampling resampling;
    std::string stitch_job_id, capture_transaction_id, completed_at_utc;
    OfflineStitchV2Request(std::filesystem::path a, std::filesystem::path b,
        std::filesystem::path job, std::filesystem::path profile, std::string assessed,
        std::string expected_sha, render::Resampling kernel, std::string job_id,
        std::string capture_id, std::string completed)
        : camera_a_original(std::move(a)), camera_b_original(std::move(b)),
          output_job_directory(std::move(job)), profile_path(std::move(profile)),
          assessed_at_utc(std::move(assessed)), expected_profile_sha256(std::move(expected_sha)),
          resampling(kernel), stitch_job_id(std::move(job_id)),
          capture_transaction_id(std::move(capture_id)), completed_at_utc(std::move(completed)) {}
};

inline constexpr std::string_view kOfflineStitcherV2BilinearVersion = "2.0.0+bilinear";
inline constexpr std::string_view kOfflineStitcherV2BicubicVersion = "2.0.0+bicubic-catmull-rom";
[[nodiscard]] OfflineStitchResult StitchCanonicalPairV2(const OfflineStitchV2Request& request);

// Decodes two retained canonical original.jpg files, applies only the supplied
// fixed profile transform, linearly feathers the geometric overlap, crops, and
// atomically publishes <output_job_directory>/stitched.jpg. Existing outputs
// are never replaced and source files are opened read-only.
[[nodiscard]] OfflineStitchResult StitchCanonicalPair(const OfflineStitchRequest& request);

// Byte-identical explicit export of a completed stitched JPEG. The copy is
// written beside the destination as a partial and atomically renamed without
// replacing an existing file.
void ExportStitchedJpeg(
    const std::filesystem::path& stitched_jpeg,
    const std::filesystem::path& destination_jpeg);

} // namespace a0::m2
