using System.Runtime.CompilerServices;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

// GitHub Issue #249 (WCAG 2.5.3 Label in Name): a control whose label follows the view model has an accessible name
// built from that label. If the label is announced as changed but the name is not, the screen reader and voice
// control keep the old name next to the new label. The recorder listens to one view model's PropertyChanged and,
// at each AssertPairsNotified call, checks that every property that was announced since the previous call brings
// the properties that are derived from it (Dependents) with it.
//
// Listening starts at Attach, or at the first AssertPairsNotified call for a view model, which then checks nothing.
// A scenario that wants the first state checked calls Attach before it changes anything.
internal sealed class AccessibleNameNotificationRecorder
{
    private static readonly ConditionalWeakTable<OperatorShellViewModel, AccessibleNameNotificationRecorder> Recorders = new();

    /// <summary>Label (or gate) property -> the properties that are derived from it and must be announced with it.</summary>
    internal static readonly (string Control, string Trigger, string[] Dependents)[] Pairs =
    {
        ("main capture button", nameof(OperatorShellViewModel.CaptureButtonText), new[] { nameof(OperatorShellViewModel.CaptureButtonAutomationName) }),
        ("live view button", nameof(OperatorShellViewModel.LiveViewButtonText), new[] { nameof(OperatorShellViewModel.LiveViewButtonAutomationName) }),
        ("view reset button", nameof(OperatorShellViewModel.ResetButtonText), new[] { nameof(OperatorShellViewModel.ResetButtonAutomationName) }),
        ("safety acknowledgement button", nameof(OperatorShellViewModel.SafetyAckText), new[] { nameof(OperatorShellViewModel.SafetyAckAutomationName) }),
        ("identity status menu item", nameof(OperatorShellViewModel.DualCameraIdentityStatusText), new[] { nameof(OperatorShellViewModel.DualCameraIdentityStatusAutomationName) }),
        ("grid settings toggle", nameof(OperatorShellViewModel.GridDivisionText), new[] { nameof(OperatorShellViewModel.GridSettingsToggleAutomationName) }),
        ("switch live camera button", nameof(OperatorShellViewModel.SwitchLiveCameraButtonText), new[] { nameof(OperatorShellViewModel.SwitchLiveCameraButtonAutomationName) }),
        ("peaking button", nameof(OperatorShellViewModel.PeakingButtonText), new[] { nameof(OperatorShellViewModel.PeakingButtonAutomationName) }),
        ("prepare new capture button", nameof(OperatorShellViewModel.PrepareNewCaptureText), new[] { nameof(OperatorShellViewModel.PrepareNewCaptureAutomationName) }),
        ("review primary button", nameof(OperatorShellViewModel.ReviewPrimaryActionText), new[] { nameof(OperatorShellViewModel.ReviewPrimaryActionAutomationName) }),
        ("capture-recovery-only approval checkbox", nameof(OperatorShellViewModel.CaptureRecoveryOnlyConfirmationText), new[] { nameof(OperatorShellViewModel.CaptureRecoveryOnlyConfirmationAutomationName) }),
        ("binding menu item", nameof(OperatorShellViewModel.BindingMenuHeader), new[] { nameof(OperatorShellViewModel.BindingMenuAutomationName) }),
        // The 撮影 + AF button and its reason row are gated by CanCapture as well as by the focus panel.
        ("撮影 + AF button and its reason row", nameof(OperatorShellViewModel.CanCapture), new[]
        {
            nameof(OperatorShellViewModel.CanCaptureWithAutoFocus),
            nameof(OperatorShellViewModel.IsCaptureWithAutoFocusUnavailableReasonVisible),
            nameof(OperatorShellViewModel.CaptureWithAutoFocusUnavailableReason),
        }),
    };

    private readonly object _gate = new();
    private readonly HashSet<string> _notified = new(StringComparer.Ordinal);

    private AccessibleNameNotificationRecorder(OperatorShellViewModel shell)
    {
        shell.PropertyChanged += (_, args) =>
        {
            lock (_gate)
            {
                _notified.Add(args.PropertyName ?? string.Empty);
            }
        };
    }

    /// <summary>Starts recording the view model's notifications (once; a second call changes nothing).</summary>
    internal static void Attach(OperatorShellViewModel shell) =>
        Recorders.GetValue(shell, static key => new AccessibleNameNotificationRecorder(key));

    /// <summary>Checks the notifications recorded since the previous call, then starts a new round.</summary>
    internal static void AssertPairsNotified(OperatorShellViewModel shell, string state)
    {
        if (!Recorders.TryGetValue(shell, out var recorder))
        {
            Attach(shell);
            return;
        }

        string[] notified;
        lock (recorder._gate)
        {
            notified = recorder._notified.ToArray();
            recorder._notified.Clear();
        }

        var announced = new HashSet<string>(notified, StringComparer.Ordinal);
        foreach (var (control, trigger, dependents) in Pairs)
        {
            if (!announced.Contains(trigger))
            {
                continue;
            }

            foreach (var dependent in dependents)
            {
                Check.True(announced.Contains(dependent),
                    $"GitHub Issue #249 ({state}): the {control} announced {trigger} as changed without announcing {dependent}.");
            }
        }
    }
}
