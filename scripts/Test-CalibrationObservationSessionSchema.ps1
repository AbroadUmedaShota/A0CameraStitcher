[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..'))
)

# Contract test of docs/schemas/calibration-observation-session.v1.schema.json (Issue #286).
# Test-Json applies the schema to the hand-written synthetic fixtures. Rules that JSON Schema cannot
# express (integer lexemes, binary64 overflow, calendar dates, uniqueness across views, references
# between points and observations, pixel range against image size, ...) are enforced by the C++
# reader. tests/fixtures/calibration-session/negative-cases.json records, per case, which layer
# rejects it, and the C++ contract test reads the same table. A rule that silently moves between
# the layers therefore fails one of the two tests.
#
# provenance.measuredAt and provenance.tool must stay structurally identical to
# calibration.measuredAt and calibration.tool of rig-profile.v2.schema.json (drift check below).

$ErrorActionPreference = 'Stop'

# Fail closed: Windows PowerShell 5.1 has no Test-Json, and ConvertFrom-Json -DateKind
# (used below to keep date-like strings as strings) arrived in PowerShell 7.5.
$psVersion = $PSVersionTable.PSVersion
if ($psVersion.Major -lt 7 -or ($psVersion.Major -eq 7 -and $psVersion.Minor -lt 5)) { throw 'PowerShell 7.5 or later is required.' }

$script:CheckCount = 0
$script:SkippedCount = 0

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    $script:CheckCount++
    if (-not $Condition) { throw $Message }
}

function Assert-StrictSchemaShape {
    param([AllowNull()]$Node, [string]$Path = '$')
    if ($null -eq $Node) { return }
    if ($Node -is [System.Collections.IEnumerable] -and $Node -isnot [string] -and $Node -isnot [System.Management.Automation.PSCustomObject]) {
        $index = 0
        foreach ($child in $Node) { Assert-StrictSchemaShape -Node $child -Path "$Path[$index]"; $index++ }
        return
    }
    if ($Node -isnot [System.Management.Automation.PSCustomObject]) { return }
    $names = @($Node.PSObject.Properties.Name)
    if ($names -contains 'properties') {
        Assert-Condition ($names -contains 'type') "Schema object using properties must declare type at $Path."
        Assert-Condition (@($Node.type) -contains 'object') "Schema properties owner must allow object at $Path."
        Assert-Condition ($names -contains 'additionalProperties' -and $Node.additionalProperties -eq $false) "Schema object must set additionalProperties to false at $Path."
    }
    if ($names -contains 'items') {
        Assert-Condition ($names -contains 'type') "Schema object using items must declare type at $Path."
        Assert-Condition (@($Node.type) -contains 'array') "Schema items owner must allow array at $Path."
    }
    foreach ($property in $Node.PSObject.Properties) {
        Assert-StrictSchemaShape -Node $property.Value -Path "$Path.$($property.Name)"
    }
}

function Read-Hashtable {
    param([Parameter(Mandatory)][string]$Text)
    return $Text | ConvertFrom-Json -AsHashtable -DateKind String
}

function Test-SchemaAccepts {
    param([Parameter(Mandatory)][string]$Json, [Parameter(Mandatory)][string]$SchemaPath)
    return [bool](Test-Json -Json $Json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue)
}

# JSON Pointer add / replace / remove on a parsed document. Arrays are fixed-size object[], so a
# changed array is rebuilt and handed back to its parent.
function Set-AtPointer {
    param($Node, [string[]]$Tokens, [int]$Index, [string]$Kind, $Value)
    $token = $Tokens[$Index]
    $last = ($Index -eq $Tokens.Count - 1)
    if ($Node -is [System.Collections.IDictionary]) {
        if ($last) {
            switch ($Kind) {
                'add' {
                    if ($Node.Contains($token)) { throw "add onto existing key $token" }
                    $Node[$token] = $Value
                }
                'replace' {
                    if (-not $Node.Contains($token)) { throw "replace of missing key $token" }
                    $Node[$token] = $Value
                }
                'remove' {
                    if (-not $Node.Contains($token)) { throw "remove of missing key $token" }
                    $Node.Remove($token)
                }
                default { throw "unknown operation $Kind" }
            }
        }
        else {
            if (-not $Node.Contains($token)) { throw "missing key $token on the path" }
            $Node[$token] = Set-AtPointer -Node $Node[$token] -Tokens $Tokens -Index ($Index + 1) -Kind $Kind -Value $Value
        }
        return $Node
    }
    if ($Node -is [System.Collections.IList]) {
        $list = [System.Collections.Generic.List[object]]::new()
        foreach ($item in $Node) { $list.Add($item) }
        $position = if ($token -eq '-') { $list.Count } else { [int]$token }
        if ($last) {
            switch ($Kind) {
                'add' {
                    if ($position -gt $list.Count) { throw "add beyond array end $position" }
                    $list.Insert($position, $Value)
                }
                'replace' {
                    if ($position -ge $list.Count) { throw "replace beyond array end $position" }
                    $list[$position] = $Value
                }
                'remove' {
                    if ($position -ge $list.Count) { throw "remove beyond array end $position" }
                    $list.RemoveAt($position)
                }
                default { throw "unknown operation $Kind" }
            }
        }
        else {
            if ($position -ge $list.Count) { throw "index $position beyond array end on the path" }
            $list[$position] = Set-AtPointer -Node $list[$position] -Tokens $Tokens -Index ($Index + 1) -Kind $Kind -Value $Value
        }
        return , $list.ToArray()
    }
    throw 'operation parent is a scalar'
}

function Expand-SchemaRefs {
    param($Node, $Defs)
    if ($Node -is [System.Collections.IDictionary]) {
        if ($Node.Contains('$ref') -and $Node.Count -eq 1 -and $Node['$ref'].StartsWith('#/$defs/')) {
            return Expand-SchemaRefs -Node $Defs[$Node['$ref'].Substring(8)] -Defs $Defs
        }
        $expanded = [ordered]@{}
        foreach ($key in @($Node.Keys | Sort-Object)) { $expanded[$key] = Expand-SchemaRefs -Node $Node[$key] -Defs $Defs }
        return $expanded
    }
    if ($Node -is [System.Collections.IList]) {
        $items = [System.Collections.Generic.List[object]]::new()
        foreach ($item in $Node) { $items.Add((Expand-SchemaRefs -Node $item -Defs $Defs)) }
        return , $items.ToArray()
    }
    return $Node
}

function Get-ExpandedJson {
    param($Schema, [string[]]$Path)
    $node = $Schema
    foreach ($step in $Path) { $node = $node[$step] }
    return ConvertTo-Json -InputObject (Expand-SchemaRefs -Node $node -Defs $Schema['$defs']) -Depth 50 -Compress
}

$schemaPath = Join-Path $RepositoryRoot 'docs/schemas/calibration-observation-session.v1.schema.json'
$v2SchemaPath = Join-Path $RepositoryRoot 'docs/schemas/rig-profile.v2.schema.json'
$fixtureDirectory = Join-Path $RepositoryRoot 'tests/fixtures/calibration-session'
$validText = Get-Content -LiteralPath (Join-Path $fixtureDirectory 'valid-minimal.json') -Raw

# 1. The schema is strict: every object closes its property set, every array declares its type.
$schemaObject = Get-Content -LiteralPath $schemaPath -Raw | ConvertFrom-Json
Assert-StrictSchemaShape -Node $schemaObject
Assert-Condition ($schemaObject.properties.schemaVersion.const -eq 'a0.calibration-observation-session.v1') 'schemaVersion must be the v1 constant.'

# 2. The positive fixture.
Assert-Condition (Test-SchemaAccepts -Json $validText -SchemaPath $schemaPath) 'JSON Schema rejected valid-minimal.json.'

# 3. The table of single-violation cases.
$cases = Get-Content -LiteralPath (Join-Path $fixtureDirectory 'negative-cases.json') -Raw | ConvertFrom-Json -AsHashtable -DateKind String
Assert-Condition ($cases.Count -ge 70) 'negative-cases.json lost cases.'
$ids = @{}
foreach ($case in $cases) {
    Assert-Condition (-not $ids.ContainsKey($case.id)) "Duplicate case id $($case.id)."
    $ids[$case.id] = $true
    Assert-Condition (@('reject', 'accept', 'skip') -contains $case.schema) "Case $($case.id) has an invalid schema column."
    if ($case.schema -eq 'skip') { $script:SkippedCount++; continue }
    $document = Read-Hashtable -Text $validText
    foreach ($operation in $case.ops) {
        # An if expression would unroll an array value: [] would become $null and a one-element array a scalar.
        $value = $null
        if ($operation.ContainsKey('value')) { $value = $operation.value }
        $document = Set-AtPointer -Node $document -Tokens @($operation.path.Substring(1).Split('/')) -Index 0 -Kind $operation.op -Value $value
    }
    $json = ConvertTo-Json -InputObject $document -Depth 50 -Compress
    $accepted = Test-SchemaAccepts -Json $json -SchemaPath $schemaPath
    if ($case.schema -eq 'reject') { Assert-Condition (-not $accepted) "JSON Schema accepted case $($case.id) ($($case.item)); the table says it rejects." }
    else { Assert-Condition $accepted "JSON Schema rejected case $($case.id) ($($case.item)); the table says only the reader does." }
}

# 4. The generator's ground-truth document is not a session.
$groundTruth = Get-Content -LiteralPath (Join-Path $fixtureDirectory 'invalid-generator-ground-truth.json') -Raw
Assert-Condition (-not (Test-SchemaAccepts -Json $groundTruth -SchemaPath $schemaPath)) 'JSON Schema accepted the generator ground truth.'

# 5. Probes on the raw text, where PowerShell would change a number or hide a key.
$overflow = $validText.Replace('"xMm": 0,', '"xMm": 1e400,')
Assert-Condition ($overflow -ne $validText) 'The overflow probe did not change the text.'
Assert-Condition (Test-SchemaAccepts -Json $overflow -SchemaPath $schemaPath) 'A coordinate of 1e400 is meant to pass the schema and fail in the reader.'
# String.Replace(old, new, 1) reads the 1 as a StringComparison and replaces every match, so the
# duplicate is inserted after the first opening brace only, by position.
$firstBrace = $validText.IndexOf('{')
Assert-Condition ($firstBrace -ge 0) 'valid-minimal.json has no opening brace.'
$duplicateKey = $validText.Insert($firstBrace + 1, "`n  `"sessionId`": `"dup`",")
Assert-Condition ($duplicateKey -ne $validText) 'The duplicate-key probe did not change the text.'
Assert-Condition (([regex]::Matches($duplicateKey, '"sessionId"')).Count -eq ([regex]::Matches($validText, '"sessionId"')).Count + 1) 'The duplicate-key probe must add exactly one sessionId key.'
Assert-Condition (-not (Test-SchemaAccepts -Json $duplicateKey -SchemaPath $schemaPath)) 'JSON Schema accepted a duplicate key.'

# 6. Drift check against rig-profile.v2: provenance mirrors calibration.
$session = Get-Content -LiteralPath $schemaPath -Raw | ConvertFrom-Json -AsHashtable
$v2 = Get-Content -LiteralPath $v2SchemaPath -Raw | ConvertFrom-Json -AsHashtable
foreach ($member in @('measuredAt', 'tool')) {
    $mine = Get-ExpandedJson -Schema $session -Path @('properties', 'provenance', 'properties', $member)
    $theirs = Get-ExpandedJson -Schema $v2 -Path @('properties', 'calibration', 'properties', $member)
    Assert-Condition ($mine -eq $theirs) "provenance.$member drifted from calibration.$member of rig-profile.v2.schema.json."
}
# The comparison must be able to fail: change one pattern and expect a difference.
$changed = Get-Content -LiteralPath $schemaPath -Raw | ConvertFrom-Json -AsHashtable
$changed['$defs']['utcSeconds']['pattern'] = '^.*$'
$changedJson = Get-ExpandedJson -Schema $changed -Path @('properties', 'provenance', 'properties', 'measuredAt')
$v2Json = Get-ExpandedJson -Schema $v2 -Path @('properties', 'calibration', 'properties', 'measuredAt')
Assert-Condition ($changedJson -ne $v2Json) 'The drift comparison did not notice a changed pattern.'
$changedTool = Get-Content -LiteralPath $schemaPath -Raw | ConvertFrom-Json -AsHashtable
$changedTool['$defs']['identifier']['pattern'] = '^[A-Za-z0-9]{1,128}$'
Assert-Condition ((Get-ExpandedJson -Schema $changedTool -Path @('properties', 'provenance', 'properties', 'tool')) -ne
    (Get-ExpandedJson -Schema $v2 -Path @('properties', 'calibration', 'properties', 'tool'))) 'The drift comparison did not notice a changed identifier pattern.'

Write-Host "calibration_observation_session_schema checks=$($script:CheckCount) skipped=$($script:SkippedCount) failures=0"
