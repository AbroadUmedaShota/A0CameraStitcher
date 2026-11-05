#pragma once
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/experimental/overlap_estimation.hpp"
#include "a0/m2/experimental/still_review.hpp"
#include <optional>

namespace a0::m2::experimental {
// Per-capture scale correction of the lens-aware fixed composite (#405 W3, design
// docs/design/fixed-composite-lens.md 5). The v2 sibling of DecideFixedScale (#400).
// Camera, file and clock free; deterministic. The v1 function and report are unchanged.
//
// Model. Both cameras are mapped to the document plane by their fixed mappings
// (lens inverse and the inverse of the document-to-image matrix). A capture may
// differ from the condition by one thing only: B appears larger or smaller than the
// profile says. With X_a the document point (mm) of a pixel of A and X_b the document
// point of the matching pixel of B, that is
//     X_a = P + k (X_b - P)         P = FixedCompositeScaleCenterMm(camera B)
// exactly the map ScaledDocumentOutputToCamera draws with: an output sample X takes
// B at the document point P + (X - P) / k, whose content lies at X when X_a = X.
//
// Estimate. The agreed tiles of the similarity estimator of #365 are reused. Its own
// fit frees rotation and scale in the image, so only its tiles are taken. For each
// used tile the matched points of A and B are moved to document mm (see the frame
// below), q = X_b - P and t = X_a - P, and
//     t = k q + d,   k = sum((q - qm).(t - tm)) / sum(|q - qm|^2),   d = tm - k qm
// is fitted by least squares: the rotation is tied to the profile (none in the
// document plane), the translation d is free inside the estimate only, so a shift that
// is not a scale (shake, one pattern period) does not leak into k. Only k is applied.
// d is what the scale could not explain and is recorded in mm and in output pixels.
//
// Frame. The estimator's tiles are in the oriented frames (A left, B right, mount
// orientation applied, 7360x4912 as the profile fixes) in pixel-corner coordinates:
// the centre of pixel i is i + 0.5. The profile's mapping is defined on the stored
// pixel order with the centre of pixel i at integer i (ADR-0034, design 3.7). A tile
// coordinate c therefore becomes c - 0.5, and for rotated-180 w-1-(c-0.5) in x and
// h-1-(c-0.5) in y (the rotation reverses the whole-pixel order).
//
// Use. A tile counts when it is in the estimator's consensus and the document points of
// its two matches are within the estimator's search window of each other (the window in
// pixels divided by the capture's pixels per mm at A's point): a match one pattern period
// away cannot be explained by a scale of a few percent and is left out, as in v1. A tile
// whose point has no document point is left out. At least three tiles over two rows.
// No threshold of this module is an approved value: the only number that decides is the
// limit of the condition record (>= limit is inside, as in v1).
//
// The result applies by building the pair with ScaledDocumentMappedFixedSeamInput(profile
// parameters, applied_scale): the one place that puts k into B's mapping. The output
// raster is fixed by the condition and is not planned again; that every k inside the limit
// is covered and keeps the seam inside the overlap was checked when the condition was made.
// FixedScaleCorrectionV2, the result of the decision, is declared in still_review.hpp: a v7 report holds it (#405 W4).

// Fits k from an estimate of the oriented frames (frame_width x frame_height, which must be
// the profile's 7360x4912) and decides against the condition's limit. Pure. status is applied
// or exceeded when a fit exists, not_estimated otherwise (reason says why); seam_outside_overlap
// is never set here. Throws std::invalid_argument when the condition is not valid, the profile
// binding is not the one the condition names (fingerprint), the frame is not the profile's, or
// camera B has no scale centre.
[[nodiscard]] FixedScaleCorrectionV2 DecideFixedScaleV2(const FixedCompositeConditionV2& condition,const FixedCompositeProfileBinding& profile,
    const OverlapSimilarityEstimate& estimate,unsigned frame_width,unsigned frame_height);

// The raw stored-order pixel of a tile coordinate of the estimator (pixel-corner coordinates in
// the oriented frame), for the orientation of the condition. Exposed for the tests of the frame.
[[nodiscard]] render::Point FixedScaleRawPixelOfTile(double tile_x,double tile_y,MountOrientation orientation,unsigned frame_width,unsigned frame_height);
}
