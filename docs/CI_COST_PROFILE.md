# CI cost profile

Last reviewed: 2026-09-04 JST

## Repository and billing context

| Item | Current evidence |
| --- | --- |
| Repository | `AbroadUmedaShota/A0CameraStitcher` |
| Visibility | Private |
| Billing owner | Personal owner `AbroadUmedaShota` |
| Runner | GitHub-hosted `windows-latest`; observed image `windows-2025-vs2026` |
| Canonical check | `Software-only build/test (no camera/SDK hardware)` |
| Allowance | Owner reported the August 2026 Windows usage investigation; exact current allowance and remaining balance are unavailable to the repository token |
| Billing API | `users/{owner}/settings/billing/usage` returned 404 because the current token lacks the `user` scope |
| Protection API | Branch protection and ruleset endpoints returned 403 for this private repository on the current plan |

The owner-provided investigation found 88 allocated Windows jobs and 1,127 runner wall-clock minutes in August 2026. A clear billing block started at run `32689537021`; 53 later runs were created without a runner or executed steps. Do not retry a billing-blocked run. Classify it as `environment-blocked` and wait for the billing state to change.

The block is not currently preventing runner allocation: runs `33716502430`, `33718362927`, and `33721619037` all received a Windows runner on 2026-09-03. This proves availability for those runs only; it does not reveal the remaining allowance or spending-limit state.

## Trigger and job profile

| State | Trigger | Expensive Windows job | Expected runner time |
| --- | --- | --- | --- |
| Before | PR to `main` or integration branch | Full Debug/Release C++, M3 and WPF suite | about 20-23 min |
| Before | push to `main` or integration branch | The same full suite again | about 20-23 min |
| After | PR to `main` or integration branch, code/build/test/workflow or unknown path | One canonical full suite | about 20-23 min |
| After | documentation-only PR | Same required check reaches a terminal result; expensive steps are skipped | about 1-2 min |
| After | push to `main` | No duplicate full suite | 0 min |
| After | `workflow_dispatch` | Full suite by explicit selection; lightweight mode is also available | about 20-23 min full, about 1-2 min lightweight |

The full suite remains one Windows job with no matrix:

1. SDK-less C++ Debug build and CTest.
2. SDK-less C++ Release build and CTest.
3. Camera Agent incremental packaging regression.
4. M3 simulated foundation Debug and Release.
5. Formal WPF DualCamera flow Release.

The workflow always runs for a target PR. It does not use event-level path filters, so the stable check name reaches a terminal result for documentation-only changes. The shared PowerShell classifier allows only pure Markdown (`.md`/`.markdown`) paths into the lightweight lane. Machine-consumed artifacts such as `docs/schemas/**`, non-Markdown files, unknown/empty path sets, mixed changes, and the source side of a source-to-docs rename run the full suite; PR path enumeration uses `git diff --no-renames` to expose both sides. A cheap classifier regression step runs locally and in the workflow before any expensive stage. `concurrency.cancel-in-progress` remains enabled for superseded commits on the same PR.

The job timeout is reduced from 90 to 45 minutes to cap runaway consumption while retaining roughly twice the observed normal runtime. The existing shared native build directory remains in use. No dependency cache or matrix split is added in this change: the solution currently has no external NuGet package set that would justify `setup-dotnet` caching, and native cache correctness across SDK-less/SDK-linked boundaries needs separate measurement before cached binaries can be trusted. A split matrix would also multiply Windows job rounding and alter failure-reporting semantics.

## Cost estimate and approval

- One merged code PR previously consumed about 40-46 Windows runner minutes across the PR run and duplicate main-push run. The new trigger model consumes about 20-23 minutes, a reduction of about 20-23 runner minutes (approximately 50%) per merged code PR.
- At an illustrative 20 merged code PRs per month, the expected total falls from about 800-920 to 400-460 Windows runner minutes, saving about 400-460 runner minutes. Recalculate from the actual monthly PR count.
- Documentation-only PRs avoid about 18-22 runner minutes compared with a full run.
- GitHub's current billed-minute multiplier, included allowance, remaining balance, and spending limit are unconfirmed because billing API access is unavailable. Runner wall time is used here and must not be represented as the final invoice amount.
- Local implementation and review are authorized by the owner's 2026-09-03 instruction. Full private Windows runs require explicit scoped cost approval because their expected duration exceeds 10 minutes; the one-time approval below does not authorize later runs.

### One-time PR #173 approval (2026-09-04 JST)

- Approval: the owner authorized the known PR #173 M2 reservation-journal fix and one additional Windows Software-Only CI run, conveyed by the integration command task `01a067d0-12f9-7cd1-80d0-07f1de90f4a3`; recorded at 2026-09-04 08:20 JST. This covers one push after local preparation, its canonical PR run, and merge only after the exact-SHA check succeeds and independent review passes.
- Approved change: M2 commit `cdaa8e805cfa2dd5432e1c7ba52b787e23d2db89` plus test-summary correction `4611f660a51badb364b5d2fb73ae97143cfe3636`. The branch incorporates main `9f6527b2873c455a1d9db2eabbc2d14e81f6e8a1` to use the merged CI-cost controls, plus this required approval record. No unrelated product changes are included.
- Pre-push review found the same obsolete 18-test expectation in `Test-DualCameraWpfFlow.ps1`. Its companion correction validates the positive PASS-line count against both summary counts and still rejects nonzero exit; this directly supports the known M2 19-test suite and does not broaden product scope or weaken the check.
- Cost basis: one `windows-latest` job, no matrix, approximately 20-23 runner minutes; per-job whole-minute rounding applies. The preceding exact-SHA full run `33778790465` took 20m42s (21 rounded runner minutes). The 45-minute timeout remains the execution ceiling, not the typical estimate.
- Monthly increment authorized by this decision: one run in September 2026, approximately 20-23 runner minutes in total (one job multiplied by one run); no recurring run budget is granted. Billed multipliers and currency cost remain unknown and are not equated with runner wall time.
- Allowance status as of 2026-09-04 08:20 JST: owner/visibility rechecked as personal `AbroadUmedaShota`, private repository. Billing usage API returned HTTP 404 and required the unavailable `user` scope; current allowance, remaining balance and spending limit are unknown. The prior successful run proves prior allocation only, not free capacity. The owner approved the stated estimate with this uncertainty surfaced.
- Trigger scope: the existing PR `synchronize` event is the only authorized full run. No manual dispatch, automatic rerun, verify-only PR, additional paid run, or duplicate main-push full suite is authorized. On CI failure or billing/quota/spending-limit block, stop and report without a chargeable retry.
- Gate status: branch-protection and ruleset APIs returned HTTP 403 (plan restriction) on this recheck. This does not waive the accepted exact-SHA full CI and independent-review gates, nor authorize any protection change.
- Excluded: hardware, SDK/USB/capture, production, distribution and release. Record the actual run ID/result and merge result in PR #173 / Issue #151 without another CI-triggering push solely to update this log.

## Recheck conditions

Revisit this profile before widening triggers or matrices, changing runner OS/SKU, adding a full post-merge suite, making the repository public, changing the billing plan/owner, or after a material change in observed runtime. Hardware, licensed SDK, packaging/release, and real-camera evidence remain outside this workflow and require their own explicit trigger and acceptance record.
