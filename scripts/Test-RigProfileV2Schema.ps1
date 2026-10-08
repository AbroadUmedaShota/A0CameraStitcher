[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..'))
)

# Contract test for the proposed rig profile 2.0.0 (docs/DECISIONS.md ADR-0034,
# docs/design/rig-profile-v2.md). Test-Json applies the schema to positive and negative cases.
# Cross-field arithmetic that JSON Schema cannot express is checked by
# Assert-RigProfileV2RuntimeContract, a reference of the runtime rules that the product validator
# must implement (#274). Every case delegated to it is also asserted to pass the schema, so that a
# rule silently moving between the two layers is noticed.
#
# All numbers below are schema-exercise sentinels. None of them is an approved rig, lens, DPI or
# threshold value, and no profile built here is written to disk.

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Throws {
    param([Parameter(Mandatory)][scriptblock]$Action, [Parameter(Mandatory)][string]$Message)
    try {
        & $Action
        throw "Expected rejection: $Message"
    }
    catch {
        if ($_.Exception.Message -eq "Expected rejection: $Message") { throw }
    }
}

# Deep copy without a JSON round-trip: date-time strings stay strings and matrix rows stay nested.
function Copy-Node {
    param([AllowNull()]$Node)
    if ($null -eq $Node) { return $null }
    if ($Node -is [System.Management.Automation.PSCustomObject]) {
        $copy = [ordered]@{}
        foreach ($property in $Node.PSObject.Properties) { $copy[$property.Name] = Copy-Node $property.Value }
        return [pscustomobject]$copy
    }
    if ($Node -is [string]) { return $Node }
    if ($Node -is [System.Collections.IList]) {
        $list = [System.Collections.Generic.List[object]]::new()
        foreach ($item in $Node) { $list.Add((Copy-Node $item)) }
        return , $list.ToArray()
    }
    return $Node
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
    }
    foreach ($keyword in @('items', 'prefixItems')) {
        if ($names -contains $keyword) {
            Assert-Condition ($names -contains 'type') "Schema object using $keyword must declare type at $Path."
            Assert-Condition (@($Node.type) -contains 'array') "Schema $keyword owner must allow array at $Path."
        }
    }
    foreach ($property in $Node.PSObject.Properties) {
        Assert-StrictSchemaShape -Node $property.Value -Path "$Path.$($property.Name)"
    }
}

function Test-SchemaAccepts {
    param([Parameter(Mandatory)]$Profile, [Parameter(Mandatory)][string]$SchemaPath)
    $json = ConvertTo-Json -InputObject $Profile -Depth 30 -Compress
    return [bool](Test-Json -Json $json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue)
}

$script:SchemaCaseCount = 0
$script:RuntimeCaseCount = 0

function Assert-SchemaAccepts {
    param([Parameter(Mandatory)]$Profile, [Parameter(Mandatory)][string]$SchemaPath, [Parameter(Mandatory)][string]$CaseName)
    Assert-Condition (Test-SchemaAccepts -Profile $Profile -SchemaPath $SchemaPath) "JSON Schema rejected valid case: $CaseName."
    $script:SchemaCaseCount++
}

function Assert-SchemaRejects {
    param([Parameter(Mandatory)]$Profile, [Parameter(Mandatory)][string]$SchemaPath, [Parameter(Mandatory)][string]$CaseName)
    Assert-Condition (-not (Test-SchemaAccepts -Profile $Profile -SchemaPath $SchemaPath)) "JSON Schema accepted invalid case: $CaseName."
    $script:SchemaCaseCount++
}

function ConvertTo-UtcInstant {
    param([Parameter(Mandatory)][string]$Text, [Parameter(Mandatory)][string]$Name)
    $parsed = [DateTimeOffset]::MinValue
    $ok = [DateTimeOffset]::TryParseExact(
        $Text, "yyyy-MM-dd'T'HH:mm:ss'Z'", [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AssumeUniversal, [ref]$parsed)
    Assert-Condition $ok "$Name is not a UTC date-time with whole seconds."
    return $parsed
}

function ConvertTo-Micrometres {
    param([Parameter(Mandatory)][double]$Millimetres, [Parameter(Mandatory)][string]$Name)
    $scaled = $Millimetres * 1000.0
    $rounded = [math]::Round($scaled)
    Assert-Condition ([math]::Abs($scaled - $rounded) -le 1e-6) "$Name is not a whole number of micrometres."
    return [long]$rounded
}

function Get-RequiredPixels {
    # ceil(micrometres * dpi / 25400) in integers, the same ceiling as src/m2/optical_planner.cpp.
    param([Parameter(Mandatory)][long]$Micrometres, [Parameter(Mandatory)][long]$Dpi)
    $numerator = $Micrometres * $Dpi
    $remainder = [long]0
    $quotient = [math]::DivRem($numerator, [long]25400, [ref]$remainder)
    if ($remainder -ne 0) { $quotient++ }
    return $quotient
}

function Assert-IntegerLiteral {
    # JSON Schema "integer" accepts 180.0; the product parser accepts only an integer literal.
    param($Value, [string]$Name)
    Assert-Condition ($Value -is [int] -or $Value -is [long]) "$Name must be written as an integer literal."
}

# Reference of the runtime rules in docs/design/rig-profile-v2.md section 9.3. The product path
# accepts approved profiles only; a draft is rejected here whatever it contains.
function Assert-RigProfileV2RuntimeContract {
    param([Parameter(Mandatory)]$Profile, [Parameter(Mandatory)][DateTimeOffset]$AssessedAt)

    Assert-Condition ($Profile.schemaVersion -eq '2.0.0') 'Rig profile schemaVersion is unsupported.'
    Assert-Condition ($Profile.status -eq 'approved') 'Product path accepts approved rig profiles only.'

    $raster = $Profile.outputRaster
    Assert-IntegerLiteral $raster.dpi 'outputRaster.dpi'
    Assert-IntegerLiteral $raster.widthPixels 'outputRaster.widthPixels'
    Assert-IntegerLiteral $raster.heightPixels 'outputRaster.heightPixels'
    $left = ConvertTo-Micrometres $raster.regionMm.left 'regionMm.left'
    $top = ConvertTo-Micrometres $raster.regionMm.top 'regionMm.top'
    $right = ConvertTo-Micrometres $raster.regionMm.right 'regionMm.right'
    $bottom = ConvertTo-Micrometres $raster.regionMm.bottom 'regionMm.bottom'
    Assert-Condition ($left -lt $right -and $top -lt $bottom) 'Output region is empty or reversed.'
    Assert-Condition ($raster.widthPixels -eq (Get-RequiredPixels ($right - $left) $raster.dpi)) 'Output width in pixels does not match the region and DPI.'
    Assert-Condition ($raster.heightPixels -eq (Get-RequiredPixels ($bottom - $top) $raster.dpi)) 'Output height in pixels does not match the region and DPI.'

    $corners = @(
        @($raster.regionMm.left, $raster.regionMm.top),
        @($raster.regionMm.right, $raster.regionMm.top),
        @($raster.regionMm.left, $raster.regionMm.bottom),
        @($raster.regionMm.right, $raster.regionMm.bottom)
    )
    foreach ($alias in @('CAM-A', 'CAM-B')) {
        $h = $Profile.cameras.$alias.projection.documentToImage
        foreach ($corner in $corners) {
            $w = [double]$h[2][0] * [double]$corner[0] + [double]$h[2][1] * [double]$corner[1] + [double]$h[2][2]
            Assert-Condition ([double]::IsFinite($w) -and $w -gt 0) "$alias projective denominator is not positive over the output region."
        }
    }

    $envelope = $Profile.correctionEnvelope
    foreach ($metricName in @('registrationErrorOutputPixels', 'rotationErrorDegrees', 'scaleDifferencePercent', 'exposureDifferenceEv', 'colorDeltaE')) {
        $metric = $envelope.$metricName
        Assert-Condition ($metric.targetMax -le $metric.autoCorrectionMax) "Correction envelope $metricName has targetMax above autoCorrectionMax."
    }

    $measuredAt = ConvertTo-UtcInstant $Profile.calibration.measuredAt 'calibration.measuredAt'
    $approvedAt = ConvertTo-UtcInstant $Profile.approval.approvedAt 'approval.approvedAt'
    $validUntil = ConvertTo-UtcInstant $Profile.approval.validUntil 'approval.validUntil'
    Assert-Condition ($measuredAt -le $approvedAt) 'Approval predates the calibration it approves.'
    Assert-Condition ($approvedAt -le $AssessedAt) 'Approval is later than the assessment time.'
    Assert-Condition ($AssessedAt -lt $validUntil) 'Approved rig profile has expired.'
}

function Assert-RuntimeAccepts {
    param([Parameter(Mandatory)]$Profile, [Parameter(Mandatory)][DateTimeOffset]$AssessedAt, [Parameter(Mandatory)][string]$CaseName)
    try { Assert-RigProfileV2RuntimeContract -Profile $Profile -AssessedAt $AssessedAt }
    catch { throw "Runtime contract rejected valid case: $CaseName. $($_.Exception.Message)" }
    $script:RuntimeCaseCount++
}

function Assert-RuntimeRejects {
    # Delegated case: the schema accepts it and the runtime contract rejects it.
    param([Parameter(Mandatory)]$Profile, [Parameter(Mandatory)][string]$SchemaPath, [Parameter(Mandatory)][DateTimeOffset]$AssessedAt, [Parameter(Mandatory)][string]$CaseName)
    Assert-Condition (Test-SchemaAccepts -Profile $Profile -SchemaPath $SchemaPath) "Delegated case must pass the schema: $CaseName."
    Assert-Throws { Assert-RigProfileV2RuntimeContract -Profile $Profile -AssessedAt $AssessedAt } $CaseName
    $script:RuntimeCaseCount++
}

function New-Projection {
    param([double]$RowOneOffset)
    return [pscustomobject]@{
        intrinsics = [pscustomobject]@{ fxPixels = 10000.0; fyPixels = 10000.0; cxPixels = 3679.5; cyPixels = 2455.5 }
        distortion = [pscustomobject]@{ model = 'brown-conrady-k1k2k3-p1p2'; k1 = 0.0; k2 = 0.0; k3 = 0.0; p1 = 0.0; p2 = 0.0 }
        documentToImage = @(
            @(0.0, 8.0, 100.0),
            @(-8.0, 0.0, $RowOneOffset),
            @(0.0, 0.0, 1.0)
        )
    }
}

function New-CalibratedDraft {
    param([Parameter(Mandatory)]$Template)
    $profile = Copy-Node $Template
    $profile.profileId = 'synthetic-schema-sentinel-draft'
    $profile.documentPlane = [pscustomobject]@{
        paper = 'ISO-216-A0'; orientation = 'landscape'; sheetWidthMm = 1189; sheetHeightMm = 841; cameraOrder = 'CAM-A-left-CAM-B-right'
    }
    $profile.cameras.'CAM-A'.projection = New-Projection 4800.0
    $profile.cameras.'CAM-B'.projection = New-Projection 9400.0
    $profile.calibration = [pscustomobject]@{
        method = 'chart-fiducial-planar'
        chartId = 'synthetic-chart-sentinel'
        provenanceId = 'synthetic-schema-sentinel'
        measuredAt = '2000-01-01T00:00:00Z'
        tool = [pscustomobject]@{ toolId = 'synthetic-calibration-tool'; version = '0.0.0' }
        inputSha256 = [pscustomobject]@{ 'CAM-A' = ('a' * 64); 'CAM-B' = ('b' * 64) }
        fitResidualPixels = [pscustomobject]@{
            'CAM-A' = [pscustomobject]@{ rms = 0.0; max = 0.0 }
            'CAM-B' = [pscustomobject]@{ rms = 0.0; max = 0.0 }
        }
        rigMeasurements = [pscustomobject]@{
            baselineMm = $null
            overlapMm = $null
            cameraToDocumentMm = [pscustomobject]@{ 'CAM-A' = $null; 'CAM-B' = $null }
            lensFocalLengthMm = [pscustomobject]@{ 'CAM-A' = $null; 'CAM-B' = $null }
        }
    }
    return $profile
}

function New-OutputRaster {
    param([int]$Dpi, [double]$Right, [double]$Bottom, [int]$WidthPixels, [int]$HeightPixels)
    return [pscustomobject]@{
        dpi = $Dpi
        regionMm = [pscustomobject]@{ left = 0; top = 0; right = $Right; bottom = $Bottom }
        widthPixels = $WidthPixels
        heightPixels = $HeightPixels
    }
}

function New-Approved {
    param([Parameter(Mandatory)]$CalibratedDraft)
    $profile = Copy-Node $CalibratedDraft
    $profile.status = 'approved'
    $profile.profileId = 'synthetic-schema-sentinel-approved'
    $profile.calibration.rigMeasurements.baselineMm = 1.0
    $profile.calibration.rigMeasurements.overlapMm = 1.0
    $profile.calibration.rigMeasurements.cameraToDocumentMm.'CAM-A' = 1.0
    $profile.calibration.rigMeasurements.cameraToDocumentMm.'CAM-B' = 1.0
    $profile.calibration.rigMeasurements.lensFocalLengthMm.'CAM-A' = 1.0
    $profile.calibration.rigMeasurements.lensFocalLengthMm.'CAM-B' = 1.0
    # 1189 mm and 841 mm at 180 dpi: ceil(1189000 * 180 / 25400) = 8426, ceil(841000 * 180 / 25400) = 5960.
    $profile.outputRaster = New-OutputRaster -Dpi 180 -Right 1189 -Bottom 841 -WidthPixels 8426 -HeightPixels 5960
    $metric = { [pscustomobject]@{ targetMax = 0.0; autoCorrectionMax = 0.0 } }
    $profile.correctionEnvelope = [pscustomobject]@{
        minimumOverlapMm = 0.0
        registrationErrorOutputPixels = & $metric
        rotationErrorDegrees = & $metric
        scaleDifferencePercent = & $metric
        exposureDifferenceEv = & $metric
        colorDeltaE = & $metric
    }
    $profile.qualityContract = [pscustomobject]@{ seamErrorOutputPixelsMax = 0.0; registrationErrorOutputPixelsMax = 0.0; colorDeltaEMax = 0.0 }
    $profile.approval = [pscustomobject]@{
        decisionRef = 'synthetic-schema-sentinel'
        approvedAt = '2000-01-02T00:00:00Z'
        validUntil = '2100-01-01T00:00:00Z'
    }
    return $profile
}

try {
    $schemaPath = Join-Path $RepositoryRoot 'docs/schemas/rig-profile.v2.schema.json'
    $examplePath = Join-Path $RepositoryRoot 'samples/public/rig-profile.v2.draft.example.json'
    $v1SchemaPath = Join-Path $RepositoryRoot 'docs/schemas/rig-profile.schema.json'
    $v1ExamplePath = Join-Path $RepositoryRoot 'samples/public/rig-profile.draft.example.json'

    $schema = Get-Content -Raw -LiteralPath $schemaPath | ConvertFrom-Json
    $example = Get-Content -Raw -LiteralPath $examplePath | ConvertFrom-Json
    $v1Schema = Get-Content -Raw -LiteralPath $v1SchemaPath | ConvertFrom-Json
    $v1Example = Get-Content -Raw -LiteralPath $v1ExamplePath | ConvertFrom-Json
    $assessedAt = [DateTimeOffset]'2026-01-01T00:00:00Z'

    # Schema shape.
    Assert-Condition ($schema.'$schema' -eq 'https://json-schema.org/draft/2020-12/schema') 'Schema must declare JSON Schema Draft 2020-12.'
    Assert-Condition ($schema.properties.schemaVersion.const -eq '2.0.0') 'Schema must guard schemaVersion 2.0.0.'
    Assert-Condition ((@($schema.properties.status.enum) -join ',') -eq 'draft,approved') 'Schema must support exactly draft and approved.'
    Assert-Condition ($schema.additionalProperties -eq $false) 'Schema must reject unknown top-level properties.'
    Assert-Condition ($schema.'$defs'.projection.properties.distortion.required -contains 'k3') 'Every distortion coefficient must be required.'
    Assert-StrictSchemaShape -Node $schema
    # Schema 1.1.0 stays in place for the existing tests (ADR-0034).
    Assert-Condition ($v1Schema.properties.schemaVersion.const -eq '1.1.0') 'Schema 1.1.0 must be retained beside the 2.0.0 proposal.'

    # Draft example: a template whose values are all null.
    Assert-Condition ($example.schemaVersion -eq '2.0.0' -and $example.status -eq 'draft') 'Draft example must be a 2.0.0 draft.'
    $exampleValues = @(
        $example.profileId, $example.documentPlane, $example.calibration, $example.outputRaster,
        $example.correctionEnvelope, $example.qualityContract, $example.approval,
        $example.cameras.'CAM-A'.projection, $example.cameras.'CAM-B'.projection,
        $example.cameras.'CAM-A'.physicalIdentity, $example.cameras.'CAM-B'.physicalIdentity
    )
    Assert-Condition (@($exampleValues | Where-Object { $null -ne $_ }).Count -eq 0) 'Draft example contains a rig, calibration, output, threshold or approval value.'
    Assert-Condition ((@($example.cameras.PSObject.Properties.Name) -join ',') -eq 'CAM-A,CAM-B') 'Draft example must define CAM-A and CAM-B only.'
    Assert-SchemaAccepts -Profile $example -SchemaPath $schemaPath -CaseName 'draft example (all null)'
    Assert-Throws { Assert-RigProfileV2RuntimeContract -Profile $example -AssessedAt $assessedAt } 'product path accepted the draft template'

    # Positive cases.
    $calibrated = New-CalibratedDraft -Template $example
    Assert-SchemaAccepts -Profile $calibrated -SchemaPath $schemaPath -CaseName 'calibrated draft without owner decisions'
    Assert-Throws { Assert-RigProfileV2RuntimeContract -Profile $calibrated -AssessedAt $assessedAt } 'product path accepted a calibrated draft'

    $approved = New-Approved -CalibratedDraft $calibrated
    Assert-SchemaAccepts -Profile $approved -SchemaPath $schemaPath -CaseName 'approved landscape sentinel'
    Assert-RuntimeAccepts -Profile $approved -AssessedAt $assessedAt -CaseName 'approved landscape sentinel'

    $portrait = Copy-Node $approved
    $portrait.documentPlane.orientation = 'portrait'
    $portrait.documentPlane.sheetWidthMm = 841
    $portrait.documentPlane.sheetHeightMm = 1189
    $portrait.documentPlane.cameraOrder = 'CAM-A-top-CAM-B-bottom'
    $portrait.outputRaster = New-OutputRaster -Dpi 180 -Right 841 -Bottom 1189 -WidthPixels 5960 -HeightPixels 8426
    Assert-SchemaAccepts -Profile $portrait -SchemaPath $schemaPath -CaseName 'approved portrait sentinel'
    Assert-RuntimeAccepts -Profile $portrait -AssessedAt $assessedAt -CaseName 'approved portrait sentinel'

    $cropped = Copy-Node $approved
    $cropped.outputRaster.regionMm.left = 10.5
    $cropped.outputRaster.regionMm.top = 7.25
    $cropped.outputRaster.regionMm.right = 1178.5
    $cropped.outputRaster.regionMm.bottom = 833.75
    # 1168 mm and 826.5 mm at 180 dpi: ceil(1168000 * 180 / 25400) = 8278, ceil(826500 * 180 / 25400) = 5858.
    $cropped.outputRaster.widthPixels = 8278
    $cropped.outputRaster.heightPixels = 5858
    Assert-SchemaAccepts -Profile $cropped -SchemaPath $schemaPath -CaseName 'approved sentinel with a cropped region'
    Assert-RuntimeAccepts -Profile $cropped -AssessedAt $assessedAt -CaseName 'approved sentinel with a cropped region'

    # Negative 1: a missing coefficient.
    $case = Copy-Node $approved
    $case.cameras.'CAM-A'.projection.distortion.PSObject.Properties.Remove('k3')
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'approved profile without k3'
    $case = Copy-Node $approved
    $case.cameras.'CAM-B'.projection.distortion.p2 = $null
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'approved profile with a null tangential coefficient'
    $case = Copy-Node $calibrated
    $case.cameras.'CAM-B'.projection.distortion.PSObject.Properties.Remove('k1')
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'calibrated draft without k1'
    $case = Copy-Node $approved
    $case.cameras.'CAM-A'.projection.intrinsics.PSObject.Properties.Remove('cyPixels')
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'approved profile without the principal point y'
    $case = Copy-Node $approved
    $case.cameras.'CAM-A'.projection.documentToImage = @(@(0.0, 8.0, 100.0), @(-8.0, 0.0, 4800.0))
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'approved profile with a two-row matrix'

    # Negative 2: DPI 0 and other invalid DPI.
    $case = Copy-Node $approved
    $case.outputRaster.dpi = 0
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'DPI 0'
    $case = Copy-Node $approved
    $case.outputRaster.dpi = 180.5
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'fractional DPI'
    $case = Copy-Node $approved
    $case.outputRaster.dpi = $null
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'approved profile with a null DPI'

    # Negative 3: output dimensions and crop that contradict each other.
    $case = Copy-Node $approved
    $case.outputRaster.regionMm.bottom = 1189
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'landscape region taller than the sheet'
    $case = Copy-Node $portrait
    $case.outputRaster.regionMm.right = 1189
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'portrait region wider than the sheet'
    $case = Copy-Node $approved
    $case.outputRaster.regionMm.left = 1189
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'region starting at the far sheet edge'
    $case = Copy-Node $approved
    $case.documentPlane.sheetWidthMm = 841
    $case.documentPlane.sheetHeightMm = 1189
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'landscape orientation with portrait sheet dimensions'
    $case = Copy-Node $approved
    $case.outputRaster.widthPixels = 0
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'zero output width'
    $case = Copy-Node $approved
    $case.outputRaster.regionMm.left = 0.0005
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'region edge below 1 micrometre resolution'

    # Negative 4: a draft that carries a value only approval may set.
    $case = Copy-Node $example
    $case.outputRaster = New-OutputRaster -Dpi 180 -Right 1189 -Bottom 841 -WidthPixels 8426 -HeightPixels 5960
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'draft template with an output raster'
    $case = Copy-Node $calibrated
    $case.outputRaster = New-OutputRaster -Dpi 180 -Right 1189 -Bottom 841 -WidthPixels 8426 -HeightPixels 5960
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'calibrated draft with an output raster'
    $case = Copy-Node $calibrated
    $case.approval = Copy-Node $approved.approval
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'draft with an approval record'
    $case = Copy-Node $calibrated
    $case.correctionEnvelope = Copy-Node $approved.correctionEnvelope
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'draft with a correction envelope'
    $case = Copy-Node $calibrated
    $case.qualityContract = Copy-Node $approved.qualityContract
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'draft with quality thresholds'
    $case = Copy-Node $example
    $case.cameras.'CAM-A'.projection = New-Projection 4800.0
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'draft with one projection and no calibration record'
    $case = Copy-Node $calibrated
    $case.calibration = $null
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'draft with projections but no calibration record'

    # Other negatives.
    $case = Copy-Node $example
    $case.schemaVersion = '1.1.0'
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName '2.0.0 document labelled 1.1.0'
    Assert-SchemaRejects -Profile $v1Example -SchemaPath $schemaPath -CaseName '1.1.0 draft example against the 2.0.0 schema'
    Assert-SchemaRejects -Profile $example -SchemaPath $v1SchemaPath -CaseName '2.0.0 draft example against the 1.1.0 schema'
    foreach ($block in @('documentPlane', 'calibration', 'outputRaster', 'correctionEnvelope', 'qualityContract', 'approval')) {
        $case = Copy-Node $approved
        $case.$block = $null
        Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName "approved profile with a null $block"
    }
    $case = Copy-Node $approved
    $case.cameras.'CAM-B'.projection = $null
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'approved profile without the CAM-B projection'
    $case = Copy-Node $approved
    $case.profileId = $null
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'approved profile without a profileId'
    $case = Copy-Node $approved
    $case.calibration.rigMeasurements.baselineMm = $null
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'approved profile with a missing rig measurement'
    $case = Copy-Node $approved
    $case.cameras.'CAM-A'.physicalIdentity = 'synthetic-identity'
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'physical identity present'
    $case = Copy-Node $approved
    $case.cameras.'CAM-A'.projection.documentToImage[2][2] = 2.0
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'matrix not normalized to H[2][2] = 1'
    $case = Copy-Node $approved
    $case.cameras.'CAM-A'.projection.documentToImage[0] = @(0.0, 8.0, 100.0, 0.0)
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'matrix row with four entries'
    $case = Copy-Node $approved
    $case.cameras.'CAM-A'.projection.intrinsics.fxPixels = 0.0
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'zero focal length in pixels'
    $case = Copy-Node $approved
    $case.cameras.'CAM-A'.projection.distortion.model = 'opencv-rational-k1-k6'
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'unsupported lens model'
    $case = Copy-Node $approved
    $case.cameras | Add-Member -NotePropertyName 'CAM-C' -NotePropertyValue (Copy-Node $approved.cameras.'CAM-A')
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'third camera'
    $case = Copy-Node $approved
    $case.cameras.PSObject.Properties.Remove('CAM-B')
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'missing CAM-B'
    $case = Copy-Node $approved
    $case | Add-Member -NotePropertyName 'notes' -NotePropertyValue 'free text'
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'unknown top-level property'
    $case = Copy-Node $approved
    $case.approval.approvedAt = '2000-01-02T09:00:00+09:00'
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'date-time with an offset instead of Z'
    $case = Copy-Node $approved
    $case.calibration.measuredAt = '2000-01-01T00:00:00.5Z'
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'date-time with fractional seconds'
    $case = Copy-Node $approved
    $case.conventions.cameraPixel = 'pixel-center-at-half-integer'
    Assert-SchemaRejects -Profile $case -SchemaPath $schemaPath -CaseName 'different pixel convention'

    # Delegated to the runtime contract: the schema accepts these and the runtime rejects them.
    $case = Copy-Node $approved
    $case.outputRaster.widthPixels = 8425
    Assert-RuntimeRejects -Profile $case -SchemaPath $schemaPath -AssessedAt $assessedAt -CaseName 'output width one pixel short of the region at the DPI'
    $case = Copy-Node $approved
    $case.outputRaster.heightPixels = 5961
    Assert-RuntimeRejects -Profile $case -SchemaPath $schemaPath -AssessedAt $assessedAt -CaseName 'output height one pixel more than the region at the DPI'
    $case = Copy-Node $approved
    $case.outputRaster.regionMm.left = 600
    $case.outputRaster.regionMm.right = 500
    Assert-RuntimeRejects -Profile $case -SchemaPath $schemaPath -AssessedAt $assessedAt -CaseName 'reversed region'
    $case = Copy-Node $approved
    $case.outputRaster.dpi = [double]180
    Assert-RuntimeRejects -Profile $case -SchemaPath $schemaPath -AssessedAt $assessedAt -CaseName 'DPI written as 180.0'
    $case = Copy-Node $approved
    $case.cameras.'CAM-B'.projection.documentToImage[2][0] = -0.001
    Assert-RuntimeRejects -Profile $case -SchemaPath $schemaPath -AssessedAt $assessedAt -CaseName 'projective denominator crossing zero inside the region'
    $case = Copy-Node $approved
    $case.correctionEnvelope.registrationErrorOutputPixels.targetMax = 2.0
    $case.correctionEnvelope.registrationErrorOutputPixels.autoCorrectionMax = 1.0
    Assert-RuntimeRejects -Profile $case -SchemaPath $schemaPath -AssessedAt $assessedAt -CaseName 'reversed correction envelope'
    $case = Copy-Node $approved
    $case.approval.approvedAt = '1999-12-31T00:00:00Z'
    Assert-RuntimeRejects -Profile $case -SchemaPath $schemaPath -AssessedAt $assessedAt -CaseName 'approval before calibration'
    $case = Copy-Node $approved
    $case.approval.approvedAt = '2027-01-01T00:00:00Z'
    Assert-RuntimeRejects -Profile $case -SchemaPath $schemaPath -AssessedAt $assessedAt -CaseName 'approval after the assessment time'
    $case = Copy-Node $approved
    $case.approval.validUntil = '2025-01-01T00:00:00Z'
    Assert-RuntimeRejects -Profile $case -SchemaPath $schemaPath -AssessedAt $assessedAt -CaseName 'expired approval'

    Write-Host "Rig profile 2.0.0 contract passed: $($script:SchemaCaseCount) schema cases, $($script:RuntimeCaseCount) runtime cases."
    exit 0
}
catch {
    Write-Error "Rig profile 2.0.0 contract failed: $($_.Exception.Message)"
    exit 1
}
