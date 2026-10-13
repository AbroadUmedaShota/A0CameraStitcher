#include "a0/m2/calibration_view_geometry.hpp"

#include "calibration_exact_predicates.hpp"
#include "calibration_jacobi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace a0::m2::calibration {
namespace {
using Homography = std::array<double, 9>;
using exact_detail::ExactNonzeroDeterminant;
using Column = std::array<double, 3>;
using Row = std::array<double, 6>;
using Constraints = jacobi_detail::Matrix<6>;
constexpr std::uint32_t maximum_views = 128;

[[noreturn]] void Invalid() {
    throw std::invalid_argument("invalid calibration view geometry input");
}
bool SafeId(const std::string& id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
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

// Image frame of the constraint rows. The same values are reported with the
// null vector, so the caller can map conditioned coordinates back to pixels.
struct ImageFrame { double center_x, center_y, scale; };
ImageFrame FrameOf(const ViewGeometryOptions& options) {
    return {(static_cast<double>(options.image_width) - 1) * .5,
        (static_cast<double>(options.image_height) - 1) * .5,
        static_cast<double>(std::max(options.image_width, options.image_height)) * .5};
}

std::array<Column, 2> ConditionColumns(const Homography& matrix, const ViewGeometryOptions& options) {
    std::array<Column, 2> columns{{{matrix[0], matrix[3], matrix[6]}, {matrix[1], matrix[4], matrix[7]}}};
    double scale = 0;
    for (const auto& column : columns) for (const auto value : column) scale = std::max(scale, std::abs(value));
    if (!(scale > 0)) Invalid();
    // Both columns share each scale. Independent unit normalization would
    // remove their relative magnitude and corrupt the v11-v22 equation.
    for (auto& column : columns) for (auto& value : column) value /= scale;
    const auto frame = FrameOf(options);
    const auto center_x = frame.center_x, center_y = frame.center_y, image_scale = frame.scale;
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
    const auto spectrum = jacobi_detail::OneSidedJacobi<6>(std::move(constraints));
    ViewGeometryAssessment assessment{ViewGeometryStatus::NumericalFailure, static_cast<std::uint32_t>(views.size()), std::nullopt, std::nullopt, spectrum.sweeps};
    if (!spectrum.converged || (spectrum.unresolved_tail
        && options.relative_singular_cutoff <= jacobi_detail::kUnresolvedTailResolution)) return assessment;
    const auto threshold = options.relative_singular_cutoff * spectrum.values[0];
    std::uint32_t rank = 0;
    for (const auto value : spectrum.values) if (value > threshold) ++rank;
    assessment.singular_values = spectrum.values; assessment.numerical_rank = rank;
    if (views.size() < options.minimum_views) assessment.status = ViewGeometryStatus::InsufficientViews;
    else if (rank == 5) assessment.status = ViewGeometryStatus::LinearConstraintsSufficient;
    else if (rank == 6) assessment.status = ViewGeometryStatus::InconsistentLinearConstraints;
    else assessment.status = ViewGeometryStatus::InsufficientDiversity;
    if (assessment.status == ViewGeometryStatus::LinearConstraintsSufficient) {
        // Rank 5 leaves exactly one column below the cutoff, so the right
        // singular vector of the smallest value is unique up to sign.
        const auto frame = FrameOf(options);
        assessment.conditioned_conic = ConditionedConic{spectrum.vectors[5], frame.center_x, frame.center_y, frame.scale};
    }
    return assessment;
}
} // namespace a0::m2::calibration
