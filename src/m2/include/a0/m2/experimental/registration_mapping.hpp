#pragma once
#include "a0/m2/experimental/registration_pose.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace a0::m2::experimental {
// The values that go into camera B's registered mapping (#417, docs/design/fixed-composite-registration.md 11.3, 11.4).
// Values only: no function of the mapping is declared here (fixed_composite_maps.hpp builds the mapping from these), and
// the only dependency is registration_pose.hpp, so a library that includes this header does not link BCrypt or the
// estimator. Camera, file and clock free. Nothing here has a default that could pass for an owner value.

// The paper surface (layer 1), SurfacePlane, is declared in registration_pose.hpp next to Pi(pi) that takes it.

// The range of the surface that is estimated or warned about (condition v3: registration.surfaceEnvelope). NaN until set,
// so an unset range is rejected, not read; there is no default in code.
struct SurfaceEnvelope {
    double height_mm = std::numeric_limits<double>::quiet_NaN();   // |h0|
    double tilt_deg = std::numeric_limits<double>::quiet_NaN();    // the paper's inclination, atan(|(g_x, g_y)|)
};

// The seam-column correction of layer 2 (11.4). Values only; the samples are what the report records (the rest is derived
// again when reading). The maker (fixed_composite_maps.hpp) uses them as follows; the functions it names are the ones it
// calls, so an estimator or a reader can evaluate the same correction.
//   - B samples, at the output sample (x, y), what the mapping without layer 2 puts at (x, y) + q(y) w(u) e_b: the shift is
//     added to the output point before Pi and G. q is measured in output px as B position minus A position (the sign of the
//     gate's d) on the mapping without layer 2, so this removes it.
//   - x, y and seam_column are output sample indices, the coordinates the mapping is called with; u = |x - seam_column|.
//   - q(y) (ParallaxShiftAtRow): the samples joined by a monotone cubic Hermite (Fritsch-Carlson tangents, 0 at the first and
//     last sample), so q stays between neighbouring samples (max |q(y)| = max |q_px|) and is C1. Rows before the first and
//     after the last sample keep the end value.
//   - w(u) (ParallaxSeamWeight): 1 for u <= a_px, (1 + cos(pi (u - a_px) / l_px)) / 2 up to a_px + l_px, 0 beyond; C1.
//   - Only the component along e_b is corrected (11.13 4): the paper height makes no displacement across e_b, and an error
//     across it stays visible to the gate. The largest strain is (pi / 2) max |q| / l_px (ParallaxMaximumStrain).
struct ParallaxProfile {
    render::Point baseline{};            // e_b: the unit vector along the baseline, in the output frame
    double row_spacing_px{};             // Delta y_e: the spacing of the q samples (output rows)
    double first_row_px{};               // the output row (sample coordinate) of the first sample
    std::vector<double> q_px;            // q at first_row_px + i * row_spacing_px, output px along e_b; 0 where the row is empty
    double a_px{};                       // w is 1 for u <= a (a is at least Omega's half width)
    double l_px{};                       // w falls to 0 over the next l (a cosine, C1); 0 beyond a + l
    std::uint32_t seam_column{};         // w's centre; u = |x - x_s| in output sample indices. Equal to the maker's seam column
    // Every sample is exactly 0: no layer 2 (the mapping is the one without it, bit for bit).
    [[nodiscard]] bool IsZero() const {
        for(const double q:q_px)if(q!=0)return false;
        return true;
    }
};

// Everything that the mapping of camera B takes besides the calibration: the pose (6 degrees of freedom, against the
// calibrated pose), the surface and, when layer 2 is applied, the profile. The one place that turns these into a
// mapping is fixed_composite_maps.hpp (RegisteredDocumentOutputToCamera, RegisteredDocumentMappedFixedSeamInput). p = 0,
// pi = 0 and no profile (or q = 0) give the calibrated mapping bit for bit; {p_b, {}, std::nullopt} is the condition's fixed
// mapping (design 11.18 G4).
struct RegisteredMapping {
    content_registration::PoseParameters pose;
    SurfacePlane surface;
    std::optional<ParallaxProfile> parallax;
    // Only the pose acts: the mapping is PoseCorrectedDocumentOutputToCamera of the pose itself, bit for bit.
    [[nodiscard]] bool IsPoseOnly() const {return surface.IsZero() && (!parallax || parallax->IsZero());}
};

// The surface as the report records it (report v8: fixedCompositeRegistration.surface, docs/design/
// fixed-composite-registration-formats.md). Filled by the estimator; this header only fixes the shape so that the
// capture path, the report and the texts can be written at the same time. A field may be added by the estimator's
// lane in the same commit as its writer, reader and test (format rule 1).
enum class SurfaceState {
    estimated,       // the paper plane was fitted
    not_estimated,   // the condition allows a surface but none was fitted (the estimate is undetermined)
    disabled,        // the condition has no surfaceEnvelope: the six-degree estimate, no surface
};
struct SurfaceEstimate {
    SurfaceState state{SurfaceState::disabled};
    SurfacePlane plane;                   // pi-hat, meaningful when state is estimated
    double tilt_deg{};                    // the inclination of the plane, degrees
    std::array<double,9> covariance{};    // row-major, order h0 (mm), g_x, g_y
    double baseline_equivalent_mm{};      // u: the camera B movement along the baseline that makes the same field as h0 (11.3)
    struct Scale {                        // how large the paper is drawn because it is higher than the table (an estimate)
        double factor{1};                 // H / (H - h0) at the seam column's middle
        double minimum{1},maximum{1};     // over the output, when the plane is inclined
    } scale;
    struct Exceeded {bool height{},tilt{};} exceeded;   // against surfaceEnvelope. There is no scale line: see 11.16 and the owner's decision 10
    bool applied{};                       // the plane was put into the mapping
};

// B's pose and the surface as a person reads them in the drawing's own frame (design 11.24): right, left, up and down of
// the drawing, mm and degrees. Filled by the estimator's DisplayOf; the texts are made from these values and not from the
// raw pose. The report records them under registration.display.outputFrame (reserved shape; see the formats document).
struct OutputFrameDisplay {
    render::Point camera_motion_mm{};          // where the camera moved, in the drawing frame (x right, y down)
    render::Point lens_direction{};            // unit vector: the way B's lens faces over the drawing; 0 when the tilt is 0
    double lens_tilt_deg{};
    double rotation_clockwise_deg{};           // signed; positive is clockwise as seen on the screen (x towards y)
    render::Point surface_high_side{};          // unit vector: towards the higher side of the paper; 0 when the plane is level
    render::Point baseline_equivalent_motion_mm{};   // the camera motion along the baseline that explains h0 (the second reading of surface-height)
};
}
