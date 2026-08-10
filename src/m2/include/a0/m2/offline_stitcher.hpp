#pragma once

#include "a0/m2/setup_assessment.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace a0::m2 {

enum class StitchLayout {
    camera_a_left_camera_b_right,
    camera_a_top_camera_b_bottom,
};

struct StitchCropPixels {
    std::uint32_t left{};
    std::uint32_t top{};
    std::uint32_t right{};
    std::uint32_t bottom{};
};

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

struct OfflineStitchRequest {
    std::filesystem::path camera_a_original;
    std::filesystem::path camera_b_original;
    std::filesystem::path output_job_directory;
    FixedRigStitchProfile profile;
};

struct OfflineStitchResult {
    std::filesystem::path stitched_jpeg;
    std::uint32_t width;
    std::uint32_t height;
    std::string profile_id;
};

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
