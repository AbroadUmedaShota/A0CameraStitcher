#include "a0/m2/optical_planner.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using a0::m2::CropPixels;
using a0::m2::FrameRotation;
using a0::m2::Layout;
using a0::m2::OpticalPlanInput;
using a0::m2::OpticalPlanResult;

[[nodiscard]] std::uint64_t ParseUnsigned(std::string_view value, std::string_view option) {
    std::uint64_t parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::invalid_argument("invalid unsigned value for " + std::string(option));
    }
    return parsed;
}

[[nodiscard]] FrameRotation ParseRotation(std::string_view value) {
    if (value == "landscape") return FrameRotation::landscape;
    if (value == "portrait") return FrameRotation::portrait;
    throw std::invalid_argument("rotation must be landscape or portrait");
}

[[nodiscard]] Layout ParseLayout(std::string_view value) {
    if (value == "horizontal") return Layout::horizontal;
    if (value == "vertical") return Layout::vertical;
    throw std::invalid_argument("layout must be horizontal or vertical");
}

void PrintUsage() {
    std::cerr << "Usage: A0CameraStitcher.OpticalPlanner --dpi <positive> --rotation landscape|portrait "
                 "--layout horizontal|vertical --overlap-px <pixels> --crop-left-px <pixels> "
                 "--crop-top-px <pixels> --crop-right-px <pixels> --crop-bottom-px <pixels>\n";
}

void PrintSize(std::string_view name, const a0::m2::PixelSize& size, bool& first) {
    if (!first) std::cout << ',';
    first = false;
    std::cout << '\n' << "  \"" << name << "\": {\"width\": " << size.width
              << ", \"height\": " << size.height << '}';
}

void PrintJson(const OpticalPlanInput& input, const OpticalPlanResult& result) {
    bool first = true;
    std::cout << "{\n";
    std::cout << "  \"schemaVersion\": \"m2.optical-candidate.v1\",\n";
    std::cout << "  \"approvalState\": \"unapproved\",\n";
    std::cout << "  \"qualityDecision\": \"not-evaluated\",\n";
    std::cout << "  \"requestedDpi\": " << input.requested_dpi << ",\n";
    std::cout << "  \"frameRotation\": \""
              << (input.frame_rotation == FrameRotation::landscape ? "landscape" : "portrait") << "\",\n";
    std::cout << "  \"layout\": \"" << (input.layout == Layout::horizontal ? "horizontal" : "vertical") << "\",\n";
    std::cout << "  \"overlapPixels\": " << input.overlap_pixels << ",\n";
    std::cout << "  \"cropPixels\": {\"left\": " << input.crop_pixels.left
              << ", \"top\": " << input.crop_pixels.top << ", \"right\": " << input.crop_pixels.right
              << ", \"bottom\": " << input.crop_pixels.bottom << '}';
    first = false;
    PrintSize("framePixelSize", result.frame_pixel_size, first);
    PrintSize("a0PageSizeMm", result.a0_page_size_mm, first);
    PrintSize("stitchedUsablePixelSize", result.stitched_usable_pixel_size, first);
    PrintSize("requiredPixelSize", result.required_pixel_size, first);
    PrintSize("shortfallPixels", result.shortfall_pixels, first);
    std::cout << ",\n  \"effectiveDpi\": " << std::setprecision(10) << result.effective_dpi
              << ",\n  \"candidateMeetsRequestedDpi\": "
              << (result.candidate_meets_requested_dpi ? "true" : "false")
              << ",\n  \"notice\": \"Geometry candidate only; this does not approve the rig or A0 image quality.\"\n}\n";
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        OpticalPlanInput input;
        bool dpi_set = false;
        bool rotation_set = false;
        bool layout_set = false;
        bool overlap_set = false;
        bool left_set = false;
        bool top_set = false;
        bool right_set = false;
        bool bottom_set = false;

        for (int index = 1; index < argc; index += 2) {
            if (index + 1 >= argc) throw std::invalid_argument("missing value for option");
            const std::string_view option(argv[index]);
            const std::string_view value(argv[index + 1]);
            if (option == "--dpi" && !dpi_set) { input.requested_dpi = ParseUnsigned(value, option); dpi_set = true; }
            else if (option == "--rotation" && !rotation_set) { input.frame_rotation = ParseRotation(value); rotation_set = true; }
            else if (option == "--layout" && !layout_set) { input.layout = ParseLayout(value); layout_set = true; }
            else if (option == "--overlap-px" && !overlap_set) { input.overlap_pixels = ParseUnsigned(value, option); overlap_set = true; }
            else if (option == "--crop-left-px" && !left_set) { input.crop_pixels.left = ParseUnsigned(value, option); left_set = true; }
            else if (option == "--crop-top-px" && !top_set) { input.crop_pixels.top = ParseUnsigned(value, option); top_set = true; }
            else if (option == "--crop-right-px" && !right_set) { input.crop_pixels.right = ParseUnsigned(value, option); right_set = true; }
            else if (option == "--crop-bottom-px" && !bottom_set) { input.crop_pixels.bottom = ParseUnsigned(value, option); bottom_set = true; }
            else throw std::invalid_argument("unknown or repeated option: " + std::string(option));
        }
        if (!dpi_set || !rotation_set || !layout_set || !overlap_set || !left_set || !top_set || !right_set || !bottom_set) {
            throw std::invalid_argument("all options are required");
        }
        PrintJson(input, a0::m2::CalculateOpticalPlan(input));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        PrintUsage();
        return 2;
    }
}
