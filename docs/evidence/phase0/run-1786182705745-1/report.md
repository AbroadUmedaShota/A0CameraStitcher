# Phase 0 cross-transport binding command verification

- Run: `run-1786182705745-1`
- Checked: 2026-08-08 18:51 JST
- Scope: the currently connected firmware V1.11 D810 already registered as CAM-B.
- `bind-cross-transport-identity --alias CAM-B --single-camera-connected-confirmed` ran under one operator-session camera-control lease.
- The SDK enumerated exactly one D810 and fully closed its session before WPD enumerated exactly one D810.
- Both local maps were validated before binding. Existing CAM-B entries matched, so the operation was idempotent and changed neither map.
- Conflict, zero/multiple camera, same-map-path, missing confirmation, and explicit-transport cases are covered by automated fail-closed contracts.
- SDK-enabled and SDK-less Release CTest each passed 5/5.
- No real identity, serial, map hash, or PnP ID was printed or committed. No capture, Live View, setting change, card access, deletion, or vendor operation was performed.
