# Dual Binding Camera Agent v1 — session-local operator binding protocol

Status: Protocol, host loop and fake SDK adapter implemented / no launcher executable, no real SDK adapter
Schema: `a0.camera-agent.hardware-dual-binding.v1`
Decision: ADR-0025 · Issue: #61 (core: #9)

This is the wire protocol that makes the session binding core
(`dual_identity_session_binding.hpp`, Issue #9) reachable from another process.
The core decides what a valid binding is. This layer carries requests to it,
drives the SDK candidate/Live View lifecycle around it, and decides what is
allowed back out.

## Why it is a separate protocol

`a0.camera-agent.hardware-dual.v2` is stable and ADR-0025 leaves it unchanged,
so binding got its own schema, its own marker and its own pipe instead of five
more v2 operations.

The marker differs from v2's `Hardware` on purpose. A v2 request can therefore
never be accepted here by accident, and a binding request can never be accepted
by the v2 host, without either dispatcher having to know the other exists.
Contract tests assert both directions.

## The five operations

| Operation | Payload | On success |
| --- | --- | --- |
| `begin-binding` | `cameraMode` | `sessionId`, `state`, `candidateOrdinals` |
| `start-candidate-live-view` | `sessionId`, `candidateOrdinal` | `liveViewActive` |
| `get-candidate-live-view-frame` | `sessionId`, `candidateOrdinal` | `frameBytes`, `frameBase64` |
| `confirm-alias` | `sessionId`, `candidateOrdinal`, `cameraAlias` | `cameraAlias`, `state` |
| `complete-binding` | `sessionId`, `confirmedAtUtc` | `state`, `evidence[]` |

There is no `stop-candidate-live-view`. Stopping is not an operator decision, it
is a consequence of two decisions they already make: switching to the other body
stops the one they were looking at, and assigning an alias ends that body's Live
View and closes its SDK session. Exposing a sixth operation would let a client
reach a state — "assigned but still streaming" — that the binding core is
specifically built to refuse.

The envelope is the same shape as v2: `schemaVersion`, `simulation` (must be
`false`), `marker`, `requestId`, `operation`, `payload`, and every field must be
present exactly once with no extras. Responses echo `requestId`; session-scoped
responses echo `sessionId`.

## Operator flow

```text
begin-binding                       -> sessionId, candidates [0,1]
  start-candidate-live-view 0
  get-candidate-live-view-frame 0   -> operator looks at body 0
  start-candidate-live-view 1       -> body 0 stops, body 1 starts
  get-candidate-live-view-frame 1   -> operator looks at body 1
  confirm-alias 1 CAM-A             -> body 1 stops, its SDK session closes
  start-candidate-live-view 0
  confirm-alias 0 CAM-B             -> body 0 stops, its SDK session closes
complete-binding                    -> Ready, evidence published
```

Capture then reaches the bound source object through
`BoundSourceObjectForCapture`, which is a process-local seam and never appears
on the wire.

## What the wire is allowed to carry

Published evidence is exactly the ADR-0025 allowlist, straight from the core:
`cameraAlias`, `providerId`, `providerVersion`, `confirmedAtUtc`,
`invalidationReason`.

Excluded, and asserted excluded by contract test on every one of the five
responses: SDK source-object tokens, serials, raw identifiers, and preview
frames. Candidate ordinals appear on the wire — the operator has to be able to
say "the one I am looking at now" — but only as session-local selectors: they
are meaningless once the session ends and never appear in evidence.

Nothing is persisted. Not the source object, not the ordinal, not the frame, not
the enumeration order. This is structural rather than policed by a counter: the
implementing translation unit depends on no filesystem, registry or store type,
so there is no call it could make to write one.

## Live View frames

One Live View at a time, enforced at the protocol layer. The fake SDK adapter
independently refuses an overlapping start and counts the violation, so a test
can prove the rule is enforced above it rather than merely intended.

Frames are capped at 256 KiB raw (`kMaximumBindingLiveViewFrameBytes`), which is
roughly 341 KB of base64 and leaves the 1 MiB pipe frame comfortable room for
the envelope. A larger frame is refused, never truncated: the operator decides
which body they are looking at from that image, and half an image is a wrong
answer waiting to happen. A contract test sends a frame at exactly the bound and
asserts the whole response still fits one pipe frame, so the cap cannot quietly
become decorative.

## Session correlation and invalidation

A session id is 32 hex characters: 12 random bytes fixed per dispatcher plus a
per-dispatcher counter. The randomness is not a secret — the id is a correlation
token, not a capability — it is there so a client cannot guess the next id and so
a restarted agent never reproduces an old one.

Session-scoped requests are checked for identity **before** the adapter is
polled. A request naming some other session can therefore neither learn the live
session's state nor consume the invalidation event the live session's next
request needs to see.

Invalidation is polled and reported typed, using the core's reasons:
`AgentRestart`, `UsbReconnect`, `CameraCountChanged`, `TopologyChanged`,
`SdkManagerRecreated`, `SdkError`. The protocol is request/response with no
server push, so the caller learns about an event at the first moment it could
act on one, and never gets a success built on a binding that stopped being
trustworthy in between.

Three cases are deliberately distinguished:

- **Queued before `begin-binding`** — drained and discarded. It describes the
  session that `begin-binding` just threw away. Acting on it would refuse the
  replacement session for something that happened before it existed, and since
  the drain is what clears it, the operator would loop forever.
- **Raised during candidate collection** — the new session is refused with
  `BindingInvalidated`, because the candidates just collected may already be
  stale. The retry starts clean.
- **Raised against a live session** — every subsequent operation returns
  `BindingInvalidated` with the first reason, which is the one that explains why
  the binding stopped being trustworthy.

A restarted agent is the strongest form: it holds no session at all, so every id
a client still remembers is a `SessionMismatch`, whatever that id is.

### There is no per-session expiry

A session ends when the host's absolute lifetime runs out, when a newer session
replaces it, or when it is invalidated. There is deliberately no separate TTL.

The risk a TTL would address — an operator viewing a Live View, walking away, and
confirming an alias much later for a body that has since been swapped — is
already covered: swapping a body cannot happen without a USB or topology event,
and both invalidate the session immediately. A TTL would be a second mechanism
for a risk the first one already handles, and the untested one of the two is
whichever fires second.

## Quiesce is the binding core's decision

`confirm-alias` stops the Live View and closes the SDK session, then reports
**what the SDK actually said** to `ConfirmCandidateQuiesced`. If either failed,
the response is `QuiesceIncomplete` and names which half failed, and the core
refuses `complete-binding` with `CandidateNotQuiesced`.

The agent does not maintain a second copy of that rule. An earlier draft
invalidated the session itself on a failed quiesce; that made the honest report
to the core dead weight, which a mutation check caught — hardcoding
`ConfirmCandidateQuiesced(ordinal, true, true)` broke no test. One mechanism,
tested.

The one place the agent does invalidate on its own is a Live View that will not
stop during a **switch**. No core mechanism covers that case: the session can no
longer guarantee one Live View at a time, and continuing would mean two bodies
streaming at once.

## Transport

The binding host shares the accept/serve loop that already serves the
Single-camera and Dual v2 hosts (`hardware_camera_agent_pipe.cpp`) via
`RunDualBindingCameraAgentNamedPipeServer`. It is a wrapper, not a copy: a
second copy of that loop is how the hosts would drift into disagreeing about
what an oversize frame or a missing acknowledgment means.

Inherited unchanged, and re-asserted for this host by contract test:

- 4-byte little-endian length prefix + UTF-8 JSON body, capped at 1 MiB
- current-logon pipe security descriptor, remote clients rejected
- bounded one-byte delivery acknowledgment; a missing one is exit code 3
  (dispatched, delivery failed), distinct from exit code 2 (never dispatched)
- zero length, partial header, partial body and early disconnect all fail closed
  before dispatch — and, specifically for binding, before any candidate is
  enumerated or any session is created
- a fixed absolute lifetime from launch, never extended by use

Default pipe name: `A0CameraStitcher.CameraAgent.HardwareDualBinding.v1`. As
with the other hosts, per-session pipe-name uniqueness is the launcher's
responsibility.

## What is deliberately not here

**No launcher executable.** There is no `A0CameraStitcher.DualBindingAgent.exe`.
The only adapter that exists is the fake one, so shipping a launcher would ship a
product process whose entire behavior is a simulation of binding two cameras —
precisely the "Agent product execution" this Issue's safety boundary excludes.
The host loop is implemented and tested; the executable arrives with the real
adapter.

**No real SDK adapter.** `DualBindingSdkAdapter` is the seam; the Nikon
implementation is Issue #10. That the whole flow, including every rejection
path, is reachable without a camera is the point of the seam, not a gap in it.

**No WPF UI.** The confirmation screen is Issue #62.

## Test coverage

| Suite | What it covers |
| --- | --- |
| `dual_binding_camera_agent_contracts` | Envelope, all five operations, candidate cardinality 0/1/2/3, duplicate and empty source objects, duplicate alias and duplicate candidate, bounded frames, quiesce failures, session mismatch, restart, all six typed invalidations, evidence allowlist |
| `dual_binding_camera_agent_pipe_contracts` | Full binding across separate connections, restarted host refusing the previous session, 1 MiB boundary, zero-length/partial/disconnect fail-closed, missing acknowledgment |
| `dual_hardware_camera_agent_contracts`, `dual_hardware_camera_agent_pipe_contracts` | v2 non-regression — unchanged by this work |
