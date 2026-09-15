param(
    [Parameter(Mandatory = $true)]
    [string]$FixtureExe
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

function Assert-Property {
    param(
        [object]$Object,
        [string]$Name
    )
    Assert-Condition ($Object.PSObject.Properties.Name -contains $Name) `
        "Missing JSON property: $Name"
}

function Invoke-FixtureCase {
    param(
        [string]$Scenario,
        [string]$CaseRoot
    )
    $output = @(& $FixtureExe pc-direct-summary-fixture `
        --scenario $Scenario --artifacts $CaseRoot 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Fixture failed for $Scenario with exit $LASTEXITCODE`: $($output -join [Environment]::NewLine)"
    }
    $summaryLine = $output | Where-Object { $_ -like 'SummaryPath: *' } |
        Select-Object -Last 1
    Assert-Condition ($null -ne $summaryLine) `
        "Fixture did not report SummaryPath for $Scenario"
    $summaryPath = [string]$summaryLine.Substring('SummaryPath: '.Length)
    Assert-Condition (Test-Path -LiteralPath $summaryPath -PathType Leaf) `
        "Fixture summary was not saved for $Scenario"
    Assert-Condition (-not (Test-Path -LiteralPath "$summaryPath.partial")) `
        "Fixture left a partial summary for $Scenario"
    $raw = Get-Content -Raw -LiteralPath $summaryPath
    $parsed = $raw | ConvertFrom-Json
    return @{
        Raw = $raw
        Parsed = $parsed
    }
}

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $tempBase `
    ("A0CameraStitcher-pc-direct-summary-v2-e2e-{0}" -f [guid]::NewGuid().ToString('N'))
$resolvedRoot = [IO.Path]::GetFullPath($testRoot)
if (-not $resolvedRoot.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to create the E2E fixture outside the system temporary directory.'
}

try {
    New-Item -ItemType Directory -Path $resolvedRoot | Out-Null
    $unmeasured = Invoke-FixtureCase 'unmeasured' `
        (Join-Path $resolvedRoot 'unmeasured')
    $observed = Invoke-FixtureCase 'observed-zero' `
        (Join-Path $resolvedRoot 'observed-zero')

    foreach ($case in @($unmeasured, $observed)) {
        $summary = $case.Parsed
        Assert-Property $summary 'schemaVersion'
        Assert-Property $summary 'transportDiagnostics'
        Assert-Property $summary 'cardFallbackAttempted'
        Assert-Property $summary 'cameraDeleteAttempted'
        Assert-Property $summary 'automaticRetryCount'
        Assert-Condition ($summary.schemaVersion -eq 'phase0.pc-direct-summary.v2') `
            'Saved summary must retain the v2 schema identifier.'
        Assert-Condition ($summary.cardFallbackAttempted -eq $false) `
            'Saved summary must retain card fallback=false.'
        Assert-Condition ($summary.cameraDeleteAttempted -eq $false) `
            'Saved summary must retain camera delete=false.'
        Assert-Condition ($summary.automaticRetryCount -eq 0) `
            'Saved summary must retain automaticRetryCount=0.'
        Assert-Condition (-not $case.Raw.Contains('fixture-private-identity')) `
            'Saved summary must redact the fixture camera identity.'
        Assert-Condition (-not $case.Raw.Contains('fixture-sensitive-detail')) `
            'Saved summary must redact the fixture error detail.'
        Assert-Condition (-not $case.Raw.Contains($resolvedRoot)) `
            'Saved summary must not contain its absolute test location.'
    }

    $unmeasuredDiagnostics = $unmeasured.Parsed.transportDiagnostics
    Assert-Condition ($unmeasuredDiagnostics.measurementStarted -eq $false) `
        'Unmeasured diagnostics must persist measurementStarted=false.'
    foreach ($name in @(
            'callbackRegistered',
            'callbackActiveBeforeCapture',
            'sessionClosed',
            'captureCompleteCount',
            'addChildNotificationCount',
            'forcedEnumerationAttemptCount',
            'forcedEnumerationSuccessCount',
            'forcedEnumerationFailureCount',
            'distinctNotifiedCandidateCount',
            'distinctEnumeratedCandidateCount',
            'duplicateCandidateNotificationCount',
            'removedCandidateCount',
            'addChildInCardCount',
            'ignoredEventCount',
            'terminalSubreason')) {
        Assert-Property $unmeasuredDiagnostics $name
        Assert-Condition ($null -eq $unmeasuredDiagnostics.$name) `
            "Unmeasured diagnostics must persist $name as null."
    }

    $observedDiagnostics = $observed.Parsed.transportDiagnostics
    Assert-Condition ($observedDiagnostics.measurementStarted -eq $true) `
        'Observed diagnostics must persist measurementStarted=true.'
    Assert-Condition ($observedDiagnostics.callbackRegistered -eq $true -and
            $observedDiagnostics.callbackActiveBeforeCapture -eq $true -and
            $observedDiagnostics.sessionClosed -eq $true) `
        'Callback registration and lifetime state must survive JSON reload.'
    Assert-Condition ($observedDiagnostics.captureCompleteCount -eq 0) `
        'Observed zero CaptureComplete count must survive JSON reload.'
    Assert-Condition ($observedDiagnostics.addChildNotificationCount -eq 0) `
        'Observed zero AddChild count must survive JSON reload.'
    Assert-Condition ($observedDiagnostics.forcedEnumerationAttemptCount -eq 1 -and
            $observedDiagnostics.forcedEnumerationSuccessCount -eq 1 -and
            $observedDiagnostics.forcedEnumerationFailureCount -eq 0) `
        'Forced enumeration counts must survive JSON reload.'
    Assert-Condition ($observedDiagnostics.distinctNotifiedCandidateCount -eq 0 -and
            $observedDiagnostics.distinctEnumeratedCandidateCount -eq 0 -and
            $observed.Parsed.candidateCount -eq 0) `
        'Observed zero candidate counts must survive JSON reload.'
    Assert-Condition ($observedDiagnostics.duplicateCandidateNotificationCount -eq 0 -and
            $observedDiagnostics.removedCandidateCount -eq 0 -and
            $observedDiagnostics.addChildInCardCount -eq 0 -and
            $observedDiagnostics.ignoredEventCount -eq 0) `
        'Remaining observed zero event counts must survive JSON reload.'
    Assert-Condition ($observedDiagnostics.terminalSubreason -eq 'capture-complete-missing') `
        'Terminal subreason must survive JSON reload.'
    Assert-Condition (($observedDiagnostics.observationOrder -join ',') -eq
            'callback-registered,baseline-ready,capture-command-started,forced-enumeration-succeeded,session-closed') `
        'Bounded observation order must survive JSON reload.'

    Write-Output 'PC-direct summary v2 JSON reload E2E passed'
}
finally {
    if (Test-Path -LiteralPath $resolvedRoot) {
        $confirmedRoot = [IO.Path]::GetFullPath($resolvedRoot)
        if ($confirmedRoot.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -and
                (Split-Path -Leaf $confirmedRoot) -like 'A0CameraStitcher-pc-direct-summary-v2-e2e-*') {
            Remove-Item -LiteralPath $confirmedRoot -Recurse -Force
        }
        else {
            throw 'Refusing to remove an unexpected E2E fixture path.'
        }
    }
}
