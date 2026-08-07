#include "a0/m2/optical_planner.hpp"

#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {

int failures = 0;

void Check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Callable>
void CheckInvalidArgument(Callable&& callable, const std::string& message) {
    try {
        callable();
        Check(false, message);
    } catch (const std::invalid_argument&) {
    }
}

a0::m2::OpticalPlanInput BaseInput(const std::uint64_t dpi) {
    return {
        dpi,
        a0::m2::FrameRotation::portrait,
        a0::m2::Layout::horizontal,
        982,
        {},
    };
}

void TestRequiredA0Pixels() {
    for (const auto [dpi, expected_width, expected_height] : {
             std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>{150, 7022, 4967},
             {180, 8426, 5960},
             {200, 9363, 6623},
         }) {
        const auto result = a0::m2::CalculateOpticalPlan(BaseInput(dpi));
        Check(result.required_pixel_size.width == expected_width && result.required_pixel_size.height == expected_height,
            "required A0 pixels must use ceiling conversion at " + std::to_string(dpi) + " DPI");
    }
}

void TestPortraitRotatedHorizontalTwentyPercentOverlap() {
    auto input = BaseInput(180);
    const auto pass = a0::m2::CalculateOpticalPlan(input);
    Check(pass.frame_pixel_size.width == 4912 && pass.frame_pixel_size.height == 7360,
        "portrait frame rotation must swap D810 dimensions");
    Check(pass.stitched_usable_pixel_size.width == 8842 && pass.stitched_usable_pixel_size.height == 7360,
        "horizontal 20 percent overlap must use the rotated frame width");
    Check(pass.candidate_meets_requested_dpi && pass.shortfall_pixels.width == 0 && pass.shortfall_pixels.height == 0,
        "portrait-rotated horizontal candidate must pass at 180 DPI");
    Check(std::abs(pass.effective_dpi - 188.8871320) < 0.0001,
        "effective DPI must be the limiting A0 dimension");

    input.requested_dpi = 200;
    const auto fail = a0::m2::CalculateOpticalPlan(input);
    Check(!fail.candidate_meets_requested_dpi && fail.shortfall_pixels.width == 521 && fail.shortfall_pixels.height == 0,
        "same candidate must fail at 200 DPI only in width");
}

void TestInvalidOverlapAndCrop() {
    auto overlap = BaseInput(180);
    overlap.overlap_pixels = 4912;
    CheckInvalidArgument([&] { (void)a0::m2::CalculateOpticalPlan(overlap); },
        "overlap equal to the joined frame dimension must be rejected");

    auto crop = BaseInput(180);
    crop.crop_pixels.left = 8842;
    CheckInvalidArgument([&] { (void)a0::m2::CalculateOpticalPlan(crop); },
        "crop equal to usable stitched width must be rejected");
}

void TestVerticalLayoutAndCrop() {
    a0::m2::OpticalPlanInput input{
        180,
        a0::m2::FrameRotation::landscape,
        a0::m2::Layout::vertical,
        1000,
        {100, 300, 200, 400},
    };
    const auto result = a0::m2::CalculateOpticalPlan(input);
    Check(result.frame_pixel_size.width == 7360 && result.frame_pixel_size.height == 4912,
        "landscape frame must keep D810 pixel dimensions");
    Check(result.a0_page_size_mm.width == 841 && result.a0_page_size_mm.height == 1189,
        "vertical layout must use portrait A0 dimensions");
    Check(result.stitched_usable_pixel_size.width == 7060 && result.stitched_usable_pixel_size.height == 8124,
        "vertical layout must subtract overlap and all four crop values");
    Check(!result.candidate_meets_requested_dpi && result.shortfall_pixels.width == 0 && result.shortfall_pixels.height == 302,
        "cropped vertical candidate must report only the height shortfall at 180 DPI");
}

void TestNumericBoundaries() {
    auto zero_dpi = BaseInput(0);
    CheckInvalidArgument([&] { (void)a0::m2::CalculateOpticalPlan(zero_dpi); },
        "zero DPI must be rejected");

    auto crop_overflow = BaseInput(180);
    crop_overflow.crop_pixels.left = std::numeric_limits<std::uint64_t>::max();
    crop_overflow.crop_pixels.right = 1;
    try {
        (void)a0::m2::CalculateOpticalPlan(crop_overflow);
        Check(false, "crop addition overflow must be rejected");
    } catch (const std::overflow_error&) {
    }

    auto dpi_overflow = BaseInput(std::numeric_limits<std::uint64_t>::max());
    try {
        (void)a0::m2::CalculateOpticalPlan(dpi_overflow);
        Check(false, "required-pixel multiplication overflow must be rejected");
    } catch (const std::overflow_error&) {
    }
}

} // namespace

int main() {
    try {
        TestRequiredA0Pixels();
        TestPortraitRotatedHorizontalTwentyPercentOverlap();
        TestInvalidOverlapAndCrop();
        TestVerticalLayoutAndCrop();
        TestNumericBoundaries();
    } catch (const std::exception& error) {
        std::cerr << "UNEXPECTED: " << error.what() << '\n';
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All M2 optical planner tests passed\n";
    return 0;
}
