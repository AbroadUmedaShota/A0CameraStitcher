#pragma once

// Internal header of the grid sheet tilt stages (#440 stage 2). Not installed; the product source and the contract test
// include it so that both call the same stages. The public header is a0/m2/experimental/grid_sheet_tilt.hpp.

#include "a0/m2/experimental/grid_sheet_tilt.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace a0::m2::experimental::grid_sheet_tilt::detail {

constexpr double kDegrees = 3.14159265358979323846 / 180.0;
using Mat3 = std::array<double, 9>;   // row-major

// ---- (a) ridge points -----------------------------------------------------------------------------------------------
// A thin bright line's centre line, found per pixel: the pixel centre is at integer coordinates (the convention of the rig
// profile), (nx, ny) is the unit normal of the line.
struct RidgePoint {
    float x{}, y{}, nx{}, ny{};
};
struct RidgeResult {
    std::vector<RidgePoint> points;   // in band order, then row order: the same for any thread count
    double threshold{};               // the second-derivative threshold that was used
};
// Throws std::invalid_argument for an unusable picture or settings.
[[nodiscard]] RidgeResult DetectRidges(const ImageView& picture, const GridSheetDetectionSettings& settings);

// ---- lens -----------------------------------------------------------------------------------------------------------
struct Lens {
    double fx{}, fy{}, cx{}, cy{}, k1{}, k2{}, k3{}, p1{}, p2{};
    [[nodiscard]] double MeanFocal() const { return 0.5 * (fx + fy); }
};
[[nodiscard]] Lens LensOf(const render::CameraProjection& projection);
// Normalised distorted -> normalised undistorted, by Newton's method; false when it does not converge.
[[nodiscard]] bool UndistortNormalized(const Lens& lens, double xd, double yd, double& x, double& y, double* jacobian = nullptr);
// The forward model (the one of render::TryProjectDocument), normalised to normalised.
void DistortNormalized(const Lens& lens, double x, double y, double& xd, double& yd);

// ---- (c) points without distortion ----------------------------------------------------------------------------------
// Ridge points in the normalised undistorted plane with the unit tangent of their line there. Points that cannot be
// undistorted are dropped; `source` is the index of the ridge point.
struct PreparedPoints {
    std::vector<double> qx, qy, tx, ty;
    std::vector<std::uint32_t> source;
    [[nodiscard]] std::size_t size() const { return qx.size(); }
};
[[nodiscard]] PreparedPoints PreparePoints(const std::vector<RidgePoint>& ridges, const Lens& lens, bool use_distortion);

// ---- (b)(d) lattice -------------------------------------------------------------------------------------------------
constexpr double kLatticeScale = 60.0;   // the fit's lattice unit is this many pitches, so its parameters are of order one

// Parameters: G = [[p0 p1 p2],[p3 p4 p5],[p6 p7 1]] maps (qx, qy, 1) to the lattice in units of kLatticeScale pitches.
using LatticeParameters = std::array<double, 8>;

struct InitialLattice {
    LatticeParameters parameters{};
    std::array<double, 2> pitch_px{};
    double centre_x{}, centre_y{};      // the window the lattice came from, normalised units
    double score{};
};
// nullopt: no window shows two periodic families of lines.
[[nodiscard]] std::optional<InitialLattice> FindInitialLattice(const PreparedPoints& points, double focal, const GridSheetDetectionSettings& s);

// Distance (pixels, first order) of every point to the nearest integer line of the family its direction belongs to.
struct Assignment {
    std::vector<float> distance_px;
    std::vector<std::uint8_t> family;     // 0: lines x = const, 1: lines y = const
    std::vector<std::int32_t> index;      // the integer of that line
    std::vector<std::uint8_t> ok;         // direction within the match angle (and within the gate, when one is given)
};
// gate_px <= 0: no gate on the distance.
[[nodiscard]] Assignment AssignPoints(const LatticeParameters& p, const PreparedPoints& points, double focal, double direction_match_deg, double gate_px);

struct LatticeFit {
    LatticeParameters parameters{};
    bool ok{};
};
// The growing fit of the prototype: windows of increasing size around the first lattice, gates shrinking inside each.
[[nodiscard]] LatticeFit FitLattice(const PreparedPoints& points, double focal, const InitialLattice& start, const GridSheetDetectionSettings& s);

// One Gauss-Newton step of the lattice fit with its normal equations: for a test of the analytic Jacobian. `residual` and
// `jacobian` (row-major, 8 columns) of the points with an assignment given.
void LatticeResiduals(const LatticeParameters& p, const PreparedPoints& points, const std::vector<std::uint32_t>& indices,
    const std::vector<std::uint8_t>& family, const std::vector<std::int32_t>& index, double focal,
    std::vector<double>& residual, std::vector<double>* jacobian);

// Lattice (in pitches) to normalised undistorted plane.
[[nodiscard]] Mat3 LatticeToPlane(const LatticeParameters& p);
[[nodiscard]] LatticeParameters ParametersOfLatticeToPlane(const Mat3& h);   // inverse of the above

// ---- the three ways to a tilt, and the evidence ---------------------------------------------------------------------
struct OrientationNumbers {
    bool ok{};
    double tilt_deg{};
    render::Point lean_sheet_deg{};
    double image_x_azimuth_deg{};
};
// The decomposition of a lattice-to-plane matrix: content_registration::OrientationOf on the projection that has the
// profile's intrinsics and distortion and the lattice matrix as its document matrix.
[[nodiscard]] OrientationNumbers OrientationOfLattice(const render::CameraProjection& profile_projection, const Mat3& lattice_to_plane);
// Same orientation sign (the plane's own right-handedness) of the matrix's linear part at the principal point.
[[nodiscard]] bool PositivelyOriented(const Mat3& lattice_to_plane);

struct LineFit {
    int family{};
    std::int32_t index{};
    std::size_t points{};
    double centre[2]{};
    double direction[2]{};
    double normal[2]{};
    double rms_px{};
    double t_min{}, t_max{};
    std::vector<double> t;               // sorted along the line
};
[[nodiscard]] std::vector<LineFit> FitLines(const PreparedPoints& points, const Assignment& a, double focal, std::size_t min_points);

struct NodeFit {
    bool ok{};
    std::size_t nodes{};
    double rms_px{};
    Mat3 lattice_to_plane{};
};
[[nodiscard]] NodeFit FitNodes(const std::vector<LineFit>& lines, const Mat3& start, double focal, const GridSheetDetectionSettings& s);

struct VanishingFit {
    bool ok{};
    double tilt_deg{};
    render::Point lean_sheet_deg{};
    double image_x_azimuth_deg{};
};
[[nodiscard]] VanishingFit FitVanishing(const std::vector<LineFit>& lines, const Mat3& lattice_to_plane);

// Share of the coverage cells (cell_px pixels square) that hold at least `min_points` used points.
[[nodiscard]] double VisibleFraction(const std::vector<RidgePoint>& ridges, const PreparedPoints& points, const Assignment& a,
    std::uint32_t width, std::uint32_t height, std::uint32_t cell_px, std::uint32_t min_points);

struct FlatnessCheck {
    double off_lattice_fraction{};
    std::size_t judged_cells{};
};
// Cells of the picture in which the points of a line family form a coherent grid whose offset from the fitted lattice
// exceeds `offset_px`, over all cells that hold enough points of the family.
[[nodiscard]] FlatnessCheck CheckFlatness(const LatticeParameters& p, const PreparedPoints& points, const std::vector<RidgePoint>& ridges,
    std::uint32_t width, std::uint32_t height, double focal, double offset_px, const GridSheetDetectionSettings& s);

}   // namespace a0::m2::experimental::grid_sheet_tilt::detail
