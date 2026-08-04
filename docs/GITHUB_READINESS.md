# GitHub readiness

## Repository

- Target: `AbroadUmedaShota/A0CameraStitcher`
- Visibility: private
- Default branch: `main`
- Local source of truth: this repository

## Prepared controls

- Issue forms: requirements, UX review, specification decision, release decision
- Pull request template: requirement and evidence traceability
- AutoDev labels: managed, active, blocked, human gate, UX review, specification decision, safety stop, release gate, done
- Protected-path policy: requirements, plans, governance state, and workflow definitions require explicit review
- Data policy: camera serial numbers, Nikon SDK packages, customer captures, and credentials must not be committed

## Current lifecycle

`phase0a-wpd-validation`

The requester accepted the Nikon SDK license and later approved `REVISE-WPD` after the SDK and official sample both failed to publish a PC-transfer SDRAM Item. The explicit WPD adapter inventories one D810 as anonymous `CAM-A` and completed hardware run `run-1785826415773-1`, preserving a verified 7360x4912 JPEG on the PC without deleting the camera-side object. Phase 0A repetition and fault tests remain open; Phase 0B remains blocked until a second D810 is available.

No recurring heartbeat, generated Issue, implementation branch, or pull request is active at bootstrap time.
