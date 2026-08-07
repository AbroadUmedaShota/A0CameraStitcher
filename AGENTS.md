# Repository Instructions

## Scope

- This repository is for the A0 Camera Stitcher only.
- Preserve the USB-only, Nikon D810 x2, fixed-rig, static planar-original MVP boundary unless a human decision changes it.
- Do not present actual shutter synchronization as guaranteed.

## Sources Of Truth

1. `docs/PRODUCT_REQUIREMENTS.md`
2. `.autodev/requirements/normalized.json`
3. `.autodev/plan.json`
4. Current GitHub Issue / PR state once issues exist

Chat summaries are source material, not current specification. Unresolved business or hardware choices stay in `.autodev/requirements/unresolved_questions.json`.

## Safety And Data

- Never commit camera serial numbers, SDK archives, licensed Nikon binaries, credentials, real customer originals, or captured production images.
- Keep real hardware work explicit and reversible. Do not change firmware or camera settings outside the documented test profile.
- Preserve both source images for every capture transaction during development.

## Development

- Windows 11 x64 is the target platform.
- Use C++20/CMake for Phase 0 and .NET 10 for the application layer from M3 onward.
- The attempted Phase 0 hybrid path (WPD baseline/close, SDK one-shot card capture/close, WPD recovery) is rejected by hardware evidence and must remain fail-closed. `HG-0008` was approved on 2026-08-06: a dedicated empty/cleared card may be used as the single-slot transient spool replacement. Empty means zero camera payload objects, including non-JPEG images, video, and generic files; WPD folders and functional nodes are structure, not payload.
- Never overlap SDK and WPD sessions. Stop and fully close SDK Live View before the hybrid transaction; re-open Live View only after the WPD recovery succeeds.
- Permit Phase 0A Live View handoff only with one physical D810 connected. With two bodies, require an explicit SDK/WPD-to-alias binding for each body before handoff.
- Do not reverse engineer or redistribute proprietary material without approval.
- Start with FX JPEG Fine L. One selected-camera Live View is in MVP; simultaneous two-camera Live View, preview-as-original/preview-as-stitch-input, RAW/NEF, GPU acceleration, and hardware shutter synchronization are deferred.
- Treat setup as an automatic-correction envelope, not pixel-perfect manual placement: use an approved fixed rig profile plus transient bounded per-capture correction. Do not freely re-estimate a homography, hard-code unapproved optical thresholds, clamp an over-limit result into success, or automatically learn/update the rig profile.
- Never open more than one Nikon SDK camera session or more than one capture transaction at a time.
- Every real SDK/WPD Phase 0 CLI command must hold the operator-session-wide camera-control lease. It covers processes in the same interactive Windows logon session; launching camera control from another user session or service is outside the MVP operating contract. Legacy `capture-single`, `capture-pair`, and `stability` commands are fake-only and must reject real transports before camera open.
- Apply the 180-second transaction watchdog to both pair and hybrid capture flows; safe session close may run after expiry, but no next transport, canonical rename, card delete, or success state may start after the deadline. Retain an already-written `.partial` or already-canonical PC original for diagnosis instead of deleting it.
- Do not automatically retry or reassign ambiguous/late images; preserve them for diagnosis and start a new transaction.
- The PC `original.jpg`, saved through `.partial`, JPEG validation, SHA-256, and atomic rename, is the sole retained product original. The camera card is transient transport only; persistent camera retention is not required.
- The approved spool path may delete only the exact just-recovered WPD object after PC `.partial` write, JPEG and size validation, SHA-256, atomic `original.jpg` rename, and reread verification; then confirm the spool is empty. On zero, multiple, late, invalid, download, persistence, or delete failure, retain the PC original if present, end `FailedPartial`, and do not delete. Bulk deletion and format of an existing card are prohibited. Never send vendor operation `0x9207` or retry the transaction.

## Verification

- Every hardware claim needs a repeatable test record.
- Record camera identity using redacted aliases in committed fixtures.
- Phase 0 completion requires the evidence listed in `docs/PHASE0_TEST_PLAN.md`.
- A human must approve the transport decision before integrated MVP implementation starts.
