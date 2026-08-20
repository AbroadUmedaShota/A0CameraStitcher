using System.Collections.ObjectModel;
using System.IO;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.OperatorShell.Simulated;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

public sealed record CameraSettingRow(string Setting, string RequiredProfile, string CameraA, string CameraB);

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
    public const string SimulationBanner = "SIMULATED / 実機未接続";
    public const string HardwareDualPendingBanner = "HARDWARE DUAL / provider未接続 / 撮影禁止";
    private const string SingleModeLabel = "1台構成";
    private const string DualModeLabel = "2台構成";
    private const string StageModeCameraALive = "CAM-A live";
    private const string StageModeCameraBLive = "CAM-B live";
    private const string StageModeCompositePreview = "合成プレビュー";
    private const string StageProcessingPlaceholderMessage = "Live View 停止中（撮影シーケンス実行中）";
    private const string StagePreviewNoteMessage = "プレビュー表示のみ・原画像／合成には不使用";
    private const string LoupeZoom100 = "100%";
    private const string LoupeZoom200 = "200%";
    private const string LoupeUnavailableText = "フレーム未取得";

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
    private readonly ISimulatedLiveViewFramePump? _liveViewFramePump;
    private readonly ISimulatedLiveViewFrameSource? _liveViewFrameSource;
    private readonly SynchronizationContext? _synchronizationContext = SynchronizationContext.Current;
    private readonly AsyncRelayCommand _captureCommand;
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

    private CancellationToken _lifetimeToken;
    private bool _isBusy;
    private bool _safetyAcknowledged;
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
    private bool _isPeakingEnabled;
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
        ISimulatedLiveViewFrameSource? liveViewFrameSource = null)
    {
        _transactionService = transactionService ?? throw new ArgumentNullException(nameof(transactionService));
        _dualCameraFlow = dualCameraFlow;
        _hardwareDualRequestProvider = hardwareDualRequestProvider;
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
            new("liveview", "Live View停止"),
            new("capture-a", "CAM-A撮影"),
            new("persist-a", "CAM-A原本検証"),
            new("capture-b", "CAM-B撮影"),
            new("persist-b", "CAM-B原本検証"),
            new("stitch", "自動合成"),
            new("review", "結果確認"),
            new("export", "明示export"),
        ];

        _acceptSafetyCommand = new RelayCommand(AcceptSafety, () => !SafetyAcknowledged && !IsBusy);
        _declineSafetyCommand = new RelayCommand(DeclineSafety, () => !SafetyAcknowledged && !IsBusy);
        _captureCommand = new AsyncRelayCommand(() => RunCaptureAsync("正常完了"), () => CanCapture, ShowUnexpectedFailure);
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

    public string BannerText => _dualCameraFlow?.ExecutionEnvironment == DualCameraExecutionEnvironment.HardwareDual
        ? HardwareDualPendingBanner
        : SimulationBanner;
    public IReadOnlyList<string> OperatingModeOptions { get; } = [SingleModeLabel, DualModeLabel];
    public IReadOnlyList<string> CameraAliases { get; } = ["CAM-A", "CAM-B"];
    public IReadOnlyList<string> ReadinessDemoOptions { get; } = ["補正不要", "自動補正範囲内", "物理調整が必要", "CAM-A未接続", "CAM-B未接続", "カード状態要確認"];
    public IReadOnlyList<string> DiagnosticScenarios => IsSingleCameraMode
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

    public ICommand AcceptSafetyCommand => _acceptSafetyCommand;
    public ICommand DeclineSafetyCommand => _declineSafetyCommand;
    public ICommand CaptureCommand => _captureCommand;
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
                RebuildReadiness(preserveOutcomeState: UiState == OperatorUiState.FailedPartial);
            }
        }
    }

    public string SafetyAckText => SafetyAcknowledged ? "同意済み（アプリ終了時に破棄）" : "未同意 — 撮影禁止";
    public string ActivityText => IsBusy ? "操作をロック中" : "操作受付中";
    public bool IsSingleCameraMode => SelectedOperatingMode == SingleModeLabel;
    public bool CanChangeOperatingMode => !IsBusy && !IsLiveViewActive &&
        UiState is OperatorUiState.AwaitingSafetyAck or OperatorUiState.CheckingReadiness or OperatorUiState.NotReady or OperatorUiState.Ready or OperatorUiState.ReadyWithCorrection;
    public bool CanSelectCamera => !IsBusy && !IsLiveViewActive &&
        UiState is not (OperatorUiState.Capturing or OperatorUiState.Stitching or OperatorUiState.Review or OperatorUiState.FailedPartial or OperatorUiState.Degraded);
    public bool CanChangeExportDirectory => !IsBusy;
    public string OperatingModeDescription => IsSingleCameraMode
        ? $"{SelectedCamera}だけを撮影し、合成せず検証済み単体原画像を保存します。他方のD810は接続しません。"
        : "CAM-A→CAM-Bを順次撮影し、両原画像を合成します。一台欠けても自動で一台構成へ変更しません。";
    public string CaptureButtonText => IsSingleCameraMode ? $"{SelectedCamera}を撮影する（確認なし）" : "2台を順次撮影する（確認なし）";
    public string CameraSelectionLabel => IsSingleCameraMode ? "撮影・Live View対象" : "一台選択式 Live View";
    public string ProcessingResultLabel => IsSingleCameraMode ? "単体出力" : "合成";
    public string OverallStateText => UiState switch
    {
        OperatorUiState.Ready => "撮影可能",
        OperatorUiState.ReadyWithCorrection => "補正予定・撮影可能",
        OperatorUiState.Capturing => "撮影処理中",
        OperatorUiState.Stitching => "合成処理中",
        OperatorUiState.Review => "結果確認",
        OperatorUiState.FailedPartial => "一部失敗",
        OperatorUiState.Degraded => "要確認（Degraded）",
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
                OnPropertyChanged(nameof(IsStageReviewMode));
                OnPropertyChanged(nameof(IsStageLiveNoteVisible));
                OnPropertyChanged(nameof(IsStageSingleLiveMode));
                OnPropertyChanged(nameof(IsStageCompositePreviewMode));
                OnPropertyChanged(nameof(StageReviewBadgeText));
                OnPropertyChanged(nameof(ReadyStatusChipText));
                RaiseStageFrameProperties();
                RaiseFocusPanelProperties();
                RecalculateAvailability();
            }
        }
    }

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
            OnPropertyChanged(nameof(OperatingModeDescription));
            OnPropertyChanged(nameof(CaptureButtonText));
            OnPropertyChanged(nameof(CameraSelectionLabel));
            OnPropertyChanged(nameof(ProcessingResultLabel));
            OnPropertyChanged(nameof(DiagnosticScenarios));
            OnPropertyChanged(nameof(StageCompositeApplicable));
            OnPropertyChanged(nameof(StageReviewBadgeText));
            OnPropertyChanged(nameof(StageSingleLiveAliasInPlan));
            OnPropertyChanged(nameof(StageSingleLiveText));
            RaiseLoupeProperties();
            RaiseFocusPanelProperties();
            ResetProgress(CurrentCapturePlan);
            RebuildReadiness();
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
    public string LiveViewPlaceholder => $"{SelectedCamera}\n\nSimulated Live View placeholder 非実画像\n原画像・合成入力には使用しません";
    public string LiveViewButtonText => IsLiveViewActive ? $"{SelectedCamera} Live Viewを停止" : $"{SelectedCamera} Live Viewを開始";
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
    public bool IsStageProcessingPlaceholder => UiState is OperatorUiState.Capturing or OperatorUiState.Stitching;
    public bool IsStageReviewMode => UiState == OperatorUiState.Review;
    public bool IsStageLiveNoteVisible => !IsStageProcessingPlaceholder && !IsStageReviewMode;
    public bool IsStageSingleLiveMode => IsStageLiveNoteVisible && SelectedStageMode != StageModeCompositePreview;
    public bool IsStageCompositePreviewMode => IsStageLiveNoteVisible && SelectedStageMode == StageModeCompositePreview;
    public string StageProcessingPlaceholderText => StageProcessingPlaceholderMessage;
    public string StagePreviewNoteText => StagePreviewNoteMessage;
    public string StageReviewBadgeText => IsSingleCameraMode ? "検証済み原本" : "合成結果";
    public string CaptureAvailabilityText => CanCapture ? "撮影可" : "撮影不可";
    public string ReadyStatusChipText => $"{OverallStateText} / {CaptureAvailabilityText}";

    public string StageSingleLiveAlias => SelectedStageMode == StageModeCameraBLive ? "CAM-B" : "CAM-A";
    public bool StageSingleLiveAliasInPlan =>
        CurrentCapturePlan.RequiredCameraAliases.Contains(StageSingleLiveAlias, StringComparer.Ordinal);
    public string StageSingleLiveText => StageSingleLiveAliasInPlan
        ? $"{StageSingleLiveAlias}\n\nフルフレーム Simulated Live View placeholder 非実画像\n{StagePreviewNoteMessage}"
        : $"{StageSingleLiveAlias}\n\n1台構成のため対象外（運用対象は{SelectedCamera}のみ）";

    public bool StageCompositeApplicable => !IsSingleCameraMode;
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
            return "STILL 未取得";
        }

        var mostRecent = hasCapturedOriginal && (!hasLiveFrame || capturedAt >= liveFrameAt) ? capturedAt : liveFrameAt;
        var elapsedSeconds = Math.Max(0, (int)(DateTimeOffset.UtcNow - mostRecent).TotalSeconds);
        return $"STILL {elapsedSeconds}秒前";
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

    /// <summary>Which camera alias the loupe currently crops. Mirrors the stage's own mode
    /// gating: a single-camera live stage mode (CAM-A live / CAM-B live) always shows that one
    /// camera; composite preview and Review split left/right by <see cref="IsTargetOnLiveSide"/>,
    /// matching the alias the stage itself renders on that side (<see cref="StageCompositeLiveAlias"/>
    /// / <see cref="StageCompositeStillAlias"/>).</summary>
    public string LoupeCameraAlias =>
        IsSingleCameraMode
            ? SelectedCamera
            : IsStageCompositePreviewMode || IsStageReviewMode
                ? (IsTargetOnLiveSide ? StageCompositeLiveAlias : StageCompositeStillAlias)
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
            var x = Math.Clamp((int)Math.Round((TargetX * image.PixelWidth) - (cropWidth / 2.0)), 0, image.PixelWidth - cropWidth);
            var y = Math.Clamp((int)Math.Round((TargetY * image.PixelHeight) - (cropHeight / 2.0)), 0, image.PixelHeight - cropHeight);
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

    private static double ComputeMarkerRelative(double targetFraction, int imageExtent, int cropOrigin, int cropExtent)
    {
        if (cropExtent <= 0)
        {
            return 0.5;
        }

        var targetPixel = targetFraction * imageExtent;
        return Math.Clamp((targetPixel - cropOrigin) / cropExtent, 0.0, 1.0);
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
        StatusMessage = $"{targetAlias} のLive Viewへ切り替えます（操作者の明示クリックのみ・自動切替ではありません）。";
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
        ? (string.IsNullOrWhiteSpace(FixedLocalExportDirectory) ? "未選択 — fixed-local folderを明示入力" : FixedLocalExportDirectory)
        : _readiness.OutputDirectory;
    public string CameraAStatus => FormatCamera(_readiness.Cameras.Single(camera => camera.Alias == "CAM-A"), CurrentCapturePlan.RequiredCameraAliases.Contains("CAM-A"));
    public string CameraBStatus => FormatCamera(_readiness.Cameras.Single(camera => camera.Alias == "CAM-B"), CurrentCapturePlan.RequiredCameraAliases.Contains("CAM-B"));
    public string SetupStatusText => _readiness.Setup.Summary;
    public string CorrectionText => _readiness.Setup.PlannedCorrections.Count == 0 ? "予定補正なし" : string.Join(" / ", _readiness.Setup.PlannedCorrections);
    public string PhysicalAdjustmentText => _readiness.Setup.PhysicalAdjustments.Count == 0 ? "物理調整なし" : string.Join(" / ", _readiness.Setup.PhysicalAdjustments);
    public string BlockerText => FormatNotices(OperatorWarningSeverity.Blocker, "赤: Blockerなし");
    public string CautionText => FormatNotices(OperatorWarningSeverity.Caution, "黄: Cautionなし");
    public string InfoText => FormatNotices(OperatorWarningSeverity.Info, "青: PC原本を保持 / Live Viewは非原画像 / シャッター時刻差は非保証");
    private bool HasRecoverableHardwareDualTransaction =>
        !IsSingleCameraMode &&
        _dualCameraFlow is
        {
            ExecutionEnvironment: DualCameraExecutionEnvironment.HardwareDual,
            Current: { FailureCode: DualCameraFailureCode.AgentResponseUnknown },
        };

    public bool CanCapture => HasRecoverableHardwareDualTransaction ||
        (_availability.Capture.Allowed &&
        (IsSingleCameraMode || _dualCameraFlow is null ||
            (_dualCameraFlow.IdentitySnapshot.IsReady &&
             (_dualCameraFlow.ExecutionEnvironment != DualCameraExecutionEnvironment.HardwareDual ||
              _hardwareDualRequestProvider is not null))));
    public string CaptureDisabledReason => CanCapture
        ? HasRecoverableHardwareDualTransaction
            ? "既存transactionの結果だけを再照会します。新規撮影は開始しません。"
            : "準備完了。確認ダイアログなしで一度だけ開始します。"
        : !IsSingleCameraMode && _dualCameraFlow is not null && !_dualCameraFlow.IdentitySnapshot.IsReady
            ? $"DualCamera identity: {_dualCameraFlow.IdentitySnapshot.Status} — 撮影禁止"
            : !IsSingleCameraMode && _dualCameraFlow?.ExecutionEnvironment == DualCameraExecutionEnvironment.HardwareDual &&
              _hardwareDualRequestProvider is null
                ? "HardwareDual approved profiles and explicit operator confirmations are unavailable — 撮影禁止"
            : _availability.Capture.DisabledReason;
    public bool CanUseLiveView => _availability.LiveView.Allowed;
    public bool CanExport => _availability.Export.Allowed &&
        (_dualCameraFlow is null || IsSingleCameraMode || Directory.Exists(FixedLocalExportDirectory));
    public bool CanRestitch => _availability.Restitch.Allowed;
    public bool CanPrepareNewCapture => _availability.PrepareNewCapture.Allowed;
    public bool CanOpenMaintenance => _availability.OpenMaintenance.Allowed;

    private CapturePlan CurrentCapturePlan => IsSingleCameraMode ? CapturePlan.Single(SelectedCamera) : CapturePlan.Dual();

    public async Task InitializeAsync(CancellationToken cancellationToken)
    {
        _lifetimeToken = cancellationToken;
        UiState = OperatorUiState.CheckingReadiness;
        StatusMessage = "未完了SIMULATED journalを検査中です。自動再開はしません。";
        IsBusy = true;
        try
        {
            var recovered = await _transactionService.InitializeAsync(cancellationToken).ConfigureAwait(true);
            if (recovered.Count > 0)
            {
                ApplyCaptureResult(recovered[^1]);
                UiState = OperatorUiState.FailedPartial;
                StatusMessage = $"FailedPartial transaction {recovered.Count}件を検出しました。同じtransactionは再開しません。";
            }
            else
            {
                UiState = OperatorUiState.AwaitingSafetyAck;
                StatusMessage = "物理シャッターを操作せず、他のカメラアプリを使わないことへ同意してください。";
            }
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
        StatusMessage = "排他使用へ同意しました。readinessを確認しました。";
        RebuildReadiness(preserveOutcomeState: UiState == OperatorUiState.FailedPartial);
    }

    private void DeclineSafety()
    {
        StatusMessage = "同意しなかったため撮影は禁止されています。閲覧と終了のみ可能です。";
        UiState = OperatorUiState.AwaitingSafetyAck;
    }

    private async Task RunCaptureAsync(string scenario)
    {
        if (!CanCapture)
        {
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
        StatusMessage = $"{scenario}: 操作をロックし、選択中Live Viewを停止します。";

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
                StatusMessage = "Live Viewを安全に停止できなかったため、シャッターを切らず終了しました。";
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
                StatusMessage = "擬似クラッシュで未完了journalを残しました。再起動時検査でFailedPartialへ閉じ、再開しません。";
                return;
            }

            if (result.State == SimulatedTransactionState.FailedPartial)
            {
                UiState = OperatorUiState.FailedPartial;
                StatusMessage = $"撮影をFailedPartialで終了しました。{NoRetryMessage(capturePlan)}";
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
                    StatusMessage = "単体原画像を保持しました。Live View再開失敗のためSDK状態確認まで新規撮影を禁止します。";
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
                StatusMessage = "撮影・合成結果を保持しました。Live View再開失敗のためSDK状態確認まで新規撮影を禁止します。";
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
            StatusMessage = "アプリ終了により中断しました。次回起動時にjournalをFailedPartialへ閉じます。";
        }
        finally
        {
            IsBusy = false;
            RebuildReadiness(preserveOutcomeState: true);
        }
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
        StatusMessage = "CAM-A→CAM-Bを一回ずつ撮影し、各canonical original.jpgを検証します。";
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
                    : "実JPEG製品結果は保持しましたが、Live View再開失敗として新規撮影を禁止します。";
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
                ? "fixed-local folderへbyte-identical明示export完了"
                : $"export失敗: {state.Export.FailureReason}";
            _exportOutcome = new ExportOutcome(
                state.Export.JobId,
                state.Export.Succeeded,
                state.Export.OutputPath,
                ExportResult,
                state.Export.Succeeded ? null : state.Export.FailureCode.ToString());
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
            StatusMessage = "操作者選択fixed-local folderへ明示export中です。";
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
        if (!preserveOutcomeState && UiState is not (OperatorUiState.Capturing or OperatorUiState.Stitching))
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
        foreach (var name in new[] { nameof(ProfileText), nameof(OutputDirectory), nameof(CameraAStatus), nameof(CameraBStatus), nameof(SetupStatusText), nameof(CorrectionText), nameof(PhysicalAdjustmentText), nameof(BlockerText), nameof(CautionText), nameof(InfoText), nameof(OperatingModeDescription), nameof(CaptureButtonText), nameof(ProcessingResultLabel), nameof(StageCompositeFreshnessText), nameof(StageSingleLiveText), nameof(StageSingleLiveAliasInPlan), nameof(StageCompositeApplicable), nameof(StageReviewBadgeText) }) OnPropertyChanged(name);
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
    }

    private string FormatNotices(OperatorWarningSeverity severity, string emptyText)
    {
        var messages = OperatorReadinessEvaluator.BuildNotices(_readiness, DateOnly.FromDateTime(DateTime.Today)).Where(notice => notice.Severity == severity).Select(notice => notice.Message).Distinct().ToArray();
        return messages.Length == 0 ? emptyText : string.Join("\n", messages);
    }

    private static string FormatCamera(CameraReadiness camera, bool required) =>
        required
            ? $"構成対象 / {(camera.Connected ? "接続" : "未接続")} / identity {(camera.IdentityBound ? "OK" : "未登録")} / 設定 {(camera.SettingsMatch ? "整合" : "不整合")} / card {(camera.CardKnownEmpty ? "empty確認" : "要確認")} / Live View {(camera.LiveViewActive ? "ON" : "OFF")}"
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
    }
}
