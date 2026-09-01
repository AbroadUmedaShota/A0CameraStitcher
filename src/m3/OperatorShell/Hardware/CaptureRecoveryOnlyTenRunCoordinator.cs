using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal enum CaptureRecoveryOnlyTenRunStatus
{
    Completed,
    Failed,
    HardwarePending,
    Blocked,
}

internal sealed record CaptureRecoveryOnlyTenRunResult(
    CaptureRecoveryOnlyTenRunStatus Status,
    int AttemptedCount,
    HardwareDualCaptureRecoveryOnlyExecution? LastOutcome,
    CaptureRecoveryOnlyRunEvidenceFiles? EvidenceFiles,
    string Detail);

/// <summary>
/// Runs the explicitly selected 10-pair CaptureRecoveryOnly acceptance slice.
/// It keeps one caller-supplied binding snapshot, never retries, and never turns
/// an incomplete hardware result into software aggregate evidence.
/// </summary>
public sealed class CaptureRecoveryOnlyTenRunCoordinator
{
    internal const int RequestedCount = 10;

    private readonly IHardwareDualCaptureRecoveryOnlyWorkflow _workflow;
    private readonly CaptureRecoveryOnlyRunEvidenceWriter _evidenceWriter;
    private readonly Func<DateTimeOffset> _utcNow;

    internal CaptureRecoveryOnlyTenRunCoordinator(
        IHardwareDualCaptureRecoveryOnlyWorkflow workflow,
        CaptureRecoveryOnlyRunEvidenceWriter evidenceWriter,
        Func<DateTimeOffset>? utcNow = null)
    {
        _workflow = workflow ?? throw new ArgumentNullException(nameof(workflow));
        _evidenceWriter = evidenceWriter ?? throw new ArgumentNullException(nameof(evidenceWriter));
        _utcNow = utcNow ?? (() => DateTimeOffset.UtcNow);
    }

    internal async Task<CaptureRecoveryOnlyTenRunResult> RunAsync(
        string runId,
        DualCameraIdentitySnapshot identitySnapshot,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(runId))
            throw new ArgumentException("A run ID is required.", nameof(runId));
        ArgumentNullException.ThrowIfNull(identitySnapshot);

        var attempts = new List<CaptureRecoveryOnlyRunAttempt>(RequestedCount);
        for (var index = 0; index < RequestedCount; index++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_workflow.HasPendingRecovery)
            {
                return Pending(
                    attempts,
                    "A prior transaction requires same-ID recovery; no new capture or retry was started.");
            }
            if (!_workflow.CanStartNewCapture)
            {
                return new(
                    CaptureRecoveryOnlyTenRunStatus.Blocked,
                    attempts.Count,
                    attempts.LastOrDefault()?.Outcome,
                    null,
                    _workflow.NewCaptureBlocker);
            }

            var startedAtUtc = _utcNow();
            var outcome = await _workflow.CaptureAsync(identitySnapshot, cancellationToken).ConfigureAwait(false);
            var completedAtUtc = _utcNow();
            attempts.Add(new(startedAtUtc, completedAtUtc, outcome));

            if (outcome.RecoveryPending ||
                outcome.TerminalState == DualHardwareCaptureTerminalState.HardwarePending ||
                outcome.BindingInvalidationReason != DualBindingInvalidationReason.None)
            {
                return Pending(
                    attempts,
                    outcome.FailureReason ?? "Hardware or binding state requires operator inspection.");
            }

            if (!outcome.Succeeded)
            {
                var evidence = _evidenceWriter.CreateAndPublish(
                    new CaptureRecoveryOnlyRunEvidenceRequest(runId, RequestedCount, attempts));
                return new(
                    CaptureRecoveryOnlyTenRunStatus.Failed,
                    attempts.Count,
                    outcome,
                    evidence,
                    "The run stopped at its first terminal failure; automatic retry count remains 0.");
            }
        }

        var files = _evidenceWriter.CreateAndPublish(
            new CaptureRecoveryOnlyRunEvidenceRequest(runId, RequestedCount, attempts));
        return new(
            CaptureRecoveryOnlyTenRunStatus.Completed,
            attempts.Count,
            attempts[^1].Outcome,
            files,
            "All 10 CAM-A to CAM-B capture/recovery pairs completed; A0 quality remains Unapproved.");
    }

    private static CaptureRecoveryOnlyTenRunResult Pending(
        IReadOnlyList<CaptureRecoveryOnlyRunAttempt> attempts,
        string detail) =>
        new(
            CaptureRecoveryOnlyTenRunStatus.HardwarePending,
            attempts.Count,
            attempts.LastOrDefault()?.Outcome,
            null,
            detail);
}
