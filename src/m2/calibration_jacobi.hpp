#pragma once

// One-sided Jacobi singular value decomposition shared by the calibration
// modules (view geometry, closed-form intrinsics).
//
// The matrix is given as rows of a fixed column count. Columns are rotated
// until they are mutually orthogonal; the column norms are then the singular
// values and the accumulated rotations are the right singular vectors. The
// correlation of two columns is computed from unit columns with compensated
// summation, so the condition number is never squared.
//
// Header-only and internal to src/m2; it has no dependency beyond the STL.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

namespace a0::m2::calibration::jacobi_detail {

inline constexpr std::uint32_t kMaximumSweeps = 128;
inline constexpr double kEpsilon = std::numeric_limits<double>::epsilon();
inline constexpr double kCorrelationTolerance = 8 * kEpsilon;
// A singular value at or below this fraction of the largest is a roundoff-sized
// null column. The view geometry tail rule and the DLT / polar failure tests use it.
inline constexpr double kUnresolvedTailResolution = 16 * kEpsilon;

template <std::size_t Columns>
using Matrix = std::vector<std::array<double, Columns>>;

template <std::size_t Columns>
struct OneSidedJacobiResult {
    bool converged{};
    bool unresolved_tail{};
    std::array<double, Columns> values{};                       // descending singular values
    std::array<std::array<double, Columns>, Columns> vectors{}; // vectors[i]: right singular vector of values[i]
    std::uint32_t sweeps{};
};

template <std::size_t Columns>
double ColumnNorm(const Matrix<Columns>& matrix, std::size_t column) {
    double norm = 0;
    for (const auto& row : matrix) norm = std::hypot(norm, row[column]);
    return norm;
}
template <std::size_t Columns>
double Correlation(const Matrix<Columns>& matrix, std::size_t p, std::size_t q, double p_norm, double q_norm) {
    // Compensated summation of unit-column products avoids squaring the
    // condition number and is safe even for a very small nonzero column.
    double sum = 0, correction = 0;
    for (const auto& row : matrix) {
        const auto term = (row[p] / p_norm) * (row[q] / q_norm);
        const auto adjusted = term - correction;
        const auto next = sum + adjusted;
        correction = (next - sum) - adjusted;
        sum = next;
    }
    return sum;
}
template <std::size_t Columns>
std::array<double, Columns> Norms(const Matrix<Columns>& matrix) {
    std::array<double, Columns> norms{};
    for (std::size_t column = 0; column < norms.size(); ++column) norms[column] = ColumnNorm(matrix, column);
    return norms;
}
template <std::size_t Columns>
bool Orthogonal(const Matrix<Columns>& matrix, bool& unresolved_tail) {
    const auto norms = Norms(matrix);
    const auto largest = *std::max_element(norms.begin(), norms.end());
    if (!std::isfinite(largest) || !(largest > 0)) return false;
    for (std::size_t p = 0; p + 1 < Columns; ++p) for (std::size_t q = p + 1; q < Columns; ++q) {
        if (norms[p] == 0 || norms[q] == 0) continue;
        const auto correlation = Correlation(matrix, p, q, norms[p], norms[q]);
        if (!std::isfinite(correlation)) return false;
        if (std::abs(correlation) <= kCorrelationTolerance) continue;
        if (norms[p] <= kEpsilon * largest || norms[q] <= kEpsilon * largest) unresolved_tail = true;
        else return false;
    }
    return true;
}

template <std::size_t Columns>
OneSidedJacobiResult<Columns> OneSidedJacobi(Matrix<Columns> matrix) {
    OneSidedJacobiResult<Columns> result;
    // Column j of the accumulated rotation V is vectors[j]; V is never fed back
    // into the matrix, so the matrix arithmetic is the same with or without it.
    std::array<std::array<double, Columns>, Columns> rotation{};
    for (std::size_t i = 0; i < Columns; ++i) rotation[i][i] = 1;
    for (std::uint32_t sweep = 1; sweep <= kMaximumSweeps; ++sweep) {
        result.sweeps = sweep;
        for (std::size_t p = 0; p + 1 < Columns; ++p) for (std::size_t q = p + 1; q < Columns; ++q) {
            const auto norms = Norms(matrix);
            const auto largest = *std::max_element(norms.begin(), norms.end());
            const auto p_norm = norms[p], q_norm = norms[q];
            if (!std::isfinite(largest) || !(largest > 0)) return result;
            // The epsilon floor only controls termination of roundoff-sized
            // null columns. Their actual norms are retained in the spectrum;
            // no caller-policy cutoff is used until numerical work is done.
            if (p_norm <= kEpsilon * largest || q_norm <= kEpsilon * largest) continue;
            const auto correlation = Correlation(matrix, p, q, p_norm, q_norm);
            if (!std::isfinite(correlation)) return result;
            if (std::abs(correlation) <= kCorrelationTolerance) continue;
            const auto scale = std::max(p_norm, q_norm);
            const auto a = p_norm / scale, b = q_norm / scale;
            const auto delta = b * b - a * a;
            const auto cross = correlation * a * b;
            const auto denominator = delta + std::copysign(std::hypot(delta, 2 * cross), delta);
            if (cross == 0 || denominator == 0 || !std::isfinite(denominator)) return result;
            const auto tangent = (2 * cross) / denominator;
            const auto cosine = 1 / std::hypot(1.0, tangent), sine = tangent * cosine;
            if (!std::isfinite(cosine) || !std::isfinite(sine)) return result;
            for (auto& row : matrix) {
                const auto old_p = row[p], old_q = row[q];
                row[p] = std::fma(-sine, old_q, cosine * old_p);
                row[q] = std::fma(sine, old_p, cosine * old_q);
                if (!std::isfinite(row[p]) || !std::isfinite(row[q])) return result;
            }
            for (std::size_t k = 0; k < Columns; ++k) {
                const auto old_p = rotation[p][k], old_q = rotation[q][k];
                rotation[p][k] = std::fma(-sine, old_q, cosine * old_p);
                rotation[q][k] = std::fma(sine, old_p, cosine * old_q);
            }
        }
        bool unresolved_tail = false;
        if (Orthogonal(matrix, unresolved_tail)) {
            const auto norms = Norms(matrix);
            if (!std::all_of(norms.begin(), norms.end(), [](double value) { return std::isfinite(value) && value >= 0; })) return result;
            std::array<std::size_t, Columns> order{};
            std::iota(order.begin(), order.end(), std::size_t{0});
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return norms[a] > norms[b]; });
            for (std::size_t i = 0; i < Columns; ++i) {
                result.values[i] = norms[order[i]];
                result.vectors[i] = rotation[order[i]];
            }
            result.converged = true;
            result.unresolved_tail = unresolved_tail;
            return result;
        }
    }
    return result;
}

} // namespace a0::m2::calibration::jacobi_detail
