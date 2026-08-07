namespace A0CameraStitcher.M3.Foundation;

public sealed class SimulationFoundationService : ISimulatedTransactionService
{
    private readonly string _rootDirectory;
    private readonly TimeProvider _timeProvider;
    private readonly SemaphoreSlim _serviceGate = new(1, 1);

    public SimulationFoundationService(string rootDirectory, TimeProvider? timeProvider = null)
    {
        if (string.IsNullOrWhiteSpace(rootDirectory))
        {
            throw new ArgumentException("A transaction root is required.", nameof(rootDirectory));
        }

        _rootDirectory = rootDirectory;
        _timeProvider = timeProvider ?? TimeProvider.System;
    }

    public async Task<IReadOnlyList<SimulatedWorkflowState>> InitializeAsync(
        CancellationToken cancellationToken = default)
    {
        await _serviceGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var coordinator = CreateCoordinator(new DeterministicSimulatedCaptureSource());
            var recovered = await coordinator.InitializeAsync(cancellationToken).ConfigureAwait(false);
            return recovered.Select(ToWorkflowState).ToArray();
        }
        finally
        {
            _serviceGate.Release();
        }
    }

    public async Task<SimulatedWorkflowState> ExecuteAsync(
        Guid transactionId,
        SimulatedWorkflowScenario scenario,
        CancellationToken cancellationToken = default)
    {
        await _serviceGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var failAlias = scenario switch
            {
                SimulatedWorkflowScenario.FailCaptureA => "CAM-A",
                SimulatedWorkflowScenario.FailCaptureB => "CAM-B",
                _ => null,
            };
            var crashPoint = scenario == SimulatedWorkflowScenario.CrashAfterPersistA
                ? SimulatedCrashPoint.AfterPersistA
                : SimulatedCrashPoint.None;
            var coordinator = CreateCoordinator(new DeterministicSimulatedCaptureSource(failAlias));

            try
            {
                var result = await coordinator.ExecuteAsync(
                    transactionId,
                    crashPoint,
                    cancellationToken).ConfigureAwait(false);
                return ToWorkflowState(result);
            }
            catch (SimulatedProcessCrashException)
            {
                return ToWorkflowState(await coordinator.LoadAsync(transactionId, cancellationToken).ConfigureAwait(false));
            }
        }
        finally
        {
            _serviceGate.Release();
        }
    }

    private DurableSimulatedCaptureCoordinator CreateCoordinator(ISimulatedCaptureSource source) =>
        new(_rootDirectory, source, _timeProvider);

    private static SimulatedWorkflowState ToWorkflowState(SimulatedTransactionJournal journal) =>
        new()
        {
            Simulation = true,
            Marker = SimulatedTransactionProtocol.Marker,
            TransactionId = journal.TransactionId,
            State = journal.State,
            IsTerminal = journal.State is SimulatedTransactionState.Complete or SimulatedTransactionState.FailedPartial,
            RetainedOriginalAliases = journal.Originals.Select(original => original.Alias).ToArray(),
            TerminalReason = journal.TerminalReason,
        };
}
