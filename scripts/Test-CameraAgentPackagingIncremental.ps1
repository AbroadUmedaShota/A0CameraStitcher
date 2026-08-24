[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')),
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
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

    $output = & $Dotnet build $ProjectPath --configuration $BuildConfiguration --nologo --maxcpucount:1 --nodeReuse:false `
        -p:UseSharedCompilation=false "-property:M2AdapterBuildDirectory=$NativeBuildDirectory" `
        "-property:BaseOutputPath=$(Join-Path $ManagedBuildDirectory 'bin')\" `
        "-property:BuildM2AdapterExecutionMarker=$MarkerPath" 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "OperatorShell build failed: $($output -join [Environment]::NewLine)"
    }
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
    $dotnet = Get-Command dotnet -ErrorAction Stop
    $projectPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/A0CameraStitcher.M3.OperatorShell.csproj'
    $phase0Input = Join-Path $RepositoryRoot 'src/phase0/include/a0/phase0/phase0.hpp'
    $commonInput = Join-Path $RepositoryRoot 'src/common/include/a0/common/protocol_json.hpp'
    $testRoot = Join-Path ([System.IO.Path]::GetTempPath()) "a0-camera-agent-packaging-$([Guid]::NewGuid().ToString('N'))"
    $nativeBuildDirectory = Join-Path $testRoot 'native'
    $managedBuildDirectory = Join-Path $testRoot 'managed'
    $markerPath = Join-Path $testRoot 'BuildM2Adapter.executed'
    $projectText = Get-Content -Raw -LiteralPath $projectPath

    foreach ($requiredInput in @(
        '$(MSBuildProjectFullPath)',
        'src\phase0\**\*.cpp',
        'src\phase0\**\*.hpp',
        'src\common\**\*.hpp')) {
        Assert-Condition ($projectText.Contains($requiredInput)) "BuildM2Adapter is missing required incremental input: $requiredInput"
    }
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
    }
    finally {
        (Get-Item -LiteralPath $phase0Input).LastWriteTimeUtc = $originalPhase0WriteTime
        (Get-Item -LiteralPath $commonInput).LastWriteTimeUtc = $originalCommonWriteTime
        if (Test-Path -LiteralPath $testRoot) {
            Remove-Item -LiteralPath $testRoot -Recurse -Force
        }
    }

    Write-Host 'Camera Agent incremental packaging regression passed (SDK-less; no camera access).'
    exit 0
}
catch {
    Write-Error "Camera Agent incremental packaging regression failed: $($_.Exception.Message)"
    exit 1
}
