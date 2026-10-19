[CmdletBinding(DefaultParameterSetName='Evidence')]
param(
    [Parameter(Mandatory,ParameterSetName='Evidence')][string]$EvidenceDirectory,
    [Parameter(Mandatory,ParameterSetName='Run')][string]$Harness,
    [Parameter(Mandatory,ParameterSetName='Run')][string]$Evaluator,
    [Parameter(Mandatory,ParameterSetName='Run')][string]$Adapter,
    [Parameter(Mandatory,ParameterSetName='Run')][string]$EvidenceParent,
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..'))
)
$ErrorActionPreference = 'Stop'
$schemaRoot = Join-Path $RepositoryRoot 'docs/schemas'
$script:checks = 0
function Assert-Schema([string]$Json, [string]$Leaf, [bool]$Expected) {
    $script:checks++
    $valid = Test-Json -Json $Json -SchemaFile (Join-Path $schemaRoot $Leaf) -ErrorAction SilentlyContinue
    if ($valid -ne $Expected) { throw "Unexpected schema verdict: $Leaf (expected $Expected)" }
}
function Compact($Node) { ConvertTo-Json -InputObject $Node -Depth 100 -Compress }
function Copy-Node($Node) { Compact $Node | ConvertFrom-Json -AsHashtable }
function Canonical($Node) {
    if ($Node -is [System.Collections.IDictionary]) {
        $result = [ordered]@{}; foreach ($key in @($Node.Keys | Sort-Object)) { $result[$key] = Canonical $Node[$key] }; return $result
    }
    if ($Node -is [System.Collections.IList]) { $items = @(); foreach ($item in $Node) { $items += ,(Canonical $item) }; return ,$items }
    return $Node
}
if ($PSCmdlet.ParameterSetName -eq 'Run') {
    $parent = Get-Item -LiteralPath $EvidenceParent
    if (-not $parent.PSIsContainer -or ($parent.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Ordinary existing evidence parent required' }
    $EvidenceDirectory = Join-Path $parent.FullName ('correction-schema-' + [Guid]::NewGuid().ToString('N'))
    [void](New-Item -ItemType Directory -Path $EvidenceDirectory)
    & $Harness (Join-Path $RepositoryRoot 'tests/fixtures/rig-profile-v2') $Evaluator $Adapter $EvidenceDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Actual JPEG correction harness failed' }
}
$jobSchema = Get-Content (Join-Path $schemaRoot 'stitch-correction-job.v1.schema.json') -Raw | ConvertFrom-Json -AsHashtable
$reportSchema = Get-Content (Join-Path $schemaRoot 'stitch-correction-report.v1.schema.json') -Raw | ConvertFrom-Json -AsHashtable
[void]$jobSchema.Remove('$schema'); [void]$jobSchema.Remove('description')
$script:checks++
if ((Compact (Canonical $jobSchema)) -ne (Compact (Canonical $reportSchema.properties.policy))) { throw 'Embedded correction policy schema drift' }
Assert-Schema (Get-Content (Join-Path $RepositoryRoot 'tests/fixtures/stitch-correction/job.json') -Raw) 'stitch-correction-job.v1.schema.json' $true
$reports = @(Get-ChildItem -LiteralPath $EvidenceDirectory -Filter 'correction-*-report.json' -File)
$manifests = @(Get-ChildItem -LiteralPath $EvidenceDirectory -Filter 'correction-*-manifest.json' -File)
if ($reports.Count -ne 7 -or $manifests.Count -ne 4) { throw 'Expected four successful and three rejected actual correction reports' }
foreach ($file in $reports) {
    $report = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json -AsHashtable
    Assert-Schema (Compact $report) 'stitch-correction-report.v1.schema.json' $true
    foreach ($mutation in @('approval','preview','quality','hash','policy','extra','candidate','point','state')) {
        $bad = Copy-Node $report
        switch ($mutation) {
            approval { $bad.productApproved = $true }
            preview { $bad.previewRequiresManifest = $false }
            quality { $bad.quality = 'passed' }
            hash { $bad.jobSha256 = '../job.json' }
            policy { $bad.policy.matching.searchRadiusSteps = 33 }
            extra { $bad.selection.extra = 1 }
            candidate { $bad.selection.candidates[0].column = 64 }
            point { $bad.selection.candidates[0].documentMm = @(1,2,3) }
            state { $bad.outcome = 'correction-estimated'; $bad.matching = $null }
        }
        Assert-Schema (Compact $bad) 'stitch-correction-report.v1.schema.json' $false
    }
}
foreach ($file in $manifests) {
    $manifest = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json -AsHashtable
    Assert-Schema (Compact $manifest) 'stitch-eval-manifest.v2.schema.json' $true
    foreach ($mutation in @('path','hash','missing','version','extra','engine','quality')) {
        $bad = Copy-Node $manifest
        switch ($mutation) {
            path { $bad.correction.report.relativePath = '../correction-report.json' }
            hash { $bad.correction.profileFileSha256 = 'a' }
            missing { [void]$bad.Remove('correction') }
            version { $bad.schemaVersion = 'a0.stitch-eval-manifest.v1' }
            extra { $bad.correction.extra = 1 }
            engine { $bad.engine.version = '1.0.0' }
            quality { $bad.quality = 'passed' }
        }
        Assert-Schema (Compact $bad) 'stitch-eval-manifest.v2.schema.json' $false
    }
}
$colorJob = Get-Content (Join-Path $schemaRoot 'stitch-color-job.v1.schema.json') -Raw | ConvertFrom-Json -AsHashtable
$colorSchema = Get-Content (Join-Path $schemaRoot 'stitch-color-report.v1.schema.json') -Raw | ConvertFrom-Json -AsHashtable
[void]$colorJob.Remove('$schema'); [void]$colorJob.Remove('description')
$script:checks++
if ((Compact (Canonical $colorJob)) -ne (Compact (Canonical $colorSchema.properties.policy))) { throw 'Embedded color policy schema drift' }
Assert-Schema (Get-Content (Join-Path $RepositoryRoot 'tests/fixtures/stitch-correction/color-job.json') -Raw) 'stitch-color-job.v1.schema.json' $true
$colorReports = @(Get-ChildItem -LiteralPath $EvidenceDirectory -Filter 'color-*-report.json' -File)
$colorManifests = @(Get-ChildItem -LiteralPath $EvidenceDirectory -Filter 'color-*-manifest.json' -File)
if ($colorReports.Count -ne 9 -or $colorManifests.Count -ne 4) { throw 'Expected four successful and five rejected actual color reports' }
$colorGeometry = @(Get-ChildItem -LiteralPath $EvidenceDirectory -Filter 'color-*-geometry.json' -File)
if ($colorGeometry.Count -ne 4) { throw 'Expected geometric prerequisites for all four successful color evaluations' }
foreach ($file in $colorGeometry) { Assert-Schema (Get-Content -LiteralPath $file.FullName -Raw) 'stitch-correction-report.v1.schema.json' $true }
foreach ($file in $colorReports) {
    $report = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json -AsHashtable
    Assert-Schema (Compact $report) 'stitch-color-report.v1.schema.json' $true
    foreach ($mutation in @('approval','preview','quality','hash','policy','extra','point','state','channel')) {
        $bad = Copy-Node $report
        switch ($mutation) {
            approval { $bad.productApproved = $true }
            preview { $bad.previewRequiresManifest = $false }
            quality { $bad.quality = 'passed' }
            hash { $bad.geometryReportSha256 = '../correction-report.json' }
            policy { $bad.policy.minimumSamples = 2 }
            extra { $bad.fit.extra = 1 }
            point { $bad.points[0].cameraBBgr = @(1,2) }
            state { $bad.outcome = 'color-fit-rejected'; $bad.fit.candidateAvailable = $true }
            channel { $bad.channelOrder = 'RGB' }
        }
        Assert-Schema (Compact $bad) 'stitch-color-report.v1.schema.json' $false
    }
}
foreach ($file in $colorManifests) {
    $manifest = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json -AsHashtable
    Assert-Schema (Compact $manifest) 'stitch-eval-manifest.v3.schema.json' $true
    foreach ($mutation in @('path','hash','missing','version','extra','engine','quality','geometry')) {
        $bad = Copy-Node $manifest
        switch ($mutation) {
            path { $bad.colorCorrection.report.relativePath = '../color-report.json' }
            hash { $bad.colorCorrection.jobSha256 = 'a' }
            missing { [void]$bad.Remove('colorCorrection') }
            version { $bad.schemaVersion = 'a0.stitch-eval-manifest.v2' }
            extra { $bad.colorCorrection.extra = 1 }
            engine { $bad.engine.version = '2.0.0' }
            quality { $bad.quality = 'passed' }
            geometry { [void]$bad.Remove('correction') }
        }
        Assert-Schema (Compact $bad) 'stitch-eval-manifest.v3.schema.json' $false
    }
}
$refinementReports = @(Get-ChildItem -LiteralPath $EvidenceDirectory -Filter 'refinement-*-report.json' -File)
$refinementManifests = @(Get-ChildItem -LiteralPath $EvidenceDirectory -Filter 'refinement-*-manifest.json' -File)
$refinementColors = @(Get-ChildItem -LiteralPath $EvidenceDirectory -Filter 'refinement-*-color.json' -File)
if ($PSCmdlet.ParameterSetName -eq 'Run' -or $refinementReports.Count -gt 0) {
    if ($refinementReports.Count -ne 14 -or $refinementManifests.Count -ne 8 -or $refinementColors.Count -ne 5) { throw 'Expected eight successful and six rejected refined evaluations with five color reports' }
    $refinementJob = Get-Content (Join-Path $schemaRoot 'stitch-correction-job.v2.schema.json') -Raw | ConvertFrom-Json -AsHashtable
    $refinementSchema = Get-Content (Join-Path $schemaRoot 'stitch-correction-report.v2.schema.json') -Raw | ConvertFrom-Json -AsHashtable
    [void]$refinementJob.Remove('$schema'); [void]$refinementJob.Remove('description')
    $script:checks++
    if ((Compact (Canonical $refinementJob)) -ne (Compact (Canonical $refinementSchema.properties.policy))) { throw 'Embedded refinement policy schema drift' }
    $example = Get-Content (Join-Path $RepositoryRoot 'tests/fixtures/stitch-correction/refinement-job.json') -Raw | ConvertFrom-Json -AsHashtable
    Assert-Schema (Compact $example) 'stitch-correction-job.v2.schema.json' $true
    Assert-Schema (Compact $example) 'stitch-correction-job.v1.schema.json' $false
    foreach ($value in @(0,9,-1,$null,'7',@(),@{})) {
        $bad = Copy-Node $example; $bad.refinement.levels = $value
        Assert-Schema (Compact $bad) 'stitch-correction-job.v2.schema.json' $false
    }
    foreach ($mutation in @('missing','unknown','version','null')) {
        $bad = Copy-Node $example
        switch ($mutation) {
            missing { [void]$bad.Remove('refinement') }
            unknown { $bad.refinement.other = 1 }
            version { $bad.schemaVersion = 'a0.stitch-correction-job.v1' }
            null { $bad.refinement = $null }
        }
        Assert-Schema (Compact $bad) 'stitch-correction-job.v2.schema.json' $false
    }
    foreach ($file in $refinementReports) {
        $report = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json -AsHashtable
        Assert-Schema (Compact $report) 'stitch-correction-report.v2.schema.json' $true
        Assert-Schema (Compact $report) 'stitch-correction-report.v1.schema.json' $false
        foreach ($mutation in @('approval','preview','version','policy','missing','extra','array')) {
            $bad = Copy-Node $report
            switch ($mutation) {
                approval { $bad.productApproved = $true }
                preview { $bad.previewRequiresManifest = $false }
                version { $bad.schemaVersion = 'a0.stitch-correction-report.v1' }
                policy { $bad.policy.refinement.levels = 9 }
                missing { [void]$bad.policy.Remove('refinement') }
                extra { $bad.matching.other = 1 }
                array { if ($bad.matching.points.Count -eq 0) { $bad.matching.points = $null } else { [void]$bad.matching.points[0].forward.Remove('refinement') } }
            }
            Assert-Schema (Compact $bad) 'stitch-correction-report.v2.schema.json' $false
        }
        if ($report.matching.points.Count -gt 0 -and $report.matching.points[0].forward.refinement.Count -gt 0) {
            foreach ($mutation in @('step','count','extra','missing','levels','state')) {
                $bad = Copy-Node $report
                switch ($mutation) {
                    step { $bad.matching.points[0].forward.refinement[0].stepMm = -1 }
                    count { $bad.matching.points[0].forward.refinement[0].examinedCandidates = 10 }
                    extra { $bad.matching.points[0].forward.refinement[0].other = 1 }
                    missing { [void]$bad.matching.points[0].forward.refinement[0].Remove('bestDocumentMm') }
                    levels { $level = $bad.matching.points[0].forward.refinement[0]; $bad.matching.points[0].forward.refinement = @($level,$level,$level,$level,$level,$level,$level,$level,$level) }
                    state { $bad.matching.points[0].forward.refinement[0].outcome = 'source-texture-insufficient' }
                }
                Assert-Schema (Compact $bad) 'stitch-correction-report.v2.schema.json' $false
            }
        }
    }
    foreach ($file in $refinementManifests) {
        $manifest = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json -AsHashtable
        Assert-Schema (Compact $manifest) 'stitch-eval-manifest.v4.schema.json' $true
        Assert-Schema (Compact $manifest) 'stitch-eval-manifest.v3.schema.json' $false
        foreach ($mutation in @('path','hash','missing','version','engine','quality','extra')) {
            $bad = Copy-Node $manifest
            switch ($mutation) {
                path { $bad.correction.report.relativePath = '../correction-report.json' }
                hash { $bad.correction.jobSha256 = 'a' }
                missing { [void]$bad.Remove('correction') }
                version { $bad.schemaVersion = 'a0.stitch-eval-manifest.v3' }
                engine { $bad.engine.version = '3.0.0' }
                quality { $bad.quality = 'passed' }
                extra { $bad.correction.refinement = 7 }
            }
            Assert-Schema (Compact $bad) 'stitch-eval-manifest.v4.schema.json' $false
        }
    }
    foreach ($file in $refinementColors) { Assert-Schema (Get-Content -LiteralPath $file.FullName -Raw) 'stitch-color-report.v1.schema.json' $true }
}
$similarityReports = @(Get-ChildItem -LiteralPath $EvidenceDirectory -Filter 'similarity-*-report.json' -File)
$similarityManifests = @(Get-ChildItem -LiteralPath $EvidenceDirectory -Filter 'similarity-*-manifest.json' -File)
$similarityColors = @(Get-ChildItem -LiteralPath $EvidenceDirectory -Filter 'similarity-*-color.json' -File)
if ($PSCmdlet.ParameterSetName -eq 'Run' -or $similarityReports.Count -gt 0) {
    if ($similarityReports.Count -ne 20 -or $similarityManifests.Count -ne 16 -or $similarityColors.Count -ne 8) { throw 'Expected sixteen successful similarity evaluations and four rejected pose budgets' }
    $byHash = @{}
    foreach ($file in $similarityReports) {
        $text = Get-Content -LiteralPath $file.FullName -Raw
        Assert-Schema $text 'stitch-correction-report.v2.schema.json' $true
        $byHash[(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()] = $text | ConvertFrom-Json -AsHashtable
    }
    foreach ($file in $similarityManifests) { Assert-Schema (Get-Content -LiteralPath $file.FullName -Raw) 'stitch-eval-manifest.v4.schema.json' $true }
    foreach ($file in $similarityColors) { Assert-Schema (Get-Content -LiteralPath $file.FullName -Raw) 'stitch-color-report.v1.schema.json' $true }
    $oracle = Get-Content -LiteralPath (Join-Path $EvidenceDirectory 'similarity-oracle.json') -Raw | ConvertFrom-Json -AsHashtable
    Assert-Schema (Compact $oracle) 'synthetic-similarity-jpeg-test.v1.schema.json' $true
    foreach ($mutation in @('approval','missing','pose','error','baseline','hash','count','extra')) {
        $bad = Copy-Node $oracle
        switch ($mutation) {
            approval { $bad.productApproved = $true }
            missing { [void]$bad.Remove('synthetic') }
            pose { $bad.cases[0].rawSimilarity = @(1,0,0,0,0) }
            error { $bad.cases[0].maximumCornerErrorRawPixels = -1 }
            baseline { $bad.cases[0].fixedOnlyMeanAbsoluteErrorDn = -1 }
            hash { $bad.cases[0].geometryReportSha256 = '../report.json' }
            count { $bad.cases = @($bad.cases[0]) }
            extra { $bad.cases[0].other = 1 }
        }
        Assert-Schema (Compact $bad) 'synthetic-similarity-jpeg-test.v1.schema.json' $false
    }
    function Test-OracleBindings($Node,$Index) {
        $keys = [Collections.Generic.HashSet[string]]::new()
        foreach ($case in $Node.cases) {
            $key = "$($case.sign)/$($case.color)/$($case.resampling)/$($case.repeat)"
            if (-not $keys.Add($key) -or -not $Index.ContainsKey($case.geometryReportSha256)) { return $false }
            $report = $Index[$case.geometryReportSha256]
            if ($report.outcome -ne 'correction-estimated' -or $report.inputs[0].sha256 -ne $case.cameraASha256 -or $report.inputs[1].sha256 -ne $case.cameraBSha256) { return $false }
        }
        return $keys.Count -eq 16
    }
    $script:checks++
    if (-not (Test-OracleBindings $oracle $byHash)) { throw 'Independent similarity oracle is not bound to the actual original/report hashes or unique cases' }
    foreach ($mutation in @('report','input','duplicate')) {
        $bad = Copy-Node $oracle
        switch ($mutation) {
            report { $bad.cases[0].geometryReportSha256 = '0' * 64 }
            input { $bad.cases[0].cameraBSha256 = 'f' * 64 }
            duplicate { $bad.cases[1] = Copy-Node $bad.cases[0] }
        }
        $script:checks++
        if (Test-OracleBindings $bad $byHash) { throw 'Tampered similarity oracle binding was accepted' }
    }
}
Write-Output "stitch_correction_schema checks=$script:checks failures=0 reports=$($reports.Count) manifests=$($manifests.Count) colorReports=$($colorReports.Count) colorManifests=$($colorManifests.Count) refinementReports=$($refinementReports.Count) refinementManifests=$($refinementManifests.Count) refinementColors=$($refinementColors.Count) similarityReports=$($similarityReports.Count) similarityManifests=$($similarityManifests.Count) similarityColors=$($similarityColors.Count)"
