[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')),
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$ContractOnly
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}


function Get-RequiredPackagingInputs {
    @(
        '$(MSBuildProjectFullPath)',
        '$(RepositoryRoot)\CMakeLists.txt',
        '$(RepositoryRoot)\src\m2\**\*.cpp',
        '$(RepositoryRoot)\src\m2\**\*.hpp',
        '$(RepositoryRoot)\src\phase0\**\*.cpp',
        '$(RepositoryRoot)\src\phase0\**\*.hpp',
        '$(RepositoryRoot)\src\common\**\*.hpp')
}

function Assert-PackagingInputContract {
    param([System.Xml.XmlDocument]$Project)
    # Repository structure assertion, not a general MSBuild condition/import evaluator.
    $inputs = @($Project.SelectNodes('/Project/ItemGroup/M2AdapterInput') |
        ForEach-Object { $_.GetAttribute('Include') })
    foreach ($requiredInput in Get-RequiredPackagingInputs) {
        Assert-Condition ($inputs -ccontains $requiredInput) "BuildM2Adapter is missing actual XML input: $requiredInput"
    }
    $targets = @($Project.SelectNodes('/Project/Target[@Name="BuildM2Adapter"]'))
    Assert-Condition ($targets.Count -eq 1) 'Exactly one BuildM2Adapter target must consume the incremental inputs.'
    $tokens = @($targets[0].GetAttribute('Inputs').Split(';') | ForEach-Object { $_.Trim() })
    Assert-Condition ($tokens -ccontains '@(M2AdapterInput)') 'BuildM2Adapter must consume the exact @(M2AdapterInput) token.'
}

function Resolve-PackagingTestRoot {
    param([string]$TempRoot, [string]$TestRoot, [bool]$HasReparsePoint = $false)
    Assert-Condition (-not $HasReparsePoint) 'Packaging temp cleanup refuses reparse points.'
    Assert-Condition (-not ($TestRoot.Split([char[]]@('\', '/')) -contains '..')) 'Packaging temp path contains parent traversal.'
    $normalizedTemp = [System.IO.Path]::GetFullPath($TempRoot)
    $normalizedTest = [System.IO.Path]::GetFullPath($TestRoot)
    $relative = [System.IO.Path]::GetRelativePath($normalizedTemp, $normalizedTest)
    Assert-Condition (-not [System.IO.Path]::IsPathRooted($relative) -and
        $relative -cmatch '^a0-camera-agent-packaging-[0-9a-fA-F]{32}$') 'Packaging temp root must be a single generated GUID leaf below OS temp.'
    return $normalizedTest
}

function Get-ValidatedPackagingTestRoot {
    param([string]$TempRoot, [string]$TestRoot, [switch]$BeforeDelete)
    $validated = Resolve-PackagingTestRoot $TempRoot $TestRoot
    # Check existing ancestors before creation, and every child without following
    # links before deletion. No rejected target is passed to Remove-Item.
    $ancestor = $validated
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force
            Assert-Condition (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) 'Packaging temp path has a reparse ancestor.'
        }
        $ancestor = [System.IO.Path]::GetDirectoryName($ancestor)
    }
    if ($BeforeDelete -and (Test-Path -LiteralPath $validated)) {
        $pending = [System.Collections.Generic.Stack[string]]::new()
        $pending.Push($validated)
        while ($pending.Count -gt 0) {
            foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
                Assert-Condition (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) 'Packaging temp tree contains a reparse point.'
                if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            }
        }
    }
    return $validated
}

function Test-PackagingContracts {
    param(
        [System.Xml.XmlDocument]$Project,
        [scriptblock]$Validator = { param($Document) Assert-PackagingInputContract $Document }
    )
    $cases = [System.Collections.Generic.List[object]]::new()
    $cases.Add(@{ Name = 'current'; Document = $Project; Expected = $true })
    foreach ($required in Get-RequiredPackagingInputs) {
        foreach ($kind in @('missing', 'comment-only')) {
            $document = [System.Xml.XmlDocument]$Project.CloneNode($true)
            foreach ($node in @($document.SelectNodes('/Project/ItemGroup/M2AdapterInput')) |
                Where-Object { $_.GetAttribute('Include') -ceq $required }) {
                if ($kind -eq 'comment-only') {
                    [void]$node.ParentNode.ReplaceChild($document.CreateComment($node.OuterXml), $node)
                }
                else { [void]$node.ParentNode.RemoveChild($node) }
            }
            $cases.Add(@{ Name = "$kind $required"; Document = $document; Expected = $false })
        }
    }
    foreach ($kind in @('disconnected', 'renamed-target', 'duplicate-hides-missing', 'duplicate-target', 'substring-token', 'extra-input', 'extra-token')) {
        $document = [System.Xml.XmlDocument]$Project.CloneNode($true)
        $target = $document.SelectSingleNode('/Project/Target[@Name="BuildM2Adapter"]')
        switch ($kind) {
            'disconnected' { $target.SetAttribute('Inputs', 'unrelated.input') }
            'renamed-target' { $target.SetAttribute('Name', 'OtherTarget') }
            'duplicate-hides-missing' {
                $nodes = $document.SelectNodes('/Project/ItemGroup/M2AdapterInput')
                $missing = $nodes | Where-Object { $_.GetAttribute('Include') -ceq '$(RepositoryRoot)\CMakeLists.txt' }
                $duplicate = $nodes | Where-Object { $_.GetAttribute('Include') -ceq '$(MSBuildProjectFullPath)' } | Select-Object -First 1
                foreach ($node in @($missing)) { [void]$node.ParentNode.ReplaceChild($duplicate.CloneNode($true), $node) }
            }
            'duplicate-target' { [void]$target.ParentNode.AppendChild($target.CloneNode($true)) }
            'substring-token' { $target.SetAttribute('Inputs', 'prefix@(M2AdapterInput)suffix') }
            'extra-input' {
                $node = $document.CreateElement('M2AdapterInput')
                $node.SetAttribute('Include', 'future.input')
                $inputs = $document.SelectNodes('/Project/ItemGroup/M2AdapterInput')
                [void]$inputs[1].ParentNode.InsertBefore($node, $inputs[1])
            }
            'extra-token' { $target.SetAttribute('Inputs', ' other.input ; @(M2AdapterInput) ; future.input ') }
        }
        $cases.Add(@{ Name = $kind; Document = $document; Expected = $kind.StartsWith('extra-') })
    }
    $failures = 0
    foreach ($case in $cases) {
        $accepted = $true
        try { & $Validator $case.Document }
        catch { $accepted = $false }
        if ($accepted -ne $case.Expected) {
            $failures++
            Write-Host "FAIL XML $($case.Name): expected=$($case.Expected) accepted=$accepted"
        }
        else { Write-Host "PASS XML $($case.Name): accepted=$accepted" }
    }

    # Pure path fixtures: no directory creation, enumeration, timestamps or delete.
    $temp = [System.IO.Path]::GetFullPath('C:\a0-contract-fixture\temp')
    $leaf = 'a0-camera-agent-packaging-0123456789abcdef0123456789abcdef'
    $valid = [System.IO.Path]::Combine($temp, $leaf)
    $pathCases = @(
        @{ Name = 'valid'; Path = $valid; Expected = $true; Reparse = $false },
        @{ Name = 'outside'; Path = "C:\outside\$leaf"; Expected = $false; Reparse = $false },
        @{ Name = 'temp-itself'; Path = $temp; Expected = $false; Reparse = $false },
        @{ Name = 'parent-traversal'; Path = "$temp\..\temp\$leaf"; Expected = $false; Reparse = $false },
        @{ Name = 'prefix-sibling'; Path = "$temp-sibling\$leaf"; Expected = $false; Reparse = $false },
        @{ Name = 'bad-leaf'; Path = "$temp\a0-camera-agent-packaging-not-a-guid"; Expected = $false; Reparse = $false },
        @{ Name = 'nested'; Path = "$temp\nested\$leaf"; Expected = $false; Reparse = $false },
        @{ Name = 'reparse'; Path = $valid; Expected = $false; Reparse = $true })
    foreach ($case in $pathCases) {
        $accepted = $true
        try { [void](Resolve-PackagingTestRoot $temp $case.Path $case.Reparse) }
        catch { $accepted = $false }
        if ($accepted -ne $case.Expected) {
            $failures++
            Write-Host "FAIL PATH $($case.Name): expected=$($case.Expected) accepted=$accepted; deletes=0"
        }
        else { Write-Host "PASS PATH $($case.Name): accepted=$accepted; deletes=0" }
    }
    Assert-Condition ($failures -eq 0) "Packaging contracts failed: $failures of $($cases.Count + $pathCases.Count)."
    Write-Host "Packaging contracts passed: XML=$($cases.Count), PATH=$($pathCases.Count); builds=0, deletes=0."
}

function Remove-ExecutionMarker {
    param([string]$MarkerPath)
    Remove-Item -LiteralPath $MarkerPath -Force -ErrorAction SilentlyContinue
    Assert-Condition (-not (Test-Path -LiteralPath $MarkerPath)) "Could not remove the prior BuildM2Adapter execution marker: $MarkerPath"
}

function Invoke-OperatorShellBuild {
    param(
        [string]$Dotnet,
        [string]$ProjectPath,
        [string]$NativeBuildDirectory,
        [string]$ManagedBuildDirectory,
        [string]$MarkerPath,
        [string]$BuildConfiguration
    )

    $script:buildInvocationCount++
    Write-Host "Packaging build $script:buildInvocationCount started."
    $output = & $Dotnet build $ProjectPath --configuration $BuildConfiguration --nologo --maxcpucount:1 --nodeReuse:false `
        -p:UseSharedCompilation=false "-property:M2AdapterBuildDirectory=$NativeBuildDirectory" `
        "-property:BaseOutputPath=$(Join-Path $ManagedBuildDirectory 'bin')\" `
        "-property:BuildM2AdapterExecutionMarker=$MarkerPath" 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "OperatorShell build failed: $($output -join [Environment]::NewLine)"
    }
    Write-Host "Packaging build $script:buildInvocationCount passed."
}

function Assert-PackagedArtifactsMatch {
    param([string[]]$CanonicalArtifacts, [string[]]$PackagedArtifacts)
    for ($index = 0; $index -lt $CanonicalArtifacts.Count; $index++) {
        Assert-Condition (Test-Path -LiteralPath $CanonicalArtifacts[$index] -PathType Leaf) "Canonical artifact is missing: $($CanonicalArtifacts[$index])"
        Assert-Condition (Test-Path -LiteralPath $PackagedArtifacts[$index] -PathType Leaf) "OperatorShell package is missing: $($PackagedArtifacts[$index])"
        $canonicalHash = (Get-FileHash -LiteralPath $CanonicalArtifacts[$index] -Algorithm SHA256).Hash
        $packagedHash = (Get-FileHash -LiteralPath $PackagedArtifacts[$index] -Algorithm SHA256).Hash
        Assert-Condition ($canonicalHash -eq $packagedHash) "Packaged artifact differs from canonical build output: $($CanonicalArtifacts[$index])"
    }
}

try {
    $projectPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/A0CameraStitcher.M3.OperatorShell.csproj'
    $projectText = Get-Content -Raw -LiteralPath $projectPath
    $project = [System.Xml.XmlDocument]::new()
    $project.LoadXml($projectText)
    Assert-PackagingInputContract $project
    Test-PackagingContracts $project
    if ($ContractOnly) { exit 0 }

    $dotnet = Get-Command dotnet -ErrorAction Stop
    $script:buildInvocationCount = 0
    $phase0Input = Join-Path $RepositoryRoot 'src/phase0/include/a0/phase0/phase0.hpp'
    $commonInput = Join-Path $RepositoryRoot 'src/common/include/a0/common/protocol_json.hpp'
    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $testRoot = Get-ValidatedPackagingTestRoot $tempRoot (Join-Path $tempRoot "a0-camera-agent-packaging-$([Guid]::NewGuid().ToString('N'))")
    Assert-Condition (-not (Test-Path -LiteralPath $testRoot)) 'Packaging temp root must not already exist.'
    [void](New-Item -ItemType Directory -Path $testRoot -ErrorAction Stop)
    $ownershipToken = [Guid]::NewGuid().ToString('N')
    $ownershipPath = Join-Path $testRoot '.packaging-owner'
    $ownershipStream = [System.IO.File]::Open($ownershipPath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try {
        $ownershipBytes = [System.Text.Encoding]::UTF8.GetBytes($ownershipToken)
        $ownershipStream.Write($ownershipBytes, 0, $ownershipBytes.Length)
    }
    finally { $ownershipStream.Dispose() }
    Write-Host "Packaging temporary root: $testRoot"
    $nativeBuildDirectory = Join-Path $testRoot 'native'
    $managedBuildDirectory = Join-Path $testRoot 'managed'
    $markerPath = Join-Path $testRoot 'BuildM2Adapter.executed'
    foreach ($requiredOutput in @(
        '$(M2AdapterSourcePath)',
        '$(SingleCameraAgentSourcePath)',
        '$(DualCameraAgentSourcePath)')) {
        Assert-Condition ($projectText.Contains($requiredOutput)) "BuildM2Adapter is missing required target output: $requiredOutput"
    }
    Assert-Condition ($projectText.Contains('BuildM2AdapterExecutionMarker')) 'BuildM2Adapter must expose the test-only execution marker.'

    $artifactNames = @(
        'A0CameraStitcher.M2Adapter.exe',
        'A0CameraStitcher.CameraAgent.exe',
        'A0CameraStitcher.DualCameraAgent.exe')
    $canonicalArtifacts = @($artifactNames | ForEach-Object { Join-Path $nativeBuildDirectory "$Configuration/$_" })
    $packagedArtifacts = @($artifactNames | ForEach-Object { Join-Path $managedBuildDirectory "bin/$Configuration/net10.0-windows/$_" })
    $originalPhase0WriteTime = (Get-Item -LiteralPath $phase0Input).LastWriteTimeUtc
    $originalCommonWriteTime = (Get-Item -LiteralPath $commonInput).LastWriteTimeUtc
    $regressionPassed = $false

    try {
        Invoke-OperatorShellBuild $dotnet.Source $projectPath $nativeBuildDirectory $managedBuildDirectory $markerPath $Configuration
        Assert-Condition (Test-Path -LiteralPath $markerPath -PathType Leaf) 'Initial OperatorShell build did not execute BuildM2Adapter.'
        Assert-PackagedArtifactsMatch $canonicalArtifacts $packagedArtifacts

        Remove-ExecutionMarker $markerPath
        Invoke-OperatorShellBuild $dotnet.Source $projectPath $nativeBuildDirectory $managedBuildDirectory $markerPath $Configuration
        Assert-Condition (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) 'Unchanged inputs unexpectedly executed BuildM2Adapter.'
        Assert-PackagedArtifactsMatch $canonicalArtifacts $packagedArtifacts

        foreach ($artifactPath in $canonicalArtifacts) {
            Remove-ExecutionMarker $markerPath
            Remove-Item -LiteralPath $artifactPath -Force
            Invoke-OperatorShellBuild $dotnet.Source $projectPath $nativeBuildDirectory $managedBuildDirectory $markerPath $Configuration
            Assert-Condition (Test-Path -LiteralPath $markerPath -PathType Leaf) "Missing native output did not execute BuildM2Adapter: $artifactPath"
            Assert-PackagedArtifactsMatch $canonicalArtifacts $packagedArtifacts
        }

        foreach ($inputPath in @($phase0Input, $commonInput)) {
            Remove-ExecutionMarker $markerPath
            $originalWriteTime = (Get-Item -LiteralPath $inputPath).LastWriteTimeUtc
            try {
                (Get-Item -LiteralPath $inputPath).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(1)
                Invoke-OperatorShellBuild $dotnet.Source $projectPath $nativeBuildDirectory $managedBuildDirectory $markerPath $Configuration
                Assert-Condition (Test-Path -LiteralPath $markerPath -PathType Leaf) "Changed input did not execute BuildM2Adapter: $inputPath"
                Assert-PackagedArtifactsMatch $canonicalArtifacts $packagedArtifacts
            }
            finally {
                (Get-Item -LiteralPath $inputPath).LastWriteTimeUtc = $originalWriteTime
            }
        }
        Assert-Condition ($script:buildInvocationCount -eq 7) 'Packaging regression must retain its seven-build sequence.'
        $regressionPassed = $true
    }
    finally {
        (Get-Item -LiteralPath $phase0Input).LastWriteTimeUtc = $originalPhase0WriteTime
        (Get-Item -LiteralPath $commonInput).LastWriteTimeUtc = $originalCommonWriteTime
        if ($regressionPassed -and (Test-Path -LiteralPath $testRoot)) {
            $cleanupRoot = Get-ValidatedPackagingTestRoot $tempRoot $testRoot -BeforeDelete
            Assert-Condition ((Get-Item -LiteralPath $ownershipPath).Length -eq 32 -and
                [System.IO.File]::ReadAllText($ownershipPath) -ceq $ownershipToken) 'Packaging temp ownership proof does not match; retaining root.'
            Remove-Item -LiteralPath $cleanupRoot -Recurse -Force
            Write-Host "Packaging temporary cleanup completed: $cleanupRoot"
        }
        elseif (Test-Path -LiteralPath $testRoot) {
            Write-Host "Packaging failure evidence retained: $testRoot"
        }
    }

    Write-Host 'Camera Agent incremental packaging regression passed (SDK-less; no camera access).'
    exit 0
}
catch {
    Write-Error "Camera Agent incremental packaging regression failed: $($_.Exception.Message)"
    exit 1
}
