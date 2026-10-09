#include "a0/m2/calibration_view_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Analytic physical planes, independently formed here. No renderer, generator,
// evaluator, product profile, or production Zhang-row/SVD helper is used.
namespace {
using namespace a0::m2::calibration;
using Views = std::vector<ViewHomography>;
int checks = 0, failures = 0;
void Check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
template<class F> void Reject(F action, const std::string& label) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, label);
}
struct Vector { long double x, y, z; };
struct Intrinsics { long double fx = 420, fy = 440, cx = 319.5L, cy = 239.5L; };
Vector Rotate(Vector point, long double x, long double y, long double z) {
    // Apply three independent axis rotations to a world basis vector. The
    // production input is only H; no production pose math is consulted.
    const Vector after_x{point.x, std::cos(x) * point.y - std::sin(x) * point.z,
        std::sin(x) * point.y + std::cos(x) * point.z};
    const Vector after_y{std::cos(y) * after_x.x + std::sin(y) * after_x.z,
        after_x.y, -std::sin(y) * after_x.x + std::cos(y) * after_x.z};
    return {std::cos(z) * after_y.x - std::sin(z) * after_y.y,
        std::sin(z) * after_y.x + std::cos(z) * after_y.y, after_y.z};
}
ViewHomography Plane(std::string id, long double x, long double y, long double z,
    Vector translation = {-80, -60, 500}, Intrinsics k = {}) {
    const std::array<Vector, 3> columns{Rotate({1, 0, 0}, x, y, z),
        Rotate({0, 1, 0}, x, y, z), translation};
    std::array<double, 9> h{};
    for (std::size_t column = 0; column < 3; ++column) {
        h[column] = static_cast<double>(k.fx * columns[column].x + k.cx * columns[column].z);
        h[3 + column] = static_cast<double>(k.fy * columns[column].y + k.cy * columns[column].z);
        h[6 + column] = static_cast<double>(columns[column].z);
    }
    return {std::move(id), h};
}
ViewGeometryOptions Policy(double cutoff = 1e-8, std::uint32_t minimum = 2) {
    return {640, 480, minimum, cutoff};
}
void Invariants(const ViewGeometryAssessment& result, std::size_t count) {
    Check(result.view_count == count, "reported count is the actual declared view count");
    Check(result.singular_values.has_value() == result.numerical_rank.has_value(), "spectrum and rank presence agree");
    if (result.status == ViewGeometryStatus::NumericalFailure) {
        Check(!result.singular_values && !result.numerical_rank, "numerical failure does not claim a spectrum or rank");
        return;
    }
    Check(result.singular_values && result.numerical_rank, "completed preflight retains diagnostic spectrum and rank");
    if (!result.singular_values || !result.numerical_rank) return;
    const auto& values = *result.singular_values;
    Check(values[0] > 0, "explicit zero-skew prior keeps largest singular value positive");
    long double squared = 0;
    for (std::size_t index = 0; index < values.size(); ++index) {
        Check(std::isfinite(values[index]) && values[index] >= 0, "spectrum is finite and nonnegative");
        if (index) Check(values[index - 1] >= values[index], "spectrum is descending");
        squared += static_cast<long double>(values[index]) * values[index];
    }
    // Independent Frobenius identity: two UNIT rows per view plus one UNIT
    // zero-skew row. This does not reconstruct either Zhang rows or an SVD.
    const long double row_count = 2 * static_cast<long double>(count) + 1;
    Check(std::abs(squared - row_count) <= row_count * 2e-11L, "squared spectrum sums to normalized constraint row count");
    Check(*result.numerical_rank <= 6, "rank cannot exceed six intrinsic coefficients");
}
ViewGeometryAssessment Assess(const Views& views, ViewGeometryOptions options = Policy()) {
    const auto before = views;
    const auto result = AssessViewGeometry(views, options);
    Invariants(result, views.size());
    bool unchanged = before.size() == views.size();
    for (std::size_t index = 0; index < views.size(); ++index)
        unchanged = unchanged && before[index].id == views[index].id
            && before[index].document_to_image == views[index].document_to_image;
    Check(unchanged, "preflight preserves every observed matrix and ID");
    return result;
}
void Status(const ViewGeometryAssessment& result, ViewGeometryStatus expected, const std::string& code) {
    Check(result.status == expected, "independent expected classification: " + code);
    Check(StatusCode(result.status) == code, "stable assessment status code: " + code);
}
void SameSpectrum(const ViewGeometryAssessment& a, const ViewGeometryAssessment& b, const std::string& label) {
    Check(a.status == b.status && a.numerical_rank == b.numerical_rank, label + " verdict and rank");
    Check(a.singular_values && b.singular_values, label + " spectra present");
    if (!a.singular_values || !b.singular_values) return;
    for (std::size_t index = 0; index < 6; ++index)
        Check(std::abs((*a.singular_values)[index] - (*b.singular_values)[index])
            <= 2e-11 * std::max(1.0, (*a.singular_values)[0]), label + " spectrum");
}
void CountsAndDiversity() {
    const auto empty = Assess({});
    Status(empty, ViewGeometryStatus::InsufficientViews, "insufficient-views");
    Check(empty.numerical_rank == 1U && empty.singular_values && (*empty.singular_values)[0] == 1,
        "empty input exposes only explicit zero-skew prior, not physical calibration");
    if (empty.singular_values) for (std::size_t index = 1; index < 6; ++index)
        Check((*empty.singular_values)[index] == 0, "empty input has no observed plane constraints");
    const auto single = Assess({Plane("only", 0.2L, -0.1L, 0.04L)});
    Status(single, ViewGeometryStatus::InsufficientViews, "insufficient-views");
    Check(single.numerical_rank && *single.numerical_rank <= 3, "one plane plus skew has at most three independent rows");
    const Views front{Plane("front-a", 0, 0, 0), Plane("front-b", 0, 0, 0, {40, -30, 350}),
        Plane("front-c", 0, 0, 0, {-200, 100, 900})};
    const auto flat = Assess(front);
    Status(flat, ViewGeometryStatus::InsufficientDiversity, "insufficient-view-diversity");
    Check(flat.numerical_rank == 2U, "translations and depth changes do not add plane-normal diversity");
    Views spinning;
    for (unsigned i = 0; i < 4; ++i) spinning.push_back(Plane("spin-" + std::to_string(i), 0, 0,
        static_cast<long double>(i) * 0.3L, {20 * static_cast<long double>(i), -40, 400 + 80 * static_cast<long double>(i)}));
    Status(Assess(spinning), ViewGeometryStatus::InsufficientDiversity, "insufficient-view-diversity");
    const auto first = Plane("duplicated-a", 0.16L, -0.21L, 0.03L);
    auto second = first, third = first; second.id = "duplicated-b"; third.id = "duplicated-c";
    const auto copies = Assess({first, second, third});
    Status(copies, ViewGeometryStatus::InsufficientDiversity, "insufficient-view-diversity");
    Check(copies.numerical_rank && *copies.numerical_rank <= 3, "distinct labels for duplicate pose do not inflate rank");
}
Views Tilts() {
    return {Plane("tilt-a", 0.18L, 0.08L, 0.07L), Plane("tilt-b", -0.12L, 0.23L, -0.09L),
        Plane("tilt-c", 0.27L, -0.19L, 0.13L)};
}
void PhysicalRankAndInvariance() {
    const auto views = Tilts();
    const auto two = Assess({views[0], views[1]});
    Status(two, ViewGeometryStatus::LinearConstraintsSufficient, "linear-constraints-sufficient");
    Check(two.numerical_rank == 5U, "two generic nonparallel tilted planes plus zero skew have the theoretical five constraints");
    const auto three = Assess(views);
    Status(three, ViewGeometryStatus::LinearConstraintsSufficient, "linear-constraints-sufficient");
    Check(three.numerical_rank == 5U, "common physical intrinsics retain one homogeneous null direction");
    auto reversed = views; std::reverse(reversed.begin(), reversed.end());
    SameSpectrum(three, Assess(reversed), "view ordering invariance");
    for (const double factor : {1e-150, -1e-150, 1e150, -1e150}) {
        auto scaled = views;
        for (auto& view : scaled) for (auto& entry : view.document_to_image) entry *= factor;
        SameSpectrum(three, Assess(scaled), "arbitrary signed nonzero homography scale invariance");
    }
    for (const double factor : {0.001, 1000.0}) {
        auto units = views;
        for (auto& view : units) for (std::size_t row = 0; row < 3; ++row)
            for (std::size_t column = 0; column < 2; ++column) view.document_to_image[row * 3 + column] *= factor;
        SameSpectrum(three, Assess(units), "isotropic document unit invariance");
    }
    const auto caller_minimum = Assess(views, Policy(1e-8, 4));
    Status(caller_minimum, ViewGeometryStatus::InsufficientViews, "insufficient-views");
    Check(caller_minimum.numerical_rank == 5U, "explicit caller view-count policy does not erase available rank");
    const Views near{Plane("near-a", 0.004L, 0.003L, 0.013L), Plane("near-b", -0.003L, 0.006L, -0.02L),
        Plane("near-c", 0.006L, -0.004L, 0.017L)};
    const auto permissive = Assess(near, Policy(1e-10));
    const auto demanding = Assess(near, Policy(0.005));
    Status(permissive, ViewGeometryStatus::LinearConstraintsSufficient, "linear-constraints-sufficient");
    Status(demanding, ViewGeometryStatus::InsufficientDiversity, "insufficient-view-diversity");
    Check(permissive.numerical_rank && demanding.numerical_rank && *permissive.numerical_rank > *demanding.numerical_rank,
        "near-parallel verdict responds to external relative cutoff instead of a hidden threshold");
    const auto repeated = Assess(near, Policy(1e-10));
    SameSpectrum(permissive, repeated, "repeat deterministic assessment");
    Check(permissive.singular_values == repeated.singular_values && permissive.svd_sweeps == repeated.svd_sweeps,
        "identical inputs and policy repeat exact spectrum and sweep record");
    const Views incompatible{Plane("k-a", 0.18L, 0.08L, 0.07L), Plane("k-b", -0.12L, 0.23L, -0.09L),
        Plane("k-c", 0.27L, -0.19L, 0.13L), Plane("k-d", -0.32L, -0.14L, 0.18L, {-40, -20, 600}, {610, 360, 319.5L, 239.5L}),
        Plane("k-e", 0.11L, 0.35L, -0.2L, {-70, 10, 450}, {330, 720, 319.5L, 239.5L})};
    const auto inconsistent = Assess(incompatible);
    Status(inconsistent, ViewGeometryStatus::InconsistentLinearConstraints, "inconsistent-linear-constraints");
    Check(inconsistent.numerical_rank == 6U, "incompatible physical K values destroy common homogeneous intrinsic nullspace");
}
void StrictBoundary() {
    const Views flat{Plane("boundary-a", 0, 0, 0), Plane("boundary-b", 0, 0, 0), Plane("boundary-c", 0, 0, 0)};
    const auto baseline = Assess(flat);
    if (!baseline.singular_values) { Check(false, "boundary fixture diagnostic spectrum exists"); return; }
    const auto& values = *baseline.singular_values;
    // Three identical front planes give orthogonal row families with norms
    // sqrt(4)=2 and sqrt(3). The strict boundary uses the API's own reported
    // ratio, not an independently copied eigensolver or Zhang-row formula.
    Check(std::abs(values[0] - 2) < 1e-12 && std::abs(values[1] - std::sqrt(3.0)) < 1e-12,
        "analytic duplicate-flat spectrum is sqrt(4), sqrt(3), four zeros");
    const double cutoff = values[1] / values[0];
    const auto at = Assess(flat, Policy(cutoff));
    const auto below = Assess(flat, Policy(std::nextafter(std::nextafter(cutoff, 0.0), 0.0)));
    const auto above = Assess(flat, Policy(std::nextafter(std::nextafter(cutoff, 1.0), 1.0)));
    Check(below.numerical_rank == 2U && above.numerical_rank == 1U, "nearby represented external cutoffs straddle the positive singular boundary");
    if (cutoff * values[0] == values[1]) Check(at.numerical_rank == 1U, "exact represented equality is excluded by STRICT greater-than cutoff");
    else std::cout << "strict_boundary_exact_product=false; equality assertion omitted for binary64 ratio rounding\n";
}
void NumericalRegressions() {
    const auto views = Tilts();
    const auto tiny = Assess({views[0], views[1]}, Policy(1e-30));
    Status(tiny, ViewGeometryStatus::NumericalFailure, "numerical-failure");
    Check(!tiny.numerical_rank && !tiny.singular_values,
        "five-row problem cannot claim rank six from an uncertified roundoff tail");
    const auto duplicate = Plane("repeat-a", 0.18L, 0.08L, 0.07L);
    auto second = duplicate, third = duplicate; second.id = "repeat-b"; third.id = "repeat-c";
    const auto repeated = Assess({duplicate, second, third}, Policy(1e-30));
    Status(repeated, ViewGeometryStatus::NumericalFailure, "numerical-failure");
    auto singular = views; singular[0].document_to_image = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    Reject([&]{ (void)AssessViewGeometry(singular, Policy()); }, "exact integer singular determinant is rejected despite pivot roundoff");
    for (const int power : {-500, 500}) {
        auto scaled_singular = singular;
        for (auto& value : scaled_singular[0].document_to_image) value = std::ldexp(value, power);
        Reject([&]{ (void)AssessViewGeometry(scaled_singular, Policy()); }, "exact zero determinant survives binary power scale without underflow/overflow");
    }
    auto valid_neighbor = views; valid_neighbor[0].document_to_image = {1, 2, 3, 4, 5, 6, 7, 8, 10};
    bool accepted = true; try { (void)AssessViewGeometry(valid_neighbor, Policy()); } catch (...) { accepted = false; }
    Check(accepted, "adjacent nonsingular integer matrix is not rejected by a blanket singular-input filter");
}
void InvalidContracts() {
    const auto good = Tilts();
    for (const auto value : {0.0, 1.0, -0.1, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        auto options = Policy(); options.relative_singular_cutoff = value;
        Reject([&]{ (void)AssessViewGeometry({}, options); }, "invalid cutoff rejected even without views");
    }
    for (const std::uint32_t count : {0U, 1U, 129U}) {
        auto options = Policy(); options.minimum_views = count;
        Reject([&]{ (void)AssessViewGeometry(good, options); }, "caller minimum-view policy has strict 2..128 bounds");
    }
    for (unsigned axis = 0; axis < 2; ++axis) for (const auto dimension : {0U, 32769U}) {
        auto options = Policy(); if (axis) options.image_height = dimension; else options.image_width = dimension;
        Reject([&]{ (void)AssessViewGeometry(good, options); }, "conditioning image dimension is within explicit 1..32768 resource range");
    }
    auto longest_id = good; longest_id[0].id = std::string(128, 'a');
    Status(Assess(longest_id), ViewGeometryStatus::LinearConstraintsSufficient, "linear-constraints-sufficient");
    auto oversized_id = good; oversized_id[0].id = std::string(129, 'a');
    Reject([&]{ (void)AssessViewGeometry(oversized_id, Policy()); }, "129-character identifier exceeds explicit resource boundary");
    for (const auto& id : std::vector<std::string>{"", "../unsafe", "bad/id", "bad:id", "line\nfeed", std::string("bad\0id", 6), std::string(1, '\xff')}) {
        auto invalid = good; invalid[0].id = id;
        Reject([&]{ (void)AssessViewGeometry(invalid, Policy()); }, "unsafe ID is refused");
    }
    auto duplicate = good; duplicate[1].id = duplicate[0].id;
    Reject([&]{ (void)AssessViewGeometry(duplicate, Policy()); }, "duplicate ID is refused rather than deduplicated");
    Views too_many;
    for (unsigned i = 0; i < 129; ++i) too_many.push_back(Plane("count-" + std::to_string(i), 0, 0, 0));
    Reject([&]{ (void)AssessViewGeometry(too_many, Policy()); }, "more than 128 declared views is outside resource contract");
    auto maximum_views = too_many; maximum_views.pop_back();
    const auto maximum = Assess(maximum_views, Policy(1e-8, 128));
    Status(maximum, ViewGeometryStatus::InsufficientDiversity, "insufficient-view-diversity");
    Check(maximum.numerical_rank == 2U, "128 views and caller minimum128 are accepted without inflating duplicate-plane information");
    for (const double invalid_entry : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        auto invalid = good; invalid[0].document_to_image[4] = invalid_entry;
        Reject([&]{ (void)AssessViewGeometry(invalid, Policy()); }, "nonfinite matrix coefficient refused");
        Reject([&]{ (void)AssessViewGeometry(std::span<const ViewHomography>(invalid.data(), 1), Policy()); }, "matrix is validated before insufficient-view classification");
    }
    for (unsigned shape = 0; shape < 3; ++shape) {
        auto invalid = good;
        if (shape == 0) invalid[0].document_to_image.fill(0);
        if (shape == 1) for (std::size_t row = 0; row < 3; ++row) invalid[0].document_to_image[row * 3 + 1] = invalid[0].document_to_image[row * 3];
        if (shape == 2) for (std::size_t column = 0; column < 3; ++column) invalid[0].document_to_image[6 + column] = 0;
        Reject([&]{ (void)AssessViewGeometry(invalid, Policy()); }, "zero or singular physical mapping refused");
    }
}
} // namespace

int main() {
    try { CountsAndDiversity(); PhysicalRankAndInvariance(); StrictBoundary(); NumericalRegressions(); InvalidContracts(); }
    catch (const std::exception& error) { ++failures; std::cerr << "Unexpected: " << error.what() << '\n'; }
    std::cout << "calibration_view_geometry checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
