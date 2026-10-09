#requires -Version 7.4
[CmdletBinding()]
param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot), [string]$BenchmarkExe)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $RepositoryRoot 'scripts/M2RenderBenchmarkMetrics.psm1') -Force
$script:checks = 0
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }; $script:checks++
}
function Reject([scriptblock]$Action, [string]$Message) {
    $threw = $false
    try { $null = & $Action } catch { $threw = $true }
    Check $threw $Message
}
Check ([Math]::Abs((Get-A0R7Percentile @(0..9) 95) - 8.55) -lt 1e-12) 'Ten-sample R7 p95 must be 8.55.'
Check ([Math]::Abs((Get-A0R7Percentile @(0..8) 95) - 7.6) -lt 1e-12) 'Nine-sample R7 p95 must be 7.6.'
Check ((Get-A0R7Percentile @(9,0,8,1,7,2,6,3,5,4) 50) -eq 4.5) 'Even p50 uses the middle pair.'
Check ((Get-A0R7Percentile @(0..8) 50) -eq 4) 'Odd p50 is the middle observation.'
Check ((Get-A0R7Percentile @(42) 95) -eq 42) 'Singleton percentile.'
Check ((Get-A0R7Percentile @(0..9) 0) -eq 0) 'R7 lower endpoint.'
Check ((Get-A0R7Percentile @(0..9) 100) -eq 9) 'R7 upper endpoint.'
Check ((Get-A0MetricMaximum @(9,0,4,5)) -eq 9) 'Maximum is independent of input ordering.'
Check ((ConvertTo-A0RoundedMetric 1.245 2) -eq 1.25) 'Exact decimal midpoint rounds away from zero.'
Check ((ConvertTo-A0RoundedMetric 1.2449999999999999 2) -eq 1.24) 'Below-half decimal must not become a binary tie.'
Check ((ConvertTo-A0RoundedMetric 1.2450000000000001 2) -eq 1.25) 'Above-half decimal.'
Check ((ConvertTo-A0RoundedMetric 5368709120 0) -eq 5368709120) 'Five GiB bytes remain exact.'
Reject { ConvertTo-A0RoundedMetric 549755813889 0 } 'Exact-safe representability bound.'
Reject { Get-A0R7Percentile @(1,2) 101 } 'Percentile range.'
Reject { Get-A0R7Percentile @(1,[double]::NaN) 95 } 'NaN rejection.'
Reject { Get-A0R7Percentile @(1,[double]::PositiveInfinity) 95 } 'Infinity rejection.'
Reject { Get-A0R7Percentile @(1,'2') 95 } 'Numeric strings are not measurements.'
Reject { Get-A0MetricMaximum @(1,-2) } 'Negative resource value rejection.'
Reject { New-A0ResourceMetricPair -MetricId 'bytes' -Unit bytes -Statistic max -Samples @(1.5) } 'Byte observations must be integers.'

$record = [pscustomobject]@{
    schema = 'a0.m2.render-benchmark-run.v1'; inputSha256 = @(('a' * 64),('b' * 64))
    outputWidth = 7360; outputHeight = 8844; wicComponentVersion = '10.0.0.1'; wicDllVersion = '10.0.0.2'; allHashesEqual = $true
    runs = @(1..10 | ForEach-Object {
        [pscustomobject]@{
            iteration = $_; phase = $(if ($_ -eq 1) { 'process-first' } else { 'warm' })
            decodeMilliseconds = $_; renderMilliseconds = $_ - 1; encodeMilliseconds = $_ + 1
            verifyHashMilliseconds = $_ / 10.0; totalMilliseconds = $_ * 4.0
            outputSizeBytes = 12345678; outputSha256 = 'c' * 64
            peakWorkingSetBytes = 100000000 + $_; peakCommitBytes = 200000000 + $_
        }
    })
}
Assert-A0BenchmarkRun $record
$pairs = @(New-A0BenchmarkMetrics $record)
Check ($pairs.Count -eq 36) 'Two populations of five durations x three statistics plus three byte maxima.'
$allP95 = @($pairs | Where-Object { $_.result.metricId -eq 'render-duration-all10-p95' })[0]
Check ([Math]::Abs($allP95.result.rawValue - 8.55) -lt 1e-12 -and $allP95.result.sampleCount -eq 10) 'All ten observations contribute to p95.'
$warmP95 = @($pairs | Where-Object { $_.result.metricId -eq 'render-duration-warm9-p95' })[0]
Check ([Math]::Abs($warmP95.result.rawValue - 8.6) -lt 1e-12 -and $warmP95.result.sampleCount -eq 9) 'Warm population excludes only first process iteration.'
Check ($allP95.definition.aggregation.percentileInterpolation -eq 'LinearR7') 'Definition identifies percentile algorithm.'
foreach ($pair in $pairs) {
    foreach ($side in @('definition','result')) {
        $schemaFile = Join-Path $RepositoryRoot "docs/schemas/stitch-metric-$side.schema.json"
        $json = ConvertTo-Json -InputObject $pair.$side -Depth 20 -Compress
        Check (Test-Json -Json $json -SchemaFile $schemaFile) "Resource $side conforms to checked-in schema."
    }
}
$peak = @($pairs | Where-Object { $_.result.metricId -eq 'peak-commit-warm9-max' })[0]
Check ($peak.measurementScope -eq 'process-lifetime-cumulative-observations') 'Warm peak is explicitly cumulative, not an independent interval.'
Check ($peak.definition.rounding.decimalPlaces -eq 0) 'Byte metrics use integer rounding.'
$record.runs[4].phase = 'process-first'
Reject { Assert-A0BenchmarkRun $record } 'Wrong warm label.'
$record.runs[4].phase = 'warm'
$record.runs[4].peakCommitBytes = 1
Reject { Assert-A0BenchmarkRun $record } 'Decreasing process-lifetime high water.'
$record.runs[4].peakCommitBytes = 200000005
$record.runs[4].outputSha256 = 'invalid'
Reject { Assert-A0BenchmarkRun $record } 'Malformed hash.'
$record.runs[4].outputSha256 = 'c' * 64
$record.runs[4].outputSizeBytes = 1.5
Reject { Assert-A0BenchmarkRun $record } 'Fractional bytes.'
$record.runs[4].outputSizeBytes = 12345678
$record.runs[4].outputSha256 = 'd' * 64
Reject { Assert-A0BenchmarkRun $record } 'Determinism flag cannot disagree with hash observations.'
$record.allHashesEqual = $false
Assert-A0BenchmarkRun $record
Check (-not $record.allHashesEqual) 'Differing valid hashes are retained as measurement evidence.'
$record.runs[4].outputSha256 = 'c' * 64; $record.allHashesEqual = $true

if (-not [string]::IsNullOrWhiteSpace($BenchmarkExe)) {
    function Invoke-NativeTest([string[]]$Arguments) {
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = [IO.Path]::GetFullPath($BenchmarkExe); $start.UseShellExecute = $false
        $start.CreateNoWindow = $true; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
        foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
        $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
        try {
            Check ($process.Start()) 'Native test process starts.'
            $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
            $process.WaitForExit()
            return [pscustomobject]@{ code = $process.ExitCode; stdout = $stdout.GetAwaiter().GetResult(); stderr = $stderr.GetAwaiter().GetResult() }
        }
        finally { $process.Dispose() }
    }
    $selfTest = Invoke-NativeTest @('--self-test')
    Check ($selfTest.code -eq 0 -and [string]::IsNullOrWhiteSpace($selfTest.stderr)) 'Small native self-test succeeds without diagnostics.'
    $nativeRecord = ConvertFrom-Json -InputObject $selfTest.stdout -Depth 30
    Assert-A0BenchmarkRun $nativeRecord
    Check ($nativeRecord.runs.Count -eq 10 -and $nativeRecord.allHashesEqual) 'Native small pair has ten identical output hashes.'
    Check ($nativeRecord.outputWidth -eq 16 -and $nativeRecord.outputHeight -eq 20) 'Self-test fixed geometry is 16 x 20.'
    Check (@($nativeRecord.runs | Where-Object { $_.outputSizeBytes -le 0 }).Count -eq 0) 'Every self-test output has bytes.'
    foreach ($pair in @(New-A0BenchmarkMetrics $nativeRecord)) {
        $definitionJson = ConvertTo-Json -InputObject $pair.definition -Depth 20 -Compress
        $resultJson = ConvertTo-Json -InputObject $pair.result -Depth 20 -Compress
        $validation = Invoke-NativeTest @('--validate-metric-pair',$definitionJson,$resultJson)
        Check ($validation.code -eq 0) 'Native exact resource validator accepts generated metric.'
    }
    $definitionJson = ConvertTo-Json -InputObject $allP95.definition -Depth 20 -Compress
    $badRounding = $allP95.result | ConvertTo-Json -Depth 20 -Compress | ConvertFrom-Json
    $badRounding.value = 999
    $validation = Invoke-NativeTest @('--validate-metric-pair',$definitionJson,($badRounding | ConvertTo-Json -Depth 20 -Compress))
    Check ($validation.code -eq 2) 'Native rejects a schema-valid but numerically wrong rounded result.'
    $badDomain = $allP95.result | ConvertTo-Json -Depth 20 -Compress | ConvertFrom-Json
    $badDomain.metricDomain = 'Image'
    $validation = Invoke-NativeTest @('--validate-metric-pair',$definitionJson,($badDomain | ConvertTo-Json -Depth 20 -Compress))
    Check ($validation.code -eq 2) 'Native rejects mismatched definition/result domain.'
    foreach ($arguments in @(@('--unknown'),@('--width'),@('--validate-metric-pair','{}','{}'))) {
        $invalid = Invoke-NativeTest $arguments
        Check ($invalid.code -eq 2) 'Invalid CLI fails closed.'
    }
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('a0-render-metrics-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $temporaryRoot
try {
    $productRoot = Join-Path $temporaryRoot 'product folder with long name'
    $null = New-Item -ItemType Directory -Path $productRoot
    $newOutput = Join-Path $temporaryRoot 'new-report.json'
    Check ((Assert-A0BenchmarkOutputPath $newOutput $productRoot) -eq [IO.Path]::GetFullPath($newOutput)) 'New file outside product root is accepted.'
    Reject { Assert-A0BenchmarkOutputPath (Join-Path $productRoot 'report.json') $productRoot } 'Product-root output rejected.'
    foreach ($suffix in @('.', ' ')) {
        Reject { Assert-A0BenchmarkOutputPath (Join-Path ($productRoot + $suffix) 'report.json') $productRoot } 'Trailing dot/space directory alias cannot bypass product-root isolation.'
    }
    $shortBuffer = [Text.StringBuilder]::new(32768)
    $shortLength = [A0.M2Benchmark.NativePaths]::GetShortPathNameW($productRoot, $shortBuffer, [uint32]$shortBuffer.Capacity)
    if ($shortLength -gt 0 -and $shortLength -lt $shortBuffer.Capacity -and $shortBuffer.ToString() -ine $productRoot) {
        $shortRoot = $shortBuffer.ToString()
        Reject { Assert-A0BenchmarkOutputPath (Join-Path $shortRoot 'report.json') $productRoot } 'Existing 8.3 directory alias cannot bypass product-root isolation.'
    }
    $absentProductRoot = Join-Path $productRoot 'not-created/child'
    Reject { Assert-A0BenchmarkOutputPath (Join-Path ($productRoot + '.') 'not-created/child/report.json') $absentProductRoot } 'Missing product root never authorizes a missing output parent.'
    Check ((Assert-A0BenchmarkOutputPath (Join-Path $temporaryRoot 'another-new.json') $absentProductRoot) -eq [IO.Path]::GetFullPath((Join-Path $temporaryRoot 'another-new.json'))) 'Missing product root resolves through an existing ancestor.'
    foreach ($invalidLeaf in @('report.json:stream', 'report.json.', 'report.json ', 'CON', 'nul.json', 'COM1.json', 'LPT9.json')) {
        Reject { Assert-A0BenchmarkOutputPath (Join-Path $temporaryRoot $invalidLeaf) $productRoot } 'ADS, trailing dot/space and device names are not regular new report files.'
    }
    [IO.File]::WriteAllText($newOutput, 'sentinel')
    Reject { Assert-A0BenchmarkOutputPath $newOutput $productRoot } 'Existing output rejected.'
    Check ([IO.File]::ReadAllText($newOutput) -ceq 'sentinel') 'Existing output preserved.'
    Reject {
        & (Join-Path $RepositoryRoot 'scripts/Measure-M2Render.ps1') -BenchmarkExe 'must-not-run.exe' -PairDirectory 'must-not-read' -OutputFile $newOutput
    } 'Wrapper rejects existing output before resolving or starting a native executable.'
    Reject { Assert-A0BenchmarkOutputPath (Join-Path $temporaryRoot 'missing/report.json') $productRoot } 'Missing parent rejected.'
    $junction = Join-Path $temporaryRoot 'linked'
    $null = New-Item -ItemType Junction -Path $junction -Target $productRoot
    try { Reject { Assert-A0BenchmarkOutputPath (Join-Path $junction 'report.json') $productRoot } 'Reparse output parent rejected.' }
    finally { Remove-Item -LiteralPath $junction -Force }
    $outsideTarget = Join-Path $temporaryRoot 'outside-target'
    $null = New-Item -ItemType Directory -Path $outsideTarget
    $null = New-Item -ItemType Junction -Path $junction -Target $outsideTarget
    try { Reject { Assert-A0BenchmarkOutputPath (Join-Path $junction 'report.json') $productRoot } 'A junction outside the product root is still forbidden as an output parent.' }
    finally { Remove-Item -LiteralPath $junction -Force }
    if (-not [string]::IsNullOrWhiteSpace($BenchmarkExe)) {
        $invalidPair = Join-Path $temporaryRoot 'invalid-pair'
        $invalidA = Join-Path $invalidPair 'CAM-A/original.jpg'
        $invalidB = Join-Path $invalidPair 'CAM-B/original.jpg'
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $invalidA), (Split-Path -Parent $invalidB)
        [IO.File]::WriteAllBytes($invalidA, [byte[]]@(1)); [IO.File]::WriteAllBytes($invalidB, [byte[]]@(2))
        $failureOutput = Join-Path $temporaryRoot 'must-not-exist.json'
        $failureMessage = $null
        try {
            & (Join-Path $RepositoryRoot 'scripts/Measure-M2Render.ps1') -BenchmarkExe $BenchmarkExe -PairDirectory $invalidPair -Width 16 -Height 12 -TranslateY -8 -CropTop 0 -OutputFile $failureOutput
        }
        catch { $failureMessage = $_.Exception.Message }
        Check ($failureMessage -ceq 'Benchmark command failed with exit code 2.') 'Actual nonzero native benchmark exit must stop the wrapper.'
        Check (-not (Test-Path -LiteralPath $failureOutput)) 'Failed native benchmark creates no report.'
        Check (([IO.File]::ReadAllBytes($invalidA)).Length -eq 1 -and ([IO.File]::ReadAllBytes($invalidA))[0] -eq 1) 'Failed native benchmark preserves CAM-A input.'
        Check (([IO.File]::ReadAllBytes($invalidB)).Length -eq 1 -and ([IO.File]::ReadAllBytes($invalidB))[0] -eq 2) 'Failed native benchmark preserves CAM-B input.'
    }
}
finally {
    # All children were created in this test; validate before recursive cleanup.
    $full = [IO.Path]::GetFullPath($temporaryRoot)
    $tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
    if (-not $full.StartsWith($tempBase + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
        -not ([IO.Path]::GetFileName($full)).StartsWith('a0-render-metrics-')) { throw 'Unsafe test cleanup target.' }
    Remove-Item -LiteralPath $full -Recurse -Force
}
Write-Output "render_benchmark_metrics checks=$script:checks failures=0"
