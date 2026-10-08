#include "a0/m2/render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace a0::m2::render {

std::uint64_t PixelCount(const std::uint32_t width, const std::uint32_t height) {
    const auto count = static_cast<std::uint64_t>(width) * height;
    if (width == 0 || height == 0 || width > kMaximumImageDimension
        || height > kMaximumImageDimension || count > kMaximumDecodedPixels) {
        throw std::invalid_argument("JPEG dimensions are empty or exceed the offline stitch limit");
    }
    return count;
}

namespace {

double TransformDenominator(const Matrix3& matrix, const Point point) {
    return matrix[6] * point.x + matrix[7] * point.y + matrix[8];
}

} // namespace

bool TryTransform(const Matrix3& matrix, const Point point, Point& transformed) {
    const double denominator = TransformDenominator(matrix, point);
    if (!std::isfinite(denominator) || std::abs(denominator) <= kMatrixEpsilon) {
        return false;
    }
    transformed = {
        (matrix[0] * point.x + matrix[1] * point.y + matrix[2]) / denominator,
        (matrix[3] * point.x + matrix[4] * point.y + matrix[5]) / denominator,
    };
    return std::isfinite(transformed.x) && std::isfinite(transformed.y);
}

Point Transform(const Matrix3& matrix, const Point point) {
    Point transformed{};
    if (!TryTransform(matrix, point, transformed)) {
        throw std::invalid_argument("fixed transform maps a coordinate to infinity");
    }
    return transformed;
}

Matrix3 Invert(const Matrix3& matrix) {
    for (const double value : matrix) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("fixed transform values must be finite");
        }
    }
    const double determinant =
        matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7])
        - matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6])
        + matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
    if (!std::isfinite(determinant) || std::abs(determinant) <= kMatrixEpsilon) {
        throw std::invalid_argument("fixed transform must be invertible");
    }
    return {
        (matrix[4] * matrix[8] - matrix[5] * matrix[7]) / determinant,
        (matrix[2] * matrix[7] - matrix[1] * matrix[8]) / determinant,
        (matrix[1] * matrix[5] - matrix[2] * matrix[4]) / determinant,
        (matrix[5] * matrix[6] - matrix[3] * matrix[8]) / determinant,
        (matrix[0] * matrix[8] - matrix[2] * matrix[6]) / determinant,
        (matrix[2] * matrix[3] - matrix[0] * matrix[5]) / determinant,
        (matrix[3] * matrix[7] - matrix[4] * matrix[6]) / determinant,
        (matrix[1] * matrix[6] - matrix[0] * matrix[7]) / determinant,
        (matrix[0] * matrix[4] - matrix[1] * matrix[3]) / determinant,
    };
}

void ValidateProjectiveDomain(
    const Matrix3& matrix,
    const std::uint32_t width,
    const std::uint32_t height) {
    const std::array<Point, 4> corners{{
        {0.0, 0.0},
        {static_cast<double>(width), 0.0},
        {0.0, static_cast<double>(height)},
        {static_cast<double>(width), static_cast<double>(height)},
    }};
    bool positive{};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        const double denominator = TransformDenominator(matrix, corners[index]);
        if (!std::isfinite(denominator) || std::abs(denominator) <= kMatrixEpsilon) {
            throw std::invalid_argument("fixed transform projective denominator crosses the input image");
        }
        const bool current_positive = denominator > 0.0;
        if (index == 0) {
            positive = current_positive;
        } else if (current_positive != positive) {
            throw std::invalid_argument("fixed transform projective denominator crosses the input image");
        }
    }
}

Bounds TransformedBounds(
    const std::uint32_t width,
    const std::uint32_t height,
    const Matrix3& matrix) {
    const std::array<Point, 4> corners{{
        {0.0, 0.0},
        {static_cast<double>(width), 0.0},
        {0.0, static_cast<double>(height)},
        {static_cast<double>(width), static_cast<double>(height)},
    }};
    const Point first = Transform(matrix, corners[0]);
    Bounds bounds{first.x, first.y, first.x, first.y};
    for (std::size_t index = 1; index < corners.size(); ++index) {
        const Point point = Transform(matrix, corners[index]);
        bounds.minimum_x = std::min(bounds.minimum_x, point.x);
        bounds.minimum_y = std::min(bounds.minimum_y, point.y);
        bounds.maximum_x = std::max(bounds.maximum_x, point.x);
        bounds.maximum_y = std::max(bounds.maximum_y, point.y);
    }
    return bounds;
}

bool SampleBilinear(const BgrImage& image, const double x, const double y, std::array<double, 3>& pixel) {
    // GitHub Issue #86: the coverage test matches the canvas size and bounds
    // (corner model: an image occupies [0,width) x [0,height)). The earlier
    // pixel-center model accepted only [0,width-1], so an edge band slightly
    // over one pixel wide (for example under a pure +50.5px translation) was
    // reported as an uncovered pixel and the stitch always failed. For integer
    // coordinates x > width-1 and x >= width are equivalent, so integer
    // fixtures are unchanged; only the fractional band between width-1 and
    // width is now covered, clamped to the last column by the existing
    // x1 = min(x0+1, width-1).
    if (x < 0.0 || y < 0.0 || x >= static_cast<double>(image.width)
        || y >= static_cast<double>(image.height)) {
        return false;
    }
    const auto x0 = static_cast<std::uint32_t>(std::floor(x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(y));
    const auto x1 = std::min(x0 + 1, image.width - 1);
    const auto y1 = std::min(y0 + 1, image.height - 1);
    const double dx = x - x0;
    const double dy = y - y0;
    for (std::size_t channel = 0; channel < pixel.size(); ++channel) {
        const auto at = [&](const std::uint32_t sx, const std::uint32_t sy) {
            return static_cast<double>(image.bgr[(static_cast<std::size_t>(sy) * image.width + sx) * 3 + channel]);
        };
        pixel[channel] = (1.0 - dy) * ((1.0 - dx) * at(x0, y0) + dx * at(x1, y0))
            + dy * ((1.0 - dx) * at(x0, y1) + dx * at(x1, y1));
    }
    return true;
}

double FeatherWeight(
    const StitchLayout layout,
    const double x,
    const double y,
    const Bounds& a_bounds,
    const Bounds& b_bounds) {
    const double start = layout == StitchLayout::camera_a_left_camera_b_right
        ? std::max(a_bounds.minimum_x, b_bounds.minimum_x)
        : std::max(a_bounds.minimum_y, b_bounds.minimum_y);
    const double end = layout == StitchLayout::camera_a_left_camera_b_right
        ? std::min(a_bounds.maximum_x, b_bounds.maximum_x)
        : std::min(a_bounds.maximum_y, b_bounds.maximum_y);
    const double coordinate = layout == StitchLayout::camera_a_left_camera_b_right ? x : y;
    if (end <= start + kMatrixEpsilon) {
        return 0.5;
    }
    return std::clamp((coordinate - start) / (end - start), 0.0, 1.0);
}

void SeamNavigationCandidate::Observe(
    const StitchLayout layout,
    const std::uint32_t output_x,
    const std::uint32_t output_y,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const bool has_a,
    const bool has_b,
    const double b_weight) {
    // A shared sample at a 0/1 endpoint has no contribution from one body, so
    // it is not a useful seam point. There is intentionally no fallback to an
    // image center when the rendered result has no feathered overlap.
    if (!has_a || !has_b || b_weight <= 0.0 || b_weight >= 1.0) return;

    const double feather_distance = std::abs(b_weight - 0.5);
    const double perpendicular = layout == StitchLayout::camera_a_left_camera_b_right
        ? std::abs(static_cast<double>(output_y) - (static_cast<double>(output_height) - 1.0) / 2.0)
        : std::abs(static_cast<double>(output_x) - (static_cast<double>(output_width) - 1.0) / 2.0);
    const double primary = layout == StitchLayout::camera_a_left_camera_b_right
        ? std::abs(static_cast<double>(output_x) - (static_cast<double>(output_width) - 1.0) / 2.0)
        : std::abs(static_cast<double>(output_y) - (static_cast<double>(output_height) - 1.0) / 2.0);
    const auto score = std::tuple{feather_distance, perpendicular, primary, output_y, output_x};
    const auto current = std::tuple{
        feather_midpoint_distance, perpendicular_center_distance, primary_center_distance, y, x};
    if (!available || score < current) {
        available = true;
        x = output_x;
        y = output_y;
        feather_midpoint_distance = feather_distance;
        perpendicular_center_distance = perpendicular;
        primary_center_distance = primary;
    }
}

PairCanvasPlan PlanPairCanvas(
    const std::uint32_t camera_a_width,
    const std::uint32_t camera_a_height,
    const std::uint32_t camera_b_width,
    const std::uint32_t camera_b_height,
    const PairRenderParameters& parameters) {
    const Bounds a_bounds{0.0, 0.0, static_cast<double>(camera_a_width), static_cast<double>(camera_a_height)};
    const Bounds b_bounds = TransformedBounds(camera_b_width, camera_b_height, parameters.camera_b_to_camera_a);
    const double minimum_x = std::floor(std::min(a_bounds.minimum_x, b_bounds.minimum_x));
    const double minimum_y = std::floor(std::min(a_bounds.minimum_y, b_bounds.minimum_y));
    const double maximum_x = std::ceil(std::max(a_bounds.maximum_x, b_bounds.maximum_x));
    const double maximum_y = std::ceil(std::max(a_bounds.maximum_y, b_bounds.maximum_y));
    const double canvas_width_value = maximum_x - minimum_x;
    const double canvas_height_value = maximum_y - minimum_y;
    if (canvas_width_value > std::numeric_limits<std::uint32_t>::max()
        || canvas_height_value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("fixed transform canvas exceeds supported dimensions");
    }
    const auto canvas_width = static_cast<std::uint32_t>(canvas_width_value);
    const auto canvas_height = static_cast<std::uint32_t>(canvas_height_value);
    (void)PixelCount(canvas_width, canvas_height);
    const auto horizontal_crop = static_cast<std::uint64_t>(parameters.crop.left) + parameters.crop.right;
    const auto vertical_crop = static_cast<std::uint64_t>(parameters.crop.top) + parameters.crop.bottom;
    if (horizontal_crop >= canvas_width || vertical_crop >= canvas_height) {
        throw std::invalid_argument("approved crop removes the complete stitched canvas");
    }
    return {
        a_bounds,
        b_bounds,
        minimum_x,
        minimum_y,
        canvas_width,
        canvas_height,
        canvas_width - static_cast<std::uint32_t>(horizontal_crop),
        canvas_height - static_cast<std::uint32_t>(vertical_crop),
    };
}

PairRenderResult RenderPair(
    const BgrImage& camera_a,
    const BgrImage& camera_b,
    const PairRenderParameters& parameters) {
    const auto inverse_b = Invert(parameters.camera_b_to_camera_a);
    const PairCanvasPlan plan = PlanPairCanvas(
        camera_a.width, camera_a.height, camera_b.width, camera_b.height, parameters);
    const Bounds& a_bounds = plan.a_bounds;
    const Bounds& b_bounds = plan.b_bounds;
    const double minimum_x = plan.origin_x;
    const double minimum_y = plan.origin_y;

    // GitHub Issue #102 (item 3): b_bounds is the axis-aligned bounding box, in
    // the same global/output coordinate space as global_x/global_y below, of
    // CAM-B's rectangle mapped forward through the approved fixed transform
    // (PlanPairCanvas already computed it for canvas sizing). Because that
    // forward transform is a fixed projective map validated by
    // ValidateProjectiveDomain to not cross its zero-denominator line inside
    // the CAM-B rectangle, the transform is a homeomorphism there and the
    // image of the rectangle is exactly the convex quadrilateral spanned by
    // its four transformed corners -- so b_bounds, the AABB of those corners,
    // is already a true, non-lossy superset of every global coordinate CAM-B
    // can possibly cover. A global point strictly outside b_bounds can never
    // produce a has_b=true, so skipping the inverse transform and bilinear
    // sample for such points cannot change any output pixel; it only skips
    // work that was always going to end in has_b=false.
    //
    // The padding below is not required by that geometric argument, but is
    // added anyway as a second, independent safety margin against floating-
    // point drift: b_bounds is computed via the forward matrix, while the
    // per-pixel skip test below is compared against values ultimately used
    // with the separately-computed inverse matrix (inverse_b), so the two are
    // not guaranteed to agree to the last bit right at the boundary. Rounding
    // the box outward to whole pixels and then padding by an extra
    // kCameraBSkipSafetyMarginPixels on every side keeps the skip test
    // conservative: on any doubt near the edge, this does not skip, and the
    // pixel falls through to the exact same TryTransform+SampleBilinear path
    // used before this change, producing an identical result.
    constexpr double kCameraBSkipSafetyMarginPixels = 2.0;
    const double b_skip_minimum_x = std::floor(b_bounds.minimum_x) - kCameraBSkipSafetyMarginPixels;
    const double b_skip_minimum_y = std::floor(b_bounds.minimum_y) - kCameraBSkipSafetyMarginPixels;
    const double b_skip_maximum_x = std::ceil(b_bounds.maximum_x) + kCameraBSkipSafetyMarginPixels;
    const double b_skip_maximum_y = std::ceil(b_bounds.maximum_y) + kCameraBSkipSafetyMarginPixels;

    PairRenderResult result;
    result.image = BgrImage{plan.output_width, plan.output_height, {}};
    BgrImage& output = result.image;
    output.bgr.resize(static_cast<std::size_t>(PixelCount(output.width, output.height) * 3));
    SeamNavigationCandidate& seam_navigation = result.seam_navigation;
    for (std::uint32_t output_y = 0; output_y < output.height; ++output_y) {
        const double global_y = minimum_y + parameters.crop.top + output_y;
        const bool row_may_hit_camera_b = global_y >= b_skip_minimum_y && global_y <= b_skip_maximum_y;
        for (std::uint32_t output_x = 0; output_x < output.width; ++output_x) {
            const double global_x = minimum_x + parameters.crop.left + output_x;
            std::array<double, 3> pixel_a{};
            std::array<double, 3> pixel_b{};
            const bool has_a = SampleBilinear(camera_a, global_x, global_y, pixel_a);
            const bool may_hit_camera_b = row_may_hit_camera_b
                && global_x >= b_skip_minimum_x && global_x <= b_skip_maximum_x;
            Point source_b{};
            const bool has_b = may_hit_camera_b
                && TryTransform(inverse_b, {global_x, global_y}, source_b)
                && SampleBilinear(camera_b, source_b.x, source_b.y, pixel_b);
            if (!has_a && !has_b) {
                throw std::invalid_argument("approved crop contains an uncovered output pixel");
            }
            const double b_weight = has_a && has_b
                ? FeatherWeight(parameters.layout, global_x, global_y, a_bounds, b_bounds)
                : (has_b ? 1.0 : 0.0);
            seam_navigation.Observe(
                parameters.layout,
                output_x,
                output_y,
                output.width,
                output.height,
                has_a,
                has_b,
                b_weight);
            const auto offset = (static_cast<std::size_t>(output_y) * output.width + output_x) * 3;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const double value = (1.0 - b_weight) * pixel_a[channel] + b_weight * pixel_b[channel];
                output.bgr[offset + channel] = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
            }
        }
    }
    return result;
}

} // namespace a0::m2::render
