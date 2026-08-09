# Phase 0 dual readiness after reboot

> Superseded by `run-1786173508082-1`: this run used the older inventory path that could persist enumeration-order SDK aliases. The count of two bodies remains evidence, but its alias-map state is not binding evidence and was quarantined without reading or printing identities.

- Run: `run-1786172900488-1`
- Checked: 2026-08-08 16:08 JST
- Result: `ReadyForIdentityBinding`
- Windows detected two healthy D810 WPD nodes without printing identifiers.
- The licensed D810 adapter anonymously enumerated two bodies.
- SDK-enabled Release build succeeded and CTest passed 5/5.
- No capture, Live View, setting change, card access, deletion, or vendor operation was performed.
- Phase 0B remains gated until SDK and WPD identities are bound to `CAM-A` and `CAM-B` with only one physical D810 connected at a time.
