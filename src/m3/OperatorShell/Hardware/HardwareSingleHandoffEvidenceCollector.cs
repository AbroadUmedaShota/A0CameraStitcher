using System.Collections.Concurrent;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal interface IHardwareSingleHandoffEvidenceCollector
{
    void ObserveLiveViewStarted(HardwareContinuousLiveViewResult result);
    void ObserveLiveViewStartUnconfirmed(string sessionId);
    void ObserveLiveViewFrame(string sessionId, ulong frameNumber);
    void BeginHandoff(string sessionId);
    void ObserveLiveViewStopRequested(string sessionId);
    void ObserveLiveViewStopped(HardwareContinuousLiveViewResult result);
    void ObserveLiveViewStopUnconfirmed(string sessionId);
    void ObserveTransactionBound(string transactionId);
    void ObserveCaptureDispatchAttempted(string transactionId);
    void ObserveCaptureResult(HardwareSingleCaptureResult result, bool applicationOriginalVerified);
    void ObserveCaptureResponseUnknown(string transactionId, bool dispatchAttempted);
    void Seal();
    Task FlushAsync();
}

/// <summary>
/// Opt-in, observation-only collector for the ten SingleCamera product handoffs.
/// Camera calls never await this component: observations are serialized on a
/// background continuation chain and an evidence failure can only invalidate the
/// acceptance file. It cannot dispatch, retry, stop, capture, or delete anything.
/// </summary>
internal sealed class HardwareSingleHandoffEvidenceCollector :
    IHardwareSingleHandoffEvidenceCollector
{
    internal const string SchemaVersion = "a0.hardware-single-handoff-acceptance.v1";
    internal const int RequestedHandoffs = 10;
    private const string EvidenceScope = "HardwareSingleProductHandoffObservation";
    private const long MaximumAgentEventsBytes = 4 * 1024 * 1024;
    private static readonly string[] RequiredSuccessfulAgentStates =
    [
        "HybridWpdBaselineOpen",
        "HybridWpdBaselineOpened",
        "HybridSpoolEmptyBefore",
        "HybridWpdBaselineClosed",
        "HybridSdkCardCapture",
        "HybridSdkSessionOpened",
        "HybridSdkCaptureCompleted",
        "HybridSdkSessionClosed",
        "HybridWpdObserveOpen",
        "HybridWpdRecoveryOpened",
        "HybridRecoveredExactlyOneCandidate",
        "HybridPersistPcOriginal",
        "Persisted",
        "HybridPcOriginalVerified",
        "HybridCameraObjectDeleteStarted",
        "HybridCameraObjectDeleted",
        "HybridSpoolEmptyAfter",
        "HybridWpdRecoveryClosed",
        "Complete",
    ];
    private static readonly HashSet<string> AllowedAgentStates = new(
        RequiredSuccessfulAgentStates.Concat(
        [
            "HybridSdkProfileRevalidation",
            "HybridFailed",
            "WatchdogExpired",
            "PcOriginalPersistWatchdogExpired",
            "PcOriginalVerificationFailed",
            "Quarantined",
            "UnconfirmedCandidateQuarantined",
            "UnconfirmedDispatchQuarantined",
            "FailedPartial",
        ]),
        StringComparer.Ordinal);
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
    };

    private readonly object _queueGate = new();
    private readonly object _frameGate = new();
    private readonly Dictionary<string, FrameAggregate> _frameAggregates = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, byte> _stopRequestedSessions = new(StringComparer.Ordinal);
    private readonly string _evidenceRoot;
    private readonly string _agentArtifactsRoot;
    private readonly string _sourceSha;
    private readonly string _evidenceRunId;
    private readonly string _targetPath;
    private readonly Func<DateTimeOffset> _utcNow;
    private readonly Action<string>? _beforePersist;
    private Task _tail = Task.CompletedTask;

    // The following state is touched only by the serialized background chain.
    private readonly List<AttemptState> _attempts = [];
    private readonly HashSet<string> _seenSessionIds = new(StringComparer.Ordinal);
    private readonly HashSet<string> _seenTransactionIds = new(StringComparer.Ordinal);
    private readonly HashSet<string> _seenCaptureRunIds = new(StringComparer.Ordinal);
    private SessionState? _currentSession;
    private AttemptState? _activeAttempt;
    private string _terminalState = "InProgress";
    private string _lastHandoffState = "NotStarted";
    private string _lastErrorCategory = "None";
    private bool _persistenceHealthy = true;
    private bool _sealed;

    internal HardwareSingleHandoffEvidenceCollector(
        string evidenceRoot,
        string agentArtifactsRoot,
        string sourceSha,
        Func<DateTimeOffset>? utcNow = null,
        Action<string>? beforePersist = null)
    {
        if (string.IsNullOrWhiteSpace(evidenceRoot) ||
            string.IsNullOrWhiteSpace(agentArtifactsRoot))
        {
            throw new ArgumentException("Fixed-local evidence and Agent artifact roots are required.");
        }
        if (!IsLowerHex(sourceSha, 40))
        {
            throw new ArgumentException("The handoff evidence source SHA must be 40 lower-hex characters.", nameof(sourceSha));
        }

        _evidenceRoot = Path.GetFullPath(evidenceRoot);
        _agentArtifactsRoot = Path.GetFullPath(agentArtifactsRoot);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_evidenceRoot);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_agentArtifactsRoot);
        _sourceSha = sourceSha;
        _evidenceRunId = "run-" + Guid.NewGuid().ToString("N");
        _targetPath = Path.Combine(_evidenceRoot, _evidenceRunId + ".json");
        _utcNow = utcNow ?? (() => DateTimeOffset.UtcNow);
        _beforePersist = beforePersist;
    }

    internal string EvidenceRunId => _evidenceRunId;
    internal string EvidencePath => _targetPath;

    public void ObserveLiveViewStarted(HardwareContinuousLiveViewResult result)
    {
        ArgumentNullException.ThrowIfNull(result);
        var observedAt = UtcNow();
        Enqueue(() => ApplyLiveViewStarted(result, observedAt));
    }

    public void ObserveLiveViewStartUnconfirmed(string sessionId)
    {
        Enqueue(() =>
        {
            if (_activeAttempt?.Phase == AttemptPhase.AwaitingRestart)
            {
                Fail("LiveViewRestartUnconfirmed");
            }
            else
            {
                Invalidate(IsLowerHex(sessionId, 32) ? "LiveViewStartUnconfirmed" : "InvalidSequence");
            }
        });
    }

    public void ObserveLiveViewFrame(string sessionId, ulong frameNumber)
    {
        var observedAt = UtcNow();
        var enqueueMilestone = false;
        lock (_frameGate)
        {
            if (!_frameAggregates.TryGetValue(sessionId, out var aggregate))
            {
                aggregate = new FrameAggregate();
                _frameAggregates.Add(sessionId, aggregate);
            }
            aggregate.Observe(frameNumber, observedAt);
            enqueueMilestone = aggregate.Count <= 2 || aggregate.Invalid ||
                _stopRequestedSessions.ContainsKey(sessionId);
        }
        if (enqueueMilestone)
        {
            Enqueue(() => ApplyFrames(SnapshotFrames(sessionId)));
        }
    }

    public void BeginHandoff(string sessionId)
    {
        var observedAt = UtcNow();
        var frames = SnapshotFrames(sessionId);
        Enqueue(() =>
        {
            ApplyFrames(frames);
            ApplyBeginHandoff(sessionId, observedAt);
        });
    }

    public void ObserveLiveViewStopRequested(string sessionId)
    {
        _stopRequestedSessions.TryAdd(sessionId, 0);
        var observedAt = UtcNow();
        var frames = SnapshotFrames(sessionId);
        Enqueue(() =>
        {
            ApplyFrames(frames);
            ApplyStopRequested(sessionId, observedAt);
        });
    }

    public void ObserveLiveViewStopped(HardwareContinuousLiveViewResult result)
    {
        ArgumentNullException.ThrowIfNull(result);
        var observedAt = UtcNow();
        Enqueue(() => ApplyStopped(result, observedAt));
    }

    public void ObserveLiveViewStopUnconfirmed(string sessionId)
    {
        var observedAt = UtcNow();
        Enqueue(() => ApplyStopUnconfirmed(sessionId, observedAt));
    }

    public void ObserveTransactionBound(string transactionId)
    {
        var observedAt = UtcNow();
        Enqueue(() =>
        {
            if (_activeAttempt is null || _activeAttempt.Phase != AttemptPhase.PreCaptureStopped ||
                !IsLowerHex(transactionId, 32) || !_seenTransactionIds.Add(transactionId))
            {
                Invalidate("InvalidSequence");
                return;
            }
            _activeAttempt.TransactionId = transactionId;
            _activeAttempt.TransactionBoundAtUtc = observedAt;
            _activeAttempt.Phase = AttemptPhase.TransactionBound;
            _lastHandoffState = "TransactionBound";
        });
    }

    public void ObserveCaptureDispatchAttempted(string transactionId)
    {
        var observedAt = UtcNow();
        Enqueue(() =>
        {
            if (_activeAttempt is null || _activeAttempt.Phase != AttemptPhase.TransactionBound ||
                !string.Equals(_activeAttempt.TransactionId, transactionId, StringComparison.Ordinal))
            {
                Invalidate("InvalidSequence");
                return;
            }
            _activeAttempt.CaptureDispatchAttempted = true;
            _activeAttempt.CaptureDispatchedAtUtc = observedAt;
            _activeAttempt.Phase = AttemptPhase.CaptureDispatched;
            _lastHandoffState = "CaptureDispatched";
        });
    }

    public void ObserveCaptureResult(
        HardwareSingleCaptureResult result,
        bool applicationOriginalVerified)
    {
        ArgumentNullException.ThrowIfNull(result);
        var observedAt = UtcNow();
        Enqueue(() => ApplyCaptureResult(result, applicationOriginalVerified, observedAt));
    }

    public void ObserveCaptureResponseUnknown(string transactionId, bool dispatchAttempted)
    {
        var observedAt = UtcNow();
        Enqueue(() =>
        {
            if (_activeAttempt is not null &&
                string.Equals(_activeAttempt.TransactionId, transactionId, StringComparison.Ordinal))
            {
                _activeAttempt.CaptureResponseObservedAtUtc = observedAt;
                _activeAttempt.CaptureDispatchAttempted |= dispatchAttempted;
            }
            Fail("CaptureResponseUnknown");
        });
    }

    public void Seal()
    {
        Enqueue(() =>
        {
            _sealed = true;
            if (_terminalState == "InProgress")
            {
                var complete = _activeAttempt is null &&
                    _attempts.Count == RequestedHandoffs &&
                    _attempts.All(item => item.Phase == AttemptPhase.Complete);
                _terminalState = complete ? "Complete" : "Incomplete";
                _lastErrorCategory = complete ? "None" : "Incomplete";
            }
        });
    }

    public Task FlushAsync()
    {
        lock (_queueGate)
        {
            return _tail;
        }
    }

    private void ApplyLiveViewStarted(HardwareContinuousLiveViewResult result, DateTimeOffset observedAt)
    {
        if (_sealed || !IsLowerHex(result.SessionId, 32) ||
            result.CameraMode != "SingleCamera" || result.CameraAlias != "CAM-A" ||
            result.State != "Started" || !result.SdkSessionOpen || !result.LiveViewRunning ||
            result.RealIdentifiersIncluded)
        {
            Invalidate("InvalidSequence");
            return;
        }
        if (!_seenSessionIds.Add(result.SessionId))
        {
            Invalidate("DuplicateSession");
            return;
        }
        if (_currentSession is not null)
        {
            Invalidate("DuplicateSession");
            return;
        }

        var session = new SessionState(result.SessionId, observedAt);
        _currentSession = session;
        lock (_frameGate)
        {
            _frameAggregates.TryAdd(result.SessionId, new FrameAggregate());
        }
        // Stop is observed synchronously and may already have arrived while this
        // start waited in the queue. Session IDs cannot be reused, so preserve the
        // marker for late-frame detection instead of resetting newer observations.

        if (_activeAttempt is not null)
        {
            if (_activeAttempt.Phase != AttemptPhase.AwaitingRestart)
            {
                Invalidate("InvalidSequence");
                return;
            }
            _activeAttempt.RestartSession = session;
            _activeAttempt.Phase = AttemptPhase.RestartRunning;
            _lastHandoffState = "RestartRunning";
        }
    }

    private void ApplyBeginHandoff(string sessionId, DateTimeOffset observedAt)
    {
        if (_sealed || _terminalState != "InProgress" || _activeAttempt is not null ||
            _attempts.Count >= RequestedHandoffs || _currentSession is null ||
            !string.Equals(_currentSession.SessionId, sessionId, StringComparison.Ordinal))
        {
            Invalidate("InvalidSequence");
            return;
        }

        var attempt = new AttemptState(_attempts.Count + 1, observedAt, _currentSession);
        _attempts.Add(attempt);
        _activeAttempt = attempt;
        _lastHandoffState = "StopPending";
        if (!SessionHasContinuousFrames(_currentSession))
        {
            Fail("InitialLiveViewNotContinuous");
        }
    }

    private void ApplyStopRequested(string sessionId, DateTimeOffset observedAt)
    {
        if (_currentSession is null ||
            !string.Equals(_currentSession.SessionId, sessionId, StringComparison.Ordinal))
        {
            Invalidate("ForeignSession");
            return;
        }
        _currentSession.StopRequestedAtUtc = observedAt;
        if (_activeAttempt is not null && _activeAttempt.Phase == AttemptPhase.Begun &&
            ReferenceEquals(_activeAttempt.InitialSession, _currentSession))
        {
            _activeAttempt.Phase = AttemptPhase.PreCaptureStopRequested;
            _lastHandoffState = "StopRequested";
        }
    }

    private void ApplyStopped(HardwareContinuousLiveViewResult result, DateTimeOffset observedAt)
    {
        if (_currentSession is null ||
            !string.Equals(_currentSession.SessionId, result.SessionId, StringComparison.Ordinal) ||
            result.State != "Stopped" || result.SdkSessionOpen || result.LiveViewRunning ||
            result.RealIdentifiersIncluded)
        {
            ApplyStopUnconfirmed(result.SessionId, observedAt);
            return;
        }

        _currentSession.StoppedAtUtc = observedAt;
        _currentSession.SdkSessionClosed = true;
        var stopped = _currentSession;
        _currentSession = null;

        if (_activeAttempt is null)
        {
            return;
        }
        if (_activeAttempt.Phase == AttemptPhase.PreCaptureStopRequested &&
            ReferenceEquals(_activeAttempt.InitialSession, stopped))
        {
            _activeAttempt.Phase = AttemptPhase.PreCaptureStopped;
            _lastHandoffState = "PreCaptureStopped";
            return;
        }
        if (_activeAttempt.Phase == AttemptPhase.RestartRunning &&
            ReferenceEquals(_activeAttempt.RestartSession, stopped))
        {
            if (!SessionHasContinuousFrames(stopped))
            {
                Fail("RestartLiveViewNotContinuous");
                return;
            }
            _activeAttempt.CompletedAtUtc = observedAt;
            _activeAttempt.Phase = AttemptPhase.Complete;
            _lastHandoffState = "Complete";
            _activeAttempt = null;
            return;
        }

        Invalidate("InvalidSequence");
    }

    private void ApplyStopUnconfirmed(string sessionId, DateTimeOffset observedAt)
    {
        if (_currentSession is not null &&
            string.Equals(_currentSession.SessionId, sessionId, StringComparison.Ordinal))
        {
            _currentSession.StopObservedAtUtc = observedAt;
            _currentSession.SdkSessionClosed = false;
            _currentSession = null;
        }
        Fail("LiveViewStopUnconfirmed");
    }

    private void ApplyCaptureResult(
        HardwareSingleCaptureResult result,
        bool applicationOriginalVerified,
        DateTimeOffset observedAt)
    {
        if (_activeAttempt is null || _activeAttempt.Phase != AttemptPhase.CaptureDispatched ||
            !string.Equals(_activeAttempt.TransactionId, result.TransactionId, StringComparison.Ordinal))
        {
            Invalidate("InvalidSequence");
            return;
        }

        var attempt = _activeAttempt;
        attempt.CaptureResponseObservedAtUtc = observedAt;
        attempt.CaptureTerminalState = NormalizeTerminalState(result.TerminalState);
        attempt.ApplicationOriginalVerified = applicationOriginalVerified;
        attempt.SpoolEmptyBeforeCapture = result.SpoolEmptyBeforeCapture;
        attempt.CameraObjectDeleteAttempted = result.CameraObjectDeleteAttempted;
        attempt.CameraObjectDeleteSucceeded = result.CameraObjectDeleteSucceeded;
        attempt.SpoolEmptyAfterCleanup = result.SpoolEmptyAfterCleanup;
        attempt.AutomaticRetryCount = result.AutomaticRetryCount;
        attempt.CaptureRunIdSha256 = HashOpaque(result.RunId);

        if (!_seenCaptureRunIds.Add(result.RunId))
        {
            Invalidate("DuplicateCaptureRun");
            return;
        }

        try
        {
            attempt.AgentTrace = ReadAgentTrace(result);
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or InvalidDataException or
            JsonException or NotSupportedException or System.Security.SecurityException)
        {
            Invalidate("AgentTraceInvalid");
            return;
        }

        if (result.TerminalState != "Complete" || result.RetainedOriginal is null ||
            !applicationOriginalVerified || !result.SpoolEmptyBeforeCapture ||
            !result.CameraObjectDeleteAttempted || !result.CameraObjectDeleteSucceeded ||
            !result.SpoolEmptyAfterCleanup || result.AutomaticRetryCount != 0 ||
            result.RealIdentifiersIncluded || result.LiveViewHandoffRequested ||
            !AgentTraceIsComplete(attempt.AgentTrace))
        {
            Fail(result.AutomaticRetryCount == 0 ? "CaptureFailed" : "RetryDetected");
            return;
        }

        attempt.Phase = AttemptPhase.AwaitingRestart;
        _lastHandoffState = "AwaitingRestart";
    }

    private IReadOnlyList<AgentTraceEvent> ReadAgentTrace(HardwareSingleCaptureResult result)
    {
        if (!IsSafeRunId(result.RunId) || !IsLowerHex(result.TransactionId, 32))
        {
            throw new InvalidDataException("Agent evidence identifiers are invalid.");
        }
        var runRoot = Path.GetFullPath(Path.Combine(_agentArtifactsRoot, result.RunId));
        var eventsPath = Path.GetFullPath(Path.Combine(runRoot, "events.jsonl"));
        if (!IsWithinRoot(runRoot, _agentArtifactsRoot) || !IsWithinRoot(eventsPath, runRoot))
        {
            throw new InvalidDataException("Agent evidence path escaped its fixed root.");
        }
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(eventsPath);
        var info = new FileInfo(eventsPath);
        if (!info.Exists || info.Length is <= 0 or > MaximumAgentEventsBytes ||
            (info.Attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint | FileAttributes.Device)) != 0)
        {
            throw new InvalidDataException("Agent event evidence is missing or invalid.");
        }

        var selected = new List<AgentTraceEvent>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        DateTimeOffset? priorTimestamp = null;
        foreach (var line in File.ReadLines(eventsPath, Encoding.UTF8))
        {
            var lineBytes = Encoding.UTF8.GetBytes(line);
            HardwareDualCaptureRecoveryOnlyProfileFile.RejectDuplicatePropertyNames(lineBytes, maxDepth: 8);
            using var document = JsonDocument.Parse(line, new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 8,
            });
            var root = document.RootElement;
            if (!root.TryGetProperty("runId", out var runIdElement) ||
                runIdElement.ValueKind != JsonValueKind.String ||
                !string.Equals(runIdElement.GetString(), result.RunId, StringComparison.Ordinal))
            {
                throw new InvalidDataException("Agent event run correlation is invalid.");
            }
            if (!root.TryGetProperty("transactionId", out var transactionElement) ||
                transactionElement.ValueKind != JsonValueKind.String ||
                !string.Equals(transactionElement.GetString(), result.TransactionId, StringComparison.Ordinal))
            {
                continue;
            }
            if (!root.TryGetProperty("state", out var stateElement) ||
                stateElement.ValueKind != JsonValueKind.String ||
                !root.TryGetProperty("timestamp", out var timestampElement) ||
                timestampElement.ValueKind != JsonValueKind.String)
            {
                throw new InvalidDataException("Agent event correlation is invalid.");
            }
            var state = stateElement.GetString() ?? string.Empty;
            var hasCameraAlias = root.TryGetProperty("cameraAlias", out var cameraAliasElement);
            if (hasCameraAlias &&
                (cameraAliasElement.ValueKind != JsonValueKind.String ||
                 !string.Equals(cameraAliasElement.GetString(), "CAM-A", StringComparison.Ordinal)) ||
                !hasCameraAlias && state is not ("Complete" or "FailedPartial"))
            {
                throw new InvalidDataException("Agent event camera correlation is invalid.");
            }
            if (!AllowedAgentStates.Contains(state))
            {
                throw new InvalidDataException("Agent event state is outside the acceptance allowlist.");
            }
            if (!seen.Add(state) || !TryParseUtc(timestampElement.GetString(), out var timestamp) ||
                priorTimestamp is not null && timestamp < priorTimestamp)
            {
                throw new InvalidDataException("Agent event ordering is invalid.");
            }
            priorTimestamp = timestamp;
            selected.Add(new AgentTraceEvent(state, timestamp));
        }
        return selected;
    }

    private static bool AgentTraceIsComplete(IReadOnlyList<AgentTraceEvent> trace)
    {
        var states = trace.Select(item => item.State).ToArray();
        if (states.SequenceEqual(RequiredSuccessfulAgentStates, StringComparer.Ordinal))
        {
            return true;
        }
        var withProfileRevalidation = RequiredSuccessfulAgentStates.ToList();
        withProfileRevalidation.Insert(6, "HybridSdkProfileRevalidation");
        return states.SequenceEqual(withProfileRevalidation, StringComparer.Ordinal);
    }

    private void ApplyFrames(FrameSnapshot snapshot)
    {
        if (_currentSession is null ||
            !string.Equals(_currentSession.SessionId, snapshot.SessionId, StringComparison.Ordinal))
        {
            if (snapshot.Count > 0)
            {
                Invalidate("LateOrForeignFrame");
            }
            return;
        }
        _currentSession.FrameCount = snapshot.Count;
        _currentSession.FirstFrameAtUtc = snapshot.FirstAtUtc;
        _currentSession.LastFrameAtUtc = snapshot.LastAtUtc;
        _currentSession.FirstFrameNumber = snapshot.FirstFrameNumber;
        _currentSession.LastFrameNumber = snapshot.LastFrameNumber;
        _currentSession.FrameSequenceValid = !snapshot.Invalid;
        if (snapshot.Invalid)
        {
            Invalidate("InvalidFrameSequence");
        }
    }

    private void Fail(string category)
    {
        if (_terminalState == "Invalid")
        {
            return;
        }
        _terminalState = "FailedPartial";
        _lastErrorCategory = category;
        _lastHandoffState = "Failed";
        if (_activeAttempt is not null)
        {
            _activeAttempt.FailureCategory = category;
            _activeAttempt.Phase = AttemptPhase.Failed;
            _activeAttempt = null;
        }
    }

    private void Invalidate(string category)
    {
        _terminalState = "Invalid";
        _lastErrorCategory = category;
        _lastHandoffState = "Invalid";
        if (_activeAttempt is not null)
        {
            _activeAttempt.FailureCategory = category;
            _activeAttempt.Phase = AttemptPhase.Invalid;
            _activeAttempt = null;
        }
    }

    private void Enqueue(Action action)
    {
        lock (_queueGate)
        {
            _tail = _tail.ContinueWith(
                _ => Process(action),
                CancellationToken.None,
                TaskContinuationOptions.None,
                TaskScheduler.Default);
        }
    }

    private void Process(Action action)
    {
        try
        {
            action();
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            Invalidate("CollectorObservationInvalid");
        }

        try
        {
            PersistCurrent();
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _persistenceHealthy = false;
            Invalidate("CollectorPersistenceFailed");
        }
    }

    private void PersistCurrent()
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_evidenceRoot);
        Directory.CreateDirectory(_evidenceRoot);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_evidenceRoot);
        var payload = BuildPayload();
        var bytes = JsonSerializer.SerializeToUtf8Bytes(payload, JsonOptions);
        _beforePersist?.Invoke(_targetPath);
        var partialPath = Path.Combine(
            _evidenceRoot,
            _evidenceRunId + "-" + Guid.NewGuid().ToString("N") + ".partial");
        try
        {
            using (var stream = new FileStream(
                       partialPath,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       4096,
                       FileOptions.WriteThrough))
            {
                stream.Write(bytes);
                stream.Flush(flushToDisk: true);
            }
            WindowsDurableFilePublisher.Publish(partialPath, _targetPath, replaceExisting: true);
        }
        catch
        {
            try
            {
                if (File.Exists(partialPath))
                {
                    File.Delete(partialPath);
                }
            }
            catch (Exception cleanupException) when (
                cleanupException is IOException or UnauthorizedAccessException)
            {
            }
            throw;
        }
    }

    private object BuildPayload()
    {
        var completed = _attempts.Count(item => item.Phase == AttemptPhase.Complete);
        var failures = _attempts.Count(item => item.Phase is AttemptPhase.Failed or AttemptPhase.Invalid);
        var terminal = !_persistenceHealthy ? "Invalid" : _terminalState;
        return new
        {
            schemaVersion = SchemaVersion,
            evidenceScope = EvidenceScope,
            sourceSha = _sourceSha,
            evidenceRunId = _evidenceRunId,
            requestedHandoffs = RequestedHandoffs,
            attemptedHandoffs = _attempts.Count,
            completedHandoffs = completed,
            failures,
            terminalState = terminal,
            lastHandoffState = _lastHandoffState,
            lastErrorCategory = !_persistenceHealthy ? "CollectorPersistenceFailed" : _lastErrorCategory,
            persistenceHealthy = _persistenceHealthy,
            sealedRun = _sealed,
            automaticRetryCount = _attempts.Sum(item => item.AutomaticRetryCount),
            realIdentifiersIncluded = false,
            attempts = _attempts.Select(ToPayload).ToArray(),
        };
    }

    private static object ToPayload(AttemptState attempt) => new
    {
        attemptNumber = attempt.Number,
        startedAtUtc = FormatUtc(attempt.StartedAtUtc),
        completedAtUtc = FormatUtc(attempt.CompletedAtUtc),
        state = attempt.Phase.ToString(),
        failureCategory = attempt.FailureCategory,
        initialLiveView = SessionPayload(attempt.InitialSession),
        transactionId = attempt.TransactionId,
        transactionBoundAtUtc = FormatUtc(attempt.TransactionBoundAtUtc),
        captureDispatchedAtUtc = FormatUtc(attempt.CaptureDispatchedAtUtc),
        captureResponseObservedAtUtc = FormatUtc(attempt.CaptureResponseObservedAtUtc),
        captureDispatchAttempted = attempt.CaptureDispatchAttempted,
        captureTerminalState = attempt.CaptureTerminalState,
        applicationOriginalVerified = attempt.ApplicationOriginalVerified,
        spoolEmptyBeforeCapture = attempt.SpoolEmptyBeforeCapture,
        cameraObjectDeleteAttempted = attempt.CameraObjectDeleteAttempted,
        cameraObjectDeleteSucceeded = attempt.CameraObjectDeleteSucceeded,
        spoolEmptyAfterCleanup = attempt.SpoolEmptyAfterCleanup,
        automaticRetryCount = attempt.AutomaticRetryCount,
        captureRunIdSha256 = attempt.CaptureRunIdSha256,
        agentTrace = attempt.AgentTrace.Select(item => new
        {
            state = item.State,
            timestampUtc = FormatUtc(item.TimestampUtc),
        }).ToArray(),
        restartLiveView = attempt.RestartSession is null ? null : SessionPayload(attempt.RestartSession),
    };

    private static object SessionPayload(SessionState session) => new
    {
        sessionId = session.SessionId,
        startedAtUtc = FormatUtc(session.StartedAtUtc),
        firstFrameAtUtc = FormatUtc(session.FirstFrameAtUtc),
        lastFrameAtUtc = FormatUtc(session.LastFrameAtUtc),
        frameCount = session.FrameCount,
        firstFrameNumber = session.FirstFrameNumber,
        lastFrameNumber = session.LastFrameNumber,
        frameSequenceValid = session.FrameSequenceValid,
        stopRequestedAtUtc = FormatUtc(session.StopRequestedAtUtc),
        stoppedAtUtc = FormatUtc(session.StoppedAtUtc ?? session.StopObservedAtUtc),
        sdkSessionClosed = session.SdkSessionClosed,
    };

    private FrameSnapshot SnapshotFrames(string sessionId)
    {
        lock (_frameGate)
        {
            if (!_frameAggregates.TryGetValue(sessionId, out var aggregate))
            {
                return new FrameSnapshot(sessionId, 0, null, null, 0, 0, false);
            }
            return aggregate.Snapshot(sessionId);
        }
    }

    private DateTimeOffset UtcNow()
    {
        var value = _utcNow();
        return value.Offset == TimeSpan.Zero ? value : value.ToUniversalTime();
    }

    private static bool SessionHasContinuousFrames(SessionState session) =>
        session.FrameSequenceValid && session.FrameCount >= 2 &&
        session.FirstFrameAtUtc is not null && session.LastFrameAtUtc is not null &&
        session.LastFrameAtUtc >= session.FirstFrameAtUtc &&
        session.LastFrameNumber > session.FirstFrameNumber;

    private static string NormalizeTerminalState(string value) => value switch
    {
        "Complete" => "Complete",
        "FailedPartial" => "FailedPartial",
        "Reserved" => "Reserved",
        "InProgress" => "InProgress",
        _ => "Other",
    };

    private static bool IsSafeRunId(string value) =>
        value.Length is >= 8 and <= 96 &&
        value.All(character =>
            character is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or '-');

    private static bool IsLowerHex(string value, int length) =>
        value.Length == length && value.All(character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f');

    private static bool TryParseUtc(string? value, out DateTimeOffset result) =>
        DateTimeOffset.TryParse(
            value,
            CultureInfo.InvariantCulture,
            DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal,
            out result) && result.Offset == TimeSpan.Zero;

    private static string? FormatUtc(DateTimeOffset? value) =>
        value?.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss.fffffff'Z'", CultureInfo.InvariantCulture);

    private static string HashOpaque(string value) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(value))).ToLowerInvariant();

    private static bool IsWithinRoot(string candidate, string root)
    {
        var relative = Path.GetRelativePath(root, candidate);
        return relative != ".." && !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) &&
            !Path.IsPathRooted(relative);
    }

    private enum AttemptPhase
    {
        Begun,
        PreCaptureStopRequested,
        PreCaptureStopped,
        TransactionBound,
        CaptureDispatched,
        AwaitingRestart,
        RestartRunning,
        Complete,
        Failed,
        Invalid,
    }

    private sealed class AttemptState(int number, DateTimeOffset startedAtUtc, SessionState initialSession)
    {
        public int Number { get; } = number;
        public DateTimeOffset StartedAtUtc { get; } = startedAtUtc;
        public DateTimeOffset? CompletedAtUtc { get; set; }
        public SessionState InitialSession { get; } = initialSession;
        public SessionState? RestartSession { get; set; }
        public AttemptPhase Phase { get; set; } = AttemptPhase.Begun;
        public string FailureCategory { get; set; } = "None";
        public string? TransactionId { get; set; }
        public DateTimeOffset? TransactionBoundAtUtc { get; set; }
        public DateTimeOffset? CaptureDispatchedAtUtc { get; set; }
        public DateTimeOffset? CaptureResponseObservedAtUtc { get; set; }
        public bool CaptureDispatchAttempted { get; set; }
        public string CaptureTerminalState { get; set; } = "NotObserved";
        public bool ApplicationOriginalVerified { get; set; }
        public bool SpoolEmptyBeforeCapture { get; set; }
        public bool CameraObjectDeleteAttempted { get; set; }
        public bool CameraObjectDeleteSucceeded { get; set; }
        public bool SpoolEmptyAfterCleanup { get; set; }
        public int AutomaticRetryCount { get; set; }
        public string? CaptureRunIdSha256 { get; set; }
        public IReadOnlyList<AgentTraceEvent> AgentTrace { get; set; } = [];
    }

    private sealed class SessionState(string sessionId, DateTimeOffset startedAtUtc)
    {
        public string SessionId { get; } = sessionId;
        public DateTimeOffset StartedAtUtc { get; } = startedAtUtc;
        public DateTimeOffset? FirstFrameAtUtc { get; set; }
        public DateTimeOffset? LastFrameAtUtc { get; set; }
        public ulong FirstFrameNumber { get; set; }
        public ulong LastFrameNumber { get; set; }
        public int FrameCount { get; set; }
        public bool FrameSequenceValid { get; set; } = true;
        public DateTimeOffset? StopRequestedAtUtc { get; set; }
        public DateTimeOffset? StopObservedAtUtc { get; set; }
        public DateTimeOffset? StoppedAtUtc { get; set; }
        public bool SdkSessionClosed { get; set; }
    }

    private sealed class FrameAggregate
    {
        public int Count { get; private set; }
        public DateTimeOffset? FirstAtUtc { get; private set; }
        public DateTimeOffset? LastAtUtc { get; private set; }
        public ulong FirstFrameNumber { get; private set; }
        public ulong LastFrameNumber { get; private set; }
        public bool Invalid { get; private set; }

        public void Observe(ulong frameNumber, DateTimeOffset observedAt)
        {
            if (Count == 0)
            {
                FirstAtUtc = observedAt;
                FirstFrameNumber = frameNumber;
            }
            else if (frameNumber <= LastFrameNumber || observedAt < LastAtUtc)
            {
                Invalid = true;
            }
            Count++;
            LastAtUtc = observedAt;
            LastFrameNumber = frameNumber;
        }

        public FrameSnapshot Snapshot(string sessionId) => new(
            sessionId,
            Count,
            FirstAtUtc,
            LastAtUtc,
            FirstFrameNumber,
            LastFrameNumber,
            Invalid);
    }

    private sealed record FrameSnapshot(
        string SessionId,
        int Count,
        DateTimeOffset? FirstAtUtc,
        DateTimeOffset? LastAtUtc,
        ulong FirstFrameNumber,
        ulong LastFrameNumber,
        bool Invalid);

    private sealed record AgentTraceEvent(string State, DateTimeOffset TimestampUtc);
}
