using System.Collections.ObjectModel;
using System.IO;
using System.Windows.Input;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

public sealed record CameraSettingRow(string Setting, string RequiredProfile, string CameraA, string CameraB);

public sealed class OperatorShellViewModel : ObservableObject
{
    public const string SimulationBanner = "SIMULATED / 実機未接続";
    private const string SingleModeLabel = "1台構成";
    private const string DualModeLabel = "2台構成";

    private readonly ISimulatedTransactionService _transactionService;
    private readonly IDualCameraProductFlow? _dualCameraFlow;
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

    private CancellationToken _lifetimeToken;
    private bool _isBusy;
    private bool _safetyAcknowledged;
    private bool _isLiveViewActive;
    private string _selectedOperatingMode = DualModeLabel;
    private string _selectedCamera = "CAM-A";
    private string _selectedReadinessDemo = "自動補正範囲内";
    private string _selectedDiagnosticScenario = "正常完了";
    private string _selectedPage = "Dashboard";
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
        IDualCameraProductFlow? dualCameraFlow)
    {
        _transactionService = transactionService ?? throw new ArgumentNullException(nameof(transactionService));
        _dualCameraFlow = dualCameraFlow;
        if (_dualCameraFlow is not null)
        {
            _dualCameraFlow.StateChanged += OnDualCameraStateChanged;
            _dualCameraFlow.IdentityChanged += OnDualCameraIdentityChanged;
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
        RebuildReadiness();
        ResetProgress();
    }

    public string BannerText => SimulationBanner;
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
                OnPropertyChanged(nameof(LiveViewButtonText));
                OnPropertyChanged(nameof(CanChangeOperatingMode));
                OnPropertyChanged(nameof(CanSelectCamera));
                RebuildReadiness();
            }
        }
    }
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
    public bool CanCapture => _availability.Capture.Allowed &&
        (IsSingleCameraMode || _dualCameraFlow is null || _dualCameraFlow.IdentitySnapshot.IsReady);
    public string CaptureDisabledReason => CanCapture
        ? "準備完了。確認ダイアログなしで一度だけ開始します。"
        : !IsSingleCameraMode && _dualCameraFlow is not null && !_dualCameraFlow.IdentitySnapshot.IsReady
            ? $"DualCamera identity: {_dualCameraFlow.IdentitySnapshot.Status} — 撮影禁止"
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
        IsBusy = true;
        TransactionStartCount++;
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
            var request = DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()) with
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
            var state = await flow.CaptureAndStitchAsync(
                request,
                _lifetimeToken).ConfigureAwait(true);
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

    private void ApplyFormalDualCameraState(DualCameraProductState state)
    {
        if (state.Mode != CameraOperatingMode.DualCamera ||
            state.ExecutionEnvironment != DualCameraExecutionEnvironment.TestSynthetic)
        {
            throw new InvalidDataException("Formal simulated WPF accepts only typed TestSynthetic DualCamera state.");
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

        LastTransactionId = result.TransactionId.ToString("N");
        CaptureResult = result.State.ToString();
        RetainedOriginals = result.RetainedOriginalAliases.Count == 0 ? "なし" : string.Join(", ", result.RetainedOriginalAliases) + "（simulated原画像）";
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
            Notices = [],
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
        foreach (var name in new[] { nameof(ProfileText), nameof(OutputDirectory), nameof(CameraAStatus), nameof(CameraBStatus), nameof(SetupStatusText), nameof(CorrectionText), nameof(PhysicalAdjustmentText), nameof(BlockerText), nameof(CautionText), nameof(InfoText), nameof(OperatingModeDescription), nameof(CaptureButtonText), nameof(ProcessingResultLabel) }) OnPropertyChanged(name);
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
    }
}
