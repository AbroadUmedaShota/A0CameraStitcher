#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/registration_pose.hpp"
#include "a0/m2/seam_gate.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::experimental::registration_probe {
// Development probe of #417 (P0): which registration model explains the B-minus-A
// displacement field of a saved pair. Camera, file and clock free; deterministic
// (no random choice, fixed iteration order). It is not a quality measure, holds no
// acceptance value and is linked by no product target.
//
// The field is the one the seam gate measures (seam_gate::MeasureSeam): on a lattice
// over the whole overlap band, where B shows A's patch minus the point, in output
// pixels, with an uncertainty per point. Both cameras are placed by their own
// fixed mapping (render::DocumentOutputToCamera), so the field is what the
// fixed composite leaves unregistered.
//
// A model predicts that field from a few parameters and is fitted by weighted
// least squares with Tukey reweighting. A point on lines in one direction
// constrains only the component across the lines, as in the gate.

// The pose model moved to registration_pose.hpp when the estimator (content_registration.hpp) needed it.
using content_registration::PlaneFrame;
using content_registration::FrameOfRaster;
using content_registration::PoseParameters;
using content_registration::PoseModel;
using content_registration::PoseDisplacement;

// One counted point of the measured field.
struct Observation {
    double x{},y{};             // output sample index of the point
    bool one_directional{};     // only the component along `normal` is known
    render::Point normal{};     // unit normal of the lines (one_directional)
    render::Point d{};          // where B shows A's patch, minus the point (output px)
    double sigma_px{};          // measurement uncertainty
    double visible_px{};        // the gate's value: |d|, or |normal . d|
};
// The counted points (seam_gate::SeamPoint::Counted) of a measurement, in lattice order. `shift` is the
// displacement that was subtracted by placing B shifted before measuring (a prior that keeps the true
// displacement inside the search window); it is added back, so d is the displacement against the unshifted placement.
[[nodiscard]] std::vector<Observation> ObservationsOf(const seam_gate::SeamMeasurement& measurement,render::Point shift={0,0});
// The change of the field between two measurements of the same lattice (joined by sample index): after minus
// before. A pair of two-directional points gives a vector; where either side holds lines in one direction only
// that direction is kept (both must then agree to within 5 degrees). Points present on one side only, or with
// lines in two different directions, are left out. The paper and everything else that did not change cancels.
[[nodiscard]] std::vector<Observation> DifferenceObservations(const std::vector<Observation>& before,const std::vector<Observation>& after);

// ---- Models -----------------------------------------------------------------------------
enum class ModelKind {
    none,                      // the field itself
    translation,               // 2
    similarity,                // 4
    similarity_vertical_scale, // 5
    pose6,                     // 6: B's pose
    projective8,               // 8
    polynomial2,               // 12: second-order surface; a reference, not a candidate
};
[[nodiscard]] std::string_view ModelName(ModelKind kind);
[[nodiscard]] unsigned ModelParameterCount(ModelKind kind);

struct FitContext {
    PlaneFrame frame;
    const PoseModel* pose{};   // required for pose6
};
struct FitOptions {
    unsigned maximum_outer_iterations{40};
    unsigned maximum_inner_iterations{25};
    double tukey_constant{4.685};
    double scale_floor_px{0.25};   // the robust scale never goes below this
    double sigma_floor_px{0.1};    // a point's uncertainty never counts as smaller than this
};
struct ModelFit {
    ModelKind kind{};
    bool fitted{};                 // false: too few observations for the model
    bool converged{};
    std::vector<double> parameters;        // in the model's own units (see the .cpp), empty for none
    std::vector<double> parameter_sigma;   // one-sigma, same units, empty when not computed
    std::optional<PoseParameters> pose;    // pose6 only
    std::optional<PoseParameters> pose_sigma;
    unsigned observations{},inliers{};
    std::vector<double> weight;            // final robust weight per observation (0: rejected), empty for none
    double robust_scale_px{};
    unsigned iterations{};
    // Displacement the model predicts at an output sample index.
    std::function<render::Point(double,double)> predict;
};
[[nodiscard]] ModelFit FitModel(ModelKind kind,const std::vector<Observation>& observations,const FitContext& context,
    const FitOptions& options={});

// What is left of an observation after a model: |d - predicted|, or the component across the lines.
[[nodiscard]] double ResidualOf(const Observation& o,const ModelFit& fit);

struct ResidualStats {
    unsigned count{};
    double mean{},rms{},p95{},maximum{};
};
[[nodiscard]] ResidualStats StatsOf(std::vector<double> values);

// Residual along the seam column for every output row. Each lattice row contributes the observation
// nearest the seam column within one lattice step; rows between two contributing lattice rows no
// further apart than two steps take the linear interpolation of their values; no row is extrapolated.
// Rows that are not covered stay NaN.
struct SeamColumnSeries {
    std::vector<double> residual;      // one value per output row, NaN where not covered
    unsigned covered_rows{};
    ResidualStats stats;               // over the covered rows
};
[[nodiscard]] SeamColumnSeries SeamColumnResidual(const std::vector<Observation>& observations,const std::vector<double>& residuals,
    unsigned seam_column,unsigned lattice_step,unsigned height);

// Where the residual sits along the covered rows of a series (the shape the model leaves).
struct ResidualShape {
    double top_third_mean{},middle_third_mean{},bottom_third_mean{};
    double end_share{};   // share of the squared residual in the first and last fifth of the covered rows
};
[[nodiscard]] ResidualShape ShapeOf(const SeamColumnSeries& series);

// The gate's design with the point lattice changed (block and strip follow the spacing).
[[nodiscard]] seam_gate::SeamGateDesign FieldDesign(unsigned point_spacing_px,unsigned global_maximum_evaluations);

// ---- The pipeline check (--pipeline) -----------------------------------------------------------
// The output position a mapping sends to a given raw position: the inverse of an OutputToCamera on a raster. A coarse grid
// of the mapping is taken once; each query starts at the grid node nearest the raw position and refines it by Newton steps
// on a central-difference Jacobian. Used to place a chart marker detected in a raw image on the output raster through
// whatever mapping the composite uses, so that its two placements can be compared without the seam gate's measurer.
class OutputInverse {
public:
    // grid_step_px >= 2. Throws std::invalid_argument for an empty raster, a missing mapping or a bad step.
    OutputInverse(render::OutputToCamera map,std::uint32_t width,std::uint32_t height,unsigned grid_step_px=32);
    // Empty when no grid node maps near the raw position, when Newton does not reach tolerance_px in raw pixels, or when the
    // answer lies outside the raster.
    [[nodiscard]] std::optional<render::Point> Find(render::Point raw,double tolerance_px=1e-6) const;
private:
    render::OutputToCamera map_;
    std::uint32_t width_{},height_{};
    unsigned step_{};
    std::vector<render::Point> nodes_;   // raw position per grid node (NaN where the mapping has no sample)
    std::uint32_t columns_{},rows_{};
};

// The displacement along the seam column split by what each point measures (the field of #417 11.6 items 1 and 2), for a
// unit baseline e_b and the threshold c_min of |n . e_b|. Two-directional points give both components of d: along e_b
// and across it (e_perp = (-e_b.y, e_b.x)). A one-directional point gives only n . d: with |n . e_b| >= c_min it is read
// as a baseline value (n . d), otherwise as a value across the baseline. A component no point measured has count 0 and
// empty statistics: it is unmeasured, not 0.
struct ComponentStats {
    unsigned count{};
    double maximum_abs{};   // max |value|
    double rms{};
};
struct SeamComponents {
    ComponentStats two_along,two_across,two_magnitude;       // two-directional points: e_b . d, e_perp . d, |d|
    ComponentStats one_with_baseline,one_across_baseline;    // one-directional points: n . d
    unsigned counted{},not_counted{};
};
// Points of `measurement` that are counted (SeamPoint::Counted) and lie within |x - seam_column| <= half_width_px.
// Throws std::invalid_argument when e_b is not a unit vector or c_min is not in (0, 1].
[[nodiscard]] SeamComponents SeamComponentsOf(const seam_gate::SeamMeasurement& measurement,std::uint32_t seam_column,
    std::uint32_t half_width_px,render::Point e_b,double c_min);
}
