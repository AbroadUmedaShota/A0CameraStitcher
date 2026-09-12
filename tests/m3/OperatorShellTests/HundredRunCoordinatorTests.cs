using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;
using A0CameraStitcher.M3.OperatorShell.Hardware;

internal static class HundredRunCoordinatorTests
{
    internal static async Task RunAsync()
    {
        await CompletesOneHundredWithoutRetryOrSnapshotReplacementAsync();
        await RejectsApprovalProblemsBeforeDispatchAsync();
        await RejectsChangedTenRunAndApprovalTimeAsync();
        await StopsAtFirstFailureAndPreservesCamAAsync();
        await PersistsInterruptedOutcomeWithoutRecoveryAsync();
        await RejectsUnsafeStartAndFrozenBudgetAsync();
        await StopsAfterEvidenceFailureOrInvalidOutcomeAsync();
        await RetainsTransactionClaimWhenItsDurableWriteFailsAsync();
        await RetainsUnknownCaptureAsStoppedEvidenceAsync();
        await RechecksFinalSuccessBeforePublicationAsync();
        await RechecksCheckpointAndFinalBudgetAsync();
        await RetainsApprovalClaimAndRejectsPublicationRetryAsync();
        DirectSessionStopCannotBecomePass();
        await RejectsCompetingRunInsteadOfQueueingAsync();
    }

    private static async Task CompletesOneHundredWithoutRetryOrSnapshotReplacementAsync()
    {
        using var fixture = new Fixture();
        var workflow = fixture.Workflow();
        var snapshot = fixture.ReadySnapshot;
        var result = await fixture.Coordinator(workflow).RunAsync(fixture.HundredRunId, fixture.ApprovalId, snapshot);

        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Completed, result.Status);
        Check.Equal(100, result.AttemptedCount);
        Check.Equal(100, workflow.CaptureCalls);
        Check.Equal(0, workflow.RecoverCalls);
        Check.True(workflow.Snapshots.All(item => ReferenceEquals(snapshot, item)), "Every fake dispatch must receive the original identity snapshot instance.");
        Check.True(workflow.ApprovalWasClaimedAtDispatch, "The approval must be consumed before the first dispatch.");
        Check.True(result.EvidenceFiles is not null && File.Exists(result.EvidenceFiles.SummaryPath), "A completed software-only run must publish aggregate evidence.");
        Check.True(File.Exists(Path.Combine(result.ProgressDirectory!, "attempt-100.json")), "The last successful attempt must be durable before publication.");
        using var summary = JsonDocument.Parse(File.ReadAllText(result.EvidenceFiles!.SummaryPath));
        Check.Equal(100, summary.RootElement.GetProperty("requestedCount").GetInt32());
        Check.False(summary.RootElement.GetProperty("hardwareExecutionVerified").GetBoolean(), "Software-only aggregate evidence must not assert hardware execution.");
        Check.Equal(100, File.ReadLines(result.EvidenceFiles.TransactionEventsPath).Count());
    }

    private static async Task RejectsApprovalProblemsBeforeDispatchAsync()
    {
        using (var missing = new Fixture(createApproval: false))
        {
            var workflow = missing.Workflow();
            var result = await missing.Coordinator(workflow).RunAsync(missing.HundredRunId, null, missing.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, result.Status);
            Check.Equal(0, workflow.CaptureCalls);
        }

        using (var mismatch = new Fixture(approvalTargetOverride: "run-ffffffffffffffffffffffffffffffff"))
        {
            var workflow = mismatch.Workflow();
            var result = await mismatch.Coordinator(workflow).RunAsync(mismatch.HundredRunId, mismatch.ApprovalId, mismatch.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, result.Status);
            Check.Equal(0, workflow.CaptureCalls);
        }

        using var tampered = new Fixture();
        File.AppendAllText(Path.Combine(tampered.EvidenceRoot, "approvals", tampered.ApprovalId + ".decision.json"), " ");
        var tamperedWorkflow = tampered.Workflow();
        var tamperedResult = await tampered.Coordinator(tamperedWorkflow).RunAsync(tampered.HundredRunId, tampered.ApprovalId, tampered.ReadySnapshot);
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, tamperedResult.Status);
        Check.Equal(0, tamperedWorkflow.CaptureCalls);
    }

    private static async Task StopsAtFirstFailureAndPreservesCamAAsync()
    {
        using var fixture = new Fixture();
        var workflow = fixture.Workflow(call => call == 3 ? fixture.Outcome(call, succeeded: false) : fixture.Outcome(call, succeeded: true));
        var result = await fixture.Coordinator(workflow).RunAsync(fixture.HundredRunId, fixture.ApprovalId, fixture.ReadySnapshot);

        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Failed, result.Status);
        Check.Equal(3, result.AttemptedCount);
        Check.Equal(3, workflow.CaptureCalls);
        Check.Equal(0, workflow.RecoverCalls);
        Check.Equal(1, result.LastOutcome!.Originals.Count);
        Check.Equal("CAM-A", result.LastOutcome.Originals[0].Alias);
        Check.True(File.ReadAllText(result.EvidenceFiles!.ReportPath).Contains("SoftwareAggregateFail", StringComparison.Ordinal), "The first CAM-B failure must publish a software aggregate failure.");
        Check.True(File.Exists(Path.Combine(fixture.EvidenceRoot, "approvals", fixture.ApprovalId + ".used")), "Approval consumption must survive even after the first failure.");

        var second = await fixture.Coordinator(fixture.Workflow()).RunAsync("run-cccccccccccccccccccccccccccccccc", fixture.ApprovalId, fixture.ReadySnapshot);
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, second.Status);
    }

    private static async Task RejectsChangedTenRunAndApprovalTimeAsync()
    {
        foreach (var file in new[] { "report.md", "summary.json", "transaction-events.jsonl" })
        {
            using var fixture = new Fixture();
            File.AppendAllText(Path.Combine(fixture.EvidenceRoot, "runs", fixture.TenRunId, file), " ");
            var workflow = fixture.Workflow();
            var result = await fixture.Coordinator(workflow).RunAsync(fixture.HundredRunId, fixture.ApprovalId, fixture.ReadySnapshot);
            Check.Equal(0, workflow.CaptureCalls);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, result.Status);
        }

        using var futureApproval = new Fixture();
        futureApproval.Time.Advance(TimeSpan.FromSeconds(-2));
        var futureWorkflow = futureApproval.Workflow();
        var futureResult = await futureApproval.Coordinator(futureWorkflow).RunAsync(
            futureApproval.HundredRunId, futureApproval.ApprovalId, futureApproval.ReadySnapshot);
        Check.Equal(0, futureWorkflow.CaptureCalls);
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, futureResult.Status);
    }

    private static async Task PersistsInterruptedOutcomeWithoutRecoveryAsync()
    {
        using var fixture = new Fixture();
        var workflow = fixture.Workflow(_ => fixture.Outcome(1, succeeded: false, pending: true));
        var result = await fixture.Coordinator(workflow).RunAsync(fixture.HundredRunId, fixture.ApprovalId, fixture.ReadySnapshot);
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.HardwarePending, result.Status);
        Check.Equal(1, workflow.CaptureCalls);
        Check.Equal(0, workflow.RecoverCalls);
        Check.True(File.Exists(Path.Combine(result.ProgressDirectory!, "attempt-001.interrupted.json")), "A pending attempt must be retained as non-aggregate interruption evidence.");
        Check.True(File.ReadAllText(Path.Combine(result.ProgressDirectory!, "stopped.json")).Contains("pending_or_invalid_binding", StringComparison.Ordinal), "The durable stop record must state the fixed reason.");

        using var invalidated = new Fixture();
        var invalidWorkflow = invalidated.Workflow(_ => invalidated.Outcome(1, succeeded: true) with { BindingInvalidationReason = DualBindingInvalidationReason.UsbReconnect });
        var invalidResult = await invalidated.Coordinator(invalidWorkflow).RunAsync(invalidated.HundredRunId, invalidated.ApprovalId, invalidated.ReadySnapshot);
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.HardwarePending, invalidResult.Status);
        Check.Equal(0, invalidWorkflow.RecoverCalls);
    }

    private static async Task RejectsUnsafeStartAndFrozenBudgetAsync()
    {
        foreach (var budget in new[] { TimeSpan.FromSeconds(180), TimeSpan.FromSeconds(601) })
        {
            using var fixture = new Fixture();
            var workflow = fixture.Workflow();
            var result = await fixture.Coordinator(workflow, budget).RunAsync(fixture.HundredRunId, fixture.ApprovalId, fixture.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, result.Status);
            Check.Equal(0, workflow.CaptureCalls);
        }

        using (var expired = new Fixture())
        {
            var workflow = expired.Workflow();
            var stale = new DualCameraIdentitySnapshot(DualCameraIdentityStatus.Ready, "test", expired.Time.Now.AddMinutes(-1), expired.Time.Now);
            var result = await expired.Coordinator(workflow).RunAsync(expired.HundredRunId, expired.ApprovalId, stale);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.HardwarePending, result.Status);
            Check.Equal(0, workflow.CaptureCalls);
        }

        using (var pending = new Fixture())
        {
            var workflow = pending.Workflow(); workflow.SetPending(Guid.NewGuid());
            var result = await pending.Coordinator(workflow).RunAsync(pending.HundredRunId, pending.ApprovalId, pending.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.HardwarePending, result.Status); Check.Equal(0, workflow.CaptureCalls);
        }
        using (var unavailable = new Fixture())
        {
            var workflow = unavailable.Workflow(); workflow.CanStartNewCapture = false;
            var result = await unavailable.Coordinator(workflow).RunAsync(unavailable.HundredRunId, unavailable.ApprovalId, unavailable.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, result.Status); Check.Equal(0, workflow.CaptureCalls);
        }
        using (var unbound = new Fixture())
        {
            var workflow = unbound.Workflow();
            var result = await unbound.Coordinator(workflow, bindingIsCurrent: () => false).RunAsync(unbound.HundredRunId, unbound.ApprovalId, unbound.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.HardwarePending, result.Status); Check.Equal(0, workflow.CaptureCalls);
        }
        using (var cancelled = new Fixture())
        {
            var workflow = cancelled.Workflow();
            var result = await cancelled.Coordinator(workflow).RunAsync(cancelled.HundredRunId, cancelled.ApprovalId, cancelled.ReadySnapshot, new CancellationToken(canceled: true));
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, result.Status); Check.Equal(0, workflow.CaptureCalls);
        }

        using var frozen = new Fixture();
        var frozenWorkflow = frozen.Workflow(call =>
        {
            frozen.Time.Advance(TimeSpan.FromSeconds(call == 1 ? 421 : 1));
            return frozen.Outcome(call, succeeded: true);
        });
        var remainingReads = 0;
        var frozenResult = await frozen.Coordinator(frozenWorkflow, remaining: () => ++remainingReads == 1 ? TimeSpan.FromSeconds(600) : TimeSpan.FromDays(1)).RunAsync(frozen.HundredRunId, frozen.ApprovalId, frozen.ReadySnapshot);
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, frozenResult.Status);
        Check.Equal(1, frozenWorkflow.CaptureCalls);
        Check.True(File.ReadAllText(Path.Combine(frozenResult.ProgressDirectory!, "stopped.json")).Contains("host_budget_insufficient", StringComparison.Ordinal), "A budget stop must be durable and fixed.");
    }

    private static async Task StopsAfterEvidenceFailureOrInvalidOutcomeAsync()
    {
        using (var before = new Fixture(failWrite: "attempt-001.start.json"))
        {
            var workflow = before.Workflow();
            var result = await before.Coordinator(workflow).RunAsync(before.HundredRunId, before.ApprovalId, before.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, result.Status);
            Check.Equal(0, workflow.CaptureCalls);
        }

        using (var after = new Fixture(failWrite: "attempt-001.json"))
        {
            var workflow = after.Workflow();
            var result = await after.Coordinator(workflow).RunAsync(after.HundredRunId, after.ApprovalId, after.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Failed, result.Status);
            Check.Equal(1, workflow.CaptureCalls);
            Check.Equal(0, workflow.RecoverCalls);
        }

        using var invalid = new Fixture();
        var duplicate = Guid.NewGuid();
        var invalidWorkflow = invalid.Workflow(call => call == 2
            ? invalid.Outcome(call, succeeded: true, transactionId: duplicate) with { Originals = invalid.Outcome(call, true, duplicate).Originals.Select(item => item with { Sha256 = "not-a-sha" }).ToArray() }
            : invalid.Outcome(call, true, duplicate));
        var invalidResult = await invalid.Coordinator(invalidWorkflow).RunAsync(invalid.HundredRunId, invalid.ApprovalId, invalid.ReadySnapshot);
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Failed, invalidResult.Status);
        Check.Equal(2, invalidWorkflow.CaptureCalls);
        Check.Equal(0, invalidWorkflow.RecoverCalls);

        using (var duplicateOnly = new Fixture())
        {
            var id = Guid.NewGuid(); var workflow = duplicateOnly.Workflow(call => duplicateOnly.Outcome(call, true, id));
            var result = await duplicateOnly.Coordinator(workflow).RunAsync(duplicateOnly.HundredRunId, duplicateOnly.ApprovalId, duplicateOnly.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Failed, result.Status); Check.Equal(2, workflow.CaptureCalls);
        }
        using (var badPath = new Fixture())
        {
            var workflow = badPath.Workflow(call => call == 2 ? badPath.Outcome(call, true) with { TransactionDirectory = Path.Combine(badPath.TransactionRoot, "wrong") } : badPath.Outcome(call, true));
            var result = await badPath.Coordinator(workflow).RunAsync(badPath.HundredRunId, badPath.ApprovalId, badPath.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Failed, result.Status); Check.Equal(2, workflow.CaptureCalls);
        }
        using (var badTime = new Fixture())
        {
            var workflow = badTime.Workflow(call => { if (call == 2) badTime.Time.Advance(TimeSpan.FromSeconds(-2)); return badTime.Outcome(call, true); });
            var result = await badTime.Coordinator(workflow).RunAsync(badTime.HundredRunId, badTime.ApprovalId, badTime.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Failed, result.Status); Check.Equal(2, workflow.CaptureCalls);
        }
    }

    private static async Task RetainsUnknownCaptureAsStoppedEvidenceAsync()
    {
        using var fixture = new Fixture();
        var pendingId = Guid.NewGuid();
        FakeWorkflow? workflow = null;
        workflow = fixture.Workflow((Func<int, HardwareDualCaptureRecoveryOnlyExecution>)(_ =>
        {
            workflow!.SetPending(pendingId);
            throw new IOException("synthetic unknown outcome");
        }));
        var result = await fixture.Coordinator(workflow).RunAsync(fixture.HundredRunId, fixture.ApprovalId, fixture.ReadySnapshot);
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.HardwarePending, result.Status);
        Check.Equal(1, workflow.CaptureCalls);
        Check.Equal(0, workflow.RecoverCalls);
        Check.True(File.Exists(Path.Combine(result.ProgressDirectory!, "attempt-001.start.json")), "The durable pre-dispatch marker must remain for an unknown outcome.");
        Check.True(File.ReadAllText(Path.Combine(result.ProgressDirectory!, "stopped.json")).Contains("capture_outcome_unknown", StringComparison.Ordinal), "Unknown outcomes must be durably stopped, never retried.");
        using var stopped = JsonDocument.Parse(File.ReadAllText(Path.Combine(result.ProgressDirectory!, "stopped.json")));
        Check.Equal(pendingId.ToString("N"), stopped.RootElement.GetProperty("pendingTransactionId").GetString() ?? string.Empty);
        Check.Equal(fixture.Time.Now, stopped.RootElement.GetProperty("pendingAttemptStartedAtUtc").GetDateTimeOffset());
        var retry = await fixture.Coordinator(workflow).RunAsync("run-dddddddddddddddddddddddddddddddd", fixture.ApprovalId, fixture.ReadySnapshot);
        Check.Equal(0, retry.AttemptedCount);
        Check.Equal(1, workflow.CaptureCalls);
    }

    private static async Task RetainsTransactionClaimWhenItsDurableWriteFailsAsync()
    {
        using var fixture = new Fixture(failClaim: true);
        var transactionId = Guid.NewGuid();
        fixture.ClaimFaultEnabled = true;
        var workflow = fixture.Workflow(call => fixture.Outcome(call, true, transactionId));
        var result = await fixture.Coordinator(workflow).RunAsync(fixture.HundredRunId, fixture.ApprovalId, fixture.ReadySnapshot);
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Failed, result.Status);
        Check.Equal(1, workflow.CaptureCalls);
        Check.True(File.Exists(Path.Combine(fixture.EvidenceRoot, "approvals", fixture.ApprovalId + ".used")), "A claim-write failure must not release the consumed approval.");
        Check.True(File.Exists(Path.Combine(fixture.EvidenceRoot, "transactions", "transaction-" + transactionId.ToString("N") + ".claim")), "An ambiguous transaction claim must remain reserved.");

        fixture.ClaimFaultEnabled = false;
        var duplicateAttempts = Enumerable.Range(1, 10).Select(number => new CaptureRecoveryOnlyRunAttempt(
            fixture.Time.Now.AddMinutes(number), fixture.Time.Now.AddMinutes(number).AddSeconds(1),
            fixture.Outcome(number, true, number == 1 ? transactionId : null))).ToArray();
        Check.Throws<InvalidDataException>(() => fixture.Writer.CreateAndPublish(new CaptureRecoveryOnlyRunEvidenceRequest(
            "run-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 10, duplicateAttempts)));
    }

    private static async Task RechecksFinalSuccessBeforePublicationAsync()
    {
        using (var binding = new Fixture())
        {
            var bindingCurrent = true;
            var workflow = binding.Workflow(call =>
            {
                if (call == 100) bindingCurrent = false;
                return binding.Outcome(call, true);
            });
            var result = await binding.Coordinator(workflow, bindingIsCurrent: () => bindingCurrent).RunAsync(binding.HundredRunId, binding.ApprovalId, binding.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.HardwarePending, result.Status);
            Check.Equal(100, workflow.CaptureCalls);
            Check.True(result.EvidenceFiles is null && File.Exists(Path.Combine(result.ProgressDirectory!, "attempt-100.json")), "A stale binding after the final capture must retain evidence but prevent aggregate publication.");
            Check.True(File.ReadAllText(Path.Combine(result.ProgressDirectory!, "stopped.json")).Contains("binding_invalid", StringComparison.Ordinal), "The final binding stop must be durable.");
        }

        using var pending = new Fixture();
        var pendingWorkflow = pending.Workflow(call => pending.Outcome(call, true));
        pendingWorkflow.ForcePendingAfterCaptureCall = 100;
        var pendingResult = await pending.Coordinator(pendingWorkflow).RunAsync(pending.HundredRunId, pending.ApprovalId, pending.ReadySnapshot);
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.HardwarePending, pendingResult.Status);
        Check.Equal(100, pendingWorkflow.CaptureCalls);
        Check.True(pendingResult.EvidenceFiles is null && File.Exists(Path.Combine(pendingResult.ProgressDirectory!, "attempt-100.json")), "A same-ID pending state after the final capture must prevent aggregate publication.");
    }

    private static void DirectSessionStopCannotBecomePass()
    {
        using (var fixture = new Fixture())
        {
            var session = FillOneHundredSuccesses(fixture);
            session.Stop("binding_invalid", null);
            Check.Throws<InvalidDataException>(() => session.Publish());
            Check.False(Directory.Exists(Path.Combine(fixture.EvidenceRoot, "runs", fixture.HundredRunId)), "A direct stopped session must not publish a pass summary.");
        }

        using var failedStop = new Fixture(failWrite: "stopped.json");
        var failedSession = FillOneHundredSuccesses(failedStop);
        Check.Throws<IOException>(() => failedSession.Stop("cancelled", null));
        Check.Throws<InvalidDataException>(() => failedSession.Publish());
        Check.False(Directory.Exists(Path.Combine(failedStop.EvidenceRoot, "runs", failedStop.HundredRunId)), "A stop write fault must remain a no-publication boundary.");
    }

    private static async Task RechecksCheckpointAndFinalBudgetAsync()
    {
        foreach (var reason in new[] { "cancelled", "binding_invalid", "host_budget_insufficient" })
        {
            using var fixture = new Fixture();
            using var cancellation = new CancellationTokenSource();
            var bindingCurrent = true;
            fixture.WriteAction = name =>
            {
                if (name != "attempt-001.start.json") return;
                if (reason == "cancelled") cancellation.Cancel();
                else if (reason == "binding_invalid") bindingCurrent = false;
                else fixture.Time.Advance(TimeSpan.FromSeconds(421));
            };
            var workflow = fixture.Workflow();
            var result = await fixture.Coordinator(workflow, bindingIsCurrent: () => bindingCurrent).RunAsync(
                fixture.HundredRunId, fixture.ApprovalId, fixture.ReadySnapshot, cancellation.Token);
            Check.Equal(0, workflow.CaptureCalls);
            Check.Equal(reason, result.Detail);
            Check.True(File.Exists(Path.Combine(result.ProgressDirectory!, "attempt-001.start.json")), "Pre-dispatch checkpoint must survive a changed gate.");
            using var stop = JsonDocument.Parse(File.ReadAllText(Path.Combine(result.ProgressDirectory!, "stopped.json")));
            Check.Equal(reason, stop.RootElement.GetProperty("fixedReasonCode").GetString() ?? string.Empty);
        }

        using var expiredAtCompletion = new Fixture();
        var remaining = TimeSpan.FromSeconds(600);
        var finalWorkflow = expiredAtCompletion.Workflow(call =>
        {
            if (call == 100) remaining = TimeSpan.Zero;
            return expiredAtCompletion.Outcome(call, true);
        });
        var final = await expiredAtCompletion.Coordinator(finalWorkflow, remaining: () => remaining).RunAsync(
            expiredAtCompletion.HundredRunId, expiredAtCompletion.ApprovalId, expiredAtCompletion.ReadySnapshot);
        Check.Equal(100, finalWorkflow.CaptureCalls);
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, final.Status);
        Check.True(final.EvidenceFiles is null, "An expired final budget cannot become aggregate Pass.");
        Check.True(File.Exists(Path.Combine(final.ProgressDirectory!, "attempt-100.json")), "Final original metadata must remain after budget expiry.");
    }

    private static async Task RetainsApprovalClaimAndRejectsPublicationRetryAsync()
    {
        using (var fixture = new Fixture())
        {
            fixture.ClaimAction = name => { if (name.EndsWith(".used", StringComparison.Ordinal)) throw new IOException("synthetic approval flush failure"); };
            var workflow = fixture.Workflow();
            var result = await fixture.Coordinator(workflow).RunAsync(fixture.HundredRunId, fixture.ApprovalId, fixture.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, result.Status);
            Check.Equal(0, workflow.CaptureCalls);
            Check.True(File.Exists(Path.Combine(fixture.EvidenceRoot, "approvals", fixture.ApprovalId + ".used")), "An uncertain approval flush retains its tombstone.");
            fixture.ClaimAction = null;
            var reused = await fixture.Coordinator(workflow).RunAsync(fixture.HundredRunId, fixture.ApprovalId, fixture.ReadySnapshot);
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, reused.Status);
            Check.Equal(0, workflow.CaptureCalls);
        }

        using var publication = new Fixture();
        var session = FillOneHundredSuccesses(publication);
        publication.WriteAction = name => { if (name == "report.md") throw new IOException("synthetic publication failure"); };
        Check.Throws<IOException>(() => session.Publish());
        publication.WriteAction = null;
        Check.Throws<InvalidDataException>(() => session.Publish());
        Check.False(Directory.Exists(Path.Combine(publication.EvidenceRoot, "runs", publication.HundredRunId)), "A failed publication must not be silently retried.");
        Check.True(File.Exists(Path.Combine(session.ProgressDirectory, "attempt-100.json")), "Publication failure must retain all completed progress.");
    }

    private static CaptureRecoveryOnlyHundredRunEvidenceSession FillOneHundredSuccesses(Fixture fixture)
    {
        var start = fixture.Time.Now;
        var session = fixture.Writer.BeginHundredRun(fixture.HundredRunId, fixture.ApprovalId, start);
        for (var number = 1; number <= 100; number++)
        {
            session.PrepareAttempt(start);
            fixture.Time.Advance(TimeSpan.FromSeconds(1));
            session.RecordAttempt(new CaptureRecoveryOnlyRunAttempt(start, fixture.Time.Now, fixture.Outcome(number, true)));
            start = fixture.Time.Now;
        }
        return session;
    }

    private static async Task RejectsCompetingRunInsteadOfQueueingAsync()
    {
        using var fixture = new Fixture();
        using var competing = new Fixture();
        using var competingCancellation = new CancellationTokenSource();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var workflow = fixture.Workflow(async call =>
        {
            if (call == 1) { entered.SetResult(); await release.Task; }
            return fixture.Outcome(call, true);
        });
        var first = fixture.Coordinator(workflow).RunAsync(fixture.HundredRunId, fixture.ApprovalId, fixture.ReadySnapshot);
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(10));
            // A second valid approval must be rejected by the workflow gate,
            // not incidentally by an approval target mismatch or existing claim.
            var second = await competing.Coordinator(workflow).RunAsync(
                competing.HundredRunId, competing.ApprovalId, competing.ReadySnapshot,
                competingCancellation.Token).WaitAsync(TimeSpan.FromSeconds(10));
            Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Blocked, second.Status);
            Check.Equal("run_already_active", second.Detail);
            Check.Equal(0, second.AttemptedCount);
            Check.Equal(1, workflow.CaptureCalls);
            Check.False(File.Exists(Path.Combine(competing.EvidenceRoot, "approvals", competing.ApprovalId + ".used")), "A competing valid run must be rejected before claiming its approval.");
        }
        finally { competingCancellation.Cancel(); release.TrySetResult(); }
        var completed = await first.WaitAsync(TimeSpan.FromSeconds(30));
        Check.Equal(CaptureRecoveryOnlyHundredRunStatus.Completed, completed.Status);
    }

    private sealed class Fixture : IDisposable
    {
        private readonly string? _failWrite;
        private readonly bool _failClaim;
        internal Fixture(bool createApproval = true, string? approvalTargetOverride = null, string? failWrite = null, bool failClaim = false)
        {
            _failWrite = failWrite; _failClaim = failClaim;
            Root = Path.Combine(Path.GetTempPath(), "a0-hundred-core-" + Guid.NewGuid().ToString("N"));
            EvidenceRoot = Path.Combine(Root, "evidence"); TransactionRoot = Path.Combine(Root, "transactions");
            Directory.CreateDirectory(Root); Directory.CreateDirectory(TransactionRoot);
            Time = new TestTimeProvider(DateTimeOffset.Parse("2026-09-12T12:00:00Z"));
            TenRunId = "run-" + Guid.NewGuid().ToString("N"); HundredRunId = "run-" + Guid.NewGuid().ToString("N");
            Writer = new CaptureRecoveryOnlyRunEvidenceWriter(EvidenceRoot, () => Time.Now,
                name => { WriteAction?.Invoke(name); if (_failWrite == name) throw new IOException("synthetic write failure"); },
                beforeClaimFlush: name => { ClaimAction?.Invoke(name); if (_failClaim && ClaimFaultEnabled && name.StartsWith("transaction-", StringComparison.Ordinal)) throw new IOException("synthetic claim flush failure"); });
            var seed = Enumerable.Range(1, 10).Select(number => new CaptureRecoveryOnlyRunAttempt(Time.Now.AddMinutes(-30 + number), Time.Now.AddMinutes(-30 + number).AddSeconds(1), SeedOutcome(number))).ToArray();
            var files = Writer.CreateAndPublish(new CaptureRecoveryOnlyRunEvidenceRequest(TenRunId, 10, seed));
            ApprovalId = "approval-" + TenRunId[4..];
            if (createApproval)
            {
                var external = Path.Combine(Root, "approval.json");
                WriteApproval(external, TenRunId, approvalTargetOverride ?? HundredRunId, files, Time.Now.AddSeconds(-1));
                Writer.RecordP95Approval(external);
            }
        }

        internal string Root { get; }
        internal string EvidenceRoot { get; }
        internal string TransactionRoot { get; }
        internal string TenRunId { get; }
        internal string HundredRunId { get; }
        internal string ApprovalId { get; }
        internal TestTimeProvider Time { get; }
        internal CaptureRecoveryOnlyRunEvidenceWriter Writer { get; }
        internal bool ClaimFaultEnabled { get; set; }
        internal Action<string>? WriteAction { get; set; }
        internal Action<string>? ClaimAction { get; set; }
        internal DualCameraIdentitySnapshot ReadySnapshot => DualCameraIdentitySnapshot.AnonymousTestSyntheticReady();

        internal FakeWorkflow Workflow(Func<int, HardwareDualCaptureRecoveryOnlyExecution>? outcomes = null) => new(TransactionRoot, this, outcomes, () => File.Exists(Path.Combine(EvidenceRoot, "approvals", ApprovalId + ".used")));
        internal FakeWorkflow Workflow(Func<int, Task<HardwareDualCaptureRecoveryOnlyExecution>> outcomes) => new(TransactionRoot, this, outcomes, () => File.Exists(Path.Combine(EvidenceRoot, "approvals", ApprovalId + ".used")));
        internal CaptureRecoveryOnlyHundredRunCoordinator Coordinator(FakeWorkflow workflow, TimeSpan? budget = null, Func<TimeSpan>? remaining = null, Func<bool>? bindingIsCurrent = null) =>
            new(workflow, Writer, bindingIsCurrent ?? (() => true), remaining ?? (() => budget ?? TimeSpan.FromSeconds(600)), Time);

        internal HardwareDualCaptureRecoveryOnlyExecution Outcome(int number, bool succeeded, Guid? transactionId = null, bool pending = false)
        {
            var id = transactionId ?? Guid.NewGuid(); var directory = Path.Combine(TransactionRoot, id.ToString("N"));
            var originals = succeeded ? new CanonicalJpegOriginal[] { Original(directory, "CAM-A", 'a'), Original(directory, "CAM-B", 'b') } : new CanonicalJpegOriginal[] { Original(directory, "CAM-A", 'a') };
            return new(id, pending ? DualHardwareCaptureTerminalState.HardwarePending : succeeded ? DualHardwareCaptureTerminalState.Succeeded : DualHardwareCaptureTerminalState.FailedPartial,
                pending ? DualCameraFailureCode.None : succeeded ? DualCameraFailureCode.None : DualCameraFailureCode.CaptureCameraB,
                null, originals, directory, pending, 0);
        }

        private HardwareDualCaptureRecoveryOnlyExecution SeedOutcome(int number)
        {
            var id = Guid.NewGuid(); var directory = Path.Combine("C:\\synthetic-hundred-seed", id.ToString("N"));
            return new(id, DualHardwareCaptureTerminalState.Succeeded, DualCameraFailureCode.None, null,
                [Original(directory, "CAM-A", 'a'), Original(directory, "CAM-B", 'b')], directory, false, 0);
        }

        private static CanonicalJpegOriginal Original(string directory, string alias, char hash) => new(alias, Path.Combine(directory, alias, "original.jpg"), alias == "CAM-A" ? 100 : 101, new string(hash, 64), 7360, 4912, true);

        private static void WriteApproval(string path, string tenRunId, string hundredRunId, CaptureRecoveryOnlyRunEvidenceFiles files, DateTimeOffset approvedAtUtc)
        {
            using var summary = JsonDocument.Parse(File.ReadAllText(files.SummaryPath));
            var hashes = new CaptureRecoveryOnlyRunFileHashes(Hash(files.ReportPath), Hash(files.SummaryPath), Hash(files.TransactionEventsPath), summary.RootElement.GetProperty("p95Milliseconds").GetDouble());
            var payload = new { approvalSchema = "a0.capture-recovery-only.p95-approval.v1", evidenceScope = "SoftwareAggregationOnly", hardwareExecutionVerified = false, productionRunner = false, capturePurpose = HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose, stitchOutcome = HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome, a0QualityApproval = HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval, approvalAuthorityVerifiedBySoftware = false, decision = "Approved", approverRole = "ProductOwner", approvalBasis = "operator-approved-p95-v1", tenRunId, targetHundredRunId = hundredRunId, p95Milliseconds = hashes.P95Milliseconds, approvedAtUtc, tenRunCompletedAtUtc = summary.RootElement.GetProperty("completedAtUtc").GetDateTimeOffset(), tenRunFiles = hashes };
            File.WriteAllText(path, JsonSerializer.Serialize(payload, new JsonSerializerOptions(JsonSerializerDefaults.Web) { WriteIndented = true }), new UTF8Encoding(false));
        }

        private static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
        public void Dispose() { if (Directory.Exists(Root)) Directory.Delete(Root, true); }
    }

    private sealed class FakeWorkflow : IHardwareDualCaptureRecoveryOnlyWorkflow
    {
        private readonly Fixture _fixture; private readonly Func<int, HardwareDualCaptureRecoveryOnlyExecution>? _outcomes; private readonly Func<int, Task<HardwareDualCaptureRecoveryOnlyExecution>>? _asyncOutcomes; private readonly Func<bool> _approvalClaimed;
        internal FakeWorkflow(string root, Fixture fixture, Func<int, HardwareDualCaptureRecoveryOnlyExecution>? outcomes, Func<bool> approvalClaimed) { TransactionRoot = root; _fixture = fixture; _outcomes = outcomes; _approvalClaimed = approvalClaimed; }
        internal FakeWorkflow(string root, Fixture fixture, Func<int, Task<HardwareDualCaptureRecoveryOnlyExecution>> outcomes, Func<bool> approvalClaimed) { TransactionRoot = root; _fixture = fixture; _asyncOutcomes = outcomes; _approvalClaimed = approvalClaimed; }
        public string TransactionRoot { get; } public bool CanStartNewCapture { get; set; } = true; public string NewCaptureBlocker => ""; public bool HasPendingRecovery { get; private set; } public Guid? PendingTransactionId { get; private set; }
        internal int CaptureCalls { get; private set; } internal int RecoverCalls { get; private set; } internal int? ForcePendingAfterCaptureCall { get; set; } internal bool ApprovalWasClaimedAtDispatch { get; private set; } internal List<DualCameraIdentitySnapshot> Snapshots { get; } = [];
        public async Task<HardwareDualCaptureRecoveryOnlyExecution> CaptureAsync(DualCameraIdentitySnapshot identitySnapshot, CancellationToken cancellationToken = default)
        {
            Snapshots.Add(identitySnapshot); CaptureCalls++; ApprovalWasClaimedAtDispatch = _approvalClaimed(); if (!ApprovalWasClaimedAtDispatch) throw new InvalidOperationException("Dispatch occurred before approval was claimed."); var outcome = _asyncOutcomes is not null ? await _asyncOutcomes(CaptureCalls) : _outcomes?.Invoke(CaptureCalls) ?? _fixture.Outcome(CaptureCalls, true); _fixture.Time.Advance(TimeSpan.FromSeconds(1));
            HasPendingRecovery = outcome.RecoveryPending || ForcePendingAfterCaptureCall == CaptureCalls; PendingTransactionId = HasPendingRecovery ? outcome.TransactionId : null; return outcome;
        }
        public Task<HardwareDualCaptureRecoveryOnlyExecution> RecoverAsync(CancellationToken cancellationToken = default) { RecoverCalls++; throw new InvalidOperationException("Coordinator must not recover."); }
        internal void SetPending(Guid transactionId) { HasPendingRecovery = true; PendingTransactionId = transactionId; }
    }

    private sealed class TestTimeProvider(DateTimeOffset now) : TimeProvider
    {
        private long _timestamp; internal DateTimeOffset Now { get; private set; } = now;
        public override DateTimeOffset GetUtcNow() => Now; public override long GetTimestamp() => _timestamp; public override long TimestampFrequency => TimeSpan.TicksPerSecond;
        internal void Advance(TimeSpan elapsed) { Now += elapsed; _timestamp += elapsed.Ticks; }
    }
}
