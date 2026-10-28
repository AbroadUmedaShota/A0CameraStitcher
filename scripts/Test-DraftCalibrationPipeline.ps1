#requires -Version 7.4
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$FixtureDirectory,
    [Parameter(Mandatory)][string]$ToolDirectory,
    [Parameter(Mandatory)][string]$EvidenceParent
)
$ErrorActionPreference = 'Stop'
$pipeline = Join-Path $PSScriptRoot 'Invoke-DraftCalibrationPipeline.ps1'
if (-not (Test-Path -LiteralPath $pipeline -PathType Leaf)) {
    Write-Error 'The public one-command calibration entry point is missing.' -ErrorAction Continue
    exit 1
}
Set-StrictMode -Version Latest
$script:checks = 0
$script:failures = 0
function Check([bool]$Value, [string]$Label) {
    $script:checks++
    if (-not $Value) { $script:failures++; Write-Host "FAIL: $Label" }
}
function Full([string]$Value) { return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Value) }
function Tree([string]$Path) { return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable }
function Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Write-Json([string]$Path, $Value) { [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 30), [Text.UTF8Encoding]::new($false)) }
$fixture = Full $FixtureDirectory
$tools = Full $ToolDirectory
$parent = Full $EvidenceParent
if (-not [IO.Directory]::Exists($parent)) { throw 'Evidence parent must exist.' }
if ((Tree (Join-Path $fixture 'truth-comparison.json'))['schema'] -cne 'a0.synthetic-full-calibration-test/1') { throw 'Use the retained synthetic #314 fixture.' }
$detector = Join-Path $tools 'A0CameraStitcher.FiducialDetector.exe'
$calibrator = Join-Path $tools 'A0CameraStitcher.DraftCalibrator.exe'
foreach ($tool in @($detector, $calibrator)) { if (-not [IO.File]::Exists($tool)) { throw 'Build the real native tools first.' } }
function Test-InGitTree([string]$Path) {
    for ($directory = $Path; -not [string]::IsNullOrEmpty($directory); $directory = [IO.Path]::GetDirectoryName($directory)) {
        if ([IO.Path]::Exists((Join-Path $directory '.git'))) { return $true }
    }
    return $false
}
# Short names: the native tools do not use long paths, and the pipeline refuses
# an output whose deepest native artifact would reach 260 characters.
$suffix = [Guid]::NewGuid().ToString('N').Substring(0, 12)
$root = Join-Path $parent ('cpt-' + $suffix)
# Longest output name below (22) + '.partial-' and 32 hex digits (41) + the
# calibrator's deepest artifact (49, see the pipeline) must stay below 260.
if ($root.Length + 1 + 22 + 41 + 49 -ge 260) { throw 'EvidenceParent is too deep for the native tools; use a shorter folder.' }
[void][IO.Directory]::CreateDirectory($root)
# The pipeline refuses jobs inside any git working tree, and the documented
# fixture and evidence folders are under build/ of a checkout. Filled-in copies
# of the jobs therefore go to a new folder under the user's temp directory.
$jobRoot = Join-Path ([IO.Path]::GetTempPath()) ('a0-cpt-jobs-' + $suffix)
if (Test-InGitTree $jobRoot) { throw 'The temp directory is inside a git working tree.' }
[void][IO.Directory]::CreateDirectory($jobRoot)
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixtureDetection = Join-Path $fixture 'detection-job.json'
$fixtureDetectionTree = Tree $fixtureDetection
foreach ($view in $fixtureDetectionTree['views']) { $view['image'] = Join-Path $fixture $view['image'] }
$goodDetection = Join-Path $jobRoot 'detection-job.json'
Write-Json $goodDetection $fixtureDetectionTree
$goodCalibration = Join-Path $jobRoot 'job.json'
[IO.File]::Copy((Join-Path $fixture 'job.json'), $goodCalibration, $false)

function Start-Pipeline([string]$Detection, [string]$Calibration, [string]$Output, [string]$Detector = $detector) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    # The running pwsh, whichever package or folder it came from.
    $info.FileName = [Environment]::ProcessPath
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    # Different from the job's base, to exercise original-job relative paths.
    $info.WorkingDirectory = $root
    foreach ($argument in @('-NoProfile', '-NonInteractive', '-File', $pipeline,
        '-DetectionJob', $Detection, '-CalibrationJob', $Calibration,
        '-DetectorExecutable', $Detector, '-CalibratorExecutable', $calibrator, '-OutputDirectory', $Output)) {
        $info.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    [void]$process.Start()
    return @{ Process = $process; Stdout = $process.StandardOutput.ReadToEndAsync(); Stderr = $process.StandardError.ReadToEndAsync() }
}
function Finish-Pipeline($Run) {
    try {
        $Run.Process.WaitForExit()
        return @{ ExitCode = $Run.Process.ExitCode; Stdout = $Run.Stdout.GetAwaiter().GetResult(); Stderr = $Run.Stderr.GetAwaiter().GetResult() }
    } finally { $Run.Process.Dispose() }
}
function Invoke-Pipeline([string]$Detection, [string]$Calibration, [string]$Output, [string]$Detector = $detector) {
    return Finish-Pipeline (Start-Pipeline $Detection $Calibration $Output $Detector)
}
function Check-Inventory([string]$Output) {
    $report = Tree (Join-Path $Output 'pipeline-report.json')
    $valid = $report['schemaVersion'] -ceq 'a0.draft-calibration-pipeline.v1' -and $report['quality'] -ceq 'not-assessed'
    foreach ($artifact in $report['artifacts']) {
        $path = Join-Path $Output $artifact['relativePath']
        $valid = $valid -and [IO.File]::Exists($path) -and (Hash $path) -ceq $artifact['sha256'] -and
            ([IO.FileInfo]::new($path).Length -eq $artifact['sizeBytes'])
    }
    Check $valid 'every recorded artifact has the independently computed bytes/size/hash'
    Check ($report['automaticRetryCount'] -eq 0 -and -not $report['inputImagesCopied']) 'no retry and no copied originals claimed'
    Check ((Get-Content -LiteralPath (Join-Path $Output 'pipeline-report.json') -Raw).IndexOf($root, [StringComparison]::OrdinalIgnoreCase) -lt 0) 'report has relative artifact paths only'
}
function Partials([string]$Output) {
    return ,@(Get-ChildItem -LiteralPath ([IO.Path]::GetDirectoryName($Output)) -Directory |
        Where-Object { $_.Name.StartsWith([IO.Path]::GetFileName($Output) + '.partial-', [StringComparison]::Ordinal) })
}

try {
    Write-Host 'integration: actual 18 JPEGs -> native detector -> native draft writer'
    $originals = @{}
    foreach ($view in (Tree $fixtureDetection)['views']) { $originals[$view['image']] = Hash (Join-Path $fixture $view['image']) }
    $success = Join-Path $root '成功 空白 [draft]'
    $run = Start-Pipeline $goodDetection $goodCalibration $success
    # Wait for snapshots, which are written after both original jobs are locked.
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $partial = Partials $success
        $lockedStage = @($partial | Where-Object { [IO.File]::Exists((Join-Path $_.FullName 'inputs/calibration-job.json')) })
        if ($lockedStage.Count -gt 0 -or $run.Process.HasExited) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    $bothLocked = $lockedStage.Count -eq 1
    foreach ($job in @($goodDetection, $goodCalibration)) {
        $blocked = $false
        try { $attempt = [IO.FileStream]::new($job, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite); $attempt.Dispose() }
        catch [IO.IOException] { $blocked = $true }
        $bothLocked = $bothLocked -and $blocked
    }
    Check $bothLocked 'both jobs deny concurrent writes while the native tools run'
    $result = Finish-Pipeline $run
    Check ($result.ExitCode -eq 0 -and $result.Stdout.Trim() -ceq 'result=draft-produced') 'actual native pipeline succeeds'
    if ($result.ExitCode -ne 0) { throw 'Positive integration failed; inspect retained partial/logs.' }
    Check ((Partials $success).Count -eq 0) 'success publishes one completed directory, no staging folder'
    Check-Inventory $success
    $report = Tree (Join-Path $success 'pipeline-report.json')
    Check ($report['steps']['detection'] -eq 0 -and $report['steps']['calibration'] -eq 0) 'both successful steps recorded'
    foreach ($pair in @(@('observations/session.json', 'session.json'), @('observations/detection-report.json', 'detection-report.json'),
        @('draft/profile.json', 'profile.json'), @('draft/calibration-report.json', 'calibration-report.json'))) {
        Check ((Hash (Join-Path $success $pair[0])) -ceq (Hash (Join-Path $fixture $pair[1]))) 'real CLI bytes agree with the independent #314 library run'
    }
    Check ((Hash (Join-Path $success 'inputs/detection-job.json')) -ceq (Hash $goodDetection) -and
        (Hash (Join-Path $success 'inputs/calibration-job.json')) -ceq (Hash $goodCalibration)) 'job snapshots preserve exact bytes'
    $draftProfile = Tree (Join-Path $success 'draft/profile.json')
    $draftReport = Tree (Join-Path $success 'draft/calibration-report.json')
    Check ($draftProfile['status'] -ceq 'draft' -and $null -eq $draftProfile['approval'] -and
        $report['profileFingerprintSha256'] -ceq $draftReport['profileFingerprintSha256']) 'existing draft/report format and canonical fingerprint retained'
    $unchanged = $true
    foreach ($image in $originals.Keys) { $unchanged = $unchanged -and (Hash (Join-Path $fixture $image)) -ceq $originals[$image] }
    Check $unchanged 'all 18 original JPEGs remain byte identical'
    $completedHash = Hash (Join-Path $success 'pipeline-report.json')
    Check ((Invoke-Pipeline $goodDetection $goodCalibration $success).ExitCode -eq 2) 'completed output cannot be reused'
    Check ((Hash (Join-Path $success 'pipeline-report.json')) -ceq $completedHash -and (Partials $success).Count -eq 0) 'collision leaves completed output unchanged and starts no tools'
    $empty = Join-Path $root 'existing-empty'
    [void][IO.Directory]::CreateDirectory($empty)
    Check ((Invoke-Pipeline $goodDetection $goodCalibration $empty).ExitCode -eq 2 -and @(Get-ChildItem -LiteralPath $empty).Count -eq 0) 'even an existing empty directory is refused'
    $noParent = Join-Path $root 'absent-parent/output'
    Check ((Invoke-Pipeline $goodDetection $goodCalibration $noParent).ExitCode -eq 2 -and -not [IO.Directory]::Exists([IO.Path]::GetDirectoryName($noParent))) 'missing parent is not created'
    $existingFile = Join-Path $root 'existing-file'
    [IO.File]::WriteAllText($existingFile, 'sentinel')
    Check ((Invoke-Pipeline $goodDetection $goodCalibration $existingFile).ExitCode -eq 2 -and
        [IO.File]::ReadAllText($existingFile) -ceq 'sentinel') 'existing file at output is unchanged'
    foreach ($case in @(@('empty-detection', 0, $true), @('large-detection', (1MB + 1), $true),
        @('empty-calibration', 0, $false), @('large-calibration', (64KB + 1), $false))) {
        $jobFile = Join-Path $jobRoot ($case[0] + '.json')
        [IO.File]::WriteAllBytes($jobFile, [byte[]]::new($case[1]))
        $output = Join-Path $root $case[0]
        $detection = if ($case[2]) { $jobFile } else { $goodDetection }
        $calibration = if ($case[2]) { $goodCalibration } else { $jobFile }
        Check ((Invoke-Pipeline $detection $calibration $output).ExitCode -eq 2 -and (Partials $output).Count -eq 0) 'empty and oversized jobs fail before staging'
    }

    Write-Host 'preflight: filled-in jobs inside a git working tree are refused'
    $refusedInside = 'error=draft-calibration-pipeline-job-inside-repository phase=preflight'
    # A tracked example inside this repository (read-only), and a path inside it
    # that does not exist: the refusal comes before the job is opened.
    $trackedCalibration = Join-Path $repositoryRoot 'samples/public/draft-calibration-job.v1.example.json'
    if (-not [IO.File]::Exists($trackedCalibration)) { throw 'The tracked calibration job example is missing.' }
    $absentDetection = Join-Path $repositoryRoot ('samples/public/detection-job-' + $suffix + '.json')
    # Another working tree (worktree or main checkout) is marked by a .git file.
    $otherTree = Join-Path $jobRoot 'other-tree'
    [void][IO.Directory]::CreateDirectory((Join-Path $otherTree 'jobs'))
    [IO.File]::WriteAllText((Join-Path $otherTree '.git'), 'gitdir: elsewhere')
    $otherTreeCalibration = Join-Path $otherTree 'jobs/job.json'
    [IO.File]::Copy($goodCalibration, $otherTreeCalibration, $false)
    foreach ($case in @(@('inside-calibration', $goodDetection, $trackedCalibration),
        @('inside-detection', $absentDetection, $goodCalibration),
        @('other-tree-calibration', $goodDetection, $otherTreeCalibration))) {
        $output = Join-Path $root $case[0]
        $result = Invoke-Pipeline $case[1] $case[2] $output
        Check ($result.ExitCode -eq 2 -and $result.Stderr.Trim() -ceq $refusedInside -and
            -not [IO.Path]::Exists($output) -and (Partials $output).Count -eq 0) ('job inside a git working tree is refused before staging: ' + $case[0])
    }
    Check (-not [IO.File]::Exists($absentDetection)) 'the refusal writes nothing into the repository'

    Write-Host 'preflight: an output inside a git working tree is refused, except below build/'
    $refusedOutput = 'error=draft-calibration-pipeline-output-inside-repository phase=preflight'
    foreach ($output in @((Join-Path $repositoryRoot ('samples/public/pipeline-' + $suffix)), (Join-Path $otherTree ('out-' + $suffix)))) {
        $result = Invoke-Pipeline $goodDetection $goodCalibration $output
        Check ($result.ExitCode -eq 2 -and $result.Stderr.Trim() -ceq $refusedOutput -and
            -not [IO.Path]::Exists($output) -and (Partials $output).Count -eq 0) 'output inside a git working tree is refused before staging'
    }
    # Below build/ the location check passes; a missing parent or the absent
    # tool then stops the run before staging, so nothing is written there.
    foreach ($output in @((Join-Path $repositoryRoot ('build/pipeline-' + $suffix)), (Join-Path $otherTree ('build/out-' + $suffix)))) {
        $result = Invoke-Pipeline $goodDetection $goodCalibration $output (Join-Path $tools 'absent.exe')
        Check ($result.ExitCode -eq 2 -and $result.Stderr.Trim() -ceq 'error=draft-calibration-pipeline-input-tool-or-io-failed phase=preflight' -and
            -not [IO.Path]::Exists($output)) 'output below build/ of a working tree passes the location check'
    }

    Write-Host 'preflight: an output too deep for the native tools is refused'
    $deepOutput = Join-Path $root ('d' * [Math]::Max(1, 220 - $root.Length))
    $result = Invoke-Pipeline $goodDetection $goodCalibration $deepOutput
    Check ($result.ExitCode -eq 2 -and $result.Stderr.Trim() -ceq 'error=draft-calibration-pipeline-output-path-too-long phase=preflight' -and
        -not [IO.Path]::Exists($deepOutput) -and (Partials $deepOutput).Count -eq 0) 'long output path is refused before staging'

    # Reuse only two synthetic references for negative jobs; actual Unicode paths.
    $negativeInputs = Join-Path $jobRoot '入力 設定'
    [void][IO.Directory]::CreateDirectory($negativeInputs)
    $references = Tree $fixtureDetection
    $references['views'] = @($references['views'] | Where-Object { $_['role'] -ceq 'reference' })
    foreach ($view in $references['views']) {
        $name = $view['camera'] + '.jpg'
        [IO.File]::Copy((Join-Path $fixture $view['image']), (Join-Path $negativeInputs $name), $false)
        $view['image'] = $name
    }
    $referenceJob = Join-Path $negativeInputs 'reference-job.json'
    Write-Json $referenceJob $references
    $noTraining = Join-Path $root 'no-training'
    $result = Invoke-Pipeline $referenceJob $goodCalibration $noTraining
    $report = Tree (Join-Path $noTraining 'pipeline-report.json')
    Check ($result.ExitCode -eq 3 -and $report['result'] -ceq 'calibration-failed' -and $report['steps']['detection'] -eq 0 -and $report['steps']['calibration'] -eq 3) 'reference-only JPEGs yield diagnostics, never a false draft'
    Check (-not [IO.File]::Exists((Join-Path $noTraining 'draft/profile.json')) -and [IO.File]::Exists((Join-Path $noTraining 'draft/calibration-report.json'))) 'calibration failure retains its report without profile'
    Check-Inventory $noTraining

    $references['views'][0]['anchors'] = @($references['views'][0]['anchors'] | Select-Object -First 2)
    $badAnchors = Join-Path $negativeInputs 'bad-anchors.json'
    Write-Json $badAnchors $references
    $failedDetection = Join-Path $root 'bad-anchors'
    $result = Invoke-Pipeline $badAnchors $goodCalibration $failedDetection
    $report = Tree (Join-Path $failedDetection 'pipeline-report.json')
    Check ($result.ExitCode -eq 3 -and $report['result'] -ceq 'detection-failed' -and $null -eq $report['steps']['calibration']) 'bad anchors stop before calibration'
    Check (-not [IO.File]::Exists((Join-Path $failedDetection 'observations/session.json')) -and -not [IO.Directory]::Exists((Join-Path $failedDetection 'draft'))) 'failed detection has no session, draft, or calibration logs'
    Check-Inventory $failedDetection

    foreach ($phase in @('detection', 'calibration')) {
        $malformed = Join-Path $negativeInputs ('malformed-' + $phase + '.json')
        [IO.File]::WriteAllText($malformed, '{}')
        $output = Join-Path $root ('malformed-' + $phase)
        $detection = if ($phase -eq 'detection') { $malformed } else { $referenceJob }
        $calibration = if ($phase -eq 'calibration') { $malformed } else { $goodCalibration }
        $result = Invoke-Pipeline $detection $calibration $output
        $partials = Partials $output
        Check ($result.ExitCode -eq 2 -and -not [IO.Directory]::Exists($output) -and $partials.Count -eq 1) 'native invalid input retains a partial, no finished output'
        $errorRecord = Tree (Join-Path $partials[0].FullName 'pipeline-error.json')
        Check ($errorRecord['exitCode'] -eq 2 -and $errorRecord['phase'] -ceq $phase) 'controlled error names the failing stage'
        Check ($result.Stderr -notlike "*$root*") 'public error text does not expose private paths'
    }
    $missingTool = Join-Path $root 'missing-tool'
    Check ((Invoke-Pipeline $goodDetection $goodCalibration $missingTool (Join-Path $tools 'absent.exe')).ExitCode -eq 2 -and (Partials $missingTool).Count -eq 0) 'missing native tool fails before staging'

    Write-Host 'integration: two actual detector processes compete to publish diagnostics'
    $race = Join-Path $root 'publication-race'
    $first = Start-Pipeline $badAnchors $goodCalibration $race
    $second = Start-Pipeline $badAnchors $goodCalibration $race
    $one = Finish-Pipeline $first
    $two = Finish-Pipeline $second
    Check ((@($one.ExitCode, $two.ExitCode) | Sort-Object) -join ',' -ceq '2,3') 'exactly one publisher succeeds and one refuses replacement'
    Check-Inventory $race
    $losers = Partials $race
    # A slow second startup may observe the completed output at preflight.
    # Otherwise it must retain its own staging directory after losing rename.
    Check ($losers.Count -le 1) 'competing invocation never reuses the winner directory'
    if ($losers.Count -eq 1) {
        Check ((Tree (Join-Path $losers[0].FullName 'pipeline-error.json'))['phase'] -ceq 'publication') 'rename loser records publication failure in its own partial'
    }
} catch {
    $script:failures++
    Write-Host "FAIL: $($_.Exception.Message)"
    Write-Host $_.ScriptStackTrace
}
Write-Host "draft_calibration_pipeline checks=$script:checks failures=$script:failures"
Write-Host "evidence=$root"
if ($script:failures -eq 0) {
    # Only the exact folder this run created under the temp directory; kept on
    # failure for diagnosis. Copied JPEGs may carry the read-only attribute.
    try {
        Get-ChildItem -LiteralPath $jobRoot -Recurse -File -Force | ForEach-Object { $_.Attributes = [IO.FileAttributes]::Normal }
        [IO.Directory]::Delete($jobRoot, $true)
        Write-Host 'jobs=removed'
    } catch {
        Write-Host "warning: could not remove the temporary job folder: $($_.Exception.Message)"
        Write-Host "jobs=$jobRoot"
    }
} else {
    Write-Host "jobs=$jobRoot"
}
exit $(if ($script:failures -eq 0) { 0 } else { 1 })
