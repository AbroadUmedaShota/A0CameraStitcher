using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

/// <summary>
/// The window-side steps of <see cref="HardwareDualWindowCloseSequence"/> that exist only when
/// the window owns a Camera Agent (HardwareDual). Each member is one action the window already
/// performed in its close handler; they are delegates so that the window and the tests run the
/// same sequence code (issue #239).
/// </summary>
/// <param name="BeginConfirmation">Shows the "confirming" indicator (issue #228). Display only.</param>
/// <param name="CancelBindingAsync">The typed binding cleanup; a refusal blocks the close.</param>
/// <param name="DisposeAgentAsync">Waits for the Agent's natural exit. Never kills it.</param>
/// <param name="ReleaseExclusiveLease">Called only after both steps above confirmed.</param>
/// <param name="EstimateRemainingAgentLifetime">Reads the Agent's own start time for the blocked guidance.</param>
/// <param name="ReportBlocked">Shows the blocked guidance (code, detail, remaining-time estimate).</param>
/// <param name="EndConfirmation">Hides the "confirming" indicator once the result is on screen.</param>
/// <param name="EnableWindow">Makes the window usable again after a blocked result.</param>
/// <param name="RestartHostLifetimeMonitor">
/// Puts the host-lifetime monitor back as it was before the close attempt (issue #239).
/// </param>
public sealed record HardwareDualWindowAgentCloseSteps(
    Action BeginConfirmation,
    Func<Task<DualBindingRefusal?>> CancelBindingAsync,
    Func<ValueTask> DisposeAgentAsync,
    Action ReleaseExclusiveLease,
    Func<TimeSpan?> EstimateRemainingAgentLifetime,
    Action<string, string, TimeSpan?> ReportBlocked,
    Action EndConfirmation,
    Action EnableWindow,
    Action RestartHostLifetimeMonitor);

/// <param name="StopHostLifetimeMonitor">Stops the local host-lifetime observation.</param>
/// <param name="DisableWindow">Disables the window while the close is awaited.</param>
/// <param name="CancelWindowLifetime">Cancels the window's own lifetime token.</param>
/// <param name="Agent">Null for a window that owns no Camera Agent; then nothing is awaited.</param>
public sealed record HardwareDualWindowCloseSteps(
    Action StopHostLifetimeMonitor,
    Action DisableWindow,
    Action CancelWindowLifetime,
    HardwareDualWindowAgentCloseSteps? Agent);

/// <summary>
/// The order of one HardwareDual window-close attempt: stop the monitor, show the indicator,
/// disable the window, cancel the window lifetime, run the fail-closed gate
/// (<see cref="HardwareDualWindowShutdownGate"/>), and on a blocked result show the guidance,
/// hide the indicator, re-enable the window and restart the monitor. The window's close handler
/// and the tests both call <see cref="RunAsync"/>, so a change to this order cannot pass the
/// tests without also changing the window (issue #239).
/// </summary>
/// <remarks>
/// Nothing here sends to native, starts or kills a process, or retries. The lease is released
/// only inside the gate, after the binding cleanup and the Agent's natural exit are confirmed.
/// </remarks>
public static class HardwareDualWindowCloseSequence
{
    public static async Task<HardwareDualWindowShutdownOutcome> RunAsync(HardwareDualWindowCloseSteps steps)
    {
        ArgumentNullException.ThrowIfNull(steps);

        steps.StopHostLifetimeMonitor();
        var agent = steps.Agent;
        // Issue #228: the wait below can take several seconds with the window disabled. Say so
        // before disabling, so the operator never sees a silent grey window. Display only.
        agent?.BeginConfirmation();
        steps.DisableWindow();
        steps.CancelWindowLifetime();
        if (agent is null)
        {
            return HardwareDualWindowShutdownOutcome.Success;
        }

        // The gate releases the exclusive lease only after both the typed binding cleanup
        // acknowledgment and the child's natural exit are confirmed. Any refusal, response loss
        // or timeout leaves the window open and the lease held; it never retries or kills Native.
        var outcome = await HardwareDualWindowShutdownGate.TryShutdownAsync(
            agent.CancelBindingAsync,
            agent.DisposeAgentAsync,
            agent.ReleaseExclusiveLease).ConfigureAwait(true);
        if (outcome.Completed)
        {
            // The indicator stays up until the caller's Close() takes the window away.
            return outcome;
        }

        // Issue #225: the Agent's own process start time (read here, not inside the ViewModel)
        // lets the blocked message estimate how much of the fixed native lifetime budget is
        // likely left, instead of only naming a status code the operator cannot act on.
        agent.ReportBlocked(outcome.BlockingCode, outcome.BlockingDetail, agent.EstimateRemainingAgentLifetime());
        // Issue #228: the result is on screen, so the "confirming" indicator goes.
        agent.EndConfirmation();
        agent.EnableWindow();
        agent.RestartHostLifetimeMonitor();
        return outcome;
    }
}
