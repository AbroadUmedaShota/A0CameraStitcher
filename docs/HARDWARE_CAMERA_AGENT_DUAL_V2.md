# Dual Hardware Camera Agent v2 — Native Named Pipe host

Status: Native host implemented / controlled Module-retention exception approved; hardware evidence pending
Ordinary schema: `a0.camera-agent.hardware-dual.v2`
CaptureRecoveryOnly start/query/close schema: `a0.camera-agent.hardware-dual-capture-recovery-only.v1`

## Controlled Module-retention boundary (ADR-0028, 2026-08-31)

ADR-0028 permits the production binding backend to retain the SDK Module only
for the opaque, memory-only CAM-A/B binding token during controlled Dual
`CaptureRecoveryOnly`. Before every WPD open, it must close every candidate
Live View, SDK source object, and SDK capture session. While WPD is open it
must issue zero SDK API operations: no Live View, capture, candidate
enumeration, or camera setting write. Module retention is not SDK/WPD session
overlap and does not permit a second transport operation.

Agent/binding-session terminal teardown and any binding invalidation (Agent restart, USB reconnect,
camera count/topology change, SDK manager regeneration, or SDK error) unload
the Module and invalidate the binding. The exception is limited to controlled
`CaptureRecoveryOnly`; ordinary approved-rig capture-and-stitch remains
unchanged. This implementation slice covers the coexistence probe and the
software gate before one-shot only. Real one-shot and fault tests remain pending
until the required preflight, coexistence probe, regression, and independent
review gates pass. The same-binding 10-pair/100-pair runner is not implemented,
and its compatibility with the fixed host lifetime is unresolved; neither run
may start or be reported as Pass. See [DualCamera safety audit 2026-08-31](DUAL_HARDWARE_SAFETY_AUDIT_2026-08-31.md).

This document covers the production Native Named Pipe host process that serves
the already-implemented Dual hardware v2 parser, durable pair journal store,
and semantic preflight (see `dual_hardware_camera_agent.hpp`/`.cpp`). It
reuses the framing, current-logon pipe access boundary, and bounded delivery
acknowledgment contract from the Single hardware v1/v2 host
(`hardware_camera_agent_pipe.cpp`) via one shared, dispatcher-agnostic
accept/serve loop.

## Process and named-pipe transport

Build target: `A0CameraStitcher.DualCameraAgent.exe`.

The WPF project builds this exact existing CMake target for the active Debug
or Release configuration and copies the resulting executable into both build
and publish output. A missing artifact fails the build. The formal WPF test
compares SHA-256 values so the bundled/test copy must be byte-identical to the
canonical CMake output.

Production launches resolve the executable only from the application base
directory, or from a mode-specific `--camera-agent` path that still resolves
to a direct child of that directory. The path must be drive-qualified on a
`DRIVE_FIXED` volume and name an existing regular `.exe`; UNC/device/ADS,
relative/traversal/outside, removable, reparse, missing, and directory paths
are rejected without echoing the raw path. There is no PATH, current-working-
directory, user/temp-directory, or environment-variable fallback. Simulated/launcher modes reject
an explicit Agent override.

For production installation, the application directory and Agent executable
ACL must grant `Administrators` and `SYSTEM` **Modify**, and standard users
only **Read & execute**. Build and contract tests verify path and byte identity
but deliberately do not modify machine ACLs.

```powershell
.\A0CameraStitcher.DualCameraAgent.exe `
  --serve-once `                                          # optional
  --pipe-name A0CameraStitcher.CameraAgent.HardwareDual.v2.<random-guid> `
  --pair-journal-root C:\approved-local-path\dual-journal `
  --approved-capture-profile C:\approved-local-path\dual-profile.json `
  --dual-identity-proof C:\approved-local-path\dual-identity-proof.json `
  --binding-pipe-name A0CameraStitcher.CameraAgent.HardwareDual.Binding.v1.<random-guid> `
  --wpd-camera-map C:\approved-local-path\camera-map-wpd.json
```

Default pipe name (used when `--pipe-name` is omitted):
`A0CameraStitcher.CameraAgent.HardwareDual.v2` (matches the .NET
`DualHardwareCameraAgentProtocol.DefaultPipeName` constant byte-for-byte).

**Pipe-name uniqueness is the launcher's responsibility, not this
process's.** Exactly like the Single hardware v1 launcher generates "a new
unpredictable ASCII pipe name for each operation" (see
`HARDWARE_CAMERA_AGENT_V1.md`), whoever starts a Dual host must pass a fresh,
unpredictable `--pipe-name` per logical session. This Native process does not
generate or randomize its own pipe name; it only validates the one it is
given (ASCII letters/digits/`.`/`-`/`_`, 1-120 characters, checked before any
filesystem access) and serves it.

Each request and response uses a 4-byte little-endian length followed by a
strict UTF-8 body, with a 1 MiB maximum. After reading and validating the full
response frame, the .NET Single/Dual client writes one byte `0x06`. The server
waits for that byte with a real overlapped `ReadFile` under the existing
1-second response timeout. Timeout, close, invalid/late ACK, or injected wait
failure cancels that exact OVERLAPPED operation with `CancelIoEx`, collects its
completion, and returns exit 3. Only a valid ACK permits `FlushFileBuffers`
and exit 0. Failure teardown never performs a second unbounded flush. An ACK
write failure is response-unknown and never authorizes redispatch.

`--pair-journal-root`, `--approved-capture-profile`, and
`--dual-identity-proof` are all required and validated before the process
opens the pipe. `--pair-journal-root` becomes the root of a
`DualHardwarePairJournalStore` (created lazily if missing). The other two
must each already exist as a plain regular local file on an absolute,
drive-qualified, `DRIVE_FIXED` volume with a reparse-free path chain -- the
same fixed-local-path contract the pair journal store applies to its own
root. UNC, NT-namespace, mapped-network, removable, optical, and symlink/
junction paths are all rejected before this process opens the pipe.

`--approved-capture-profile` and `--dual-identity-proof` remain part of the host
launch contract. Each start request also carries frozen
`captureProfileSnapshot`/`identitySnapshot` values that the dispatcher validates
before dispatch. Supplying both `--binding-pipe-name` and `--wpd-camera-map`
enables one process-owned backend: the binding pipe first establishes a
memory-only, operator-confirmed CAM-A/B binding. Before WPD opens, all candidate
Live View, SDK source objects, and SDK capture sessions are closed; the SDK
Module alone remains loaded to retain session-local tokens. While WPD is open,
the backend issues zero SDK API operations. If WPD cleanup cannot be confirmed,
the backend issues no SDK API call (including `End`), isolates and terminals that
Agent, reports the old binding invalidation reason upward, and requires a new
binding. Agent/binding-session terminal teardown or invalidation unloads the
Module and invalidates the binding. Omitting both options preserves the fail-closed
legacy host; starts return `PairDispatcherUnavailable` after full preflight.

The `start-reserved-capture-recovery-only` capability is advertised additively
by the ordinary v2 capabilities response, but its start/query/close envelopes
use the separate `a0.camera-agent.hardware-dual-capture-recovery-only.v1`
schema. The operation is limited to
controlled Dual transport verification under ADR-0027. It omits rig evidence,
requires an explicit `captureRecoveryOnlyApproved` confirmation, and reports
`capturePurpose=CaptureRecoveryOnly`, `stitchOutcome=Pending`, and
`a0QualityApproval=Unapproved`. It does not run a stitch or establish A0 quality.
The ordinary v2 capabilities, reservation, start, query, and close payload/result
shapes remain byte-shape compatible with the pre-exception contract. Ordinary
`start-reserved-pair` is outside ADR-0028, so the production binding host returns
the existing `PairDispatcherUnavailable` response without adding availability or
preflight fields to v2. CaptureRecoveryOnly preflight state is exposed only by
its separate schema.
Its separate external/request profile schema is
`a0.dual-capture-profile.operator-approved.v1` and accepts only `DualCamera`,
`Nikon D810`, `JPEG Fine`, `L`, `7360x4912`, all of
`cameraSettingWritesApproved`／`automaticRetryApproved`／
`actualShutterSynchronizationGuaranteed=false`, and the fixed approval code
`operator-approved-capture-recovery-only-v1`.
Normal capture-profile or rig fields are rejected instead of inferred.

## Operations

| Operation | Backing | Notes |
| --- | --- | --- |
| `get-dual-capabilities` | none | always succeeds |
| `reserve-pair-transaction` | durable pair journal store | fails closed (`PairStoreUnavailable`) only if the store failed to construct |
| `start-reserved-pair` | durable pair journal plus injected session-bound backend | v2 payload/result shape and approved-rig validation remain unchanged; the production binding host returns the existing `PairDispatcherUnavailable` because this operation is outside ADR-0028 |
| `start-reserved-capture-recovery-only` | durable pair journal plus injected session-bound backend | advertised as an additive capability in v2, but start/query/close use `a0.camera-agent.hardware-dual-capture-recovery-only.v1`; requires identity, approved capture profile, capture-only confirmations, and a current Ready binding; no rig input or stitch; result remains `stitchOutcome=Pending` and A0 quality unapproved |
| `close-reserved-pair-transaction` | durable pair journal store | closes only the exact same-ID `Reserved` transaction before dispatch; missing, wrong-ID, `Dispatching`, and capture-terminal states are rejected |
| `get-pair-transaction-result` | durable pair journal store | a same-ID query recovers a Reserved, `ClosedBeforeDispatch`, or capture-terminal transaction |

Closing a reservation first atomically publishes a durable
`ClosedBeforeDispatch` tombstone, rereads that tombstone, and only then removes
the active `Reserved` record. A repeated same-ID close is idempotent and also
finishes removal if a previous process stopped after publishing the tombstone.
It never closes a transaction that reached `Dispatching` or a capture-terminal
state.

The ordinary .NET pending snapshot schema is v2 and records the recovery intent. A v1
snapshot migrates fail-closed to `MayHaveDispatched`. Only a typed
`ConfirmedUndispatched` start outcome may change the intent to
`CloseReservedBeforeDispatch`, and the PC pending snapshot is cleared only
after the Agent confirms the durable `ClosedBeforeDispatch` tombstone. If the
close response is unknown or the process restarts, recovery uses only the
frozen transaction ID for close/query operations: it does not reserve, start,
redispatch, capture, or retry.

`CaptureRecoveryOnly` uses its own snapshot schema and saves the frozen request
before reservation. A restart first requires a fresh operator CAM-A/B binding and
activation of the new Agent process, then performs only a same-ID query. `NotFound`
proves that no reservation was recorded; `Reserved` is closed by exact ID; a
terminal result is revalidated without another reserve/start.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Graceful shutdown: `--serve-once` completed one connection, or the persistent loop reached its lifetime deadline with the last connection (if any) fully delivered |
| 1 | Startup/argument failure caught in `wmain` (bad argv, an unsafe or missing `--pipe-name`, a required argument missing, an argument path that fails the fixed-local-path or existing-regular-file check, or pair journal store construction failure) |
| 2 | `kFailedBeforeDispatchExitCode` -- a connection failed before any request was dispatched (oversized/zero-length/partial frame, accept timeout, client disconnect mid-frame) |
| 3 | `kDispatchedDeliveryFailureExitCode` -- a request was dispatched and durably persisted, but the response could not be confirmed delivered to the client. The client must resolve this with a same-ID query or, for a persisted confirmed-undispatched cleanup intent, a same-ID close/query; it must never replay reserve/start |

## Lifetime policy

The Dual host uses the exact same lifetime contract as the Single hosts: a
fixed **600-second maximum lifetime**, computed once at launch and never
extended by activity. It is not an idle timeout: a host that has been
serving requests continuously for 600 seconds still exits, and a host that
served nothing at all also exits at 600 seconds. `--serve-once` is
unaffected by this budget; it always terminates after its single connection.

A rolling/idle-extended deadline (reset on every completed request) was
considered during implementation and rejected: it would let a steady trickle
of requests -- including rejected or malformed ones -- keep the process
alive indefinitely, in tension with the fixed maximum-lifetime contract
already documented for the Single v2 continuous Live View host (see
`HARDWARE_CAMERA_AGENT_V2.md`, "the native process also enforces a
600-second maximum lifetime"). The Dual host supports persistent multi-request
use only within that fixed 600s window (capabilities / reserve / start /
same-ID close/query issued as separate pipe connections over one long-running
process); it does not extend its own lifetime in response to that use. This is
sufficient for the current coexistence-probe and one-shot gate, but it is not
an approved or verified host-lifetime strategy for 10-pair or 100-pair runs.

## Verification boundary

### Required coexistence pre-gate

Before any real shutter operation, the exact production adapter must complete a
read-only coexistence probe. It must prove the pair-level WPD preflight (D810
exact-two, CAM-A/B map exact-one, both cards payload zero, all WPD sessions
closed), then prove source/capture session close before WPD open, Module
retained, WPD sessions closed after inspection, and SDK API operation count zero
throughout WPD open. If WPD cleanup is unconfirmed, it calls no SDK API (including
`End`), isolates and terminals the Agent, reports the invalidation reason, and
requires re-binding. The probe sends no capture, delete, setting write, vendor
operation, or retry. It is transport-boundary evidence only: it does not prove a
shutter, recovery, pair result, A0 quality, or release.

Native contract tests (`tests/dual_hardware_camera_agent_tests.cpp` and
`tests/dual_hardware_camera_agent_pipe_tests.cpp`)
drive this host over a real Windows named pipe. They cover the 1 MiB frame
boundary, zero-length and partial header/body frames, malformed JSON
delivered as a typed dispatch-level rejection rather than a transport
failure, persistent multi-request handling (capabilities/reserve/duplicate/
close/query as separate connections against one running host), client disconnect
mid-frame, the backend-unavailable `PairDispatcherUnavailable` contract after
a full preflight, and the "response unknown" resilience contract
(dispatched-but-undelivered response, recovered only via a same-ID query
across a simulated host restart, with zero reservation replay), and exact-ID
reserved cleanup with a durable `ClosedBeforeDispatch` tombstone. The
transport cases include response-header-only, partial-body, full response
without ACK, invalid ACK, late ACK, a completely unread response, normal ACK,
and injected ACK-wait failure; each failure remains bounded and has zero
redispatch. They also cover the strict rig-free CaptureRecoveryOnly request,
Pending/A0-unapproved result, CAM-A-before-CAM-B ordering, partial retention,
and zero retry with an injected deterministic backend. These software tests do
not load or call a real camera. Real backend construction occurs only in the
production executable when both binding options are supplied. Its current
claim is limited to the coexistence-probe and one-shot software gate; real
one-shot and fault evidence remain required on the licensed Windows hardware
PC. A separate runner and host-lifetime decision are prerequisites for later
10-pair, approved-p95, and 100-pair evidence.

## Known gaps (tracked, not fixed here)

- A transient `CreateNamedPipeW` failure inside the persistent accept loop
  currently throws out of the loop rather than retrying. The .NET launcher's
  restart/recovery handling (tracked under Issue #8) is expected to absorb
  this by relaunching the process with a fresh pipe name.
- `hardware_camera_agent_pipe.cpp` (a Single-hardware-named translation
  unit) now also implements the Dual host's shared accept/serve loop and
  includes `dual_hardware_camera_agent.hpp`. This is a structural naming/
  layering wrinkle, not a behavioral one; a future pass could rename the
  file or split the two public entry points into their own translation
  units.
