using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;

var tests = new (string Name, Func<Task> Run)[]
{
    ("native capture stitch restitch export E2E", NativeEndToEndAsync),
    ("malformed and oversized JPEG fail closed", InvalidJpegAsync),
    ("WIC rejects fake SOF truncated scan and corrupt entropy before stitch", CorruptJpegDecodeAsync),
    ("draft and malformed profiles fail before capture", InvalidProfilesAsync),
    ("duplicate start and mode fallback are rejected", ConcurrencyAndModeLockAsync),
    ("capture A and B failures retain only verified originals", CaptureFailuresAsync),
    ("interruption retains originals and never retries", InterruptionAsync),
    ("stitch and export failures preserve product artifacts", OutputFailuresAsync),
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
        IDualCameraProductFlow flow = new DualCameraProductFlow(root, bridge, bridge);
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
            var flow = new DualCameraProductFlow(root, bridge, bridge);
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
            var flow = new DualCameraProductFlow(root, new CorruptJpegCamera(payload), bridge);
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
        var flow = new DualCameraProductFlow(root, bridge, bridge);
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
        var flow = new DualCameraProductFlow(root, camera, bridge);
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
            var flow = new DualCameraProductFlow(root, bridge, bridge);
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
        var flow = new DualCameraProductFlow(root, camera, bridge);
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
        var flow = new DualCameraProductFlow(root, bridge, bridge);
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
        var flow = new DualCameraProductFlow(root, bridge, bridge);
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

enum InvalidPayload { None, Malformed, Oversized }

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
