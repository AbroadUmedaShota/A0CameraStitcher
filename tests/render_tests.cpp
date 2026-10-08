#include "a0/m2/render.hpp"

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using a0::m2::StitchCropPixels;
using a0::m2::StitchLayout;
using namespace a0::m2::render;

int failures = 0;

void Check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Callable>
std::string RejectionMessage(Callable&& callable) {
    try {
        callable();
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return {};
}

BgrImage Solid(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint8_t red,
    const std::uint8_t green,
    const std::uint8_t blue) {
    BgrImage image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3)};
    for (std::size_t index = 0; index < image.bgr.size(); index += 3) {
        image.bgr[index] = blue;
        image.bgr[index + 1] = green;
        image.bgr[index + 2] = red;
    }
    return image;
}

// Deterministic textured buffer: gradients plus an integer hash term.
BgrImage Textured(const std::uint32_t width, const std::uint32_t height, const std::uint32_t seed) {
    BgrImage image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3)};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t n = (x * 73856093u) ^ (y * 19349663u) ^ (seed * 83492791u);
            n ^= n >> 13;
            n *= 0x5bd1e995u;
            n ^= n >> 15;
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 3;
            image.bgr[offset] = static_cast<std::uint8_t>((x * 255u / (width - 1) + (n & 31u)) & 255u);
            image.bgr[offset + 1] = static_cast<std::uint8_t>((y * 255u / (height - 1) + ((n >> 5) & 31u)) & 255u);
            image.bgr[offset + 2] = static_cast<std::uint8_t>(
                ((x + y) * 255u / (width + height - 2) + ((n >> 10) & 63u)) & 255u);
        }
    }
    return image;
}

std::uint64_t Fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

PairRenderParameters Translation(
    const double dx,
    const double dy,
    const StitchLayout layout,
    const StitchCropPixels crop) {
    return {{1.0, 0.0, dx, 0.0, 1.0, dy, 0.0, 0.0, 1.0}, layout, crop};
}

void TestTranslationRenderMatchesStitcherContract() {
    const auto a = Solid(16, 8, 220, 20, 20);
    const auto b = Solid(16, 8, 20, 20, 220);
    const auto parameters = Translation(12.0, 0.0, StitchLayout::camera_a_left_camera_b_right, {1, 1, 1, 1});
    const auto result = RenderPair(a, b, parameters);
    Check(result.image.width == 26 && result.image.height == 6,
        "fixed translation, overlap and crop must define the output dimensions");
    Check(result.image.bgr.size() == 26U * 6U * 3U, "output buffer must be tightly packed BGR");
    const auto at = [&](const std::uint32_t x, const std::uint32_t y) {
        const auto offset = (static_cast<std::size_t>(y) * result.image.width + x) * 3;
        return std::array<std::uint8_t, 3>{result.image.bgr[offset], result.image.bgr[offset + 1], result.image.bgr[offset + 2]};
    };
    const auto left = at(1, 2);
    const auto overlap = at(13, 2);
    const auto right = at(24, 2);
    Check(left[2] == 220 && left[0] == 20, "CAM-A-only region must keep the CAM-A pixel exactly");
    Check(right[0] == 220 && right[2] == 20, "CAM-B-only region must keep the CAM-B pixel exactly");
    Check(overlap[0] > 65 && overlap[2] > 65, "overlap must feather contributions from both cameras");
    Check(result.seam_navigation.available && result.seam_navigation.x == 13 && result.seam_navigation.y == 2,
        "seam point must be the crop-relative 50 percent feather point");

    const auto again = RenderPair(a, b, parameters);
    Check(again.image.bgr == result.image.bgr && again.seam_navigation.x == result.seam_navigation.x
            && again.seam_navigation.y == result.seam_navigation.y,
        "the same buffers and parameters must render the same bytes");
}

void TestNonIntegerTranslationCoversTheEdgeBand() {
    const auto a = Solid(16, 8, 220, 20, 20);
    const auto b = Solid(16, 8, 20, 20, 220);
    const auto result = RenderPair(
        a, b, Translation(12.5, 0.0, StitchLayout::camera_a_left_camera_b_right, {}));
    Check(result.image.width == 29 && result.image.height == 8,
        "non-integer +12.5 translation must size the canvas to 29x8 without an uncovered pixel");
}

void TestLayoutSelectsFeatherAxis() {
    const auto a = Solid(8, 16, 200, 0, 0);
    const auto b = Solid(8, 16, 0, 0, 200);
    const auto result = RenderPair(
        a, b, Translation(0.0, 12.0, StitchLayout::camera_a_top_camera_b_bottom, {0, 1, 0, 1}));
    Check(result.image.width == 8 && result.image.height == 26, "top/bottom layout must stack along y");
    Check(result.seam_navigation.available && result.seam_navigation.y == 13,
        "top/bottom seam point must lie on the vertical feather midpoint");
}

void TestRenderPinsTexturedOutput() {
    // Raw-buffer pins for the pre-split behaviour (Issue #268). The same
    // fixtures produced byte-identical stitched.jpg files before and after the
    // split, so these values describe the pre-split pixel computation. A change
    // to them is a change of rendered output, not a refactor.
    const auto a = Textured(64, 48, 1);
    const auto b = Textured(64, 48, 2);
    const auto left_right = RenderPair(a, b, PairRenderParameters{
        {1.01, 0.01, 40.3, 0.005, 0.99, 1.7, 0.00003, 0.00001, 1.0},
        StitchLayout::camera_a_left_camera_b_right,
        {0, 4, 4, 4},
    });
    Check(left_right.image.width == 102 && left_right.image.height == 42,
        "left/right projective fixture dimensions");
    Check(Fnv1a64(left_right.image.bgr) == 0x416ec57387fd8c28ULL, "left/right projective fixture raw pixel hash");
    Check(left_right.seam_navigation.x == 52 && left_right.seam_navigation.y == 20,
        "left/right projective fixture seam point");

    const auto top_bottom = RenderPair(a, b, PairRenderParameters{
        {0.99, -0.01, 1.2, 0.008, 1.0, 30.6, 0.00002, 0.00002, 1.0},
        StitchLayout::camera_a_top_camera_b_bottom,
        {4, 0, 5, 0},
    });
    Check(top_bottom.image.width == 56 && top_bottom.image.height == 79,
        "top/bottom projective fixture dimensions");
    Check(Fnv1a64(top_bottom.image.bgr) == 0x3c175c55ed0ebc0fULL, "top/bottom projective fixture raw pixel hash");
    Check(top_bottom.seam_navigation.x == 27 && top_bottom.seam_navigation.y == 39,
        "top/bottom projective fixture seam point");
}

void TestFailClosedGeometry() {
    const auto a = Solid(16, 8, 1, 2, 3);
    const auto b = Solid(16, 8, 3, 2, 1);
    Check(RejectionMessage([&] {
              (void)RenderPair(a, b, Translation(12.0, 3.0, StitchLayout::camera_a_left_camera_b_right, {}));
          }).find("uncovered output pixel") != std::string::npos,
        "a crop that leaves a corner covered by neither camera must be rejected");
    Check(RejectionMessage([&] {
              (void)RenderPair(a, b, Translation(12.0, 0.0, StitchLayout::camera_a_left_camera_b_right, {14, 0, 14, 0}));
          }).find("removes the complete stitched canvas") != std::string::npos,
        "a crop that removes the whole canvas must be rejected");
    Check(RejectionMessage([&] {
              (void)RenderPair(a, b, PairRenderParameters{
                  {1, 0, 0, 0, 1, 0, 0, 0, 0}, StitchLayout::camera_a_left_camera_b_right, {}});
          }).find("invertible") != std::string::npos,
        "a singular transform must be rejected");
    Check(RejectionMessage([&] {
              (void)RenderPair(a, b, PairRenderParameters{
                  {1, 0, std::numeric_limits<double>::quiet_NaN(), 0, 1, 0, 0, 0, 1},
                  StitchLayout::camera_a_left_camera_b_right,
                  {}});
          }).find("finite") != std::string::npos,
        "a non-finite transform must be rejected");
}

// RenderPair must verify its own preconditions: callers other than the stitcher
// (evaluation and measurement tools) call it directly.
void TestRenderPairChecksItsPreconditions() {
    const auto a = Solid(16, 8, 1, 2, 3);
    const auto b = Solid(16, 8, 3, 2, 1);
    // The denominator 0.2 * x - 1 is negative at x = 0 and positive at x = 16.
    Check(RejectionMessage([&] {
              (void)RenderPair(a, b, PairRenderParameters{
                  {1, 0, 0, 0, 1, 0, 0.2, 0, -1}, StitchLayout::camera_a_left_camera_b_right, {}});
          }).find("projective denominator crosses the input image") != std::string::npos,
        "a transform whose denominator changes sign over CAM-B must be rejected");
    auto short_b = b;
    short_b.bgr.pop_back();
    Check(RejectionMessage([&] {
              (void)RenderPair(a, short_b, Translation(12.0, 0.0, StitchLayout::camera_a_left_camera_b_right, {}));
          }).find("CAM-B BGR buffer size") != std::string::npos,
        "a CAM-B buffer shorter than width * height * 3 must be rejected");
    auto short_a = a;
    short_a.bgr.resize(short_a.bgr.size() / 2);
    Check(RejectionMessage([&] {
              (void)RenderPair(short_a, b, Translation(12.0, 0.0, StitchLayout::camera_a_left_camera_b_right, {}));
          }).find("CAM-A BGR buffer size") != std::string::npos,
        "a CAM-A buffer shorter than width * height * 3 must be rejected");
    auto long_b = b;
    long_b.bgr.push_back(0);
    Check(!RejectionMessage([&] {
              (void)RenderPair(a, long_b, Translation(12.0, 0.0, StitchLayout::camera_a_left_camera_b_right, {}));
          }).empty(),
        "a buffer longer than width * height * 3 must also be rejected");
}

void TestCanvasPlan() {
    const auto plan = PlanPairCanvas(
        16, 8, 16, 8, Translation(-5.5, 0.0, StitchLayout::camera_a_left_camera_b_right, {1, 0, 2, 0}));
    Check(plan.origin_x == -6.0 && plan.origin_y == 0.0, "canvas origin must be the floored union minimum");
    Check(plan.canvas_width == 22 && plan.canvas_height == 8, "canvas must be the union of both cameras");
    Check(plan.output_width == 19 && plan.output_height == 8, "output must be the canvas minus the crop");
    Check(plan.b_bounds.minimum_x == -5.5 && plan.b_bounds.maximum_x == 10.5,
        "CAM-B bounds must be the transformed rectangle");
}

void TestMatrixHelpers() {
    const Matrix3 matrix{1.01, 0.01, 40.3, 0.005, 0.99, 1.7, 0.00003, 0.00001, 1.0};
    const auto inverse = Invert(matrix);
    Point forward{};
    Point back{};
    Check(TryTransform(matrix, {10.0, 20.0}, forward) && TryTransform(inverse, forward, back),
        "finite points must transform through the matrix and its inverse");
    Check(std::abs(back.x - 10.0) < 1e-9 && std::abs(back.y - 20.0) < 1e-9,
        "inverse must undo the forward transform");
    Point unused{};
    Check(!TryTransform({1, 0, 0, 0, 1, 0, 1, 0, 0}, {0.0, 5.0}, unused),
        "a zero denominator must not transform");
    Check(RejectionMessage([&] { ValidateProjectiveDomain({1, 0, 0, 0, 1, 0, 0, 0, 1}, 16, 8); }).empty(),
        "an affine matrix must pass the projective domain check");
    Check(RejectionMessage([&] { ValidateProjectiveDomain({1, 0, 0, 0, 1, 0, 0.2, 0, 1}, 16, 8); }).empty(),
        "a positive denominator over the rectangle must pass");
    Check(!RejectionMessage([&] { ValidateProjectiveDomain({1, 0, 0, 0, 1, 0, 0.2, 0, -1}, 16, 8); }).empty(),
        "a denominator that changes sign over the rectangle must be rejected");
    Check(!RejectionMessage([] { (void)PixelCount(0, 4); }).empty(), "empty dimensions must be rejected");
    Check(!RejectionMessage([] { (void)PixelCount(kMaximumImageDimension + 1, 1); }).empty(),
        "an oversized dimension must be rejected");
    Check(!RejectionMessage([] { (void)PixelCount(20'000, 20'000); }).empty(),
        "a pixel count above the decoded limit must be rejected");
    Check(PixelCount(16, 8) == 128, "pixel count must be width times height");
}

void TestBilinearSampling() {
    BgrImage image{2, 2, {0, 0, 0, 100, 100, 100, 0, 0, 0, 100, 100, 100}};
    std::array<double, 3> pixel{};
    Check(SampleBilinear(image, 0.5, 0.5, pixel) && pixel[0] == 50.0 && pixel[2] == 50.0,
        "interior samples must interpolate linearly");
    Check(SampleBilinear(image, 1.5, 0.0, pixel) && pixel[1] == 100.0,
        "the fractional band past the last pixel centre must clamp to the last column");
    Check(!SampleBilinear(image, 2.0, 0.0, pixel) && !SampleBilinear(image, -0.01, 0.0, pixel),
        "samples outside [0, width) must report no coverage");
}

#if defined(_WIN32)
// Reads this executable's import table. The render library must not pull in
// ole32, windowscodecs or bcrypt, directly or through anything it links.
void TestExecutableDoesNotImportJpegOrCryptoLibraries() {
    const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    Check(directory.VirtualAddress != 0, "the test executable must expose an import table to inspect");
    std::vector<std::string> imported;
    if (directory.VirtualAddress != 0) {
        const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        for (; descriptor->Name != 0; ++descriptor) {
            std::string name(reinterpret_cast<const char*>(base + descriptor->Name));
            std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            imported.push_back(name);
        }
    }
    // Delay-loaded imports live in a separate directory and would otherwise
    // escape the check above.
    const auto& delay_directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delay_directory.VirtualAddress != 0) {
        const auto* delay_descriptor =
            reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delay_directory.VirtualAddress);
        for (; delay_descriptor->DllNameRVA != 0; ++delay_descriptor) {
            std::string name(reinterpret_cast<const char*>(base + delay_descriptor->DllNameRVA));
            std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            imported.push_back(name);
        }
    }
    Check(!imported.empty(), "the import table of the test executable must list at least the C runtime");
    for (const auto& name : imported) {
        Check(name != "ole32.dll" && name != "windowscodecs.dll" && name != "bcrypt.dll",
            "a0_m2_render must not link " + name);
    }
}
#endif

} // namespace

int main() {
    try {
        TestTranslationRenderMatchesStitcherContract();
        TestNonIntegerTranslationCoversTheEdgeBand();
        TestLayoutSelectsFeatherAxis();
        TestRenderPinsTexturedOutput();
        TestFailClosedGeometry();
        TestRenderPairChecksItsPreconditions();
        TestCanvasPlan();
        TestMatrixHelpers();
        TestBilinearSampling();
#if defined(_WIN32)
        TestExecutableDoesNotImportJpegOrCryptoLibraries();
#endif
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " render check(s) failed\n";
        return 1;
    }
    std::cout << "m2 render contracts passed\n";
    return 0;
}
