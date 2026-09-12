using System.IO;
using System.Runtime.CompilerServices;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal enum CaptureRecoveryOnlyHundredRunStatus
{
    Completed,
    Failed,
    HardwarePending,
    Blocked,
}

internal sealed record CaptureRecoveryOnlyHundredRunResult(
    CaptureRecoveryOnlyHundredRunStatus Status,
    int AttemptedCount,
    HardwareDualCaptureRecoveryOnlyExecution? LastOutcome,
    CaptureRecoveryOnlyRunEvidenceFiles? EvidenceFiles,
    string? ProgressDirectory,
    string Detail);

/// <summary>
/// Internal, deliberately unwired software slice. It neither starts/binds an Agent
/// nor establishes operator authority or hardware readiness. A caller must supply
/// a side-effect-free check of the SAME binding and the existing host's remaining
/// budget, not a freshly reset 600 seconds. No UI/CLI can start this coordinator.
/// </summary>
internal sealed class CaptureRecoveryOnlyHundredRunCoordinator
{
    internal const int RequestedCount = 100;
    private static readonly TimeSpan PairWatchdog = TimeSpan.FromSeconds(180);
    private static readonly TimeSpan MaximumHostBudget = TimeSpan.FromSeconds(600);
    private static readonly ConditionalWeakTable<IHardwareDualCaptureRecoveryOnlyWorkflow, SemaphoreSlim>
        WorkflowGates = new();

    private readonly IHardwareDualCaptureRecoveryOnlyWorkflow _workflow;
    private readonly CaptureRecoveryOnlyRunEvidenceWriter _writer;
    private readonly Func<bool> _bindingIsCurrent;
    private readonly Func<TimeSpan> _remainingHostBudget;
    private readonly TimeProvider _time;

    internal CaptureRecoveryOnlyHundredRunCoordinator(
        IHardwareDualCaptureRecoveryOnlyWorkflow workflow,
        CaptureRecoveryOnlyRunEvidenceWriter writer,
        Func<bool> bindingIsCurrent,
        Func<TimeSpan> remainingHostBudget,
        TimeProvider? timeProvider = null)
    {
        _workflow = workflow ?? throw new ArgumentNullException(nameof(workflow));
        _writer = writer ?? throw new ArgumentNullException(nameof(writer));
        _bindingIsCurrent = bindingIsCurrent ?? throw new ArgumentNullException(nameof(bindingIsCurrent));
        _remainingHostBudget = remainingHostBudget ?? throw new ArgumentNullException(nameof(remainingHostBudget));
        _time = timeProvider ?? TimeProvider.System;
    }

    internal async Task<CaptureRecoveryOnlyHundredRunResult> RunAsync(
        string runId,
        string? approvalRecordId,
        DualCameraIdentitySnapshot identitySnapshot,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(identitySnapshot);
        var gate = WorkflowGates.GetValue(_workflow, _ => new SemaphoreSlim(1, 1));
        // Reject competing invocations instead of queueing an implicit later run.
        if (!await gate.WaitAsync(0, CancellationToken.None).ConfigureAwait(false))
            return new(CaptureRecoveryOnlyHundredRunStatus.Blocked, 0, null, null, null, "run_already_active");

        CaptureRecoveryOnlyHundredRunEvidenceSession? evidence = null;
        HardwareDualCaptureRecoveryOnlyExecution? last = null;
        var attempted = 0;
        try
        {
            var startedTimestamp = _time.GetTimestamp();
            var initialBudget = _remainingHostBudget();
            var previousCompletion = _time.GetUtcNow();
            var block = CheckStart(identitySnapshot, initialBudget, startedTimestamp, previousCompletion, cancellationToken);
            if (block is not null)
                return Stop(block.Value.Status, block.Value.Reason);

            // This consumes a validated, exact-target approval before CaptureAsync.
            // Failed IO after claiming it is deliberately not rolled back/reused.
            evidence = _writer.BeginHundredRun(runId, approvalRecordId, previousCompletion);
            for (var index = 0; index < RequestedCount; index++)
            {
                block = CheckStart(identitySnapshot, initialBudget, startedTimestamp, previousCompletion, cancellationToken);
                if (block is not null)
                    return Stop(block.Value.Status, block.Value.Reason);

                var attemptStarted = _time.GetUtcNow();
                evidence.PrepareAttempt(attemptStarted);
                // Approval/checkpoint persistence can take time, or invalidate the
                // binding. Recheck immediately before invoking the workflow.
                block = CheckStart(identitySnapshot, initialBudget, startedTimestamp, attemptStarted, cancellationToken);
                if (block is not null)
                    return Stop(block.Value.Status, block.Value.Reason);

                attempted++;
                try
                {
                    last = await _workflow.CaptureAsync(identitySnapshot, cancellationToken).ConfigureAwait(false);
                }
                catch (Exception exception) when (exception is not OutOfMemoryException)
                {
                    // The workflow owns the pending ID and same-ID recovery. This
                    // coordinator never calls RecoverAsync or dispatches a replacement.
                    return Stop(CaptureRecoveryOnlyHundredRunStatus.HardwarePending, "capture_outcome_unknown");
                }

                if (last is null)
                    return Stop(CaptureRecoveryOnlyHundredRunStatus.HardwarePending, "capture_outcome_unknown");
                previousCompletion = _time.GetUtcNow();
                ValidateOutcomeLocation(last);
                if (last.RecoveryPending ||
                    last.TerminalState == DualHardwareCaptureTerminalState.HardwarePending ||
                    last.BindingInvalidationReason != DualBindingInvalidationReason.None)
                {
                    evidence.RecordInterruptedAttempt(new(attemptStarted, previousCompletion, last));
                    return Stop(CaptureRecoveryOnlyHundredRunStatus.HardwarePending, "pending_or_invalid_binding");
                }
                evidence.RecordAttempt(new(attemptStarted, previousCompletion, last));
                if (!last.Succeeded)
                {
                    var failureFiles = evidence.Publish();
                    return new(CaptureRecoveryOnlyHundredRunStatus.Failed, attempted, last,
                        failureFiles, evidence.ProgressDirectory, "first_failure_no_retry");
                }
                // A successful typed outcome does not prove the binding/session is
                // still current. Include the final attempt in this completion gate;
                // it has no next iteration in which to discover pending/expiry.
                block = CheckStart(identitySnapshot, initialBudget, startedTimestamp,
                    previousCompletion, cancellationToken, reserveNextPair: false);
                if (block is not null)
                    return Stop(block.Value.Status, block.Value.Reason);
            }

            block = CheckStart(identitySnapshot, initialBudget, startedTimestamp,
                previousCompletion, cancellationToken, reserveNextPair: false);
            if (block is not null)
                return Stop(block.Value.Status, block.Value.Reason);
            var files = evidence.Publish();
            return new(CaptureRecoveryOnlyHundredRunStatus.Completed, attempted, last,
                files, evidence.ProgressDirectory, "100_first_attempts_software_only");
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            return Stop(attempted == 0 ? CaptureRecoveryOnlyHundredRunStatus.Blocked : CaptureRecoveryOnlyHundredRunStatus.Failed,
                "approval_or_evidence_rejected");
        }
        finally
        {
            gate.Release();
        }

        CaptureRecoveryOnlyHundredRunResult Stop(CaptureRecoveryOnlyHundredRunStatus status, string reason)
        {
            if (evidence is not null)
            {
                try
                {
                    evidence.Stop(reason, _workflow.PendingTransactionId ??
                        (last is { RecoveryPending: true } && last.TransactionId != Guid.Empty ? last.TransactionId : null));
                }
                catch (Exception exception) when (exception is not OutOfMemoryException)
                {
                    // Keep the start/attempt markers and consumed approval. No retry,
                    // cleanup, alternate destination or success substitution.
                    reason += ";progress_write_failed";
                }
            }
            return new(status, attempted, last, null, evidence?.ProgressDirectory, reason);
        }
    }

    private (CaptureRecoveryOnlyHundredRunStatus Status, string Reason)? CheckStart(
        DualCameraIdentitySnapshot identity,
        TimeSpan initialBudget,
        long startedTimestamp,
        DateTimeOffset previousTime,
        CancellationToken cancellationToken,
        bool reserveNextPair = true)
    {
        if (cancellationToken.IsCancellationRequested)
            return (CaptureRecoveryOnlyHundredRunStatus.Blocked, "cancelled");
        if (_workflow.HasPendingRecovery || _workflow.PendingTransactionId is not null)
            return (CaptureRecoveryOnlyHundredRunStatus.HardwarePending, "same_id_recovery_required");
        var now = _time.GetUtcNow();
        if (now.Offset != TimeSpan.Zero || now < previousTime)
            return (CaptureRecoveryOnlyHundredRunStatus.Blocked, "clock_invalid");
        if (!_bindingIsCurrent() || !identity.EvaluateAt(now).IsReady ||
            identity.ObservedAtUtc.Offset != TimeSpan.Zero || identity.ExpiresAtUtc.Offset != TimeSpan.Zero ||
            identity.ObservedAtUtc > now || identity.ExpiresAtUtc <= now)
            return (CaptureRecoveryOnlyHundredRunStatus.HardwarePending, "binding_invalid");
        if (reserveNextPair && !_workflow.CanStartNewCapture)
            return (CaptureRecoveryOnlyHundredRunStatus.Blocked, "workflow_not_ready");

        var elapsed = _time.GetElapsedTime(startedTimestamp, _time.GetTimestamp());
        var remaining = _remainingHostBudget();
        // No host reset/extension. Reserve a full existing per-pair watchdog and
        // reject the exact boundary too; measured p95 is NOT an upper time bound.
        var requiredRemaining = reserveNextPair ? PairWatchdog : TimeSpan.Zero;
        if (initialBudget > MaximumHostBudget || initialBudget <= requiredRemaining || elapsed < TimeSpan.Zero ||
            remaining <= requiredRemaining || initialBudget - elapsed <= requiredRemaining)
            return (CaptureRecoveryOnlyHundredRunStatus.Blocked, "host_budget_insufficient");
        return null;
    }

    private void ValidateOutcomeLocation(HardwareDualCaptureRecoveryOnlyExecution outcome)
    {
        // Unknown/pre-dispatch outcomes may have no ID/original yet; evidence
        // validation keeps them out of successful aggregate publication.
        if (outcome.TransactionId == Guid.Empty && outcome.Originals.Count == 0)
            return;
        var expectedDirectory = Path.GetFullPath(Path.Combine(_workflow.TransactionRoot, outcome.TransactionId.ToString("N")));
        if (!string.Equals(Path.GetFullPath(outcome.TransactionDirectory), expectedDirectory, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Outcome transaction directory does not match its ID.");
        foreach (var original in outcome.Originals)
        {
            var expected = Path.Combine(expectedDirectory, original.Alias, "original.jpg");
            if (!string.Equals(Path.GetFullPath(original.Path), expected, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Original does not belong to the outcome transaction and alias.");
        }
    }
}
