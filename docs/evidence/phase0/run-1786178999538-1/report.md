# Phase 0 CAM-B identity-v2 binding checkpoint

- Run: `run-1786178999538-1`
- Checked: 2026-08-08 17:49 JST
- The operator physically replaced the previously connected body with the intended CAM-B body; exactly one D810 was present through PnP, SDK, and WPD.
- The prior SDK CAM-A map was quarantined without reading its identity because it was derived from an ephemeral MAID source object ID.
- SDK identity v2 derives a local-only digest from the documented MAID Source `Name` and `Interface` strings with domain separation and length framing; source object IDs are excluded.
- SDK-enabled and SDK-less Release CTest each passed 5/5 after the change.
- Before binding, SDK and WPD each reported one unbound body. Explicit CAM-B binding then reported bound 1, unbound 0 in both transports, and Single readiness was `READY`.
- This is a checkpoint, not completed two-body identity acceptance. CAM-A must now be connected alone: SDK must report it unbound while WPD reports CAM-A, after which CAM-A SDK v2 can be rebound. Reconnect and USB-port-swap continuity still remain.
- No real identifier or map hash was printed or committed. No capture, Live View, setting change, card access, deletion, or vendor operation was performed.
