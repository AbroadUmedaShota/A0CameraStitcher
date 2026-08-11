# Hardware Camera Agent v1

The hardware Camera Agent is the product boundary by which the WPF application
uses the existing Phase 0 C++ executor for one-camera operation. It runs the
executor in-process; it does not spawn the legacy Phase 0 CLI. Its protocol and
types are deliberately separate from the M3 simulation protocol.

This boundary is fail-closed. It does not relax the one-D810 cardinality,
redacted-alias binding, operator-session lease, SDK/WPD non-overlap, empty-card
spool, exact-object delete, 180-second deadline, or no-retry contracts.

## Process and named-pipe transport

Build target: `A0CameraStitcher.CameraAgent.exe`.

The WPF launcher generates a new unpredictable ASCII pipe name for each
operation, passes the same operator-approved local capture profile used for readiness and
capture, and starts:

```powershell
.\A0CameraStitcher.CameraAgent.exe `
  --serve-once `
  --pipe-name A0CameraStitcher.CameraAgent.Hardware.v1.<random-guid> `
  --approved-capture-profile C:\approved-local-path\single-profile.json `
  --single-identity-v3 C:\approved-local-path\single-identity-v3.json
```

The initial product SingleCamera path requires `--single-identity-v3`. Its
strict schema stores only `selectedAlias:"CAM-A"`, a lowercase WPD serial
digest, and `sdkSelectionPolicy:"exactly-one-current-session"`. The colliding
SDK Source Name/Interface projection is never persisted. Missing, malformed,
CAM-B, digest mismatch, or SDK/WPD cardinality other than exactly one fails
before camera open. This identity is not DualCamera binding evidence.

Legacy/test-only optional path arguments are `--camera-map SDK_MAP`, `--wpd-camera-map WPD_MAP`,
`--artifacts-root PATH`, `--reports-root PATH`, and
`--transaction-state-root PATH`. If `--camera-map` is supplied without an
explicit WPD map, `name.ext` derives `name-wpd.ext`. The two strict product maps
must each contain exactly the `CAM-A` and `CAM-B` keys (a value may be `null`),
must not duplicate stable identities, and must bind the one current SDK and WPD
projection to the selected redacted alias. Map and profile inputs must be
bounded regular files on an absolute, drive-qualified, fixed local Windows
volume with a reparse-free path chain. UNC, device-path, alternate-data-stream,
mapped-network, removable, optical, RAM-drive, and relative paths are rejected.
Artifact, report, and transaction-state roots use the same fixed-local policy.

## DualCamera identity software contract

DualCamera does not reuse SingleCamera identity-v3 and no current Nikon SDK
property is presented as a stable per-body identity. The production library
therefore exposes a software-only extension seam for a future documented
SDK/WPD correlation provider. No provider is installed or approved by this
change, so the operational DualCamera lane remains
`Blocked/identity_strategy_unresolved` and `HG-0003B` remains open.

The seam accepts exactly two operator-created, local-only proof files, one for
`CAM-A` and one for `CAM-B`. Each file must have the exact schema
`a0.camera-agent.dual-identity-binding-proof.v1`, `bindingVersion:1`,
`cameraMode:"DualCamera"`, a bounded provider ID/version, separate anonymous
lowercase SHA-256 projections for SDK and WPD, a strict UTC creation/expiry
window, and both `singleCameraConnectedConfirmed:true` and
`documentedCorrelationConfirmed:true`. `proofPayloadSha256` detects a changed
proof payload; it is an integrity check, not a signature or a substitute for
operator-controlled local storage. Unknown, missing, duplicate, trailing,
wrongly typed, stale, or digest-mismatched content is rejected.

After a future provider performs non-overlapping read-only SDK and WPD
inventory, the verifier returns the typed `DualIdentityReady` variant only
when all of the following hold:

- the provider explicitly declares documented stable same-body correlation and
  its ID/version matches both proofs and every current projection;
- SDK and WPD each contain exactly two Nikon D810 projections;
- `CAM-A` and `CAM-B` each match exactly once in each transport, with zero
  unbound, duplicate, cross-alias collision, or cross-transport matches;
- both proofs are structurally valid, unexpired, untampered, and carry the
  required one-body-at-a-time operator confirmations.

Every failure returns typed `DualIdentityBlocked` with a specific category,
including `identity_strategy_unresolved`, `legacy_map_fallback_prohibited`,
`proof_invalid`, `proof_tampered`, `proof_stale`, `provider_mismatch`,
`camera_count_mismatch`, `missing_identity`, `duplicate_identity`,
`identity_collision`, `mismatched_transport`, `unbound_identity`, and
`alias_cardinality_mismatch`. Both Ready and Block variants freeze the safety
facts at read-only inventory, capture/card access/Live View/settings write/
delete/format/vendor operation all false, and automatic retry count zero.

The legacy identity-v2 map counter can still produce anonymous diagnostics,
but it can no longer produce `Ready`; even exact CAM-A/B counts finish as
`Blocked/identity_strategy_unresolved`. This prevents `verify-dual-spools` and
pair capture entry points from treating enumeration order, USB port, ephemeral
Source ID, or the colliding SDK Name/Interface digest as DualCamera evidence.

`--serve-once` accepts one connection and one complete request, completes the
operation, attempts one response, then exits. Accept is bounded to 15 seconds;
the request header and body each have a 5-second absolute deadline, and the
response header and body each have a 1-second absolute deadline. A disconnect after a complete capture request does not
cancel the transaction: dispatch and the terminal journal commit happen before
the response write. The client must later call `get-transaction-result` with
the same `transactionId`; it must never resubmit that capture.

The server uses `FILE_FLAG_FIRST_PIPE_INSTANCE`, rejects remote clients, and
applies a protected DACL for the current interactive logon-session SID. The WPF
launcher additionally owns the random pipe name and validates that its child
agent exits successfully. Services and other Windows logon sessions are outside
the MVP contract and fail closed.

Each byte-mode pipe frame is:

1. a 4-byte unsigned little-endian UTF-8 JSON byte count;
2. exactly that many UTF-8 JSON bytes.

The transport maximum is 1 MiB; the hardware JSON contract maximum is 64 KiB.

## Envelope

Every request has exactly these root fields:

```json
{
  "schemaVersion": "a0.camera-agent.hardware.v1",
  "simulation": false,
  "marker": "Hardware",
  "requestId": "request-001",
  "operation": "get-single-readiness",
  "payload": { "cameraAlias": "CAM-A" }
}
```

Every response repeats `schemaVersion`, `simulation:false`, `marker`, and
`requestId`, then adds `success`, `resultCode`, and `payload`. Unknown, missing,
duplicate, trailing, wrongly typed, or structurally invalid fields are rejected
before backend access. Aliases are exactly `CAM-A` or `CAM-B`; capture
transaction IDs are exactly 32 hexadecimal characters. Every typed operation
payload carries `cameraMode:"SingleCamera"`. Capture and durable-query payloads
also carry `requiredCameraAlias`, exactly equal to their `cameraAlias`.

## Operator-approved Single profile

Readiness and capture require `--approved-capture-profile`. The WPF operator
explicitly approves the observed read-only CAM-A settings for 30 days; this
does not write camera settings. The file has exact
schema `a0.camera-agent.capture-profile.v1`:

```json
{
  "schemaVersion": "a0.camera-agent.capture-profile.v1",
  "profileId": "approved-single-profile",
  "profileVersion": 1,
  "selectedAlias": "CAM-A",
  "cameraMode": "SingleCamera",
  "approved": true,
  "approvedBy": "human approver",
  "approvalReference": "local approval record",
  "approvedAtUtc": "2026-08-01T00:00:00Z",
  "expiresAtUtc": "2026-12-31T23:59:59Z",
  "expectedSettings": {
    "fileType": {
      "available": true,
      "currentLabel": "JPEG"
    },
    "compressionLevel": {
      "available": true,
      "currentLabel": "Fine"
    }
  }
}
```

`expectedSettings` is a nonempty profile-selected subset of `fileType`,
`compressionLevel`, `imageSize`, `exposureMode`, `shutterSpeed`, `aperture`,
`sensitivity`, `whiteBalanceMode`, and `focusMode`. Each expectation may choose
a nonempty subset of `available`, `capType`, `probeState`, `valueType`,
`currentValue`, `currentIndex`, and `currentLabel`. The agent does not embed
unapproved numeric thresholds or require all nine fields. It hashes the exact
profile bytes and returns the ID, version, SHA-256, selected alias, and expiry.

The profile must be approved, internally time-consistent, applicable to the
selected alias, unexpired at dispatch, and still unexpired and matching a fresh
read-only settings probe inside the already-open SDK capture session immediately
before shutter. If it is missing, malformed, expired, changed, or mismatched,
capture is blocked with zero shutter commands.

## Operations

### `get-single-readiness`

Request payload:

```json
{ "cameraAlias": "CAM-A" }
```

The read-only result includes current SDK/WPD counts and alias-binding facts,
SDK status and spool inspection, `firmware`, `liveViewStatus`, the approved
profile ID/version/SHA/alias/expiry, and `captureProfileAliasMatches` plus
`settingsMatchApprovedProfile`. `observedSettings` contains all nine named
settings, each with this exact typed shape:

```json
{
  "available": true,
  "capType": "choice",
  "probeState": "read",
  "valueType": "unsigned",
  "currentValue": 1,
  "currentIndex": null,
  "currentLabel": "JPEG"
}
```

The result explicitly reports `readOnly:true`, `captureCommandSent:false`,
`cameraObjectDeleteAttempted:false`, `cameraSettingsChanged:false`, and
`realIdentifiersIncluded:false`. Before opening WPD to inspect the spool, the
SDK status capability must be available and report Live View exactly `off`.
`ready:true` requires exactly one current D810 on both transports, strict
binding agreement, confirmed Live View OFF, an empty dedicated spool, an
approved/non-expired/applicable profile, and the profile-selected setting
expectations to match. `resultCode` is `SingleReady` or `SingleNotReady`.

### `capture-single`

Request payload (all fields are required):

```json
{
  "transactionId": "0123456789abcdef0123456789abcdef",
  "cameraAlias": "CAM-A",
  "expectedCaptureProfileId": "approved-single-profile",
  "expectedCaptureProfileVersion": 1,
  "expectedCaptureProfileSha256": "64-lowercase-hex-characters",
  "expectedCaptureProfileExpiresAtUtc": "2026-12-31T23:59:59Z",
  "exclusiveCameraControlConfirmed": true,
  "dedicatedSpoolScopeConfirmed": true,
  "exactObjectDeleteConfirmed": true,
  "liveViewHandoffRequested": false
}
```

The four `expectedCaptureProfile*` values bind capture to the last readiness
snapshot. Any difference from the profile loaded and frozen for capture becomes
durable `Blocked/capture_profile_snapshot_mismatch` before camera access. All
three confirmations must be true.

The result includes `cameraMode`, `cameraAlias`, `requiredCameraAlias`, `runId`,
the fixed client `transactionId`, the frozen profile identity and expiry,
`terminalState`, `errorCategory`, `errorDetail`, cleanup facts,
`automaticRetryCount:0`, `transactionWatchdogSeconds:180`, and either
`retainedOriginal:null` or:

```json
{
  "cameraAlias": "CAM-A",
  "path": "C:\\...\\original.jpg",
  "sizeBytes": 12345678,
  "sha256": "64-lowercase-hex-characters"
}
```

Success requires `Complete`, one durable reread-verified canonical PC original,
empty spool before capture, exact-object delete attempted and successful, empty
spool after cleanup, zero retries, and no real identifiers. The absolute
180-second deadline starts before reservation/preflight and continues through
the existing hybrid executor. Live View OFF is revalidated before WPD and in
the open SDK capture session immediately before shutter. Current one-D810
cardinality is revalidated when the WPD and SDK sessions open; SDK and WPD
sessions never overlap.

If `liveViewHandoffRequested:true`, the result additionally snapshots:

- `liveViewStoppedBeforeCapture` and
  `liveViewSdkSessionClosedBeforeCapture`; capture cannot proceed if either is
  false;
- `postCaptureLiveViewProbeAttempted` and
  `postCaptureLiveViewProbeSucceeded`;
- nullable `postCapturePreview` with verified local JPEG path, size, and SHA.

The post-capture action is deliberately a finite one-frame probe which then
stops Live View and closes its SDK session. It is not a continuous resumed Live
View stream. A post-capture probe failure yields `FailedPartial`, preserves the
retained original, and never retries.

### `get-transaction-result`

Request payload:

```json
{ "transactionId": "0123456789abcdef0123456789abcdef" }
```

The response payload has the same typed shape as `capture-single`. Result codes include
`TransactionReserved`, `TransactionInProgress`, `TransactionNotFound`,
`CaptureComplete`, or the terminal error category. The journal freezes
`cameraMode:"SingleCamera"`, alias, profile ID/version/SHA/expiry, handoff
intent/outcome, original metadata, cleanup facts, deadline, and retry count.

Reserved and InProgress recovery is monotonic and protected by the same
transaction mutex used by capture. This includes the narrow interval after the
transaction directory is exclusively created but before its initial Reserved
journal is committed: a live owner yields `TransactionReserved`, never a false
terminal result. Only after the owner mutex can be acquired and the journal is
still absent is the reservation terminalized as
`FailedPartial/transaction_reservation_incomplete`. If a Reserved or InProgress
owner is gone, the agent does not retry: it safely recovers a canonical
original if present and commits the corresponding typed `FailedPartial` state.
Each transaction journal is bounded to 64 KiB before allocation and strict JSON
parsing; an oversized, duplicate-field, structurally inconsistent, or
out-of-scope journal fails closed and is never treated as capture success.
Every terminal original/preview is re-read from its exact non-reparse run path,
JPEG-checked, size/SHA-checked, and fail-closed on later loss or tampering.
Historical Complete journals remain queryable after the recorded profile expiry;
expiry is a shutter-time gate, not a reason to rewrite history.
Transaction recovery does not load or trust the current profile or identity-map
files. A later missing, malformed, nonlocal, or reparse-substituted policy file
therefore cannot prevent querying the already-durable transaction snapshot.

### `live-view-probe`

Request payload:

```json
{
  "cameraAlias": "CAM-A",
  "exclusiveCameraControlConfirmed": true,
  "liveViewFrames": 2,
  "liveViewIntervalMs": 100
}
```

Frames are bounded to 1..30 and the interval to 0..1000 ms. The agent acquires a
finite set of frames, stops Live View, closes the SDK session, then persists the
last valid frame as `preview.jpg.partial` followed by write-through atomic
`preview.jpg` rename and reread. The result carries frame/hash/duration and
stop/close evidence, plus nullable preview path/size/SHA. It always reports
`previewIsOriginal:false`, `previewIsStitchInput:false`, and no real IDs. This
operation verifies finite acquisition only; it is not a continuous display

This v1 operation is diagnostic only and does not satisfy product interactive
Live View. Continuous start/frame/stop, heartbeat, bounded lifetime,
backpressure, and same-session capture handoff belong to the separately
versioned hardware v2 contract and remain unimplemented at this checkpoint.
session.

## Durability and retry rules

- Protocol rejection: `success:false`, `resultCode:"ProtocolRejected"`, typed
  rejection payload, and no backend/camera access.
- A malformed backend response is `InvalidBackendResult`; the agent never wraps
  it in the Hardware marker as success.
- Readiness dispatch succeeds independently of its `ready` value.
- A transaction-specific durable reservation precedes camera access. The same
  transaction mutex is acquired before the exclusive reservation-directory
  creation and remains held through the terminal atomic journal commit; the
  operator-session-wide camera lease spans InProgress through that terminal
  commit.
- Canonical PC bytes use exclusive write-through creation, `FlushFileBuffers`,
  JPEG/size/SHA validation, write-through atomic rename, and final same-handle
  reread. The verified canonical file identity is then held open with writes and
  deletes denied through exact WPD-object deletion and empty-after cleanup, so a
  path replacement cannot authorize deletion while changing the sole PC
  original. Quarantine is contained under the exclusively-created run
  directory.
- There is no automatic retry, alias reassignment, bulk card deletion, card
  format, or vendor operation `0x9207` path in this boundary.

No command in the contract tests operates a real camera. Product hardware
claims still require the human-approved repeatable evidence in the Phase 0 test
plan.

The 2026-08-10 SDK-less and licensed-SDK-enabled Release builds and CTest each
completed 6/6, including `hardware_camera_agent_contracts`. These tests include
the gap-free initial reservation query, locked-original-through-delete, and
fixed-local root/map/profile negative contracts. This is software-only contract
evidence and sent no camera command: it does not verify a D810 shutter, WPD
recovery, an actual camera JPEG, the WPF hardware workflow, or `HG-0009`
acceptance.
