#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/experimental/registration_pose.hpp"
#include "a0/m2/experimental/seam_blend.hpp"

#include <cstdint>

namespace a0::m2::experimental {
// The output-to-camera mappings of the lens-aware fixed composite (#405,
// docs/design/fixed-composite-lens.md 4.1 and 5). Camera, file and clock free.
//
// The mappings are the document render's mappings (render::DocumentOutputToCamera:
// output pixel (i,j) is the document point (left+(i+0.5)p, top+(j+0.5)p) mm,
// projected through the lens) plus the one per-capture correction the owner allowed,
// a scale of camera B about a fixed centre P:
//
//   (X', Y') = P + ((X, Y) - P) / k      applied to B's document point before projection
//
// k = 1 is the uncorrected mapping and is bit for bit render::DocumentOutputToCamera.
// The camera's intrinsics, coefficients and document-to-image matrix are never
// changed, so the profile (and its fingerprint) stays the same for every k.
//
// This is the one place that puts k into B's mapping. The creating entrance
// (fixed_composite_creation.hpp) evaluates the allowed range of k through it, and the
// per-capture estimation and the JPEG, live and gate paths (#405 W3 to W5) build their
// pairs from the same functions, so that what was checked at creation is what is drawn.

// The document point (mm) that camera's fixed mapping puts on the raw stored-order
// pixel: the inverse of render::TryProjectDocument (Newton on the lens, then the
// inverse of the document-to-image matrix). False where the pixel has no document
// point (the lens inverse does not converge, the matrix is not invertible). The
// iteration limit and tolerance inside are numerical stopping rules, not optical
// acceptance values.
[[nodiscard]] bool TryDocumentPointOfRawPixel(const render::CameraProjection& camera,render::Point raw_pixel,render::Point& document_mm);

// The centre P of camera B's scale correction, in document mm: the document point that
// B's fixed mapping puts on the centre of B's pixel grid, ((w-1)/2,(h-1)/2) of the
// 7360x4912 frame the rig profile fixes (design 5.2). Where the principal point is the
// grid centre and the lens is symmetric there, this is the closed form H^-1(centre);
// otherwise the lens inverse is solved. Throws std::invalid_argument if there is none.
[[nodiscard]] render::Point FixedCompositeScaleCenterMm(const render::CameraProjection& camera_b);

// render::DocumentOutputToCamera with the document point scaled about center_mm by 1/scale
// first. scale must be finite and positive; scale == 1 returns the unscaled mapping
// exactly. Throws std::invalid_argument for an invalid raster or scale, and, when called,
// where the projection is not representable (as the unscaled mapping does).
[[nodiscard]] render::OutputToCamera ScaledDocumentOutputToCamera(const render::OutputRaster& raster,const render::CameraProjection& camera,
    render::Point center_mm,double scale);

// DocumentMappedFixedSeamInput(parameters) with camera B's mapping scaled by `scale` about
// FixedCompositeScaleCenterMm(parameters.camera_b). A is untouched. Throws what
// DocumentMappedFixedSeamInput throws, and for an invalid scale.
[[nodiscard]] MappedFixedSeamInput ScaledDocumentMappedFixedSeamInput(const render::DocumentRenderParameters& parameters,double scale);

// The per-capture pose of camera B (#417, docs/design/fixed-composite-registration.md). The scale k above is
// one component of it (the height); a mapping uses k or a pose, not both.
//
// B's document point X is replaced by G(X) before the calibrated projection, where G is the document-plane
// homography of the pose (content_registration::PoseModel::Homography): a feature that the calibrated mapping
// puts at X is then put at X again when B has moved by the pose. The intrinsics, the coefficients and the
// document-to-image matrix of the camera are never changed.
//
// The zero pose is exactly the unchanged mapping: PoseCorrectedDocumentOutputToCamera(raster, camera, model,
// {}) returns render::DocumentOutputToCamera(raster, camera) itself, bit for bit. This is the one place that
// puts the pose into B's mapping.
//
// pose must be finite. Throws std::invalid_argument for an invalid raster or pose, and, when called, where the
// projection is not representable (as the unmoved mapping does).
[[nodiscard]] render::OutputToCamera PoseCorrectedDocumentOutputToCamera(const render::OutputRaster& raster,const render::CameraProjection& camera,
    const content_registration::PoseModel& model,const content_registration::PoseParameters& pose);
// Same mapping from the homography G itself (document mm to document mm, G(X) as above). G must be finite and
// invertible in effect (a point whose image has no finite position is a failed sample, a false return).
[[nodiscard]] render::OutputToCamera HomographyCorrectedDocumentOutputToCamera(const render::OutputRaster& raster,const render::CameraProjection& camera,
    const render::Matrix3& document_homography);
// DocumentMappedFixedSeamInput(parameters) with camera B's mapping corrected by the pose. A is untouched. The
// zero pose gives DocumentMappedFixedSeamInput(parameters) exactly.
[[nodiscard]] MappedFixedSeamInput PoseCorrectedDocumentMappedFixedSeamInput(const render::DocumentRenderParameters& parameters,
    const content_registration::PoseParameters& pose);

// The registered mapping of camera B (#417, docs/design/fixed-composite-registration.md 11.3, 11.4, 11.13 1 and 2): G(p) Pi(pi)
// with layer 2. This is the one place that puts the paper surface and layer 2 into B's mapping; the composite, the gate and the
// estimator take their pairs from it. A is never changed. See registration_mapping.hpp for the values.
//
// For the output sample (x, y), B's raw position is the calibrated projection of
//   G(p) Pi(pi) (D(x + s e_b.x, y + s e_b.y)),   s = q(y) w(|x - seam_column|),
// where D is render::DocumentOutputToCamera's output-to-document arithmetic. Bit for bit:
//   - pi = 0 and q = 0 (no profile, or every sample 0): PoseCorrectedDocumentOutputToCamera(raster, camera_b, model_b, p) itself,
//     so p = p_b gives the condition's fixed mapping and p = 0 the calibrated mapping;
//   - where w = 0 (u >= a + L): the same arithmetic as the mapping without layer 2.
// Throws std::invalid_argument for an invalid raster, a seam column outside it, a non-finite pose or surface, an invalid
// profile (ValidateParallaxProfile) or one made for another seam column, or a surface that is not below both cameras over the
// raster (SurfaceHomographyHoldsAt at the corners, widened by the largest shift); and, when called, where the projection is
// not representable.
[[nodiscard]] render::OutputToCamera RegisteredDocumentOutputToCamera(const render::OutputRaster& raster,const render::CameraProjection& camera_a,
    const render::CameraProjection& camera_b,const content_registration::PoseModel& model_b,const RegisteredMapping& mapping,
    std::uint32_t seam_column);
// DocumentMappedFixedSeamInput(parameters) with camera B's mapping registered. A is untouched. A mapping with only the pose
// gives PoseCorrectedDocumentMappedFixedSeamInput(parameters, mapping.pose) exactly (after the same validation).
[[nodiscard]] MappedFixedSeamInput RegisteredDocumentMappedFixedSeamInput(const render::DocumentRenderParameters& parameters,
    const RegisteredMapping& mapping,std::uint32_t seam_column);

// Layer 2 pieces, output px (design 11.4). The makers above use these same functions.
// w(u): 1 for u <= a_px, (1 + cos(pi (u - a_px) / l_px)) / 2 for a_px < u < a_px + l_px, 0 beyond; C1, with the largest slope
// pi / (2 l_px) in the middle of the taper. u is |x - seam column|. Throws std::invalid_argument for a non-finite or negative u
// or a_px, or a non-finite or non-positive l_px.
[[nodiscard]] double ParallaxSeamWeight(double u_px,double a_px,double l_px);
// q(y): the monotone cubic Hermite through the samples at first_row_px + i row_spacing_px (registration_mapping.hpp), with the
// end values before the first and after the last sample. Throws std::invalid_argument for an empty profile, a non-finite first
// row, a row spacing that is not finite and positive, or a non-finite y.
[[nodiscard]] double ParallaxShiftAtRow(const ParallaxProfile& profile,double y);
// The largest strain of the shift across the columns, (pi / 2) max |q_px| / l_px (design 11.4). Throws std::invalid_argument for
// an empty profile, a non-finite sample or an L that is not finite and positive.
[[nodiscard]] double ParallaxMaximumStrain(const ParallaxProfile& profile);
// A profile the makers accept: at least one sample, every sample finite, a finite first row, a finite positive row spacing, a
// finite a_px >= 0, a finite l_px > 0, and a finite baseline of unit length (|norm - 1| <= 1e-9, a numerical check of a computed
// unit vector, not an acceptance value). Throws std::invalid_argument otherwise. The makers also require profile.seam_column to
// be their seam column.
void ValidateParallaxProfile(const ParallaxProfile& profile);
}
