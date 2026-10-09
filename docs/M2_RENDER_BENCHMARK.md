# M2 legacy render resource measurement (#270)

This is the actual-size resource baseline preceding #48. It measures the current
single-thread `a0_m2_render` pixel loop and WIC codecs. It produces no quality
verdict, approved rig profile, StitchJob, or product output. ADR-0033's draft
evaluation path (#273) and ADR-0034's document-plane renderer (#274) remain separate.

## Build and run

Use a fresh SDK-less build. The evaluation executable is absent by default;
enable it explicitly. Run in an otherwise idle software session, with no camera
commands. The synthetic input and report directories belong outside the product
root and are ignored by Git.

```powershell
cmake -S . -B build/benchmark -DA0_BUILD_RENDER_BENCHMARK=ON -DBUILD_TESTING=ON
cmake --build build/benchmark --config Release --target A0CameraStitcher.RenderBenchmark A0CameraStitcher.SyntheticPairGenerator --parallel 2
& build/benchmark/Release/A0CameraStitcher.SyntheticPairGenerator.exe --spec tests/fixtures/synthetic-pair/two-camera-bodies-rotated.spec.json --output build/benchmark-pair --threads 2
pwsh -NoProfile -File scripts/Measure-M2Render.ps1 -BenchmarkExe build/benchmark/Release/A0CameraStitcher.RenderBenchmark.exe -PairDirectory build/benchmark-pair -OutputFile build/benchmark-result.json
ctest --test-dir build/benchmark -C Release -R 'm2_render_benchmark|m2_render_contracts|m2_stitch_metric_contracts' --output-on-failure
```

The fixed test parameter set is `synthetic269-legacy-render`. Both JPEGs are
7360 x 4912. The nominal CAM-B to CAM-A translation is -541 x 7.27 = -3933.07
pixels on the y axis, with the legacy vertical layout and a one-row top crop.
The crop removes the fractional union-boundary row that neither image covers.
The resulting raw-coordinate canvas is 7360 x 8845. No homography is estimated,
no lens distortion is corrected, and no mm/DPI raster is claimed. This is not
the future 180-DPI A0 raster. The two Brown-Conrady source models remain present
in the synthetic input; this benchmark deliberately exercises the existing
legacy renderer.

## Measurement boundaries

One fresh process makes ten sequential calls. `process-first` means the first
call in that process; the other nine are `warm`. It does not mean a cold OS file
cache. COM/factory initialization, locked compressed-input loading, initial
input hashing, and canvas planning happen before the first timer. The same
compressed bytes and parameters are used for every call. Inputs are held open
with read sharing only, and reread hashes must match before successful exit.

For each call, the timed stages are:

1. Decode both JPEGs to 24-bit BGR, ignoring orientation metadata.
2. Call the production pure pixel renderer.
3. Encode BGR as JPEG with the current product's default WIC property bag.
4. Fully decode the encoded JPEG at its expected dimensions and hash its exact
   compressed bytes with BCrypt SHA-256.

`totalMilliseconds` spans those four stages. Destruction of their buffers is
outside that timer. JSON output and metadata collection are also outside it.
Encoding uses an owned memory `IStream`; its logical stream length, rather than
allocated HGLOBAL capacity, is the recorded JPEG size. There is no filesystem
flush, input snapshot protocol, atomic publish, product manifest, approved-profile
gate, or product adapter verification in this measurement. The 64-MiB comparison
therefore concerns compressed size, not evidence that the product's file limit
or publishing path has executed.

Memory readings use `GetProcessMemoryInfo` while input BGR, rendered BGR,
verification BGR, and compressed output are alive. `PeakWorkingSetSize` and
`PeakPagefileUsage` are cumulative process-lifetime high-water marks; the latter
is peak committed virtual memory, not a pagefile read/write count. The measured
process excludes the wrapper and the rest of the application. See Microsoft's
[PROCESS_MEMORY_COUNTERS documentation](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters).

The report records the WIC JPEG encoder component version and the file version
of the actually loaded `WindowsCodecs.dll`, without recording its path. It also
records OS build/revision, CPU model/core counts, physical RAM, and the measured
executable's SHA-256. It contains no machine name, user name, input path, EXIF, or
real capture identifiers.

Resource metric definition/result pairs use the existing schemas and native
contract validator. Both all-ten and warm-nine duration statistics are retained.
p50/p95 use Linear R7 interpolation on sorted samples; rounding is
HalfAwayFromZero after aggregation (two decimal places for milliseconds, zero
for bytes). High-water and JPEG size aggregates use Maximum. `Success` in a
metric result means a measurement exists; it is not a quality pass. The report
compares render-only p95 numerically with the v0.1 candidate stitch-only p95
10000 ms. Render-only is a partial component of stitch-only, so this comparison
does not establish the full stitch-only time. The total pipeline statistic is
retained separately. Process peak commit and JPEG size are compared with the
5-GiB app-commit and 64-MiB compressed-output figures, without a verdict.

Ten output hashes must match. If they differ, preserve the raw record and open
a separate determinism issue; do not select a convenient run or retry away the
failure. This one-machine synthetic baseline does not complete #48's hardware
matrix, leak/long-run evaluation, or #42's holdout acceptance gate. Re-measure
after the v2 renderer and evaluation path are available.

## Recorded baseline

The Release x64 baseline is recorded in
[m2-render-resource-baseline.json](measurements/m2-render-resource-baseline.json)
and Issue #270. It contains all ten raw runs and 36 validated Resource
definition/result pairs. Both input hashes remained unchanged, and all ten
outputs were 11,159,404 bytes with SHA-256
`c640184f056dc6c5d6e82284788605d7ffd13c214c881afa49c798ffdcac96e2`.

| Measurement | All ten | Warm nine |
|---|---:|---:|
| Render p50 / p95 / max (ms) | 3787.41 / 5232.50 / 5576.21 | 3778.55 / 4591.41 / 4812.40 |
| Pipeline p50 / p95 / max (ms) | 5214.05 / 7296.62 / 7543.25 | 5176.97 / 6540.66 / 6995.18 |

Process lifetime peak working set was 653,328,384 bytes (623.06 MiB), and peak
commit was 648,548,352 bytes (618.50 MiB). Render p95 differs from the 10000-ms
candidate by -4767.50 ms; peak process commit differs from the 5-GiB app
candidate by -4,720,160,768 bytes; JPEG size differs from 64 MiB by
-55,949,460 bytes. These are numeric comparisons across the explicitly stated
boundaries, with no pass/fail classification.

The measured CPU was Intel Core i5-10310U (4 cores / 8 logical processors),
with `Win32_ComputerSystem.TotalPhysicalMemory` reporting 8,215,056,384 bytes
(7.65 GiB). OS build was 26200.9457; WIC encoder component version was 1.0.0.0,
and loaded WindowsCodecs file version was 6.2.26100.9457. This is one baseline
on this environment; it does not cover #48's 16/32-GB reference-machine matrix.
The report records the exact executable hash.

Verification: SDK-less Debug and Release builds succeeded. Related CTest
checks passed (Debug 4/4, Release 6/6, including link isolation and v2 schema);
after the output-path guard revision, the metric/integration script passed
214 checks in each configuration. All 36 published metric pairs were reread
and accepted by the native validator, and the measured executable hash was
rechecked. A fresh default-OFF configuration's File API codemodel contained
no benchmark target. Independent static review found no remaining blockers.
No product, hardware, or quality acceptance is inferred from these checks.
