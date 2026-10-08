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
    # OperatorShell.csproj の BuildM2Adapter と Test-DualCameraWpfFlow.ps1 が使うのと同じ
    # ディレクトリを共有する。専用ディレクトリを持つと、同じ入力から同じアダプタを
    # もう一度フルコンパイルすることになる（GitHub Issue #28）。下の cmake 呼び出しは
    # 残してあり、csproj 側が増分判定で飛ばされた場合でもアダプタの存在を保証する。
    $nativeBuildDirectory = Join-Path $RepositoryRoot 'build/wpf-m2-adapter'

    $dotnet = Get-Command dotnet -ErrorAction Stop
    & $dotnet.Source build $solutionPath --configuration $Configuration --nologo --maxcpucount:1 --nodeReuse:false -p:UseSharedCompilation=false
    if ($LASTEXITCODE -ne 0) { throw "M3 simulated solution build failed with exit code $LASTEXITCODE." }

    Assert-Condition (Test-Path -LiteralPath $foundationTestExecutable -PathType Leaf) 'M3 foundation test executable was not produced by the solution build.'
    $testOutput = & $foundationTestExecutable 2>&1
    if ($LASTEXITCODE -ne 0) { throw "M3 foundation tests failed: $($testOutput -join [Environment]::NewLine)" }
    $foundationPassLines = @($testOutput | Where-Object { $_ -match '^PASS ' })
    Assert-Condition ($foundationPassLines.Count -gt 0) 'M3 foundation tests did not emit any PASS result.'
    $foundationSummary = @($testOutput | Where-Object { $_ -match '^Foundation tests: (\d+)/(\d+) passed\.$' }) | Select-Object -Last 1
    Assert-Condition ($null -ne $foundationSummary) 'M3 foundation test summary is missing.'
    if ($foundationSummary -notmatch '^Foundation tests: (\d+)/(\d+) passed\.$') { throw 'M3 foundation test summary is malformed.' }
    Assert-Condition ([int]$Matches[1] -eq $foundationPassLines.Count -and [int]$Matches[2] -eq $foundationPassLines.Count) 'M3 foundation test summary does not match emitted PASS lines.'

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
        $dualCameraPassLines = @($dualCameraFlowOutput | Where-Object { $_ -match '^PASS ' })
        Assert-Condition ($dualCameraPassLines.Count -gt 0) 'DualCamera flow tests did not emit any PASS result.'
        $dualCameraSummary = @($dualCameraFlowOutput | Where-Object { $_ -match '^DualCamera flow tests: (\d+)/(\d+) passed\.$' }) | Select-Object -Last 1
        Assert-Condition ($null -ne $dualCameraSummary) 'DualCamera flow test summary is missing.'
        if ($dualCameraSummary -notmatch '^DualCamera flow tests: (\d+)/(\d+) passed\.$') { throw 'DualCamera flow test summary is malformed.' }
        Assert-Condition ([int]$Matches[1] -eq $dualCameraPassLines.Count -and [int]$Matches[2] -eq $dualCameraPassLines.Count) 'DualCamera flow test summary does not match emitted PASS lines.'

        Assert-Condition (Test-Path -LiteralPath $operatorShellTestExecutable -PathType Leaf) 'M3 operator shell test executable was not produced by the solution build.'
        $operatorShellTestOutput = & $operatorShellTestExecutable 2>&1
        if ($LASTEXITCODE -ne 0) { throw "M3 operator shell tests failed: $($operatorShellTestOutput -join [Environment]::NewLine)" }
        $operatorPassLines = @($operatorShellTestOutput | Where-Object { $_ -match '^PASS ' })
        Assert-Condition ($operatorPassLines.Count -gt 0) 'M3 operator shell tests did not emit any PASS result.'
        $operatorSummary = @($operatorShellTestOutput | Where-Object { $_ -match '^Operator shell tests: (\d+)/(\d+) passed\.$' }) | Select-Object -Last 1
        Assert-Condition ($null -ne $operatorSummary) 'M3 operator shell test summary is missing.'
        if ($operatorSummary -notmatch '^Operator shell tests: (\d+)/(\d+) passed\.$') { throw 'M3 operator shell test summary is malformed.' }
        Assert-Condition ([int]$Matches[1] -eq $operatorPassLines.Count -and [int]$Matches[2] -eq $operatorPassLines.Count) 'M3 operator shell test summary does not match emitted PASS lines.'
        # The summary is derived from the PASS lines themselves, so the comparison above cannot catch a check
        # that stopped reporting. Pin the PASS count to the structure of Program.cs: the WPF contract check (1),
        # one PASS per top-level try block, and one PASS per top-level RunScenarioAsync call. Every try block
        # needs its catch, which is checked first so an unbalanced edit is reported as such. A block that
        # prints two PASS lines or none changes the count, and this derivation has to be updated with it.
        # remaining= in every UNRUN report means the number of checks after the one that stopped the run, where a
        # check is a top-level try block or a top-level RunScenarioAsync call (each ends in one PASS line). It is
        # derived from the structure below and compared with each literal, so adding a check without updating the
        # literals fails here instead of leaving a stale count (issue #236).
        $operatorProgramText = Get-Content -Raw -LiteralPath (Join-Path $RepositoryRoot 'tests/m3/OperatorShellTests/Program.cs')
        $tryCount = ([regex]::Matches($operatorProgramText, '(?m)^try[ \t]*\r?$')).Count
        $catchCount = ([regex]::Matches($operatorProgramText, '(?m)^catch\b')).Count
        $scenarioCount = ([regex]::Matches($operatorProgramText, '(?m)^if \(await WpfCommandTestRunner\.RunScenarioAsync\(')).Count
        Assert-Condition ($tryCount -eq $catchCount) "Program.cs top-level try/catch count mismatch ($tryCount/$catchCount)."
        $operatorSourceCheckCount = 1 + $tryCount + $scenarioCount
        Assert-Condition ($operatorPassLines.Count -eq $operatorSourceCheckCount) "M3 operator shell tests emitted $($operatorPassLines.Count) PASS lines, expected $operatorSourceCheckCount (1 WPF contract + $tryCount top-level try + $scenarioCount top-level RunScenarioAsync) in Program.cs."
        $operatorCheckMatches = @([regex]::Matches($operatorProgramText, '(?m)^(?:try[ \t]*\r?$|if \(await WpfCommandTestRunner\.RunScenarioAsync\()'))
        Assert-Condition ($operatorCheckMatches.Count -eq $tryCount + $scenarioCount) 'The check pattern must match exactly the top-level try blocks and RunScenarioAsync calls.'
        # The WPF contract check runs first and its failure stops everything after it: every try block and scenario.
        $expectedEarlyRemaining = $operatorCheckMatches.Count
        Assert-Condition ($operatorProgramText.Contains("UNRUN runner=normal remaining=$expectedEarlyRemaining reason=lifetime-contract-failure")) "The early UNRUN remaining count in Program.cs is stale; it must be $expectedEarlyRemaining (every top-level try block and RunScenarioAsync call)."
        for ($checkIndex = 0; $checkIndex -lt $operatorCheckMatches.Count; $checkIndex++) {
            $checkMatch = $operatorCheckMatches[$checkIndex]
            if (-not $checkMatch.Value.StartsWith('if')) { continue }
            $scenarioRemaining = [regex]::Match($operatorProgramText.Substring($checkMatch.Index), 'remaining: (\d+)\)')
            Assert-Condition $scenarioRemaining.Success "A RunScenarioAsync call at character $($checkMatch.Index) in Program.cs has no 'remaining:' argument."
            $expectedScenarioRemaining = $operatorCheckMatches.Count - $checkIndex - 1
            Assert-Condition ([int]$scenarioRemaining.Groups[1].Value -eq $expectedScenarioRemaining) "The RunScenarioAsync call at character $($checkMatch.Index) in Program.cs has remaining: $($scenarioRemaining.Groups[1].Value); it must be $expectedScenarioRemaining (the number of checks after it)."
        }
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
    Assert-Condition ($windowXml.Window.Title -eq '{Binding WindowTitle}' -and $viewModelText.Contains('public const string SimulationBanner = "模擬動作（実機未接続）"') -and $viewModelText.Contains('public string WindowTitle')) 'Window title must keep its runtime banner binding and explicit simulation banner.'
    Assert-Condition ($windowText.Contains('AutomationProperties.Name="{Binding WindowAutomationName}"') -and $viewModelText.Contains('public string WindowAutomationName')) 'Window accessibility name must keep its runtime environment binding.'
    foreach ($marker in @('模擬動作（実機未接続）', 'NO AUTO RETRY', '模擬動作のライブ表示（実画像ではありません）', 'FailedPartial', '新しい撮影を準備', '確認なし', 'read-only', '1台構成', '2台構成', 'SelectedOperatingMode', 'CaptureButtonText')) {
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

    foreach ($marker in @('TargetX', 'TargetY', 'MoveTargetByStageDrag', 'MoveTargetByLoupeDrag', 'TargetFineDragScale', 'LoupeCameraAlias', 'LoupeImage', 'IsLoupeSourceLive', 'LoupeFreshnessText')) {
        Assert-Condition ($viewModelText.Contains($marker)) "Operator shell is missing required target reticle / loupe marker (issue #30): $marker"
    }
    foreach ($marker in @('共通ターゲット□', '拡大エリア', 'LoupeDisplayArea', 'StageDisplayArea', 'FractionToMarginConverter')) {
        Assert-Condition ($windowText.Contains($marker)) "Operator shell window is missing required target reticle / loupe binding/marker (issue #30): $marker"
    }
    # 機体照合オーバーレイ（issue #62・ADR-0025）
    foreach ($marker in @('機体照合（CAM-A / CAM-B の割当）', 'DualBinding.IsOverlayVisible', 'DualBinding.ShowCandidateCommand', 'DualBinding.AssignCameraACommand', 'DualBinding.AssignCameraBCommand', 'DualBinding.CompleteBindingCommand', 'DualBinding.ResidualRiskText', 'DualBinding.InvalidationText', 'DualBinding.OverlayAutomationName', 'DualBinding.IsNotShutdownBlocked', '機体照合を表示（模擬）')) {
        Assert-Condition (($windowText + $viewModelText).Contains($marker)) "Operator shell is missing required dual binding overlay binding/marker (issue #62): $marker"
    }
    # キーボードだけで到達できることと、読み上げ名が付いていることを markup 段で固定する。
    # WPF の Button は既定で Focusable かつ IsTabStop なので、守るべきなのは
    # 「それを打ち消していないこと」と「名前が付いていること」の2点。
    # The root name follows the state (issue #225): the assignment wording lives in the view model next to
    # the window-close wording, and the root binds to it.
    $bindingOverlayNode = @($windowXml.SelectNodes('//*[@*[local-name()="AutomationProperties.Name" and contains(., "DualBinding.OverlayAutomationName")]]')) | Select-Object -First 1
    Assert-Condition ($null -ne $bindingOverlayNode) 'The dual binding overlay must expose an accessibility name on its root (issue #62).'
    Assert-Condition ((Get-Content -Raw -LiteralPath (Join-Path $RepositoryRoot 'src/m3/OperatorShell/ViewModels/DualBindingViewModel.cs')).Contains('"機体照合（CAM-A / CAM-B の割当）"')) 'The dual binding view model must keep the assignment wording of the overlay accessibility name (issue #62).'
    # While window close waits for the Agent (issue #225), the residual-risk text and the start button are hidden.
    foreach ($hiddenWhileBlocked in @('機体照合の残留リスク', '機体照合を開始する')) {
        $hiddenNode = @($bindingOverlayNode.SelectNodes(('.//*[@*[local-name()="AutomationProperties.Name" and .="{0}"]]' -f $hiddenWhileBlocked))) | Select-Object -First 1
        Assert-Condition ($null -ne $hiddenNode -and $hiddenNode.GetAttribute('Visibility').Contains('DualBinding.IsNotShutdownBlocked')) "The overlay element '$hiddenWhileBlocked' must be hidden while shutdown is blocked (issue #225)."
    }
    $bindingButtons = @($bindingOverlayNode.SelectNodes('.//*[local-name()="Button"]'))
    Assert-Condition ($bindingButtons.Count -ge 4) "The dual binding overlay must offer its actions as focusable buttons (issue #62); found $($bindingButtons.Count)."
    foreach ($button in $bindingButtons) {
        $automationName = $button.GetAttribute('AutomationProperties.Name')
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($automationName)) 'Every dual binding overlay button must carry an AutomationProperties.Name for screen readers (issue #62).'
        Assert-Condition ($button.GetAttribute('IsTabStop') -ne 'False') 'A dual binding overlay button must never be removed from the tab order (issue #62).'
        Assert-Condition ($button.GetAttribute('Focusable') -ne 'False') 'A dual binding overlay button must never be made unfocusable (issue #62).'
    }
    Assert-Condition ($windowText.Contains('AutomationProperties.LiveSetting="Assertive"')) 'The binding invalidation notice must announce itself assertively (issue #62).'
    # Window-close "confirming" indicator (issue #228). It must not live inside the binding overlay (✕ can be pressed
    # after that overlay is gone), its text must be the announced live region (an explicit AutomationProperties.Name on
    # it would replace the text for a screen reader), and the code-behind must raise it before disabling the window.
    $confirmingNode = @($windowXml.SelectNodes('//*[@*[local-name()="Visibility" and contains(., "DualBinding.IsShutdownConfirming")]]')) | Select-Object -First 1
    Assert-Condition ($null -ne $confirmingNode) 'The window must bind an indicator to DualBinding.IsShutdownConfirming (issue #228).'
    Assert-Condition ($null -eq $confirmingNode.SelectSingleNode('ancestor::*[@*[local-name()="AutomationProperties.Name" and contains(., "DualBinding.OverlayAutomationName")]]')) 'The close indicator must not be nested in the binding overlay (issue #228).'
    $confirmingText = @($confirmingNode.SelectNodes('.//*[@*[local-name()="Text" and contains(., "DualBinding.ShutdownConfirmingText")]]')) | Select-Object -First 1
    Assert-Condition ($null -ne $confirmingText) 'The close indicator must show DualBinding.ShutdownConfirmingText (issue #228).'
    Assert-Condition ($confirmingText.GetAttribute('AutomationProperties.LiveSetting') -eq 'Polite') 'The close indicator text must be a polite live region (issue #228).'
    Assert-Condition ([string]::IsNullOrEmpty($confirmingText.GetAttribute('AutomationProperties.Name'))) 'The close indicator text must not carry an AutomationProperties.Name that would replace it for a screen reader (issue #228).'
    Assert-Condition ($confirmingText.GetAttribute('TextWrapping') -eq 'Wrap') 'The close indicator headline must wrap (issue #228).'
    Assert-Condition ($confirmingText.GetAttribute('FontSize') -eq '{StaticResource FontSizeDialogTitle}' -and $confirmingText.GetAttribute('FontWeight') -eq 'Bold') 'The close indicator headline must use the existing dialog title size and Bold, with no new token (issue #228).'
    $confirmingDetail = @($confirmingNode.SelectNodes('.//*[@*[local-name()="Text" and contains(., "DualBinding.ShutdownConfirmingDetailText")]]')) | Select-Object -First 1
    Assert-Condition ($null -ne $confirmingDetail) 'The close indicator must show DualBinding.ShutdownConfirmingDetailText as a second line (issue #228).'
    Assert-Condition ($confirmingDetail.GetAttribute('AutomationProperties.LiveSetting') -eq 'Polite') 'The close indicator second line must be a polite live region (issue #228).'
    Assert-Condition ([string]::IsNullOrEmpty($confirmingDetail.GetAttribute('AutomationProperties.Name'))) 'The close indicator second line must not carry an AutomationProperties.Name (issue #228).'
    Assert-Condition ($confirmingDetail.GetAttribute('TextWrapping') -eq 'Wrap') 'The close indicator second line must wrap (issue #228).'
    Assert-Condition ($confirmingDetail.GetAttribute('Style') -eq '{StaticResource MutedTextStyle}' -and $confirmingDetail.GetAttribute('FontSize') -eq '{StaticResource FontSizeLabel}') 'The close indicator second line must use MutedTextStyle and FontSizeLabel (issue #228).'
    Assert-Condition ($null -eq $confirmingNode.SelectSingleNode('.//*[@*[local-name()="Text" and contains(., "秒")]]')) 'The close indicator must not state a number of seconds (issue #228).'
    # The order of one window-close attempt lives in HardwareDualWindowCloseSequence (issue #239), which the window
    # and OperatorShellTests both run; the order itself is exercised there. This text check only guards what a
    # behaviour test cannot see: that OnClosing still reaches the sequence, supplies each window action exactly
    # once, and does not run the gate or the individual steps itself. It reads OnClosing's body only (up to the
    # next member), not the rest of the file, so a token elsewhere cannot satisfy or break it.
    $mainWindowCode = Get-Content -Raw -LiteralPath (Join-Path $RepositoryRoot 'src/m3/OperatorShell/MainWindow.xaml.cs')
    $closingBegin = $mainWindowCode.IndexOf('private async void OnClosing')
    Assert-Condition ($closingBegin -ge 0 -and $mainWindowCode.IndexOf('private async void OnClosing', $closingBegin + 1) -lt 0) 'MainWindow must declare exactly one OnClosing handler (issue #239).'
    $closingRest = $mainWindowCode.Substring($closingBegin + 1)
    $closingEnd = [regex]::Match($closingRest, '\r?\n    (private|public|internal|protected)\s')
    Assert-Condition $closingEnd.Success 'The member after OnClosing could not be found to bound the scan (issue #239).'
    $closingText = $closingRest.Substring(0, $closingEnd.Index)
    foreach ($closingToken in @('HardwareDualWindowCloseSequence.RunAsync(', 'BeginShutdownConfirmation()', 'EndShutdownConfirmation()', 'CancelBindingOnShutdownAsync()', 'ReportShutdownBlocked(', 'IsEnabled = false', 'IsEnabled = true', '_shutdownStarted = true', '_shutdownStarted = false', '_lifetime.Cancel()', '_dualBindingHostLifetimeMonitor?.Stop()', '_dualBindingHostLifetimeMonitor?.Start()')) {
        $closingTokenCount = ([regex]::Matches($closingText, [regex]::Escape($closingToken))).Count
        Assert-Condition ($closingTokenCount -eq 1) "OnClosing must contain '$closingToken' exactly once; found $closingTokenCount (issues #228, #239)."
    }
    foreach ($closingForbidden in @('HardwareDualWindowShutdownGate', 'TryShutdownAsync')) {
        Assert-Condition (-not $closingText.Contains($closingForbidden)) "OnClosing must not run '$closingForbidden' itself; the close sequence owns it (issue #239)."
    }
    $closeSequenceCode = Get-Content -Raw -LiteralPath (Join-Path $RepositoryRoot 'src/m3/OperatorShell/Hardware/HardwareDualWindowCloseSequence.cs')
    $closeSequenceIndexes = @('steps.StopHostLifetimeMonitor()', 'agent?.BeginConfirmation()', 'steps.DisableWindow()', 'steps.CancelWindowLifetime()', 'HardwareDualWindowShutdownGate.TryShutdownAsync(', 'agent.ReportBlocked(', 'agent.EndConfirmation()', 'agent.EnableWindow()', 'agent.RestartHostLifetimeMonitor()') | ForEach-Object {
        Assert-Condition (([regex]::Matches($closeSequenceCode, [regex]::Escape($_))).Count -eq 1) "HardwareDualWindowCloseSequence must contain '$_' exactly once (issue #239)."
        $closeSequenceCode.IndexOf($_)
    }
    for ($closeSequenceStep = 1; $closeSequenceStep -lt $closeSequenceIndexes.Count; $closeSequenceStep++) {
        Assert-Condition ($closeSequenceIndexes[$closeSequenceStep] -gt $closeSequenceIndexes[$closeSequenceStep - 1]) 'HardwareDualWindowCloseSequence must keep its order: stop monitor, show the indicator, disable the window, cancel the lifetime, run the gate, then report, hide the indicator, re-enable and restart the monitor (issues #228, #239).'
    }

    $fractionConverterPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/Converters/FractionToMarginConverter.cs'
    Assert-Condition (Test-Path -LiteralPath $fractionConverterPath -PathType Leaf) 'FractionToMarginConverter.cs must exist to position the target reticle and loupe marker overlays.'
    Assert-Condition ((Get-Content -Raw -LiteralPath (Join-Path $RepositoryRoot 'src/m3/OperatorShell/MainWindow.xaml.cs')).Contains('MoveTargetByStageDrag')) 'MainWindow code-behind must wire stage drag input to the target reticle view model method.'

    foreach ($marker in @('AutoFocusCommand', 'CanExecuteAutoFocus', 'IsFocusTargetOutsideLiveCameraDomain', 'IsFocusPanelAvailable', 'DualCameraExecutionEnvironment.HardwareDual', 'MfCoarseForwardCommand', 'MfFineForwardCommand', 'TogglePeakingCommand', 'FocusPeakingOverlayRenderer', 'SwitchLiveCameraToTargetDomainCommand', 'CameraAFocusStatusText', 'FocusExecutionResult', 'LastPreCaptureAutoFocusResult')) {
        Assert-Condition ($viewModelText.Contains($marker)) "Operator shell is missing required focus panel marker (issue #31): $marker"
    }
    foreach ($marker in @('フォーカスパネル AF実行 MFステップ ピーキング 固定状態チップ 撮影系操作', 'フォーカスパネル 実機モードでは無効表示 fail-closed 理由', 'MF粗ステップ', 'MF微ステップ', 'フォーカスピーキング')) {
        Assert-Condition ($windowText.Contains($marker)) "Operator shell window is missing required focus panel binding/marker (issue #31): $marker"
    }
    $peakingRendererPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/Simulated/FocusPeakingOverlayRenderer.cs'
    Assert-Condition (Test-Path -LiteralPath $peakingRendererPath -PathType Leaf) 'FocusPeakingOverlayRenderer.cs must exist to render the preview-only focus peaking overlay (issue #31).'

    foreach ($marker in @('CaptureWithAutoFocusCommand', 'CanCaptureWithAutoFocus', 'RunCaptureWithAutoFocusAsync', 'SimulatePreCaptureAutoFocus', 'IsActionZonePreparing', 'IsActionZoneProcessing', 'IsActionZoneReview', 'ProgressWatchdogText', 'RecordPreCaptureAutoFocusOutcome')) {
        Assert-Condition ($viewModelText.Contains($marker)) "Operator shell is missing required action zone / 撮影+AF marker (issue #33): $marker"
    }
    foreach ($marker in @('アクションゾーン 状態駆動 設置判定撮影 自動進捗 結果', '従ボタン 撮影 + AF', 'A→Bの順に撮影し、完了後に合成へ進みます', 'アクションゾーン state1 準備中 設置判定と撮影ボタン', 'アクションゾーン state2 自動進捗ストリップ', 'アクションゾーン state3 結果パネル')) {
        Assert-Condition ($windowText.Contains($marker)) "Operator shell window is missing required action zone binding/marker (issue #33): $marker"
    }
    Assert-Condition (-not $windowText.Contains('アクションゾーン 撮影と結果 暫定配置')) 'Issue #33 must replace the provisional single-block アクションゾーン layout with the state-driven 3-way one.'

    foreach ($marker in @('DocumentTiltDetector', 'TiltRollDegrees', 'TiltRollDegreesText', 'CurrentLiveTiltSourceImage', 'TiltToleranceDegrees', 'TiltToleranceInputText', 'TiltToleranceChipText', 'IsGridOverlayEnabled', 'IsTombOverlayEnabled', 'IsOverlapBandOverlayEnabled', 'IsSafeMarginOverlayEnabled', 'IsOverlapBandVisible', '許容値未設定', '検出不能')) {
        Assert-Condition ($viewModelText.Contains($marker)) "Operator shell is missing required alignment guide / tilt reading marker (issue #32): $marker"
    }
    $tiltDetectorPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/Simulated/DocumentTiltDetector.cs'
    Assert-Condition (Test-Path -LiteralPath $tiltDetectorPath -PathType Leaf) 'DocumentTiltDetector.cs must exist to compute the preview-only ROLL tilt reading (issue #32).'
    Assert-Condition (-not (Get-Content -Raw -LiteralPath $tiltDetectorPath).Contains('CanCapture')) 'DocumentTiltDetector must stay a pure detection function with no reference back into capture/readiness state.'
    foreach ($marker in @('設置ガイドオーバーレイ', '方眼グリッド', 'トンボ', '安全マージン', 'SAFE MARGIN', '傾き読み値 常駐行', '許容範囲チップ')) {
        Assert-Condition ($windowText.Contains($marker)) "Operator shell window is missing required alignment guide / tilt reading binding/marker (issue #32): $marker"
    }

    # issue #34: menu bar replaces the left-nav TabControl. ファイル/カメラ/表示/ツール/ヘルプの
    # 5メニューのみで構成し、左ナビ（TabStripPlacement="Left"）は削除され、編集メニューは
    # 設置しない（原本の無加工・byte-identical保存契約のため画像編集機能が設計上存在しない）。
    Assert-Condition ($windowText.Contains('<Menu ')) 'Operator shell window must add a WPF Menu element for the menu bar (issue #34).'
    Assert-Condition (-not $windowText.Contains('TabStripPlacement="Left"')) 'Issue #34 must remove the left-nav TabControl (TabStripPlacement="Left").'
    Assert-Condition (-not $windowText.Contains('Header="編集')) 'Issue #34 must not add an 編集 (Edit) top-level menu — no image-editing feature exists in this contract.'
    foreach ($marker in @('メニューバー ファイル カメラ 表示 ツール ヘルプ', 'ファイル(_F)', 'カメラ(_C)', '表示(_V)', 'ツール(_T)', 'ヘルプ(_H)', '保存先を指定', 'このPCのフォルダへ保存(_E)', '終了(_X)', '運用構成(_M)', 'カメラ設定を表示（read-only）', 'readiness再検査(_R)', '拡大エリア倍率', '傾き読み値の表示', '設置・校正(_S)', '再合成（別job）(_R)', '保存・診断(_D)', '技術情報（error code・ログ位置）(_T)', 'バージョン(_V)', '保守画面から撮影ダッシュボードへ戻る')) {
        Assert-Condition (($windowText + $viewModelText).Contains($marker)) "Operator shell is missing required menu bar binding/marker (issue #34): $marker"
    }
    foreach ($marker in @('SelectedPage', 'PageTitle', 'ShowDashboardCommand', 'ShowSetupCommand', 'ShowCameraSettingsCommand', 'ShowDiagnosticsCommand', 'IsSingleCameraModeChecked', 'IsDualCameraModeChecked', 'IsLoupeZoom100Checked', 'IsLoupeZoom200Checked', 'IsTiltReadingVisible', 'DualCameraIdentityStatusText', 'AppVersionText')) {
        Assert-Condition ($viewModelText.Contains($marker)) "Operator shell is missing required menu bar view model marker (issue #34): $marker"
    }
    $stringEqualsVisibilityConverterPath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/Converters/StringEqualsVisibilityConverter.cs'
    Assert-Condition (Test-Path -LiteralPath $stringEqualsVisibilityConverterPath -PathType Leaf) 'StringEqualsVisibilityConverter.cs must exist to switch between the dashboard and maintenance pages without a TabControl (issue #34).'

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
    # Window-close indicator of the SingleCamera window (issue #244). The window is disabled while the Camera Agent's
    # end is awaited, so the texts must be announced live regions (no fixed AutomationProperties.Name, which would
    # replace the text for a screen reader), shown through IsShutdownConfirming, and raised before the window is disabled.
    Assert-Condition ($hardwareWindowText.Contains('IsShutdownConfirming')) 'The SingleCamera window must show its close indicator through IsShutdownConfirming (issue #244).'
    foreach ($shutdownTextBinding in @('ShutdownConfirmingText', 'ShutdownConfirmingDetailText', 'ShutdownFrameWaitText')) {
        $shutdownTextNode = @($hardwareWindowXml.SelectNodes(('//*[@*[local-name()="Text" and .="{{Binding {0}}}"]]' -f $shutdownTextBinding))) | Select-Object -First 1
        Assert-Condition ($null -ne $shutdownTextNode) "The SingleCamera window must show $shutdownTextBinding (issue #244)."
        Assert-Condition ($shutdownTextNode.GetAttribute('AutomationProperties.LiveSetting') -eq 'Polite' -and $shutdownTextNode.GetAttribute('controls:LiveRegion.Announce') -eq 'True') "$shutdownTextBinding must be a polite announced live region (issue #244)."
        Assert-Condition ([string]::IsNullOrEmpty($shutdownTextNode.GetAttribute('AutomationProperties.Name'))) "$shutdownTextBinding must not carry a fixed AutomationProperties.Name (issue #244)."
        Assert-Condition ($shutdownTextNode.GetAttribute('TextWrapping') -eq 'Wrap') "$shutdownTextBinding must wrap (issue #244)."
    }
    $hardwareWindowCode = Get-Content -Raw -LiteralPath (Join-Path $RepositoryRoot 'src/m3/OperatorShell/HardwareSingleCameraWindow.xaml.cs')
    $hardwareClosingBegin = $hardwareWindowCode.IndexOf('private async void OnClosing')
    Assert-Condition ($hardwareClosingBegin -ge 0) 'The SingleCamera window must keep its OnClosing handler (issue #244).'
    # Bound the scan to OnClosing's own body (up to the next member, as for MainWindow); OnClosing is the last member
    # today, in which case the rest of the file is its body.
    $hardwareClosingRest = $hardwareWindowCode.Substring($hardwareClosingBegin + 1)
    $hardwareClosingEnd = [regex]::Match($hardwareClosingRest, '\r?\n    (private|public|internal|protected)\s')
    $hardwareClosingText = if ($hardwareClosingEnd.Success) { $hardwareClosingRest.Substring(0, $hardwareClosingEnd.Index) } else { $hardwareClosingRest }
    $hardwareBeginIndex = $hardwareClosingText.IndexOf('BeginShutdownConfirmation()')
    Assert-Condition ($hardwareBeginIndex -ge 0 -and ([regex]::Matches($hardwareClosingText, [regex]::Escape('BeginShutdownConfirmation()'))).Count -eq 1) 'The SingleCamera OnClosing must raise the close indicator exactly once (issue #244).'
    Assert-Condition ($hardwareBeginIndex -lt $hardwareClosingText.IndexOf('IsEnabled = false;')) 'The SingleCamera OnClosing must raise the close indicator before it disables the window (issue #244).'
    Assert-Condition ($hardwareBeginIndex -lt $hardwareClosingText.IndexOf('ShutdownAsync()')) 'The SingleCamera OnClosing must raise the close indicator before it waits for ShutdownAsync (issue #244).'

    foreach ($marker in @('CAM-A原画像の確認', 'CAM-B原画像の確認', 'このPCのフォルダへ保存', '実JPEG合成完了', 'DualCameraExecutionEnvironment.TestSynthetic')) {
        Assert-Condition (($windowText + $viewModelText + $dualCompositionText).Contains($marker)) "Formal DualCamera WPF flow is missing required marker: $marker"
    }
    Assert-Condition ($dualCompositionText.Contains('DualCameraProductFlow')) 'WPF composition must use the typed DualCamera application flow.'
    Assert-Condition ($dualCompositionText.Contains('M2OfflineStitcherProcessAdapter')) 'WPF composition must connect the M2 offline stitcher adapter.'
    Assert-Condition ($dualCompositionText.Contains('does not fall back to SingleCamera')) 'Missing M2 adapter must fail closed without SingleCamera fallback.'
    foreach ($marker in @('DualCameraExecutionEnvironment.HardwareDual', 'HardwareDualCaptureSource', 'DualCameraIdentitySnapshot.HardwarePending')) {
        Assert-Condition ($dualCompositionText.Contains($marker)) "HardwareDual production composition is missing fail-closed marker: $marker"
    }

    # Issue #238: AutomationProperties.LiveSetting only declares a live region; WPF does not raise LiveRegionChanged
    # when its text changes. Every live region must therefore opt in to the LiveRegion.Announce attached property
    # (which raises the event from the peer), and Announce is only meaningful on a region that has a LiveSetting.
    $liveRegionSourcePath = Join-Path $RepositoryRoot 'src/m3/OperatorShell/Controls/LiveRegion.cs'
    Assert-Condition (Test-Path -LiteralPath $liveRegionSourcePath -PathType Leaf) 'LiveRegion.cs must exist to raise LiveRegionChanged for live regions (issue #238).'
    $liveRegionSourceText = Get-Content -Raw -LiteralPath $liveRegionSourcePath
    Assert-Condition ($liveRegionSourceText.Contains('RaiseAutomationEvent(AutomationEvents.LiveRegionChanged)')) 'LiveRegion must raise AutomationEvents.LiveRegionChanged from the peer (issue #238).'
    foreach ($liveRegionWindow in @(
            @{ Name = 'MainWindow.xaml'; Xml = $windowXml; MinimumCount = 20 },
            @{ Name = 'HardwareSingleCameraWindow.xaml'; Xml = $hardwareWindowXml; MinimumCount = 4 })) {
        $liveRegionXml = $liveRegionWindow.Xml
        Assert-Condition ($liveRegionXml.DocumentElement.GetAttribute('xmlns:controls') -eq 'clr-namespace:A0CameraStitcher.M3.OperatorShell.Controls') "$($liveRegionWindow.Name) must map the controls prefix to the LiveRegion namespace (issue #238)."
        $liveNodes = @($liveRegionXml.SelectNodes('//*[@*[local-name()="AutomationProperties.LiveSetting"]]'))
        # MinimumCount counts the regions that announce (LiveSetting other than Off). Elements that change every
        # second or duplicate a persistent region are deliberately Off and are not counted (issue #238).
        $announcingLiveNodes = @($liveNodes | Where-Object { $_.GetAttribute('AutomationProperties.LiveSetting') -ne 'Off' })
        Assert-Condition ($announcingLiveNodes.Count -ge $liveRegionWindow.MinimumCount) "$($liveRegionWindow.Name) has $($announcingLiveNodes.Count) announcing LiveSetting elements, expected at least $($liveRegionWindow.MinimumCount) (issue #238)."
        foreach ($liveNode in $liveNodes) {
            $liveSetting = $liveNode.GetAttribute('AutomationProperties.LiveSetting')
            if ($liveSetting -eq 'Off') {
                Assert-Condition ([string]::IsNullOrEmpty($liveNode.GetAttribute('controls:LiveRegion.Announce'))) "$($liveRegionWindow.Name) <$($liveNode.LocalName)> has LiveSetting=Off together with LiveRegion.Announce; remove Announce (issue #238)."
                continue
            }
            $liveBinding = @($liveNode.Attributes | Where-Object { $_.Name -eq 'Text' }) | Select-Object -First 1
            $liveDescription = "$($liveRegionWindow.Name) <$($liveNode.LocalName) Text=$($liveBinding.Value)>"
            Assert-Condition ($liveNode.LocalName -eq 'TextBlock') "$liveDescription has LiveSetting but is not a TextBlock, which LiveRegion.Announce cannot observe (issue #238)."
            Assert-Condition ($liveNode.GetAttribute('controls:LiveRegion.Announce') -eq 'True') "$liveDescription has LiveSetting=$liveSetting without controls:LiveRegion.Announce=""True"", so a text change is not announced (issue #238)."
        }
        $announceNodes = @($liveRegionXml.SelectNodes('//*[@*[local-name()="LiveRegion.Announce"]]'))
        foreach ($announceNode in $announceNodes) {
            Assert-Condition (-not [string]::IsNullOrEmpty($announceNode.GetAttribute('AutomationProperties.LiveSetting'))) "$($liveRegionWindow.Name) has LiveRegion.Announce on an element without AutomationProperties.LiveSetting (issue #238)."
            # WPF returns a fixed AutomationProperties.Name in preference to the Text, so a fixed Name on a live region
            # makes the screen reader read the label instead of the changed text. Only a Name bound to the text is allowed.
            $announceName = $announceNode.GetAttribute('AutomationProperties.Name')
            Assert-Condition ([string]::IsNullOrEmpty($announceName) -or $announceName.StartsWith('{Binding')) "$($liveRegionWindow.Name) <$($announceNode.LocalName) Text=$($announceNode.GetAttribute('Text'))> has a fixed AutomationProperties.Name ""$announceName"" on a LiveRegion.Announce element; delete it or bind it to the text (issue #238)."
        }
    }
    # Issue #242: the main capture button is shown only while the action zone is in its preparing state. During a run
    # the progress strip replaces it, which is why the re-check wording cannot flash on it when the stored ID is set mid-run.
    foreach ($commandName in @('CaptureCommand', 'CaptureWithAutoFocusCommand')) {
        $captureButtons = @($windowXml.SelectNodes("//*[local-name()='Button'][@Command='{Binding $commandName}']"))
        Assert-Condition ($captureButtons.Count -eq 1) "MainWindow.xaml must have exactly one button bound to $commandName, found $($captureButtons.Count) (issue #242)."
        $ancestor = $captureButtons[0].ParentNode
        $underPreparingZone = $false
        while ($null -ne $ancestor -and $ancestor.NodeType -eq 'Element') {
            if ($ancestor.GetAttribute('Visibility') -like '*IsActionZonePreparing*') { $underPreparingZone = $true; break }
            $ancestor = $ancestor.ParentNode
        }
        Assert-Condition $underPreparingZone "The button bound to $commandName must sit under the element whose Visibility is bound to IsActionZonePreparing (issue #242)."
    }
    # Issue #242: in the result panel the re-stitch button and the prepare-new-capture button used to sit in the
    # same Grid cell, so one covered the other. They must occupy different cells, and the re-stitch button is only
    # shown on the ordinary two-camera path.
    $restitchButtons = @($windowXml.SelectNodes('//*[local-name()="Button"][@Command="{Binding RestitchCommand}"]'))
    Assert-Condition ($restitchButtons.Count -eq 1) "MainWindow.xaml must have exactly one re-stitch button, found $($restitchButtons.Count) (issue #242)."
    $restitchButton = $restitchButtons[0]
    Assert-Condition ($restitchButton.GetAttribute('Visibility') -like '*IsRestitchButtonVisible*') 'The re-stitch button must bind its Visibility to IsRestitchButtonVisible so that it is hidden where it can never apply (issue #242).'
    # A cell range covers Grid.Row..Row+RowSpan-1 and Grid.Column..Column+ColumnSpan-1, so a button that spans into
    # another button's cell is caught as well. Missing attributes mean row 0, column 0, span 1.
    $gridCellsOf = {
        param($node)
        $intAttribute = {
            param($name, $default)
            $text = $node.GetAttribute($name)
            if ([string]::IsNullOrEmpty($text)) { $default } else { [int]$text }
        }
        $row = & $intAttribute 'Grid.Row' 0
        $column = & $intAttribute 'Grid.Column' 0
        $rowSpan = & $intAttribute 'Grid.RowSpan' 1
        $columnSpan = & $intAttribute 'Grid.ColumnSpan' 1
        foreach ($cellRow in $row..($row + $rowSpan - 1)) {
            foreach ($cellColumn in $column..($column + $columnSpan - 1)) { "$cellRow,$cellColumn" }
        }
    }
    Assert-Condition ([string]::IsNullOrEmpty($restitchButton.GetAttribute('Grid.ColumnSpan'))) 'The re-stitch button must stay within its own column; spanning the row makes it wider than the buttons above it (issue #242).'
    $restitchCells = @(& $gridCellsOf $restitchButton)
    $siblingButtons = @($restitchButton.ParentNode.ChildNodes | Where-Object { $_.NodeType -eq 'Element' -and $_.LocalName -eq 'Button' -and -not [object]::ReferenceEquals($_, $restitchButton) })
    Assert-Condition ($siblingButtons.Count -ge 2) 'The re-stitch button must share its Grid with the prepare-new-capture and accept buttons (issue #242).'
    foreach ($siblingButton in $siblingButtons) {
        $sharedCells = @(& $gridCellsOf $siblingButton | Where-Object { $restitchCells -contains $_ })
        Assert-Condition ($sharedCells.Count -eq 0) "Button '$($siblingButton.GetAttribute('Content'))' covers Grid cell ($($sharedCells -join '; ')) of the re-stitch button; one covers the other (issue #242)."
    }
    # Issues #229 and #242 review: the accessible name of these two buttons starts with the visible label
    # (WCAG 2.5.3, label in name) and is Japanese. The result panel's accept button follows its label, which
    # changes with the mode, so its name is bound to the view model instead of being fixed.
    $acceptReviewButtons = @($windowXml.SelectNodes('//*[local-name()="Button"][@Command="{Binding AcceptReviewCommand}"]'))
    Assert-Condition ($acceptReviewButtons.Count -eq 1) "MainWindow.xaml must have exactly one button bound to AcceptReviewCommand, found $($acceptReviewButtons.Count) (issue #242)."
    Assert-Condition ($acceptReviewButtons[0].GetAttribute('Content') -eq '{Binding ReviewPrimaryActionText}' -and $acceptReviewButtons[0].GetAttribute('AutomationProperties.Name') -eq '{Binding ReviewPrimaryActionAutomationName}') 'The accept button must bind its accessible name to ReviewPrimaryActionAutomationName so that it starts with the changing label (issue #242).'
    Assert-Condition (-not $windowText.Contains('固定ローカルへ保持')) 'MainWindow.xaml must call the app''s own store 保管, not 固定ローカルへ保持 (issue #242).'
    $singleExportButtons = @($hardwareWindowXml.SelectNodes('//*[local-name()="Button"][@Command="{Binding ExportCommand}"]'))
    Assert-Condition ($singleExportButtons.Count -eq 1) "HardwareSingleCameraWindow.xaml must have exactly one button bound to ExportCommand, found $($singleExportButtons.Count) (issue #229)."
    $singleExportLabel = $singleExportButtons[0].GetAttribute('Content')
    $singleExportName = $singleExportButtons[0].GetAttribute('AutomationProperties.Name')
    Assert-Condition ($singleExportName.StartsWith($singleExportLabel)) "The single export button's accessible name '$singleExportName' must start with its label '$singleExportLabel' (issue #229)."
    Assert-Condition (-not ($singleExportName -match '[A-Za-z]{3,}')) "The single export button's accessible name '$singleExportName' must not carry English words (issue #229)."
    # Issue #249 (WCAG 2.5.3 Label in Name): the accessible name of a control must contain the text it shows, or voice
    # control by that text does not reach it. A fixed AutomationProperties.Name is compared with the fixed label
    # (Content / Header / nested Text) after dropping what is not spoken: the access key marker "(_X)" or "_", a
    # trailing ellipsis, and leading symbol glyphs. A control whose label is bound must bind its name too; the view
    # model tests (AssertAccessibleNamesContainVisibleLabels in OperatorShellTests) check those state by state. A
    # static text with a fixed Text must not hide it behind a different fixed name either.
    $normalizeLabel = {
        param([string]$text)
        $label = [regex]::Replace($text, '\(_.\)', '')
        $label = $label.Replace('_', '')
        $label = [regex]::Replace($label, '(\.\.\.|…)\s*$', '')
        $label = [regex]::Replace($label, '^[^\p{L}\p{N}]+', '')
        $label.Trim()
    }
    $labelledControlNames = @('Button', 'CheckBox', 'RadioButton', 'ToggleButton', 'MenuItem')
    $labelInNameFailures = New-Object System.Collections.Generic.List[string]
    $labelInNameChecked = 0
    foreach ($labelFile in @('MainWindow.xaml', 'HardwareSingleCameraWindow.xaml', 'LaunchWindow.xaml', 'ReviewImageWindow.xaml', 'HistoricalReviewWindow.xaml')) {
        [xml]$labelXml = Get-Content -Raw -LiteralPath (Join-Path $RepositoryRoot "src/m3/OperatorShell/$labelFile")
        foreach ($labelNode in @($labelXml.SelectNodes('//*[@*[local-name()="AutomationProperties.Name"]]'))) {
            $accessibleName = $labelNode.GetAttribute('AutomationProperties.Name')
            $isControl = $labelledControlNames -contains $labelNode.LocalName
            $visibleTexts = @()
            foreach ($attributeName in @('Content', 'Header', 'Text')) {
                if ($labelNode.HasAttribute($attributeName)) { $visibleTexts += $labelNode.GetAttribute($attributeName) }
            }
            if ($isControl -and $labelNode.LocalName -ne 'MenuItem') {
                foreach ($nested in @($labelNode.SelectNodes('.//*[@Text or @Content]'))) {
                    foreach ($attributeName in @('Content', 'Text')) {
                        if ($nested.HasAttribute($attributeName)) { $visibleTexts += $nested.GetAttribute($attributeName) }
                    }
                }
            }
            if ($visibleTexts.Count -eq 0 -or (-not $isControl -and -not $labelNode.HasAttribute('Text'))) { continue }
            $where = "$labelFile <$($labelNode.LocalName)> name='$accessibleName'"
            if ($accessibleName.Contains('{')) { continue }
            foreach ($visibleText in $visibleTexts) {
                if ($visibleText.Contains('{')) {
                    if ($isControl) { $labelInNameFailures.Add("$where has a fixed name but a bound label '$visibleText'; bind the name to the view model so that it follows the label.") }
                    continue
                }
                $label = & $normalizeLabel $visibleText
                if ($label.Length -eq 0) { continue }
                $labelInNameChecked++
                if (-not $accessibleName.Contains($label)) { $labelInNameFailures.Add("$where does not contain its visible label '$label'.") }
            }
        }
    }
    Assert-Condition ($labelInNameFailures.Count -eq 0) ("Accessible names must contain the visible label (WCAG 2.5.3, issue #249):$([Environment]::NewLine)" + ($labelInNameFailures -join [Environment]::NewLine))
    Assert-Condition ($labelInNameChecked -ge 40) "The label-in-name check compared only $labelInNameChecked labels; it must keep reaching the windows' controls (issue #249)."
    Assert-Condition ($windowText.Contains('AutomationProperties.Name="{Binding CaptureWithAutoFocusUnavailableReason, StringFormat=') -and -not $windowText.Contains('撮影+AF 実機モードでは実行不可 理由')) 'The 撮影+AF reason row must bind its accessible name to its text instead of replacing it with a fixed name (issue #249).'
    # Long reasons and status sentences must wrap instead of being cut off (issue #249).
    foreach ($wrappedBinding in @('CaptureDisabledReason', 'StatusMessage', 'BlockerText', 'CautionText', 'InfoText', 'CaptureWithAutoFocusUnavailableReason')) {
        $wrappedNode = @($windowXml.SelectNodes(('//*[local-name()="TextBlock"][@Text="{{Binding {0}}}"]' -f $wrappedBinding))) | Select-Object -First 1
        Assert-Condition ($null -ne $wrappedNode -and $wrappedNode.GetAttribute('TextWrapping') -eq 'Wrap') "The TextBlock showing $wrappedBinding must set TextWrapping=Wrap so that a long sentence is not cut off (issue #249)."
    }
    Assert-Condition ($viewModelText.Contains('IsCaptureWithAutoFocusUnavailableReasonVisible => CanCapture && !CanCaptureWithAutoFocus;')) 'The 撮影+AF reason row must be shown whenever the main button is pressable but 撮影+AF is not (issue #249).'
    Write-Host 'M3 simulated foundation, formal DualCamera JPEG product flow, and SingleCamera regression passed validation.'
    exit 0
}
catch {
    Write-Error "M3 simulated validation failed: $($_.Exception.Message)"
    exit 1
}
