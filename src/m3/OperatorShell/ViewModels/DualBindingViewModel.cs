using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

/// <summary>
/// One SDK candidate the operator is deciding about. The ordinal is a session-local selector, not
/// identity: it is meaningless once the session ends and never leaves the screen.
/// </summary>
public sealed class DualBindingCandidateViewModel(int ordinal) : ObservableObject
{
    private string _assignedAlias = string.Empty;
    private bool _isSelected;

    public int Ordinal { get; } = ordinal;

    /// <summary>1-based for the operator. "候補0" reads as an error to anyone who is not a programmer.</summary>
    public string DisplayName => string.Create(CultureInfo.InvariantCulture, $"候補{Ordinal + 1}");

    public string AssignedAlias
    {
        get => _assignedAlias;
        set
        {
            if (SetProperty(ref _assignedAlias, value))
            {
                OnPropertyChanged(nameof(IsAssigned));
                OnPropertyChanged(nameof(StateKey));
                OnPropertyChanged(nameof(AssignmentText));
                OnPropertyChanged(nameof(AccessibleName));
            }
        }
    }

    public bool IsSelected
    {
        get => _isSelected;
        set
        {
            if (SetProperty(ref _isSelected, value))
            {
                OnPropertyChanged(nameof(StateKey));
                OnPropertyChanged(nameof(AccessibleName));
            }
        }
    }

    public bool IsAssigned => AssignedAlias.Length > 0;

    public string AssignmentText => IsAssigned ? AssignedAlias : "未割当";

    /// <summary>Resolved to a colour in XAML so the view model never holds a brush.</summary>
    public string StateKey => IsAssigned ? "assigned" : IsSelected ? "current" : "pending";

    /// <summary>
    /// Read aloud instead of the colour. The Block/Caution/Info wording exists because colour alone
    /// is not a state anyone can hear.
    /// </summary>
    public string AccessibleName => string.Create(
        CultureInfo.InvariantCulture,
        $"{DisplayName} {AssignmentText}{(IsSelected ? " 表示中" : string.Empty)}");
}

public enum DualBindingPhase
{
    /// <summary>No session yet. The operator has to start one.</summary>
    NotStarted,

    /// <summary>Candidates are being viewed and assigned, one Live View at a time.</summary>
    Collecting,

    /// <summary>Both aliases assigned; the operator reviews what they chose before committing.</summary>
    Summary,

    /// <summary>Binding confirmed. Capture may start.</summary>
    Ready,

    /// <summary>The binding stopped being trustworthy. Only a fresh session recovers.</summary>
    Invalid,
}

/// <summary>
/// Dual session binding confirmation (ADR-0025, Issue #62).
/// </summary>
/// <remarks>
/// <para>
/// The operator looks at one body's Live View at a time and says which of CAM-A / CAM-B it is.
/// There is no automatic same-body proof behind that decision -- HG-0003B was closed by accepting
/// that, not by finding one -- so this screen has two jobs: make the choice as hard to get wrong as
/// the UI can, and be honest that a wrong choice is not detectable afterwards.
/// </para>
/// <para>
/// Nothing here is persisted: no preview, no candidate ordinal, no source object, no assignment.
/// The binding lives in the Agent process that produced the candidates and dies with it, and this
/// screen holds no more than that.
/// </para>
/// </remarks>
public sealed class DualBindingViewModel : ObservableObject
{
    /// <summary>
    /// Shown whenever a binding is being decided. It names the risk the operator carries, because
    /// no part of the system can carry it for them.
    /// </summary>
    public const string ResidualRiskText =
        "CAM-A と CAM-B の割当は操作者の目視判断です。取り違えても自動検出できません。" +
        "実機の撮影可否は未承認（HardwarePending）で、二台の実シャッター開口時刻の同期は保証しません。";

    private readonly DualBindingSessionClient _client;

    private bool _isRequired;
    private DualBindingPhase _phase = DualBindingPhase.NotStarted;
    private DualBindingCandidateViewModel? _selectedCandidate;
    private ImageSource? _previewImage;
    private string _previewPlaceholderText = string.Empty;
    private string _noticeText = string.Empty;
    private string _noticeKind = "info";
    private bool _shutdownBlocked;
    private string _invalidationText = string.Empty;
    private bool _isBusy;
    // This remains true after the child has naturally exited. The dynamic
    // IsCaptureHostActivated property correctly becomes false then, but shutdown
    // must still never attempt to send cancel-binding to the retired pipe.
    private bool _captureHostActivationAcknowledged;

    public DualBindingViewModel(DualBindingSessionClient client, bool isRequired = false)
    {
        ArgumentNullException.ThrowIfNull(client);
        _client = client;
        _isRequired = isRequired;

        // Re-binding is available from Ready too. The operator is the one who knows they swapped a
        // body, and refusing to let them redo a binding they no longer trust would leave the only
        // recovery as restarting the app.
        BeginBindingCommand = new AsyncRelayCommand(
            BeginBindingAsync,
            () => !IsShutdownBlocked &&
                !IsCaptureHostActivated &&
                Phase is DualBindingPhase.NotStarted or DualBindingPhase.Invalid or DualBindingPhase.Ready,
            ReportFailure);
        ShowCandidateCommand = new AsyncRelayCommand<DualBindingCandidateViewModel>(
            ShowCandidateAsync,
            candidate => !IsShutdownBlocked && Phase == DualBindingPhase.Collecting && !candidate.IsAssigned,
            ReportFailure);
        AssignCameraACommand = new AsyncRelayCommand(
            () => ConfirmAliasAsync(DualBindingCameraAgentProtocol.CameraAliasA),
            () => CanAssign(DualBindingCameraAgentProtocol.CameraAliasA),
            ReportFailure);
        AssignCameraBCommand = new AsyncRelayCommand(
            () => ConfirmAliasAsync(DualBindingCameraAgentProtocol.CameraAliasB),
            () => CanAssign(DualBindingCameraAgentProtocol.CameraAliasB),
            ReportFailure);
        CompleteBindingCommand = new AsyncRelayCommand(
            CompleteBindingAsync,
            () => !IsShutdownBlocked && Phase == DualBindingPhase.Summary,
            ReportFailure);
    }

    public ObservableCollection<DualBindingCandidateViewModel> Candidates { get; } = [];

    public AsyncRelayCommand BeginBindingCommand { get; }

    public AsyncRelayCommand<DualBindingCandidateViewModel> ShowCandidateCommand { get; }

    public AsyncRelayCommand AssignCameraACommand { get; }

    public AsyncRelayCommand AssignCameraBCommand { get; }

    public AsyncRelayCommand CompleteBindingCommand { get; }

    public DualBindingPhase Phase
    {
        get => _phase;
        private set
        {
            if (SetProperty(ref _phase, value))
            {
                OnPropertyChanged(nameof(IsOverlayVisible));
                OnPropertyChanged(nameof(IsReady));
                OnPropertyChanged(nameof(IsSummaryVisible));
                OnPropertyChanged(nameof(IsCandidateStageVisible));
                OnPropertyChanged(nameof(HeadlineText));
                OnPropertyChanged(nameof(HeadlineKind));
                OnPropertyChanged(nameof(PhaseKey));
                NotifyCommandsChanged();
            }
        }
    }

    /// <summary>Lower-cased phase name, so XAML triggers match on one stable key.</summary>
    public string PhaseKey => Phase.ToString().ToLowerInvariant();

    /// <summary>
    /// True whenever a binding has to exist before anything else can happen. HardwareDual sets it
    /// at construction; a simulated run sets it only when the operator asks to see the flow, since
    /// there is no body to identify.
    /// </summary>
    public bool IsRequired
    {
        get => _isRequired;
        set
        {
            if (SetProperty(ref _isRequired, value))
            {
                OnPropertyChanged(nameof(IsOverlayVisible));
            }
        }
    }

    /// <summary>Covers the screen until the binding is Ready.</summary>
    public bool IsOverlayVisible => IsRequired && Phase != DualBindingPhase.Ready;

    /// <summary>
    /// The single gate on capture. There is no Single-camera fallback: a Dual capture on an
    /// unconfirmed binding would attribute one body's frame to the other alias, and nothing
    /// downstream could tell.
    /// </summary>
    public bool IsReady => Phase == DualBindingPhase.Ready;

    /// <summary>
    /// True after the one-time handoff from the binding pipe to the capture pipe.
    /// The same Agent process and the same CAM-A/B assignment remain in force;
    /// re-binding and binding-pipe freshness probes are no longer valid.
    /// </summary>
    public bool IsCaptureHostActivated => _client.CaptureHostActivated;

    public bool IsSummaryVisible => Phase == DualBindingPhase.Summary;

    public bool IsCandidateStageVisible => Phase == DualBindingPhase.Collecting;

    public bool IsBusy
    {
        get => _isBusy;
        private set => SetProperty(ref _isBusy, value);
    }

    public DualBindingCandidateViewModel? SelectedCandidate
    {
        get => _selectedCandidate;
        private set
        {
            var previous = _selectedCandidate;
            if (!SetProperty(ref _selectedCandidate, value))
            {
                return;
            }

            if (previous is not null)
            {
                previous.IsSelected = false;
            }

            if (value is not null)
            {
                value.IsSelected = true;
            }

            OnPropertyChanged(nameof(SelectedCandidateName));
            NotifyCommandsChanged();
        }
    }

    public string SelectedCandidateName => SelectedCandidate?.DisplayName ?? "なし";

    /// <summary>
    /// The transient Live View frame, decoded. Never written anywhere and dropped as soon as the
    /// next one arrives or the session ends.
    /// </summary>
    public ImageSource? PreviewImage
    {
        get => _previewImage;
        private set
        {
            if (SetProperty(ref _previewImage, value))
            {
                OnPropertyChanged(nameof(IsPreviewVisible));
                OnPropertyChanged(nameof(IsPreviewPlaceholderVisible));
            }
        }
    }

    public bool IsPreviewVisible => PreviewImage is not null;

    public bool IsPreviewPlaceholderVisible => PreviewImage is null && PreviewPlaceholderText.Length > 0;

    /// <summary>
    /// Shown when the frame is not a decodable image -- which is what the simulated agent always
    /// returns. It says so rather than drawing something that could be mistaken for a camera image.
    /// </summary>
    public string PreviewPlaceholderText
    {
        get => _previewPlaceholderText;
        private set
        {
            if (SetProperty(ref _previewPlaceholderText, value))
            {
                OnPropertyChanged(nameof(IsPreviewPlaceholderVisible));
            }
        }
    }

    public string HeadlineText => Phase switch
    {
        DualBindingPhase.NotStarted => "二台のカメラを識別します",
        DualBindingPhase.Collecting => "表示中のカメラを CAM-A / CAM-B に割り当ててください",
        DualBindingPhase.Summary => "割当を確認してください",
        DualBindingPhase.Ready => "機体照合が完了しました",
        _ => "再 binding が必要です",
    };

    /// <summary>
    /// Block / Caution / Info. Written out because colour alone is not a state a screen reader can
    /// convey, and because the operator may not be able to distinguish the colours.
    /// </summary>
    public string HeadlineKind => Phase switch
    {
        DualBindingPhase.Ready => "Info",
        DualBindingPhase.Invalid => "Block",
        DualBindingPhase.Summary => "Caution",
        _ => "Caution",
    };

    public string InvalidationText
    {
        get => _invalidationText;
        private set
        {
            if (SetProperty(ref _invalidationText, value))
            {
                OnPropertyChanged(nameof(IsInvalidationVisible));
            }
        }
    }

    public bool IsInvalidationVisible => InvalidationText.Length > 0;

    /// <summary>The overlay is asking the operator to start over, and says why.</summary>
    public bool RequiresRebindingText => Phase == DualBindingPhase.Invalid && InvalidationText.Length > 0;

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

    public bool IsNoticeVisible => NoticeText.Length > 0;

    /// <summary>block / caution / info, resolved to a colour in XAML.</summary>
    public string NoticeKind
    {
        get => _noticeKind;
        private set => SetProperty(ref _noticeKind, value);
    }

    public ObservableCollection<string> SummaryLines { get; } = [];

    public string EvidenceSummaryText => _client.Evidence.Count == 0
        ? "なし"
        : string.Join(
            " / ",
            _client.Evidence.Select(evidence => $"{evidence.CameraAlias} {evidence.ConfirmedAtUtc}"));

    /// <summary>
    /// Confirms the binding is still the one the Agent is serving, and reports it if not.
    /// </summary>
    /// <remarks>
    /// Called immediately before a capture starts. Between confirming a binding and pressing the
    /// shutter there is a window in which a body can be unplugged, and a request/response protocol
    /// has no way to tell anyone until something asks. Returns true when capture may proceed.
    /// </remarks>
    public async Task<bool> VerifyBindingIsCurrentAsync(CancellationToken cancellationToken = default)
    {
        if (!IsRequired)
        {
            return true;
        }

        // After activation the binding pipe is intentionally closed and the
        // capture host owns the same session-local assignment. Native validates
        // that topology on capture; sending complete-binding again would be an
        // invalid cross-protocol operation.
        if (IsCaptureHostActivated)
        {
            return true;
        }

        if (!IsReady)
        {
            return false;
        }

        var refusal = await _client.VerifyBindingIsCurrentAsync(cancellationToken).ConfigureAwait(true);
        if (refusal is null)
        {
            return true;
        }

        ApplyRefusal(refusal);
        return false;
    }

    /// <summary>
    /// Performs the exactly-once Ready binding handoff immediately before the
    /// first CaptureRecoveryOnly reservation. A refusal is shown and no capture
    /// operation may follow it.
    /// </summary>
    public async Task<bool> ActivateCaptureAsync(CancellationToken cancellationToken = default)
    {
        if (IsCaptureHostActivated)
        {
            return true;
        }
        if (!IsReady)
        {
            return false;
        }

        var reply = await _client.ActivateCaptureAsync(cancellationToken).ConfigureAwait(true);
        if (reply.Value is not null)
        {
            _captureHostActivationAcknowledged = true;
            OnPropertyChanged(nameof(IsCaptureHostActivated));
            NotifyCommandsChanged();
            Notify("機体照合を同じAgentの撮影処理へ引き継ぎました。", "info");
            return true;
        }

        ApplyRefusal(reply.Refusal!);
        OnPropertyChanged(nameof(IsCaptureHostActivated));
        return false;
    }

    /// <summary>
    /// True when window shutdown could not prove SDK cleanup and natural Agent
    /// exit. The binding UI remains visible but every operation is locked while
    /// the process-wide hardware lease stays owned.
    /// </summary>
    public bool IsShutdownBlocked
    {
        get => _shutdownBlocked;
        private set
        {
            if (SetProperty(ref _shutdownBlocked, value))
            {
                NotifyCommandsChanged();
            }
        }
    }

    /// <summary>
    /// Window-shutdown path for a hardware binding that has not transitioned to
    /// capture. The native acknowledgment proves that Live View and the retained
    /// SDK session were ended; there is no retry if cleanup is refused.
    /// </summary>
    public async Task<DualBindingRefusal?> CancelBindingOnShutdownAsync(
        CancellationToken cancellationToken = default)
    {
        // Activation already performed the native Live View/SDK handoff. The
        // binding pipe can no longer accept cancel-binding; process cleanup is
        // owned by DualCameraAgentLifecycle.DisposeAsync instead.
        if (_captureHostActivationAcknowledged)
        {
            return null;
        }

        var reply = await _client.CancelBindingAsync(cancellationToken).ConfigureAwait(true);
        if (reply.Value is not null)
        {
            ClearSessionSurface();
            Phase = DualBindingPhase.NotStarted;
            InvalidationText = string.Empty;
            Notify("機体照合を終了し、SDKセッションを閉じました。", "info");
            return null;
        }

        if (reply.Refusal is { ResultCode: "NoBindingSession" })
        {
            return null;
        }

        if (reply.Refusal is { } refusal)
        {
            ApplyRefusal(refusal);
            return refusal;
        }

        return new DualBindingRefusal
        {
            ResultCode = "BindingCleanupFailed",
            State = DualBindingSessionState.Invalid,
            InvalidationReason = DualBindingInvalidationReason.SdkError,
            Detail = "Binding cancellation returned no result.",
        };
    }

    public void ReportShutdownBlocked(string blockingCode)
    {
        // A timeout after ActivateCapture must keep the one-way handoff marker:
        // the next explicit window-close attempt must wait again, never send the
        // retired binding pipe a cancel-binding request because the child happened
        // to exit between attempts.
        ClearSessionSurface(preserveCaptureHostActivationAcknowledgement: true);
        IsShutdownBlocked = true;
        Phase = DualBindingPhase.Invalid;
        InvalidationText =
            "実機セッションの終了を確認できないため、この画面と実機の排他を保持しています。" +
            "自動再試行やAgentの強制終了は行いません。実機操作を止めたまま技術担当者が確認してください。" +
            $"（状態: {blockingCode}）";
        Notify(InvalidationText, "block");
    }

    private bool CanAssign(string alias) =>
        !IsShutdownBlocked &&
        Phase == DualBindingPhase.Collecting &&
        SelectedCandidate is { IsAssigned: false } &&
        !Candidates.Any(candidate => string.Equals(candidate.AssignedAlias, alias, StringComparison.Ordinal));

    private async Task BeginBindingAsync()
    {
        IsBusy = true;
        try
        {
            ClearSessionSurface();
            var reply = await _client.BeginBindingAsync().ConfigureAwait(true);
            OnPropertyChanged(nameof(IsCaptureHostActivated));
            if (reply.Value is not { } started)
            {
                ApplyRefusal(reply.Refusal!);
                return;
            }

            foreach (var ordinal in started.CandidateOrdinals)
            {
                Candidates.Add(new DualBindingCandidateViewModel(ordinal));
            }

            InvalidationText = string.Empty;
            Phase = DualBindingPhase.Collecting;
            Notify("候補を 2 台取得しました。1 台ずつ確認してください。", "info");
        }
        finally
        {
            IsBusy = false;
        }
    }

    private async Task ShowCandidateAsync(DualBindingCandidateViewModel candidate)
    {
        IsBusy = true;
        try
        {
            // Starting this one stops the other: the agent allows exactly one Live View, and the
            // screen must never render two previews side by side. Comparing them at a glance is
            // precisely the mistake this flow is shaped to prevent.
            var started = await _client.StartCandidateLiveViewAsync(candidate.Ordinal).ConfigureAwait(true);
            if (!started.Succeeded)
            {
                ApplyRefusal(started.Refusal!);
                return;
            }

            SelectedCandidate = candidate;
            var frame = await _client.GetCandidateLiveViewFrameAsync(candidate.Ordinal).ConfigureAwait(true);
            if (frame.Value is not { } preview)
            {
                PreviewImage = null;
                PreviewPlaceholderText = "ライブ表示を取得できませんでした";
                ApplyRefusal(frame.Refusal!);
                return;
            }

            ApplyPreview(preview);
        }
        finally
        {
            IsBusy = false;
        }
    }

    private void ApplyPreview(DualBindingFrameResult preview)
    {
        try
        {
            var image = new BitmapImage();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.StreamSource = new MemoryStream(preview.Frame, writable: false);
            image.EndInit();
            image.Freeze();
            PreviewImage = image;
            PreviewPlaceholderText = string.Empty;
        }
        catch (Exception exception) when (exception is NotSupportedException or ArgumentException or IOException)
        {
            // The simulated agent returns generated bytes, not an image, and says so rather than
            // drawing something an operator could mistake for a camera frame.
            PreviewImage = null;
            PreviewPlaceholderText = string.Create(
                CultureInfo.InvariantCulture,
                $"模擬フレーム {preview.FrameBytes:N0} バイト（カメラ画像ではありません）");
        }
    }

    private async Task ConfirmAliasAsync(string alias)
    {
        if (SelectedCandidate is not { } candidate)
        {
            return;
        }

        IsBusy = true;
        try
        {
            var reply = await _client.ConfirmAliasAsync(candidate.Ordinal, alias).ConfigureAwait(true);
            if (reply.Value is null)
            {
                ApplyRefusal(reply.Refusal!);
                return;
            }

            candidate.AssignedAlias = alias;
            // The assignment stopped this candidate's Live View and closed its SDK session, so the
            // preview it produced is gone too.
            PreviewImage = null;
            PreviewPlaceholderText = string.Empty;
            SelectedCandidate = null;

            if (Candidates.All(item => item.IsAssigned))
            {
                BuildSummary();
                Phase = DualBindingPhase.Summary;
                Notify("両方の割当が終わりました。内容を確認してください。", "caution");
            }
            else
            {
                Notify($"{candidate.DisplayName} を {alias} に割り当てました。", "info");
                NotifyCommandsChanged();
            }
        }
        finally
        {
            IsBusy = false;
        }
    }

    private async Task CompleteBindingAsync()
    {
        IsBusy = true;
        try
        {
            var reply = await _client.CompleteBindingAsync().ConfigureAwait(true);
            if (reply.Value is null)
            {
                ApplyRefusal(reply.Refusal!);
                return;
            }

            Phase = DualBindingPhase.Ready;
            OnPropertyChanged(nameof(EvidenceSummaryText));
            Notify("機体照合が完了しました。撮影を開始できます。", "info");
        }
        finally
        {
            IsBusy = false;
        }
    }

    private void BuildSummary()
    {
        SummaryLines.Clear();
        foreach (var candidate in Candidates.OrderBy(item => item.AssignedAlias, StringComparer.Ordinal))
        {
            SummaryLines.Add($"{candidate.AssignedAlias} ← {candidate.DisplayName}");
        }
    }

    private void ApplyRefusal(DualBindingRefusal refusal)
    {
        if (refusal.RequiresRebinding)
        {
            ClearSessionSurface();
            Phase = DualBindingPhase.Invalid;
            InvalidationText = refusal.ResultCode switch
            {
                "BindingInvalidated" => $"binding が無効になりました（理由: {ReasonText(refusal.InvalidationReason)}）。最初からやり直してください。",
                "SessionMismatch" => "Agent が再起動したため、以前の binding は失効しました。最初からやり直してください。",
                _ => "ライブ表示の停止または SDK セッションの終了を確認できませんでした。最初からやり直してください。",
            };
            Notify(InvalidationText, "block");
            return;
        }

        // Everything else leaves the session usable; the operator retries the step.
        Notify(RefusalText(refusal), "caution");
        NotifyCommandsChanged();
    }

    private static string RefusalText(DualBindingRefusal refusal) => refusal.ResultCode switch
    {
        "NoBindingSession" => "binding が開始されていません。",
        "AliasAlreadyAssigned" => "その割当先は既に使われています。",
        "CandidateAlreadyAssigned" => "この候補は既に割り当て済みです。",
        "UnknownCameraAlias" => "割当先は CAM-A か CAM-B のみです。",
        "LiveViewStartFailed" => "ライブ表示を開始できませんでした。",
        "LiveViewNotActive" => "この候補のライブ表示は動作していません。",
        "LiveViewFrameUnavailable" => "ライブ表示のフレームを取得できませんでした。",
        "LiveViewFrameTooLarge" => "ライブ表示のフレームが上限を超えています。",
        "CandidateCountNotTwo" => "接続台数が 2 台ではありません。2 台だけ接続してやり直してください。",
        "DuplicateCandidateSourceObject" => "2 台を区別できませんでした。接続し直してやり直してください。",
        "CandidateSourceObjectMissing" => "候補の取得に失敗しました。接続し直してやり直してください。",
        "SdkUnavailable" => "カメラ接続が利用できません。",
        _ => $"binding の操作が拒否されました（{refusal.ResultCode}）。",
    };

    private static string ReasonText(DualBindingInvalidationReason reason) => reason switch
    {
        DualBindingInvalidationReason.AgentRestart => "Agent の再起動",
        DualBindingInvalidationReason.UsbReconnect => "USB の再接続",
        DualBindingInvalidationReason.CameraCountChanged => "接続台数の変化",
        DualBindingInvalidationReason.TopologyChanged => "接続構成の変化",
        DualBindingInvalidationReason.SdkManagerRecreated => "SDK の再初期化",
        DualBindingInvalidationReason.SdkError => "SDK エラー",
        _ => "不明",
    };

    private void ClearSessionSurface(bool preserveCaptureHostActivationAcknowledgement = false)
    {
        if (!preserveCaptureHostActivationAcknowledgement)
        {
            _captureHostActivationAcknowledged = false;
        }
        Candidates.Clear();
        SummaryLines.Clear();
        SelectedCandidate = null;
        PreviewImage = null;
        PreviewPlaceholderText = string.Empty;
        OnPropertyChanged(nameof(EvidenceSummaryText));
    }

    private void Notify(string text, string kind)
    {
        NoticeKind = kind;
        NoticeText = text;
    }

    private void ReportFailure(Exception exception) =>
        Notify($"binding の通信に失敗しました: {exception.Message}", "block");

    private void NotifyCommandsChanged()
    {
        BeginBindingCommand.NotifyCanExecuteChanged();
        ShowCandidateCommand.NotifyCanExecuteChanged();
        AssignCameraACommand.NotifyCanExecuteChanged();
        AssignCameraBCommand.NotifyCanExecuteChanged();
        CompleteBindingCommand.NotifyCanExecuteChanged();
    }
}
