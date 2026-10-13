#pragma once

// Exact determinant predicates shared by the calibration modules.
//
// Every input must be a finite binary64 value; callers validate finiteness
// before calling. No tolerance is used anywhere in this header.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace a0::m2::calibration::exact_detail {

using Matrix3x3 = std::array<double, 9>;

struct Factor { std::uint64_t mantissa; int exponent; bool negative; };
inline Factor BinaryFactor(double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559
        && std::numeric_limits<double>::digits == 53 && std::numeric_limits<double>::max_exponent == 1024);
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const auto exponent = static_cast<int>((bits >> 52U) & 2047U);
    auto mantissa = bits & ((1ULL << 52U) - 1);
    if (exponent) mantissa |= 1ULL << 52U;
    return {mantissa, exponent ? exponent - 1023 - 52 : -1074, (bits >> 63U) != 0};
}
struct DeterminantTerm { std::array<std::uint32_t, 6> words{}; int exponent{}; bool negative{}, zero{}; };
inline DeterminantTerm Product(const Matrix3x3& matrix, const std::array<std::size_t, 3>& indices, bool negative) {
    DeterminantTerm term; term.words[0] = 1; term.negative = negative; std::size_t used = 1;
    for (const auto index : indices) {
        const auto factor = BinaryFactor(matrix[index]);
        if (!factor.mantissa) { term.zero = true; return term; }
        term.exponent += factor.exponent; term.negative = term.negative != factor.negative;
        const std::array<std::uint32_t, 2> multiplier{static_cast<std::uint32_t>(factor.mantissa), static_cast<std::uint32_t>(factor.mantissa >> 32U)};
        const auto previous = term.words; term.words.fill(0);
        for (std::size_t i = 0; i < used; ++i) {
            std::uint64_t carry = 0;
            for (std::size_t j = 0; j < 2; ++j) {
                const auto sum = static_cast<std::uint64_t>(previous[i]) * multiplier[j] + term.words[i + j] + carry;
                term.words[i + j] = static_cast<std::uint32_t>(sum); carry = sum >> 32U;
            }
            term.words[i + 2] = static_cast<std::uint32_t>(carry);
        }
        used = std::min(used + 2, term.words.size());
        while (used > 1 && term.words[used - 1] == 0) --used;
    }
    return term;
}
inline bool ExactNonzeroDeterminant(const Matrix3x3& matrix) {
    const std::array<DeterminantTerm, 6> terms{
        Product(matrix, {0, 4, 8}, false), Product(matrix, {1, 5, 6}, false), Product(matrix, {2, 3, 7}, false),
        Product(matrix, {2, 4, 6}, true), Product(matrix, {1, 3, 8}, true), Product(matrix, {0, 5, 7}, true)};
    int minimum_exponent = std::numeric_limits<int>::max();
    for (const auto& term : terms) if (!term.zero) minimum_exponent = std::min(minimum_exponent, term.exponent);
    if (minimum_exponent == std::numeric_limits<int>::max()) return false;
    // A finite binary64 factor is an integer of at most 53 bits times 2^e,
    // -1074<=e<=971. Each triple therefore needs <=159 mantissa bits, and
    // aligning its exponent needs <=3*(971+1074)=6135 extra bits. Adding
    // six signed terms needs <=3 further bits: <6297 bits, within 200*32.
    // Exact signed accumulation avoids rounded-pivot false nonsingularity
    // and determinant product under/overflow, without a quality epsilon.
    using Integer = std::array<std::uint32_t, 200>;
    Integer positive{}, negative{};
    for (const auto& term : terms) {
        if (term.zero) continue;
        const auto shift = static_cast<unsigned>(term.exponent - minimum_exponent);
        const auto word_shift = static_cast<std::size_t>(shift / 32U); const auto bit_shift = shift % 32U;
        Integer aligned{};
        for (std::size_t i = 0; i < term.words.size(); ++i) {
            aligned[word_shift + i] |= term.words[i] << bit_shift;
            if (bit_shift) aligned[word_shift + i + 1] |= term.words[i] >> (32U - bit_shift);
        }
        auto& destination = term.negative ? negative : positive; std::uint64_t carry = 0;
        for (std::size_t i = 0; i < destination.size(); ++i) {
            const auto sum = static_cast<std::uint64_t>(destination[i]) + aligned[i] + carry;
            destination[i] = static_cast<std::uint32_t>(sum); carry = sum >> 32U;
        }
        if (carry) throw std::invalid_argument("exact determinant accumulator overflow"); // unreachable within the proved finite-input bound
    }
    return positive != negative;
}

// Exact orient2d zero test on finite binary64 inputs. No tolerance.
// det [[ax, ay, 1], [bx, by, 1], [cx, cy, 1]] equals orient2d(a, b, c).
inline bool ExactCollinear(double ax, double ay, double bx, double by, double cx, double cy) {
    return !ExactNonzeroDeterminant(Matrix3x3{ax, ay, 1.0, bx, by, 1.0, cx, cy, 1.0});
}

} // namespace a0::m2::calibration::exact_detail
