# Hardware Camera Agent v2 — continuous Live View contract

Status: software implemented / real D810 acceptance unverified
Schema: `a0.camera-agent.hardware.v2`

This protocol is a separate, long-lived Live View contract. It does not change
the durable capture and recovery schema in `hardware.v1`, and a finite v1 probe
is never reported as a continuous session.

## Ownership and lifetime

- The WPF hardware window starts one randomly named local Camera Agent process.
- One 32-character lowercase hexadecimal `sessionId` owns CAM-A Live View.
- The process holds the operator-session camera lease and one SDK session while
  Live View is running. It never overlaps WPD.
- A request/response gate permits only one outstanding frame or control request,
  providing backpressure.
- Every response uses the v1 framing and bounded `0x06` delivery-ACK contract;
  see `HARDWARE_CAMERA_AGENT_V1.md`.
- Frame activity or `live-view-heartbeat` refreshes a 20-second idle deadline.
  The native process also enforces a 600-second maximum lifetime.
- `close-agent-session` orders shutdown. The app never force-kills an agent after
  an ambiguous capture dispatch; durable v1 recovery remains authoritative.

## Operations

| Operation | Required payload | Success state |
| --- | --- | --- |
| `start-live-view` | `cameraAlias=CAM-A`, `sessionId`, `exclusiveCameraControlConfirmed=true` | `Started` |
| `read-live-view-frame` | `sessionId` | `Frame` |
| `live-view-heartbeat` | `sessionId` | `Heartbeat` |
| `stop-live-view` | `sessionId` | `Stopped` |
| `close-agent-session` | `sessionId` | `Closed` |

Every response is `simulation:false`, marker `Hardware`, and carries
`cameraMode:SingleCamera`, CAM-A, the correlated session ID, SDK/Live View state,
heartbeat/max-lifetime values, and redacted error fields. Frame responses contain
at most 512 KiB of canonical-base64 JPEG bytes plus size and lowercase SHA-256.
The .NET client decodes base64, re-encodes it, requires an ordinal byte-for-byte
match with the received string, then rechecks size, hash, and JPEG markers before
WPF decoding. Whitespace, line breaks, extra padding, and other non-canonical
base64 representations are rejected.
Frames remain memory-only and always set `previewIsOriginal:false` and
`previewIsStitchInput:false`.

## Capture handoff

The WPF workflow explicitly stops v2 and verifies both Live View and the SDK
session are closed before it persists and dispatches the existing v1 capture.
It sends v1 `liveViewHandoffRequested:false`; v2 is not relabelled as the older
finite-probe handoff. A failed or ambiguous stop sends zero capture requests.
Only a verified terminal `CaptureComplete` causes a new v2 session to start.
Failed, partial, reserved, in-progress, disconnected, or invalid capture results
remain stopped and are never retried automatically.

## Opt-in ten-handoff acceptance evidence

The hardware Single window enables the observation-only collector only when it
is started with both `--single-handoff-acceptance-count 10` and
`--source-sha <40-lower-hex>`. Ordinary launcher and hardware operation do not
create an acceptance run. The collector writes
`a0.hardware-single-handoff-acceptance.v1` under the fixed-local
`%LOCALAPPDATA%\A0CameraStitcher\hardware-single\handoff-evidence` root.

One accepted handoff requires at least two ordered verified frames before the
capture, a confirmed v2 stop/SDK close, one v1 dispatch with retry count zero,
the time-ordered Agent trace from WPD baseline through SDK capture, canonical
original verification, exact delete, empty-after and WPD recovery close, then
at least two ordered frames from a new v2 session and its explicit final stop.
The run becomes `Complete` only when it is sealed after exactly
requested/attempted/completed 10, failures 0, and the tenth restarted session
is stopped. Early close is
`Incomplete`; missing, corrupt, duplicate, foreign-session, late or out-of-order
observations are `Invalid`; a typed operation failure is `FailedPartial`.

Observation is queued off the UI/camera path. Persistence or parsing failure
cannot dispatch, retry, delete, change a timeout, or alter the existing stop and
capture order. It only prevents an acceptance Pass. Output is an allowlist of
the schema/scope, supplied source SHA, opaque run/session/transaction IDs, UTC
timestamps, state/count/boolean results and normalized failure categories. It
does not copy preview/image data, hashes from preview, serials, SDK/WPD raw
identifiers, profile values, local/user paths, or exception details.

## Verification boundary

Native contract tests drive the production backend through a narrow injected SDK
transport and monotonic clock without loading or calling a camera. They cover the
512 KiB frame boundary, session takeover and active double-start rejection,
heartbeat expiry, serialized backpressure, stop/close failure, and capture refusal
while cleanup remains unsafe. Real named-pipe tests accept 1 MiB minus one byte and
exactly 1 MiB, and reject 1 MiB plus one byte before dispatch. Parsed v2 schema
correlation is retained for rejection envelopes despite whitespace, field order,
unknown payload fields, or duplicate payload fields; no raw substring detection is
used. Foundation tests cover strict JSON, canonical base64, and JPEG validation.
Operator tests cover in-memory frame display, stop-before-capture ordering, v1
handoff=false, success-only restart, zero capture when stop is unconfirmed, and
the opt-in collector's exact 10/10, partial, persistence-failure, missing,
duplicate, foreign-session, out-of-order and post-delete failure results.
These are software-only tests: no actual D810 Live View, WPF interaction,
empty-card capture, or ten-handoff acceptance is claimed.
