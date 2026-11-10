#requires -Version 7.5
<#
Contract test of scripts/Compare-PlanarDistortionModels.ps1 on the synthetic
planar scenario (the --write-fixture inputs of m2_planar_calibration_contracts):
the true lens has all five coefficients, so a model that solves all five must
return it exactly, and the reduced models must visibly fit worse. The numeric
limits below are inputs of this test, not acceptance lines for real data.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CalibratorExecutable,
    [Parameter(Mandatory)][string]$ContractsExecutable
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$script = Join-Path $PSScriptRoot '..\scripts\Compare-PlanarDistortionModels.ps1'
$checks = 0; $failures = 0
function Check([bool]$Value, [string]$Label) {
    $script:checks++
    if (-not $Value) { $script:failures++; [Console]::Error.WriteLine("FAIL: $Label") }
}
function Refuses([scriptblock]$Action, [string]$Label) {
    $refused = $false
    try { & $Action | Out-Null } catch { $refused = $true }
    Check $refused $Label
}

$root = Join-Path ([IO.Path]::GetTempPath()) ("a0-model-compare-$PID")
if (Test-Path $root) { Remove-Item -LiteralPath $root -Recurse -Force }
$null = New-Item -ItemType Directory -Path $root
try {
    $fixture = Join-Path $root 'fixture'
    & $ContractsExecutable --write-fixture $fixture
    if ($LASTEXITCODE -ne 0) { throw 'The fixture could not be written.' }
    $session = Join-Path $fixture 'session.json'; $job = Join-Path $fixture 'job.json'
    $output = Join-Path $root 'out'

    & pwsh -NoProfile -File $script -Session $session -PlanarJob $job -CalibratorExecutable $CalibratorExecutable `
        -OutputDirectory $output -Models 'none,k1-k2,k1-k2-k3-p1-p2' | Out-Null
    Check ($LASTEXITCODE -eq 0) 'the comparison script succeeds'
    $comparison = Get-Content -LiteralPath (Join-Path $output 'comparison.json') -Raw | ConvertFrom-Json -Depth 100
    Check (@($comparison.models.PSObject.Properties.Name) -join ',' -ceq 'none,k1-k2,k1-k2-k3-p1-p2') 'the three models are compared in the given order'
    Check (-not (Test-Path (Join-Path $output 'work'))) 'the work folder is removed'

    # Full model: the known lens comes back, in the report, the band measure and every fold.
    $full = $comparison.models.'k1-k2-k3-p1-p2'
    Check ($full.solvedCoefficients -join ',' -ceq 'k1,k2,k3,p1,p2') 'the full model solves five coefficients'
    Check ($full.metrics.seamOverlap.maxPixels -lt 1e-5 -and $full.metrics.seamCheckerboardHeldOut.maxPixels -lt 1e-5) 'full model: the seam is about zero (1e-5 px, test input)'
    foreach ($camera in $full.metrics.cameras) {
        Check ($camera.heldOutResidual.rms -lt 1e-6) "full model: $($camera.camera) held-out residual is about zero (1e-6 px, test input)"
    }
    Check ($full.seamBand.bands.Count -eq 3 -and (($full.seamBand.bands | Measure-Object -Property n -Sum).Sum -eq $full.metrics.seamOverlap.n)) 'the bands partition the seam points'
    Check ([Math]::Abs($full.seamBand.reportOverlapDifference.mean) -lt 1e-9 -and [Math]::Abs($full.seamBand.reportOverlapDifference.max) -lt 1e-9 `
        -and [Math]::Abs($full.seamBand.reportOverlapDifference.p95) -lt 1e-9) 'the band measure reproduces the report seam (1e-9 px)'
    Check ($full.leaveOneOut.Count -eq 10) 'one fold per training view (5 + 5)'
    Check (@($full.leaveOneOut | Where-Object { $_.status -ceq 'planar-calibration-estimated' }).Count -eq 10) 'every fold is estimated'
    foreach ($summary in $full.leaveOneOutSummary) {
        Check ($summary.coefficients.k1.range -lt 1e-6 -and $summary.coefficients.k3.range -lt 1e-6 -and $summary.coefficients.p2.range -lt 1e-6) "full model: $($summary.camera) coefficients do not move between folds (1e-6, test input)"
        Check ($summary.heldOutRms.max -lt 1e-6 -and $summary.mappingWobbleMaxPixels.max -lt 1e-4) "full model: $($summary.camera) held-out and mapping wobble are about zero"
    }
    $fold = $full.leaveOneOut | Where-Object { $_.leftOutView -ceq 'A-T1' }
    Check ($null -ne $fold -and $fold.camera -ceq 'CAM-A' -and $fold.heldOutResidual.n -gt 0) 'a fold names the left-out view and measures it as held out'

    # Reduced models: the same lens does not fit, the unsolved coefficients are 0.
    $reduced = $comparison.models.'k1-k2'
    Check ($reduced.solvedCoefficients -join ',' -ceq 'k1,k2') 'the reduced model records its coefficients'
    foreach ($camera in $reduced.metrics.cameras) {
        $c = $camera.coefficients
        Check ($c.k3 -eq 0 -and $c.p1 -eq 0 -and $c.p2 -eq 0 -and $c.k1 -ne 0) "k1-k2: $($camera.camera) fixes k3 p1 p2 at 0"
        Check ($camera.fitResidual.rms -gt 0.01) "k1-k2: $($camera.camera) shows the model error in the fit residual"
    }
    $none = $comparison.models.none
    Check (@($none.metrics.cameras | Where-Object { $_.coefficients.k1 -ne 0 -or $_.coefficients.k2 -ne 0 }).Count -eq 0) 'none: no coefficient is solved'
    Check ($none.metrics.seamOverlap.maxPixels -gt $reduced.metrics.seamOverlap.maxPixels) 'none: the seam is worse than the two-coefficient model'

    # The five-coefficient model is the unchanged estimate: same bytes as a plain run of the job.
    $plain = Join-Path $root 'plain'
    & $CalibratorExecutable --session $session --planar-job $job --output $plain | Out-Null
    foreach ($name in 'profile.json', 'calibration-report.json') {
        $a = (Get-FileHash -LiteralPath (Join-Path $plain $name) -Algorithm SHA256).Hash
        $b = (Get-FileHash -LiteralPath (Join-Path $output "k1-k2-k3-p1-p2\draft\$name") -Algorithm SHA256).Hash
        Check ($a -ceq $b) "the five-coefficient model writes the plain run's $name"
    }
    Check (Test-Path (Join-Path $output 'k1-k2\draft\profile.json')) 'a reduced model writes its draft profile below its own folder'

    # Edge margin: the job gets the key, the fit points near a border are counted per model.
    $trimmedOut = Join-Path $root 'trimmed'
    & pwsh -NoProfile -File $script -Session $session -PlanarJob $job -CalibratorExecutable $CalibratorExecutable `
        -OutputDirectory $trimmedOut -Models 'k1-k2-k3-p1-p2' -SkipLeaveOneOut -EdgeMarginPixels 300 | Out-Null
    Check ($LASTEXITCODE -eq 0) 'the comparison script runs with an edge margin'
    $trimmed = Get-Content -LiteralPath (Join-Path $trimmedOut 'comparison.json') -Raw | ConvertFrom-Json -Depth 100
    $trimmedModel = $trimmed.models.'k1-k2-k3-p1-p2'
    Check ($trimmed.edgeMarginPixels -eq 300) 'the comparison records the edge margin'
    Check ($trimmedModel.edgeExcludedFitPoints.'CAM-A' -gt 0 -and $trimmedModel.edgeExcludedFitPoints.'CAM-B' -gt 0) 'the model records the fit points left out near the border'
    Check ($null -eq $full.edgeExcludedFitPoints) 'a run without a margin records no left-out points'
    Check ($trimmedModel.metrics.cameras[0].fitResidual.n -lt $full.metrics.cameras[0].fitResidual.n) 'fewer fit points are used with the margin'
    Check ([Math]::Abs($trimmedModel.metrics.cameras[0].coefficients.k1 - $full.metrics.cameras[0].coefficients.k1) -lt 1e-6) 'exact observations: the coefficients stay when points are left out (1e-6, test input)'

    # Excluded views: the named training view is gone, so the job's minimum of training views is no longer met.
    $droppedOut = Join-Path $root 'dropped'
    & pwsh -NoProfile -File $script -Session $session -PlanarJob $job -CalibratorExecutable $CalibratorExecutable `
        -OutputDirectory $droppedOut -Models 'k1-k2-k3-p1-p2' -SkipLeaveOneOut -ExcludeViews 'A-T1' | Out-Null
    Check ($LASTEXITCODE -eq 0) 'the comparison script runs with an excluded view'
    $dropped = Get-Content -LiteralPath (Join-Path $droppedOut 'comparison.json') -Raw | ConvertFrom-Json -Depth 100
    Check (($dropped.excludedViews -join ',') -ceq 'A-T1' -and $dropped.models.'k1-k2-k3-p1-p2'.metrics.cameras[0].status -ceq 'insufficient-views') 'an excluded training view is removed from the session'

    # Refusals.
    $other = Join-Path $root 'other'
    Refuses { & pwsh -NoProfile -File $script -Session $session -PlanarJob $job -CalibratorExecutable $CalibratorExecutable -OutputDirectory $other -Models none -SkipLeaveOneOut -ExcludeViews 'no-such-view'; if ($LASTEXITCODE) { throw 'exit' } } 'an unknown excluded view is refused'
    Remove-Item -LiteralPath $other -Recurse -Force -ErrorAction SilentlyContinue
    Refuses { & pwsh -NoProfile -File $script -Session $session -PlanarJob $job -CalibratorExecutable $CalibratorExecutable -OutputDirectory $other -Models k2-k1 -SkipLeaveOneOut; if ($LASTEXITCODE) { throw 'exit' } } 'coefficients out of order are refused'
    Refuses { & pwsh -NoProfile -File $script -Session $session -PlanarJob $job -CalibratorExecutable $CalibratorExecutable -OutputDirectory $other -Models k4 -SkipLeaveOneOut; if ($LASTEXITCODE) { throw 'exit' } } 'an unknown coefficient is refused'
    Refuses { & pwsh -NoProfile -File $script -Session $session -PlanarJob $job -CalibratorExecutable $CalibratorExecutable -OutputDirectory $output -Models none -SkipLeaveOneOut; if ($LASTEXITCODE) { throw 'exit' } } 'an existing model folder is not overwritten'
    $inside = Join-Path $PSScriptRoot 'compare-output-must-not-exist'
    Refuses { & pwsh -NoProfile -File $script -Session $session -PlanarJob $job -CalibratorExecutable $CalibratorExecutable -OutputDirectory $inside -Models none -SkipLeaveOneOut; if ($LASTEXITCODE) { throw 'exit' } } 'an output inside a git working tree is refused'
    Check (-not (Test-Path $inside)) 'nothing was created inside the working tree'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "$checks checks, $failures failures"
exit ($failures -eq 0 ? 0 : 1)
