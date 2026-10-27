#pragma once

// Shared private zero-mean normalized correlation for patch matching (#318)
// and the uncalibrated overlap estimator (#364). One arithmetic, one place.
#include <cmath>
#include <cstddef>
#include <vector>

namespace a0::m2::correlation_detail {
// Subtracts the mean in place and returns the centered vector's norm.
// Raw pointers keep large windows affordable in unoptimized builds; the
// summation order is the plain index order either way.
inline double Center(std::vector<double>& values) {
    double* const v = values.data(); const std::size_t n = values.size();
    double sum = 0; for (std::size_t i = 0; i < n; ++i) sum += v[i];
    const double mean = sum/static_cast<double>(n);
    double square_sum = 0;
    for (std::size_t i = 0; i < n; ++i) { v[i] -= mean; square_sum += v[i]*v[i]; }
    return std::sqrt(square_sum);
}
// Both inputs centered by Center, same length; norms are its return values.
inline double NormalizedDot(const std::vector<double>& a, double norm_a, const std::vector<double>& b, double norm_b) {
    const double* const pa = a.data(); const double* const pb = b.data(); const std::size_t n = a.size();
    double dot = 0; for (std::size_t i = 0; i < n; ++i) dot += pa[i]*pb[i];
    return (dot/norm_a)/norm_b;
}
} // namespace a0::m2::correlation_detail
