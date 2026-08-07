using System.Text.Json;
using System.Collections.Concurrent;
using A0CameraStitcher.M3.Foundation;

var tests = new (string Name, Func<Task> Run)[]
{
    ("protocol serialization and rejection", ProtocolSerializationAndRejectionAsync),
    ("named pipe fake agent roundtrip", NamedPipeRoundtripAsync),
    ("durable sequential success", DurableSequentialSuccessAsync),
    ("partial failure preserves CAM-A and does not retry", PartialFailureAndNoRetryAsync),
    ("crash restart closes journal without recapture", CrashRestartRecoveryAsync),
    ("WPF-facing service boundary stays simulated", WorkflowServiceBoundaryAsync),
    ("cross-coordinator ownership prevents duplicate capture", CrossCoordinatorOwnershipAsync),
    ("partial artifact restart terminates without capture", PartialArtifactRecoveryAsync),
    ("operator readiness requires safety and classifies correction", OperatorReadinessClassificationAsync),
    ("operator readiness exposes every blocking reason", OperatorReadinessBlockersAsync),
    ("operator action availability locks active workflows", OperatorActionAvailabilityAsync),
};

var failures = new List<string>();
foreach (var test in tests)
{
    try
    {
        await test.Run();
        Console.WriteLine($"PASS {test.Name}");
    }
    catch (Exception exception)
    {
        failures.Add(test.Name);
        Console.Error.WriteLine($"FAIL {test.Name}: {exception}");
    }
}

Console.WriteLine($"Foundation tests: {tests.Length - failures.Count}/{tests.Length} passed.");
return failures.Count == 0 ? 0 : 1;

static Task OperatorReadinessClassificationAsync()
{
    var today = new DateOnly(2026, 8, 7);
    var withoutAck = CreateReadySnapshot(safetyAcknowledged: false);
    Check.Equal(OperatorUiState.AwaitingSafetyAck, OperatorReadinessEvaluator.GetReadyState(withoutAck, today));
    Check.False(
        OperatorReadinessEvaluator.Evaluate(withoutAck, OperatorUiState.AwaitingSafetyAck, today, false, false).Capture.Allowed,
        "Capture must remain blocked before the startup-session acknowledgment.");

    var readyWithCorrection = CreateReadySnapshot(safetyAcknowledged: true);
    Check.Equal(OperatorUiState.ReadyWithCorrection, OperatorReadinessEvaluator.GetReadyState(readyWithCorrection, today));
    var notices = OperatorReadinessEvaluator.BuildNotices(readyWithCorrection, today);
    Check.True(notices.Any(notice => notice.Code == "AutomaticCorrectionPlanned"), "Planned correction must stay visible.");
    Check.True(notices.Any(notice => notice.Code == "PreviewIsNotOriginal"), "Preview provenance must stay visible.");
    Check.True(
        OperatorReadinessEvaluator.Evaluate(readyWithCorrection, OperatorUiState.ReadyWithCorrection, today, false, false).Capture.Allowed,
        "In-range correction must allow one-click capture without another dialog.");
    return Task.CompletedTask;
}

static Task OperatorReadinessBlockersAsync()
{
    var today = new DateOnly(2026, 8, 7);
    var snapshot = CreateReadySnapshot(safetyAcknowledged: true) with
    {
        Cameras =
        [
            new CameraReadiness("CAM-A", false, false, false, false),
            new CameraReadiness("CAM-B", true, false, false, false),
        ],
        Profile = new RigProfileReadiness("RIG-TEST", "1", today.AddDays(-1), false, false),
        Setup = new SetupAssessment(SetupAssessmentStatus.PhysicalAdjustmentRequired, "adjust", [], ["left"]),
        OutputDirectoryValid = false,
        HasActiveTransaction = true,
        CameraStateRequiresInspection = true,
    };
    var codes = OperatorReadinessEvaluator.BuildNotices(snapshot, today).Select(notice => notice.Code).ToHashSet(StringComparer.Ordinal);
    foreach (var code in new[] { "CameraMissing", "IdentityUnbound", "SettingsMismatch", "CardNotKnownEmpty", "ProfileInvalid", "PhysicalAdjustmentRequired", "OutputInvalid", "ActiveTransaction", "CameraInspectionRequired" })
    {
        Check.True(codes.Contains(code), $"Missing blocker code {code}.");
    }
    Check.Equal(OperatorUiState.NotReady, OperatorReadinessEvaluator.GetReadyState(snapshot, today));
    return Task.CompletedTask;
}

static Task OperatorActionAvailabilityAsync()
{
    var today = new DateOnly(2026, 8, 7);
    var active = CreateReadySnapshot(safetyAcknowledged: true) with { HasActiveTransaction = true };
    var availability = OperatorReadinessEvaluator.Evaluate(active, OperatorUiState.Capturing, today, true, true);
    Check.False(availability.Capture.Allowed, "Double capture must be blocked.");
    Check.False(availability.LiveView.Allowed, "Live View changes must be blocked during capture.");
    Check.False(availability.Export.Allowed, "Export must be blocked during capture.");
    Check.False(availability.Restitch.Allowed, "Restitch must be blocked during capture.");
    Check.False(availability.PrepareNewCapture.Allowed, "Readiness reset must be blocked during capture.");
    Check.False(availability.OpenMaintenance.Allowed, "Maintenance must be blocked during capture.");

    var review = CreateReadySnapshot(safetyAcknowledged: true);
    availability = OperatorReadinessEvaluator.Evaluate(review, OperatorUiState.Review, today, true, true);
    Check.True(availability.Export.Allowed, "A reviewed stitch result must be explicitly exportable.");
    Check.True(availability.Restitch.Allowed, "Two retained originals must allow a separate restitch job.");
    Check.True(availability.PrepareNewCapture.Allowed, "A completed outcome must allow read-only preparation.");
    return Task.CompletedTask;
}

static ReadinessSnapshot CreateReadySnapshot(bool safetyAcknowledged) => new()
{
    SafetyAcknowledged = safetyAcknowledged,
    Cameras =
    [
        new CameraReadiness("CAM-A", true, true, true, true),
        new CameraReadiness("CAM-B", true, true, true, true),
    ],
    Profile = new RigProfileReadiness("RIG-TEST", "1", new DateOnly(2027, 1, 1), true, true),
    Setup = new SetupAssessment(SetupAssessmentStatus.ReadyWithCorrection, "within bounds", ["rotation -0.2"], []),
    OutputDirectory = "simulated-output",
    OutputDirectoryValid = true,
    HasActiveTransaction = false,
    CameraStateRequiresInspection = false,
    Notices = [],
};

static Task ProtocolSerializationAndRejectionAsync()
{
    var request = CameraAgentProtocolCodec.CreateRequest(
        CameraAgentProtocol.Operations.Status,
        "request-serialization");
    var json = CameraAgentProtocolCodec.SerializeRequest(request);
    var roundTrip = CameraAgentProtocolCodec.DeserializeRequest(json);

    Check.Equal(CameraAgentProtocol.SchemaVersion, roundTrip.SchemaVersion);
    Check.True(roundTrip.Simulation, "The request must remain marked as simulation.");
    Check.Equal(CameraAgentProtocol.Marker, roundTrip.Marker);

    var unknownVersion = json.Replace(
        CameraAgentProtocol.SchemaVersion,
        "a0.camera-agent.simulated.v999",
        StringComparison.Ordinal);
    Check.ThrowsProtocol("UnsupportedSchemaVersion", () =>
        CameraAgentProtocolCodec.DeserializeRequest(unknownVersion));

    var nonSimulation = json.Replace("\"simulation\":true", "\"simulation\":false", StringComparison.Ordinal);
    Check.ThrowsProtocol("SimulationRequired", () =>
        CameraAgentProtocolCodec.DeserializeRequest(nonSimulation));

    var wrongMarker = json.Replace(
        $"\"marker\":\"{CameraAgentProtocol.Marker}\"",
        "\"marker\":\"Hardware\"",
        StringComparison.Ordinal);
    Check.ThrowsProtocol("SimulationMarkerRequired", () =>
        CameraAgentProtocolCodec.DeserializeRequest(wrongMarker));

    var response = CameraAgentProtocolCodec.CreateSuccess(
        request.RequestId,
        "StatusReady",
        JsonSerializer.SerializeToElement(new { marker = CameraAgentProtocol.Marker }));
    var responseRoundTrip = CameraAgentProtocolCodec.DeserializeResponse(
        CameraAgentProtocolCodec.SerializeResponse(response));
    Check.True(responseRoundTrip.Simulation, "The response must remain marked as simulation.");
    Check.Equal(CameraAgentProtocol.Marker, responseRoundTrip.Marker);

    var responseJson = CameraAgentProtocolCodec.SerializeResponse(response);
    var unknownResponseVersion = responseJson.Replace(
        CameraAgentProtocol.SchemaVersion,
        "a0.camera-agent.simulated.v999",
        StringComparison.Ordinal);
    Check.ThrowsProtocol("UnsupportedSchemaVersion", () =>
        CameraAgentProtocolCodec.DeserializeResponse(unknownResponseVersion));

    var nonSimulationResponse = responseJson.Replace(
        "\"simulation\":true",
        "\"simulation\":false",
        StringComparison.Ordinal);
    Check.ThrowsProtocol("SimulationRequired", () =>
        CameraAgentProtocolCodec.DeserializeResponse(nonSimulationResponse));

    return Task.CompletedTask;
}

static async Task NamedPipeRoundtripAsync()
{
    var pipeName = $"a0-camera-stitcher-simulated-{Guid.NewGuid():N}";
    await using var server = new NamedPipeCameraAgentServer(pipeName);
    server.Start();
    var client = new NamedPipeCameraAgentClient(pipeName);

    foreach (var operation in new[]
             {
                 CameraAgentProtocol.Operations.Status,
                 CameraAgentProtocol.Operations.Inventory,
                 CameraAgentProtocol.Operations.PreviewPlaceholder,
             })
    {
        var response = await client.SendAsync(CameraAgentProtocolCodec.CreateRequest(operation));
        Check.True(response.Success, $"The {operation} request should succeed.");
        Check.Equal(CameraAgentProtocol.Marker, response.Marker);
        Check.Equal("Simulated", response.Payload.GetProperty("marker").GetString());
    }

    const string invalidJson =
        "{\"schemaVersion\":\"a0.camera-agent.simulated.v999\",\"simulation\":true,\"marker\":\"Simulated\",\"requestId\":\"invalid-version\",\"operation\":\"status\",\"payload\":{}}";
    var rejectionJson = await client.SendRawAsync(invalidJson);
    var rejection = CameraAgentProtocolCodec.DeserializeResponse(rejectionJson);
    Check.False(rejection.Success, "An unknown schema version must be rejected by the pipe server.");
    Check.Equal("ProtocolRejected", rejection.ResultCode);
    Check.Equal("UnsupportedSchemaVersion", rejection.Payload.GetProperty("rejectionCode").GetString());

    var nonSimulationJson =
        $"{{\"schemaVersion\":\"{CameraAgentProtocol.SchemaVersion}\",\"simulation\":false,\"marker\":\"Simulated\",\"requestId\":\"invalid-mode\",\"operation\":\"status\",\"payload\":{{}}}}";
    var modeRejection = CameraAgentProtocolCodec.DeserializeResponse(
        await client.SendRawAsync(nonSimulationJson));
    Check.False(modeRejection.Success, "A non-simulation request must be rejected by the pipe server.");
    Check.Equal("SimulationRequired", modeRejection.Payload.GetProperty("rejectionCode").GetString());

    var overlongRequestIdJson =
        $"{{\"schemaVersion\":\"{CameraAgentProtocol.SchemaVersion}\",\"simulation\":true,\"marker\":\"Simulated\",\"requestId\":\"{new string('x', 129)}\",\"operation\":\"status\",\"payload\":{{}}}}";
    var requestIdRejection = CameraAgentProtocolCodec.DeserializeResponse(
        await client.SendRawAsync(overlongRequestIdJson));
    Check.False(requestIdRejection.Success, "An invalid request ID must receive a safe rejection.");
    Check.Equal("rejected", requestIdRejection.RequestId);
    Check.Equal("InvalidRequestId", requestIdRejection.Payload.GetProperty("rejectionCode").GetString());

    var wrongMarkerJson =
        $"{{\"schemaVersion\":\"{CameraAgentProtocol.SchemaVersion}\",\"simulation\":true,\"marker\":\"Hardware\",\"requestId\":\"invalid-marker\",\"operation\":\"status\",\"payload\":{{}}}}";
    var markerRejection = CameraAgentProtocolCodec.DeserializeResponse(
        await client.SendRawAsync(wrongMarkerJson));
    Check.False(markerRejection.Success, "A wrong marker must be rejected by the pipe server.");
    Check.Equal("SimulationMarkerRequired", markerRejection.Payload.GetProperty("rejectionCode").GetString());
}

static async Task DurableSequentialSuccessAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        var source = new DeterministicSimulatedCaptureSource();
        var coordinator = new DurableSimulatedCaptureCoordinator(root, source);
        Check.Equal(0, (await coordinator.InitializeAsync()).Count);

        var transactionId = Guid.NewGuid();
        var result = await coordinator.ExecuteAsync(transactionId);
        Check.Equal(SimulatedTransactionState.Complete, result.State);
        Check.SequenceEqual(
            new[]
            {
                SimulatedTransactionState.Idle,
                SimulatedTransactionState.CaptureA,
                SimulatedTransactionState.PersistA,
                SimulatedTransactionState.CaptureB,
                SimulatedTransactionState.PersistB,
                SimulatedTransactionState.Complete,
            },
            result.TransitionHistory);
        Check.Equal(2, result.Originals.Count);
        Check.Equal(1, source.GetCaptureCount("CAM-A"));
        Check.Equal(1, source.GetCaptureCount("CAM-B"));
        Check.Equal(0, result.AutomaticRetryCount);

        foreach (var alias in new[] { "CAM-A", "CAM-B" })
        {
            var path = coordinator.GetOriginalPath(transactionId, alias);
            Check.True(path.EndsWith(".simulated", StringComparison.OrdinalIgnoreCase), "The artifact extension must be simulated.");
            Check.True(File.Exists(path), $"The retained {alias} original should exist.");
            var content = await File.ReadAllTextAsync(path);
            Check.True(content.Contains("Simulated", StringComparison.Ordinal), "The artifact content must be visibly simulated.");
            Check.False(content.Contains("JFIF", StringComparison.Ordinal), "The artifact must not resemble a JPEG payload.");
        }
    });
}

static async Task PartialFailureAndNoRetryAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        var source = new DeterministicSimulatedCaptureSource(failAlias: "CAM-B");
        var coordinator = new DurableSimulatedCaptureCoordinator(root, source);
        await coordinator.InitializeAsync();

        var transactionId = Guid.NewGuid();
        var result = await coordinator.ExecuteAsync(transactionId);
        Check.Equal(SimulatedTransactionState.FailedPartial, result.State);
        Check.Equal("CaptureOrPersistenceFailure", result.TerminalReason);
        Check.Equal(1, result.Originals.Count);
        Check.Equal("CAM-A", result.Originals[0].Alias);
        Check.True(File.Exists(coordinator.GetOriginalPath(transactionId, "CAM-A")), "CAM-A must be retained.");
        Check.False(File.Exists(coordinator.GetOriginalPath(transactionId, "CAM-B")), "CAM-B must not be invented.");
        Check.Equal(1, source.GetCaptureCount("CAM-A"));
        Check.Equal(1, source.GetCaptureCount("CAM-B"));
        Check.Equal(0, result.AutomaticRetryCount);

        await Check.ThrowsAsync<InvalidOperationException>(() => coordinator.ExecuteAsync(transactionId));
        Check.Equal(1, source.GetCaptureCount("CAM-A"));
        Check.Equal(1, source.GetCaptureCount("CAM-B"));
    });
}

static async Task CrashRestartRecoveryAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        var firstSource = new DeterministicSimulatedCaptureSource();
        var firstCoordinator = new DurableSimulatedCaptureCoordinator(root, firstSource);
        await firstCoordinator.InitializeAsync();
        var transactionId = Guid.NewGuid();

        await Check.ThrowsAsync<SimulatedProcessCrashException>(() =>
            firstCoordinator.ExecuteAsync(transactionId, SimulatedCrashPoint.AfterPersistA));
        var interrupted = await firstCoordinator.LoadAsync(transactionId);
        Check.Equal(SimulatedTransactionState.PersistA, interrupted.State);
        Check.Equal(1, interrupted.Originals.Count);
        Check.True(File.Exists(firstCoordinator.GetOriginalPath(transactionId, "CAM-A")), "CAM-A must survive the crash.");

        var restartedSource = new DeterministicSimulatedCaptureSource();
        var restartedCoordinator = new DurableSimulatedCaptureCoordinator(root, restartedSource);
        await Check.ThrowsAsync<InvalidOperationException>(() =>
            restartedCoordinator.ExecuteAsync(Guid.NewGuid()));
        Check.Equal(0, restartedSource.GetCaptureCount("CAM-A"));
        Check.Equal(0, restartedSource.GetCaptureCount("CAM-B"));

        var recovered = await restartedCoordinator.InitializeAsync();
        Check.Equal(1, recovered.Count);
        Check.Equal(SimulatedTransactionState.FailedPartial, recovered[0].State);
        Check.Equal("RestartDetectedIncompleteTransaction", recovered[0].TerminalReason);
        Check.Equal(0, restartedSource.GetCaptureCount("CAM-A"));
        Check.Equal(0, restartedSource.GetCaptureCount("CAM-B"));
        Check.True(File.Exists(restartedCoordinator.GetOriginalPath(transactionId, "CAM-A")), "Recovery must preserve CAM-A.");
        Check.False(File.Exists(restartedCoordinator.GetOriginalPath(transactionId, "CAM-B")), "Recovery must not recapture CAM-B.");

        await Check.ThrowsAsync<InvalidOperationException>(() => restartedCoordinator.ExecuteAsync(transactionId));
        Check.Equal(0, restartedSource.GetCaptureCount("CAM-A"));
        Check.Equal(0, restartedSource.GetCaptureCount("CAM-B"));
    });
}

static async Task WorkflowServiceBoundaryAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        ISimulatedTransactionService service = new SimulationFoundationService(root);
        Check.Equal(0, (await service.InitializeAsync()).Count);

        var state = await service.ExecuteAsync(Guid.NewGuid(), SimulatedWorkflowScenario.FailCaptureB);
        Check.True(state.Simulation, "The application-facing state must be explicitly simulated.");
        Check.Equal("Simulated", state.Marker);
        Check.Equal(SimulatedTransactionState.FailedPartial, state.State);
        Check.True(state.IsTerminal, "A controlled partial failure must be terminal.");
        Check.SequenceEqual(new[] { "CAM-A" }, state.RetainedOriginalAliases);
    });
}

static async Task CrossCoordinatorOwnershipAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        var firstSource = new BlockingSimulatedCaptureSource();
        var secondSource = new DeterministicSimulatedCaptureSource();
        var firstCoordinator = new DurableSimulatedCaptureCoordinator(root, firstSource);
        var secondCoordinator = new DurableSimulatedCaptureCoordinator(root, secondSource);
        await firstCoordinator.InitializeAsync();
        var transactionId = Guid.NewGuid();

        var firstRun = firstCoordinator.ExecuteAsync(transactionId);
        await firstSource.CaptureStarted.WaitAsync(TimeSpan.FromSeconds(5));
        try
        {
            await Check.ThrowsAsync<InvalidOperationException>(() =>
                secondCoordinator.ExecuteAsync(transactionId));
        }
        finally
        {
            firstSource.ReleaseCapture();
        }

        var completed = await firstRun;
        Check.Equal(SimulatedTransactionState.Complete, completed.State);
        Check.Equal(1, firstSource.GetCaptureCount("CAM-A"));
        Check.Equal(1, firstSource.GetCaptureCount("CAM-B"));
        Check.Equal(0, secondSource.GetCaptureCount("CAM-A"));
        Check.Equal(0, secondSource.GetCaptureCount("CAM-B"));
    });
}

static async Task PartialArtifactRecoveryAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        var transactionId = Guid.NewGuid();
        var transactionDirectory = Path.Combine(root, transactionId.ToString("N"));
        Directory.CreateDirectory(transactionDirectory);
        var partialPath = Path.Combine(transactionDirectory, "transaction.json.interrupted.partial");
        await File.WriteAllTextAsync(
            partialPath,
            "Simulated - interrupted journal bytes retained for diagnosis");

        var source = new DeterministicSimulatedCaptureSource();
        var coordinator = new DurableSimulatedCaptureCoordinator(root, source);
        var recovered = await coordinator.InitializeAsync();
        Check.Equal(1, recovered.Count);
        Check.Equal(SimulatedTransactionState.FailedPartial, recovered[0].State);
        Check.Equal("RestartDetectedPartialArtifact", recovered[0].TerminalReason);
        Check.Equal(0, recovered[0].AutomaticRetryCount);
        Check.True(File.Exists(partialPath), "The interrupted partial artifact must remain available for diagnosis.");
        Check.Equal(0, source.GetCaptureCount("CAM-A"));
        Check.Equal(0, source.GetCaptureCount("CAM-B"));

        await Check.ThrowsAsync<InvalidOperationException>(() => coordinator.ExecuteAsync(transactionId));
        Check.Equal(0, source.GetCaptureCount("CAM-A"));
        Check.Equal(0, source.GetCaptureCount("CAM-B"));
    });
}

static async Task WithTemporaryRootAsync(Func<string, Task> test)
{
    var parent = Path.Combine(Path.GetTempPath(), "A0CameraStitcher-M3-FoundationTests");
    var root = Path.Combine(parent, Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        await test(root);
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static class Check
{
    public static void True(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    public static void False(bool condition, string message) => True(!condition, message);

    public static void Equal<T>(T expected, T actual)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
        {
            throw new InvalidOperationException($"Expected '{expected}', actual '{actual}'.");
        }
    }

    public static void SequenceEqual<T>(IEnumerable<T> expected, IEnumerable<T> actual)
    {
        if (!expected.SequenceEqual(actual))
        {
            throw new InvalidOperationException(
                $"Expected sequence '{string.Join(",", expected)}', actual '{string.Join(",", actual)}'.");
        }
    }

    public static void ThrowsProtocol(string expectedErrorCode, Action action)
    {
        try
        {
            action();
        }
        catch (ProtocolViolationException exception)
        {
            Equal(expectedErrorCode, exception.ErrorCode);
            return;
        }

        throw new InvalidOperationException($"Expected protocol error '{expectedErrorCode}'.");
    }

    public static async Task ThrowsAsync<TException>(Func<Task> action)
        where TException : Exception
    {
        try
        {
            await action();
        }
        catch (TException)
        {
            return;
        }

        throw new InvalidOperationException($"Expected exception {typeof(TException).Name}.");
    }
}

sealed class BlockingSimulatedCaptureSource : ISimulatedCaptureSource
{
    private readonly TaskCompletionSource _captureStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly TaskCompletionSource _releaseCapture = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly ConcurrentDictionary<string, int> _captureCounts = new(StringComparer.Ordinal);

    public Task CaptureStarted => _captureStarted.Task;

    public async ValueTask<SimulatedCaptureContent> CaptureAsync(
        string alias,
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        _captureCounts.AddOrUpdate(alias, 1, static (_, count) => count + 1);
        if (alias == "CAM-A")
        {
            _captureStarted.TrySetResult();
            await _releaseCapture.Task.WaitAsync(cancellationToken);
        }

        return new SimulatedCaptureContent(
            $"Simulated{Environment.NewLine}" +
            $"transactionId={transactionId:N}{Environment.NewLine}" +
            $"alias={alias}{Environment.NewLine}" +
            "This is blocking test text, not a camera image.");
    }

    public int GetCaptureCount(string alias) =>
        _captureCounts.TryGetValue(alias, out var count) ? count : 0;

    public void ReleaseCapture() => _releaseCapture.TrySetResult();
}
