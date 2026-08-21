# Dual Hardware Camera Agent v2 — Native Named Pipe host

Status: Native host implemented / real SDK backend not wired (fake orchestrator stays test-only)
Schema: `a0.camera-agent.hardware-dual.v2`

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
  --dual-identity-proof C:\approved-local-path\dual-identity-proof.json
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

`--approved-capture-profile` and `--dual-identity-proof` are part of the host
launch contract agreed with Issue #8. Their **content is not read** yet:
every `start-reserved-pair` request already carries its own frozen
`captureProfileSnapshot`/`identitySnapshot`, which the existing dispatcher
validates per request. Reading these two files to cross-check the
client-carried snapshot, and wiring a real SDK/WPD camera backend into
`start-reserved-pair`, are both future work outside this host's current
scope: without an injected backend, every `start-reserved-pair` request is
answered `PairDispatcherUnavailable` after running its full preflight.

## Operations

| Operation | Backing | Notes |
| --- | --- | --- |
| `get-dual-capabilities` | none | always succeeds |
| `reserve-pair-transaction` | durable pair journal store | fails closed (`PairStoreUnavailable`) only if the store failed to construct |
| `start-reserved-pair` | full preflight, then `PairDispatcherUnavailable` | preflight (identity/capture-profile/rig-profile/confirmations) always runs; no camera dispatch occurs |
| `close-reserved-pair-transaction` | durable pair journal store | closes only the exact same-ID `Reserved` transaction before dispatch; missing, wrong-ID, `Dispatching`, and capture-terminal states are rejected |
| `get-pair-transaction-result` | durable pair journal store | a same-ID query recovers a Reserved, `ClosedBeforeDispatch`, or capture-terminal transaction |

Closing a reservation first atomically publishes a durable
`ClosedBeforeDispatch` tombstone, rereads that tombstone, and only then removes
the active `Reserved` record. A repeated same-ID close is idempotent and also
finishes removal if a previous process stopped after publishing the tombstone.
It never closes a transaction that reached `Dispatching` or a capture-terminal
state.

The .NET pending snapshot schema is v2 and records the recovery intent. A v1
snapshot migrates fail-closed to `MayHaveDispatched`. Only a typed
`ConfirmedUndispatched` start outcome may change the intent to
`CloseReservedBeforeDispatch`, and the PC pending snapshot is cleared only
after the Agent confirms the durable `ClosedBeforeDispatch` tombstone. If the
close response is unknown or the process restarts, recovery uses only the
frozen transaction ID for close/query operations: it does not reserve, start,
redispatch, capture, or retry.

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
600-second maximum lifetime"). The Dual host is still designed for
persistent multi-request use within that fixed 600s window (capabilities /
reserve / start / same-ID close/query issued as separate pipe connections over one
long-running process); it just does not extend its own lifetime in response
to that use.

## Verification boundary

Native contract tests (`tests/dual_hardware_camera_agent_pipe_tests.cpp`)
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
redispatch. They do not load or call a real camera;
`A0CameraStitcher.DualCameraAgent.exe` never
constructs a real SDK/WPD backend or the test-only fake orchestrator.

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
