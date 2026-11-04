#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/seam_blend.hpp"

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
}
