#requires -Version 7.5
<#
.SYNOPSIS
Compare reduced Brown-Conrady models of the planar calibration (#374) on one observation session.
.DESCRIPTION
Runs A0CameraStitcher.DraftCalibrator.exe --planar-job once per model with the
job's optional distortionCoefficients key (a coefficient that is not named is
fixed to 0; a model named k1-k2-k3-p1-p2 is run without the key, which is the
estimate before the selection existed). Per model it records, from the report:
seam (overlap and checkerboardHeldOut, output pixels), per camera the fit and
held-out residual, the straightness, the coefficients, and the training view
residuals. It then measures two things the report does not carry:

1. Seam disagreement along the seam band. Each chart point detected in both
   overlap views is mapped to the sheet by both cameras (undistortion with the
   camera's K_nom and coefficients, then the inverse of documentToImage), as
   the calibrator does for its seam measure. The points are split into equal
   width parts of the band axis (sheet y for CAM-A-left-CAM-B-right, sheet x
   for CAM-A-top-CAM-B-bottom) and each part reports the count, the mean
   position, the disagreement (mean / p95 / max, output pixels) and the mean
   signed offset (CAM-A minus CAM-B). The script checks its own mean, p95 and
   max against the report's seam.overlap and records the difference.
2. Leave-one-out: for each camera and each training view, that view and the
   camera's validation view swap roles (the validation view becomes a training
   view with all its detected points; the training view becomes the validation
   view, its detected points split by the checkerboard rule), the model is
   fitted again, and the coefficients, the held-out residual, the seam and the
   mapping wobble are recorded. Mapping wobble is how far a raw pixel of a grid
   over the whole frame moves on the sheet, in output pixels at seamOutputDpi,
   against the same model's fit on the original roles.

Everything is written below OutputDirectory (outside any git working tree: the
session and the reports hold real measurements): <model>\draft\profile.json and
calibration-report.json for every model, and comparison.json. Nothing is
written to the calibration settings folder and no camera is used.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Session,
    [Parameter(Mandatory)][string]$PlanarJob,
    [Parameter(Mandatory)][string]$CalibratorExecutable,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string[]]$Models = @('none', 'k1', 'k1-k2', 'k1-k2-k3', 'k1-k2-p1-p2', 'k1-k2-k3-p1-p2'),
    [switch]$SkipLeaveOneOut,
    [ValidateRange(1, 20)][int]$Bands = 3,
    [switch]$KeepWork
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$coefficientNames = @('k1', 'k2', 'k3', 'p1', 'p2')

function Full-Path([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { throw 'Empty path.' }
    return [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Value))
}
function Test-InsideGitWorkingTree([string]$Path) {
    $directory = [IO.Path]::GetDirectoryName($Path)
    while (-not [string]::IsNullOrEmpty($directory)) {
        if ([IO.Path]::Exists((Join-Path $directory '.git'))) { return $true }
        $directory = [IO.Path]::GetDirectoryName($directory)
    }
    return $false
}
function Read-Json([string]$Path) {
    return [IO.File]::ReadAllText($Path, $utf8) | ConvertFrom-Json -Depth 100 -DateKind String
}
function Write-Text([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, $utf8)
}

# ---------------------------------------------------------------------------
# Models. Name -> coefficients to solve; $null (all five) runs without the key.

function Resolve-Model([string]$Name) {
    if ($Name -ceq 'none') { return [pscustomobject]@{ Name = $Name; Solved = @() } }
    $parts = $Name -split '-'
    foreach ($part in $parts) { if ($coefficientNames -cnotcontains $part) { throw "Unknown model '$Name'." } }
    if (@($parts | Select-Object -Unique).Count -ne $parts.Count) { throw "Repeated coefficient in model '$Name'." }
    $ordered = @($coefficientNames | Where-Object { $parts -ccontains $_ })
    if (($ordered -join '-') -cne $Name) { throw "Model '$Name' must list coefficients in k1 k2 k3 p1 p2 order." }
    return [pscustomobject]@{ Name = $Name; Solved = if ($ordered.Count -eq 5) { $null } else { $ordered } }
}
function New-ModelJob([string]$JobText, $Model) {
    if ($null -eq $Model.Solved) { return $JobText }
    $trimmed = $JobText.TrimEnd()
    if (-not $trimmed.EndsWith('}')) { throw 'The job is not a JSON object.' }
    if ($trimmed.Contains('"distortionCoefficients"')) { throw 'The base job already selects coefficients.' }
    $list = '[' + ((@($Model.Solved) | ForEach-Object { '"' + $_ + '"' }) -join ',') + ']'
    return $trimmed.Substring(0, $trimmed.Length - 1) + ',"distortionCoefficients":' + $list + '}'
}

# ---------------------------------------------------------------------------
# Calibrator.

function Invoke-Calibrator([string]$SessionPath, [string]$JobPath, [string]$OutputPath) {
    $null = & $CalibratorExecutable --session $SessionPath --planar-job $JobPath --output $OutputPath 2>&1
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3) { throw "DraftCalibrator exited with $LASTEXITCODE." }
    return $LASTEXITCODE
}

# ---------------------------------------------------------------------------
# Geometry (the same operations as calibration_lens_model.hpp and planar_calibration.cpp).

function Convert-RawToUndistorted($K, $D, [double]$U, [double]$V, [int]$MaxIterations, [double]$Tolerance) {
    $xd = ($U - $K.cx) / $K.fx; $yd = ($V - $K.cy) / $K.fy
    $x = $xd; $y = $yd
    for ($iteration = 0; $iteration -le $MaxIterations; $iteration++) {
        if (-not [double]::IsFinite($x) -or -not [double]::IsFinite($y)) { return $null }
        $r2 = $x * $x + $y * $y
        $radial = 1.0 + $r2 * ($D.k1 + $r2 * ($D.k2 + $r2 * $D.k3))
        $px = $x * $radial + 2.0 * $D.p1 * $x * $y + $D.p2 * ($r2 + 2.0 * $x * $x)
        $py = $y * $radial + $D.p1 * ($r2 + 2.0 * $y * $y) + 2.0 * $D.p2 * $x * $y
        $du = $px - $xd; $dv = $py - $yd
        $slope = $D.k1 + $r2 * (2.0 * $D.k2 + $r2 * 3.0 * $D.k3)
        $a = $radial + 2.0 * $x * $x * $slope + 2.0 * $D.p1 * $y + 6.0 * $D.p2 * $x
        $b = 2.0 * $x * $y * $slope + 2.0 * $D.p1 * $x + 2.0 * $D.p2 * $y
        $c = $radial + 2.0 * $y * $y * $slope + 6.0 * $D.p1 * $y + 2.0 * $D.p2 * $x
        $determinant = $a * $c - $b * $b
        if (-not ($determinant -gt 0.0)) { return $null }
        if ([Math]::Abs($du * $K.fx) -le $Tolerance -and [Math]::Abs($dv * $K.fy) -le $Tolerance) {
            return [double[]]@(($K.fx * $x + $K.cx), ($K.fy * $y + $K.cy))
        }
        if ($iteration -eq $MaxIterations) { break }
        $x -= ($c * $du - $b * $dv) / $determinant
        $y -= ($a * $dv - $b * $du) / $determinant
    }
    return $null
}
function Get-Inverse3([double[]]$M) {
    $adjugate = @(
        ($M[4] * $M[8] - $M[5] * $M[7]), ($M[2] * $M[7] - $M[1] * $M[8]), ($M[1] * $M[5] - $M[2] * $M[4]),
        ($M[5] * $M[6] - $M[3] * $M[8]), ($M[0] * $M[8] - $M[2] * $M[6]), ($M[2] * $M[3] - $M[0] * $M[5]),
        ($M[3] * $M[7] - $M[4] * $M[6]), ($M[1] * $M[6] - $M[0] * $M[7]), ($M[0] * $M[4] - $M[1] * $M[3]))
    $determinant = $M[0] * $adjugate[0] + $M[1] * $adjugate[3] + $M[2] * $adjugate[6]
    if (-not [double]::IsFinite($determinant) -or $determinant -eq 0) { throw 'Singular homography.' }
    return [double[]]@($adjugate | ForEach-Object { $_ / $determinant })
}
function Convert-ByHomography([double[]]$M, [double]$X, [double]$Y) {
    $w = $M[6] * $X + $M[7] * $Y + $M[8]
    return [double[]]@((($M[0] * $X + $M[1] * $Y + $M[2]) / $w), (($M[3] * $X + $M[4] * $Y + $M[5]) / $w))
}
function Get-CameraModel($CameraReport, $Policy) {
    $n = $CameraReport.nominalIntrinsics
    return [pscustomobject]@{
        K = [pscustomobject]@{ fx = [double]$n.fxPixels; fy = [double]$n.fyPixels; cx = [double]$n.cxPixels; cy = [double]$n.cyPixels }
        D = $CameraReport.distortion
        Inverse = Get-Inverse3 ([double[]]@($CameraReport.documentToImage | ForEach-Object { $_ }))
        MaxIterations = [int]$Policy.maxUndistortionIterations
        Tolerance = [double]$Policy.undistortionTolerancePixels
    }
}
# Raw pixel -> sheet (mm) through the camera's undistortion and documentToImage^-1; null if it cannot be undistorted.
function Convert-RawToSheet($Model, [double]$U, [double]$V) {
    $undistorted = Convert-RawToUndistorted $Model.K $Model.D $U $V $Model.MaxIterations $Model.Tolerance
    if ($null -eq $undistorted) { return $null }
    return Convert-ByHomography $Model.Inverse $undistorted[0] $undistorted[1]
}

# ---------------------------------------------------------------------------
# Statistics.

function Get-Percentile95([double[]]$Sorted) {
    $rank = [int][Math]::Ceiling(0.95 * $Sorted.Count)
    return $Sorted[$rank - 1]
}
function Get-Summary([double[]]$Values) {
    if ($Values.Count -eq 0) { return [pscustomobject]@{ n = 0; mean = $null; p95 = $null; max = $null } }
    $sorted = [double[]]($Values | Sort-Object)
    return [pscustomobject]@{ n = $Values.Count; mean = ($Values | Measure-Object -Average).Average; p95 = (Get-Percentile95 $sorted); max = $sorted[-1] }
}
function Get-Spread([double[]]$Values) {
    if ($Values.Count -eq 0) { return [pscustomobject]@{ n = 0; mean = $null; min = $null; max = $null; range = $null } }
    $m = $Values | Measure-Object -Average -Minimum -Maximum
    return [pscustomobject]@{ n = $Values.Count; mean = $m.Average; min = $m.Minimum; max = $m.Maximum; range = $m.Maximum - $m.Minimum }
}

# ---------------------------------------------------------------------------
# Seam along the band.

function Get-ViewById($SessionObject, [string]$Id) {
    foreach ($view in $SessionObject.views) { if ($view.viewId -ceq $Id) { return $view } }
    throw "View '$Id' is not in the session."
}
function Get-SeamBands($SessionObject, $Report, [int]$BandCount) {
    $policy = $Report.numericalPolicy
    $a = Get-CameraModel $Report.cameras[0] $policy
    $b = Get-CameraModel $Report.cameras[1] $policy
    $dpi = [double]$Report.seam.outputDpi
    $viewA = Get-ViewById $SessionObject $Report.overlapViews.'CAM-A'
    $viewB = Get-ViewById $SessionObject $Report.overlapViews.'CAM-B'
    $pixelsB = @{}
    foreach ($o in $viewB.observations) { if ($o.state -ceq 'detected') { $pixelsB[$o.pointId] = $o.pixel } }
    $axis = if ($Report.documentPlane.cameraOrder -ceq 'CAM-A-left-CAM-B-right') { 1 } else { 0 }
    $points = [Collections.Generic.List[object]]::new()
    foreach ($o in $viewA.observations) {
        if ($o.state -cne 'detected' -or -not $pixelsB.ContainsKey($o.pointId)) { continue }
        $sa = Convert-RawToSheet $a $o.pixel.x $o.pixel.y
        $sb = Convert-RawToSheet $b $pixelsB[$o.pointId].x $pixelsB[$o.pointId].y
        if ($null -eq $sa -or $null -eq $sb) { throw "A seam point could not be undistorted ($($o.pointId))." }
        $dx = $sa[0] - $sb[0]; $dy = $sa[1] - $sb[1]
        $points.Add([pscustomobject]@{
            Id = $o.pointId; Position = 0.5 * ($sa[$axis] + $sb[$axis]); Dx = $dx * $dpi / 25.4; Dy = $dy * $dpi / 25.4
            Pixels = [Math]::Sqrt($dx * $dx + $dy * $dy) * $dpi / 25.4
        })
    }
    if ($points.Count -eq 0) { return $null }
    $all = Get-Summary ([double[]]@($points | ForEach-Object { $_.Pixels }))
    $low = ($points | Measure-Object -Property Position -Minimum).Minimum
    $high = ($points | Measure-Object -Property Position -Maximum).Maximum
    $width = ($high - $low) / $BandCount
    $bands = for ($i = 0; $i -lt $BandCount; $i++) {
        $members = @($points | Where-Object {
            $index = if ($width -gt 0) { [Math]::Min($BandCount - 1, [int][Math]::Floor(($_.Position - $low) / $width)) } else { 0 }
            $index -eq $i })
        $summary = Get-Summary ([double[]]@($members | ForEach-Object { $_.Pixels }))
        [pscustomobject]@{
            band = $i; positionFromMm = $low + $i * $width; positionToMm = $low + ($i + 1) * $width
            n = $summary.n
            meanPositionMm = if ($members.Count) { ($members | Measure-Object -Property Position -Average).Average } else { $null }
            meanPixels = $summary.mean; p95Pixels = $summary.p95; maxPixels = $summary.max
            meanDxPixels = if ($members.Count) { ($members | Measure-Object -Property Dx -Average).Average } else { $null }
            meanDyPixels = if ($members.Count) { ($members | Measure-Object -Property Dy -Average).Average } else { $null }
        }
    }
    return [pscustomobject]@{
        axis = if ($axis -eq 1) { 'sheet-y' } else { 'sheet-x' }
        overall = $all
        reportOverlapDifference = [pscustomobject]@{
            mean = $all.mean - [double]$Report.seam.overlap.meanOutputPixels
            p95 = $all.p95 - [double]$Report.seam.overlap.p95OutputPixels
            max = $all.max - [double]$Report.seam.overlap.maxOutputPixels
        }
        bands = @($bands)
    }
}

# ---------------------------------------------------------------------------
# Report -> metrics.

function Get-ResidualPair($Residual) {
    if ($null -eq $Residual) { return $null }
    return [pscustomobject]@{ rms = [double]$Residual.rms; max = [double]$Residual.max; n = [int]$Residual.pointCount }
}
function Get-LineRms($Lines) {
    if ($null -eq $Lines -or $null -eq $Lines.undistorted) { return $null }
    return [pscustomobject]@{ rms = [double]$Lines.undistorted.rms; max = [double]$Lines.undistorted.max }
}
function Get-SeamPair($Seam) {
    if ($null -eq $Seam -or $Seam.outcome -cne 'measured') { return $null }
    return [pscustomobject]@{ n = [int]$Seam.pointCount; meanPixels = [double]$Seam.meanOutputPixels; p95Pixels = [double]$Seam.p95OutputPixels; maxPixels = [double]$Seam.maxOutputPixels }
}
function Get-ReportMetrics($Report) {
    $cameras = foreach ($c in $Report.cameras) {
        $trainingResiduals = [ordered]@{}
        foreach ($v in $c.views) { if ($v.role -ceq 'training' -and $null -ne $v.residual) { $trainingResiduals[$v.viewId] = [double]$v.residual.rms } }
        [pscustomobject]@{
            camera = $c.camera; status = $c.status
            coefficients = $c.distortion
            fitResidual = Get-ResidualPair $c.fitResidual
            overlapResidual = Get-ResidualPair $c.overlapResidual
            heldOutResidual = Get-ResidualPair $c.heldOutResidual
            straightnessFit = Get-LineRms $c.straightness.fit
            straightnessHeldOut = Get-LineRms $c.straightness.heldOut
            minimumRadialSlope = if ($c.distortionCheck) { [double]$c.distortionCheck.minimumRadialSlope } else { $null }
            maxRadiusSquared = if ($c.distortionCheck) { [double]$c.distortionCheck.maxRadiusSquared } else { $null }
            trainingViewRms = $trainingResiduals
            fitPointCells = $c.coverage.fitPointCells
        }
    }
    return [pscustomobject]@{
        result = $Report.result; rejection = $Report.rejection
        seamOverlap = Get-SeamPair $Report.seam.overlap
        seamCheckerboardHeldOut = Get-SeamPair $Report.seam.checkerboardHeldOut
        cameras = @($cameras)
    }
}

# ---------------------------------------------------------------------------
# Leave-one-out.

function Get-CheckerboardEven($SessionObject) {
    $columns = @($SessionObject.chartPoints | ForEach-Object { [double]$_.xMm } | Sort-Object -Unique)
    $rows = @($SessionObject.chartPoints | ForEach-Object { [double]$_.yMm } | Sort-Object -Unique)
    $even = @{}
    foreach ($p in $SessionObject.chartPoints) {
        $even[$p.pointId] = (([Array]::IndexOf($columns, [double]$p.xMm)) + ([Array]::IndexOf($rows, [double]$p.yMm))) % 2 -eq 0
    }
    return $even
}
function Copy-ViewProperties($View, [string[]]$Skip) {
    $copy = [ordered]@{}
    foreach ($property in $View.PSObject.Properties) { if ($Skip -cnotcontains $property.Name) { $copy[$property.Name] = $property.Value } }
    return $copy
}
function Get-DetectedIds($View) {
    return [object[]]@($View.observations | Where-Object { $_.state -ceq 'detected' } | ForEach-Object { $_.pointId })
}
# Session text in which `TrainingViewId` and the camera's validation view swap roles.
function New-SwappedSessionText($SessionObject, [string]$CameraAlias, [string]$TrainingViewId, $Even) {
    $validation = @($SessionObject.views | Where-Object { $_.camera -ceq $CameraAlias -and $_.role -ceq 'validation' })
    if ($validation.Count -ne 1) { throw "Camera $CameraAlias needs exactly one validation view for the swap." }
    $validationId = $validation[0].viewId
    $views = foreach ($view in $SessionObject.views) {
        if ($view.viewId -ceq $TrainingViewId) {
            if ($view.role -cne 'training' -or $view.camera -cne $CameraAlias) { throw "View '$TrainingViewId' is not a training view of $CameraAlias." }
            $ids = Get-DetectedIds $view
            $copy = Copy-ViewProperties $view @('usedPointIds', 'observations', 'role')
            $copy['role'] = 'validation'
            $copy['posePointIds'] = [object[]]@($ids | Where-Object { $Even[$_] })
            $copy['measurementPointIds'] = [object[]]@($ids | Where-Object { -not $Even[$_] })
            $copy['observations'] = $view.observations
            [pscustomobject]$copy
        } elseif ($view.viewId -ceq $validationId) {
            $copy = Copy-ViewProperties $view @('posePointIds', 'measurementPointIds', 'observations', 'role')
            $copy['role'] = 'training'
            $copy['usedPointIds'] = Get-DetectedIds $view
            $copy['observations'] = $view.observations
            [pscustomobject]$copy
        } else { $view }
    }
    $swapped = [ordered]@{}
    foreach ($property in $SessionObject.PSObject.Properties) { $swapped[$property.Name] = if ($property.Name -ceq 'views') { [object[]]@($views) } else { $property.Value } }
    return [pscustomobject]$swapped | ConvertTo-Json -Depth 100 -Compress
}
# Mapping wobble: raw pixels of a grid over the whole frame -> sheet, fold against base, in output pixels.
function Get-MappingWobble($BaseReport, $FoldReport, [int]$CameraIndex, [int]$Width, [int]$Height) {
    $dpi = [double]$BaseReport.seam.outputDpi
    $base = Get-CameraModel $BaseReport.cameras[$CameraIndex] $BaseReport.numericalPolicy
    $fold = Get-CameraModel $FoldReport.cameras[$CameraIndex] $FoldReport.numericalPolicy
    $distances = [Collections.Generic.List[double]]::new()
    $failed = 0
    foreach ($iy in 0..6) { foreach ($ix in 0..8) {
        $u = $ix * ($Width - 1) / 8.0; $v = $iy * ($Height - 1) / 6.0
        $p = Convert-RawToSheet $base $u $v; $q = Convert-RawToSheet $fold $u $v
        if ($null -eq $p -or $null -eq $q) { $failed++; continue }
        $distances.Add([Math]::Sqrt(($p[0] - $q[0]) * ($p[0] - $q[0]) + ($p[1] - $q[1]) * ($p[1] - $q[1])) * $dpi / 25.4)
    } }
    $summary = Get-Summary ([double[]]$distances.ToArray())
    return [pscustomobject]@{ gridPoints = 63; failedPoints = $failed; meanPixels = $summary.mean; maxPixels = $summary.max }
}

# ---------------------------------------------------------------------------
# Main.

$sessionPath = Full-Path $Session
$jobPath = Full-Path $PlanarJob
$calibrator = Full-Path $CalibratorExecutable
$CalibratorExecutable = $calibrator
$output = Full-Path $OutputDirectory
foreach ($path in @($sessionPath, $jobPath, $calibrator)) { if (-not [IO.File]::Exists($path)) { throw 'An input file is missing.' } }
if (Test-InsideGitWorkingTree $output) { throw 'The output holds real measurements and must be outside any git working tree.' }
if (-not [IO.Directory]::Exists($output)) {
    if (-not [IO.Directory]::Exists([IO.Path]::GetDirectoryName($output))) { throw 'The parent of the output directory must exist.' }
    $null = New-Item -ItemType Directory -Path $output
}
# `pwsh -File` passes -Models a,b as the one string "a,b".
$modelList = @($Models | ForEach-Object { $_ -split ',' } | Where-Object { $_.Trim() } | ForEach-Object { Resolve-Model $_.Trim() })
if (@($modelList.Name | Select-Object -Unique).Count -ne $modelList.Count) { throw 'A model is listed twice.' }
foreach ($model in $modelList) { if ([IO.Directory]::Exists((Join-Path $output $model.Name))) { throw "The output for model '$($model.Name)' already exists." } }

$jobText = [IO.File]::ReadAllText($jobPath, $utf8)
$sessionObject = Read-Json $sessionPath
$even = Get-CheckerboardEven $sessionObject
$work = Join-Path $output 'work'
if ([IO.Directory]::Exists($work)) { throw 'The work directory already exists.' }
$null = New-Item -ItemType Directory -Path $work

$comparison = [ordered]@{
    schema = 'a0.planar-distortion-model-comparison.v1'
    note = 'Report values only; no acceptance threshold is applied.'
    models = [ordered]@{}
}
try {
    foreach ($model in $modelList) {
        Write-Host "model $($model.Name)"
        $modelDirectory = Join-Path $output $model.Name
        $null = New-Item -ItemType Directory -Path $modelDirectory
        $modelJob = Join-Path $work "$($model.Name).job.json"
        Write-Text $modelJob (New-ModelJob $jobText $model)
        $draft = Join-Path $modelDirectory 'draft'
        $exitCode = Invoke-Calibrator $sessionPath $modelJob $draft
        $baseReport = Read-Json (Join-Path $draft 'calibration-report.json')
        $entry = [ordered]@{
            solvedCoefficients = [string[]]@(if ($null -eq $model.Solved) { $coefficientNames } else { $model.Solved })
            exitCode = $exitCode
            profileProduced = [IO.File]::Exists((Join-Path $draft 'profile.json'))
            metrics = Get-ReportMetrics $baseReport
            seamBand = $null
            leaveOneOut = @()
        }
        if ($baseReport.seam.overlap.outcome -ceq 'measured') { $entry.seamBand = Get-SeamBands $sessionObject $baseReport $Bands }
        if (-not $SkipLeaveOneOut -and $baseReport.cameras[0].status -ceq 'planar-calibration-estimated' -and $baseReport.cameras[1].status -ceq 'planar-calibration-estimated') {
            $folds = [Collections.Generic.List[object]]::new()
            foreach ($cameraIndex in 0, 1) {
                $alias = if ($cameraIndex -eq 0) { 'CAM-A' } else { 'CAM-B' }
                $width = [int]$sessionObject.cameras.$alias.widthPixels; $height = [int]$sessionObject.cameras.$alias.heightPixels
                $trainingIds = @($sessionObject.views | Where-Object { $_.camera -ceq $alias -and $_.role -ceq 'training' } | ForEach-Object { $_.viewId })
                foreach ($trainingId in $trainingIds) {
                    $foldName = "$($model.Name)-$trainingId"
                    $foldSession = Join-Path $work "$foldName.session.json"
                    Write-Text $foldSession (New-SwappedSessionText $sessionObject $alias $trainingId $even)
                    $foldDirectory = Join-Path $work "$foldName.out"
                    $record = [ordered]@{ camera = $alias; leftOutView = $trainingId; status = 'not-run' }
                    try {
                        $null = Invoke-Calibrator $foldSession $modelJob $foldDirectory
                        $foldReport = Read-Json (Join-Path $foldDirectory 'calibration-report.json')
                        $metrics = Get-ReportMetrics $foldReport
                        $camera = $metrics.cameras[$cameraIndex]
                        $record.status = $camera.status
                        $record.coefficients = $camera.coefficients
                        $record.fitResidual = $camera.fitResidual
                        $record.heldOutResidual = $camera.heldOutResidual
                        $record.straightnessHeldOut = $camera.straightnessHeldOut
                        $record.seamOverlap = $metrics.seamOverlap
                        $record.seamCheckerboardHeldOut = $metrics.seamCheckerboardHeldOut
                        if ($camera.status -ceq 'planar-calibration-estimated') {
                            $record.mappingWobble = Get-MappingWobble $baseReport $foldReport $cameraIndex $width $height
                        }
                    } catch { $record.status = 'failed: ' + $_.Exception.Message }
                    $folds.Add([pscustomobject]$record)
                    if (-not $KeepWork) { Remove-Item -LiteralPath $foldSession, $foldDirectory -Recurse -Force -ErrorAction SilentlyContinue }
                    Write-Host "  fold $alias $trainingId $($record.status)"
                }
            }
            $entry.leaveOneOut = @($folds)
            $entry.leaveOneOutSummary = @('CAM-A', 'CAM-B' | ForEach-Object {
                $alias = $_
                $own = @($folds | Where-Object { $_.camera -ceq $alias -and $_.status -ceq 'planar-calibration-estimated' })
                $coefficientRanges = [ordered]@{}
                foreach ($name in $coefficientNames) { $coefficientRanges[$name] = Get-Spread ([double[]]@($own | ForEach-Object { $_.coefficients.$name })) }
                [pscustomobject]@{
                    camera = $alias; foldsEstimated = $own.Count
                    coefficients = $coefficientRanges
                    heldOutRms = Get-Spread ([double[]]@($own | ForEach-Object { $_.heldOutResidual.rms }))
                    heldOutMax = Get-Spread ([double[]]@($own | ForEach-Object { $_.heldOutResidual.max }))
                    seamOverlapP95Pixels = Get-Spread ([double[]]@($own | ForEach-Object { $_.seamOverlap.p95Pixels }))
                    seamOverlapMaxPixels = Get-Spread ([double[]]@($own | ForEach-Object { $_.seamOverlap.maxPixels }))
                    seamCheckerboardMaxPixels = Get-Spread ([double[]]@($own | ForEach-Object { $_.seamCheckerboardHeldOut.maxPixels }))
                    mappingWobbleMeanPixels = Get-Spread ([double[]]@($own | ForEach-Object { $_.mappingWobble.meanPixels }))
                    mappingWobbleMaxPixels = Get-Spread ([double[]]@($own | ForEach-Object { $_.mappingWobble.maxPixels }))
                }
            })
        }
        $comparison.models[$model.Name] = [pscustomobject]$entry
    }
    Write-Text (Join-Path $output 'comparison.json') ($comparison | ConvertTo-Json -Depth 100)
} finally {
    if (-not $KeepWork) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host "comparison.json written ($($modelList.Count) models)"
