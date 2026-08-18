using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.Hardware;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.OperatorShell;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.OperatorShell.ViewModels;
using System.Buffers.Binary;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Windows.Media;
using System.Windows.Media.Imaging;

const string persistentChildScenarioVariable = "A0_CAMERA_AGENT_TEST_CHILD_SCENARIO";
if (Environment.GetEnvironmentVariable(persistentChildScenarioVariable) is { Length: > 0 } childScenario)
{
    return await RunPersistentCameraAgentTestChildAsync(childScenario, args);
}
if (args is ["--sdkless-camera-agent-e2e", var sdklessAgentPath])
{
    return await RunSdklessPersistentReadinessE2EAsync(sdklessAgentPath);
}
const string dualChildScenarioVariable = "A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO";
if (Environment.GetEnvironmentVariable(dualChildScenarioVariable) is { Length: > 0 } dualChildScenario)
{
    return await RunDualCameraAgentTestChildAsync(dualChildScenario, args);
}

var failures = new List<string>();
try
{
    await PersistentHardwareCameraAgentPipeFailuresAsync();
    Console.WriteLine("PASS persistent hardware Camera Agent classifies pipe exits and typed readiness");
}
catch (Exception exception)
{
    failures.Add("persistent hardware Camera Agent classifies pipe exits and typed readiness");
    Console.Error.WriteLine($"FAIL persistent hardware Camera Agent classifies pipe exits and typed readiness: {exception}");
}

try
{
    await LiveViewStopFailureWorkflowAsync();
    Console.WriteLine("PASS live view stop failure survives restart and rejects duplicate start");
}
catch (Exception exception)
{
    failures.Add("live view stop failure survives restart and rejects duplicate start");
    Console.Error.WriteLine($"FAIL live view stop failure survives restart and rejects duplicate start: {exception}");
}

try
{
    await SingleCameraWorkflowAsync();
    Console.WriteLine("PASS single-camera CAM-B capture skips stitch and exports one original");
}
catch (Exception exception)
{
    failures.Add("single-camera CAM-B capture skips stitch and exports one original");
    Console.Error.WriteLine($"FAIL single-camera CAM-B capture skips stitch and exports one original: {exception}");
}

try
{
    await ModeAndAliasLockDuringCaptureAsync();
    Console.WriteLine("PASS active capture locks operating mode and selected alias");
}
catch (Exception exception)
{
    failures.Add("active capture locks operating mode and selected alias");
    Console.Error.WriteLine($"FAIL active capture locks operating mode and selected alias: {exception}");
}

try
{
    await DualCameraRegressionAsync();
    Console.WriteLine("PASS dual-camera workflow still captures, stitches, restitches, and exports");
}
catch (Exception exception)
{
    failures.Add("dual-camera workflow still captures, stitches, restitches, and exports");
    Console.Error.WriteLine($"FAIL dual-camera workflow still captures, stitches, restitches, and exports: {exception}");
}

try
{
    await DualIdentityBlocksWpfCaptureAsync();
    Console.WriteLine("PASS non-ready dual identity blocks WPF and capture side effects");
}
catch (Exception exception)
{
    failures.Add("non-ready dual identity blocks WPF and capture side effects");
    Console.Error.WriteLine($"FAIL non-ready dual identity blocks WPF and capture side effects: {exception}");
}

try
{
    await DualIdentityExpiryBlocksWpfCaptureAsync();
    Console.WriteLine("PASS expired ready dual identity blocks WPF transaction start");
}
catch (Exception exception)
{
    failures.Add("expired ready dual identity blocks WPF transaction start");
    Console.Error.WriteLine($"FAIL expired ready dual identity blocks WPF transaction start: {exception}");
}

try
{
    await FormalDualCameraWpfFlowAsync();
    Console.WriteLine("PASS formal WPF dual-camera flow uses real JPEG product artifacts");
}
catch (Exception exception)
{
    failures.Add("formal WPF dual-camera flow uses real JPEG product artifacts");
    Console.Error.WriteLine($"FAIL formal WPF dual-camera flow uses real JPEG product artifacts: {exception}");
}

try
{
    await FormalDualCameraExportFailureProgressAsync();
    Console.WriteLine("PASS formal WPF maps active and failed explicit export progress");
}
catch (Exception exception)
{
    failures.Add("formal WPF maps active and failed explicit export progress");
    Console.Error.WriteLine($"FAIL formal WPF maps active and failed explicit export progress: {exception}");
}

try
{
    await SingleCameraRestartPreservesPlanAsync();
    Console.WriteLine("PASS single-camera restart restores the durable mode and selected alias");
}
catch (Exception exception)
{
    failures.Add("single-camera restart restores the durable mode and selected alias");
    Console.Error.WriteLine($"FAIL single-camera restart restores the durable mode and selected alias: {exception}");
}

try
{
    await HardwareSingleHappyPathAsync();
    Console.WriteLine("PASS hardware single-camera path captures once and explicitly exports verified original");
}
catch (Exception exception)
{
    failures.Add("hardware single-camera path captures once and explicitly exports verified original");
    Console.Error.WriteLine($"FAIL hardware single-camera path captures once and explicitly exports verified original: {exception}");
}

try
{
    await HardwarePendingTransactionRecoveryAsync();
    Console.WriteLine("PASS hardware pending transaction is queried without recapture and survives in-progress result");
}
catch (Exception exception)
{
    failures.Add("hardware pending transaction is queried without recapture and survives in-progress result");
    Console.Error.WriteLine($"FAIL hardware pending transaction is queried without recapture and survives in-progress result: {exception}");
}

try
{
    await HardwareLiveViewRequiresFreshReadinessAsync();
    Console.WriteLine("PASS hardware finite Live View verifies preview and requires fresh readiness");
}
catch (Exception exception)
{
    failures.Add("hardware finite Live View verifies preview and requires fresh readiness");
    Console.Error.WriteLine($"FAIL hardware finite Live View verifies preview and requires fresh readiness: {exception}");
}

try
{
    await HardwareMalformedLocalStateFailsClosedAsync();
    Console.WriteLine("PASS malformed hardware app state fails closed without camera access");
}
catch (Exception exception)
{
    failures.Add("malformed hardware app state fails closed without camera access");
    Console.Error.WriteLine($"FAIL malformed hardware app state fails closed without camera access: {exception}");
}

try
{
    HardwareLaunchOptionsAreExplicit();
    Console.WriteLine("PASS app launch options keep hardware and simulation explicit");
}
catch (Exception exception)
{
    failures.Add("app launch options keep hardware and simulation explicit");
    Console.Error.WriteLine($"FAIL app launch options keep hardware and simulation explicit: {exception}");
}

try
{
    await HardwareAppStateCompareAndSetAsync();
    Console.WriteLine("PASS hardware app state cannot overwrite or clear another transaction");
}
catch (Exception exception)
{
    failures.Add("hardware app state cannot overwrite or clear another transaction");
    Console.Error.WriteLine($"FAIL hardware app state cannot overwrite or clear another transaction: {exception}");
}

try
{
    HardwareOperatorSessionLeaseIsExclusive();
    Console.WriteLine("PASS hardware operator window lease excludes a second local process owner");
}
catch (Exception exception)
{
    failures.Add("hardware operator window lease excludes a second local process owner");
    Console.Error.WriteLine($"FAIL hardware operator window lease excludes a second local process owner: {exception}");
}

try
{
    await HardwareExportVerificationFailureStaysUnpublishedAsync();
    Console.WriteLine("PASS verified export handle blocks writes and defeats path replacement");
}
catch (Exception exception)
{
    failures.Add("verified export handle blocks writes and defeats path replacement");
    Console.Error.WriteLine($"FAIL verified export handle blocks writes and defeats path replacement: {exception}");
}

try
{
    await HardwarePreDispatchAndNotFoundRecoveryBoundariesAsync();
    Console.WriteLine("PASS startup closes known pre-dispatch failure but blocks ambiguous missing journal");
}
catch (Exception exception)
{
    failures.Add("startup closes known pre-dispatch failure but blocks ambiguous missing journal");
    Console.Error.WriteLine($"FAIL startup closes known pre-dispatch failure but blocks ambiguous missing journal: {exception}");
}

try
{
    await HardwareInitializationAndProfileExpiryGateAsync();
    Console.WriteLine("PASS startup inspection and profile expiry keep every hardware command closed");
}
catch (Exception exception)
{
    failures.Add("startup inspection and profile expiry keep every hardware command closed");
    Console.Error.WriteLine($"FAIL startup inspection and profile expiry keep every hardware command closed: {exception}");
}

try
{
    await HardwareContinuousLiveViewCaptureHandoffAsync();
    Console.WriteLine("PASS hardware continuous Live View stops before capture and restarts only after success");
}
catch (Exception exception)
{
    failures.Add("hardware continuous Live View stops before capture and restarts only after success");
    Console.Error.WriteLine($"FAIL hardware continuous Live View stops before capture and restarts only after success: {exception}");
}

try
{
    await HardwareSinglePreferencesAndProfileApprovalAsync();
    Console.WriteLine("PASS local export preference and 30-day CAM-A profile approval are durable and fail closed");
}
catch (Exception exception)
{
    failures.Add("local export preference and 30-day CAM-A profile approval are durable and fail closed");
    Console.Error.WriteLine($"FAIL local export preference and 30-day CAM-A profile approval are durable and fail closed: {exception}");
}

try
{
    await HardwareSingleRequiresAndRepairsOperatorExportDirectoryAsync();
    Console.WriteLine("PASS hardware single requires an operator export folder and permits repair after capture");
}
catch (Exception exception)
{
    failures.Add("hardware single requires an operator export folder and permits repair after capture");
    Console.Error.WriteLine($"FAIL hardware single requires an operator export folder and permits repair after capture: {exception}");
}

try
{
    await DualCameraAgentLifecycleIdentityPendingKeepsZeroProcessAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle keeps process/camera at zero while identity is Pending");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle keeps process/camera at zero while identity is Pending");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle keeps process/camera at zero while identity is Pending: {exception}");
}

try
{
    await DualCameraAgentLifecycleRequiresExistingArtifactFilesAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle requires existing approved-capture-profile/dual-identity-proof files before launch");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle requires existing approved-capture-profile/dual-identity-proof files before launch");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle requires existing approved-capture-profile/dual-identity-proof files before launch: {exception}");
}

try
{
    await DualCameraAgentLifecycleFakeHostHappyPathAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle fake host reserve-start typed success end-to-end");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle fake host reserve-start typed success end-to-end");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle fake host reserve-start typed success end-to-end: {exception}");
}

try
{
    await DualCameraAgentLifecycleExitCodeClassificationAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle classifies all four exit codes without inverting dispatch ambiguity");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle classifies all four exit codes without inverting dispatch ambiguity");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle classifies all four exit codes without inverting dispatch ambiguity: {exception}");
}

try
{
    await DualCameraAgentLifecycleRestartRecoveryAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle process exit and native max-lifetime exit both recover via same-ID query only");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle process exit and native max-lifetime exit both recover via same-ID query only");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle process exit and native max-lifetime exit both recover via same-ID query only: {exception}");
}

try
{
    await DualCameraAgentLifecyclePipeFailureWithoutProcessExitAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle pipe failure without process exit resumes on the same process");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle pipe failure without process exit resumes on the same process");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle pipe failure without process exit resumes on the same process: {exception}");
}

try
{
    await DualCameraAgentLifecycleConnectFailureSurfacesExitDiagnosticsAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle connect failure surfaces exit code and stderr diagnostics");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle connect failure surfaces exit code and stderr diagnostics");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle connect failure surfaces exit code and stderr diagnostics: {exception}");
}

Console.WriteLine($"Operator shell tests: {30 - failures.Count}/30 passed.");
return failures.Count == 0 ? 0 : 1;

static async Task PersistentHardwareCameraAgentPipeFailuresAsync()
{
    VerifyHardwareCameraAgentStderrSanitizer();

    var executablePath = Path.Combine(
        AppContext.BaseDirectory,
        "A0CameraStitcher.M3.OperatorShellTests.exe");
    Check.True(File.Exists(executablePath), "The persistent test child apphost must exist.");

    var root = Path.Combine(Path.GetTempPath(), $"a0-persistent-agent-{Guid.NewGuid():N}");
    Directory.CreateDirectory(root);
    try
    {
        var storagePaths = HardwareSingleStoragePaths.Resolve(root);
        var profilePath = storagePaths.CaptureProfilePath;
        var identityPath = storagePaths.SingleIdentityV3Path;
        Directory.CreateDirectory(Path.GetDirectoryName(profilePath)!);
        Directory.CreateDirectory(Path.GetDirectoryName(identityPath)!);

        foreach (var (scenario, expectedStage) in new[]
                 {
                     ("before-response", HardwarePipeResponseFailureStage.BeforeResponse),
                     ("partial-header", HardwarePipeResponseFailureStage.PartialHeader),
                     ("partial-body", HardwarePipeResponseFailureStage.PartialBody),
                 })
        {
            Environment.SetEnvironmentVariable(persistentChildScenarioVariable, scenario);
            await using var operations = new PersistentHardwareCameraAgentOperations(
                executablePath,
                profilePath,
                identityPath);
            try
            {
                _ = await operations.GetReadinessAsync("CAM-A");
                throw new InvalidOperationException($"The {scenario} child must fail before a full response.");
            }
            catch (HardwareCameraAgentLaunchException exception)
            {
                Check.True(exception.ProcessExitCode == 37, "The child exit code must be preserved.");
                Check.True(
                    exception.ResponseFailureStage == expectedStage,
                    $"The {scenario} response failure stage must remain distinct.");
                Check.True(
                    exception.RequestMayHaveBeenDispatched,
                    "A response-side pipe close must remain dispatch-ambiguous.");
                Check.True(
                    exception.SanitizedStandardError.Contains("synthetic child failed closed", StringComparison.Ordinal),
                    "The safe stderr classification must be retained.");
                Check.True(
                    exception.SanitizedStandardError.Contains("requestId=[redacted]", StringComparison.Ordinal),
                    "A long general alphanumeric request identifier must be redacted.");
                Check.False(
                    exception.SanitizedStandardError.Contains("super-secret", StringComparison.Ordinal) ||
                    exception.SanitizedStandardError.Contains("C:\\private", StringComparison.Ordinal) ||
                    exception.SanitizedStandardError.Contains("RAW-CAMERA-IDENTITY", StringComparison.Ordinal) ||
                    exception.SanitizedStandardError.Contains(
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", StringComparison.Ordinal),
                    "Secrets, paths, and raw identity must be removed from diagnostics.");
                Check.True(
                    exception.SanitizedStandardError.Length <= 512,
                    "The stderr diagnostic must be bounded.");
            }
        }

        var tracePath = Path.Combine(root, "composition.json");
        Environment.SetEnvironmentVariable(persistentChildScenarioVariable, "typed-fail-closed");
        Environment.SetEnvironmentVariable("A0_CAMERA_AGENT_TEST_CHILD_TRACE", tracePath);
        await using (var operations = new PersistentHardwareCameraAgentOperations(
            executablePath,
            profilePath,
            identityPath))
        {
            var reply = await operations.GetReadinessAsync("CAM-A");
            Check.True(reply.Success, "A typed not-ready result is a successful protocol response.");
            Check.False(reply.Payload.Ready, "The SDK-less child must fail closed as not ready.");
            Check.Equal("licensed_adapter_unavailable", reply.Payload.FailureCategory);
        }

        using var trace = JsonDocument.Parse(await File.ReadAllTextAsync(tracePath));
        var traceRoot = trace.RootElement;
        Check.Equal(Path.GetFullPath(executablePath), traceRoot.GetProperty("executablePath").GetString()!);
        Check.Equal(Path.GetDirectoryName(executablePath)!, traceRoot.GetProperty("workingDirectory").GetString()!);
        var childArguments = traceRoot.GetProperty("arguments").EnumerateArray()
            .Select(value => value.GetString()!)
            .ToArray();
        Check.True(childArguments.Contains("--pipe-name", StringComparer.Ordinal), "The persistent pipe argument is required.");
        Check.True(childArguments.Contains(profilePath, StringComparer.Ordinal), "WPF and the Agent must share the profile path.");
        Check.True(childArguments.Contains(identityPath, StringComparer.Ordinal), "WPF and the Agent must share identity-v3.");
        Check.False(childArguments.Contains("--serve-once", StringComparer.Ordinal), "The formal WPF must use the persistent Agent mode.");
    }
    finally
    {
        Environment.SetEnvironmentVariable(persistentChildScenarioVariable, null);
        Environment.SetEnvironmentVariable("A0_CAMERA_AGENT_TEST_CHILD_TRACE", null);
        Directory.Delete(root, recursive: true);
    }
}

static async Task<int> RunSdklessPersistentReadinessE2EAsync(string agentExecutablePath)
{
    var root = Path.Combine(Path.GetTempPath(), $"a0-sdkless-agent-e2e-{Guid.NewGuid():N}");
    Directory.CreateDirectory(root);
    try
    {
        var storagePaths = HardwareSingleStoragePaths.Resolve(root);
        var profilePath = storagePaths.CaptureProfilePath;
        var identityPath = storagePaths.SingleIdentityV3Path;
        Directory.CreateDirectory(Path.GetDirectoryName(profilePath)!);
        Directory.CreateDirectory(Path.GetDirectoryName(identityPath)!);
        await using var operations = new PersistentHardwareCameraAgentOperations(
            agentExecutablePath,
            profilePath,
            identityPath);
        var reply = await operations.GetReadinessAsync("CAM-A");
        Check.True(reply.Success, "The real SDK-less Agent must return a typed hardware.v1 response.");
        Check.False(reply.Payload.Ready, "An unconfigured SDK-less Agent must fail closed.");
        Check.True(reply.Payload.ReadOnly, "SDK-less readiness must remain read-only.");
        Check.False(reply.Payload.CaptureCommandSent, "SDK-less readiness must not capture.");
        Check.False(reply.Payload.CameraObjectDeleteAttempted, "SDK-less readiness must not delete.");
        Console.WriteLine($"PASS SDK-less persistent Camera Agent typed fail-closed: {reply.Payload.FailureCategory}");
        return 0;
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task<int> RunPersistentCameraAgentTestChildAsync(
    string scenario,
    IReadOnlyList<string> arguments)
{
    var pipeIndex = arguments.ToList().IndexOf("--pipe-name");
    if (pipeIndex < 0 || pipeIndex + 1 >= arguments.Count)
    {
        return 64;
    }

    var tracePath = Environment.GetEnvironmentVariable("A0_CAMERA_AGENT_TEST_CHILD_TRACE");
    if (!string.IsNullOrWhiteSpace(tracePath))
    {
        await File.WriteAllTextAsync(
            tracePath,
            JsonSerializer.Serialize(new
            {
                executablePath = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M3.OperatorShellTests.exe")),
                workingDirectory = Environment.CurrentDirectory,
                arguments,
            }));
    }

    await using var pipe = new NamedPipeServerStream(
        arguments[pipeIndex + 1],
        PipeDirection.InOut,
        maxNumberOfServerInstances: 1,
        PipeTransmissionMode.Byte,
        PipeOptions.Asynchronous);
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
    await pipe.WaitForConnectionAsync(timeout.Token);
    var requestJson = await ReadPersistentTestFrameAsync(pipe, timeout.Token);
    using var request = JsonDocument.Parse(requestJson);

    if (scenario == "before-response")
    {
        WriteSyntheticSensitiveStderr();
        return 37;
    }
    if (scenario == "partial-header")
    {
        await pipe.WriteAsync(new byte[] { 20, 0 }, timeout.Token);
        await pipe.FlushAsync(timeout.Token);
        WriteSyntheticSensitiveStderr();
        return 37;
    }
    if (scenario == "partial-body")
    {
        var header = new byte[sizeof(int)];
        BinaryPrimitives.WriteInt32LittleEndian(header, 20);
        await pipe.WriteAsync(header, timeout.Token);
        await pipe.WriteAsync("{\"short\":"u8.ToArray(), timeout.Token);
        await pipe.FlushAsync(timeout.Token);
        WriteSyntheticSensitiveStderr();
        return 37;
    }
    if (scenario != "typed-fail-closed")
    {
        return 65;
    }

    var readiness = HardwareTestData.ReadyHardware("CAM-A") with
    {
        Ready = false,
        SdkCameraCount = 0,
        WpdCameraCount = 0,
        SdkIdentityBound = false,
        WpdIdentityBound = false,
        SdkAliasMatches = false,
        WpdAliasMatches = false,
        SdkStatusProbed = false,
        SpoolInspected = false,
        SpoolKnownEmpty = false,
        CaptureProfileApproved = false,
        CaptureProfileId = string.Empty,
        CaptureProfileVersion = 0,
        CaptureProfileSha256 = string.Empty,
        CaptureProfileCameraAlias = string.Empty,
        CaptureProfileExpiresAtUtc = null,
        CaptureProfileAliasMatches = false,
        SettingsMatchApprovedProfile = false,
        FailureCategory = "licensed_adapter_unavailable",
        FailureDetail = "licensed adapter is unavailable",
    };
    var responseJson = JsonSerializer.Serialize(
        new
        {
            schemaVersion = HardwareCameraAgentProtocol.SchemaVersion,
            simulation = false,
            marker = HardwareCameraAgentProtocol.Marker,
            requestId = request.RootElement.GetProperty("requestId").GetString(),
            success = true,
            resultCode = "SingleNotReady",
            payload = readiness,
        },
        new JsonSerializerOptions(JsonSerializerDefaults.Web));
    await WritePersistentTestFrameAsync(pipe, responseJson, timeout.Token);
    return 0;
}

static void WriteSyntheticSensitiveStderr() =>
    Console.Error.WriteLine(
        "synthetic child failed closed secret=super-secret path=C:\\private\\sdk " +
        "rawIdentity=RAW-CAMERA-IDENTITY requestId=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");

static async Task<string> ReadPersistentTestFrameAsync(Stream stream, CancellationToken cancellationToken)
{
    var header = new byte[sizeof(int)];
    await stream.ReadExactlyAsync(header, cancellationToken);
    var length = BinaryPrimitives.ReadInt32LittleEndian(header);
    var payload = new byte[length];
    await stream.ReadExactlyAsync(payload, cancellationToken);
    return Encoding.UTF8.GetString(payload);
}

static async Task WritePersistentTestFrameAsync(
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

static async Task HardwareSingleRequiresAndRepairsOperatorExportDirectoryAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var sourcePath = Path.Combine(root, "agent", "run-export-selection", "CAM-A", "original.jpg");
        var original = WriteJpegRecord(sourcePath, "CAM-A");
        var wrongDimensionsPath = Path.Combine(root, "agent", "wrong-dimensions", "CAM-A", "original.jpg");
        var wrongDimensions = WriteJpegRecord(wrongDimensionsPath, "CAM-A", preserveOnePixelDimensions: true);
        await Check.ThrowsAsync<InvalidDataException>(() =>
            HardwareArtifactVerifier.VerifyOriginalAsync(wrongDimensions));
        var operations = new FakeHardwareSingleCameraOperations
        {
            CaptureResultFactory = (transactionId, alias) =>
                CompleteCapture(transactionId, alias, original),
        };
        var preferences = new HardwareSinglePreferencesStore(
            Path.Combine(root, "state", "preferences.json"));
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            new HardwareSingleAppStateStore(Path.Combine(root, "state")),
            new HardwareOriginalExporter(Path.Combine(root, "default-not-selected")),
            preferences,
            profileStore: null);

        await viewModel.InitializeAsync();
        viewModel.ExclusiveCameraControlConfirmed = true;
        await viewModel.CheckReadinessAsync();
        viewModel.DedicatedSpoolScopeConfirmed = true;
        viewModel.ExactObjectDeleteConfirmed = true;

        Check.False(viewModel.CanCapture,
            "The product composition must not treat its default LocalAppData path as an operator choice.");
        Check.True(
            viewModel.BlockerText.Contains("保存先", StringComparison.Ordinal),
            "A missing operator export destination must be a visible capture blocker.");

        var firstChoice = Path.Combine(root, "operator-choice-1");
        await viewModel.ChangeExportDirectoryAsync(firstChoice);
        Check.True(viewModel.CanCapture, "An explicit fixed-local destination must release the capture gate.");
        await viewModel.CaptureAsync();
        Check.True(viewModel.CanExport, "A verified original must be exportable to the selected destination.");
        Check.True(viewModel.CanChangeExportDirectory,
            "A terminal capture must allow the operator to repair the destination before export.");

        var repairedChoice = Path.Combine(root, "operator-choice-2");
        await viewModel.ChangeExportDirectoryAsync(repairedChoice);
        await viewModel.ExportAsync();
        Check.Equal(Path.GetFullPath(repairedChoice), Path.GetDirectoryName(viewModel.LastExportPath)!);
        Check.True(
            File.ReadAllBytes(sourcePath).SequenceEqual(File.ReadAllBytes(viewModel.LastExportPath)),
            "The repaired destination must receive a byte-identical copy of the verified canonical original.");

        var loaded = await preferences.LoadAsync();
        Check.Equal(Path.GetFullPath(repairedChoice), loaded!.ExportDirectory);
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwareSinglePreferencesAndProfileApprovalAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var exportDirectory = Path.Combine(root, "selected-exports");
        Directory.CreateDirectory(exportDirectory);
        var preferences = new HardwareSinglePreferencesStore(Path.Combine(root, "state", "preferences.json"));
        await preferences.SaveAsync(exportDirectory);
        var loaded = await preferences.LoadAsync();
        Check.Equal(Path.GetFullPath(exportDirectory), loaded!.ExportDirectory);

        var profilePath = Path.Combine(root, "camera-agent", "approved-single-capture-profile.json");
        var now = DateTimeOffset.Parse("2026-08-10T01:02:03Z");
        var timeProvider = new MutableTimeProvider(now);
        var profileStore = new HardwareSingleCaptureProfileStore(profilePath, timeProvider);
        await profileStore.ApproveCamAAsync(ApprovedCamAObservedSettings());
        using var profile = JsonDocument.Parse(await File.ReadAllTextAsync(profilePath));
        var document = profile.RootElement;
        Check.Equal("a0.camera-agent.capture-profile.v1", document.GetProperty("schemaVersion").GetString() ?? string.Empty);
        Check.Equal("CAM-A", document.GetProperty("selectedAlias").GetString() ?? string.Empty);
        Check.Equal("SingleCamera", document.GetProperty("cameraMode").GetString() ?? string.Empty);
        Check.Equal("2026-09-09T01:02:03Z", document.GetProperty("expiresAtUtc").GetString() ?? string.Empty);
        Check.False(document.GetRawText().Contains("serial", StringComparison.OrdinalIgnoreCase), "Profile must not contain a camera serial.");
        Check.False(document.GetProperty("expectedSettings").GetProperty("fileType").GetProperty("available").GetBoolean(), "FileType must remain explicitly unavailable.");
        Check.Equal("JPEG Fine", document.GetProperty("expectedSettings").GetProperty("compressionLevel").GetProperty("currentLabel").GetString() ?? string.Empty);
        Check.Equal((uint)3, document.GetProperty("expectedSettings").GetProperty("exposureMode").GetProperty("currentValue").GetUInt32());
        Check.Equal("Preset 1", document.GetProperty("expectedSettings").GetProperty("whiteBalanceMode").GetProperty("currentLabel").GetString() ?? string.Empty);

        var rejectedPath = Path.Combine(root, "camera-agent", "rejected-profile.json");
        var rejectedStore = new HardwareSingleCaptureProfileStore(rejectedPath, timeProvider);
        await Check.ThrowsAsync<InvalidOperationException>(() =>
            rejectedStore.ApproveCamAAsync(ApprovedCamAObservedSettings() with
            {
                ExposureMode = ApprovedCamAObservedSettings().ExposureMode with { CurrentValue = 1 },
            }));
        Check.False(File.Exists(rejectedPath), "Rejected observations must not publish a profile.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static HardwareObservedCameraSettings ApprovedCamAObservedSettings()
{
    static HardwareObservedCameraSetting Setting(string label) => new()
    {
        Available = true,
        CapType = "enum",
        ProbeState = "observed",
        ValueType = "label",
        CurrentValue = null,
        CurrentIndex = 0,
        CurrentLabel = label,
    };
    return new HardwareObservedCameraSettings
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
        CompressionLevel = Setting("JPEG Fine"),
        ImageSize = Setting("L(7360*4912)"),
        ExposureMode = new HardwareObservedCameraSetting
        {
            Available = true,
            CapType = "enum",
            ProbeState = "available",
            ValueType = "unsigned",
            CurrentValue = 3,
            CurrentIndex = 3,
            CurrentLabel = null,
        },
        ShutterSpeed = Setting("1/6"),
        Aperture = Setting("8"),
        Sensitivity = Setting("64"),
        WhiteBalanceMode = Setting("Preset 1"),
        FocusMode = new HardwareObservedCameraSetting
        {
            Available = true,
            CapType = "generic",
            ProbeState = "available",
            ValueType = "unsigned",
            CurrentValue = 1,
            CurrentIndex = null,
            CurrentLabel = null,
        },
    };
}

static async Task HardwareSingleHappyPathAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var sourcePath = Path.Combine(root, "agent", "run-1000-1", "CAM-B", "original.jpg");
        var original = WriteJpegRecord(sourcePath, "CAM-B");
        var resumedPreview = WritePreviewRecord(
            Path.Combine(root, "agent", "run-1000-1", "live-view", "CAM-B", "preview.jpg"));
        var operations = new FakeHardwareSingleCameraOperations
        {
            Preview = resumedPreview,
            CaptureResultFactory = (transactionId, alias) =>
                CompleteCaptureWithHandoff(transactionId, alias, original, resumedPreview),
        };
        var store = new HardwareSingleAppStateStore(Path.Combine(root, "state"));
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            store,
            new HardwareOriginalExporter(Path.Combine(root, "exports")));
        await viewModel.InitializeAsync();
        viewModel.SelectedCamera = "CAM-B";
        viewModel.ExclusiveCameraControlConfirmed = true;
        await viewModel.CheckReadinessAsync();
        Check.True(viewModel.CanProbeLiveView, "Ready hardware must allow a finite Live View probe.");
        await viewModel.ProbeLiveViewAsync();
        Check.False(viewModel.CanCapture, "Finite Live View must require a fresh pre-capture readiness snapshot.");
        await viewModel.CheckReadinessAsync();
        viewModel.DedicatedSpoolScopeConfirmed = true;
        viewModel.ExactObjectDeleteConfirmed = true;
        Check.True(viewModel.CanCapture, "All confirmations plus readiness must enable one hardware capture.");

        var firstCapture = viewModel.CaptureAsync();
        var duplicateCapture = viewModel.CaptureAsync();
        await Task.WhenAll(firstCapture, duplicateCapture);
        Check.Equal(1, operations.CaptureCallCount);
        Check.True(operations.LastCaptureLiveViewHandoffRequested, "A selected finite Live View must request capture handoff and post-capture probe.");
        Check.Equal("single-profile", operations.LastCaptureExpectedProfile!.ProfileId);
        Check.True(viewModel.ReadinessDetail.Contains("2099-01-01", StringComparison.Ordinal), "The approved profile expiry must be visible before capture.");
        Check.True(viewModel.LiveViewSummary.Contains("有限1フレームprobe成功", StringComparison.Ordinal), "The post-capture finite Live View result must not claim a continuous stream.");
        Check.True(viewModel.CaptureSummary.Contains("成功", StringComparison.Ordinal), "Hardware capture must reach reviewed success.");
        Check.True(viewModel.StitchSummary.Contains("対象外", StringComparison.Ordinal), "Single hardware mode must never claim stitch output.");
        Check.True(viewModel.CanExport, "A reread-verified complete original must be explicitly exportable.");

        await viewModel.ExportAsync();
        Check.True(File.Exists(viewModel.LastExportPath), "Explicit hardware export must create a file.");
        Check.True(
            File.ReadAllBytes(sourcePath).SequenceEqual(File.ReadAllBytes(viewModel.LastExportPath)),
            "Hardware export must be byte-identical to the canonical original.");
        Check.True(viewModel.ExportSummary.Contains("byte-identical", StringComparison.Ordinal), "The UI must label the single export provenance.");
        Check.True(await store.LoadPendingAsync() is not null, "A terminal result must remain durable until explicit operator preparation.");
        await viewModel.PrepareNewCaptureAsync();
        Check.True(await store.LoadPendingAsync() is null, "Only explicit new-capture preparation may clear the app transaction marker.");
        Check.Equal(string.Empty, viewModel.PreviewPath);
        Check.True(
            viewModel.LiveViewSummary.Contains("未実行", StringComparison.Ordinal),
            "Preparing a new transaction must not display the previous transaction's post-capture preview.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwarePendingTransactionRecoveryAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        const string transactionId = "0123456789abcdef0123456789abcdef";
        var sourcePath = Path.Combine(root, "agent", "run-2000-1", "CAM-A", "original.jpg");
        var original = WriteJpegRecord(sourcePath, "CAM-A");
        var store = new HardwareSingleAppStateStore(Path.Combine(root, "state"));
        await store.SavePendingAsync(new HardwarePendingTransaction
        {
            OperatingMode = "SingleCamera",
            TransactionId = transactionId,
            CameraAlias = "CAM-A",
            RequiredCameraAlias = "CAM-A",
            CaptureProfileId = "single-profile",
            CaptureProfileVersion = 1,
            CaptureProfileSha256 = new string('a', 64),
            CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
            LiveViewHandoffRequested = false,
            CaptureRequestDispatchAttempted = true,
            StartedAtUtc = DateTimeOffset.UtcNow,
        });
        var operations = new FakeHardwareSingleCameraOperations();
        operations.TransactionResults.Enqueue(ReservedCapture(transactionId, "CAM-A"));
        operations.TransactionResults.Enqueue(InProgressCapture(transactionId, "CAM-A"));
        operations.TransactionResults.Enqueue(FailedPartialCapture(transactionId, "CAM-A", original));
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            store,
            new HardwareOriginalExporter(Path.Combine(root, "exports")));
        await viewModel.InitializeAsync();

        Check.False(viewModel.CanCapture, "A pending hardware transaction must block a new shutter operation.");
        Check.Equal("CAM-A", viewModel.SelectedCamera);
        Check.Equal(0, operations.CaptureCallCount);
        Check.Equal(1, operations.TransactionResultCallCount);
        Check.Equal("single-profile", operations.LastTransactionExpectedProfile!.ProfileId);
        Check.Equal((uint)1, operations.LastTransactionExpectedProfile.ProfileVersion);
        Check.Equal(new string('a', 64), operations.LastTransactionExpectedProfile.Sha256);
        Check.Equal(DateTimeOffset.Parse("2099-01-01T00:00:00Z"), operations.LastTransactionExpectedProfile.ExpiresAtUtc);
        Check.Equal("CAM-A", operations.LastTransactionExpectedCameraAlias!);
        Check.False(operations.LastTransactionExpectedHandoff, "Startup recovery must preserve the original handoff choice.");
        Check.False(viewModel.CanPrepareNewCapture, "Startup recovery must keep a reserved transaction nonterminal.");
        Check.True(await store.LoadPendingAsync() is not null, "Startup recovery must preserve the durable pending marker.");
        await viewModel.RecoverTransactionAsync();
        Check.False(viewModel.CanCapture, "TransactionInProgress must keep new capture blocked.");
        Check.True(await store.LoadPendingAsync() is not null, "In-progress lookup must preserve the durable pending marker.");
        await viewModel.RecoverTransactionAsync();

        Check.Equal(0, operations.CaptureCallCount);
        Check.Equal(3, operations.TransactionResultCallCount);
        Check.True(viewModel.CaptureSummary.Contains("FailedPartial", StringComparison.Ordinal), "Recovered cleanup failure must remain FailedPartial.");
        Check.True(viewModel.CanExport, "A FailedPartial transaction may explicitly export its reread-verified retained original.");
        await viewModel.ExportAsync();
        Check.True(
            File.Exists(viewModel.LastExportPath),
            $"FailedPartial retained original export must be explicit and byte-verified. {viewModel.TechnicalDetail}");
        Check.True(viewModel.ExportSummary.Contains("FailedPartial", StringComparison.Ordinal), "Export must preserve the transaction failure label.");
        Check.True(await store.LoadPendingAsync() is not null, "Recovered terminal result must remain discoverable until operator preparation.");
        await viewModel.PrepareNewCaptureAsync();
        Check.True(await store.LoadPendingAsync() is null, "Explicit preparation must clear the recovered transaction marker.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwareContinuousLiveViewCaptureHandoffAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var sourcePath = Path.Combine(root, "agent", "run-live-1", "CAM-A", "original.jpg");
        var original = WriteJpegRecord(sourcePath, "CAM-A");
        var framePath = Path.Combine(root, "agent", "run-live-1", "preview.jpg");
        var frameBytes = File.ReadAllBytes(WritePreviewRecord(framePath).Path);
        var operations = new FakeContinuousHardwareOperations(frameBytes)
        {
            CaptureResultFactory = (transactionId, alias) =>
                CompleteCapture(transactionId, alias, original),
        };
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            new HardwareSingleAppStateStore(Path.Combine(root, "state")),
            new HardwareOriginalExporter(Path.Combine(root, "exports")));
        await viewModel.InitializeAsync();
        viewModel.ExclusiveCameraControlConfirmed = true;
        await viewModel.CheckReadinessAsync();
        viewModel.DedicatedSpoolScopeConfirmed = true;
        viewModel.ExactObjectDeleteConfirmed = true;

        Check.True(viewModel.CanStartContinuousLiveView, "Ready CAM-A must allow continuous Live View v2.");
        await viewModel.StartContinuousLiveViewAsync();
        await operations.FirstFrame.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Check.True(viewModel.IsContinuousLiveViewActive, "The UI must expose the owned active Live View session.");
        Check.True(viewModel.PreviewImage is not null, "A verified in-memory JPEG frame must be displayed.");

        await viewModel.CaptureAsync();
        Check.Equal(1, operations.CaptureCallCount);
        Check.False(operations.LastCaptureLiveViewHandoffRequested,
            "Continuous v2 must stop explicitly and must not be relabelled as the finite v1 handoff.");
        Check.Equal(2, operations.StartCount);
        Check.True(viewModel.IsContinuousLiveViewActive,
            "A successful terminal capture must restart the continuous Live View session.");
        Check.True(
            operations.CallOrder.IndexOf("stop") < operations.CallOrder.IndexOf("capture"),
            "SDK Live View stop and close must precede capture dispatch.");

        await viewModel.StopContinuousLiveViewAsync();
        Check.False(viewModel.IsContinuousLiveViewActive, "Explicit stop must close the continuous session.");
        await viewModel.ShutdownAsync();
        viewModel.Dispose();

        var blockedOperations = new FakeContinuousHardwareOperations(frameBytes)
        {
            CaptureResultFactory = (transactionId, alias) =>
                CompleteCapture(transactionId, alias, original),
            FailNextStop = true,
        };
        var blockedViewModel = new HardwareSingleCameraViewModel(
            blockedOperations,
            new HardwareSingleAppStateStore(Path.Combine(root, "blocked-state")),
            new HardwareOriginalExporter(Path.Combine(root, "blocked-exports")));
        await blockedViewModel.InitializeAsync();
        blockedViewModel.ExclusiveCameraControlConfirmed = true;
        await blockedViewModel.CheckReadinessAsync();
        blockedViewModel.DedicatedSpoolScopeConfirmed = true;
        blockedViewModel.ExactObjectDeleteConfirmed = true;
        await blockedViewModel.StartContinuousLiveViewAsync();
        await blockedOperations.FirstFrame.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await blockedViewModel.CaptureAsync();
        Check.Equal(0, blockedOperations.CaptureCallCount);
        Check.False(blockedViewModel.IsContinuousLiveViewActive,
            "A failed stop whose typed result confirms SDK closed must invalidate the stale session.");
        Check.False(blockedViewModel.CanCapture,
            "A failed stop must invalidate readiness and keep capture disabled.");
        await blockedViewModel.ShutdownAsync();
        blockedViewModel.Dispose();
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwareLiveViewRequiresFreshReadinessAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var preview = WritePreviewRecord(Path.Combine(root, "agent", "run-3000-1", "live-view", "CAM-A", "preview.jpg"));
        var operations = new FakeHardwareSingleCameraOperations
        {
            Preview = preview,
            ReadinessFactory = alias => HardwareTestData.ReadyHardware(alias) with
            {
                Ready = false,
                CaptureProfileApproved = false,
                CaptureProfileId = string.Empty,
                CaptureProfileVersion = 0,
                CaptureProfileSha256 = string.Empty,
                CaptureProfileCameraAlias = string.Empty,
                CaptureProfileExpiresAtUtc = null,
                CaptureProfileAliasMatches = false,
                SettingsMatchApprovedProfile = false,
                FailureCategory = "capture_profile_not_approved",
                FailureDetail = "No approved Single profile is configured.",
            },
        };
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            new HardwareSingleAppStateStore(Path.Combine(root, "state")),
            new HardwareOriginalExporter(Path.Combine(root, "exports")));
        await viewModel.InitializeAsync();
        viewModel.ExclusiveCameraControlConfirmed = true;
        await viewModel.CheckReadinessAsync();
        Check.True(viewModel.CanProbeLiveView, "A bound read-only camera may preview even while capture profile approval blocks shutter.");
        Check.False(viewModel.CanCapture, "Unapproved capture profile must keep shutter blocked.");
        await viewModel.ProbeLiveViewAsync();

        Check.Equal(1, operations.LiveViewCallCount);
        Check.Equal(preview.Path, viewModel.PreviewPath);
        Check.True(viewModel.HasPreview, "Verified finite preview must be displayed.");
        Check.False(viewModel.CanCapture, "Live View handoff must invalidate readiness before capture.");
        Check.True(viewModel.ReadinessSummary.Contains("再確認", StringComparison.Ordinal), "Fresh readiness must be visible after Live View close.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwareMalformedLocalStateFailsClosedAsync()
{
    Check.True(
        WindowsLocalPathGuard.IsSupportedLocalDrive(DriveType.Fixed),
        "A fixed Windows drive must satisfy the product-local storage boundary.");
    Check.False(
        WindowsLocalPathGuard.IsSupportedLocalDrive(DriveType.Network),
        "A mapped network drive must not be accepted as local product storage.");
    Check.False(
        WindowsLocalPathGuard.IsSupportedLocalDrive(DriveType.Removable),
        "Removable storage must not host durable application state or accepted exports.");

    var root = CreateHardwareTestRoot();
    try
    {
        var stateDirectory = Path.Combine(root, "state");
        Directory.CreateDirectory(stateDirectory);
        await File.WriteAllTextAsync(
            Path.Combine(stateDirectory, "app-state.json"),
            "{\"schemaVersion\":\"a0.hardware-single-app-state.v1\",\"pendingTransaction\":null,\"unexpected\":true}");
        var operations = new FakeHardwareSingleCameraOperations();
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            new HardwareSingleAppStateStore(stateDirectory),
            new HardwareOriginalExporter(Path.Combine(root, "exports")));
        await viewModel.InitializeAsync();

        Check.False(viewModel.CanCheckReadiness, "Malformed local state must block even read-only hardware access.");
        Check.False(viewModel.CanCapture, "Malformed local state must block capture.");
        Check.True(viewModel.BlockerText.Contains("安全に読めません", StringComparison.Ordinal), "Fail-closed state must be explicit.");
        Check.Equal(0, operations.TotalCallCount);

        var duplicateDirectory = Path.Combine(root, "duplicate-state");
        Directory.CreateDirectory(duplicateDirectory);
        await File.WriteAllTextAsync(
            Path.Combine(duplicateDirectory, "app-state.json"),
            $"{{\"schemaVersion\":\"{HardwareSingleAppStateProtocol.SchemaVersion}\",\"schemaVersion\":\"{HardwareSingleAppStateProtocol.SchemaVersion}\",\"pendingTransaction\":null}}");
        var duplicateOperations = new FakeHardwareSingleCameraOperations();
        var duplicateViewModel = new HardwareSingleCameraViewModel(
            duplicateOperations,
            new HardwareSingleAppStateStore(duplicateDirectory),
            new HardwareOriginalExporter(Path.Combine(root, "duplicate-exports")));
        await duplicateViewModel.InitializeAsync();
        Check.False(duplicateViewModel.CanCheckReadiness, "Duplicate state properties must fail closed.");
        Check.Equal(0, duplicateOperations.TotalCallCount);

        var oversizedDirectory = Path.Combine(root, "oversized-state");
        Directory.CreateDirectory(oversizedDirectory);
        await File.WriteAllBytesAsync(
            Path.Combine(oversizedDirectory, "app-state.json"),
            new byte[65 * 1024]);
        var oversizedOperations = new FakeHardwareSingleCameraOperations();
        var oversizedViewModel = new HardwareSingleCameraViewModel(
            oversizedOperations,
            new HardwareSingleAppStateStore(oversizedDirectory),
            new HardwareOriginalExporter(Path.Combine(root, "oversized-exports")));
        await oversizedViewModel.InitializeAsync();
        Check.False(oversizedViewModel.CanCheckReadiness, "Oversized state must fail closed before parsing.");
        Check.Equal(0, oversizedOperations.TotalCallCount);
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static void HardwareLaunchOptionsAreExplicit()
{
    var baseDirectory = Path.Combine(Path.GetTempPath(), "A0CameraStitcher-launch");
    var launcher = ApplicationLaunchOptions.Parse([], baseDirectory);
    Check.Equal(ApplicationLaunchMode.Launcher, launcher.Mode);
    var hardware = ApplicationLaunchOptions.Parse(["--hardware-single", "--camera-agent", "C:\\agent\\A0CameraStitcher.CameraAgent.exe"], baseDirectory);
    Check.Equal(ApplicationLaunchMode.HardwareSingle, hardware.Mode);
    var hardwareDual = ApplicationLaunchOptions.Parse(["--hardware-dual"], baseDirectory);
    Check.Equal(ApplicationLaunchMode.HardwareDual, hardwareDual.Mode);
    var simulated = ApplicationLaunchOptions.Parse(["--simulated"], baseDirectory);
    Check.Equal(ApplicationLaunchMode.Simulated, simulated.Mode);
    Check.Throws<ArgumentException>(() => ApplicationLaunchOptions.Parse(["--simulated", "--hardware-single"], baseDirectory));
    Check.Throws<ArgumentException>(() => ApplicationLaunchOptions.Parse(["--hardware-dual", "--hardware-single"], baseDirectory));
    Check.Throws<ArgumentException>(() => ApplicationLaunchOptions.Parse(["--hardware-dual", "--camera-agent", "C:\\agent\\agent.exe"], baseDirectory));
    Check.Throws<ArgumentException>(() => ApplicationLaunchOptions.Parse(["--simulated", "--camera-agent", "C:\\agent\\agent.exe"], baseDirectory));
    Check.Throws<InvalidDataException>(() => HardwareSingleStoragePaths.Resolve(string.Empty));
    Check.Throws<InvalidDataException>(() => HardwareSingleStoragePaths.Resolve("relative-local-app-data"));
    var storagePaths = HardwareSingleStoragePaths.Resolve(Path.GetTempPath());
    Check.True(Path.IsPathFullyQualified(storagePaths.StateDirectory), "Hardware state must derive from an absolute LocalApplicationData root.");
    Check.True(
        storagePaths.ExportDirectory.StartsWith(Path.GetFullPath(Path.GetTempPath()), StringComparison.OrdinalIgnoreCase),
        "Hardware exports must stay below the validated LocalApplicationData root.");
}

static async Task HardwareAppStateCompareAndSetAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var store = new HardwareSingleAppStateStore(Path.Combine(root, "state"));
        var first = new HardwarePendingTransaction
        {
            OperatingMode = "SingleCamera",
            TransactionId = "11111111111111111111111111111111",
            CameraAlias = "CAM-A",
            RequiredCameraAlias = "CAM-A",
            CaptureProfileId = "single-profile",
            CaptureProfileVersion = 1,
            CaptureProfileSha256 = new string('a', 64),
            CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
            LiveViewHandoffRequested = false,
            CaptureRequestDispatchAttempted = false,
            StartedAtUtc = DateTimeOffset.UtcNow,
        };
        var second = new HardwarePendingTransaction
        {
            OperatingMode = "SingleCamera",
            TransactionId = "22222222222222222222222222222222",
            CameraAlias = "CAM-B",
            RequiredCameraAlias = "CAM-B",
            CaptureProfileId = "single-profile",
            CaptureProfileVersion = 1,
            CaptureProfileSha256 = new string('b', 64),
            CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
            LiveViewHandoffRequested = true,
            CaptureRequestDispatchAttempted = false,
            StartedAtUtc = DateTimeOffset.UtcNow,
        };
        await store.SavePendingAsync(first);
        await store.MarkCaptureRequestDispatchAttemptedAsync(first.TransactionId);
        Check.True((await store.LoadPendingAsync())!.CaptureRequestDispatchAttempted, "Dispatch intent must be persisted before invoking the Camera Agent.");
        await Check.ThrowsAsync<InvalidOperationException>(() => store.SavePendingAsync(second));
        Check.Equal(first.TransactionId, (await store.LoadPendingAsync())!.TransactionId);
        await Check.ThrowsAsync<InvalidOperationException>(() => store.ClearPendingAsync(second.TransactionId));
        Check.Equal(first.TransactionId, (await store.LoadPendingAsync())!.TransactionId);
        await store.ClearPendingAsync(first.TransactionId);
        Check.True(await store.LoadPendingAsync() is null, "Expected transaction compare-and-clear must succeed.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static void HardwareOperatorSessionLeaseIsExclusive()
{
    using var acquired = new ManualResetEventSlim(false);
    using var release = new ManualResetEventSlim(false);
    Exception? ownerFailure = null;
    var owner = new Thread(() =>
    {
        try
        {
            using var lease = HardwareSingleAppSessionLease.Acquire();
            acquired.Set();
            release.Wait(TimeSpan.FromSeconds(10));
        }
        catch (Exception exception)
        {
            ownerFailure = exception;
            acquired.Set();
        }
    });
    owner.Start();
    Check.True(acquired.Wait(TimeSpan.FromSeconds(5)), "The hardware session lease owner did not start.");
    if (ownerFailure is not null)
    {
        throw new InvalidOperationException("The hardware session lease owner failed.", ownerFailure);
    }

    try
    {
        Check.Throws<HardwareSingleAppSessionBusyException>(() => HardwareSingleAppSessionLease.Acquire());
    }
    finally
    {
        release.Set();
        Check.True(owner.Join(millisecondsTimeout: 5000), "The hardware session lease owner did not exit.");
    }

    using var reacquired = HardwareSingleAppSessionLease.Acquire();
}

static async Task HardwareExportVerificationFailureStaysUnpublishedAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var sourcePath = Path.Combine(root, "agent", "run-4000-1", "CAM-A", "original.jpg");
        var original = WriteJpegRecord(sourcePath, "CAM-A");
        var exportDirectory = Path.Combine(root, "exports");
        var exporter = new HardwareOriginalExporter(
            exportDirectory,
            (stagingPath, cancellationToken) =>
                File.AppendAllTextAsync(
                    stagingPath,
                    "tampered-after-locked-verification",
                    cancellationToken));

        await Check.ThrowsAsync<IOException>(() => exporter.ExportAsync(
            original,
            "44444444444444444444444444444444",
            DateTimeOffset.Parse("2026-08-10T00:00:00Z")));

        Check.Equal(0, Directory.GetFiles(exportDirectory, "*.jpg", SearchOption.TopDirectoryOnly).Length);
        Check.Equal(1, Directory.GetFiles(exportDirectory, "*.partial", SearchOption.TopDirectoryOnly).Length);

        var replacementDirectory = Path.Combine(root, "replacement-exports");
        var replacementHookRan = false;
        var exactHandleExporter = new HardwareOriginalExporter(
            replacementDirectory,
            async (stagingPath, cancellationToken) =>
            {
                File.Move(stagingPath, stagingPath + ".verified-handle");
                await File.WriteAllTextAsync(
                    stagingPath,
                    "unverified path replacement",
                    cancellationToken);
                replacementHookRan = true;
            });
        var finalPath = await exactHandleExporter.ExportAsync(
            original,
            "45454545454545454545454545454545",
            DateTimeOffset.Parse("2026-08-10T00:00:01Z"));
        Check.True(replacementHookRan, "The post-verification replacement seam must execute.");
        Check.True(
            File.Exists(finalPath),
            $"Handle-based publication did not create the expected path. Files: {string.Join(", ", Directory.GetFiles(replacementDirectory, "*", SearchOption.AllDirectories))}");
        Check.True(
            File.ReadAllBytes(sourcePath).SequenceEqual(File.ReadAllBytes(finalPath)),
            "Handle-based publication must publish the verified file identity, not a path replacement.");
        Check.Equal(
            "unverified path replacement",
            await File.ReadAllTextAsync(finalPath + ".partial"));
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwarePreDispatchAndNotFoundRecoveryBoundariesAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var preparedStore = new HardwareSingleAppStateStore(Path.Combine(root, "prepared-state"));
        var prepared = HardwarePending(
            "55555555555555555555555555555555",
            "CAM-A",
            captureRequestDispatchAttempted: false);
        await preparedStore.SavePendingAsync(prepared);
        var preparedOperations = new FakeHardwareSingleCameraOperations();
        var preparedViewModel = new HardwareSingleCameraViewModel(
            preparedOperations,
            preparedStore,
            new HardwareOriginalExporter(Path.Combine(root, "prepared-exports")));

        await preparedViewModel.InitializeAsync();
        Check.Equal(0, preparedOperations.TotalCallCount);
        Check.True(preparedViewModel.CanPrepareNewCapture, "A durable known pre-dispatch failure must be explicitly closable without querying hardware.");
        Check.True(preparedViewModel.CaptureSummary.Contains("撮影要求0回", StringComparison.Ordinal), "The zero-dispatch provenance must be visible.");
        await preparedViewModel.PrepareNewCaptureAsync();
        Check.True(await preparedStore.LoadPendingAsync() is null, "Explicit preparation must clear only the known pre-dispatch marker.");

        var launchFailureStore = new HardwareSingleAppStateStore(Path.Combine(root, "launch-failure-state"));
        var launchFailureOperations = new FakeHardwareSingleCameraOperations
        {
            CaptureException = new HardwareCameraAgentLaunchException("Synthetic process start failure."),
        };
        var launchFailureViewModel = new HardwareSingleCameraViewModel(
            launchFailureOperations,
            launchFailureStore,
            new HardwareOriginalExporter(Path.Combine(root, "launch-failure-exports")));
        await launchFailureViewModel.InitializeAsync();
        launchFailureViewModel.ExclusiveCameraControlConfirmed = true;
        launchFailureViewModel.DedicatedSpoolScopeConfirmed = true;
        launchFailureViewModel.ExactObjectDeleteConfirmed = true;
        await launchFailureViewModel.CheckReadinessAsync();
        await launchFailureViewModel.CaptureAsync();
        Check.True(launchFailureViewModel.CanPrepareNewCapture, "A typed undispatched launch failure must be durably closable as zero shutter.");
        Check.False((await launchFailureStore.LoadPendingAsync())!.CaptureRequestDispatchAttempted, "Typed pre-dispatch failure must durably restore known-undispatched provenance.");
        await launchFailureViewModel.PrepareNewCaptureAsync();
        Check.True(await launchFailureStore.LoadPendingAsync() is null, "Explicit preparation must clear the typed undispatched failure.");

        var ambiguousStore = new HardwareSingleAppStateStore(Path.Combine(root, "ambiguous-state"));
        var ambiguous = HardwarePending(
            "66666666666666666666666666666666",
            "CAM-B",
            captureRequestDispatchAttempted: true,
            liveViewHandoffRequested: true);
        await ambiguousStore.SavePendingAsync(ambiguous);
        var ambiguousOperations = new FakeHardwareSingleCameraOperations();
        ambiguousOperations.TransactionResults.Enqueue(TransactionNotFoundCapture(
            ambiguous.TransactionId,
            ambiguous.CameraAlias));
        var ambiguousViewModel = new HardwareSingleCameraViewModel(
            ambiguousOperations,
            ambiguousStore,
            new HardwareOriginalExporter(Path.Combine(root, "ambiguous-exports")));

        await ambiguousViewModel.InitializeAsync();
        Check.Equal(0, ambiguousOperations.CaptureCallCount);
        Check.Equal(1, ambiguousOperations.TransactionResultCallCount);
        Check.Equal("CAM-B", ambiguousOperations.LastTransactionExpectedCameraAlias!);
        Check.True(ambiguousOperations.LastTransactionExpectedHandoff, "Ambiguous restart recovery must preserve a true handoff snapshot.");
        Check.False(ambiguousViewModel.CanPrepareNewCapture, "An attempted dispatch with no journal must remain support-required and block a new shutter.");
        Check.True(ambiguousViewModel.CaptureSummary.Contains("TransactionNotFound", StringComparison.Ordinal), "The missing journal blocker must remain explicit.");
        Check.True(await ambiguousStore.LoadPendingAsync() is not null, "Ambiguous missing-journal state must never be cleared automatically.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwareInitializationAndProfileExpiryGateAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var blockingStore = new BlockingHardwareStateStore();
        var startupOperations = new FakeHardwareSingleCameraOperations();
        var startupViewModel = new HardwareSingleCameraViewModel(
            startupOperations,
            blockingStore,
            new HardwareOriginalExporter(Path.Combine(root, "startup-exports")));
        Check.False(startupViewModel.CanChangeConfirmations, "Hardware controls must be disabled before startup state inspection begins.");
        Check.False(startupViewModel.CanCheckReadiness, "Readiness must be disabled before startup state inspection.");

        var initialize = startupViewModel.InitializeAsync();
        await blockingStore.LoadStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        startupViewModel.ExclusiveCameraControlConfirmed = true;
        await startupViewModel.CheckReadinessAsync();
        await startupViewModel.CaptureAsync();
        Check.False(startupViewModel.ExclusiveCameraControlConfirmed, "Startup-time confirmation changes must be ignored.");
        Check.Equal(0, startupOperations.TotalCallCount);
        blockingStore.ReleaseLoad();
        await initialize;
        Check.True(startupViewModel.CanChangeConfirmations, "Controls may open only after durable startup inspection completes.");

        var now = DateTimeOffset.Parse("2026-08-10T10:00:00Z");
        var timeProvider = new MutableTimeProvider(now);
        var expiryOperations = new FakeHardwareSingleCameraOperations
        {
            ReadinessFactory = alias => HardwareTestData.ReadyHardware(alias) with
            {
                CaptureProfileExpiresAtUtc = now.AddSeconds(1),
            },
        };
        var expiryViewModel = new HardwareSingleCameraViewModel(
            expiryOperations,
            new HardwareSingleAppStateStore(Path.Combine(root, "expiry-state")),
            new HardwareOriginalExporter(Path.Combine(root, "expiry-exports")),
            timeProvider);
        await expiryViewModel.InitializeAsync();
        expiryViewModel.ExclusiveCameraControlConfirmed = true;
        expiryViewModel.DedicatedSpoolScopeConfirmed = true;
        expiryViewModel.ExactObjectDeleteConfirmed = true;
        await expiryViewModel.CheckReadinessAsync();
        Check.True(expiryViewModel.CanCapture, "A future approved profile may enable capture.");
        timeProvider.Advance(TimeSpan.FromSeconds(2));
        Check.False(expiryViewModel.CanCapture, "Local capture availability must close when the frozen profile expires.");
        Check.Equal("期限切れ", expiryViewModel.ReadinessSummary);
        Check.True(
            expiryViewModel.BlockerText.Contains("capture_profile_expired", StringComparison.Ordinal),
            "Profile expiry must immediately become a visible blocker without waiting for a click.");
        await expiryViewModel.CaptureAsync();
        Check.Equal(0, expiryOperations.CaptureCallCount);
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task DualCameraRegressionAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-OperatorShellTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    string? exportPath = null;
    try
    {
        var viewModel = new OperatorShellViewModel(new SimulationFoundationService(root));
        await viewModel.InitializeAsync(CancellationToken.None);
        Check.False(viewModel.IsSingleCameraMode, "Dual mode must remain the safe default for the existing workflow.");
        Check.True(viewModel.CaptureButtonText.Contains("2台", StringComparison.Ordinal), "The dual action must remain explicit.");

        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.CanCapture, "A ready dual plan must allow capture.");
        viewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(() => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy, "The dual workflow did not finish.");

        Check.Equal(OperatorUiState.Review, viewModel.UiState);
        Check.True(viewModel.RetainedOriginals.Contains("CAM-A", StringComparison.Ordinal), "Dual mode must retain CAM-A.");
        Check.True(viewModel.RetainedOriginals.Contains("CAM-B", StringComparison.Ordinal), "Dual mode must retain CAM-B.");
        Check.False(viewModel.StitchResult.Contains("対象外", StringComparison.Ordinal), "Dual mode must still produce a stitch result.");
        Check.True(viewModel.CanExport, "A reviewed dual stitch must remain exportable.");
        Check.True(viewModel.CanRestitch, "Two retained originals must remain restitchable.");

        var firstStitchJob = viewModel.LastStitchJobId;
        viewModel.RestitchCommand.Execute(null);
        Check.False(
            string.Equals(firstStitchJob, viewModel.LastStitchJobId, StringComparison.Ordinal),
            "Restitch must create a distinct stitch job.");

        viewModel.ExportCommand.Execute(null);
        exportPath = viewModel.LastExportPath;
        Check.True(File.Exists(exportPath), "The explicit simulated dual export must exist.");
        Check.True(viewModel.ExportResult.Contains("合成出力", StringComparison.Ordinal), "The dual export must remain labeled as stitched output.");
    }
    finally
    {
        if (!string.IsNullOrWhiteSpace(exportPath) && File.Exists(exportPath))
        {
            File.Delete(exportPath);
        }
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task DualIdentityBlocksWpfCaptureAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-DualIdentityWpfTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var bridge = new NeverCaptureDualBridge();
        var flow = new DualCameraProductFlow(
            Path.Combine(root, "products"),
            bridge,
            bridge,
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.HardwarePending()));
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "journals")),
            flow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.False(viewModel.CanCapture, "HardwarePending identity must close the WPF capture gate.");
        Check.True(viewModel.CaptureDisabledReason.Contains("HardwarePending", StringComparison.Ordinal),
            "The typed identity blocker must be visible without identifiers.");
        viewModel.CaptureCommand.Execute(null);
        Check.Equal(0, viewModel.TransactionStartCount);
        Check.Equal(0, bridge.CaptureCalls);
    }
    finally
    {
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static async Task DualIdentityExpiryBlocksWpfCaptureAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-DualIdentityExpiryWpfTests",
        Guid.NewGuid().ToString("N"));
    var exportRoot = Path.Combine(root, "export");
    Directory.CreateDirectory(exportRoot);
    try
    {
        var now = DateTimeOffset.Parse("2026-08-11T00:00:00Z");
        var time = new MutableTimeProvider(now);
        var finiteReady = new DualCameraIdentitySnapshot(
            DualCameraIdentityStatus.Ready,
            "ready",
            now.AddMinutes(-1),
            now.AddSeconds(1));
        var bridge = new NeverCaptureDualBridge();
        var flow = new DualCameraProductFlow(
            Path.Combine(root, "products"),
            bridge,
            bridge,
            new FixedDualCameraIdentitySnapshotSource(finiteReady),
            time);
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "journals")),
            flow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.FixedLocalExportDirectory = exportRoot;
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.CanCapture, "A finite unexpired Ready snapshot must allow WPF capture.");
        time.Advance(TimeSpan.FromSeconds(2));
        Check.False(viewModel.CanCapture, "An expired Ready snapshot must close the WPF capture gate.");
        Check.True(viewModel.CaptureDisabledReason.Contains("Expired", StringComparison.Ordinal),
            "The WPF blocker must expose the normalized Expired state.");
        viewModel.CaptureCommand.Execute(null);
        Check.Equal(0, viewModel.TransactionStartCount);
        Check.Equal(0, bridge.CaptureCalls);
    }
    finally
    {
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static async Task FormalDualCameraWpfFlowAsync()
{
    var previousAdapterPath = Environment.GetEnvironmentVariable("A0_M2_ADAPTER_PATH");
    Environment.SetEnvironmentVariable("A0_M2_ADAPTER_PATH", null);
    var bundledAdapterPath = Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M2Adapter.exe");
    Check.True(File.Exists(bundledAdapterPath), "The formal WPF output must bundle A0CameraStitcher.M2Adapter.exe.");
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-FormalDualWpfTests",
        Guid.NewGuid().ToString("N"));
    var transactionRoot = Path.Combine(root, "legacy-journals");
    var productRoot = Path.Combine(root, "products");
    var exportRoot = Path.Combine(root, "operator-export");
    Directory.CreateDirectory(transactionRoot);
    Directory.CreateDirectory(exportRoot);
    try
    {
        var productFlow = DualCameraProductComposition.Create(productRoot);
        var pendingHardwareFlow = DualCameraProductComposition.Create(
            Path.Combine(root, "hardware-pending-products"),
            DualCameraExecutionEnvironment.HardwareDual);
        var pendingViewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "hardware-pending-journals")),
            pendingHardwareFlow);
        await pendingViewModel.InitializeAsync(CancellationToken.None);
        pendingViewModel.FixedLocalExportDirectory = exportRoot;
        pendingViewModel.AcceptSafetyCommand.Execute(null);
        Check.Equal(DualCameraIdentityStatus.HardwarePending, pendingHardwareFlow.IdentitySnapshot.Status);
        Check.False(pendingViewModel.CanCapture, "HardwareDual production composition must remain HardwarePending without provider/Agent operations.");
        Check.Equal(OperatorShellViewModel.HardwareDualPendingBanner, pendingViewModel.BannerText);

        var hardwareAdapter = new M2OfflineStitcherProcessAdapter(bundledAdapterPath);
        var hardwareOperations = new WpfHardwareDualFakeOperations(hardwareAdapter);
        var hardwareFlow = new DualCameraProductFlow(
            Path.Combine(root, "hardware-fake-products"),
            new HardwareDualCaptureSource(hardwareOperations),
            hardwareAdapter,
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));
        var noRequestProviderViewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "hardware-no-request-provider-journals")),
            hardwareFlow);
        await noRequestProviderViewModel.InitializeAsync(CancellationToken.None);
        noRequestProviderViewModel.FixedLocalExportDirectory = exportRoot;
        noRequestProviderViewModel.AcceptSafetyCommand.Execute(null);
        Check.False(noRequestProviderViewModel.CanCapture, "Ready identity alone must not bypass approved profile and explicit confirmation injection.");
        Check.True(noRequestProviderViewModel.CaptureDisabledReason.Contains("explicit operator confirmations", StringComparison.Ordinal), "The WPF blocker must identify the missing HardwareDual request boundary.");
        var hardwareViewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "hardware-fake-journals")),
            hardwareFlow,
            () => DualCameraCaptureRequest.CreateHardwareDual(
                DualCameraRigProfile.ApprovedSynthetic(),
                HardwareDualCaptureProfile.ApprovedSynthetic(),
                new HardwareDualOperatorConfirmations(true, true, true, true, true),
                Guid.NewGuid()));
        await hardwareViewModel.InitializeAsync(CancellationToken.None);
        hardwareViewModel.FixedLocalExportDirectory = exportRoot;
        hardwareViewModel.AcceptSafetyCommand.Execute(null);
        Check.True(hardwareViewModel.CanCapture, "Explicit fake Agent, Ready identity, approved profiles, and confirmations must enable the software-only HardwareDual path.");
        hardwareViewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => hardwareViewModel.TransactionStartCount == 1 && !hardwareViewModel.IsBusy,
            "HardwareDual WPF fake capture did not finish.");
        Check.Equal(OperatorUiState.Review, hardwareViewModel.UiState);
        Check.Equal(1, hardwareOperations.StartCalls);
        Check.True(hardwareViewModel.RetainedOriginals.Contains("CAM-A: original.jpg", StringComparison.Ordinal), "HardwareDual WPF must retain CAM-A original.");
        Check.True(hardwareViewModel.RetainedOriginals.Contains("CAM-B: original.jpg", StringComparison.Ordinal), "HardwareDual WPF must retain CAM-B original.");
        hardwareViewModel.ExportCommand.Execute(null);
        await WaitUntilAsync(
            () => !hardwareViewModel.IsBusy && hardwareViewModel.ExportResult.Contains("byte-identical", StringComparison.Ordinal),
            "HardwareDual WPF fixed-local export did not finish.");
        Check.True(File.Exists(hardwareViewModel.LastExportPath), "HardwareDual WPF must publish the explicit fixed-local export.");

        var recoveryIdentity = new MutableDualIdentitySource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady());
        var recoveryOperations = new WpfHardwareDualFakeOperations(hardwareAdapter, responseUnknownOnce: true);
        var recoveryProductRoot = Path.Combine(root, "hardware-recovery-products");
        var recoveryStore = new HardwareDualTransactionSnapshotStore(recoveryProductRoot);
        var recoveryFlow = new DualCameraProductFlow(
            recoveryProductRoot,
            new HardwareDualCaptureSource(recoveryOperations, recoveryStore: recoveryStore),
            hardwareAdapter,
            recoveryIdentity);
        var requestProviderCalls = 0;
        var recoveryViewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "hardware-recovery-journals")),
            recoveryFlow,
            () =>
            {
                requestProviderCalls++;
                return DualCameraCaptureRequest.CreateHardwareDual(
                    DualCameraRigProfile.ApprovedSynthetic(),
                    HardwareDualCaptureProfile.ApprovedSynthetic(),
                    new HardwareDualOperatorConfirmations(true, true, true, true, true),
                    Guid.NewGuid());
            });
        await recoveryViewModel.InitializeAsync(CancellationToken.None);
        recoveryViewModel.AcceptSafetyCommand.Execute(null);
        recoveryViewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => !recoveryViewModel.IsBusy && recoveryFlow.Current?.FailureCode == DualCameraFailureCode.AgentResponseUnknown,
            "HardwareDual WPF response-unknown state was not retained.");
        recoveryIdentity.Set(DualCameraIdentitySnapshot.HardwarePending());
        var restartedRecoveryFlow = new DualCameraProductFlow(
            recoveryProductRoot,
            new HardwareDualCaptureSource(
                recoveryOperations,
                recoveryStore: new HardwareDualTransactionSnapshotStore(recoveryProductRoot)),
            hardwareAdapter,
            recoveryIdentity);
        var restartedRecoveryViewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "hardware-restarted-recovery-journals")),
            restartedRecoveryFlow,
            () =>
            {
                requestProviderCalls++;
                throw new InvalidOperationException("Restart recovery must not request current capture inputs.");
            });
        await restartedRecoveryViewModel.InitializeAsync(CancellationToken.None);
        restartedRecoveryViewModel.AcceptSafetyCommand.Execute(null);
        Check.True(restartedRecoveryViewModel.CanCapture, "Saved HardwareDual transaction recovery must remain available after restart with current identity Pending.");
        restartedRecoveryViewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => !restartedRecoveryViewModel.IsBusy && restartedRecoveryFlow.Current?.FailureCode == DualCameraFailureCode.None,
            "HardwareDual WPF saved transaction recovery did not finish after restart.");
        Check.Equal(1, requestProviderCalls);
        Check.Equal(0, restartedRecoveryViewModel.TransactionStartCount);
        Check.Equal(1, recoveryOperations.ReserveCalls);
        Check.Equal(1, recoveryOperations.StartCalls);
        Check.Equal(2, recoveryOperations.QueryCalls);
        Check.Equal(OperatorUiState.Review, restartedRecoveryViewModel.UiState);
        var recoveryStatePath = Path.Combine(
            recoveryProductRoot,
            "recovery-state",
            "pending-transaction.json");
        File.WriteAllText(recoveryStatePath, "{\"schemaVersion\":\"unsupported\",\"dispatchMayHaveOccurred\":false,\"pendingRequest\":null}");
        Check.Throws<InvalidDataException>(() =>
            new HardwareDualTransactionSnapshotStore(recoveryProductRoot).LoadPending());
        File.WriteAllBytes(recoveryStatePath, new byte[256 * 1024 + 1]);
        Check.Throws<InvalidDataException>(() =>
            new HardwareDualTransactionSnapshotStore(recoveryProductRoot).LoadPending());

        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(transactionRoot),
            productFlow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.FixedLocalExportDirectory = exportRoot;
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.CanCapture, "The typed TestSynthetic dual flow must be ready.");

        viewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy,
            "The formal dual-camera capture did not finish.");
        Check.Equal(OperatorUiState.Review, viewModel.UiState);
        Check.True(viewModel.CaptureResult.Contains("canonical JPEG", StringComparison.Ordinal), "Both originals must be displayed as verified JPEGs.");
        Check.True(viewModel.RetainedOriginals.Contains("SHA-256", StringComparison.Ordinal), "The UI must display canonical original verification evidence.");
        Check.True(viewModel.StitchResult.Contains("実JPEG合成完了", StringComparison.Ordinal), "The formal shell must display a real stitched JPEG.");
        Check.True(viewModel.ProgressSteps.Where(step => step.Id is not ("liveview" or "review" or "export")).All(step => step.StatusText == "完了"), "Every capture, validation, and stitch stage must be complete.");
        Check.Equal("処理中", viewModel.ProgressSteps.Single(step => step.Id == "review").StatusText);
        Check.Equal("待機", viewModel.ProgressSteps.Single(step => step.Id == "export").StatusText);
        var firstJob = viewModel.LastStitchJobId;

        viewModel.RestitchCommand.Execute(null);
        await WaitUntilAsync(
            () => !viewModel.IsBusy && !string.Equals(firstJob, viewModel.LastStitchJobId, StringComparison.Ordinal),
            "Formal restitch did not publish a distinct job.");
        Check.True(viewModel.CanExport, "The reviewed restitch must be explicitly exportable.");

        viewModel.ExportCommand.Execute(null);
        await WaitUntilAsync(
            () => !viewModel.IsBusy && viewModel.ExportResult.Contains("byte-identical", StringComparison.Ordinal),
            "Formal fixed-local export did not finish.");
        Check.True(File.Exists(viewModel.LastExportPath), "The formal WPF export must publish a JPEG.");
        Check.True(File.ReadAllBytes(viewModel.LastExportPath) is [0xff, 0xd8, .., 0xff, 0xd9], "The WPF export must be an actual JPEG.");
        Check.Equal("完了", viewModel.ProgressSteps.Single(step => step.Id == "review").StatusText);
        Check.Equal("完了", viewModel.ProgressSteps.Single(step => step.Id == "export").StatusText);

        viewModel.PrepareNewCaptureCommand.Execute(null);
        await WaitUntilAsync(() => viewModel.CanCapture, "A new formal diagnostic capture was not prepared.");
        viewModel.SelectedDiagnosticScenario = "CAM-B撮影失敗";
        viewModel.DiagnosticCommand.Execute(null);
        await WaitUntilAsync(
            () => viewModel.TransactionStartCount == 2 && !viewModel.IsBusy,
            "The typed CAM-B failure diagnostic did not finish.");
        Check.Equal(OperatorUiState.FailedPartial, viewModel.UiState);
        Check.True(viewModel.RetainedOriginals.Contains("CAM-A: original.jpg", StringComparison.Ordinal), "CAM-A actual JPEG must remain after CAM-B failure.");
        Check.False(viewModel.RetainedOriginals.Contains("CAM-B: original.jpg", StringComparison.Ordinal), "CAM-B failure must not invent an original.");
        Check.True(viewModel.TechnicalDetail.Contains("automatic retry count: 0", StringComparison.Ordinal), "The formal diagnostic must show zero retries.");
        Check.Equal(0, Directory.EnumerateFiles(productRoot, "*.simulated", SearchOption.AllDirectories).Count());
    }
    finally
    {
        Environment.SetEnvironmentVariable("A0_M2_ADAPTER_PATH", previousAdapterPath);
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static async Task FormalDualCameraExportFailureProgressAsync()
{
    var bundledAdapterPath = Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M2Adapter.exe");
    Check.True(File.Exists(bundledAdapterPath), "The WPF test output must contain the bundled M2 adapter.");
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-FormalDualExportFailureTests",
        Guid.NewGuid().ToString("N"));
    var transactionRoot = Path.Combine(root, "legacy-journals");
    var productRoot = Path.Combine(root, "products");
    var exportRoot = Path.Combine(root, "operator-export");
    Directory.CreateDirectory(transactionRoot);
    Directory.CreateDirectory(exportRoot);
    try
    {
        var native = new M2OfflineStitcherProcessAdapter(bundledAdapterPath);
        var bridge = new BlockingFailedExportBridge(native);
        var flow = new DualCameraProductFlow(
            productRoot,
            bridge,
            bridge,
            new FixedDualCameraIdentitySnapshotSource(
                DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(transactionRoot),
            flow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.FixedLocalExportDirectory = exportRoot;
        viewModel.AcceptSafetyCommand.Execute(null);
        viewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy,
            "The export-failure fixture capture did not finish.");

        Check.Equal("処理中", viewModel.ProgressSteps.Single(step => step.Id == "review").StatusText);
        Check.Equal("待機", viewModel.ProgressSteps.Single(step => step.Id == "export").StatusText);
        viewModel.ExportCommand.Execute(null);
        await bridge.ExportStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Check.Equal("完了", viewModel.ProgressSteps.Single(step => step.Id == "review").StatusText);
        Check.Equal("処理中", viewModel.ProgressSteps.Single(step => step.Id == "export").StatusText);

        bridge.ReleaseExport.TrySetResult();
        await WaitUntilAsync(
            () => !viewModel.IsBusy && viewModel.ExportResult.Contains("export失敗", StringComparison.Ordinal),
            "The deterministic export failure did not reach WPF.");
        Check.Equal("失敗", viewModel.ProgressSteps.Single(step => step.Id == "export").StatusText);
        Check.Equal(OperatorUiState.Review, viewModel.UiState);
    }
    finally
    {
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static async Task SingleCameraRestartPreservesPlanAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-OperatorShellTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var first = new OperatorShellViewModel(new SimulationFoundationService(root));
        await first.InitializeAsync(CancellationToken.None);
        first.SelectedOperatingMode = "1台構成";
        first.SelectedCamera = "CAM-B";
        first.AcceptSafetyCommand.Execute(null);
        first.SelectedDiagnosticScenario = "Live View停止失敗";
        first.DiagnosticCommand.Execute(null);
        await WaitUntilAsync(
            () => first.TransactionStartCount == 1 && !first.IsBusy,
            "The single-camera durable failure did not finish.");
        Check.Equal(OperatorUiState.FailedPartial, first.UiState);
        var transactionId = first.LastTransactionId;

        var restarted = new OperatorShellViewModel(new SimulationFoundationService(root));
        await restarted.InitializeAsync(CancellationToken.None);
        Check.Equal(OperatorUiState.FailedPartial, restarted.UiState);
        Check.Equal(transactionId, restarted.LastTransactionId);
        Check.True(restarted.IsSingleCameraMode, "Restart must restore Single mode from the journal.");
        Check.Equal("CAM-B", restarted.SelectedCamera);
        Check.True(restarted.CaptureButtonText.Contains("CAM-B", StringComparison.Ordinal), "Restart must display the durable selected alias.");
        Check.True(restarted.CameraAStatus.Contains("構成対象外", StringComparison.Ordinal), "Restart must not reclassify CAM-A as required.");
        Check.False(restarted.CanCapture, "Restart must not automatically resume or replace the failed transaction.");

        restarted.AcceptSafetyCommand.Execute(null);
        Check.False(restarted.CanCapture, "Safety acknowledgment alone must not bypass explicit preparation.");
        restarted.PrepareNewCaptureCommand.Execute(null);
        await WaitUntilAsync(() => !restarted.IsBusy, "Preparing the restarted single-camera workflow did not finish.");
        Check.True(restarted.CanCapture, "Explicit preparation must preserve the recovered Single CAM-B plan.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task SingleCameraWorkflowAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-OperatorShellTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    string? exportPath = null;
    try
    {
        var viewModel = new OperatorShellViewModel(new SimulationFoundationService(root));
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.SelectedOperatingMode = "1台構成";
        viewModel.SelectedCamera = "CAM-B";

        Check.True(viewModel.IsSingleCameraMode, "The operator must explicitly select Single mode.");
        Check.True(viewModel.CaptureButtonText.Contains("CAM-B", StringComparison.Ordinal), "The capture action must name the selected body.");
        Check.True(viewModel.CameraAStatus.Contains("構成対象外", StringComparison.Ordinal), "Inactive CAM-A must be shown as outside the plan.");
        Check.False(viewModel.CanCapture, "Safety acknowledgment remains mandatory in Single mode.");

        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.CanCapture, "A ready Single CAM-B plan must allow capture.");
        viewModel.CaptureCommand.Execute(null);
        viewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy,
            "The single-camera workflow did not finish.");

        Check.Equal(OperatorUiState.Review, viewModel.UiState);
        Check.True(viewModel.RetainedOriginals.Contains("CAM-B", StringComparison.Ordinal), "CAM-B original must be retained.");
        Check.False(viewModel.RetainedOriginals.Contains("CAM-A", StringComparison.Ordinal), "Single CAM-B must not invent CAM-A.");
        Check.True(viewModel.StitchResult.Contains("対象外", StringComparison.Ordinal), "Stitch must be NotApplicable in Single mode.");
        Check.True(viewModel.CanExport, "A reviewed single original must be exportable.");
        Check.False(viewModel.CanRestitch, "Single mode must not enable restitch.");

        viewModel.ExportCommand.Execute(null);
        exportPath = viewModel.LastExportPath;
        Check.True(File.Exists(exportPath), "The explicit simulated single export must exist.");
        Check.True(viewModel.ExportResult.Contains("単体原画像", StringComparison.Ordinal), "The export must be labeled as a single original.");
    }
    finally
    {
        if (!string.IsNullOrWhiteSpace(exportPath) && File.Exists(exportPath))
        {
            File.Delete(exportPath);
        }
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task ModeAndAliasLockDuringCaptureAsync()
{
    var service = new BlockingTransactionService();
    var viewModel = new OperatorShellViewModel(service);
    await viewModel.InitializeAsync(CancellationToken.None);
    viewModel.SelectedOperatingMode = "1台構成";
    viewModel.SelectedCamera = "CAM-B";
    viewModel.AcceptSafetyCommand.Execute(null);

    viewModel.CaptureCommand.Execute(null);
    await service.Started.WaitAsync(TimeSpan.FromSeconds(5));
    Check.True(viewModel.IsBusy, "The capture must hold the UI operation lock.");
    Check.False(viewModel.CanChangeOperatingMode, "Operating mode must be locked during capture.");
    Check.False(viewModel.CanSelectCamera, "The selected alias must be locked during capture.");

    viewModel.SelectedOperatingMode = "2台構成";
    viewModel.SelectedCamera = "CAM-A";
    Check.Equal("1台構成", viewModel.SelectedOperatingMode);
    Check.Equal("CAM-B", viewModel.SelectedCamera);

    service.Release();
    await WaitUntilAsync(() => !viewModel.IsBusy, "The blocking capture did not finish.");
    Check.Equal(OperatorUiState.Review, viewModel.UiState);
    Check.True(viewModel.RetainedOriginals.Contains("CAM-B", StringComparison.Ordinal), "The snapshotted plan must remain CAM-B.");
}

static void VerifyHardwareCameraAgentStderrSanitizer()
{
    const string longIdentifier = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    var sanitized = HardwareCameraAgentDiagnostic.SanitizeStandardError(
        "ordinary readiness failure sdk_load_failed " +
        $"requestId={longIdentifier} identifier={longIdentifier} cameraId={longIdentifier} " +
        $"bare {longIdentifier} repeated {longIdentifier}");

    Check.True(
        sanitized.Contains("ordinary readiness failure sdk_load_failed", StringComparison.Ordinal),
        "Ordinary diagnostic prose and a safe error code must remain readable.");
    Check.False(
        sanitized.Contains(longIdentifier, StringComparison.Ordinal),
        "A long general alphanumeric identifier must be redacted regardless of key or position.");
    Check.True(
        sanitized.Contains("requestId=[redacted]", StringComparison.Ordinal) &&
        sanitized.Contains("identifier=[redacted]", StringComparison.Ordinal) &&
        sanitized.Contains("cameraId=[redacted-identifier]", StringComparison.Ordinal),
        "Known and unknown identifier keys must both redact their values.");
    Check.True(
        sanitized.Split("[redacted-identifier]", StringSplitOptions.None).Length - 1 == 3,
        "Every unknown-key and bare occurrence must be redacted.");

    const string belowBoundary = "ABCDEFGHIJKLMNOPQRSTUV1";
    const string atBoundary = "ABCDEFGHIJKLMNOPQRSTUVW1";
    Check.True(belowBoundary.Length == 23 && atBoundary.Length == 24, "The identifier boundary fixture must remain exact.");
    var boundary = HardwareCameraAgentDiagnostic.SanitizeStandardError(
        $"short token {belowBoundary} boundary token {atBoundary} safe code E_CAMERA_17");
    Check.True(
        boundary.Contains(belowBoundary, StringComparison.Ordinal),
        "A 23-character alphanumeric token must remain available to diagnostics.");
    Check.False(
        boundary.Contains(atBoundary, StringComparison.Ordinal),
        "A 24-character alphanumeric identifier must be redacted.");
    Check.True(
        boundary.Contains("safe code E_CAMERA_17", StringComparison.Ordinal),
        "A short safe error code must not be damaged.");

    const string hexadecimalIdentifier = "0123456789abcdef0123456789abcdef";
    const string uuidIdentifier = "123e4567-e89b-12d3-a456-426614174000";
    var existingProtections = HardwareCameraAgentDiagnostic.SanitizeStandardError(
        "diagnostic secret=super-secret path=C:\\private\\sdk rawIdentity=RAW-CAMERA-IDENTITY " +
        $"hex={hexadecimalIdentifier} uuid={uuidIdentifier} " + new string('x', 600));
    Check.False(
        existingProtections.Contains("super-secret", StringComparison.Ordinal) ||
        existingProtections.Contains("C:\\private", StringComparison.Ordinal) ||
        existingProtections.Contains("RAW-CAMERA-IDENTITY", StringComparison.Ordinal) ||
        existingProtections.Contains(hexadecimalIdentifier, StringComparison.Ordinal) ||
        existingProtections.Contains(uuidIdentifier, StringComparison.Ordinal),
        "Existing secret, path, raw identity, hex, and UUID protections must remain active.");
    Check.True(existingProtections.Length <= 512, "Sanitized stderr must remain bounded to 512 characters.");
}

static async Task LiveViewStopFailureWorkflowAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-OperatorShellTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var viewModel = new OperatorShellViewModel(new SimulationFoundationService(root));
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.CanCapture, "The simulated shell must be ready before the diagnostic starts.");

        viewModel.SelectedDiagnosticScenario = "Live View停止失敗";
        viewModel.DiagnosticCommand.Execute(null);
        viewModel.DiagnosticCommand.Execute(null);
        await WaitUntilAsync(
            () => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy,
            "The durable failure workflow did not finish.");

        Check.Equal(1, viewModel.TransactionStartCount);
        Check.Equal(OperatorUiState.FailedPartial, viewModel.UiState);
        Check.True(viewModel.TechnicalDetail.Contains("LiveViewStopFailed", StringComparison.Ordinal), "The terminal reason must be visible.");
        Check.True(viewModel.TechnicalDetail.Contains("capture calls: 0", StringComparison.Ordinal), "Zero capture calls must be visible.");
        Check.True(viewModel.TechnicalDetail.Contains("automatic retry count: 0", StringComparison.Ordinal), "Zero retries must be visible.");
        Check.False(viewModel.CanCapture, "A failed transaction must block another capture until preparation.");
        Check.True(viewModel.CanPrepareNewCapture, "The operator must be able to explicitly prepare a new transaction.");
        Check.Equal(1, Directory.EnumerateDirectories(root).Count());

        var transactionId = Guid.ParseExact(viewModel.LastTransactionId, "N");
        viewModel.DiagnosticCommand.Execute(null);
        await Task.Delay(100);
        Check.Equal(1, viewModel.TransactionStartCount);

        var restarted = new OperatorShellViewModel(new SimulationFoundationService(root));
        await restarted.InitializeAsync(CancellationToken.None);
        Check.Equal(OperatorUiState.FailedPartial, restarted.UiState);
        Check.Equal(transactionId.ToString("N"), restarted.LastTransactionId);
        Check.True(restarted.TechnicalDetail.Contains("LiveViewStopFailed", StringComparison.Ordinal), "Restart must rediscover the same terminal reason.");
        Check.False(restarted.CanCapture, "Restart must not resume or recapture the failed transaction.");

        restarted.AcceptSafetyCommand.Execute(null);
        Check.False(restarted.CanCapture, "Safety acknowledgment must not bypass explicit new-capture preparation.");
        restarted.PrepareNewCaptureCommand.Execute(null);
        await WaitUntilAsync(() => !restarted.IsBusy, "Preparing a new transaction did not finish.");
        Check.True(restarted.CanCapture, "Only explicit preparation may allow a new transaction.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task WaitUntilAsync(Func<bool> predicate, string message)
{
    var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
    while (!predicate())
    {
        if (DateTime.UtcNow >= deadline)
        {
            throw new TimeoutException(message);
        }
        await Task.Delay(20);
    }
}

static string CreateHardwareTestRoot()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-HardwareOperatorTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    return root;
}

static HardwareRetainedOriginalRecord WriteJpegRecord(
    string path,
    string alias,
    bool preserveOnePixelDimensions = false)
{
    var pixels = new byte[] { 0x20, 0x80, 0xE0 };
    var bitmap = BitmapSource.Create(
        1, 1, 96, 96, PixelFormats.Bgr24, null, pixels, stride: 3);
    var encoder = new JpegBitmapEncoder();
    encoder.Frames.Add(BitmapFrame.Create(bitmap));
    using var encoded = new MemoryStream();
    encoder.Save(encoded);
    var bytes = encoded.ToArray();
    if (!preserveOnePixelDimensions)
    {
        RewriteJpegDimensions(bytes, width: 7360, height: 4912);
    }
    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    File.WriteAllBytes(path, bytes);
    return new HardwareRetainedOriginalRecord
    {
        CameraAlias = alias,
        Path = path,
        SizeBytes = bytes.Length,
        Sha256 = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
    };
}

static HardwarePreviewJpegRecord WritePreviewRecord(string path)
{
    var original = WriteJpegRecord(path, "CAM-A", preserveOnePixelDimensions: true);
    return new HardwarePreviewJpegRecord
    {
        Path = original.Path,
        SizeBytes = original.SizeBytes,
        Sha256 = original.Sha256,
    };
}

static void RewriteJpegDimensions(byte[] bytes, int width, int height)
{
    for (var index = 2; index + 8 < bytes.Length;)
    {
        if (bytes[index++] != 0xFF)
        {
            throw new InvalidDataException("Test JPEG marker structure is invalid.");
        }
        while (index < bytes.Length && bytes[index] == 0xFF) index++;
        var marker = bytes[index++];
        if (marker is 0xD9 or 0xDA) break;
        if (marker == 0x01 || marker is >= 0xD0 and <= 0xD7) continue;
        var segmentLength = (bytes[index] << 8) | bytes[index + 1];
        var isStartOfFrame = marker is >= 0xC0 and <= 0xCF and not (0xC4 or 0xC8 or 0xCC);
        if (isStartOfFrame)
        {
            bytes[index + 3] = (byte)(height >> 8);
            bytes[index + 4] = (byte)height;
            bytes[index + 5] = (byte)(width >> 8);
            bytes[index + 6] = (byte)width;
            return;
        }
        index += segmentLength;
    }
    throw new InvalidDataException("Test JPEG has no start-of-frame marker.");
}

static HardwareSingleCaptureResult CompleteCapture(
    string transactionId,
    string alias,
    HardwareRetainedOriginalRecord original) =>
    new()
    {
        CameraMode = "SingleCamera",
        CameraAlias = alias,
        RequiredCameraAlias = alias,
        RunId = "run-1000-1",
        TransactionId = transactionId,
        CaptureProfileId = "single-profile",
        CaptureProfileVersion = 1,
        CaptureProfileSha256 = new string('a', 64),
        CaptureProfileCameraAlias = alias,
        CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
        TerminalState = "Complete",
        ErrorCategory = string.Empty,
        ErrorDetail = string.Empty,
        RetainedOriginal = original with { CameraAlias = alias },
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

static HardwareSingleCaptureResult InProgressCapture(string transactionId, string alias) =>
    new()
    {
        CameraMode = "SingleCamera",
        CameraAlias = alias,
        RequiredCameraAlias = alias,
        RunId = "run-2000-1",
        TransactionId = transactionId,
        CaptureProfileId = "single-profile",
        CaptureProfileVersion = 1,
        CaptureProfileSha256 = new string('a', 64),
        CaptureProfileCameraAlias = alias,
        CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
        TerminalState = "InProgress",
        ErrorCategory = "transaction_in_progress",
        ErrorDetail = "The same transaction remains active.",
        RetainedOriginal = null,
        LiveViewHandoffRequested = false,
        LiveViewStoppedBeforeCapture = false,
        LiveViewSdkSessionClosedBeforeCapture = false,
        PostCaptureLiveViewProbeAttempted = false,
        PostCaptureLiveViewProbeSucceeded = false,
        PostCapturePreview = null,
        SpoolEmptyBeforeCapture = false,
        CameraObjectDeleteAttempted = false,
        CameraObjectDeleteSucceeded = false,
        SpoolEmptyAfterCleanup = false,
        AutomaticRetryCount = 0,
        TransactionWatchdogSeconds = 180,
        RealIdentifiersIncluded = false,
    };

static HardwareSingleCaptureResult CompleteCaptureWithHandoff(
    string transactionId,
    string alias,
    HardwareRetainedOriginalRecord original,
    HardwarePreviewJpegRecord resumedPreview) =>
    CompleteCapture(transactionId, alias, original) with
    {
        LiveViewHandoffRequested = true,
        LiveViewStoppedBeforeCapture = true,
        LiveViewSdkSessionClosedBeforeCapture = true,
        PostCaptureLiveViewProbeAttempted = true,
        PostCaptureLiveViewProbeSucceeded = true,
        PostCapturePreview = resumedPreview,
    };

static HardwareSingleCaptureResult FailedPartialCapture(
    string transactionId,
    string alias,
    HardwareRetainedOriginalRecord original) =>
    CompleteCapture(transactionId, alias, original) with
    {
        TerminalState = "FailedPartial",
        ErrorCategory = "spool_empty_after_failed",
        ErrorDetail = "The canonical PC original is retained but spool cleanup did not complete.",
        SpoolEmptyAfterCleanup = false,
    };

static HardwareSingleCaptureResult ReservedCapture(string transactionId, string alias) =>
    InProgressCapture(transactionId, alias) with
    {
        TerminalState = "Reserved",
        ErrorCategory = "transaction_reserved",
        ErrorDetail = "The transaction reservation exists before camera access.",
    };

static HardwareSingleCaptureResult TransactionNotFoundCapture(string transactionId, string alias) =>
    InProgressCapture(transactionId, alias) with
    {
        TerminalState = "NotFound",
        ErrorCategory = "TransactionNotFound",
        ErrorDetail = "No durable Camera Agent journal exists for the attempted dispatch.",
    };

static HardwarePendingTransaction HardwarePending(
    string transactionId,
    string alias,
    bool captureRequestDispatchAttempted,
    bool liveViewHandoffRequested = false) =>
    new()
    {
        OperatingMode = "SingleCamera",
        TransactionId = transactionId,
        CameraAlias = alias,
        RequiredCameraAlias = alias,
        CaptureProfileId = "single-profile",
        CaptureProfileVersion = 1,
        CaptureProfileSha256 = new string('a', 64),
        CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
        LiveViewHandoffRequested = liveViewHandoffRequested,
        CaptureRequestDispatchAttempted = captureRequestDispatchAttempted,
        StartedAtUtc = DateTimeOffset.UtcNow,
    };

// ---------------------------------------------------------------------------
// HardwareDual .NET Agent lifecycle tests (Issue #8).
//
// These tests exercise DualCameraAgentLifecycle against a real, separate OS
// process (this same test apphost, re-invoked with a scenario environment
// variable) speaking the actual named-pipe wire protocol -- not an in-memory
// fake IDualHardwareCaptureOperations. That is the only way to exercise
// process/pipe failure and restart recovery for real.
// ---------------------------------------------------------------------------

static DualCameraCaptureRequest HardwareDualTestRequest(Guid transactionId) =>
    DualCameraCaptureRequest.CreateHardwareDual(
        DualCameraRigProfile.ApprovedSynthetic(),
        HardwareDualCaptureProfile.ApprovedSynthetic(),
        new HardwareDualOperatorConfirmations(true, true, true, true, true),
        transactionId);

static string DualCameraAgentTestHostPath()
{
    var path = Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M3.OperatorShellTests.exe");
    Check.True(File.Exists(path), "The fake Dual Camera Agent test host apphost must exist.");
    return path;
}

static string DualCameraM2AdapterPath() =>
    Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M2Adapter.exe");

// Native (and the fake host below, matched to the same strength) requires
// --approved-capture-profile / --dual-identity-proof to already be existing
// regular files. This writes test-only placeholder content -- DualCameraAgentLifecycle
// and the fake host only ever check that the paths are existing regular files; neither
// parses the contents in this harness. Never treat this as a stand-in for a real
// approved capture profile or identity proof.
static void WriteDualAgentTestArtifactFiles(string approvedCaptureProfilePath, string dualIdentityProofPath)
{
    Directory.CreateDirectory(Path.GetDirectoryName(approvedCaptureProfilePath)!);
    File.WriteAllText(
        approvedCaptureProfilePath,
        "{\"note\":\"test-only placeholder, not a real approved capture profile\"}");
    Directory.CreateDirectory(Path.GetDirectoryName(dualIdentityProofPath)!);
    File.WriteAllText(
        dualIdentityProofPath,
        "{\"note\":\"test-only placeholder, not a real dual identity proof\"}");
}

static async Task DualCameraAgentLifecycleIdentityPendingKeepsZeroProcessAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        // A configured-but-nonexistent Agent executable: the lifecycle must construct
        // cleanly and never probe or launch it while DualCameraProductFlow's identity
        // gate keeps every capture rejected before any side effect.
        var agentPath = Path.Combine(root, "A0CameraStitcher.DualCameraAgent.exe");
        var journalRoot = Path.Combine(root, "agent-pair-journal");
        await using var lifecycle = new DualCameraAgentLifecycle(
            agentPath,
            journalRoot,
            Path.Combine(root, "camera-agent", "approved-dual-capture-profile.json"),
            Path.Combine(root, "phase0", "dual-identity-proof.json"));
        Check.False(lifecycle.AgentExecutableAvailable, "A nonexistent Agent path must report unavailable, not throw.");

        var flow = new DualCameraProductFlow(
            Path.Combine(root, "products"),
            new HardwareDualCaptureSource(lifecycle),
            new M2OfflineStitcherProcessAdapter(DualCameraM2AdapterPath()),
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.HardwarePending()));
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "journals")),
            flow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);

        Check.Equal(DualCameraIdentityStatus.HardwarePending, flow.IdentitySnapshot.Status);
        Check.False(
            viewModel.CanCapture,
            "HardwareDual must stay CanCapture=false while identity is Pending, even with a real Agent lifecycle wired in.");
        Check.Equal(OperatorShellViewModel.HardwareDualPendingBanner, viewModel.BannerText);

        var rejected = false;
        try
        {
            await flow.CaptureAndStitchAsync(HardwareDualTestRequest(Guid.NewGuid()));
        }
        catch (DualCameraFlowException exception) when (exception.Code == DualCameraFailureCode.IdentityNotReady)
        {
            rejected = true;
        }
        Check.True(rejected, "A Pending identity must reject capture with IdentityNotReady before any Agent call.");
        Check.False(
            Directory.Exists(journalRoot),
            "Zero process/camera while Pending: the Agent lifecycle must not even create its pair journal directory.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task DualCameraAgentLifecycleRequiresExistingArtifactFilesAsync()
{
    // Native requires --approved-capture-profile / --dual-identity-proof to already
    // be existing regular files and exits 1 if either is missing. This class never
    // fabricates those approval/proof artifacts, so a missing file must fail closed
    // before any process launch (zero process, zero pipe) -- not silently create a
    // placeholder and hand it to a real Native agent as if it were approved.
    foreach (var missing in new[] { "capture-profile", "identity-proof" })
    {
        var root = CreateHardwareTestRoot();
        try
        {
            var captureProfilePath = Path.Combine(root, "camera-agent", "approved-dual-capture-profile.json");
            var identityProofPath = Path.Combine(root, "phase0", "dual-identity-proof.json");
            Directory.CreateDirectory(Path.GetDirectoryName(captureProfilePath)!);
            Directory.CreateDirectory(Path.GetDirectoryName(identityProofPath)!);
            if (missing != "capture-profile")
            {
                File.WriteAllText(captureProfilePath, "{\"note\":\"test-only placeholder\"}");
            }
            if (missing != "identity-proof")
            {
                File.WriteAllText(identityProofPath, "{\"note\":\"test-only placeholder\"}");
            }

            var journalRoot = Path.Combine(root, "agent-pair-journal");
            await using var lifecycle = new DualCameraAgentLifecycle(
                DualCameraAgentTestHostPath(),
                journalRoot,
                captureProfilePath,
                identityProofPath);

            HardwareCameraAgentLaunchException? caught = null;
            try
            {
                await lifecycle.ReservePairTransactionAsync(Guid.NewGuid(), CancellationToken.None);
            }
            catch (HardwareCameraAgentLaunchException exception)
            {
                caught = exception;
            }

            Check.True(
                caught is not null,
                $"missing {missing}: reserve must fail closed with a typed exception before any process launch.");
            Check.False(
                caught!.RequestMayHaveBeenDispatched,
                $"missing {missing}: nothing was ever dispatched -- the process never even started.");
            var expectedMissingPath = missing == "capture-profile" ? captureProfilePath : identityProofPath;
            Check.True(
                caught.Message.Contains(expectedMissingPath, StringComparison.Ordinal),
                $"missing {missing}: the exception must name the missing artifact path, was: {caught.Message}");
            Check.False(
                Directory.Exists(journalRoot),
                $"missing {missing}: zero process/pipe means the pair journal directory must not be created either.");
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task DualCameraAgentLifecycleFakeHostHappyPathAsync()
{
    var root = CreateHardwareTestRoot();
    var tracePath = Path.Combine(root, "dual-agent-trace.jsonl");
    var previousScenario = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO");
    var previousTrace = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE");
    try
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", "happy");
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", tracePath);

        await using var lifecycle = CreateDualAgentTestLifecycle(root);
        var stitcher = new M2OfflineStitcherProcessAdapter(DualCameraM2AdapterPath());
        var flow = new DualCameraProductFlow(
            Path.Combine(root, "products"),
            new HardwareDualCaptureSource(lifecycle),
            stitcher,
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));

        var transactionId = Guid.NewGuid();
        var state = await flow.CaptureAndStitchAsync(HardwareDualTestRequest(transactionId));

        Check.Equal(DualCameraFailureCode.None, state.FailureCode);
        Check.Equal(2, state.Capture!.Originals.Count);
        Check.True(
            state.Stitch is { Succeeded: true },
            "The Ready synthetic seam's reserve->start Completed path must reach a real stitched JPEG.");

        var entries = ReadDualAgentTraceEntries(tracePath);
        Check.Equal(1, entries.Count(entry => entry.Operation == "reserve-pair-transaction"));
        Check.Equal(1, entries.Count(entry => entry.Operation == "start-reserved-pair"));
        Check.Equal(0, entries.Count(entry => entry.Operation == "get-pair-transaction-result"));
        Check.True(
            entries.Select(entry => entry.PipeName).Distinct().Count() == 1,
            "A single-process happy path must stay on exactly one pipe name.");
        Check.True(
            entries.All(entry => entry.TransactionId is null || entry.TransactionId == transactionId.ToString("N")),
            "Every observed operation must reference the one dispatched transaction id.");
    }
    finally
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", previousScenario);
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", previousTrace);
        Directory.Delete(root, recursive: true);
    }
}

static async Task DualCameraAgentLifecycleExitCodeClassificationAsync()
{
    // Directly exercises DualCameraAgentLifecycle's own exit-code -> "may have been
    // dispatched" classification (HardwareDualCaptureSource/DualCameraProductFlow
    // never inspect it, so no flow-level test can catch a regression here). If this
    // mapping ever inverts, a genuinely-dispatched pair could be misclassified as
    // safe to retry -- the exact failure the absolute invariants forbid. Exit codes
    // 1/2 are driven through the same "read the request, then exit" mechanics as 0/3
    // to isolate the classifier's mapping table itself; a real Native agent would
    // more plausibly fail before ever accepting the connection for exit 1, but that
    // path never reaches this classifier at all (it surfaces as a distinct connect
    // failure), so it is out of scope for this test.
    foreach (var (exitCode, expectedDispatched) in new (int ExitCode, bool ExpectedDispatched)[]
             {
                 (0, true),  // complete, including a natural max-lifetime exit -> ambiguous
                 (1, false), // argument/launch failure -> never reached the pipe
                 (2, false), // failed_before_dispatch -> explicitly known not dispatched
                 (3, true),  // dispatched_delivery_failed -> ambiguous
             })
    {
        var root = CreateHardwareTestRoot();
        var tracePath = Path.Combine(root, "dual-agent-trace.jsonl");
        var previousScenario = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO");
        var previousExitCode = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE");
        var previousTrace = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE");
        try
        {
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", "die-after-start");
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", exitCode.ToString());
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", tracePath);

            await using var lifecycle = CreateDualAgentTestLifecycle(root);

            var transactionId = Guid.NewGuid();
            Check.True(
                await lifecycle.ReservePairTransactionAsync(transactionId, CancellationToken.None),
                $"exit {exitCode}: reserve must be accepted before the classified failure.");

            HardwareCameraAgentLaunchException? caught = null;
            try
            {
                await lifecycle.StartReservedPairAsync(
                    BuildDualAgentTestCaptureRequest(root, transactionId),
                    CancellationToken.None);
            }
            catch (HardwareCameraAgentLaunchException exception)
            {
                caught = exception;
            }

            Check.True(
                caught is not null,
                $"exit {exitCode}: an incomplete pipe response must classify as a typed launch exception.");
            Check.True(
                caught!.ProcessExitCode == exitCode,
                $"exit {exitCode}: ProcessExitCode must be observed as {exitCode}, was {caught.ProcessExitCode?.ToString() ?? "null"}.");
            Check.Equal(expectedDispatched, caught.RequestMayHaveBeenDispatched);
        }
        finally
        {
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", previousScenario);
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", previousExitCode);
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", previousTrace);
            Directory.Delete(root, recursive: true);
        }
    }
}

static DualCameraAgentLifecycle CreateDualAgentTestLifecycle(string root)
{
    var captureProfilePath = Path.Combine(root, "camera-agent", "approved-dual-capture-profile.json");
    var identityProofPath = Path.Combine(root, "phase0", "dual-identity-proof.json");
    WriteDualAgentTestArtifactFiles(captureProfilePath, identityProofPath);
    return new DualCameraAgentLifecycle(
        DualCameraAgentTestHostPath(),
        Path.Combine(root, "agent-pair-journal"),
        captureProfilePath,
        identityProofPath);
}

static DualHardwareCaptureRequest BuildDualAgentTestCaptureRequest(string root, Guid transactionId)
{
    var startedAtUtc = DateTimeOffset.UtcNow;
    return new DualHardwareCaptureRequest(
        transactionId,
        Path.Combine(root, "products", "transactions", transactionId.ToString("N")),
        DualCameraIdentitySnapshot.AnonymousTestSyntheticReady(),
        HardwareDualCaptureProfile.ApprovedSynthetic(),
        DualCameraRigProfile.ApprovedSynthetic(),
        new HardwareDualOperatorConfirmations(true, true, true, true, true),
        startedAtUtc,
        startedAtUtc.AddSeconds(180));
}

static async Task DualCameraAgentLifecycleRestartRecoveryAsync()
{
    foreach (var (scenarioLabel, exitCode) in new (string, int)[]
             {
                 ("crash after dispatch (exit 3, dispatched_delivery_failed)", 3),
                 ("native max-lifetime exit (exit 0, complete)", 0),
             })
    {
        var root = CreateHardwareTestRoot();
        var tracePath = Path.Combine(root, "dual-agent-trace.jsonl");
        var previousScenario = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO");
        var previousExitCode = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE");
        var previousTrace = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE");
        try
        {
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", "die-after-start");
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", exitCode.ToString());
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", tracePath);

            await using var lifecycle = CreateDualAgentTestLifecycle(root);
            var stitcher = new M2OfflineStitcherProcessAdapter(DualCameraM2AdapterPath());
            var flow = new DualCameraProductFlow(
                Path.Combine(root, "products"),
                new HardwareDualCaptureSource(lifecycle),
                stitcher,
                new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));

            var transactionId = Guid.NewGuid();
            var unknown = await flow.CaptureAndStitchAsync(HardwareDualTestRequest(transactionId));
            Check.Equal(DualCameraFailureCode.AgentResponseUnknown, unknown.FailureCode);

            // AgentResponseUnknown is the catch-all failure code for the whole
            // HardwareDual path -- a broken fake host (wrong scenario wiring, a crash
            // before start-reserved-pair, etc.) would produce the exact same code as
            // a correctly-behaving one. Assert what actually happened at the
            // process/pipe level immediately, before any later ambiguous-outcome
            // assert can hide which stage really broke.
            var afterDispatchEntries = ReadDualAgentTraceEntries(tracePath);
            Check.Equal(1, afterDispatchEntries.Count(entry => entry.Operation == "reserve-pair-transaction"));
            Check.Equal(1, afterDispatchEntries.Count(entry => entry.Operation == "start-reserved-pair"));
            Check.Equal(1, afterDispatchEntries.Select(entry => entry.PipeName).Distinct().Count());

            // Process exit/pipe failure keeps support-required: a plain new capture
            // attempt must stay blocked on the same frozen transaction, with zero new
            // reserve/redispatch/retry -- verified directly against the Agent, not
            // just against the returned failure code.
            var blocked = await flow.CaptureAndStitchAsync(HardwareDualTestRequest(Guid.NewGuid()));
            Check.Equal(DualCameraFailureCode.AgentResponseUnknown, blocked.FailureCode);
            Check.Equal(transactionId, blocked.TransactionId);
            Check.Equal(afterDispatchEntries.Count, ReadDualAgentTraceEntries(tracePath).Count);

            var recovered = await flow.RecoverAndStitchAsync(transactionId);

            var afterRecoveryEntries = ReadDualAgentTraceEntries(tracePath);
            Check.Equal(1, afterRecoveryEntries.Count(entry => entry.Operation == "reserve-pair-transaction"));
            Check.Equal(1, afterRecoveryEntries.Count(entry => entry.Operation == "start-reserved-pair"));
            Check.Equal(1, afterRecoveryEntries.Count(entry => entry.Operation == "get-pair-transaction-result"));
            Check.True(
                afterRecoveryEntries.Where(entry => entry.TransactionId is not null)
                    .All(entry => entry.TransactionId == transactionId.ToString("N")),
                $"{scenarioLabel}: every observed operation must reference the same transaction id (zero different-ID query).");
            Check.Equal(2, afterRecoveryEntries.Select(entry => entry.PipeName).Distinct().Count());

            Check.Equal(DualCameraFailureCode.None, recovered.FailureCode);
            Check.Equal(2, recovered.Capture!.Originals.Count);
            Check.True(
                recovered.Stitch is { Succeeded: true },
                $"{scenarioLabel}: recovery must reach a real stitched JPEG from the frozen snapshot.");
        }
        finally
        {
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", previousScenario);
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", previousExitCode);
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", previousTrace);
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task DualCameraAgentLifecyclePipeFailureWithoutProcessExitAsync()
{
    var root = CreateHardwareTestRoot();
    var tracePath = Path.Combine(root, "dual-agent-trace.jsonl");
    var previousScenario = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO");
    var previousTrace = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE");
    try
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", "pipe-only-after-start");
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", tracePath);

        await using var lifecycle = CreateDualAgentTestLifecycle(root);
        var stitcher = new M2OfflineStitcherProcessAdapter(DualCameraM2AdapterPath());
        var flow = new DualCameraProductFlow(
            Path.Combine(root, "products"),
            new HardwareDualCaptureSource(lifecycle),
            stitcher,
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));

        var transactionId = Guid.NewGuid();
        var unknown = await flow.CaptureAndStitchAsync(HardwareDualTestRequest(transactionId));
        Check.Equal(DualCameraFailureCode.AgentResponseUnknown, unknown.FailureCode);

        // Assert what actually happened before trusting the later ambiguous-outcome
        // asserts: AgentResponseUnknown alone cannot distinguish a working scenario
        // from a broken one.
        var afterDispatchEntries = ReadDualAgentTraceEntries(tracePath);
        Check.Equal(1, afterDispatchEntries.Count(entry => entry.Operation == "reserve-pair-transaction"));
        Check.Equal(1, afterDispatchEntries.Count(entry => entry.Operation == "start-reserved-pair"));
        Check.Equal(1, afterDispatchEntries.Select(entry => entry.PipeName).Distinct().Count());

        var recovered = await flow.RecoverAndStitchAsync(transactionId);

        var afterRecoveryEntries = ReadDualAgentTraceEntries(tracePath);
        Check.Equal(
            1,
            afterRecoveryEntries.Select(entry => entry.PipeName).Distinct().Count()); // process stayed alive: no restart needed
        Check.Equal(1, afterRecoveryEntries.Count(entry => entry.Operation == "reserve-pair-transaction"));
        Check.Equal(1, afterRecoveryEntries.Count(entry => entry.Operation == "start-reserved-pair"));
        Check.Equal(1, afterRecoveryEntries.Count(entry => entry.Operation == "get-pair-transaction-result"));

        Check.Equal(DualCameraFailureCode.None, recovered.FailureCode);
        Check.Equal(2, recovered.Capture!.Originals.Count);
    }
    finally
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", previousScenario);
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", previousTrace);
        Directory.Delete(root, recursive: true);
    }
}

static async Task DualCameraAgentLifecycleConnectFailureSurfacesExitDiagnosticsAsync()
{
    // When the Agent process fails before ever creating its pipe (e.g. an
    // immediate startup failure), the client's connect attempt itself fails --
    // this is a different failure path than an incomplete response, and it used to
    // discard the process's exit code and stderr, leaving only a generic "connection
    // failed" after the full connect timeout.
    var root = CreateHardwareTestRoot();
    var previousScenario = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO");
    var previousExitCode = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE");
    try
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", "die-before-listen");
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", "1");

        await using var lifecycle = CreateDualAgentTestLifecycle(root);

        HardwareCameraAgentLaunchException? caught = null;
        try
        {
            await lifecycle.ReservePairTransactionAsync(Guid.NewGuid(), CancellationToken.None);
        }
        catch (HardwareCameraAgentLaunchException exception)
        {
            caught = exception;
        }

        Check.True(
            caught is not null,
            "A connect failure must classify as a typed launch exception, not a raw connect exception.");
        Check.False(
            caught!.RequestMayHaveBeenDispatched,
            "A connect failure means the request was definitely never dispatched.");
        Check.True(
            caught.ProcessExitCode == 1,
            $"The Native exit code must be surfaced instead of discarded, was {caught.ProcessExitCode?.ToString() ?? "null"}.");
        Check.True(
            caught.SanitizedStandardError.Length > 0,
            "The Native failure reason must reach the operator instead of only a generic connect timeout.");
        Check.False(
            caught.SanitizedStandardError.Contains("super-secret", StringComparison.Ordinal),
            "Native stderr must remain sanitized even when surfaced from a connect failure.");
    }
    finally
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", previousScenario);
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", previousExitCode);
        Directory.Delete(root, recursive: true);
    }
}

static List<(string PipeName, string Operation, string? TransactionId)> ReadDualAgentTraceEntries(string tracePath)
{
    var result = new List<(string, string, string?)>();
    if (!File.Exists(tracePath))
    {
        return result;
    }
    foreach (var line in File.ReadAllLines(tracePath))
    {
        if (string.IsNullOrWhiteSpace(line))
        {
            continue;
        }
        using var document = JsonDocument.Parse(line);
        var root = document.RootElement;
        result.Add((
            root.GetProperty("pipeName").GetString()!,
            root.GetProperty("operation").GetString()!,
            root.TryGetProperty("transactionId", out var idElement) && idElement.ValueKind == JsonValueKind.String
                ? idElement.GetString()
                : null));
    }
    return result;
}

// --- Fake Dual Camera Agent test host (child process) -----------------------
//
// Speaks the real a0.camera-agent.hardware-dual.v2 named-pipe wire protocol so
// DualCameraAgentLifecycle (production process/pipe lifecycle code) is exercised
// end-to-end, including a real process exit and a real second-generation process
// picking a fresh unique pipe name back up. Scenario and exit behavior are driven
// by env vars set by the parent test process before it starts the child.

static async Task<int> RunDualCameraAgentTestChildAsync(string scenario, IReadOnlyList<string> arguments)
{
    var pipeName = TryGetDualAgentArgument(arguments, "--pipe-name");
    var pairJournalRoot = TryGetDualAgentArgument(arguments, "--pair-journal-root");
    var approvedCaptureProfile = TryGetDualAgentArgument(arguments, "--approved-capture-profile");
    var dualIdentityProof = TryGetDualAgentArgument(arguments, "--dual-identity-proof");
    if (pipeName is null || pairJournalRoot is null || approvedCaptureProfile is null || dualIdentityProof is null)
    {
        return 1; // argument/launch failure
    }
    // Matched to the same strength as the real Native contract
    // (docs/HARDWARE_CAMERA_AGENT_DUAL_V2.md, Issue #22): both paths must already be
    // existing regular files, or exit 1. Previously these two values were extracted
    // and only null-checked, never actually verified to exist -- so a client-side
    // regression that stopped passing real files could never be caught here.
    if (!File.Exists(approvedCaptureProfile) || !File.Exists(dualIdentityProof))
    {
        return 1;
    }

    var tracePath = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE");
    var exitCodeText = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE");
    var dieExitCode = int.TryParse(exitCodeText, out var parsedExitCode) ? parsedExitCode : 3;

    if (scenario == "die-before-listen")
    {
        // Simulates Native failing fast (e.g. on startup) before ever creating the
        // pipe: the client's connect attempt fails outright rather than an
        // in-flight request going unanswered. Exercises DualCameraAgentLifecycle's
        // connect-failure diagnostics path (exit code + stderr), not the
        // incomplete-response path the other scenarios cover.
        await Console.Error.WriteLineAsync(
            "synthetic dual agent failed before listening secret=super-secret");
        return dieExitCode;
    }

    Directory.CreateDirectory(pairJournalRoot);

    await using var pipe = new NamedPipeServerStream(
        pipeName,
        PipeDirection.InOut,
        maxNumberOfServerInstances: 1,
        PipeTransmissionMode.Byte,
        PipeOptions.Asynchronous);
    // Generous idle window: this repo has hit timing-sensitive failures on slow CI
    // runners more than once (#17 / #20 / #5-S7), and this fake host is a real
    // subprocess talking over a real named pipe, not an in-memory fake.
    var idleTimeout = TimeSpan.FromSeconds(12);
    while (true)
    {
        using var acceptTimeout = new CancellationTokenSource(idleTimeout);
        try
        {
            await pipe.WaitForConnectionAsync(acceptTimeout.Token);
        }
        catch (OperationCanceledException)
        {
            return 0; // idle: no further requests within the window; complete (0).
        }

        string requestJson;
        try
        {
            using var requestTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            requestJson = await ReadPersistentTestFrameAsync(pipe, requestTimeout.Token);
        }
        catch (Exception) when (pipe.IsConnected)
        {
            pipe.Disconnect();
            continue;
        }

        using var request = JsonDocument.Parse(requestJson);
        var root = request.RootElement;
        if (root.GetProperty("schemaVersion").GetString() != DualHardwareCameraAgentProtocol.SchemaVersion ||
            root.GetProperty("marker").GetString() != DualHardwareCameraAgentProtocol.Marker ||
            root.GetProperty("simulation").GetBoolean())
        {
            // A real Native agent runs a strict parser and would reject a malformed
            // envelope outright instead of guessing at it; the fake host must fail
            // the same way rather than silently accepting whatever the client sent,
            // or a client-side envelope regression would only ever surface on real
            // hardware.
            return 66;
        }
        var operation = root.GetProperty("operation").GetString()!;
        var requestId = root.GetProperty("requestId").GetString()!;
        var payload = root.GetProperty("payload");

        await AppendDualAgentTraceAsync(tracePath, pipeName, operation, TryGetDualAgentTransactionId(payload));

        using var responseTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        switch (operation)
        {
            case DualHardwareCameraAgentProtocol.Operations.GetCapabilities:
                await WritePersistentTestFrameAsync(
                    pipe,
                    BuildDualAgentResponseEnvelopeJson(requestId, true, "DualCapabilities", BuildDualCapabilitiesPayload()),
                    responseTimeout.Token);
                break;

            case DualHardwareCameraAgentProtocol.Operations.ReservePairTransaction:
                await WritePersistentTestFrameAsync(
                    pipe,
                    BuildDualAgentResponseEnvelopeJson(
                        requestId,
                        true,
                        "PairTransactionReserved",
                        new { transactionId = payload.GetProperty("transactionId").GetString()!, accepted = true }),
                    responseTimeout.Token);
                break;

            case DualHardwareCameraAgentProtocol.Operations.StartReservedPair:
            {
                var transaction = payload.GetProperty("transaction");
                var transactionIdHex = transaction.GetProperty("transactionId").GetString()!;
                var transactionDirectory = transaction.GetProperty("transactionDirectory").GetString()!;
                var (camAPath, camBPath) = await WriteDualAgentOriginalsAsync(
                    transactionDirectory,
                    Guid.ParseExact(transactionIdHex, "N"));
                await SaveDualAgentJournalEntryAsync(pairJournalRoot, transactionIdHex, transaction, camAPath, camBPath);

                if (scenario == "happy")
                {
                    var resultPayload = BuildDualAgentCaptureResultPayload(transaction, transactionIdHex, camAPath, camBPath);
                    await WritePersistentTestFrameAsync(
                        pipe,
                        BuildDualAgentResponseEnvelopeJson(
                            requestId,
                            true,
                            "PairDispatchAccepted",
                            new { transactionId = transactionIdHex, dispatchState = "Completed", result = resultPayload }),
                        responseTimeout.Token);
                    break;
                }

                // "die-after-start" / "pipe-only-after-start": the journal entry above
                // is already durable (as a real Agent's own durable pair journal would
                // be), but the response is never sent -- the exact ambiguous point the
                // absolute invariant describes ("after start-reserved-pair's write
                // completes, any transport failure may mean the pair was captured").
                if (pipe.IsConnected)
                {
                    pipe.Disconnect();
                }
                if (scenario == "die-after-start")
                {
                    return dieExitCode;
                }
                continue; // pipe-only-after-start: stay alive for the next connection.
            }

            case DualHardwareCameraAgentProtocol.Operations.GetPairTransactionResult:
            {
                var transactionIdHex = payload.GetProperty("transactionId").GetString()!;
                var journalResult = await LoadDualAgentJournalCaptureResultPayloadAsync(pairJournalRoot, transactionIdHex);
                var responseJson = journalResult is null
                    ? BuildDualAgentResponseEnvelopeJson(
                        requestId,
                        false,
                        "PairTransactionNotFound",
                        new { transactionId = transactionIdHex, found = false, result = (object?)null })
                    : BuildDualAgentResponseEnvelopeJson(
                        requestId,
                        true,
                        "PairTransactionFound",
                        new { transactionId = transactionIdHex, found = true, result = journalResult });
                await WritePersistentTestFrameAsync(pipe, responseJson, responseTimeout.Token);
                break;
            }

            default:
                return 65;
        }

        if (pipe.IsConnected)
        {
            pipe.Disconnect();
        }
    }
}

static string? TryGetDualAgentArgument(IReadOnlyList<string> arguments, string flag)
{
    var index = arguments.ToList().IndexOf(flag);
    return index >= 0 && index + 1 < arguments.Count ? arguments[index + 1] : null;
}

static string? TryGetDualAgentTransactionId(JsonElement payload)
{
    if (payload.ValueKind != JsonValueKind.Object)
    {
        return null;
    }
    if (payload.TryGetProperty("transactionId", out var direct) && direct.ValueKind == JsonValueKind.String)
    {
        return direct.GetString();
    }
    if (payload.TryGetProperty("transaction", out var nested) && nested.ValueKind == JsonValueKind.Object &&
        nested.TryGetProperty("transactionId", out var nestedId) && nestedId.ValueKind == JsonValueKind.String)
    {
        return nestedId.GetString();
    }
    return null;
}

static async Task AppendDualAgentTraceAsync(string? tracePath, string pipeName, string operation, string? transactionId)
{
    if (string.IsNullOrWhiteSpace(tracePath))
    {
        return;
    }
    var line = JsonSerializer.Serialize(
        new { pipeName, operation, transactionId },
        new JsonSerializerOptions(JsonSerializerDefaults.Web));
    await File.AppendAllTextAsync(tracePath, line + Environment.NewLine);
}

static string BuildDualAgentResponseEnvelopeJson(string requestId, bool success, string resultCode, object payload) =>
    JsonSerializer.Serialize(
        new
        {
            schemaVersion = DualHardwareCameraAgentProtocol.SchemaVersion,
            simulation = false,
            marker = DualHardwareCameraAgentProtocol.Marker,
            requestId,
            success,
            resultCode,
            payload,
        },
        new JsonSerializerOptions(JsonSerializerDefaults.Web));

static object BuildDualCapabilitiesPayload() => new
{
    cameraMode = "DualCamera",
    protocolVersion = 2,
    orderedRequiredAliases = new[] { "CAM-A", "CAM-B" },
    supportedOperations = DualHardwareCameraAgentProtocol.Operations.Required,
    pairJournalDurable = true,
    sameTransactionQueryOnly = true,
    automaticRetryCount = 0,
};

static async Task<(string CamAPath, string CamBPath)> WriteDualAgentOriginalsAsync(
    string transactionDirectory,
    Guid transactionId)
{
    var adapter = new M2OfflineStitcherProcessAdapter(DualCameraM2AdapterPath());
    var camAPath = Path.Combine(transactionDirectory, "CAM-A", "original.jpg");
    var camBPath = Path.Combine(transactionDirectory, "CAM-B", "original.jpg");
    await adapter.CaptureAsync("CAM-A", transactionId, camAPath, CancellationToken.None);
    await adapter.CaptureAsync("CAM-B", transactionId, camBPath, CancellationToken.None);
    return (camAPath, camBPath);
}

static object BuildDualAgentCaptureResultPayload(
    JsonElement transaction,
    string transactionIdHex,
    string camAPath,
    string camBPath)
{
    var captureProfile = transaction.GetProperty("captureProfileSnapshot");
    var rigProfile = transaction.GetProperty("rigProfileSnapshot");
    var startedAtUtc = transaction.GetProperty("startedAtUtc").GetString()!;
    var watchdogDeadlineUtc = transaction.GetProperty("watchdogDeadlineUtc").GetString()!;
    return new
    {
        transactionId = transactionIdHex,
        originals = new object[]
        {
            new { alias = "CAM-A", canonicalOriginalPath = camAPath, exactRecoveredObjectDeleted = true, spoolEmptyAfterDelete = true },
            new { alias = "CAM-B", canonicalOriginalPath = camBPath, exactRecoveredObjectDeleted = true, spoolEmptyAfterDelete = true },
        },
        terminalState = "Succeeded",
        failureCode = "None",
        evidence = new
        {
            terminalState = "Succeeded",
            identitySnapshot = transaction.GetProperty("identitySnapshot").Clone(),
            captureProfileId = captureProfile.GetProperty("profileId").GetString()!,
            captureProfileVersion = captureProfile.GetProperty("version").GetString()!,
            profileId = rigProfile.GetProperty("profileId").GetString()!,
            profileVersion = rigProfile.GetProperty("version").GetString()!,
            watchdogStartedAtUtc = startedAtUtc,
            watchdogDeadlineUtc,
            completedAtUtc = startedAtUtc,
            watchdogCompletedInTime = true,
            liveViewStopAndCloseConfirmed = true,
            exactDeleteConfirmedForEveryRetainedOriginal = true,
            bothSpoolsEmptyAfter = true,
            automaticRetryCount = 0,
        },
    };
}

static async Task SaveDualAgentJournalEntryAsync(
    string pairJournalRoot,
    string transactionIdHex,
    JsonElement transaction,
    string camAPath,
    string camBPath)
{
    var path = Path.Combine(pairJournalRoot, $"{transactionIdHex}.json");
    var json = JsonSerializer.Serialize(
        new { transaction, camAPath, camBPath },
        new JsonSerializerOptions(JsonSerializerDefaults.Web));
    await File.WriteAllTextAsync(path, json);
}

static async Task<object?> LoadDualAgentJournalCaptureResultPayloadAsync(string pairJournalRoot, string transactionIdHex)
{
    var path = Path.Combine(pairJournalRoot, $"{transactionIdHex}.json");
    if (!File.Exists(path))
    {
        return null;
    }
    var json = await File.ReadAllTextAsync(path);
    using var document = JsonDocument.Parse(json);
    var root = document.RootElement;
    return BuildDualAgentCaptureResultPayload(
        root.GetProperty("transaction"),
        transactionIdHex,
        root.GetProperty("camAPath").GetString()!,
        root.GetProperty("camBPath").GetString()!);
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
        where T : notnull
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
        {
            throw new InvalidOperationException($"Expected {expected}, got {actual}.");
        }
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

        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
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

        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }
}

static class HardwareTestData
{
    public static HardwareSingleReadinessResult ReadyHardware(string alias) =>
        new()
        {
            CameraMode = "SingleCamera",
            CameraAlias = alias,
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
            Firmware = "redacted-known",
            LiveViewStatus = "off",
            LiveViewStatusAvailable = true,
            CaptureProfileApproved = true,
            CaptureProfileId = "single-profile",
            CaptureProfileVersion = 1,
            CaptureProfileSha256 = new string('a', 64),
            CaptureProfileCameraAlias = alias,
            CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
            CaptureProfileAliasMatches = true,
            SettingsMatchApprovedProfile = true,
            ObservedSettings = ReadyObservedSettings(),
            ReadOnly = true,
            CaptureCommandSent = false,
            CameraObjectDeleteAttempted = false,
            CameraSettingsChanged = false,
            RealIdentifiersIncluded = false,
            FailureCategory = string.Empty,
            FailureDetail = string.Empty,
        };

    private static HardwareObservedCameraSettings ReadyObservedSettings()
    {
        static HardwareObservedCameraSetting Setting(string label) => new()
        {
            Available = true,
            CapType = "enum",
            ProbeState = "observed",
            ValueType = "label",
            CurrentValue = null,
            CurrentIndex = 0,
            CurrentLabel = label,
        };

        return new HardwareObservedCameraSettings
        {
            FileType = Setting("JPEG"),
            CompressionLevel = Setting("Fine"),
            ImageSize = Setting("Large"),
            ExposureMode = Setting("Manual"),
            ShutterSpeed = Setting("profile-match"),
            Aperture = Setting("profile-match"),
            Sensitivity = Setting("profile-match"),
            WhiteBalanceMode = Setting("profile-match"),
            FocusMode = Setting("profile-match"),
        };
    }
}

sealed class WpfHardwareDualFakeOperations(
    ITestSyntheticCamera camera,
    bool responseUnknownOnce = false) : IDualHardwareCaptureOperations
{
    private DualHardwareCaptureRequest? _unknownRequest;
    public int ReserveCalls { get; private set; }
    public int StartCalls { get; private set; }
    public int QueryCalls { get; private set; }

    public Task<bool> ReservePairTransactionAsync(Guid transactionId, CancellationToken cancellationToken)
    {
        ReserveCalls++;
        return Task.FromResult(true);
    }

    public async Task<DualHardwareDispatchResult> StartReservedPairAsync(
        DualHardwareCaptureRequest request,
        CancellationToken cancellationToken)
    {
        StartCalls++;
        if (responseUnknownOnce)
        {
            _unknownRequest = request;
            return new(DualHardwareDispatchState.ResponseUnknown, null);
        }
        return new(DualHardwareDispatchState.Completed, await CreateResultAsync(request, cancellationToken));
    }

    private async Task<DualHardwareCaptureResult> CreateResultAsync(
        DualHardwareCaptureRequest request,
        CancellationToken cancellationToken)
    {
        var originals = new List<DualHardwareOriginalRecord>();
        foreach (var alias in new[] { "CAM-A", "CAM-B" })
        {
            var path = Path.Combine(request.TransactionDirectory, alias, "original.jpg");
            await camera.CaptureAsync(alias, request.TransactionId, path, cancellationToken);
            originals.Add(new(alias, path, true, true));
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
        return new DualHardwareCaptureResult(
            request.TransactionId,
            originals,
            DualHardwareCaptureTerminalState.Succeeded,
            DualCameraFailureCode.None,
            evidence);
    }

    public async Task<DualHardwarePairQueryOutcome> QueryPairTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        QueryCalls++;
        if (!responseUnknownOnce || QueryCalls < 2 || _unknownRequest is null)
            return new(DualHardwarePairQueryState.Reserved, null);
        return new(
            DualHardwarePairQueryState.Terminal,
            await CreateResultAsync(_unknownRequest, cancellationToken));
    }
}

sealed class MutableDualIdentitySource(DualCameraIdentitySnapshot initial) : IDualCameraIdentitySnapshotSource
{
    public event EventHandler<DualCameraIdentitySnapshot>? SnapshotChanged;

    public DualCameraIdentitySnapshot Current { get; private set; } = initial;

    public void Set(DualCameraIdentitySnapshot snapshot)
    {
        Current = snapshot;
        SnapshotChanged?.Invoke(this, snapshot);
    }
}

sealed class NeverCaptureDualBridge : ITestSyntheticCamera, IOfflineStitcherAdapter
{
    public int CaptureCalls { get; private set; }

    public Task<string> CaptureAsync(
        string alias,
        Guid transactionId,
        string destinationPath,
        CancellationToken cancellationToken)
    {
        _ = alias;
        _ = transactionId;
        _ = destinationPath;
        _ = cancellationToken;
        CaptureCalls++;
        return Task.FromException<string>(new InvalidOperationException("Identity gate was bypassed."));
    }

    public Task ValidateCanonicalJpegAsync(string jpegPath, int expectedWidth, int expectedHeight, CancellationToken cancellationToken) =>
        Task.FromException(new InvalidOperationException("Identity gate was bypassed."));

    public Task<OfflineStitchArtifact> StitchAsync(
        IReadOnlyList<CanonicalJpegOriginal> originals,
        string outputJobDirectory,
        DualCameraRigProfile profile,
        CancellationToken cancellationToken) =>
        Task.FromException<OfflineStitchArtifact>(new InvalidOperationException("Identity gate was bypassed."));

    public Task ExportAsync(string stitchedJpeg, string destinationJpeg, CancellationToken cancellationToken) =>
        Task.FromException(new InvalidOperationException("Identity gate was bypassed."));
}

sealed class BlockingFailedExportBridge(M2OfflineStitcherProcessAdapter inner) :
    ITestSyntheticCamera,
    IOfflineStitcherAdapter
{
    public TaskCompletionSource ExportStarted { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public TaskCompletionSource ReleaseExport { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public Task<string> CaptureAsync(
        string alias,
        Guid transactionId,
        string destinationPath,
        CancellationToken cancellationToken) =>
        inner.CaptureAsync(alias, transactionId, destinationPath, cancellationToken);

    public Task ValidateCanonicalJpegAsync(
        string jpegPath,
        int expectedWidth,
        int expectedHeight,
        CancellationToken cancellationToken) =>
        inner.ValidateCanonicalJpegAsync(jpegPath, expectedWidth, expectedHeight, cancellationToken);

    public Task<OfflineStitchArtifact> StitchAsync(
        IReadOnlyList<CanonicalJpegOriginal> originals,
        string outputJobDirectory,
        DualCameraRigProfile profile,
        CancellationToken cancellationToken) =>
        inner.StitchAsync(originals, outputJobDirectory, profile, cancellationToken);

    public async Task ExportAsync(
        string stitchedJpeg,
        string destinationJpeg,
        CancellationToken cancellationToken)
    {
        _ = stitchedJpeg;
        _ = destinationJpeg;
        ExportStarted.TrySetResult();
        await ReleaseExport.Task.WaitAsync(cancellationToken);
        throw new IOException("deterministic formal WPF export failure");
    }
}

sealed class BlockingTransactionService : ISimulatedTransactionService
{
    private readonly TaskCompletionSource _started = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly TaskCompletionSource _release = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public Task Started => _started.Task;

    public Task<IReadOnlyList<SimulatedWorkflowState>> InitializeAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult<IReadOnlyList<SimulatedWorkflowState>>([]);

    public Task<SimulatedWorkflowState> ExecuteAsync(
        Guid transactionId,
        SimulatedWorkflowScenario scenario,
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(transactionId, CapturePlan.Dual(), scenario, cancellationToken);

    public async Task<SimulatedWorkflowState> ExecuteAsync(
        Guid transactionId,
        CapturePlan capturePlan,
        SimulatedWorkflowScenario scenario,
        CancellationToken cancellationToken = default)
    {
        capturePlan.Validate();
        _started.TrySetResult();
        await _release.Task.WaitAsync(cancellationToken);
        return new SimulatedWorkflowState
        {
            Simulation = true,
            Marker = SimulatedTransactionProtocol.Marker,
            TransactionId = transactionId,
            OperatingMode = capturePlan.OperatingMode,
            RequiredCameraAliases = capturePlan.RequiredCameraAliases.ToArray(),
            State = SimulatedTransactionState.Complete,
            IsTerminal = true,
            RetainedOriginalAliases = capturePlan.RequiredCameraAliases.ToArray(),
            TerminalReason = null,
            AutomaticRetryCount = 0,
        };
    }

    public void Release() => _release.TrySetResult();
}

class FakeHardwareSingleCameraOperations : IHardwareSingleCameraOperations
{
    public string AgentExecutablePath => "C:\\fake\\A0CameraStitcher.CameraAgent.exe";

    public bool AgentExecutableAvailable => true;

    public int ReadinessCallCount { get; private set; }

    public int LiveViewCallCount { get; private set; }

    public int CaptureCallCount { get; private set; }

    public int TransactionResultCallCount { get; private set; }

    public bool LastCaptureLiveViewHandoffRequested { get; private set; }

    public HardwareCaptureProfileSnapshot? LastCaptureExpectedProfile { get; private set; }

    public HardwareCaptureProfileSnapshot? LastTransactionExpectedProfile { get; private set; }

    public string? LastTransactionExpectedCameraAlias { get; private set; }

    public bool LastTransactionExpectedHandoff { get; private set; }

    public List<string> CallOrder { get; } = [];

    public int TotalCallCount => ReadinessCallCount + LiveViewCallCount + CaptureCallCount + TransactionResultCallCount;

    public HardwarePreviewJpegRecord? Preview { get; init; }

    public Func<string, string, HardwareSingleCaptureResult>? CaptureResultFactory { get; init; }

    public Func<string, HardwareSingleReadinessResult>? ReadinessFactory { get; init; }

    public Exception? CaptureException { get; init; }

    public Queue<HardwareSingleCaptureResult> TransactionResults { get; } = new();

    public Task<HardwareCameraAgentReply<HardwareSingleReadinessResult>> GetReadinessAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default)
    {
        ReadinessCallCount++;
        CallOrder.Add("readiness");
        return Task.FromResult(new HardwareCameraAgentReply<HardwareSingleReadinessResult>(
            "readiness-request",
            true,
            "SingleReady",
            ReadinessFactory?.Invoke(cameraAlias) ?? HardwareTestData.ReadyHardware(cameraAlias)));
    }

    public Task<HardwareCameraAgentReply<HardwareSingleLiveViewResult>> ProbeLiveViewAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default)
    {
        LiveViewCallCount++;
        CallOrder.Add("finite-live-view");
        if (Preview is null)
        {
            throw new InvalidOperationException("A fake preview was not configured.");
        }

        return Task.FromResult(new HardwareCameraAgentReply<HardwareSingleLiveViewResult>(
            "live-view-request",
            true,
            "LiveViewProbeComplete",
            new HardwareSingleLiveViewResult
            {
                CameraMode = "SingleCamera",
                CameraAlias = cameraAlias,
                RunId = "run-3000-1",
                Frames = 1,
                LastFrameBytes = Preview.SizeBytes,
                LastFrameSha256 = Preview.Sha256,
                DurationMs = 100,
                PreviewPersisted = true,
                Preview = Preview,
                PreviewIsOriginal = false,
                PreviewIsStitchInput = false,
                LiveViewStopped = true,
                SdkSessionClosed = true,
                RealIdentifiersIncluded = false,
                ErrorCategory = string.Empty,
                ErrorDetail = string.Empty,
            }));
    }

    public Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> CaptureAsync(
        string transactionId,
        string cameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool liveViewHandoffRequested,
        CancellationToken cancellationToken = default)
    {
        CaptureCallCount++;
        CallOrder.Add("capture");
        LastCaptureExpectedProfile = expectedProfile;
        LastCaptureLiveViewHandoffRequested = liveViewHandoffRequested;
        if (CaptureException is not null)
        {
            throw CaptureException;
        }

        var payload = CaptureResultFactory?.Invoke(transactionId, cameraAlias)
            ?? throw new InvalidOperationException("A fake capture result was not configured.");
        return Task.FromResult(new HardwareCameraAgentReply<HardwareSingleCaptureResult>(
            "capture-request",
            payload.TerminalState == "Complete",
            payload.TerminalState == "Complete" ? "CaptureComplete" : payload.ErrorCategory,
            payload));
    }

    public Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> GetTransactionResultAsync(
        string transactionId,
        string expectedCameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool expectedLiveViewHandoffRequested,
        CancellationToken cancellationToken = default)
    {
        TransactionResultCallCount++;
        CallOrder.Add("get-result");
        LastTransactionExpectedCameraAlias = expectedCameraAlias;
        LastTransactionExpectedProfile = expectedProfile;
        LastTransactionExpectedHandoff = expectedLiveViewHandoffRequested;
        if (!TransactionResults.TryDequeue(out var payload))
        {
            throw new InvalidOperationException("A fake transaction result was not configured.");
        }

        var success = payload.TerminalState == "Complete";
        var resultCode = payload.TerminalState switch
        {
            "Complete" => "CaptureComplete",
            "Reserved" => "TransactionReserved",
            "InProgress" => "TransactionInProgress",
            _ => payload.ErrorCategory,
        };
        return Task.FromResult(new HardwareCameraAgentReply<HardwareSingleCaptureResult>(
            "transaction-request",
            success,
            resultCode,
            payload));
    }
}

sealed class FakeContinuousHardwareOperations(byte[] frameBytes) :
    FakeHardwareSingleCameraOperations,
    IHardwareContinuousLiveViewOperations
{
    private ulong _frameNumber;

    public int StartCount { get; private set; }

    public int StopCount { get; private set; }

    public bool FailNextStop { get; set; }

    public TaskCompletionSource FirstFrame { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public string CreateSessionId() => Guid.NewGuid().ToString("N");

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> StartLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default)
    {
        StartCount++;
        CallOrder.Add("start");
        return Task.FromResult(Reply(sessionId, true, "Started", 0, []));
    }

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> ReadLiveViewFrameAsync(
        string sessionId,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        _frameNumber++;
        CallOrder.Add("frame");
        FirstFrame.TrySetResult();
        return Task.FromResult(Reply(sessionId, true, "Frame", _frameNumber, frameBytes));
    }

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> HeartbeatLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default) =>
        Task.FromResult(Reply(sessionId, true, "Heartbeat", _frameNumber, []));

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> StopLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default)
    {
        StopCount++;
        CallOrder.Add("stop");
        if (FailNextStop)
        {
            FailNextStop = false;
            return Task.FromResult(Reply(
                sessionId, false, "continuous_live_view_stop_failed", _frameNumber, []));
        }
        return Task.FromResult(Reply(sessionId, true, "Stopped", _frameNumber, []));
    }

    private static HardwareCameraAgentReply<HardwareContinuousLiveViewResult> Reply(
        string sessionId,
        bool success,
        string state,
        ulong frameNumber,
        byte[] bytes)
    {
        var frame = state == "Frame";
        var running = state is "Started" or "Frame" or "Heartbeat";
        var errorCategory = success ? string.Empty : state;
        var payload = new HardwareContinuousLiveViewResult
        {
            CameraMode = "SingleCamera",
            CameraAlias = "CAM-A",
            SessionId = sessionId,
            State = success ? state : string.Empty,
            FrameNumber = frameNumber,
            FrameSize = frame ? bytes.Length : 0,
            FrameSha256 = frame
                ? Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant()
                : string.Empty,
            FrameJpegBase64 = frame ? Convert.ToBase64String(bytes) : string.Empty,
            PreviewIsOriginal = false,
            PreviewIsStitchInput = false,
            SdkSessionOpen = success && running,
            LiveViewRunning = success && running,
            HeartbeatTimeoutSeconds = 20,
            MaximumSessionSeconds = 600,
            RealIdentifiersIncluded = false,
            ErrorCategory = errorCategory,
            ErrorDetail = success ? string.Empty : "deterministic stop failure",
        };
        return new HardwareCameraAgentReply<HardwareContinuousLiveViewResult>(
            "continuous-request", success, state, payload);
    }
}

sealed class BlockingHardwareStateStore : IHardwareSingleAppStateStore
{
    private readonly TaskCompletionSource _release = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public TaskCompletionSource LoadStarted { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public async Task<HardwarePendingTransaction?> LoadPendingAsync(CancellationToken cancellationToken = default)
    {
        LoadStarted.TrySetResult();
        await _release.Task.WaitAsync(cancellationToken);
        return null;
    }

    public Task SavePendingAsync(
        HardwarePendingTransaction pendingTransaction,
        CancellationToken cancellationToken = default) =>
        throw new InvalidOperationException("The blocking startup store does not accept writes.");

    public Task MarkCaptureRequestDispatchAttemptedAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default) =>
        throw new InvalidOperationException("The blocking startup store does not accept writes.");

    public Task MarkCaptureRequestNotDispatchedAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default) =>
        throw new InvalidOperationException("The blocking startup store does not accept writes.");

    public Task ClearPendingAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default) =>
        throw new InvalidOperationException("The blocking startup store does not accept writes.");

    public void ReleaseLoad() => _release.TrySetResult();
}

sealed class MutableTimeProvider(DateTimeOffset utcNow) : TimeProvider
{
    private DateTimeOffset _utcNow = utcNow;
    private readonly List<MutableTimer> _timers = [];

    public override DateTimeOffset GetUtcNow() => _utcNow;

    public override ITimer CreateTimer(
        TimerCallback callback,
        object? state,
        TimeSpan dueTime,
        TimeSpan period)
    {
        var timer = new MutableTimer(this, callback, state, dueTime, period);
        _timers.Add(timer);
        return timer;
    }

    public void Advance(TimeSpan duration)
    {
        _utcNow = _utcNow.Add(duration);
        foreach (var timer in _timers.ToArray())
        {
            timer.FireIfDue(_utcNow);
        }
    }

    private sealed class MutableTimer : ITimer
    {
        private readonly MutableTimeProvider _owner;
        private readonly TimerCallback _callback;
        private readonly object? _state;
        private DateTimeOffset? _nextFireAt;
        private TimeSpan _period;
        private bool _disposed;

        public MutableTimer(
            MutableTimeProvider owner,
            TimerCallback callback,
            object? state,
            TimeSpan dueTime,
            TimeSpan period)
        {
            _owner = owner;
            _callback = callback;
            _state = state;
            Change(dueTime, period);
        }

        public bool Change(TimeSpan dueTime, TimeSpan period)
        {
            if (_disposed)
            {
                return false;
            }

            _period = period;
            _nextFireAt = dueTime == Timeout.InfiniteTimeSpan
                ? null
                : _owner.GetUtcNow().Add(dueTime);
            return true;
        }

        public void FireIfDue(DateTimeOffset now)
        {
            if (_disposed || _nextFireAt is null || now < _nextFireAt.Value)
            {
                return;
            }

            _nextFireAt = _period == Timeout.InfiniteTimeSpan
                ? null
                : now.Add(_period);
            _callback(_state);
        }

        public void Dispose()
        {
            _disposed = true;
            _nextFireAt = null;
        }

        public ValueTask DisposeAsync()
        {
            Dispose();
            return ValueTask.CompletedTask;
        }
    }
}
