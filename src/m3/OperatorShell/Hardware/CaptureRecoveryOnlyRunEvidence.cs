using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using A0CameraStitcher.M3.Foundation.DualCamera;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

/// <summary>
/// Writes anonymous, software-only summaries of already completed CaptureRecoveryOnly
/// outcomes. It has no Agent, camera, SDK, or capture dependency.
/// </summary>
internal sealed class CaptureRecoveryOnlyRunEvidenceWriter
{
    private const string Scope = "SoftwareAggregationOnly";
    private const string ApprovalSchema = "a0.capture-recovery-only.p95-approval.v1";
    private const string ApprovalBasis = "operator-approved-p95-v1";
    private const string OperatorRecordedApproval = "OperatorRecordedApproval";
    private const long MaximumEvidenceFileBytes = 4 * 1024 * 1024;
    private const long MaximumApprovalFileBytes = 64 * 1024;
    internal static readonly Regex RunId = new("^run-[0-9a-f]{32}$", RegexOptions.CultureInvariant);
    private static readonly Regex ApprovalId = new("^approval-[0-9a-f]{32}$", RegexOptions.CultureInvariant);
    internal static readonly Regex Hash = new("^[0-9a-f]{64}$", RegexOptions.CultureInvariant);
    internal static readonly JsonSerializerOptions IndentedJsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
    };
    private static readonly JsonSerializerOptions CompactJsonOptions = new(JsonSerializerDefaults.Web);

    internal readonly string _root;
    private readonly Func<DateTimeOffset> _utcNow;
    private readonly Action<string>? _beforeWrite;
    private readonly Action<string>? _afterPublish;
    private readonly Action<string>? _beforeClaimFlush;

    internal CaptureRecoveryOnlyRunEvidenceWriter(
        string evidenceRoot,
        Func<DateTimeOffset>? utcNow = null,
        Action<string>? beforeWrite = null,
        Action<string>? afterPublish = null,
        Action<string>? beforeClaimFlush = null)
    {
        if (string.IsNullOrWhiteSpace(evidenceRoot))
            throw new ArgumentException("A fixed evidence root is required.", nameof(evidenceRoot));

        _root = Path.GetFullPath(evidenceRoot);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_root);
        _utcNow = utcNow ?? (() => DateTimeOffset.UtcNow);
        _beforeWrite = beforeWrite;
        _afterPublish = afterPublish;
        _beforeClaimFlush = beforeClaimFlush;
    }

    internal CaptureRecoveryOnlyRunEvidenceFiles CreateAndPublish(CaptureRecoveryOnlyRunEvidenceRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateRequest(request);
        EnsureRunDestinationAvailable(request.RunId);

        var attempts = request.Attempts
            .Select((item, index) => ToAttempt(index + 1, item))
            .ToArray();
        var firstFailure = attempts.FirstOrDefault(item => !item.Succeeded);
        if (firstFailure is not null && attempts[^1] != firstFailure)
            throw new InvalidDataException("A run must stop at its first failure.");
        if (attempts.Length < request.RequestedCount && firstFailure is null)
            throw new InvalidDataException("A shortened run requires a recorded failure.");

        CaptureRecoveryOnlyP95Approval? approval = null;
        string? approvalClaim = null;
        IReadOnlyList<string> transactionClaims = Array.Empty<string>();
        try
        {
            if (request.RequestedCount == 100)
            {
                approval = LoadAndValidateApproval(request.ApprovalRecordId, request.RunId, attempts);
                approvalClaim = ClaimApproval(approval.ApprovalRecordId, request.RunId);
            }
            else if (request.ApprovalRecordId is not null)
            {
                throw new InvalidDataException("A 10-run cannot carry a prior approval.");
            }

            transactionClaims = ClaimTransactions(request.RunId, attempts);
            var evidence = new CaptureRecoveryOnlyRunEvidence(
                request.RunId,
                request.RequestedCount,
                attempts,
                firstFailure,
                Percentile50(attempts.Select(item => item.Duration).Order().ToArray()),
                Percentile95(attempts.Select(item => item.Duration).Order().ToArray()),
                attempts.Max(item => item.Duration),
                firstFailure is not null
                    ? CaptureRecoveryOnlyRunVerdict.SoftwareAggregateFail
                    : request.RequestedCount == 100
                        ? CaptureRecoveryOnlyRunVerdict.SoftwareAggregatePass
                        : CaptureRecoveryOnlyRunVerdict.SoftwareAggregatePartial,
                request.RequestedCount == 10 ? "Unapproved" : "OperatorRecorded",
                approval);
            return WriteRun(evidence);
        }
        catch (PublishedEvidenceException)
        {
            throw;
        }
        catch
        {
            foreach (var transactionClaim in transactionClaims)
                TryDeleteOwnedFile(transactionClaim);
            if (approvalClaim is not null)
                TryDeleteOwnedFile(approvalClaim);
            throw;
        }
    }

    /// <summary>
    /// Reserves a previously recorded approval before a 100-run dispatcher can start.
    /// The returned session is deliberately internal: the public command surface must
    /// continue to reject production 100-run execution until its separate gate exists.
    /// </summary>
    internal CaptureRecoveryOnlyHundredRunEvidenceSession BeginHundredRun(
        string runId,
        string? approvalId,
        DateTimeOffset startedAtUtc) =>
        new(this, runId, approvalId, startedAtUtc);

    /// <summary>
    /// Records a separately supplied operator decision. This component validates and
    /// preserves that decision but cannot authenticate the human approver.
    /// </summary>
    internal CaptureRecoveryOnlyP95Approval RecordP95Approval(string externalApprovalPath)
    {
        if (string.IsNullOrWhiteSpace(externalApprovalPath))
            throw new InvalidDataException("An external approval artifact is required.");

        var normalizedInput = Path.GetFullPath(externalApprovalPath);
        if (IsWithinRoot(normalizedInput, _root))
            throw new InvalidDataException("The approval input must be supplied outside the generated evidence root.");

        var externalBytes = ReadRegularLocalFile(normalizedInput, MaximumApprovalFileBytes);
        var decision = ParseExternalApprovalDecision(externalBytes);
        var ten = LoadPublishedTenRun(decision.TenRunId);
        if (decision.P95 != ten.P95 ||
            decision.TenRunCompletedAtUtc != ten.CompletedAtUtc ||
            decision.TenRunFiles != ten.Files ||
            decision.ApprovedAtUtc <= ten.CompletedAtUtc ||
            decision.ApprovedAtUtc > _utcNow())
        {
            throw new InvalidDataException("The operator decision does not bind the published 10-run evidence.");
        }

        var approvalId = "approval-" + decision.TenRunId["run-".Length..];
        var approvalPath = ApprovalPath(approvalId);
        var decisionPath = ApprovalDecisionPath(approvalId);
        if (File.Exists(approvalPath) || File.Exists(decisionPath))
            throw new InvalidDataException("The 10-run already has an approval artifact.");

        var externalHash = HashBytes(externalBytes);
        var approval = new CaptureRecoveryOnlyP95Approval(
            approvalId,
            decision.TenRunId,
            decision.TargetHundredRunId,
            decision.P95,
            decision.ApprovedAtUtc,
            "ProductOwner",
            decision.TenRunCompletedAtUtc,
            decision.TenRunFiles,
            string.Empty,
            externalHash);
        var targetClaim = TargetApprovalClaimPath(decision.TargetHundredRunId);
        var targetClaimCreated = false;
        var decisionPublished = false;
        var approvalPublished = false;
        try
        {
            CreateExclusiveClaim(
                targetClaim,
                approval.ApprovalRecordId,
                "The target 100-run already has an approval artifact.");
            targetClaimCreated = true;
            WriteAtomicBytes(
                Path.GetDirectoryName(decisionPath)!,
                Path.GetFileName(decisionPath),
                externalBytes);
            decisionPublished = true;
            var approvalRecordHash = WriteApproval(approval);
            approvalPublished = true;
            return approval with { ApprovalRecordSha256 = approvalRecordHash };
        }
        catch
        {
            if (!approvalPublished)
            {
                if (decisionPublished)
                    TryDeleteOwnedFile(decisionPath);
                if (targetClaimCreated)
                    TryDeleteOwnedFile(targetClaim);
            }
            throw;
        }
    }

    internal CaptureRecoveryOnlyRunEvidenceFiles WriteRun(CaptureRecoveryOnlyRunEvidence evidence)
    {
        var runs = Path.Combine(_root, "runs");
        EnsureWriteDirectory(runs);
        var destination = Path.Combine(runs, evidence.RunId);
        EnsureRunDestinationAvailable(evidence.RunId);
        var staging = Path.Combine(runs, evidence.RunId + "-" + Guid.NewGuid().ToString("N") + ".partial");
        var published = false;
        try
        {
            Directory.CreateDirectory(staging);
            EnsureWriteDirectory(staging);
            WriteAtomic(staging, "report.md", Report(evidence));
            WriteAtomic(staging, "summary.json", JsonSerializer.Serialize(Summary(evidence), IndentedJsonOptions));
            WriteAtomic(staging, "transaction-events.jsonl", Events(evidence));

            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(staging);
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(runs);
            if (Directory.Exists(destination) || File.Exists(destination))
                throw new IOException("Evidence destination already exists.");

            Directory.Move(staging, destination);
            published = true;
            _afterPublish?.Invoke(evidence.RunId);
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(destination);
            foreach (var name in new[] { "report.md", "summary.json", "transaction-events.jsonl" })
                EnsureRegularLocalFile(Path.Combine(destination, name));

            return new(
                Path.Combine(destination, "report.md"),
                Path.Combine(destination, "summary.json"),
                Path.Combine(destination, "transaction-events.jsonl"));
        }
        catch (Exception exception)
        {
            if (published)
                throw new PublishedEvidenceException("Evidence was published but final verification failed; claims remain held.", exception);
            CleanupStaging(staging);
            throw;
        }
    }

    private string WriteApproval(CaptureRecoveryOnlyP95Approval approval)
    {
        var approvals = Path.Combine(_root, "approvals");
        EnsureWriteDirectory(approvals);
        var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new
        {
            evidenceScope = Scope,
            hardwareExecutionVerified = false,
            productionRunner = false,
            capturePurpose = HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose,
            stitchOutcome = HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome,
            a0QualityApproval = HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval,
            approvalAuthorityVerifiedBySoftware = false,
            approvalStatus = OperatorRecordedApproval,
            approvalBasis = ApprovalBasis,
            approval.ApprovalRecordId,
            approval.TenRunId,
            approval.TargetHundredRunId,
            p95Milliseconds = approval.P95.TotalMilliseconds,
            approval.RecordedAtUtc,
            approverRole = "ProductOwner",
            tenRunCompletedAtUtc = approval.TenRunCompletedAtUtc,
            tenRunFiles = approval.TenRunFiles,
            externalApprovalSha256 = approval.ExternalApprovalSha256,
            externalApprovalFileName = approval.ApprovalRecordId + ".decision.json",
        }, IndentedJsonOptions));
        var hash = HashBytes(bytes);
        WriteAtomicBytes(approvals, approval.ApprovalRecordId + ".json", bytes);
        return hash;
    }

    internal CaptureRecoveryOnlyP95Approval LoadAndValidateApproval(
        string? id,
        string hundredId,
        IReadOnlyList<CaptureRecoveryOnlyRunAttemptEvidence> attempts)
    {
        var approval = LoadAndValidateApproval(id, hundredId, attempts[0].StartedAtUtc);
        var ten = LoadPublishedTenRun(approval.TenRunId);
        if (attempts.Any(item => ten.TransactionIds.Contains(item.TransactionId)))
            throw new InvalidDataException("10-run and 100-run transaction IDs must be disjoint.");
        return approval;
    }

    internal CaptureRecoveryOnlyP95Approval LoadAndValidateApproval(
        string? id,
        string hundredId,
        DateTimeOffset startedAtUtc)
    {
        if (id is null || !ApprovalId.IsMatch(id))
            throw new InvalidDataException("A 100-run requires an approval artifact.");
        if (startedAtUtc.Offset != TimeSpan.Zero)
            throw new InvalidDataException("A 100-run start time must be UTC.");

        var approval = ReadApproval(ApprovalPath(id));
        if (approval.ApprovalRecordId != id ||
            approval.TargetHundredRunId != hundredId ||
            approval.TenRunId == hundredId ||
            approval.RecordedAtUtc >= startedAtUtc)
        {
            throw new InvalidDataException("Approval identity, target, or time ordering is invalid.");
        }

        var ten = LoadPublishedTenRun(approval.TenRunId);
        if (approval.RecordedAtUtc <= ten.CompletedAtUtc ||
            approval.P95 != ten.P95 ||
            approval.TenRunCompletedAtUtc != ten.CompletedAtUtc ||
            approval.TenRunFiles != ten.Files)
        {
            throw new InvalidDataException("Approval does not bind the published 10-run evidence.");
        }

        return approval;
    }

    private CaptureRecoveryOnlyP95Approval ReadApproval(string path)
    {
        var bytes = ReadRegularLocalFile(path, MaximumApprovalFileBytes);
        try
        {
            using var document = JsonDocument.Parse(bytes);
            var root = document.RootElement;
            RequireMarkers(root);
            if (!HasExactProperties(root,
                    "evidenceScope", "hardwareExecutionVerified", "productionRunner",
                    "capturePurpose", "stitchOutcome", "a0QualityApproval",
                    "approvalAuthorityVerifiedBySoftware", "approvalStatus", "approvalBasis",
                    "approvalRecordId", "tenRunId", "targetHundredRunId", "p95Milliseconds",
                    "recordedAtUtc", "approverRole", "tenRunCompletedAtUtc", "tenRunFiles",
                    "externalApprovalSha256", "externalApprovalFileName"))
            {
                throw new InvalidDataException("Approval artifact properties are invalid.");
            }

            var id = root.GetProperty("approvalRecordId").GetString() ?? string.Empty;
            var tenId = root.GetProperty("tenRunId").GetString() ?? string.Empty;
            var target = root.GetProperty("targetHundredRunId").GetString() ?? string.Empty;
            var role = root.GetProperty("approverRole").GetString();
            if (!ApprovalId.IsMatch(id) ||
                !RunId.IsMatch(tenId) ||
                !RunId.IsMatch(target) ||
                tenId == target ||
                id != "approval-" + tenId["run-".Length..] ||
                role != "ProductOwner" ||
                root.GetProperty("approvalAuthorityVerifiedBySoftware").GetBoolean() ||
                root.GetProperty("approvalStatus").GetString() != OperatorRecordedApproval ||
                root.GetProperty("approvalBasis").GetString() != ApprovalBasis ||
                root.GetProperty("externalApprovalFileName").GetString() != id + ".decision.json")
            {
                throw new InvalidDataException("Approval artifact identity is invalid.");
            }

            var files = root.GetProperty("tenRunFiles");
            if (!HasExactProperties(files, "reportSha256", "summarySha256", "eventsSha256", "p95Milliseconds"))
                throw new InvalidDataException("Approval hash properties are invalid.");
            var topLevelP95 = root.GetProperty("p95Milliseconds").GetDouble();
            var nestedP95 = files.GetProperty("p95Milliseconds").GetDouble();
            var reportHash = files.GetProperty("reportSha256").GetString() ?? string.Empty;
            var summaryHash = files.GetProperty("summarySha256").GetString() ?? string.Empty;
            var eventsHash = files.GetProperty("eventsSha256").GetString() ?? string.Empty;
            var externalApprovalHash = root.GetProperty("externalApprovalSha256").GetString() ?? string.Empty;
            var recordedAt = root.GetProperty("recordedAtUtc").GetDateTimeOffset();
            var tenCompletedAt = root.GetProperty("tenRunCompletedAtUtc").GetDateTimeOffset();
            if (!double.IsFinite(topLevelP95) ||
                topLevelP95 < 0 ||
                topLevelP95 != nestedP95 ||
                !Hash.IsMatch(reportHash) ||
                !Hash.IsMatch(summaryHash) ||
                !Hash.IsMatch(eventsHash) ||
                !Hash.IsMatch(externalApprovalHash) ||
                recordedAt.Offset != TimeSpan.Zero ||
                tenCompletedAt.Offset != TimeSpan.Zero)
            {
                throw new InvalidDataException("Approval artifact values are invalid.");
            }

            var targetClaimBytes = ReadRegularLocalFile(TargetApprovalClaimPath(target), 256);
            if (Encoding.UTF8.GetString(targetClaimBytes) != id)
                throw new InvalidDataException("Approval target claim is invalid.");
            var decisionBytes = ReadRegularLocalFile(ApprovalDecisionPath(id), MaximumApprovalFileBytes);
            if (HashBytes(decisionBytes) != externalApprovalHash)
                throw new InvalidDataException("The preserved operator decision hash is invalid.");
            var decision = ParseExternalApprovalDecision(decisionBytes);
            var boundFiles = new CaptureRecoveryOnlyRunFileHashes(
                reportHash,
                summaryHash,
                eventsHash,
                nestedP95);
            if (decision.TenRunId != tenId ||
                decision.TargetHundredRunId != target ||
                decision.P95 != TimeSpan.FromMilliseconds(topLevelP95) ||
                decision.ApprovedAtUtc != recordedAt ||
                decision.TenRunCompletedAtUtc != tenCompletedAt ||
                decision.TenRunFiles != boundFiles)
            {
                throw new InvalidDataException("The approval record does not match the preserved operator decision.");
            }

            return new(
                id,
                tenId,
                target,
                TimeSpan.FromMilliseconds(topLevelP95),
                recordedAt,
                "ProductOwner",
                tenCompletedAt,
                boundFiles,
                HashBytes(bytes),
                externalApprovalHash);
        }
        catch (Exception exception) when (
            exception is JsonException or KeyNotFoundException or InvalidOperationException or FormatException or OverflowException)
        {
            throw new InvalidDataException("Approval artifact is invalid.", exception);
        }
    }

    private static ExternalApprovalDecision ParseExternalApprovalDecision(byte[] bytes)
    {
        try
        {
            using var document = JsonDocument.Parse(bytes);
            var root = document.RootElement;
            RequireMarkers(root);
            if (!HasExactProperties(root,
                    "approvalSchema", "evidenceScope", "hardwareExecutionVerified", "productionRunner",
                    "capturePurpose", "stitchOutcome", "a0QualityApproval",
                    "approvalAuthorityVerifiedBySoftware", "decision", "approverRole", "approvalBasis",
                    "tenRunId", "targetHundredRunId", "p95Milliseconds", "approvedAtUtc",
                    "tenRunCompletedAtUtc", "tenRunFiles"))
            {
                throw new InvalidDataException("External approval properties are invalid.");
            }

            var tenId = root.GetProperty("tenRunId").GetString() ?? string.Empty;
            var target = root.GetProperty("targetHundredRunId").GetString() ?? string.Empty;
            var files = root.GetProperty("tenRunFiles");
            if (!HasExactProperties(files, "reportSha256", "summarySha256", "eventsSha256", "p95Milliseconds"))
                throw new InvalidDataException("External approval hash properties are invalid.");
            var p95Milliseconds = root.GetProperty("p95Milliseconds").GetDouble();
            var nestedP95 = files.GetProperty("p95Milliseconds").GetDouble();
            var approvedAt = root.GetProperty("approvedAtUtc").GetDateTimeOffset();
            var tenCompletedAt = root.GetProperty("tenRunCompletedAtUtc").GetDateTimeOffset();
            var reportHash = files.GetProperty("reportSha256").GetString() ?? string.Empty;
            var summaryHash = files.GetProperty("summarySha256").GetString() ?? string.Empty;
            var eventsHash = files.GetProperty("eventsSha256").GetString() ?? string.Empty;
            if (root.GetProperty("approvalSchema").GetString() != ApprovalSchema ||
                root.GetProperty("approvalAuthorityVerifiedBySoftware").GetBoolean() ||
                root.GetProperty("decision").GetString() != "Approved" ||
                root.GetProperty("approverRole").GetString() != "ProductOwner" ||
                root.GetProperty("approvalBasis").GetString() != ApprovalBasis ||
                !RunId.IsMatch(tenId) ||
                !RunId.IsMatch(target) ||
                tenId == target ||
                !double.IsFinite(p95Milliseconds) ||
                p95Milliseconds < 0 ||
                p95Milliseconds != nestedP95 ||
                approvedAt.Offset != TimeSpan.Zero ||
                tenCompletedAt.Offset != TimeSpan.Zero ||
                approvedAt <= tenCompletedAt ||
                !Hash.IsMatch(reportHash) ||
                !Hash.IsMatch(summaryHash) ||
                !Hash.IsMatch(eventsHash))
            {
                throw new InvalidDataException("External approval values are invalid.");
            }

            return new(
                tenId,
                target,
                TimeSpan.FromMilliseconds(p95Milliseconds),
                approvedAt,
                tenCompletedAt,
                new(reportHash, summaryHash, eventsHash, nestedP95));
        }
        catch (Exception exception) when (
            exception is JsonException or KeyNotFoundException or InvalidOperationException or FormatException or OverflowException)
        {
            throw new InvalidDataException("External approval artifact is invalid.", exception);
        }
    }

    internal PublishedTenRun LoadPublishedTenRun(string runId)
    {
        if (!RunId.IsMatch(runId))
            throw new InvalidDataException("The 10-run ID is invalid.");

        var directory = Path.Combine(_root, "runs", runId);
        var reportPath = Path.Combine(directory, "report.md");
        var summaryPath = Path.Combine(directory, "summary.json");
        var eventsPath = Path.Combine(directory, "transaction-events.jsonl");
        var reportBytes = ReadRegularLocalFile(reportPath, MaximumEvidenceFileBytes);
        var summaryBytes = ReadRegularLocalFile(summaryPath, MaximumEvidenceFileBytes);
        var eventsBytes = ReadRegularLocalFile(eventsPath, MaximumEvidenceFileBytes);
        var files = new CaptureRecoveryOnlyRunFileHashes(
            HashBytes(reportBytes),
            HashBytes(summaryBytes),
            HashBytes(eventsBytes),
            0);

        try
        {
            using var summaryDocument = JsonDocument.Parse(summaryBytes);
            var summary = summaryDocument.RootElement;
            RequireMarkers(summary);
            if (summary.GetProperty("runId").GetString() != runId ||
                summary.GetProperty("requestedCount").GetInt32() != 10 ||
                summary.GetProperty("succeededCount").GetInt32() != 10 ||
                summary.GetProperty("verdict").GetString() != nameof(CaptureRecoveryOnlyRunVerdict.SoftwareAggregatePartial) ||
                summary.GetProperty("p95ApprovalStatus").GetString() != "Unapproved" ||
                summary.GetProperty("automaticRetryCount").GetInt32() != 0 ||
                summary.GetProperty("firstFailureAttempt").ValueKind != JsonValueKind.Null ||
                summary.GetProperty("approval").ValueKind != JsonValueKind.Null)
            {
                throw new InvalidDataException("Published 10-run summary is not a successful software aggregate.");
            }

            var p95Milliseconds = summary.GetProperty("p95Milliseconds").GetDouble();
            var startedAt = summary.GetProperty("startedAtUtc").GetDateTimeOffset();
            var completedAt = summary.GetProperty("completedAtUtc").GetDateTimeOffset();
            if (!double.IsFinite(p95Milliseconds) ||
                p95Milliseconds < 0 ||
                startedAt.Offset != TimeSpan.Zero ||
                completedAt.Offset != TimeSpan.Zero ||
                completedAt < startedAt)
            {
                throw new InvalidDataException("Published 10-run summary timing is invalid.");
            }

            var transactionIds = new HashSet<Guid>();
            var durations = new List<double>();
            DateTimeOffset? firstStartedAt = null;
            DateTimeOffset? previousCompletedAt = null;
            DateTimeOffset? lastCompletedAt = null;
            var lines = Encoding.UTF8.GetString(eventsBytes)
                .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            if (lines.Length != 10)
                throw new InvalidDataException("10-run event count is invalid.");

            for (var index = 0; index < lines.Length; index++)
            {
                using var eventDocument = JsonDocument.Parse(lines[index]);
                var item = eventDocument.RootElement;
                RequireMarkers(item);
                var transactionId = Guid.ParseExact(
                    item.GetProperty("transactionId").GetString() ?? string.Empty,
                    "N");
                var eventStartedAt = item.GetProperty("startedAtUtc").GetDateTimeOffset();
                var eventCompletedAt = item.GetProperty("completedAtUtc").GetDateTimeOffset();
                var durationMilliseconds = item.GetProperty("durationMilliseconds").GetDouble();
                if (item.GetProperty("attemptNumber").GetInt32() != index + 1 ||
                    !item.GetProperty("succeeded").GetBoolean() ||
                    item.GetProperty("terminalState").GetString() != nameof(DualHardwareCaptureTerminalState.Succeeded) ||
                    item.GetProperty("failureCode").GetString() != nameof(DualCameraFailureCode.None) ||
                    item.GetProperty("automaticRetryCount").GetInt32() != 0 ||
                    item.GetProperty("approvalRecordId").ValueKind != JsonValueKind.Null ||
                    !transactionIds.Add(transactionId) ||
                    eventStartedAt.Offset != TimeSpan.Zero ||
                    eventCompletedAt.Offset != TimeSpan.Zero ||
                    eventCompletedAt < eventStartedAt ||
                    previousCompletedAt is not null && eventStartedAt < previousCompletedAt ||
                    !double.IsFinite(durationMilliseconds) ||
                    durationMilliseconds < 0 ||
                    Math.Abs(durationMilliseconds - (eventCompletedAt - eventStartedAt).TotalMilliseconds) > 0.0001)
                {
                    throw new InvalidDataException("10-run event evidence is invalid.");
                }

                ValidatePublishedOriginals(item.GetProperty("originals"));
                firstStartedAt ??= eventStartedAt;
                previousCompletedAt = eventCompletedAt;
                lastCompletedAt = eventCompletedAt;
                durations.Add(durationMilliseconds);
            }

            var computedP95 = durations.Order().ElementAt((int)Math.Ceiling(durations.Count * .95) - 1);
            if (firstStartedAt != startedAt ||
                lastCompletedAt != completedAt ||
                Math.Abs(computedP95 - p95Milliseconds) > 0.0001)
            {
                throw new InvalidDataException("10-run summary and events do not agree.");
            }

            var report = Encoding.UTF8.GetString(reportBytes);
            if (!report.Contains("- evidenceScope: SoftwareAggregationOnly", StringComparison.Ordinal) ||
                !report.Contains("- hardwareExecutionVerified: false", StringComparison.Ordinal) ||
                !report.Contains("- productionRunner: false", StringComparison.Ordinal) ||
                !report.Contains("- runId: " + runId, StringComparison.Ordinal) ||
                !report.Contains("- verdict: SoftwareAggregatePartial", StringComparison.Ordinal))
            {
                throw new InvalidDataException("Published 10-run report markers are invalid.");
            }

            return new(
                TimeSpan.FromMilliseconds(p95Milliseconds),
                completedAt,
                transactionIds,
                files with { P95Milliseconds = p95Milliseconds });
        }
        catch (Exception exception) when (
            exception is JsonException or KeyNotFoundException or InvalidOperationException or FormatException or OverflowException)
        {
            throw new InvalidDataException("Published 10-run evidence is invalid.", exception);
        }
    }

    internal IReadOnlyList<string> ClaimTransactions(
        string runId,
        IReadOnlyList<CaptureRecoveryOnlyRunAttemptEvidence> attempts,
        bool retainOnFailure = false)
    {
        var directory = Path.Combine(_root, "transactions");
        EnsureWriteDirectory(directory);
        var claims = new List<string>(attempts.Count);
        try
        {
            foreach (var attempt in attempts)
            {
                var claim = Path.Combine(
                    directory,
                    "transaction-" + attempt.TransactionId.ToString("N") + ".claim");
                CreateExclusiveClaim(
                    claim,
                    runId,
                    "A transaction ID was already used by another evidence run.",
                    retainOnFailure);
                claims.Add(claim);
            }
            return claims;
        }
        catch
        {
            if (!retainOnFailure)
            {
                foreach (var claim in claims)
                    TryDeleteOwnedFile(claim);
            }
            throw;
        }
    }

    internal string ClaimApproval(string approvalId, string hundredRunId, bool retainOnFailure = false)
    {
        var claim = Path.Combine(_root, "approvals", approvalId + ".used");
        CreateExclusiveClaim(
            claim,
            hundredRunId,
            "Approval is already used or in progress.",
            retainOnFailure);
        return claim;
    }

    internal void EnsureApprovalClaimOwned(string approvalId, string hundredRunId)
    {
        var bytes = ReadRegularLocalFile(Path.Combine(_root, "approvals", approvalId + ".used"), 256);
        if (Encoding.UTF8.GetString(bytes) != hundredRunId)
            throw new InvalidDataException("100-run approval claim is not owned by this run.");
    }

    private void CreateExclusiveClaim(
        string path,
        string content,
        string failureMessage,
        bool retainOnFailure = false)
    {
        var directory = Path.GetDirectoryName(path)
            ?? throw new InvalidDataException("A claim directory is required.");
        EnsureWriteDirectory(directory);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(directory);
        var created = false;
        try
        {
            using var stream = new FileStream(
                path,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                4096,
                FileOptions.WriteThrough);
            created = true;
            using var writer = new StreamWriter(stream, new UTF8Encoding(false));
            writer.Write(content);
            writer.Flush();
            _beforeClaimFlush?.Invoke(Path.GetFileName(path));
            stream.Flush(true);
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(path);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            if (created && !retainOnFailure)
                TryDeleteOwnedFile(path);
            throw new InvalidDataException(failureMessage, exception);
        }
    }

    internal void EnsureRunDestinationAvailable(string runId)
    {
        var runs = Path.Combine(_root, "runs");
        EnsureWriteDirectory(runs);
        var destination = Path.Combine(runs, runId);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(destination);
        if (Directory.Exists(destination) || File.Exists(destination))
            throw new IOException("Evidence destination already exists.");
    }

    private static void ValidateRequest(CaptureRecoveryOnlyRunEvidenceRequest request)
    {
        if (!RunId.IsMatch(request.RunId) ||
            request.RequestedCount is not (10 or 100) ||
            request.Attempts is null ||
            request.Attempts.Count is 0 or > 100 ||
            request.Attempts.Count > request.RequestedCount)
        {
            throw new InvalidDataException("Run request is invalid.");
        }

        var ids = new HashSet<Guid>();
        for (var index = 0; index < request.Attempts.Count; index++)
        {
            var item = request.Attempts[index]
                ?? throw new InvalidDataException("Attempt is missing.");
            ValidateOutcome(item);
            if (!ids.Add(item.Outcome.TransactionId) ||
                item.CompletedAtUtc < item.StartedAtUtc ||
                item.StartedAtUtc.Offset != TimeSpan.Zero ||
                item.CompletedAtUtc.Offset != TimeSpan.Zero ||
                index > 0 && item.StartedAtUtc < request.Attempts[index - 1].CompletedAtUtc)
            {
                throw new InvalidDataException("Attempt ordering is invalid.");
            }
        }
    }

    internal static void ValidateOutcome(CaptureRecoveryOnlyRunAttempt item)
    {
        var outcome = item.Outcome
            ?? throw new InvalidDataException("Outcome is missing.");
        if (outcome.TransactionId == Guid.Empty ||
            outcome.AutomaticRetryCount != 0 ||
            outcome.RecoveryPending ||
            outcome.Originals is null ||
            outcome.Succeeded &&
                (outcome.TerminalState != DualHardwareCaptureTerminalState.Succeeded ||
                 outcome.FailureCode != DualCameraFailureCode.None) ||
            !outcome.Succeeded &&
                (outcome.TerminalState == DualHardwareCaptureTerminalState.Succeeded ||
                 outcome.FailureCode == DualCameraFailureCode.None))
        {
            throw new InvalidDataException("Typed terminal state is inconsistent.");
        }

        var expectedAliases = new[] { "CAM-A", "CAM-B" }.Take(outcome.Originals.Count);
        if (outcome.Originals.Count > 2 ||
            !outcome.Originals.Select(original => original.Alias).SequenceEqual(expectedAliases) ||
            outcome.Succeeded && outcome.Originals.Count != 2)
        {
            throw new InvalidDataException("Original aliases are invalid.");
        }
        if (outcome.FailureCode == DualCameraFailureCode.CaptureCameraA && outcome.Originals.Count != 0)
            throw new InvalidDataException("A CAM-A capture failure cannot claim a recovered original.");
        if (outcome.FailureCode == DualCameraFailureCode.CaptureCameraB &&
            (outcome.TerminalState != DualHardwareCaptureTerminalState.FailedPartial ||
             outcome.Originals.Count != 1 ||
             outcome.Originals[0].Alias != "CAM-A"))
        {
            throw new InvalidDataException("A CAM-B capture failure must retain exactly the CAM-A original.");
        }

        foreach (var original in outcome.Originals)
        {
            if (Path.GetFileName(original.Path) != "original.jpg" ||
                original.Width != 7360 ||
                original.Height != 4912 ||
                original.SizeBytes <= 0 ||
                !original.IsCanonicalJpeg ||
                !Hash.IsMatch(original.Sha256) ||
                ContainsSensitive(original.Path))
            {
                throw new InvalidDataException("Original evidence is invalid.");
            }
        }
    }

    private static void ValidatePublishedOriginals(JsonElement originals)
    {
        if (originals.ValueKind != JsonValueKind.Array || originals.GetArrayLength() != 2)
            throw new InvalidDataException("Published originals are invalid.");

        var aliases = new[] { "CAM-A", "CAM-B" };
        var index = 0;
        foreach (var original in originals.EnumerateArray())
        {
            if (original.GetProperty("alias").GetString() != aliases[index] ||
                original.GetProperty("fileName").GetString() != "original.jpg" ||
                original.GetProperty("sizeBytes").GetInt64() <= 0 ||
                !Hash.IsMatch(original.GetProperty("sha256").GetString() ?? string.Empty))
            {
                throw new InvalidDataException("Published originals are invalid.");
            }
            index++;
        }
    }

    internal static CaptureRecoveryOnlyRunAttemptEvidence ToAttempt(
        int number,
        CaptureRecoveryOnlyRunAttempt item) => new(
            number,
            item.Outcome.TransactionId,
            item.StartedAtUtc,
            item.CompletedAtUtc,
            item.CompletedAtUtc - item.StartedAtUtc,
            item.Outcome.Succeeded,
            item.Outcome.TerminalState,
            item.Outcome.FailureCode,
            item.Outcome.AutomaticRetryCount,
            item.Outcome.Originals.Select(original => new CaptureRecoveryOnlyOriginalEvidence(
                original.Alias,
                "original.jpg",
                original.SizeBytes,
                original.Sha256)).ToArray());

    internal static TimeSpan Percentile50(TimeSpan[] values) =>
        values.Length % 2 == 1
            ? values[values.Length / 2]
            : TimeSpan.FromTicks((values[values.Length / 2 - 1].Ticks + values[values.Length / 2].Ticks) / 2);

    internal static TimeSpan Percentile95(TimeSpan[] values) =>
        values[(int)Math.Ceiling(values.Length * .95) - 1];

    internal static bool ContainsSensitive(string value) =>
        value.Contains("serial", StringComparison.OrdinalIgnoreCase) ||
        value.Contains("identifier", StringComparison.OrdinalIgnoreCase) ||
        value.Contains("deviceid", StringComparison.OrdinalIgnoreCase) ||
        value.Contains("cameraid", StringComparison.OrdinalIgnoreCase);

    internal void EnsureWriteDirectory(string directory)
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(directory);
        Directory.CreateDirectory(directory);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(directory);
    }

    internal void WriteAtomic(string directory, string name, string content)
    {
        WriteAtomicBytes(directory, name, Encoding.UTF8.GetBytes(content));
    }

    private void WriteAtomicBytes(string directory, string name, byte[] bytes)
    {
        EnsureWriteDirectory(directory);
        _beforeWrite?.Invoke(name);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(directory);
        var target = Path.Combine(directory, name);
        var partial = target + ".partial";
        try
        {
            using (var stream = new FileStream(
                       partial,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       4096,
                       FileOptions.WriteThrough))
            {
                stream.Write(bytes);
                stream.Flush(true);
            }

            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(directory);
            EnsureRegularLocalFile(partial);
            if (File.Exists(target) || Directory.Exists(target))
                throw new IOException("Evidence file already exists.");
            File.Move(partial, target);
        }
        catch
        {
            TryDeleteOwnedFile(partial);
            throw;
        }
    }

    private static void CleanupStaging(string directory)
    {
        try
        {
            if (!Directory.Exists(directory))
                return;
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(directory);
            foreach (var file in Directory.EnumerateFiles(directory))
            {
                if (Path.GetFileName(file) is not (
                        "report.md" or
                        "summary.json" or
                        "transaction-events.jsonl" or
                        "report.md.partial" or
                        "summary.json.partial" or
                        "transaction-events.jsonl.partial"))
                {
                    return;
                }
                WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(file);
                File.Delete(file);
            }
            if (!Directory.EnumerateFileSystemEntries(directory).Any())
                Directory.Delete(directory);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private static void TryDeleteOwnedFile(string path)
    {
        try
        {
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(path);
            File.Delete(path);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private static void EnsureRegularLocalFile(string path)
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(path);
        var attributes = File.GetAttributes(path);
        if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint | FileAttributes.Device)) != 0)
            throw new InvalidDataException("Evidence requires a regular local file.");
    }

    private static byte[] ReadRegularLocalFile(string path, long maximumBytes)
    {
        try
        {
            EnsureRegularLocalFile(path);
            using var stream = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                4096,
                FileOptions.SequentialScan);
            if (stream.Length is <= 0 || stream.Length > maximumBytes)
                throw new InvalidDataException("Evidence file size is invalid.");
            var bytes = new byte[checked((int)stream.Length)];
            stream.ReadExactly(bytes);
            return bytes;
        }
        catch (InvalidDataException)
        {
            throw;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            throw new InvalidDataException("Evidence file could not be read safely.", exception);
        }
    }

    private static string HashBytes(byte[] bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    private static void RequireMarkers(JsonElement root)
    {
        if (root.GetProperty("evidenceScope").GetString() != Scope ||
            root.GetProperty("hardwareExecutionVerified").GetBoolean() ||
            root.GetProperty("productionRunner").GetBoolean() ||
            root.GetProperty("capturePurpose").GetString() != HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose ||
            root.GetProperty("stitchOutcome").GetString() != HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome ||
            root.GetProperty("a0QualityApproval").GetString() != HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval)
        {
            throw new InvalidDataException("Software-only markers are invalid.");
        }
    }

    private static string Report(CaptureRecoveryOnlyRunEvidence value)
    {
        var builder = new StringBuilder();
        builder.AppendLine("# CaptureRecoveryOnly software aggregation");
        builder.AppendLine();
        builder.AppendLine("- evidenceScope: SoftwareAggregationOnly");
        builder.AppendLine("- hardwareExecutionVerified: false");
        builder.AppendLine("- productionRunner: false");
        builder.AppendLine("- approvalAuthorityVerifiedBySoftware: false");
        builder.AppendLine("- capturePurpose: CaptureRecoveryOnly");
        builder.AppendLine("- stitchOutcome: Pending");
        builder.AppendLine("- a0QualityApproval: Unapproved");
        builder.AppendLine("- runId: " + value.RunId);
        builder.AppendLine("- requestedCount: " + value.RequestedCount.ToString(CultureInfo.InvariantCulture));
        builder.AppendLine("- succeededCount: " + value.Attempts.Count(attempt => attempt.Succeeded).ToString(CultureInfo.InvariantCulture));
        builder.AppendLine("- verdict: " + value.Verdict);
        builder.AppendLine("- p95ApprovalStatus: " + value.P95ApprovalStatus);
        builder.AppendLine("- automaticRetryCount: " + value.Attempts.Sum(attempt => attempt.AutomaticRetryCount).ToString(CultureInfo.InvariantCulture));
        builder.AppendLine("- firstFailure: " + (value.FirstFailure is null
            ? "None"
            : value.FirstFailure.AttemptNumber.ToString(CultureInfo.InvariantCulture) + ":" + value.FirstFailure.FailureCode));
        builder.AppendLine("- p50: " + value.P50.TotalMilliseconds.ToString("F3", CultureInfo.InvariantCulture) + " ms");
        builder.AppendLine("- p95: " + value.P95.TotalMilliseconds.ToString("F3", CultureInfo.InvariantCulture) + " ms");
        builder.AppendLine("- max: " + value.Max.TotalMilliseconds.ToString("F3", CultureInfo.InvariantCulture) + " ms");
        if (value.PriorApproval is null)
        {
            builder.AppendLine("- approvalRecordId: NotApplicable");
        }
        else
        {
            var approval = value.PriorApproval;
            builder.AppendLine("- approvalRecordId: " + approval.ApprovalRecordId);
            builder.AppendLine("- approvedTenRunId: " + approval.TenRunId);
            builder.AppendLine("- approvalTargetHundredRunId: " + approval.TargetHundredRunId);
            builder.AppendLine("- approvedP95Milliseconds: " + approval.P95.TotalMilliseconds.ToString("F3", CultureInfo.InvariantCulture));
            builder.AppendLine("- approvalRecordedAtUtc: " + approval.RecordedAtUtc.ToString("O", CultureInfo.InvariantCulture));
            builder.AppendLine("- approvedTenRunReportSha256: " + approval.TenRunFiles.ReportSha256);
            builder.AppendLine("- approvedTenRunSummarySha256: " + approval.TenRunFiles.SummarySha256);
            builder.AppendLine("- approvedTenRunEventsSha256: " + approval.TenRunFiles.EventsSha256);
            builder.AppendLine("- approvalRecordSha256: " + approval.ApprovalRecordSha256);
            builder.AppendLine("- externalApprovalSha256: " + approval.ExternalApprovalSha256);
        }
        return builder.ToString();
    }

    private static object Summary(CaptureRecoveryOnlyRunEvidence value) => new
    {
        evidenceScope = Scope,
        hardwareExecutionVerified = false,
        productionRunner = false,
        approvalAuthorityVerifiedBySoftware = false,
        capturePurpose = HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose,
        stitchOutcome = HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome,
        a0QualityApproval = HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval,
        value.RunId,
        value.RequestedCount,
        succeededCount = value.Attempts.Count(attempt => attempt.Succeeded),
        startedAtUtc = value.Attempts[0].StartedAtUtc,
        completedAtUtc = value.Attempts[^1].CompletedAtUtc,
        verdict = value.Verdict.ToString(),
        value.P95ApprovalStatus,
        automaticRetryCount = value.Attempts.Sum(attempt => attempt.AutomaticRetryCount),
        firstFailureAttempt = value.FirstFailure?.AttemptNumber,
        firstFailureCode = value.FirstFailure?.FailureCode.ToString() ?? nameof(DualCameraFailureCode.None),
        p50Milliseconds = value.P50.TotalMilliseconds,
        p95Milliseconds = value.P95.TotalMilliseconds,
        maxMilliseconds = value.Max.TotalMilliseconds,
        approval = value.PriorApproval is null ? null : ApprovalReference(value.PriorApproval),
    };

    private static string Events(CaptureRecoveryOnlyRunEvidence value)
    {
        var approval = value.PriorApproval is null ? null : ApprovalReference(value.PriorApproval);
        return string.Join(Environment.NewLine, value.Attempts.Select(attempt => JsonSerializer.Serialize(new
        {
            evidenceScope = Scope,
            hardwareExecutionVerified = false,
            productionRunner = false,
            approvalAuthorityVerifiedBySoftware = false,
            capturePurpose = HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose,
            stitchOutcome = HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome,
            a0QualityApproval = HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval,
            attempt.AttemptNumber,
            transactionId = attempt.TransactionId.ToString("N"),
            attempt.StartedAtUtc,
            attempt.CompletedAtUtc,
            durationMilliseconds = attempt.Duration.TotalMilliseconds,
            attempt.Succeeded,
            terminalState = attempt.TerminalState.ToString(),
            failureCode = attempt.FailureCode.ToString(),
            attempt.AutomaticRetryCount,
            attempt.Originals,
            approvalRecordId = approval?.ApprovalRecordId,
            approvedTenRunId = approval?.TenRunId,
            approvedP95Milliseconds = approval?.P95Milliseconds,
            approvalRecordSha256 = approval?.ApprovalRecordSha256,
            externalApprovalSha256 = approval?.ExternalApprovalSha256,
        }, CompactJsonOptions))) + Environment.NewLine;
    }

    private static CaptureRecoveryOnlyApprovalReference ApprovalReference(
        CaptureRecoveryOnlyP95Approval approval) => new(
            approval.ApprovalRecordId,
            approval.TenRunId,
            approval.TargetHundredRunId,
            approval.P95.TotalMilliseconds,
            approval.RecordedAtUtc,
            approval.TenRunCompletedAtUtc,
            approval.TenRunFiles,
            approval.ApprovalRecordSha256,
            approval.ExternalApprovalSha256);

    private string ApprovalPath(string approvalId) =>
        Path.Combine(_root, "approvals", approvalId + ".json");

    private string ApprovalDecisionPath(string approvalId) =>
        Path.Combine(_root, "approvals", approvalId + ".decision.json");

    private string TargetApprovalClaimPath(string targetHundredRunId) =>
        Path.Combine(
            _root,
            "approvals",
            "target-" + targetHundredRunId["run-".Length..] + ".claim");

    private static bool IsWithinRoot(string path, string root)
    {
        var relative = Path.GetRelativePath(root, path);
        return !Path.IsPathRooted(relative) &&
               relative != ".." &&
               !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) &&
               !relative.StartsWith(".." + Path.AltDirectorySeparatorChar, StringComparison.Ordinal);
    }

    private static bool HasExactProperties(JsonElement element, params string[] expected)
    {
        if (element.ValueKind != JsonValueKind.Object)
            return false;
        var properties = element.EnumerateObject().Select(property => property.Name).ToArray();
        return properties.Length == expected.Length &&
               new HashSet<string>(properties, StringComparer.Ordinal).SetEquals(expected);
    }

    internal sealed record PublishedTenRun(
        TimeSpan P95,
        DateTimeOffset CompletedAtUtc,
        HashSet<Guid> TransactionIds,
        CaptureRecoveryOnlyRunFileHashes Files);

    private sealed record ExternalApprovalDecision(
        string TenRunId,
        string TargetHundredRunId,
        TimeSpan P95,
        DateTimeOffset ApprovedAtUtc,
        DateTimeOffset TenRunCompletedAtUtc,
        CaptureRecoveryOnlyRunFileHashes TenRunFiles);

    private sealed class PublishedEvidenceException : IOException
    {
        internal PublishedEvidenceException(string message, Exception innerException)
            : base(message, innerException)
        {
        }
    }
}

internal sealed record CaptureRecoveryOnlyRunEvidenceRequest(
    string RunId,
    int RequestedCount,
    IReadOnlyList<CaptureRecoveryOnlyRunAttempt> Attempts,
    string? ApprovalRecordId = null);

internal sealed record CaptureRecoveryOnlyRunAttempt(
    DateTimeOffset StartedAtUtc,
    DateTimeOffset CompletedAtUtc,
    HardwareDualCaptureRecoveryOnlyExecution Outcome);

internal sealed record CaptureRecoveryOnlyP95Approval(
    string ApprovalRecordId,
    string TenRunId,
    string TargetHundredRunId,
    TimeSpan P95,
    DateTimeOffset RecordedAtUtc,
    string ApproverRole,
    DateTimeOffset TenRunCompletedAtUtc,
    CaptureRecoveryOnlyRunFileHashes TenRunFiles,
    string ApprovalRecordSha256,
    string ExternalApprovalSha256);

internal enum CaptureRecoveryOnlyRunVerdict
{
    SoftwareAggregatePartial,
    SoftwareAggregatePass,
    SoftwareAggregateFail,
}

internal sealed record CaptureRecoveryOnlyRunEvidence(
    string RunId,
    int RequestedCount,
    IReadOnlyList<CaptureRecoveryOnlyRunAttemptEvidence> Attempts,
    CaptureRecoveryOnlyRunAttemptEvidence? FirstFailure,
    TimeSpan P50,
    TimeSpan P95,
    TimeSpan Max,
    CaptureRecoveryOnlyRunVerdict Verdict,
    string P95ApprovalStatus,
    CaptureRecoveryOnlyP95Approval? PriorApproval);

internal sealed record CaptureRecoveryOnlyRunAttemptEvidence(
    int AttemptNumber,
    Guid TransactionId,
    DateTimeOffset StartedAtUtc,
    DateTimeOffset CompletedAtUtc,
    TimeSpan Duration,
    bool Succeeded,
    DualHardwareCaptureTerminalState TerminalState,
    DualCameraFailureCode FailureCode,
    int AutomaticRetryCount,
    IReadOnlyList<CaptureRecoveryOnlyOriginalEvidence> Originals);

internal sealed record CaptureRecoveryOnlyOriginalEvidence(
    string Alias,
    string FileName,
    long SizeBytes,
    string Sha256);

internal sealed record CaptureRecoveryOnlyRunFileHashes(
    string ReportSha256,
    string SummarySha256,
    string EventsSha256,
    double P95Milliseconds);

internal sealed record CaptureRecoveryOnlyApprovalReference(
    string ApprovalRecordId,
    string TenRunId,
    string TargetHundredRunId,
    double P95Milliseconds,
    DateTimeOffset RecordedAtUtc,
    DateTimeOffset TenRunCompletedAtUtc,
    CaptureRecoveryOnlyRunFileHashes TenRunFiles,
    string ApprovalRecordSha256,
    string ExternalApprovalSha256);

internal sealed record CaptureRecoveryOnlyRunEvidenceFiles(
    string ReportPath,
    string SummaryPath,
    string TransactionEventsPath);
