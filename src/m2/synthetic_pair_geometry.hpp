#pragma once

#include "a0/m2/synthetic_pair.hpp"

namespace a0::m2::synthetic::detail {

// Residual allowed after inverting the distortion, in raw camera pixels.
inline constexpr double kInversionResidualPixels = 1e-7;

struct NormalizedPoint {
    double x{};
    double y{};
};

// Distorted normalized coordinates of an undistorted normalized point.
[[nodiscard]] NormalizedPoint DistortNormalized(const DistortionSpec& distortion, double x, double y) noexcept;

// Inverts DistortNormalized by Newton's method in two dimensions. Returns false
// when the Jacobian degenerates, the iteration does not settle, or the residual
// after the last step exceeds tolerance_x / tolerance_y (normalized units). Only
// basic arithmetic is used, so the result is the same on every machine.
[[nodiscard]] bool UndistortNormalized(const DistortionSpec& distortion, double xd, double yd,
    double tolerance_x, double tolerance_y, NormalizedPoint& undistorted) noexcept;

// Raw pixel position -> undistorted pixel position for one camera.
[[nodiscard]] bool UndistortPixel(const CameraSpec& camera, double u_raw, double v_raw, Vec2& undistorted) noexcept;

// Smallest t = r^2 > 0 where the derivative of r * radial(r) with respect to r,
// 1 + 3 k1 t + 5 k2 t^2 + 7 k3 t^3, reaches 0 (the radial map folds back
// there). Positive infinity when it never does.
[[nodiscard]] double RadialFoldSquared(const DistortionSpec& distortion) noexcept;

// Inverse of a 3x3 matrix in the row-major order; false when not invertible or not finite.
[[nodiscard]] bool InvertMatrix3(const std::array<double, 9>& matrix, std::array<double, 9>& inverse) noexcept;

} // namespace a0::m2::synthetic::detail
