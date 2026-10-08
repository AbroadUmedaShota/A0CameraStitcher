# Public test fixtures

Only rights-cleared synthetic charts may be committed here. Do not add customer originals, camera serials, or unreviewed photographs.

`a0-synthetic-chart.svg` is a wholly synthetic calibration target. Its grid, fiducials, grayscale and colour patches are generated artwork; it contains no camera capture, customer original, or third-party chart asset.

`pilot-chart` contains two self-authored A0 print masters derived from `a0-synthetic-chart.svg` for the pilot captures of Issue #276: `a0-pilot-chart-development-v1.svg` (chart ID `A0CS-PILOT-DEV`, development split) and `a0-pilot-chart-holdout-v1.svg` (chart ID `A0CS-PILOT-HOLD`, locked-holdout split). The two use different fiducial, grid, circle, edge, line and patch layouts so that one printed master never serves two splits. Their hashes and fiducial lists are recorded in `corpus-contracts/vector-specs/pilot-chart-*-v1.json`. Printing PDFs are derived files and are not committed; see `docs/PILOT_CHART.md`.

`rig-profile.draft.example.json` is a pre-approval fixture, not an approved optical rig. Its final layout, DPI, crop, calibration provenance/validity, automatic-correction envelope, and quality thresholds intentionally remain `null`. Neither file is evidence of imaging quality, calibration accuracy, colour fidelity, A0 coverage, or production readiness.

`rig-profile.v2.draft.example.json` is the all-null template of the proposed rig profile 2.0.0 (`docs/schemas/rig-profile.v2.schema.json`, ADR-0034, Proposed). Its document plane, lens and projection values, calibration provenance, output raster, correction envelope, quality thresholds and approval record are all `null`. It contains no rig measurement, lens coefficient, DPI or threshold, and it is not an approved or calibrated profile.

`m2-fixtures` contains tiny JSON pixel pairs with exact overlap metrics. The perfect pair and two deliberately damaged variants are wholly synthetic and marked `test-only` / `not-evaluated`; they define deterministic software-test oracles, not acceptable quality thresholds.

`corpus-contracts` contains self-authored vector specifications and metadata-only examples for rights provenance, anonymous split manifests, and an implementation-independent contract oracle. It contains no photographed image or camera identity. The examples are `contract-example` / `not-evaluated`; real D810 corpus blobs must stay in an approved external store.
