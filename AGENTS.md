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
- Treat Nikon D810 Camera Remote SDK exclusive sequential control as the primary Phase 0 path.
- Do not implement WPD/PTP fallback until SDK failure evidence is approved by the product owner.
- Do not reverse engineer or redistribute proprietary material without approval.
- Start with FX JPEG Fine L. RAW/NEF, live view, GPU acceleration, and hardware shutter synchronization are deferred.
- Never open more than one Nikon SDK camera session or more than one capture transaction at a time.
- Do not automatically retry or reassign ambiguous/late images; preserve them for diagnosis and start a new transaction.

## Verification

- Every hardware claim needs a repeatable test record.
- Record camera identity using redacted aliases in committed fixtures.
- Phase 0 completion requires the evidence listed in `docs/PHASE0_TEST_PLAN.md`.
- A human must approve the transport decision before integrated MVP implementation starts.
