#requires -Version 7.4
[CmdletBinding()]
param(
    [Parameter(Mandatory)][Alias('BenchmarkExe')][string]$Executable,
    [Parameter(Mandatory)][string]$PairDirectory,
    [Parameter(Mandatory)][string]$OutputFile,
    [ValidateRange(1,32768)][uint32]$Width = 7360,
    [ValidateRange(1,32768)][uint32]$Height = 4912,
    [double]$TranslateY = -3933.07,
    [uint32]$CropTop = 1,
    [ValidatePattern('^[a-z][a-z0-9-]{0,63}$')][string]$ParameterId = 'synthetic269-legacy-render'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'M2RenderBenchmarkMetrics.psm1') -Force
if (-not [double]::IsFinite($TranslateY)) { throw 'Translation must be finite.' }
if ($ParameterId -cnotmatch '^[a-z][a-z0-9-]{0,63}$') { throw 'Parameter ID must be a lowercase public token.' }
if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) { throw 'LOCALAPPDATA is required for product-root isolation.' }
$productRoot = Join-Path $env:LOCALAPPDATA 'A0CameraStitcher'
$output = Assert-A0BenchmarkOutputPath -Path $OutputFile -ProductRoot $productRoot
$exe = (Get-Item -LiteralPath $Executable -ErrorAction Stop).FullName
if (-not [IO.File]::Exists($exe)) { throw 'Benchmark executable must be a file.' }
$pair = (Get-Item -LiteralPath $PairDirectory -ErrorAction Stop).FullName
if (-not [IO.Directory]::Exists($pair)) { throw 'Pair directory must be a directory.' }
$cameraA = Join-Path $pair 'CAM-A/original.jpg'
$cameraB = Join-Path $pair 'CAM-B/original.jpg'
foreach ($inputFile in @($cameraA,$cameraB)) {
    if (-not [IO.File]::Exists($inputFile)) { throw 'Both canonical pair input files are required.' }
}

function Invoke-BenchmarkCommand {
    param([Parameter(Mandatory)][string[]]$Arguments)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $exe; $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Benchmark process could not start.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $stdoutText = $stdout.GetAwaiter().GetResult(); $stderrText = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            # Native stderr may contain local paths: do not relay it into a public report.
            throw "Benchmark command failed with exit code $($process.ExitCode)."
        }
        if (-not [string]::IsNullOrWhiteSpace($stderrText)) { throw 'Benchmark command emitted unexpected diagnostics.' }
        return $stdoutText
    }
    finally { $process.Dispose() }
}

$exeHashBefore = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
$inputHashesBefore = @($cameraA,$cameraB | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant() })
$rawJson = Invoke-BenchmarkCommand -Arguments @(
    '--camera-a',$cameraA,'--camera-b',$cameraB,
    '--width',$Width.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--height',$Height.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--translate-y',$TranslateY.ToString('R',[Globalization.CultureInfo]::InvariantCulture),
    '--crop-top',$CropTop.ToString([Globalization.CultureInfo]::InvariantCulture)
)
$record = ConvertFrom-Json -InputObject $rawJson -Depth 30
Assert-A0BenchmarkRun $record
for ($index = 0; $index -lt 2; $index++) {
    if ($record.inputSha256[$index] -ine $inputHashesBefore[$index]) { throw 'Native input hash does not match the wrapper snapshot.' }
}
$inputHashesAfter = @($cameraA,$cameraB | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant() })
if ($inputHashesBefore[0] -cne $inputHashesAfter[0] -or $inputHashesBefore[1] -cne $inputHashesAfter[1]) { throw 'Benchmark inputs changed.' }
$exeHashAfter = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
if ($exeHashBefore -cne $exeHashAfter) { throw 'Benchmark executable changed.' }
$metrics = @(New-A0BenchmarkMetrics $record)
$schemaDirectory = Join-Path $PSScriptRoot '../docs/schemas'
foreach ($metric in $metrics) {
    $definitionJson = ConvertTo-Json -InputObject $metric.definition -Depth 20 -Compress
    $resultJson = ConvertTo-Json -InputObject $metric.result -Depth 20 -Compress
    if (-not (Test-Json -Json $definitionJson -SchemaFile (Join-Path $schemaDirectory 'stitch-metric-definition.schema.json')) -or
        -not (Test-Json -Json $resultJson -SchemaFile (Join-Path $schemaDirectory 'stitch-metric-result.schema.json'))) { throw 'Resource metric schema validation failed.' }
    $null = Invoke-BenchmarkCommand -Arguments @('--validate-metric-pair',$definitionJson,$resultJson)
}
# These projections contain no computer name, account, processor ID or serial.
$version = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -Name CurrentBuildNumber,UBR
$processors = @(Get-CimInstance -ClassName Win32_Processor -Property Name,NumberOfCores,NumberOfLogicalProcessors)
$memory = Get-CimInstance -ClassName Win32_ComputerSystem -Property TotalPhysicalMemory
$allRenderP95 = @($metrics | Where-Object { $_.result.metricId -ceq 'render-duration-all10-p95' })[0].result.rawValue
$allTotalP95 = @($metrics | Where-Object { $_.result.metricId -ceq 'total-duration-all10-p95' })[0].result.rawValue
$hashesIdentical = @($record.runs.outputSha256 | ForEach-Object { $_.ToLowerInvariant() } | Select-Object -Unique).Count -eq 1
$report = [ordered]@{
    schema = 'a0.m2.render-benchmark-report.v1'; issue = 270; environment = 'software-only'
    measurementOutcome = $(if ($hashesIdentical) { 'recorded' } else { 'determinism-mismatch' })
    parameterId = $ParameterId; profileStatus = 'not-used-fixed-test-parameters'; quality = 'not-evaluated'
    parameters = [ordered]@{ width = $Width; height = $Height; translateY = $TranslateY; cropTop = $CropTop }
    execution = [ordered]@{
        engine = 'legacy-render-pair'; coldMeaning = 'first-iteration-in-process-os-cache-not-controlled'
        warmCount = 9; totalCount = 10; peakScope = 'process-lifetime-cumulative-observations'
        totalTimingIncludes = @('wic-decode','render-pair','wic-encode','full-output-decode-verification','output-hash')
        totalTimingExcludes = @('initial-locked-input-load-and-hash','com-setup','canvas-plan','buffer-cleanup','filesystem-write-flush-atomic-publish-product-manifest','json-output-and-metadata-collection')
        encoderConfiguration = 'wic-jpeg-default-property-bag-memory-istream'
        qualityEvaluated = $false; productManifestWritten = $false; productPathExecuted = $false
        v2PipelineMeasured = $false; actual64MiBLimitPathTested = $false
    }
    machine = [ordered]@{
        osBuild = [string]$version.CurrentBuildNumber; osUbr = [uint32]$version.UBR; executableSha256 = $exeHashBefore
        processors = @($processors | ForEach-Object { [ordered]@{ name = $_.Name; cores = $_.NumberOfCores; logicalProcessors = $_.NumberOfLogicalProcessors } })
        memoryBytes = [uint64]$memory.TotalPhysicalMemory
        wicComponentVersion = $record.wicComponentVersion; wicDllVersion = $record.wicDllVersion
    }
    inputsUnchanged = $true
    outputHashesIdentical = $hashesIdentical
    measurement = $record
    summary = $metrics
    comparisons = [ordered]@{
        status = 'numeric-comparison-only-no-product-verdict'
        renderP95All10Milliseconds = $allRenderP95; candidateStitchOnlyP95Milliseconds = 10000
        renderP95MinusCandidateMilliseconds = $allRenderP95 - 10000
        pipelineTotalP95All10Milliseconds = $allTotalP95
        pipelineTotalScope = 'decode-render-encode-output-verification-hash-diagnostic-only'
        processPeakCommitBytes = Get-A0MetricMaximum @($record.runs.peakCommitBytes)
        candidateAppCommitBytes = [uint64]5368709120
        processPeakCommitMinusAppCandidateBytes = (Get-A0MetricMaximum @($record.runs.peakCommitBytes)) - 5368709120
        outputMaxBytes = Get-A0MetricMaximum @($record.runs.outputSizeBytes)
        existingReadLimitBytes = [uint64]67108864
        outputMaxMinusExistingReadLimitBytes = (Get-A0MetricMaximum @($record.runs.outputSizeBytes)) - 67108864
        limitations = @('render-only-is-not-full-stitch-only','native-process-is-not-whole-app-commit','size-comparison-is-not-product-limit-path-test')
    }
}
$json = ConvertTo-Json -InputObject $report -Depth 30
# Recheck after the measurement, then claim a new leaf atomically. Never overwrite.
$null = Assert-A0BenchmarkOutputPath -Path $output -ProductRoot $productRoot
$stream = [IO.FileStream]::new($output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json + [Environment]::NewLine)
    $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true)
}
finally { $stream.Dispose() }
if (-not $hashesIdentical) {
    throw 'Determinism mismatch: raw run evidence was retained in the new report. A separate determinism Issue is required.'
}
Write-Output "report=written metrics=$($metrics.Count) runs=10 outputHashesIdentical=$($report.outputHashesIdentical) quality=not-evaluated"
