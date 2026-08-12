[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')),
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

try {
    $dotnet = Get-Command dotnet -ErrorAction Stop
    $solutionPath = Join-Path $RepositoryRoot 'A0CameraStitcher.M3.slnx'
    & $dotnet.Source build $solutionPath --configuration $Configuration --nologo --maxcpucount:1 --nodeReuse:false -p:UseSharedCompilation=false
    if ($LASTEXITCODE -ne 0) { throw ".NET product flow build failed with exit code $LASTEXITCODE." }
    $dualTestExecutable = Join-Path $RepositoryRoot "tests/m3/DualCameraFlowTests/bin/$Configuration/net10.0/A0CameraStitcher.M3.DualCameraFlowTests.exe"
    $operatorTestExecutable = Join-Path $RepositoryRoot "tests/m3/OperatorShellTests/bin/$Configuration/net10.0-windows/A0CameraStitcher.M3.OperatorShellTests.exe"
    $wpfAdapterPath = Join-Path $RepositoryRoot "src/m3/OperatorShell/bin/$Configuration/net10.0-windows/A0CameraStitcher.M2Adapter.exe"
    $operatorTestAdapterPath = Join-Path $RepositoryRoot "tests/m3/OperatorShellTests/bin/$Configuration/net10.0-windows/A0CameraStitcher.M2Adapter.exe"
    if (-not (Test-Path -LiteralPath $wpfAdapterPath -PathType Leaf)) { throw 'Formal WPF output does not contain A0CameraStitcher.M2Adapter.exe.' }
    if (-not (Test-Path -LiteralPath $operatorTestAdapterPath -PathType Leaf)) { throw 'Formal WPF test output does not contain the transitive M2 adapter artifact.' }
    $previousAdapterPath = $env:A0_M2_ADAPTER_PATH
    try {
        $env:A0_M2_ADAPTER_PATH = Join-Path $RepositoryRoot "build/wpf-m2-adapter/$Configuration/A0CameraStitcher.M2Adapter.exe"
        $dualOutput = & $dualTestExecutable 2>&1
        if ($LASTEXITCODE -ne 0 -or -not (($dualOutput -join "`n").Contains('DualCamera flow tests: 16/16 passed.'))) {
            throw "Focused DualCamera product E2E failed: $($dualOutput -join [Environment]::NewLine)"
        }
        Remove-Item Env:A0_M2_ADAPTER_PATH -ErrorAction SilentlyContinue
        $operatorOutput = & $operatorTestExecutable 2>&1
        if ($LASTEXITCODE -ne 0 -or -not (($operatorOutput -join "`n").Contains('PASS formal WPF dual-camera flow uses real JPEG product artifacts'))) {
            throw "Focused DualCamera WPF E2E failed: $($operatorOutput -join [Environment]::NewLine)"
        }
    }
    finally {
        $env:A0_M2_ADAPTER_PATH = $previousAdapterPath
    }
    Write-Host 'Focused DualCamera capture/stitch/restitch/export E2E passed.'
    exit 0
}
catch {
    Write-Error "Focused DualCamera WPF flow validation failed: $($_.Exception.Message)"
    exit 1
}
