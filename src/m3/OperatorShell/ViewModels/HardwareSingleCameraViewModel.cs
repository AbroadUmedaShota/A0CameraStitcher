using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Input;
using A0CameraStitcher.M3.Foundation.Hardware;
using A0CameraStitcher.M3.OperatorShell.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

public sealed class HardwareSingleCameraViewModel : ObservableObject, IDisposable
{
    private static readonly TimeSpan MaximumProfileExpiryTimerDelay = TimeSpan.FromHours(1);
    private readonly IHardwareSingleCameraOperations _operations;
    private readonly IHardwareContinuousLiveViewOperations? _continuousLiveViewOperations;
    private readonly IHardwareSingleAppStateStore _stateStore;
    private HardwareOriginalExporter _exporter;
    private readonly HardwareSinglePreferencesStore? _preferencesStore;
    private readonly HardwareSingleCaptureProfileStore? _profileStore;
    private readonly TimeProvider _timeProvider;
    private HardwarePendingTransaction? _pendingTransaction;
    private HardwareSingleReadinessResult? _readiness;
    private HardwareSingleCaptureResult? _captureResult;
    private VerifiedHardwareJpeg? _verifiedOriginal;
    private string _selectedCamera = "CAM-A";
    private bool _exclusiveCameraControlConfirmed;
    private bool _dedicatedSpoolScopeConfirmed;
    private bool _exactObjectDeleteConfirmed;
    private bool _liveViewHandoffRequested;
    private bool _isContinuousLiveViewActive;
    private string? _continuousLiveViewSessionId;
    private CancellationTokenSource? _continuousLiveViewLoopCancellation;
    private Task? _continuousLiveViewLoop;
    private ImageSource? _previewImage;
    private bool _localPreDispatchFailure;
    private bool _isBusy;
    private bool _initializationStarted;
    private bool _initializationComplete;
    private bool _stateLoadFailed;
    private bool _hasOperatorSelectedExportDirectory;
    private bool _disposed;
    private int _profileExpiryGeneration;
    private ITimer? _profileExpiryTimer;
    private string _activityText = "ローカル状態を初期化してください。";
    private string _readinessSummary = "未確認";
    private string _readinessDetail = "排他使用へ同意した後、状態確認を実行してください。";
    private string _observedSettingsText = "未確認";
    private string _liveViewSummary = "未実行";
    private string _previewPath = string.Empty;
    private string _captureSummary = "未撮影";
    private string _retainedOriginalSummary = "なし";
    private string _lastTransactionId = string.Empty;
    private string _exportSummary = "未保存";
    private string _lastExportPath = string.Empty;
    private string _technicalDetail = "automatic retry count: 0";

    public HardwareSingleCameraViewModel(
        IHardwareSingleCameraOperations operations,
        IHardwareSingleAppStateStore stateStore,
        HardwareOriginalExporter exporter,
        TimeProvider? timeProvider = null)
        : this(operations, stateStore, exporter, null, null, timeProvider)
    {
    }

    internal HardwareSingleCameraViewModel(
        IHardwareSingleCameraOperations operations,
        IHardwareSingleAppStateStore stateStore,
        HardwareOriginalExporter exporter,
        HardwareSinglePreferencesStore? preferencesStore,
        HardwareSingleCaptureProfileStore? profileStore,
        TimeProvider? timeProvider = null)
    {
        _operations = operations ?? throw new ArgumentNullException(nameof(operations));
        _continuousLiveViewOperations = operations as IHardwareContinuousLiveViewOperations;
        _stateStore = stateStore ?? throw new ArgumentNullException(nameof(stateStore));
        _exporter = exporter ?? throw new ArgumentNullException(nameof(exporter));
        _preferencesStore = preferencesStore;
        _profileStore = profileStore;
        _timeProvider = timeProvider ?? TimeProvider.System;
        // An injected exporter is an explicit destination chosen by the caller.
        // The product composition supplies a preference store, so its default
        // LocalAppData path remains only a folder-picker starting point until a
        // durable operator choice is loaded or saved.
        _hasOperatorSelectedExportDirectory = preferencesStore is null;

        CheckReadinessCommand = new AsyncRelayCommand(CheckReadinessAsync, () => CanCheckReadiness, HandleCommandException);
        ProbeLiveViewCommand = new AsyncRelayCommand(ProbeLiveViewAsync, () => CanProbeLiveView, HandleCommandException);
        StartContinuousLiveViewCommand = new AsyncRelayCommand(
            StartContinuousLiveViewAsync, () => CanStartContinuousLiveView, HandleCommandException);
        StopContinuousLiveViewCommand = new AsyncRelayCommand(
            StopContinuousLiveViewAsync, () => CanStopContinuousLiveView, HandleCommandException);
        CaptureCommand = new AsyncRelayCommand(CaptureAsync, () => CanCapture, HandleCommandException);
        RecoverTransactionCommand = new AsyncRelayCommand(RecoverTransactionAsync, () => CanRecoverTransaction, HandleCommandException);
        ExportCommand = new AsyncRelayCommand(ExportAsync, () => CanExport, HandleCommandException);
        PrepareNewCaptureCommand = new AsyncRelayCommand(PrepareNewCaptureAsync, () => CanPrepareNewCapture, HandleCommandException);
        ApproveProfileCommand = new AsyncRelayCommand(ApproveProfileAsync, () => CanApproveProfile, HandleCommandException);
    }

    public IReadOnlyList<string> CameraAliases { get; } = ["CAM-A"];

    public string AgentExecutablePath => _operations.AgentExecutablePath;

    public string AgentAvailabilityText => _operations.AgentExecutableAvailable
        ? "Camera Agent実行ファイル: 検出済み"
        : "Camera Agent実行ファイル: 未検出";

    public string ExportDirectory => _exporter.ExportDirectory;

    public string ExportDirectoryDisplay => _hasOperatorSelectedExportDirectory
        ? _exporter.ExportDirectory
        : "未選択（保存先を選択してください）";

    public string SelectedCamera
    {
        get => _selectedCamera;
        set
        {
            if (!CanSelectCamera || value != "CAM-A" || !SetProperty(ref _selectedCamera, value))
            {
                return;
            }

            InvalidateReadiness("選択カメラを変更しました。状態を再確認してください。");
            _liveViewHandoffRequested = false;
            PreviewPath = string.Empty;
            PreviewImage = null;
            LiveViewSummary = "未実行";
            OnPropertyChanged(nameof(SelectedCameraDescription));
            NotifyAvailability();
        }
    }

    public string SelectedCameraDescription =>
        $"SingleCamera / {_selectedCamera} のみ。もう一台が接続されている場合は撮影を拒否します。";

    public bool ExclusiveCameraControlConfirmed
    {
        get => _exclusiveCameraControlConfirmed;
        set
        {
            if (CanChangeConfirmations && SetProperty(ref _exclusiveCameraControlConfirmed, value))
            {
                NotifyAvailability();
            }
        }
    }

    public bool DedicatedSpoolScopeConfirmed
    {
        get => _dedicatedSpoolScopeConfirmed;
        set
        {
            if (CanChangeConfirmations && SetProperty(ref _dedicatedSpoolScopeConfirmed, value))
            {
                NotifyAvailability();
            }
        }
    }

    public bool ExactObjectDeleteConfirmed
    {
        get => _exactObjectDeleteConfirmed;
        set
        {
            if (CanChangeConfirmations && SetProperty(ref _exactObjectDeleteConfirmed, value))
            {
                NotifyAvailability();
            }
        }
    }

    public bool IsBusy
    {
        get => _isBusy;
        private set
        {
            if (SetProperty(ref _isBusy, value))
            {
                OnPropertyChanged(nameof(CanSelectCamera));
                OnPropertyChanged(nameof(CanChangeConfirmations));
                NotifyAvailability();
            }
        }
    }

    public bool CanSelectCamera =>
        _initializationComplete && !IsBusy && !IsContinuousLiveViewActive &&
        _pendingTransaction is null && _captureResult is null && !_stateLoadFailed;

    public bool CanChangeConfirmations =>
        _initializationComplete && !IsBusy && !IsContinuousLiveViewActive &&
        _pendingTransaction is null && !_stateLoadFailed;

    public string ActivityText
    {
        get => _activityText;
        private set => SetProperty(ref _activityText, value);
    }

    public string ReadinessSummary
    {
        get => _readinessSummary;
        private set => SetProperty(ref _readinessSummary, value);
    }

    public string ReadinessDetail
    {
        get => _readinessDetail;
        private set => SetProperty(ref _readinessDetail, value);
    }

    public string ObservedSettingsText
    {
        get => _observedSettingsText;
        private set => SetProperty(ref _observedSettingsText, value);
    }

    public string LiveViewSummary
    {
        get => _liveViewSummary;
        private set => SetProperty(ref _liveViewSummary, value);
    }

    public string PreviewPath
    {
        get => _previewPath;
        private set
        {
            if (SetProperty(ref _previewPath, value))
            {
                OnPropertyChanged(nameof(HasPreview));
            }
        }
    }

    public ImageSource? PreviewImage
    {
        get => _previewImage;
        private set
        {
            if (SetProperty(ref _previewImage, value))
            {
                OnPropertyChanged(nameof(HasPreview));
            }
        }
    }

    public bool HasPreview => PreviewImage is not null || !string.IsNullOrEmpty(PreviewPath);

    public bool SupportsContinuousLiveView => _continuousLiveViewOperations is not null;

    public bool IsContinuousLiveViewActive
    {
        get => _isContinuousLiveViewActive;
        private set
        {
            if (SetProperty(ref _isContinuousLiveViewActive, value))
            {
                OnPropertyChanged(nameof(ContinuousLiveViewButtonText));
                OnPropertyChanged(nameof(CanSelectCamera));
                OnPropertyChanged(nameof(CanChangeConfirmations));
                NotifyAvailability();
            }
        }
    }

    public string ContinuousLiveViewButtonText =>
        IsContinuousLiveViewActive ? "継続Live Viewを停止" : "継続Live Viewを開始";

    public string CaptureSummary
    {
        get => _captureSummary;
        private set => SetProperty(ref _captureSummary, value);
    }

    public string RetainedOriginalSummary
    {
        get => _retainedOriginalSummary;
        private set => SetProperty(ref _retainedOriginalSummary, value);
    }

    public string StitchSummary => "対象外（SingleCameraでは合成・再合成を行いません）";

    public string LastTransactionId
    {
        get => _lastTransactionId;
        private set => SetProperty(ref _lastTransactionId, value);
    }

    public string ExportSummary
    {
        get => _exportSummary;
        private set => SetProperty(ref _exportSummary, value);
    }

    public string LastExportPath
    {
        get => _lastExportPath;
        private set => SetProperty(ref _lastExportPath, value);
    }

    public string TechnicalDetail
    {
        get => _technicalDetail;
        private set => SetProperty(ref _technicalDetail, value);
    }

    public string BlockerText
    {
        get
        {
            if (!_initializationComplete)
            {
                return "起動時transaction検査が完了するまで、実機操作を開始しません。";
            }

            if (_stateLoadFailed)
            {
                return "ローカルtransaction状態を安全に読めません。撮影を開始しません。";
            }

            if (_pendingTransaction is not null && _captureResult is null)
            {
                if (_localPreDispatchFailure)
                {
                    return "Camera Agent要求前に中断しました。「新しい撮影を準備」で明示的に閉じてください。";
                }

                return "未確定transactionがあります。新しい撮影ではなく「結果を確認」を実行してください。";
            }

            if (!_operations.AgentExecutableAvailable)
            {
                return "Camera Agent実行ファイルがありません。実機操作は開始されません。";
            }

            if (_captureResult is not null)
            {
                return "結果が確定しています。次の撮影は「新しい撮影を準備」で明示的に開始してください。";
            }

            if (!_exclusiveCameraControlConfirmed)
            {
                return "Camera Agentを使う前に排他使用へ同意してください。";
            }

            if (!_hasOperatorSelectedExportDirectory)
            {
                return "検証済み原画像の保存先として、固定ローカルフォルダを選択してください。";
            }

            if (_readiness is null)
            {
                return "接続台数・identity binding・empty spool・read-only設定を確認してください。";
            }

            return _readiness.Ready
                ? "なし"
                : $"撮影不可: {_readiness.FailureCategory} / {_readiness.FailureDetail}";
        }
    }

    public string CautionText =>
        "実機操作です。物理シャッター・他のカメラアプリを使わず、撮影中にUSBを抜かないでください。自動再試行は0回です。";

    public string InfoText =>
        "Live View previewは原画像や合成入力ではありません。設定はread-onlyです。単体出力は操作者指定の固定ローカルフォルダへの検証済みoriginal.jpgのbyte-identical copyです。";

    public bool CanCheckReadiness =>
        _initializationComplete && !IsBusy && !IsContinuousLiveViewActive && !_stateLoadFailed &&
        _pendingTransaction is null && _captureResult is null &&
        _operations.AgentExecutableAvailable && ExclusiveCameraControlConfirmed;

    public bool CanProbeLiveView =>
        _continuousLiveViewOperations is null && _initializationComplete && !IsBusy && !_stateLoadFailed &&
        _pendingTransaction is null && _captureResult is null &&
        IsIdentityReadyForLiveView(_readiness) && ExclusiveCameraControlConfirmed;

    public bool CanStartContinuousLiveView =>
        _continuousLiveViewOperations is not null && _initializationComplete && !IsBusy &&
        !IsContinuousLiveViewActive && !_stateLoadFailed && _pendingTransaction is null &&
        _captureResult is null && IsIdentityReadyForLiveView(_readiness) &&
        ExclusiveCameraControlConfirmed;

    public bool CanStopContinuousLiveView =>
        _continuousLiveViewOperations is not null && _initializationComplete && !IsBusy &&
        IsContinuousLiveViewActive;

    public bool CanCapture =>
        _initializationComplete && !IsBusy && !_stateLoadFailed &&
        _pendingTransaction is null && _captureResult is null &&
        _readiness?.Ready == true && ExclusiveCameraControlConfirmed &&
        _hasOperatorSelectedExportDirectory &&
        _readiness.CaptureProfileExpiresAtUtc is { } profileExpiry &&
        profileExpiry > _timeProvider.GetUtcNow() &&
        DedicatedSpoolScopeConfirmed && ExactObjectDeleteConfirmed;

    public bool CanRecoverTransaction =>
        _initializationComplete && !IsBusy && !_stateLoadFailed &&
        _pendingTransaction is not null && _captureResult is null &&
        _pendingTransaction.CaptureRequestDispatchAttempted && _operations.AgentExecutableAvailable;

    public bool CanExport =>
        _initializationComplete && !IsBusy && !_stateLoadFailed && _captureResult is not null &&
        _captureResult.TerminalState is not ("Reserved" or "InProgress") &&
        _verifiedOriginal is not null && _hasOperatorSelectedExportDirectory;

    public bool CanPrepareNewCapture =>
        _initializationComplete && !IsBusy && !_stateLoadFailed && _pendingTransaction is not null &&
        (_captureResult is not null || _localPreDispatchFailure);

    public bool CanApproveProfile =>
        _profileStore is not null && _initializationComplete && !IsBusy && !_stateLoadFailed &&
        _pendingTransaction is null && _captureResult is null && _readiness is not null &&
        SelectedCamera == "CAM-A" && ExclusiveCameraControlConfirmed;

    public bool CanChangeExportDirectory =>
        _preferencesStore is not null && _initializationComplete && !IsBusy && !_stateLoadFailed &&
        (_pendingTransaction is null || _captureResult is not null);

    public ICommand CheckReadinessCommand { get; }

    public ICommand ProbeLiveViewCommand { get; }

    public ICommand StartContinuousLiveViewCommand { get; }

    public ICommand StopContinuousLiveViewCommand { get; }

    public ICommand CaptureCommand { get; }

    public ICommand RecoverTransactionCommand { get; }

    public ICommand ExportCommand { get; }

    public ICommand PrepareNewCaptureCommand { get; }

    public ICommand ApproveProfileCommand { get; }

    public async Task InitializeAsync(CancellationToken cancellationToken = default)
    {
        if (_initializationStarted || _initializationComplete)
        {
            return;
        }

        _initializationStarted = true;
        IsBusy = true;
        try
        {
            if (_preferencesStore is not null)
            {
                var preferences = await _preferencesStore.LoadAsync(cancellationToken).ConfigureAwait(true);
                if (preferences is not null)
                {
                    _exporter = new HardwareOriginalExporter(preferences.ExportDirectory);
                    _hasOperatorSelectedExportDirectory = true;
                    OnPropertyChanged(nameof(ExportDirectory));
                    OnPropertyChanged(nameof(ExportDirectoryDisplay));
                }
            }
            _pendingTransaction = await _stateStore.LoadPendingAsync(cancellationToken).ConfigureAwait(true);
            if (_pendingTransaction is not null)
            {
                _selectedCamera = _pendingTransaction.CameraAlias;
                LastTransactionId = _pendingTransaction.TransactionId;
                ActivityText = "未確定transactionを検出しました。結果照会のみ可能です。";
                CaptureSummary = "未確定（新規撮影禁止）";
                TechnicalDetail =
                    $"transaction: {_pendingTransaction.TransactionId}\n" +
                    $"mode: {_pendingTransaction.OperatingMode}\n" +
                    $"camera: {_pendingTransaction.CameraAlias}\n" +
                    "automatic retry count: 0";
                OnPropertyChanged(nameof(SelectedCamera));
                OnPropertyChanged(nameof(SelectedCameraDescription));
                if (!_pendingTransaction.CaptureRequestDispatchAttempted)
                {
                    _localPreDispatchFailure = true;
                    ActivityText = "Camera Agent要求前に中断したtransactionです。撮影0回として明示的に閉じられます。";
                    CaptureSummary = "開始前FailedPartial（撮影要求0回）";
                }
                else if (_operations.AgentExecutableAvailable)
                {
                    ActivityText = "起動時に未完了transactionを検査中です。撮影や再試行は行いません。";
                    await QueryPendingTransactionAsync(_pendingTransaction, cancellationToken).ConfigureAwait(true);
                }
            }
            else
            {
                ActivityText = _operations.AgentExecutableAvailable
                    ? "SingleCameraを明示選択済み。実機操作はまだ開始していません。"
                    : "Camera Agentが見つからないため実機操作をブロックしています。";
            }

            _initializationComplete = true;
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            _stateLoadFailed = true;
            ActivityText = "ローカル状態の読取に失敗しました。fail-closedです。";
            TechnicalDetail = $"app_state_invalid: {SafeMessage(exception)}\nautomatic retry count: 0";
            _initializationComplete = true;
        }
        finally
        {
            _initializationStarted = false;
            IsBusy = false;
            NotifyAvailability();
        }
    }

    public async Task ChangeExportDirectoryAsync(
        string exportDirectory,
        CancellationToken cancellationToken = default)
    {
        if (!CanChangeExportDirectory || _preferencesStore is null)
        {
            return;
        }

        IsBusy = true;
        try
        {
            var normalized = Path.GetFullPath(exportDirectory);
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(normalized);
            Directory.CreateDirectory(normalized);
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(normalized);
            await _preferencesStore.SaveAsync(normalized, cancellationToken).ConfigureAwait(true);
            _exporter = new HardwareOriginalExporter(normalized);
            _hasOperatorSelectedExportDirectory = true;
            OnPropertyChanged(nameof(ExportDirectory));
            OnPropertyChanged(nameof(ExportDirectoryDisplay));
            ActivityText = "保存先をローカル固定ドライブへ更新しました。";
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            ActivityText = "保存先を安全に更新できませんでした。以前の保存先を維持します。";
            TechnicalDetail += $"\nexport_directory_failed: {SafeMessage(exception)}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    public async Task ApproveProfileAsync()
    {
        if (!CanApproveProfile || _profileStore is null || _readiness is null)
        {
            return;
        }

        IsBusy = true;
        ActivityText = "read-only観測値を一台用プロファイルとして確認中…";
        try
        {
            await _profileStore.ApproveCamAAsync(_readiness.ObservedSettings).ConfigureAwait(true);
            InvalidateReadiness("30日有効のCAM-Aプロファイルを承認しました。状態を再確認してください。");
            ActivityText = "CAM-Aプロファイルをローカル承認しました。カメラ設定は変更していません。";
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            ActivityText = "観測値が承認条件に一致しないため、プロファイルを作成しませんでした。";
            TechnicalDetail += $"\nprofile_approval_failed: {SafeMessage(exception)}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    public async Task CheckReadinessAsync()
    {
        if (!CanCheckReadiness)
        {
            return;
        }

        CancelProfileExpiryInvalidation();
        IsBusy = true;
        ActivityText = $"{SelectedCamera} のread-only状態を確認中…";
        ReadinessSummary = "確認中";
        try
        {
            var reply = await _operations.GetReadinessAsync(SelectedCamera).ConfigureAwait(true);
            _readiness = reply.Payload;
            ReadinessSummary = reply.Payload.Ready ? "撮影準備OK" : "撮影不可";
            ReadinessDetail =
                $"SDK/WPD: {reply.Payload.SdkCameraCount}/{reply.Payload.WpdCameraCount} 台、" +
                $"alias: {(reply.Payload.SdkAliasMatches && reply.Payload.WpdAliasMatches ? "一致" : "不一致")}、" +
                $"spool payload: {reply.Payload.SpoolPayloadObjectCount}、" +
                $"profile: {(reply.Payload.CaptureProfileApproved ? $"{reply.Payload.CaptureProfileId} v{reply.Payload.CaptureProfileVersion}" : "未承認")} / " +
                $"期限(UTC): {(reply.Payload.CaptureProfileExpiresAtUtc is { } expiry ? expiry.ToString("O") : "なし")} / " +
                $"alias: {(reply.Payload.CaptureProfileAliasMatches ? "一致" : "未一致")} / " +
                $"settings: {(reply.Payload.SettingsMatchApprovedProfile ? "一致" : "未一致")}、" +
                $"firmware: {reply.Payload.Firmware}、Live View: {reply.Payload.LiveViewStatus}";
            ObservedSettingsText = FormatObservedSettings(reply.Payload.ObservedSettings);
            ActivityText = reply.Payload.Ready
                ? $"{SelectedCamera} は一台構成の撮影条件を満たしています。"
                : $"状態確認でblockerを検出しました: {reply.ResultCode}";
            TechnicalDetail =
                $"resultCode: {reply.ResultCode}\n" +
                $"captureProfileId: {reply.Payload.CaptureProfileId}\n" +
                $"captureProfileVersion: {reply.Payload.CaptureProfileVersion}\n" +
                $"captureProfileExpiresAtUtc: {reply.Payload.CaptureProfileExpiresAtUtc?.ToString("O") ?? "none"}\n" +
                $"failureCategory: {reply.Payload.FailureCategory}\n" +
                "capture command sent: false\nsettings changed: false\nautomatic retry count: 0";
            ScheduleProfileExpiryInvalidation();
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            _readiness = null;
            ReadinessSummary = "確認失敗";
            ReadinessDetail = SafeMessage(exception);
            ActivityText = "状態確認に失敗しました。撮影は開始されません。";
            TechnicalDetail = $"readiness_failed: {SafeMessage(exception)}\nautomatic retry count: 0";
        }
        finally
        {
            IsBusy = false;
            OnPropertyChanged(nameof(BlockerText));
        }
    }

    public async Task ProbeLiveViewAsync()
    {
        if (!CanProbeLiveView)
        {
            return;
        }

        IsBusy = true;
        var alias = SelectedCamera;
        ActivityText = $"{alias} の有限Live Viewを1フレーム取得中…";
        LiveViewSummary = "取得中";
        PreviewPath = string.Empty;
        try
        {
            var reply = await _operations.ProbeLiveViewAsync(alias).ConfigureAwait(true);
            if (!reply.Success || reply.Payload.Preview is null)
            {
                LiveViewSummary = $"失敗: {reply.Payload.ErrorCategory}";
                ActivityText = "Live View probeは失敗しました。撮影は実行していません。";
            }
            else
            {
                var verified = await HardwareArtifactVerifier
                    .VerifyPreviewAsync(reply.Payload.Preview)
                    .ConfigureAwait(true);
                PreviewPath = verified.Path;
                PreviewImage = TryLoadFrozenImage(verified.Path);
                _liveViewHandoffRequested = true;
                LiveViewSummary = $"1フレーム確認済み ({verified.SizeBytes:N0} bytes)";
                ActivityText = "Live Viewを停止しSDKを閉じました。撮影前に状態を再確認してください。";
            }

            TechnicalDetail =
                $"resultCode: {reply.ResultCode}\n" +
                $"liveViewStopped: {reply.Payload.LiveViewStopped}\n" +
                $"sdkSessionClosed: {reply.Payload.SdkSessionClosed}\n" +
                "previewIsOriginal: false\npreviewIsStitchInput: false\nautomatic retry count: 0";
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            LiveViewSummary = "取得または検証に失敗";
            ActivityText = "Live View結果を採用しませんでした。撮影は実行していません。";
            TechnicalDetail = $"live_view_failed: {SafeMessage(exception)}\nautomatic retry count: 0";
        }
        finally
        {
            InvalidateReadiness("Live View後は撮影前の状態再確認が必要です。");
            IsBusy = false;
        }
    }

    public async Task StartContinuousLiveViewAsync()
    {
        if (!CanStartContinuousLiveView || _continuousLiveViewOperations is null)
        {
            return;
        }

        IsBusy = true;
        try
        {
            await StartContinuousLiveViewCoreAsync().ConfigureAwait(true);
        }
        finally
        {
            IsBusy = false;
        }
    }

    public async Task StopContinuousLiveViewAsync()
    {
        if (!CanStopContinuousLiveView || _continuousLiveViewOperations is null)
        {
            return;
        }

        IsBusy = true;
        try
        {
            _ = await StopContinuousLiveViewCoreAsync().ConfigureAwait(true);
        }
        finally
        {
            IsBusy = false;
        }
    }

    private async Task StartContinuousLiveViewCoreAsync()
    {
        if (_continuousLiveViewOperations is null)
        {
            return;
        }

        var sessionId = _continuousLiveViewOperations.CreateSessionId();
        ActivityText = "継続Live Viewセッションを開始中…";
        LiveViewSummary = "開始中";
        PreviewPath = string.Empty;
        PreviewImage = null;
        try
        {
            var reply = await _continuousLiveViewOperations
                .StartLiveViewAsync(sessionId)
                .ConfigureAwait(true);
            if (!reply.Success)
            {
                LiveViewSummary = $"開始失敗: {reply.Payload.ErrorCategory}";
                ActivityText = "継続Live Viewは開始されませんでした。撮影は実行していません。";
                return;
            }

            _continuousLiveViewSessionId = sessionId;
            _liveViewHandoffRequested = false;
            IsContinuousLiveViewActive = true;
            LiveViewSummary = "継続表示中（preview only）";
            ActivityText = "CAM-A 継続Live Viewを表示しています。";
            _continuousLiveViewLoopCancellation = new CancellationTokenSource();
            _continuousLiveViewLoop = RunContinuousLiveViewLoopAsync(
                sessionId, _continuousLiveViewLoopCancellation.Token);
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            LiveViewSummary = "開始失敗";
            ActivityText = "継続Live Viewを開始できませんでした。撮影は実行していません。";
            TechnicalDetail += $"\ncontinuous_live_view_start_failed: {SafeMessage(exception)}";
        }
    }

    private async Task<bool> StopContinuousLiveViewCoreAsync()
    {
        if (_continuousLiveViewOperations is null || _continuousLiveViewSessionId is null)
        {
            return false;
        }

        ActivityText = "継続Live Viewを停止し、SDKセッションをclose中…";
        _continuousLiveViewLoopCancellation?.Cancel();
        if (_continuousLiveViewLoop is not null)
        {
            try
            {
                await _continuousLiveViewLoop.ConfigureAwait(true);
            }
            catch (OperationCanceledException)
            {
            }
        }

        try
        {
            var reply = await _continuousLiveViewOperations
                .StopLiveViewAsync(_continuousLiveViewSessionId)
                .ConfigureAwait(true);
            if (!reply.Success || reply.Payload.SdkSessionOpen || reply.Payload.LiveViewRunning)
            {
                LiveViewSummary = $"停止未確認: {reply.Payload.ErrorCategory}";
                ActivityText = "Live View停止とSDK closeを確認できません。撮影を開始しません。";
                if (!reply.Payload.SdkSessionOpen && !reply.Payload.LiveViewRunning)
                {
                    IsContinuousLiveViewActive = false;
                    _continuousLiveViewLoopCancellation?.Dispose();
                    _continuousLiveViewLoopCancellation = null;
                    _continuousLiveViewLoop = null;
                    InvalidateReadiness("Live View停止はfail-closedでSDK close済みですが、撮影前の状態再確認が必要です。");
                }
                return false;
            }

            IsContinuousLiveViewActive = false;
            _continuousLiveViewLoopCancellation?.Dispose();
            _continuousLiveViewLoopCancellation = null;
            _continuousLiveViewLoop = null;
            LiveViewSummary = "停止済み（SDK session closed）";
            ActivityText = "継続Live Viewを停止し、SDKセッションをcloseしました。";
            return true;
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            LiveViewSummary = "停止結果を確認できません";
            ActivityText = "Live View停止状態が不明です。撮影を開始しません。";
            TechnicalDetail += $"\ncontinuous_live_view_stop_unconfirmed: {SafeMessage(exception)}";
            return false;
        }
    }

    private async Task RunContinuousLiveViewLoopAsync(
        string sessionId,
        CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested &&
               _continuousLiveViewOperations is not null)
        {
            HardwareCameraAgentReply<HardwareContinuousLiveViewResult> reply;
            try
            {
                // 停止操作はループのトークンをキャンセルするが、フレーム要求自体には渡さない。
                // 中断するとリンクされたトークンがトランスポートの応答読み取りを打ち切り、
                // クライアント側パイプが閉じてしまう。サーバはdispatch中の応答を配送できず
                // （delivery-ACK契約違反）exit code 3でCamera Agentプロセスごと終了する。
                // 停止は「中断」ではなく「1フレーム分の完了待ち」で表現する。
                reply = await _continuousLiveViewOperations
                    .ReadLiveViewFrameAsync(sessionId, CancellationToken.None)
                    .ConfigureAwait(true);
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                // ループのトークンをこの呼び出しに渡さなくなったため、ここに届く
                // OperationCanceledExceptionは「停止された」ことを意味しない。
                // 応答タイムアウト（LiveViewResponseTimeout）由来の失敗として扱う。
                LiveViewSummary = "フレーム取得状態が不明です。停止操作が必要です。";
                ActivityText = "Live View通信が中断しました。撮影前に停止を確認してください。";
                TechnicalDetail += $"\ncontinuous_live_view_frame_unconfirmed: {SafeMessage(exception)}";
                return;
            }

            if (cancellationToken.IsCancellationRequested)
            {
                // 停止済み。古いフレームでPreviewImage/LiveViewSummaryを上書きしない。
                return;
            }

            if (!reply.Success)
            {
                IsContinuousLiveViewActive = false;
                LiveViewSummary = $"Live View終了: {reply.Payload.ErrorCategory}";
                ActivityText = "Camera AgentがLive View SDKセッションをfail-closedで終了しました。";
                InvalidateReadiness("Live View failure後は撮影前の状態再確認が必要です。");
                return;
            }

            try
            {
                var frame = reply.Payload.DecodeVerifiedFrame();
                PreviewImage = LoadFrozenImage(frame);
                PreviewPath = string.Empty;
                LiveViewSummary = $"継続表示中 / frame {reply.Payload.FrameNumber:N0} / {frame.Length:N0} bytes";
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                LiveViewSummary = "preview JPEGを表示できません。停止操作が必要です。";
                ActivityText = "Live View frameを採用しません。撮影前に停止を確認してください。";
                TechnicalDetail += $"\ncontinuous_live_view_frame_invalid: {SafeMessage(exception)}";
                return;
            }
            try
            {
                await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken).ConfigureAwait(true);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
        }
    }

    public async Task CaptureAsync()
    {
        if (!CanCapture)
        {
            return;
        }

        var restartContinuousLiveView = IsContinuousLiveViewActive;
        if (restartContinuousLiveView)
        {
            IsBusy = true;
            if (!await StopContinuousLiveViewCoreAsync().ConfigureAwait(true))
            {
                IsBusy = false;
                NotifyAvailability();
                return;
            }
        }

        CancelProfileExpiryInvalidation();
        var alias = SelectedCamera;
        var readiness = _readiness!;
        var finiteLiveViewHandoffRequested =
            _continuousLiveViewOperations is null && _liveViewHandoffRequested;
        var expectedProfile = new HardwareCaptureProfileSnapshot(
            readiness.CaptureProfileId,
            readiness.CaptureProfileVersion,
            readiness.CaptureProfileSha256,
            readiness.CaptureProfileExpiresAtUtc!.Value);
        var transactionId = HardwareCameraAgentProtocolCodec.CreateTransactionId();
        var pending = new HardwarePendingTransaction
        {
            OperatingMode = "SingleCamera",
            TransactionId = transactionId,
            CameraAlias = alias,
            RequiredCameraAlias = alias,
            CaptureProfileId = expectedProfile.ProfileId,
            CaptureProfileVersion = expectedProfile.ProfileVersion,
            CaptureProfileSha256 = expectedProfile.Sha256,
            CaptureProfileExpiresAtUtc = expectedProfile.ExpiresAtUtc,
            LiveViewHandoffRequested = finiteLiveViewHandoffRequested,
            CaptureRequestDispatchAttempted = false,
            StartedAtUtc = _timeProvider.GetUtcNow(),
        };

        IsBusy = true;
        LastTransactionId = transactionId;
        ActivityText = $"{alias} のtransactionを永続化中…";
        var pendingSaved = false;
        var dispatchAttempted = false;
        try
        {
            await _stateStore.SavePendingAsync(pending).ConfigureAwait(true);
            _pendingTransaction = pending;
            pendingSaved = true;
            OnPropertyChanged(nameof(CanSelectCamera));
            OnPropertyChanged(nameof(CanChangeConfirmations));
            OnPropertyChanged(nameof(BlockerText));
            _ = HardwareCameraAgentProtocolCodec.CreateCaptureRequest(
                transactionId,
                alias,
                expectedProfile,
                HardwareCaptureSafetyConfirmations.AllConfirmed,
                finiteLiveViewHandoffRequested);
            await _stateStore.MarkCaptureRequestDispatchAttemptedAsync(transactionId).ConfigureAwait(true);
            pending = pending with { CaptureRequestDispatchAttempted = true };
            _pendingTransaction = pending;
            dispatchAttempted = true;
            ActivityText = $"{alias} を一回撮影中。自動再試行はしません…";
            CaptureSummary = "撮影・PC永続化・exact cleanup処理中";
            PreviewPath = string.Empty;
            PreviewImage = null;

            var reply = await _operations
                .CaptureAsync(transactionId, alias, expectedProfile, finiteLiveViewHandoffRequested)
                .ConfigureAwait(true);
            if (reply.Payload.TerminalState is "Reserved" or "InProgress")
            {
                ActivityText = "transactionは処理中です。新規撮影せず結果を再照会してください。";
                CaptureSummary = "未確定（自動再試行なし）";
                return;
            }

            await ApplyTerminalResultAsync(reply).ConfigureAwait(true);
            if (restartContinuousLiveView && reply.Success &&
                reply.Payload.TerminalState == "Complete")
            {
                await StartContinuousLiveViewCoreAsync().ConfigureAwait(true);
                if (!IsContinuousLiveViewActive)
                {
                    ActivityText = "撮影は完了しましたが、継続Live Viewの再開に失敗しました。自動再試行しません。";
                }
            }
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            string? undispatchedStateFailure = null;
            if (pendingSaved && dispatchAttempted && IsConfirmedUndispatched(exception))
            {
                try
                {
                    await _stateStore.MarkCaptureRequestNotDispatchedAsync(transactionId).ConfigureAwait(true);
                    pending = pending with { CaptureRequestDispatchAttempted = false };
                    _pendingTransaction = pending;
                    dispatchAttempted = false;
                }
                catch (Exception stateException) when (
                    stateException is not OperationCanceledException and not OutOfMemoryException)
                {
                    undispatchedStateFailure = SafeMessage(stateException);
                }
            }

            if (pendingSaved)
            {
                if (dispatchAttempted)
                {
                    ActivityText = "応答を確定できません。新規撮影は禁止し、同じtransaction IDを照会してください。";
                    CaptureSummary = "未確定（再撮影禁止）";
                    TechnicalDetail =
                        $"transaction: {transactionId}\n" +
                        $"capture_response_unconfirmed: {SafeMessage(exception)}\n" +
                        (undispatchedStateFailure is null
                            ? string.Empty
                            : $"undispatched_state_update_failed: {undispatchedStateFailure}\n") +
                        "capture request dispatch attempted: true\n" +
                        "automatic retry count: 0";
                }
                else
                {
                    _localPreDispatchFailure = true;
                    ActivityText = "Camera Agent要求前にローカル準備が失敗しました。撮影0回として明示的に閉じられます。";
                    CaptureSummary = "開始前FailedPartial（撮影要求0回）";
                    TechnicalDetail =
                        $"transaction: {transactionId}\n" +
                        $"pre_dispatch_state_failed: {SafeMessage(exception)}\n" +
                        "capture request dispatch attempted: false\n" +
                        "automatic retry count: 0";
                }
            }
            else
            {
                _stateLoadFailed = true;
                ActivityText = "ローカルtransaction予約に失敗しました。カメラ要求は送信していません。再起動までfail-closedです。";
                CaptureSummary = "開始前に拒否（撮影0回）";
                TechnicalDetail =
                    $"transaction: {transactionId}\n" +
                    $"app_state_reservation_failed: {SafeMessage(exception)}\n" +
                    "capture request dispatched: false\nautomatic retry count: 0";
            }
        }
        finally
        {
            _readiness = null;
            IsBusy = false;
            NotifyAvailability();
        }
    }

    public async Task RecoverTransactionAsync()
    {
        if (!CanRecoverTransaction || _pendingTransaction is null)
        {
            return;
        }

        var pending = _pendingTransaction;
        IsBusy = true;
        ActivityText = $"transaction {pending.TransactionId} の結果を照会中…";
        try
        {
            await QueryPendingTransactionAsync(pending, CancellationToken.None).ConfigureAwait(true);
        }
        finally
        {
            IsBusy = false;
            NotifyAvailability();
        }
    }

    private async Task QueryPendingTransactionAsync(
        HardwarePendingTransaction pending,
        CancellationToken cancellationToken)
    {
        try
        {
            var reply = await _operations
                .GetTransactionResultAsync(
                    pending.TransactionId,
                    pending.CameraAlias,
                    new HardwareCaptureProfileSnapshot(
                        pending.CaptureProfileId,
                        pending.CaptureProfileVersion,
                        pending.CaptureProfileSha256,
                        pending.CaptureProfileExpiresAtUtc),
                    pending.LiveViewHandoffRequested,
                    cancellationToken)
                .ConfigureAwait(true);
            if (reply.ResultCode is "TransactionReserved" or "TransactionInProgress" or "TransactionNotFound" ||
                reply.Payload.TerminalState is "Reserved" or "InProgress")
            {
                ActivityText = reply.ResultCode switch
                {
                    "TransactionReserved" => "Camera Agentが同じtransactionを予約済みです。再撮影せず後で再照会してください。",
                    "TransactionInProgress" => "Camera Agentが同じtransactionを処理中です。再撮影せず後で再照会してください。",
                    _ => "transaction journalを確認できません。pendingを保持し、新規撮影せずサポート点検してください。",
                };
                CaptureSummary = $"未確定: {reply.ResultCode}";
                TechnicalDetail =
                    $"transaction: {pending.TransactionId}\n" +
                    $"mode: {pending.OperatingMode}\n" +
                    $"camera: {pending.CameraAlias}\n" +
                    $"resultCode: {reply.ResultCode}\n" +
                    "capture request dispatched by recovery: false\n" +
                    "automatic retry count: 0";
                return;
            }

            await ApplyTerminalResultAsync(reply).ConfigureAwait(true);
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            ActivityText = "結果照会に失敗しました。pending状態を保持し、新規撮影を禁止します。";
            TechnicalDetail =
                $"transaction: {pending.TransactionId}\n" +
                $"mode: {pending.OperatingMode}\n" +
                $"camera: {pending.CameraAlias}\n" +
                $"transaction_query_failed: {SafeMessage(exception)}\n" +
                "capture request dispatched by recovery: false\n" +
                "automatic retry count: 0";
        }
    }

    public async Task ExportAsync()
    {
        if (!CanExport || _captureResult?.RetainedOriginal is null)
        {
            return;
        }

        IsBusy = true;
        ActivityText = "検証済み単体原画像をbyte-identical copyで保存中…";
        try
        {
            var outputPath = await _exporter.ExportAsync(
                    _captureResult.RetainedOriginal,
                    _captureResult.TransactionId,
                    _timeProvider.GetUtcNow())
                .ConfigureAwait(true);
            LastExportPath = outputPath;
            ExportSummary = _captureResult.TerminalState == "Complete"
                ? "保存完了（単体原画像・byte-identical）"
                : "保存完了（FailedPartial保持原画像・byte-identical）";
            ActivityText = _captureResult.TerminalState == "Complete"
                ? "明示保存が完了しました。"
                : "失敗状態を維持したまま、検証済み保持原画像の明示保存が完了しました。";
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            ExportSummary = "保存失敗（上書き・自動再試行なし）";
            ActivityText = "保存結果を採用しませんでした。product originalは変更していません。";
            TechnicalDetail += $"\nexport_failed: {SafeMessage(exception)}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    private async Task ApplyTerminalResultAsync(
        HardwareCameraAgentReply<HardwareSingleCaptureResult> reply)
    {
        _captureResult = reply.Payload;
        _verifiedOriginal = null;
        var validationIssues = new List<string>();
        var handoffArtifactVerified = !reply.Payload.LiveViewHandoffRequested;
        LastTransactionId = reply.Payload.TransactionId;
        if (reply.Payload.RetainedOriginal is not null)
        {
            try
            {
                _verifiedOriginal = await HardwareArtifactVerifier
                    .VerifyOriginalAsync(reply.Payload.RetainedOriginal)
                    .ConfigureAwait(true);
                RetainedOriginalSummary =
                    $"{reply.Payload.RetainedOriginal.CameraAlias} / {_verifiedOriginal.SizeBytes:N0} bytes / SHA-256確認済み";
            }
            catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
            {
                RetainedOriginalSummary = "recordはありますが、アプリ側の再検証に失敗しました。保存不可です。";
                validationIssues.Add($"original_reread_failed: {SafeMessage(exception)}");
            }
        }
        else
        {
            RetainedOriginalSummary = "なし";
        }

        if (reply.Payload.LiveViewHandoffRequested)
        {
            if (!reply.Payload.LiveViewStoppedBeforeCapture ||
                !reply.Payload.LiveViewSdkSessionClosedBeforeCapture)
            {
                validationIssues.Add("live_view_handoff_close_not_verified");
            }

            if (reply.Payload.PostCaptureLiveViewProbeSucceeded && reply.Payload.PostCapturePreview is not null)
            {
                try
                {
                    var postCapturePreview = await HardwareArtifactVerifier
                        .VerifyPreviewAsync(reply.Payload.PostCapturePreview)
                        .ConfigureAwait(true);
                    PreviewPath = postCapturePreview.Path;
                    PreviewImage = LoadFrozenImage(postCapturePreview.Path);
                    LiveViewSummary = "撮影後の有限1フレームprobe成功（取得後に停止・SDK close済み）";
                    handoffArtifactVerified =
                        reply.Payload.LiveViewStoppedBeforeCapture &&
                        reply.Payload.LiveViewSdkSessionClosedBeforeCapture;
                }
                catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
                {
                    PreviewPath = string.Empty;
                    PreviewImage = null;
                    LiveViewSummary = "撮影後の有限Live View preview再検証に失敗";
                    validationIssues.Add($"post_capture_preview_invalid: {SafeMessage(exception)}");
                }
            }
            else
            {
                LiveViewSummary = reply.Payload.PostCaptureLiveViewProbeAttempted
                    ? "撮影後の有限Live View probe失敗（自動再試行なし）"
                    : "撮影後の有限Live View probe未実施";
            }
        }
        var completeAndVerified = reply.Success && reply.Payload.TerminalState == "Complete" &&
            _verifiedOriginal is not null && handoffArtifactVerified;
        CaptureSummary = completeAndVerified
            ? $"成功: {reply.Payload.CameraAlias} original.jpg"
            : $"{reply.Payload.TerminalState}: {reply.Payload.ErrorCategory}";
        ActivityText = completeAndVerified
            ? "一台撮影が完了しました。合成は対象外です。必要なら明示保存してください。"
            : "transactionは失敗または部分失敗で確定しました。自動再試行しません。";
        TechnicalDetail =
            $"transaction: {reply.Payload.TransactionId}\n" +
            $"run: {reply.Payload.RunId}\n" +
            $"resultCode: {reply.ResultCode}\n" +
            $"cameraMode: {reply.Payload.CameraMode}\n" +
            $"requiredCameraAlias: {reply.Payload.RequiredCameraAlias}\n" +
            $"captureProfile: {reply.Payload.CaptureProfileId} v{reply.Payload.CaptureProfileVersion}\n" +
            $"captureProfileSha256: {reply.Payload.CaptureProfileSha256}\n" +
            $"captureProfileExpiresAtUtc: {reply.Payload.CaptureProfileExpiresAtUtc?.ToString("O") ?? "none"}\n" +
            $"terminalState: {reply.Payload.TerminalState}\n" +
            $"errorCategory: {reply.Payload.ErrorCategory}\n" +
            $"spoolEmptyBefore: {reply.Payload.SpoolEmptyBeforeCapture}\n" +
            $"exactDeleteSucceeded: {reply.Payload.CameraObjectDeleteSucceeded}\n" +
            $"spoolEmptyAfter: {reply.Payload.SpoolEmptyAfterCleanup}\n" +
            $"liveViewHandoffRequested: {reply.Payload.LiveViewHandoffRequested}\n" +
            $"liveViewStoppedBeforeCapture: {reply.Payload.LiveViewStoppedBeforeCapture}\n" +
            $"liveViewSdkSessionClosedBeforeCapture: {reply.Payload.LiveViewSdkSessionClosedBeforeCapture}\n" +
            $"postCaptureLiveViewProbeAttempted: {reply.Payload.PostCaptureLiveViewProbeAttempted}\n" +
            $"postCaptureLiveViewProbeSucceeded: {reply.Payload.PostCaptureLiveViewProbeSucceeded}\n" +
            $"automatic retry count: {reply.Payload.AutomaticRetryCount}" +
            (validationIssues.Count == 0 ? string.Empty : $"\n{string.Join("\n", validationIssues)}");
    }

    public async Task PrepareNewCaptureAsync()
    {
        if (!CanPrepareNewCapture)
        {
            return;
        }

        IsBusy = true;
        ActivityText = "確認済みtransactionを閉じ、新しい撮影を準備中…";
        try
        {
            await _stateStore.ClearPendingAsync(_pendingTransaction!.TransactionId).ConfigureAwait(true);
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            ActivityText = "transaction状態を安全に閉じられません。新しい撮影を禁止します。";
            TechnicalDetail += $"\napp_state_clear_failed: {SafeMessage(exception)}";
            IsBusy = false;
            return;
        }

        _pendingTransaction = null;
        _captureResult = null;
        _verifiedOriginal = null;
        _readiness = null;
        _liveViewHandoffRequested = false;
        _localPreDispatchFailure = false;
        SetProperty(ref _exclusiveCameraControlConfirmed, false, nameof(ExclusiveCameraControlConfirmed));
        SetProperty(ref _dedicatedSpoolScopeConfirmed, false, nameof(DedicatedSpoolScopeConfirmed));
        SetProperty(ref _exactObjectDeleteConfirmed, false, nameof(ExactObjectDeleteConfirmed));
        CaptureSummary = "未撮影";
        RetainedOriginalSummary = "なし";
        LastTransactionId = string.Empty;
        ExportSummary = "未保存";
        LastExportPath = string.Empty;
        ReadinessSummary = "未確認";
        ReadinessDetail = "安全確認後、状態を再確認してください。";
        ObservedSettingsText = "未確認";
        PreviewPath = string.Empty;
        PreviewImage = null;
        LiveViewSummary = "未実行";
        ActivityText = "新しいSingleCamera transactionの準備を開始しました。";
        TechnicalDetail = "automatic retry count: 0";
        IsBusy = false;
        OnPropertyChanged(nameof(CanSelectCamera));
        OnPropertyChanged(nameof(CanChangeConfirmations));
        NotifyAvailability();
    }

    private void InvalidateReadiness(string reason)
    {
        CancelProfileExpiryInvalidation();
        _readiness = null;
        ReadinessSummary = "要再確認";
        ReadinessDetail = reason;
        OnPropertyChanged(nameof(BlockerText));
    }

    private void ScheduleProfileExpiryInvalidation()
    {
        CancelProfileExpiryInvalidation();
        if (_readiness?.Ready != true ||
            _readiness.CaptureProfileExpiresAtUtc is not { } profileExpiry)
        {
            return;
        }

        var now = _timeProvider.GetUtcNow();
        if (profileExpiry <= now)
        {
            TransitionToExpiredProfile(profileExpiry);
            return;
        }

        var generation = _profileExpiryGeneration;
        var synchronizationContext = SynchronizationContext.Current;
        var dueTime = profileExpiry - now;
        if (dueTime > MaximumProfileExpiryTimerDelay)
        {
            dueTime = MaximumProfileExpiryTimerDelay;
        }

        _profileExpiryTimer = _timeProvider.CreateTimer(
            _ =>
            {
                void ApplyOnOwnerContext()
                {
                    if (_disposed || generation != _profileExpiryGeneration ||
                        _readiness?.CaptureProfileExpiresAtUtc != profileExpiry)
                    {
                        return;
                    }

                    if (_timeProvider.GetUtcNow() < profileExpiry)
                    {
                        ScheduleProfileExpiryInvalidation();
                        return;
                    }

                    TransitionToExpiredProfile(profileExpiry);
                }

                if (synchronizationContext is null)
                {
                    ApplyOnOwnerContext();
                }
                else
                {
                    synchronizationContext.Post(_ => ApplyOnOwnerContext(), null);
                }
            },
            state: null,
            dueTime,
            Timeout.InfiniteTimeSpan);
    }

    private void TransitionToExpiredProfile(DateTimeOffset profileExpiry)
    {
        CancelProfileExpiryInvalidation();
        if (_readiness is not null)
        {
            _readiness = _readiness with
            {
                Ready = false,
                FailureCategory = "capture_profile_expired",
                FailureDetail =
                    $"The approved capture profile expired at {profileExpiry:O}; refresh readiness.",
            };
        }
        ReadinessSummary = "期限切れ";
        ReadinessDetail =
            $"承認profileの期限(UTC) {profileExpiry:O} を過ぎました。状態を再確認してください。";
        ActivityText = "capture profileが期限切れになったため、撮影をブロックしました。";
        OnPropertyChanged(nameof(BlockerText));
        NotifyAvailability();
    }

    private void CancelProfileExpiryInvalidation()
    {
        _profileExpiryGeneration++;
        _profileExpiryTimer?.Dispose();
        _profileExpiryTimer = null;
    }

    public async Task ShutdownAsync()
    {
        if (IsContinuousLiveViewActive)
        {
            _ = await StopContinuousLiveViewCoreAsync().ConfigureAwait(true);
        }
        _continuousLiveViewLoopCancellation?.Cancel();
        if (_continuousLiveViewLoop is not null)
        {
            try
            {
                await _continuousLiveViewLoop.ConfigureAwait(true);
            }
            catch (OperationCanceledException)
            {
            }
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _continuousLiveViewLoopCancellation?.Cancel();
        _continuousLiveViewLoopCancellation?.Dispose();
        CancelProfileExpiryInvalidation();
        GC.SuppressFinalize(this);
    }

    private void HandleCommandException(Exception exception)
    {
        ActivityText = "予期しないエラーをfail-closedで処理しました。";
        TechnicalDetail += $"\nunhandled_command_error: {SafeMessage(exception)}";
        IsBusy = false;
    }

    private void NotifyAvailability()
    {
        OnPropertyChanged(nameof(AgentAvailabilityText));
        OnPropertyChanged(nameof(CanCheckReadiness));
        OnPropertyChanged(nameof(CanProbeLiveView));
        OnPropertyChanged(nameof(CanStartContinuousLiveView));
        OnPropertyChanged(nameof(CanStopContinuousLiveView));
        OnPropertyChanged(nameof(CanCapture));
        OnPropertyChanged(nameof(CanRecoverTransaction));
        OnPropertyChanged(nameof(CanExport));
        OnPropertyChanged(nameof(CanPrepareNewCapture));
        OnPropertyChanged(nameof(CanApproveProfile));
        OnPropertyChanged(nameof(CanChangeExportDirectory));
        OnPropertyChanged(nameof(ExportDirectoryDisplay));
        OnPropertyChanged(nameof(BlockerText));
        ((AsyncRelayCommand)CheckReadinessCommand).NotifyCanExecuteChanged();
        ((AsyncRelayCommand)ProbeLiveViewCommand).NotifyCanExecuteChanged();
        ((AsyncRelayCommand)StartContinuousLiveViewCommand).NotifyCanExecuteChanged();
        ((AsyncRelayCommand)StopContinuousLiveViewCommand).NotifyCanExecuteChanged();
        ((AsyncRelayCommand)CaptureCommand).NotifyCanExecuteChanged();
        ((AsyncRelayCommand)RecoverTransactionCommand).NotifyCanExecuteChanged();
        ((AsyncRelayCommand)ExportCommand).NotifyCanExecuteChanged();
        ((AsyncRelayCommand)PrepareNewCaptureCommand).NotifyCanExecuteChanged();
        ((AsyncRelayCommand)ApproveProfileCommand).NotifyCanExecuteChanged();
    }

    private static string SafeMessage(Exception exception)
    {
        var message = string.Join(
            ' ',
            exception.Message.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
        return message.Length <= 400 ? message : message[..400];
    }

    private static ImageSource LoadFrozenImage(string path)
    {
        using var stream = new FileStream(
            path, FileMode.Open, FileAccess.Read, FileShare.Read,
            bufferSize: 64 * 1024, FileOptions.SequentialScan);
        return LoadFrozenImage(stream);
    }

    private static ImageSource? TryLoadFrozenImage(string path)
    {
        try
        {
            return LoadFrozenImage(path);
        }
        catch (Exception exception) when (
            exception is IOException or NotSupportedException or InvalidOperationException)
        {
            return null;
        }
    }

    private static ImageSource LoadFrozenImage(byte[] bytes)
    {
        using var stream = new MemoryStream(bytes, writable: false);
        return LoadFrozenImage(stream);
    }

    private static ImageSource LoadFrozenImage(Stream stream)
    {
        var image = new BitmapImage();
        image.BeginInit();
        image.CacheOption = BitmapCacheOption.OnLoad;
        image.StreamSource = stream;
        image.EndInit();
        image.Freeze();
        return image;
    }

    private static bool IsConfirmedUndispatched(Exception exception) =>
        exception is HardwareCameraAgentConnectException ||
        exception is HardwareCameraAgentLaunchException { RequestMayHaveBeenDispatched: false };

    private static string FormatObservedSettings(HardwareObservedCameraSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        return string.Join(
            " / ",
            FormatSetting("file", settings.FileType),
            FormatSetting("compression", settings.CompressionLevel),
            FormatSetting("size", settings.ImageSize),
            FormatSetting("exposure", settings.ExposureMode),
            FormatSetting("shutter", settings.ShutterSpeed),
            FormatSetting("aperture", settings.Aperture),
            FormatSetting("ISO", settings.Sensitivity),
            FormatSetting("WB", settings.WhiteBalanceMode),
            FormatSetting("focus", settings.FocusMode));
    }

    private static string FormatSetting(string name, HardwareObservedCameraSetting setting)
    {
        if (!setting.Available)
        {
            return $"{name}=unavailable({setting.ProbeState})";
        }

        var value = setting.CurrentLabel ??
            setting.CurrentValue?.ToString(System.Globalization.CultureInfo.InvariantCulture) ??
            setting.CurrentIndex?.ToString(System.Globalization.CultureInfo.InvariantCulture) ??
            "unknown";
        return $"{name}={value}";
    }

    private static bool IsIdentityReadyForLiveView(HardwareSingleReadinessResult? readiness) =>
        readiness is
        {
            SdkCameraCount: 1,
            WpdCameraCount: 1,
            SdkIdentityBound: true,
            WpdIdentityBound: true,
            SdkAliasMatches: true,
            WpdAliasMatches: true,
            SdkStatusProbed: true,
            ReadOnly: true,
            CaptureCommandSent: false,
            CameraObjectDeleteAttempted: false,
            CameraSettingsChanged: false,
            RealIdentifiersIncluded: false,
        };
}
