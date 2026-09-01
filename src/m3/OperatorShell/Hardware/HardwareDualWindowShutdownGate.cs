using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

/// <summary>
/// Coordinates the fail-closed order for closing a HardwareDual window.
/// The exclusive hardware lease is released only after Native confirms one
/// binding cleanup attempt and the Agent lifecycle confirms natural exit.
/// </summary>
public static class HardwareDualWindowShutdownGate
{
    public static async Task<HardwareDualWindowShutdownOutcome> TryShutdownAsync(
        Func<Task<DualBindingRefusal?>> cancelBindingAsync,
        Func<ValueTask> disposeAgentAsync,
        Action releaseExclusiveLease)
    {
        ArgumentNullException.ThrowIfNull(cancelBindingAsync);
        ArgumentNullException.ThrowIfNull(disposeAgentAsync);
        ArgumentNullException.ThrowIfNull(releaseExclusiveLease);

        try
        {
            var refusal = await cancelBindingAsync().ConfigureAwait(true);
            if (refusal is not null)
            {
                return HardwareDualWindowShutdownOutcome.Blocked(refusal.ResultCode);
            }

            await disposeAgentAsync().ConfigureAwait(true);
            releaseExclusiveLease();
            return HardwareDualWindowShutdownOutcome.Success;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            // No retry and no force-kill occur here. MainWindow remains open and
            // keeps the lease; a later close action is an explicit operator action.
            return HardwareDualWindowShutdownOutcome.Blocked(exception.GetType().Name);
        }
    }
}

public sealed record HardwareDualWindowShutdownOutcome(bool Completed, string BlockingCode)
{
    public static HardwareDualWindowShutdownOutcome Success { get; } = new(true, string.Empty);

    public static HardwareDualWindowShutdownOutcome Blocked(string code) =>
        new(false, string.IsNullOrWhiteSpace(code) ? "BindingCleanupUnconfirmed" : code);
}
