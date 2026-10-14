using System.Globalization;
using System.IO;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using A0CameraStitcher.M3.Foundation.Hardware;
using A0CameraStitcher.M3.OperatorShell.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

/// <summary>
/// Every word the dual Live stage shows, in one place. The wording rule (Issue #291): nothing here
/// says "同時" or "同期" -- the two cameras' pictures are taken at different moments, and the screen
/// must not suggest otherwise. A test walks these constants and the view models' text properties.
/// </summary>
public static class DualLiveStageText
{
    public const string PermanentNote =
        "確認用の表示のみ・原画像／合成には使いません。2 台の画像は撮った時刻が違います";

    public const string LiveBadge = "Live";
    public const string StillBadgePrefix = "静止画";
    public const string LastReceivedPrefix = "最終受信";
    public const string NotAcquiredBadge = "静止画なし";
    public const string OpeningBadge = "準備中";
    public const string UpdatingBadge = "更新中";
    public const string StoppedBadge = "映像停止";
    public const string StoppedImageNote = "この画像は停止前のものです";
    public const string NotAcquiredPlaceholder = "[両方を更新] で取得できます";
    public const string WaitingPlaceholder = "画像を待っています";
    public const string StoppedPlaceholder = "画像はありません";
    public const string SwitchButton = "ライブ表示に切り替える";
    public const string RefreshButton = "両方を更新";

    public const string ClosingForCapture = "ライブ表示を閉じています";
    public const string RemainingUnknown = "撮影できる残り時間を確認できません";
    public const string RemainingCautionSoon = "まもなく撮影できなくなります。撮影するなら今です";
    public const string RemainingExpired = "撮影を受け付ける時間が過ぎました。機体照合をやり直してください";
    public const string AfterCapture = "撮影後は機体照合のやり直しが必要です";

    public const string DisabledNotBound = "機体照合が済んでいません";
    public const string DisabledSwitching = "表示を切り替えています";
    public const string DisabledCapturing = "撮影中は更新できません";
    public const string DisabledRemainingUnknown = "撮影できる残り時間を確認できないため、切り替えできません";
    public const string DisabledRemainingShort =
        "撮影できる残り時間が足りません。撮影するか、機体照合をやり直してください";
    public const string DisabledExpired = "撮影を受け付ける時間が過ぎました";

    public const string BindingLostUsb = "USB の接続が変わりました。機体照合をやり直してください";
    public const string BindingLostCameraCount = "接続されているカメラの台数が変わりました。機体照合をやり直してください";
    public const string BindingLostTopology = "カメラの接続構成が変わりました。機体照合をやり直してください";
    public const string BindingLostAgent = "Camera Agent が終了したため、機体照合が切れました。機体照合をやり直してください";
    public const string BindingLostUnconfirmed = "ライブ表示を安全に閉じられませんでした。機体照合をやり直してください";
    public const string Unexpected = "ライブ表示の操作に失敗しました。状態を確認してから、もう一度操作してください";

    public static string Switching(string alias) => $"{alias} のライブ表示に切り替えています";

    public static string Fetching(string alias, int step) => $"{alias} を取得しています（{step}/2）";

    public static string Returning(string alias) => $"{alias} のライブ表示に戻しています";

    public static string FrameFailed(string alias) =>
        $"{alias} の画像を取得できませんでした。もう一度 [両方を更新] を押してください";

    public static string FrameFailedRebind(string alias) =>
        $"{alias} の画像を取得できませんでした。機体照合をやり直してください";

    public static string Refreshed(string timesText) => $"両方を更新しました（{timesText}）";

    public static string RefreshHelp(string selectedAlias) =>
        $"CAM-A、CAM-B の順に 1 枚ずつ取り直して、{selectedAlias} のライブ表示に戻ります。撮影できる残り時間を使います";

    public static string RefreshAutomationName(string selectedAlias) =>
        $"{RefreshButton} {RefreshHelp(selectedAlias)}";
}

/// <summary>Where the stage is in its sequence. Display and gating only; the Agent's state is the authority.</summary>
public enum DualLiveStageState
{
    /// <summary>No Ready binding.</summary>
    NoBinding,

    /// <summary>Ready, nothing is Live. Panes show stills or "no still".</summary>
    Idle,

    /// <summary>One camera is Live.</summary>
    LiveOne,

    /// <summary>Moving Live from one camera to the other.</summary>
    Switching,

    /// <summary>"Update both", fetching CAM-A.</summary>
    RefreshA,

    /// <summary>"Update both", fetching CAM-B.</summary>
    RefreshB,

    /// <summary>"Update both", putting the selected camera back to Live.</summary>
    Returning,

    /// <summary>A frame could not be obtained. The binding is still usable. Nothing is Live, and nothing reopens by itself.</summary>
    Failed,

    /// <summary>Everything is being closed and confirmed before a capture transaction.</summary>
    ClosingForCapture,

    /// <summary>The time left to admit a capture ran out.</summary>
    Expired,

    /// <summary>The binding was lost.</summary>
    Invalid,
}

public interface IDualLiveStageClock
{
    DateTimeOffset UtcNow { get; }

    /// <summary>Monotonic milliseconds. Differences are meaningful; the value itself is not.</summary>
    long MonotonicMilliseconds { get; }
}

public sealed class SystemDualLiveStageClock : IDualLiveStageClock
{
    public DateTimeOffset UtcNow => DateTimeOffset.UtcNow;

    public long MonotonicMilliseconds => Environment.TickCount64;
}

public sealed record DualLiveStageOptions
{
    public IDualLiveStageClock Clock { get; init; } = new SystemDualLiveStageClock();

    /// <summary>
    /// The Agent process's start time (UTC), or null when unknown. When null the stage cannot say how
    /// long a capture will still be admitted, and so it does not offer switching.
    /// </summary>
    public Func<DateTimeOffset?>? AgentStartTimeUtc { get; init; }

    /// <summary>Waits between frame requests and between polls for a first frame. Tests substitute a clock-advancing wait.</summary>
    public Func<TimeSpan, CancellationToken, Task>? Delay { get; init; }

    /// <summary>False leaves frame fetching to the caller (a test drives it one frame at a time).</summary>
    public bool RunFrameLoop { get; init; } = true;

    /// <summary>Turns frame bytes into an image, or null when they are not an image. Default: WPF imaging.</summary>
    public Func<byte[], ImageSource?>? FrameDecoder { get; init; }
}

/// <summary>
/// The stage of the dual Live view: CAM-A on the left, CAM-B on the right, one of them Live and the
/// other a still, and "update both" (Issue #291, ADR-0035).
/// </summary>
/// <remarks>
/// <para>
/// Why one object owns all of this: the Agent allows one open SDK source at a time, and every way the
/// screen changes (switch, update both, close for capture, stop on shutdown) is a sequence of
/// "stop and confirm, start, wait for a frame". Keeping the sequences next to the single state they
/// move is what keeps "never two open" and "never Live without a fresh frame" checkable in one file.
/// </para>
/// <para>
/// Rules the code below keeps:
/// </para>
/// <list type="bullet">
/// <item>A pane is Live only while a confirmed Live View has delivered a frame recently. Any failure,
/// switch, update or loss removes the Live state from both panes at once.</item>
/// <item>Nothing reopens by itself and nothing is retried. A failure stops the sequence, closes and
/// confirms, and waits for the operator. Waiting for a first frame is not a retry: it is one start
/// followed by polling for at most <see cref="FirstFrameTimeout"/>.</item>
/// <item>Before a capture transaction the stage closes and confirms, or reports that it could not.
/// When nothing was ever opened it sends nothing, so a capture that never used Live View sends the
/// Agent what it always did.</item>
/// <item>The time shown is the time left to <em>capture</em> (Agent lifetime minus the capture
/// reserve), not a binding expiry: the binding has no clock of its own.</item>
/// </list>
/// </remarks>
public sealed class DualLiveStageViewModel : ObservableObject
{
    public static readonly TimeSpan FrameInterval = TimeSpan.FromMilliseconds(100);
    public static readonly TimeSpan FirstFrameTimeout = TimeSpan.FromSeconds(10);
    public static readonly TimeSpan FrameStaleAfter = TimeSpan.FromSeconds(3);
    public static readonly TimeSpan RecentFrameForRefresh = TimeSpan.FromSeconds(3);

    /// <summary>The capture host admits a capture only with at least this much of its lifetime left (180 + 3 + 2).</summary>
    public static readonly TimeSpan CaptureAdmissionReserve = TimeSpan.FromSeconds(185);

    /// <summary>"Update both" is not offered with less capture time than this (※仮: to be replaced by the measured duration).</summary>
    public static readonly TimeSpan RefreshMinimumRemaining = TimeSpan.FromSeconds(30);

    public static readonly TimeSpan RemainingCautionThreshold = TimeSpan.FromSeconds(60);

    private readonly DualBoundLiveViewClient _client;
    private readonly DualBindingViewModel _binding;
    private readonly Func<bool> _isCaptureActive;
    private readonly IDualLiveStageClock _clock;
    private readonly Func<DateTimeOffset?> _agentStartTimeUtc;
    private readonly Func<TimeSpan, CancellationToken, Task> _delay;
    private readonly bool _runFrameLoop;
    private readonly Func<byte[], ImageSource?> _decode;
    private readonly SemaphoreSlim _gate = new(1, 1);

    private CancellationTokenSource? _loopCancellation;
    private Task? _loopTask;
    private DualLiveStageState _state = DualLiveStageState.NoBinding;
    private string _statusText = string.Empty;
    private string _alertText = string.Empty;
    private string _alertKind = "caution";
    private string _selectedAlias = DualBindingCameraAgentProtocol.CameraAliasA;
    // The camera whose pane is Live, or null. Moves together with the panes' states and the client.
    private string? _liveAlias;
    private bool _operationActive;
    private bool _closingForCapture;
    private bool _wasReady;
    private bool _expiredHandled;
    private long _lastFrameMonotonicMs;
    private bool _frameSinceLive;
    private long? _nativeRemainingMs;
    private long _nativeRemainingAtMs;
    private string? _currentTarget;

    public DualLiveStageViewModel(
        DualBoundLiveViewClient client,
        DualBindingViewModel binding,
        Func<bool> isCaptureActive,
        DualLiveStageOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(client);
        ArgumentNullException.ThrowIfNull(binding);
        ArgumentNullException.ThrowIfNull(isCaptureActive);
        options ??= new DualLiveStageOptions();
        _client = client;
        _binding = binding;
        _isCaptureActive = isCaptureActive;
        _clock = options.Clock;
        _agentStartTimeUtc = options.AgentStartTimeUtc ?? (() => null);
        _delay = options.Delay ?? ((span, token) => Task.Delay(span, token));
        _runFrameLoop = options.RunFrameLoop;
        _decode = options.FrameDecoder ?? new Func<byte[], ImageSource?>(DecodeWithWpf);

        PaneA = new DualLivePaneViewModel(DualBindingCameraAgentProtocol.CameraAliasA, () => _clock.UtcNow);
        PaneB = new DualLivePaneViewModel(DualBindingCameraAgentProtocol.CameraAliasB, () => _clock.UtcNow);
        Panes = [PaneA, PaneB];

        SwitchLiveCommand = new AsyncRelayCommand<string>(SwitchAsync, CanSwitchTo, OnUnexpectedFailure);
        RefreshBothCommand = new AsyncRelayCommand(RefreshBothAsync, () => RefreshDisabledReason.Length == 0, OnUnexpectedFailure);
        PaneA.BindSwitchCommand(SwitchLiveCommand);
        PaneB.BindSwitchCommand(SwitchLiveCommand);

        _wasReady = _binding.IsReady;
        _state = _wasReady ? DualLiveStageState.Idle : DualLiveStageState.NoBinding;
        _binding.PropertyChanged += OnBindingChanged;
    }

    public DualLivePaneViewModel PaneA { get; }

    public DualLivePaneViewModel PaneB { get; }

    /// <summary>Left to right: CAM-A, then CAM-B. The order never changes with the selection.</summary>
    public IReadOnlyList<DualLivePaneViewModel> Panes { get; }

    public AsyncRelayCommand<string> SwitchLiveCommand { get; }

    public AsyncRelayCommand RefreshBothCommand { get; }

    /// <summary>Raised with the completion text of an operator action, for the shell's notice line.</summary>
    public event Action<string>? NoticeRaised;

    /// <summary>
    /// Requests repeated without the operator asking. Zero by construction: nothing in this class
    /// retries a failed request or reopens a Live View. (Polling for a first frame is not a retry:
    /// it is one start followed by bounded frame requests.) Exposed so a test can pin it.
    /// </summary>
    public int AutomaticRetryCount => 0;

    public DualLiveStageState State
    {
        get => _state;
        private set
        {
            if (SetProperty(ref _state, value))
            {
                RefreshView();
            }
        }
    }

    public string SelectedAlias => _selectedAlias;

    /// <summary>The camera whose pane is Live, or null when nothing is.</summary>
    public string? LiveAlias => _liveAlias;

    public string PermanentNoteText => DualLiveStageText.PermanentNote;

    public string RefreshButtonText => DualLiveStageText.RefreshButton;

    public string RefreshButtonHelpText => RefreshDisabledReason.Length > 0
        ? RefreshDisabledReason
        : DualLiveStageText.RefreshHelp(_selectedAlias);

    public string RefreshButtonAutomationName => RefreshDisabledReason.Length > 0
        ? $"{DualLiveStageText.RefreshButton} {RefreshDisabledReason}"
        : DualLiveStageText.RefreshAutomationName(_selectedAlias);

    public string HeadingText =>
        $"二台の表示  {PaneA.Alias}: {PaneLabel(PaneA)} ／ {PaneB.Alias}: {PaneLabel(PaneB)}";

    /// <summary>Progress and results of what the operator asked for. A polite live region.</summary>
    public string StatusText
    {
        get => _statusText;
        private set => SetProperty(ref _statusText, value);
    }

    /// <summary>A condition the operator has to act on: loss, expiry, after-capture. An assertive live region.</summary>
    public string AlertText
    {
        get => AlertTextCore();
        private set
        {
            _alertText = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(IsAlertVisible));
            OnPropertyChanged(nameof(AlertKind));
        }
    }

    public bool IsAlertVisible => AlertText.Length > 0;

    /// <summary>block / caution, resolved to colours in XAML.</summary>
    public string AlertKind => _alertText.Length > 0 ? _alertKind : "caution";

    public string RefreshDisabledReason => DisabledReason(forRefresh: true, ownOperation: false);

    public bool IsRefreshDisabledReasonVisible => RefreshDisabledReason.Length > 0;

    /// <summary>Why a pane's switch button is unavailable right now, or empty.</summary>
    public string SwitchDisabledReason => DisabledReason(forRefresh: false, ownOperation: false);

    public bool IsOperationInProgress => _operationActive || _closingForCapture;

    // ---- Time left to capture ---------------------------------------------------------------

    /// <summary>
    /// Time left to admit a capture, or null when it cannot be told (the Agent's start time is
    /// unknown). The local figure counts down from the Agent's start time; a figure the Agent
    /// reported on its own clock, aged by the time since it arrived, can only make it shorter.
    /// </summary>
    public TimeSpan? CaptureTimeRemaining
    {
        get
        {
            if (_agentStartTimeUtc() is not { } startedAt)
            {
                return null;
            }

            var deadline = startedAt + (DualCameraAgentLifecycle.AgentMaxLifetime - CaptureAdmissionReserve);
            var remaining = deadline - _clock.UtcNow;
            if (_nativeRemainingMs is { } reported)
            {
                var aged = TimeSpan.FromMilliseconds(reported - (_clock.MonotonicMilliseconds - _nativeRemainingAtMs));
                if (aged < remaining)
                {
                    remaining = aged;
                }
            }

            return remaining < TimeSpan.Zero ? TimeSpan.Zero : remaining;
        }
    }

    public string RemainingText => CaptureTimeRemaining is { } remaining
        ? string.Create(
            CultureInfo.InvariantCulture,
            $"撮影できる残り {(int)remaining.TotalMinutes}:{remaining.Seconds:00}")
        : DualLiveStageText.RemainingUnknown;

    /// <summary>normal / caution / unknown / expired.</summary>
    public string RemainingKind => CaptureTimeRemaining switch
    {
        null => "unknown",
        { } remaining when remaining <= TimeSpan.Zero => "expired",
        { } remaining when remaining <= RemainingCautionThreshold => "caution",
        _ => "normal",
    };

    public string RemainingAutomationName => CaptureTimeRemaining is { } remaining
        ? string.Create(
            CultureInfo.InvariantCulture,
            $"撮影できる残り時間 {(int)remaining.TotalMinutes}分{remaining.Seconds}秒")
        : $"撮影できる残り時間 {DualLiveStageText.RemainingUnknown}";

    /// <summary>
    /// Called about twice a second by the window. Re-reads the clock for the texts that depend on it
    /// and closes the stage when the time to capture has run out. Never throws.
    /// </summary>
    public async Task TickAsync()
    {
        try
        {
            PaneA.RefreshTime();
            PaneB.RefreshTime();
            OnPropertyChanged(nameof(RemainingText));
            OnPropertyChanged(nameof(RemainingKind));
            OnPropertyChanged(nameof(RemainingAutomationName));
            OnPropertyChanged(nameof(AlertText));
            OnPropertyChanged(nameof(IsAlertVisible));
            RaiseCommandProperties();

            if (_binding.IsReady && !_binding.IsCaptureHostActivated && !_expiredHandled &&
                CaptureTimeRemaining is { } remaining && remaining <= TimeSpan.Zero)
            {
                await ExpireAsync().ConfigureAwait(true);
            }
        }
        catch (Exception exception) when (exception is not (OperationCanceledException or OutOfMemoryException))
        {
            // A display refresh must never take the window down. Whatever failed here shows up as
            // the next operation's refusal, which is checked against the Agent each time.
            _ = exception;
        }
    }

    // ---- Operator actions -------------------------------------------------------------------

    private bool CanSwitchTo(string alias) =>
        DualBindingCameraAgentProtocol.OrderedAliases.Contains(alias, StringComparer.Ordinal) &&
        !string.Equals(alias, _liveAlias, StringComparison.Ordinal) &&
        SwitchDisabledReason.Length == 0;

    internal async Task SwitchAsync(string alias)
    {
        // Set before the first await, so a second press cannot start a second sequence.
        _operationActive = true;
        RaiseCommandProperties();
        var gateHeld = false;
        try
        {
            await _gate.WaitAsync().ConfigureAwait(true);
            gateHeld = true;
            if (DisabledReason(forRefresh: false, ownOperation: true).Length > 0)
            {
                return;
            }

            await RunSequenceAsync(alias, () => SwitchCoreAsync(alias)).ConfigureAwait(true);
        }
        finally
        {
            if (gateHeld)
            {
                _gate.Release();
            }

            _operationActive = false;
            RefreshView();
        }
    }

    private async Task SwitchCoreAsync(string alias)
    {
        _currentTarget = alias;
        State = DualLiveStageState.Switching;
        StatusText = DualLiveStageText.Switching(alias);
        await StopFrameLoopAsync().ConfigureAwait(true);
        DemoteLiveToStill();
        PaneFor(alias).State = DualLivePaneState.Opening;
        RefreshView();

        var opened = await EnsureOpenAsync(alias).ConfigureAwait(true);
        if (opened == OpenOutcome.Failed)
        {
            return;
        }

        if (opened == OpenOutcome.Opened && !await WaitForFirstFrameAsync(alias).ConfigureAwait(true))
        {
            return;
        }

        BecomeLive(alias);
        StatusText = string.Empty;
    }

    internal async Task RefreshBothAsync()
    {
        _operationActive = true;
        RaiseCommandProperties();
        var gateHeld = false;
        try
        {
            await _gate.WaitAsync().ConfigureAwait(true);
            gateHeld = true;
            if (DisabledReason(forRefresh: true, ownOperation: true).Length > 0)
            {
                return;
            }

            await RunSequenceAsync(_selectedAlias, RefreshBothCoreAsync).ConfigureAwait(true);
        }
        finally
        {
            if (gateHeld)
            {
                _gate.Release();
            }

            _operationActive = false;
            RefreshView();
        }
    }

    private async Task RefreshBothCoreAsync()
    {
        var selected = _selectedAlias;
        await StopFrameLoopAsync().ConfigureAwait(true);

        // A frame from the Live camera that arrived within the last few seconds already is that
        // camera's fresh picture, so the sequence does not open it again to get another. This is what
        // keeps the number of SDK opens at two (not three) when the operator was watching Live.
        string? reused = null;
        if (_client.State == DualBoundLiveViewState.Live && _client.LiveAlias is { } live &&
            PaneFor(live).HasImage &&
            _clock.MonotonicMilliseconds - _lastFrameMonotonicMs <= RecentFrameForRefresh.TotalMilliseconds)
        {
            reused = live;
        }

        PaneA.State = DualLivePaneState.Updating;
        PaneB.State = DualLivePaneState.Updating;
        _liveAlias = null;
        RefreshView();

        var step = 0;
        foreach (var target in DualBindingCameraAgentProtocol.OrderedAliases)
        {
            step++;
            _currentTarget = target;
            State = step == 1 ? DualLiveStageState.RefreshA : DualLiveStageState.RefreshB;
            StatusText = DualLiveStageText.Fetching(target, step);
            if (string.Equals(target, reused, StringComparison.Ordinal))
            {
                continue;
            }

            if (await EnsureOpenAsync(target).ConfigureAwait(true) == OpenOutcome.Failed ||
                !await WaitForFirstFrameAsync(target).ConfigureAwait(true))
            {
                return;
            }

            // The last camera of the sequence is the selected one: it simply stays open.
            if (string.Equals(target, DualBindingCameraAgentProtocol.CameraAliasB, StringComparison.Ordinal) &&
                string.Equals(selected, target, StringComparison.Ordinal))
            {
                break;
            }
        }

        _currentTarget = selected;
        State = DualLiveStageState.Returning;
        StatusText = DualLiveStageText.Returning(selected);
        var returned = await EnsureOpenAsync(selected).ConfigureAwait(true);
        if (returned == OpenOutcome.Failed ||
            (returned == OpenOutcome.Opened && !await WaitForFirstFrameAsync(selected).ConfigureAwait(true)))
        {
            return;
        }

        var times = string.Join(
            "、",
            Panes.Select(pane => string.Create(
                CultureInfo.InvariantCulture,
                $"{pane.Alias} {(pane.LastFrameAtUtc ?? _clock.UtcNow).ToLocalTime():HH:mm:ss}")));
        BecomeLive(selected);
        StatusText = DualLiveStageText.Refreshed(times);
        NoticeRaised?.Invoke(StatusText);
    }

    /// <summary>
    /// Runs one operator-requested sequence under the gate. Whatever escapes the sequence is turned
    /// into the same handling as any other failure (close, confirm, stop), never left half done.
    /// </summary>
    private async Task RunSequenceAsync(string fallbackAlias, Func<Task> sequence)
    {
        _currentTarget = fallbackAlias;
        try
        {
            await sequence().ConfigureAwait(true);
        }
        catch (Exception exception) when (exception is not (OperationCanceledException or OutOfMemoryException))
        {
            await HandleFailureAsync(_currentTarget ?? fallbackAlias, null, exception).ConfigureAwait(true);
        }
    }

    // ---- Closing for a capture --------------------------------------------------------------

    /// <summary>
    /// Stops the frame loop, waits for a request in flight, and makes sure the Agent has nothing
    /// open. Returns true only when everything is confirmed closed (or nothing was ever opened, in
    /// which case nothing is sent). False means the capture must not start; the binding has been
    /// reported lost.
    /// </summary>
    public async Task<bool> PrepareForCaptureAsync(CancellationToken cancellationToken = default)
    {
        // Set before the first await: from here no new sequence can start.
        _closingForCapture = true;
        RaiseCommandProperties();
        var gateHeld = false;
        try
        {
            await _gate.WaitAsync(cancellationToken).ConfigureAwait(true);
            gateHeld = true;
            if (_client.State == DualBoundLiveViewState.ConfirmedClosed && _loopTask is null)
            {
                return true;
            }

            State = DualLiveStageState.ClosingForCapture;
            StatusText = DualLiveStageText.ClosingForCapture;
            await StopFrameLoopAsync().ConfigureAwait(true);
            var close = await _client.CloseAsync(cancellationToken).ConfigureAwait(true);
            if (close.Confirmed)
            {
                DemoteLiveToStill();
                _liveAlias = null;
                State = DualLiveStageState.Idle;
                StatusText = string.Empty;
                return true;
            }

            ReportBindingLost(null, UsableRefusal(close.Refusal));
            return false;
        }
        finally
        {
            if (gateHeld)
            {
                _gate.Release();
            }

            _closingForCapture = false;
            RefreshView();
        }
    }

    /// <summary>
    /// One stop request on the way out of the window. Bounded: it waits briefly for a sequence in
    /// flight and for a frame request in flight, sends one stop, and never throws. What it cannot
    /// confirm is left to cancel-binding, which follows.
    /// </summary>
    public async Task StopForShutdownAsync(CancellationToken cancellationToken = default)
    {
        var gateHeld = false;
        try
        {
            _closingForCapture = true;
            gateHeld = await _gate.WaitAsync(TimeSpan.FromSeconds(5), cancellationToken).ConfigureAwait(true);
            if (!gateHeld)
            {
                return;
            }

            await StopFrameLoopAsync(TimeSpan.FromSeconds(3)).ConfigureAwait(true);
            if (_client.MayBeOpen)
            {
                _ = await _client.CloseAsync(cancellationToken).ConfigureAwait(true);
            }

            _liveAlias = null;
            DemoteLiveToStill();
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _ = exception;
        }
        finally
        {
            if (gateHeld)
            {
                _gate.Release();
            }
        }
    }

    // ---- Opening, waiting, failing ----------------------------------------------------------

    private enum OpenOutcome
    {
        /// <summary>It was already open, as the camera asked for.</summary>
        AlreadyOpen,

        /// <summary>It was opened by this call; the first frame has not arrived.</summary>
        Opened,

        /// <summary>It could not be opened; the failure has been handled.</summary>
        Failed,
    }

    /// <summary>
    /// Makes the Agent have exactly this camera's Live View open: stops and confirms whatever else
    /// is open, then starts this one. Never starts while anything may still be open.
    /// </summary>
    private async Task<OpenOutcome> EnsureOpenAsync(string alias)
    {
        if (_client.State == DualBoundLiveViewState.Live &&
            string.Equals(_client.LiveAlias, alias, StringComparison.Ordinal))
        {
            return OpenOutcome.AlreadyOpen;
        }

        if (_client.MayBeOpen)
        {
            var close = await _client.CloseAsync().ConfigureAwait(true);
            if (!close.Confirmed)
            {
                // The stop itself is what failed: it is not sent a second time, and the new camera
                // is not started. The binding cannot be trusted to have nothing open.
                await HandleFailureAsync(alias, close.Refusal, close.Failure, closeAttempted: true)
                    .ConfigureAwait(true);
                return OpenOutcome.Failed;
            }
        }

        PaneFor(alias).State = DualLivePaneState.Opening;
        DualBindingReply<DualBoundLiveViewStartResult> started;
        try
        {
            started = await _client.StartAsync(alias).ConfigureAwait(true);
        }
        catch (Exception exception) when (exception is not (OperationCanceledException or OutOfMemoryException))
        {
            await HandleFailureAsync(alias, null, exception).ConfigureAwait(true);
            return OpenOutcome.Failed;
        }

        if (started.Value is { } result)
        {
            RecordAdmission(result.CaptureAdmissionRemainingMs);
            _lastFrameMonotonicMs = _clock.MonotonicMilliseconds;
            _frameSinceLive = false;
            return OpenOutcome.Opened;
        }

        await HandleFailureAsync(alias, started.Refusal, null).ConfigureAwait(true);
        return OpenOutcome.Failed;
    }

    /// <summary>
    /// Polls for this camera's first frame for at most <see cref="FirstFrameTimeout"/>. "No frame
    /// yet" is normal for a camera that has just started; running out of time is a failure.
    /// </summary>
    private async Task<bool> WaitForFirstFrameAsync(string alias)
    {
        var startedAt = _clock.MonotonicMilliseconds;
        while (true)
        {
            DualBindingReply<DualBoundLiveViewFrameResult> reply;
            try
            {
                reply = await _client.GetFrameAsync(alias).ConfigureAwait(true);
            }
            catch (Exception exception) when (exception is not (OperationCanceledException or OutOfMemoryException))
            {
                await HandleFailureAsync(alias, null, exception).ConfigureAwait(true);
                return false;
            }

            if (reply.Value is { } frame)
            {
                RecordAdmission(frame.CaptureAdmissionRemainingMs);
                if (_decode(frame.Frame) is { } image)
                {
                    PaneFor(alias).ApplyFrame(image, _clock.UtcNow);
                    _lastFrameMonotonicMs = _clock.MonotonicMilliseconds;
                    _frameSinceLive = true;
                    return true;
                }
            }
            else if (reply.Refusal is { } refusal && !IsNoFrameYet(refusal))
            {
                await HandleFailureAsync(alias, refusal, null).ConfigureAwait(true);
                return false;
            }

            if (_clock.MonotonicMilliseconds - startedAt >= FirstFrameTimeout.TotalMilliseconds)
            {
                // No frame in time. Not an Agent refusal, so the generic failure path applies.
                await HandleFailureAsync(alias, null, null).ConfigureAwait(true);
                return false;
            }

            await _delay(FrameInterval, CancellationToken.None).ConfigureAwait(true);
        }
    }

    private static bool IsNoFrameYet(DualBindingRefusal refusal) =>
        refusal.ResultCode is DualBoundLiveViewProtocol.RefusalCodes.FrameUnavailable or
            DualBoundLiveViewProtocol.RefusalCodes.FrameTooLarge;

    /// <summary>
    /// A frame, a start or a stop did not work. If the Agent says the binding is gone, report that.
    /// Otherwise close and confirm: confirmed means the binding is still usable and the operator may
    /// try again by hand; unconfirmed means the binding cannot be trusted and is reported lost.
    /// </summary>
    private async Task HandleFailureAsync(
        string alias,
        DualBindingRefusal? refusal,
        Exception? exception,
        bool closeAttempted = false)
    {
        _liveAlias = null;
        if (refusal is not null && DualBoundLiveViewClient.NeedsRebinding(refusal))
        {
            ReportBindingLost(alias, refusal);
            return;
        }

        if (closeAttempted)
        {
            ReportBindingLost(alias, UsableRefusal(null));
            return;
        }

        var close = await _client.CloseAsync().ConfigureAwait(true);
        if (close.Refusal is { } closeRefusal && DualBoundLiveViewClient.NeedsRebinding(closeRefusal))
        {
            ReportBindingLost(alias, closeRefusal);
            return;
        }

        if (!close.Confirmed)
        {
            ReportBindingLost(alias, UsableRefusal(null));
            return;
        }

        // Closed and confirmed: the binding is still usable, nothing is Live, and nothing will
        // reopen until the operator asks. The cause (an exception, an Agent refusal, running out of
        // time) is not shown to the operator; it is in the typed refusal / exception the caller had.
        _ = exception;
        SettlePanesAfterFailure(alias);
        State = DualLiveStageState.Failed;
        var message = refusal?.ResultCode == DualBoundLiveViewProtocol.RefusalCodes.HostLifetimeInsufficient
            ? DualLiveStageText.DisabledRemainingShort
            : DualLiveStageText.FrameFailed(alias);
        StatusText = message;
    }

    private void ReportBindingLost(string? failedAlias, DualBindingRefusal refusal)
    {
        _liveAlias = null;
        // The client learns of it first; the stage settles its own panes and state next; the binding
        // screen is told last. The binding screen's change notification then finds the stage already
        // Invalid and leaves the panes as they were settled here.
        _client.MarkBindingUnusable(refusal);

        // A change in the rig (USB, camera count, topology) or the Agent's own end says nothing about
        // one camera, so both panes stop. An SDK failure belongs to the camera it happened on.
        var rigOrAgentLoss = refusal.ResultCode is "BindingHostExpired" or "SessionMismatch" ||
            refusal.InvalidationReason is DualBindingInvalidationReason.UsbReconnect or
                DualBindingInvalidationReason.CameraCountChanged or
                DualBindingInvalidationReason.TopologyChanged or
                DualBindingInvalidationReason.AgentRestart or
                DualBindingInvalidationReason.SdkManagerRecreated;
        if (failedAlias is null || rigOrAgentLoss)
        {
            PaneA.State = DualLivePaneState.Closed;
            PaneB.State = DualLivePaneState.Closed;
        }
        else
        {
            SettlePanesAfterFailure(failedAlias);
        }

        DetachLoop();
        State = DualLiveStageState.Invalid;
        StatusText = string.Empty;
        SetAlert(BindingLostText(failedAlias, refusal), "block");
        _binding.ReportBoundLiveViewRefusal(refusal);
    }

    private static string BindingLostText(string? failedAlias, DualBindingRefusal refusal) =>
        refusal.ResultCode is "BindingHostExpired" or "SessionMismatch" ||
        refusal.InvalidationReason == DualBindingInvalidationReason.AgentRestart
            ? DualLiveStageText.BindingLostAgent
            : refusal.InvalidationReason switch
            {
                DualBindingInvalidationReason.UsbReconnect => DualLiveStageText.BindingLostUsb,
                DualBindingInvalidationReason.CameraCountChanged => DualLiveStageText.BindingLostCameraCount,
                DualBindingInvalidationReason.TopologyChanged => DualLiveStageText.BindingLostTopology,
                _ => failedAlias is null
                    ? DualLiveStageText.BindingLostUnconfirmed
                    : DualLiveStageText.FrameFailedRebind(failedAlias),
            };

    private static DualBindingRefusal UsableRefusal(DualBindingRefusal? refusal) =>
        refusal is not null && DualBoundLiveViewClient.NeedsRebinding(refusal)
            ? refusal
            : new DualBindingRefusal
            {
                ResultCode = "BindingCleanupFailed",
                State = DualBindingSessionState.Invalid,
                InvalidationReason = DualBindingInvalidationReason.SdkError,
                Detail = "The bound Live View could not be confirmed closed.",
            };

    // ---- Expiry -----------------------------------------------------------------------------

    private async Task ExpireAsync()
    {
        // Never waits behind a sequence: the next tick asks again.
        if (!await _gate.WaitAsync(0).ConfigureAwait(true))
        {
            return;
        }

        try
        {
            if (_expiredHandled || _operationActive || _closingForCapture)
            {
                return;
            }

            _expiredHandled = true;
            await StopFrameLoopAsync().ConfigureAwait(true);
            if (_client.MayBeOpen)
            {
                var close = await _client.CloseAsync().ConfigureAwait(true);
                if (!close.Confirmed)
                {
                    ReportBindingLost(null, UsableRefusal(close.Refusal));
                    return;
                }
            }

            _liveAlias = null;
            DemoteLiveToStill();
            State = DualLiveStageState.Expired;
            StatusText = string.Empty;
            SetAlert(DualLiveStageText.RemainingExpired, "block");
        }
        finally
        {
            _gate.Release();
            RefreshView();
        }
    }

    // ---- Frame loop -------------------------------------------------------------------------

    private void BecomeLive(string alias)
    {
        _liveAlias = alias;
        _selectedAlias = alias;
        foreach (var pane in Panes)
        {
            pane.State = string.Equals(pane.Alias, alias, StringComparison.Ordinal)
                ? DualLivePaneState.Live
                : pane.HasImage ? DualLivePaneState.Still : DualLivePaneState.NotAcquired;
        }

        _alertText = string.Empty;
        State = DualLiveStageState.LiveOne;
        OnPropertyChanged(nameof(SelectedAlias));
        StartFrameLoop(alias);
    }

    private void StartFrameLoop(string alias)
    {
        if (!_runFrameLoop)
        {
            return;
        }

        var cancellation = new CancellationTokenSource();
        _loopCancellation = cancellation;
        _loopTask = RunFrameLoopAsync(alias, cancellation.Token);
    }

    private async Task RunFrameLoopAsync(string alias, CancellationToken token)
    {
        try
        {
            while (!token.IsCancellationRequested)
            {
                await _delay(FrameInterval, token).ConfigureAwait(true);
                if (token.IsCancellationRequested || !await PumpFrameAsync(alias, token).ConfigureAwait(true))
                {
                    return;
                }
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested)
        {
            // An operation took over, or the stage is closing. Nothing to report.
        }
        catch (Exception exception) when (exception is not (OperationCanceledException or OutOfMemoryException))
        {
            try
            {
                await FailLiveFromLoopAsync(alias, null, exception, token).ConfigureAwait(true);
            }
            catch (OperationCanceledException)
            {
                // Same as above.
            }
        }
    }

    /// <summary>
    /// One frame request for the Live camera. Returns false when the loop must end. Exposed to the
    /// tests so a frame can be fetched deterministically with <c>RunFrameLoop = false</c>.
    /// </summary>
    internal async Task<bool> PumpFrameAsync(string alias, CancellationToken token)
    {
        DualBindingReply<DualBoundLiveViewFrameResult> reply;
        try
        {
            // Not cancelled by the token: a request that already went out is waited for, so the
            // next request is never queued behind an answer nobody read.
            reply = await _client.GetFrameAsync(alias).ConfigureAwait(true);
        }
        catch (Exception exception) when (exception is not (OperationCanceledException or OutOfMemoryException))
        {
            if (token.IsCancellationRequested)
            {
                return false;
            }

            await FailLiveFromLoopAsync(alias, null, exception, token).ConfigureAwait(true);
            return false;
        }

        if (token.IsCancellationRequested || !string.Equals(_liveAlias, alias, StringComparison.Ordinal))
        {
            return false;
        }

        if (reply.Value is { } frame)
        {
            RecordAdmission(frame.CaptureAdmissionRemainingMs);
            if (_decode(frame.Frame) is { } image)
            {
                PaneFor(alias).ApplyFrame(image, _clock.UtcNow);
                _lastFrameMonotonicMs = _clock.MonotonicMilliseconds;
                _frameSinceLive = true;
                return true;
            }
        }
        else if (reply.Refusal is { } refusal && !IsNoFrameYet(refusal))
        {
            await FailLiveFromLoopAsync(alias, refusal, null, token).ConfigureAwait(true);
            return false;
        }

        // No usable frame this time. A Live view that has not delivered one in too long is not Live.
        var limit = _frameSinceLive ? FrameStaleAfter : FirstFrameTimeout;
        if (_clock.MonotonicMilliseconds - _lastFrameMonotonicMs > limit.TotalMilliseconds)
        {
            await FailLiveFromLoopAsync(alias, null, null, token).ConfigureAwait(true);
            return false;
        }

        return true;
    }

    private async Task FailLiveFromLoopAsync(
        string alias,
        DualBindingRefusal? refusal,
        Exception? exception,
        CancellationToken token)
    {
        // An operation that holds the gate cancels this loop first, so waiting here ends with it.
        await _gate.WaitAsync(token).ConfigureAwait(true);
        try
        {
            if (token.IsCancellationRequested || !string.Equals(_liveAlias, alias, StringComparison.Ordinal))
            {
                return;
            }

            DetachLoop();
            await HandleFailureAsync(alias, refusal, exception).ConfigureAwait(true);
        }
        finally
        {
            _gate.Release();
            RefreshView();
        }
    }

    /// <summary>Forgets the loop without waiting for it: the caller is the loop, or the loop is finished.</summary>
    private void DetachLoop()
    {
        _loopCancellation = null;
        _loopTask = null;
    }

    private async Task StopFrameLoopAsync(TimeSpan? maxWait = null)
    {
        var cancellation = _loopCancellation;
        var loop = _loopTask;
        _loopCancellation = null;
        _loopTask = null;
        if (cancellation is null)
        {
            return;
        }

        cancellation.Cancel();
        if (loop is not null)
        {
            try
            {
                await (maxWait is { } limit ? loop.WaitAsync(limit) : loop).ConfigureAwait(true);
            }
            catch (Exception exception) when (exception is OperationCanceledException or TimeoutException)
            {
                // Cancelled, or still waiting for a request in flight: later requests queue behind
                // it inside the transport, so the order on the wire is unchanged.
                _ = exception;
            }
        }

        cancellation.Dispose();
    }

    // ---- Binding changes --------------------------------------------------------------------

    private void OnBindingChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs args)
    {
        if (args.PropertyName is nameof(DualBindingViewModel.IsReady) or nameof(DualBindingViewModel.Phase))
        {
            var ready = _binding.IsReady;
            if (ready && !_wasReady)
            {
                _wasReady = true;
                ResetForNewBinding();
            }
            else if (!ready && _wasReady)
            {
                _wasReady = false;
                DetachLoopAndCancel();
                _liveAlias = null;
                if (_state != DualLiveStageState.Invalid)
                {
                    // The binding ended somewhere else (the host's lifetime, a re-binding). Nothing is
                    // Live without a binding, whatever the panes showed a moment ago.
                    foreach (var pane in Panes)
                    {
                        if (pane.State is DualLivePaneState.Live or DualLivePaneState.Opening or DualLivePaneState.Updating)
                        {
                            pane.State = DualLivePaneState.Closed;
                        }
                    }

                    State = DualLiveStageState.NoBinding;
                }
            }
        }

        if (args.PropertyName is nameof(DualBindingViewModel.IsCaptureHostActivated) or
            nameof(DualBindingViewModel.IsReady) or nameof(DualBindingViewModel.IsBusy) or
            nameof(DualBindingViewModel.IsShutdownBlocked))
        {
            RefreshView();
        }
    }

    private void DetachLoopAndCancel()
    {
        _loopCancellation?.Cancel();
        DetachLoop();
    }

    private void ResetForNewBinding()
    {
        PaneA.Clear();
        PaneB.Clear();
        _selectedAlias = DualBindingCameraAgentProtocol.CameraAliasA;
        _liveAlias = null;
        _expiredHandled = false;
        _nativeRemainingMs = null;
        _alertText = string.Empty;
        StatusText = string.Empty;
        State = DualLiveStageState.Idle;
        OnPropertyChanged(nameof(SelectedAlias));
        RefreshView();
    }

    // ---- Panes and view ---------------------------------------------------------------------

    private DualLivePaneViewModel PaneFor(string alias) =>
        string.Equals(alias, PaneA.Alias, StringComparison.Ordinal) ? PaneA : PaneB;

    /// <summary>The picture of a camera that stopped being Live stays on screen, as a still.</summary>
    private void DemoteLiveToStill()
    {
        foreach (var pane in Panes)
        {
            if (pane.State is DualLivePaneState.Live or DualLivePaneState.Opening or DualLivePaneState.Updating)
            {
                pane.State = pane.HasImage ? DualLivePaneState.Still : DualLivePaneState.NotAcquired;
            }
        }
    }

    private void SettlePanesAfterFailure(string failedAlias)
    {
        foreach (var pane in Panes)
        {
            if (string.Equals(pane.Alias, failedAlias, StringComparison.Ordinal))
            {
                pane.State = DualLivePaneState.Failed;
            }
            else if (pane.State is DualLivePaneState.Live or DualLivePaneState.Opening or DualLivePaneState.Updating)
            {
                pane.State = pane.HasImage ? DualLivePaneState.Still : DualLivePaneState.NotAcquired;
            }
        }
    }

    private void RecordAdmission(long? remainingMs)
    {
        if (remainingMs is { } milliseconds)
        {
            _nativeRemainingMs = milliseconds;
            _nativeRemainingAtMs = _clock.MonotonicMilliseconds;
        }
    }

    private void SetAlert(string text, string kind)
    {
        _alertKind = kind;
        AlertText = text;
    }

    private string AlertTextCore()
    {
        if (_alertText.Length > 0)
        {
            return _alertText;
        }

        if (_binding.IsCaptureHostActivated)
        {
            return DualLiveStageText.AfterCapture;
        }

        if (_binding.IsReady && CaptureTimeRemaining is { } remaining &&
            remaining > TimeSpan.Zero && remaining <= RemainingCautionThreshold)
        {
            return DualLiveStageText.RemainingCautionSoon;
        }

        return string.Empty;
    }

    private string DisabledReason(bool forRefresh, bool ownOperation)
    {
        if (_binding.IsCaptureHostActivated)
        {
            return DualLiveStageText.AfterCapture;
        }

        if (!_binding.IsReady || _state is DualLiveStageState.NoBinding or DualLiveStageState.Invalid)
        {
            return DualLiveStageText.DisabledNotBound;
        }

        if (_state == DualLiveStageState.Expired)
        {
            return DualLiveStageText.DisabledExpired;
        }

        if (_isCaptureActive())
        {
            return DualLiveStageText.DisabledCapturing;
        }

        if (_closingForCapture || (!ownOperation && _operationActive))
        {
            return DualLiveStageText.DisabledSwitching;
        }

        if (CaptureTimeRemaining is not { } remaining)
        {
            return DualLiveStageText.DisabledRemainingUnknown;
        }

        if (remaining <= TimeSpan.Zero)
        {
            return DualLiveStageText.DisabledExpired;
        }

        if (forRefresh && remaining < RefreshMinimumRemaining)
        {
            return DualLiveStageText.DisabledRemainingShort;
        }

        return string.Empty;
    }

    private static string PaneLabel(DualLivePaneViewModel pane) => pane.State switch
    {
        DualLivePaneState.Live => DualLiveStageText.LiveBadge,
        DualLivePaneState.Still => DualLiveStageText.StillBadgePrefix,
        DualLivePaneState.Opening => DualLiveStageText.OpeningBadge,
        DualLivePaneState.Updating => DualLiveStageText.UpdatingBadge,
        DualLivePaneState.Failed or DualLivePaneState.Closed => DualLiveStageText.StoppedBadge,
        _ => DualLiveStageText.NotAcquiredBadge,
    };

    private void RefreshView()
    {
        OnPropertyChanged(nameof(HeadingText));
        OnPropertyChanged(nameof(AlertText));
        OnPropertyChanged(nameof(IsAlertVisible));
        OnPropertyChanged(nameof(AlertKind));
        OnPropertyChanged(nameof(RemainingText));
        OnPropertyChanged(nameof(RemainingKind));
        OnPropertyChanged(nameof(RemainingAutomationName));
        OnPropertyChanged(nameof(IsOperationInProgress));
        RaiseCommandProperties();
    }

    private void RaiseCommandProperties()
    {
        OnPropertyChanged(nameof(RefreshDisabledReason));
        OnPropertyChanged(nameof(IsRefreshDisabledReasonVisible));
        OnPropertyChanged(nameof(SwitchDisabledReason));
        OnPropertyChanged(nameof(RefreshButtonHelpText));
        OnPropertyChanged(nameof(RefreshButtonAutomationName));
        SwitchLiveCommand.NotifyCanExecuteChanged();
        RefreshBothCommand.NotifyCanExecuteChanged();
    }

    private void OnUnexpectedFailure(Exception exception)
    {
        // The sequences handle their own failures; this is only for something outside them. It is
        // shown as a failure of the stage rather than swallowed.
        _ = exception;
        StatusText = DualLiveStageText.Unexpected;
    }

    private static ImageSource? DecodeWithWpf(byte[] bytes)
    {
        try
        {
            var image = new BitmapImage();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.StreamSource = new MemoryStream(bytes, writable: false);
            image.EndInit();
            image.Freeze();
            return image;
        }
        catch (Exception exception) when (
            exception is NotSupportedException or ArgumentException or IOException or FormatException or
                InvalidOperationException)
        {
            // Not an image. The frame counts as "no frame", so the staleness rule decides how long
            // that can last before the camera is no longer shown as Live.
            return null;
        }
    }
}
