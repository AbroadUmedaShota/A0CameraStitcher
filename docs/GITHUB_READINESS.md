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

`in-progress-software-active-hardware-capture-paused`

HG-0007 approved the attempted hybrid evaluation, but [run-1785914842210-1](evidence/phase0/run-1785914842210-1/report.md) failed at WPD baseline before SDK open/capture, and [run-1785917005306-1](evidence/phase0/run-1785917005306-1/report.md) rejected datetime cutoff correlation while proving 3/3 WPD session closes. `HG-0008` approved the dedicated empty/cleared single-slot spool on 2026-08-06. The approved path may delete only the exact just-recovered WPD object after `.partial`, JPEG and size validation, SHA-256, atomic `original.jpg`, and reread verification, then must check empty-after. The PC original remains canonical; existing-card bulk delete/format, vendor operations, and retry remain prohibited. Hardware one-shot and subsequent acceptance evidence are still pending.

Software-only work continues through `WI-0010A` and `WI-0022C`; the M1A hardware lane stays paused until a physical card-state change and explicit resume. No recurring heartbeat, generated Issue, implementation branch, pull request, push, or external post is active.
