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
The .NET client rechecks base64, size, hash, and JPEG markers before WPF decoding.
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

## Verification boundary

Native contract tests cover strict v2 envelopes, session correlation, start / one
verified frame / stop / close, and rejection of v1 capture under v2. Foundation
tests cover strict JSON and JPEG validation. Operator tests cover in-memory frame
display, stop-before-capture ordering, v1 handoff=false, success-only restart, and
zero capture when stop is unconfirmed. These are software-only tests: no actual
D810 Live View, WPF interaction, empty-card capture, or ten-handoff acceptance is
claimed.
