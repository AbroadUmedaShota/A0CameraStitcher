using System.Collections.ObjectModel;
using System.IO;
using System.Windows.Input;
using A0CameraStitcher.M3.Foundation;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

public sealed class OperatorShellViewModel : ObservableObject
{
    public const string SimulationBanner = "SIMULATED / 実機未接続";

    private static readonly SimulatedTransactionState[] NormalFlow =
    [
        SimulatedTransactionState.Idle,
        SimulatedTransactionState.CaptureA,
        SimulatedTransactionState.PersistA,
        SimulatedTransactionState.CaptureB,
        SimulatedTransactionState.PersistB,
        SimulatedTransactionState.Complete,
    ];

    private readonly ISimulatedTransactionService _transactionService;
    private readonly AsyncRelayCommand _runSuccessCommand;
    private readonly AsyncRelayCommand _runFailCaptureACommand;
    private readonly AsyncRelayCommand _runFailCaptureBCommand;
    private readonly AsyncRelayCommand _runCrashAfterPersistACommand;
    private readonly AsyncRelayCommand _recoverCommand;
    private readonly RelayCommand _applyPreviewCommand;
    private CancellationToken _lifetimeToken;
    private bool _isBusy;
    private string _selectedCamera = "CAM-A";
    private SimulatedTransactionState _selectedPreviewState = SimulatedTransactionState.Idle;
    private SimulatedTransactionState _displayedState = SimulatedTransactionState.Idle;
    private string _runStatus = "初期化待ち — この画面は実機へ接続しません。";
    private string _cameraAStatus = "待機（実機未接続）";
    private string _cameraBStatus = "待機（実機未接続）";
    private string _lastTransactionId = "未実行";
    private string _lastResultState = "未実行";
    private string _lastScenario = "未実行";
    private string _retainedOriginals = "なし";
    private string _terminalReason = "なし";
    private string _warningMessage = "自動再試行は禁止です。FailedPartial後は新しいtransactionで操作者が再実行します。";

    public OperatorShellViewModel(ISimulatedTransactionService transactionService)
    {
        _transactionService = transactionService ?? throw new ArgumentNullException(nameof(transactionService));
        ProgressSteps =
        [
            new(SimulatedTransactionState.Idle, "Idle"),
            new(SimulatedTransactionState.CaptureA, "CAM-A 撮影"),
            new(SimulatedTransactionState.PersistA, "CAM-A 保存"),
            new(SimulatedTransactionState.CaptureB, "CAM-B 撮影"),
            new(SimulatedTransactionState.PersistB, "CAM-B 保存"),
            new(SimulatedTransactionState.Complete, "Complete"),
            new(SimulatedTransactionState.FailedPartial, "FailedPartial"),
        ];

        _runSuccessCommand = CreateScenarioCommand(SimulatedWorkflowScenario.Success);
        _runFailCaptureACommand = CreateScenarioCommand(SimulatedWorkflowScenario.FailCaptureA);
        _runFailCaptureBCommand = CreateScenarioCommand(SimulatedWorkflowScenario.FailCaptureB);
        _runCrashAfterPersistACommand = CreateScenarioCommand(SimulatedWorkflowScenario.CrashAfterPersistA);
        _recoverCommand = new AsyncRelayCommand(RecoverAsync, CanOperate, HandleCommandException);
        _applyPreviewCommand = new RelayCommand(ApplySelectedPreview, () => !IsBusy);
        UpdateDisplay(SimulatedTransactionState.Idle, []);
    }

    public string BannerText => SimulationBanner;

    public IReadOnlyList<string> CameraAliases { get; } = ["CAM-A", "CAM-B"];

    public IReadOnlyList<SimulatedTransactionState> PreviewStates { get; } =
        Enum.GetValues<SimulatedTransactionState>();

    public ObservableCollection<ProgressStepViewModel> ProgressSteps { get; }

    public ICommand RunSuccessCommand => _runSuccessCommand;

    public ICommand RunFailCaptureACommand => _runFailCaptureACommand;

    public ICommand RunFailCaptureBCommand => _runFailCaptureBCommand;

    public ICommand RunCrashAfterPersistACommand => _runCrashAfterPersistACommand;

    public ICommand RecoverCommand => _recoverCommand;

    public ICommand ApplyPreviewCommand => _applyPreviewCommand;

    public bool IsBusy
    {
        get => _isBusy;
        private set
        {
            if (SetProperty(ref _isBusy, value))
            {
                OnPropertyChanged(nameof(ActivityText));
                NotifyCommandStateChanged();
            }
        }
    }

    public string ActivityText => IsBusy ? "SIMULATED処理中" : "操作可能（実機未接続）";

    public string SelectedCamera
    {
        get => _selectedCamera;
        set
        {
            if (value is not ("CAM-A" or "CAM-B"))
            {
                return;
            }

            if (SetProperty(ref _selectedCamera, value))
            {
                OnPropertyChanged(nameof(LiveViewPlaceholder));
            }
        }
    }

    public string LiveViewPlaceholder =>
        $"{SelectedCamera} 選択中\n\nSIMULATED LIVE VIEW PLACEHOLDER\n非実画像 / SDK・WPD・カメラへ接続しません";

    public SimulatedTransactionState SelectedPreviewState
    {
        get => _selectedPreviewState;
        set => SetProperty(ref _selectedPreviewState, value);
    }

    public SimulatedTransactionState DisplayedState
    {
        get => _displayedState;
        private set => SetProperty(ref _displayedState, value);
    }

    public string RunStatus
    {
        get => _runStatus;
        private set => SetProperty(ref _runStatus, value);
    }

    public string CameraAStatus
    {
        get => _cameraAStatus;
        private set => SetProperty(ref _cameraAStatus, value);
    }

    public string CameraBStatus
    {
        get => _cameraBStatus;
        private set => SetProperty(ref _cameraBStatus, value);
    }

    public string LastTransactionId
    {
        get => _lastTransactionId;
        private set => SetProperty(ref _lastTransactionId, value);
    }

    public string LastResultState
    {
        get => _lastResultState;
        private set => SetProperty(ref _lastResultState, value);
    }

    public string LastScenario
    {
        get => _lastScenario;
        private set => SetProperty(ref _lastScenario, value);
    }

    public string RetainedOriginals
    {
        get => _retainedOriginals;
        private set => SetProperty(ref _retainedOriginals, value);
    }

    public string TerminalReason
    {
        get => _terminalReason;
        private set => SetProperty(ref _terminalReason, value);
    }

    public string WarningMessage
    {
        get => _warningMessage;
        private set => SetProperty(ref _warningMessage, value);
    }

    public async Task InitializeAsync(CancellationToken cancellationToken)
    {
        _lifetimeToken = cancellationToken;
        await RecoverCoreAsync(isInitialLoad: true).ConfigureAwait(true);
    }

    private AsyncRelayCommand CreateScenarioCommand(SimulatedWorkflowScenario scenario) =>
        new(() => RunScenarioAsync(scenario), CanOperate, HandleCommandException);

    private bool CanOperate() => !IsBusy;

    private async Task RunScenarioAsync(SimulatedWorkflowScenario scenario)
    {
        IsBusy = true;
        LastScenario = scenario.ToString();
        RunStatus = $"{scenario} をSIMULATED実行中。実機には接続していません。";
        WarningMessage = "自動再試行なし。失敗時も同じtransactionを再開しません。";
        UpdateDisplay(SimulatedTransactionState.Idle, []);

        try
        {
            var transactionId = Guid.NewGuid();
            var result = await _transactionService.ExecuteAsync(
                transactionId,
                scenario,
                _lifetimeToken).ConfigureAwait(true);
            ApplyResult(result);

            if (!result.IsTerminal)
            {
                RunStatus = "擬似クラッシュで未完了journalを保持しました。『起動時回復を再検査』でFailedPartialへ確定できます。";
                WarningMessage = "未完了transactionは自動再開しません。回復処理はFailedPartialへ閉じ、原画像を保持します。";
            }
            else if (result.State == SimulatedTransactionState.Complete)
            {
                RunStatus = "SIMULATED transactionがCompleteになりました。これは実機成功の証拠ではありません。";
            }
            else
            {
                RunStatus = "SIMULATED transactionをFailedPartialで終了しました。自動再試行は行っていません。";
                WarningMessage = "FailedPartial: 取得済みsimulated原画像は保持。再操作には新しいtransactionが必要です。";
            }
        }
        catch (OperationCanceledException) when (_lifetimeToken.IsCancellationRequested)
        {
            RunStatus = "window終了によりSIMULATED操作を中止しました。自動再試行は行いません。";
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            ShowUnexpectedFailure(exception);
        }
        finally
        {
            IsBusy = false;
        }
    }

    private Task RecoverAsync() => RecoverCoreAsync(isInitialLoad: false);

    private async Task RecoverCoreAsync(bool isInitialLoad)
    {
        IsBusy = true;
        RunStatus = isInitialLoad
            ? "SIMULATED durable journalを確認中。実カメラ探索は行いません。"
            : "未完了SIMULATED journalを再検査中。";

        try
        {
            var recovered = await _transactionService.InitializeAsync(_lifetimeToken).ConfigureAwait(true);
            if (recovered.Count == 0)
            {
                RunStatus = isInitialLoad
                    ? "SIMULATED operator shell準備完了。実機未接続です。"
                    : "回復対象の未完了SIMULATED transactionはありません。";
                return;
            }

            var latest = recovered[^1];
            ApplyResult(latest);
            LastScenario = "StartupRecovery";
            RunStatus = $"未完了SIMULATED transaction {recovered.Count}件をFailedPartialで確定しました。";
            WarningMessage = "起動時回復は未完了transactionを再開せずFailedPartialへ閉じます。自動再試行は0回です。";
        }
        catch (OperationCanceledException) when (_lifetimeToken.IsCancellationRequested)
        {
            RunStatus = "SIMULATED journal確認を中止しました。";
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            ShowUnexpectedFailure(exception);
        }
        finally
        {
            IsBusy = false;
        }
    }

    private void ApplySelectedPreview()
    {
        var aliases = SelectedPreviewState switch
        {
            SimulatedTransactionState.CaptureB or SimulatedTransactionState.PersistB => new[] { "CAM-A" },
            SimulatedTransactionState.Complete => new[] { "CAM-A", "CAM-B" },
            SimulatedTransactionState.FailedPartial => new[] { "CAM-A" },
            _ => [],
        };
        UpdateDisplay(SelectedPreviewState, aliases);
        RunStatus = $"表示デモ: {SelectedPreviewState}（transactionは実行していません）。";
        WarningMessage = SelectedPreviewState == SimulatedTransactionState.FailedPartial
            ? "表示デモのみ: FailedPartialでは原画像を保持し、自動再試行せず、新しいtransactionを要求します。"
            : "表示デモのみです。実機状態、撮影成功、保存成功を示しません。";
    }

    private void ApplyResult(SimulatedWorkflowState result)
    {
        if (!result.Simulation || !string.Equals(result.Marker, "Simulated", StringComparison.Ordinal))
        {
            throw new InvalidDataException("実機非接続shellはSimulated markerのない結果を表示できません。");
        }

        LastTransactionId = result.TransactionId.ToString("N");
        LastResultState = result.State.ToString();
        RetainedOriginals = result.RetainedOriginalAliases.Count == 0
            ? "なし"
            : string.Join(", ", result.RetainedOriginalAliases) + "（simulated）";
        TerminalReason = string.IsNullOrWhiteSpace(result.TerminalReason)
            ? "なし"
            : result.TerminalReason;
        UpdateDisplay(result.State, result.RetainedOriginalAliases);
    }

    private void UpdateDisplay(
        SimulatedTransactionState state,
        IReadOnlyCollection<string> retainedAliases)
    {
        DisplayedState = state;
        foreach (var step in ProgressSteps)
        {
            step.SetPending();
        }

        if (state == SimulatedTransactionState.FailedPartial)
        {
            ProgressSteps.Single(step => step.State == SimulatedTransactionState.Idle).SetCompleted();
            if (retainedAliases.Contains("CAM-A", StringComparer.Ordinal))
            {
                ProgressSteps.Single(step => step.State == SimulatedTransactionState.CaptureA).SetCompleted();
                ProgressSteps.Single(step => step.State == SimulatedTransactionState.PersistA).SetCompleted();
            }

            if (retainedAliases.Contains("CAM-B", StringComparer.Ordinal))
            {
                ProgressSteps.Single(step => step.State == SimulatedTransactionState.CaptureB).SetCompleted();
                ProgressSteps.Single(step => step.State == SimulatedTransactionState.PersistB).SetCompleted();
            }

            ProgressSteps.Single(step => step.State == SimulatedTransactionState.FailedPartial).SetFailure();
        }
        else
        {
            var currentIndex = Array.IndexOf(NormalFlow, state);
            for (var index = 0; index < currentIndex; ++index)
            {
                ProgressSteps.Single(step => step.State == NormalFlow[index]).SetCompleted();
            }

            ProgressSteps.Single(step => step.State == state).SetCurrent();
        }

        CameraAStatus = GetCameraStatus("CAM-A", state, retainedAliases);
        CameraBStatus = GetCameraStatus("CAM-B", state, retainedAliases);
    }

    private static string GetCameraStatus(
        string alias,
        SimulatedTransactionState state,
        IReadOnlyCollection<string> retainedAliases)
    {
        if (retainedAliases.Contains(alias, StringComparer.Ordinal))
        {
            return "simulated原画像保持（実機未接続）";
        }

        return (alias, state) switch
        {
            ("CAM-A", SimulatedTransactionState.CaptureA) => "撮影デモ中（非実画像）",
            ("CAM-A", SimulatedTransactionState.PersistA) => "保存デモ中（非実画像）",
            ("CAM-B", SimulatedTransactionState.CaptureB) => "撮影デモ中（非実画像）",
            ("CAM-B", SimulatedTransactionState.PersistB) => "保存デモ中（非実画像）",
            (_, SimulatedTransactionState.FailedPartial) => "停止（FailedPartial）",
            _ => "待機（実機未接続）",
        };
    }

    private void HandleCommandException(Exception exception) => ShowUnexpectedFailure(exception);

    private void ShowUnexpectedFailure(Exception exception)
    {
        LastResultState = "ShellError";
        TerminalReason = exception.GetType().Name;
        RunStatus = "SIMULATED shell内でエラーが発生しました。自動再試行は行いません。";
        WarningMessage = $"{exception.Message} 新しい操作は内容を確認してから明示的に実行してください。";
    }

    private void NotifyCommandStateChanged()
    {
        _runSuccessCommand.NotifyCanExecuteChanged();
        _runFailCaptureACommand.NotifyCanExecuteChanged();
        _runFailCaptureBCommand.NotifyCanExecuteChanged();
        _runCrashAfterPersistACommand.NotifyCanExecuteChanged();
        _recoverCommand.NotifyCanExecuteChanged();
        _applyPreviewCommand.NotifyCanExecuteChanged();
    }
}
