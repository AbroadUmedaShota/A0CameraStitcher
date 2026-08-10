using System.Text.Json.Serialization;

namespace A0CameraStitcher.M3.Foundation;

public static class SimulatedTransactionProtocol
{
    public const string SchemaVersion = "a0.simulated-transaction.v1";
    public const string Marker = "Simulated";
}

[JsonConverter(typeof(JsonStringEnumConverter<CameraOperatingMode>))]
public enum CameraOperatingMode
{
    DualCamera,
    SingleCamera,
}

public sealed record CapturePlan
{
    public required CameraOperatingMode OperatingMode { get; init; }

    public required IReadOnlyList<string> RequiredCameraAliases { get; init; }

    public static CapturePlan Single(string alias)
    {
        var plan = new CapturePlan
        {
            OperatingMode = CameraOperatingMode.SingleCamera,
            RequiredCameraAliases = Array.AsReadOnly([alias]),
        };
        plan.Validate();
        return plan;
    }

    public static CapturePlan Dual()
    {
        var plan = new CapturePlan
        {
            OperatingMode = CameraOperatingMode.DualCamera,
            RequiredCameraAliases = Array.AsReadOnly(["CAM-A", "CAM-B"]),
        };
        plan.Validate();
        return plan;
    }

    public void Validate()
    {
        ArgumentNullException.ThrowIfNull(RequiredCameraAliases);
        var aliases = RequiredCameraAliases.ToArray();
        if (aliases.Any(alias => alias is not ("CAM-A" or "CAM-B")) ||
            aliases.Distinct(StringComparer.Ordinal).Count() != aliases.Length)
        {
            throw new ArgumentException("Capture aliases must be unique CAM-A or CAM-B values.", nameof(RequiredCameraAliases));
        }

        if (OperatingMode == CameraOperatingMode.SingleCamera && aliases.Length == 1)
        {
            return;
        }

        if (OperatingMode == CameraOperatingMode.DualCamera &&
            aliases.SequenceEqual(["CAM-A", "CAM-B"], StringComparer.Ordinal))
        {
            return;
        }

        throw new ArgumentException(
            "SingleCamera requires exactly one alias; DualCamera requires CAM-A then CAM-B.",
            nameof(RequiredCameraAliases));
    }
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

    public required CameraOperatingMode OperatingMode { get; init; }

    public required IReadOnlyList<string> RequiredCameraAliases { get; init; }

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

    Task<SimulatedWorkflowState> ExecuteAsync(
        Guid transactionId,
        CapturePlan capturePlan,
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

    // Backward-compatible additive fields: journals written before explicit modes are legacy DualCamera journals.
    public CameraOperatingMode OperatingMode { get; init; } = CameraOperatingMode.DualCamera;

    public List<string> RequiredCameraAliases { get; init; } = ["CAM-A", "CAM-B"];

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
