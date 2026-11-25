# Grid sheet tilt (#440 stage 2)

Status: library and contract tests only. Nothing in the preview window, the capture path, the composite or the registration calls it yet.

## Input

One original picture per camera, decoded in stored pixel order, at the full size of the pictures the camera's rig profile was calibrated on. The sheet itself (the cutting mat) is what the picture shows; a sheet of paper over the middle of it is measured from the mat around it, as long as the share of the picture that holds grid points is at least `min_visible_fraction` of `WorkingDraftSettings()` (a fifth). The share is counted in picture cells, not in area of the sheet: a mat that fills the picture is measured with a good part of it hidden, while a drawing or a booklet that hides the mat so far that less than that share of the picture still holds grid points is outside what this code is made for (refused as `view-too-small`).

The caller passes the profile as `GridSheetProfile`: the projection (intrinsics, distortion, document matrix) and the profile's picture width and height (its sensor size). A picture of any other size is refused with `picture-not-profile-size` before anything is detected: the pixel numbers of a projection hold for that size only, and a reduced picture with a full-size projection would put the principal point and the focal length in the wrong place and roughly double the tilt. Live view frames and reduced copies are therefore not measured; there is no rescaling. A profile whose principal point does not lie in the middle half of its own declared size is a programming error (`std::invalid_argument`).

The detection numbers assume that full size (a grid pitch of roughly 50 to 120 pixels).

## What it measures

From one picture of a flat square-grid sheet it reports, for one camera:

- the tilt: the angle between the camera's optical axis and the sheet's normal;
- the lean: the direction the lens points on the sheet, as an angle times a unit direction, in three frames (the sheet's own axes, the picture's axes with x right and y down, and the drawing's axes);
- how it got there (`GridSheetEvidence`): the number of ridge points, the number used and their share of the ridge points, lines and intersections used, whether the fit settled, the residual (rms and largest), the share of the picture that holds grid points, the pitch of the first lattice, the lattice's step ratio and skew on the sheet, three tilts (lattice fit, line intersections, vanishing points) and their largest vector distance, a formal standard deviation of the tilt, the spread between halves of the picture, and the share of picture cells that hold a coherent grid which is not on the fitted lattice.

With the value it returns the lattice behind it: the last fit's lattice-to-plane matrix (lattice positions in grid pitches to the normalised plane of the profile's lens without distortion, as fitted in all eight numbers, not forced to a square sheet). The numbering of the lattice is the fit's own, fixed up to whole pitches and quarter turns and never a mirror image. The tilt, the lean and the azimuth are `OrientationOf` of that matrix. A refusal carries no matrix. The largest distance of a used point to its lattice line is in the evidence next to the rms, in the same unit, and a refusal by the residual line keeps both.

For two cameras that see the same sheet it also reports the angle between the two optical axes, the difference of their leans in the reference camera's drawing frame, the turn of their picture axes against each other and the rotation mismatch against the calibration.

The reference is the sheet, not gravity. A sheet that lies tilted on the table is measured as it lies.

## What it does not measure

- Any pass or fail. No value in this code says what tilt is acceptable, and the result carries no such word or colour.
- The lens. The intrinsics and the distortion of the given rig profile projection are used as they are and never re-estimated. A focal length that is off by a fraction scales the tilt by about the same fraction; a principal point that is off moves the lean a little.
- The camera's height or position, or the sheet's real pitch. The grid is assumed square and flat; its pitch cancels out. A lattice that does not come out square is refused.
- Whether two pictures show the same sheet. The caller has to know; a large rotation mismatch is refused.
- How accurate it is on real pictures. The contract tests are drawn pictures; the accuracy on the rig's own pictures is not established by this stage.

## Stages

1. Ridge points: thin bright lines are found per pixel from the second derivatives of the picture (Gaussian derivative kernels, the line's centre from the zero of the first derivative along the normal). The normal is taken from the form of the eigenvector that does not cancel, so a line along an image axis is not mistaken for one across it. Banded, optionally multi-threaded; the points do not depend on the thread count or the band size. An exception in a worker is passed to the caller after every thread has stopped.
2. Points without lens distortion: each ridge point and its line direction go to the undistorted normalised plane with the profile's own Brown-Conrady model (the model of `render::TryProjectDocument`).
3. First lattice: the picture is scored in windows for how periodic its lines are in two directions; the best window gives two directions, two pitches and two phases.
4. Lattice fit: a projective map from the plane to the lattice is fitted to all points (distance to the nearest integer line of the point's family, a point counted only when its own line direction is the family's, robust weights), growing the fitted window and shrinking the gate. The last step of the last gate must move no point's line by more than `fit_settle_px` (the robust weights and the assignment are taken afresh at every step, so a fit can keep stepping by a few hundredths of a pixel without going anywhere; one that still moves by more has not settled); otherwise the measurement is refused as `fit-failed`. How far that step moved is in the evidence.
5. Shape: for a square grid the lattice-to-plane matrix's first two columns (normalised camera units) are two equal, orthogonal sheet directions seen by the camera, whatever the pose. Their length ratio and their angle are the evidence that each line family was locked to the real pitch; a family that locked to another divisor of the pitch (a pitch outside the assumed range) gives a ratio such as 3:2 and is refused as `grid-not-square`.
6. Tilt: `content_registration::OrientationOf` on a projection that has the profile's intrinsics and distortion and the lattice matrix as its document matrix. Tilt, direction and turn are defined there once. The two other ways (line intersections fitted to a lattice; the two vanishing points) are computed from the same points as a cross-check. They share the points and the distortion, so their agreement is not an accuracy.
7. Uncertainty evidence: the formal standard deviation of the tilt from the fit's normal equations (every point taken as independent, which points along one line are not: a lower bound), and the lean refitted to the left and right halves and to the top and bottom halves of the used points. No line is set on either here.
8. Two cameras: the numbering of a grid is fixed only up to a multiple of 90 degrees. The multiple is the one that brings the turn of the target's picture axes closest to the turn the two calibrated projections have against each other. What is left is the rotation mismatch. Two cameras over the same sheet that have not turned about their axes since calibration give a mismatch near zero; a different sheet, a sheet seen in two turned pieces or a camera turned since calibration gives a larger one, and the difference of the leans would be off by about the lean times the mismatch in radians. A mismatch above the caller's line is refused as `turn-mismatch`; a line below 45 degrees also makes the quarter turn unique.

## Refusals

A measurement either returns a value or a refusal with a reason (`GridSheetRefusal`): the picture is not the profile's size; too few ridge points; no grid found (no window shows two periodic line families, which includes lines in one direction only); the fit failed, did not settle or its geometry is degenerate; too few lines; the lattice covers too few lines; the grid fills too little of the picture; the grid is not square; the residual is too large; the flatness could not be judged (no part of the picture holds enough points); the sheet is not flat (another part of the picture holds a coherent grid that is not on the fitted lattice, as a bent or curved sheet gives); the three ways to the tilt disagree. The evidence is filled as far as the run got, also for a refusal. Every comparison against a line is written so that a value that was not reached refuses. A pair is refused when either camera was not measured or the rotation mismatch is above the line.

The lines behind these reasons are the caller's (`GridSheetAcceptance`, `GridSheetPairAcceptance`). `WorkingDraftSettings()` is the one place that holds starting values; they are working numbers for development, not precision lines, and they only decide when the code gives up. The method spread line is a disagreement between three computations on the same points, not a tolerance on the tilt. The residual is the rms over the points inside the final gate, which stays near the gate over the square root of three even when those points lie anywhere in it; a residual line at or above that would never refuse and is rejected as a programming error. The detection numbers (`GridSheetDetectionSettings`) are working numbers too: the assumed range of the grid pitch in pixels, window sizes, gates and point counts are in picture pixels of the profile's full size.

The share of the ridge points that were used (`used_point_fraction`) is reported and not judged here: a drawing on the sheet lowers it, and whether and where to draw a line on it is not decided in this stage.

## Invariants

- The result types are measurement values for a person to read. They convert to nothing in the registration, the composite, the confirmed position, the capture gate, a rig profile or a fixed composite condition, and no function of those accepts them. Only the targets named in `A0_GRID_SHEET_TILT_ALLOWED_USERS` (the library and its contract test) may link the library; the configure step checks every target of the top-level directory against that list at its end and fails when any other target reaches it, whether it is defined before or after the library.
- Camera, file, clock and OpenCV free. A camera is a name and its profile; no camera name is built in.
- A programming error (an empty picture, a profile without a focal length or a picture size, a principal point outside the middle half of that size, unset settings, a line that is not a number) throws `std::invalid_argument`. What the picture shows is never an exception.
- The tilt, direction and turn are `OrientationOf`'s. The drawing frame assumes the camera turned about its axis as at calibration.

## Tests

`tests/grid_sheet_tilt_tests.cpp` (CTest `m2_grid_sheet_tilt_contracts`, label `slow`; sections can be run one by one by name). The pictures are drawn by `tests/grid_sheet_scene.hpp`, which shares no code with the product: it renders a sheet from a known pose through a lens with distortion (the lens inverse, the ray and plane intersection, the line profile and the truth are its own). No real picture is used.

- lens: the stages' distortion model against `render::TryProjectDocument`;
- ridge: line positions to a fraction of a pixel at several angles and offsets, including lines exactly and nearly along an image axis, noise free and faint; the same points for any thread count, band size, pixel format or stride;
- jacobian: the analytic Jacobian of the lattice fit against central differences;
- assign: a point counts for a family only when its line direction is the family's;
- shape: the step ratio and skew of a square, a stretched and a sheared lattice;
- fit: a known lattice with a quarter of the points on one side 1.5 pixels off their lines is fitted without being pulled, and settles;
- decomposition: the lattice matrix of a known pose, no picture: tilt, lean and azimuth against the geometry; a mirrored lattice is refused;
- accuracy: known tilts of 0 to 5 degrees about both axes, mats turned to several angles, cameras turned about their axes by 180, 90, 37 and -63 degrees (so that the turn into the drawing frame is checked in both directions), with the evidence of each value;
- layouts: mat turned, sheet filling 70, 30 and 10 per cent of the picture, dark, bright, noisy, blurred, thick and thin lines mixed;
- distractors: a bundle of straight lines that is not a grid, a rectangle, slanted lines;
- refusals: no sheet, one direction, too small a piece, a bent sheet (with room above the flatness line), a rectangular grid, a reduced picture with the full-size profile, every acceptance line made impossible one at a time (a refusal by the residual line keeps the rms and the largest residual), a flatness that cannot be judged, and the programming errors;
- pairs: two cameras over the same sheet, the answer the same for every way the mat lies and every numbering; one camera's mat turned by 1.5 to 40 degrees, compared below the mismatch line and refused above it;
- mapping: the returned lattice map is the last fit's matrix and the evidence's largest residual is the largest distance of a used point (both recomputed from the stages, bit for bit); the map gives the reported tilt, lean and azimuth back bit for bit; the map takes lattice points to the true lattice points of the drawn sheet (three poses, mats and mounts, the fit's numbering a quarter turn of the drawn one); a refusal has no map;
- visible: a sheet of paper hiding the middle of the mat, drawn at five sizes: a share of the picture that holds grid points above the working line is measured within the tolerances of the tests above, a share below it is refused as `view-too-small`, and the line is the working draft's own;
- paper: `WorkingDraftSettings()` itself on full-size pictures with part of the mat hidden, a sheet of paper in the middle and a piece of the mat in one corner: measured within the tolerances of the tests above;
- isolation: the result types do not convert to a pose, an orientation, a relative orientation, a projection or a surface;
- defaults: `WorkingDraftSettings()` itself on full-size pictures: a clean sheet is measured, a bent sheet is refused, and a sheet under a dense or a wide bundle of lines or at a pitch above the assumed range is refused or measured within 0.05 degrees;
- speed: a full-size picture, with the thread count varied.

## Not done in this stage

No screen, no capture, no composite, no recording format, no estimate of the lens from the grid, no live view. The lines on the formal standard deviation, the half split and the share of used points are left to the stage that shows the value.
