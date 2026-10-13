# Calibration observation session fixtures

All data here is hand-written and synthetic. No file was produced from a camera, a chart print or a real image.

- Image hashes are a single hexadecimal digit repeated 64 times. They are not hashes of any real image.
- Image sizes (64 x 48) are small values for arithmetic checks. They are not the size of any real camera.
- Labels (`synthetic-lens`, `f50mm`, `fixed-a`) are made up.

| File | Role |
|---|---|
| `valid-minimal.json` | A session the reader accepts and the schema validates. Six chart points; each camera has one training, one validation and one reference view. CAM-B's training view holds one `missing` and one `ambiguous` observation with a null pixel. The diagnostics for it are empty. |
| `negative-cases.json` | Changes applied to `valid-minimal.json`, one structural violation per case. The C++ contract test and the PowerShell schema test both read this table. `category` is the rejection category the reader must report, and `pointer` is the JSON Pointer the reader's message must name (the C++ test compares the whole message, so a case that is rejected for another reason elsewhere is caught). `schema` says what the JSON Schema does with the changed document: `reject`, `accept` (only the reader can reject it), or `skip` (PowerShell cannot keep the number literal, so only the C++ test uses the case). |
| `invalid-generator-ground-truth.json` | A hand-written document in the shape of the synthetic pair generator's ground truth. The reader must reject it as a truth field, and the schema must reject it. |

The operations in `negative-cases.json` are `add`, `replace` and `remove` on JSON Pointers into `valid-minimal.json`. For `add` into an array, the index `-` appends.

Contract: `docs/M2_CALIBRATION_OBSERVATION_SESSION.md`. Schema: `docs/schemas/calibration-observation-session.v1.schema.json`.
