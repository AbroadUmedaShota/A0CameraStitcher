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
    $operatorShellTestExecutable = Join-Path $RepositoryRoot "tests/m3/OperatorShellTests/bin/$Configuration/net10.0-windows/A0CameraStitcher.M3.OperatorShellTests.exe"
    $dualCameraFlowTestExecutable = Join-Path $RepositoryRoot "tests/m3/DualCameraFlowTests/bin/$Configuration/net10.0/A0CameraStitcher.M3.DualCameraFlowTests.exe"
    $shellProject = Join-Path $RepositoryRoot 'src/m3/OperatorShell/A0CameraStitcher.M3.OperatorShell.csproj'
    $windowPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/MainWindow.xaml'
    $viewModelPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/ViewModels/OperatorShellViewModel.cs'
    $hardwareWindowPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/HardwareSingleCameraWindow.xaml'
    $hardwareViewModelPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/ViewModels/HardwareSingleCameraViewModel.cs'
    $dualCompositionPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/DualCameraProductComposition.cs'
    $nativeBuildDirectory = Join-Path $RepositoryRoot 'build/m3-simulated-native'

    $dotnet = Get-Command dotnet -ErrorAction Stop
    & $dotnet.Source build $solutionPath --configuration $Configuration --nologo --maxcpucount:1 --nodeReuse:false -p:UseSharedCompilation=false
    if ($LASTEXITCODE -ne 0) { throw "M3 simulated solution build failed with exit code $LASTEXITCODE." }

    Assert-Condition (Test-Path -LiteralPath $foundationTestExecutable -PathType Leaf) 'M3 foundation test executable was not produced by the solution build.'
    $testOutput = & $foundationTestExecutable 2>&1
    if ($LASTEXITCODE -ne 0) { throw "M3 foundation tests failed: $($testOutput -join [Environment]::NewLine)" }
    Assert-Condition (($testOutput -join "`n").Contains('Foundation tests: 22/22 passed.')) 'M3 foundation test summary is missing or incomplete.'

    $cmake = Get-Command cmake -ErrorAction Stop
    & $cmake.Source -S $RepositoryRoot -B $nativeBuildDirectory -A x64
    if ($LASTEXITCODE -ne 0) { throw "M2 adapter configure failed with exit code $LASTEXITCODE." }
    & $cmake.Source --build $nativeBuildDirectory --config $Configuration --target A0CameraStitcher.M2Adapter -- /m:1
    if ($LASTEXITCODE -ne 0) { throw "M2 adapter build failed with exit code $LASTEXITCODE." }
    $m2AdapterPath = Join-Path $nativeBuildDirectory "$Configuration/A0CameraStitcher.M2Adapter.exe"
    Assert-Condition (Test-Path -LiteralPath $m2AdapterPath -PathType Leaf) 'M2 adapter executable was not produced.'

    $previousAdapterPath = $env:A0_M2_ADAPTER_PATH
    try {
        $env:A0_M2_ADAPTER_PATH = $m2AdapterPath
        Assert-Condition (Test-Path -LiteralPath $dualCameraFlowTestExecutable -PathType Leaf) 'DualCamera flow test executable was not produced by the solution build.'
        $dualCameraFlowOutput = & $dualCameraFlowTestExecutable 2>&1
        if ($LASTEXITCODE -ne 0) { throw "DualCamera flow tests failed: $($dualCameraFlowOutput -join [Environment]::NewLine)" }
        Assert-Condition (($dualCameraFlowOutput -join "`n").Contains('DualCamera flow tests: 18/18 passed.')) 'DualCamera flow test summary is missing or incomplete.'

        Assert-Condition (Test-Path -LiteralPath $operatorShellTestExecutable -PathType Leaf) 'M3 operator shell test executable was not produced by the solution build.'
        $operatorShellTestOutput = & $operatorShellTestExecutable 2>&1
        if ($LASTEXITCODE -ne 0) { throw "M3 operator shell tests failed: $($operatorShellTestOutput -join [Environment]::NewLine)" }
        Assert-Condition (($operatorShellTestOutput -join "`n").Contains('Operator shell tests: 34/34 passed.')) 'M3 operator shell test summary is missing or incomplete.'
    }
    finally {
        $env:A0_M2_ADAPTER_PATH = $previousAdapterPath
    }

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
    foreach ($marker in @('SIMULATED / 実機未接続', 'NO AUTO RETRY', 'Simulated Live View placeholder 非実画像', 'FailedPartial', '新しい撮影を準備', '確認なし', 'read-only', '1台構成', '2台構成', 'SelectedOperatingMode', 'CaptureButtonText')) {
        Assert-Condition (($windowText + $viewModelText).Contains($marker)) "Operator shell is missing required marker: $marker"
    }
    Assert-Condition ($viewModelText.Contains('ISimulatedTransactionService')) 'Operator shell must retain the SingleCamera and diagnostic simulated transaction facade.'
    Assert-Condition ($viewModelText.Contains('if (!result.Simulation')) 'Operator shell must reject results without the simulation flag.'
    Assert-Condition ($viewModelText.Contains('OperatorActionAvailability')) 'Operator actions must be controlled by one availability contract.'
    Assert-Condition ($viewModelText.Contains('TransactionStartCount++')) 'Capture start count must be observable for duplicate-start validation.'
    Assert-Condition ($viewModelText.Contains('SimulatedWorkflowScenario')) 'Diagnostic capture failures must remain routed through the simulated facade.'
    Assert-Condition ($viewModelText.Contains('SimulatedWorkflowScenario.FailLiveViewStop')) 'Live View stop failure must remain routed through the durable simulated transaction facade.'
    Assert-Condition ($windowText.Contains('AutomationProperties.LiveSetting="Assertive"')) 'Blocking and result announcements must expose an assertive accessibility live region.'
    Assert-Condition (-not $viewModelText.Contains('DllImport')) 'Operator shell must not invoke native camera APIs.'

    foreach ($marker in @('ISimulatedLiveViewFramePump', 'SimulatedFramePatternOptions', 'StageCompositeLiveImage', 'StageCompositeStillImage', 'StageSingleLiveImage', 'IsSimulatedFrameSourceAvailable')) {
        Assert-Condition ($viewModelText.Contains($marker)) "Operator shell is missing required SIMULATED frame-source marker: $marker"
    }
    Assert-Condition ($viewModelText.Contains('!frame.Simulation') -and $viewModelText.Contains('!IsLiveViewActive')) 'Operator shell must guard applied SIMULATED live view frames the same way it guards capture results.'
    foreach ($marker in @('疑似LVフレームソース パターン切替', 'StageCompositeLiveImage', 'StageCompositeStillImage', 'StageSingleLiveImage')) {
        Assert-Condition ($windowText.Contains($marker)) "Operator shell window is missing required SIMULATED frame-source binding/marker: $marker"
    }

    [xml]$hardwareWindowXml = Get-Content -Raw -LiteralPath $hardwareWindowPath
    $hardwareWindowText = Get-Content -Raw -LiteralPath $hardwareWindowPath
    $hardwareViewModelText = Get-Content -Raw -LiteralPath $hardwareViewModelPath
    $dualCompositionText = Get-Content -Raw -LiteralPath $dualCompositionPath
    Assert-Condition ($hardwareWindowXml.Window.Title.Contains('HARDWARE') -and $hardwareWindowXml.Window.Title.Contains('一台構成')) 'Hardware window title must identify the real SingleCamera boundary.'
    foreach ($marker in @('SingleCamera', '接続台数から推定・自動降格しません', '継続Live View', '未確定transactionの結果を確認', 'byte-identical', '再合成（SingleCameraでは対象外）')) {
        Assert-Condition (($hardwareWindowText + $hardwareViewModelText).Contains($marker)) "Hardware SingleCamera shell is missing required marker: $marker"
    }
    Assert-Condition ($hardwareViewModelText.Contains('IHardwareSingleCameraOperations')) 'Hardware SingleCamera shell must use the typed Camera Agent facade.'
    Assert-Condition ($hardwareViewModelText.Contains('SavePendingAsync')) 'Hardware capture must durably reserve its client transaction ID before dispatch.'
    Assert-Condition ($hardwareViewModelText.Contains('GetTransactionResultAsync')) 'Hardware recovery must query the existing transaction without recapture.'
    Assert-Condition (-not $hardwareViewModelText.Contains('DllImport')) 'Hardware shell must not invoke native camera APIs in-process.'

    foreach ($marker in @('CAM-A原本検証', 'CAM-B原本検証', 'fixed-local folder', '実JPEG合成完了', 'DualCameraExecutionEnvironment.TestSynthetic')) {
        Assert-Condition (($windowText + $viewModelText + $dualCompositionText).Contains($marker)) "Formal DualCamera WPF flow is missing required marker: $marker"
    }
    Assert-Condition ($dualCompositionText.Contains('DualCameraProductFlow')) 'WPF composition must use the typed DualCamera application flow.'
    Assert-Condition ($dualCompositionText.Contains('M2OfflineStitcherProcessAdapter')) 'WPF composition must connect the M2 offline stitcher adapter.'
    Assert-Condition ($dualCompositionText.Contains('does not fall back to SingleCamera')) 'Missing M2 adapter must fail closed without SingleCamera fallback.'
    foreach ($marker in @('DualCameraExecutionEnvironment.HardwareDual', 'HardwareDualCaptureSource', 'DualCameraIdentitySnapshot.HardwarePending')) {
        Assert-Condition ($dualCompositionText.Contains($marker)) "HardwareDual production composition is missing fail-closed marker: $marker"
    }

    Write-Host 'M3 simulated foundation, formal DualCamera JPEG product flow, and SingleCamera regression passed validation.'
    exit 0
}
catch {
    Write-Error "M3 simulated validation failed: $($_.Exception.Message)"
    exit 1
}
