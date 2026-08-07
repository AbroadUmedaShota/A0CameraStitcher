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
    $foundationTestExecutable = Join-Path $RepositoryRoot "tests/m3/FoundationTests/bin/$Configuration/net10.0/A0CameraStitcher.M3.FoundationTests.exe"
    $shellProject = Join-Path $RepositoryRoot 'src/m3/OperatorShell/A0CameraStitcher.M3.OperatorShell.csproj'
    $windowPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/MainWindow.xaml'
    $viewModelPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/ViewModels/OperatorShellViewModel.cs'

    $dotnet = Get-Command dotnet -ErrorAction Stop
    & $dotnet.Source build $solutionPath --configuration $Configuration --nologo --maxcpucount:1 --nodeReuse:false -p:UseSharedCompilation=false
    if ($LASTEXITCODE -ne 0) { throw "M3 simulated solution build failed with exit code $LASTEXITCODE." }

    Assert-Condition (Test-Path -LiteralPath $foundationTestExecutable -PathType Leaf) 'M3 foundation test executable was not produced by the solution build.'
    $testOutput = & $foundationTestExecutable 2>&1
    if ($LASTEXITCODE -ne 0) { throw "M3 foundation tests failed: $($testOutput -join [Environment]::NewLine)" }
    Assert-Condition (($testOutput -join "`n").Contains('Foundation tests: 11/11 passed.')) 'M3 foundation test summary is missing or incomplete.'

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
    foreach ($marker in @('SIMULATED / 実機未接続', 'NO AUTO RETRY', 'Simulated Live View placeholder 非実画像', 'FailedPartial', '新しい撮影を準備', '確認なし', 'read-only')) {
        Assert-Condition (($windowText + $viewModelText).Contains($marker)) "Operator shell is missing required marker: $marker"
    }
    Assert-Condition ($viewModelText.Contains('ISimulatedTransactionService')) 'Operator shell must use only the simulated transaction facade.'
    Assert-Condition ($viewModelText.Contains('if (!result.Simulation')) 'Operator shell must reject results without the simulation flag.'
    Assert-Condition ($viewModelText.Contains('OperatorActionAvailability')) 'Operator actions must be controlled by one availability contract.'
    Assert-Condition ($viewModelText.Contains('TransactionStartCount++')) 'Capture start count must be observable for duplicate-start validation.'
    Assert-Condition ($viewModelText.Contains('SimulatedWorkflowScenario')) 'Diagnostic capture failures must remain routed through the simulated facade.'
    Assert-Condition ($windowText.Contains('AutomationProperties.LiveSetting="Assertive"')) 'Blocking and result announcements must expose an assertive accessibility live region.'
    Assert-Condition (-not $viewModelText.Contains('DllImport')) 'Operator shell must not invoke native camera APIs.'

    Write-Host 'M3 simulated foundation and operator shell passed validation.'
    exit 0
}
catch {
    Write-Error "M3 simulated validation failed: $($_.Exception.Message)"
    exit 1
}
