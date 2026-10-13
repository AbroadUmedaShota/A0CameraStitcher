#pragma once

// Internal header of the lens intrinsics stage (#290). Not installed; the
// product source and the contract test include it so that both call the same
// projection, Jacobian and physicality functions.

#include "a0/m2/calibration_lens.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

namespace a0::m2::calibration::lens_detail {

// Index of each parameter in a Jacobian row.
inline constexpr std::size_t kFx = 0, kFy = 1, kCx = 2, kCy = 3, kK1 = 4, kK2 = 5, kK3 = 6, kP1 = 7, kP2 = 8;
inline constexpr std::size_t kOmega = 9;       // three columns: rotation increment
inline constexpr std::size_t kTranslation = 12; // three columns
inline constexpr std::size_t kIntrinsicColumns = 9;
inline constexpr std::size_t kPoseColumns = 6;
inline constexpr std::size_t kJacobianColumns = kIntrinsicColumns + kPoseColumns;

struct NormalizedPoint {
    double x;
    double y;
};

// Same operations, in the same order, as the synthetic pair generator, so that
// the two agree bit for bit (checked by m2_calibration_lens_formula).
inline NormalizedPoint DistortNormalized(const BrownConradyDistortion& d, double x, double y) {
    const double r2 = x * x + y * y;
    const double radial = 1.0 + r2 * (d.k1 + r2 * (d.k2 + r2 * d.k3));
    return {
        x * radial + 2.0 * d.p1 * x * y + d.p2 * (r2 + 2.0 * x * x),
        y * radial + d.p1 * (r2 + 2.0 * y * y) + 2.0 * d.p2 * x * y,
    };
}

// Derivatives of the distortion map with respect to (x, y). The Jacobian is
// symmetric: [[a, b], [b, c]].
struct DistortionDerivatives {
    double radial;       // 1 + k1 r2 + k2 r2^2 + k3 r2^3
    double radial_slope; // d radial / d r2
    double a;
    double b;
    double c;
};
inline DistortionDerivatives DistortionDerivativesAt(const BrownConradyDistortion& d, double x, double y) {
    const double r2 = x * x + y * y;
    const double radial = 1.0 + r2 * (d.k1 + r2 * (d.k2 + r2 * d.k3));
    const double radial_slope = d.k1 + r2 * (2.0 * d.k2 + r2 * 3.0 * d.k3);
    return {
        radial,
        radial_slope,
        radial + 2.0 * x * x * radial_slope + 2.0 * d.p1 * y + 6.0 * d.p2 * x,
        2.0 * x * y * radial_slope + 2.0 * d.p1 * x + 2.0 * d.p2 * y,
        radial + 2.0 * y * y * radial_slope + 6.0 * d.p1 * y + 2.0 * d.p2 * x,
    };
}
// a c - b^2. Positive exactly where the distortion map is locally invertible
// with preserved orientation.
inline double DistortionJacobianDeterminant(const BrownConradyDistortion& d, double x, double y) {
    const auto derivative = DistortionDerivativesAt(d, x, y);
    return derivative.a * derivative.c - derivative.b * derivative.b;
}

// Derivative of the radial part of the map r -> r * radial(r) with respect to
// r, written in s = r^2: 1 + 3 k1 s + 5 k2 s^2 + 7 k3 s^3.
inline double RadialSlope(double k1, double k2, double k3, double s) {
    return 1.0 + s * (3.0 * k1 + s * (5.0 * k2 + s * (7.0 * k3)));
}

// Minimum of RadialSlope over [0, s_max], in closed form: the ends and the
// roots of the derivative (a quadratic) that lie strictly inside the interval.
inline double MinimumRadialSlope(double k1, double k2, double k3, double s_max) {
    double minimum = 1.0; // value at s = 0
    bool not_a_number = false;
    const auto consider = [&](double s) {
        const double value = RadialSlope(k1, k2, k3, s);
        if (std::isnan(value)) not_a_number = true;
        else if (value < minimum) minimum = value;
    };
    consider(s_max);
    // d/ds of RadialSlope = c + b s + a s^2
    const double a = 21.0 * k3, b = 10.0 * k2, c = 3.0 * k1;
    const auto inside = [&](double s) { return s > 0 && s < s_max; };
    if (a != 0) {
        const double discriminant = b * b - 4.0 * a * c;
        if (discriminant >= 0) {
            const double q = -0.5 * (b + std::copysign(std::sqrt(discriminant), b));
            if (inside(q / a)) consider(q / a);
            if (q != 0 && inside(c / q)) consider(c / q);
        }
    } else if (b != 0) {
        const double root = -c / b;
        if (inside(root)) consider(root);
    }
    return not_a_number ? std::numeric_limits<double>::quiet_NaN() : minimum;
}

// The forward-only physicality rule for the distortion: the radial map and the
// full distortion map are both strictly increasing in orientation over the
// observed range. Zero is not physical, and neither is a NaN.
inline bool PhysicalDistortion(double minimum_radial_slope, double minimum_jacobian_determinant) {
    return minimum_radial_slope > 0 && minimum_jacobian_determinant > 0;
}

inline bool PhysicalIntrinsics(const PinholeIntrinsics& k) {
    return std::isfinite(k.fx) && std::isfinite(k.fy) && std::isfinite(k.cx) && std::isfinite(k.cy)
        && k.fx > 0 && k.fy > 0;
}

// Exp(omega) for a rotation vector. theta = |omega|; the small-angle case needs
// no threshold: sin(theta)/theta and 2 (sin(theta/2)/theta)^2 are well behaved.
inline std::array<double, 9> RotationIncrement(const std::array<double, 3>& omega) {
    const double theta = std::hypot(omega[0], omega[1], omega[2]);
    if (theta == 0) return {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double a = std::sin(theta) / theta;
    const double half = std::sin(0.5 * theta) / theta;
    const double b = 2.0 * half * half;
    const std::array<double, 9> cross{0, -omega[2], omega[1], omega[2], 0, -omega[0], -omega[1], omega[0], 0};
    std::array<double, 9> square{};
    for (std::size_t i = 0; i < 3; ++i) for (std::size_t j = 0; j < 3; ++j) {
        double sum = 0;
        for (std::size_t k = 0; k < 3; ++k) sum += cross[i * 3 + k] * cross[k * 3 + j];
        square[i * 3 + j] = sum;
    }
    std::array<double, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (std::size_t i = 0; i < 9; ++i) rotation[i] += a * cross[i] + b * square[i];
    return rotation;
}

// One chart point through pose, perspective division, distortion and K.
struct PointProjection {
    double u = 0;
    double v = 0;
    double depth = 0;          // camera z of the point
    double radius_squared = 0; // r2 of the undistorted normalized point
    double determinant = 0;    // of the distortion map at that point
    // d(u, v) / d(fx fy cx cy k1 k2 k3 p1 p2 | omega(3) | t(3)); filled only when requested.
    std::array<std::array<double, kJacobianColumns>, 2> jacobian{};
};

// Null when the depth is not strictly positive or any output is not finite.
inline std::optional<PointProjection> ProjectChartPoint(const PinholeIntrinsics& k, const BrownConradyDistortion& d,
    const ChartPose& pose, double x_mm, double y_mm, bool with_jacobian = true) {
    const auto& r = pose.rotation;
    const auto& t = pose.translation_mm;
    const double q0 = r[0] * x_mm + r[1] * y_mm;
    const double q1 = r[3] * x_mm + r[4] * y_mm;
    const double q2 = r[6] * x_mm + r[7] * y_mm;
    const double cam_x = q0 + t[0], cam_y = q1 + t[1], depth = q2 + t[2];
    if (!std::isfinite(depth) || !(depth > 0)) return std::nullopt;
    const double x = cam_x / depth, y = cam_y / depth;
    const auto derivative = DistortionDerivativesAt(d, x, y);
    const auto distorted = DistortNormalized(d, x, y);
    PointProjection out;
    out.u = k.fx * distorted.x + k.cx;
    out.v = k.fy * distorted.y + k.cy;
    out.depth = depth;
    out.radius_squared = x * x + y * y;
    out.determinant = derivative.a * derivative.c - derivative.b * derivative.b;
    if (!std::isfinite(out.u) || !std::isfinite(out.v) || !std::isfinite(out.radius_squared)
        || !std::isfinite(out.determinant)) return std::nullopt;
    if (!with_jacobian) return out;

    const double r2 = out.radius_squared;
    const double inverse_depth = 1.0 / depth;
    // M = G * Pi, G = [[fx a, fx b], [fy b, fy c]], Pi = (1/z) [[1, 0, -x], [0, 1, -y]]
    const double g00 = k.fx * derivative.a, g01 = k.fx * derivative.b;
    const double g10 = k.fy * derivative.b, g11 = k.fy * derivative.c;
    const std::array<std::array<double, 3>, 2> m{{
        {g00 * inverse_depth, g01 * inverse_depth, -(g00 * x + g01 * y) * inverse_depth},
        {g10 * inverse_depth, g11 * inverse_depth, -(g10 * x + g11 * y) * inverse_depth}}};
    // -[q]x, with q = R [X, Y, 0]^T (translation excluded)
    const std::array<std::array<double, 3>, 3> minus_cross{{{0, q2, -q1}, {-q2, 0, q0}, {q1, -q0, 0}}};
    const double r4 = r2 * r2, r6 = r4 * r2;
    auto& ju = out.jacobian[0];
    auto& jv = out.jacobian[1];
    ju[kFx] = distorted.x; ju[kFy] = 0; ju[kCx] = 1; ju[kCy] = 0;
    jv[kFx] = 0; jv[kFy] = distorted.y; jv[kCx] = 0; jv[kCy] = 1;
    ju[kK1] = k.fx * x * r2; ju[kK2] = k.fx * x * r4; ju[kK3] = k.fx * x * r6;
    jv[kK1] = k.fy * y * r2; jv[kK2] = k.fy * y * r4; jv[kK3] = k.fy * y * r6;
    ju[kP1] = k.fx * 2.0 * x * y; ju[kP2] = k.fx * (r2 + 2.0 * x * x);
    jv[kP1] = k.fy * (r2 + 2.0 * y * y); jv[kP2] = k.fy * 2.0 * x * y;
    for (std::size_t j = 0; j < 3; ++j) {
        for (std::size_t row = 0; row < 2; ++row) {
            double sum = 0;
            for (std::size_t l = 0; l < 3; ++l) sum += m[row][l] * minus_cross[l][j];
            out.jacobian[row][kOmega + j] = sum;
            out.jacobian[row][kTranslation + j] = m[row][j];
        }
    }
    for (const auto& row : out.jacobian) for (const auto value : row) if (!std::isfinite(value)) return std::nullopt;
    return out;
}

} // namespace a0::m2::calibration::lens_detail
