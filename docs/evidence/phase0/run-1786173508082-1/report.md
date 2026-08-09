# Phase 0 dual binding readiness

- Run: `run-1786173508082-1`
- Checked: 2026-08-08 16:18 JST
- Final rebuild recheck: 2026-08-08 17:05 JST
- Result: `ReadyForIdentityBinding`
- Supersedes the readiness interpretation in `run-1786172900488-1`.
- SDK inventory found two D810 bodies: bound 0, unbound 2.
- WPD inventory found two D810 bodies: bound 0, unbound 2.
- Inventory is now read-only and cannot assign `CAM-A/B` from enumeration order.
- The earlier SDK map containing an enumeration-order assignment was moved to a reversible local quarantine without reading or printing identity values.
- The active SDK and WPD identity maps remained absent after the real dual inventory.
- SDK-enabled and SDK-less CTest both passed 5/5.
- Readiness now requires the SDK, WPD, and PnP body counts to equal the selected stage exactly: Single rejects the connected two-body state with exit 1, while Dual reports `READY_FOR_IDENTITY_BINDING` with exit 2.
- No production auto-assignment call remains; the final rebuilt CLI repeated the same read-only result and created neither active map.
- Real `sdk-status` and `wpd-status` requests for unbound `CAM-A` both exited 3 at alias resolution, and active map count remained zero.
- No capture, Live View, setting change, card access, deletion, or vendor operation was performed.
- The next gate is explicit SDK and WPD binding with only one physical D810 connected at a time.
