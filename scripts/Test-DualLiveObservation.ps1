#requires -Version 7.4
[CmdletBinding()]
param([string]$Path = '')

# Offline evidence reader only. No SDK, WPD, process invocation, device access,
# output files, or hardware authorization. Input labels are untrusted receipts.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$document = $null
$inputHash = $null
$report = [ordered]@{
    schemaVersion = 'a0.dual-live-observation-result.v1'
    inputSha256 = $null
    source = $null
    window = $null
    outcome = 'InvalidObservation'
    bothUpdated = $false
    allClosed = $false
    hardwareReady = $false
    physicalIdentityVerified = $false
    shutterSynchronized = $false
    cameras = @()
    reasons = @('invalid-observation')
}
$resultCode = 2
function Require-Object([System.Text.Json.JsonElement]$Node, [string[]]$Fields) {
    if ($Node.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) { throw 'shape' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($property in $Node.EnumerateObject()) {
        if (-not $seen.Add($property.Name) -or $Fields -cnotcontains $property.Name) { throw 'shape' }
    }
    if ($seen.Count -ne $Fields.Count) { throw 'shape' }
}
function Read-String([System.Text.Json.JsonElement]$Node) {
    if ($Node.ValueKind -ne [System.Text.Json.JsonValueKind]::String) { throw 'string' }
    return $Node.GetString()
}
function Read-Integer([System.Text.Json.JsonElement]$Node, [long]$Minimum = 0, [long]$Maximum = 9007199254740991) {
    [long]$number = 0
    if ($Node.ValueKind -ne [System.Text.Json.JsonValueKind]::Number -or
        $Node.GetRawText() -cnotmatch '\A(?:0|[1-9][0-9]*)\z' -or
        -not $Node.TryGetInt64([ref]$number) -or $number -lt $Minimum -or $number -gt $Maximum) { throw 'integer' }
    return $number
}
try {
    # Read a bounded immutable snapshot. Do not report paths or untrusted data
    # on parser/IO errors; the caller can retain the original privately.
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'path' }
    $stream = [IO.File]::Open($Path, 'Open', 'Read', 'Read')
    try {
        if ($stream.Length -gt 4MB) { throw 'size' }
        $bytes = [byte[]]::new([int]$stream.Length)
        $stream.ReadExactly($bytes)
    } finally { $stream.Dispose() }
    $inputHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    $report.inputSha256 = $inputHash
    $json = [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
    $options = [System.Text.Json.JsonDocumentOptions]::new()
    $options.MaxDepth = 8
    $document = [System.Text.Json.JsonDocument]::Parse($json, $options)
    $root = $document.RootElement
    Require-Object $root @('schemaVersion','source','clock','window','events')
    if ((Read-String $root.GetProperty('schemaVersion')) -cne 'a0.dual-live-observation.v1' -or
        (Read-String $root.GetProperty('clock')) -cne 'controller-monotonic-ms') { throw 'version-clock' }
    $source = Read-String $root.GetProperty('source')
    if (@('simulation','hardware') -cnotcontains $source) { throw 'source' }
    $window = $root.GetProperty('window')
    Require-Object $window @('startMs','endMs','maxFrameGapMs','minFramesPerCamera')
    $start = Read-Integer $window.GetProperty('startMs')
    $end = Read-Integer $window.GetProperty('endMs')
    $gapLimit = Read-Integer $window.GetProperty('maxFrameGapMs') 1
    $minimumCount = Read-Integer $window.GetProperty('minFramesPerCamera') 2 32768
    if ($end -le $start) { throw 'window' }
    $events = $root.GetProperty('events')
    if ($events.ValueKind -ne [System.Text.Json.JsonValueKind]::Array -or $events.GetArrayLength() -gt 32768) { throw 'events' }
    $states = @{}
    foreach ($alias in @('CAM-A','CAM-B')) {
        $states[$alias] = @{
            stage = 'new'; opened = $null; stopped = $null; sequence = 0L
            frames = [Collections.Generic.List[long]]::new()
        }
    }
    [long]$lastTime = -1
    foreach ($event in $events.EnumerateArray()) {
        if ($event.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) { throw 'event' }
        $type = Read-String $event.GetProperty('type')
        $fields = @('alias','type','atMs')
        if ($type -ceq 'frame') { $fields += 'sequence' }
        Require-Object $event $fields
        $alias = Read-String $event.GetProperty('alias')
        if (@('CAM-A','CAM-B') -cnotcontains $alias) { throw 'alias' }
        $time = Read-Integer $event.GetProperty('atMs')
        if ($time -lt $lastTime) { throw 'clock-regressed' }
        $lastTime = $time
        $state = $states[$alias]
        if ($type -ceq 'opened') {
            if ($state.stage -cne 'new') { throw 'open-state' }
            $state.stage = 'open'; $state.opened = $time
        } elseif ($type -ceq 'frame') {
            if ($state.stage -cne 'open') { throw 'frame-state' }
            $sequence = Read-Integer $event.GetProperty('sequence') 1
            if ($sequence -le $state.sequence) { throw 'sequence' }
            $state.sequence = $sequence
            if ($time -ge $start -and $time -le $end) { $state.frames.Add($time) }
        } elseif ($type -ceq 'stopped') {
            if ($state.stage -cne 'open') { throw 'stop-state' }
            $state.stage = 'stopped'; $state.stopped = $time
        } elseif ($type -ceq 'closed') {
            if ($state.stage -cne 'stopped') { throw 'close-state' }
            $state.stage = 'closed'
        } else { throw 'event-type' }
    }
    $reasons = [Collections.Generic.List[string]]::new()
    $cameras = @()
    $allClosed = $true
    foreach ($alias in @('CAM-A','CAM-B')) {
        $state = $states[$alias]
        $covered = $null -ne $state.opened -and $null -ne $state.stopped -and
            $state.opened -le $start -and $state.stopped -ge $end
        [long]$previous = $start
        [long]$maxGap = 0
        foreach ($time in $state.frames) {
            $maxGap = [Math]::Max($maxGap, ($time - $previous))
            $previous = $time
        }
        $maxGap = [Math]::Max($maxGap, ($end - $previous))
        $closed = $state.stage -ceq 'closed'
        if (-not $covered) { $reasons.Add("${alias}:window-not-covered") }
        if ($state.frames.Count -lt $minimumCount) { $reasons.Add("${alias}:insufficient-frames") }
        if ($maxGap -gt $gapLimit) { $reasons.Add("${alias}:frame-gap-exceeded") }
        if (-not $closed) { $reasons.Add("${alias}:close-unconfirmed"); $allClosed = $false }
        $cameras += [ordered]@{
            alias = $alias; frameCount = $state.frames.Count; maximumGapMs = $maxGap
            windowCovered = [bool]$covered; closed = $closed
        }
    }
    $report.source = $source
    $report.window = [ordered]@{ startMs = $start; endMs = $end; maxFrameGapMs = $gapLimit; minFramesPerCamera = $minimumCount }
    $report.cameras = $cameras
    $report.allClosed = $allClosed
    $report.reasons = @($reasons.ToArray())
    $report.bothUpdated = $reasons.Count -eq 0
    $report.outcome = if ($report.bothUpdated) { 'BothUpdatedObserved' } else { 'CriteriaNotMet' }
    $resultCode = if ($report.bothUpdated) { 0 } else { 3 }
} catch {
    # Deliberately no exception text, unknown properties, input paths or pixels.
    # No partial camera verdict is emitted for malformed or inconsistent logs.
    $report.source = $null; $report.window = $null
    $report.outcome = 'InvalidObservation'; $report.bothUpdated = $false
    $report.allClosed = $false; $report.cameras = @()
    $report.reasons = @('invalid-observation'); $resultCode = 2
} finally {
    if ($null -ne $document) { $document.Dispose() }
}
$report | ConvertTo-Json -Depth 6 -Compress
exit $resultCode
