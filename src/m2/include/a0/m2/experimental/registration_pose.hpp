#pragma once
#include "a0/m2/document_render.hpp"

#include <array>
#include <cstdint>

namespace a0::m2::experimental {
// The paper surface (layer 1): z = h0 + g_x (x - x_0) + g_y (y - y_0) over the table, with (x_0, y_0) the seam column's
// middle (11.3, content_registration::SurfaceOriginMm). pi = (h0, g_x, g_y). All three components 0 is the table plane, and
// the mapping is then the one without a surface bit for bit (11.13 item 2). Declared here, next to Pi(pi) that takes it;
// registration_mapping.hpp holds the other values of the mapping.
struct SurfacePlane {
    double h0_mm{};      // height of the paper over the table at (x_0, y_0)
    double slope_x{};    // dz/dx, dimensionless (tangent), x to the right in the output frame
    double slope_y{};    // dz/dy, dimensionless (tangent), y downward in the output frame
    [[nodiscard]] bool IsZero() const {return h0_mm==0 && slope_x==0 && slope_y==0;}
};
}

namespace a0::m2::experimental::content_registration {
// The pose of camera B as a small change from its calibrated pose (#417). Camera, file and clock
// free; deterministic.
//
// The change is a rigid motion of the document plane in B's camera frame (x right, y down, z
// forward, mm). The plane turns by (rx,ry,rz) about the pivot, the document point on B's optical
// axis (so a tilt does not move the image centre), and then moves by (tx,ty,tz). tz is given as a
// percent of the pivot depth (the "height"). Rotations are right-handed about the camera axes.
// The intrinsics and the lens coefficients are never changed.
struct PoseParameters {
    double rx_deg{},ry_deg{},rz_deg{};
    double tx_mm{},ty_mm{},tz_percent{};
    [[nodiscard]] bool IsZero() const {return rx_deg==0 && ry_deg==0 && rz_deg==0 && tx_mm==0 && ty_mm==0 && tz_percent==0;}
};

// Output sample index to document mm (render::DocumentOutputToCamera's arithmetic).
struct PlaneFrame {
    double left_mm{},top_mm{},pitch_mm{};
};
[[nodiscard]] PlaneFrame FrameOfRaster(const render::OutputRaster& raster);

// ---- The paper surface (#417, docs/design/fixed-composite-registration.md 11.2 and 11.3) ------------------------------
//
// The table frame: (x, y) are document mm on the table plane (the calibration plane), and a height is measured from the
// table toward the cameras, in mm. A point at height h over the document point (x, y) is, in a camera's frame,
// M (x, y, 1) - h n, where M is the camera's plane matrix (PoseModel) and n the unit normal of the plane pointing away
// from the camera; this holds for the calibrated plane and for the plane moved by a pose alike.

// A camera centre in the table frame: the document point under it (x_mm, y_mm) and its height above the table.
struct CameraCentre {
    double x_mm{},y_mm{},height_mm{};
};

// The paper surface is a0::m2::experimental::SurfacePlane (above): the paper over the document point (x, y) is at the height
// h0_mm + slope_x (x - x0) + slope_y (y - y0) mm above the table, where (x0, y0) is SurfaceOriginMm of the raster and the seam
// column. The zero plane is decided on the components being exactly 0, as the zero pose is.

// The plane origin (x0, y0) of design 11.3, document mm: the middle of the seam column, the document point of the output
// sample (seam_column, (height_pixels - 1) / 2), computed with render::DocumentOutputToCamera's arithmetic. It depends on the
// raster and the seam column only, so the estimator, the mapping and the report read the same h0. Throws
// std::invalid_argument for an invalid raster or a seam column outside it.
[[nodiscard]] render::Point SurfaceOriginMm(const render::OutputRaster& raster,std::uint32_t seam_column);

// Pi(pi) of design 11.3: the homography, document mm to document mm and normalized to [8] = 1, from the output document point
// X (the table point camera a's calibrated mapping puts there) to the table point on the ray from camera b's centre through
// the paper point P that a's ray through X meets on the surface. B's calibrated projection of Pi(X) therefore shows the
// paper point that A shows at X. The construction is exact for a plane, including the departure from proportionality by
// 1/(H - h); for equal heights and a level surface the displacement is -(b - a) h0 / (H - h0) (design 11.2).
// The zero plane gives the identity up to rounding; the mapping makers return the unchanged mapping for it exactly.
// Throws std::invalid_argument for a centre that is not finite or not above the table, a plane that is not finite, or a
// surface that is not below camera a at a's foot point.
[[nodiscard]] render::Matrix3 SurfaceHomography(const CameraCentre& a,const CameraCentre& b,render::Point origin_mm,const SurfacePlane& surface);
// Whether Pi(pi) is the physical construction at the document point X: a's ray through X meets the surface in front of a
// (the denominator of a's ray is positive) and the paper point P is below b. Both are affine in X, so they hold over a
// rectangle when they hold at its four corners. False for a non-finite input.
[[nodiscard]] bool SurfaceHomographyHoldsAt(const CameraCentre& a,const CameraCentre& b,render::Point origin_mm,const SurfacePlane& surface,
    render::Point document_mm);

class PoseModel {
public:
    // Decomposes the camera's document-to-image matrix with its own intrinsics. Throws
    // std::invalid_argument when the camera is not in front of the plane or the columns are degenerate.
    explicit PoseModel(const render::CameraProjection& camera);
    // The document-plane homography G = H0^-1 H(p), document mm to document mm, normalized to
    // g22 = 1. A feature of B that the calibrated mapping puts at document point X is put at G(X) once
    // B has moved by p; B's mapping that follows the moved pose is the calibrated projection of G(X).
    // Close to the identity for p = 0 (equal up to rounding; PoseCorrectedDocumentOutputToCamera in
    // fixed_composite_maps.hpp returns the unchanged mapping for p = 0 exactly).
    [[nodiscard]] render::Matrix3 Homography(const PoseParameters& p) const;
    // H(p), document mm to the undistorted pixel (the matrix a camera projection holds), normalized to
    // h22 = 1 like a profile's matrix.
    [[nodiscard]] render::Matrix3 CameraHomography(const PoseParameters& p) const;
    // Depth of the pivot (mm) and the pivot as a document point (mm).
    [[nodiscard]] double PivotDepthMm() const {return pivot_camera_[2];}
    [[nodiscard]] render::Point PivotDocumentMm() const {return pivot_document_;}
    // The camera centre in the table frame once the camera has moved by p (the zero pose: the calibrated centre, from the
    // calibrated plane matrix itself). Pi(pi) is built with B's centre after the move: G(p) Pi(pi) is then the exact
    // mapping of a camera B moved by p over a paper on the surface (design 11.3). Throws std::invalid_argument for a
    // non-finite pose or when the camera is not above the table.
    [[nodiscard]] CameraCentre Centre(const PoseParameters& p) const;
private:
    [[nodiscard]] render::Matrix3 MovedPlane(const PoseParameters& p) const;
    render::Matrix3 k_{},m_{},m_inverse_{};
    double lambda_{};
    std::array<double,3> pivot_camera_{};
    render::Point pivot_document_{};
};

// Displacement (B minus A, output px) at an output sample index that the pose p causes.
[[nodiscard]] render::Point PoseDisplacement(const PoseModel& model,const PlaneFrame& frame,const PoseParameters& p,double x,double y);

// G(p) Pi(pi), document mm to document mm and normalized to [8] = 1: B's document point for the output document point X once
// B has moved by p and the paper lies on the surface (design 11.3). Pi(pi) is built from a (camera A's calibrated centre) and
// model_b.Centre(p). A zero surface gives model_b.Homography(p) itself. Throws what Centre and SurfaceHomography throw.
[[nodiscard]] render::Matrix3 RegisteredHomography(const PoseModel& model_b,const CameraCentre& a,render::Point origin_mm,
    const PoseParameters& p,const SurfacePlane& surface);
// Displacement (B minus A, output px) at an output sample index that the pose p and the surface cause together:
// (G(p) Pi(pi) (X) - X) / pitch. A zero surface gives PoseDisplacement(model_b, frame, p, x, y) exactly.
[[nodiscard]] render::Point RegisteredDisplacement(const PoseModel& model_b,const PlaneFrame& frame,const CameraCentre& a,render::Point origin_mm,
    const PoseParameters& p,const SurfacePlane& surface,double x,double y);
}
