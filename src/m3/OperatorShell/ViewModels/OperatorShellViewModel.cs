using System.Collections.ObjectModel;
using System.IO;
using System.Windows.Input;
using A0CameraStitcher.M3.Foundation;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

public sealed record CameraSettingRow(string Setting, string RequiredProfile, string CameraA, string CameraB);

public sealed class OperatorShellViewModel : ObservableObject
{
    public const string SimulationBanner = "SIMULATED / 実機未接続";
    private const string NoRetryMessage = "NO AUTO RETRY: FailedPartial後は同じtransactionを再開せず、両カメラを新しいtransactionで撮り直します。";

    private readonly ISimulatedTransactionService _transactionService;
    private readonly AsyncRelayCommand _captureCommand;
    private readonly AsyncRelayCommand _diagnosticCommand;
    private readonly AsyncRelayCommand _prepareNewCaptureCommand;
    private readonly RelayCommand _acceptSafetyCommand;
    private readonly RelayCommand _declineSafetyCommand;
    private readonly RelayCommand _toggleLiveViewCommand;
    private readonly RelayCommand _exportCommand;
    private readonly RelayCommand _restitchCommand;
    private readonly RelayCommand _showDashboardCommand;
    private readonly RelayCommand _showSetupCommand;
    private readonly RelayCommand _showCameraSettingsCommand;
    private readonly RelayCommand _showDiagnosticsCommand;

    private CancellationToken _lifetimeToken;
    private bool _isBusy;
    private bool _safetyAcknowledged;
    private bool _isLiveViewActive;
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
    private bool _cameraInspectionRequired;
    private int _transactionStartCount;
    private CaptureOutcome? _captureOutcome;
    private StitchOutcome? _stitchOutcome;
    private ExportOutcome? _exportOutcome;
    private ReadinessSnapshot _readiness = null!;
    private OperatorActionAvailability _availability = null!;

    public OperatorShellViewModel(ISimulatedTransactionService transactionService)
    {
        _transactionService = transactionService ?? throw new ArgumentNullException(nameof(transactionService));
        ProgressSteps =
        [
            new("liveview", "Live View停止"),
            new("capture-a", "CAM-A撮影"),
            new("persist-a", "CAM-A保存"),
            new("capture-b", "CAM-B撮影"),
            new("persist-b", "CAM-B保存"),
            new("stitch", "自動合成"),
        ];

        _acceptSafetyCommand = new RelayCommand(AcceptSafety, () => !SafetyAcknowledged && !IsBusy);
        _declineSafetyCommand = new RelayCommand(DeclineSafety, () => !SafetyAcknowledged && !IsBusy);
        _captureCommand = new AsyncRelayCommand(() => RunCaptureAsync("正常完了"), () => CanCapture, ShowUnexpectedFailure);
        _diagnosticCommand = new AsyncRelayCommand(() => RunCaptureAsync(SelectedDiagnosticScenario), () => CanCapture, ShowUnexpectedFailure);
        _prepareNewCaptureCommand = new AsyncRelayCommand(PrepareNewCaptureAsync, () => CanPrepareNewCapture, ShowUnexpectedFailure);
        _toggleLiveViewCommand = new RelayCommand(ToggleLiveView, () => CanUseLiveView);
        _exportCommand = new RelayCommand(ExportSimulatedResult, () => CanExport);
        _restitchCommand = new RelayCommand(Restitch, () => CanRestitch);
        _showDashboardCommand = new RelayCommand(() => SelectedPage = "Dashboard", () => CanOpenMaintenance);
        _showSetupCommand = new RelayCommand(() => SelectedPage = "Setup", () => CanOpenMaintenance);
        _showCameraSettingsCommand = new RelayCommand(() => SelectedPage = "CameraSettings", () => CanOpenMaintenance);
        _showDiagnosticsCommand = new RelayCommand(() => SelectedPage = "Diagnostics", () => CanOpenMaintenance);
        RebuildReadiness();
        ResetProgress();
    }

    public string BannerText => SimulationBanner;
    public IReadOnlyList<string> CameraAliases { get; } = ["CAM-A", "CAM-B"];
    public IReadOnlyList<string> ReadinessDemoOptions { get; } = ["補正不要", "自動補正範囲内", "物理調整が必要", "CAM-B未接続", "カード状態要確認"];
    public IReadOnlyList<string> DiagnosticScenarios { get; } = ["正常完了", "Live View停止失敗", "CAM-A撮影失敗", "CAM-B撮影失敗", "cleanup失敗", "合成失敗", "Live View再開失敗", "CAM-A保存後クラッシュ"];
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
                RebuildReadiness();
            }
        }
    }

    public string SafetyAckText => SafetyAcknowledged ? "同意済み（アプリ終了時に破棄）" : "未同意 — 撮影禁止";
    public string ActivityText => IsBusy ? "操作をロック中" : "操作受付中";
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
                RecalculateAvailability();
            }
        }
    }

    public string SelectedCamera
    {
        get => _selectedCamera;
        set
        {
            if (value is ("CAM-A" or "CAM-B") && SetProperty(ref _selectedCamera, value))
            {
                OnPropertyChanged(nameof(LiveViewPlaceholder));
                OnPropertyChanged(nameof(LiveViewButtonText));
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

    public string SelectedDiagnosticScenario { get => _selectedDiagnosticScenario; set => SetProperty(ref _selectedDiagnosticScenario, value); }
    public string SelectedPage { get => _selectedPage; private set { if (SetProperty(ref _selectedPage, value)) OnPropertyChanged(nameof(PageTitle)); } }
    public string PageTitle => SelectedPage switch { "Setup" => "設置・校正", "CameraSettings" => "カメラ設定（read-only）", "Diagnostics" => "保存・診断", _ => "撮影ダッシュボード" };
    public string LiveViewPlaceholder => $"{SelectedCamera}\n\nSimulated Live View placeholder 非実画像\n原画像・合成入力には使用しません";
    public string LiveViewButtonText => IsLiveViewActive ? $"{SelectedCamera} Live Viewを停止" : $"{SelectedCamera} Live Viewを開始";
    public bool IsLiveViewActive { get => _isLiveViewActive; private set { if (SetProperty(ref _isLiveViewActive, value)) { OnPropertyChanged(nameof(LiveViewButtonText)); RebuildReadiness(); } } }
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

    public string ProfileText => $"{_readiness.Profile.ProfileId} / v{_readiness.Profile.Version} / 期限 {_readiness.Profile.ExpiresOn:yyyy-MM-dd}";
    public string OutputDirectory => _readiness.OutputDirectory;
    public string CameraAStatus => FormatCamera(_readiness.Cameras.Single(camera => camera.Alias == "CAM-A"));
    public string CameraBStatus => FormatCamera(_readiness.Cameras.Single(camera => camera.Alias == "CAM-B"));
    public string SetupStatusText => _readiness.Setup.Summary;
    public string CorrectionText => _readiness.Setup.PlannedCorrections.Count == 0 ? "予定補正なし" : string.Join(" / ", _readiness.Setup.PlannedCorrections);
    public string PhysicalAdjustmentText => _readiness.Setup.PhysicalAdjustments.Count == 0 ? "物理調整なし" : string.Join(" / ", _readiness.Setup.PhysicalAdjustments);
    public string BlockerText => FormatNotices(OperatorWarningSeverity.Blocker, "赤: Blockerなし");
    public string CautionText => FormatNotices(OperatorWarningSeverity.Caution, "黄: Cautionなし");
    public string InfoText => FormatNotices(OperatorWarningSeverity.Info, "青: PC原本を保持 / Live Viewは非原画像 / シャッター時刻差は非保証");
    public bool CanCapture => _availability.Capture.Allowed;
    public string CaptureDisabledReason => CanCapture ? "準備完了。確認ダイアログなしで一度だけ開始します。" : _availability.Capture.DisabledReason;
    public bool CanUseLiveView => _availability.LiveView.Allowed;
    public bool CanExport => _availability.Export.Allowed;
    public bool CanRestitch => _availability.Restitch.Allowed;
    public bool CanPrepareNewCapture => _availability.PrepareNewCapture.Allowed;
    public bool CanOpenMaintenance => _availability.OpenMaintenance.Allowed;

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
                StatusMessage = $"未完了transaction {recovered.Count}件をFailedPartialへ確定しました。保持原画像を確認してください。";
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
        RebuildReadiness();
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

        IsBusy = true;
        TransactionStartCount++;
        var transactionId = Guid.NewGuid();
        LastTransactionId = transactionId.ToString("N");
        CaptureResult = "処理中";
        StitchResult = "未実行";
        ExportResult = "未実行";
        _captureOutcome = null;
        _stitchOutcome = null;
        _exportOutcome = null;
        ResetProgress();
        UiState = OperatorUiState.Capturing;
        StatusMessage = $"{scenario}: 操作をロックし、選択中Live Viewを停止します。";

        try
        {
            SetStep("liveview", "current");
            if (scenario == "Live View停止失敗")
            {
                SetStep("liveview", "failure");
                CaptureResult = "撮影前に失敗（シャッター未実行）";
                _captureOutcome = new CaptureOutcome(transactionId, SimulatedTransactionState.FailedPartial, [], CaptureResult, "LiveViewStopFailed", DateTimeOffset.Now);
                UiState = OperatorUiState.FailedPartial;
                TechnicalDetail = "error code: LiveViewStopFailed / capture calls: 0";
                StatusMessage = "Live Viewを安全に停止できなかったため、シャッターを切らず終了しました。";
                return;
            }

            IsLiveViewActive = false;
            SetStep("liveview", "completed");
            SetStep("capture-a", "current");
            var foundationScenario = scenario switch
            {
                "CAM-A撮影失敗" => SimulatedWorkflowScenario.FailCaptureA,
                "CAM-B撮影失敗" => SimulatedWorkflowScenario.FailCaptureB,
                "CAM-A保存後クラッシュ" => SimulatedWorkflowScenario.CrashAfterPersistA,
                _ => SimulatedWorkflowScenario.Success,
            };
            var result = await _transactionService.ExecuteAsync(transactionId, foundationScenario, _lifetimeToken).ConfigureAwait(true);
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
                StatusMessage = $"撮影をFailedPartialで終了しました。{NoRetryMessage}";
                return;
            }

            MarkCaptureStepsCompleted();
            if (scenario == "cleanup失敗")
            {
                _cameraInspectionRequired = true;
                SetStep("stitch", "completed");
                CreateSuccessfulStitch("cleanup異常あり");
                UiState = OperatorUiState.Review;
                StatusMessage = "PC原本と合成結果は利用できますが、カード状態を再確認するまで新規撮影は禁止です。";
                TechnicalDetail = "error code: EmptyAfterCheckFailed / exact-object cleanup: simulated";
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

    private Task PrepareNewCaptureAsync()
    {
        UiState = OperatorUiState.CheckingReadiness;
        StatusMessage = "read-onlyで接続・identity・profile・設置・カード・保存先を再検査しました。";
        _cameraInspectionRequired = false;
        ResetProgress();
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

    private void Restitch()
    {
        var jobId = Guid.NewGuid();
        LastStitchJobId = jobId.ToString("N");
        StitchResult = $"別jobで再合成成功 ({jobId:N})";
        _stitchOutcome = new StitchOutcome(jobId, true, StitchResult, _readiness.Setup.PlannedCorrections, null);
        UiState = OperatorUiState.Review;
        StatusMessage = "保持済みの左右原画像から、新しいstitch jobとして再合成しました。撮影transactionは変更していません。";
        RebuildReadiness(preserveOutcomeState: true);
    }

    private void ExportSimulatedResult()
    {
        var exportId = Guid.NewGuid();
        var exportDirectory = Path.Combine(Path.GetTempPath(), "A0CameraStitcher", "simulated-exports");
        Directory.CreateDirectory(exportDirectory);
        var outputPath = Path.Combine(exportDirectory, $"{exportId:N}.simulated-export.txt");
        File.WriteAllText(outputPath, $"Simulated export only{Environment.NewLine}transaction={LastTransactionId}{Environment.NewLine}stitchJob={LastStitchJobId}");
        _exportOutcome = new ExportOutcome(exportId, true, outputPath, "明示操作でSIMULATED出力を保存しました（JPEGではありません）", null);
        LastExportPath = _exportOutcome.OutputPath ?? "未実行";
        ExportResult = _exportOutcome.OperatorMessage;
        StatusMessage = "保存が完了しました。原画像を上書き・削除していません。";
        RecalculateAvailability();
    }

    private void ApplyCaptureResult(SimulatedWorkflowState result)
    {
        if (!result.Simulation || !string.Equals(result.Marker, "Simulated", StringComparison.Ordinal))
        {
            throw new InvalidDataException("実機非接続shellはSimulated markerのない結果を表示できません。");
        }

        LastTransactionId = result.TransactionId.ToString("N");
        CaptureResult = result.State.ToString();
        RetainedOriginals = result.RetainedOriginalAliases.Count == 0 ? "なし" : string.Join(", ", result.RetainedOriginalAliases) + "（simulated原画像）";
        _captureOutcome = new CaptureOutcome(
            result.TransactionId,
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
            var failedStep = result.RetainedOriginalAliases.Count == 0 ? "capture-a" : "capture-b";
            SetStep(failedStep, "failure");
        }
    }

    private void MarkCaptureStepsCompleted()
    {
        foreach (var id in new[] { "liveview", "capture-a", "persist-a", "capture-b", "persist-b" }) SetStep(id, "completed");
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
        var setup = SelectedReadinessDemo switch
        {
            "補正不要" => new SetupAssessment(SetupAssessmentStatus.Ready, "補正不要 — 撮影可能", [], []),
            "物理調整が必要" => new SetupAssessment(SetupAssessmentStatus.PhysicalAdjustmentRequired, "物理調整が必要 — 撮影禁止", [], ["CAM-Bを左へ2.4 mm", "時計回りに0.8°"]),
            _ => new SetupAssessment(SetupAssessmentStatus.ReadyWithCorrection, "自動補正範囲内 — 撮影可能", ["位置 +0.7 mm", "回転 -0.2°", "露出 +0.1 EV"], []),
        };
        var camBConnected = SelectedReadinessDemo != "CAM-B未接続";
        var cardsKnownEmpty = SelectedReadinessDemo != "カード状態要確認" && !_cameraInspectionRequired;
        var outputDirectory = Path.Combine(Path.GetTempPath(), "A0CameraStitcher", "simulated-exports");
        _readiness = new ReadinessSnapshot
        {
            SafetyAcknowledged = SafetyAcknowledged,
            Cameras =
            [
                new("CAM-A", true, true, true, cardsKnownEmpty, IsLiveViewActive && SelectedCamera == "CAM-A"),
                new("CAM-B", camBConnected, camBConnected, true, cardsKnownEmpty, IsLiveViewActive && SelectedCamera == "CAM-B"),
            ],
            Profile = new("RIG-SIM-A0", "0.3", new DateOnly(2027, 3, 31), true, true),
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
        var hasBothOriginals = _captureOutcome is not null &&
            _captureOutcome.RetainedOriginalAliases.Contains("CAM-A") &&
            _captureOutcome.RetainedOriginalAliases.Contains("CAM-B");
        _availability = OperatorReadinessEvaluator.Evaluate(
            _readiness,
            UiState,
            DateOnly.FromDateTime(DateTime.Today),
            _stitchOutcome?.Succeeded == true,
            hasBothOriginals);
        OnPropertyChanged(nameof(CanCapture));
        OnPropertyChanged(nameof(CaptureDisabledReason));
        OnPropertyChanged(nameof(CanUseLiveView));
        OnPropertyChanged(nameof(CanExport));
        OnPropertyChanged(nameof(CanRestitch));
        OnPropertyChanged(nameof(CanPrepareNewCapture));
        OnPropertyChanged(nameof(CanOpenMaintenance));
        NotifyAllCommands();
    }

    private void RaiseReadinessProperties()
    {
        foreach (var name in new[] { nameof(ProfileText), nameof(OutputDirectory), nameof(CameraAStatus), nameof(CameraBStatus), nameof(SetupStatusText), nameof(CorrectionText), nameof(PhysicalAdjustmentText), nameof(BlockerText), nameof(CautionText), nameof(InfoText) }) OnPropertyChanged(name);
    }

    private string FormatNotices(OperatorWarningSeverity severity, string emptyText)
    {
        var messages = OperatorReadinessEvaluator.BuildNotices(_readiness, DateOnly.FromDateTime(DateTime.Today)).Where(notice => notice.Severity == severity).Select(notice => notice.Message).Distinct().ToArray();
        return messages.Length == 0 ? emptyText : string.Join("\n", messages);
    }

    private static string FormatCamera(CameraReadiness camera) =>
        $"{(camera.Connected ? "接続" : "未接続")} / identity {(camera.IdentityBound ? "OK" : "未登録")} / 設定 {(camera.SettingsMatch ? "整合" : "不整合")} / card {(camera.CardKnownEmpty ? "empty確認" : "要確認")} / Live View {(camera.LiveViewActive ? "ON" : "OFF")}";

    private void SetStep(string id, string state)
    {
        var step = ProgressSteps.Single(item => item.Id == id);
        switch (state) { case "completed": step.SetCompleted(); break; case "current": step.SetCurrent(); break; case "failure": step.SetFailure(); break; default: step.SetPending(); break; }
    }

    private void ResetProgress() { foreach (var step in ProgressSteps) step.SetPending(); }

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
