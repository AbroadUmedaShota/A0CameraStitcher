# Independent evaluator fixtures

Nine analytic fiducials have stitched sample centers at the Cartesian product
of {48,128,208} pixels. At 254 dpi and document origin zero,
`document_mm = (pixel + 0.5) / 10`. Both test programs draw their own analytic
image; no product renderer or synthetic generator computes its expectations.

`ground-truth.json` is an independently authored compatible `/2` annotation,
not claimed output of the generator. Its raw camera `image_px` values deliberately
differ from the stitched centers. The evaluator must use `document_mm` instead.
No real captures, camera identities or approved profiles appear here.

The threshold values are explicit test inputs, not approved optical limits.
Pristine decoded JPEG allows the declared 0.5 px position allowance; analytic
uncompressed symmetry is tested separately. Equal-content seam patches occupy
(112,224,8,8) and (144,224,8,8). Validity masks and lossy images are created
only in owned temporary test directories.
