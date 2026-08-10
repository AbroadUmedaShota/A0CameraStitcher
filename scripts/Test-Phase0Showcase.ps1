[CmdletBinding()]
param(
    [string]$RepositoryRoot,
    [string]$SdkBuildRoot,
    [string]$SdklessBuildRoot,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Read-JsonFile {
    param([string]$Path)

    Assert-Condition (Test-Path -LiteralPath $Path -PathType Leaf) "Required file is missing: $Path"
    return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
}

function Invoke-CheckedCommand {
    param(
        [string]$Command,
        [string[]]$Arguments,
        [string]$FailureMessage
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage (exit $LASTEXITCODE)"
    }
}

try {
    if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
        $RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
    } else {
        $RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
    }

    if ([string]::IsNullOrWhiteSpace($SdkBuildRoot)) {
        $SdkBuildRoot = Join-Path $RepositoryRoot 'build-sdk'
    }
    if ([string]::IsNullOrWhiteSpace($SdklessBuildRoot)) {
        $SdklessBuildRoot = Join-Path $RepositoryRoot 'build-sdkless'
    }

    $showcaseDocument = Join-Path $RepositoryRoot 'docs\PHASE0_SHOWCASE.md'
    $pairContractPath = Join-Path $RepositoryRoot 'docs\evidence\phase0\run-1786174618989-1\hybrid-pair-contract-summary.json'
    $bindingPath = Join-Path $RepositoryRoot 'docs\evidence\phase0\run-1786182705745-1\cross-transport-binding-command-summary.json'
    $spoolPath = Join-Path $RepositoryRoot 'docs\evidence\phase0\run-1786183481065-1\dual-spool-verification-summary.json'
    $identityPath = Join-Path $RepositoryRoot 'docs\evidence\phase0\run-1786184898081-1\dual-identity-verification-summary.json'

    Assert-Condition (Test-Path -LiteralPath $showcaseDocument -PathType Leaf) 'Showcase document is missing.'
    $pairContract = Read-JsonFile $pairContractPath
    $binding = Read-JsonFile $bindingPath
    $spool = Read-JsonFile $spoolPath
    $identity = Read-JsonFile $identityPath

    Assert-Condition ($pairContract.schema_version -eq 'phase0.hybrid-pair-contract-summary.v1') 'Unexpected pair contract schema.'
    Assert-Condition ($pairContract.scope -eq 'software-only-fake-transports') 'Pair contract must remain software-only.'
    Assert-Condition ($pairContract.software_run_100_pairs_completed -eq 100) 'The synthetic 100-pair contract is incomplete.'
    Assert-Condition (-not $pairContract.camera_session_overlap_allowed) 'Camera-session overlap must remain prohibited.'
    Assert-Condition (-not $pairContract.automatic_retry) 'Automatic retry must remain prohibited.'
    Assert-Condition (-not $pairContract.actual_shutter_synchronization_guaranteed) 'Actual shutter synchronization must not be claimed.'
    Assert-Condition (-not $pairContract.real_pair_capture_executed) 'Curated evidence unexpectedly claims a real pair capture.'
    Assert-Condition (-not $pairContract.real_identifiers_in_summary) 'Pair contract evidence must remain anonymous.'

    Assert-Condition ($binding.camera_alias -eq 'CAM-B') 'Curated hardware binding must identify only the CAM-B alias.'
    Assert-Condition ($binding.binding_state -eq 'bound-both') 'CAM-B must remain bound in both transports in the curated evidence.'
    Assert-Condition (-not $binding.real_identifiers_in_evidence) 'Binding evidence must remain anonymous.'
    Assert-Condition (-not $binding.capture_command_sent -and -not $binding.card_access_performed) 'Binding evidence must remain non-capture and non-card-access.'

    Assert-Condition ($spool.terminalState -eq 'Blocked' -and $spool.failureCategory -eq 'dual_identity_not_ready') 'Dual spool evidence must show a pre-card-access identity block.'
    Assert-Condition (-not $spool.cardInspectionPerformed -and -not $spool.realIdentifiersIncluded) 'Dual spool evidence crossed the permitted showcase boundary.'

    Assert-Condition ($identity.sdkCameraCount -eq 1 -and $identity.wpdCameraCount -eq 1) 'Latest hardware evidence must show exactly one SDK and one WPD camera.'
    Assert-Condition ($identity.sdkCamACount -eq 0 -and $identity.wpdCamACount -eq 0) 'Latest hardware evidence unexpectedly claims CAM-A.'
    Assert-Condition ($identity.sdkCamBCount -eq 1 -and $identity.wpdCamBCount -eq 1) 'Latest hardware evidence must show CAM-B in both transports.'
    Assert-Condition ($identity.terminalState -eq 'Blocked' -and $identity.failureCategory -eq 'camera_count_mismatch') 'Latest dual identity evidence must remain fail-closed.'
    Assert-Condition (-not $identity.captureCommandSent -and -not $identity.cardAccessPerformed -and -not $identity.realIdentifiersIncluded) 'Latest identity evidence crossed the non-destructive boundary.'

    $sdkBuildResult = 'skipped'
    $sdklessBuildResult = 'skipped'
    $sdkCTestResult = 'skipped'
    $sdklessCTestResult = 'skipped'

    if (-not $SkipBuild) {
        $cmake = Get-Command cmake -ErrorAction Stop
        $ctest = Get-Command ctest -ErrorAction Stop
        Assert-Condition (Test-Path -LiteralPath (Join-Path $SdkBuildRoot 'CMakeCache.txt') -PathType Leaf) 'SDK-enabled build tree is not configured.'
        Assert-Condition (Test-Path -LiteralPath (Join-Path $SdklessBuildRoot 'CMakeCache.txt') -PathType Leaf) 'SDK-less build tree is not configured.'

        Invoke-CheckedCommand $cmake.Source @('--build', $SdkBuildRoot, '--config', 'Release') 'SDK-enabled Release build failed.'
        $sdkBuildResult = 'passed'
        Invoke-CheckedCommand $ctest.Source @('--test-dir', $SdkBuildRoot, '-C', 'Release', '--output-on-failure') 'SDK-enabled Release CTest failed.'
        $sdkCTestResult = 'passed'
        Invoke-CheckedCommand $cmake.Source @('--build', $SdklessBuildRoot, '--config', 'Release') 'SDK-less Release build failed.'
        $sdklessBuildResult = 'passed'
        Invoke-CheckedCommand $ctest.Source @('--test-dir', $SdklessBuildRoot, '-C', 'Release', '--output-on-failure') 'SDK-less Release CTest failed.'
        $sdklessCTestResult = 'passed'
    }

    [pscustomobject]@{
        ShowcaseState = 'SOFTWARE_CONTRACT_READY_HARDWARE_PENDING'
        SdkBuild = $sdkBuildResult
        SdklessBuild = $sdklessBuildResult
        SdkCTest = $sdkCTestResult
        SdklessCTest = $sdklessCTestResult
        RealDualCaptureExecuted = $false
        LatestHardwareState = 'SINGLE_CAM_A_IDENTITY_V3_BOUND_SDK_STATUS_RERUN_PENDING'
        ActualShutterSyncGuaranteed = $false
        PhysicalPowerCycleRequired = $false
        HardwareCommandsExecuted = $false
    } | Format-List

    exit 0
} catch {
    Write-Error $_
    exit 1
}
