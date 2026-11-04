#pragma once
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include <cstdint>
#include <vector>

namespace a0::m2::experimental {
// The checks made when a fixed composite condition v2 is created
// (docs/design/fixed-composite-lens.md 3.9, items 3 to 5). Camera, file and clock
// free; no image is read. They run once, in the creating entrance, because they
// evaluate the mapped composite over every row; reading a condition does not repeat
// them (the profile and the record are hash-bound to what was checked here).
//
// The checks use the same mappings the renderers use (fixed_composite_maps.hpp) and the
// mapped fixed-seam functions of seam_blend.hpp (CanRenderMappedFixedSeam,
// MappedFixedSeamBand), over a blank frame of the profile's fixed sensor size, so a
// column passes here exactly when the blend would accept it.

// The scales k of camera B that are checked for a limit in percent: 1-limit/100, 1 and
// 1+limit/100 (once when the limit is 0). design 3.3: with the lens monotonic, covering at
// the ends and at 1 covers the range between. Throws std::invalid_argument unless the
// limit is finite and in [0, 100): at 100 or more a scale of 1-limit/100 is not a scale.
[[nodiscard]] std::vector<double> FixedCompositeCheckedScales(double scale_auto_correction_max_percent);

// Item 3. On every output row, camera A covers the columns from the first one up to
// some column, camera B covers the columns from some column up to the last one, and the
// two meet or overlap: every output sample is covered by A or B, and A is the left
// camera. Coverage is the one of render::SampleBilinear ([0,w)x[0,h) of the frame).
// Throws std::invalid_argument naming the row and columns that are not.
void RequireFixedCompositeCoverage(const MappedFixedSeamInput& input);

// Item 4. The seam column and half the blend width (kSeamBlendDesign.width_pixels) lie in
// the overlap of A and B on every row (CanRenderMappedFixedSeam). Throws
// std::invalid_argument otherwise, including when a row has no overlap or its overlap is
// not one run.
void RequireFixedCompositeSeam(const MappedFixedSeamInput& input,std::uint32_t seam_column);

// The columns [first,last_exclusive) covered by both cameras on every row, from the rows
// of MappedFixedSeamBand (the same rule as v1's FixedCompositeOverlapBand: the largest
// first and the smallest end). Throws std::invalid_argument if there is none or no rows.
[[nodiscard]] FixedCompositeBand FixedCompositeBandOfRows(const std::vector<MappedOverlapRow>& rows);
// (first+last_exclusive)/2 rounded down, as v1's ProvisionalSeamColumn: an even-width band
// takes the right one of the two middle columns.
[[nodiscard]] std::uint32_t FixedCompositeBandCenter(const FixedCompositeBand& band);
// The band at scale 1 for these parameters (a calibrated draft with the condition's raster,
// BindFixedCompositeProfile's parameters), and its centre: the provisional seam column
// (item 5). Throws std::invalid_argument like FixedCompositeBandOfRows and MappedFixedSeamBand.
[[nodiscard]] FixedCompositeBand FixedCompositeBandAtUnitScale(const render::DocumentRenderParameters& parameters);
[[nodiscard]] std::uint32_t FixedCompositeProvisionalSeamColumn(const render::DocumentRenderParameters& parameters);

// The creation check for MakeFixedCompositeConditionV2: for each checked scale items 3
// and 4 on camera B scaled about FixedCompositeScaleCenterMm, then item 5 for a
// `band-center-provisional` seam (an `operator` column is checked by item 4 only).
// Throws std::invalid_argument, naming the scale, for the first check that fails.
[[nodiscard]] FixedCompositeCreationCheck MakeFixedCompositeCreationCheck();

// Capture pixels per millimetre of the document at a point, sqrt(|det J|) of the projection
// by central differences of 1 mm; times 25.4 it is the DPI at which one output pixel is one
// capture pixel there (design 3.3, 8). A hint for the person who chooses the DPI, never
// recorded. Throws std::invalid_argument where the projection is not representable.
[[nodiscard]] double FixedCompositeCapturePixelsPerMm(const render::CameraProjection& camera,render::Point document_mm);
}
