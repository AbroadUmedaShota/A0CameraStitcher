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

function Assert-Rejected {
    param([Parameter(Mandatory)][scriptblock]$Action, [Parameter(Mandatory)][string]$CaseName)
    try {
        & $Action
        throw "Expected rejection: $CaseName"
    }
    catch {
        if ($_.Exception.Message -eq "Expected rejection: $CaseName") { throw }
    }
}

function Assert-SchemaAccepts {
    param([Parameter(Mandatory)]$Value, [Parameter(Mandatory)][string]$SchemaPath, [Parameter(Mandatory)][string]$CaseName)
    $json = $Value | ConvertTo-Json -Depth 30 -Compress
    Assert-Condition (Test-Json -Json $json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue) "JSON Schema rejected valid case: $CaseName."
}

function Assert-SchemaRejects {
    param([Parameter(Mandatory)]$Value, [Parameter(Mandatory)][string]$SchemaPath, [Parameter(Mandatory)][string]$CaseName)
    $json = $Value | ConvertTo-Json -Depth 30 -Compress
    Assert-Condition (-not (Test-Json -Json $json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue)) "JSON Schema accepted invalid case: $CaseName."
}

function Assert-UniqueValues {
    param([Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Values, [Parameter(Mandatory)][string]$Name)
    $normalized = @($Values | ForEach-Object { [string]$_ })
    Assert-Condition (($normalized | Sort-Object -Unique).Count -eq $normalized.Count) "$Name contains a duplicate value."
}

function Assert-NoForbiddenPropertyNames {
    param([AllowNull()]$Node, [string]$Path = '$')
    if ($null -eq $Node) { return }
    if ($Node -is [System.Collections.IEnumerable] -and $Node -isnot [string] -and $Node -isnot [pscustomobject]) {
        $index = 0
        foreach ($child in $Node) {
            Assert-NoForbiddenPropertyNames -Node $child -Path "$Path[$index]"
            $index++
        }
        return
    }
    if ($Node -isnot [pscustomobject]) { return }
    $forbidden = @('serial', 'serialNumber', 'cameraSerial', 'physicalIdentity', 'customerOriginal', 'credential', 'sdkArchive', 'sdkPath')
    foreach ($property in $Node.PSObject.Properties) {
        Assert-Condition ($forbidden -notcontains $property.Name) "Forbidden property $($property.Name) at $Path."
        Assert-NoForbiddenPropertyNames -Node $property.Value -Path "$Path.$($property.Name)"
    }
}

function Resolve-RepositoryArtifact {
    param([Parameter(Mandatory)][string]$RelativePath)
    Assert-Condition (-not [IO.Path]::IsPathRooted($RelativePath)) 'Corpus artifact path must be repository-relative.'
    Assert-Condition ($RelativePath -notmatch '(^|[\\/])\.\.([\\/]|$)') 'Corpus artifact path must not traverse outside the repository.'
    $root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $candidate = [IO.Path]::GetFullPath((Join-Path $root $RelativePath))
    Assert-Condition ($candidate.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) 'Corpus artifact path escaped the repository.'
    $item = Get-Item -LiteralPath $candidate -Force -ErrorAction Stop
    Assert-Condition (-not $item.PSIsContainer) 'Corpus artifact must be a regular file.'
    Assert-Condition (-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Corpus artifact must not be a reparse point.'
    return $item.FullName
}

function Assert-CorpusContract {
    param(
        [Parameter(Mandatory)]$Rights,
        [Parameter(Mandatory)]$Oracle,
        [Parameter(Mandatory)]$Manifest
    )

    Assert-NoForbiddenPropertyNames $Rights
    Assert-NoForbiddenPropertyNames $Oracle
    Assert-NoForbiddenPropertyNames $Manifest

    $requiredUses = @('calibration-design', 'development-validation', 'locked-holdout-evaluation', 'release-confirmation')
    Assert-Condition ((@($Rights.approvedUses | Sort-Object) -join ',') -eq (($requiredUses | Sort-Object) -join ',')) 'Rights approvedUses must cover the four isolated purposes exactly.'
    Assert-UniqueValues -Values @($Rights.assets.assetId) -Name 'Rights assetId'
    Assert-UniqueValues -Values @($Rights.assets.repositoryPath | Where-Object { $null -ne $_ }) -Name 'Rights repositoryPath'
    Assert-UniqueValues -Values @($Rights.assets.externalObjectId | Where-Object { $null -ne $_ }) -Name 'Rights externalObjectId'
    Assert-UniqueValues -Values @($Rights.assets.contentSha256) -Name 'Rights contentSha256'

    $rightsById = @{}
    foreach ($asset in $Rights.assets) {
        if ($asset.assetKind -eq 'SelfAuthoredVectorSpec') {
            $path = Resolve-RepositoryArtifact -RelativePath $asset.repositoryPath
            $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            Assert-Condition ($actualHash -ceq [string]$asset.contentSha256) "Rights hash mismatch for $($asset.assetId)."
            $spec = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
            Assert-Condition ($spec.marker -eq 'Synthetic' -and $spec.authoringSource -eq 'in-house-original-work') "Vector spec provenance is invalid for $($asset.assetId)."
            Assert-Condition ($spec.qualityDecision -eq 'not-evaluated') "Vector spec claims a quality decision for $($asset.assetId)."
            Assert-Condition (-not $spec.containsCustomerContent -and -not $spec.containsCameraIdentity -and -not $spec.containsLicensedSdkMaterial) "Vector spec contains prohibited material for $($asset.assetId)."
        }
        else {
            Assert-Condition ($asset.assetKind -eq 'ExternalD810Pair' -and $null -eq $asset.repositoryPath -and $null -ne $asset.externalObjectId) "External rights provenance is invalid for $($asset.assetId)."
        }
        $rightsById[$asset.assetId] = $asset
    }

    Assert-Condition ($Oracle.independence.oracleOwnerRole -ne $Oracle.independence.productionPipelineOwnerRole) 'Oracle owner must be independent from the production pipeline owner.'
    Assert-Condition ($Oracle.independence.independentReviewerRole -ne $Oracle.independence.productionPipelineOwnerRole) 'Oracle reviewer must be independent from the production pipeline owner.'
    Assert-Condition ($Oracle.independence.independentReviewerRole -ne $Oracle.independence.oracleOwnerRole) 'Oracle reviewer must be independent from the oracle owner.'
    Assert-Condition (-not $Oracle.independence.usesProductionImplementation -and -not $Oracle.independence.usesProductionMetricOutput -and -not $Oracle.independence.usesTrainingFeatures -and -not $Oracle.independence.derivesExpectedFromProductOutput) 'Oracle reuses a production or training signal.'
    Assert-UniqueValues -Values @($Oracle.expectedOutcomes.fixtureId) -Name 'Oracle fixtureId'
    $oracleByFixture = @{}
    foreach ($outcome in $Oracle.expectedOutcomes) { $oracleByFixture[$outcome.fixtureId] = $outcome }

    $expectedPolicies = @{
        'calibration' = @('profile-calibration', 'engineering-visible', $true, $true)
        'development' = @('implementation-development', 'engineering-visible', $true, $true)
        'locked-holdout' = @('blind-acceptance-evaluation', 'blind-until-evaluation', $false, $false)
        'release' = @('release-confirmation', 'release-only', $false, $false)
    }
    Assert-UniqueValues -Values @($Manifest.splitPolicies.split) -Name 'split policy'
    Assert-Condition ($Manifest.splitPolicies.Count -eq $expectedPolicies.Count) 'Manifest must define exactly four split policies.'
    foreach ($policy in $Manifest.splitPolicies) {
        Assert-Condition $expectedPolicies.ContainsKey([string]$policy.split) "Unsupported split policy: $($policy.split)."
        $expected = $expectedPolicies[[string]$policy.split]
        Assert-Condition ($policy.purpose -eq $expected[0] -and $policy.accessRule -eq $expected[1] -and $policy.mutable -eq $expected[2] -and $policy.mayTuneProduct -eq $expected[3]) "Split policy is unsafe for $($policy.split)."
    }

    Assert-Condition ($Manifest.rightsRecordId -eq $Rights.recordId) 'Manifest rights record reference is invalid.'
    Assert-Condition ($Manifest.oracleId -eq $Oracle.oracleId) 'Manifest oracle reference is invalid.'
    Assert-UniqueValues -Values @($Manifest.entries.fixtureId) -Name 'manifest fixtureId'
    Assert-UniqueValues -Values @($Manifest.entries.assetId) -Name 'manifest assetId'
    Assert-UniqueValues -Values @($Manifest.entries.artifactPath | Where-Object { $null -ne $_ }) -Name 'manifest artifactPath'
    Assert-UniqueValues -Values @($Manifest.entries.externalObjectId | Where-Object { $null -ne $_ }) -Name 'manifest externalObjectId'
    Assert-UniqueValues -Values @($Manifest.entries.contentSha256) -Name 'manifest contentSha256'

    $groupOwners = @{
        originalMasterGroupId = @{}
        captureSessionGroupId = @{}
        rigStateGroupId = @{}
        derivationFamilyId = @{}
    }
    foreach ($entry in $Manifest.entries) {
        Assert-Condition $rightsById.ContainsKey([string]$entry.assetId) "Manifest asset has no cleared rights record: $($entry.assetId)."
        $rightsAsset = $rightsById[[string]$entry.assetId]
        Assert-Condition ($entry.artifactPath -eq $rightsAsset.repositoryPath -and $entry.externalObjectId -eq $rightsAsset.externalObjectId -and $entry.contentSha256 -ceq $rightsAsset.contentSha256) "Manifest asset provenance differs from the rights record for $($entry.assetId)."
        Assert-Condition ($entry.rightsRecordId -eq $Rights.recordId -and $entry.oracleId -eq $Oracle.oracleId) "Manifest entry references an unexpected rights or oracle record for $($entry.fixtureId)."
        Assert-Condition $oracleByFixture.ContainsKey([string]$entry.fixtureId) "Manifest fixture has no independent oracle outcome: $($entry.fixtureId)."
        $oracleOutcome = $oracleByFixture[[string]$entry.fixtureId]
        Assert-Condition ($entry.expectedOutcome.outcome -eq $oracleOutcome.outcome -and $entry.expectedOutcome.failureCode -eq $oracleOutcome.failureCode) "Manifest and oracle disagree for $($entry.fixtureId)."

        if ($entry.assetKind -eq 'SelfAuthoredVectorSpec') {
            $artifact = Resolve-RepositoryArtifact -RelativePath $entry.artifactPath
            $actualHash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
            Assert-Condition ($actualHash -ceq [string]$entry.contentSha256) "Manifest hash mismatch for $($entry.fixtureId)."
            $spec = Get-Content -LiteralPath $artifact -Raw | ConvertFrom-Json
            Assert-Condition ($spec.fixtureId -eq $entry.fixtureId) "Vector spec fixtureId mismatch for $($entry.fixtureId)."
            if ($entry.expectedOutcome.outcome -eq 'Reject') {
                Assert-Condition ($null -ne $spec.injectedDamage) "Reject fixture has no declared damage: $($entry.fixtureId)."
            }
            else {
                Assert-Condition ($null -eq $spec.injectedDamage) "Accept fixture contains declared damage: $($entry.fixtureId)."
            }
        }
        else {
            Assert-Condition ($entry.assetKind -eq 'ExternalD810Pair' -and $null -eq $entry.artifactPath -and $null -ne $entry.externalObjectId -and $null -ne $entry.captureCondition -and $entry.hardwareEvidence) "External manifest evidence is incomplete for $($entry.fixtureId)."
        }

        foreach ($groupName in $groupOwners.Keys) {
            $groupId = [string]$entry.groups.$groupName
            if ($groupOwners[$groupName].ContainsKey($groupId)) {
                Assert-Condition ($groupOwners[$groupName][$groupId] -eq $entry.split) "Split contamination detected for $groupName=$groupId."
            }
            else {
                $groupOwners[$groupName][$groupId] = [string]$entry.split
            }
        }
    }
    Assert-Condition ($oracleByFixture.Count -eq $Manifest.entries.Count) 'Oracle contains an outcome that is not represented in the manifest.'
    Assert-Condition (@($Manifest.entries | Where-Object { $_.split -eq 'locked-holdout' }).Count -gt 0) 'Locked holdout is empty.'
    Assert-Condition (@($Manifest.entries | Where-Object { $_.split -eq 'release' }).Count -gt 0) 'Release confirmation set is empty.'
    Assert-Condition (@($Manifest.entries | Where-Object { $_.expectedOutcome.outcome -eq 'Reject' }).Count -gt 0) 'Corpus has no failure-sensitivity fixture.'
}

try {
    $schemaRoot = Join-Path $RepositoryRoot 'docs/schemas'
    $fixtureRoot = Join-Path $RepositoryRoot 'samples/public/corpus-contracts'
    $rightsSchemaPath = Join-Path $schemaRoot 'corpus-rights-record.schema.json'
    $oracleSchemaPath = Join-Path $schemaRoot 'corpus-oracle.schema.json'
    $manifestSchemaPath = Join-Path $schemaRoot 'corpus-manifest.schema.json'
    $rights = Get-Content -LiteralPath (Join-Path $fixtureRoot 'rights-record.example.json') -Raw | ConvertFrom-Json
    $oracle = Get-Content -LiteralPath (Join-Path $fixtureRoot 'oracle.example.json') -Raw | ConvertFrom-Json
    $manifest = Get-Content -LiteralPath (Join-Path $fixtureRoot 'manifest.example.json') -Raw | ConvertFrom-Json

    foreach ($schemaPath in @($rightsSchemaPath, $oracleSchemaPath, $manifestSchemaPath)) {
        $schema = Get-Content -LiteralPath $schemaPath -Raw | ConvertFrom-Json
        Assert-Condition ($schema.'$schema' -eq 'https://json-schema.org/draft/2020-12/schema') "Schema must declare Draft 2020-12: $schemaPath."
    }
    Assert-SchemaAccepts -Value $rights -SchemaPath $rightsSchemaPath -CaseName 'rights example'
    Assert-SchemaAccepts -Value $oracle -SchemaPath $oracleSchemaPath -CaseName 'oracle example'
    Assert-SchemaAccepts -Value $manifest -SchemaPath $manifestSchemaPath -CaseName 'manifest example'
    Assert-CorpusContract -Rights $rights -Oracle $oracle -Manifest $manifest

    # Schema-only future D810 sentinel. No camera identity, image, SDK material,
    # or external store access is created or performed by this test.
    $externalRights = Copy-JsonObject $rights
    $externalRights.rightsBasis = 'documented-capture-rights'
    $externalAsset = [pscustomobject]@{
        assetId = 'asset-external-d810-pair'
        assetKind = 'ExternalD810Pair'
        repositoryPath = $null
        externalObjectId = 'external-pair-anonymous-001'
        contentSha256 = '1' * 64
        source = 'approved-external-corpus-store'
        containsCustomerContent = $false
        containsCameraIdentity = $false
        containsLicensedSdkMaterial = $false
    }
    $externalRights.assets = @($externalRights.assets) + $externalAsset
    Assert-SchemaAccepts -Value $externalRights -SchemaPath $rightsSchemaPath -CaseName 'anonymous external D810 rights sentinel'

    $externalManifest = Copy-JsonObject $manifest
    $externalEntry = $externalManifest.entries[0]
    $externalEntry.assetId = 'asset-external-d810-pair'
    $externalEntry.assetKind = 'ExternalD810Pair'
    $externalEntry.artifactPath = $null
    $externalEntry.externalObjectId = 'external-pair-anonymous-001'
    $externalEntry.contentSha256 = '1' * 64
    $externalEntry.hardwareEvidence = $true
    $externalEntry.captureCondition = [pscustomobject]@{
        rigProfileId = 'approved-rig-profile'
        rigProfileSha256 = '2' * 64
        cameraConfigurationId = 'approved-camera-configuration'
        cameraConfigurationSha256 = '3' * 64
        lightingProfileId = 'approved-lighting-profile'
        lightingProfileSha256 = '4' * 64
        capturedAtUtc = '2026-09-18T00:00:00Z'
    }
    Assert-SchemaAccepts -Value $externalManifest -SchemaPath $manifestSchemaPath -CaseName 'anonymous external D810 manifest sentinel'
    Assert-CorpusContract -Rights $externalRights -Oracle $oracle -Manifest $externalManifest
    $externalManifest.entries[0].captureCondition = $null
    Assert-SchemaRejects -Value $externalManifest -SchemaPath $manifestSchemaPath -CaseName 'external D810 entry without capture condition'
    $externalManifest.entries[0].captureCondition = [pscustomobject]@{
        rigProfileId = 'approved-rig-profile'
        rigProfileSha256 = '2' * 64
        cameraConfigurationId = 'approved-camera-configuration'
        cameraConfigurationSha256 = '3' * 64
        lightingProfileId = 'approved-lighting-profile'
        lightingProfileSha256 = '4' * 64
        capturedAtUtc = '2026-09-18T00:00:00Z'
    }
    $externalManifest.entries[0].hardwareEvidence = $false
    Assert-SchemaRejects -Value $externalManifest -SchemaPath $manifestSchemaPath -CaseName 'external D810 entry without hardware evidence'

    $wrongRights = Copy-JsonObject $rights
    $wrongRights.status = 'pending'
    Assert-SchemaRejects -Value $wrongRights -SchemaPath $rightsSchemaPath -CaseName 'uncleared rights'

    $serialLeak = Copy-JsonObject $manifest
    $serialLeak.entries[0] | Add-Member -NotePropertyName serialNumber -NotePropertyValue 'forbidden-fixture-serial'
    Assert-SchemaRejects -Value $serialLeak -SchemaPath $manifestSchemaPath -CaseName 'serial property leak'
    Assert-Rejected { Assert-CorpusContract -Rights $rights -Oracle $oracle -Manifest $serialLeak } 'runtime serial property leak'

    $hashMismatch = Copy-JsonObject $manifest
    $hashMismatch.entries[0].contentSha256 = '0' * 64
    Assert-Rejected { Assert-CorpusContract -Rights $rights -Oracle $oracle -Manifest $hashMismatch } 'artifact hash mismatch'

    $crossSplit = Copy-JsonObject $manifest
    $crossSplit.entries[-1].groups.originalMasterGroupId = $crossSplit.entries[0].groups.originalMasterGroupId
    Assert-Rejected { Assert-CorpusContract -Rights $rights -Oracle $oracle -Manifest $crossSplit } 'cross-split master contamination'

    $unsafeHoldout = Copy-JsonObject $manifest
    ($unsafeHoldout.splitPolicies | Where-Object split -eq 'locked-holdout').mayTuneProduct = $true
    Assert-Rejected { Assert-CorpusContract -Rights $rights -Oracle $oracle -Manifest $unsafeHoldout } 'holdout tuning enabled'

    $missingRights = Copy-JsonObject $manifest
    $missingRights.entries[0].assetId = 'asset-without-rights'
    Assert-Rejected { Assert-CorpusContract -Rights $rights -Oracle $oracle -Manifest $missingRights } 'missing rights asset'

    $dependentOracle = Copy-JsonObject $oracle
    $dependentOracle.independence.usesProductionImplementation = $true
    Assert-SchemaRejects -Value $dependentOracle -SchemaPath $oracleSchemaPath -CaseName 'production-dependent oracle'

    $oracleDisagreement = Copy-JsonObject $oracle
    ($oracleDisagreement.expectedOutcomes | Where-Object fixtureId -eq 'vector-development-seam-damaged').outcome = 'Accept'
    ($oracleDisagreement.expectedOutcomes | Where-Object fixtureId -eq 'vector-development-seam-damaged').failureCode = $null
    Assert-Rejected { Assert-CorpusContract -Rights $rights -Oracle $oracleDisagreement -Manifest $manifest } 'oracle disagreement'

    Write-Host 'M2 corpus contracts passed validation.'
    exit 0
}
catch {
    Write-Error "M2 corpus contracts validation failed: $($_.Exception.Message)"
    exit 1
}
