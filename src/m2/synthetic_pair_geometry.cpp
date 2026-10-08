#include "synthetic_pair_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace a0::m2::synthetic::detail {

namespace {

constexpr int kMaximumNewtonIterations = 40;
constexpr double kSettledStep = 1e-14;
constexpr double kMinimumJacobianDeterminant = 1e-12;
constexpr int kBisectionIterations = 200;

} // namespace

NormalizedPoint DistortNormalized(const DistortionSpec& d, const double x, const double y) noexcept {
    const double r2 = x * x + y * y;
    const double radial = 1.0 + r2 * (d.k1 + r2 * (d.k2 + r2 * d.k3));
    return {
        x * radial + 2.0 * d.p1 * x * y + d.p2 * (r2 + 2.0 * x * x),
        y * radial + d.p1 * (r2 + 2.0 * y * y) + 2.0 * d.p2 * x * y,
    };
}

bool UndistortNormalized(const DistortionSpec& d, const double xd, const double yd, const double tolerance_x,
    const double tolerance_y, NormalizedPoint& undistorted) noexcept {
    double x = xd;
    double y = yd;
    for (int iteration = 0; iteration < kMaximumNewtonIterations; ++iteration) {
        const double r2 = x * x + y * y;
        const double radial = 1.0 + r2 * (d.k1 + r2 * (d.k2 + r2 * d.k3));
        const double radial_slope = d.k1 + r2 * (2.0 * d.k2 + r2 * 3.0 * d.k3);
        const double error_x = x * radial + 2.0 * d.p1 * x * y + d.p2 * (r2 + 2.0 * x * x) - xd;
        const double error_y = y * radial + d.p1 * (r2 + 2.0 * y * y) + 2.0 * d.p2 * x * y - yd;
        // The Jacobian of the distortion is symmetric: [[a, b], [b, c]].
        const double a = radial + 2.0 * x * x * radial_slope + 2.0 * d.p1 * y + 6.0 * d.p2 * x;
        const double b = 2.0 * x * y * radial_slope + 2.0 * d.p1 * x + 2.0 * d.p2 * y;
        const double c = radial + 2.0 * y * y * radial_slope + 6.0 * d.p1 * y + 2.0 * d.p2 * x;
        const double determinant = a * c - b * b;
        if (!(determinant > kMinimumJacobianDeterminant)) return false;
        const double step_x = (c * error_x - b * error_y) / determinant;
        const double step_y = (a * error_y - b * error_x) / determinant;
        x -= step_x;
        y -= step_y;
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        if (std::abs(step_x) + std::abs(step_y) <= kSettledStep) break;
    }
    const NormalizedPoint again = DistortNormalized(d, x, y);
    if (!(std::abs(again.x - xd) <= tolerance_x) || !(std::abs(again.y - yd) <= tolerance_y)) return false;
    undistorted = {x, y};
    return true;
}

bool UndistortPixel(const CameraSpec& camera, const double u_raw, const double v_raw, Vec2& undistorted) noexcept {
    const auto& k = camera.intrinsics;
    NormalizedPoint normalized;
    if (!UndistortNormalized(camera.distortion, (u_raw - k.cx_pixels) / k.fx_pixels,
            (v_raw - k.cy_pixels) / k.fy_pixels, kInversionResidualPixels / k.fx_pixels,
            kInversionResidualPixels / k.fy_pixels, normalized)) {
        return false;
    }
    undistorted = {k.fx_pixels * normalized.x + k.cx_pixels, k.fy_pixels * normalized.y + k.cy_pixels};
    return true;
}

double RadialFoldSquared(const DistortionSpec& d) noexcept {
    const double infinity = std::numeric_limits<double>::infinity();
    const std::array<double, 3> c{3.0 * d.k1, 5.0 * d.k2, 7.0 * d.k3};
    const auto g = [&c](const double t) { return 1.0 + t * (c[0] + t * (c[1] + t * c[2])); };

    int degree = 0;
    for (int index = 0; index < 3; ++index) {
        if (c[static_cast<std::size_t>(index)] != 0.0) degree = index + 1;
    }
    if (degree == 0) return infinity;

    // Every root has |t| below the Cauchy bound.
    const double top = std::abs(c[static_cast<std::size_t>(degree - 1)]);
    double largest = 1.0;
    for (int index = 0; index < degree - 1; ++index) largest = std::max(largest, std::abs(c[static_cast<std::size_t>(index)]));
    const double bound = 1.0 + largest / top;

    // g is monotonic between its critical points, so the first sign change is
    // found by bisection inside one interval.
    std::array<double, 4> breakpoints{};
    std::size_t count = 0;
    breakpoints[count++] = 0.0;
    std::array<double, 2> critical{};
    std::size_t critical_count = 0;
    if (degree == 2) {
        critical[critical_count++] = -c[0] / (2.0 * c[1]);
    } else if (degree == 3) {
        const double discriminant = 4.0 * c[1] * c[1] - 12.0 * c[2] * c[0];
        if (discriminant >= 0.0) {
            const double root = std::sqrt(discriminant);
            critical[critical_count++] = (-2.0 * c[1] - root) / (6.0 * c[2]);
            critical[critical_count++] = (-2.0 * c[1] + root) / (6.0 * c[2]);
            if (critical[0] > critical[1]) std::swap(critical[0], critical[1]);
        }
    }
    for (std::size_t index = 0; index < critical_count; ++index) {
        if (critical[index] > 0.0 && critical[index] < bound) breakpoints[count++] = critical[index];
    }
    breakpoints[count++] = bound;

    for (std::size_t index = 0; index + 1 < count; ++index) {
        double low = breakpoints[index];
        double high = breakpoints[index + 1];
        if (!(g(low) > 0.0) || g(high) > 0.0) continue;
        for (int step = 0; step < kBisectionIterations; ++step) {
            const double middle = 0.5 * (low + high);
            if (g(middle) > 0.0) low = middle;
            else high = middle;
        }
        return 0.5 * (low + high);
    }
    return infinity;
}

bool InvertMatrix3(const std::array<double, 9>& h, std::array<double, 9>& inverse) noexcept {
    const double c00 = h[4] * h[8] - h[5] * h[7];
    const double c01 = h[5] * h[6] - h[3] * h[8];
    const double c02 = h[3] * h[7] - h[4] * h[6];
    const double determinant = h[0] * c00 + h[1] * c01 + h[2] * c02;
    if (!std::isfinite(determinant) || determinant == 0.0) return false;
    const double inv = 1.0 / determinant;
    inverse = {
        c00 * inv, (h[2] * h[7] - h[1] * h[8]) * inv, (h[1] * h[5] - h[2] * h[4]) * inv,
        c01 * inv, (h[0] * h[8] - h[2] * h[6]) * inv, (h[2] * h[3] - h[0] * h[5]) * inv,
        c02 * inv, (h[1] * h[6] - h[0] * h[7]) * inv, (h[0] * h[4] - h[1] * h[3]) * inv,
    };
    for (const double value : inverse) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

} // namespace a0::m2::synthetic::detail
