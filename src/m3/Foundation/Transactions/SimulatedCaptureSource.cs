using System.Collections.ObjectModel;

namespace A0CameraStitcher.M3.Foundation;

public sealed record SimulatedCaptureContent(string Text);

public interface ISimulatedCaptureSource
{
    ValueTask<SimulatedCaptureContent> CaptureAsync(
        string alias,
        Guid transactionId,
        CancellationToken cancellationToken);
}

public sealed class DeterministicSimulatedCaptureSource : ISimulatedCaptureSource
{
    private readonly string? _failAlias;
    private readonly Dictionary<string, int> _captureCounts = new(StringComparer.Ordinal);

    public DeterministicSimulatedCaptureSource(string? failAlias = null)
    {
        if (failAlias is not null && failAlias is not ("CAM-A" or "CAM-B"))
        {
            throw new ArgumentOutOfRangeException(nameof(failAlias));
        }

        _failAlias = failAlias;
    }

    public IReadOnlyDictionary<string, int> CaptureCounts =>
        new ReadOnlyDictionary<string, int>(_captureCounts);

    public ValueTask<SimulatedCaptureContent> CaptureAsync(
        string alias,
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        _captureCounts[alias] = GetCaptureCount(alias) + 1;

        if (string.Equals(alias, _failAlias, StringComparison.Ordinal))
        {
            throw new SimulatedCaptureException(alias);
        }

        return ValueTask.FromResult(
            new SimulatedCaptureContent(
                $"{SimulatedTransactionProtocol.Marker}{Environment.NewLine}" +
                $"transactionId={transactionId:N}{Environment.NewLine}" +
                $"alias={alias}{Environment.NewLine}" +
                "This is deterministic test text, not a camera image."));
    }

    public int GetCaptureCount(string alias) =>
        _captureCounts.TryGetValue(alias, out var count) ? count : 0;
}
