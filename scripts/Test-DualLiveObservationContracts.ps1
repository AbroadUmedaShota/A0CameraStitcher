#requires -Version 7.4
[CmdletBinding()]
param(
    [string]$Evaluator = (Join-Path $PSScriptRoot 'Test-DualLiveObservation.ps1'),
    [string]$EvidenceParent = (Join-Path $PSScriptRoot '../build/reference')
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
if (-not (Test-Path -LiteralPath $Evaluator -PathType Leaf)) { throw 'Observation evaluator missing' }
$root = Join-Path $EvidenceParent ('dual-live-observation-327-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($root)
$script:checks = 0
function Check([bool]$Value, [string]$Message) {
    $script:checks++
    if (-not $Value) { throw "FAIL: $Message" }
}
function Fixture {
    # Synthetic policy and receipt times, not product fps or hardware evidence.
    return @{
        schemaVersion = 'a0.dual-live-observation.v1'; source = 'simulation'
        clock = 'controller-monotonic-ms'
        window = @{ startMs = 100; endMs = 300; maxFrameGapMs = 100; minFramesPerCamera = 3 }
        events = @(
            @{ alias = 'CAM-A'; type = 'opened'; atMs = 0 }
            @{ alias = 'CAM-B'; type = 'opened'; atMs = 10 }
            @{ alias = 'CAM-A'; type = 'frame'; atMs = 100; sequence = 1 }
            @{ alias = 'CAM-B'; type = 'frame'; atMs = 100; sequence = 1 }
            @{ alias = 'CAM-A'; type = 'frame'; atMs = 200; sequence = 2 }
            @{ alias = 'CAM-B'; type = 'frame'; atMs = 200; sequence = 2 }
            @{ alias = 'CAM-A'; type = 'frame'; atMs = 300; sequence = 3 }
            @{ alias = 'CAM-B'; type = 'frame'; atMs = 300; sequence = 3 }
            @{ alias = 'CAM-A'; type = 'stopped'; atMs = 310 }
            @{ alias = 'CAM-B'; type = 'stopped'; atMs = 320 }
            @{ alias = 'CAM-A'; type = 'closed'; atMs = 330 }
            @{ alias = 'CAM-B'; type = 'closed'; atMs = 340 }
        )
    }
}
function Run([string]$Name, $InputNode, [int]$ExpectedExit, [string]$Reason = '', [switch]$Raw) {
    $path = Join-Path $root ($Name + '.json')
    $json = if ($Raw) { $InputNode } else { ConvertTo-Json -InputObject $InputNode -Depth 20 -Compress }
    [IO.File]::WriteAllText($path, $json)
    $hash = (Get-FileHash -LiteralPath $path).Hash
    $output = & $Evaluator -Path $path
    $code = $LASTEXITCODE
    Check ($code -eq $ExpectedExit) "$Name exit $code expected $ExpectedExit"
    $report = ($output -join "`n") | ConvertFrom-Json -AsHashtable
    Check ($report.schemaVersion -ceq 'a0.dual-live-observation-result.v1') "$Name result version"
    Check ($report.hardwareReady -eq $false -and $report.physicalIdentityVerified -eq $false -and
        $report.shutterSynchronized -eq $false) "$Name cannot authorize hardware or prove identity/sync"
    Check ($report.bothUpdated -eq ($ExpectedExit -eq 0)) "$Name verdict"
    Check ($report.inputSha256 -ceq $hash.ToLowerInvariant() -or
        ($Name -ceq 'oversize-input' -and $null -eq $report.inputSha256)) "$Name exact input binding"
    if ($ExpectedExit -eq 2) {
        Check ($report.cameras.Count -eq 0 -and $null -eq $report.window -and
            -not $report.allClosed -and $null -eq $report.source) "$Name no partial success on malformed log"
    }
    if ($Reason) { Check ($report.reasons -ccontains $Reason) "$Name expected reason $Reason" }
    Check ((Get-FileHash -LiteralPath $path).Hash -ceq $hash) "$Name input unchanged"
    [IO.File]::WriteAllText((Join-Path $root ($Name + '-result.json')), ($output -join "`n"))
    return $report
}
$ok = Run 'both-updated' (Fixture) 0
Check ($ok.cameras.Count -eq 2 -and $ok.cameras[0].frameCount -eq 3 -and
    $ok.cameras[1].frameCount -eq 3 -and $ok.cameras[0].maximumGapMs -eq 100 -and
    $ok.cameras[1].maximumGapMs -eq 100 -and $ok.allClosed) 'independent count and interval oracle'
Check ($ok.window.startMs -eq 100 -and $ok.window.endMs -eq 300 -and $ok.window.maxFrameGapMs -eq 100 -and
    $ok.window.minFramesPerCamera -eq 3) 'explicit policy retained'
$hardware = Fixture; $hardware.source = 'hardware'
$h = Run 'hardware-label-is-not-proof' $hardware 0
Check ($h.source -ceq 'hardware' -and -not $h.hardwareReady) 'untrusted source label never grants readiness'
$bad = Fixture; $bad.events = @($bad.events | Where-Object alias -CEQ 'CAM-A')
$null = Run 'missing-camera' $bad 3 'CAM-B:window-not-covered'
$bad = Fixture; $bad.events = @($bad.events | Where-Object { $_.alias -cne 'CAM-B' -or $_.type -cne 'frame' -or $_.sequence -eq 1 })
$null = Run 'frozen-side' $bad 3 'CAM-B:insufficient-frames'
$bad = Fixture; $bad.window.minFramesPerCamera = 2
$bad.events = @($bad.events | Where-Object { $_.alias -cne 'CAM-B' -or $_.type -cne 'frame' -or $_.sequence -ne 2 })
$null = Run 'gap-in-middle' $bad 3 'CAM-B:frame-gap-exceeded'
$bad = Fixture; $bad.window.maxFrameGapMs = 99
$null = Run 'one-ms-over-gap' $bad 3 'CAM-A:frame-gap-exceeded'
$bad = Fixture; $bad.window.startMs = 0
$null = Run 'startup-gap' $bad 3 'CAM-B:window-not-covered'
$bad = Fixture; $bad.window.startMs = 10; $bad.events[3].atMs = 111
$bad.events[2].atMs = 111
$null = Run 'initial-frame-gap' $bad 3 'CAM-A:frame-gap-exceeded'
$bad = Fixture; $bad.window.endMs = 310; $bad.events[6].atMs = 209; $bad.events[7].atMs = 209
$null = Run 'final-frame-gap' $bad 3 'CAM-A:frame-gap-exceeded'
$bad = Fixture; $bad.events = @($bad.events | Where-Object type -CNE 'frame')
$null = Run 'no-frames' $bad 3 'CAM-A:insufficient-frames'
$bad = Fixture; $bad.events = @()
$null = Run 'no-events' $bad 3 'CAM-A:close-unconfirmed'
$bad = Fixture; $bad.events[4].sequence = 8; $bad.events[6].sequence = 9
$null = Run 'sequence-gaps-are-not-replay' $bad 0
$bad = Fixture; $bad.events[1].atMs = 101
$bad.events[2].atMs = 102; $bad.events[3].atMs = 102
$null = Run 'late-open' $bad 3 'CAM-B:window-not-covered'
$bad = Fixture; $bad.window.endMs = 321
$null = Run 'stopped-before-end' $bad 3 'CAM-A:window-not-covered'
$bad = Fixture; $bad.events = @($bad.events | Where-Object { $_.alias -cne 'CAM-B' -or $_.type -cne 'closed' })
$null = Run 'close-unconfirmed' $bad 3 'CAM-B:close-unconfirmed'
$bad = Fixture; $bad.events = @($bad.events | Where-Object type -CEQ 'opened')
$null = Run 'open-only' $bad 3 'CAM-A:close-unconfirmed'
$bad = Fixture; $bad.events[5].sequence = 1
$null = Run 'replayed-sequence' $bad 2 'invalid-observation'
$bad = Fixture; $bad.events[5].atMs = 199
$null = Run 'clock-regression' $bad 2 'invalid-observation'
$bad = Fixture; $bad.events[0].type = 'frame'; $bad.events[0].sequence = 1
$null = Run 'frame-before-open' $bad 2 'invalid-observation'
$bad = Fixture; $bad.events[10].type = 'frame'; $bad.events[10].sequence = 4
$null = Run 'frame-after-stop' $bad 2 'invalid-observation'
$bad = Fixture; $bad.events[8].type = 'closed'
$null = Run 'close-without-stop' $bad 2 'invalid-observation'
$bad = Fixture; $bad.events[2].type = 'opened'; $bad.events[2].Remove('sequence')
$null = Run 'automatic-reopen' $bad 2 'invalid-observation'
foreach ($mutation in @('extra-root','extra-event','extra-window','unknown-alias','unknown-type','unknown-clock',
    'unknown-source','unknown-version','missing-events','missing-sequence','sequence-on-open','string-number',
    'fraction-time','negative-time','unsafe-integer','zero-gap','one-frame-policy','empty-window','null-window','null-events')) {
    $bad = Fixture
    switch ($mutation) {
        extra-root { $bad.serial = 'SYNTHETIC-DO-NOT-ECHO' }
        extra-event { $bad.events[0].payload = 'SYNTHETIC-DO-NOT-ECHO' }
        extra-window { $bad.window.implicit = $true }
        unknown-alias { $bad.events[0].alias = 'cam-a' }
        unknown-type { $bad.events[0].type = 'capture' }
        unknown-clock { $bad.clock = 'wall-clock' }
        unknown-source { $bad.source = 'trusted' }
        unknown-version { $bad.schemaVersion = 'v2' }
        missing-events { $bad.Remove('events') }
        missing-sequence { $bad.events[2].Remove('sequence') }
        sequence-on-open { $bad.events[0].sequence = 1 }
        string-number { $bad.events[0].atMs = '0' }
        fraction-time { $bad.events[2].atMs = 100.5 }
        negative-time { $bad.events[0].atMs = -1 }
        unsafe-integer { $bad.events[0].atMs = 9007199254740992L }
        zero-gap { $bad.window.maxFrameGapMs = 0 }
        one-frame-policy { $bad.window.minFramesPerCamera = 1 }
        empty-window { $bad.window.endMs = 100 }
        null-window { $bad.window = $null }
        null-events { $bad.events = $null }
    }
    $report = Run $mutation $bad 2 'invalid-observation'
    Check (($report | ConvertTo-Json -Depth 10) -cnotmatch 'SYNTHETIC-DO-NOT-ECHO') "$mutation no unknown input echo"
}
$raw = Fixture | ConvertTo-Json -Depth 20 -Compress
$null = Run 'duplicate-root' ('{"source":"simulation",' + $raw.Substring(1)) 2 'invalid-observation' -Raw
$null = Run 'duplicate-case-key' ($raw.Replace('"atMs":0', '"atMs":0,"atMs":1')) 2 'invalid-observation' -Raw
$null = Run 'case-variant-key' ($raw.Replace('"atMs":0', '"atMs":0,"AtMs":0')) 2 'invalid-observation' -Raw
$null = Run 'exponent-integer' ($raw.Replace('"atMs":0', '"atMs":0e0')) 2 'invalid-observation' -Raw
$null = Run 'trailing-json' ($raw + '{}') 2 'invalid-observation' -Raw
$null = Run 'malformed-json' '{' 2 'invalid-observation' -Raw
$null = Run 'null-root' 'null' 2 'invalid-observation' -Raw
$bad = Fixture; $bad.events = @($bad.events[0]) * 32769
$null = Run 'too-many-events' $bad 2 'invalid-observation'
$null = Run 'oversize-input' (' ' * (4MB + 1)) 2 'invalid-observation' -Raw
$missingOutput = & $Evaluator -Path (Join-Path $root 'missing.json')
Check ($LASTEXITCODE -eq 2 -and ($missingOutput | ConvertFrom-Json).bothUpdated -eq $false) 'missing file rejected'
$invalidUtf8Path = Join-Path $root 'invalid-utf8.json'
[IO.File]::WriteAllBytes($invalidUtf8Path, [byte[]]@(0xc3, 0x28))
$invalidUtf8Output = & $Evaluator -Path $invalidUtf8Path
Check ($LASTEXITCODE -eq 2 -and ($invalidUtf8Output | ConvertFrom-Json).outcome -ceq 'InvalidObservation') 'malformed UTF8 rejected'
foreach ($case in @(@('both-updated',0), @('frozen-side',3), @('malformed-json',2))) {
    $cliOutput = & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -NonInteractive -File $Evaluator -Path (Join-Path $root ($case[0] + '.json'))
    Check ($LASTEXITCODE -eq $case[1]) "process exit code $($case[0])"
    Check (($cliOutput | ConvertFrom-Json).bothUpdated -eq ($case[1] -eq 0)) "process JSON $($case[0])"
}
$noPathOutput = & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -NonInteractive -File $Evaluator
Check ($LASTEXITCODE -eq 2 -and ($noPathOutput | ConvertFrom-Json).outcome -ceq 'InvalidObservation') 'missing path fails without prompting'
Write-Output "dual_live_observation checks=$script:checks failures=0 evidence=$root"
