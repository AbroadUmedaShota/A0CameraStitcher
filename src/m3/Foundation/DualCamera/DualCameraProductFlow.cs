using System.Buffers.Binary;
using System.Collections.ObjectModel;
using System.Security.Cryptography;

namespace A0CameraStitcher.M3.Foundation.DualCamera;

public sealed class DualCameraProductFlow : IDualCameraProductFlow
{
    public const long MaximumCompressedJpegBytes = 64L * 1024L * 1024L;
    public static readonly TimeSpan DefaultFailedOriginalDiscoveryTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan MaximumFailedOriginalDiscoveryTimeout = TimeSpan.FromMinutes(5);
    private readonly string _rootDirectory;
    private readonly IDualCameraCaptureSource _captureSource;
    private readonly IOfflineStitcherAdapter _stitcher;
    private readonly IDualCameraIdentitySnapshotSource _identitySource;
    private readonly TimeProvider _timeProvider;
    private readonly TimeSpan _failedOriginalDiscoveryTimeout;
    private readonly object _sync = new();
    private readonly Dictionary<DualCameraProductStage, DualCameraStageRecord> _stages = [];
    private readonly List<DualCameraStitchResult> _stitchJobs = [];
    private DualCameraProductState? _current;
    private DualCameraRigProfile? _profile;
    private HardwareDualCaptureProfile? _hardwareCaptureProfile;
    private DualCameraIdentitySnapshot _transactionIdentity = DualCameraIdentitySnapshot.HardwarePending();
    private Guid? _responseUnknownTransactionId;
    private bool _active;

    public event EventHandler<DualCameraProductState>? StateChanged;

    public event EventHandler<DualCameraIdentitySnapshot>? IdentityChanged;

    public DualCameraProductFlow(
        string rootDirectory,
        ITestSyntheticCamera camera,
        IOfflineStitcherAdapter stitcher)
        : this(
            rootDirectory,
            new TestSyntheticCaptureSource(camera),
            stitcher,
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.HardwarePending()))
    {
    }

    public DualCameraProductFlow(
        string rootDirectory,
        ITestSyntheticCamera camera,
        IOfflineStitcherAdapter stitcher,
        IDualCameraIdentitySnapshotSource identitySource,
        TimeProvider? timeProvider = null)
        : this(rootDirectory, camera, stitcher, identitySource, timeProvider, DefaultFailedOriginalDiscoveryTimeout)
    {
    }

    public DualCameraProductFlow(
        string rootDirectory,
        ITestSyntheticCamera camera,
        IOfflineStitcherAdapter stitcher,
        IDualCameraIdentitySnapshotSource identitySource,
        TimeProvider? timeProvider,
        TimeSpan failedOriginalDiscoveryTimeout)
    {
        if (string.IsNullOrWhiteSpace(rootDirectory))
        {
            throw new ArgumentException("A product artifact root is required.", nameof(rootDirectory));
        }

        _rootDirectory = Path.GetFullPath(rootDirectory);
        _captureSource = new TestSyntheticCaptureSource(camera ?? throw new ArgumentNullException(nameof(camera)));
        _stitcher = stitcher ?? throw new ArgumentNullException(nameof(stitcher));
        _identitySource = identitySource ?? throw new ArgumentNullException(nameof(identitySource));
        _timeProvider = timeProvider ?? TimeProvider.System;
        _failedOriginalDiscoveryTimeout = ValidateFailedOriginalDiscoveryTimeout(failedOriginalDiscoveryTimeout);
        _identitySource.SnapshotChanged += OnIdentitySnapshotChanged;
        Directory.CreateDirectory(_rootDirectory);
        RestorePendingRecovery();
    }

    public DualCameraProductFlow(
        string rootDirectory,
        IDualCameraCaptureSource captureSource,
        IOfflineStitcherAdapter stitcher,
        IDualCameraIdentitySnapshotSource identitySource,
        TimeProvider? timeProvider = null)
        : this(rootDirectory, captureSource, stitcher, identitySource, timeProvider, DefaultFailedOriginalDiscoveryTimeout)
    {
    }

    public DualCameraProductFlow(
        string rootDirectory,
        IDualCameraCaptureSource captureSource,
        IOfflineStitcherAdapter stitcher,
        IDualCameraIdentitySnapshotSource identitySource,
        TimeProvider? timeProvider,
        TimeSpan failedOriginalDiscoveryTimeout)
    {
        if (string.IsNullOrWhiteSpace(rootDirectory))
        {
            throw new ArgumentException("A product artifact root is required.", nameof(rootDirectory));
        }
        _rootDirectory = Path.GetFullPath(rootDirectory);
        _captureSource = captureSource ?? throw new ArgumentNullException(nameof(captureSource));
        _stitcher = stitcher ?? throw new ArgumentNullException(nameof(stitcher));
        _identitySource = identitySource ?? throw new ArgumentNullException(nameof(identitySource));
        _timeProvider = timeProvider ?? TimeProvider.System;
        _failedOriginalDiscoveryTimeout = ValidateFailedOriginalDiscoveryTimeout(failedOriginalDiscoveryTimeout);
        _identitySource.SnapshotChanged += OnIdentitySnapshotChanged;
        Directory.CreateDirectory(_rootDirectory);
        RestorePendingRecovery();
    }

    public DualCameraIdentitySnapshot IdentitySnapshot =>
        _identitySource.Current.EvaluateAt(_timeProvider.GetUtcNow());

    public DualCameraExecutionEnvironment ExecutionEnvironment => _captureSource.Environment;

    public DualCameraProductState? Current
    {
        get
        {
            lock (_sync)
            {
                return _current;
            }
        }
    }

    public async Task<DualCameraProductState> CaptureAndStitchAsync(
        DualCameraCaptureRequest request,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.Mode != CameraOperatingMode.DualCamera)
        {
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidMode, "DualCamera product flow cannot change or fall back to SingleCamera.");
        }
        if (request.ExecutionEnvironment != _captureSource.Environment)
        {
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidExecutionEnvironment, "The requested execution environment does not match the configured typed capture source; fallback is prohibited.");
        }
        if (!Enum.IsDefined(request.TestFault) ||
            (request.ExecutionEnvironment == DualCameraExecutionEnvironment.HardwareDual && request.TestFault != DualCameraTestFault.None))
        {
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidExecutionEnvironment, "The TestSynthetic fault plan is unsupported.");
        }

        lock (_sync)
        {
            if (_responseUnknownTransactionId.HasValue)
                return _current ?? throw new InvalidOperationException("The response-unknown transaction state is unavailable.");
        }

        var startedAtUtc = _timeProvider.GetUtcNow();
        request.Profile.Validate(startedAtUtc);
        var transactionId = request.TransactionId ?? Guid.NewGuid();
        if (transactionId == Guid.Empty)
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidExecutionEnvironment, "A non-empty transaction ID is required.");
        BeginOperation(transactionId, request);
        try
        {
            SetStage(DualCameraProductStage.CaptureCameraA, DualCameraStageStatus.Active, "typed pair capture source started");
            var transactionDirectory = Path.Combine(_rootDirectory, "transactions", transactionId.ToString("N"));
            var sourceResult = await _captureSource.CapturePairAsync(
                new DualCameraCaptureSourceRequest(
                    transactionId,
                    transactionDirectory,
                    _transactionIdentity,
                    _profile!,
                    _hardwareCaptureProfile,
                    request.HardwareConfirmations,
                    request.TestFault,
                    startedAtUtc,
                    startedAtUtc.AddSeconds(180)),
                cancellationToken).ConfigureAwait(false);
            return await CompleteCaptureAndStitchAsync(
                sourceResult,
                transactionId,
                request.TestFault,
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException exception)
        {
            return await FailOperationAsync(DualCameraFailureCode.Interrupted, "The active product flow was interrupted; no automatic retry was attempted.", exception).ConfigureAwait(false);
        }
        catch (DualCameraFlowException exception)
        {
            return await FailOperationAsync(exception.Code, exception.Message, exception).ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            return await FailOperationAsync(DualCameraFailureCode.StitchFailed, exception.Message, exception).ConfigureAwait(false);
        }
    }

    public async Task<DualCameraProductState> RecoverAndStitchAsync(
        Guid transactionId,
        CancellationToken cancellationToken = default)
    {
        BeginRecovery(transactionId);
        try
        {
            SetStage(DualCameraProductStage.CaptureCameraA, DualCameraStageStatus.Active, "saved HardwareDual transaction query started");
            var recoverableSource = (IRecoverableDualCameraCaptureSource)_captureSource;
            var sourceResult = await recoverableSource.RecoverPairAsync(transactionId, cancellationToken).ConfigureAwait(false);
            return await CompleteCaptureAndStitchAsync(
                sourceResult,
                transactionId,
                DualCameraTestFault.None,
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException exception)
        {
            return await FailOperationAsync(DualCameraFailureCode.Interrupted, "The transaction query was interrupted; no capture was reserved or dispatched again.", exception).ConfigureAwait(false);
        }
        catch (DualCameraFlowException exception)
        {
            return await FailOperationAsync(exception.Code, exception.Message, exception).ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            return await FailOperationAsync(DualCameraFailureCode.StitchFailed, exception.Message, exception).ConfigureAwait(false);
        }
    }

    private async Task<DualCameraProductState> CompleteCaptureAndStitchAsync(
        DualCameraCaptureSourceResult sourceResult,
        Guid transactionId,
        DualCameraTestFault testFault,
        CancellationToken cancellationToken)
    {
        var profile = _profile ?? throw new InvalidOperationException("Profile snapshot is unavailable.");
        var originals = new List<CanonicalJpegOriginal>(2);
        foreach (var retained in sourceResult.RetainedOriginals)
        {
            var alias = retained.Alias;
            var captureStage = alias == "CAM-A" ? DualCameraProductStage.CaptureCameraA : DualCameraProductStage.CaptureCameraB;
            var validationStage = alias == "CAM-A" ? DualCameraProductStage.ValidateCameraA : DualCameraProductStage.ValidateCameraB;
            SetStage(captureStage, DualCameraStageStatus.Succeeded, $"{alias} captured once");
            SetStage(validationStage, DualCameraStageStatus.Active, $"{alias} canonical JPEG validation started");
            var original = await ValidateCanonicalOriginalAsync(
                alias,
                retained.Path,
                profile.ExpectedInputWidth,
                profile.ExpectedInputHeight,
                cancellationToken).ConfigureAwait(false);
            originals.Add(original);
            SetStage(validationStage, DualCameraStageStatus.Succeeded, $"{alias} canonical JPEG verified");
        }

        lock (_sync)
        {
            _responseUnknownTransactionId = sourceResult.FailureCode == DualCameraFailureCode.AgentResponseUnknown
                ? transactionId
                : null;
            _current = Snapshot(
                capture: new DualCameraCaptureResult(
                    transactionId,
                    originals.AsReadOnly(),
                    sourceResult.Succeeded,
                    sourceResult.FailureCode,
                    sourceResult.HardwareEvidence));
        }

        if (!sourceResult.Succeeded)
            throw new DualCameraFlowException(sourceResult.FailureCode, sourceResult.FailureReason ?? "Typed pair capture failed without automatic retry.");
        if (originals.Count != 2 || !originals.Select(item => item.Alias).SequenceEqual(["CAM-A", "CAM-B"], StringComparer.Ordinal))
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, "Typed pair capture must retain exactly CAM-A and CAM-B canonical originals.");

        if (testFault == DualCameraTestFault.FailStitch)
        {
            const string reason = "TestSynthetic stitch failure was requested; originals remain retained.";
            var jobId = Guid.NewGuid();
            var failedStitch = new DualCameraStitchResult(
                jobId,
                string.Empty,
                false,
                DualCameraFailureCode.StitchFailed,
                reason);
            lock (_sync)
            {
                _stitchJobs.Add(failedStitch);
                _current = Snapshot(stitch: failedStitch, failureCode: DualCameraFailureCode.StitchFailed, failureReason: reason);
            }
            SetStage(DualCameraProductStage.Stitch, DualCameraStageStatus.Failed, reason);
            throw new DualCameraFlowException(DualCameraFailureCode.StitchFailed, reason);
        }
        await StitchCoreAsync(cancellationToken).ConfigureAwait(false);
        SetStage(DualCameraProductStage.Review, DualCameraStageStatus.Active, "Stitched JPEG awaiting operator review");
        return CompleteOperation();
    }

    public async Task<DualCameraProductState> RestitchAsync(CancellationToken cancellationToken = default)
    {
        BeginContinuation();
        try
        {
            SetStage(DualCameraProductStage.Review, DualCameraStageStatus.Pending, "A distinct restitch job was requested");
            await StitchCoreAsync(cancellationToken).ConfigureAwait(false);
            SetStage(DualCameraProductStage.Review, DualCameraStageStatus.Active, "Restitched JPEG awaiting operator review");
            return CompleteOperation();
        }
        catch (OperationCanceledException exception)
        {
            return await FailOperationAsync(DualCameraFailureCode.Interrupted, "The stitch job was interrupted; no automatic retry was attempted.", exception).ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            return await FailOperationAsync(DualCameraFailureCode.StitchFailed, exception.Message, exception).ConfigureAwait(false);
        }
    }

    public async Task<DualCameraProductState> ExportAsync(
        string fixedLocalDirectory,
        CancellationToken cancellationToken = default)
    {
        var directory = ValidateFixedLocalDirectory(fixedLocalDirectory);
        BeginContinuation(requireSuccessfulStitch: true);
        var exportJobId = Guid.NewGuid();
        var destination = Path.Combine(directory, $"a0-stitched-{exportJobId:N}.jpg");
        SetStage(DualCameraProductStage.Review, DualCameraStageStatus.Succeeded, "Operator proceeded from result review to explicit export");
        SetStage(DualCameraProductStage.Export, DualCameraStageStatus.Active, "Explicit export started");
        try
        {
            var stitch = Current!.Stitch!;
            await _stitcher.ExportAsync(stitch.OutputPath, destination, cancellationToken).ConfigureAwait(false);
            if (!File.ReadAllBytes(stitch.OutputPath).SequenceEqual(File.ReadAllBytes(destination)))
            {
                throw new IOException("Export verification was not byte-identical.");
            }

            SetStage(DualCameraProductStage.Export, DualCameraStageStatus.Succeeded, "Explicit byte-identical export verified");
            lock (_sync)
            {
                _current = Snapshot(export: new DualCameraExportResult(exportJobId, destination, true, DualCameraFailureCode.None, null));
            }
            return CompleteOperation();
        }
        catch (OperationCanceledException exception)
        {
            return FailExport(exportJobId, destination, DualCameraFailureCode.Interrupted, exception.Message);
        }
        catch (Exception exception)
        {
            return FailExport(exportJobId, destination, DualCameraFailureCode.ExportFailed, exception.Message);
        }
    }

    private void BeginOperation(Guid transactionId, DualCameraCaptureRequest request)
    {
        DualCameraProductState state;
        lock (_sync)
        {
            if (_active)
            {
                throw new DualCameraFlowException(DualCameraFailureCode.DuplicateStart, "A DualCamera product operation is already active.");
            }

            var identity = IdentitySnapshot;
            if (!identity.IsReady)
            {
                throw new DualCameraFlowException(
                    DualCameraFailureCode.IdentityNotReady,
                    $"DualCamera identity is {identity.Status}; capture was rejected before side effects.");
            }

            if (request.ExecutionEnvironment == DualCameraExecutionEnvironment.HardwareDual)
            {
                if (request.HardwareCaptureProfile is null)
                    throw new DualCameraFlowException(DualCameraFailureCode.InvalidProfile, "A HardwareDual capture profile snapshot is required.");
                request.HardwareCaptureProfile.Validate(_timeProvider.GetUtcNow());
                _hardwareCaptureProfile = request.HardwareCaptureProfile.Freeze();
            }
            else
            {
                _hardwareCaptureProfile = null;
            }

            _active = true;
            _transactionIdentity = identity with { };
            _profile = request.Profile with
            {
                CameraBToCameraA = Array.AsReadOnly(request.Profile.CameraBToCameraA.ToArray()),
                Crop = Array.AsReadOnly(request.Profile.Crop.ToArray()),
                CameraAliases = Array.AsReadOnly(request.Profile.CameraAliases.ToArray()),
            };
            _stages.Clear();
            foreach (var stage in Enum.GetValues<DualCameraProductStage>())
            {
                _stages[stage] = new(stage, DualCameraStageStatus.Pending, null);
            }
            _stitchJobs.Clear();
            _current = null;
            _current = Snapshot(transactionId: transactionId, executionEnvironment: request.ExecutionEnvironment);
            state = _current;
        }
        PublishState(state);
    }

    private void BeginRecovery(Guid transactionId)
    {
        DualCameraProductState state;
        lock (_sync)
        {
            if (_active)
                throw new DualCameraFlowException(DualCameraFailureCode.DuplicateStart, "A DualCamera product operation is already active.");
            if (_captureSource.Environment != DualCameraExecutionEnvironment.HardwareDual ||
                _captureSource is not IRecoverableDualCameraCaptureSource)
                throw new DualCameraFlowException(DualCameraFailureCode.InvalidExecutionEnvironment, "Only HardwareDual supports transaction recovery.");
            if (transactionId == Guid.Empty || _current?.TransactionId != transactionId ||
                _responseUnknownTransactionId != transactionId ||
                _current.FailureCode != DualCameraFailureCode.AgentResponseUnknown ||
                _profile is null || _hardwareCaptureProfile is null)
                throw new DualCameraFlowException(DualCameraFailureCode.AgentResponseUnknown, "The requested response-unknown transaction has no frozen product snapshot.");

            _active = true;
            _current = Snapshot();
            state = _current;
        }
        PublishState(state);
    }

    private void RestorePendingRecovery()
    {
        if (_captureSource is not IRecoverableDualCameraCaptureSource recoverable ||
            recoverable.PendingRecoveryRequest is not { } pending)
        {
            return;
        }

        _transactionIdentity = pending.IdentitySnapshot with { };
        _profile = pending.RigProfileSnapshot with
        {
            CameraBToCameraA = Array.AsReadOnly(pending.RigProfileSnapshot.CameraBToCameraA.ToArray()),
            Crop = Array.AsReadOnly(pending.RigProfileSnapshot.Crop.ToArray()),
            CameraAliases = Array.AsReadOnly(pending.RigProfileSnapshot.CameraAliases.ToArray()),
        };
        _hardwareCaptureProfile = pending.CaptureProfileSnapshot.Freeze();
        _responseUnknownTransactionId = pending.TransactionId;
        _stages.Clear();
        foreach (var stage in Enum.GetValues<DualCameraProductStage>())
        {
            _stages[stage] = new(stage, DualCameraStageStatus.Pending, null);
        }
        _current = Snapshot(
            transactionId: pending.TransactionId,
            executionEnvironment: DualCameraExecutionEnvironment.HardwareDual,
            failureCode: DualCameraFailureCode.AgentResponseUnknown,
            failureReason: "A durable response-unknown HardwareDual transaction is awaiting query-only recovery.");
    }

    private void BeginContinuation(bool requireSuccessfulStitch = false)
    {
        DualCameraProductState state;
        lock (_sync)
        {
            if (_active)
            {
                throw new DualCameraFlowException(DualCameraFailureCode.DuplicateStart, "A DualCamera product operation is already active.");
            }
            if (_current?.Capture is null || _current.Capture.Originals.Count != 2)
            {
                throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, "Both canonical originals are required.");
            }
            if (requireSuccessfulStitch && _current.Stitch?.Succeeded != true)
            {
                throw new DualCameraFlowException(DualCameraFailureCode.StitchFailed, "A successful stitch result is required for export.");
            }
            _active = true;
            _current = Snapshot();
            state = _current;
        }
        PublishState(state);
    }

    private async Task StitchCoreAsync(CancellationToken cancellationToken)
    {
        var current = Current ?? throw new InvalidOperationException("Product state is unavailable.");
        var profile = _profile ?? throw new InvalidOperationException("Profile snapshot is unavailable.");
        var originals = current.Capture?.Originals ?? throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, "Both canonical originals are required.");
        foreach (var original in originals)
        {
            await VerifyOriginalUnchangedAsync(original, cancellationToken).ConfigureAwait(false);
        }

        var jobId = Guid.NewGuid();
        var jobDirectory = Path.Combine(_rootDirectory, "stitch-jobs", jobId.ToString("N"));
        SetStage(DualCameraProductStage.Stitch, DualCameraStageStatus.Active, $"stitch job {jobId:N}");
        try
        {
            var captureTransactionId = current.Capture?.TransactionId
                ?? throw new DualCameraFlowException(
                    DualCameraFailureCode.InvalidOriginal,
                    "The CaptureTransaction this stitch consumes must be identified before it is recorded.");
            var artifact = await _stitcher.StitchAsync(
                originals,
                jobDirectory,
                profile,
                jobId,
                captureTransactionId,
                DateTimeOffset.UtcNow,
                cancellationToken).ConfigureAwait(false);
            // The manifest is the commit point: without it this is a file on disk,
            // not a completed job (Issue #39 decision).
            if (!string.Equals(Path.GetFileName(artifact.OutputPath), "stitched.jpg", StringComparison.OrdinalIgnoreCase) ||
                !File.Exists(artifact.OutputPath) ||
                artifact.ManifestFileName.Length == 0 ||
                !File.Exists(Path.Combine(jobDirectory, artifact.ManifestFileName)) ||
                !string.Equals(artifact.ProfileId, profile.ProfileId, StringComparison.Ordinal))
            {
                throw new InvalidDataException("The offline stitcher did not publish the expected atomic stitched.jpg artifact.");
            }
            foreach (var original in originals)
            {
                await VerifyOriginalUnchangedAsync(original, cancellationToken).ConfigureAwait(false);
            }

            var result = new DualCameraStitchResult(jobId, artifact.OutputPath, true, DualCameraFailureCode.None, null);
            lock (_sync)
            {
                _stitchJobs.Add(result);
                _current = Snapshot(stitch: result);
            }
            SetStage(DualCameraProductStage.Stitch, DualCameraStageStatus.Succeeded, $"stitch job {jobId:N} published atomically");
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            var result = new DualCameraStitchResult(jobId, string.Empty, false, DualCameraFailureCode.StitchFailed, exception.Message);
            lock (_sync)
            {
                _stitchJobs.Add(result);
                _current = Snapshot(stitch: result, failureCode: DualCameraFailureCode.StitchFailed, failureReason: exception.Message);
            }
            SetStage(DualCameraProductStage.Stitch, DualCameraStageStatus.Failed, exception.Message);
            throw new DualCameraFlowException(DualCameraFailureCode.StitchFailed, exception.Message, exception);
        }
    }

    private async Task<CanonicalJpegOriginal> ValidateCanonicalOriginalAsync(
        string alias,
        string path,
        int expectedWidth,
        int expectedHeight,
        CancellationToken cancellationToken)
    {
        if (!string.Equals(Path.GetFileName(path), "original.jpg", StringComparison.OrdinalIgnoreCase) || !File.Exists(path))
        {
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, $"{alias} canonical original.jpg is missing.");
        }
        var fileLength = new FileInfo(path).Length;
        if (fileLength < 4 || fileLength > MaximumCompressedJpegBytes)
        {
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, $"{alias} original is malformed or exceeds the compressed JPEG limit.");
        }
        var bytes = await File.ReadAllBytesAsync(path, cancellationToken).ConfigureAwait(false);
        if (bytes.LongLength != fileLength ||
            bytes[0] != 0xff || bytes[1] != 0xd8 || bytes[^2] != 0xff || bytes[^1] != 0xd9)
        {
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, $"{alias} original is malformed or exceeds the compressed JPEG limit.");
        }
        var (width, height) = ReadJpegDimensions(bytes);
        if (width != expectedWidth || height != expectedHeight)
        {
            throw new DualCameraFlowException(
                DualCameraFailureCode.InvalidOriginal,
                $"{alias} original dimensions {width}x{height} do not match the fixed profile {expectedWidth}x{expectedHeight}.");
        }
        try
        {
            await _stitcher.ValidateCanonicalJpegAsync(
                path,
                expectedWidth,
                expectedHeight,
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception exception)
        {
            throw new DualCameraFlowException(
                DualCameraFailureCode.InvalidOriginal,
                $"{alias} original failed complete WIC JPEG decode validation.",
                exception);
        }
        return new CanonicalJpegOriginal(
            alias,
            Path.GetFullPath(path),
            bytes.LongLength,
            Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
            width,
            height,
            true);
    }

    private async Task VerifyOriginalUnchangedAsync(
        CanonicalJpegOriginal original,
        CancellationToken cancellationToken)
    {
        var verified = await ValidateCanonicalOriginalAsync(
            original.Alias,
            original.Path,
            original.Width,
            original.Height,
            cancellationToken).ConfigureAwait(false);
        if (verified.SizeBytes != original.SizeBytes || !string.Equals(verified.Sha256, original.Sha256, StringComparison.Ordinal))
        {
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, $"{original.Alias} canonical original changed after capture.");
        }
    }

    private static string ValidateFixedLocalDirectory(string directory)
    {
        if (string.IsNullOrWhiteSpace(directory))
        {
            throw new ArgumentException("An operator-selected fixed local directory is required.", nameof(directory));
        }
        var fullPath = Path.GetFullPath(directory);
        if (!Path.IsPathRooted(fullPath) || fullPath.StartsWith("\\\\", StringComparison.Ordinal) ||
            fullPath.StartsWith("//", StringComparison.Ordinal) ||
            fullPath.StartsWith("\\\\?\\", StringComparison.Ordinal) ||
            fullPath.StartsWith("\\\\.\\", StringComparison.Ordinal) ||
            !Directory.Exists(fullPath))
        {
            throw new ArgumentException("Export requires an existing fixed local directory.", nameof(directory));
        }
        var root = Path.GetPathRoot(fullPath);
        if (string.IsNullOrWhiteSpace(root) || new DriveInfo(root).DriveType != DriveType.Fixed ||
            fullPath.IndexOf(':', root.Length) >= 0)
        {
            throw new ArgumentException("Export requires a fixed local drive without an alternate data stream.", nameof(directory));
        }
        var current = root;
        foreach (var segment in fullPath[root.Length..].Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, segment);
            if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
            {
                throw new ArgumentException("Export does not allow a reparse-point path.", nameof(directory));
            }
        }
        return fullPath;
    }

    private void SetStage(DualCameraProductStage stage, DualCameraStageStatus status, string? detail)
    {
        DualCameraProductState? state;
        lock (_sync)
        {
            _stages[stage] = new(stage, status, detail);
            if (_current is not null)
            {
                _current = Snapshot();
            }
            state = _current;
        }
        if (state is not null) PublishState(state);
    }

    private DualCameraProductState CompleteOperation()
    {
        DualCameraProductState state;
        lock (_sync)
        {
            _active = false;
            _current = Snapshot(failureCode: DualCameraFailureCode.None, failureReason: null);
            state = _current;
        }
        PublishState(state);
        return state;
    }

    private async Task<DualCameraProductState> FailOperationAsync(DualCameraFailureCode code, string reason, Exception exception)
    {
        _ = exception;
        Guid? transactionId;
        DualCameraRigProfile? profile;
        lock (_sync)
        {
            transactionId = _current?.Capture is null ? _current?.TransactionId : null;
            profile = transactionId.HasValue ? _profile : null;
        }

        var originals = transactionId.HasValue && profile is not null
            ? await DiscoverValidatedOriginalsAsync(transactionId.Value, profile).ConfigureAwait(false)
            : [];
        DualCameraProductState state;
        lock (_sync)
        {
            _active = false;
            var capture = _current?.Capture;
            if (capture is null && _current is not null)
            {
                capture = new(_current.TransactionId, originals, false, code);
            }
            var activeStage = _stages.Values.FirstOrDefault(record => record.Status == DualCameraStageStatus.Active);
            if (activeStage is not null)
            {
                _stages[activeStage.Stage] = activeStage with { Status = DualCameraStageStatus.Failed, Detail = reason };
            }
            _current = Snapshot(capture: capture, failureCode: code, failureReason: reason);
            state = _current;
        }
        PublishState(state);
        return state;
    }

    private DualCameraProductState FailExport(Guid jobId, string destination, DualCameraFailureCode code, string reason)
    {
        var outputPath = File.Exists(destination) ? destination : null;
        DualCameraProductState state;
        lock (_sync)
        {
            _active = false;
            _stages[DualCameraProductStage.Export] = new(DualCameraProductStage.Export, DualCameraStageStatus.Failed, reason);
            _current = Snapshot(
                export: new DualCameraExportResult(jobId, outputPath, false, code, reason),
                failureCode: code,
                failureReason: reason);
            state = _current;
        }
        PublishState(state);
        return state;
    }

    private void PublishState(DualCameraProductState state)
    {
        var handlers = StateChanged;
        if (handlers is null) return;
        foreach (EventHandler<DualCameraProductState> handler in handlers.GetInvocationList())
        {
            try
            {
                handler(this, state);
            }
            catch (Exception)
            {
                // Product persistence and no-retry behavior must not depend on a UI observer.
            }
        }
    }

    private void OnIdentitySnapshotChanged(object? sender, DualCameraIdentitySnapshot snapshot)
    {
        var handlers = IdentityChanged;
        if (handlers is null) return;
        foreach (EventHandler<DualCameraIdentitySnapshot> handler in handlers.GetInvocationList())
        {
            try
            {
                handler(this, snapshot);
            }
            catch (Exception)
            {
                // Identity gating must not depend on a UI observer.
            }
        }
    }

    private async Task<IReadOnlyList<CanonicalJpegOriginal>> DiscoverValidatedOriginalsAsync(
        Guid transactionId,
        DualCameraRigProfile profile)
    {
        var originals = new List<CanonicalJpegOriginal>();
        using var timeout = new CancellationTokenSource(_failedOriginalDiscoveryTimeout);
        foreach (var alias in new[] { "CAM-A", "CAM-B" })
        {
            if (timeout.IsCancellationRequested) break;
            var path = Path.Combine(_rootDirectory, "transactions", transactionId.ToString("N"), alias, "original.jpg");
            Task<CanonicalJpegOriginal>? validation = null;
            try
            {
                validation = ValidateCanonicalOriginalAsync(
                    alias,
                    path,
                    profile.ExpectedInputWidth,
                    profile.ExpectedInputHeight,
                    timeout.Token);
                originals.Add(await validation.WaitAsync(timeout.Token).ConfigureAwait(false));
            }
            catch (OperationCanceledException) when (timeout.IsCancellationRequested)
            {
                if (validation is not null) ObserveFault(validation);
                break;
            }
            catch (Exception)
            {
                // Original discovery is best-effort only. A failed, canceled, or timed-out
                // revalidation must not prevent terminal failure publication.
            }
        }
        return new ReadOnlyCollection<CanonicalJpegOriginal>(originals);
    }

    private static TimeSpan ValidateFailedOriginalDiscoveryTimeout(TimeSpan? timeout)
    {
        var effective = timeout ?? DefaultFailedOriginalDiscoveryTimeout;
        if (effective <= TimeSpan.Zero || effective > MaximumFailedOriginalDiscoveryTimeout)
            throw new ArgumentOutOfRangeException(nameof(timeout), "The failed-original discovery timeout must be greater than zero and no more than five minutes.");
        return effective;
    }

    private static void ObserveFault(Task task)
    {
        _ = task.ContinueWith(
            completed => _ = completed.Exception,
            CancellationToken.None,
            TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }

    private static (int Width, int Height) ReadJpegDimensions(ReadOnlySpan<byte> bytes)
    {
        var offset = 2;
        while (offset + 3 < bytes.Length)
        {
            while (offset < bytes.Length && bytes[offset] != 0xff) offset++;
            while (offset < bytes.Length && bytes[offset] == 0xff) offset++;
            if (offset >= bytes.Length) break;
            var marker = bytes[offset++];
            if (marker is 0xd8 or 0xd9 || marker is >= 0xd0 and <= 0xd7) continue;
            if (offset + 2 > bytes.Length) break;
            var segmentLength = BinaryPrimitives.ReadUInt16BigEndian(bytes[offset..]);
            if (segmentLength < 2 || offset + segmentLength > bytes.Length) break;
            var isStartOfFrame = marker is >= 0xc0 and <= 0xc3 or
                >= 0xc5 and <= 0xc7 or
                >= 0xc9 and <= 0xcb or
                >= 0xcd and <= 0xcf;
            if (isStartOfFrame)
            {
                if (segmentLength < 7) break;
                var height = BinaryPrimitives.ReadUInt16BigEndian(bytes[(offset + 3)..]);
                var width = BinaryPrimitives.ReadUInt16BigEndian(bytes[(offset + 5)..]);
                if (width == 0 || height == 0) break;
                return (width, height);
            }
            if (marker == 0xda) break;
            offset += segmentLength;
        }
        throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, "The JPEG does not contain a valid start-of-frame dimension record.");
    }

    private DualCameraProductState Snapshot(
        Guid? transactionId = null,
        DualCameraExecutionEnvironment? executionEnvironment = null,
        DualCameraCaptureResult? capture = null,
        DualCameraStitchResult? stitch = null,
        DualCameraExportResult? export = null,
        DualCameraFailureCode? failureCode = null,
        string? failureReason = null)
    {
        var previous = _current;
        var profile = _profile;
        return new DualCameraProductState
        {
            Mode = CameraOperatingMode.DualCamera,
            ExecutionEnvironment = executionEnvironment ?? previous?.ExecutionEnvironment ?? DualCameraExecutionEnvironment.TestSynthetic,
            TransactionId = transactionId ?? previous?.TransactionId ?? Guid.Empty,
            ProfileId = profile?.ProfileId ?? previous?.ProfileId ?? string.Empty,
            ProfileVersion = profile?.Version ?? previous?.ProfileVersion ?? string.Empty,
            IdentitySnapshot = previous?.IdentitySnapshot ?? _transactionIdentity,
            IsActive = _active,
            Stages = _stages.Values.OrderBy(record => record.Stage).ToArray(),
            Capture = capture ?? previous?.Capture,
            Stitch = stitch ?? previous?.Stitch,
            StitchJobs = _stitchJobs.ToArray(),
            Export = export ?? previous?.Export,
            FailureCode = failureCode ?? previous?.FailureCode ?? DualCameraFailureCode.None,
            FailureReason = failureReason,
            AutomaticRetryCount = 0,
        };
    }
}
