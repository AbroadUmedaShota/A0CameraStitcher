using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;

var tests = new (string Name, Func<Task> Run)[]
{
    ("protocol serialization and rejection", ProtocolSerializationAndRejectionAsync),
    ("named pipe fake agent roundtrip", NamedPipeRoundtripAsync),
    ("durable sequential success", DurableSequentialSuccessAsync),
    ("durable sequential pair stability completes 100 of 100", DurableSequentialPairStabilityAsync),
    ("live view stop failure is durable before capture", LiveViewStopFailureIsDurableAsync),
    ("partial failure preserves CAM-A and does not retry", PartialFailureAndNoRetryAsync),
    ("crash restart closes journal without recapture", CrashRestartRecoveryAsync),
    ("WPF-facing service boundary stays simulated", WorkflowServiceBoundaryAsync),
    ("cross-coordinator ownership prevents duplicate capture", CrossCoordinatorOwnershipAsync),
    ("partial artifact restart terminates without capture", PartialArtifactRecoveryAsync),
    ("operator readiness requires safety and classifies correction", OperatorReadinessClassificationAsync),
    ("operator readiness exposes every blocking reason", OperatorReadinessBlockersAsync),
    ("operator action availability locks active workflows", OperatorActionAvailabilityAsync),
    ("capture plans reject implicit or ambiguous camera topology", CapturePlanValidationAsync),
    ("durable single-camera plans capture only the selected alias", DurableSingleCameraSuccessAsync),
    ("single-camera failure is terminal and never retries", SingleCameraFailureAndNoRetryAsync),
    ("single-camera restart preserves the snapshotted plan", SingleCameraCrashRestartAsync),
    ("legacy v1 journal migrates as an explicit dual-camera plan", LegacyJournalMigratesAsDualAsync),
    ("single-camera readiness ignores only the inactive body", SingleCameraReadinessAsync),
    ("continuous hardware Live View v2 validates sessions and JPEG frames", ContinuousHardwareLiveViewV2Async),
    ("dual hardware Agent v2 reserves starts and queries one pair transaction", DualHardwareAgentV2RoundTripAsync),
    ("dual hardware Agent v2 rejects retry capability and mismatched journals", DualHardwareAgentV2NegativesAsync),
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

static async Task DualHardwareAgentV2RoundTripAsync()
{
    var transactionId = Guid.ParseExact("0123456789abcdef0123456789abcdef", "N");
    var startedAtUtc = new DateTimeOffset(2026, 8, 14, 1, 2, 3, TimeSpan.Zero);
    var request = new DualHardwareCaptureRequest(
        transactionId,
        $@"C:\A0CameraStitcher\transactions\{transactionId:N}",
        DualCameraIdentitySnapshot.AnonymousTestSyntheticReady(),
        HardwareDualCaptureProfile.ApprovedSynthetic(),
        DualCameraRigProfile.ApprovedSynthetic(),
        new HardwareDualOperatorConfirmations(true, true, true, true, true),
        startedAtUtc,
        startedAtUtc + TimeSpan.FromSeconds(180));
    var terminal = new DualHardwareCaptureResult(
        transactionId,
        Array.AsReadOnly([
            new DualHardwareOriginalRecord("CAM-A", $@"{request.TransactionDirectory}\CAM-A\original.jpg", true, true),
            new DualHardwareOriginalRecord("CAM-B", $@"{request.TransactionDirectory}\CAM-B\original.jpg", true, true),
        ]),
        DualHardwareCaptureTerminalState.Succeeded,
        DualCameraFailureCode.None,
        new DualHardwareCaptureEvidence(
            DualHardwareCaptureTerminalState.Succeeded,
            request.IdentitySnapshot,
            request.CaptureProfileSnapshot.ProfileId,
            request.CaptureProfileSnapshot.Version,
            request.RigProfileSnapshot.ProfileId,
            request.RigProfileSnapshot.Version,
            request.StartedAtUtc,
            request.WatchdogDeadlineUtc,
            request.StartedAtUtc + TimeSpan.FromSeconds(20),
            true,
            true,
            true,
            true,
            0));
    var operationsSeen = new List<string>();
    var transport = new RecordingHardwareTransport(requestJson =>
    {
        using var document = JsonDocument.Parse(requestJson);
        var root = document.RootElement;
        Check.Equal(DualHardwareCameraAgentProtocol.SchemaVersion, root.GetProperty("schemaVersion").GetString());
        Check.False(root.GetProperty("simulation").GetBoolean(), "Dual hardware Agent requests must never be simulation requests.");
        var requestId = root.GetProperty("requestId").GetString()!;
        var operation = root.GetProperty("operation").GetString()!;
        operationsSeen.Add(operation);
        var payload = root.GetProperty("payload");
        if (operation == "start-reserved-pair")
        {
            var transaction = payload.GetProperty("transaction");
            static string FieldSet(JsonElement element) => string.Join(",",
                element.EnumerateObject().Select(property => property.Name).Order(StringComparer.Ordinal));
            Check.Equal(
                "captureProfileSnapshot,identitySnapshot,operatorConfirmations,rigProfileSnapshot,startedAtUtc,transactionDirectory,transactionId,watchdogDeadlineUtc",
                FieldSet(transaction));
            Check.Equal("expiresAtUtc,observedAtUtc,reasonCode,status",
                FieldSet(transaction.GetProperty("identitySnapshot")));
            Check.False(transaction.GetProperty("identitySnapshot").TryGetProperty("isReady", out _),
                "Computed identity readiness must not cross the Native protocol boundary.");
            Check.Equal("approvedAtUtc,bodies,profileId,schemaVersion,status,validUntilUtc,version",
                FieldSet(transaction.GetProperty("captureProfileSnapshot")));
            Check.Equal(
                "assessedAtUtc,cameraAliases,cameraBToCameraA,crop,expectedInputHeight,expectedInputWidth,layout,measuredAtUtc,profileId,provenance,schemaVersion,status,validUntilUtc,version",
                FieldSet(transaction.GetProperty("rigProfileSnapshot")));
            Check.Equal(
                "bothCardsConfirmedEmpty,captureProfileFrozen,identitySnapshotApproved,liveViewStoppedAndClosed,rigProfileFrozen",
                FieldSet(transaction.GetProperty("operatorConfirmations")));
            Check.False(transaction.GetProperty("operatorConfirmations").TryGetProperty("allConfirmed", out _),
                "Computed confirmation state must not cross the Native protocol boundary.");
            Check.Equal("2026-08-14T01:02:03+00:00", transaction.GetProperty("startedAtUtc").GetString());
            Check.Equal("2026-08-14T01:05:03+00:00", transaction.GetProperty("watchdogDeadlineUtc").GetString());
        }
        return operation switch
        {
            "get-dual-capabilities" => DualHardwareCapabilitiesResponseJson(requestId),
            "reserve-pair-transaction" => DualHardwareResponseJson(requestId, true, "PairTransactionReserved", new
            {
                transactionId = payload.GetProperty("transactionId").GetString(),
                accepted = true,
            }),
            "start-reserved-pair" => DualHardwareResponseJson(requestId, true, "PairDispatchAccepted", new
            {
                transactionId = payload.GetProperty("transaction").GetProperty("transactionId").GetString(),
                dispatchState = "ResponseUnknown",
                result = (object?)null,
            }),
            "get-pair-transaction-result" => DualHardwareResponseJson(requestId, true, "PairTransactionFound", new
            {
                transactionId = payload.GetProperty("transactionId").GetString(),
                found = true,
                result = terminal,
            }),
            "close-reserved-pair-transaction" => DualHardwareResponseJson(
                requestId,
                true,
                "PairTransactionClosedBeforeDispatch",
                new
                {
                    transactionId = payload.GetProperty("transactionId").GetString(),
                    closedBeforeDispatch = true,
                }),
            _ => throw new InvalidOperationException($"Unexpected Dual Agent operation: {operation}"),
        };
    });
    var operations = new DualHardwareCameraAgentOperations(transport);

    Check.True(await operations.ReservePairTransactionAsync(transactionId, default), "The pair reservation must be accepted once.");
    var dispatch = await operations.StartReservedPairAsync(request, default);
    Check.Equal(DualHardwareDispatchState.ResponseUnknown, dispatch.State);
    Check.True(dispatch.Result is null, "A response-unknown dispatch must not forge a terminal result.");
    var queried = await operations.QueryPairTransactionAsync(transactionId, default);
    Check.Equal(DualHardwarePairQueryState.Terminal, queried.State);
    Check.True(queried.Result is not null, "The same transaction query must return its terminal journal.");
    Check.Equal(transactionId, queried.Result!.TransactionId);
    Check.Equal(DualHardwareCaptureTerminalState.Succeeded, queried.Result.TerminalState);
    var closed = await operations.CloseReservedPairTransactionAsync(transactionId, default);
    Check.Equal(DualHardwareCloseState.ClosedBeforeDispatch, closed);

    var closeRequestId = "request-negative-close";
    var rejectedClose = DualHardwareResponseJson(
        closeRequestId,
        false,
        "PairCloseRejected",
        new
        {
            transactionId = transactionId.ToString("N"),
            closedBeforeDispatch = false,
        });
    Check.Throws<HardwareCameraAgentRemoteException>(() =>
        DualHardwareCameraAgentProtocolCodec.DeserializeCloseResponse(
            rejectedClose, closeRequestId, transactionId));
    var mismatchedClose = DualHardwareResponseJson(
        closeRequestId,
        true,
        "PairTransactionClosedBeforeDispatch",
        new
        {
            transactionId = Guid.ParseExact("22222222222222222222222222222222", "N").ToString("N"),
            closedBeforeDispatch = true,
        });
    Check.ThrowsHardwareProtocol("TransactionIdMismatch", () =>
        DualHardwareCameraAgentProtocolCodec.DeserializeCloseResponse(
            mismatchedClose, closeRequestId, transactionId));
    foreach (var forged in new[]
    {
        DualHardwareResponseJson(
            closeRequestId,
            true,
            "PairTransactionClosedBeforeDispatch",
            new
            {
                transactionId = transactionId.ToString("N"),
                closedBeforeDispatch = false,
            }),
        DualHardwareResponseJson(
            closeRequestId,
            true,
            "PairTransactionFound",
            new
            {
                transactionId = transactionId.ToString("N"),
                closedBeforeDispatch = true,
            }),
    })
    {
        Check.ThrowsHardwareProtocol("ForgedPairClose", () =>
            DualHardwareCameraAgentProtocolCodec.DeserializeCloseResponse(
                forged, closeRequestId, transactionId));
    }
    Check.SequenceEqual(
        new[]
        {
            "get-dual-capabilities",
            "reserve-pair-transaction",
            "start-reserved-pair",
            "get-pair-transaction-result",
            "close-reserved-pair-transaction",
        },
        operationsSeen);
}

static async Task DualHardwareAgentV2NegativesAsync()
{
    var transactionId = Guid.ParseExact("11111111111111111111111111111111", "N");
    var retryTransport = new RecordingHardwareTransport(requestJson =>
    {
        using var request = JsonDocument.Parse(requestJson);
        return DualHardwareCapabilitiesResponseJson(
            request.RootElement.GetProperty("requestId").GetString()!,
            automaticRetryCount: 1);
    });
    var retryOperations = new DualHardwareCameraAgentOperations(retryTransport);
    await Check.ThrowsAsync<HardwareProtocolViolationException>(
        () => retryOperations.ReservePairTransactionAsync(transactionId, default));
    Check.Equal(1, retryTransport.RequestCount);

    var requestId = "request-negative-query";
    var anotherTransaction = Guid.ParseExact("22222222222222222222222222222222", "N");
    var mismatched = DualHardwareResponseJson(requestId, true, "PairTransactionFound", new
    {
        transactionId = anotherTransaction.ToString("N"),
        found = true,
        result = (object?)null,
    });
    Check.ThrowsHardwareProtocol(
        "TransactionIdMismatch",
        () => DualHardwareCameraAgentProtocolCodec.DeserializeQueryResponse(
            mismatched,
            requestId,
            transactionId));

    var validNotFound = DualHardwareResponseJson(requestId, false, "PairTransactionNotFound", new
    {
        transactionId = transactionId.ToString("N"),
        found = false,
        result = (object?)null,
    });
    var reserved = DualHardwareResponseJson(requestId, false, "PairTransactionReserved", new
    {
        transactionId = transactionId.ToString("N"),
        found = true,
        result = (object?)null,
    });
    var reservedOutcome = DualHardwareCameraAgentProtocolCodec.DeserializeQueryResponse(
        reserved,
        requestId,
        transactionId);
    Check.Equal(DualHardwarePairQueryState.Reserved, reservedOutcome.State);
    Check.True(reservedOutcome.Result is null, "A Reserved query must not forge a terminal result.");
    var notFoundOutcome = DualHardwareCameraAgentProtocolCodec.DeserializeQueryResponse(
        validNotFound,
        requestId,
        transactionId);
    Check.Equal(DualHardwarePairQueryState.NotFound, notFoundOutcome.State);
    Check.True(notFoundOutcome.Result is null, "A not-found query must not forge a terminal result.");
    var closedQuery = DualHardwareResponseJson(
        requestId,
        true,
        "PairTransactionClosedBeforeDispatch",
        new
        {
            transactionId = transactionId.ToString("N"),
            found = true,
            result = (object?)null,
        });
    var closedOutcome = DualHardwareCameraAgentProtocolCodec.DeserializeQueryResponse(
        closedQuery,
        requestId,
        transactionId);
    Check.Equal(DualHardwarePairQueryState.ClosedBeforeDispatch, closedOutcome.State);
    Check.True(closedOutcome.Result is null, "A close tombstone query must not forge a capture result.");

    var inconsistentReserved = DualHardwareResponseJson(requestId, false, "PairTransactionReserved", new
    {
        transactionId = transactionId.ToString("N"),
        found = false,
        result = (object?)null,
    });
    Check.ThrowsHardwareProtocol(
        "InvalidPairQuery",
        () => DualHardwareCameraAgentProtocolCodec.DeserializeQueryResponse(
            inconsistentReserved,
            requestId,
            transactionId));
    var forgedTerminal = DualHardwareResponseJson(requestId, true, "PairTransactionFound", new
    {
        transactionId = transactionId.ToString("N"),
        found = true,
        result = (object?)null,
    });
    Check.ThrowsHardwareProtocol(
        "InvalidPairQuery",
        () => DualHardwareCameraAgentProtocolCodec.DeserializeQueryResponse(
            forgedTerminal,
            requestId,
            transactionId));
    foreach (var (terminalState, failureCode) in new[]
    {
        (DualHardwareCaptureTerminalState.ResponseUnknown, DualCameraFailureCode.AgentResponseUnknown),
        (DualHardwareCaptureTerminalState.HardwarePending, DualCameraFailureCode.HardwarePending),
    })
    {
        var nonTerminalResult = new DualHardwareCaptureResult(
            transactionId,
            Array.Empty<DualHardwareOriginalRecord>(),
            terminalState,
            failureCode,
            new DualHardwareCaptureEvidence(
                terminalState,
                DualCameraIdentitySnapshot.AnonymousTestSyntheticReady(),
                "anonymous-capture-v2",
                "v2",
                "anonymous-rig-v2",
                "v2",
                DateTimeOffset.UnixEpoch,
                DateTimeOffset.UnixEpoch.AddSeconds(180),
                DateTimeOffset.UnixEpoch.AddSeconds(1),
                false,
                true,
                false,
                false,
                0));
        var forgedNonTerminal = DualHardwareResponseJson(requestId, true, "PairTransactionFound", new
        {
            transactionId = transactionId.ToString("N"),
            found = true,
            result = nonTerminalResult,
        });
        Check.ThrowsHardwareProtocol(
            "InvalidPairQuery",
            () => DualHardwareCameraAgentProtocolCodec.DeserializeQueryResponse(
                forgedNonTerminal,
                requestId,
                transactionId));
    }
    var schemaField = $"\"schemaVersion\":\"{DualHardwareCameraAgentProtocol.SchemaVersion}\"";
    var duplicateField = validNotFound.Replace(
        schemaField,
        $"{schemaField},{schemaField}",
        StringComparison.Ordinal);
    Check.ThrowsHardwareProtocol(
        "DuplicateField",
        () => DualHardwareCameraAgentProtocolCodec.DeserializeQueryResponse(
            duplicateField,
            requestId,
            transactionId));
}

static string DualHardwareCapabilitiesResponseJson(string requestId, int automaticRetryCount = 0) =>
    DualHardwareResponseJson(requestId, true, "DualCapabilities", new
    {
        cameraMode = "DualCamera",
        protocolVersion = 2,
        orderedRequiredAliases = new[] { "CAM-A", "CAM-B" },
        supportedOperations = new[]
        {
            "get-dual-capabilities",
            "reserve-pair-transaction",
            "start-reserved-pair",
            "get-pair-transaction-result",
            "close-reserved-pair-transaction",
        },
        pairJournalDurable = true,
        sameTransactionQueryOnly = true,
        automaticRetryCount,
    });

static string DualHardwareResponseJson(string requestId, bool success, string resultCode, object payload) =>
    JsonSerializer.Serialize(new
    {
        schemaVersion = DualHardwareCameraAgentProtocol.SchemaVersion,
        simulation = false,
        marker = DualHardwareCameraAgentProtocol.Marker,
        requestId,
        success,
        resultCode,
        payload,
    }, new JsonSerializerOptions
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        Converters = { new JsonStringEnumConverter(), new TestTransactionIdConverter() },
    });

static async Task ContinuousHardwareLiveViewV2Async()
{
    var sessionId = new string('a', 32);
    var frame = new byte[] { 0xFF, 0xD8, 0xFF, 0xD9 };
    var frameHash = Convert.ToHexString(SHA256.HashData(frame)).ToLowerInvariant();
    var transport = new RecordingHardwareTransport(requestJson =>
    {
        using var request = JsonDocument.Parse(requestJson);
        var root = request.RootElement;
        var requestId = root.GetProperty("requestId").GetString()!;
        var operation = root.GetProperty("operation").GetString()!;
        var state = operation switch
        {
            "start-live-view" => "Started",
            "read-live-view-frame" => "Frame",
            "live-view-heartbeat" => "Heartbeat",
            "stop-live-view" => "Stopped",
            "close-agent-session" => "Closed",
            _ => throw new InvalidOperationException("unexpected v2 operation"),
        };
        var running = state is "Started" or "Frame" or "Heartbeat";
        return JsonSerializer.Serialize(new
        {
            schemaVersion = "a0.camera-agent.hardware.v2",
            simulation = false,
            marker = "Hardware",
            requestId,
            success = true,
            resultCode = state,
            payload = new
            {
                cameraMode = "SingleCamera",
                cameraAlias = "CAM-A",
                sessionId,
                state,
                frameNumber = state == "Frame" ? 1UL : 0UL,
                frameSize = state == "Frame" ? frame.Length : 0,
                frameSha256 = state == "Frame" ? frameHash : string.Empty,
                frameJpegBase64 = state == "Frame" ? Convert.ToBase64String(frame) : string.Empty,
                previewIsOriginal = false,
                previewIsStitchInput = false,
                sdkSessionOpen = running,
                liveViewRunning = running,
                heartbeatTimeoutSeconds = 20,
                maximumSessionSeconds = 600,
                realIdentifiersIncluded = false,
                errorCategory = string.Empty,
                errorDetail = string.Empty,
            },
        });
    });
    var client = new HardwareContinuousLiveViewClient(transport);
    Check.Equal("Started", (await client.StartAsync(sessionId)).Payload.State);
    var frameReply = await client.ReadFrameAsync(sessionId);
    Check.SequenceEqual(frame, frameReply.Payload.DecodeVerifiedFrame());
    Check.False(frameReply.Payload.PreviewIsOriginal, "Live View frame must never be an original.");
    Check.Equal("Stopped", (await client.StopAsync(sessionId)).Payload.State);
    Check.Equal("Closed", (await client.CloseAsync(sessionId)).Payload.State);
    Check.Equal(4, transport.RequestCount);

    await Check.ThrowsAsync<ArgumentException>(() => client.StartAsync("not-a-session"));

    foreach (var nonCanonicalBase64 in new[]
             {
                 Convert.ToBase64String(frame) + "\r\n",
                 Convert.ToBase64String(frame).Insert(4, " "),
                 Convert.ToBase64String(frame) + "=",
                 "/9j/2Q",
             })
    {
        var nonCanonical = frameReply.Payload with
        {
            FrameJpegBase64 = nonCanonicalBase64,
        };
        Check.Throws<HardwareProtocolViolationException>(
            () => nonCanonical.DecodeVerifiedFrame());
    }
}

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

static Task CapturePlanValidationAsync()
{
    Check.SequenceEqual(new[] { "CAM-A" }, CapturePlan.Single("CAM-A").RequiredCameraAliases);
    Check.SequenceEqual(new[] { "CAM-B" }, CapturePlan.Single("CAM-B").RequiredCameraAliases);
    Check.SequenceEqual(new[] { "CAM-A", "CAM-B" }, CapturePlan.Dual().RequiredCameraAliases);
    Check.Throws<ArgumentException>(() => CapturePlan.Single("CAM-C"));
    Check.Throws<ArgumentException>(() => new CapturePlan
    {
        OperatingMode = CameraOperatingMode.SingleCamera,
        RequiredCameraAliases = ["CAM-A", "CAM-B"],
    }.Validate());
    Check.Throws<ArgumentException>(() => new CapturePlan
    {
        OperatingMode = CameraOperatingMode.DualCamera,
        RequiredCameraAliases = ["CAM-B", "CAM-A"],
    }.Validate());
    return Task.CompletedTask;
}

static async Task DurableSingleCameraSuccessAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        var source = new DeterministicSimulatedCaptureSource();
        var coordinator = new DurableSimulatedCaptureCoordinator(root, source);
        await coordinator.InitializeAsync();

        var transactionA = Guid.NewGuid();
        var resultA = await coordinator.ExecuteAsync(transactionA, CapturePlan.Single("CAM-A"));
        Check.Equal(SimulatedTransactionState.Complete, resultA.State);
        Check.Equal(CameraOperatingMode.SingleCamera, resultA.OperatingMode);
        Check.SequenceEqual(new[] { "CAM-A" }, resultA.RequiredCameraAliases);
        Check.SequenceEqual(
            new[]
            {
                SimulatedTransactionState.Idle,
                SimulatedTransactionState.CaptureA,
                SimulatedTransactionState.PersistA,
                SimulatedTransactionState.Complete,
            },
            resultA.TransitionHistory);
        Check.SequenceEqual(new[] { "CAM-A" }, resultA.Originals.Select(original => original.Alias));

        var transactionB = Guid.NewGuid();
        var resultB = await coordinator.ExecuteAsync(transactionB, CapturePlan.Single("CAM-B"));
        Check.Equal(SimulatedTransactionState.Complete, resultB.State);
        Check.Equal(CameraOperatingMode.SingleCamera, resultB.OperatingMode);
        Check.SequenceEqual(new[] { "CAM-B" }, resultB.RequiredCameraAliases);
        Check.SequenceEqual(
            new[]
            {
                SimulatedTransactionState.Idle,
                SimulatedTransactionState.CaptureB,
                SimulatedTransactionState.PersistB,
                SimulatedTransactionState.Complete,
            },
            resultB.TransitionHistory);
        Check.SequenceEqual(new[] { "CAM-B" }, resultB.Originals.Select(original => original.Alias));

        Check.Equal(1, source.GetCaptureCount("CAM-A"));
        Check.Equal(1, source.GetCaptureCount("CAM-B"));
        Check.False(File.Exists(coordinator.GetOriginalPath(transactionA, "CAM-B")), "Single CAM-A must not invent CAM-B.");
        Check.False(File.Exists(coordinator.GetOriginalPath(transactionB, "CAM-A")), "Single CAM-B must not invent CAM-A.");

        var restarted = new DurableSimulatedCaptureCoordinator(root, new DeterministicSimulatedCaptureSource());
        Check.Equal(0, (await restarted.InitializeAsync()).Count);
        var loadedB = await restarted.LoadAsync(transactionB);
        Check.Equal(CameraOperatingMode.SingleCamera, loadedB.OperatingMode);
        Check.SequenceEqual(new[] { "CAM-B" }, loadedB.RequiredCameraAliases);
    });
}

static async Task SingleCameraFailureAndNoRetryAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        var source = new DeterministicSimulatedCaptureSource(failAlias: "CAM-B");
        var coordinator = new DurableSimulatedCaptureCoordinator(root, source);
        await coordinator.InitializeAsync();
        var transactionId = Guid.NewGuid();

        var result = await coordinator.ExecuteAsync(transactionId, CapturePlan.Single("CAM-B"));
        Check.Equal(SimulatedTransactionState.FailedPartial, result.State);
        Check.Equal(CameraOperatingMode.SingleCamera, result.OperatingMode);
        Check.SequenceEqual(new[] { "CAM-B" }, result.RequiredCameraAliases);
        Check.Equal(0, result.Originals.Count);
        Check.Equal(0, source.GetCaptureCount("CAM-A"));
        Check.Equal(1, source.GetCaptureCount("CAM-B"));
        Check.Equal(0, result.AutomaticRetryCount);

        await Check.ThrowsAsync<InvalidOperationException>(() =>
            coordinator.ExecuteAsync(transactionId, CapturePlan.Single("CAM-B")));
        Check.Equal(1, source.GetCaptureCount("CAM-B"));
    });
}

static async Task SingleCameraCrashRestartAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        var firstSource = new DeterministicSimulatedCaptureSource();
        var firstCoordinator = new DurableSimulatedCaptureCoordinator(root, firstSource);
        await firstCoordinator.InitializeAsync();
        var transactionId = Guid.NewGuid();

        await Check.ThrowsAsync<SimulatedProcessCrashException>(() =>
            firstCoordinator.ExecuteAsync(
                transactionId,
                CapturePlan.Single("CAM-B"),
                SimulatedCrashPoint.AfterPersistA));
        var interrupted = await firstCoordinator.LoadAsync(transactionId);
        Check.Equal(SimulatedTransactionState.PersistB, interrupted.State);
        Check.Equal(CameraOperatingMode.SingleCamera, interrupted.OperatingMode);
        Check.SequenceEqual(new[] { "CAM-B" }, interrupted.RequiredCameraAliases);
        Check.SequenceEqual(new[] { "CAM-B" }, interrupted.Originals.Select(original => original.Alias));

        var restartedSource = new DeterministicSimulatedCaptureSource();
        var restarted = new DurableSimulatedCaptureCoordinator(root, restartedSource);
        var recovered = await restarted.InitializeAsync();
        Check.Equal(1, recovered.Count);
        Check.Equal(SimulatedTransactionState.FailedPartial, recovered[0].State);
        Check.Equal(CameraOperatingMode.SingleCamera, recovered[0].OperatingMode);
        Check.SequenceEqual(new[] { "CAM-B" }, recovered[0].RequiredCameraAliases);
        Check.SequenceEqual(new[] { "CAM-B" }, recovered[0].Originals.Select(original => original.Alias));
        Check.Equal(0, restartedSource.GetCaptureCount("CAM-A"));
        Check.Equal(0, restartedSource.GetCaptureCount("CAM-B"));
    });
}

static async Task LegacyJournalMigratesAsDualAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        var transactionId = Guid.NewGuid();
        var transactionDirectory = Path.Combine(root, transactionId.ToString("N"));
        Directory.CreateDirectory(transactionDirectory);
        var journalPath = Path.Combine(transactionDirectory, "transaction.json");
        var legacyJournal = $$"""
        {
          "schemaVersion": "a0.simulated-transaction.v1",
          "simulation": true,
          "marker": "Simulated",
          "transactionId": "{{transactionId}}",
          "state": "PersistA",
          "transitionHistory": ["Idle", "CaptureA", "PersistA"],
          "originals": [],
          "automaticRetryCount": 0,
          "terminalReason": null,
          "updatedAtUtc": "2026-08-07T00:00:00+00:00"
        }
        """;
        await File.WriteAllTextAsync(journalPath, legacyJournal);

        var source = new DeterministicSimulatedCaptureSource();
        var coordinator = new DurableSimulatedCaptureCoordinator(root, source);
        var recovered = await coordinator.InitializeAsync();

        Check.Equal(1, recovered.Count);
        Check.Equal(SimulatedTransactionState.FailedPartial, recovered[0].State);
        Check.Equal(CameraOperatingMode.DualCamera, recovered[0].OperatingMode);
        Check.SequenceEqual(new[] { "CAM-A", "CAM-B" }, recovered[0].RequiredCameraAliases);
        Check.Equal("RestartDetectedIncompleteTransaction", recovered[0].TerminalReason);
        Check.Equal(0, source.GetCaptureCount("CAM-A"));
        Check.Equal(0, source.GetCaptureCount("CAM-B"));

        var migratedJson = await File.ReadAllTextAsync(journalPath);
        using var migratedDocument = JsonDocument.Parse(migratedJson);
        Check.Equal("DualCamera", migratedDocument.RootElement.GetProperty("operatingMode").GetString());
        Check.SequenceEqual(
            new[] { "CAM-A", "CAM-B" },
            migratedDocument.RootElement.GetProperty("requiredCameraAliases")
                .EnumerateArray()
                .Select(element => element.GetString()!));
    });
}

static Task SingleCameraReadinessAsync()
{
    var today = new DateOnly(2026, 8, 10);
    var snapshot = CreateReadySnapshot(safetyAcknowledged: true) with
    {
        CapturePlan = CapturePlan.Single("CAM-A"),
        Cameras =
        [
            new CameraReadiness("CAM-A", true, true, true, true),
            new CameraReadiness("CAM-B", false, false, false, false),
        ],
        Setup = new SetupAssessment(SetupAssessmentStatus.Ready, "single ready", [], []),
    };

    var notices = OperatorReadinessEvaluator.BuildNotices(snapshot, today);
    Check.False(notices.Any(notice => notice.Code == "CameraMissing"), "Inactive CAM-B must not block Single CAM-A.");
    Check.False(notices.Any(notice => notice.Code == "NoShutterSync"), "Single mode must not display a pair-only warning.");
    Check.True(notices.Any(notice => notice.Code == "SingleCameraOutput"), "Single output provenance must be visible.");
    Check.Equal(OperatorUiState.Ready, OperatorReadinessEvaluator.GetReadyState(snapshot, today));

    var availability = OperatorReadinessEvaluator.Evaluate(snapshot, OperatorUiState.Review, today, true, false);
    Check.True(availability.Export.Allowed, "A reviewed single original must be explicitly exportable.");
    Check.False(availability.Restitch.Allowed, "A single original must not enable restitch.");

    var missingSelected = snapshot with
    {
        Cameras =
        [
            new CameraReadiness("CAM-A", false, false, false, false),
            new CameraReadiness("CAM-B", true, true, true, true),
        ],
    };
    Check.True(
        OperatorReadinessEvaluator.BuildNotices(missingSelected, today).Any(notice => notice.Code == "CameraMissing"),
        "The selected Single camera must remain required.");

    var extraConnected = snapshot with
    {
        Cameras =
        [
            new CameraReadiness("CAM-A", true, true, true, true),
            new CameraReadiness("CAM-B", true, true, true, true),
        ],
    };
    var extraConnectedNotices = OperatorReadinessEvaluator.BuildNotices(extraConnected, today);
    Check.True(
        extraConnectedNotices.Any(notice => notice.Code == "UnexpectedCameraConnected"),
        "Single mode must block an additional connected D810 instead of silently ignoring it.");
    Check.Equal(OperatorUiState.NotReady, OperatorReadinessEvaluator.GetReadyState(extraConnected, today));
    return Task.CompletedTask;
}

static ReadinessSnapshot CreateReadySnapshot(bool safetyAcknowledged) => new()
{
    SafetyAcknowledged = safetyAcknowledged,
    CapturePlan = CapturePlan.Dual(),
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

    HardwareProtocolValidation();

    return Task.CompletedTask;
}

static void HardwareProtocolValidation()
{
    const string requestId = "hardware-contract";
    const string transactionId = "0123456789abcdef0123456789abcdef";
    const string runId = "run-1786300000000-1";
    var hash = new string('a', 64);
    var profileExpiry = new DateTimeOffset(2099, 8, 10, 0, 0, 0, TimeSpan.Zero);
    var profileSnapshot = new HardwareCaptureProfileSnapshot(
        "approved-single-cam-a",
        3,
        new string('b', 64),
        profileExpiry);

    var readinessRequest = HardwareCameraAgentProtocolCodec.CreateReadinessRequest("CAM-A", requestId);
    var readinessRequestJson = HardwareCameraAgentProtocolCodec.SerializeRequest(readinessRequest);
    var readinessRequestRoundTrip = HardwareCameraAgentProtocolCodec.DeserializeRequest(readinessRequestJson);
    Check.False(readinessRequestRoundTrip.Simulation, "Hardware requests must remain simulation=false.");
    Check.Equal(HardwareCameraAgentProtocol.Marker, readinessRequestRoundTrip.Marker);
    Check.Equal("CAM-A", readinessRequestRoundTrip.Payload.GetProperty("cameraAlias").GetString());
    Check.ThrowsProtocol("UnsupportedSchemaVersion", () =>
        CameraAgentProtocolCodec.DeserializeRequest(readinessRequestJson));

    Check.ThrowsHardwareProtocol("InvalidAlias", () =>
        HardwareCameraAgentProtocolCodec.CreateReadinessRequest("CAM-C"));
    Check.ThrowsHardwareProtocol("UnsupportedSchemaVersion", () =>
        HardwareCameraAgentProtocolCodec.DeserializeRequest(readinessRequestJson.Replace(
            HardwareCameraAgentProtocol.SchemaVersion,
            "a0.camera-agent.hardware.v999",
            StringComparison.Ordinal)));
    Check.ThrowsHardwareProtocol("HardwareProtocolRequired", () =>
        HardwareCameraAgentProtocolCodec.DeserializeRequest(readinessRequestJson.Replace(
            "\"simulation\":false",
            "\"simulation\":true",
            StringComparison.Ordinal)));
    Check.ThrowsHardwareProtocol("HardwareMarkerRequired", () =>
        HardwareCameraAgentProtocolCodec.DeserializeRequest(readinessRequestJson.Replace(
            "\"marker\":\"Hardware\"",
            "\"marker\":\"Simulated\"",
            StringComparison.Ordinal)));
    Check.ThrowsHardwareProtocol("UnsupportedOperation", () =>
        HardwareCameraAgentProtocolCodec.DeserializeRequest(readinessRequestJson.Replace(
            "\"operation\":\"get-single-readiness\"",
            "\"operation\":\"capture-pair\"",
            StringComparison.Ordinal)));
    Check.ThrowsHardwareProtocol("DuplicateField", () =>
        HardwareCameraAgentProtocolCodec.DeserializeRequest(readinessRequestJson.Replace(
            "\"requestId\":\"hardware-contract\"",
            "\"requestId\":\"hardware-contract\",\"requestId\":\"hardware-contract\"",
            StringComparison.Ordinal)));

    var requestWithUnknownRoot = readinessRequestJson.Insert(
        readinessRequestJson.Length - 1,
        ",\"unknown\":false");
    Check.ThrowsHardwareProtocol("MalformedEnvelope", () =>
        HardwareCameraAgentProtocolCodec.DeserializeRequest(requestWithUnknownRoot));

    var captureRequest = HardwareCameraAgentProtocolCodec.CreateCaptureRequest(
        transactionId,
        "CAM-A",
        profileSnapshot,
        HardwareCaptureSafetyConfirmations.AllConfirmed,
        liveViewHandoffRequested: false,
        requestId: "hardware-capture");
    var captureRequestJson = HardwareCameraAgentProtocolCodec.SerializeRequest(captureRequest);
    Check.True(
        captureRequestJson.Contains("\"exclusiveCameraControlConfirmed\":true", StringComparison.Ordinal) &&
        captureRequestJson.Contains("\"dedicatedSpoolScopeConfirmed\":true", StringComparison.Ordinal) &&
        captureRequestJson.Contains("\"exactObjectDeleteConfirmed\":true", StringComparison.Ordinal) &&
        captureRequestJson.Contains("\"expectedCaptureProfileId\":\"approved-single-cam-a\"", StringComparison.Ordinal) &&
        captureRequestJson.Contains("\"expectedCaptureProfileVersion\":3", StringComparison.Ordinal) &&
        captureRequestJson.Contains($"\"expectedCaptureProfileSha256\":\"{new string('b', 64)}\"", StringComparison.Ordinal) &&
        captureRequestJson.Contains("\"expectedCaptureProfileExpiresAtUtc\":\"2099-08-10T00:00:00Z\"", StringComparison.Ordinal),
        "Capture serialization must carry every safety confirmation and approved profile snapshot.");
    Check.ThrowsHardwareProtocol("CaptureProfileSnapshotRequired", () =>
        HardwareCameraAgentProtocolCodec.CreateCaptureRequest(
            transactionId,
            "CAM-A",
            HardwareCaptureSafetyConfirmations.AllConfirmed));
    Check.ThrowsHardwareProtocol("SafetyConfirmationRequired", () =>
        HardwareCameraAgentProtocolCodec.CreateCaptureRequest(
            transactionId,
            "CAM-A",
            profileSnapshot,
            new HardwareCaptureSafetyConfirmations(true, false, true),
            liveViewHandoffRequested: false));
    Check.ThrowsHardwareProtocol("InvalidTransactionId", () =>
        HardwareCameraAgentProtocolCodec.CreateCaptureRequest(
            "not-a-transaction",
            "CAM-A",
            profileSnapshot,
            HardwareCaptureSafetyConfirmations.AllConfirmed,
            liveViewHandoffRequested: false));
    Check.ThrowsHardwareProtocol("InvalidCaptureProfileSnapshot", () =>
        HardwareCameraAgentProtocolCodec.CreateCaptureRequest(
            transactionId,
            "CAM-A",
            profileSnapshot with { ExpiresAtUtc = profileExpiry.AddMilliseconds(1) },
            HardwareCaptureSafetyConfirmations.AllConfirmed,
            liveViewHandoffRequested: false));

    var captureWithNonCanonicalExpiry = captureRequestJson.Replace(
        "2099-08-10T00:00:00Z",
        "2099-08-10T00:00:00+00:00",
        StringComparison.Ordinal);
    Check.ThrowsHardwareProtocol("InvalidCaptureProfileSnapshot", () =>
        HardwareCameraAgentProtocolCodec.DeserializeRequest(captureWithNonCanonicalExpiry));
    Check.ThrowsHardwareProtocol("InvalidCaptureProfileSnapshot", () =>
        HardwareCameraAgentProtocolCodec.CreateCaptureRequest(
            transactionId,
            "CAM-A",
            profileSnapshot with
            {
                ExpiresAtUtc = new DateTimeOffset(2020, 1, 1, 0, 0, 0, TimeSpan.Zero),
            },
            HardwareCaptureSafetyConfirmations.AllConfirmed,
            liveViewHandoffRequested: false));

    var captureWithUnknownPayload = captureRequestJson.Replace(
        "\"liveViewHandoffRequested\":false}",
        "\"liveViewHandoffRequested\":false,\"unknown\":true}",
        StringComparison.Ordinal);
    Check.ThrowsHardwareProtocol("InvalidPayload", () =>
        HardwareCameraAgentProtocolCodec.DeserializeRequest(captureWithUnknownPayload));

    Check.ThrowsHardwareProtocol("SafetyConfirmationRequired", () =>
        HardwareCameraAgentProtocolCodec.CreateLiveViewProbeRequest(
            "CAM-A",
            new HardwareLiveViewSafetyConfirmation(false)));
    Check.ThrowsHardwareProtocol("InvalidLiveViewRequest", () =>
        HardwareCameraAgentProtocolCodec.CreateLiveViewProbeRequest(
            "CAM-A",
            HardwareLiveViewSafetyConfirmation.Confirmed,
            frames: 31));
    var liveViewRequest = HardwareCameraAgentProtocolCodec.CreateLiveViewProbeRequest(
        "CAM-B",
        HardwareLiveViewSafetyConfirmation.Confirmed,
        frames: 3,
        intervalMs: 25,
        requestId: "hardware-live-view");
    var liveViewRequestRoundTrip = HardwareCameraAgentProtocolCodec.DeserializeRequest(
        HardwareCameraAgentProtocolCodec.SerializeRequest(liveViewRequest));
    Check.Equal(3, liveViewRequestRoundTrip.Payload.GetProperty("liveViewFrames").GetInt32());
    Check.Equal(25, liveViewRequestRoundTrip.Payload.GetProperty("liveViewIntervalMs").GetInt32());

    var transactionRequest = HardwareCameraAgentProtocolCodec.CreateTransactionResultRequest(
        transactionId,
        "hardware-transaction-result");
    var transactionRequestRoundTrip = HardwareCameraAgentProtocolCodec.DeserializeRequest(
        HardwareCameraAgentProtocolCodec.SerializeRequest(transactionRequest));
    Check.Equal(HardwareCameraAgentProtocol.Operations.GetTransactionResult, transactionRequestRoundTrip.Operation);
    Check.Equal(transactionId, transactionRequestRoundTrip.Payload.GetProperty("transactionId").GetString());

    var ready = new HardwareSingleReadinessResult
    {
        CameraMode = "SingleCamera",
        CameraAlias = "CAM-A",
        Ready = true,
        SdkCameraCount = 1,
        WpdCameraCount = 1,
        SdkIdentityBound = true,
        WpdIdentityBound = true,
        SdkAliasMatches = true,
        WpdAliasMatches = true,
        SdkStatusProbed = true,
        SpoolInspected = true,
        SpoolPayloadObjectCount = 0,
        SpoolKnownEmpty = true,
        Firmware = "1.11",
        LiveViewStatus = "off",
        LiveViewStatusAvailable = true,
        CaptureProfileApproved = true,
        CaptureProfileId = "approved-single-cam-a",
        CaptureProfileVersion = 3,
        CaptureProfileSha256 = new string('b', 64),
        CaptureProfileCameraAlias = "CAM-A",
        CaptureProfileExpiresAtUtc = profileExpiry,
        CaptureProfileAliasMatches = true,
        SettingsMatchApprovedProfile = true,
        ObservedSettings = CreateObservedHardwareSettings(),
        ReadOnly = true,
        CaptureCommandSent = false,
        CameraObjectDeleteAttempted = false,
        CameraSettingsChanged = false,
        RealIdentifiersIncluded = false,
        FailureCategory = "",
        FailureDetail = "",
    };
    var readinessResponseJson = HardwareResponseJson(requestId, true, "SingleReady", ready);
    var readinessReply = HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
        readinessResponseJson,
        requestId,
        "CAM-A");
    Check.True(readinessReply.Payload.Ready, "Typed hardware readiness should accept complete one-camera evidence.");
    var forgedLiveViewOnReady = ready with { LiveViewStatus = "on" };
    Check.ThrowsHardwareProtocol("ForgedReadinessResult", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            HardwareResponseJson(requestId, true, "SingleReady", forgedLiveViewOnReady),
            requestId,
            "CAM-A"));
    var liveViewOnNotReady = ready with
    {
        Ready = false,
        LiveViewStatus = "on",
        FailureCategory = "live_view_not_off",
        FailureDetail = "SDK Live View must be off before the WPD baseline.",
    };
    Check.False(HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
        HardwareResponseJson(requestId, true, "SingleNotReady", liveViewOnNotReady),
        requestId,
        "CAM-A").Payload.Ready, "Live View ON must remain a typed not-ready condition.");
    var liveViewUnknownNotReady = ready with
    {
        Ready = false,
        LiveViewStatus = "",
        LiveViewStatusAvailable = false,
        FailureCategory = "live_view_status_unavailable",
        FailureDetail = "SDK Live View status is unavailable.",
    };
    Check.False(HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
        HardwareResponseJson(requestId, true, "SingleNotReady", liveViewUnknownNotReady),
        requestId,
        "CAM-A").Payload.Ready, "Unavailable Live View status must remain a typed not-ready condition.");
    var extraCameraNotReady = ready with
    {
        Ready = false,
        SdkCameraCount = 2,
        WpdCameraCount = 2,
        SdkIdentityBound = false,
        WpdIdentityBound = false,
        SdkAliasMatches = false,
        WpdAliasMatches = false,
        SdkStatusProbed = false,
        SpoolInspected = false,
        SpoolKnownEmpty = false,
        FailureCategory = "camera_count_mismatch",
        FailureDetail = "single-camera operation requires exactly one body",
    };
    var extraCameraReply = HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
        HardwareResponseJson(requestId, true, "SingleNotReady", extraCameraNotReady),
        requestId,
        "CAM-A");
    Check.False(extraCameraReply.Payload.Ready, "A second connected body must remain an explicit hardware blocker.");
    Check.ThrowsProtocol("UnsupportedSchemaVersion", () =>
        CameraAgentProtocolCodec.DeserializeResponse(readinessResponseJson));
    Check.ThrowsHardwareProtocol("UnsupportedSchemaVersion", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            readinessResponseJson.Replace(
                HardwareCameraAgentProtocol.SchemaVersion,
                "a0.camera-agent.hardware.v999",
                StringComparison.Ordinal),
            requestId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("MalformedEnvelope", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            readinessResponseJson.Insert(readinessResponseJson.Length - 1, ",\"unknown\":true"),
            requestId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("InvalidPayload", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            readinessResponseJson.Replace(
                "\"currentLabel\":null",
                "\"currentLabel\":null,\"unknownSettingField\":true",
                StringComparison.Ordinal),
            requestId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("InvalidAlias", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            HardwareResponseJson(requestId, true, "SingleReady", ready with { CameraAlias = "CAM-C" }),
            requestId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("UnsafeReadinessResult", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            HardwareResponseJson(requestId, true, "SingleReady", ready with { RealIdentifiersIncluded = true }),
            requestId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("ForgedReadinessResult", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            HardwareResponseJson(requestId, true, "SingleReady", ready with { SdkCameraCount = 2 }),
            requestId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("ForgedReadinessResult", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            HardwareResponseJson(
                requestId,
                true,
                "SingleReady",
                ready with
                {
                    CaptureProfileCameraAlias = "CAM-B",
                    CaptureProfileAliasMatches = false,
                    SettingsMatchApprovedProfile = false,
                }),
            requestId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("ForgedReadinessResult", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            HardwareResponseJson(
                requestId,
                true,
                "SingleReady",
                ready with
                {
                    CaptureProfileExpiresAtUtc =
                        new DateTimeOffset(2020, 1, 1, 0, 0, 0, TimeSpan.Zero),
                }),
            requestId,
            "CAM-A"));
    var invalidProfileUtcJson = HardwareResponseJson(requestId, true, "SingleReady", ready)
        .Replace(
            "2099-08-10T00:00:00Z",
            "2099-08-10T00:00:00+00:00",
            StringComparison.Ordinal);
    Check.ThrowsHardwareProtocol("InvalidPayload", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            invalidProfileUtcJson,
            requestId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("ForgedReadinessResult", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            HardwareResponseJson(
                requestId,
                true,
                "SingleReady",
                ready with
                {
                    CaptureProfileApproved = false,
                    CaptureProfileId = "",
                    CaptureProfileVersion = 0,
                    CaptureProfileSha256 = "",
                    CaptureProfileCameraAlias = "",
                    CaptureProfileExpiresAtUtc = null,
                    CaptureProfileAliasMatches = false,
                    SettingsMatchApprovedProfile = false,
                }),
            requestId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("InvalidReadinessResult", () =>
        HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            HardwareResponseJson(
                requestId,
                true,
                "SingleReady",
                ready with
                {
                    ObservedSettings = ready.ObservedSettings with
                    {
                        CompressionLevel = ready.ObservedSettings.CompressionLevel with
                        {
                            Available = false,
                        },
                    },
                }),
            requestId,
            "CAM-A"));

    var original = new HardwareRetainedOriginalRecord
    {
        CameraAlias = "CAM-A",
        Path = @"C:\A0CameraStitcher\artifacts\run-1786300000000-1\hybrid-tx-1786300000001-1\CAM-A\original.jpg",
        SizeBytes = 4096,
        Sha256 = hash,
    };
    var capture = new HardwareSingleCaptureResult
    {
        CameraMode = "SingleCamera",
        CameraAlias = "CAM-A",
        RequiredCameraAlias = "CAM-A",
        RunId = runId,
        TransactionId = transactionId,
        CaptureProfileId = "approved-single-cam-a",
        CaptureProfileVersion = 3,
        CaptureProfileSha256 = new string('b', 64),
        CaptureProfileCameraAlias = "CAM-A",
        CaptureProfileExpiresAtUtc = profileExpiry,
        TerminalState = "Complete",
        ErrorCategory = "",
        ErrorDetail = "",
        RetainedOriginal = original,
        LiveViewHandoffRequested = false,
        LiveViewStoppedBeforeCapture = false,
        LiveViewSdkSessionClosedBeforeCapture = false,
        PostCaptureLiveViewProbeAttempted = false,
        PostCaptureLiveViewProbeSucceeded = false,
        PostCapturePreview = null,
        SpoolEmptyBeforeCapture = true,
        CameraObjectDeleteAttempted = true,
        CameraObjectDeleteSucceeded = true,
        SpoolEmptyAfterCleanup = true,
        AutomaticRetryCount = 0,
        TransactionWatchdogSeconds = 180,
        RealIdentifiersIncluded = false,
    };
    var captureResponseJson = HardwareResponseJson("capture-response", true, "CaptureComplete", capture);
    var captureReply = HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
        captureResponseJson,
        "capture-response",
        transactionId,
        "CAM-A",
        profileSnapshot,
        expectedLiveViewHandoffRequested: false);
    Check.Equal(original.Path, captureReply.Payload.RetainedOriginal?.Path);

    var historicalExpiry = new DateTimeOffset(2020, 1, 1, 0, 0, 0, TimeSpan.Zero);
    var historicalProfile = profileSnapshot with { ExpiresAtUtc = historicalExpiry };
    var historicalReply = HardwareCameraAgentProtocolCodec.DeserializeTransactionResultResponse(
        HardwareResponseJson(
            "historical-capture",
            true,
            "CaptureComplete",
            capture with { CaptureProfileExpiresAtUtc = historicalExpiry }),
        "historical-capture",
        transactionId,
        "CAM-A",
        historicalProfile,
        expectedLiveViewHandoffRequested: false);
    Check.Equal(
        "Complete",
        historicalReply.Payload.TerminalState);

    Check.ThrowsHardwareProtocol("CaptureProfileSnapshotMismatch", () =>
        HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
            HardwareResponseJson(
                "capture-response",
                true,
                "CaptureComplete",
                capture with { CaptureProfileSha256 = new string('c', 64) }),
            "capture-response",
            transactionId,
            "CAM-A",
            profileSnapshot,
            expectedLiveViewHandoffRequested: false));

    var profileSnapshotMismatch = capture with
    {
        CaptureProfileId = "replacement-single-cam-a",
        CaptureProfileVersion = 4,
        CaptureProfileSha256 = new string('c', 64),
        TerminalState = "Blocked",
        ErrorCategory = "capture_profile_snapshot_mismatch",
        ErrorDetail = "approved capture profile changed since readiness",
        RetainedOriginal = null,
        SpoolEmptyBeforeCapture = false,
        CameraObjectDeleteAttempted = false,
        CameraObjectDeleteSucceeded = false,
        SpoolEmptyAfterCleanup = false,
    };
    var mismatchReply = HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
        HardwareResponseJson(
            "capture-profile-mismatch",
            false,
            "capture_profile_snapshot_mismatch",
            profileSnapshotMismatch),
        "capture-profile-mismatch",
        transactionId,
        "CAM-A",
        profileSnapshot,
        expectedLiveViewHandoffRequested: false);
    Check.Equal("Blocked", mismatchReply.Payload.TerminalState);
    Check.ThrowsHardwareProtocol("ForgedCaptureProfileMismatch", () =>
        HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
            HardwareResponseJson(
                "forged-profile-mismatch",
                false,
                "capture_profile_snapshot_mismatch",
                profileSnapshotMismatch with
                {
                    CaptureProfileId = profileSnapshot.ProfileId,
                    CaptureProfileVersion = profileSnapshot.ProfileVersion,
                    CaptureProfileSha256 = profileSnapshot.Sha256,
                }),
            "forged-profile-mismatch",
            transactionId,
            "CAM-A",
            profileSnapshot,
            expectedLiveViewHandoffRequested: false));

    Check.ThrowsHardwareProtocol("InvalidVerifiedJpeg", () =>
        HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
            HardwareResponseJson(
                "capture-response",
                true,
                "CaptureComplete",
                capture with { RetainedOriginal = original with { Sha256 = hash.ToUpperInvariant() } }),
            "capture-response",
            transactionId,
            "CAM-A"));
    foreach (var invalidPath in new[]
             {
                 @"artifacts\CAM-A\original.jpg",
                 @"\\server\share\CAM-A\original.jpg",
                 @"C:\artifacts\..\outside\original.jpg",
             })
    {
        Check.ThrowsHardwareProtocol("InvalidArtifactPath", () =>
            HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
                HardwareResponseJson(
                    "capture-response",
                    true,
                    "CaptureComplete",
                    capture with { RetainedOriginal = original with { Path = invalidPath } }),
                "capture-response",
                transactionId,
                "CAM-A"));
    }

    Check.ThrowsHardwareProtocol("ForgedCaptureSuccess", () =>
        HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
            HardwareResponseJson(
                "capture-response",
                true,
                "CaptureComplete",
                capture with { CameraObjectDeleteSucceeded = false }),
            "capture-response",
            transactionId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("RequiredCameraAliasMismatch", () =>
        HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
            HardwareResponseJson(
                "capture-response",
                true,
                "CaptureComplete",
                capture with { RequiredCameraAlias = "CAM-B" }),
            "capture-response",
            transactionId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("RequestIdMismatch", () =>
        HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
            captureResponseJson,
            "different-request",
            transactionId,
            "CAM-A"));
    Check.ThrowsHardwareProtocol("TransactionIdMismatch", () =>
        HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
            captureResponseJson,
            "capture-response",
            "ffffffffffffffffffffffffffffffff",
            "CAM-A"));

    var inProgress = capture with
    {
        TerminalState = "InProgress",
        ErrorCategory = "transaction_in_progress",
        ErrorDetail = "query again without resubmitting capture",
        RetainedOriginal = null,
        SpoolEmptyBeforeCapture = false,
        CameraObjectDeleteAttempted = false,
        CameraObjectDeleteSucceeded = false,
        SpoolEmptyAfterCleanup = false,
    };
    var inProgressReply = HardwareCameraAgentProtocolCodec.DeserializeTransactionResultResponse(
        HardwareResponseJson("lookup-progress", false, "TransactionInProgress", inProgress),
        "lookup-progress",
        transactionId);
    Check.False(inProgressReply.Success, "An active transaction query must not masquerade as capture success.");
    Check.Equal("InProgress", inProgressReply.Payload.TerminalState);

    var reserved = inProgress with
    {
        TerminalState = "Reserved",
        ErrorCategory = "transaction_reserved",
        ErrorDetail = "transaction owner is active before camera access",
    };
    var reservedReply = HardwareCameraAgentProtocolCodec.DeserializeTransactionResultResponse(
        HardwareResponseJson("lookup-reserved", false, "TransactionReserved", reserved),
        "lookup-reserved",
        transactionId);
    Check.Equal("Reserved", reservedReply.Payload.TerminalState);

    var notFound = inProgress with
    {
        CameraAlias = "",
        RequiredCameraAlias = "",
        RunId = "",
        CaptureProfileId = "",
        CaptureProfileVersion = 0,
        CaptureProfileSha256 = "",
        CaptureProfileCameraAlias = "",
        CaptureProfileExpiresAtUtc = null,
        TerminalState = "Blocked",
        ErrorCategory = "transaction_not_found",
        ErrorDetail = "no durable transaction exists",
    };
    var notFoundReply = HardwareCameraAgentProtocolCodec.DeserializeTransactionResultResponse(
        HardwareResponseJson("lookup-missing", false, "TransactionNotFound", notFound),
        "lookup-missing",
        transactionId);
    Check.Equal("TransactionNotFound", notFoundReply.ResultCode);

    var incompleteReservation = notFound with
    {
        TerminalState = "FailedPartial",
        ErrorCategory = "transaction_reservation_incomplete",
        ErrorDetail = "reserved transaction has no committed journal",
    };
    var incompleteReply = HardwareCameraAgentProtocolCodec.DeserializeTransactionResultResponse(
        HardwareResponseJson(
            "lookup-incomplete",
            false,
            "transaction_reservation_incomplete",
            incompleteReservation),
        "lookup-incomplete",
        transactionId,
        "CAM-A",
        profileSnapshot,
        expectedLiveViewHandoffRequested: true);
    Check.Equal("FailedPartial", incompleteReply.Payload.TerminalState);

    Check.ThrowsHardwareProtocol("CameraAliasMismatch", () =>
        HardwareCameraAgentProtocolCodec.DeserializeTransactionResultResponse(
            HardwareResponseJson(
                "lookup-wrong-alias",
                false,
                "TransactionInProgress",
                inProgress with
                {
                    CameraAlias = "CAM-B",
                    RequiredCameraAlias = "CAM-B",
                    CaptureProfileCameraAlias = "CAM-B",
                }),
            "lookup-wrong-alias",
            transactionId,
            "CAM-A",
            profileSnapshot,
            expectedLiveViewHandoffRequested: false));

    Check.ThrowsHardwareProtocol("CaptureProfileCameraAliasMismatch", () =>
        HardwareCameraAgentProtocolCodec.DeserializeTransactionResultResponse(
            HardwareResponseJson(
                "lookup-wrong-profile-alias",
                false,
                "TransactionInProgress",
                inProgress with { CaptureProfileCameraAlias = "CAM-B" }),
            "lookup-wrong-profile-alias",
            transactionId,
            "CAM-A",
            profileSnapshot,
            expectedLiveViewHandoffRequested: false));

    var preview = new HardwarePreviewJpegRecord
    {
        Path = @"C:\A0CameraStitcher\artifacts\run-1786300000000-1\live-view\CAM-A\preview.jpg",
        SizeBytes = 2048,
        Sha256 = hash,
    };
    var handoffCapture = capture with
    {
        LiveViewHandoffRequested = true,
        LiveViewStoppedBeforeCapture = true,
        LiveViewSdkSessionClosedBeforeCapture = true,
        PostCaptureLiveViewProbeAttempted = true,
        PostCaptureLiveViewProbeSucceeded = true,
        PostCapturePreview = preview,
    };
    var handoffReply = HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
        HardwareResponseJson("handoff-capture", true, "CaptureComplete", handoffCapture),
        "handoff-capture",
        transactionId,
        "CAM-A",
        profileSnapshot,
        expectedLiveViewHandoffRequested: true);
    Check.True(
        handoffReply.Payload.PostCaptureLiveViewProbeSucceeded,
        "Successful handoff must prove a finite post-capture probe, not a continuing stream.");
    Check.ThrowsHardwareProtocol("LiveViewHandoffMismatch", () =>
        HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
            HardwareResponseJson("handoff-mismatch", true, "CaptureComplete", handoffCapture),
            "handoff-mismatch",
            transactionId,
            "CAM-A",
            profileSnapshot,
            expectedLiveViewHandoffRequested: false));

    var failedPostCaptureProbe = handoffCapture with
    {
        TerminalState = "FailedPartial",
        ErrorCategory = "post_capture_live_view_probe_failed",
        ErrorDetail = "finite post-capture Live View probe failed closed",
        PostCaptureLiveViewProbeSucceeded = false,
        PostCapturePreview = null,
    };
    var failedPostProbeReply = HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
        HardwareResponseJson(
            "handoff-post-probe-failed",
            false,
            "post_capture_live_view_probe_failed",
            failedPostCaptureProbe),
        "handoff-post-probe-failed",
        transactionId,
        "CAM-A",
        profileSnapshot,
        expectedLiveViewHandoffRequested: true);
    Check.Equal("FailedPartial", failedPostProbeReply.Payload.TerminalState);

    var liveView = new HardwareSingleLiveViewResult
    {
        CameraMode = "SingleCamera",
        CameraAlias = "CAM-A",
        RunId = runId,
        Frames = 3,
        LastFrameBytes = preview.SizeBytes,
        LastFrameSha256 = preview.Sha256,
        DurationMs = 240,
        PreviewPersisted = true,
        Preview = preview,
        PreviewIsOriginal = false,
        PreviewIsStitchInput = false,
        LiveViewStopped = true,
        SdkSessionClosed = true,
        RealIdentifiersIncluded = false,
        ErrorCategory = "",
        ErrorDetail = "",
    };
    var liveViewReply = HardwareCameraAgentProtocolCodec.DeserializeLiveViewResponse(
        HardwareResponseJson("live-view-response", true, "LiveViewProbeComplete", liveView),
        "live-view-response",
        "CAM-A",
        expectedFrames: 3);
    Check.False(liveViewReply.Payload.PreviewIsOriginal, "Preview provenance must remain non-original.");
    var previewPersistenceFailure = liveView with
    {
        PreviewPersisted = false,
        Preview = null,
        ErrorCategory = "live_view_exception",
        ErrorDetail = "verified preview persistence failed closed",
    };
    var previewFailureReply = HardwareCameraAgentProtocolCodec.DeserializeLiveViewResponse(
        HardwareResponseJson(
            "live-view-preview-failure",
            false,
            "live_view_exception",
            previewPersistenceFailure),
        "live-view-preview-failure",
        "CAM-A",
        expectedFrames: 3);
    Check.False(
        previewFailureReply.Success,
        "A failed preview persistence may retain frame hash and closed-session evidence without claiming success.");
    Check.ThrowsHardwareProtocol("ForgedLiveViewSuccess", () =>
        HardwareCameraAgentProtocolCodec.DeserializeLiveViewResponse(
            HardwareResponseJson("live-view-response", true, "LiveViewProbeComplete", liveView),
            "live-view-response",
            "CAM-A",
            expectedFrames: 2));
    Check.ThrowsHardwareProtocol("InvalidVerifiedJpeg", () =>
        HardwareCameraAgentProtocolCodec.DeserializeLiveViewResponse(
            HardwareResponseJson(
                "live-view-response",
                true,
                "LiveViewProbeComplete",
                liveView with { Preview = preview with { Sha256 = new string('F', 64) } }),
            "live-view-response",
            "CAM-A",
            expectedFrames: 3));
    Check.ThrowsHardwareProtocol("UnsafeLiveViewResult", () =>
        HardwareCameraAgentProtocolCodec.DeserializeLiveViewResponse(
            HardwareResponseJson(
                "live-view-response",
                true,
                "LiveViewProbeComplete",
                liveView with { PreviewIsOriginal = true }),
            "live-view-response",
            "CAM-A",
            expectedFrames: 3));
    Check.ThrowsHardwareProtocol("ForgedLiveViewSuccess", () =>
        HardwareCameraAgentProtocolCodec.DeserializeLiveViewResponse(
            HardwareResponseJson(
                "live-view-response",
                true,
                "LiveViewProbeComplete",
                liveView with { SdkSessionClosed = false }),
            "live-view-response",
            "CAM-A",
            expectedFrames: 3));

    var remoteFailure = new HardwareCameraAgentFailure
    {
        CameraAccess = "None",
        RejectionCode = "SafetyConfirmationRequired",
        ErrorDetail = "all capture confirmations are required",
        RealIdentifiersIncluded = false,
    };
    try
    {
        HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
            HardwareResponseJson("capture-rejected", false, "ProtocolRejected", remoteFailure),
            "capture-rejected",
            transactionId,
            "CAM-A");
        throw new InvalidOperationException("A protocol rejection must surface as a typed remote exception.");
    }
    catch (HardwareCameraAgentRemoteException exception)
    {
        Check.Equal("ProtocolRejected", exception.ResultCode);
        Check.Equal("SafetyConfirmationRequired", exception.RejectionCode);
    }
}

static string HardwareResponseJson<T>(
    string requestId,
    bool success,
    string resultCode,
    T payload) =>
    JsonSerializer.Serialize(
        new
        {
            schemaVersion = HardwareCameraAgentProtocol.SchemaVersion,
            simulation = false,
            marker = HardwareCameraAgentProtocol.Marker,
            requestId,
            success,
            resultCode,
            payload,
        },
        new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });

static HardwareObservedCameraSettings CreateObservedHardwareSettings() => new()
{
    FileType = new HardwareObservedCameraSetting
    {
        Available = false,
        CapType = "unsupported",
        ProbeState = "not-advertised",
        ValueType = "unsupported",
        CurrentValue = null,
        CurrentIndex = null,
        CurrentLabel = null,
    },
    CompressionLevel = ObservedLabelSetting("JPEG Fine", 2),
    ImageSize = ObservedLabelSetting("L(7360*4912)", 0),
    ExposureMode = ObservedNumericSetting(3, 3),
    ShutterSpeed = ObservedLabelSetting("1/6", 26),
    Aperture = ObservedLabelSetting("8", 9),
    Sensitivity = ObservedLabelSetting("64", 3),
    WhiteBalanceMode = ObservedLabelSetting("Preset 1", 7),
    FocusMode = ObservedNumericSetting(1, null),
};

static HardwareObservedCameraSetting ObservedLabelSetting(string label, uint index) => new()
{
    Available = true,
    CapType = "enum",
    ProbeState = "available",
    ValueType = "packed-string",
    CurrentValue = null,
    CurrentIndex = index,
    CurrentLabel = label,
};

static HardwareObservedCameraSetting ObservedNumericSetting(uint value, uint? index) => new()
{
    Available = true,
    CapType = "unsigned",
    ProbeState = "available",
    ValueType = "unsigned",
    CurrentValue = value,
    CurrentIndex = index,
    CurrentLabel = null,
};

static async Task HardwareNamedPipeRoundtripAsync()
{
    var pipeName = $"a0-camera-stitcher-hardware-{Guid.NewGuid():N}";
    var serverTask = ServeHardwareReadinessOnceAsync(pipeName);
    var client = new HardwareCameraAgentClient(
        pipeName,
        connectTimeout: TimeSpan.FromSeconds(5),
        responseTimeout: TimeSpan.FromSeconds(5));

    var reply = await client.GetSingleReadinessAsync("CAM-B");
    Check.True(reply.Success && reply.Payload.Ready, "The typed hardware client must accept a valid local pipe response.");
    Check.Equal("CAM-B", reply.Payload.CameraAlias);
    await serverTask;

    await HardwareConnectFailureIsTypedAsync();
    await HardwarePostDispatchCancellationIsNotConnectFailureAsync();
}

static async Task HardwareConnectFailureIsTypedAsync()
{
    var pipeName = $"a0-camera-stitcher-hardware-missing-{Guid.NewGuid():N}";
    var transport = new NamedPipeHardwareCameraAgentTransport(
        pipeName,
        connectTimeout: TimeSpan.FromSeconds(5),
        responseTimeout: TimeSpan.FromSeconds(5));
    using var cancellationSource = new CancellationTokenSource();
    cancellationSource.Cancel();

    try
    {
        await transport.SendAsync("{}", cancellationSource.Token);
        throw new InvalidOperationException("A cancelled pre-dispatch connection must fail.");
    }
    catch (HardwareCameraAgentConnectException exception)
    {
        Check.Equal(pipeName, exception.PipeName);
        Check.True(
            exception.CallerCancellationRequested,
            "The typed connect failure must preserve caller-cancellation context.");
        Check.True(
            exception.InnerException is OperationCanceledException,
            "The typed connect failure must preserve its transport cause.");
    }
}

static async Task HardwarePostDispatchCancellationIsNotConnectFailureAsync()
{
    var pipeName = $"a0-camera-stitcher-hardware-dispatched-{Guid.NewGuid():N}";
    var requestReceived = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
    var releaseServer = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
    var serverTask = HoldHardwarePipeAfterRequestAsync(
        pipeName,
        requestReceived,
        releaseServer);
    var transport = new NamedPipeHardwareCameraAgentTransport(
        pipeName,
        connectTimeout: TimeSpan.FromSeconds(5),
        responseTimeout: TimeSpan.FromSeconds(5));
    using var cancellationSource = new CancellationTokenSource();
    var responseTask = transport.SendAsync("{}", cancellationSource.Token);

    await requestReceived.Task.WaitAsync(TimeSpan.FromSeconds(5));
    cancellationSource.Cancel();
    try
    {
        await responseTask;
        throw new InvalidOperationException("A cancelled dispatched request must fail.");
    }
    catch (HardwareCameraAgentConnectException)
    {
        throw new InvalidOperationException(
            "A post-dispatch cancellation must never authorize connect-failure cleanup.");
    }
    catch (OperationCanceledException)
    {
        // Expected: only pre-dispatch connection failures receive the lifecycle-safe wrapper.
    }
    finally
    {
        releaseServer.TrySetResult(true);
        await serverTask;
    }
}

static async Task HoldHardwarePipeAfterRequestAsync(
    string pipeName,
    TaskCompletionSource<bool> requestReceived,
    TaskCompletionSource<bool> releaseServer)
{
    await using var pipe = new NamedPipeServerStream(
        pipeName,
        PipeDirection.InOut,
        maxNumberOfServerInstances: 1,
        PipeTransmissionMode.Byte,
        PipeOptions.Asynchronous);
    using var timeoutSource = new CancellationTokenSource(TimeSpan.FromSeconds(5));
    await pipe.WaitForConnectionAsync(timeoutSource.Token);
    _ = await ReadTestPipeFrameAsync(pipe, timeoutSource.Token);
    requestReceived.TrySetResult(true);
    await releaseServer.Task.WaitAsync(timeoutSource.Token);
}

static async Task ServeHardwareReadinessOnceAsync(string pipeName)
{
    await using var pipe = new NamedPipeServerStream(
        pipeName,
        PipeDirection.InOut,
        maxNumberOfServerInstances: 1,
        PipeTransmissionMode.Byte,
        PipeOptions.Asynchronous);
    using var timeoutSource = new CancellationTokenSource(TimeSpan.FromSeconds(5));
    await pipe.WaitForConnectionAsync(timeoutSource.Token);
    var requestJson = await ReadTestPipeFrameAsync(pipe, timeoutSource.Token);
    var request = HardwareCameraAgentProtocolCodec.DeserializeRequest(requestJson);
    Check.Equal(HardwareCameraAgentProtocol.Operations.GetSingleReadiness, request.Operation);
    Check.Equal("CAM-B", request.Payload.GetProperty("cameraAlias").GetString());

    var ready = new HardwareSingleReadinessResult
    {
        CameraMode = "SingleCamera",
        CameraAlias = "CAM-B",
        Ready = true,
        SdkCameraCount = 1,
        WpdCameraCount = 1,
        SdkIdentityBound = true,
        WpdIdentityBound = true,
        SdkAliasMatches = true,
        WpdAliasMatches = true,
        SdkStatusProbed = true,
        SpoolInspected = true,
        SpoolPayloadObjectCount = 0,
        SpoolKnownEmpty = true,
        Firmware = "1.11",
        LiveViewStatus = "off",
        LiveViewStatusAvailable = true,
        CaptureProfileApproved = true,
        CaptureProfileId = "approved-single-cam-b",
        CaptureProfileVersion = 5,
        CaptureProfileSha256 = new string('c', 64),
        CaptureProfileCameraAlias = "CAM-B",
        CaptureProfileExpiresAtUtc = new DateTimeOffset(2099, 8, 10, 0, 0, 0, TimeSpan.Zero),
        CaptureProfileAliasMatches = true,
        SettingsMatchApprovedProfile = true,
        ObservedSettings = CreateObservedHardwareSettings(),
        ReadOnly = true,
        CaptureCommandSent = false,
        CameraObjectDeleteAttempted = false,
        CameraSettingsChanged = false,
        RealIdentifiersIncluded = false,
        FailureCategory = "",
        FailureDetail = "",
    };
    await WriteTestPipeFrameAsync(
        pipe,
        HardwareResponseJson(request.RequestId, true, "SingleReady", ready),
        timeoutSource.Token);
}

static async Task<string> ReadTestPipeFrameAsync(Stream stream, CancellationToken cancellationToken)
{
    var header = new byte[sizeof(int)];
    await ReadTestPipeExactlyAsync(stream, header, cancellationToken);
    var length = BinaryPrimitives.ReadInt32LittleEndian(header);
    Check.True(length is > 0 and <= 1024 * 1024, "The hardware test pipe frame length is invalid.");
    var payload = new byte[length];
    await ReadTestPipeExactlyAsync(stream, payload, cancellationToken);
    return Encoding.UTF8.GetString(payload);
}

static async Task WriteTestPipeFrameAsync(
    Stream stream,
    string message,
    CancellationToken cancellationToken)
{
    var payload = Encoding.UTF8.GetBytes(message);
    var header = new byte[sizeof(int)];
    BinaryPrimitives.WriteInt32LittleEndian(header, payload.Length);
    await stream.WriteAsync(header, cancellationToken);
    await stream.WriteAsync(payload, cancellationToken);
    await stream.FlushAsync(cancellationToken);
}

static async Task ReadTestPipeExactlyAsync(
    Stream stream,
    Memory<byte> buffer,
    CancellationToken cancellationToken)
{
    var offset = 0;
    while (offset < buffer.Length)
    {
        var read = await stream.ReadAsync(buffer[offset..], cancellationToken);
        if (read == 0)
        {
            throw new EndOfStreamException("The hardware test pipe closed before a complete frame.");
        }

        offset += read;
    }
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

    await HardwareNamedPipeRoundtripAsync();
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

static async Task DurableSequentialPairStabilityAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        var source = new DeterministicSimulatedCaptureSource();
        var coordinator = new DurableSimulatedCaptureCoordinator(root, source);
        await coordinator.InitializeAsync();

        var transactionIds = new List<Guid>();
        for (var index = 0; index < 100; index++)
        {
            var transactionId = Guid.NewGuid();
            transactionIds.Add(transactionId);
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
            Check.SequenceEqual(new[] { "CAM-A", "CAM-B" }, result.Originals.Select(original => original.Alias));
            Check.Equal(0, result.AutomaticRetryCount);
        }

        Check.Equal(100, source.GetCaptureCount("CAM-A"));
        Check.Equal(100, source.GetCaptureCount("CAM-B"));
        foreach (var transactionId in transactionIds)
        {
            Check.True(File.Exists(coordinator.GetOriginalPath(transactionId, "CAM-A")), "CAM-A original is missing from the 100-pair run.");
            Check.True(File.Exists(coordinator.GetOriginalPath(transactionId, "CAM-B")), "CAM-B original is missing from the 100-pair run.");
        }

        var restarted = new DurableSimulatedCaptureCoordinator(
            root,
            new DeterministicSimulatedCaptureSource());
        Check.Equal(0, (await restarted.InitializeAsync()).Count);
    });
}

static async Task LiveViewStopFailureIsDurableAsync()
{
    await WithTemporaryRootAsync(async root =>
    {
        var transactionId = Guid.NewGuid();
        ISimulatedTransactionService service = new SimulationFoundationService(root);

        var failed = await service.ExecuteAsync(
            transactionId,
            SimulatedWorkflowScenario.FailLiveViewStop);

        Check.Equal(transactionId, failed.TransactionId);
        Check.Equal(SimulatedTransactionState.FailedPartial, failed.State);
        Check.Equal("LiveViewStopFailed", failed.TerminalReason);
        Check.Equal(0, failed.RetainedOriginalAliases.Count);

        var source = new DeterministicSimulatedCaptureSource();
        var coordinator = new DurableSimulatedCaptureCoordinator(root, source);
        var journal = await coordinator.LoadAsync(transactionId);
        Check.SequenceEqual(
            new[]
            {
                SimulatedTransactionState.Idle,
                SimulatedTransactionState.FailedPartial,
            },
            journal.TransitionHistory);
        Check.Equal(0, journal.Originals.Count);
        Check.Equal(0, journal.AutomaticRetryCount);
        Check.Equal(0, source.GetCaptureCount("CAM-A"));
        Check.Equal(0, source.GetCaptureCount("CAM-B"));

        ISimulatedTransactionService restarted = new SimulationFoundationService(root);
        var discovered = await restarted.InitializeAsync();
        Check.Equal(1, discovered.Count);
        Check.Equal(transactionId, discovered[0].TransactionId);
        Check.Equal(SimulatedTransactionState.FailedPartial, discovered[0].State);
        Check.Equal("LiveViewStopFailed", discovered[0].TerminalReason);

        await Check.ThrowsAsync<InvalidOperationException>(() =>
            restarted.ExecuteAsync(transactionId, SimulatedWorkflowScenario.Success));
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

sealed class RecordingHardwareTransport(
    Func<string, string> responseFactory) : IHardwareCameraAgentTransport
{
    public int RequestCount { get; private set; }

    public Task<string> SendAsync(
        string requestJson,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        ++RequestCount;
        return Task.FromResult(responseFactory(requestJson));
    }
}

sealed class TestTransactionIdConverter : JsonConverter<Guid>
{
    public override Guid Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
        Guid.ParseExact(reader.GetString()!, "N");

    public override void Write(Utf8JsonWriter writer, Guid value, JsonSerializerOptions options) =>
        writer.WriteStringValue(value.ToString("N"));
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

    public static void ThrowsHardwareProtocol(string expectedErrorCode, Action action)
    {
        try
        {
            action();
        }
        catch (HardwareProtocolViolationException exception)
        {
            Equal(expectedErrorCode, exception.ErrorCode);
            return;
        }

        throw new InvalidOperationException($"Expected hardware protocol error '{expectedErrorCode}'.");
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

    public static void Throws<TException>(Action action)
        where TException : Exception
    {
        try
        {
            action();
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
