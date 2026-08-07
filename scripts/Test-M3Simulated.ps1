[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')),
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

try {
    $solutionPath = Join-Path $RepositoryRoot 'A0CameraStitcher.M3.slnx'
    $foundationTestProject = Join-Path $RepositoryRoot 'tests/m3/FoundationTests/A0CameraStitcher.M3.FoundationTests.csproj'
    $shellProject = Join-Path $RepositoryRoot 'src/m3/OperatorShell/A0CameraStitcher.M3.OperatorShell.csproj'
    $windowPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/MainWindow.xaml'
    $viewModelPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/ViewModels/OperatorShellViewModel.cs'

    $dotnet = Get-Command dotnet -ErrorAction Stop
    & $dotnet.Source build $solutionPath --configuration $Configuration --nologo
    if ($LASTEXITCODE -ne 0) { throw "M3 simulated solution build failed with exit code $LASTEXITCODE." }

    $testOutput = & $dotnet.Source run --project $foundationTestProject --configuration $Configuration --no-build --nologo 2>&1
    if ($LASTEXITCODE -ne 0) { throw "M3 foundation tests failed: $($testOutput -join [Environment]::NewLine)" }
    Assert-Condition (($testOutput -join "`n").Contains('Foundation tests: 8/8 passed.')) 'M3 foundation test summary is missing or incomplete.'

    [xml]$shellProjectXml = Get-Content -Raw -LiteralPath $shellProject
    Assert-Condition ($shellProjectXml.Project.PropertyGroup.TargetFramework -eq 'net10.0-windows') 'Operator shell must target net10.0-windows.'
    Assert-Condition ($shellProjectXml.Project.PropertyGroup.UseWPF -eq 'true') 'Operator shell must keep UseWPF enabled.'
    $packageReferences = @($shellProjectXml.SelectNodes('//PackageReference'))
    Assert-Condition ($packageReferences.Count -eq 0) 'Operator shell must not add external NuGet packages.'

    [xml]$windowXml = Get-Content -Raw -LiteralPath $windowPath
    $windowText = Get-Content -Raw -LiteralPath $windowPath
    $viewModelText = Get-Content -Raw -LiteralPath $viewModelPath
    Assert-Condition ($windowXml.Window.Title.Contains('SIMULATED') -and $windowXml.Window.Title.Contains('実機未接続')) 'Window title must remain visibly simulated.'
    Assert-Condition ($windowText.Contains('AutomationProperties.Name="A0 Camera Stitcher SIMULATED operator shell 実機未接続"')) 'Window accessibility name must remain visibly simulated.'
    foreach ($marker in @('SIMULATED / 実機未接続', 'NO AUTO RETRY', 'Simulated Live View placeholder 非実画像', 'FailedPartial')) {
        Assert-Condition (($windowText + $viewModelText).Contains($marker)) "Operator shell is missing required marker: $marker"
    }
    Assert-Condition ($viewModelText.Contains('ISimulatedTransactionService')) 'Operator shell must use only the simulated transaction facade.'
    Assert-Condition ($viewModelText.Contains('if (!result.Simulation')) 'Operator shell must reject results without the simulation flag.'
    Assert-Condition (-not $viewModelText.Contains('DllImport')) 'Operator shell must not invoke native camera APIs.'

    Write-Host 'M3 simulated foundation and operator shell passed validation.'
    exit 0
}
catch {
    Write-Error "M3 simulated validation failed: $($_.Exception.Message)"
    exit 1
}
