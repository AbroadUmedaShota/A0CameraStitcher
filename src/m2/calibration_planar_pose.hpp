#pragma once

// Internal shared seed for the calibration stages. Coordinates must already
// be undistorted; callers must not fit a homography to raw distorted pixels.
#include "a0/m2/calibration_intrinsics.hpp"

#include <optional>
#include <span>

namespace a0::m2::calibration::pose_detail {
struct PlanarObservation { double x_mm, y_mm, u, v; };
[[nodiscard]] std::optional<ChartPose> EstimatePlanarPose(
    std::span<const PlanarObservation> points, const PinholeIntrinsics& intrinsics);
} // namespace a0::m2::calibration::pose_detail
