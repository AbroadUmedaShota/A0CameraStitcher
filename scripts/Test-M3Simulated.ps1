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
        Assert-Condition (($dualCameraFlowOutput -join "`n").Contains('DualCamera flow tests: 18/18 passed.')) 'DualCamera flow test summary is missing or incomplete.'

        Assert-Condition (Test-Path -LiteralPath $operatorShellTestExecutable -PathType Leaf) 'M3 operator shell test executable was not produced by the solution build.'
        $operatorShellTestOutput = & $operatorShellTestExecutable 2>&1
        if ($LASTEXITCODE -ne 0) { throw "M3 operator shell tests failed: $($operatorShellTestOutput -join [Environment]::NewLine)" }
        $operatorPassLines = @($operatorShellTestOutput | Where-Object { $_ -match '^PASS ' })
        Assert-Condition ($operatorPassLines.Count -gt 0) 'M3 operator shell tests did not emit any PASS result.'
        $operatorSummary = @($operatorShellTestOutput | Where-Object { $_ -match '^Operator shell tests: (\d+)/(\d+) passed\.$' }) | Select-Object -Last 1
        Assert-Condition ($null -ne $operatorSummary) 'M3 operator shell test summary is missing.'
        if ($operatorSummary -notmatch '^Operator shell tests: (\d+)/(\d+) passed\.$') { throw 'M3 operator shell test summary is malformed.' }
        Assert-Condition ([int]$Matches[1] -eq $operatorPassLines.Count -and [int]$Matches[2] -eq $operatorPassLines.Count) 'M3 operator shell test summary does not match emitted PASS lines.'
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
    foreach ($marker in @('機体照合（CAM-A / CAM-B の割当）', 'DualBinding.IsOverlayVisible', 'DualBinding.ShowCandidateCommand', 'DualBinding.AssignCameraACommand', 'DualBinding.AssignCameraBCommand', 'DualBinding.CompleteBindingCommand', 'DualBinding.ResidualRiskText', 'DualBinding.InvalidationText', '機体照合を表示（模擬）')) {
        Assert-Condition (($windowText + $viewModelText).Contains($marker)) "Operator shell is missing required dual binding overlay binding/marker (issue #62): $marker"
    }
    # キーボードだけで到達できることと、読み上げ名が付いていることを markup 段で固定する。
    # WPF の Button は既定で Focusable かつ IsTabStop なので、守るべきなのは
    # 「それを打ち消していないこと」と「名前が付いていること」の2点。
    $bindingOverlayNode = @($windowXml.SelectNodes('//*[@*[local-name()="AutomationProperties.Name" and contains(., "機体照合（CAM-A / CAM-B の割当）")]]')) | Select-Object -First 1
    Assert-Condition ($null -ne $bindingOverlayNode) 'The dual binding overlay must expose an accessibility name on its root (issue #62).'
    $bindingButtons = @($bindingOverlayNode.SelectNodes('.//*[local-name()="Button"]'))
    Assert-Condition ($bindingButtons.Count -ge 4) "The dual binding overlay must offer its actions as focusable buttons (issue #62); found $($bindingButtons.Count)."
    foreach ($button in $bindingButtons) {
        $automationName = $button.GetAttribute('AutomationProperties.Name')
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($automationName)) 'Every dual binding overlay button must carry an AutomationProperties.Name for screen readers (issue #62).'
        Assert-Condition ($button.GetAttribute('IsTabStop') -ne 'False') 'A dual binding overlay button must never be removed from the tab order (issue #62).'
        Assert-Condition ($button.GetAttribute('Focusable') -ne 'False') 'A dual binding overlay button must never be made unfocusable (issue #62).'
    }
    Assert-Condition ($windowText.Contains('AutomationProperties.LiveSetting="Assertive"')) 'The binding invalidation notice must announce itself assertively (issue #62).'

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
    foreach ($marker in @('アクションゾーン 状態駆動 設置判定撮影 自動進捗 結果', '従ボタン 撮影+AF', 'A→Bの順に撮影し、完了後に合成へ進みます', 'アクションゾーン state1 準備中 設置判定と撮影ボタン', 'アクションゾーン state2 自動進捗ストリップ', 'アクションゾーン state3 結果パネル')) {
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

    foreach ($marker in @('CAM-A原画像の確認', 'CAM-B原画像の確認', 'このPCのフォルダへ保存', '実JPEG合成完了', 'DualCameraExecutionEnvironment.TestSynthetic')) {
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
