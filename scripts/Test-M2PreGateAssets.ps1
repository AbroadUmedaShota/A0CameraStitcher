[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..'))
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Copy-JsonObject {
    param([Parameter(Mandatory)]$Value)
    return ($Value | ConvertTo-Json -Depth 30 -Compress | ConvertFrom-Json)
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

function Assert-StrictSchemaShape {
    param([AllowNull()]$Node, [string]$Path = '$')
    if ($null -eq $Node) { return }
    if ($Node -is [System.Collections.IEnumerable] -and $Node -isnot [string] -and $Node -isnot [pscustomobject]) {
        $index = 0
        foreach ($child in $Node) { Assert-StrictSchemaShape -Node $child -Path "$Path[$index]"; $index++ }
        return
    }
    if ($Node -isnot [pscustomobject]) { return }
    $names = @($Node.PSObject.Properties.Name)
    if ($names -contains 'properties') {
        Assert-Condition ($names -contains 'type') "Schema object using properties must declare type at $Path."
        $types = @($Node.type)
        Assert-Condition ($types -contains 'object') "Schema properties owner must allow object at $Path."
    }
    if ($names -contains 'items') {
        Assert-Condition ($names -contains 'type') "Schema object using items must declare type at $Path."
        $types = @($Node.type)
        Assert-Condition ($types -contains 'array') "Schema items owner must allow array at $Path."
    }
    foreach ($property in $Node.PSObject.Properties) {
        Assert-StrictSchemaShape -Node $property.Value -Path "$Path.$($property.Name)"
    }
}

function Assert-RigProfileContract {
    param(
        [Parameter(Mandatory)]$Profile,
        [Parameter(Mandatory)]$Schema,
        [DateTimeOffset]$AssessedAt = [DateTimeOffset]::UtcNow
    )

    $propertyNames = @($Profile.PSObject.Properties.Name)
    foreach ($required in $Schema.required) {
        Assert-Condition ($propertyNames -contains $required) "Rig profile is missing required property $required."
    }
    Assert-Condition ($Profile.schemaVersion -eq $Schema.properties.schemaVersion.const) 'Rig profile schemaVersion is unsupported.'
    Assert-Condition (@($Schema.properties.status.enum) -contains $Profile.status) 'Rig profile status is unsupported.'
    Assert-Condition ($Profile.cameraModel.manufacturer -eq 'Nikon' -and $Profile.cameraModel.model -eq 'D810') 'Rig profile camera model is invalid.'
    Assert-Condition ($Profile.cameraModel.sensorWidthPixels -eq 7360 -and $Profile.cameraModel.sensorHeightPixels -eq 4912) 'Rig profile D810 dimensions are invalid.'
    Assert-Condition ($Profile.cameraSlots.Count -eq 2) 'Rig profile must have exactly two camera slots.'
    $aliases = @($Profile.cameraSlots.alias | Sort-Object)
    Assert-Condition (($aliases -join ',') -eq 'CAM-A,CAM-B') 'Rig profile must contain CAM-A and CAM-B exactly once.'
    Assert-Condition (($Profile.cameraSlots | Where-Object { $null -ne $_.physicalIdentity }).Count -eq 0) 'Rig profile must never contain a physical identity.'

    $cropValues = @($Profile.layout.crop.leftPercent, $Profile.layout.crop.topPercent, $Profile.layout.crop.rightPercent, $Profile.layout.crop.bottomPercent)
    $qualityValues = @($Profile.qualityContract.seamErrorPixelsMax, $Profile.qualityContract.registrationErrorPixelsMax, $Profile.qualityContract.colorDeltaEMax)
    $correctionMetrics = @(
        $Profile.correctionEnvelope.registrationErrorPixels,
        $Profile.correctionEnvelope.rotationErrorDegrees,
        $Profile.correctionEnvelope.scaleDifferencePercent,
        $Profile.correctionEnvelope.exposureDifferenceEv,
        $Profile.correctionEnvelope.colorDeltaE
    )
    $correctionValues = @($Profile.correctionEnvelope.minimumOverlapPercent)
    foreach ($metric in $correctionMetrics) { $correctionValues += @($metric.targetMax, $metric.autoCorrectionMax) }
    if ($Profile.status -eq 'draft') {
        $draftValues = @(
            $Profile.cameraSlots[0].rotationDegrees, $Profile.cameraSlots[1].rotationDegrees,
            $Profile.layout.orientation, $Profile.layout.cameraOrder, $Profile.layout.overlapPercent
        ) + $cropValues + @(
            $Profile.optics.focalLengthMm, $Profile.optics.cameraToOriginalMm, $Profile.optics.targetDpi,
            $Profile.calibration.method, $Profile.calibration.homography, $Profile.calibration.measuredAt,
            $Profile.calibration.provenanceId, $Profile.calibration.validUntil
        ) + $qualityValues + $correctionValues
        Assert-Condition (($draftValues | Where-Object { $null -ne $_ }).Count -eq 0) 'Draft rig profile contains an unapproved value.'
        return
    }

    foreach ($slot in $Profile.cameraSlots) {
        Assert-Condition (@(0, 90, 180, 270) -contains $slot.rotationDegrees) 'Approved profile rotation is invalid.'
    }
    Assert-Condition (@('landscape', 'portrait') -contains $Profile.layout.orientation) 'Approved profile orientation is invalid.'
    Assert-Condition (@('CAM-A-left-CAM-B-right', 'CAM-A-top-CAM-B-bottom') -contains $Profile.layout.cameraOrder) 'Approved profile camera order is invalid.'
    Assert-Condition ($Profile.layout.overlapPercent -is [ValueType] -and $Profile.layout.overlapPercent -ge 0 -and $Profile.layout.overlapPercent -lt 100) 'Approved profile overlap is out of bounds.'
    foreach ($crop in $cropValues) { Assert-Condition ($crop -is [ValueType] -and $crop -ge 0 -and $crop -le 100) 'Approved profile crop is out of bounds.' }
    Assert-Condition ($Profile.optics.focalLengthMm -is [ValueType] -and $Profile.optics.focalLengthMm -gt 0) 'Approved focal length must be positive.'
    Assert-Condition ($Profile.optics.cameraToOriginalMm -is [ValueType] -and $Profile.optics.cameraToOriginalMm -gt 0) 'Approved camera distance must be positive.'
    Assert-Condition ($Profile.optics.targetDpi -is [ValueType] -and $Profile.optics.targetDpi -ge 1 -and [math]::Floor([double]$Profile.optics.targetDpi) -eq $Profile.optics.targetDpi) 'Approved target DPI must be a positive integer.'
    Assert-Condition ($Profile.calibration.method -eq 'planar-homography') 'Approved profile must use planar homography.'
    Assert-Condition ($Profile.calibration.homography.Count -eq 9 -and ($Profile.calibration.homography | Where-Object { $_ -isnot [ValueType] }).Count -eq 0) 'Approved homography must contain nine numbers.'
    $parsedDate = [DateTimeOffset]::MinValue
    Assert-Condition ([DateTimeOffset]::TryParse([string]$Profile.calibration.measuredAt, [ref]$parsedDate)) 'Approved measuredAt must be a date-time.'
    $validUntil = [DateTimeOffset]::MinValue
    Assert-Condition ([DateTimeOffset]::TryParse([string]$Profile.calibration.validUntil, [ref]$validUntil) -and $validUntil -gt $parsedDate) 'Approved validUntil must be later than measuredAt.'
    Assert-Condition ($parsedDate -le $AssessedAt) 'Approved measuredAt must not be later than the assessment time.'
    Assert-Condition ($validUntil -gt $AssessedAt) 'Approved rig profile has expired.'
    Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$Profile.calibration.provenanceId)) 'Approved provenanceId must be present.'
    Assert-Condition ($Profile.correctionEnvelope.minimumOverlapPercent -is [ValueType] -and $Profile.correctionEnvelope.minimumOverlapPercent -ge 0 -and $Profile.correctionEnvelope.minimumOverlapPercent -le 100) 'Approved minimum overlap is out of bounds.'
    foreach ($metric in $correctionMetrics) {
        Assert-Condition ($metric.targetMax -is [ValueType] -and $metric.targetMax -ge 0) 'Approved correction target must be non-negative.'
        Assert-Condition ($metric.autoCorrectionMax -is [ValueType] -and $metric.autoCorrectionMax -ge $metric.targetMax) 'Approved auto-correction limit must be at least the target limit.'
    }
    foreach ($quality in $qualityValues) { Assert-Condition ($quality -is [ValueType] -and $quality -ge 0) 'Approved quality bounds must be non-negative numbers.' }
}

function Assert-SchemaAccepts {
    param([Parameter(Mandatory)]$Profile, [Parameter(Mandatory)][string]$SchemaPath, [Parameter(Mandatory)][string]$CaseName)
    $json = $Profile | ConvertTo-Json -Depth 30 -Compress
    $accepted = Test-Json -Json $json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue
    Assert-Condition $accepted "JSON Schema rejected valid case: $CaseName."
}

function Assert-SchemaRejects {
    param([Parameter(Mandatory)]$Profile, [Parameter(Mandatory)][string]$SchemaPath, [Parameter(Mandatory)][string]$CaseName)
    $json = $Profile | ConvertTo-Json -Depth 30 -Compress
    $accepted = Test-Json -Json $json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue
    Assert-Condition (-not $accepted) "JSON Schema accepted invalid case: $CaseName."
}

function Measure-SyntheticPair {
    param([Parameter(Mandatory)]$Fixture)
    Assert-Condition ($Fixture.schemaVersion -eq 'm2.synthetic-pixel-pair.v1') 'Synthetic pair version is invalid.'
    Assert-Condition ($Fixture.simulation -eq $true -and $Fixture.marker -eq 'Synthetic') 'Synthetic pair marker is invalid.'
    Assert-Condition ($Fixture.approvalState -eq 'test-only' -and $Fixture.qualityDecision -eq 'not-evaluated') 'Synthetic pair must remain test-only and not-evaluated.'
    Assert-Condition ($Fixture.rights -eq 'wholly-synthetic-generated-values') 'Synthetic pair rights marker is invalid.'

    $a = $Fixture.sources.camA
    $b = $Fixture.sources.camB
    foreach ($source in @($a, $b)) {
        Assert-Condition ($source.pixels.Count -eq $source.heightPixels) 'Synthetic source row count is invalid.'
        foreach ($row in $source.pixels) {
            Assert-Condition ($row.Count -eq $source.widthPixels) 'Synthetic source column count is invalid.'
            Assert-Condition (($row | Where-Object { $_ -isnot [ValueType] }).Count -eq 0) 'Synthetic source contains a non-numeric pixel.'
        }
    }
    Assert-Condition ($Fixture.placement.camA.y -eq $Fixture.placement.camB.y -and $a.heightPixels -eq $b.heightPixels) 'Current deterministic oracle requires equal-height horizontal pairs.'
    $overlapLeft = [math]::Max($Fixture.placement.camA.x, $Fixture.placement.camB.x)
    $overlapRight = [math]::Min($Fixture.placement.camA.x + $a.widthPixels, $Fixture.placement.camB.x + $b.widthPixels)
    $overlapWidth = $overlapRight - $overlapLeft
    Assert-Condition ($overlapWidth -gt 0) 'Synthetic pair must overlap.'
    [double]$sum = 0
    [double]$maximum = 0
    [int]$count = 0
    for ($y = 0; $y -lt $a.heightPixels; $y++) {
        for ($x = $overlapLeft; $x -lt $overlapRight; $x++) {
            $difference = [math]::Abs([double]$a.pixels[$y][$x - $Fixture.placement.camA.x] - [double]$b.pixels[$y][$x - $Fixture.placement.camB.x])
            $sum += $difference
            $maximum = [math]::Max($maximum, $difference)
            $count++
        }
    }
    return [pscustomobject]@{
        canvasWidthPixels = [math]::Max($Fixture.placement.camA.x + $a.widthPixels, $Fixture.placement.camB.x + $b.widthPixels)
        canvasHeightPixels = [math]::Max($Fixture.placement.camA.y + $a.heightPixels, $Fixture.placement.camB.y + $b.heightPixels)
        overlapWidthPixels = $overlapWidth
        rawOverlapMeanAbsoluteError = $sum / $count
        rawOverlapMaxAbsoluteError = $maximum
    }
}

try {
    $schemaPath = Join-Path $RepositoryRoot 'docs/schemas/rig-profile.schema.json'
    $examplePath = Join-Path $RepositoryRoot 'samples/public/rig-profile.draft.example.json'
    $svgPath = Join-Path $RepositoryRoot 'samples/public/a0-synthetic-chart.svg'
    $fixtureRoot = Join-Path $RepositoryRoot 'samples/public/m2-fixtures'
    $fixtureManifestPath = Join-Path $fixtureRoot 'manifest.json'

    $schema = Get-Content -Raw $schemaPath | ConvertFrom-Json
    $example = Get-Content -Raw $examplePath | ConvertFrom-Json
    [xml]$svg = Get-Content -Raw $svgPath
    $fixtureManifest = Get-Content -Raw $fixtureManifestPath | ConvertFrom-Json

    Assert-Condition ($schema.'$schema' -eq 'https://json-schema.org/draft/2020-12/schema') 'Schema must declare JSON Schema Draft 2020-12.'
    Assert-Condition ($schema.properties.schemaVersion.const -eq '1.1.0') 'Schema must guard schemaVersion 1.1.0.'
    Assert-Condition (($schema.properties.status.enum -contains 'draft') -and ($schema.properties.status.enum -contains 'approved')) 'Schema must support draft and approved states.'
    Assert-Condition ($schema.properties.cameraModel.properties.model.const -eq 'D810') 'Schema must guard the D810 model.'
    Assert-Condition ($schema.properties.cameraModel.properties.sensorWidthPixels.const -eq 7360) 'Schema must guard D810 width.'
    Assert-Condition ($schema.properties.cameraModel.properties.sensorHeightPixels.const -eq 4912) 'Schema must guard D810 height.'
    Assert-Condition ($schema.allOf.Count -eq 2) 'Schema must define separate draft and approved guards.'
    $draftRotationSchema = $schema.allOf[0].then.properties.cameraSlots.items.properties.rotationDegrees
    Assert-Condition (@($draftRotationSchema.PSObject.Properties.Name) -contains 'const') 'Draft rotation guard must declare const explicitly.'
    Assert-Condition ($null -eq $draftRotationSchema.const) 'Draft guard must keep camera rotation unapproved.'
    Assert-Condition ($schema.allOf[1].then.properties.calibration.properties.method.const -eq 'planar-homography') 'Approved guard must require planar homography.'
    Assert-Condition ($schema.allOf[1].then.properties.correctionEnvelope.properties.registrationErrorPixels.properties.autoCorrectionMax.type -eq 'number') 'Approved guard must require numeric correction limits.'
    Assert-Condition ($schema.allOf[1].then.properties.qualityContract.properties.seamErrorPixelsMax.type -eq 'number') 'Approved guard must require numeric quality thresholds.'
    Assert-StrictSchemaShape -Node $schema

    Assert-Condition ($example.schemaVersion -eq '1.1.0') 'Draft example schemaVersion is invalid.'
    Assert-Condition ($example.status -eq 'draft') 'Example must remain a draft.'
    Assert-Condition ($example.cameraSlots.Count -eq 2) 'Example must define exactly two camera slots.'
    Assert-Condition (($example.cameraSlots.alias -contains 'CAM-A') -and ($example.cameraSlots.alias -contains 'CAM-B')) 'Example must define CAM-A and CAM-B.'
    Assert-Condition ($example.cameraModel.model -eq 'D810' -and $example.cameraModel.sensorWidthPixels -eq 7360 -and $example.cameraModel.sensorHeightPixels -eq 4912) 'Example camera model dimensions are invalid.'
    $draftValues = @(
        $example.layout.orientation, $example.layout.cameraOrder, $example.layout.overlapPercent,
        $example.layout.crop.leftPercent, $example.layout.crop.topPercent, $example.layout.crop.rightPercent, $example.layout.crop.bottomPercent,
        $example.optics.focalLengthMm, $example.optics.cameraToOriginalMm, $example.optics.targetDpi,
        $example.calibration.method, $example.calibration.homography, $example.calibration.measuredAt,
        $example.calibration.provenanceId, $example.calibration.validUntil,
        $example.correctionEnvelope.minimumOverlapPercent,
        $example.correctionEnvelope.registrationErrorPixels.targetMax, $example.correctionEnvelope.registrationErrorPixels.autoCorrectionMax,
        $example.correctionEnvelope.rotationErrorDegrees.targetMax, $example.correctionEnvelope.rotationErrorDegrees.autoCorrectionMax,
        $example.correctionEnvelope.scaleDifferencePercent.targetMax, $example.correctionEnvelope.scaleDifferencePercent.autoCorrectionMax,
        $example.correctionEnvelope.exposureDifferenceEv.targetMax, $example.correctionEnvelope.exposureDifferenceEv.autoCorrectionMax,
        $example.correctionEnvelope.colorDeltaE.targetMax, $example.correctionEnvelope.colorDeltaE.autoCorrectionMax,
        $example.qualityContract.seamErrorPixelsMax, $example.qualityContract.registrationErrorPixelsMax, $example.qualityContract.colorDeltaEMax
    )
    Assert-Condition (($draftValues | Where-Object { $null -ne $_ }).Count -eq 0) 'Draft example contains an unapproved rig, DPI, threshold, or calibration value.'
    Assert-Condition (($example.cameraSlots | Where-Object { $null -ne $_.physicalIdentity }).Count -eq 0) 'Draft example must not contain physical camera identities.'
    Assert-RigProfileContract -Profile $example -Schema $schema
    Assert-SchemaAccepts -Profile $example -SchemaPath $schemaPath -CaseName 'draft example'

    $roundTripJson = $example | ConvertTo-Json -Depth 30 -Compress
    $roundTrip = $roundTripJson | ConvertFrom-Json
    Assert-RigProfileContract -Profile $roundTrip -Schema $schema
    Assert-SchemaAccepts -Profile $roundTrip -SchemaPath $schemaPath -CaseName 'draft round-trip'
    Assert-Condition (($roundTrip | ConvertTo-Json -Depth 30 -Compress) -eq $roundTripJson) 'Draft profile round-trip changed data.'

    $wrongVersion = Copy-JsonObject $example
    $wrongVersion.schemaVersion = '2.0.0'
    Assert-Throws { Assert-RigProfileContract -Profile $wrongVersion -Schema $schema } 'wrong schema version'
    Assert-SchemaRejects -Profile $wrongVersion -SchemaPath $schemaPath -CaseName 'wrong schema version'

    $draftWithValue = Copy-JsonObject $example
    $draftWithValue.optics.targetDpi = 180
    Assert-Throws { Assert-RigProfileContract -Profile $draftWithValue -Schema $schema } 'draft with unapproved value'
    Assert-SchemaRejects -Profile $draftWithValue -SchemaPath $schemaPath -CaseName 'draft with unapproved value'

    # Schema-exercise sentinels only. This in-memory object is never emitted or persisted as an approved rig.
    $approved = Copy-JsonObject $example
    $approved.status = 'approved'
    $approved.cameraSlots[0].rotationDegrees = 0
    $approved.cameraSlots[1].rotationDegrees = 0
    $approved.layout.orientation = 'landscape'
    $approved.layout.cameraOrder = 'CAM-A-left-CAM-B-right'
    $approved.layout.overlapPercent = 0
    $approved.layout.crop.leftPercent = 0
    $approved.layout.crop.topPercent = 0
    $approved.layout.crop.rightPercent = 0
    $approved.layout.crop.bottomPercent = 0
    $approved.optics.focalLengthMm = 1
    $approved.optics.cameraToOriginalMm = 1
    $approved.optics.targetDpi = 1
    $approved.calibration.method = 'planar-homography'
    $approved.calibration.homography = @(1, 0, 0, 0, 1, 0, 0, 0, 1)
    $approved.calibration.measuredAt = '2000-01-01T00:00:00Z'
    $approved.calibration.provenanceId = 'synthetic-schema-sentinel'
    $approved.calibration.validUntil = '2100-01-01T00:00:00Z'
    $approved.correctionEnvelope.minimumOverlapPercent = 0
    foreach ($metric in @(
        $approved.correctionEnvelope.registrationErrorPixels,
        $approved.correctionEnvelope.rotationErrorDegrees,
        $approved.correctionEnvelope.scaleDifferencePercent,
        $approved.correctionEnvelope.exposureDifferenceEv,
        $approved.correctionEnvelope.colorDeltaE
    )) {
        $metric.targetMax = 0
        $metric.autoCorrectionMax = 0
    }
    $approved.qualityContract.seamErrorPixelsMax = 0
    $approved.qualityContract.registrationErrorPixelsMax = 0
    $approved.qualityContract.colorDeltaEMax = 0
    Assert-RigProfileContract -Profile $approved -Schema $schema
    Assert-SchemaAccepts -Profile $approved -SchemaPath $schemaPath -CaseName 'approved schema sentinel'

    $approvedNull = Copy-JsonObject $approved
    $approvedNull.optics.targetDpi = $null
    Assert-Throws { Assert-RigProfileContract -Profile $approvedNull -Schema $schema } 'approved null value'
    Assert-SchemaRejects -Profile $approvedNull -SchemaPath $schemaPath -CaseName 'approved null value'
    $badOverlap = Copy-JsonObject $approved
    $badOverlap.layout.overlapPercent = 100
    Assert-Throws { Assert-RigProfileContract -Profile $badOverlap -Schema $schema } 'overlap upper bound'
    Assert-SchemaRejects -Profile $badOverlap -SchemaPath $schemaPath -CaseName 'overlap upper bound'
    $badCrop = Copy-JsonObject $approved
    $badCrop.layout.crop.leftPercent = -1
    Assert-Throws { Assert-RigProfileContract -Profile $badCrop -Schema $schema } 'crop lower bound'
    Assert-SchemaRejects -Profile $badCrop -SchemaPath $schemaPath -CaseName 'crop lower bound'
    $badDpi = Copy-JsonObject $approved
    $badDpi.optics.targetDpi = 0
    Assert-Throws { Assert-RigProfileContract -Profile $badDpi -Schema $schema } 'DPI lower bound'
    Assert-SchemaRejects -Profile $badDpi -SchemaPath $schemaPath -CaseName 'DPI lower bound'
    $badCorrectionEnvelope = Copy-JsonObject $approved
    $badCorrectionEnvelope.correctionEnvelope.registrationErrorPixels.targetMax = 2
    $badCorrectionEnvelope.correctionEnvelope.registrationErrorPixels.autoCorrectionMax = 1
    Assert-Throws { Assert-RigProfileContract -Profile $badCorrectionEnvelope -Schema $schema } 'reversed correction envelope'
    Assert-SchemaAccepts -Profile $badCorrectionEnvelope -SchemaPath $schemaPath -CaseName 'reversed envelope shape delegated to runtime contract'
    $unsupportedVersion = Copy-JsonObject $approved
    $unsupportedVersion.schemaVersion = '9.9.9'
    Assert-Throws { Assert-RigProfileContract -Profile $unsupportedVersion -Schema $schema } 'unsupported profile version'
    Assert-SchemaRejects -Profile $unsupportedVersion -SchemaPath $schemaPath -CaseName 'unsupported profile version'
    $missingProvenance = Copy-JsonObject $approved
    $missingProvenance.calibration.provenanceId = ''
    Assert-Throws { Assert-RigProfileContract -Profile $missingProvenance -Schema $schema } 'missing provenance'
    Assert-SchemaRejects -Profile $missingProvenance -SchemaPath $schemaPath -CaseName 'missing provenance'
    $invalidValidityOrder = Copy-JsonObject $approved
    $invalidValidityOrder.calibration.validUntil = $invalidValidityOrder.calibration.measuredAt
    Assert-Throws { Assert-RigProfileContract -Profile $invalidValidityOrder -Schema $schema -AssessedAt ([DateTimeOffset]'2000-01-01T00:00:00Z') } 'validity not later than calibration'
    Assert-SchemaAccepts -Profile $invalidValidityOrder -SchemaPath $schemaPath -CaseName 'validity ordering delegated to runtime contract'
    $expiredProfile = Copy-JsonObject $approved
    $expiredProfile.calibration.validUntil = '2025-01-01T00:00:00Z'
    Assert-Throws { Assert-RigProfileContract -Profile $expiredProfile -Schema $schema -AssessedAt ([DateTimeOffset]'2026-08-07T00:00:00Z') } 'expired approved profile'
    Assert-SchemaAccepts -Profile $expiredProfile -SchemaPath $schemaPath -CaseName 'expiry delegated to runtime contract'
    $futureCalibration = Copy-JsonObject $approved
    $futureCalibration.calibration.measuredAt = '2027-01-01T00:00:00Z'
    Assert-Throws { Assert-RigProfileContract -Profile $futureCalibration -Schema $schema -AssessedAt ([DateTimeOffset]'2026-08-07T00:00:00Z') } 'future calibration timestamp'
    Assert-SchemaAccepts -Profile $futureCalibration -SchemaPath $schemaPath -CaseName 'assessment-time ordering delegated to runtime contract'

    $root = $svg.DocumentElement
    Assert-Condition ($root.GetAttribute('width') -eq '841mm' -and $root.GetAttribute('height') -eq '1189mm' -and $root.GetAttribute('viewBox') -eq '0 0 841 1189') 'SVG must be A0 portrait (841 x 1189 mm) with matching viewBox.'
    $svgText = Get-Content -Raw $svgPath
    foreach ($marker in @('id="fiducial"', 'GRAYSCALE PATCHES', 'COLOUR PATCHES', 'TOP ↑', 'CENTER', 'minorGrid', 'majorGrid')) {
        Assert-Condition ($svgText.Contains($marker)) "SVG is missing required marker: $marker"
    }

    Assert-Condition ($fixtureManifest.schemaVersion -eq 'm2.synthetic-stitch-manifest.v1') 'Synthetic fixture manifest version is invalid.'
    Assert-Condition ($fixtureManifest.approvalState -eq 'test-only' -and $fixtureManifest.qualityDecision -eq 'not-evaluated') 'Synthetic fixture manifest must not make a quality decision.'
    Assert-Condition ($fixtureManifest.fixtures.Count -eq 3) 'Synthetic fixture manifest must include one perfect pair and two damaged variants.'
    $fixturePaths = @($fixtureManifest.fixtures | ForEach-Object { Join-Path $fixtureRoot $_.file })
    $beforeHashes = @{}
    foreach ($fixturePath in $fixturePaths) { $beforeHashes[$fixturePath] = (Get-FileHash -Algorithm SHA256 -LiteralPath $fixturePath).Hash }
    foreach ($manifestItem in $fixtureManifest.fixtures) {
        $fixturePath = Join-Path $fixtureRoot $manifestItem.file
        $fixture = Get-Content -Raw -LiteralPath $fixturePath | ConvertFrom-Json
        Assert-Condition ($fixture.id -eq $manifestItem.id) "Synthetic fixture id mismatch for $($manifestItem.file)."
        $measured = Measure-SyntheticPair -Fixture $fixture
        foreach ($metricName in @('canvasWidthPixels', 'canvasHeightPixels', 'overlapWidthPixels', 'rawOverlapMeanAbsoluteError', 'rawOverlapMaxAbsoluteError')) {
            Assert-Condition ([double]$measured.$metricName -eq [double]$fixture.expectedMetrics.$metricName) "Fixture metric $metricName does not match its oracle for $($fixture.id)."
        }
        Assert-Condition ($measured.overlapWidthPixels -eq $manifestItem.overlapWidthPixels) "Manifest overlap mismatch for $($fixture.id)."
        Assert-Condition ($measured.rawOverlapMeanAbsoluteError -eq $manifestItem.rawOverlapMeanAbsoluteError) "Manifest mean-error mismatch for $($fixture.id)."
        Assert-Condition ($measured.rawOverlapMaxAbsoluteError -eq $manifestItem.rawOverlapMaxAbsoluteError) "Manifest max-error mismatch for $($fixture.id)."
    }
    $damagedFixtures = @($fixtureManifest.fixtures | Where-Object { $_.id -like 'pair-damaged-*' })
    Assert-Condition ($damagedFixtures.Count -eq 2) 'Synthetic fixture set must contain two named damaged variants.'
    foreach ($fixturePath in $fixturePaths) {
        Assert-Condition ((Get-FileHash -Algorithm SHA256 -LiteralPath $fixturePath).Hash -eq $beforeHashes[$fixturePath]) "Synthetic original was modified during validation: $fixturePath"
    }

    Write-Host 'M2 pre-gate assets passed validation.'
    exit 0
}
catch {
    Write-Error "M2 pre-gate assets validation failed: $($_.Exception.Message)"
    exit 1
}
