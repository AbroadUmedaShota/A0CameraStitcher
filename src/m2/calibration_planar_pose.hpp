#pragma once

// Internal shared seed for the calibration stages. Coordinates must already
// be undistorted; callers must not fit a homography to raw distorted pixels.
#include "a0/m2/calibration_intrinsics.hpp"

#include <array>
#include <optional>
#include <span>

namespace a0::m2::calibration::pose_detail {
struct PlanarObservation { double x_mm, y_mm, u, v; };
[[nodiscard]] std::optional<ChartPose> EstimatePlanarPose(
    std::span<const PlanarObservation> points, const PinholeIntrinsics& intrinsics);
// Chart mm -> (u, v) by the stage 2 DLT (Hartley normalization, Jacobi), row
// major, Frobenius norm 1 and w > 0 at the centroid. Null for fewer than four
// points or a degenerate configuration. The planar calibration (#374) uses it
// on raw pixels only as a start value, with distortion assumed zero.
[[nodiscard]] std::optional<std::array<double, 9>> FitPlanarHomography(std::span<const PlanarObservation> points);
} // namespace a0::m2::calibration::pose_detail
