#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace a0::m2::calibration {

// These are observed/fitted document-mm -> undistorted stored-order pixel
// homographies for ONE camera. Truth projections must never replace observed
// ones when this preflight is connected to a calibration tool.
struct ViewHomography {
    std::string id;
    std::array<double, 9> document_to_image; // Row major, any nonzero scale.
};

struct ViewGeometryOptions {
    std::uint32_t image_width, image_height; // Required dimensions, 1..32768.
    std::uint32_t minimum_views; // Required caller policy, 2..128.
    double relative_singular_cutoff; // Required caller policy, strictly (0,1).
};

enum class ViewGeometryStatus {
    LinearConstraintsSufficient,
    InsufficientViews,
    InsufficientDiversity,
    InconsistentLinearConstraints,
    NumericalFailure
};

constexpr std::string_view StatusCode(ViewGeometryStatus status) {
    switch (status) {
    case ViewGeometryStatus::LinearConstraintsSufficient: return "linear-constraints-sufficient";
    case ViewGeometryStatus::InsufficientViews: return "insufficient-views";
    case ViewGeometryStatus::InsufficientDiversity: return "insufficient-view-diversity";
    case ViewGeometryStatus::InconsistentLinearConstraints: return "inconsistent-linear-constraints";
    case ViewGeometryStatus::NumericalFailure: return "numerical-failure";
    }
    return "numerical-failure";
}

struct ViewGeometryAssessment {
    ViewGeometryStatus status;
    std::uint32_t view_count;
    // Spectrum/rank remain available for insufficient views (zero views means
    // only the known zero-skew prior, rank 1). Both null on NumericalFailure.
    std::optional<std::array<double, 6>> singular_values; // Descending; normalized rows.
    std::optional<std::uint32_t> numerical_rank;
    std::uint32_t svd_sweeps;
};

// Zhang's two linear constraints per plane + explicit zero-skew constraint.
// Condition image coordinates at ((w-1)/2,(h-1)/2), with isotropic scale
// max(w,h)/2; normalize each constraint row. Count singular values STRICTLY
// greater than cutoff * largest. Only rank 5 and enough views is sufficient.
// Rank 6 means mutually inconsistent constraints at the supplied tolerance.
// This is not a lens fit, positive-definite intrinsic check, calibration
// accuracy/quality gate or permission to issue a profile. No profile is made.
// Invalid/nonfinite/singular matrices, duplicate/unsafe IDs, invalid options
// or >128 views throw invalid_argument. Unconverged numerical work is reported.
ViewGeometryAssessment AssessViewGeometry(std::span<const ViewHomography> views,
    const ViewGeometryOptions& options);

} // namespace a0::m2::calibration
