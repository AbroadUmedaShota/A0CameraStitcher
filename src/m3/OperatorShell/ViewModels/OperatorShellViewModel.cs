using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.OperatorShell.Simulated;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

public sealed record CameraSettingRow(string Setting, string RequiredProfile, string CameraA, string CameraB);

/// <summary>保存したファイル1件の控え（保存時刻と実際の出力先）。表示専用。</summary>
public sealed record SavedFileViewModel(string Time, string Path)
{
    public string FileName => System.IO.Path.GetFileName(Path);
}

/// <summary>One completed AF execution (issue #31 focus panel): which camera it ran on, its
/// SIMULATED convergence result, when it ran, and the target reticle position used as the AF
/// area — the fields the panel needs to show "合焦OK/NG・実行時刻・使用した□位置" per
/// docs/OPERATOR_UI_SPEC.md's フォーカス操作 section.</summary>
public sealed record FocusExecutionResult(
    string CameraAlias,
    bool Success,
    DateTimeOffset ExecutedAt,
    double TargetX,
    double TargetY);

public sealed class OperatorShellViewModel : ObservableObject
{
    public const string SimulationBanner = "模擬動作（実機未接続）";
    public const string HardwareDualPendingBanner = "実機2台 / 機体照合の提供元が未接続 / 撮影禁止";
    public const string HardwareDualCaptureRecoveryOnlyBanner =
        "実機2台 / 撮影・原画像保存のみ / 合成保留 / A0品質未承認";
    private const string SingleModeLabel = "1台構成";
    private const string DualModeLabel = "2台構成";
    private const string StageModeCameraALive = "CAM-A live";
    private const string StageModeCameraBLive = "CAM-B live";
    private const string StageModeCompositePreview = "合成プレビュー";
    private const string StageProcessingPlaceholderMessage = "ライブ表示は停止中（撮影を実行しています）";
    private const string StagePreviewNoteMessage = "確認用の表示のみ・原画像／合成には使いません";
    private const string LoupeZoom100 = "100%";
    private const string LoupeZoom200 = "200%";
    private const string LoupeUnavailableText = "フレーム未取得";
    private const int DefaultGridDivision = 3;
    private const int MinGridDivision = 1;
    private const int MaxGridDivision = 24;
    private const int NoticeVisibleMilliseconds = 3200;
    private const int ResetArmedMilliseconds = 3000;
    private const int MaxSavedFileRows = 6;

    /// <summary>ポインタ移動の再通知を間引く閾値。1px未満の揺れでクロップを作り直さない。</summary>
    private const double PointerEpsilon = 0.0005;

    /// <summary>Scale applied to a drag delta captured inside the loupe, relative to the same
    /// delta captured on the stage — the issue #30 "細かい移動" contract (loupe drags move the
    /// target 1/4 as far as an equivalent stage drag, for fine positioning once roughly placed).</summary>
    public const double TargetFineDragScale = 0.25;

    /// <summary>Fraction of the source frame's smaller dimension the loupe crops at 100% zoom;
    /// halved again at 200% so "zoom in" means "crop a smaller region and stretch it to fill
    /// the same display area", not an additional transform on top of the crop.</summary>
    private const double LoupeBaseCropFraction = 0.32;

    /// <summary>Blur radius (see <see cref="SimulatedLiveViewFrame.BlurRadius"/>) at or below
    /// which SIMULATED AF execution reports 合焦OK. Always 0 outside
    /// <see cref="SimulatedFramePattern.BlurToFocusTransition"/>, so AF only has a chance to
    /// report NG while that pattern is selected and mid-ramp.</summary>
    private const double SharpBlurRadiusThreshold = 1.0;

    private const double MfCoarseStep = 10.0;
    private const double MfFineStep = 2.0;
    private const double MinFocusPositionValue = 0.0;
    private const double MaxFocusPositionValue = 100.0;
    private const double DefaultFocusPositionValue = 50.0;

    private readonly ISimulatedTransactionService _transactionService;
    private readonly IDualCameraProductFlow? _dualCameraFlow;
    private readonly Func<DualCameraCaptureRequest>? _hardwareDualRequestProvider;
    private readonly IHardwareDualCaptureRecoveryOnlyWorkflow? _captureRecoveryOnlyWorkflow;
    private readonly ISimulatedLiveViewFramePump? _liveViewFramePump;
    private readonly ISimulatedLiveViewFrameSource? _liveViewFrameSource;
    private readonly SynchronizationContext? _synchronizationContext = SynchronizationContext.Current;
    private readonly AsyncRelayCommand _captureCommand;
    private readonly AsyncRelayCommand _captureWithAutoFocusCommand;
    private readonly AsyncRelayCommand _diagnosticCommand;
    private readonly AsyncRelayCommand _prepareNewCaptureCommand;
    private readonly RelayCommand _acceptSafetyCommand;
    private readonly RelayCommand _declineSafetyCommand;
    private readonly RelayCommand _toggleLiveViewCommand;
    private readonly AsyncRelayCommand _exportCommand;
    private readonly AsyncRelayCommand _restitchCommand;
    private readonly RelayCommand _showDashboardCommand;
    private readonly RelayCommand _showSetupCommand;
    private readonly RelayCommand _showCameraSettingsCommand;
    private readonly RelayCommand _showDiagnosticsCommand;
    private readonly AsyncRelayCommand _autoFocusCommand;
    private readonly RelayCommand _mfCoarseBackwardCommand;
    private readonly RelayCommand _mfCoarseForwardCommand;
    private readonly RelayCommand _mfFineBackwardCommand;
    private readonly RelayCommand _mfFineForwardCommand;
    private readonly RelayCommand _togglePeakingCommand;
    private readonly RelayCommand _switchLiveCameraToTargetDomainCommand;
    private readonly RelayCommand _showConsentCommand;
    private readonly RelayCommand _showBindingDemoCommand;
    private readonly RelayCommand _gridPreset3Command;
    private readonly RelayCommand _gridPreset4Command;
    private readonly RelayCommand _gridPreset5Command;
    private readonly RelayCommand _resetViewCommand;

    private CancellationToken _lifetimeToken;
    private bool _isBusy;
    // InitializeAsync が起動時のdurable journal読取に失敗したら true のまま保持する。
    // HardwareSingleCameraViewModel._stateLoadFailed と同型のラッチ（issue #142/PR #152
    // レビュー指摘）: _transactionService.InitializeAsync を一度も正常に読めていない以上、
    // PrepareNewCapture で見た目だけ Ready に戻すのはfail-closed原則に反するため、
    // 再起動して読取が成功するまでブロックし続ける（この場で再試行はしない）。
    private bool _initializationFailed;
    private bool _safetyAcknowledged;
    private bool _consentOverlayDismissed;
    private bool _physicalShutterAckAccepted;
    private bool _exclusiveUseAckAccepted;
    private bool _captureRecoveryOnlyOperatorApproved;
    private int _gridColumns = DefaultGridDivision;
    private int _gridRows = DefaultGridDivision;
    private string _noticeText = string.Empty;
    private string _noticeKind = "ok";
    private int _noticeGeneration;
    private bool _isResetArmed;
    private int _resetArmGeneration;
    private Guid? _lastNotifiedExportJobId;
    private bool _isPointerOverStage;
    private double _pointerX = 0.5;
    private double _pointerY = 0.5;
    private bool _isLiveViewActive;
    private string _selectedOperatingMode = DualModeLabel;
    private string _selectedCamera = "CAM-A";
    private string _selectedReadinessDemo = "自動補正範囲内";
    private string _selectedDiagnosticScenario = "正常完了";
    private string _selectedPage = "Dashboard";
    private string _selectedStageMode = StageModeCompositePreview;
    private string _selectedSimulatedFramePattern = SimulatedFramePatternCatalog.DefaultLabel;
    /// <summary>Normalized (0..1) position of the single shared target reticle, common to the
    /// stage and the loupe (issue #30). Defaults to the stage center.</summary>
    private double _targetX = 0.5;
    private double _targetY = 0.5;
    private string _selectedLoupeZoom = LoupeZoom100;
    private int _currentLiveViewFrameGeneration;
    /// <summary>Timestamps of canonical captured originals (persisted by a completed
    /// capture). Consulted by <see cref="StageCompositeFreshnessText"/> alongside
    /// <see cref="_lastLiveFrameTimestamps"/> — the more recent of the two wins.</summary>
    private readonly Dictionary<string, DateTimeOffset> _lastCapturedOriginalTimestamps = new(StringComparer.Ordinal);
    /// <summary>Timestamps of the last SIMULATED live view frame rendered per alias (not a
    /// capture) — separate from <see cref="_lastCapturedOriginalTimestamps"/> because the two
    /// have different real-world meaning even though both can drive the same freshness badge.</summary>
    private readonly Dictionary<string, DateTimeOffset> _lastLiveFrameTimestamps = new(StringComparer.Ordinal);
    private readonly Dictionary<string, SimulatedLiveViewFrame> _lastLiveFrames = new(StringComparer.Ordinal);
    /// <summary>Blur radius of the last live-ticked frame per alias (issue #31), read by AF
    /// execution to decide 合焦OK/NG. Populated alongside <see cref="_lastLiveFrames"/> in
    /// <see cref="ApplySimulatedFrameTick"/>; never populated for a non-live/frozen alias.</summary>
    private readonly Dictionary<string, double> _lastLiveFrameBlurRadius = new(StringComparer.Ordinal);
    /// <summary>Per-camera "focus fixed" state (issue #31's A:固定済/B:未固定 chips). Starts
    /// unfixed for both cameras; a successful AF execution fixes the camera it ran on, and any
    /// manual MF step un-fixes it again (a manual nudge invalidates the AF-then-fixed
    /// assumption). Read by <see cref="RebuildReadiness"/> to add a Caution notice — never a
    /// Blocker, per the "撮影のハードゲートにしない" requirement.</summary>
    private readonly Dictionary<string, bool> _focusFixed = new(StringComparer.Ordinal) { ["CAM-A"] = false, ["CAM-B"] = false };
    /// <summary>Per-camera MF position (issue #31), a relative dimensionless 0..100 value —
    /// never an absolute SDK focus position, per the "フォーカス位置の絶対値スライダーは提供
    /// しない" contract in docs/OPERATOR_UI_SPEC.md.</summary>
    private readonly Dictionary<string, double> _focusPositionValues = new(StringComparer.Ordinal);
    private bool _isAutoFocusRunning;
    /// <summary>True only while the issue #33 "撮影+AF" pre-capture AF gate is running (the
    /// stage between the button press and the point where the unchanged existing capture flow
    /// — <see cref="RunCaptureAsync"/> / <see cref="RunFormalDualCameraCaptureAsync"/> — is
    /// invoked). Folded into <see cref="CanCapture"/> so the primary capture button and this
    /// command cannot both run at once, mirroring the standard operation order's "直ちに...全
    /// 競合操作をロック" intent for the small window before <see cref="IsBusy"/> itself would
    /// otherwise be set by the downstream flow.</summary>
    private bool _isPreCaptureAutoFocusRunning;
    private bool _isPeakingEnabled;
    /// <summary>Issue #32 設置ガイドオーバーレイ toggle state. See the "Alignment guide overlays
    /// and tilt reading" region below for the derived visibility/text properties.</summary>
    private bool _isGridOverlayEnabled;
    private bool _isTombOverlayEnabled;
    private bool _isOverlapBandOverlayEnabled = true;
    private bool _isSafeMarginOverlayEnabled;
    /// <summary>issue #34 表示(V)メニューの「傾き読み値の表示切替」トグル。傾き常駐行は#32で
    /// 追加済みの既存表示のため、既定値はON（<see cref="IsOverlapBandOverlayEnabled"/>と同じ
    /// 理由で、新規トグルが既存表示を黙って隠さないようにする）。</summary>
    private bool _isTiltReadingVisible = true;
    private string _tiltToleranceInputText = string.Empty;
    private double? _tiltToleranceDegrees;
    private FocusExecutionResult? _lastFocusResult;
    private OperatorUiState _uiState = OperatorUiState.AwaitingSafetyAck;
    private string _statusMessage = "起動時の安全確認を行ってください。この画面は実機へ接続しません。";
    private string _technicalDetail = "error code: なし / log: ローカルsimulated journal";
    private string _lastTransactionId = "未実行";
    private string _captureResult = "未実行";
    private string _stitchResult = "未実行";
    private string _exportResult = "未実行";
    private string _retainedOriginals = "なし";
    private string _lastStitchJobId = "未実行";
    private string _lastExportPath = "未実行";
    private string _fixedLocalExportDirectory = string.Empty;
    private bool _cameraInspectionRequired;
    private int _transactionStartCount;
    private CaptureOutcome? _captureOutcome;
    private StitchOutcome? _stitchOutcome;
    private ExportOutcome? _exportOutcome;
    private ReadinessSnapshot _readiness = null!;
    private OperatorActionAvailability _availability = null!;

    public OperatorShellViewModel(ISimulatedTransactionService transactionService)
        : this(transactionService, null)
    {
    }

    public OperatorShellViewModel(
        ISimulatedTransactionService transactionService,
        IDualCameraProductFlow? dualCameraFlow,
        Func<DualCameraCaptureRequest>? hardwareDualRequestProvider = null,
        ISimulatedLiveViewFramePump? liveViewFramePump = null,
        ISimulatedLiveViewFrameSource? liveViewFrameSource = null,
        IHardwareCameraAgentTransport? dualBindingTransport = null,
        IHardwareDualCaptureRecoveryOnlyWorkflow? captureRecoveryOnlyWorkflow = null)
    {
        _transactionService = transactionService ?? throw new ArgumentNullException(nameof(transactionService));
        _dualCameraFlow = dualCameraFlow;
        _hardwareDualRequestProvider = hardwareDualRequestProvider;
        _captureRecoveryOnlyWorkflow = captureRecoveryOnlyWorkflow;
        _liveViewFramePump = liveViewFramePump;
        _liveViewFrameSource = liveViewFrameSource;
        if (_dualCameraFlow is not null)
        {
            _dualCameraFlow.StateChanged += OnDualCameraStateChanged;
            _dualCameraFlow.IdentityChanged += OnDualCameraIdentityChanged;
        }
        if (_liveViewFramePump is not null)
        {
            _liveViewFramePump.Tick += OnSimulatedFrameTick;
        }
        ProgressSteps =
        [
            new("liveview", "ライブ表示停止"),
            new("capture-a", "CAM-A撮影"),
            new("persist-a", "CAM-A原画像の確認"),
            new("capture-b", "CAM-B撮影"),
            new("persist-b", "CAM-B原画像の確認"),
            new("stitch", "合成"),
            new("review", "結果確認"),
            new("export", "保存"),
        ];

        // 機体照合（Dual session binding・ADR-0025）。HardwareDual では binding が Ready に
        // なるまで撮影を開始できない。production composition は、同じ子AgentのPIDに束縛した
        // transport を明示注入する。注入されないHardwareDualは固定パイプ名へは接続せず、
        // NoLauncherProcessIdで必ずfail-closedにするため、模擬bindingを実機合格にはしない。
        var isHardwareDual =
            _dualCameraFlow?.ExecutionEnvironment == DualCameraExecutionEnvironment.HardwareDual;
        DualBinding = new DualBindingViewModel(
            new DualBindingSessionClient(dualBindingTransport ?? (isHardwareDual
                ? new NamedPipeHardwareCameraAgentTransport(
                    DualBindingCameraAgentProtocol.DefaultPipeName,
                    NamedPipeServerIdentity.NoLauncherProcessId)
                : new SimulatedDualBindingAgentTransport(new SimulatedDualBindingAgent()))),
            isRequired: isHardwareDual);
        DualBinding.PropertyChanged += OnDualBindingChanged;
        _showBindingDemoCommand = new RelayCommand(
            () => DualBinding.IsRequired = true,
            () => !DualBinding.IsRequired && !IsBusy);

        // 同意は2項目の両方にチェックが入るまで押せない。読まずに流す操作を防ぐため。
        _acceptSafetyCommand = new RelayCommand(
            AcceptSafety,
            () => !SafetyAcknowledged && !IsBusy && IsPhysicalShutterAckAccepted && IsExclusiveUseAckAccepted);
        _declineSafetyCommand = new RelayCommand(DeclineSafety, () => !SafetyAcknowledged && !IsBusy);
        _showConsentCommand = new RelayCommand(ShowConsent, () => !SafetyAcknowledged && !IsBusy);
        _gridPreset3Command = new RelayCommand(() => ApplyGridPreset(3), () => !IsBusy);
        _gridPreset4Command = new RelayCommand(() => ApplyGridPreset(4), () => !IsBusy);
        _gridPreset5Command = new RelayCommand(() => ApplyGridPreset(5), () => !IsBusy);
        _resetViewCommand = new RelayCommand(RequestResetView, () => !IsBusy);
        _captureCommand = new AsyncRelayCommand(() => RunCaptureAsync("正常完了"), () => CanCapture, ShowUnexpectedFailure);
        _captureWithAutoFocusCommand = new AsyncRelayCommand(() => RunCaptureWithAutoFocusAsync("正常完了"), () => CanCaptureWithAutoFocus, ShowUnexpectedFailure);
        _diagnosticCommand = new AsyncRelayCommand(() => RunCaptureAsync(SelectedDiagnosticScenario), () => CanCapture, ShowUnexpectedFailure);
        _prepareNewCaptureCommand = new AsyncRelayCommand(PrepareNewCaptureAsync, () => CanPrepareNewCapture, ShowUnexpectedFailure);
        _toggleLiveViewCommand = new RelayCommand(ToggleLiveView, () => CanUseLiveView);
        _exportCommand = new AsyncRelayCommand(ExportAsync, () => CanExport, ShowUnexpectedFailure);
        _restitchCommand = new AsyncRelayCommand(RestitchAsync, () => CanRestitch, ShowUnexpectedFailure);
        _showDashboardCommand = new RelayCommand(() => SelectedPage = "Dashboard", () => CanOpenMaintenance);
        _showSetupCommand = new RelayCommand(() => SelectedPage = "Setup", () => CanOpenMaintenance);
        _showCameraSettingsCommand = new RelayCommand(() => SelectedPage = "CameraSettings", () => CanOpenMaintenance);
        _showDiagnosticsCommand = new RelayCommand(() => SelectedPage = "Diagnostics", () => CanOpenMaintenance);
        _autoFocusCommand = new AsyncRelayCommand(ExecuteAutoFocusAsync, () => CanExecuteAutoFocus, ShowUnexpectedFailure);
        _mfCoarseBackwardCommand = new RelayCommand(() => StepFocus(-MfCoarseStep), () => CanUseFocusPanel);
        _mfCoarseForwardCommand = new RelayCommand(() => StepFocus(MfCoarseStep), () => CanUseFocusPanel);
        _mfFineBackwardCommand = new RelayCommand(() => StepFocus(-MfFineStep), () => CanUseFocusPanel);
        _mfFineForwardCommand = new RelayCommand(() => StepFocus(MfFineStep), () => CanUseFocusPanel);
        _togglePeakingCommand = new RelayCommand(() => IsPeakingEnabled = !IsPeakingEnabled, () => IsFocusPanelAvailable);
        _switchLiveCameraToTargetDomainCommand = new RelayCommand(SwitchLiveCameraToTargetDomain, () => CanSwitchLiveCameraToTargetDomain);
        RebuildReadiness();
        ResetProgress();
    }

    public string BannerText => IsCaptureRecoveryOnlyMode
        ? HardwareDualCaptureRecoveryOnlyBanner
        : _dualCameraFlow?.ExecutionEnvironment == DualCameraExecutionEnvironment.HardwareDual
            ? HardwareDualPendingBanner
            : SimulationBanner;

    /// <summary>ヘルプ(H)メニューの「バージョン」項目用（issue #34）。実装時点でセマンティック
    /// バージョンの運用ルールは未確定のため、独自の番号を捏造せずビルド済みアセンブリのメタ
    /// データをそのまま表示する。</summary>
    public static string AppVersionText =>
        $"A0 Camera Stitcher M3 OperatorShell — v{Assembly.GetExecutingAssembly().GetName().Version}";
    public IReadOnlyList<string> OperatingModeOptions { get; } = [SingleModeLabel, DualModeLabel];
    public IReadOnlyList<string> CameraAliases { get; } = ["CAM-A", "CAM-B"];
    public IReadOnlyList<string> ReadinessDemoOptions { get; } = ["補正不要", "自動補正範囲内", "物理調整が必要", "CAM-A未接続", "CAM-B未接続", "カード状態要確認"];
    public IReadOnlyList<string> DiagnosticScenarios => IsCaptureRecoveryOnlyMode
        ? ["正常完了"]
        : IsSingleCameraMode
        ? ["正常完了", "Live View停止失敗", "対象カメラ撮影失敗", "cleanup失敗", "Live View再開失敗", "1台目保存後クラッシュ"]
        : ["正常完了", "Live View停止失敗", "CAM-A撮影失敗", "CAM-B撮影失敗", "cleanup失敗", "合成失敗", "Live View再開失敗", "CAM-A保存後クラッシュ"];
    public IReadOnlyList<CameraSettingRow> CameraSettingRows { get; } =
    [
        new("記録形式", "FX / JPEG Fine L", "SIMULATED: 整合", "SIMULATED: 整合"),
        new("露出", "Manual固定 / Auto ISOなし", "SIMULATED: 整合", "SIMULATED: 整合"),
        new("フォーカス", "Manual固定", "SIMULATED: 整合", "SIMULATED: 整合"),
        new("WB", "固定・左右一致", "SIMULATED: 整合", "SIMULATED: 整合"),
        new("VR", "OFF", "SIMULATED: 整合", "SIMULATED: 整合"),
    ];
    public ObservableCollection<ProgressStepViewModel> ProgressSteps { get; }

    /// <summary>Issue #33 アクションゾーン state 2 (自動進捗ストリップ) の watchdog表示。この
    /// シェルのSIMULATED経路（<see cref="ISimulatedTransactionService"/>直結、または
    /// TestSynthetic経由の<see cref="IDualCameraProductFlow"/>）はwatchdog残り秒の実測値を一切
    /// 保持しない — 180秒dispatch watchdogはHardwareDual専用の契約（
    /// <c>src/m3/Foundation/DualCamera/DualHardwareCapture.cs</c>）であり、VMへ公開されていない。
    /// 存在しない値をカウントダウン風に捏造しないため、実データがない今は契約値の静的表示に
    /// 留める。1台構成にはwatchdog契約自体が存在しないため対象外と明記する。</summary>
    public string ProgressWatchdogText => IsSingleCameraMode
        ? "制限時間: 1台構成では なし"
        : "制限時間 180秒（残り秒の実データが未接続のため、数値は固定表示でカウントダウンしません）";

    public ICommand AcceptSafetyCommand => _acceptSafetyCommand;
    public ICommand ShowConsentCommand => _showConsentCommand;

    /// <summary>
    /// 機体照合（Dual session binding）の状態。HardwareDual では Ready になるまで撮影を
    /// 開始できない唯一のゲートで、Single へのフォールバックは無い。
    /// </summary>
    public DualBindingViewModel DualBinding { get; }

    /// <summary>
    /// 模擬動作で機体照合の流れを確認するための表示切替。実機の合格判定ではない。
    /// </summary>
    public ICommand ShowBindingDemoCommand => _showBindingDemoCommand;
    public ICommand GridPreset3Command => _gridPreset3Command;
    public ICommand GridPreset4Command => _gridPreset4Command;
    public ICommand GridPreset5Command => _gridPreset5Command;
    public ICommand ResetViewCommand => _resetViewCommand;
    public ICommand DeclineSafetyCommand => _declineSafetyCommand;
    public ICommand CaptureCommand => _captureCommand;
    public ICommand CaptureWithAutoFocusCommand => _captureWithAutoFocusCommand;
    public ICommand DiagnosticCommand => _diagnosticCommand;
    public ICommand PrepareNewCaptureCommand => _prepareNewCaptureCommand;
    public ICommand ToggleLiveViewCommand => _toggleLiveViewCommand;
    public ICommand ExportCommand => _exportCommand;
    public ICommand RestitchCommand => _restitchCommand;
    public ICommand ShowDashboardCommand => _showDashboardCommand;
    public ICommand ShowSetupCommand => _showSetupCommand;
    public ICommand ShowCameraSettingsCommand => _showCameraSettingsCommand;
    public ICommand ShowDiagnosticsCommand => _showDiagnosticsCommand;
    public ICommand AutoFocusCommand => _autoFocusCommand;
    public ICommand MfCoarseBackwardCommand => _mfCoarseBackwardCommand;
    public ICommand MfCoarseForwardCommand => _mfCoarseForwardCommand;
    public ICommand MfFineBackwardCommand => _mfFineBackwardCommand;
    public ICommand MfFineForwardCommand => _mfFineForwardCommand;
    public ICommand TogglePeakingCommand => _togglePeakingCommand;
    public ICommand SwitchLiveCameraToTargetDomainCommand => _switchLiveCameraToTargetDomainCommand;

    public bool IsBusy
    {
        get => _isBusy;
        private set
        {
            if (SetProperty(ref _isBusy, value))
            {
                var preserveOutcomeState = UiState is OperatorUiState.Capturing or OperatorUiState.Stitching or OperatorUiState.Review or OperatorUiState.FailedPartial or OperatorUiState.Degraded;
                RebuildReadiness(preserveOutcomeState);
                NotifyAllCommands();
                OnPropertyChanged(nameof(ActivityText));
                OnPropertyChanged(nameof(CanChangeOperatingMode));
                OnPropertyChanged(nameof(CanSelectCamera));
                OnPropertyChanged(nameof(CanChangeExportDirectory));
                OnPropertyChanged(nameof(CanChangeSimulatedFramePattern));
                RaiseFocusPanelProperties();
                if (IsBusy)
                {
                    // Re-announce the current value so a pattern change attempted while busy
                    // (rejected by the SelectedSimulatedFramePattern setter) doesn't leave the
                    // ComboBox showing a value the ViewModel never actually accepted.
                    OnPropertyChanged(nameof(SelectedSimulatedFramePattern));
                }
            }
        }
    }

    public bool SafetyAcknowledged
    {
        get => _safetyAcknowledged;
        private set
        {
            if (SetProperty(ref _safetyAcknowledged, value))
            {
                OnPropertyChanged(nameof(SafetyAckText));
                OnPropertyChanged(nameof(IsConsentOverlayVisible));
                OnPropertyChanged(nameof(IsSafetyAckPending));
                RebuildReadiness(preserveOutcomeState: UiState == OperatorUiState.FailedPartial);
            }
        }
    }

    public string SafetyAckText => SafetyAcknowledged ? "同意済み（アプリ終了時に破棄）" : "未同意 — 撮影禁止";

    /// <summary>起動セッションの排他同意モーダルの表示可否。未同意の間だけ前面に出す。
    /// 「同意しない」を選んだ場合は閲覧できるよう畳み、タイトルバーの再開ボタンから開き直す。</summary>
    public bool IsConsentOverlayVisible => !SafetyAcknowledged && !_consentOverlayDismissed;

    /// <summary>未同意であることをタイトルバーへ常時示すためのフラグ。</summary>
    public bool IsSafetyAckPending => !SafetyAcknowledged;

    /// <summary>同意チェック1: 撮影シーケンス中に物理シャッターへ触れないこと。</summary>
    public bool IsPhysicalShutterAckAccepted
    {
        get => _physicalShutterAckAccepted;
        set
        {
            if (SetProperty(ref _physicalShutterAckAccepted, value))
            {
                NotifyAllCommands();
            }
        }
    }

    /// <summary>同意チェック2: 他のカメラ撮影ソフトでカメラを占有しないこと。</summary>
    public bool IsExclusiveUseAckAccepted
    {
        get => _exclusiveUseAckAccepted;
        set
        {
            if (SetProperty(ref _exclusiveUseAckAccepted, value))
            {
                NotifyAllCommands();
            }
        }
    }

    /// <summary>
    /// Dedicated acceptance mode. Its output is two verified originals only;
    /// ordinary rig/stitch behavior is deliberately not selected by this flag.
    /// </summary>
    public bool IsCaptureRecoveryOnlyMode => _captureRecoveryOnlyWorkflow is not null;

    /// <summary>
    /// One explicit operator confirmation covering the two facts that cannot be
    /// inferred by this WPF layer: both dedicated cards were checked empty and
    /// this run is approved as capture/recovery-only (not an A0-quality test).
    /// </summary>
    public bool IsCaptureRecoveryOnlyOperatorApproved
    {
        get => _captureRecoveryOnlyOperatorApproved;
        set
        {
            if (!IsCaptureRecoveryOnlyMode || IsBusy ||
                !SetProperty(ref _captureRecoveryOnlyOperatorApproved, value))
            {
                return;
            }

            OnPropertyChanged(nameof(CanCapture));
            OnPropertyChanged(nameof(CaptureAvailabilityText));
            OnPropertyChanged(nameof(CaptureDisabledReason));
            NotifyAllCommands();
        }
    }

    public string ActivityText => IsBusy ? "操作をロック中" : "操作受付中";
    public bool IsSingleCameraMode => SelectedOperatingMode == SingleModeLabel;
    public bool CanChangeOperatingMode => !IsCaptureRecoveryOnlyMode && !IsBusy && !IsLiveViewActive && !_isPreCaptureAutoFocusRunning &&
        UiState is OperatorUiState.AwaitingSafetyAck or OperatorUiState.CheckingReadiness or OperatorUiState.NotReady or OperatorUiState.Ready or OperatorUiState.ReadyWithCorrection;
    public bool CanSelectCamera => !IsBusy && !IsLiveViewActive && !_isPreCaptureAutoFocusRunning &&
        UiState is not (OperatorUiState.Capturing or OperatorUiState.Stitching or OperatorUiState.Review or OperatorUiState.FailedPartial or OperatorUiState.Degraded);
    public bool CanChangeExportDirectory => !IsBusy;
    public string OperatingModeDescription => IsCaptureRecoveryOnlyMode
        ? "CAM-A→CAM-Bを順次撮影し、検証済み原画像2枚だけを固定ローカルへ保持します。合成とA0品質判定は行いません。"
        : IsSingleCameraMode
        ? $"{SelectedCamera}だけを撮影し、合成せず検証済み単体原画像を保存します。他方のD810は接続しません。"
        : "CAM-A→CAM-Bを順次撮影し、両原画像を合成します。一台欠けても自動で一台構成へ変更しません。";
    public string CaptureButtonText => IsCaptureRecoveryOnlyMode
        ? "2台を順次撮影・回収する（1回）"
        : IsSingleCameraMode ? $"{SelectedCamera}を撮影する（確認なし）" : "2台を順次撮影する（確認なし）";
    public string CameraSelectionLabel => IsSingleCameraMode ? "撮影・ライブ表示の対象" : "ライブ表示するカメラ（1台ずつ）";
    public string ProcessingResultLabel => IsCaptureRecoveryOnlyMode
        ? "原画像2枚（合成保留）"
        : IsSingleCameraMode ? "単体出力" : "合成";
    public string OverallStateText => UiState switch
    {
        OperatorUiState.Ready => "撮影可能",
        OperatorUiState.ReadyWithCorrection => "補正予定・撮影可能",
        OperatorUiState.Capturing => "撮影処理中",
        OperatorUiState.Stitching => "合成処理中",
        OperatorUiState.Review => "結果確認",
        OperatorUiState.FailedPartial => "撮影失敗（再開不可）",
        OperatorUiState.Degraded => "警告確認",
        OperatorUiState.CheckingReadiness => "状態確認中",
        _ => "撮影不可",
    };

    public OperatorUiState UiState
    {
        get => _uiState;
        private set
        {
            if (SetProperty(ref _uiState, value))
            {
                OnPropertyChanged(nameof(OverallStateText));
                OnPropertyChanged(nameof(CanChangeOperatingMode));
                OnPropertyChanged(nameof(CanSelectCamera));
                OnPropertyChanged(nameof(CanChangeStageMode));
                OnPropertyChanged(nameof(IsStageProcessingPlaceholder));
                OnPropertyChanged(nameof(IsGridOverlayVisible));
                OnPropertyChanged(nameof(IsTombOverlayVisible));
                OnPropertyChanged(nameof(IsSafeMarginOverlayVisible));
                OnPropertyChanged(nameof(IsStageReviewMode));
                OnPropertyChanged(nameof(IsStageLiveNoteVisible));
                OnPropertyChanged(nameof(IsStageSingleLiveMode));
                OnPropertyChanged(nameof(IsStageCompositePreviewMode));
                OnPropertyChanged(nameof(StageReviewBadgeText));
                OnPropertyChanged(nameof(ReadyStatusChipText));
                OnPropertyChanged(nameof(IsActionZonePreparing));
                OnPropertyChanged(nameof(IsActionZoneProcessing));
                OnPropertyChanged(nameof(IsActionZoneReview));
                RaiseStageFrameProperties();
                RaiseFocusPanelProperties();
                RecalculateAvailability();
            }
        }
    }

    /// <summary>Issue #33 アクションゾーン state 1 (設置判定カード＋撮影ボタン2種): every
    /// pre-capture state, including the two states that precede readiness evaluation itself
    /// (<see cref="OperatorUiState.AwaitingSafetyAck"/>/<see cref="OperatorUiState.CheckingReadiness"/>).
    /// The issue's own state table only enumerates NotReady/Ready/ReadyWithCorrection for this
    /// slot, but the action zone must show *something* in every <see cref="OperatorUiState"/>
    /// value — folding these two earliest states in here (rather than leaving a fourth, unlisted
    /// gap) matches their existing display today: a disabled capture button with
    /// <see cref="CaptureDisabledReason"/> explaining why (未同意, checking, etc.), which is
    /// exactly this state's shape. ※要確認: Designer should confirm this reading if a future spec
    /// revision wants a distinct fourth "起動中" treatment instead.</summary>
    public bool IsActionZonePreparing =>
        UiState is OperatorUiState.AwaitingSafetyAck or OperatorUiState.CheckingReadiness or
            OperatorUiState.NotReady or OperatorUiState.Ready or OperatorUiState.ReadyWithCorrection;

    /// <summary>Issue #33 アクションゾーン state 2 (自動進捗ストリップ)。</summary>
    public bool IsActionZoneProcessing => UiState is OperatorUiState.Capturing or OperatorUiState.Stitching;

    /// <summary>Issue #33 アクションゾーン state 3 (結果パネル)。</summary>
    public bool IsActionZoneReview =>
        UiState is OperatorUiState.Review or OperatorUiState.FailedPartial or OperatorUiState.Degraded;

    public string SelectedOperatingMode
    {
        get => _selectedOperatingMode;
        set
        {
            if (!CanChangeOperatingMode || value is not (SingleModeLabel or DualModeLabel) ||
                !SetProperty(ref _selectedOperatingMode, value))
            {
                return;
            }

            SelectedDiagnosticScenario = "正常完了";
            OnPropertyChanged(nameof(IsSingleCameraMode));
            OnPropertyChanged(nameof(IsSingleCameraModeChecked));
            OnPropertyChanged(nameof(IsDualCameraModeChecked));
            OnPropertyChanged(nameof(DualCameraIdentityStatusText));
            OnPropertyChanged(nameof(OperatingModeDescription));
            OnPropertyChanged(nameof(CaptureButtonText));
            OnPropertyChanged(nameof(CameraSelectionLabel));
            OnPropertyChanged(nameof(ProcessingResultLabel));
            OnPropertyChanged(nameof(DiagnosticScenarios));
            OnPropertyChanged(nameof(StageCompositeApplicable));
            OnPropertyChanged(nameof(ShowAutomaticStitchInstruction));
            OnPropertyChanged(nameof(IsOverlapBandVisible));
            OnPropertyChanged(nameof(StageReviewBadgeText));
            OnPropertyChanged(nameof(StageSingleLiveAliasInPlan));
            OnPropertyChanged(nameof(StageSingleLiveText));
            RaiseLoupeProperties();
            RaiseFocusPanelProperties();
            ResetProgress(CurrentCapturePlan);
            RebuildReadiness();
        }
    }

    /// <summary>カメラ(C)メニューの「運用構成」サブメニュー（issue #34）を、<see cref="SelectedOperatingMode"/>
    /// と同じ一意選択の2つのチェック可能<see cref="System.Windows.Controls.MenuItem"/>として
    /// バインドするための補助プロパティ。<see cref="System.Windows.Controls.MenuItem"/>には
    /// <see cref="System.Windows.Controls.RadioButton.GroupName"/>のような相互排他の仕組みが
    /// ないため、排他はこのプロパティのペア自体が担う: 読み取りは<see cref="IsSingleCameraMode"/>
    /// をそのまま反映し、<c>true</c>への書き込みだけが<see cref="SelectedOperatingMode"/>を
    /// 切り替える（もう一方は連動して自動的にfalseへ通知される）。ユーザーが選択中の項目を
    /// 直接<c>false</c>へ外そうとした場合は無視し、WPFのローカル表示だけが先行して不一致に
    /// ならないよう現在値を再通知して戻す。</summary>
    public bool IsSingleCameraModeChecked
    {
        get => IsSingleCameraMode;
        set
        {
            if (value)
            {
                SelectedOperatingMode = SingleModeLabel;
            }
            else
            {
                OnPropertyChanged(nameof(IsSingleCameraModeChecked));
            }
        }
    }

    /// <summary><see cref="IsSingleCameraModeChecked"/>の対（DualCamera側）。</summary>
    public bool IsDualCameraModeChecked
    {
        get => !IsSingleCameraMode;
        set
        {
            if (value)
            {
                SelectedOperatingMode = DualModeLabel;
            }
            else
            {
                OnPropertyChanged(nameof(IsDualCameraModeChecked));
            }
        }
    }

    public string SelectedCamera
    {
        get => _selectedCamera;
        set
        {
            if (CanSelectCamera && value is ("CAM-A" or "CAM-B") && SetProperty(ref _selectedCamera, value))
            {
                OnPropertyChanged(nameof(LiveViewPlaceholder));
                OnPropertyChanged(nameof(LiveViewButtonText));
                OnPropertyChanged(nameof(OperatingModeDescription));
                OnPropertyChanged(nameof(CaptureButtonText));
                OnPropertyChanged(nameof(StageCompositeLiveAlias));
                OnPropertyChanged(nameof(StageCompositeStillAlias));
                OnPropertyChanged(nameof(StageSingleLiveAliasInPlan));
                OnPropertyChanged(nameof(StageSingleLiveText));
                RaiseStageFrameProperties();
                RaiseFocusPanelProperties();
                OnPropertyChanged(nameof(CameraAFocusStatusText));
                OnPropertyChanged(nameof(CameraBFocusStatusText));
                OnPropertyChanged(nameof(FocusResultText));
                if (IsSingleCameraMode)
                {
                    SelectedDiagnosticScenario = "正常完了";
                    ResetProgress(CurrentCapturePlan);
                    RebuildReadiness();
                }
            }
        }
    }

    public string SelectedReadinessDemo
    {
        get => _selectedReadinessDemo;
        set
        {
            if (!IsBusy && SetProperty(ref _selectedReadinessDemo, value))
            {
                RebuildReadiness();
            }
        }
    }

    public string SelectedDiagnosticScenario
    {
        get => _selectedDiagnosticScenario;
        set
        {
            if (!IsBusy && DiagnosticScenarios.Contains(value, StringComparer.Ordinal))
            {
                SetProperty(ref _selectedDiagnosticScenario, value);
            }
        }
    }
    public string SelectedPage { get => _selectedPage; private set { if (SetProperty(ref _selectedPage, value)) OnPropertyChanged(nameof(PageTitle)); } }
    public string PageTitle => SelectedPage switch { "Setup" => "設置・校正", "CameraSettings" => "カメラ設定（read-only）", "Diagnostics" => "保存・診断", _ => "撮影ダッシュボード" };
    public string LiveViewPlaceholder => $"{SelectedCamera}\n\n模擬動作のライブ表示（実画像ではありません）\n原画像・合成入力には使いません";
    public string LiveViewButtonText => IsLiveViewActive ? $"{SelectedCamera} ライブ表示を停止" : $"{SelectedCamera} ライブ表示を開始";
    public bool IsLiveViewActive
    {
        get => _isLiveViewActive;
        private set
        {
            if (SetProperty(ref _isLiveViewActive, value))
            {
                // The pump is the sole SIMULATED frame supply gate: it only runs between
                // Start/Stop, so no frame is ever produced while Live View is OFF. The
                // generation Start() returns is remembered so a tick from a since-stopped
                // session (including OFF then back ON for the same alias) can be told apart
                // from one belonging to the session that is current right now.
                if (value)
                {
                    _currentLiveViewFrameGeneration = _liveViewFramePump?.Start(SelectedCamera, CurrentSimulatedFramePattern) ?? 0;
                }
                else
                {
                    _liveViewFramePump?.Stop();
                    _currentLiveViewFrameGeneration = 0;
                }
                OnPropertyChanged(nameof(LiveViewButtonText));
                OnPropertyChanged(nameof(CanChangeOperatingMode));
                OnPropertyChanged(nameof(CanSelectCamera));
                RaiseStageFrameProperties();
                RaiseFocusPanelProperties();
                RebuildReadiness();
            }
        }
    }
    public IReadOnlyList<string> StageModeOptions { get; } = [StageModeCameraALive, StageModeCameraBLive, StageModeCompositePreview];

    public string SelectedStageMode
    {
        get => _selectedStageMode;
        set
        {
            if (value is not (StageModeCameraALive or StageModeCameraBLive or StageModeCompositePreview) ||
                !SetProperty(ref _selectedStageMode, value))
            {
                return;
            }

            OnPropertyChanged(nameof(IsStageSingleLiveMode));
            OnPropertyChanged(nameof(IsStageCompositePreviewMode));
            OnPropertyChanged(nameof(StageSingleLiveAlias));
            OnPropertyChanged(nameof(StageSingleLiveAliasInPlan));
            OnPropertyChanged(nameof(StageSingleLiveText));
            RaiseStageFrameProperties();
        }
    }

    public IReadOnlyList<string> SimulatedFramePatternOptions { get; } = SimulatedFramePatternCatalog.Labels;

    public bool IsSimulatedFrameSourceAvailable => _liveViewFramePump is not null && _liveViewFrameSource is not null;

    public bool CanChangeSimulatedFramePattern => !IsBusy;

    public string SelectedSimulatedFramePattern
    {
        get => _selectedSimulatedFramePattern;
        set
        {
            if (CanChangeSimulatedFramePattern && SimulatedFramePatternCatalog.TryGetPattern(value, out var pattern))
            {
                if (SetProperty(ref _selectedSimulatedFramePattern, value))
                {
                    _liveViewFramePump?.SetPattern(pattern);
                }
                return;
            }

            // Rejected (busy, or a value that doesn't map to a known pattern somehow reached
            // the binding): re-announce the current value so the ComboBox snaps back to it
            // instead of silently displaying a selection the ViewModel never accepted.
            OnPropertyChanged(nameof(SelectedSimulatedFramePattern));
        }
    }

    private SimulatedFramePattern CurrentSimulatedFramePattern =>
        SimulatedFramePatternCatalog.TryGetPattern(_selectedSimulatedFramePattern, out var pattern)
            ? pattern
            : SimulatedFramePatternCatalog.DefaultPattern;

    public bool CanChangeStageMode => UiState is not (OperatorUiState.Capturing or OperatorUiState.Stitching or OperatorUiState.Review);

    /// <summary>A / B キーからのステージ表示切替。表示モードを変えるだけで、
    /// 撮影対象カメラ（<see cref="SelectedCamera"/>）やライブ表示の開閉には触らない。
    /// 切替できない状態のときは黙って無視する（キーで状態ゲートを迂回させない）。</summary>
    public void SelectStageCamera(string alias)
    {
        if (!CanChangeStageMode)
        {
            return;
        }

        if (alias == "CAM-B" && IsSingleCameraMode)
        {
            return;
        }

        SelectedStageMode = alias == "CAM-B" ? StageModeCameraBLive : StageModeCameraALive;
    }
    public bool IsStageProcessingPlaceholder => UiState is OperatorUiState.Capturing or OperatorUiState.Stitching;
    public bool IsStageReviewMode => UiState == OperatorUiState.Review;
    public bool IsStageLiveNoteVisible => !IsStageProcessingPlaceholder && !IsStageReviewMode;
    public bool IsStageSingleLiveMode => IsStageLiveNoteVisible && SelectedStageMode != StageModeCompositePreview;
    public bool IsStageCompositePreviewMode => IsStageLiveNoteVisible && SelectedStageMode == StageModeCompositePreview;
    public string StageProcessingPlaceholderText => StageProcessingPlaceholderMessage;
    public string StagePreviewNoteText => StagePreviewNoteMessage;
    public string StageReviewBadgeText => IsCaptureRecoveryOnlyMode
        ? "検証済み原画像（合成保留）"
        : IsSingleCameraMode ? "検証済み原本" : "合成結果";
    public string CaptureAvailabilityText => CanCapture ? "撮影可" : "撮影不可";
    public string ReadyStatusChipText => $"{OverallStateText} / {CaptureAvailabilityText}";

    public string StageSingleLiveAlias => SelectedStageMode == StageModeCameraBLive ? "CAM-B" : "CAM-A";
    public bool StageSingleLiveAliasInPlan =>
        CurrentCapturePlan.RequiredCameraAliases.Contains(StageSingleLiveAlias, StringComparer.Ordinal);
    public string StageSingleLiveText => StageSingleLiveAliasInPlan
        ? $"{StageSingleLiveAlias}\n\n全画面の模擬ライブ表示（実画像ではありません）\n{StagePreviewNoteMessage}"
        : $"{StageSingleLiveAlias}\n\n1台構成のため対象外（運用対象は{SelectedCamera}のみ）";

    public bool StageCompositeApplicable => !IsSingleCameraMode;
    public bool ShowAutomaticStitchInstruction => StageCompositeApplicable && !IsCaptureRecoveryOnlyMode;
    public bool ShowCaptureRecoveryOnlyInstruction => IsCaptureRecoveryOnlyMode;
    public string StageCompositeLiveAlias => SelectedCamera;
    public string StageCompositeStillAlias => SelectedCamera == "CAM-A" ? "CAM-B" : "CAM-A";
    public string StageCompositeOverlapBandText => "重複帯\n幅px実測未接続";
    public string StageCompositeFreshnessText => FreshnessText(StageCompositeStillAlias);

    /// <summary>Either a completed capture or a SIMULATED live view frame can be the more
    /// recent "last known state" of <paramref name="alias"/>; whichever is newer drives the
    /// freshness badge (a live frame taken after the last capture is more current, and vice
    /// versa). Shared by the stage's composite "still" badge and the loupe badge so the two
    /// never drift apart.</summary>
    private string FreshnessText(string alias)
    {
        var hasCapturedOriginal = _lastCapturedOriginalTimestamps.TryGetValue(alias, out var capturedAt);
        var hasLiveFrame = _lastLiveFrameTimestamps.TryGetValue(alias, out var liveFrameAt);
        if (!hasCapturedOriginal && !hasLiveFrame)
        {
            return "静止画 未取得";
        }

        var mostRecent = hasCapturedOriginal && (!hasLiveFrame || capturedAt >= liveFrameAt) ? capturedAt : liveFrameAt;
        var elapsedSeconds = Math.Max(0, (int)(DateTimeOffset.UtcNow - mostRecent).TotalSeconds);
        return $"静止画 {elapsedSeconds}秒前";
    }

    // --- Target reticle (□) and loupe (拡大エリア): issue #30. The reticle is a single
    // shared position, common to the stage and the loupe; the loupe crops the same
    // SIMULATED frame dictionary the stage already reads from (_lastLiveFrames), so it
    // never needs its own frame supply or preview-image contract. ---

    public double TargetX { get => _targetX; private set => SetProperty(ref _targetX, value); }
    public double TargetY { get => _targetY; private set => SetProperty(ref _targetY, value); }

    /// <summary>True once the reticle is left-of-center. In composite preview (and Review,
    /// which shows the same left/right split frozen), the stage lays the live camera out on
    /// the left and the still camera on the right — see MainWindow.xaml's Grid.Column 0/2 —
    /// so this is the same left/right split used to decide which camera's frame the loupe
    /// should crop. It is a simplified proxy for "which camera's physical capture area the
    /// target sits in": the real A0 layout geometry (rotation, measured overlap) does not
    /// exist yet (still a placeholder per StageCompositeOverlapBandText), so an exact
    /// document-space boundary is not available to route on. ※要確認: Architect should confirm
    /// this half-split proxy is acceptable until #29/#32 geometry lands.</summary>
    public bool IsTargetOnLiveSide => TargetX < 0.5;

    public bool CanAdjustTarget => !IsStageProcessingPlaceholder;
    public bool IsTargetOverlayVisible => !IsStageProcessingPlaceholder;

    /// <summary>拡大エリアが切り出す中心。ステージにポインタが載っている間はその位置を、
    /// 離れている間は共通ターゲット□の位置を使う。見たい場所へポインタを運べばそこが
    /// 拡大されるという、原稿を覗き込む動作に一番近い形にするため。</summary>
    public double LoupeFocusX => _isPointerOverStage ? _pointerX : TargetX;

    public double LoupeFocusY => _isPointerOverStage ? _pointerY : TargetY;

    /// <summary>拡大エリアを出すかどうか。ライブ表示中にポインタがステージ上にある間だけ。
    /// 常時出しっぱなしにすると、見たい場所をルーペ自身が隠す。</summary>
    public bool IsLoupeVisible => IsStageLiveNoteVisible && _isPointerOverStage;

    /// <summary>ステージ上のポインタ位置を受け取る。位置そのものは撮影の可否や
    /// ターゲット位置には影響しない（表示の中心が動くだけ）。</summary>
    public void UpdatePointerPosition(double x, double y)
    {
        var clampedX = Math.Clamp(x, 0.0, 1.0);
        var clampedY = Math.Clamp(y, 0.0, 1.0);
        if (_isPointerOverStage && Math.Abs(_pointerX - clampedX) < PointerEpsilon && Math.Abs(_pointerY - clampedY) < PointerEpsilon)
        {
            return;
        }

        _pointerX = clampedX;
        _pointerY = clampedY;
        _isPointerOverStage = true;
        RaiseLoupeProperties();
    }

    /// <summary>ポインタがステージから外れた。拡大エリアを畳み、切り出し中心を
    /// ターゲット□へ戻す。</summary>
    public void ClearPointerPosition()
    {
        if (!_isPointerOverStage)
        {
            return;
        }

        _isPointerOverStage = false;
        RaiseLoupeProperties();
    }

    /// <summary>Moves the target in response to a stage-area drag: a direct (coarse) move,
    /// expressed as a delta already normalized to the drag surface's own size (0..1, same
    /// space as <see cref="TargetX"/>/<see cref="TargetY"/>). The code-behind mouse handler
    /// computes this normalization; this method only owns the resulting state change so it
    /// stays testable without a live WPF visual tree.</summary>
    public void MoveTargetByStageDrag(double normalizedDeltaX, double normalizedDeltaY) =>
        SetTargetPosition(_targetX + normalizedDeltaX, _targetY + normalizedDeltaY);

    /// <summary>Moves the target in response to a loupe-area drag: the same normalized delta
    /// as <see cref="MoveTargetByStageDrag"/>, but scaled by <see cref="TargetFineDragScale"/>
    /// so the same physical drag distance produces a finer position change — the issue #30
    /// "ルーペ表示内のドラッグ = 細かい移動" contract.</summary>
    public void MoveTargetByLoupeDrag(double normalizedDeltaX, double normalizedDeltaY) =>
        SetTargetPosition(_targetX + (normalizedDeltaX * TargetFineDragScale), _targetY + (normalizedDeltaY * TargetFineDragScale));

    /// <summary>Sets the target's normalized position directly, clamped to the 0..1 stage
    /// bounds. Public (not just reachable via the drag deltas) so tests can place the target
    /// exactly without simulating a drag gesture.</summary>
    public void SetTargetPosition(double x, double y)
    {
        var changedX = SetProperty(ref _targetX, Math.Clamp(x, 0.0, 1.0), nameof(TargetX));
        var changedY = SetProperty(ref _targetY, Math.Clamp(y, 0.0, 1.0), nameof(TargetY));
        if (changedX || changedY)
        {
            RaiseLoupeProperties();
            RaiseFocusPanelProperties();
        }
    }

    public IReadOnlyList<string> LoupeZoomOptions { get; } = [LoupeZoom100, LoupeZoom200];

    public string SelectedLoupeZoom
    {
        get => _selectedLoupeZoom;
        set
        {
            if (value is (LoupeZoom100 or LoupeZoom200) && SetProperty(ref _selectedLoupeZoom, value))
            {
                RaiseLoupeProperties();
                return;
            }

            if (value is not (LoupeZoom100 or LoupeZoom200))
            {
                // Re-announce the current value so a bound ComboBox reverts instead of keeping
                // an unrecognized selection on screen (same rejection pattern as
                // SelectedSimulatedFramePattern).
                OnPropertyChanged(nameof(SelectedLoupeZoom));
            }
        }
    }

    private double LoupeZoomFactor => SelectedLoupeZoom == LoupeZoom200 ? 2.0 : 1.0;

    /// <summary>表示(V)メニューの「拡大エリア倍率」サブメニュー（issue #34）を、
    /// <see cref="SelectedLoupeZoom"/>と同じ一意選択の2つのチェック可能MenuItemペアとして
    /// バインドするための補助プロパティ。<see cref="IsSingleCameraModeChecked"/>と同じ理由・
    /// 同じ再通知パターンを使う。</summary>
    public bool IsLoupeZoom100Checked
    {
        get => SelectedLoupeZoom == LoupeZoom100;
        set
        {
            if (value)
            {
                SelectedLoupeZoom = LoupeZoom100;
            }
            else
            {
                OnPropertyChanged(nameof(IsLoupeZoom100Checked));
            }
        }
    }

    /// <summary><see cref="IsLoupeZoom100Checked"/>の対（200%側）。</summary>
    public bool IsLoupeZoom200Checked
    {
        get => SelectedLoupeZoom == LoupeZoom200;
        set
        {
            if (value)
            {
                SelectedLoupeZoom = LoupeZoom200;
            }
            else
            {
                OnPropertyChanged(nameof(IsLoupeZoom200Checked));
            }
        }
    }

    /// <summary>Which camera alias the loupe currently crops. Mirrors the stage's own mode
    /// gating: a single-camera live stage mode (CAM-A live / CAM-B live) always shows that one
    /// camera; composite preview and Review split left/right by <see cref="IsTargetOnLiveSide"/>,
    /// matching the alias the stage itself renders on that side (<see cref="StageCompositeLiveAlias"/>
    /// / <see cref="StageCompositeStillAlias"/>).</summary>
    public string LoupeCameraAlias =>
        IsSingleCameraMode
            ? SelectedCamera
            : IsStageCompositePreviewMode || IsStageReviewMode
                ? (LoupeFocusX < 0.5 ? StageCompositeLiveAlias : StageCompositeStillAlias)
                : StageSingleLiveAlias;

    /// <summary>True only when the loupe's current alias is actually streaming right now
    /// (Live View on, not paused for Review/processing, and it is the alias Live View is
    /// bound to). False covers every "frozen" case: non-live composite side, Review (Live
    /// View is always stopped by the time Review is reached), and the processing placeholder.</summary>
    public bool IsLoupeSourceLive =>
        !IsStageProcessingPlaceholder && !IsStageReviewMode && IsLiveViewActive &&
        string.Equals(LoupeCameraAlias, SelectedCamera, StringComparison.Ordinal);

    /// <summary>The full (uncropped) SIMULATED frame the loupe crops from — the same
    /// preview-only frame dictionary the stage reads, looked up for <see cref="LoupeCameraAlias"/>
    /// instead of the stage's own alias. Null whenever the stage itself would show a
    /// placeholder for that alias (no frame ever supplied while not live, and not retained
    /// across a "新しい撮影を準備" reset).</summary>
    private BitmapSource? LoupeBaseImage =>
        _lastLiveFrames.TryGetValue(LoupeCameraAlias, out var frame) ? frame.Image : null;

    /// <summary>The crop rectangle (in <see cref="LoupeBaseImage"/> pixel space) centered on
    /// the target, sized by <see cref="LoupeBaseCropFraction"/> / <see cref="LoupeZoomFactor"/>
    /// and clamped so it always stays fully inside the source frame — including degenerate
    /// tiny frames (down to 1x1, as used by headless tests), which is why this clamps the crop
    /// size itself rather than assuming the frame is at least crop-sized.</summary>
    private Int32Rect? LoupeCropRect
    {
        get
        {
            var image = LoupeBaseImage;
            if (image is null || image.PixelWidth <= 0 || image.PixelHeight <= 0)
            {
                return null;
            }

            var fraction = LoupeBaseCropFraction / LoupeZoomFactor;
            var cropWidth = Math.Clamp((int)Math.Round(image.PixelWidth * fraction), 1, image.PixelWidth);
            var cropHeight = Math.Clamp((int)Math.Round(image.PixelHeight * fraction), 1, image.PixelHeight);
            var x = Math.Clamp((int)Math.Round((LoupeFocusX * image.PixelWidth) - (cropWidth / 2.0)), 0, image.PixelWidth - cropWidth);
            var y = Math.Clamp((int)Math.Round((LoupeFocusY * image.PixelHeight) - (cropHeight / 2.0)), 0, image.PixelHeight - cropHeight);
            return new Int32Rect(x, y, cropWidth, cropHeight);
        }
    }

    public BitmapSource? LoupeImage
    {
        get
        {
            var image = LoupeBaseImage;
            var cropRect = LoupeCropRect;
            if (image is null || cropRect is null)
            {
                return null;
            }

            var cropped = new CroppedBitmap(image, cropRect.Value);
            cropped.Freeze();
            return cropped;
        }
    }

    public bool IsLoupeImageVisible => LoupeImage is not null;
    public bool IsLoupePlaceholderVisible => LoupeImage is null;
    public string LoupePlaceholderText => LoupeUnavailableText;
    public string LoupeSourceLabelText => IsLoupeSourceLive ? $"表示: {LoupeCameraAlias} / Live" : $"表示: {LoupeCameraAlias}";
    public string LoupeFreshnessText => FreshnessText(LoupeCameraAlias);
    public bool IsLoupeFreshnessVisible => IsLoupeImageVisible && !IsLoupeSourceLive;

    /// <summary>Where the target sits within <see cref="LoupeCropRect"/>, as a 0..1 fraction
    /// of the crop's own width/height. Equal to the crop's center (0.5, 0.5) except when the
    /// target is near a frame edge and the crop had to be clamped to stay inside the source
    /// frame — at which point the reticle drawn inside the loupe should shift off-center to
    /// stay accurate, instead of silently pretending the target is still centered.</summary>
    public double LoupeMarkerRelativeX
    {
        get
        {
            var image = LoupeBaseImage;
            var cropRect = LoupeCropRect;
            return image is null || cropRect is null
                ? 0.5
                : ComputeMarkerRelative(TargetX, image.PixelWidth, cropRect.Value.X, cropRect.Value.Width);
        }
    }

    public double LoupeMarkerRelativeY
    {
        get
        {
            var image = LoupeBaseImage;
            var cropRect = LoupeCropRect;
            return image is null || cropRect is null
                ? 0.5
                : ComputeMarkerRelative(TargetY, image.PixelHeight, cropRect.Value.Y, cropRect.Value.Height);
        }
    }

    /// <summary>拡大エリア内にターゲット□の枠線を出すかどうか。切り出しはポインタ位置を
    /// 中心にするので、ターゲットが切り出し範囲の外にあることがある。範囲外のときに枠線を
    /// 端へ張り付けて描くと、そこにターゲットがあるように見えてしまうため出さない。</summary>
    public bool IsLoupeMarkerVisible
    {
        get
        {
            var image = LoupeBaseImage;
            var cropRect = LoupeCropRect;
            if (image is null || cropRect is null)
            {
                return false;
            }

            var crop = cropRect.Value;
            var targetPixelX = TargetX * image.PixelWidth;
            var targetPixelY = TargetY * image.PixelHeight;
            return targetPixelX >= crop.X && targetPixelX <= crop.X + crop.Width &&
                   targetPixelY >= crop.Y && targetPixelY <= crop.Y + crop.Height;
        }
    }

    private static double ComputeMarkerRelative(double targetFraction, int imageExtent, int cropOrigin, int cropExtent)
    {
        if (cropExtent <= 0)
        {
            return 0.5;
        }

        var targetPixel = targetFraction * imageExtent;
        return Math.Clamp((targetPixel - cropOrigin) / cropExtent, 0.0, 1.0);
    }

    // --- Alignment guide overlays and tilt reading (issue #32): grid/トンボ/overlap-band/safe-
    // margin stage overlays and a bottom-of-stage ROLL tilt readout, both purely advisory per
    // docs/OPERATOR_UI_SPEC.md's 設置ガイドオーバーレイと傾き読み値 section. Detection runs
    // through DocumentTiltDetector, a pure BitmapSource->double? function. Neither the overlay
    // toggle state nor the detected angle is ever read by RebuildReadiness/RecalculateAvailability
    // or folded into CanCapture — the常時禁止 "原稿エッジ検出・傾き読み値による撮影可否の判定と
    // 自動補正への接続" is enforced structurally by never wiring these properties into that path. ---

    /// <summary>方眼グリッド overlay toggle. Off by default — a newly introduced guide layer the
    /// operator opts into, not a change to any pre-existing stage display.</summary>
    public bool IsGridOverlayEnabled
    {
        get => _isGridOverlayEnabled;
        set { if (SetProperty(ref _isGridOverlayEnabled, value)) OnPropertyChanged(nameof(IsGridOverlayVisible)); }
    }

    /// <summary>トンボ（四隅＋辺中央の合わせマーク） overlay toggle. Off by default, same
    /// reasoning as <see cref="IsGridOverlayEnabled"/>.</summary>
    public bool IsTombOverlayEnabled
    {
        get => _isTombOverlayEnabled;
        set { if (SetProperty(ref _isTombOverlayEnabled, value)) OnPropertyChanged(nameof(IsTombOverlayVisible)); }
    }

    /// <summary>重複帯 overlay toggle. On by default: the band display already existed
    /// unconditionally before this issue made it toggleable
    /// (<see cref="StageCompositeOverlapBandText"/>), so the default preserves that existing
    /// behavior instead of silently hiding something operators already relied on.</summary>
    public bool IsOverlapBandOverlayEnabled
    {
        get => _isOverlapBandOverlayEnabled;
        set { if (SetProperty(ref _isOverlapBandOverlayEnabled, value)) OnPropertyChanged(nameof(IsOverlapBandVisible)); }
    }

    /// <summary>安全マージン（SAFE MARGIN 枠） overlay toggle. Off by default, same reasoning as
    /// <see cref="IsGridOverlayEnabled"/>.</summary>
    public bool IsSafeMarginOverlayEnabled
    {
        get => _isSafeMarginOverlayEnabled;
        set { if (SetProperty(ref _isSafeMarginOverlayEnabled, value)) OnPropertyChanged(nameof(IsSafeMarginOverlayVisible)); }
    }

    /// <summary>表示(V)メニューの「傾き読み値の表示」トグル（issue #34）。ステージ下部の傾き
    /// 読み値常駐行（#32で追加済み）自体の表示・非表示だけを切り替える — 検出結果や許容範囲の
    /// 判定ロジックには一切触れない、純粋な表示トグル。</summary>
    public bool IsTiltReadingVisible
    {
        get => _isTiltReadingVisible;
        set => SetProperty(ref _isTiltReadingVisible, value);
    }

    public bool IsGridOverlayVisible => IsGridOverlayEnabled && !IsStageProcessingPlaceholder;
    public bool IsTombOverlayVisible => IsTombOverlayEnabled && !IsStageProcessingPlaceholder;
    public bool IsSafeMarginOverlayVisible => IsSafeMarginOverlayEnabled && !IsStageProcessingPlaceholder;

    /// <summary>構図グリッドの列数。ステージ表示領域をこの数でちょうど等分する。
    /// 原稿サイズや割り付けは案件ごとに違うため、3分割固定にはしない。</summary>
    public int GridColumns
    {
        get => _gridColumns;
        set
        {
            var clamped = ClampGridDivision(value);
            var changed = SetProperty(ref _gridColumns, clamped);
            if (changed)
            {
                OnPropertyChanged(nameof(GridDivisionText));
            }
            else if (clamped != value)
            {
                // 範囲外の入力を丸めた結果が今の値と同じだと変更通知が出ず、
                // 入力欄には拒否したはずの値が残ってしまう。丸めたことを必ず返す。
                OnPropertyChanged(nameof(GridColumns));
            }
        }
    }

    /// <summary>構図グリッドの行数。<see cref="GridColumns"/> の対。</summary>
    public int GridRows
    {
        get => _gridRows;
        set
        {
            var clamped = ClampGridDivision(value);
            var changed = SetProperty(ref _gridRows, clamped);
            if (changed)
            {
                OnPropertyChanged(nameof(GridDivisionText));
            }
            else if (clamped != value)
            {
                OnPropertyChanged(nameof(GridRows));
            }
        }
    }

    public string GridDivisionText => $"{GridColumns} × {GridRows}";

    /// <summary>入力欄からの直接指定を受けるため、範囲外はここで丸める（1未満・24超は作らない）。</summary>
    private static int ClampGridDivision(int value) => Math.Clamp(value, MinGridDivision, MaxGridDivision);

    public void ApplyGridPreset(int division)
    {
        GridColumns = division;
        GridRows = division;
        IsGridOverlayEnabled = true;
    }

    /// <summary>ステージ下部に一時的に出す通知。3.2秒で自然に消える。</summary>
    public string NoticeText
    {
        get => _noticeText;
        private set
        {
            if (SetProperty(ref _noticeText, value))
            {
                OnPropertyChanged(nameof(IsNoticeVisible));
            }
        }
    }

    public bool IsNoticeVisible => !string.IsNullOrEmpty(NoticeText);

    /// <summary>通知の種別。"ok" は完了、"warn" は注意。色はXAML側でトークンへ解決する。</summary>
    public string NoticeKind
    {
        get => _noticeKind;
        private set => SetProperty(ref _noticeKind, value);
    }

    /// <summary>通知を出す。世代番号で上書きを判定するので、連続して出しても
    /// 古い通知のタイマーが新しい通知を消してしまうことはない。</summary>
    public void Notify(string text, bool succeeded)
    {
        NoticeKind = succeeded ? "ok" : "warn";
        NoticeText = text;
        var generation = ++_noticeGeneration;
        _ = DismissNoticeAsync(generation);
    }

    private async Task DismissNoticeAsync(int generation)
    {
        try
        {
            await Task.Delay(NoticeVisibleMilliseconds, _lifetimeToken).ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (_noticeGeneration == generation)
        {
            NoticeText = string.Empty;
        }
    }

    /// <summary>表示設定の一括リセット。誤操作で構図やピント表示が消えると撮り直しになるため、
    /// 1回目で確認待ちにし、2回目の押下で確定する。3秒放置で解除。</summary>
    public bool IsResetArmed
    {
        get => _isResetArmed;
        private set
        {
            if (SetProperty(ref _isResetArmed, value))
            {
                OnPropertyChanged(nameof(ResetButtonText));
            }
        }
    }

    public string ResetButtonText => IsResetArmed ? "もう一度で初期化" : "リセット";

    private void RequestResetView()
    {
        if (!IsResetArmed)
        {
            IsResetArmed = true;
            var generation = ++_resetArmGeneration;
            _ = DisarmResetAsync(generation);
            return;
        }

        _resetArmGeneration++;
        IsResetArmed = false;
        GridColumns = DefaultGridDivision;
        GridRows = DefaultGridDivision;
        IsGridOverlayEnabled = false;
        IsTombOverlayEnabled = false;
        IsSafeMarginOverlayEnabled = false;
        IsOverlapBandOverlayEnabled = true;
        IsTiltReadingVisible = true;
        SelectedLoupeZoom = LoupeZoom100;
        Notify("表示設定を初期状態へ戻しました（撮影データは変えていません）", true);
    }

    private async Task DisarmResetAsync(int generation)
    {
        try
        {
            await Task.Delay(ResetArmedMilliseconds, _lifetimeToken).ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (_resetArmGeneration == generation)
        {
            IsResetArmed = false;
        }
    }

    /// <summary>保存したファイルの控え。保存を押して実際に出力できたときだけ増える
    /// （自動保存はしないので、ここが増えていれば操作者が保存したということ）。</summary>
    public ObservableCollection<SavedFileViewModel> SavedFiles { get; } = [];

    public bool HasSavedFiles => SavedFiles.Count > 0;

    public string SavedFileCountText => $"このセッション {SavedFiles.Count} 件";

    private void RecordSavedFile(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || path == "未実行")
        {
            return;
        }

        if (SavedFiles.Any(saved => string.Equals(saved.Path, path, StringComparison.OrdinalIgnoreCase)))
        {
            return;
        }

        SavedFiles.Insert(0, new SavedFileViewModel(DateTime.Now.ToString("HH:mm", CultureInfo.InvariantCulture), path));
        while (SavedFiles.Count > MaxSavedFileRows)
        {
            SavedFiles.RemoveAt(SavedFiles.Count - 1);
        }

        OnPropertyChanged(nameof(HasSavedFiles));
        OnPropertyChanged(nameof(SavedFileCountText));
    }

    /// <summary>Combines the new #32 toggle with the pre-existing
    /// <see cref="StageCompositeApplicable"/> gate the band's Border already used, so turning the
    /// toggle off actually hides it instead of being overridden by the older binding.</summary>
    public bool IsOverlapBandVisible => IsOverlapBandOverlayEnabled && StageCompositeApplicable;

    /// <summary>The SIMULATED live camera's most recently rendered frame — the same "preview
    /// only, never original/stitch input" source the stage image bindings already read from
    /// (<see cref="StageSingleLiveImage"/>/<see cref="StageCompositeLiveImage"/>), reused here so
    /// the tilt reading always reflects "Live View frameからの原稿エッジ検出" per
    /// docs/OPERATOR_UI_SPEC.md, and only ever reflects the live camera — never a frozen still.</summary>
    private BitmapSource? CurrentLiveTiltSourceImage =>
        IsLiveViewActive && _lastLiveFrames.TryGetValue(SelectedCamera, out var liveFrame) ? liveFrame.Image : null;

    /// <summary>The detected in-plane rotation (ROLL) of the live camera's current frame, or
    /// null when there is nothing to detect from — not live, no frame yet, or
    /// <see cref="DocumentTiltDetector"/> itself could not find the document
    /// (<see cref="TiltRollDegreesText"/>'s 検出不能 case). This property and everything derived
    /// from it are read-only display data: nothing in this VM feeds it back into
    /// <see cref="CanCapture"/>, <see cref="RebuildReadiness"/>, or any other readiness/capture
    /// path (docs/OPERATOR_UI_SPEC.md's 常時禁止).</summary>
    public double? TiltRollDegrees => DocumentTiltDetector.DetectRollDegrees(CurrentLiveTiltSourceImage);

    public string TiltRollDegreesText => TiltRollDegrees is { } degrees ? $"傾き {degrees:F2}°" : "傾き 検出不能";

    /// <summary>Operator-entered tolerance, typed as free text (issue #32's "設定手段は簡素な
    /// 入力" — a menu-based settings surface is #34's scope). Deliberately starts empty/unset:
    /// the issue's own contract text says "許容値は設定値とし、初期値の決定は実装時に操作者へ
    /// 確認する（勝手に既定値を作らない）" — with no operator available to ask during this
    /// automated implementation, leaving it unset is the compliant choice docs/OPERATOR_UI_SPEC.md
    /// itself allows ("本仕様では既定値を定めない"), not a stand-in default value.</summary>
    public string TiltToleranceInputText
    {
        get => _tiltToleranceInputText;
        set
        {
            var trimmed = value?.Trim() ?? string.Empty;
            if (trimmed.Length == 0)
            {
                if (SetProperty(ref _tiltToleranceInputText, trimmed))
                {
                    _tiltToleranceDegrees = null;
                    OnPropertyChanged(nameof(TiltToleranceDegrees));
                    OnPropertyChanged(nameof(TiltToleranceChipText));
                }
                return;
            }

            if (double.TryParse(trimmed, NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed) && parsed >= 0)
            {
                if (SetProperty(ref _tiltToleranceInputText, trimmed))
                {
                    _tiltToleranceDegrees = parsed;
                    OnPropertyChanged(nameof(TiltToleranceDegrees));
                    OnPropertyChanged(nameof(TiltToleranceChipText));
                }
                return;
            }

            // Reject: re-announce the last accepted value so the TextBox reverts, matching the
            // rejection pattern used elsewhere in this VM (e.g. SelectedSimulatedFramePattern).
            OnPropertyChanged(nameof(TiltToleranceInputText));
        }
    }

    public double? TiltToleranceDegrees => _tiltToleranceDegrees;

    /// <summary>The 許容範囲チップ text. While unset, this deliberately shows only "許容値未設定"
    /// and never a 許容内/超過 judgment — docs/OPERATOR_UI_SPEC.md: "許容値は設定値とし、本仕様
    /// では既定値を定めない" together with issue #32's "未設定の間はチップに『許容値未設定』と表示
    /// し、判定（許容内/超過）を出さない". Once the operator sets a tolerance, this chip does
    /// describe whether the current reading sits inside it — but purely as display text on this
    /// VM's own properties; it is never read by CanCapture or any readiness path, so it stays a
    /// guide-only annotation, not the Go/NoGo judgment (which remains ReadinessSnapshot's alone).</summary>
    public string TiltToleranceChipText
    {
        get
        {
            if (TiltToleranceDegrees is not { } tolerance)
            {
                return "許容値未設定";
            }

            return TiltRollDegrees is { } degrees
                ? $"許容 ±{tolerance:F2}° 内 / {(Math.Abs(degrees) <= tolerance ? "許容内" : "許容超過")}"
                : $"許容 ±{tolerance:F2}° 内 / 検出不能のため判定不可";
        }
    }

    // --- Focus panel (issue #31): AF execution, MF stepping, focus peaking, and per-camera
    // fixed-state chips. UI and SIMULATED (fake backend) only, per #35 Option A — the panel is
    // disabled with a shown reason whenever the DualCamera flow's execution environment is
    // HardwareDual (real hardware pending human-gate approval), regardless of the operating
    // mode selected. No real AF/MF command is ever sent; every result below is derived from
    // this shell's own SIMULATED state (target position, blur radius of the last live-ticked
    // frame). ---

    /// <summary>False whenever the DualCamera flow was composed against real hardware
    /// (<see cref="DualCameraExecutionEnvironment.HardwareDual"/>) — mirrors the same check
    /// <see cref="BannerText"/> uses. The focus panel is a 撮影系操作 per docs/OPERATOR_UI_SPEC.md
    /// and must stay invisible/disabled here until a hardware-required Issue and human gate
    /// approve real AF/MF wiring (#35 Option A); this VM never reaches that approval, so the
    /// gate here is unconditional for HardwareDual.</summary>
    public bool IsFocusPanelAvailable =>
        _dualCameraFlow?.ExecutionEnvironment != DualCameraExecutionEnvironment.HardwareDual;

    /// <summary>Inverse of <see cref="IsFocusPanelAvailable"/>, so XAML can bind both branches
    /// (disabled-reason panel vs. interactive panel) through the one shared
    /// <c>BoolToVisibilityConverter</c> already used everywhere else in this window, instead of
    /// introducing a second inverse converter.</summary>
    public bool IsFocusPanelUnavailable => !IsFocusPanelAvailable;

    public string FocusPanelUnavailableReason =>
        "実機モードでは、承認までフォーカスパネルを無効表示とします（fail-closed）。UIとSIMULATEDだけを先行実装しており、実機へのAF・MFコマンド配線はhardware-requiredの別Issueとhuman gate承認後にだけ有効化します。";

    /// <summary>The camera the focus panel operates on — always the camera currently in Live
    /// View, since SIMULATED contrast AF (like the real D810 capability table) only makes sense
    /// while Live View is streaming.</summary>
    public string FocusPanelCameraAlias => SelectedCamera;

    /// <summary>Which camera alias physically owns the target reticle's current position, under
    /// a fixed left-half=CAM-A / right-half=CAM-B document split. This is deliberately NOT
    /// <see cref="IsTargetOnLiveSide"/>: that property is screen-column-relative (whichever
    /// alias is live is always drawn in the left column, so X&lt;0.5 always means "the column
    /// currently showing the live feed" and does not change when the live alias is switched) —
    /// using it here would make the one-click switch button never actually resolve the block
    /// (switching cameras would leave the target on the same screen-column-relative side).
    /// This property instead anchors the split to a fixed alias, mirroring the physical rig
    /// fact that CAM-A/CAM-B each cover a fixed half of the document regardless of which one
    /// currently has Live View open. ※要確認: like <see cref="IsTargetOnLiveSide"/>, this is a
    /// placeholder proxy for the real A0 layout geometry (#29/#32); Architect should confirm
    /// the fixed-alias split (rather than the live-relative one) is the correct reading for AF
    /// domain gating once that geometry lands.</summary>
    private string TargetDomainCameraAlias => TargetX < 0.5 ? "CAM-A" : "CAM-B";

    /// <summary>True when the shared target reticle (issue #30) sits in a document half owned
    /// by the camera that is not currently live (see <see cref="TargetDomainCameraAlias"/>). AF
    /// cannot aim at a domain the live camera cannot see, so AF execution is disabled and a
    /// one-click switch-to-live button for the owning camera is offered instead. Always false in
    /// SingleCamera mode, where there is only one camera and no domain split.</summary>
    public bool IsFocusTargetOutsideLiveCameraDomain =>
        !IsSingleCameraMode && !string.Equals(TargetDomainCameraAlias, SelectedCamera, StringComparison.Ordinal);

    public bool ShowSwitchLiveCameraButton => IsFocusPanelAvailable && IsFocusTargetOutsideLiveCameraDomain;

    public bool ShowAutoFocusButton => IsFocusPanelAvailable && !IsFocusTargetOutsideLiveCameraDomain;

    public string SwitchLiveCameraButtonText => $"{TargetDomainCameraAlias} live に切替";

    /// <summary>Base gate shared by AF and MF: the focus panel must be available (see
    /// <see cref="IsFocusPanelAvailable"/>), Live View must actually be streaming (contrast AF
    /// and MF stepping both operate on the live camera), no other lock (busy / in-flight AF /
    /// the Capturing-Stitching processing placeholder) may be active.</summary>
    public bool CanUseFocusPanel => IsFocusPanelAvailable && IsLiveViewActive && !IsBusy && !IsAutoFocusRunning && CanAdjustTarget;

    public bool CanExecuteAutoFocus => CanUseFocusPanel && !IsFocusTargetOutsideLiveCameraDomain;

    /// <summary>Mirrors <see cref="CanSelectCamera"/>'s UiState condition exactly (rather than
    /// <see cref="CanAdjustTarget"/>'s looser one) because <see cref="SwitchLiveCameraToTargetDomain"/>
    /// relies on <c>SelectedCamera</c>'s own setter succeeding once Live View is stopped — if
    /// this were true while <see cref="CanSelectCamera"/> could still be false (e.g. UiState is
    /// Review with Live View re-enabled ahead of a new capture), the camera switch would
    /// silently no-op. Also requires <see cref="CanUseLiveView"/> directly: the switch writes
    /// <see cref="IsLiveViewActive"/> straight through (bypassing <see cref="ToggleLiveViewCommand"/>'s
    /// own CanExecute check), so this is the only thing stopping it from turning Live View back
    /// on while it is blocked (e.g. <c>CameraStateRequiresInspection</c>).</summary>
    public bool CanSwitchLiveCameraToTargetDomain =>
        IsFocusPanelAvailable && !IsSingleCameraMode && !IsBusy && !IsAutoFocusRunning && CanUseLiveView &&
        IsFocusTargetOutsideLiveCameraDomain &&
        UiState is not (OperatorUiState.Capturing or OperatorUiState.Stitching or OperatorUiState.Review or OperatorUiState.FailedPartial or OperatorUiState.Degraded);

    public bool IsAutoFocusRunning
    {
        get => _isAutoFocusRunning;
        private set
        {
            if (SetProperty(ref _isAutoFocusRunning, value))
            {
                RaiseFocusPanelProperties();
            }
        }
    }

    public string FocusResultText => _lastFocusResult is { } result
        ? $"{(result.Success ? "合焦OK" : "合焦NG")} / {result.CameraAlias} / {result.ExecutedAt:HH:mm:ss} / □ ({result.TargetX:F2}, {result.TargetY:F2})"
        : "AF未実行";

    /// <summary>SIMULATED contract hook for issue #33's future "撮影+AF" action-zone button:
    /// #33 can call <see cref="RecordPreCaptureAutoFocusOutcome"/> immediately before starting a
    /// capture to record whether pre-capture AF ran and its outcome, giving the eventual
    /// capture flow a typed result to write into its journal (AF実行の有無と結果) and to
    /// fail-closed before opening the shutter if AF did not converge. This VM never calls it
    /// itself and never gates capture on it — wiring an actual "撮影+AF" button, running AF for
    /// each required camera in sequence, and stopping before the shutter on NG is #33's scope
    /// (see issue #31's 対象外: "「撮影」「撮影+AF」ボタン自体の設置（#33）").</summary>
    public FocusExecutionResult? LastPreCaptureAutoFocusResult { get; private set; }

    public void RecordPreCaptureAutoFocusOutcome(FocusExecutionResult result)
    {
        LastPreCaptureAutoFocusResult = result ?? throw new ArgumentNullException(nameof(result));
        OnPropertyChanged(nameof(LastPreCaptureAutoFocusResult));
    }

    public double FocusPositionValue =>
        _focusPositionValues.TryGetValue(SelectedCamera, out var value) ? value : DefaultFocusPositionValue;

    public string FocusPositionText => $"{FocusPositionValue:F0} / 100（相対値・無次元・read-only表示）";

    public string CameraAFocusStatusText => FormatFocusChip("CAM-A");
    public string CameraBFocusStatusText => FormatFocusChip("CAM-B");

    private string FormatFocusChip(string alias) =>
        $"{alias}: {(_focusFixed.TryGetValue(alias, out var fixedState) && fixedState ? "固定済" : "未固定")}";

    public bool IsPeakingEnabled
    {
        get => _isPeakingEnabled;
        private set
        {
            if (SetProperty(ref _isPeakingEnabled, value))
            {
                RaiseStageFrameProperties();
            }
        }
    }

    public string PeakingButtonText => IsPeakingEnabled ? "ピーキング OFF" : "ピーキング ON";

    /// <summary>Preview-only edge-highlight overlay (see <see cref="FocusPeakingOverlayRenderer"/>)
    /// for the stage's single-live full-frame image. Null whenever peaking is off or no base
    /// image exists — never computed unless <see cref="IsPeakingEnabled"/>, so the per-tick cost
    /// is zero while the toggle is off.</summary>
    public BitmapSource? StageSingleLivePeakingOverlay =>
        IsPeakingEnabled ? FocusPeakingOverlayRenderer.BuildOverlay(StageSingleLiveImage) : null;
    public bool IsStageSingleLivePeakingOverlayVisible => StageSingleLivePeakingOverlay is not null;

    public BitmapSource? StageCompositeLivePeakingOverlay =>
        IsPeakingEnabled ? FocusPeakingOverlayRenderer.BuildOverlay(StageCompositeLiveImage) : null;
    public bool IsStageCompositeLivePeakingOverlayVisible => StageCompositeLivePeakingOverlay is not null;

    public BitmapSource? LoupePeakingOverlay =>
        IsPeakingEnabled ? FocusPeakingOverlayRenderer.BuildOverlay(LoupeImage) : null;
    public bool IsLoupePeakingOverlayVisible => LoupePeakingOverlay is not null;

    private async Task ExecuteAutoFocusAsync()
    {
        if (!CanExecuteAutoFocus)
        {
            return;
        }

        var alias = SelectedCamera;
        var targetX = TargetX;
        var targetY = TargetY;
        IsAutoFocusRunning = true;
        StatusMessage = $"{alias}: AF実行中です（AFエリア＝□ {targetX:F2}, {targetY:F2}）。";
        try
        {
            await Task.Delay(TimeSpan.FromMilliseconds(150), _lifetimeToken).ConfigureAwait(true);
        }
        catch (OperationCanceledException) when (_lifetimeToken.IsCancellationRequested)
        {
            IsAutoFocusRunning = false;
            return;
        }

        var blurRadius = _lastLiveFrameBlurRadius.TryGetValue(alias, out var radius) ? radius : 0.0;
        var success = blurRadius <= SharpBlurRadiusThreshold;
        _lastFocusResult = new FocusExecutionResult(alias, success, DateTimeOffset.Now, targetX, targetY);
        if (success)
        {
            _focusFixed[alias] = true;
            StatusMessage = $"{alias}: AF実行完了 — 合焦OK。原稿撮影の運用方針どおり撮影前固定として扱います。";
        }
        else
        {
            StatusMessage = $"{alias}: AF実行完了 — 合焦NGです（ボケ→合焦遷移パターンの遷移中に実行されました）。再実行してください。";
        }

        OnPropertyChanged(nameof(FocusResultText));
        OnPropertyChanged(nameof(CameraAFocusStatusText));
        OnPropertyChanged(nameof(CameraBFocusStatusText));
        RebuildReadiness(preserveOutcomeState: true);
        IsAutoFocusRunning = false;
    }

    /// <summary>Issue #33's "撮影+AF" 従ボタン: runs a SIMULATED pre-capture AF check for every
    /// camera <see cref="CurrentCapturePlan"/> requires (A→B順, per docs/OPERATOR_UI_SPEC.md's
    /// 標準操作順 5), then — only if every camera converges — calls the unchanged existing capture
    /// entry point (<see cref="RunCaptureAsync"/>) exactly as the primary <see cref="CaptureCommand"/>
    /// does. AF is deliberately prepended in front of the existing flow rather than woven into it
    /// (a scoped simplification directed by the issue's own contract text: "撮影シーケンス・合成
    /// ロジック自体を変更しない…AF段を前置してから既存フローを呼ぶ構造にする"), so the spec's more
    /// precise technical description — this AF runs after each camera's own Live View stop, as a
    /// phase-detection AF — is intentionally not modeled: this method's AF stage runs once, before
    /// the (unmodified) flow performs its own single Live View stop. Any convergence failure stops
    /// here with no shutter ever fired (fail-closed, no automatic retry) and never reaches
    /// <see cref="RunCaptureAsync"/>.</summary>
    private async Task RunCaptureWithAutoFocusAsync(string scenario)
    {
        if (!CanCaptureWithAutoFocus)
        {
            return;
        }

        var capturePlan = CurrentCapturePlan;
        _isPreCaptureAutoFocusRunning = true;
        RaisePreCaptureAutoFocusGateProperties();
        var allFocused = true;
        try
        {
            StatusMessage = "撮影+AF: 撮影直前AFを実行しています。合焦を確認するまでシャッターは切りません。";
            var afSummaries = new List<string>();
            foreach (var alias in capturePlan.RequiredCameraAliases)
            {
                try
                {
                    await Task.Delay(TimeSpan.FromMilliseconds(150), _lifetimeToken).ConfigureAwait(true);
                }
                catch (OperationCanceledException) when (_lifetimeToken.IsCancellationRequested)
                {
                    return;
                }

                var afResult = SimulatePreCaptureAutoFocus(alias);
                RecordPreCaptureAutoFocusOutcome(afResult);
                afSummaries.Add($"{alias}:{(afResult.Success ? "合焦OK" : "合焦NG")}");
                OnPropertyChanged(nameof(FocusResultText));
                OnPropertyChanged(nameof(CameraAFocusStatusText));
                OnPropertyChanged(nameof(CameraBFocusStatusText));

                if (!afResult.Success)
                {
                    allFocused = false;
                    CaptureResult = $"未実行（{alias} 撮影直前AF NG）";
                    TechnicalDetail = $"error code: PreCaptureAutoFocusFailed / camera: {alias} / capture calls: 0 / automatic retry count: 0 / 撮影+AF: {string.Join(" / ", afSummaries)}";
                    StatusMessage = $"撮影+AF: {alias}の撮影直前AFが合焦しなかったため、シャッターを実行せず撮影失敗（再開不可）で停止しました。{NoRetryMessage(capturePlan)}";
                    UiState = OperatorUiState.FailedPartial;
                    break;
                }
            }

            if (allFocused)
            {
                TechnicalDetail = $"error code: なし / 撮影+AF: {string.Join(" / ", afSummaries)}";
            }
        }
        finally
        {
            _isPreCaptureAutoFocusRunning = false;
            RaisePreCaptureAutoFocusGateProperties();
            RebuildReadiness(preserveOutcomeState: !allFocused);
        }

        if (allFocused)
        {
            await RunCaptureAsync(scenario).ConfigureAwait(true);
        }
    }

    /// <summary>SIMULATED stand-in for the 撮影+AF pre-capture phase-detection AF check (docs/
    /// OPERATOR_UI_SPEC.md 標準操作順 5): unlike <see cref="ExecuteAutoFocusAsync"/> (contrast AF,
    /// which requires <paramref name="alias"/> to currently be the live-streaming camera), this
    /// does not require Live View to be active on <paramref name="alias"/> — matching the spec's
    /// description of this AF running after Live View has stopped. It reuses whatever blur radius
    /// was last recorded for that alias while it *was* live (defaulting to sharp/0 — this shell's
    /// existing "no data recorded means sharp" convention — when the camera was never live-ticked),
    /// so the same 合焦NG scenario used for the Focus panel's "AF実行" button (the blur-to-focus
    /// ramp pattern on the live camera) also exercises a 撮影+AF failure.</summary>
    private FocusExecutionResult SimulatePreCaptureAutoFocus(string alias)
    {
        var blurRadius = _lastLiveFrameBlurRadius.TryGetValue(alias, out var radius) ? radius : 0.0;
        var success = blurRadius <= SharpBlurRadiusThreshold;
        var result = new FocusExecutionResult(alias, success, DateTimeOffset.Now, TargetX, TargetY);
        _lastFocusResult = result;
        if (success)
        {
            _focusFixed[alias] = true;
        }
        return result;
    }

    private void StepFocus(double delta)
    {
        if (!CanUseFocusPanel)
        {
            return;
        }

        var alias = SelectedCamera;
        var current = _focusPositionValues.TryGetValue(alias, out var value) ? value : DefaultFocusPositionValue;
        var updated = Math.Clamp(current + delta, MinFocusPositionValue, MaxFocusPositionValue);
        _focusPositionValues[alias] = updated;
        // A manual nudge after AF invalidates the "AF実行後に固定" assumption until re-confirmed.
        _focusFixed[alias] = false;
        OnPropertyChanged(nameof(FocusPositionValue));
        OnPropertyChanged(nameof(FocusPositionText));
        OnPropertyChanged(nameof(CameraAFocusStatusText));
        OnPropertyChanged(nameof(CameraBFocusStatusText));
        StatusMessage = $"{alias}: MFステップ {(delta > 0 ? "+" : string.Empty)}{delta:F0} を適用しました（相対値 {updated:F0}/100）。";
        RebuildReadiness(preserveOutcomeState: true);
    }

    /// <summary>Explicit, one-click operator action (never automatic — see docs/OPERATOR_UI_SPEC.md's
    /// 常時禁止 "ターゲット□位置によるLive Viewカメラの自動切替"): stops Live View for the
    /// currently selected camera, selects the other camera, then restarts Live View for it, so
    /// a single click actually lands on the camera whose domain the target reticle sits in.</summary>
    private void SwitchLiveCameraToTargetDomain()
    {
        if (!CanSwitchLiveCameraToTargetDomain)
        {
            return;
        }

        var targetAlias = TargetDomainCameraAlias;
        StatusMessage = $"{targetAlias} のライブ表示へ切り替えます（操作者の明示クリックのみ・自動切替ではありません）。";
        IsLiveViewActive = false;
        SelectedCamera = targetAlias;
        IsLiveViewActive = true;
    }

    private void RaiseFocusPanelProperties()
    {
        OnPropertyChanged(nameof(FocusPanelCameraAlias));
        OnPropertyChanged(nameof(IsFocusTargetOutsideLiveCameraDomain));
        OnPropertyChanged(nameof(ShowSwitchLiveCameraButton));
        OnPropertyChanged(nameof(ShowAutoFocusButton));
        OnPropertyChanged(nameof(SwitchLiveCameraButtonText));
        OnPropertyChanged(nameof(CanUseFocusPanel));
        OnPropertyChanged(nameof(CanExecuteAutoFocus));
        OnPropertyChanged(nameof(CanSwitchLiveCameraToTargetDomain));
        OnPropertyChanged(nameof(FocusPositionValue));
        OnPropertyChanged(nameof(FocusPositionText));
        OnPropertyChanged(nameof(CanCaptureWithAutoFocus));
        OnPropertyChanged(nameof(IsCaptureWithAutoFocusUnavailableReasonVisible));
        NotifyAllCommands();
    }

    /// <summary>Refreshes every binding gated by <see cref="_isPreCaptureAutoFocusRunning"/> —
    /// called when issue #33's "撮影+AF" pre-capture AF gate starts and ends, mirroring how
    /// <see cref="IsBusy"/>'s own setter refreshes the equivalent set of dependent bindings.</summary>
    private void RaisePreCaptureAutoFocusGateProperties()
    {
        OnPropertyChanged(nameof(CanCapture));
        OnPropertyChanged(nameof(CanCaptureWithAutoFocus));
        OnPropertyChanged(nameof(CaptureDisabledReason));
        OnPropertyChanged(nameof(CanChangeOperatingMode));
        OnPropertyChanged(nameof(CanSelectCamera));
        NotifyAllCommands();
    }

    // Stage frame wiring: preview-only images sourced from the SIMULATED live view frame
    // pump (see Simulated/). Every one of these falls back to null/false — and the
    // existing text placeholders stay visible — when no pump was injected or no frame has
    // arrived yet, so the headless ViewModel tests that construct this class without a pump
    // keep behaving exactly as before.
    public BitmapSource? StageSingleLiveImage =>
        IsLiveViewActive && string.Equals(SelectedCamera, StageSingleLiveAlias, StringComparison.Ordinal) &&
        _lastLiveFrames.TryGetValue(StageSingleLiveAlias, out var singleLiveFrame)
            ? singleLiveFrame.Image
            : null;
    public bool IsStageSingleLiveImageVisible => IsStageSingleLiveMode && StageSingleLiveImage is not null;
    public bool IsStageSingleLivePlaceholderVisible => IsStageSingleLiveMode && StageSingleLiveImage is null;

    public BitmapSource? StageCompositeLiveImage =>
        IsLiveViewActive && _lastLiveFrames.TryGetValue(StageCompositeLiveAlias, out var compositeLiveFrame)
            ? compositeLiveFrame.Image
            : null;
    public bool IsStageCompositeLiveImageVisible => StageCompositeApplicable && StageCompositeLiveImage is not null;
    public bool IsStageCompositeLivePlaceholderVisible => StageCompositeApplicable && StageCompositeLiveImage is null;

    /// <summary>The still alias never receives live frames directly (Live View is
    /// one-camera-at-a-time); this is simply the last frame captured while that alias was
    /// selected, frozen in place — the "非ライブ側は最終フレームの静止画" contract.</summary>
    public BitmapSource? StageCompositeStillImage =>
        _lastLiveFrames.TryGetValue(StageCompositeStillAlias, out var compositeStillFrame) ? compositeStillFrame.Image : null;
    public bool IsStageCompositeStillImageVisible => StageCompositeApplicable && StageCompositeStillImage is not null;
    public bool IsStageCompositeStillPlaceholderVisible => StageCompositeApplicable && StageCompositeStillImage is null;

    public string StatusMessage { get => _statusMessage; private set => SetProperty(ref _statusMessage, value); }
    public string TechnicalDetail { get => _technicalDetail; private set => SetProperty(ref _technicalDetail, value); }
    public string LastTransactionId { get => _lastTransactionId; private set => SetProperty(ref _lastTransactionId, value); }
    public string CaptureResult { get => _captureResult; private set => SetProperty(ref _captureResult, value); }
    public string StitchResult { get => _stitchResult; private set => SetProperty(ref _stitchResult, value); }
    public string ExportResult { get => _exportResult; private set => SetProperty(ref _exportResult, value); }
    public string RetainedOriginals { get => _retainedOriginals; private set => SetProperty(ref _retainedOriginals, value); }
    public string LastStitchJobId { get => _lastStitchJobId; private set => SetProperty(ref _lastStitchJobId, value); }
    public string LastExportPath { get => _lastExportPath; private set => SetProperty(ref _lastExportPath, value); }
    public int TransactionStartCount { get => _transactionStartCount; private set => SetProperty(ref _transactionStartCount, value); }

    public string FixedLocalExportDirectory
    {
        get => _fixedLocalExportDirectory;
        set
        {
            if (!IsBusy && SetProperty(ref _fixedLocalExportDirectory, value?.Trim() ?? string.Empty))
            {
                OnPropertyChanged(nameof(OutputDirectory));
                OnPropertyChanged(nameof(CanExport));
                NotifyAllCommands();
            }
        }
    }

    public string ProfileText => $"{_readiness.Profile.ProfileId} / v{_readiness.Profile.Version} / 期限 {_readiness.Profile.ExpiresOn:yyyy-MM-dd}";
    public string OutputDirectory => _dualCameraFlow is not null && !IsSingleCameraMode
        ? (string.IsNullOrWhiteSpace(FixedLocalExportDirectory) ? "未選択 — 「選択…」からフォルダを選んでください" : FixedLocalExportDirectory)
        : _readiness.OutputDirectory;

    /// <summary>フォルダ選択ダイアログで選ばれた保存先を受け取る。ネットワーク共有・
    /// リムーバブルメディア・reparse point 先はここで弾く。撮り終えてから保存に失敗すると
    /// 撮り直しになるので、選んだ時点で判定して理由を返す。</summary>
    public void ChangeExportDirectory(string folder)
    {
        if (!CanChangeExportDirectory || string.IsNullOrWhiteSpace(folder))
        {
            return;
        }

        string normalized;
        try
        {
            normalized = Path.GetFullPath(folder);
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(normalized);
        }
        catch (Exception exception) when (exception is IOException or ArgumentException or NotSupportedException or UnauthorizedAccessException or InvalidDataException)
        {
            Notify("この場所は保存先にできません: " + exception.Message, false);
            return;
        }

        FixedLocalExportDirectory = normalized;
        Notify("保存先を設定しました", true);
    }
    public string CameraAStatus => FormatCamera(_readiness.Cameras.Single(camera => camera.Alias == "CAM-A"), CurrentCapturePlan.RequiredCameraAliases.Contains("CAM-A"));
    public string CameraBStatus => FormatCamera(_readiness.Cameras.Single(camera => camera.Alias == "CAM-B"), CurrentCapturePlan.RequiredCameraAliases.Contains("CAM-B"));
    public string SetupStatusText => _readiness.Setup.Summary;
    public string CorrectionText => _readiness.Setup.PlannedCorrections.Count == 0 ? "予定補正なし" : string.Join(" / ", _readiness.Setup.PlannedCorrections);
    public string PhysicalAdjustmentText => _readiness.Setup.PhysicalAdjustments.Count == 0 ? "物理調整なし" : string.Join(" / ", _readiness.Setup.PhysicalAdjustments);
    public string BlockerText => FormatNotices(OperatorWarningSeverity.Blocker, "撮影を止める要因はありません");
    public string CautionText => FormatNotices(OperatorWarningSeverity.Caution, "注意する点はありません");
    public string InfoText => FormatNotices(OperatorWarningSeverity.Info, "原画像はPCに保持 ／ ライブ表示は原画像ではありません ／ 2台のシャッター時刻差は保証しません");
    private bool HasRecoverableHardwareDualTransaction =>
        !IsSingleCameraMode &&
        _dualCameraFlow is
        {
            ExecutionEnvironment: DualCameraExecutionEnvironment.HardwareDual,
            Current: { FailureCode: DualCameraFailureCode.AgentResponseUnknown },
        };

    private bool HasRecoverableCaptureRecoveryOnlyTransaction =>
        IsCaptureRecoveryOnlyMode && _captureRecoveryOnlyWorkflow!.HasPendingRecovery;

    // 新規撮影は、必要な機体照合が Ready になるまで開始できない。Single へのフォールバックも
    // 無い——未確定の binding で 2 台を撮ると、片方の本体の画像がもう片方の alias として
    // 記録され、後から誰も判別できない（ADR-0025）。
    // ordinary HardwareDual の同一撮影ID読み直しは既存host契約どおりbinding gate対象外。
    // CaptureRecoveryOnlyは再起動後の新しいAgentがbinding pipeから始まるため、同一ID照会だけでも
    // CAM-A/Bを再割当してcapture hostへactivationしなければならない（新規撮影は送らない）。
    public bool CanCapture => !_isPreCaptureAutoFocusRunning &&
        (HasRecoverableHardwareDualTransaction ||
        (HasRecoverableCaptureRecoveryOnlyTransaction &&
         (!DualBinding.IsRequired || DualBinding.IsReady) &&
         _availability.Capture.Allowed) ||
        ((!DualBinding.IsRequired || DualBinding.IsReady) &&
        _availability.Capture.Allowed &&
        (IsCaptureRecoveryOnlyMode
            ? !IsSingleCameraMode && IsCaptureRecoveryOnlyOperatorApproved &&
              _captureRecoveryOnlyWorkflow!.CanStartNewCapture
            : IsSingleCameraMode || _dualCameraFlow is null ||
            (_dualCameraFlow.IdentitySnapshot.IsReady &&
             (_dualCameraFlow.ExecutionEnvironment != DualCameraExecutionEnvironment.HardwareDual ||
              _hardwareDualRequestProvider is not null)))));
    public string CaptureDisabledReason => _isPreCaptureAutoFocusRunning
        ? "撮影+AF: 各カメラの撮影直前AFを実行中です。完了までお待ちください。"
        : CanCapture
        ? HasRecoverableHardwareDualTransaction || HasRecoverableCaptureRecoveryOnlyTransaction
            ? "この撮影IDの結果だけを再確認します。新しい撮影は始めません。"
            : "準備完了。確認ダイアログなしで一度だけ開始します。"
        // binding が先に来る。identity が Pending でも、操作者にとっては「まず割当を終わらせる」
        // が次の一手なので、そちらを名指しする。
        : DualBinding.IsRequired && !DualBinding.IsReady
            ? "機体照合（CAM-A / CAM-B の割当）が未完了です — 撮影禁止"
        : HasRecoverableCaptureRecoveryOnlyTransaction
            ? _availability.Capture.DisabledReason
        : IsCaptureRecoveryOnlyMode && !IsCaptureRecoveryOnlyOperatorApproved
            ? "専用カード2枚が空であることと『撮影・回収のみ』の実行承認を確認してください — 撮影禁止"
        : IsCaptureRecoveryOnlyMode && !_captureRecoveryOnlyWorkflow!.CanStartNewCapture
            ? $"CaptureRecoveryOnly開始条件を満たしていません: {_captureRecoveryOnlyWorkflow.NewCaptureBlocker} — 撮影禁止"
        : !IsCaptureRecoveryOnlyMode && !IsSingleCameraMode && _dualCameraFlow is not null && !_dualCameraFlow.IdentitySnapshot.IsReady
            ? $"2台の機体照合: {_dualCameraFlow.IdentitySnapshot.Status} — 撮影禁止"
            : !IsCaptureRecoveryOnlyMode && !IsSingleCameraMode && _dualCameraFlow?.ExecutionEnvironment == DualCameraExecutionEnvironment.HardwareDual &&
              _hardwareDualRequestProvider is null
                ? "HardwareDual approved profiles and explicit operator confirmations are unavailable — 撮影禁止"
            : _availability.Capture.DisabledReason;

    /// <summary>Issue #33 従ボタン「撮影+AF」のゲート。撮影可否そのものは<see cref="CanCapture"/>
    /// を完全に共有し、それに加えて#31の実機フォーカスゲート（<see cref="IsFocusPanelAvailable"/>）
    /// を要求する。HardwareDualでは常にfalseになり、#35 Option Aの「実機モードでは撮影+AFを実行不可」
    /// をこのVMの外へ一切コマンドを出さずに満たす。</summary>
    public bool CanCaptureWithAutoFocus => CanCapture && IsFocusPanelAvailable;

    /// <summary>主ボタンは押せる（<see cref="CanCapture"/>）のに「撮影+AF」だけがHardwareDualゲート
    /// で無効な場合だけ表示する、撮影+AF専用の理由行。両方とも無効なときは共通の
    /// <see cref="CaptureDisabledReason"/> が既に理由を説明しているため、二重表示しない。</summary>
    public bool IsCaptureWithAutoFocusUnavailableReasonVisible => CanCapture && !IsFocusPanelAvailable;

    public string CaptureWithAutoFocusUnavailableReason => FocusPanelUnavailableReason;
    /// <summary>カメラ(C)メニューの「identity状態」項目用（issue #34）の読み取り専用表示。
    /// <see cref="CaptureDisabledReason"/>が既に読んでいる同じ<see cref="IDualCameraProductFlow.IdentitySnapshot"/>
    /// を専用の表示文字列として公開するだけで、新しい業務ロジックは追加しない — 撮影失敗を
    /// 待たずにidentity状態を確認できるようにする目的のみ。</summary>
    public string DualCameraIdentityStatusText => IsCaptureRecoveryOnlyMode
        ? $"同一Agent内の機体照合: {DualBinding.Phase} / 撮影引継ぎ: {(DualBinding.IsCaptureHostActivated ? "完了" : "未実施")}"
        : _dualCameraFlow is null
        ? "DualCamera未接続"
        : IsSingleCameraMode
            ? "1台構成のため対象外"
            : $"機体照合: {_dualCameraFlow.IdentitySnapshot.Status}（{_dualCameraFlow.IdentitySnapshot.ReasonCode}）";

    public bool CanUseLiveView => _availability.LiveView.Allowed;
    public bool CanExport => !IsCaptureRecoveryOnlyMode && _availability.Export.Allowed &&
        (_dualCameraFlow is null || IsSingleCameraMode || Directory.Exists(FixedLocalExportDirectory));
    public bool CanRestitch => !IsCaptureRecoveryOnlyMode && _availability.Restitch.Allowed;
    // _initializationFailed が立っている間は PrepareNewCapture 自体をブロックする。
    // durable journal を一度も読めていない状態で見た目だけ Ready に戻さないための
    // ラッチ（issue #142/PR #152 レビュー指摘・要修正2）。
    public bool CanPrepareNewCapture => _availability.PrepareNewCapture.Allowed && !_initializationFailed;
    public bool CanOpenMaintenance => _availability.OpenMaintenance.Allowed;

    private CapturePlan CurrentCapturePlan => IsSingleCameraMode ? CapturePlan.Single(SelectedCamera) : CapturePlan.Dual();

    public async Task InitializeAsync(CancellationToken cancellationToken)
    {
        _lifetimeToken = cancellationToken;
        UiState = OperatorUiState.CheckingReadiness;
        StatusMessage = "未完了の記録を検査中です。自動再開はしません。";
        IsBusy = true;
        try
        {
            var recovered = await _transactionService.InitializeAsync(cancellationToken).ConfigureAwait(true);
            if (recovered.Count > 0)
            {
                ApplyCaptureResult(recovered[^1]);
                UiState = OperatorUiState.FailedPartial;
                StatusMessage = $"撮影失敗（再開不可）の撮影ID {recovered.Count}件を検出しました。同じ撮影IDは再開しません。";
            }
            else
            {
                UiState = OperatorUiState.AwaitingSafetyAck;
                StatusMessage = "物理シャッターを操作せず、他のカメラアプリを使わないことへ同意してください。";
            }
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            // HardwareSingleCameraViewModel.InitializeAsync の fail-closed catch に倣う
            // （issue #142 症状3）。_transactionService.InitializeAsync は journal 破損時に
            // InvalidDataException 等を投げうるため、ここで確実に捕捉しユーザーへ状態を
            // 伝える。黙って握り潰さず、新規撮影は禁止のまま停止する。
            //
            // _initializationFailed を立てるのは、UiState=FailedPartial だけだと
            // PrepareNewCapture が1クリックで CheckingReadiness→RebuildReadiness 経由の
            // 見た目上の Ready に戻ってしまうため（PrepareNewCaptureAsync は
            // _transactionService.InitializeAsync を再実行しない）。durable journal を
            // 一度も読めていない以上、このラッチで再起動までブロックし続ける
            // （PR #152 レビュー指摘・要修正2）。
            UiState = OperatorUiState.FailedPartial;
            _initializationFailed = true;
            StatusMessage = "起動時の状態確認に失敗しました。fail-closedのため新規撮影はできません。";
            TechnicalDetail = $"error code: {exception.GetType().Name} / {exception.Message}";
        }
        finally
        {
            IsBusy = false;
            RebuildReadiness(preserveOutcomeState: recoveredStateShouldRemain());
        }

        bool recoveredStateShouldRemain() => UiState == OperatorUiState.FailedPartial;
    }

    private void AcceptSafety()
    {
        SafetyAcknowledged = true;
        StatusMessage = "排他使用へ同意しました。撮影できる状態かを確認しました。";
        RebuildReadiness(preserveOutcomeState: UiState == OperatorUiState.FailedPartial);
    }

    private void DeclineSafety()
    {
        StatusMessage = "同意しなかったため撮影は禁止されています。閲覧と終了のみ可能です。";
        // 閲覧はできるようモーダルを畳む。撮影が禁止されたままであることは
        // タイトルバーの未同意表示と Blocker が示し続ける。
        _consentOverlayDismissed = true;
        OnPropertyChanged(nameof(IsConsentOverlayVisible));
        UiState = OperatorUiState.AwaitingSafetyAck;
    }

    private void ShowConsent()
    {
        _consentOverlayDismissed = false;
        OnPropertyChanged(nameof(IsConsentOverlayVisible));
    }

    private async Task RunCaptureAsync(string scenario)
    {
        if (!CanCapture)
        {
            return;
        }

        // 機体照合が確定してからシャッターを切るまでの間にも本体は抜ける。request/response の
        // protocol は誰かが訊ねるまで無効化を伝えられないので、撮影を始める直前にここで訊ねる。
        // ordinary HardwareDualだけは既存hostで同一IDを読める。CaptureRecoveryOnlyはAgent再起動時に
        // capture pipeへ到達するため再binding/activationが必要だが、新規Reserve/Startは送らない。
        if (!HasRecoverableHardwareDualTransaction &&
            !await DualBinding.VerifyBindingIsCurrentAsync().ConfigureAwait(true))
        {
            StatusMessage = "機体照合が無効になりました。撮影は開始していません。";
            Notify(StatusMessage, false);
            OnPropertyChanged(nameof(CanCapture));
            OnPropertyChanged(nameof(CaptureDisabledReason));
            return;
        }

        if (IsCaptureRecoveryOnlyMode)
        {
            if (!await DualBinding.ActivateCaptureAsync(_lifetimeToken).ConfigureAwait(true))
            {
                StatusMessage = "機体照合を撮影処理へ引き継げなかったため、撮影は開始していません。";
                Notify(StatusMessage, false);
                OnPropertyChanged(nameof(CanCapture));
                OnPropertyChanged(nameof(CaptureDisabledReason));
                return;
            }

            await RunCaptureRecoveryOnlyAsync(
                    recoverPending: HasRecoverableCaptureRecoveryOnlyTransaction)
                .ConfigureAwait(true);
            return;
        }

        if (!IsSingleCameraMode && _dualCameraFlow is not null)
        {
            await RunFormalDualCameraCaptureAsync(scenario).ConfigureAwait(true);
            return;
        }

        IsBusy = true;
        var capturePlan = CurrentCapturePlan;
        TransactionStartCount++;
        var transactionId = Guid.NewGuid();
        LastTransactionId = transactionId.ToString("N");
        CaptureResult = "処理中";
        StitchResult = "未実行";
        ExportResult = "未実行";
        _captureOutcome = null;
        _stitchOutcome = null;
        _exportOutcome = null;
        ResetProgress(capturePlan);
        UiState = OperatorUiState.Capturing;
        StatusMessage = $"{scenario}: 操作を受け付けず、表示中のライブ表示を停止します。";

        try
        {
            SetStep("liveview", "current");
            if (scenario == "Live View停止失敗")
            {
                var liveViewFailure = await _transactionService.ExecuteAsync(
                    transactionId,
                    capturePlan,
                    SimulatedWorkflowScenario.FailLiveViewStop,
                    _lifetimeToken).ConfigureAwait(true);
                ApplyCaptureResult(liveViewFailure);
                UiState = OperatorUiState.FailedPartial;
                TechnicalDetail = $"error code: {liveViewFailure.TerminalReason} / capture calls: 0 / automatic retry count: {liveViewFailure.AutomaticRetryCount}";
                StatusMessage = "ライブ表示を安全に停止できなかったため、シャッターを切らず終了しました。";
                return;
            }

            IsLiveViewActive = false;
            SetStep("liveview", "completed");
            SetStep(CaptureStep(capturePlan.RequiredCameraAliases[0]), "current");
            var foundationScenario = scenario switch
            {
                "CAM-A撮影失敗" => SimulatedWorkflowScenario.FailCaptureA,
                "CAM-B撮影失敗" => SimulatedWorkflowScenario.FailCaptureB,
                "対象カメラ撮影失敗" when capturePlan.RequiredCameraAliases[0] == "CAM-A" => SimulatedWorkflowScenario.FailCaptureA,
                "対象カメラ撮影失敗" => SimulatedWorkflowScenario.FailCaptureB,
                "CAM-A保存後クラッシュ" => SimulatedWorkflowScenario.CrashAfterPersistA,
                "1台目保存後クラッシュ" => SimulatedWorkflowScenario.CrashAfterPersistA,
                _ => SimulatedWorkflowScenario.Success,
            };
            var result = await _transactionService.ExecuteAsync(
                transactionId,
                capturePlan,
                foundationScenario,
                _lifetimeToken).ConfigureAwait(true);
            ApplyCaptureResult(result);

            if (!result.IsTerminal)
            {
                UiState = OperatorUiState.FailedPartial;
                StatusMessage = "擬似クラッシュで未完了の記録を残しました。再起動時の検査で撮影失敗（再開不可）として閉じ、再開しません。";
                return;
            }

            if (result.State == SimulatedTransactionState.FailedPartial)
            {
                UiState = OperatorUiState.FailedPartial;
                StatusMessage = $"撮影を撮影失敗（再開不可）で終了しました。{NoRetryMessage(capturePlan)}";
                return;
            }

            MarkCaptureStepsCompleted(capturePlan);
            if (scenario == "cleanup失敗")
            {
                _cameraInspectionRequired = true;
                if (capturePlan.OperatingMode == CameraOperatingMode.SingleCamera)
                {
                    SetStep("stitch", "skipped");
                    StitchResult = "対象外（1台構成）";
                    _stitchOutcome = null;
                }
                else
                {
                    SetStep("stitch", "completed");
                    CreateSuccessfulStitch("cleanup異常あり");
                }
                UiState = OperatorUiState.Review;
                StatusMessage = capturePlan.OperatingMode == CameraOperatingMode.SingleCamera
                    ? "PC単体原本は利用できますが、カード状態を再確認するまで新規撮影は禁止です。"
                    : "PC原本と合成結果は利用できますが、カード状態を再確認するまで新規撮影は禁止です。";
                TechnicalDetail = "error code: EmptyAfterCheckFailed / exact-object cleanup: simulated";
                return;
            }

            if (capturePlan.OperatingMode == CameraOperatingMode.SingleCamera)
            {
                SetStep("stitch", "skipped");
                StitchResult = "対象外（1台構成）";
                _stitchOutcome = null;
                UiState = scenario == "Live View再開失敗" ? OperatorUiState.Degraded : OperatorUiState.Review;
                if (scenario == "Live View再開失敗")
                {
                    _cameraInspectionRequired = true;
                    StatusMessage = "単体の原画像を保持しました。ライブ表示を再開できなかったため、カメラの状態を確認するまで新しい撮影を禁止します。";
                    TechnicalDetail = "error code: LiveViewResumeFailed / single original retained: true";
                }
                else
                {
                    StatusMessage = $"{capturePlan.RequiredCameraAliases[0]}の撮影が完了しました。合成は行わず、検証済み単体原画像を明示保存できます。";
                    TechnicalDetail = "error code: なし / stitch: NotApplicable / automatic retry count: 0";
                }
                return;
            }

            UiState = OperatorUiState.Stitching;
            SetStep("stitch", "current");
            if (scenario == "合成失敗")
            {
                SetStep("stitch", "failure");
                StitchResult = "失敗（撮影原画像は確定済み）";
                _stitchOutcome = new StitchOutcome(Guid.NewGuid(), false, StitchResult, [], "SimulatedStitchFailure");
                LastStitchJobId = _stitchOutcome.StitchJobId.ToString("N");
                UiState = OperatorUiState.Review;
                StatusMessage = "合成に失敗しました。『再合成』は別のstitch jobとして実行できます。";
                TechnicalDetail = "error code: SimulatedStitchFailure";
                return;
            }

            CreateSuccessfulStitch("自動合成完了");
            SetStep("stitch", "completed");
            UiState = scenario == "Live View再開失敗" ? OperatorUiState.Degraded : OperatorUiState.Review;
            if (scenario == "Live View再開失敗")
            {
                _cameraInspectionRequired = true;
                StatusMessage = "撮影・合成結果を保持しました。ライブ表示を再開できなかったため、カメラの状態を確認するまで新しい撮影を禁止します。";
                TechnicalDetail = "error code: LiveViewResumeFailed / result retained: true";
            }
            else
            {
                StatusMessage = "撮影と自動合成が完了しました。結果を確認し、必要なら明示的に保存してください。";
                TechnicalDetail = "error code: なし / automatic retry count: 0";
            }
        }
        catch (OperationCanceledException) when (_lifetimeToken.IsCancellationRequested)
        {
            UiState = OperatorUiState.FailedPartial;
            StatusMessage = "アプリ終了により中断しました。次回起動時に撮影失敗（再開不可）として閉じます。";
        }
        finally
        {
            IsBusy = false;
            RebuildReadiness(preserveOutcomeState: true);
        }
    }

    private async Task RunCaptureRecoveryOnlyAsync(bool recoverPending)
    {
        var workflow = _captureRecoveryOnlyWorkflow ??
            throw new InvalidOperationException("CaptureRecoveryOnly workflow is unavailable.");
        IsBusy = true;
        if (!recoverPending)
        {
            TransactionStartCount++;
        }
        CaptureResult = recoverPending ? "同じ撮影IDの結果確認中" : "CAM-A→CAM-B 撮影・回収中";
        StitchResult = "保留（CaptureRecoveryOnly）";
        ExportResult = "原画像の固定ローカル保存を確認中";
        _captureOutcome = null;
        _stitchOutcome = null;
        _exportOutcome = null;
        ResetProgress(CapturePlan.Dual());
        SetStep("liveview", "completed");
        SetStep("capture-a", "current");
        UiState = OperatorUiState.Capturing;
        StatusMessage = recoverPending
            ? "新しい撮影は行わず、同じtransaction IDの終端結果だけを確認します。"
            : "同じAgentの割当を使い、CAM-A→CAM-Bを各1回だけ撮影・回収します。";

        try
        {
            HardwareDualCaptureRecoveryOnlyExecution outcome;
            if (recoverPending)
            {
                outcome = await workflow.RecoverAsync(_lifetimeToken).ConfigureAwait(true);
            }
            else
            {
                var observedAtUtc = DateTimeOffset.UtcNow;
                var identity = new DualCameraIdentitySnapshot(
                    DualCameraIdentityStatus.Ready,
                    "same_agent_operator_binding",
                    observedAtUtc,
                    observedAtUtc.AddMinutes(5));
                outcome = await workflow.CaptureAsync(identity, _lifetimeToken).ConfigureAwait(true);
            }
            ApplyCaptureRecoveryOnlyExecution(outcome);
        }
        catch (OperationCanceledException) when (_lifetimeToken.IsCancellationRequested)
        {
            UiState = OperatorUiState.FailedPartial;
            CaptureResult = "中断（結果不明）";
            StitchResult = "保留（CaptureRecoveryOnly）";
            ExportResult = "取得済み原画像がある場合は保持";
            StatusMessage = "終了操作で中断しました。自動再試行せず、次回は同じ撮影IDの結果だけを確認します。";
            TechnicalDetail = "capturePurpose=CaptureRecoveryOnly / stitchOutcome=Pending / a0QualityApproval=Unapproved / automatic retry count: 0";
        }
        finally
        {
            IsBusy = false;
            RebuildReadiness(preserveOutcomeState: true);
        }
    }

    private void ApplyCaptureRecoveryOnlyExecution(HardwareDualCaptureRecoveryOnlyExecution outcome)
    {
        LastTransactionId = outcome.TransactionId == Guid.Empty
            ? "未実行"
            : outcome.TransactionId.ToString("N");
        var originals = outcome.Originals;
        RetainedOriginals = originals.Count == 0
            ? "なし"
            : string.Join(" / ", originals.Select(original =>
                $"{original.Alias}: original.jpg {original.SizeBytes} bytes SHA-256 {original.Sha256[..12]}…"));
        foreach (var original in originals)
        {
            _lastCapturedOriginalTimestamps[original.Alias] = DateTimeOffset.UtcNow;
            RecordSavedFile(original.Path);
            SetStep(original.Alias == "CAM-A" ? "capture-a" : "capture-b", "completed");
            SetStep(original.Alias == "CAM-A" ? "persist-a" : "persist-b", "completed");
        }

        if (outcome.Succeeded)
        {
            SetStep("capture-a", "completed");
            SetStep("persist-a", "completed");
            SetStep("capture-b", "completed");
            SetStep("persist-b", "completed");
        }
        else if (!outcome.RecoveryPending)
        {
            var failedStep = originals.Any(item => item.Alias == "CAM-A")
                ? "capture-b"
                : "capture-a";
            SetStep(failedStep, "failure");
        }
        SetStep("stitch", "skipped");
        SetStep("review", outcome.RecoveryPending ? "pending" : "completed");
        SetStep("export", "skipped");

        _captureOutcome = new CaptureOutcome(
            outcome.TransactionId,
            CapturePlan.Dual(),
            outcome.Succeeded ? SimulatedTransactionState.Complete : SimulatedTransactionState.FailedPartial,
            originals.Select(original => original.Alias).ToArray(),
            outcome.Succeeded ? "二原本検証済み" : "撮影・回収または原画像検証が未完了",
            outcome.FailureCode == DualCameraFailureCode.None ? null : outcome.FailureCode.ToString(),
            DateTimeOffset.UtcNow);
        CaptureResult = outcome.Succeeded
            ? "CAM-A/CAM-B JPEG 7360×4912・SHA-256再検証済み"
            : outcome.RecoveryPending
                ? "結果不明（同じ撮影IDのみ再確認可）"
                : $"{outcome.TerminalState}: {outcome.FailureCode}";
        StitchResult = "Pending（未実施・A0品質未承認）";
        ExportResult = originals.Count == 0
            ? "保存済み原画像なし"
            : "原画像を固定ローカルtransactionフォルダへ保存済み";
        LastExportPath = string.IsNullOrWhiteSpace(outcome.TransactionDirectory)
            ? "未確定"
            : outcome.TransactionDirectory;
        _stitchOutcome = null;
        _exportOutcome = null;
        UiState = outcome.Succeeded ? OperatorUiState.Review : OperatorUiState.FailedPartial;
        _cameraInspectionRequired = !outcome.Succeeded && !outcome.RecoveryPending;
        StatusMessage = outcome.Succeeded
            ? "原画像2枚の撮影・回収・再検証が完了しました。合成は実施せず、A0品質は未承認です。"
            : outcome.RecoveryPending
                ? "結果が確定していません。新しい撮影や自動再試行は行わず、同じ撮影IDだけを再確認します。"
                : $"撮影・回収を{outcome.TerminalState}で停止しました。取得済み原画像は保持しています。";
        TechnicalDetail =
            $"capturePurpose={HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose} / " +
            $"stitchOutcome={HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome} / " +
            $"a0QualityApproval={HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval} / " +
            $"automatic retry count: {outcome.AutomaticRetryCount} / failure={outcome.FailureCode}";
        OnPropertyChanged(nameof(StageCompositeFreshnessText));
        RaiseLoupeProperties();
        RecalculateAvailability();
    }

    private async Task RunFormalDualCameraCaptureAsync(string scenario)
    {
        var flow = _dualCameraFlow ?? throw new InvalidOperationException("DualCamera product flow is unavailable.");
        var recoveringUnknownTransaction =
            flow.ExecutionEnvironment == DualCameraExecutionEnvironment.HardwareDual &&
            flow.Current is { FailureCode: DualCameraFailureCode.AgentResponseUnknown };
        IsBusy = true;
        if (!recoveringUnknownTransaction)
        {
            TransactionStartCount++;
        }
        CaptureResult = "DualCamera撮影処理中";
        StitchResult = "未実行";
        ExportResult = "未実行";
        _captureOutcome = null;
        _stitchOutcome = null;
        _exportOutcome = null;
        ResetProgress(CapturePlan.Dual());
        SetStep("liveview", scenario == "Live View停止失敗" ? "current" : "completed");
        UiState = OperatorUiState.Capturing;
        StatusMessage = "CAM-A→CAM-Bを一回ずつ撮影し、それぞれの原画像を確認します。";
        try
        {
            DualCameraCaptureRequest? request = null;
            if (!recoveringUnknownTransaction)
            {
                request = flow.ExecutionEnvironment == DualCameraExecutionEnvironment.HardwareDual
                    ? (_hardwareDualRequestProvider?.Invoke() ??
                        throw new DualCameraFlowException(DualCameraFailureCode.HardwarePending, "HardwareDual approved profiles and operator confirmations are unavailable."))
                    : DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()) with
                    {
                        TestFault = scenario switch
                        {
                            "Live View停止失敗" => DualCameraTestFault.FailBeforeCapture,
                            "CAM-A撮影失敗" => DualCameraTestFault.FailCaptureCameraA,
                            "CAM-B撮影失敗" => DualCameraTestFault.FailCaptureCameraB,
                            "CAM-A保存後クラッシュ" => DualCameraTestFault.InterruptAfterCameraA,
                            "合成失敗" => DualCameraTestFault.FailStitch,
                            _ => DualCameraTestFault.None,
                        },
                    };
            }
            var state = recoveringUnknownTransaction
                ? await flow.RecoverAndStitchAsync(flow.Current!.TransactionId, _lifetimeToken).ConfigureAwait(true)
                : await flow.CaptureAndStitchAsync(request!, _lifetimeToken).ConfigureAwait(true);
            ApplyFormalDualCameraState(state);
            if (scenario == "Live View停止失敗")
            {
                SetStep("liveview", "failure");
            }
            else if (state.FailureCode == DualCameraFailureCode.None && scenario is "cleanup失敗" or "Live View再開失敗")
            {
                _cameraInspectionRequired = true;
                UiState = OperatorUiState.Degraded;
                StatusMessage = scenario == "cleanup失敗"
                    ? "実JPEG製品結果は保持しましたが、TestSynthetic cleanup確認失敗として新規撮影を禁止します。"
                    : "実JPEGの結果は保持しましたが、ライブ表示を再開できなかったため新しい撮影を禁止します。";
            }
        }
        finally
        {
            IsBusy = false;
            RebuildReadiness(preserveOutcomeState: true);
        }
    }

    private void OnDualCameraStateChanged(object? sender, DualCameraProductState state)
    {
        if (_synchronizationContext is not null && SynchronizationContext.Current != _synchronizationContext)
        {
            _synchronizationContext.Post(_ => ApplyFormalDualCameraState(state), null);
            return;
        }
        ApplyFormalDualCameraState(state);
    }

    // 機体照合が Ready/未 Ready を跨いだ瞬間に撮影ボタンの可否が変わる。ここで拾わないと、
    // binding 完了後もボタンが無効なまま残る。
    private void OnDualBindingChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs args)
    {
        if (args.PropertyName is not (nameof(DualBindingViewModel.IsReady)
            or nameof(DualBindingViewModel.IsRequired)
            or nameof(DualBindingViewModel.IsCaptureHostActivated)
            or nameof(DualBindingViewModel.Phase)))
        {
            return;
        }

        if (_synchronizationContext is not null && SynchronizationContext.Current != _synchronizationContext)
        {
            _synchronizationContext.Post(_ => NotifyBindingGateChanged(), null);
            return;
        }

        NotifyBindingGateChanged();
    }

    private void NotifyBindingGateChanged()
    {
        OnPropertyChanged(nameof(CanCapture));
        OnPropertyChanged(nameof(CaptureDisabledReason));
        OnPropertyChanged(nameof(DualCameraIdentityStatusText));
        _showBindingDemoCommand.NotifyCanExecuteChanged();
        _captureCommand.NotifyCanExecuteChanged();
        _captureWithAutoFocusCommand.NotifyCanExecuteChanged();
        _diagnosticCommand.NotifyCanExecuteChanged();
    }

    private void OnDualCameraIdentityChanged(object? sender, DualCameraIdentitySnapshot snapshot)
    {
        _ = snapshot;
        if (_synchronizationContext is not null && SynchronizationContext.Current != _synchronizationContext)
        {
            _synchronizationContext.Post(_ => RebuildReadiness(preserveOutcomeState: true), null);
            return;
        }
        RebuildReadiness(preserveOutcomeState: true);
    }

    private void OnSimulatedFrameTick(object? sender, SimulatedLiveViewFrameTick tick)
    {
        if (_synchronizationContext is not null && SynchronizationContext.Current != _synchronizationContext)
        {
            // A callback posted through SynchronizationContext.Post that throws becomes an
            // unhandled Dispatcher exception in WPF (it does not propagate back to the
            // caller), so ApplySimulatedFrameTick must never throw — it wraps its own body in
            // try/catch below, precisely so this posted lambda cannot surface an exception.
            _synchronizationContext.Post(_ => ApplySimulatedFrameTick(tick), null);
            return;
        }
        ApplySimulatedFrameTick(tick);
    }

    /// <summary>
    /// Turns one pump tick into an actual rendered frame (the WPF rendering call itself
    /// happens here, on whichever thread this runs on — the UI thread once marshalled via
    /// SynchronizationContext.Post) and applies it, or drops it. A tick is dropped — without
    /// throwing — when: Live View is OFF, the tick's camera alias no longer matches the
    /// selected camera, the tick's generation no longer matches the current Live View
    /// session's generation (guards the OFF-then-back-ON-for-the-same-alias race, which alias
    /// and IsLiveViewActive alone cannot detect), the frame source produced a frame missing
    /// the Simulated marker, or rendering itself threw.
    /// </summary>
    private void ApplySimulatedFrameTick(SimulatedLiveViewFrameTick tick)
    {
        try
        {
            if (!IsLiveViewActive ||
                tick.Generation != _currentLiveViewFrameGeneration ||
                !string.Equals(tick.CameraAlias, SelectedCamera, StringComparison.Ordinal))
            {
                return;
            }

            var frameSource = _liveViewFrameSource;
            if (frameSource is null)
            {
                return;
            }

            var frame = frameSource.CreateFrame(tick.CameraAlias, tick.Pattern, tick.SequenceNumber, tick.CapturedAtUtc);
            if (!frame.Simulation || !string.Equals(frame.Marker, "Simulated", StringComparison.Ordinal))
            {
                StatusMessage = "SIMULATED live view frame sourceがSimulated markerのないフレームを返したため破棄しました。";
                return;
            }

            _lastLiveFrames[frame.CameraAlias] = frame;
            _lastLiveFrameTimestamps[frame.CameraAlias] = frame.CapturedAtUtc;
            _lastLiveFrameBlurRadius[frame.CameraAlias] = frame.BlurRadius;
            RaiseStageFrameProperties();
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            StatusMessage = $"SIMULATED live view frameの生成に失敗したため破棄しました: {exception.GetType().Name}.";
        }
    }

    private void ApplyFormalDualCameraState(DualCameraProductState state)
    {
        if (state.Mode != CameraOperatingMode.DualCamera || _dualCameraFlow is null ||
            state.ExecutionEnvironment != _dualCameraFlow.ExecutionEnvironment)
        {
            throw new InvalidDataException("Formal WPF accepts only the configured typed DualCamera execution environment.");
        }

        LastTransactionId = state.TransactionId == Guid.Empty ? "未実行" : state.TransactionId.ToString("N");
        foreach (var stage in state.Stages)
        {
            var stepId = stage.Stage switch
            {
                DualCameraProductStage.CaptureCameraA => "capture-a",
                DualCameraProductStage.ValidateCameraA => "persist-a",
                DualCameraProductStage.CaptureCameraB => "capture-b",
                DualCameraProductStage.ValidateCameraB => "persist-b",
                DualCameraProductStage.Stitch => "stitch",
                DualCameraProductStage.Review => "review",
                DualCameraProductStage.Export => "export",
                _ => null,
            };
            if (stepId is null) continue;
            SetStep(stepId, stage.Status switch
            {
                DualCameraStageStatus.Active => "current",
                DualCameraStageStatus.Succeeded => "completed",
                DualCameraStageStatus.Failed => "failure",
                _ => "pending",
            });
        }

        var originals = state.Capture?.Originals ?? [];
        RetainedOriginals = originals.Count == 0
            ? "なし"
            : string.Join(" / ", originals.Select(original =>
                $"{original.Alias}: original.jpg {original.SizeBytes} bytes SHA-256 {original.Sha256[..12]}…"));
        foreach (var original in originals)
        {
            _lastCapturedOriginalTimestamps[original.Alias] = DateTimeOffset.UtcNow;
        }
        OnPropertyChanged(nameof(StageCompositeFreshnessText));
        RaiseLoupeProperties();
        if (state.Capture is not null)
        {
            _captureOutcome = new CaptureOutcome(
                state.TransactionId,
                CapturePlan.Dual(),
                state.Capture.Succeeded ? SimulatedTransactionState.Complete : SimulatedTransactionState.FailedPartial,
                originals.Select(original => original.Alias).ToArray(),
                state.Capture.Succeeded ? "二原本検証済み" : "撮影または原本検証失敗",
                state.Capture.FailureCode == DualCameraFailureCode.None ? null : state.Capture.FailureCode.ToString(),
                DateTimeOffset.UtcNow);
            CaptureResult = state.Capture.Succeeded ? "CAM-A/CAM-B canonical JPEG検証済み" : "FailedPartial";
        }

        if (state.Stitch is not null)
        {
            LastStitchJobId = state.Stitch.JobId.ToString("N");
            StitchResult = state.Stitch.Succeeded
                ? $"実JPEG合成完了 / 別job {state.Stitch.JobId:N} / stitched.jpg"
                : $"合成失敗 / job {state.Stitch.JobId:N}: {state.Stitch.FailureReason}";
            _stitchOutcome = new StitchOutcome(
                state.Stitch.JobId,
                state.Stitch.Succeeded,
                StitchResult,
                _readiness.Setup.PlannedCorrections,
                state.Stitch.Succeeded ? null : state.Stitch.FailureCode.ToString());
        }

        if (state.Export is not null)
        {
            LastExportPath = state.Export.OutputPath ?? "未公開";
            ExportResult = state.Export.Succeeded
                ? "このPCのフォルダへ保存しました（画像は無加工）"
                : $"export失敗: {state.Export.FailureReason}";
            _exportOutcome = new ExportOutcome(
                state.Export.JobId,
                state.Export.Succeeded,
                state.Export.OutputPath,
                ExportResult,
                state.Export.Succeeded ? null : state.Export.FailureCode.ToString());

            // 直近のexportはこの後の状態（再合成など）へも引き継がれるため、
            // job IDが変わったときだけ通知する。そうしないと再合成のたびに
            // 保存していないのに「保存しました」と出てしまう。
            if (_lastNotifiedExportJobId != state.Export.JobId)
            {
                _lastNotifiedExportJobId = state.Export.JobId;
                if (state.Export.Succeeded)
                {
                    RecordSavedFile(state.Export.OutputPath ?? string.Empty);
                    Notify("このPCのフォルダへ保存しました（画像は無加工）", true);
                }
                else
                {
                    Notify("保存できませんでした。撮影データは保持しています", false);
                }
            }
        }

        var activeStage = state.Stages.FirstOrDefault(stage => stage.Status == DualCameraStageStatus.Active)?.Stage;
        UiState = activeStage switch
        {
            DualCameraProductStage.Stitch => OperatorUiState.Stitching,
            DualCameraProductStage.Review or DualCameraProductStage.Export => OperatorUiState.Review,
            _ when state.IsActive => OperatorUiState.Capturing,
            _ => state.FailureCode switch
            {
                DualCameraFailureCode.None => OperatorUiState.Review,
                DualCameraFailureCode.StitchFailed or DualCameraFailureCode.ExportFailed => OperatorUiState.Review,
                _ => OperatorUiState.FailedPartial,
            },
        };
        StatusMessage = state.IsActive
            ? activeStage is null ? "DualCamera product flow処理中" : $"進行中: {activeStage}"
            : state.FailureCode == DualCameraFailureCode.None
                ? "撮影・原本検証・合成が完了しました。結果確認後に明示exportできます。"
                : $"{state.FailureCode}: {state.FailureReason}";
        TechnicalDetail = $"mode={state.Mode} / execution={state.ExecutionEnvironment} / profile={state.ProfileId} v{state.ProfileVersion} / automatic retry count: {state.AutomaticRetryCount} / failure={state.FailureCode}";
        RecalculateAvailability();
    }

    private Task PrepareNewCaptureAsync()
    {
        UiState = OperatorUiState.CheckingReadiness;
        StatusMessage = "read-onlyで接続・identity・profile・設置・カード・保存先を再検査しました。";
        _cameraInspectionRequired = false;
        ResetProgress(CurrentCapturePlan);
        CaptureResult = "新しい撮影待ち（過去画像は再利用しません）";
        StitchResult = "未実行";
        ExportResult = "未実行";
        _stitchOutcome = null;
        _exportOutcome = null;
        _captureOutcome = null;
        _lastCapturedOriginalTimestamps.Clear();
        _lastLiveFrameTimestamps.Clear();
        _lastLiveFrames.Clear();
        _lastLiveFrameBlurRadius.Clear();
        _focusFixed["CAM-A"] = false;
        _focusFixed["CAM-B"] = false;
        _focusPositionValues.Clear();
        _lastFocusResult = null;
        LastPreCaptureAutoFocusResult = null;
        if (IsCaptureRecoveryOnlyMode)
        {
            _captureRecoveryOnlyOperatorApproved = false;
            OnPropertyChanged(nameof(IsCaptureRecoveryOnlyOperatorApproved));
        }
        RaiseStageFrameProperties();
        RaiseFocusPanelProperties();
        OnPropertyChanged(nameof(FocusResultText));
        OnPropertyChanged(nameof(CameraAFocusStatusText));
        OnPropertyChanged(nameof(CameraBFocusStatusText));
        OnPropertyChanged(nameof(LastPreCaptureAutoFocusResult));
        RebuildReadiness();
        return Task.CompletedTask;
    }

    private void ToggleLiveView()
    {
        IsLiveViewActive = !IsLiveViewActive;
        StatusMessage = IsLiveViewActive
            ? $"{SelectedCamera} のSimulated Live Viewを開始しました。プレビューは非原画像です。"
            : $"{SelectedCamera} のSimulated Live Viewを停止しました。";
    }

    private async Task RestitchAsync()
    {
        if (_dualCameraFlow is not null && _captureOutcome?.CapturePlan.OperatingMode == CameraOperatingMode.DualCamera)
        {
            IsBusy = true;
            UiState = OperatorUiState.Stitching;
            SetStep("stitch", "current");
            StatusMessage = "同じ二原本から別のstitch jobを開始しました。";
            try
            {
                var state = await _dualCameraFlow.RestitchAsync(_lifetimeToken).ConfigureAwait(true);
                ApplyFormalDualCameraState(state);
            }
            finally
            {
                IsBusy = false;
                RebuildReadiness(preserveOutcomeState: true);
            }
            return;
        }

        var jobId = Guid.NewGuid();
        LastStitchJobId = jobId.ToString("N");
        StitchResult = $"別jobで再合成成功 ({jobId:N})";
        _stitchOutcome = new StitchOutcome(jobId, true, StitchResult, _readiness.Setup.PlannedCorrections, null);
        UiState = OperatorUiState.Review;
        StatusMessage = "保持済みの左右原画像から、新しいstitch jobとして再合成しました。撮影transactionは変更していません。";
        RebuildReadiness(preserveOutcomeState: true);
        await Task.CompletedTask;
    }

    private async Task ExportAsync()
    {
        if (_dualCameraFlow is not null && _captureOutcome?.CapturePlan.OperatingMode == CameraOperatingMode.DualCamera)
        {
            IsBusy = true;
            StatusMessage = "このPCの選んだフォルダへ保存しています。";
            try
            {
                var state = await _dualCameraFlow.ExportAsync(FixedLocalExportDirectory, _lifetimeToken).ConfigureAwait(true);
                ApplyFormalDualCameraState(state);
            }
            finally
            {
                IsBusy = false;
                RebuildReadiness(preserveOutcomeState: true);
            }
            return;
        }

        var exportId = Guid.NewGuid();
        var exportDirectory = Path.Combine(Path.GetTempPath(), "A0CameraStitcher", "simulated-exports");
        Directory.CreateDirectory(exportDirectory);
        var outputPath = Path.Combine(exportDirectory, $"{exportId:N}.simulated-export.txt");
        var singleOutput = _captureOutcome?.CapturePlan.OperatingMode == CameraOperatingMode.SingleCamera;
        var resultKind = singleOutput ? "single-canonical-original" : "stitched-result";
        File.WriteAllText(outputPath, $"Simulated export only{Environment.NewLine}transaction={LastTransactionId}{Environment.NewLine}resultKind={resultKind}{Environment.NewLine}stitchJob={LastStitchJobId}");
        var message = singleOutput
            ? "明示操作でSIMULATED単体原画像を保存しました（JPEGではありません）"
            : "明示操作でSIMULATED合成出力を保存しました（JPEGではありません）";
        _exportOutcome = new ExportOutcome(exportId, true, outputPath, message, null);
        LastExportPath = _exportOutcome.OutputPath ?? "未実行";
        ExportResult = _exportOutcome.OperatorMessage;
        StatusMessage = "保存が完了しました。原画像を上書き・削除していません。";
        RecordSavedFile(LastExportPath);
        Notify(message, true);
        RecalculateAvailability();
        await Task.CompletedTask;
    }

    private void ApplyCaptureResult(SimulatedWorkflowState result)
    {
        if (!result.Simulation || !string.Equals(result.Marker, "Simulated", StringComparison.Ordinal))
        {
            throw new InvalidDataException("実機非接続shellはSimulated markerのない結果を表示できません。");
        }

        var resultPlan = new CapturePlan
        {
            OperatingMode = result.OperatingMode,
            RequiredCameraAliases = result.RequiredCameraAliases,
        };
        resultPlan.Validate();
        _selectedOperatingMode = resultPlan.OperatingMode == CameraOperatingMode.SingleCamera
            ? SingleModeLabel
            : DualModeLabel;
        if (resultPlan.OperatingMode == CameraOperatingMode.SingleCamera)
        {
            _selectedCamera = resultPlan.RequiredCameraAliases[0];
        }
        OnPropertyChanged(nameof(SelectedOperatingMode));
        OnPropertyChanged(nameof(SelectedCamera));
        OnPropertyChanged(nameof(IsSingleCameraMode));
        OnPropertyChanged(nameof(OperatingModeDescription));
        OnPropertyChanged(nameof(CaptureButtonText));
        OnPropertyChanged(nameof(CameraSelectionLabel));
        OnPropertyChanged(nameof(ProcessingResultLabel));
        OnPropertyChanged(nameof(DiagnosticScenarios));
        OnPropertyChanged(nameof(StageCompositeApplicable));
        OnPropertyChanged(nameof(StageReviewBadgeText));
        OnPropertyChanged(nameof(StageSingleLiveAliasInPlan));
        OnPropertyChanged(nameof(StageSingleLiveText));

        LastTransactionId = result.TransactionId.ToString("N");
        CaptureResult = result.State.ToString();
        RetainedOriginals = result.RetainedOriginalAliases.Count == 0 ? "なし" : string.Join(", ", result.RetainedOriginalAliases) + "（simulated原画像）";
        foreach (var alias in result.RetainedOriginalAliases)
        {
            _lastCapturedOriginalTimestamps[alias] = DateTimeOffset.UtcNow;
        }
        OnPropertyChanged(nameof(StageCompositeFreshnessText));
        RaiseLoupeProperties();
        _captureOutcome = new CaptureOutcome(
            result.TransactionId,
            resultPlan,
            result.State,
            result.RetainedOriginalAliases,
            CaptureResult,
            result.TerminalReason,
            DateTimeOffset.Now);
        ApplyProgressFromResult(result);
        TechnicalDetail = $"error code: {result.TerminalReason ?? "なし"} / transaction: {result.TransactionId:N}";
    }

    private void ApplyProgressFromResult(SimulatedWorkflowState result)
    {
        var resultPlan = new CapturePlan
        {
            OperatingMode = result.OperatingMode,
            RequiredCameraAliases = result.RequiredCameraAliases,
        };
        resultPlan.Validate();
        ResetProgress(resultPlan);
        if (string.Equals(result.TerminalReason, "LiveViewStopFailed", StringComparison.Ordinal))
        {
            SetStep("liveview", "failure");
            return;
        }

        SetStep("liveview", "completed");
        if (result.RetainedOriginalAliases.Contains("CAM-A"))
        {
            SetStep("capture-a", "completed");
            SetStep("persist-a", "completed");
        }
        if (result.RetainedOriginalAliases.Contains("CAM-B"))
        {
            SetStep("capture-b", "completed");
            SetStep("persist-b", "completed");
        }
        if (result.State == SimulatedTransactionState.FailedPartial)
        {
            var failedAlias = resultPlan.RequiredCameraAliases
                .FirstOrDefault(alias => !result.RetainedOriginalAliases.Contains(alias, StringComparer.Ordinal));
            if (failedAlias is not null)
            {
                SetStep(CaptureStep(failedAlias), "failure");
            }
        }
    }

    private void MarkCaptureStepsCompleted(CapturePlan capturePlan)
    {
        SetStep("liveview", "completed");
        foreach (var alias in capturePlan.RequiredCameraAliases)
        {
            SetStep(CaptureStep(alias), "completed");
            SetStep(PersistStep(alias), "completed");
        }
    }

    private void CreateSuccessfulStitch(string description)
    {
        var jobId = Guid.NewGuid();
        LastStitchJobId = jobId.ToString("N");
        StitchResult = $"{description} / job {jobId:N}";
        _stitchOutcome = new StitchOutcome(jobId, true, StitchResult, _readiness.Setup.PlannedCorrections, null);
    }

    private void RebuildReadiness(bool preserveOutcomeState = false)
    {
        var capturePlan = CurrentCapturePlan;
        var setup = IsSingleCameraMode
            ? SelectedReadinessDemo switch
            {
                "物理調整が必要" => new SetupAssessment(SetupAssessmentStatus.PhysicalAdjustmentRequired, "選択カメラの物理調整が必要 — 撮影禁止", [], [$"{SelectedCamera}の原稿範囲を調整"]),
                _ => new SetupAssessment(SetupAssessmentStatus.Ready, "1台構成 — 合成補正なし・撮影可能", [], []),
            }
            : SelectedReadinessDemo switch
        {
            "補正不要" => new SetupAssessment(SetupAssessmentStatus.Ready, "補正不要 — 撮影可能", [], []),
            "物理調整が必要" => new SetupAssessment(SetupAssessmentStatus.PhysicalAdjustmentRequired, "物理調整が必要 — 撮影禁止", [], ["CAM-Bを左へ2.4 mm", "時計回りに0.8°"]),
            _ => new SetupAssessment(SetupAssessmentStatus.ReadyWithCorrection, "自動補正範囲内 — 撮影可能", ["位置 +0.7 mm", "回転 -0.2°", "露出 +0.1 EV"], []),
        };
        var camAConnected = IsSingleCameraMode
            ? SelectedCamera == "CAM-A" && SelectedReadinessDemo != "CAM-A未接続"
            : SelectedReadinessDemo != "CAM-A未接続";
        var camBConnected = IsSingleCameraMode
            ? SelectedCamera == "CAM-B" && SelectedReadinessDemo != "CAM-B未接続"
            : SelectedReadinessDemo != "CAM-B未接続";
        var cardsKnownEmpty = SelectedReadinessDemo != "カード状態要確認" && !_cameraInspectionRequired;
        var outputDirectory = Path.Combine(Path.GetTempPath(), "A0CameraStitcher", "simulated-exports");
        // Focus-not-fixed is Caution-only, never a Blocker (撮影のハードゲートにしない):
        // OperatorReadinessEvaluator.BuildNotices concatenates snapshot.Notices verbatim and its
        // Blocker gate is what CanCapture ultimately checks, so a Caution severity here cannot
        // by itself disable capture — see docs/OPERATOR_UI_SPEC.md's フォーカス操作 section.
        var focusNotices = capturePlan.RequiredCameraAliases
            .Where(alias => !_focusFixed.TryGetValue(alias, out var fixedState) || !fixedState)
            .Select(alias => new OperatorNotice(
                OperatorWarningSeverity.Caution,
                "FocusNotFixed",
                $"{alias}: フォーカス未固定です。AF実行後の固定、または手動調整の確認を推奨します。"))
            .ToArray();
        _readiness = new ReadinessSnapshot
        {
            SafetyAcknowledged = SafetyAcknowledged,
            CapturePlan = capturePlan,
            Cameras =
            [
                new("CAM-A", camAConnected, camAConnected, true, cardsKnownEmpty, IsLiveViewActive && SelectedCamera == "CAM-A"),
                new("CAM-B", camBConnected, camBConnected, true, cardsKnownEmpty, IsLiveViewActive && SelectedCamera == "CAM-B"),
            ],
            Profile = new(IsSingleCameraMode ? $"SINGLE-SIM-{SelectedCamera}" : "RIG-SIM-A0", "0.4", new DateOnly(2027, 3, 31), true, true),
            Setup = setup,
            OutputDirectory = outputDirectory,
            OutputDirectoryValid = true,
            HasActiveTransaction = IsBusy,
            CameraStateRequiresInspection = _cameraInspectionRequired,
            Notices = focusNotices,
        };
        RecalculateAvailability();
        // _initializationFailed のときは preserveOutcomeState の値によらず自動遷移させない
        // （PR #152 レビュー指摘・要修正2の再差し戻し）。preserveOutcomeState だけに頼ると、
        // ここを preserveOutcomeState: false（既定）で呼ぶ通常のUI操作（運用構成/カメラ選択/
        // 確認シナリオのコンボ/Live View切替/オートフォーカス等、いずれもゲート無しか
        // IsBusy 程度のゲートしか持たない setter 経由）のたびに GetReadyState が再計算され、
        // durable journal を一度も読めていないのに UiState が Ready 系へ書き換わってしまう
        // （Capturing/Stitching と同様、_initializationFailed もここで凍結する）。
        if (!preserveOutcomeState && !_initializationFailed &&
            UiState is not (OperatorUiState.Capturing or OperatorUiState.Stitching))
        {
            UiState = OperatorReadinessEvaluator.GetReadyState(_readiness, DateOnly.FromDateTime(DateTime.Today));
        }
        RaiseReadinessProperties();
    }

    private void RecalculateAvailability()
    {
        if (_readiness is null) return;
        var outcomePlan = _captureOutcome?.CapturePlan;
        var hasRequiredOriginals = _captureOutcome is not null && outcomePlan is not null &&
            outcomePlan.RequiredCameraAliases.All(alias =>
                _captureOutcome.RetainedOriginalAliases.Contains(alias, StringComparer.Ordinal));
        var hasExportableResult = outcomePlan?.OperatingMode == CameraOperatingMode.SingleCamera
            ? hasRequiredOriginals
            : _stitchOutcome?.Succeeded == true;
        var canRestitch = outcomePlan?.OperatingMode == CameraOperatingMode.DualCamera && hasRequiredOriginals;
        _availability = OperatorReadinessEvaluator.Evaluate(
            _readiness,
            UiState,
            DateOnly.FromDateTime(DateTime.Today),
            hasExportableResult,
            canRestitch);
        OnPropertyChanged(nameof(CanCapture));
        OnPropertyChanged(nameof(CanCaptureWithAutoFocus));
        OnPropertyChanged(nameof(IsCaptureWithAutoFocusUnavailableReasonVisible));
        OnPropertyChanged(nameof(CaptureAvailabilityText));
        OnPropertyChanged(nameof(ReadyStatusChipText));
        OnPropertyChanged(nameof(CaptureDisabledReason));
        OnPropertyChanged(nameof(CanUseLiveView));
        OnPropertyChanged(nameof(CanExport));
        OnPropertyChanged(nameof(CanRestitch));
        OnPropertyChanged(nameof(CanPrepareNewCapture));
        OnPropertyChanged(nameof(CanOpenMaintenance));
        OnPropertyChanged(nameof(CanChangeOperatingMode));
        OnPropertyChanged(nameof(CanSelectCamera));
        NotifyAllCommands();
    }

    private void RaiseReadinessProperties()
    {
        foreach (var name in new[] { nameof(ProfileText), nameof(OutputDirectory), nameof(CameraAStatus), nameof(CameraBStatus), nameof(SetupStatusText), nameof(CorrectionText), nameof(PhysicalAdjustmentText), nameof(BlockerText), nameof(CautionText), nameof(InfoText), nameof(OperatingModeDescription), nameof(CaptureButtonText), nameof(ProcessingResultLabel), nameof(StageCompositeFreshnessText), nameof(StageSingleLiveText), nameof(StageSingleLiveAliasInPlan), nameof(StageCompositeApplicable), nameof(StageReviewBadgeText), nameof(DualCameraIdentityStatusText) }) OnPropertyChanged(name);
    }

    /// <summary>
    /// Refreshes every stage Image/placeholder-visibility binding at once. Called whenever
    /// something that feeds those computed properties changes: Live View on/off, the
    /// selected camera or stage mode, a newly produced SIMULATED frame, or a state reset.
    /// </summary>
    private void RaiseStageFrameProperties()
    {
        OnPropertyChanged(nameof(StageSingleLiveImage));
        OnPropertyChanged(nameof(IsStageSingleLiveImageVisible));
        OnPropertyChanged(nameof(IsStageSingleLivePlaceholderVisible));
        OnPropertyChanged(nameof(StageCompositeLiveImage));
        OnPropertyChanged(nameof(IsStageCompositeLiveImageVisible));
        OnPropertyChanged(nameof(IsStageCompositeLivePlaceholderVisible));
        OnPropertyChanged(nameof(StageCompositeStillImage));
        OnPropertyChanged(nameof(IsStageCompositeStillImageVisible));
        OnPropertyChanged(nameof(IsStageCompositeStillPlaceholderVisible));
        OnPropertyChanged(nameof(StageCompositeFreshnessText));
        OnPropertyChanged(nameof(StageSingleLivePeakingOverlay));
        OnPropertyChanged(nameof(IsStageSingleLivePeakingOverlayVisible));
        OnPropertyChanged(nameof(StageCompositeLivePeakingOverlay));
        OnPropertyChanged(nameof(IsStageCompositeLivePeakingOverlayVisible));
        OnPropertyChanged(nameof(TiltRollDegrees));
        OnPropertyChanged(nameof(TiltRollDegreesText));
        OnPropertyChanged(nameof(TiltToleranceChipText));
        RaiseLoupeProperties();
    }

    /// <summary>Refreshes every loupe binding at once. Called by <see cref="RaiseStageFrameProperties"/>
    /// (so any change that could move a stage image also refreshes the loupe crop of it) and
    /// directly by the target-position and zoom setters (which do not otherwise touch the
    /// stage image properties).</summary>
    private void RaiseLoupeProperties()
    {
        OnPropertyChanged(nameof(LoupeCameraAlias));
        OnPropertyChanged(nameof(IsLoupeSourceLive));
        OnPropertyChanged(nameof(LoupeImage));
        OnPropertyChanged(nameof(IsLoupeImageVisible));
        OnPropertyChanged(nameof(IsLoupePlaceholderVisible));
        OnPropertyChanged(nameof(LoupeFreshnessText));
        OnPropertyChanged(nameof(IsLoupeFreshnessVisible));
        OnPropertyChanged(nameof(LoupeSourceLabelText));
        OnPropertyChanged(nameof(LoupeMarkerRelativeX));
        OnPropertyChanged(nameof(LoupeMarkerRelativeY));
        OnPropertyChanged(nameof(IsTargetOverlayVisible));
        OnPropertyChanged(nameof(CanAdjustTarget));
        OnPropertyChanged(nameof(LoupePeakingOverlay));
        OnPropertyChanged(nameof(IsLoupePeakingOverlayVisible));
        OnPropertyChanged(nameof(IsLoupeZoom100Checked));
        OnPropertyChanged(nameof(IsLoupeZoom200Checked));
        OnPropertyChanged(nameof(LoupeFocusX));
        OnPropertyChanged(nameof(LoupeFocusY));
        OnPropertyChanged(nameof(IsLoupeVisible));
        OnPropertyChanged(nameof(IsLoupeMarkerVisible));
    }

    private string FormatNotices(OperatorWarningSeverity severity, string emptyText)
    {
        var messages = OperatorReadinessEvaluator.BuildNotices(_readiness, DateOnly.FromDateTime(DateTime.Today)).Where(notice => notice.Severity == severity).Select(notice => notice.Message).Distinct().ToArray();
        return messages.Length == 0 ? emptyText : string.Join("\n", messages);
    }

    private static string FormatCamera(CameraReadiness camera, bool required) =>
        required
            ? $"構成対象 / {(camera.Connected ? "接続" : "未接続")} / 機体照合 {(camera.IdentityBound ? "済" : "未登録")} / 設定 {(camera.SettingsMatch ? "整合" : "不整合")} / カード {(camera.CardKnownEmpty ? "空を確認" : "要確認")} / ライブ表示 {(camera.LiveViewActive ? "ON" : "OFF")}"
            : "構成対象外 / 一台構成では接続しません";

    private void SetStep(string id, string state)
    {
        var step = ProgressSteps.Single(item => item.Id == id);
        switch (state) { case "completed": step.SetCompleted(); break; case "current": step.SetCurrent(); break; case "failure": step.SetFailure(); break; case "skipped": step.SetSkipped(); break; default: step.SetPending(); break; }
    }

    private void ResetProgress(CapturePlan? capturePlan = null)
    {
        capturePlan ??= CurrentCapturePlan;
        foreach (var step in ProgressSteps) step.SetPending();
        foreach (var alias in new[] { "CAM-A", "CAM-B" }.Where(alias => !capturePlan.RequiredCameraAliases.Contains(alias, StringComparer.Ordinal)))
        {
            SetStep(CaptureStep(alias), "skipped");
            SetStep(PersistStep(alias), "skipped");
        }
        if (capturePlan.OperatingMode == CameraOperatingMode.SingleCamera)
        {
            SetStep("stitch", "skipped");
        }
    }

    private static string CaptureStep(string alias) => alias == "CAM-A" ? "capture-a" : "capture-b";

    private static string PersistStep(string alias) => alias == "CAM-A" ? "persist-a" : "persist-b";

    private static string NoRetryMessage(CapturePlan capturePlan) =>
        capturePlan.OperatingMode == CameraOperatingMode.SingleCamera
            ? "NO AUTO RETRY: FailedPartial後は同じtransactionを再開せず、選択カメラを新しいtransactionで撮り直します。"
            : "NO AUTO RETRY: FailedPartial後は同じtransactionを再開せず、両カメラを新しいtransactionで撮り直します。";

    private void ShowUnexpectedFailure(Exception exception)
    {
        UiState = OperatorUiState.FailedPartial;
        StatusMessage = "SIMULATED shell内で予期しないエラーが発生しました。同じtransactionは再開しません。";
        TechnicalDetail = $"error code: {exception.GetType().Name} / {exception.Message}";
        IsBusy = false;
        RebuildReadiness(preserveOutcomeState: true);
    }

    private void NotifyAllCommands()
    {
        _acceptSafetyCommand.NotifyCanExecuteChanged();
        _declineSafetyCommand.NotifyCanExecuteChanged();
        _captureCommand.NotifyCanExecuteChanged();
        _captureWithAutoFocusCommand.NotifyCanExecuteChanged();
        _diagnosticCommand.NotifyCanExecuteChanged();
        _prepareNewCaptureCommand.NotifyCanExecuteChanged();
        _toggleLiveViewCommand.NotifyCanExecuteChanged();
        _exportCommand.NotifyCanExecuteChanged();
        _restitchCommand.NotifyCanExecuteChanged();
        _showDashboardCommand.NotifyCanExecuteChanged();
        _showSetupCommand.NotifyCanExecuteChanged();
        _showCameraSettingsCommand.NotifyCanExecuteChanged();
        _showDiagnosticsCommand.NotifyCanExecuteChanged();
        _autoFocusCommand.NotifyCanExecuteChanged();
        _mfCoarseBackwardCommand.NotifyCanExecuteChanged();
        _mfCoarseForwardCommand.NotifyCanExecuteChanged();
        _mfFineBackwardCommand.NotifyCanExecuteChanged();
        _mfFineForwardCommand.NotifyCanExecuteChanged();
        _togglePeakingCommand.NotifyCanExecuteChanged();
        _switchLiveCameraToTargetDomainCommand.NotifyCanExecuteChanged();
        _showConsentCommand.NotifyCanExecuteChanged();
        _showBindingDemoCommand.NotifyCanExecuteChanged();
        _gridPreset3Command.NotifyCanExecuteChanged();
        _gridPreset4Command.NotifyCanExecuteChanged();
        _gridPreset5Command.NotifyCanExecuteChanged();
        _resetViewCommand.NotifyCanExecuteChanged();
    }
}
