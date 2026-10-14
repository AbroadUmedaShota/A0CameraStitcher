using System.Globalization;
using System.Windows.Input;
using System.Windows.Media;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

/// <summary>What one camera's pane on the dual Live stage is showing (Issue #291).</summary>
public enum DualLivePaneState
{
    /// <summary>No frame has been received from this camera in this binding.</summary>
    NotAcquired,

    /// <summary>The camera's Live View is being opened. The previous still, if any, stays.</summary>
    Opening,

    /// <summary>The camera's Live View is open and frames are arriving. The only state that is shown as Live.</summary>
    Live,

    /// <summary>The last frame received, shown as a still with the time it was received.</summary>
    Still,

    /// <summary>Part of "update both": the previous still stays until this camera's new frame arrives.</summary>
    Updating,

    /// <summary>This camera's frame could not be obtained. The binding may still be usable.</summary>
    Failed,

    /// <summary>The stage is closed (binding lost, shutting down). Nothing here is Live.</summary>
    Closed,
}

/// <summary>
/// One pane of the dual Live stage: a camera's alias, what it shows, and the words that describe it.
/// </summary>
/// <remarks>
/// A pane is Live only in <see cref="DualLivePaneState.Live"/>. Every other state, including one that
/// still has an image, is worded and framed as "not live", so a frozen image can never be read as a
/// running one: the badge text, the frame style key and the accessible name all change together
/// because they are derived from <see cref="State"/> in one place. The image is a preview only: it is
/// held in memory, never written, and never used as an input to capture or composition.
/// </remarks>
public sealed class DualLivePaneViewModel : ObservableObject
{
    private readonly Func<DateTimeOffset> _utcNow;
    private DualLivePaneState _state = DualLivePaneState.NotAcquired;
    private ImageSource? _image;
    private DateTimeOffset? _lastFrameAtUtc;
    // The 10-second bucket the accessible name last announced, so a name that reads "38 seconds ago"
    // is not re-announced as a change every second.
    private long _announcedElapsedBucket = -1;

    public DualLivePaneViewModel(string alias, Func<DateTimeOffset> utcNow)
    {
        ArgumentException.ThrowIfNullOrEmpty(alias);
        ArgumentNullException.ThrowIfNull(utcNow);
        Alias = alias;
        _utcNow = utcNow;
    }

    public string Alias { get; }

    /// <summary>The stage's switch command. Set once by the stage; null only before it is built.</summary>
    public ICommand? SwitchLiveCommand { get; private set; }

    internal void BindSwitchCommand(ICommand command) => SwitchLiveCommand = command;

    public DualLivePaneState State
    {
        get => _state;
        internal set
        {
            if (SetProperty(ref _state, value))
            {
                RaiseStateProperties();
            }
        }
    }

    public ImageSource? Image
    {
        get => _image;
        private set
        {
            if (SetProperty(ref _image, value))
            {
                OnPropertyChanged(nameof(HasImage));
                OnPropertyChanged(nameof(IsImageVisible));
                OnPropertyChanged(nameof(IsPlaceholderVisible));
                OnPropertyChanged(nameof(StoppedImageNoteText));
                OnPropertyChanged(nameof(IsStoppedImageNoteVisible));
            }
        }
    }

    public bool HasImage => _image is not null;

    /// <summary>When the frame on screen was received (UTC). Null when no frame has been received.</summary>
    public DateTimeOffset? LastFrameAtUtc => _lastFrameAtUtc;

    public bool IsLive => _state == DualLivePaneState.Live;

    /// <summary>Resolved to brushes and a stroke style in XAML: live, failed, or pending (dashed).</summary>
    public string StateKey => _state switch
    {
        DualLivePaneState.Live => "live",
        DualLivePaneState.Failed => "failed",
        _ => "pending",
    };

    public string BadgeText => _state switch
    {
        DualLivePaneState.Live => DualLiveStageText.LiveBadge,
        DualLivePaneState.Still when _lastFrameAtUtc is { } at =>
            $"{DualLiveStageText.StillBadgePrefix} {Local(at).ToString("HH:mm:ss", CultureInfo.InvariantCulture)}・{ElapsedText(Elapsed(at))}",
        DualLivePaneState.Opening => DualLiveStageText.OpeningBadge,
        DualLivePaneState.Updating => DualLiveStageText.UpdatingBadge,
        DualLivePaneState.Failed or DualLivePaneState.Closed => DualLiveStageText.StoppedBadge,
        _ => DualLiveStageText.NotAcquiredBadge,
    };

    /// <summary>Live: the time the last frame was received. Otherwise empty (a switch button shows instead).</summary>
    public string FooterText => _state == DualLivePaneState.Live && _lastFrameAtUtc is { } at
        ? $"{DualLiveStageText.LastReceivedPrefix} {Local(at).ToString("HH:mm:ss", CultureInfo.InvariantCulture)}"
        : string.Empty;

    public bool IsFooterTextVisible => FooterText.Length > 0;

    /// <summary>The switch button is the way into Live, so it is offered everywhere except on the Live pane.</summary>
    public bool IsSwitchButtonVisible => _state != DualLivePaneState.Live;

    public string SwitchButtonText => DualLiveStageText.SwitchButton;

    /// <summary>Names the camera, so two buttons on the screen never share one spoken name.</summary>
    public string SwitchButtonAutomationName => $"{Alias} をライブ表示に切り替える";

    public bool IsImageVisible => _image is not null;

    public bool IsPlaceholderVisible => _image is null;

    public string PlaceholderText => _state switch
    {
        DualLivePaneState.NotAcquired => DualLiveStageText.NotAcquiredPlaceholder,
        DualLivePaneState.Opening or DualLivePaneState.Updating => DualLiveStageText.WaitingPlaceholder,
        DualLivePaneState.Failed or DualLivePaneState.Closed => DualLiveStageText.StoppedPlaceholder,
        _ => string.Empty,
    };

    /// <summary>Failed with a picture on screen: that picture is from before the stop.</summary>
    public bool IsStoppedImageNoteVisible => _state == DualLivePaneState.Failed && _image is not null;

    public string StoppedImageNoteText => IsStoppedImageNoteVisible ? DualLiveStageText.StoppedImageNote : string.Empty;

    /// <summary>
    /// What a screen reader says for this pane. Follows the state, so colour and frame style are
    /// never the only carriers of "live or not".
    /// </summary>
    public string AccessibleName => _state switch
    {
        DualLivePaneState.Live when _lastFrameAtUtc is { } at =>
            $"{Alias} ライブ表示中 最終受信 {Local(at).ToString("HH'時'mm'分'ss'秒'", CultureInfo.InvariantCulture)}",
        DualLivePaneState.Live => $"{Alias} ライブ表示中",
        DualLivePaneState.Still when _lastFrameAtUtc is { } at =>
            $"{Alias} 静止画 {Local(at).ToString("HH'時'mm'分'ss'秒'", CultureInfo.InvariantCulture)}に取得 " +
            ElapsedText(RoundedElapsed(at)),
        DualLivePaneState.Opening => $"{Alias} 準備中",
        DualLivePaneState.Updating => $"{Alias} 更新中",
        DualLivePaneState.Failed => $"{Alias} 映像停止 取得に失敗",
        DualLivePaneState.Closed => $"{Alias} 映像停止",
        _ => $"{Alias} 静止画なし",
    };

    /// <summary>Stores a received frame. Does not change <see cref="State"/>; the stage decides that.</summary>
    internal void ApplyFrame(ImageSource image, DateTimeOffset receivedAtUtc)
    {
        ArgumentNullException.ThrowIfNull(image);
        _lastFrameAtUtc = receivedAtUtc;
        Image = image;
        RaiseTimeProperties();
    }

    /// <summary>Drops the picture and the time: nothing from a binding that ended may be reused.</summary>
    internal void Clear()
    {
        _lastFrameAtUtc = null;
        Image = null;
        State = DualLivePaneState.NotAcquired;
        RaiseTimeProperties();
    }

    /// <summary>
    /// Re-reads the clock for the texts that depend on it. The accessible name changes only when its
    /// 10-second bucket does.
    /// </summary>
    internal void RefreshTime()
    {
        OnPropertyChanged(nameof(BadgeText));
        var bucket = _lastFrameAtUtc is { } at ? RoundedElapsed(at).Ticks : -1;
        if (bucket != _announcedElapsedBucket)
        {
            _announcedElapsedBucket = bucket;
            OnPropertyChanged(nameof(AccessibleName));
        }
    }

    internal static string ElapsedText(TimeSpan elapsed)
    {
        var seconds = Math.Max(0, (long)elapsed.TotalSeconds);
        return seconds < 60
            ? string.Create(CultureInfo.InvariantCulture, $"{seconds}秒前")
            : string.Create(CultureInfo.InvariantCulture, $"{seconds / 60}分前");
    }

    private TimeSpan Elapsed(DateTimeOffset at)
    {
        var elapsed = _utcNow() - at;
        return elapsed < TimeSpan.Zero ? TimeSpan.Zero : elapsed;
    }

    // Seconds rounded down to a multiple of 10 (under a minute). Beyond that the text is in whole
    // minutes already, so the value is returned as it is.
    private TimeSpan RoundedElapsed(DateTimeOffset at)
    {
        var elapsed = Elapsed(at);
        if (elapsed.TotalSeconds >= 60)
        {
            return elapsed;
        }

        var seconds = (long)elapsed.TotalSeconds;
        return TimeSpan.FromSeconds(seconds - (seconds % 10));
    }

    private static DateTimeOffset Local(DateTimeOffset utc) => utc.ToLocalTime();

    private void RaiseStateProperties()
    {
        OnPropertyChanged(nameof(IsLive));
        OnPropertyChanged(nameof(StateKey));
        OnPropertyChanged(nameof(BadgeText));
        OnPropertyChanged(nameof(FooterText));
        OnPropertyChanged(nameof(IsFooterTextVisible));
        OnPropertyChanged(nameof(IsSwitchButtonVisible));
        OnPropertyChanged(nameof(PlaceholderText));
        OnPropertyChanged(nameof(IsStoppedImageNoteVisible));
        OnPropertyChanged(nameof(StoppedImageNoteText));
        OnPropertyChanged(nameof(AccessibleName));
    }

    private void RaiseTimeProperties()
    {
        OnPropertyChanged(nameof(LastFrameAtUtc));
        OnPropertyChanged(nameof(BadgeText));
        OnPropertyChanged(nameof(FooterText));
        OnPropertyChanged(nameof(IsFooterTextVisible));
        OnPropertyChanged(nameof(AccessibleName));
    }
}
