[CmdletBinding()]
param(
    [string]$RepositoryRoot,
    [string]$SdkBuildRoot,
    [string]$SdklessBuildRoot,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
# GitHub Issue #96: 未初期化変数・存在しないプロパティ参照を握りつぶさない。
Set-StrictMode -Version Latest

function Assert-Condition {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

# GitHub Issue #96: 否定アサーション(「〜であってはならない」)を欠落プロパティで
# 空振りさせない。ConvertFrom-Json の PSCustomObject では、存在しないプロパティ参照が
# $null を返し -not $null = $true となって安全ゲートが素通りしていた(フィールドの
# リネーム/削除で全否定チェックが無効化される)。プロパティの存在を明示検証してから
# 偽であることを要求する。
function Assert-FalseProperty {
    param(
        [Parameter(Mandatory)]$Object,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Message
    )

    if ($null -eq $Object -or
        $Object.PSObject.Properties.Match($Name).Count -eq 0) {
        throw "$Message (expected boolean property '$Name' is missing)"
    }
    if ($Object.$Name) {
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
    Assert-FalseProperty $pairContract 'camera_session_overlap_allowed' 'Camera-session overlap must remain prohibited.'
    Assert-FalseProperty $pairContract 'automatic_retry' 'Automatic retry must remain prohibited.'
    Assert-FalseProperty $pairContract 'actual_shutter_synchronization_guaranteed' 'Actual shutter synchronization must not be claimed.'
    Assert-FalseProperty $pairContract 'real_pair_capture_executed' 'Curated evidence unexpectedly claims a real pair capture.'
    Assert-FalseProperty $pairContract 'real_identifiers_in_summary' 'Pair contract evidence must remain anonymous.'

    Assert-Condition ($binding.camera_alias -eq 'CAM-B') 'Curated hardware binding must identify only the CAM-B alias.'
    Assert-Condition ($binding.binding_state -eq 'bound-both') 'CAM-B must remain bound in both transports in the curated evidence.'
    Assert-FalseProperty $binding 'real_identifiers_in_evidence' 'Binding evidence must remain anonymous.'
    Assert-FalseProperty $binding 'capture_command_sent' 'Binding evidence must remain non-capture.'
    Assert-FalseProperty $binding 'card_access_performed' 'Binding evidence must remain non-card-access.'

    Assert-Condition ($spool.terminalState -eq 'Blocked' -and $spool.failureCategory -eq 'dual_identity_not_ready') 'Dual spool evidence must show a pre-card-access identity block.'
    Assert-FalseProperty $spool 'cardInspectionPerformed' 'Dual spool evidence crossed the permitted showcase boundary (card inspection).'
    Assert-FalseProperty $spool 'realIdentifiersIncluded' 'Dual spool evidence crossed the permitted showcase boundary (real identifiers).'

    Assert-Condition ($identity.sdkCameraCount -eq 1 -and $identity.wpdCameraCount -eq 1) 'Latest hardware evidence must show exactly one SDK and one WPD camera.'
    Assert-Condition ($identity.sdkCamACount -eq 0 -and $identity.wpdCamACount -eq 0) 'Latest hardware evidence unexpectedly claims CAM-A.'
    Assert-Condition ($identity.sdkCamBCount -eq 1 -and $identity.wpdCamBCount -eq 1) 'Latest hardware evidence must show CAM-B in both transports.'
    Assert-Condition ($identity.terminalState -eq 'Blocked' -and $identity.failureCategory -eq 'camera_count_mismatch') 'Latest dual identity evidence must remain fail-closed.'
    Assert-FalseProperty $identity 'captureCommandSent' 'Latest identity evidence crossed the non-destructive boundary (capture command).'
    Assert-FalseProperty $identity 'cardAccessPerformed' 'Latest identity evidence crossed the non-destructive boundary (card access).'
    Assert-FalseProperty $identity 'realIdentifiersIncluded' 'Latest identity evidence crossed the non-destructive boundary (real identifiers).'

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
