using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

/// <summary>
/// Durable, fail-closed evidence state for one internally coordinated 100-run.
/// It reserves the supplied p95 approval before dispatch and never releases it.
/// </summary>
internal sealed class CaptureRecoveryOnlyHundredRunEvidenceSession
{
    private readonly CaptureRecoveryOnlyRunEvidenceWriter _writer;
    private readonly string _runId;
    private readonly CaptureRecoveryOnlyP95Approval _approval;
    private readonly HashSet<Guid> _tenRunTransactions;
    private readonly DateTimeOffset _startedAtUtc;
    private readonly List<CaptureRecoveryOnlyRunAttempt> _attempts = [];
    private readonly HashSet<Guid> _transactions = [];
    private readonly string _progressDirectory;
    private DateTimeOffset? _preparedAtUtc;
    private bool _stopped;
    private bool _published;
    private bool _interrupted;
    private bool _stopRecorded;
    private bool _stopRequested;
    private bool _publicationAttempted;

    internal CaptureRecoveryOnlyHundredRunEvidenceSession(
        CaptureRecoveryOnlyRunEvidenceWriter writer,
        string runId,
        string? approvalId,
        DateTimeOffset startedAtUtc)
    {
        _writer = writer ?? throw new ArgumentNullException(nameof(writer));
        if (!CaptureRecoveryOnlyRunEvidenceWriter.RunId.IsMatch(runId) ||
            startedAtUtc.Offset != TimeSpan.Zero)
        {
            throw new InvalidDataException("100-run identity or start time is invalid.");
        }

        _writer.EnsureRunDestinationAvailable(runId);
        _approval = _writer.LoadAndValidateApproval(approvalId, runId, startedAtUtc);
        var ten = _writer.LoadPublishedTenRun(_approval.TenRunId);
        _tenRunTransactions = ten.TransactionIds;
        _runId = runId;
        _startedAtUtc = startedAtUtc;
        _progressDirectory = Path.Combine(_writer._root, "hundred-run-executions", runId);
        _writer.EnsureWriteDirectory(Path.GetDirectoryName(_progressDirectory)!);
        if (Directory.Exists(_progressDirectory) || File.Exists(_progressDirectory))
            throw new IOException("100-run progress destination already exists.");

        // Claim before any dispatch-capable caller receives this session. The claim is
        // intentionally retained if a later durable write fails.
        _writer.ClaimApproval(_approval.ApprovalRecordId, runId, retainOnFailure: true);
        Directory.CreateDirectory(_progressDirectory);
        _writer.EnsureWriteDirectory(_progressDirectory);
        _writer.WriteAtomic(
            _progressDirectory,
            "start.json",
            JsonSerializer.Serialize(new
            {
                evidenceScope = "SoftwareAggregationOnly",
                hardwareExecutionVerified = false,
                productionRunner = false,
                capturePurpose = HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose,
                stitchOutcome = HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome,
                a0QualityApproval = HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval,
                runId,
                approvalRecordId = _approval.ApprovalRecordId,
                requestedCount = 100,
                startedAtUtc,
            }, CaptureRecoveryOnlyRunEvidenceWriter.IndentedJsonOptions));
    }

    internal string ProgressDirectory => _progressDirectory;

    /// <summary>Persists the next attempt start before capture is dispatched.</summary>
    internal void PrepareAttempt(DateTimeOffset startedAtUtc)
    {
        try
        {
            EnsureCanPrepare();
            _writer.EnsureApprovalClaimOwned(_approval.ApprovalRecordId, _runId);
            if (startedAtUtc.Offset != TimeSpan.Zero ||
                startedAtUtc < (_attempts.LastOrDefault()?.CompletedAtUtc ?? _startedAtUtc))
            {
                throw new InvalidDataException("Attempt start ordering is invalid.");
            }

            var number = _attempts.Count + 1;
            _writer.WriteAtomic(
                _progressDirectory,
                "attempt-" + number.ToString("D3", CultureInfo.InvariantCulture) + ".start.json",
                JsonSerializer.Serialize(new
                {
                    evidenceScope = "SoftwareAggregationOnly",
                    hardwareExecutionVerified = false,
                    productionRunner = false,
                    capturePurpose = HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose,
                    stitchOutcome = HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome,
                    a0QualityApproval = HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval,
                    runId = _runId,
                    attemptNumber = number,
                    startedAtUtc,
                }, CaptureRecoveryOnlyRunEvidenceWriter.IndentedJsonOptions));
            _preparedAtUtc = startedAtUtc;
        }
        catch
        {
            _stopped = true;
            throw;
        }
    }

    /// <summary>Durably records one typed terminal outcome before another dispatch.</summary>
    internal void RecordAttempt(CaptureRecoveryOnlyRunAttempt attempt)
    {
        try
        {
            RecordAttemptCore(attempt);
        }
        catch
        {
            _stopped = true;
            throw;
        }
    }

    private void RecordAttemptCore(CaptureRecoveryOnlyRunAttempt attempt)
    {
        ArgumentNullException.ThrowIfNull(attempt);
        EnsureCanRecord();
        if (_preparedAtUtc != attempt.StartedAtUtc ||
            attempt.CompletedAtUtc < attempt.StartedAtUtc ||
            attempt.StartedAtUtc.Offset != TimeSpan.Zero ||
            attempt.CompletedAtUtc.Offset != TimeSpan.Zero)
        {
            throw new InvalidDataException("Attempt does not match its durable start marker.");
        }

        CaptureRecoveryOnlyRunEvidenceWriter.ValidateOutcome(attempt);
        var transactionId = attempt.Outcome.TransactionId;
        if (!_transactions.Add(transactionId) || _tenRunTransactions.Contains(transactionId))
            throw new InvalidDataException("100-run transaction identity is invalid.");

        var number = _attempts.Count + 1;
        var evidence = CaptureRecoveryOnlyRunEvidenceWriter.ToAttempt(number, attempt);
        // A write failure deliberately leaves the global claim held and makes this
        // in-memory session non-dispatchable; an ambiguous attempt cannot advance.
        _writer.ClaimTransactions(_runId, [evidence], retainOnFailure: true);
        try
        {
            _writer.WriteAtomic(
                _progressDirectory,
                "attempt-" + number.ToString("D3", CultureInfo.InvariantCulture) + ".json",
                JsonSerializer.Serialize(new
                {
                    evidenceScope = "SoftwareAggregationOnly",
                    hardwareExecutionVerified = false,
                    productionRunner = false,
                    capturePurpose = HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose,
                    stitchOutcome = HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome,
                    a0QualityApproval = HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval,
                    runId = _runId,
                    evidence.AttemptNumber,
                    transactionId = evidence.TransactionId.ToString("N"),
                    evidence.StartedAtUtc,
                    evidence.CompletedAtUtc,
                    durationMilliseconds = evidence.Duration.TotalMilliseconds,
                    evidence.Succeeded,
                    terminalState = evidence.TerminalState.ToString(),
                    failureCode = evidence.FailureCode.ToString(),
                    evidence.AutomaticRetryCount,
                    evidence.Originals,
                }, CaptureRecoveryOnlyRunEvidenceWriter.IndentedJsonOptions));
        }
        catch
        {
            _stopped = true;
            throw;
        }

        _attempts.Add(attempt);
        _preparedAtUtc = null;
        if (!attempt.Outcome.Succeeded)
            _stopped = true;
    }

    /// <summary>
    /// Records a non-aggregate interruption after a durable start marker. This path
    /// preserves any recovered originals but never makes the outcome eligible for
    /// a 100-run aggregate or a later dispatch in this session.
    /// </summary>
    internal void RecordInterruptedAttempt(CaptureRecoveryOnlyRunAttempt attempt)
    {
        try
        {
            RecordInterruptedAttemptCore(attempt);
        }
        catch
        {
            _stopped = true;
            throw;
        }
    }

    private void RecordInterruptedAttemptCore(CaptureRecoveryOnlyRunAttempt attempt)
    {
        ArgumentNullException.ThrowIfNull(attempt);
        EnsureCanRecord();
        if (_preparedAtUtc != attempt.StartedAtUtc ||
            attempt.CompletedAtUtc < attempt.StartedAtUtc ||
            attempt.StartedAtUtc.Offset != TimeSpan.Zero ||
            attempt.CompletedAtUtc.Offset != TimeSpan.Zero)
        {
            throw new InvalidDataException("Interrupted attempt does not match its durable start marker.");
        }

        var outcome = attempt.Outcome ?? throw new InvalidDataException("Interrupted outcome is missing.");
        if (!(outcome.RecoveryPending ||
              outcome.TerminalState == DualHardwareCaptureTerminalState.HardwarePending ||
              outcome.BindingInvalidationReason != DualBindingInvalidationReason.None) ||
            outcome.TransactionId == Guid.Empty ||
            outcome.AutomaticRetryCount != 0 ||
            outcome.Originals is null ||
            outcome.Originals.Count > 2 ||
            !outcome.Originals.Select(original => original.Alias)
                .SequenceEqual(new[] { "CAM-A", "CAM-B" }.Take(outcome.Originals.Count)))
        {
            throw new InvalidDataException("Interrupted outcome is not a safe stop condition.");
        }

        foreach (var original in outcome.Originals)
        {
            if (Path.GetFileName(original.Path) != "original.jpg" ||
                original.Width != 7360 || original.Height != 4912 ||
                original.SizeBytes <= 0 || !original.IsCanonicalJpeg ||
                !CaptureRecoveryOnlyRunEvidenceWriter.Hash.IsMatch(original.Sha256) ||
                CaptureRecoveryOnlyRunEvidenceWriter.ContainsSensitive(original.Path))
            {
                throw new InvalidDataException("Interrupted original evidence is invalid.");
            }
        }

        var number = _attempts.Count + 1;
        var written = false;
        try
        {
            if (!_transactions.Add(outcome.TransactionId) || _tenRunTransactions.Contains(outcome.TransactionId))
                throw new InvalidDataException("Interrupted transaction identity is invalid.");
            var interruptedEvidence = new CaptureRecoveryOnlyRunAttemptEvidence(
                number,
                outcome.TransactionId,
                attempt.StartedAtUtc,
                attempt.CompletedAtUtc,
                attempt.CompletedAtUtc - attempt.StartedAtUtc,
                false,
                outcome.TerminalState,
                outcome.FailureCode,
                outcome.AutomaticRetryCount,
                outcome.Originals.Select(original => new CaptureRecoveryOnlyOriginalEvidence(
                    original.Alias, "original.jpg", original.SizeBytes, original.Sha256)).ToArray());
            _writer.ClaimTransactions(_runId, [interruptedEvidence], retainOnFailure: true);
            _writer.WriteAtomic(
                _progressDirectory,
                "attempt-" + number.ToString("D3", CultureInfo.InvariantCulture) + ".interrupted.json",
                JsonSerializer.Serialize(new
                {
                    evidenceScope = "SoftwareAggregationOnly",
                    hardwareExecutionVerified = false,
                    productionRunner = false,
                    capturePurpose = HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose,
                    stitchOutcome = HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome,
                    a0QualityApproval = HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval,
                    runId = _runId,
                    attemptNumber = number,
                    transactionId = outcome.TransactionId.ToString("N"),
                    attempt.StartedAtUtc,
                    attempt.CompletedAtUtc,
                    durationMilliseconds = (attempt.CompletedAtUtc - attempt.StartedAtUtc).TotalMilliseconds,
                    recoveryPending = outcome.RecoveryPending,
                    terminalState = outcome.TerminalState.ToString(),
                    failureCode = outcome.FailureCode.ToString(),
                    automaticRetryCount = outcome.AutomaticRetryCount,
                    bindingInvalidationReason = outcome.BindingInvalidationReason.ToString(),
                    originals = outcome.Originals.Select(original => new
                    {
                        original.Alias,
                        fileName = "original.jpg",
                        original.SizeBytes,
                        original.Sha256,
                    }).ToArray(),
                }, CaptureRecoveryOnlyRunEvidenceWriter.IndentedJsonOptions));
            written = true;
        }
        finally
        {
            // The approval and observed transaction claim remain held. Closing this
            // session prevents a new dispatch under another identity.
            if (written)
                _preparedAtUtc = null;
            _stopped = true;
            _interrupted = true;
        }
    }

    internal CaptureRecoveryOnlyRunEvidenceFiles Publish()
    {
        if (_published || _publicationAttempted || _stopRequested || _interrupted ||
            _preparedAtUtc is not null || _attempts.Count == 0)
            throw new InvalidDataException("100-run evidence is not publishable.");
        var firstFailure = _attempts.FindIndex(attempt => !attempt.Outcome.Succeeded);
        if (firstFailure >= 0 && firstFailure != _attempts.Count - 1 ||
            firstFailure < 0 && _attempts.Count != 100)
        {
            throw new InvalidDataException("100-run must contain 100 successes or stop at its first failure.");
        }

        var attempts = _attempts.Select((attempt, index) =>
            CaptureRecoveryOnlyRunEvidenceWriter.ToAttempt(index + 1, attempt)).ToArray();
        var failure = firstFailure < 0 ? null : attempts[firstFailure];
        var durations = attempts.Select(attempt => attempt.Duration).Order().ToArray();
        var evidence = new CaptureRecoveryOnlyRunEvidence(
            _runId,
            100,
            attempts,
            failure,
            CaptureRecoveryOnlyRunEvidenceWriter.Percentile50(durations),
            CaptureRecoveryOnlyRunEvidenceWriter.Percentile95(durations),
            durations.Max(),
            failure is null
                ? CaptureRecoveryOnlyRunVerdict.SoftwareAggregatePass
                : CaptureRecoveryOnlyRunVerdict.SoftwareAggregateFail,
            "OperatorRecorded",
            _approval);
        // If publishing has reached its side-effect boundary, an I/O uncertainty
        // cannot be converted into a second publication attempt.
        _publicationAttempted = true;
        _stopped = true;
        var files = _writer.WriteRun(evidence);
        _writer.WriteAtomic(
            _progressDirectory,
            "published.json",
            JsonSerializer.Serialize(new
            {
                evidenceScope = "SoftwareAggregationOnly",
                hardwareExecutionVerified = false,
                productionRunner = false,
                capturePurpose = HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose,
                stitchOutcome = HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome,
                a0QualityApproval = HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval,
                runId = _runId,
                completedAtUtc = attempts[^1].CompletedAtUtc,
                verdict = evidence.Verdict.ToString(),
            }, CaptureRecoveryOnlyRunEvidenceWriter.IndentedJsonOptions));
        _published = true;
        _stopped = true;
        return files;
    }

    internal void Stop(string fixedReasonCode, Guid? pendingTransactionId)
    {
        if (_published || _stopRecorded || !IsCoordinatorStopReason(fixedReasonCode))
        {
            throw new InvalidDataException("100-run stop state is invalid.");
        }
        if (pendingTransactionId == Guid.Empty)
            throw new InvalidDataException("A pending transaction ID cannot be empty.");

        // Latch before the durable write: a failed stop write is still an unknown
        // stop boundary, never a reason to publish a pass from prior successes.
        _stopRequested = true;
        _stopped = true;
        _writer.WriteAtomic(
            _progressDirectory,
            "stopped.json",
            JsonSerializer.Serialize(new
            {
                evidenceScope = "SoftwareAggregationOnly",
                hardwareExecutionVerified = false,
                productionRunner = false,
                capturePurpose = HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose,
                stitchOutcome = HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome,
                a0QualityApproval = HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval,
                runId = _runId,
                fixedReasonCode,
                pendingTransactionId = pendingTransactionId?.ToString("N"),
                pendingAttemptStartedAtUtc = _preparedAtUtc,
                completedAttemptCount = _attempts.Count,
            }, CaptureRecoveryOnlyRunEvidenceWriter.IndentedJsonOptions));
        _preparedAtUtc = null;
        _stopRecorded = true;
    }

    private static bool IsCoordinatorStopReason(string? value) => value is
        "capture_outcome_unknown" or
        "pending_or_invalid_binding" or
        "approval_or_evidence_rejected" or
        "cancelled" or
        "same_id_recovery_required" or
        "clock_invalid" or
        "binding_invalid" or
        "workflow_not_ready" or
        "host_budget_insufficient";

    private void EnsureCanPrepare()
    {
        if (_stopped || _published || _preparedAtUtc is not null || _attempts.Count >= 100)
            throw new InvalidDataException("100-run cannot dispatch another attempt.");
    }

    private void EnsureCanRecord()
    {
        if (_stopped || _published || _preparedAtUtc is null)
            throw new InvalidDataException("100-run has no dispatchable attempt to record.");
    }
}
