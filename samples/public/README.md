# Public test fixtures

Only rights-cleared synthetic charts may be committed here. Do not add customer originals, camera serials, or unreviewed photographs.

`a0-synthetic-chart.svg` is a wholly synthetic calibration target. Its grid, fiducials, grayscale and colour patches are generated artwork; it contains no camera capture, customer original, or third-party chart asset.

`rig-profile.draft.example.json` is a pre-approval fixture, not an approved optical rig. Its final layout, DPI, crop, calibration provenance/validity, automatic-correction envelope, and quality thresholds intentionally remain `null`. Neither file is evidence of imaging quality, calibration accuracy, colour fidelity, A0 coverage, or production readiness.

`m2-fixtures` contains tiny JSON pixel pairs with exact overlap metrics. The perfect pair and two deliberately damaged variants are wholly synthetic and marked `test-only` / `not-evaluated`; they define deterministic software-test oracles, not acceptable quality thresholds.

`corpus-contracts` contains self-authored vector specifications and metadata-only examples for rights provenance, anonymous split manifests, and an implementation-independent contract oracle. It contains no photographed image or camera identity. The examples are `contract-example` / `not-evaluated`; real D810 corpus blobs must stay in an approved external store.
