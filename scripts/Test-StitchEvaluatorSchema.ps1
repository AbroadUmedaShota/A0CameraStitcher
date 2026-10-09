[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Harness,
    [Parameter(Mandatory)][string]$Evaluator,
    [Parameter(Mandatory)][string]$Generator,
    [Parameter(Mandatory)][string]$EvidenceParent,
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..'))
)
$ErrorActionPreference = 'Stop'
$schemaRoot = Join-Path $RepositoryRoot 'docs/schemas'
$fixtures = Join-Path $RepositoryRoot 'tests/fixtures/stitch-evaluation'
$script:checks = 0
function Assert-Schema([string]$Json, [string]$Leaf, [bool]$Expected) {
    $script:checks++
    $valid = Test-Json -Json $Json -SchemaFile (Join-Path $schemaRoot $Leaf) -ErrorAction SilentlyContinue
    if ($valid -ne $Expected) { throw "Unexpected schema verdict: $Leaf (expected $Expected)" }
}
function Compact($Node) { ConvertTo-Json -InputObject $Node -Depth 100 -Compress }
function Copy-Node($Node) { Compact $Node | ConvertFrom-Json -AsHashtable }
function Rebase-Refs($Node, [string]$Prefix) {
    if ($Node -is [System.Collections.IDictionary]) {
        foreach ($key in @($Node.Keys)) {
            if ($key -eq '$ref' -and $Node[$key].StartsWith('#/$defs/')) { $Node[$key] = '#/$defs/' + $Prefix + '/$defs/' + $Node[$key].Substring(8) }
            else { Rebase-Refs $Node[$key] $Prefix }
        }
    } elseif ($Node -is [System.Collections.IList]) { foreach ($item in $Node) { Rebase-Refs $item $Prefix } }
}
# Compare structurally, ignoring object key order; detect drift of embedded contracts.
function Canonical($Node) {
    if ($Node -is [System.Collections.IDictionary]) {
        $result = [ordered]@{}; foreach ($key in @($Node.Keys | Sort-Object)) { $result[$key] = Canonical $Node[$key] }; return $result
    }
    if ($Node -is [System.Collections.IList]) { $items = @(); foreach ($item in $Node) { $items += ,(Canonical $item) }; return ,$items }
    return $Node
}
$reportSchema = Get-Content (Join-Path $schemaRoot 'stitch-evaluation-report.v1.schema.json') -Raw | ConvertFrom-Json -AsHashtable
foreach ($pair in @(@('metricDefinition', 'stitch-metric-definition.schema.json'), @('metricResult', 'stitch-metric-result.schema.json'))) {
    $old = Get-Content (Join-Path $schemaRoot $pair[1]) -Raw | ConvertFrom-Json -AsHashtable
    [void]$old.Remove('$id'); [void]$old.Remove('$schema'); Rebase-Refs $old $pair[0]
    $script:checks++
    if ((Compact (Canonical $old)) -ne (Compact (Canonical $reportSchema.'$defs'[$pair[0]]))) { throw 'Embedded metric contract drift' }
}
foreach ($kind in @('plan', 'thresholds')) {
    $node = Get-Content (Join-Path $fixtures "$kind.json") -Raw | ConvertFrom-Json -AsHashtable
    Assert-Schema (Compact $node) "stitch-evaluation-$kind.v1.schema.json" $true
    $bad = Copy-Node $node; $bad.extra = 1
    Assert-Schema (Compact $bad) "stitch-evaluation-$kind.v1.schema.json" $false
}
$thresholds = Get-Content (Join-Path $fixtures 'thresholds.json') -Raw | ConvertFrom-Json -AsHashtable
$bad = Copy-Node $thresholds; $bad.checks[1] = $bad.checks[0]
Assert-Schema (Compact $bad) 'stitch-evaluation-thresholds.v1.schema.json' $false
$bad = Copy-Node $thresholds; $bad.checks[3].value = 1.01
Assert-Schema (Compact $bad) 'stitch-evaluation-thresholds.v1.schema.json' $false
$validity = @{schemaVersion='a0.stitch-output-validity.v1';imageSha256=('a'*64);maskSha256=('b'*64);widthPixels=256;heightPixels=256;maskRelativePath='validity.pgm'}
Assert-Schema (Compact $validity) 'stitch-output-validity.v1.schema.json' $true
$bad = Copy-Node $validity; $bad.maskRelativePath = '../validity.pgm'
Assert-Schema (Compact $bad) 'stitch-output-validity.v1.schema.json' $false

# The native harness authors only synthetic images in its owned temp directory.
# Its actual published reports are copied to this new ignored evidence directory.
$parent = Get-Item -LiteralPath $EvidenceParent
if (-not $parent.PSIsContainer -or ($parent.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Ordinary existing evidence parent required' }
$evidence = Join-Path $parent.FullName ('evaluator-schema-' + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $evidence)
& $Harness $fixtures $Evaluator $Generator $evidence
if ($LASTEXITCODE -ne 0) { throw 'Native JPEG evaluator evidence harness failed' }
$reports = @(Get-ChildItem -LiteralPath $evidence -Filter '*.json' -File)
if ($reports.Count -ne 8) { throw 'Expected actual pristine/damage/NoResult/producer evidence reports' }
foreach ($file in $reports) {
    $report = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json -AsHashtable
    Assert-Schema (Compact $report) 'stitch-evaluation-report.v1.schema.json' $true
    foreach ($entry in $report.metrics) {
        Assert-Schema (Compact $entry.definition) 'stitch-metric-definition.schema.json' $true
        Assert-Schema (Compact $entry.result) 'stitch-metric-result.schema.json' $true
    }
    foreach ($mutation in @('quality', 'hash', 'extra', 'metricId', 'null', 'model')) {
        $bad = Copy-Node $report
        switch ($mutation) {
            quality { $bad.quality = 'approved' }
            hash { $bad.sourceHashes.image = 'a' }
            extra { $bad.extra = 1 }
            metricId { $bad.metrics[0].result.metricId = 'unrelated' }
            null { $bad.fiducials[0].status = 'missing'; $bad.fiducials[0].observedPixel = @{x=48;y=48} }
            model { $bad.measurementModel.validity = 'inferred-from-black' }
        }
        Assert-Schema (Compact $bad) 'stitch-evaluation-report.v1.schema.json' $false
    }
}
Write-Output "stitch_evaluator_schema checks=$script:checks failures=0 reports=$($reports.Count)"
