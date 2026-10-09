# Document-plane pixel renderer (#280, preceding #274 / #273)

The pure `a0_m2_render` library now has `RenderDocumentPair` and
`RenderMappedPair` alongside the unchanged legacy `RenderPair`. This step adds
the shared geometry and sampling engine. It does not add a profile reader,
fingerprint, approved-v2 product entry point, .NET adapter, image I/O, or draft
evaluation executable. Those remain in #274 and #273. No calibration or
quality decision is made here.

## Mapping and validation

All constructors require their parameters: intrinsics, all five named
Brown-Conrady coefficients, the document-to-image homography, integer-micrometre
output region, DPI, declared dimensions, layout, and interpolation kernel.
There are no numerical defaults. The existing decoded-image limits apply:
32768 pixels per dimension and 200 million pixels total.

The raster dimensions must equal the exact integer ceiling of
`extentUm * dpi / 25400`. DPI is an integer in the JFIF range 1..65535.
One pitch `p = 25.4 / dpi` is computed, then output sample `(i,j)` uses
`X = leftMm + (i+0.5)*p`, `Y = topMm + (j+0.5)*p`. Each camera directly maps
that point through its homography, normalized intrinsics, Brown-Conrady
distortion, and intrinsics back to stored-order raw sample coordinates.
Each output sample is interpolated once from each contributing raw BGR image.
There is no undistorted intermediate image or inverse lens solver.

The preliminary coverage traversal evaluates geometry without sampling image
values; the subsequent rendering traversal applies that same mapping and
interpolates raw pixels. This is two geometry traversals, not two image
resamplings. Mapping functions supplied to the low-level pixel-space API must
be immutable and deterministic.

Validation rejects nonfinite parameters, nonpositive focal lengths, an
unnormalized or numerically singular homography, nonpositive projective
denominators, inconsistent raster dimensions, and numerical projection
overflow. The mathematical lens check requires
`1 + 3*k1*t + 5*k2*t² + 7*k3*t³ > 0` for every `t` in the used squared-radius
interval, including its interior stationary points. Both the declared region
and the ceil raster's outer edges are covered by these domain checks.
Positive-denominator projective geometry gives a convex quadrilateral;
normalized squared radius reaches its maximum at a corner. This validates
the radial map, without claiming the entire tangential model is an approved
physical calibration.

A low-level mapping returning `false` means no projection. A `true` result
must contain finite coordinates. Invalid coordinates fail even if the other
camera covers the sample. A finite coordinate outside `[0,W) × [0,H)` simply
does not contribute. If neither camera covers an output sample, rendering
fails. The second traversal checks that sampling coverage agrees with the
preliminary traversal. Both source BGR buffers remain immutable.

## Coverage, blending, and interpolation

Overlap comes from coverage on the actual output sample grid, rather than
from warped image corner bounds. In a horizontal layout each row has its own
contiguous overlap runs; in a vertical layout each column has them. A run
`[begin,end)` has CAM-B weight `(position-begin)/(end-begin)`. Multiple runs
are separate bands. An overlap of one sample has weight zero at its sole
sample. Single-camera samples use that camera. Seam navigation only observes
real shared-coverage pixels, with no fallback center.

The explicit kernels are:

- `bilinear`: existing legacy sampler and its half-open coverage convention.
- `bicubic_catmull_rom`: separable Catmull-Rom, coefficient a=-0.5, 4×4 taps.
  Tap indices are clamped at the image borders. Overshoot is retained through
  blending; final BGR conversion rounds with `lround` and clamps to 0..255.

The generic pixel-space mapping entry point permits the exact identity test
specified by design section 3.4. It avoids the mm/DPI rounding step and uses
the same integer source samples as legacy rendering. Bilinear identity output
and seam navigation agree byte-for-byte in both layouts. This identity check
does not assert equality for fractional or distorted coverage boundaries:
legacy feather uses continuous AABBs, while the new renderer uses sampled
coverage runs.

Coverage and overlap metadata are immutable during rendering, and output rows
write separate bytes. This implementation is sequential; it does not claim
that a parallel executor or parallel seam-candidate reduction has been tested.

## Independent evidence

`m2_document_render_contracts` evaluates two projective, five-coefficient
camera models at 234 document points with a separately written polynomial
oracle. On MSVC `long double` has binary64 precision: the independent
algebraic implementation, rather than extra precision, is the check.
Additional fixtures exercise real BGR drawing, affine reproduction, borders,
Catmull-Rom overshoot, single/multiple/no overlap, changing coverage,
projection overflow, and radial derivative extrema.

The MTF comparison uses an independent analytic Gaussian-blurred slanted
edge, sigma 0.8 input pixels, sampled at integer indices and quantized to
grayscale values 32..224. Both cameras contain the same edge. A 256×256
output samples an interior region of a 512×512 source with an explicit
fractional translation and unit scale. No JPEG, aperture, gamma, noise, or
camera optics are part of this fixture. The valid ROI has rows [16,240) and
known signed normal distances [-12,12) pixels; all bicubic taps are interior.

The independent estimator bins the ESF at 1/8 output pixel along the known
edge normal, takes a central derivative, applies a Hann LSF window, normalizes
the DFT by DC, and interpolates the first 0.5 crossing at a frequency grid
of 1/4096 cycles/output pixel. It applies no derivative/bin correction and
claims no ISO conformance. Six direct analytic controls use three slopes and
sigma 0.8/1.2, comparing the estimate with the known Gaussian frequency
`sqrt(2*ln(2))/(2*pi*sigma)`. A flat image must return no measurement.

The resulting kernel comparison is a diagnostic measurement of this fixture,
including its original blur and input/output quantization. It is not a lens
MTF, quality verdict, choice of production kernel, or evidence that #274's
compressed synthetic-pair fiducial criterion has been completed.

```powershell
cmake -S . -B build/stub -A x64 -DBUILD_TESTING=ON
cmake --build build/stub --config Release --parallel 2
ctest --test-dir build/stub -C Release -R 'm2_document_render|m2_render_contracts' -V
```

| Slant and source phase (x,y) | Direct analytic MTF50 | Bilinear MTF50 | Catmull-Rom MTF50 | Bilinear / cubic RMS (8-bit levels) |
|---|---:|---:|---:|---:|
| +1/8, (0.37,0.23) | 0.233603606106 | 0.199243617870 | 0.221999422095 | 2.821566996 / 1.147967533 |
| +1/12, (0.61,0.47) | 0.234591926685 | 0.200180945430 | 0.223408367189 | 2.834310291 / 1.182535320 |
| -1/10, (0.19,0.73) | 0.234779108783 | 0.211649850277 | 0.229848222554 | 1.848385244 / 0.815136885 |

MTF50 units are cycles/output pixel. The six Gaussian estimator controls had
maximum absolute discrepancy below 0.0031 cycles/pixel from their analytic
frequency. These controls validate the diagnostic estimator under its stated
sampling/quantization conditions; they do not calibrate a camera.

The independent projection oracle measured p95 9.0949470177292824e-13 and
maximum 1.8749713606747085e-12 raw camera pixels across the 234 points.
This is a forward-equation verification. Compressed synthetic-image fiducial
localization and full-size document-raster evaluation remain in #274/#275.

The generator isolation policy was expanded to reject the new warp source as
well as the legacy renderer. The regression first showed that the old source
list accepted `document_render.cpp`; the repaired policy passed all sixteen
configure controls, including direct and transitive source contamination.
The tests now call the actual shared policy rather than copying its list.

Final verification: SDK-less x64 builds succeeded in Debug and Release.
With `A0_LEASE_TEST_STRICT=1`, the full CTest suites passed 61/61 in both
configurations (Release 392.03 s, Debug 565.01 s). The explicit unit/zero-lens
projection bridge was strengthened after the Release full run; the final
Release geometry contract executable then passed 869 checks, and the Debug
full run used that same 869-check revision. The pure renderer was unchanged
by that test refinement. The independent projection and three MTF comparison
rows were identical across configurations. Source buffers were unchanged and
repeated draws were byte-identical in the tests for both kernels. Independent
static review and its NaN/fallback finding were resolved before the final
suite runs. No hardware, profile approval, or quality acceptance is inferred.