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
                return HardwareDualWindowShutdownOutcome.Blocked(refusal.ResultCode, refusal.Detail);
            }

            await disposeAgentAsync().ConfigureAwait(true);
            releaseExclusiveLease();
            return HardwareDualWindowShutdownOutcome.Success;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            // No retry and no force-kill occur here. MainWindow remains open and
            // keeps the lease; a later close action is an explicit operator action.
            return HardwareDualWindowShutdownOutcome.Blocked(exception.GetType().Name, exception.Message);
        }
    }
}

/// <summary>
/// <paramref name="BlockingCode"/> is the typed code (an exception type name or a refusal result
/// code) that tests and other call sites match on. <see cref="Blocked"/> passes it through
/// <see cref="SanitizeForOperatorDisplay"/> as well because it is shown on the operator screen
/// too; a regular code is a short single-line identifier, so that pass leaves it unchanged.
/// <paramref name="BlockingDetail"/> is the additional human-readable reason (an exception
/// message or refusal detail) that <paramref name="BlockingCode"/> alone does not carry. An
/// exception message can be multi-line and can embed local paths, so <see cref="Blocked"/>
/// sanitizes it the same way.
/// </summary>
public sealed record HardwareDualWindowShutdownOutcome(bool Completed, string BlockingCode, string BlockingDetail = "")
{
    /// <summary>
    /// Upper bound, in characters, of the code and the detail text each as shown on the
    /// operator screen.
    /// </summary>
    public const int MaxDetailLength = 160;

    public static HardwareDualWindowShutdownOutcome Success { get; } = new(true, string.Empty);

    public static HardwareDualWindowShutdownOutcome Blocked(string code, string? detail = null) =>
        new(
            false,
            string.IsNullOrWhiteSpace(code) ? "BindingCleanupUnconfirmed" : SanitizeForOperatorDisplay(code),
            SanitizeForOperatorDisplay(detail));

    /// <summary>
    /// Collapses text to a single line (every line terminator .NET recognizes, including
    /// U+2028, U+2029, U+0085 and form feed, plus tabs, vertical tabs and runs of spaces become one space) and
    /// bounds it to <see cref="MaxDetailLength"/> characters (an ellipsis marks a cut), so a raw
    /// exception message cannot flood or reshape the operator-facing text. Idempotent.
    /// </summary>
    public static string SanitizeForOperatorDisplay(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return string.Empty;
        }

        var singleLine = string.Join(
            ' ',
            value.ReplaceLineEndings(" ").Replace('\t', ' ').Replace('\v', ' ').Split(
                ' ',
                StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
        if (singleLine.Length <= MaxDetailLength)
        {
            return singleLine;
        }

        var cut = MaxDetailLength - 1;
        if (char.IsHighSurrogate(singleLine[cut - 1]))
        {
            cut--;
        }

        return singleLine[..cut] + "…";
    }
}
