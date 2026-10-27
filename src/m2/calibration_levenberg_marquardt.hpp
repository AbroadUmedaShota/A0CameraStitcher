#pragma once

// Internal Levenberg-Marquardt loop shared by the lens stage (#290) and the
// planar calibration (#374). Marquardt's diagonal scaling, a dense Cholesky
// solve and the stopping rules of LensIntrinsicsPolicy. No randomness.
//
// A problem supplies, for its own parameter state S:
//   std::size_t Size() const;                                  // n
//   std::optional<double> Evaluate(const S&) const;            // sum of squared residuals, null if unusable
//   bool Linearize(const S&, std::vector<double>& a, std::vector<double>& g) const; // A = J^T J (n x n), g = J^T r
//   S Step(const S&, const std::vector<double>& delta) const;
//   double Norm(const S&) const;                                // norm of the parameters that carry a unit

#include "a0/m2/calibration_lens.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace a0::m2::calibration::lm_detail {

// In-place lower Cholesky factor of the symmetric matrix held in the lower
// triangle of l (row major). False if a pivot is not strictly positive and finite.
inline bool Factorize(std::vector<double>& l, std::size_t n) {
    double* base = l.data();
    for (std::size_t j = 0; j < n; ++j) {
        double* row_j = base + j * n;
        double pivot = row_j[j];
        for (std::size_t k = 0; k < j; ++k) pivot -= row_j[k] * row_j[k];
        if (!std::isfinite(pivot) || !(pivot > 0)) return false;
        const double diagonal = std::sqrt(pivot);
        row_j[j] = diagonal;
        for (std::size_t i = j + 1; i < n; ++i) {
            double* row_i = base + i * n;
            double sum = row_i[j];
            for (std::size_t k = 0; k < j; ++k) sum -= row_i[k] * row_j[k];
            row_i[j] = sum / diagonal;
        }
    }
    return true;
}

// Solves L L^T x = b in place (b becomes x).
inline void SolveFactored(const std::vector<double>& l, std::size_t n, std::vector<double>& b) {
    const double* base = l.data();
    for (std::size_t i = 0; i < n; ++i) {
        const double* row = base + i * n;
        double sum = b[i];
        for (std::size_t k = 0; k < i; ++k) sum -= row[k] * b[k];
        b[i] = sum / row[i];
    }
    for (std::size_t i = n; i-- > 0;) {
        double sum = b[i];
        for (std::size_t k = i + 1; k < n; ++k) sum -= base[k * n + i] * b[k];
        b[i] = sum / base[i * n + i];
    }
}

template <class State>
struct Outcome {
    State parameters;
    SolverRecord record;
};

// Null when the start is unusable. Policy needs max_iterations,
// relative_cost_tolerance, relative_step_tolerance and initial_damping.
template <class Problem, class State, class Policy>
std::optional<Outcome<State>> Run(const Problem& problem, State start, const Policy& policy) {
    const auto initial_cost = problem.Evaluate(start);
    if (!initial_cost) return std::nullopt;
    const std::size_t n = problem.Size();
    Outcome<State> outcome{std::move(start), SolverRecord{0, 0, *initial_cost, *initial_cost, SolverStopReason::MaximumIterations}};
    auto& current = outcome.parameters;
    auto& record = outcome.record;
    double cost = *initial_cost;
    double damping = policy.initial_damping;
    bool stopped = false;

    std::vector<double> a, g, factor(n * n), scale(n), step(n);
    if (!problem.Linearize(current, a, g)) {
        record.stop_reason = SolverStopReason::NumericalFailure;
        return outcome;
    }
    while (record.iterations < policy.max_iterations && !stopped) {
        ++record.iterations;
        bool scale_ok = true;
        for (std::size_t j = 0; j < n; ++j) {
            scale[j] = std::sqrt(a[j * n + j]);
            if (!std::isfinite(scale[j]) || !(scale[j] > 0)) scale_ok = false;
        }
        if (!scale_ok) {
            record.stop_reason = SolverStopReason::NumericalFailure;
            stopped = true;
            break;
        }
        // (A^ + damping I) s^ = -g^ with A^ = S^-1 A S^-1 (unit diagonal) and g^ = S^-1 g.
        for (std::size_t i = 0; i < n; ++i) {
            double* row = factor.data() + i * n;
            const double* source = a.data() + i * n;
            for (std::size_t j = 0; j < i; ++j) row[j] = source[j] / (scale[i] * scale[j]);
            row[i] = 1.0 + damping;
            step[i] = -g[i] / scale[i];
        }
        if (!Factorize(factor, n)) {
            damping *= 10.0;
            if (!std::isfinite(damping)) { record.stop_reason = SolverStopReason::NumericalFailure; stopped = true; }
            continue;
        }
        SolveFactored(factor, n, step);
        double step_squares = 0;
        for (std::size_t j = 0; j < n; ++j) {
            step[j] /= scale[j];
            step_squares += step[j] * step[j];
        }
        const double step_norm = std::sqrt(step_squares);
        if (!std::isfinite(step_norm)) {
            damping *= 10.0;
            if (!std::isfinite(damping)) { record.stop_reason = SolverStopReason::NumericalFailure; stopped = true; }
            continue;
        }
        const double tolerance = policy.relative_step_tolerance;
        if (step_norm <= tolerance * (problem.Norm(current) + tolerance)) {
            record.stop_reason = SolverStopReason::RelativeStepSize;
            stopped = true;
            break;
        }
        auto candidate = problem.Step(current, step);
        const auto candidate_cost = problem.Evaluate(candidate);
        if (candidate_cost && *candidate_cost < cost) {
            const double previous_cost = cost;
            current = std::move(candidate);
            cost = *candidate_cost;
            record.final_cost = cost;
            ++record.accepted_steps;
            damping = std::max(damping / 10.0, std::numeric_limits<double>::min());
            if (previous_cost - cost <= policy.relative_cost_tolerance * previous_cost) {
                record.stop_reason = SolverStopReason::RelativeCostReduction;
                stopped = true;
                break;
            }
            if (!problem.Linearize(current, a, g)) {
                record.stop_reason = SolverStopReason::NumericalFailure;
                stopped = true;
                break;
            }
        } else {
            damping *= 10.0;
            if (!std::isfinite(damping)) { record.stop_reason = SolverStopReason::NumericalFailure; stopped = true; }
        }
    }
    return outcome;
}

} // namespace a0::m2::calibration::lm_detail
