#pragma once
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace a0::m2::experimental {
// The checks made when a fixed composite condition v3 is created, and the cheap form that runs on every capture (#417,
// docs/design/fixed-composite-registration.md 11.9 "checks when making", 11.23). Camera, file and clock free; no image is
// read. fixed_composite_creation.cpp (v2) is not changed. This header declares the shapes only; the lane that owns
// fixed_composite_creation_v3.cpp writes the bodies.

// What the creation check found, for the creating entrance to show (no value of a measured rig; counts and design numbers only).
struct CreationCheckDetailsV3 {
    double lift_cap_px{};                       // cap = maxLiftMm x k, k from the base pose and the calibration's geometry
    double maximum_strain{};                    // (pi/2) cap / L
    std::uint32_t uncovered_edge_pixels{};      // pixels of the output's outer edge that B does not cover at the range's end (11.23.3); not a refusal
    std::vector<std::string> details;           // "key=value" lines
};

// 11.23.2: with Theta = (p_b + B's envelope) x the paper surface's range x q in [-cap, +cap],
//  (a) the seam with half the blend width lies in the cover of A and B on every row, over all of Theta;
//  (b) every output pixel is covered by A or B over B's envelope (pi = 0, q = 0);
//  (c) the strain bound (pi/2) cap / L is not above s_max, at q = +-cap;
//  (d) the base pose lies inside B's envelope measured from the calibration.
// The paper surface's range does not refuse by the outer edge (11.23.3); the count is in the details. Evaluated by
// linearising the margin at the ten components of Theta and checking the worst vertex per row block with the true mapping
// (11.23.4). Throws std::invalid_argument naming the row and the check that failed. The returned check keeps what it found in
// `details` when it is not null.
[[nodiscard]] FixedCompositeCreationCheckV3 MakeFixedCompositeCreationCheckV3(CreationCheckDetailsV3* details);

// Design constants of layer 2 that the strain bound (c) needs (ParallaxDesign::weight_falloff_px = L and ::maximum_strain = s_max,
// parallax_correction.hpp). Not acceptance values and not defaulted: the caller takes them from the layer-2 lane's design. Without them
// a condition that enables layer 2 (registration.parallax) cannot be checked, and the check refuses it (fail closed).
struct LayerTwoStrainDesignV3 {
    double falloff_px = std::numeric_limits<double>::quiet_NaN();
    double maximum_strain = std::numeric_limits<double>::quiet_NaN();
};
struct CreationCheckOptionsV3 {
    std::optional<LayerTwoStrainDesignV3> layer_two;
};
// The same check with the layer-2 design given. MakeFixedCompositeCreationCheckV3(details) is this with no layer-2 design.
[[nodiscard]] FixedCompositeCreationCheckV3 MakeFixedCompositeCreationCheckV3(CreationCheckDetailsV3* details,const CreationCheckOptionsV3& options);

// The baseline geometry of a record (11.2): e_b, the unit vector of the baseline in the output frame, |b| in mm, the mean camera height
// over the table minus the paper's mean height (the surface range's height when there is one, which is the larger k), and k = rho |b| / (H - h),
// output pixels per mm of paper height. B is taken at the base pose. Throws std::invalid_argument when the cameras are not in front of the plane.
struct BaselineGeometryV3 {
    render::Point e_b{};
    double baseline_mm{};
    double mean_camera_height_mm{};
    double k_px_per_mm{};
};
[[nodiscard]] BaselineGeometryV3 BaselineGeometryOfCondition(const FixedCompositeConditionV3& c,const FixedCompositeProfileBinding& binding);

// The cheap check made on each capture on the mapping that was chosen (11.23.4): the seam and half the blend width inside the
// overlap on every row (CanRenderMappedFixedSeam's rule) and the outer edge of the output covered. A mapping beyond the
// ranges is not covered by the creation check; this is where it is first checked.
struct RegisteredMappingCover {
    bool seam_covered{};
    bool output_covered{};
    std::uint32_t uncovered_edge_pixels{};
};
[[nodiscard]] RegisteredMappingCover CheckRegisteredMappingCover(const render::DocumentRenderParameters& parameters,
    const RegisteredMapping& mapping,std::uint32_t seam_column,const SeamBlendSettings& blend);
}
