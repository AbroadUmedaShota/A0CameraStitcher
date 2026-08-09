# Phase 0 CAM-A cross-transport binding — invalidated

> Superseded on 2026-08-08. After the operator physically replaced this body
> with the intended CAM-B body, WPD reported a new unbound camera while the SDK
> incorrectly restored CAM-A. Code inspection proved that the SDK map used an
> ephemeral MAID source object ID, not a physical-body identity. This run is not
> valid same-body cross-transport evidence. The WPD CAM-A registration remains
> local, but CAM-A must be rebound through SDK identity v2 and reverified.

- Run: `run-1786177891666-1`
- Checked: 2026-08-08 17:31 JST
- Exactly one physical Nikon D810 was present through PnP, licensed SDK, and WPD.
- Before binding, SDK and WPD each reported one unbound body and zero bound bodies.
- The operator confirmed that all other D810 bodies were disconnected.
- `bind-identity` explicitly registered that one body as `CAM-A` through SDK and then WPD.
- The post-binding read-only check reported SDK and WPD bound 1, unbound 0, and `Phase0Preflight: READY` with exit 0, but the SDK result was a false match caused by the defective identity scheme.
- Local identity values were not read into this report, printed, or committed.
- No capture, Live View, setting change, card access, deletion, or vendor operation was performed.
- No capture or camera mutation occurred, so invalidating this evidence requires no camera or card recovery.
