#requires -Version 7.4
<#
.SYNOPSIS
Run the development-only JPEG detector and draft calibrator as one recorded job.
.DESCRIPTION
Image paths remain relative to the ORIGINAL DetectionJob directory. Both job
files are read-locked while the native tools run. Originals are not copied or
modified. A new directory holds observations, draft artifacts, job snapshots,
private child logs and a hash inventory. This does not approve calibration or
optical quality and does not control a camera.

Exit 0: draft produced. Exit 3: recorded detection/calibration failure, no
profile. Exit 2: input, tool-protocol or IO error; any partial folder is retained.
PowerShell parameter-binding errors use PowerShell's own exit code.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DetectionJob,
    [Parameter(Mandatory)][string]$CalibrationJob,
    [Parameter(Mandatory)][string]$DetectorExecutable,
    [Parameter(Mandatory)][string]$CalibratorExecutable,
    [Parameter(Mandatory)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$utf8 = [Text.UTF8Encoding]::new($false, $true)

function Full-Path([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { throw 'Empty path.' }
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Value)
}
function New-Bytes([string]$Path, [byte[]]$Bytes) {
    $file = [IO.FileStream]::new($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $file.Write($Bytes, 0, $Bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
    if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([IO.File]::ReadAllBytes($Path))) -cne
        [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes))) { throw 'Reread mismatch.' }
}
function Read-LockedJob([string]$Path, [int]$Maximum) {
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -lt 1 -or $stream.Length -gt $Maximum) { throw 'Job size outside limit.' }
        $bytes = [byte[]]::new([int]$stream.Length)
        $stream.ReadExactly($bytes, 0, $bytes.Length)
        return @{ Stream = $stream; Bytes = $bytes }
    } catch { $stream.Dispose(); throw }
}
function Run-Tool([string]$Executable, [string[]]$Arguments, [string]$LogName) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw 'Tool did not start.' }
        # Drain BOTH pipes before waiting; a large diagnostic must not deadlock.
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        New-Bytes (Join-Path $stage "logs/$LogName.stdout.txt") ($utf8.GetBytes($stdout.GetAwaiter().GetResult()))
        New-Bytes (Join-Path $stage "logs/$LogName.stderr.txt") ($utf8.GetBytes($stderr.GetAwaiter().GetResult()))
        return $process.ExitCode
    } finally { $process.Dispose() }
}
function Require-File([string]$RelativePath, [long]$Maximum) {
    $file = [IO.FileInfo]::new((Join-Path $stage $RelativePath))
    if (-not $file.Exists -or $file.Length -le 0 -or $file.Length -gt $Maximum) { throw 'Missing or oversized native artifact.' }
}
function Json-Artifact([string]$RelativePath) {
    return $utf8.GetString([IO.File]::ReadAllBytes((Join-Path $stage $RelativePath))) | ConvertFrom-Json -AsHashtable
}
function Save-Report([string]$Result, [int]$ExitCode) {
    $artifacts = @()
    foreach ($relative in @(
        'inputs/detection-job.json', 'inputs/calibration-job.json',
        'observations/session.json', 'observations/detection-report.json',
        'draft/profile.json', 'draft/calibration-report.json',
        'logs/detection.stdout.txt', 'logs/detection.stderr.txt',
        'logs/calibration.stdout.txt', 'logs/calibration.stderr.txt', 'logs/pipeline.stderr.txt', 'pipeline-error.json'
    )) {
        $path = Join-Path $stage $relative
        if ([IO.File]::Exists($path)) {
            $bytes = [IO.File]::ReadAllBytes($path)
            $artifacts += [ordered]@{
                relativePath = $relative
                sizeBytes = $bytes.LongLength
                sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
            }
        }
    }
    $report = [ordered]@{
        schemaVersion = 'a0.draft-calibration-pipeline.v1'
        result = $Result
        exitCode = $ExitCode
        phase = $phase
        quality = 'not-assessed'
        imageBase = 'original-detection-job-directory'
        inputImagesCopied = $false
        automaticRetryCount = 0
        publicationRequiresFinalDirectory = $true
        steps = $steps
        profileFingerprintSha256 = $fingerprint
        artifacts = @($artifacts)
    }
    New-Bytes (Join-Path $stage 'pipeline-report.json') ($utf8.GetBytes(($report | ConvertTo-Json -Depth 10)))
}

$stage = $null
$detectionInput = $null
$calibrationInput = $null
$steps = [ordered]@{ detection = $null; calibration = $null }
$fingerprint = $null
$exitCode = 2
$phase = 'preflight'
try {
    if (-not $IsWindows) { throw 'Windows is required.' }
    $detectionPath = Full-Path $DetectionJob
    $calibrationPath = Full-Path $CalibrationJob
    $detector = Full-Path $DetectorExecutable
    $calibrator = Full-Path $CalibratorExecutable
    $output = [IO.Path]::TrimEndingDirectorySeparator((Full-Path $OutputDirectory))
    $parent = [IO.Path]::GetDirectoryName($output)
    if (-not [IO.Directory]::Exists($parent) -or [IO.Path]::Exists($output)) { throw 'Output must be new with an existing parent.' }
    if (-not [IO.File]::Exists($detector) -or -not [IO.File]::Exists($calibrator)) { throw 'Both native tools are required.' }
    $detectionInput = Read-LockedJob $detectionPath (1MB)
    $calibrationInput = Read-LockedJob $calibrationPath (64KB)

    $candidate = $output + '.partial-' + [Guid]::NewGuid().ToString('N')
    # Both resolved move targets are siblings inside the explicitly requested
    # output parent. No recursive move/delete or arbitrary directory cleanup.
    if ([IO.Path]::GetDirectoryName($candidate) -cne $parent) { throw 'Invalid staging parent.' }
    # Unlike CreateDirectory (managed), the Win32 call refuses an existing
    # directory atomically. There is no wildcard expansion or reuse of partials.
    if (-not ('A0DraftCalibrationDirectory' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class A0DraftCalibrationDirectory {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CreateDirectoryW(string path, IntPtr attributes);
}
'@
    }
    if (-not [A0DraftCalibrationDirectory]::CreateDirectoryW($candidate, [IntPtr]::Zero)) { throw 'Cannot create new staging directory.' }
    $stage = $candidate
    foreach ($directory in @('inputs', 'logs')) { [void][IO.Directory]::CreateDirectory((Join-Path $stage $directory)) }
    New-Bytes (Join-Path $stage 'inputs/detection-job.json') $detectionInput.Bytes
    New-Bytes (Join-Path $stage 'inputs/calibration-job.json') $calibrationInput.Bytes
    $phase = 'detection'
    $steps.detection = Run-Tool $detector @('--job', $detectionPath, '--output', (Join-Path $stage 'observations')) 'detection'
    if ($steps.detection -notin @(0, 3)) { throw 'Detector input or IO error.' }
    Require-File 'observations/detection-report.json' (16MB)
    if ($steps.detection -eq 3) {
        if ([IO.File]::Exists((Join-Path $stage 'observations/session.json'))) { throw 'Failed detection emitted a session.' }
        $result = 'detection-failed'
        $exitCode = 3
    } else {
        Require-File 'observations/session.json' (8MB)
        $phase = 'calibration'
        $steps.calibration = Run-Tool $calibrator @('--session', (Join-Path $stage 'observations/session.json'),
            '--job', $calibrationPath, '--output', (Join-Path $stage 'draft')) 'calibration'
        if ($steps.calibration -notin @(0, 3)) { throw 'Calibrator input or IO error.' }
        Require-File 'draft/calibration-report.json' (16MB)
        $report = Json-Artifact 'draft/calibration-report.json'
        if ($report['schemaVersion'] -cne 'a0.draft-calibration-report.v1' -or $report['quality'] -cne 'not-assessed') { throw 'Unexpected calibration report.' }
        $sessionHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData(
            [IO.File]::ReadAllBytes((Join-Path $stage 'observations/session.json')))).ToLowerInvariant()
        if ($report['sessionSha256'] -cne $sessionHash) { throw 'Calibration report refers to another session.' }
        if ($steps.calibration -eq 3) {
            if ($report['result'] -cne 'no-profile' -or [IO.File]::Exists((Join-Path $stage 'draft/profile.json'))) { throw 'Failed calibration emitted a profile.' }
            $result = 'calibration-failed'
            $exitCode = 3
        } else {
            Require-File 'draft/profile.json' (256KB)
            $draftProfile = Json-Artifact 'draft/profile.json'
            $fingerprint = $report['profileFingerprintSha256']
            if ($report['result'] -cne 'draft-produced' -or $draftProfile['schemaVersion'] -cne '2.0.0' -or $draftProfile['status'] -cne 'draft' -or
                $fingerprint -cnotmatch '^[0-9a-f]{64}$' -or $draftProfile['profileId'] -cne $report['profileId'] -or
                $draftProfile['calibration']['provenanceId'] -cne $report['provenanceId']) { throw 'Draft/report contract mismatch.' }
            foreach ($field in @('approval', 'outputRaster', 'correctionEnvelope', 'qualityContract')) {
                if ($null -ne $draftProfile[$field]) { throw 'Draft contains an owner decision.' }
            }
            $result = 'draft-produced'
            $exitCode = 0
        }
    }
    $phase = 'publication'
    Save-Report $result $exitCode
    # Directory.Move has no replacement mode. A competing publisher can win;
    # this invocation then keeps its own partial directory for diagnosis.
    [IO.Directory]::Move($stage, $output)
    $stage = $null
    Write-Output "result=$result"
} catch {
    $exitCode = 2
    # Exception text and native logs can contain private input paths. The report
    # and public error code never embed those values. Logs remain local.
    if ($null -ne $stage) {
        try { New-Bytes (Join-Path $stage 'logs/pipeline.stderr.txt') ($utf8.GetBytes($_.ToString())) } catch { }
        try {
            $failure = [ordered]@{ schemaVersion = 'a0.draft-calibration-pipeline-error.v1'; result = 'pipeline-error'; exitCode = 2; phase = $phase }
            New-Bytes (Join-Path $stage 'pipeline-error.json') ($utf8.GetBytes(($failure | ConvertTo-Json)))
        } catch { }
        if (-not [IO.File]::Exists((Join-Path $stage 'pipeline-report.json'))) {
            try { Save-Report 'pipeline-error' 2 } catch { }
        }
    }
    [Console]::Error.WriteLine("error=draft-calibration-pipeline-input-tool-or-io-failed phase=$phase")
} finally {
    if ($null -ne $calibrationInput) { $calibrationInput.Stream.Dispose() }
    if ($null -ne $detectionInput) { $detectionInput.Stream.Dispose() }
}
exit $exitCode
