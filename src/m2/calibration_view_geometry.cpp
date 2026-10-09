#include "a0/m2/calibration_view_geometry.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace a0::m2::calibration {
namespace {
using Homography = std::array<double, 9>;
using Column = std::array<double, 3>;
using Row = std::array<double, 6>;
using Constraints = std::vector<Row>;
constexpr std::uint32_t maximum_views = 128;
constexpr std::uint32_t maximum_sweeps = 128;
constexpr double epsilon = std::numeric_limits<double>::epsilon();
constexpr double correlation_tolerance = 8 * epsilon;
constexpr double unresolved_tail_resolution = 16 * epsilon;

[[noreturn]] void Invalid() {
    throw std::invalid_argument("invalid calibration view geometry input");
}
bool SafeId(const std::string& id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

struct Factor { std::uint64_t mantissa; int exponent; bool negative; };
Factor BinaryFactor(double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559
        && std::numeric_limits<double>::digits == 53 && std::numeric_limits<double>::max_exponent == 1024);
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const auto exponent = static_cast<int>((bits >> 52U) & 2047U);
    auto mantissa = bits & ((1ULL << 52U) - 1);
    if (exponent) mantissa |= 1ULL << 52U;
    return {mantissa, exponent ? exponent - 1023 - 52 : -1074, (bits >> 63U) != 0};
}
struct DeterminantTerm { std::array<std::uint32_t, 6> words{}; int exponent{}; bool negative{}, zero{}; };
DeterminantTerm Product(const Homography& matrix, const std::array<std::size_t, 3>& indices, bool negative) {
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
bool ExactNonzeroDeterminant(const Homography& matrix) {
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
        if (carry) Invalid(); // unreachable within the proved finite-input bound
    }
    return positive != negative;
}
void ValidateHomography(const Homography& original) {
    double scale = 0;
    for (const auto value : original) {
        if (!std::isfinite(value)) Invalid();
        scale = std::max(scale, std::abs(value));
    }
    if (!(scale > 0)) Invalid();
    for (const auto value : original) if (!std::isfinite(value / scale)) Invalid();
    // Global H normalization bounds the finite view. Exact original binary64
    // factors remain authoritative for invertibility: floating normalization
    // can itself lose a nonzero tiny entry or perturb an exact cancellation.
    if (!ExactNonzeroDeterminant(original)) Invalid();
}

std::array<Column, 2> ConditionColumns(const Homography& matrix, const ViewGeometryOptions& options) {
    std::array<Column, 2> columns{{{matrix[0], matrix[3], matrix[6]}, {matrix[1], matrix[4], matrix[7]}}};
    double scale = 0;
    for (const auto& column : columns) for (const auto value : column) scale = std::max(scale, std::abs(value));
    if (!(scale > 0)) Invalid();
    // Both columns share each scale. Independent unit normalization would
    // remove their relative magnitude and corrupt the v11-v22 equation.
    for (auto& column : columns) for (auto& value : column) value /= scale;
    const auto center_x = (static_cast<double>(options.image_width) - 1) * .5;
    const auto center_y = (static_cast<double>(options.image_height) - 1) * .5;
    const auto image_scale = static_cast<double>(std::max(options.image_width, options.image_height)) * .5;
    for (auto& column : columns) {
        column[0] = std::fma(-center_x, column[2], column[0]) / image_scale;
        column[1] = std::fma(-center_y, column[2], column[1]) / image_scale;
    }
    scale = 0;
    for (const auto& column : columns) for (const auto value : column) scale = std::max(scale, std::abs(value));
    if (!(scale > 0) || !std::isfinite(scale)) Invalid();
    for (auto& column : columns) for (auto& value : column) value /= scale;
    return columns;
}
Row ZhangProducts(const Column& a, const Column& b) {
    // b = [B11,B12,B22,B13,B23,B33], Zhang §3.1 equations (6)-(8).
    return {a[0] * b[0], a[0] * b[1] + a[1] * b[0], a[1] * b[1],
        a[2] * b[0] + a[0] * b[2], a[2] * b[1] + a[1] * b[2], a[2] * b[2]};
}
Row UnitRow(Row row) {
    double norm = 0;
    for (const auto value : row) {
        if (!std::isfinite(value)) Invalid();
        norm = std::hypot(norm, value);
    }
    if (!(norm > 0) || !std::isfinite(norm)) Invalid();
    for (auto& value : row) value /= norm;
    return row;
}

double ColumnNorm(const Constraints& matrix, std::size_t column) {
    double norm = 0;
    for (const auto& row : matrix) norm = std::hypot(norm, row[column]);
    return norm;
}
double Correlation(const Constraints& matrix, std::size_t p, std::size_t q, double p_norm, double q_norm) {
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
std::array<double, 6> Norms(const Constraints& matrix) {
    std::array<double, 6> norms{};
    for (std::size_t column = 0; column < norms.size(); ++column) norms[column] = ColumnNorm(matrix, column);
    return norms;
}
bool Orthogonal(const Constraints& matrix, bool& unresolved_tail) {
    const auto norms = Norms(matrix);
    const auto largest = *std::max_element(norms.begin(), norms.end());
    if (!std::isfinite(largest) || !(largest > 0)) return false;
    for (std::size_t p = 0; p < 5; ++p) for (std::size_t q = p + 1; q < 6; ++q) {
        if (norms[p] == 0 || norms[q] == 0) continue;
        const auto correlation = Correlation(matrix, p, q, norms[p], norms[q]);
        if (!std::isfinite(correlation)) return false;
        if (std::abs(correlation) <= correlation_tolerance) continue;
        if (norms[p] <= epsilon * largest || norms[q] <= epsilon * largest) unresolved_tail = true;
        else return false;
    }
    return true;
}
struct Spectrum {
    bool converged{};
    bool unresolved_tail{};
    std::array<double, 6> values{};
    std::uint32_t sweeps{};
};
Spectrum Jacobi(Constraints matrix) {
    Spectrum result;
    for (std::uint32_t sweep = 1; sweep <= maximum_sweeps; ++sweep) {
        result.sweeps = sweep;
        for (std::size_t p = 0; p < 5; ++p) for (std::size_t q = p + 1; q < 6; ++q) {
            const auto norms = Norms(matrix);
            const auto largest = *std::max_element(norms.begin(), norms.end());
            const auto p_norm = norms[p], q_norm = norms[q];
            if (!std::isfinite(largest) || !(largest > 0)) return result;
            // The epsilon floor only controls termination of roundoff-sized
            // null columns. Their actual norms are retained in the spectrum;
            // no caller-policy cutoff is used until numerical work is done.
            if (p_norm <= epsilon * largest || q_norm <= epsilon * largest) continue;
            const auto correlation = Correlation(matrix, p, q, p_norm, q_norm);
            if (!std::isfinite(correlation)) return result;
            if (std::abs(correlation) <= correlation_tolerance) continue;
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
        }
        bool unresolved_tail = false;
        if (Orthogonal(matrix, unresolved_tail)) {
            result.values = Norms(matrix);
            if (!std::all_of(result.values.begin(), result.values.end(), [](double value) { return std::isfinite(value) && value >= 0; })) return result;
            std::sort(result.values.begin(), result.values.end(), [](double a, double b) { return a > b; });
            result.converged = true;
            result.unresolved_tail = unresolved_tail;
            return result;
        }
    }
    return result;
}
} // namespace

ViewGeometryAssessment AssessViewGeometry(std::span<const ViewHomography> views, const ViewGeometryOptions& options) {
    if (!options.image_width || !options.image_height || options.image_width > 32768 || options.image_height > 32768
        || options.minimum_views < 2 || options.minimum_views > maximum_views || views.size() > maximum_views
        || !std::isfinite(options.relative_singular_cutoff) || options.relative_singular_cutoff <= 0 || options.relative_singular_cutoff >= 1) Invalid();
    std::set<std::string> identities;
    Constraints constraints;
    constraints.reserve(views.size() * 2 + 1);
    for (const auto& view : views) {
        if (!SafeId(view.id) || !identities.insert(view.id).second) Invalid();
        ValidateHomography(view.document_to_image);
        const auto columns = ConditionColumns(view.document_to_image, options);
        constraints.push_back(UnitRow(ZhangProducts(columns[0], columns[1])));
        auto equal_norm = ZhangProducts(columns[0], columns[0]);
        const auto second = ZhangProducts(columns[1], columns[1]);
        for (std::size_t i = 0; i < equal_norm.size(); ++i) equal_norm[i] -= second[i];
        constraints.push_back(UnitRow(equal_norm));
    }
    // Centering and isotropic image scaling preserve zero skew. This is the
    // explicit linear B12=0 constraint, not an intrinsic estimate or profile.
    constraints.push_back({0, 1, 0, 0, 0, 0});
    const auto spectrum = Jacobi(std::move(constraints));
    ViewGeometryAssessment assessment{ViewGeometryStatus::NumericalFailure, static_cast<std::uint32_t>(views.size()), std::nullopt, std::nullopt, spectrum.sweeps};
    if (!spectrum.converged || (spectrum.unresolved_tail
        && options.relative_singular_cutoff <= unresolved_tail_resolution)) return assessment;
    const auto threshold = options.relative_singular_cutoff * spectrum.values[0];
    std::uint32_t rank = 0;
    for (const auto value : spectrum.values) if (value > threshold) ++rank;
    assessment.singular_values = spectrum.values; assessment.numerical_rank = rank;
    if (views.size() < options.minimum_views) assessment.status = ViewGeometryStatus::InsufficientViews;
    else if (rank == 5) assessment.status = ViewGeometryStatus::LinearConstraintsSufficient;
    else if (rank == 6) assessment.status = ViewGeometryStatus::InconsistentLinearConstraints;
    else assessment.status = ViewGeometryStatus::InsufficientDiversity;
    return assessment;
}
} // namespace a0::m2::calibration
