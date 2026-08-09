using System.Text.Json.Serialization;

namespace A0CameraStitcher.M3.Foundation;

public static class SimulatedTransactionProtocol
{
    public const string SchemaVersion = "a0.simulated-transaction.v1";
    public const string Marker = "Simulated";
}

[JsonConverter(typeof(JsonStringEnumConverter<SimulatedTransactionState>))]
public enum SimulatedTransactionState
{
    Idle,
    CaptureA,
    PersistA,
    CaptureB,
    PersistB,
    Complete,
    FailedPartial,
}

public enum SimulatedCrashPoint
{
    None,
    AfterPersistA,
}

public enum SimulatedWorkflowScenario
{
    Success,
    FailLiveViewStop,
    FailCaptureA,
    FailCaptureB,
    CrashAfterPersistA,
}

public sealed record SimulatedWorkflowState
{
    public required bool Simulation { get; init; }

    public required string Marker { get; init; }

    public required Guid TransactionId { get; init; }

    public required SimulatedTransactionState State { get; init; }

    public required bool IsTerminal { get; init; }

    public required IReadOnlyList<string> RetainedOriginalAliases { get; init; }

    public required string? TerminalReason { get; init; }

    public required int AutomaticRetryCount { get; init; }
}

public interface ISimulatedTransactionService
{
    Task<IReadOnlyList<SimulatedWorkflowState>> InitializeAsync(CancellationToken cancellationToken = default);

    Task<SimulatedWorkflowState> ExecuteAsync(
        Guid transactionId,
        SimulatedWorkflowScenario scenario,
        CancellationToken cancellationToken = default);
}

public sealed record SimulatedOriginalRecord
{
    public required string Alias { get; init; }

    public required string RelativePath { get; init; }

    public required long Size { get; init; }

    public required string Sha256 { get; init; }

    public required string Marker { get; init; }
}

public sealed record SimulatedTransactionJournal
{
    public required string SchemaVersion { get; init; }

    public required bool Simulation { get; init; }

    public required string Marker { get; init; }

    public required Guid TransactionId { get; init; }

    public required SimulatedTransactionState State { get; set; }

    public required List<SimulatedTransactionState> TransitionHistory { get; init; }

    public required List<SimulatedOriginalRecord> Originals { get; init; }

    public required int AutomaticRetryCount { get; init; }

    public required string? TerminalReason { get; set; }

    public required DateTimeOffset UpdatedAtUtc { get; set; }
}

public sealed class SimulatedProcessCrashException : Exception
{
    public SimulatedProcessCrashException(Guid transactionId)
        : base($"A simulated process crash interrupted transaction {transactionId:N}.")
    {
        TransactionId = transactionId;
    }

    public Guid TransactionId { get; }
}

public sealed class SimulatedCaptureException : Exception
{
    public SimulatedCaptureException(string alias)
        : base($"The simulated capture for {alias} failed.")
    {
        Alias = alias;
    }

    public string Alias { get; }
}
