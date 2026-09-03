# CI cost profile

Last reviewed: 2026-09-03 JST

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

The workflow always runs for a target PR. It does not use event-level path filters, so the stable check name reaches a terminal result for documentation-only changes. Unknown or unclassified paths run the full suite. `concurrency.cancel-in-progress` remains enabled for superseded commits on the same PR.

The job timeout is reduced from 90 to 45 minutes to cap runaway consumption while retaining roughly twice the observed normal runtime. The existing shared native build directory remains in use. No dependency cache or matrix split is added in this change: the solution currently has no external NuGet package set that would justify `setup-dotnet` caching, and native cache correctness across SDK-less/SDK-linked boundaries needs separate measurement before cached binaries can be trusted. A split matrix would also multiply Windows job rounding and alter failure-reporting semantics.

## Cost estimate and approval

- One merged code PR previously consumed about 40-46 Windows runner minutes across the PR run and duplicate main-push run. The new trigger model consumes about 20-23 minutes, a reduction of about 20-23 runner minutes (approximately 50%) per merged code PR.
- At an illustrative 20 merged code PRs per month, the expected total falls from about 800-920 to 400-460 Windows runner minutes, saving about 400-460 runner minutes. Recalculate from the actual monthly PR count.
- Documentation-only PRs avoid about 18-22 runner minutes compared with a full run.
- GitHub's current billed-minute multiplier, included allowance, remaining balance, and spending limit are unconfirmed because billing API access is unavailable. Runner wall time is used here and must not be represented as the final invoice amount.
- Local implementation and review are authorized by the owner's 2026-09-03 instruction. Activating a full private Windows run remains pending explicit cost approval because its expected duration exceeds 10 minutes.

## Recheck conditions

Revisit this profile before widening triggers or matrices, changing runner OS/SKU, adding a full post-merge suite, making the repository public, changing the billing plan/owner, or after a material change in observed runtime. Hardware, licensed SDK, packaging/release, and real-camera evidence remain outside this workflow and require their own explicit trigger and acceptance record.
