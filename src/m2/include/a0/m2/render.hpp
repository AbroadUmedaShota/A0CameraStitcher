#pragma once

// Pure pixel computation for the fixed-transform two-camera stitch.
//
// This header and render.cpp form the a0_m2_render library. They hold the
// inverse mapping, bilinear sampling, linear feather and crop that turn two
// decoded BGR buffers and a fixed 3x3 transform into one BGR buffer. They have
// no Win32, COM, file, hashing or manifest dependency: inputs are decoded
// buffers plus fixed parameters, outputs are buffers, and every failure is a
// std::invalid_argument. JPEG decoding, input locking, encoding, publishing and
// the StitchJob manifest stay in a0_m2_offline_stitcher, which depends on this
// library and never the other way around.
//
// Coordinate model: an image of width W and height H occupies [0, W) x [0, H)
// in a corner model (pixel (i, j) covers [i, i+1) x [j, j+1)). CAM-A is the
// output coordinate reference; the transform maps CAM-B pixel coordinates into
// that space.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

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

namespace render {

inline constexpr std::uint64_t kMaximumDecodedPixels = 200'000'000;
inline constexpr std::uint32_t kMaximumImageDimension = 32'768;
inline constexpr double kMatrixEpsilon = 1e-12;

// Row-major 3x3 homography: {m00, m01, m02, m10, m11, m12, m20, m21, m22}.
using Matrix3 = std::array<double, 9>;

// Interleaved 8-bit BGR, row-major, no row padding (stride = width * 3).
struct BgrImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> bgr;
};

struct Point {
    double x{};
    double y{};
};

struct Bounds {
    double minimum_x{};
    double minimum_y{};
    double maximum_x{};
    double maximum_y{};
};

// Throws std::invalid_argument when the dimensions are empty or exceed
// kMaximumImageDimension / kMaximumDecodedPixels. Returns width * height.
std::uint64_t PixelCount(std::uint32_t width, std::uint32_t height);

// Throws std::invalid_argument when a value is not finite or the matrix is not
// invertible.
Matrix3 Invert(const Matrix3& matrix);

// Throws std::invalid_argument when the projective denominator reaches zero or
// changes sign over the width x height rectangle.
void ValidateProjectiveDomain(const Matrix3& matrix, std::uint32_t width, std::uint32_t height);

// False when the denominator is zero or the result is not finite.
bool TryTransform(const Matrix3& matrix, Point point, Point& transformed);

// Throws std::invalid_argument when the point maps to infinity.
Point Transform(const Matrix3& matrix, Point point);

// Axis-aligned bounds of the width x height rectangle mapped through `matrix`.
Bounds TransformedBounds(std::uint32_t width, std::uint32_t height, const Matrix3& matrix);

// Bilinear sample at (x, y) in the corner model. Returns false when the point
// is outside [0, width) x [0, height); the last row and column are clamped for
// the fractional band between width-1 and width.
bool SampleBilinear(const BgrImage& image, double x, double y, std::array<double, 3>& pixel);

// Linear CAM-B weight across the geometric overlap of the two bounds, along the
// layout's primary axis. 0.5 when the overlap is empty.
double FeatherWeight(
    StitchLayout layout, double x, double y, const Bounds& a_bounds, const Bounds& b_bounds);

// Review navigation point accumulated from the same has_a / has_b / feather
// weight values that produce the pixels, so it always names a pixel of the real
// feathered overlap. `available` stays false when there is no such pixel; there
// is deliberately no fallback to an image center.
struct SeamNavigationCandidate {
    bool available{};
    std::uint32_t x{};
    std::uint32_t y{};
    double feather_midpoint_distance{std::numeric_limits<double>::infinity()};
    double perpendicular_center_distance{std::numeric_limits<double>::infinity()};
    double primary_center_distance{std::numeric_limits<double>::infinity()};

    void Observe(
        StitchLayout layout,
        std::uint32_t output_x,
        std::uint32_t output_y,
        std::uint32_t output_width,
        std::uint32_t output_height,
        bool has_a,
        bool has_b,
        double b_weight);
};

// Fixed parameters of a two-camera render. No default values are supplied here;
// the caller owns them (for the product path they come from an approved rig
// profile).
struct PairRenderParameters {
    Matrix3 camera_b_to_camera_a;
    StitchLayout layout;
    StitchCropPixels crop;
};

// Canvas geometry of a pair render. `origin_x` / `origin_y` are the global
// coordinates of the canvas top-left (before crop); output pixel (x, y) is
// global (origin_x + crop.left + x, origin_y + crop.top + y).
struct PairCanvasPlan {
    Bounds a_bounds;
    Bounds b_bounds;
    double origin_x{};
    double origin_y{};
    std::uint32_t canvas_width{};
    std::uint32_t canvas_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
};

// Sizes the canvas as the union of CAM-A and the transformed CAM-B, then applies
// the crop. Throws std::invalid_argument when the canvas exceeds the supported
// dimensions or the crop removes all of it.
PairCanvasPlan PlanPairCanvas(
    std::uint32_t camera_a_width,
    std::uint32_t camera_a_height,
    std::uint32_t camera_b_width,
    std::uint32_t camera_b_height,
    const PairRenderParameters& parameters);

struct PairRenderResult {
    BgrImage image;
    SeamNavigationCandidate seam_navigation;
};

// Renders the cropped union of CAM-A and the inverse-mapped CAM-B, linearly
// feathering the geometric overlap. Deterministic: the same buffers and
// parameters always produce the same bytes. Throws std::invalid_argument when
// the transform is not invertible or its projective denominator changes sign
// over the CAM-B rectangle (ValidateProjectiveDomain), a buffer length differs
// from width * height * 3, the canvas is unsupported, or an output pixel is
// covered by neither camera. These preconditions are checked here, so callers
// other than the stitcher need not repeat them.
PairRenderResult RenderPair(
    const BgrImage& camera_a,
    const BgrImage& camera_b,
    const PairRenderParameters& parameters);

} // namespace render
} // namespace a0::m2
