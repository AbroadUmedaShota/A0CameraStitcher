using System.Diagnostics;
using System.Runtime.CompilerServices;

/// <summary>
/// Waits that a test places on events it expects to happen (a completion source, a task, a
/// predicate that must become true). The ceiling only separates "stuck" from "slow": it is far
/// above anything a loaded machine needs, so a test never fails because other builds or tests
/// were running at the same time. A wait that reaches the ceiling reports what it was waiting
/// for, how long it waited and where it was written.
///
/// Waits that stand for the passage of time itself (a product timer, an expiry) do not use
/// this class. Those tests inject a fake clock instead.
/// </summary>
internal static class TestWaits
{
    /// <summary>Upper bound for a wait on an event that is expected to happen.</summary>
    public static readonly TimeSpan Ceiling = TimeSpan.FromSeconds(60);

    private static readonly TimeSpan DefaultPollInterval = TimeSpan.FromMilliseconds(20);

    public static async Task AwaitAsync(
        this Task task,
        string? waitingFor = null,
        TimeSpan? ceiling = null,
        [CallerArgumentExpression(nameof(task))] string? expression = null,
        [CallerFilePath] string? file = null,
        [CallerLineNumber] int line = 0)
    {
        var limit = ceiling ?? Ceiling;
        var elapsed = Stopwatch.StartNew();
        try
        {
            await task.WaitAsync(limit).ConfigureAwait(false);
        }
        catch (TimeoutException) when (!task.IsCompleted)
        {
            throw TimedOut(waitingFor ?? expression, elapsed, limit, file, line);
        }
    }

    public static async Task<T> AwaitAsync<T>(
        this Task<T> task,
        string? waitingFor = null,
        TimeSpan? ceiling = null,
        [CallerArgumentExpression(nameof(task))] string? expression = null,
        [CallerFilePath] string? file = null,
        [CallerLineNumber] int line = 0)
    {
        var limit = ceiling ?? Ceiling;
        var elapsed = Stopwatch.StartNew();
        try
        {
            return await task.WaitAsync(limit).ConfigureAwait(false);
        }
        catch (TimeoutException) when (!task.IsCompleted)
        {
            throw TimedOut(waitingFor ?? expression, elapsed, limit, file, line);
        }
    }

    /// <summary>
    /// Waits until the task has finished, whatever its outcome, without rethrowing its exception.
    /// The caller then inspects the faulted or canceled task itself.
    /// </summary>
    public static async Task SettleAsync(
        this Task task,
        string? waitingFor = null,
        TimeSpan? ceiling = null,
        [CallerArgumentExpression(nameof(task))] string? expression = null,
        [CallerFilePath] string? file = null,
        [CallerLineNumber] int line = 0)
    {
        var limit = ceiling ?? Ceiling;
        var elapsed = Stopwatch.StartNew();
        using var stopTimer = new CancellationTokenSource();
        var finished = await Task.WhenAny(task, Task.Delay(limit, stopTimer.Token)).ConfigureAwait(false);
        if (finished != task)
        {
            throw TimedOut(waitingFor ?? expression, elapsed, limit, file, line);
        }
        stopTimer.Cancel();
    }

    /// <summary>Polls a condition that has no completion signal of its own. The condition is evaluated in the caller's synchronization context, because it may read UI objects.</summary>
    public static async Task PollUntilAsync(
        Func<bool> ready,
        string waitingFor,
        TimeSpan? ceiling = null,
        TimeSpan? interval = null,
        [CallerFilePath] string? file = null,
        [CallerLineNumber] int line = 0)
    {
        var limit = ceiling ?? Ceiling;
        var elapsed = Stopwatch.StartNew();
        while (!ready())
        {
            if (elapsed.Elapsed >= limit)
            {
                throw TimedOut(waitingFor, elapsed, limit, file, line);
            }
            await Task.Delay(interval ?? DefaultPollInterval);
        }
    }

    public static TimeoutException TimedOut(
        string? waitingFor, Stopwatch elapsed, TimeSpan limit, string? file, int line)
    {
        var where = file is null ? string.Empty : $" [{Path.GetFileName(file)}:{line}]";
        return new TimeoutException(
            $"Timed out after {elapsed.ElapsedMilliseconds} ms (ceiling {limit.TotalSeconds:0.#} s) " +
            $"waiting for: {waitingFor ?? "an unnamed task"}{where}");
    }
}
