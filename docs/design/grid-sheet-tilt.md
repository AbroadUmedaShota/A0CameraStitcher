# Grid sheet tilt (#440 stage 2)

Status: library and contract tests only. Nothing in the preview window, the capture path, the composite or the registration calls it yet.

## What it measures

From one picture of a flat square-grid sheet (the cutting mat that lies under the cameras) it reports, for one camera:

- the tilt: the angle between the camera's optical axis and the sheet's normal;
- the lean: the direction the lens points on the sheet, as an angle times a unit direction, in three frames (the sheet's own axes, the picture's axes with x right and y down, and the drawing's axes);
- how it got there: the number of ridge points, lines and intersections used, the residual, the share of the picture that holds grid points, the pitch of the first lattice, three tilts (lattice fit, line intersections, vanishing points) and their largest vector distance, and the share of picture cells that hold a coherent grid which is not on the fitted lattice.

For two cameras that see the same sheet it also reports the angle between the two optical axes, the difference of their leans in the reference camera's drawing frame, and the turn of their picture axes against each other.

The reference is the sheet, not gravity. A sheet that lies tilted on the table is measured as it lies.

## What it does not measure

- Any pass or fail. No value in this code says what tilt is acceptable, and the result carries no such word or colour.
- The lens. The intrinsics and the distortion of the given rig profile projection are used as they are and never re-estimated. A focal length that is off by a fraction scales the tilt by about the same fraction.
- The camera's height or position, or the sheet's real pitch. The grid is assumed square and flat; its pitch cancels out.
- Whether two pictures show the same sheet. The caller has to know.

## Stages

1. Ridge points: thin bright lines are found per pixel from the second derivatives of the picture (Gaussian derivative kernels, the line's centre from the zero of the first derivative along the normal). Banded, optionally multi-threaded; the points do not depend on the thread count or the band size.
2. Points without lens distortion: each ridge point and its line direction go to the undistorted normalised plane with the profile's own Brown-Conrady model (the model of `render::TryProjectDocument`).
3. First lattice: the picture is scored in windows for how periodic its lines are in two directions; the best window gives two directions, two pitches and two phases.
4. Lattice fit: a projective map from the plane to the lattice is fitted to all points (distance to the nearest integer line of the point's family, robust weights), growing the fitted window and shrinking the gate.
5. Tilt: `content_registration::OrientationOf` on a projection that has the profile's intrinsics and distortion and the lattice matrix as its document matrix. Tilt, direction and turn are defined there once. The two other ways (line intersections fitted to a lattice; the two vanishing points) are computed from the same points as a cross-check.
6. Two cameras: the numbering of a grid is fixed only up to a multiple of 90 degrees. The multiple is the one that brings the turn of the target's picture axes closest to the turn the two calibrated projections have against each other. If the next best multiple is not far enough behind, the pair is refused.

## Refusals

A measurement either returns a value or a refusal with a reason (`GridSheetRefusal`): too few ridge points; no grid found (no window shows two periodic line families, which includes lines in one direction only); the fit failed or its geometry is degenerate; too few lines; the lattice covers too few lines; the grid fills too little of the picture; the residual is too large; the three ways to the tilt disagree; the sheet is not flat (another part of the picture holds a coherent grid that is not on the fitted lattice, as a bent or curved sheet gives). The evidence is filled as far as the run got, also for a refusal. A pair is refused when either camera was not measured or the numbering cannot be told apart.

The lines behind these reasons are the caller's (`GridSheetAcceptance`). `WorkingDraftSettings()` is the one place that holds starting values; they are working numbers for development, not precision lines, and they only decide when the code gives up. The detection numbers (`GridSheetDetectionSettings`) are working numbers too: the assumed range of the grid pitch in pixels, window sizes, gates and point counts are in picture pixels and scale with the picture's resolution.

## Invariants

- The result types are measurement values for a person to read. They convert to nothing in the registration, the composite, the confirmed position, the capture gate, a rig profile or a fixed composite condition, and no function of those accepts them. The library is linked by no registration, composite, confirmed-position or capture library; CMake fails the configure step if one reaches it.
- Camera, file, clock and OpenCV free. A camera is a name and its projection; no camera name is built in.
- A programming error (an empty picture, a projection without a focal length, unset settings) throws `std::invalid_argument`. What the picture shows is never an exception.
- The tilt, direction and turn are `OrientationOf`'s. The drawing frame assumes the camera turned about its axis as at calibration.

## Tests

`tests/grid_sheet_tilt_tests.cpp` (CTest `m2_grid_sheet_tilt_contracts`; sections can be run one by one by name). The pictures are drawn by `tests/grid_sheet_scene.hpp`, which shares no code with the product: it renders a sheet from a known pose through a lens with distortion (the lens inverse, the ray and plane intersection, the line profile and the truth are its own). No real picture is used.

- lens: the stages' distortion model against `render::TryProjectDocument`;
- ridge: line positions to a fraction of a pixel at several angles and offsets; the same points for any thread count, band size, pixel format or stride;
- jacobian: the analytic Jacobian of the lattice fit against central differences;
- decomposition: the lattice matrix of a known pose, no picture: tilt, lean and azimuth against the geometry; a mirrored lattice is refused;
- accuracy: known tilts of 0 to 5 degrees about both axes, mats turned to several angles, full picture;
- layouts: mat turned, sheet filling 70, 30 and 10 per cent of the picture, dark, bright, noisy, blurred, thick and thin lines mixed;
- distractors: a bundle of straight lines that is not a grid, a rectangle, slanted lines;
- refusals: no sheet, one direction, too small a piece, a bent sheet, and every acceptance line made impossible one at a time;
- pairs: two cameras over the same sheet, the answer the same for every way the mat lies and every numbering;
- isolation: the result types do not convert to a pose, an orientation or a surface;
- speed: a full-size picture, with the thread count varied.

## Not done in this stage

No screen, no capture, no composite, no recording format, no estimate of the lens from the grid. A picture's resolution is not turned into settings automatically; a caller that uses a smaller picture (the live view) has to give settings that match it.
