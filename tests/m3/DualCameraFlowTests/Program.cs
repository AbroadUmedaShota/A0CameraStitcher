using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;

var tests = new (string Name, Func<Task> Run)[]
{
    ("anonymous native identity DTO maps to typed states", IdentityAdapterAsync),
    ("identity gate rejects before capture and freezes active snapshot", IdentityGateAsync),
    ("identity expiry is reevaluated at each capture boundary", IdentityExpiryBoundaryAsync),
    ("native capture stitch restitch export E2E", NativeEndToEndAsync),
    ("malformed and oversized JPEG fail closed", InvalidJpegAsync),
    ("WIC rejects fake SOF truncated scan and corrupt entropy before stitch", CorruptJpegDecodeAsync),
    ("draft and malformed profiles fail before capture", InvalidProfilesAsync),
    ("duplicate start and mode fallback are rejected", ConcurrencyAndModeLockAsync),
    ("capture A and B failures retain only verified originals", CaptureFailuresAsync),
    ("interruption retains originals and never retries", InterruptionAsync),
    ("stitch and export failures preserve product artifacts", OutputFailuresAsync),
    ("HardwareDual anonymous fake completes capture stitch review export", HardwareDualEndToEndAsync),
    ("HardwareDual preflight negatives have zero capture side effects", HardwareDualPreflightNegativesAsync),
    ("HardwareDual Agent negatives retain only safe originals and never retry", HardwareDualAgentNegativesAsync),
    ("HardwareDual unknown recovery ignores current identity and profile inputs", HardwareDualFrozenRecoveryAsync),
    ("HardwareDual active identity and profile snapshots remain frozen", HardwareDualSnapshotFreezeAsync),
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
Console.WriteLine($"DualCamera flow tests: {tests.Length - failures.Count}/{tests.Length} passed.");
return failures.Count == 0 ? 0 : 1;

static Task IdentityAdapterAsync()
{
    const string schema = "a0.dual-identity-application.v1";
    var now = DateTimeOffset.Parse("2026-08-11T00:00:00Z");
    var ready = IdentityJson(schema, "ready", [
        ("CAM-B", new string('b', 64), new string('d', 64)),
        ("CAM-A", new string('a', 64), new string('c', 64)),
    ]);
    Check.Equal(DualCameraIdentityStatus.Ready,
        DualCameraNativeIdentityAdapter.ParseAnonymousSnapshot(ready, now).Status);

    var cases = new (string Json, DualCameraIdentityStatus Status)[]
    {
        (IdentityJson(schema, "camera_count_mismatch", []), DualCameraIdentityStatus.Missing),
        (IdentityJson(schema, "unbound_identity", []), DualCameraIdentityStatus.Ambiguous),
        (IdentityJson(schema, "duplicate_identity", []), DualCameraIdentityStatus.Collision),
        (IdentityJson(schema, "alias_cardinality_mismatch", []), DualCameraIdentityStatus.AliasMismatch),
        (IdentityJson(schema, "mismatched_transport", []), DualCameraIdentityStatus.TransportMismatch),
        (IdentityJson(schema, "proof_stale", []), DualCameraIdentityStatus.Expired),
        (IdentityJson(schema, "proof_invalid", []), DualCameraIdentityStatus.InvalidSchema),
        (IdentityJson(schema, "identity_strategy_unresolved", []), DualCameraIdentityStatus.HardwarePending),
        (IdentityJson(schema, "ready", [
            ("CAM-A", new string('a', 64), new string('c', 64)),
        ]), DualCameraIdentityStatus.Missing),
        (IdentityJson(schema, "ready", [
            ("CAM-A", new string('a', 64), new string('c', 64)),
            ("CAM-A", new string('b', 64), new string('d', 64)),
        ]), DualCameraIdentityStatus.AliasMismatch),
        (IdentityJson(schema, "ready", [
            ("CAM-A", new string('a', 64), new string('c', 64)),
            ("CAM-B", new string('a', 64), new string('d', 64)),
        ]), DualCameraIdentityStatus.Collision),
        (IdentityJson(schema, "ready", [
            ("CAM-A", new string('a', 64), new string('c', 64)),
            ("CAM-B", new string('b', 64), string.Empty),
        ]), DualCameraIdentityStatus.Missing),
        (IdentityJson(schema, "ready", [
            ("CAM-A", new string('a', 64), new string('c', 64)),
            ("CAM-B", new string('b', 64), new string('a', 64)),
        ]), DualCameraIdentityStatus.TransportMismatch),
        (IdentityJson(schema, "ready", [
            ("CAM-A", new string('a', 64), new string('c', 64)),
            ("UNKNOWN", new string('b', 64), new string('d', 64)),
        ]), DualCameraIdentityStatus.AliasMismatch),
        (IdentityJson("a0.dual-identity-application.v2", "ready", []), DualCameraIdentityStatus.InvalidSchema),
        ("{malformed", DualCameraIdentityStatus.InvalidSchema),
    };
    foreach (var (json, status) in cases)
    {
        var snapshot = DualCameraNativeIdentityAdapter.ParseAnonymousSnapshot(json, now);
        Check.Equal(status, snapshot.Status);
        Check.False(snapshot.ToString().Contains(new string('a', 64), StringComparison.Ordinal));
    }
    const string unsafeUnknownReason = "CameraBody7ABCDEF123456789XYZ";
    var sanitized = DualCameraNativeIdentityAdapter.ParseAnonymousSnapshot(
        IdentityJson(schema, unsafeUnknownReason, []),
        now);
    Check.Equal(DualCameraIdentityStatus.InvalidSchema, sanitized.Status);
    Check.False(sanitized.ToString().Contains(unsafeUnknownReason, StringComparison.Ordinal));
    return Task.CompletedTask;
}

static async Task IdentityGateAsync()
{
    await WithRootAsync(async root =>
    {
        var bridge = new FailureBridge();
        var flow = new DualCameraProductFlow(root, bridge, bridge);
        Check.Equal(DualCameraIdentityStatus.HardwarePending, flow.IdentitySnapshot.Status);
        await Check.ThrowsCodeAsync(DualCameraFailureCode.IdentityNotReady,
            () => flow.CaptureAndStitchAsync(DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic())));
        Check.Equal(0, bridge.CaptureCalls);
        await Check.ThrowsCodeAsync(DualCameraFailureCode.InvalidMode,
            () => flow.CaptureAndStitchAsync(DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()) with
            { Mode = CameraOperatingMode.SingleCamera }));
        Check.Equal(0, bridge.CaptureCalls);
    });

    foreach (var status in Enum.GetValues<DualCameraIdentityStatus>().Where(value => value != DualCameraIdentityStatus.Ready))
    {
        await WithRootAsync(async root =>
        {
            var bridge = new FailureBridge();
            var snapshot = new DualCameraIdentitySnapshot(
                status,
                "anonymous_test_block",
                DateTimeOffset.UnixEpoch,
                DateTimeOffset.UnixEpoch);
            var flow = new DualCameraProductFlow(
                root,
                bridge,
                bridge,
                new FixedDualCameraIdentitySnapshotSource(snapshot));
            await Check.ThrowsCodeAsync(DualCameraFailureCode.IdentityNotReady,
                () => flow.CaptureAndStitchAsync(DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic())));
            Check.Equal(0, bridge.CaptureCalls);
        });
    }

    await WithRootAsync(async root =>
    {
        var camera = new BlockingCamera();
        var bridge = new FailureBridge();
        var source = new MutableIdentitySource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady());
        var flow = new DualCameraProductFlow(root, camera, bridge, source);
        var first = flow.CaptureAndStitchAsync(
            DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()));
        await camera.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        source.Set(DualCameraIdentitySnapshot.HardwarePending());
        Check.Equal(DualCameraIdentityStatus.Ready, flow.Current!.IdentitySnapshot.Status);
        await Check.ThrowsCodeAsync(DualCameraFailureCode.DuplicateStart,
            () => flow.CaptureAndStitchAsync(DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic())));
        camera.Release.TrySetResult();
        var completed = await first;
        Check.Equal(DualCameraIdentityStatus.Ready, completed.IdentitySnapshot.Status);
        Check.Equal(2, camera.CaptureCalls);
        await Check.ThrowsCodeAsync(DualCameraFailureCode.IdentityNotReady,
            () => flow.CaptureAndStitchAsync(DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic())));
        Check.Equal(2, camera.CaptureCalls);
    });
}

static async Task IdentityExpiryBoundaryAsync()
{
    var now = DateTimeOffset.Parse("2026-08-11T00:00:00Z");
    await WithRootAsync(async root =>
    {
        var bridge = new FailureBridge();
        var time = new MutableIdentityTimeProvider(now);
        var expiredReady = new DualCameraIdentitySnapshot(
            DualCameraIdentityStatus.Ready,
            "ready",
            now.AddMinutes(-1),
            now);
        var flow = new DualCameraProductFlow(
            root,
            bridge,
            bridge,
            new FixedDualCameraIdentitySnapshotSource(expiredReady),
            time);
        Check.Equal(DualCameraIdentityStatus.Expired, flow.IdentitySnapshot.Status);
        await Check.ThrowsCodeAsync(DualCameraFailureCode.IdentityNotReady,
            () => flow.CaptureAndStitchAsync(DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic())));
        Check.Equal(0, bridge.CaptureCalls);
    });

    await WithRootAsync(async root =>
    {
        var bridge = new FailureBridge();
        var time = new MutableIdentityTimeProvider(now);
        var validReady = new DualCameraIdentitySnapshot(
            DualCameraIdentityStatus.Ready,
            "ready",
            now.AddMinutes(-1),
            now.AddMinutes(1));
        var flow = new DualCameraProductFlow(
            root,
            bridge,
            bridge,
            new FixedDualCameraIdentitySnapshotSource(validReady),
            time);
        var completed = await flow.CaptureAndStitchAsync(
            DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()));
        Check.Equal(DualCameraIdentityStatus.Ready, completed.IdentitySnapshot.Status);
        Check.Equal(2, bridge.CaptureCalls);
    });

    await WithRootAsync(async root =>
    {
        var camera = new BlockingCamera();
        var bridge = new FailureBridge();
        var time = new MutableIdentityTimeProvider(now);
        var finiteReady = new DualCameraIdentitySnapshot(
            DualCameraIdentityStatus.Ready,
            "ready",
            now.AddMinutes(-1),
            now.AddSeconds(1));
        var flow = new DualCameraProductFlow(
            root,
            camera,
            bridge,
            new FixedDualCameraIdentitySnapshotSource(finiteReady),
            time);
        var active = flow.CaptureAndStitchAsync(
            DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()));
        await camera.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        time.Advance(TimeSpan.FromSeconds(2));
        Check.Equal(DualCameraIdentityStatus.Expired, flow.IdentitySnapshot.Status);
        Check.Equal(DualCameraIdentityStatus.Ready, flow.Current!.IdentitySnapshot.Status);
        camera.Release.TrySetResult();
        var completed = await active;
        Check.Equal(DualCameraIdentityStatus.Ready, completed.IdentitySnapshot.Status);
        Check.Equal(2, camera.CaptureCalls);
        await Check.ThrowsCodeAsync(DualCameraFailureCode.IdentityNotReady,
            () => flow.CaptureAndStitchAsync(DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic())));
        Check.Equal(2, camera.CaptureCalls);
    });
}

static string IdentityJson(
    string schema,
    string reason,
    IReadOnlyList<(string Alias, string Sdk, string Wpd)> bindings)
{
    var bindingJson = string.Join(",", bindings.Select(binding =>
        $"{{\"alias\":\"{binding.Alias}\",\"sdkIdentitySha256\":\"{binding.Sdk}\",\"wpdIdentitySha256\":\"{binding.Wpd}\"}}"));
    return $"{{\"schemaVersion\":\"{schema}\",\"reason\":\"{reason}\",\"observedAtUtc\":\"2026-08-11T00:00:00Z\",\"expiresAtUtc\":\"2026-08-12T00:00:00Z\",\"bindings\":[{bindingJson}]}}";
}

static async Task NativeEndToEndAsync()
{
    var adapterPath = Environment.GetEnvironmentVariable("A0_M2_ADAPTER_PATH");
    if (string.IsNullOrWhiteSpace(adapterPath))
    {
        throw new InvalidOperationException("A0_M2_ADAPTER_PATH is required for the focused native E2E.");
    }
    await WithRootAsync(async root =>
    {
        var bridge = new M2OfflineStitcherProcessAdapter(adapterPath);
        IDualCameraProductFlow flow = new DualCameraProductFlow(root, bridge, bridge, SyntheticIdentitySource());
        var captured = await flow.CaptureAndStitchAsync(
            DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()));
        Check.Equal(CameraOperatingMode.DualCamera, captured.Mode);
        Check.Equal(DualCameraExecutionEnvironment.TestSynthetic, captured.ExecutionEnvironment);
        Check.Equal("synthetic-approved-rig-v1", captured.ProfileId);
        Check.Equal("1", captured.ProfileVersion);
        Check.Equal(2, captured.Capture!.Originals.Count);
        Check.True(captured.Capture.Originals.All(original => original.IsCanonicalJpeg));
        Check.True(captured.Capture.Originals.All(original => IsJpeg(original.Path)));
        Check.True(captured.Stitch!.Succeeded);
        Check.True(IsJpeg(captured.Stitch.OutputPath));
        var originalBytes = captured.Capture.Originals.ToDictionary(
            original => original.Alias,
            original => File.ReadAllBytes(original.Path));
        var firstJobId = captured.Stitch.JobId;
        var firstJobPath = captured.Stitch.OutputPath;

        var restitched = await flow.RestitchAsync();
        Check.True(restitched.Stitch!.Succeeded);
        Check.NotEqual(firstJobId, restitched.Stitch.JobId);
        Check.NotEqual(firstJobPath, restitched.Stitch.OutputPath);
        Check.True(restitched.Capture!.Originals.All(original =>
            File.ReadAllBytes(original.Path).SequenceEqual(originalBytes[original.Alias])));

        var exportDirectory = Path.Combine(root, "operator-selected-fixed-local");
        Directory.CreateDirectory(exportDirectory);
        var exported = await flow.ExportAsync(exportDirectory);
        Check.True(exported.Export!.Succeeded);
        Check.True(File.ReadAllBytes(exported.Stitch!.OutputPath)
            .SequenceEqual(File.ReadAllBytes(exported.Export.OutputPath!)));
        Check.Equal(0, exported.AutomaticRetryCount);
        Check.True(exported.Stages.Single(stage => stage.Stage == DualCameraProductStage.Export).Status == DualCameraStageStatus.Succeeded);
    });
}

static async Task InvalidJpegAsync()
{
    foreach (var payload in new[] { InvalidPayload.Malformed, InvalidPayload.Oversized })
    {
        await WithRootAsync(async root =>
        {
            var bridge = new FailureBridge { InvalidPayload = payload };
            var flow = new DualCameraProductFlow(root, bridge, bridge, SyntheticIdentitySource());
            var result = await flow.CaptureAndStitchAsync(
                DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()));
            if (result.FailureCode != DualCameraFailureCode.InvalidOriginal)
            {
                throw new InvalidOperationException($"{payload} was accepted before stitch with {result.FailureCode}.");
            }
            Check.False(result.IsActive);
            Check.Equal(0, bridge.StitchCalls);
            Check.Equal(0, result.AutomaticRetryCount);
        });
    }
}

static async Task CorruptJpegDecodeAsync()
{
    var adapterPath = Environment.GetEnvironmentVariable("A0_M2_ADAPTER_PATH");
    if (string.IsNullOrWhiteSpace(adapterPath))
    {
        throw new InvalidOperationException("A0_M2_ADAPTER_PATH is required for focused WIC corruption validation.");
    }
    foreach (var payload in new[] { CorruptPayload.FakeSofAndEoi, CorruptPayload.TruncatedScan, CorruptPayload.CorruptEntropy })
    {
        await WithRootAsync(async root =>
        {
            var bridge = new M2OfflineStitcherProcessAdapter(adapterPath);
            var flow = new DualCameraProductFlow(root, new CorruptJpegCamera(payload), bridge, SyntheticIdentitySource());
            var result = await flow.CaptureAndStitchAsync(
                DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()));
            if (result.FailureCode != DualCameraFailureCode.InvalidOriginal)
            {
                throw new InvalidOperationException($"{payload} was accepted before stitch with {result.FailureCode}.");
            }
            Check.Equal(0, result.Capture!.Originals.Count);
            Check.True(result.Stitch is null);
            Check.Equal(0, result.AutomaticRetryCount);
        });
    }
}

static async Task InvalidProfilesAsync()
{
    await WithRootAsync(async root =>
    {
        var bridge = new FailureBridge();
        var flow = new DualCameraProductFlow(root, bridge, bridge, SyntheticIdentitySource());
        var draft = DualCameraRigProfile.ApprovedSynthetic() with { Status = DualCameraProfileStatus.Draft };
        var singular = DualCameraRigProfile.ApprovedSynthetic() with { CameraBToCameraA = new double[9] };
        await Check.ThrowsCodeAsync(DualCameraFailureCode.InvalidProfile, () => flow.CaptureAndStitchAsync(DualCameraCaptureRequest.CreateTestSynthetic(draft)));
        await Check.ThrowsCodeAsync(DualCameraFailureCode.InvalidProfile, () => flow.CaptureAndStitchAsync(DualCameraCaptureRequest.CreateTestSynthetic(singular)));
        Check.Equal(0, bridge.CaptureCalls);
    });
}

static async Task ConcurrencyAndModeLockAsync()
{
    await WithRootAsync(async root =>
    {
        var camera = new BlockingCamera();
        var bridge = new FailureBridge();
        var flow = new DualCameraProductFlow(root, camera, bridge, SyntheticIdentitySource());
        var request = DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic());
        var first = flow.CaptureAndStitchAsync(request);
        await camera.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await Check.ThrowsCodeAsync(DualCameraFailureCode.DuplicateStart, () => flow.CaptureAndStitchAsync(request));
        await Check.ThrowsCodeAsync(DualCameraFailureCode.InvalidMode, () => flow.CaptureAndStitchAsync(request with { Mode = CameraOperatingMode.SingleCamera }));
        await Check.ThrowsCodeAsync(DualCameraFailureCode.InvalidExecutionEnvironment, () => flow.CaptureAndStitchAsync(request with { ExecutionEnvironment = (DualCameraExecutionEnvironment)999 }));
        Check.Equal(CameraOperatingMode.DualCamera, flow.Current!.Mode);
        camera.Release.TrySetResult();
        var completed = await first;
        Check.True(completed.Capture!.Succeeded);
        Check.Equal(2, camera.CaptureCalls);
    });
}

static async Task CaptureFailuresAsync()
{
    foreach (var alias in new[] { "CAM-A", "CAM-B" })
    {
        await WithRootAsync(async root =>
        {
            var bridge = new FailureBridge { FailCaptureAlias = alias };
            var flow = new DualCameraProductFlow(root, bridge, bridge, SyntheticIdentitySource());
            var result = await flow.CaptureAndStitchAsync(
                DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()));
            Check.Equal(alias == "CAM-A" ? DualCameraFailureCode.CaptureCameraA : DualCameraFailureCode.CaptureCameraB, result.FailureCode);
            Check.Equal(alias == "CAM-A" ? 0 : 1, result.Capture!.Originals.Count);
            Check.Equal(alias == "CAM-A" ? 1 : 2, bridge.CaptureCalls);
            Check.Equal(0, bridge.StitchCalls);
            Check.Equal(0, result.AutomaticRetryCount);
        });
    }
}

static async Task InterruptionAsync()
{
    await WithRootAsync(async root =>
    {
        using var cancellation = new CancellationTokenSource();
        var camera = new CancelOnCameraB(cancellation);
        var bridge = new FailureBridge();
        var flow = new DualCameraProductFlow(root, camera, bridge, SyntheticIdentitySource());
        var result = await flow.CaptureAndStitchAsync(
            DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()),
            cancellation.Token);
        Check.Equal(DualCameraFailureCode.Interrupted, result.FailureCode);
        Check.Equal(1, result.Capture!.Originals.Count);
        Check.Equal("CAM-A", result.Capture.Originals[0].Alias);
        Check.Equal(2, camera.CaptureCalls);
        Check.Equal(0, bridge.StitchCalls);
        Check.Equal(0, result.AutomaticRetryCount);
    });
}

static async Task OutputFailuresAsync()
{
    await WithRootAsync(async root =>
    {
        var bridge = new FailureBridge { FailStitch = true };
        var flow = new DualCameraProductFlow(root, bridge, bridge, SyntheticIdentitySource());
        var failed = await flow.CaptureAndStitchAsync(
            DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()));
        Check.Equal(DualCameraFailureCode.StitchFailed, failed.FailureCode);
        Check.Equal(2, failed.Capture!.Originals.Count);
        Check.True(failed.Capture.Originals.All(original => File.Exists(original.Path)));
        Check.Equal(1, bridge.StitchCalls);
        Check.Equal(0, failed.AutomaticRetryCount);
    });

    await WithRootAsync(async root =>
    {
        var bridge = new FailureBridge();
        var flow = new DualCameraProductFlow(root, bridge, bridge, SyntheticIdentitySource());
        var completed = await flow.CaptureAndStitchAsync(
            DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic()));
        var stitchedBytes = File.ReadAllBytes(completed.Stitch!.OutputPath);
        await Check.ThrowsAsync<ArgumentException>(() => flow.ExportAsync(@"\\server\share\a0-export"));
        await Check.ThrowsAsync<ArgumentException>(() => flow.ExportAsync(Path.Combine(root, "missing-export-folder")));
        bridge.FailExport = true;
        var exportDirectory = Path.Combine(root, "export-failure");
        Directory.CreateDirectory(exportDirectory);
        var failed = await flow.ExportAsync(exportDirectory);
        Check.Equal(DualCameraFailureCode.ExportFailed, failed.FailureCode);
        Check.False(failed.Export!.Succeeded);
        Check.True(File.ReadAllBytes(failed.Stitch!.OutputPath).SequenceEqual(stitchedBytes));
        Check.Equal(1, bridge.ExportCalls);
        Check.Equal(0, failed.AutomaticRetryCount);
    });
}

static async Task HardwareDualEndToEndAsync()
{
    await WithRootAsync(async root =>
    {
        var operations = new FakeDualHardwareOperations(HardwareFakeScenario.Success);
        var stitcher = new FailureBridge();
        var flow = HardwareFlow(root, operations, stitcher);
        var transactionId = Guid.NewGuid();
        var state = await flow.CaptureAndStitchAsync(HardwareRequest(transactionId));
        Check.Equal(DualCameraExecutionEnvironment.HardwareDual, state.ExecutionEnvironment);
        Check.Equal(2, state.Capture!.Originals.Count);
        Check.True(state.Capture.Originals[0].Path != state.Capture.Originals[1].Path);
        Check.True(state.Capture.Originals.All(item => Path.GetFileName(item.Path) == "original.jpg"));
        Check.True(state.Stitch!.Succeeded);
        Check.Equal(DualHardwareCaptureTerminalState.Succeeded, state.Capture.HardwareEvidence!.TerminalState);
        Check.Equal(0, state.Capture.HardwareEvidence.AutomaticRetryCount);
        Check.Equal(1, operations.ReserveCalls);
        Check.Equal(1, operations.StartCalls);
        Check.Equal(0, operations.QueryCalls);
        var originalHashes = state.Capture.Originals.Select(item => item.Sha256).ToArray();
        var exportDirectory = Path.Combine(root, "exports");
        Directory.CreateDirectory(exportDirectory);
        var exported = await flow.ExportAsync(exportDirectory);
        Check.True(exported.Export!.Succeeded);
        Check.True(state.Capture.Originals.Select(item => item.Sha256).SequenceEqual(originalHashes));
        Check.Equal(1, stitcher.StitchCalls);
        Check.Equal(1, stitcher.ExportCalls);
    });
}

static async Task HardwareDualPreflightNegativesAsync()
{
    foreach (var status in Enum.GetValues<DualCameraIdentityStatus>().Where(value => value != DualCameraIdentityStatus.Ready))
    {
        await WithRootAsync(async root =>
        {
            var operations = new FakeDualHardwareOperations(HardwareFakeScenario.Success);
            var stitcher = new FailureBridge();
            var identity = new DualCameraIdentitySnapshot(status, "anonymous_block", DateTimeOffset.UnixEpoch, DateTimeOffset.MaxValue);
            var flow = HardwareFlow(root, operations, stitcher, identity);
            await Check.ThrowsCodeAsync(DualCameraFailureCode.IdentityNotReady, () => flow.CaptureAndStitchAsync(HardwareRequest(Guid.NewGuid())));
            AssertNoHardwareCaptureSideEffects(operations, stitcher);
        });
    }

    await WithRootAsync(async root =>
    {
        var operations = new FakeDualHardwareOperations(HardwareFakeScenario.Success);
        var stitcher = new FailureBridge();
        var flow = HardwareFlow(root, operations, stitcher);
        var unconfirmed = HardwareRequest(Guid.NewGuid()) with
        {
            HardwareConfirmations = new HardwareDualOperatorConfirmations(true, true, true, false, true),
        };
        var result = await flow.CaptureAndStitchAsync(unconfirmed);
        Check.Equal(DualCameraFailureCode.LiveViewStopFailed, result.FailureCode);
        AssertNoHardwareCaptureSideEffects(operations, stitcher);
    });

    foreach (var profile in new[]
    {
        DualCameraRigProfile.ApprovedSynthetic() with { Status = DualCameraProfileStatus.Draft },
        DualCameraRigProfile.ApprovedSynthetic() with { ValidUntilUtc = DateTimeOffset.UnixEpoch },
        DualCameraRigProfile.ApprovedSynthetic() with { CameraAliases = Array.AsReadOnly(["CAM-A", "CAM-A"]) },
    })
    {
        await WithRootAsync(async root =>
        {
            var operations = new FakeDualHardwareOperations(HardwareFakeScenario.Success);
            var stitcher = new FailureBridge();
            var flow = HardwareFlow(root, operations, stitcher);
            await Check.ThrowsCodeAsync(DualCameraFailureCode.InvalidProfile,
                () => flow.CaptureAndStitchAsync(HardwareRequest(Guid.NewGuid()) with { Profile = profile }));
            AssertNoHardwareCaptureSideEffects(operations, stitcher);
        });
    }

    var approvedCapture = HardwareDualCaptureProfile.ApprovedSynthetic();
    var mismatchedBodies = approvedCapture.Bodies.ToArray();
    mismatchedBodies[1] = mismatchedBodies[1] with { JpegQuality = "Normal" };
    var captureProfileCases = new[]
    {
        (approvedCapture with { Status = DualCameraProfileStatus.Draft }, DualCameraFailureCode.InvalidProfile),
        (approvedCapture with { ValidUntilUtc = DateTimeOffset.UnixEpoch }, DualCameraFailureCode.InvalidProfile),
        (approvedCapture with { Bodies = Array.AsReadOnly(mismatchedBodies) }, DualCameraFailureCode.ProfileMismatch),
    };
    foreach (var (captureProfile, expectedCode) in captureProfileCases)
    {
        await WithRootAsync(async root =>
        {
            var operations = new FakeDualHardwareOperations(HardwareFakeScenario.Success);
            var stitcher = new FailureBridge();
            var flow = HardwareFlow(root, operations, stitcher);
            await Check.ThrowsCodeAsync(expectedCode,
                () => flow.CaptureAndStitchAsync(HardwareRequest(Guid.NewGuid()) with
                {
                    HardwareCaptureProfile = captureProfile,
                }));
            AssertNoHardwareCaptureSideEffects(operations, stitcher);
        });
    }

    await WithRootAsync(async root =>
    {
        var stitcher = new FailureBridge();
        var flow = new DualCameraProductFlow(root, new HardwareDualCaptureSource(null), stitcher, SyntheticIdentitySource());
        var result = await flow.CaptureAndStitchAsync(HardwareRequest(Guid.NewGuid()));
        Check.Equal(DualCameraFailureCode.HardwarePending, result.FailureCode);
        Check.Equal(0, stitcher.StitchCalls);
        Check.Equal(0, stitcher.ExportCalls);
    });
}

static async Task HardwareDualAgentNegativesAsync()
{
    var cases = new[]
    {
        (HardwareFakeScenario.LiveViewUnconfirmed, DualCameraFailureCode.LiveViewStopFailed, 0),
        (HardwareFakeScenario.SpoolNotEmpty, DualCameraFailureCode.SpoolNotEmpty, 2),
        (HardwareFakeScenario.CameraAFailure, DualCameraFailureCode.CaptureCameraA, 0),
        (HardwareFakeScenario.CameraBFailure, DualCameraFailureCode.CaptureCameraB, 1),
        (HardwareFakeScenario.WatchdogExpired, DualCameraFailureCode.WatchdogExpired, 2),
        (HardwareFakeScenario.ResponseUnknown, DualCameraFailureCode.AgentResponseUnknown, 0),
        (HardwareFakeScenario.InvalidSnapshot, DualCameraFailureCode.InvalidOriginal, 2),
        (HardwareFakeScenario.ExternalPath, DualCameraFailureCode.InvalidOriginal, 0),
        (HardwareFakeScenario.LateCompletion, DualCameraFailureCode.WatchdogExpired, 2),
    };
    foreach (var (scenario, code, retainedCount) in cases)
    {
        await WithRootAsync(async root =>
        {
            var operations = new FakeDualHardwareOperations(scenario);
            var stitcher = new FailureBridge();
            var flow = HardwareFlow(root, operations, stitcher);
            var transactionId = Guid.NewGuid();
            var result = await flow.CaptureAndStitchAsync(HardwareRequest(transactionId));
            Check.Equal(code, result.FailureCode);
            Check.Equal(retainedCount, result.Capture!.Originals.Count);
            Check.Equal(0, result.AutomaticRetryCount);
            Check.Equal(scenario != HardwareFakeScenario.ResponseUnknown, result.Capture.HardwareEvidence is not null);
            Check.Equal(0, stitcher.StitchCalls);
            Check.Equal(0, stitcher.ExportCalls);
            Check.Equal(1, operations.StartCalls);
            Check.Equal(scenario == HardwareFakeScenario.ResponseUnknown ? 1 : 0, operations.QueryCalls);
            Check.False((result.FailureReason ?? string.Empty).Contains("CameraBody", StringComparison.Ordinal));
            if (scenario == HardwareFakeScenario.ResponseUnknown)
            {
                var blocked = await flow.CaptureAndStitchAsync(HardwareRequest(Guid.NewGuid()));
                Check.Equal(DualCameraFailureCode.AgentResponseUnknown, blocked.FailureCode);
                Check.Equal(1, operations.ReserveCalls);
                Check.Equal(1, operations.StartCalls);
                Check.Equal(1, operations.QueryCalls);
                var recovered = await flow.RecoverAndStitchAsync(transactionId);
                Check.Equal(DualCameraFailureCode.None, recovered.FailureCode);
                Check.Equal(2, recovered.Capture!.Originals.Count);
                Check.Equal(1, operations.ReserveCalls);
                Check.Equal(1, operations.StartCalls);
                Check.Equal(2, operations.QueryCalls);
                Check.Equal(1, stitcher.StitchCalls);
            }
        });
    }

    await WithRootAsync(async root =>
    {
        var operations = new FakeDualHardwareOperations(HardwareFakeScenario.Success);
        var stitcher = new FailureBridge();
        var flow = HardwareFlow(root, operations, stitcher);
        var id = Guid.NewGuid();
        var first = await flow.CaptureAndStitchAsync(HardwareRequest(id));
        Check.True(first.Capture!.Succeeded);
        var duplicate = await flow.CaptureAndStitchAsync(HardwareRequest(id));
        Check.Equal(DualCameraFailureCode.DuplicateStart, duplicate.FailureCode);
        Check.Equal(1, operations.StartCalls);
        Check.Equal(0, operations.QueryCalls);
        await Check.ThrowsCodeAsync(DualCameraFailureCode.InvalidExecutionEnvironment,
            () => flow.CaptureAndStitchAsync(DualCameraCaptureRequest.CreateTestSynthetic(DualCameraRigProfile.ApprovedSynthetic())));
        Check.Equal(1, operations.StartCalls);
    });
}

static async Task HardwareDualSnapshotFreezeAsync()
{
    await WithRootAsync(async root =>
    {
        var matrix = DualCameraRigProfile.ApprovedSynthetic().CameraBToCameraA.ToList();
        var crop = DualCameraRigProfile.ApprovedSynthetic().Crop.ToList();
        var profile = DualCameraRigProfile.ApprovedSynthetic() with { CameraBToCameraA = matrix, Crop = crop };
        var identitySource = new MutableIdentitySource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady());
        var operations = new BlockingDualHardwareOperations();
        var stitcher = new FailureBridge();
        var flow = new DualCameraProductFlow(root, new HardwareDualCaptureSource(operations), stitcher, identitySource);
        var active = flow.CaptureAndStitchAsync(
            DualCameraCaptureRequest.CreateHardwareDual(
                profile,
                HardwareDualCaptureProfile.ApprovedSynthetic(),
                new HardwareDualOperatorConfirmations(true, true, true, true, true),
                Guid.NewGuid()));
        await operations.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        identitySource.Set(DualCameraIdentitySnapshot.HardwarePending());
        matrix[0] = 0;
        matrix[4] = 0;
        matrix[8] = 0;
        crop[0] = 999;
        Check.Equal(DualCameraIdentityStatus.Ready, flow.Current!.IdentitySnapshot.Status);
        Check.Equal(1.0, operations.Request!.RigProfileSnapshot.CameraBToCameraA[0]);
        Check.Equal(1, operations.Request.RigProfileSnapshot.Crop[0]);
        operations.Release.TrySetResult();
        var completed = await active;
        Check.Equal(DualCameraFailureCode.None, completed.FailureCode);
        Check.Equal(DualCameraIdentityStatus.Ready, completed.IdentitySnapshot.Status);
        identitySource.Set(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady());
        await Check.ThrowsCodeAsync(DualCameraFailureCode.InvalidProfile,
            () => flow.CaptureAndStitchAsync(
                DualCameraCaptureRequest.CreateHardwareDual(
                    profile,
                    HardwareDualCaptureProfile.ApprovedSynthetic(),
                    new HardwareDualOperatorConfirmations(true, true, true, true, true),
                    Guid.NewGuid())));
        identitySource.Set(DualCameraIdentitySnapshot.HardwarePending());
        await Check.ThrowsCodeAsync(DualCameraFailureCode.IdentityNotReady,
            () => flow.CaptureAndStitchAsync(HardwareRequest(Guid.NewGuid())));
        Check.Equal(1, operations.StartCalls);
    });
}

static async Task HardwareDualFrozenRecoveryAsync()
{
    foreach (var changedIdentity in new[]
    {
        DualCameraIdentitySnapshot.HardwarePending(),
        new DualCameraIdentitySnapshot(
            DualCameraIdentityStatus.Expired,
            "anonymous_expired",
            DateTimeOffset.UnixEpoch,
            DateTimeOffset.UnixEpoch),
        new DualCameraIdentitySnapshot(
            DualCameraIdentityStatus.Ready,
            "anonymous_different_ready",
            DateTimeOffset.FromUnixTimeSeconds(300),
            DateTimeOffset.MaxValue),
    })
    {
        await WithRootAsync(async root =>
        {
            var identitySource = new MutableIdentitySource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady());
            var operations = new FakeDualHardwareOperations(HardwareFakeScenario.ResponseUnknown);
            var stitcher = new FailureBridge();
            IDualCameraProductFlow flow = new DualCameraProductFlow(
                root,
                new HardwareDualCaptureSource(operations),
                stitcher,
                identitySource);
            var transactionId = Guid.NewGuid();

            var unknown = await flow.CaptureAndStitchAsync(HardwareRequest(transactionId));
            Check.Equal(DualCameraFailureCode.AgentResponseUnknown, unknown.FailureCode);
            var frozenIdentity = unknown.IdentitySnapshot;
            var frozenProfileId = unknown.ProfileId;

            identitySource.Set(changedIdentity);
            var changedStart = HardwareRequest(Guid.NewGuid()) with
            {
                Profile = DualCameraRigProfile.ApprovedSynthetic() with { ProfileId = "anonymous-different-rig-v2" },
                HardwareCaptureProfile = HardwareDualCaptureProfile.ApprovedSynthetic() with
                {
                    ProfileId = "anonymous-different-capture-v2",
                },
            };
            var stillUnknown = await flow.CaptureAndStitchAsync(changedStart);
            Check.Equal(transactionId, stillUnknown.TransactionId);
            Check.Equal(DualCameraFailureCode.AgentResponseUnknown, stillUnknown.FailureCode);
            Check.Equal(1, operations.ReserveCalls);
            Check.Equal(1, operations.StartCalls);
            Check.Equal(1, operations.QueryCalls);

            var recovered = await flow.RecoverAndStitchAsync(transactionId);

            Check.Equal(DualCameraFailureCode.None, recovered.FailureCode);
            Check.Equal(frozenIdentity, recovered.IdentitySnapshot);
            Check.Equal(frozenProfileId, recovered.ProfileId);
            Check.Equal(2, recovered.Capture!.Originals.Count);
            Check.Equal(1, operations.ReserveCalls);
            Check.Equal(1, operations.StartCalls);
            Check.Equal(2, operations.QueryCalls);
            Check.Equal(1, stitcher.StitchCalls);
        });
    }
}

static DualCameraProductFlow HardwareFlow(
    string root,
    FakeDualHardwareOperations operations,
    IOfflineStitcherAdapter stitcher,
    DualCameraIdentitySnapshot? identity = null) => new(
        root,
        new HardwareDualCaptureSource(operations),
        stitcher,
        new FixedDualCameraIdentitySnapshotSource(identity ?? DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));

static DualCameraCaptureRequest HardwareRequest(Guid transactionId) =>
    DualCameraCaptureRequest.CreateHardwareDual(
        DualCameraRigProfile.ApprovedSynthetic(),
        HardwareDualCaptureProfile.ApprovedSynthetic(),
        new HardwareDualOperatorConfirmations(true, true, true, true, true),
        transactionId);

static void AssertNoHardwareCaptureSideEffects(FakeDualHardwareOperations operations, FailureBridge stitcher)
{
    Check.Equal(0, operations.ReserveCalls);
    Check.Equal(0, operations.StartCalls);
    Check.Equal(0, operations.QueryCalls);
    Check.Equal(0, stitcher.StitchCalls);
    Check.Equal(0, stitcher.ExportCalls);
}

static bool IsJpeg(string path)
{
    var bytes = File.ReadAllBytes(path);
    return bytes is [0xff, 0xd8, .., 0xff, 0xd9];
}

static async Task WithRootAsync(Func<string, Task> action)
{
    var root = Path.Combine(Path.GetTempPath(), "A0CameraStitcher-DualCameraFlowTests", Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try { await action(root); }
    finally { if (Directory.Exists(root)) Directory.Delete(root, recursive: true); }
}

static IDualCameraIdentitySnapshotSource SyntheticIdentitySource() =>
    new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady());

enum InvalidPayload { None, Malformed, Oversized }

enum HardwareFakeScenario
{
    Success,
    LiveViewUnconfirmed,
    SpoolNotEmpty,
    CameraAFailure,
    CameraBFailure,
    WatchdogExpired,
    ResponseUnknown,
    InvalidSnapshot,
    ExternalPath,
    LateCompletion,
}

sealed class FakeDualHardwareOperations(HardwareFakeScenario scenario) : IDualHardwareCaptureOperations
{
    private static readonly byte[] TestJpeg = TestJpegBytes.Value;
    private DualHardwareCaptureRequest? _unknownRequest;
    public int ReserveCalls { get; private set; }
    public int StartCalls { get; private set; }
    public int QueryCalls { get; private set; }

    public Task<bool> ReservePairTransactionAsync(Guid transactionId, CancellationToken cancellationToken)
    {
        _ = transactionId;
        cancellationToken.ThrowIfCancellationRequested();
        ReserveCalls++;
        return Task.FromResult(true);
    }

    public Task<DualHardwareDispatchResult> StartReservedPairAsync(
        DualHardwareCaptureRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        StartCalls++;
        if (scenario == HardwareFakeScenario.ResponseUnknown)
        {
            _unknownRequest = request;
            return Task.FromResult(new DualHardwareDispatchResult(DualHardwareDispatchState.ResponseUnknown, null));
        }

        return Task.FromResult(new DualHardwareDispatchResult(
            DualHardwareDispatchState.Completed,
            CreateResult(request, scenario)));
    }

    private static DualHardwareCaptureResult CreateResult(
        DualHardwareCaptureRequest request,
        HardwareFakeScenario effectiveScenario)
    {

        var count = effectiveScenario switch
        {
            HardwareFakeScenario.CameraAFailure or HardwareFakeScenario.LiveViewUnconfirmed => 0,
            HardwareFakeScenario.CameraBFailure => 1,
            _ => 2,
        };
        var originals = new List<DualHardwareOriginalRecord>();
        foreach (var alias in new[] { "CAM-A", "CAM-B" }.Take(count))
        {
            var path = Path.Combine(request.TransactionDirectory, alias, "original.jpg");
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllBytes(path, TestJpeg);
            var reportedPath = effectiveScenario == HardwareFakeScenario.ExternalPath
                ? Path.Combine(Path.GetDirectoryName(request.TransactionDirectory)!, "outside", alias, "original.jpg")
                : path;
            if (effectiveScenario == HardwareFakeScenario.ExternalPath)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(reportedPath)!);
                File.WriteAllBytes(reportedPath, TestJpeg);
            }
            originals.Add(new(alias, reportedPath,
                effectiveScenario != HardwareFakeScenario.SpoolNotEmpty,
                effectiveScenario != HardwareFakeScenario.SpoolNotEmpty));
        }

        var (terminal, code) = effectiveScenario switch
        {
            HardwareFakeScenario.CameraAFailure => (DualHardwareCaptureTerminalState.Failed, DualCameraFailureCode.CaptureCameraA),
            HardwareFakeScenario.CameraBFailure => (DualHardwareCaptureTerminalState.FailedPartial, DualCameraFailureCode.CaptureCameraB),
            HardwareFakeScenario.WatchdogExpired => (DualHardwareCaptureTerminalState.WatchdogExpired, DualCameraFailureCode.WatchdogExpired),
            _ => (DualHardwareCaptureTerminalState.Succeeded, DualCameraFailureCode.None),
        };
        var evidenceIdentity = effectiveScenario == HardwareFakeScenario.InvalidSnapshot
            ? DualCameraIdentitySnapshot.HardwarePending()
            : request.IdentitySnapshot;
        var evidence = new DualHardwareCaptureEvidence(
            terminal,
            evidenceIdentity,
            request.CaptureProfileSnapshot.ProfileId,
            request.CaptureProfileSnapshot.Version,
            request.RigProfileSnapshot.ProfileId,
            request.RigProfileSnapshot.Version,
            request.StartedAtUtc,
            request.WatchdogDeadlineUtc,
            effectiveScenario == HardwareFakeScenario.LateCompletion
                ? request.WatchdogDeadlineUtc.AddMilliseconds(1)
                : request.StartedAtUtc.AddSeconds(1),
            effectiveScenario != HardwareFakeScenario.WatchdogExpired,
            effectiveScenario != HardwareFakeScenario.LiveViewUnconfirmed,
            effectiveScenario != HardwareFakeScenario.SpoolNotEmpty,
            effectiveScenario != HardwareFakeScenario.SpoolNotEmpty,
            0);
        var result = new DualHardwareCaptureResult(
            request.TransactionId,
            originals,
            terminal,
            code,
            evidence);
        return result;
    }

    public Task<DualHardwareCaptureResult?> QueryPairTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        _ = transactionId;
        cancellationToken.ThrowIfCancellationRequested();
        QueryCalls++;
        if (scenario == HardwareFakeScenario.ResponseUnknown && QueryCalls >= 2 && _unknownRequest is not null)
            return Task.FromResult<DualHardwareCaptureResult?>(CreateResult(_unknownRequest, HardwareFakeScenario.Success));
        return Task.FromResult<DualHardwareCaptureResult?>(null);
    }
}

sealed class BlockingDualHardwareOperations : IDualHardwareCaptureOperations
{
    public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public TaskCompletionSource Release { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public DualHardwareCaptureRequest? Request { get; private set; }
    public int StartCalls { get; private set; }

    public Task<bool> ReservePairTransactionAsync(Guid transactionId, CancellationToken cancellationToken) => Task.FromResult(true);

    public async Task<DualHardwareDispatchResult> StartReservedPairAsync(DualHardwareCaptureRequest request, CancellationToken cancellationToken)
    {
        Request = request;
        StartCalls++;
        Started.TrySetResult();
        await Release.Task.WaitAsync(cancellationToken);
        var records = new List<DualHardwareOriginalRecord>();
        foreach (var alias in new[] { "CAM-A", "CAM-B" })
        {
            var path = Path.Combine(request.TransactionDirectory, alias, "original.jpg");
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllBytes(path, TestJpegBytes.Value);
            records.Add(new(alias, path, true, true));
        }
        var evidence = new DualHardwareCaptureEvidence(
            DualHardwareCaptureTerminalState.Succeeded,
            request.IdentitySnapshot,
            request.CaptureProfileSnapshot.ProfileId,
            request.CaptureProfileSnapshot.Version,
            request.RigProfileSnapshot.ProfileId,
            request.RigProfileSnapshot.Version,
            request.StartedAtUtc,
            request.WatchdogDeadlineUtc,
            request.StartedAtUtc.AddSeconds(1),
            true, true, true, true, 0);
        var result = new DualHardwareCaptureResult(request.TransactionId, records,
            DualHardwareCaptureTerminalState.Succeeded, DualCameraFailureCode.None, evidence);
        return new(DualHardwareDispatchState.Completed, result);
    }

    public Task<DualHardwareCaptureResult?> QueryPairTransactionAsync(Guid transactionId, CancellationToken cancellationToken) =>
        Task.FromResult<DualHardwareCaptureResult?>(null);
}

sealed class FailureBridge : ITestSyntheticCamera, IOfflineStitcherAdapter
{
    private static readonly byte[] TestJpeg = TestJpegBytes.Value;
    public string? FailCaptureAlias { get; init; }
    public InvalidPayload InvalidPayload { get; init; }
    public bool FailStitch { get; init; }
    public bool FailExport { get; set; }
    public int CaptureCalls { get; private set; }
    public int StitchCalls { get; private set; }
    public int ExportCalls { get; private set; }

    public Task ValidateCanonicalJpegAsync(string jpegPath, int expectedWidth, int expectedHeight, CancellationToken cancellationToken)
    {
        _ = jpegPath;
        _ = expectedWidth;
        _ = expectedHeight;
        cancellationToken.ThrowIfCancellationRequested();
        return Task.CompletedTask;
    }

    public Task<string> CaptureAsync(string alias, Guid transactionId, string destinationPath, CancellationToken cancellationToken)
    {
        _ = transactionId;
        cancellationToken.ThrowIfCancellationRequested();
        CaptureCalls++;
        if (alias == FailCaptureAlias) throw new IOException($"{alias} deterministic failure");
        Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);
        if (InvalidPayload == InvalidPayload.Malformed)
        {
            File.WriteAllText(destinationPath, "not a JPEG");
        }
        else if (InvalidPayload == InvalidPayload.Oversized)
        {
            using var stream = File.Create(destinationPath);
            stream.SetLength(DualCameraProductFlow.MaximumCompressedJpegBytes + 1);
        }
        else
        {
            File.WriteAllBytes(destinationPath, TestJpeg);
        }
        return Task.FromResult(destinationPath);
    }

    public Task<OfflineStitchArtifact> StitchAsync(IReadOnlyList<CanonicalJpegOriginal> originals, string outputJobDirectory, DualCameraRigProfile profile, CancellationToken cancellationToken)
    {
        _ = originals;
        cancellationToken.ThrowIfCancellationRequested();
        StitchCalls++;
        if (FailStitch) throw new InvalidOperationException("deterministic stitch failure");
        Directory.CreateDirectory(outputJobDirectory);
        var output = Path.Combine(outputJobDirectory, "stitched.jpg");
        File.WriteAllBytes(output, TestJpeg);
        return Task.FromResult(new OfflineStitchArtifact(output, 1, 1, profile.ProfileId));
    }

    public Task ExportAsync(string stitchedJpeg, string destinationJpeg, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        ExportCalls++;
        if (FailExport) throw new IOException("deterministic export failure");
        File.Copy(stitchedJpeg, destinationJpeg, overwrite: false);
        return Task.CompletedTask;
    }
}

enum CorruptPayload { FakeSofAndEoi, TruncatedScan, CorruptEntropy }

sealed class CorruptJpegCamera(CorruptPayload payload) : ITestSyntheticCamera
{
    public async Task<string> CaptureAsync(
        string alias,
        Guid transactionId,
        string destinationPath,
        CancellationToken cancellationToken)
    {
        _ = alias;
        _ = transactionId;
        Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);
        await File.WriteAllBytesAsync(destinationPath, CreatePayload(payload), cancellationToken);
        return destinationPath;
    }

    private static byte[] CreatePayload(CorruptPayload value)
    {
        if (value == CorruptPayload.FakeSofAndEoi)
        {
            return
            [
                0xff, 0xd8,
                0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x10, 0x03,
                0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
                0xff, 0xd9,
            ];
        }
        var source = TestJpegBytes.Value;
        var scan = FindMarker(source, 0xda);
        var headerLength = (source[scan + 2] << 8) | source[scan + 3];
        var entropyStart = scan + 2 + headerLength;
        if (value == CorruptPayload.TruncatedScan)
        {
            return [.. source[..(scan + 5)], 0xff, 0xd9];
        }
        return [.. source[..entropyStart], 0xff, 0xdb, 0x00, 0x02, 0xff, 0xd9];
    }

    private static int FindMarker(byte[] bytes, byte marker)
    {
        for (var index = 0; index < bytes.Length - 1; index++)
        {
            if (bytes[index] == 0xff && bytes[index + 1] == marker) return index;
        }
        throw new InvalidDataException("JPEG scan marker is missing from the test fixture.");
    }
}

sealed class BlockingCamera : ITestSyntheticCamera
{
    private static readonly byte[] TestJpeg = TestJpegBytes.Value;
    public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public TaskCompletionSource Release { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public int CaptureCalls { get; private set; }

    public async Task<string> CaptureAsync(string alias, Guid transactionId, string destinationPath, CancellationToken cancellationToken)
    {
        _ = transactionId;
        CaptureCalls++;
        if (alias == "CAM-A")
        {
            Started.TrySetResult();
            await Release.Task.WaitAsync(cancellationToken);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);
        await File.WriteAllBytesAsync(destinationPath, TestJpeg, cancellationToken);
        return destinationPath;
    }
}

sealed class CancelOnCameraB(CancellationTokenSource cancellation) : ITestSyntheticCamera
{
    private static readonly byte[] TestJpeg = TestJpegBytes.Value;
    public int CaptureCalls { get; private set; }

    public async Task<string> CaptureAsync(string alias, Guid transactionId, string destinationPath, CancellationToken cancellationToken)
    {
        _ = transactionId;
        CaptureCalls++;
        if (alias == "CAM-B")
        {
            cancellation.Cancel();
            cancellationToken.ThrowIfCancellationRequested();
        }
        Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);
        await File.WriteAllBytesAsync(destinationPath, TestJpeg, cancellationToken);
        return destinationPath;
    }
}

static class TestJpegBytes
{
    public static readonly byte[] Value = Convert.FromBase64String(
        "/9j/4AAQSkZJRgABAQEAAAAAAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAAIABADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD5/ooor8kP9Cz/2Q==");
}

sealed class MutableIdentitySource(DualCameraIdentitySnapshot initial) : IDualCameraIdentitySnapshotSource
{
    public event EventHandler<DualCameraIdentitySnapshot>? SnapshotChanged;

    public DualCameraIdentitySnapshot Current { get; private set; } = initial;

    public void Set(DualCameraIdentitySnapshot snapshot)
    {
        Current = snapshot;
        SnapshotChanged?.Invoke(this, snapshot);
    }
}

sealed class MutableIdentityTimeProvider(DateTimeOffset utcNow) : TimeProvider
{
    private DateTimeOffset _utcNow = utcNow;

    public override DateTimeOffset GetUtcNow() => _utcNow;

    public void Advance(TimeSpan delta) => _utcNow += delta;
}

static class Check
{
    public static void True(bool condition) { if (!condition) throw new InvalidOperationException("Expected true."); }
    public static void False(bool condition) { if (condition) throw new InvalidOperationException("Expected false."); }
    public static void Equal<T>(T expected, T actual) where T : notnull
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
            throw new InvalidOperationException($"Expected {expected}, actual {actual}.");
    }
    public static void NotEqual<T>(T unexpected, T actual) where T : notnull
    {
        if (EqualityComparer<T>.Default.Equals(unexpected, actual))
            throw new InvalidOperationException($"Did not expect {actual}.");
    }
    public static async Task ThrowsCodeAsync(DualCameraFailureCode code, Func<Task<DualCameraProductState>> action)
    {
        try { await action(); }
        catch (DualCameraFlowException exception) when (exception.Code == code) { return; }
        throw new InvalidOperationException($"Expected {code} rejection.");
    }
    public static async Task ThrowsAsync<TException>(Func<Task<DualCameraProductState>> action)
        where TException : Exception
    {
        try { await action(); }
        catch (TException) { return; }
        throw new InvalidOperationException($"Expected {typeof(TException).Name} rejection.");
    }
}
