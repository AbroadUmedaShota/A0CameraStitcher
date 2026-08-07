#include "a0/m2/optical_planner.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace a0::m2 {
namespace {

[[nodiscard]] std::uint64_t CheckedAdd(const std::uint64_t left, const std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::overflow_error("pixel arithmetic overflow");
    }
    return left + right;
}

[[nodiscard]] std::uint64_t CheckedMultiply(const std::uint64_t left, const std::uint64_t right) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        throw std::overflow_error("pixel arithmetic overflow");
    }
    return left * right;
}

[[nodiscard]] std::uint64_t CheckedSubtract(
    const std::uint64_t value,
    const std::uint64_t subtract,
    const char* const name) {
    if (subtract >= value) {
        throw std::invalid_argument(name);
    }
    return value - subtract;
}

[[nodiscard]] std::uint64_t RequiredPixels(const std::uint64_t millimetres, const std::uint64_t dpi) {
    // mm * dpi / 25.4 == mm * dpi * 10 / 254. Use integer ceiling, not floating point rounding.
    const auto numerator = CheckedMultiply(CheckedMultiply(millimetres, dpi), 10);
    return numerator / 254 + (numerator % 254 == 0 ? 0 : 1);
}

[[nodiscard]] double DpiFor(const std::uint64_t pixels, const std::uint64_t millimetres) {
    return static_cast<double>(pixels) * 25.4 / static_cast<double>(millimetres);
}

} // namespace

OpticalPlanResult CalculateOpticalPlan(const OpticalPlanInput& input) {
    if (input.requested_dpi == 0) {
        throw std::invalid_argument("requested DPI must be greater than zero");
    }

    const PixelSize frame = input.frame_rotation == FrameRotation::landscape
        ? PixelSize{kD810SensorWidthPixels, kD810SensorHeightPixels}
        : PixelSize{kD810SensorHeightPixels, kD810SensorWidthPixels};
    const PixelSize page = input.layout == Layout::horizontal
        ? PixelSize{kA0PortraitHeightMm, kA0PortraitWidthMm}
        : PixelSize{kA0PortraitWidthMm, kA0PortraitHeightMm};

    const auto overlap_limit = input.layout == Layout::horizontal ? frame.width : frame.height;
    if (input.overlap_pixels >= overlap_limit) {
        throw std::invalid_argument("overlap pixels must be less than the joined frame dimension");
    }

    PixelSize stitched{
        input.layout == Layout::horizontal
            ? CheckedSubtract(CheckedMultiply(frame.width, 2), input.overlap_pixels, "overlap leaves no stitched width")
            : frame.width,
        input.layout == Layout::vertical
            ? CheckedSubtract(CheckedMultiply(frame.height, 2), input.overlap_pixels, "overlap leaves no stitched height")
            : frame.height,
    };

    const auto horizontal_crop = CheckedAdd(input.crop_pixels.left, input.crop_pixels.right);
    const auto vertical_crop = CheckedAdd(input.crop_pixels.top, input.crop_pixels.bottom);
    stitched.width = CheckedSubtract(stitched.width, horizontal_crop, "horizontal crop leaves no usable width");
    stitched.height = CheckedSubtract(stitched.height, vertical_crop, "vertical crop leaves no usable height");

    const PixelSize required{
        RequiredPixels(page.width, input.requested_dpi),
        RequiredPixels(page.height, input.requested_dpi),
    };
    const PixelSize shortfall{
        required.width > stitched.width ? required.width - stitched.width : 0,
        required.height > stitched.height ? required.height - stitched.height : 0,
    };

    return {
        frame,
        page,
        stitched,
        required,
        shortfall,
        std::min(DpiFor(stitched.width, page.width), DpiFor(stitched.height, page.height)),
        shortfall.width == 0 && shortfall.height == 0,
    };
}

} // namespace a0::m2
