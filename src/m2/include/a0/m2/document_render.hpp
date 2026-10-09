#pragma once

#include "a0/m2/render.hpp"

#include <functional>
#include <utility>

namespace a0::m2::render {

// Evaluation offers both kernels; this enum makes no product-quality choice.
enum class Resampling { bilinear, bicubic_catmull_rom };

struct CameraIntrinsics {
    double fx_pixels, fy_pixels, cx_pixels, cy_pixels;
    CameraIntrinsics(double fx, double fy, double cx, double cy)
        : fx_pixels(fx), fy_pixels(fy), cx_pixels(cx), cy_pixels(cy) {}
};
struct LensDistortion {
    double k1, k2, k3, p1, p2;
    LensDistortion(double radial1, double radial2, double radial3, double tangential1, double tangential2)
        : k1(radial1), k2(radial2), k3(radial3), p1(tangential1), p2(tangential2) {}
};
struct CameraProjection {
    CameraIntrinsics intrinsics;
    LensDistortion distortion;
    Matrix3 document_to_image;
    CameraProjection(CameraIntrinsics camera, LensDistortion lens, Matrix3 homography)
        : intrinsics(camera), distortion(lens), document_to_image(homography) {}
};
struct DocumentRegionUm {
    std::int64_t left, top, right, bottom;
    DocumentRegionUm(std::int64_t l, std::int64_t t, std::int64_t r, std::int64_t b)
        : left(l), top(t), right(r), bottom(b) {}
};
struct OutputRaster {
    DocumentRegionUm region_um;
    std::uint32_t dpi, width_pixels, height_pixels;
    OutputRaster(DocumentRegionUm region, std::uint32_t density, std::uint32_t width, std::uint32_t height)
        : region_um(region), dpi(density), width_pixels(width), height_pixels(height) {}
};
struct DocumentRenderParameters {
    OutputRaster raster;
    CameraProjection camera_a, camera_b;
    StitchLayout layout;
    Resampling resampling;
    DocumentRenderParameters(OutputRaster output, CameraProjection a, CameraProjection b,
        StitchLayout order, Resampling kernel)
        : raster(output), camera_a(a), camera_b(b), layout(order), resampling(kernel) {}
};

// Caller-supplied pure mapping from output sample index (i,j) directly to raw
// camera sample coordinates. Used for the exact v1 pixel-space compatibility
// check (ADR-0034 design section 3.4). A false return means no valid projection.
// A true return requires finite coordinates, including for an out-of-frame
// point; a nonfinite result fails instead of using the other camera as fallback.
// The function must be immutable/deterministic; a second traversal verifies
// that the sampling coverage still agrees with the first traversal.
using OutputToCamera = std::function<bool(Point, Point&)>;
struct MappedRenderParameters {
    std::uint32_t width, height;
    OutputToCamera camera_a, camera_b;
    StitchLayout layout;
    Resampling resampling;
    MappedRenderParameters(std::uint32_t w, std::uint32_t h, OutputToCamera a, OutputToCamera b,
        StitchLayout order, Resampling kernel)
        : width(w), height(h), camera_a(std::move(a)), camera_b(std::move(b)), layout(order), resampling(kernel) {}
};

// Catmull-Rom a=-0.5, separable 4x4 support. Coverage is [0,W)x[0,H), exactly
// as for bilinear. Support indexes are clamped at the edges; overshoot is kept
// until the final shared blend/quantization to 8-bit BGR.
bool SampleBicubic(const BgrImage& image, double x, double y, std::array<double, 3>& pixel);

// One forward document -> undistorted image -> normalized Brown-Conrady -> raw
// pixel projection. No Newton/inverse lens solve, intermediate image or I/O.
bool TryProjectDocument(const CameraProjection& camera, Point document_mm, Point& raw_pixel);

// Mathematical validity, not calibration acceptance. Includes all extrema of
// 1+3*k1*t+5*k2*t^2+7*k3*t^3 for 0<=t<=maximum_radius_squared.
void ValidateRadialMonotonicity(const LensDistortion& lens, double maximum_radius_squared);
void ValidateOutputRaster(const OutputRaster& raster);
void ValidateCameraProjection(const CameraProjection& camera, const OutputRaster& raster);

// Coverage is computed on the actual output sample grid. Every contiguous
// overlap run [begin,end) along each row (horizontal layout) or column
// (vertical layout) has CAM-B weight (sampleIndex-begin)/(end-begin).
// Single-camera pixels use that camera; any uncovered sample fails.
// Inputs are immutable and the result holds only BGR and seam navigation.
PairRenderResult RenderMappedPair(const BgrImage& a, const BgrImage& b, const MappedRenderParameters& parameters);
PairRenderResult RenderDocumentPair(const BgrImage& a, const BgrImage& b, const DocumentRenderParameters& parameters);

} // namespace a0::m2::render
