#pragma once

#include <cstdint>

namespace a0::m2 {

inline constexpr std::uint64_t kA0PortraitWidthMm = 841;
inline constexpr std::uint64_t kA0PortraitHeightMm = 1189;
inline constexpr std::uint64_t kD810SensorWidthPixels = 7360;
inline constexpr std::uint64_t kD810SensorHeightPixels = 4912;

enum class FrameRotation {
    landscape,
    portrait,
};

enum class Layout {
    horizontal,
    vertical,
};

struct PixelSize {
    std::uint64_t width{};
    std::uint64_t height{};
};

struct CropPixels {
    std::uint64_t left{};
    std::uint64_t top{};
    std::uint64_t right{};
    std::uint64_t bottom{};
};

struct OpticalPlanInput {
    std::uint64_t requested_dpi{};
    FrameRotation frame_rotation{FrameRotation::landscape};
    Layout layout{Layout::horizontal};
    std::uint64_t overlap_pixels{};
    CropPixels crop_pixels{};
};

struct OpticalPlanResult {
    PixelSize frame_pixel_size;
    PixelSize a0_page_size_mm;
    PixelSize stitched_usable_pixel_size;
    PixelSize required_pixel_size;
    PixelSize shortfall_pixels;
    double effective_dpi{};
    bool candidate_meets_requested_dpi{};
};

// Calculates geometry only. It does not choose a camera arrangement or approve image quality.
// A horizontal layout uses landscape A0 (1189 x 841 mm); vertical uses portrait A0 (841 x 1189 mm).
// Throws std::invalid_argument for invalid dimensions and std::overflow_error for unrepresentable arithmetic.
[[nodiscard]] OpticalPlanResult CalculateOpticalPlan(const OpticalPlanInput& input);

} // namespace a0::m2
