#include "a0/m2/document_render.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace a0::m2::render {
namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void ValidateImage(const BgrImage& image) {
    Require(image.bgr.size() == PixelCount(image.width, image.height) * 3,
        "BGR buffer length differs from dimensions");
}
bool Inside(const BgrImage& image, Point source) {
    return std::isfinite(source.x) && std::isfinite(source.y) && source.x >= 0 && source.y >= 0 &&
        source.x < image.width && source.y < image.height;
}
std::array<double, 4> CubicWeights(double t) {
    const double t2 = t * t, t3 = t2 * t;
    return {-.5 * t + t2 - .5 * t3, 1 - 2.5 * t2 + 1.5 * t3,
        .5 * t + 2 * t2 - 1.5 * t3, -.5 * t2 + .5 * t3};
}
bool SampleCubicUnchecked(const BgrImage& image, double x, double y, std::array<double, 3>& pixel) {
    if (!Inside(image, {x, y})) return false;
    const auto ix = static_cast<std::int32_t>(std::floor(x));
    const auto iy = static_cast<std::int32_t>(std::floor(y));
    const auto wx = CubicWeights(x - ix), wy = CubicWeights(y - iy);
    pixel = {};
    for (int row = 0; row < 4; ++row) {
        const auto sy = std::clamp(iy + row - 1, 0, static_cast<std::int32_t>(image.height) - 1);
        for (int column = 0; column < 4; ++column) {
            const auto sx = std::clamp(ix + column - 1, 0, static_cast<std::int32_t>(image.width) - 1);
            const auto offset = (static_cast<std::size_t>(sy) * image.width + sx) * 3;
            const double weight = wy[row] * wx[column];
            for (std::size_t channel = 0; channel < 3; ++channel) pixel[channel] += weight * image.bgr[offset + channel];
        }
    }
    return true;
}
std::uint32_t RequiredPixels(std::uint64_t micrometres, std::uint32_t dpi) {
    Require(micrometres <= std::numeric_limits<std::uint64_t>::max() / dpi, "raster extent product overflows");
    const auto numerator = micrometres * dpi;
    const auto pixels = numerator / 25400 + (numerator % 25400 != 0 ? 1 : 0);
    Require(pixels > 0 && pixels <= kMaximumImageDimension, "raster dimension outside engine limits");
    return static_cast<std::uint32_t>(pixels);
}
std::array<Point, 4> RasterCorners(const OutputRaster& raster) {
    const double left = static_cast<double>(raster.region_um.left) / 1000.0;
    const double top = static_cast<double>(raster.region_um.top) / 1000.0;
    const double pitch = 25.4 / raster.dpi;
    // Include the ceil raster's outer edges, not just the declared region.
    const double right = std::max(static_cast<double>(raster.region_um.right) / 1000.0,
        left + raster.width_pixels * pitch);
    const double bottom = std::max(static_cast<double>(raster.region_um.bottom) / 1000.0,
        top + raster.height_pixels * pitch);
    return {Point{left, top}, Point{right, top}, Point{right, bottom}, Point{left, bottom}};
}
void ValidateLens(const LensDistortion& lens) {
    for (const auto value : {lens.k1, lens.k2, lens.k3, lens.p1, lens.p2})
        Require(std::isfinite(value), "lens coefficients must all be finite");
}
struct Band { std::uint32_t begin, end; };

bool MapSample(const OutputToCamera& mapping, Point sample, Point& source) {
    if (!mapping(sample, source)) return false;
    Require(std::isfinite(source.x) && std::isfinite(source.y), "mapping returned nonfinite camera coordinates");
    return true;
}

} // namespace

bool SampleBicubic(const BgrImage& image, double x, double y, std::array<double, 3>& pixel) {
    if (!Inside(image, {x, y})) return false;
    ValidateImage(image);
    return SampleCubicUnchecked(image, x, y, pixel);
}

bool TryProjectDocument(const CameraProjection& camera, Point document_mm, Point& raw_pixel) {
    const auto& h = camera.document_to_image;
    const auto& i = camera.intrinsics;
    const auto& d = camera.distortion;
    if (!std::isfinite(document_mm.x) || !std::isfinite(document_mm.y) || !(i.fx_pixels > 0) || !(i.fy_pixels > 0)) return false;
    const double w = h[6] * document_mm.x + h[7] * document_mm.y + h[8];
    if (!(w > 0) || !std::isfinite(w)) return false;
    const double u = (h[0] * document_mm.x + h[1] * document_mm.y + h[2]) / w;
    const double v = (h[3] * document_mm.x + h[4] * document_mm.y + h[5]) / w;
    const double x = (u - i.cx_pixels) / i.fx_pixels;
    const double y = (v - i.cy_pixels) / i.fy_pixels;
    const double r2 = x * x + y * y;
    const double radial = 1 + d.k1 * r2 + d.k2 * r2 * r2 + d.k3 * r2 * r2 * r2;
    const double xd = x * radial + 2 * d.p1 * x * y + d.p2 * (r2 + 2 * x * x);
    const double yd = y * radial + d.p1 * (r2 + 2 * y * y) + 2 * d.p2 * x * y;
    raw_pixel = {i.fx_pixels * xd + i.cx_pixels, i.fy_pixels * yd + i.cy_pixels};
    return std::isfinite(raw_pixel.x) && std::isfinite(raw_pixel.y);
}

void ValidateRadialMonotonicity(const LensDistortion& lens, double maximum_radius_squared) {
    ValidateLens(lens);
    Require(std::isfinite(maximum_radius_squared) && maximum_radius_squared >= 0, "invalid lens radius domain");
    const long double a = 7.L * lens.k3, b = 5.L * lens.k2, c = 3.L * lens.k1;
    const long double maximum = maximum_radius_squared;
    const auto evaluate = [&](long double t) {
        const long double value = ((a * t + b) * t + c) * t + 1;
        Require(std::isfinite(value) && value > 0, "radial mapping folds in the output domain");
    };
    evaluate(0);
    evaluate(maximum);
    // Extrema of the cubic derivative. Scale the quadratic before solving;
    // a cancellation-resistant root and its product partner cover both roots.
    long double qa = 3 * a, qb = 2 * b, qc = c;
    const long double scale = std::max({std::abs(qa), std::abs(qb), std::abs(qc)});
    if (scale == 0) return;
    Require(std::isfinite(scale), "radial derivative coefficients overflow");
    qa /= scale; qb /= scale; qc /= scale;
    const auto at = [&](long double t) { if (std::isfinite(t) && t > 0 && t < maximum) evaluate(t); };
    if (qa == 0) { if (qb != 0) at(-qc / qb); return; }
    const long double discriminant = qb * qb - 4 * qa * qc;
    if (discriminant < 0) return;
    const long double q = -.5L * (qb + std::copysign(std::sqrt(discriminant), qb));
    if (q == 0) at(-qb / (2 * qa));
    else { at(q / qa); at(qc / q); }
}

void ValidateOutputRaster(const OutputRaster& raster) {
    const auto& r = raster.region_um;
    Require(r.left >= 0 && r.top >= 0 && r.right > r.left && r.bottom > r.top, "invalid micrometre region");
    Require(raster.dpi > 0 && raster.dpi <= 65535, "DPI outside JFIF range");
    Require(raster.width_pixels == RequiredPixels(static_cast<std::uint64_t>(r.right - r.left), raster.dpi) &&
        raster.height_pixels == RequiredPixels(static_cast<std::uint64_t>(r.bottom - r.top), raster.dpi),
        "declared raster dimensions disagree with exact micrometre/DPI ceiling");
    (void)PixelCount(raster.width_pixels, raster.height_pixels);
}

void ValidateCameraProjection(const CameraProjection& camera, const OutputRaster& raster) {
    ValidateOutputRaster(raster);
    const auto& i = camera.intrinsics;
    for (const auto value : {i.fx_pixels, i.fy_pixels, i.cx_pixels, i.cy_pixels})
        Require(std::isfinite(value), "intrinsics must all be finite");
    Require(i.fx_pixels > 0 && i.fy_pixels > 0, "focal lengths must be positive");
    ValidateLens(camera.distortion);
    const auto& h = camera.document_to_image;
    for (const auto value : h) Require(std::isfinite(value), "homography must be finite");
    Require(h[8] == 1, "document homography must be normalized to h22=1");
    const long double determinant = static_cast<long double>(h[0]) * (static_cast<long double>(h[4]) * h[8] - static_cast<long double>(h[5]) * h[7])
        - static_cast<long double>(h[1]) * (static_cast<long double>(h[3]) * h[8] - static_cast<long double>(h[5]) * h[6])
        + static_cast<long double>(h[2]) * (static_cast<long double>(h[3]) * h[7] - static_cast<long double>(h[4]) * h[6]);
    Require(std::isfinite(determinant) && determinant != 0, "document homography is numerically singular");
    double maximum_r2 = 0;
    for (const auto point : RasterCorners(raster)) {
        const double w = h[6] * point.x + h[7] * point.y + 1;
        Require(std::isfinite(w) && w > 0, "projective denominator is not positive throughout the ceil raster");
        const double u = (h[0] * point.x + h[1] * point.y + h[2]) / w;
        const double v = (h[3] * point.x + h[4] * point.y + h[5]) / w;
        const double x = (u - i.cx_pixels) / i.fx_pixels, y = (v - i.cy_pixels) / i.fy_pixels;
        const double r2 = x * x + y * y;
        Require(std::isfinite(r2), "normalized projection radius is not representable");
        maximum_r2 = std::max(maximum_r2, r2);
    }
    // Positive-denominator homography maps the rectangle to a convex quad;
    // squared normalized radius is convex and reaches its maximum at a corner.
    ValidateRadialMonotonicity(camera.distortion, maximum_r2);
}

PairRenderResult RenderMappedPair(const BgrImage& a, const BgrImage& b, const MappedRenderParameters& parameters) {
    ValidateImage(a); ValidateImage(b);
    const auto count = PixelCount(parameters.width, parameters.height);
    Require(parameters.camera_a && parameters.camera_b, "both output-to-camera mappings are required");
    Require(parameters.layout == StitchLayout::camera_a_left_camera_b_right ||
        parameters.layout == StitchLayout::camera_a_top_camera_b_bottom, "unknown camera layout");
    Require(parameters.resampling == Resampling::bilinear || parameters.resampling == Resampling::bicubic_catmull_rom,
        "unknown resampling kernel");
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(count));
    for (std::uint32_t y = 0; y < parameters.height; ++y) {
        for (std::uint32_t x = 0; x < parameters.width; ++x) {
            Point pa{}, pb{};
            const bool has_a = MapSample(parameters.camera_a, {static_cast<double>(x), static_cast<double>(y)}, pa) && Inside(a, pa);
            const bool has_b = MapSample(parameters.camera_b, {static_cast<double>(x), static_cast<double>(y)}, pb) && Inside(b, pb);
            auto& mask = coverage[static_cast<std::size_t>(y) * parameters.width + x];
            mask = static_cast<std::uint8_t>((has_a ? 1 : 0) | (has_b ? 2 : 0));
            Require(mask != 0, "document raster contains an uncovered sample");
        }
    }
    const bool horizontal = parameters.layout == StitchLayout::camera_a_left_camera_b_right;
    const auto line_count = horizontal ? parameters.height : parameters.width;
    const auto line_length = horizontal ? parameters.width : parameters.height;
    std::vector<std::vector<Band>> bands(line_count);
    const auto mask_at = [&](std::uint32_t line, std::uint32_t position) {
        const auto x = horizontal ? position : line, y = horizontal ? line : position;
        return coverage[static_cast<std::size_t>(y) * parameters.width + x];
    };
    for (std::uint32_t line = 0; line < line_count; ++line) {
        std::uint32_t position = 0;
        while (position < line_length) {
            if (mask_at(line, position) != 3) { ++position; continue; }
            const auto begin = position++;
            while (position < line_length && mask_at(line, position) == 3) ++position;
            bands[line].push_back({begin, position});
        }
    }
    PairRenderResult result;
    result.image = {parameters.width, parameters.height, std::vector<std::uint8_t>(static_cast<std::size_t>(count) * 3)};
    const auto sample = [&](const BgrImage& image, Point source, std::array<double, 3>& pixel) {
        if (!Inside(image, source)) return false;
        return parameters.resampling == Resampling::bilinear
            ? SampleBilinear(image, source.x, source.y, pixel)
            : SampleCubicUnchecked(image, source.x, source.y, pixel);
    };
    // Bands and coverage are immutable. Each output row only writes its own
    // image bytes; a future row executor can merge seam candidates separately.
    for (std::uint32_t y = 0; y < parameters.height; ++y) {
        for (std::uint32_t x = 0; x < parameters.width; ++x) {
            Point pa{}, pb{};
            std::array<double, 3> pixel_a{}, pixel_b{};
            const bool has_a = MapSample(parameters.camera_a, {static_cast<double>(x), static_cast<double>(y)}, pa) && sample(a, pa, pixel_a);
            const bool has_b = MapSample(parameters.camera_b, {static_cast<double>(x), static_cast<double>(y)}, pb) && sample(b, pb, pixel_b);
            const auto offset = (static_cast<std::size_t>(y) * parameters.width + x) * 3;
            Require(coverage[offset / 3] == ((has_a ? 1 : 0) | (has_b ? 2 : 0)), "mapping changed coverage between traversals");
            double weight = has_b ? 1.0 : 0.0;
            if (has_a && has_b) {
                const auto primary = horizontal ? x : y;
                const auto& runs = bands[horizontal ? y : x];
                const auto band = std::lower_bound(runs.begin(), runs.end(), primary,
                    [](const Band& run, std::uint32_t value) { return run.end <= value; });
                Require(band != runs.end() && band->begin <= primary, "overlap band lookup failed");
                weight = static_cast<double>(primary - band->begin) / (band->end - band->begin);
            }
            result.seam_navigation.Observe(parameters.layout, x, y, parameters.width, parameters.height, has_a, has_b, weight);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const double value = (1 - weight) * pixel_a[channel] + weight * pixel_b[channel];
                result.image.bgr[offset + channel] = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
            }
        }
    }
    return result;
}

PairRenderResult RenderDocumentPair(const BgrImage& a, const BgrImage& b, const DocumentRenderParameters& parameters) {
    ValidateCameraProjection(parameters.camera_a, parameters.raster);
    ValidateCameraProjection(parameters.camera_b, parameters.raster);
    const double pitch = 25.4 / parameters.raster.dpi;
    const double left = static_cast<double>(parameters.raster.region_um.left) / 1000.0;
    const double top = static_cast<double>(parameters.raster.region_um.top) / 1000.0;
    const auto mapping = [&](const CameraProjection& camera) -> OutputToCamera {
        return [camera, pitch, left, top](Point output, Point& raw) {
            const Point document{left + (output.x + .5) * pitch, top + (output.y + .5) * pitch};
            Require(TryProjectDocument(camera, document, raw), "document projection is not numerically representable");
            return true;
        };
    };
    return RenderMappedPair(a, b, {parameters.raster.width_pixels, parameters.raster.height_pixels,
        mapping(parameters.camera_a), mapping(parameters.camera_b), parameters.layout, parameters.resampling});
}

} // namespace a0::m2::render
