using System.Collections.ObjectModel;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

/// <summary>
/// Result shown by the dedicated DualCamera hardware acceptance path. It is not a
/// product stitch result: both fields below remain explicitly pending/unapproved.
/// </summary>
public sealed record HardwareDualCaptureRecoveryOnlyExecution(
    Guid TransactionId,
    DualHardwareCaptureTerminalState TerminalState,
    DualCameraFailureCode FailureCode,
    string? FailureReason,
    IReadOnlyList<CanonicalJpegOriginal> Originals,
    string TransactionDirectory,
    bool RecoveryPending,
    int AutomaticRetryCount)
{
    public const string CapturePurpose = "CaptureRecoveryOnly";
    public const string StitchOutcome = "Pending";
    public const string A0QualityApproval = "Unapproved";

    public bool Succeeded =>
        !RecoveryPending &&
        TerminalState == DualHardwareCaptureTerminalState.Succeeded &&
        FailureCode == DualCameraFailureCode.None &&
        Originals.Select(item => item.Alias).SequenceEqual(["CAM-A", "CAM-B"]);
}

public interface IHardwareDualCaptureRecoveryOnlyWorkflow
{
    string TransactionRoot { get; }

    bool CanStartNewCapture { get; }

    string NewCaptureBlocker { get; }

    bool HasPendingRecovery { get; }

    Guid? PendingTransactionId { get; }

    Task<HardwareDualCaptureRecoveryOnlyExecution> CaptureAsync(
        DualCameraIdentitySnapshot identitySnapshot,
        CancellationToken cancellationToken = default);

    Task<HardwareDualCaptureRecoveryOnlyExecution> RecoverAsync(
        CancellationToken cancellationToken = default);
}

/// <summary>
/// Strict loader for the human-approved external capture profile. This never
/// creates or upgrades an approval artifact and rejects additional JSON fields.
/// </summary>
internal static class HardwareDualCaptureRecoveryOnlyProfileFile
{
    private const long MaximumProfileBytes = 64 * 1024;
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        RespectRequiredConstructorParameters = true,
        MaxDepth = 8,
        Converters = { new JsonStringEnumConverter() },
    };

    public static HardwareDualCaptureRecoveryOnlyProfile Load(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new InvalidDataException("The approved CaptureRecoveryOnly profile input is invalid.");

        var normalized = Path.GetFullPath(path);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(normalized);
        var attributes = File.GetAttributes(normalized);
        if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint | FileAttributes.Device)) != 0)
            throw new InvalidDataException("The approved HardwareDual capture profile must be a regular local file.");

        using var stream = new FileStream(
            normalized,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 4096,
            FileOptions.SequentialScan);
        if (stream.Length is <= 0 or > MaximumProfileBytes)
            throw new InvalidDataException("The approved HardwareDual capture profile size is invalid.");

        var bytes = new byte[(int)stream.Length];
        stream.ReadExactly(bytes);
        RejectDuplicatePropertyNames(bytes, maxDepth: 8);
        try
        {
            var profile = JsonSerializer.Deserialize<HardwareDualCaptureRecoveryOnlyProfile>(bytes, JsonOptions)
                ?? throw new InvalidDataException("The approved CaptureRecoveryOnly profile is empty.");
            profile.Validate();
            return profile;
        }
        catch (JsonException exception)
        {
            throw new InvalidDataException("The approved CaptureRecoveryOnly profile is malformed.", exception);
        }
        catch (DualCameraFlowException exception)
        {
            throw new InvalidDataException("The approved CaptureRecoveryOnly profile is not approved for this operation.", exception);
        }
    }

    internal static void RejectDuplicatePropertyNames(ReadOnlySpan<byte> json, int maxDepth)
    {
        var reader = new Utf8JsonReader(json, new JsonReaderOptions
        {
            AllowTrailingCommas = false,
            CommentHandling = JsonCommentHandling.Disallow,
            MaxDepth = maxDepth,
        });
        var objects = new Stack<HashSet<string>>();
        while (reader.Read())
        {
            if (reader.TokenType == JsonTokenType.StartObject)
            {
                objects.Push(new HashSet<string>(StringComparer.Ordinal));
            }
            else if (reader.TokenType == JsonTokenType.PropertyName)
            {
                var name = reader.GetString() ?? throw new InvalidDataException("JSON property name is invalid.");
                if (objects.Count == 0 || !objects.Peek().Add(name))
                    throw new InvalidDataException($"JSON contains a duplicate property: {name}");
            }
            else if (reader.TokenType == JsonTokenType.EndObject)
            {
                if (objects.Count == 0)
                    throw new InvalidDataException("JSON object structure is invalid.");
                objects.Pop();
            }
        }
        if (objects.Count != 0)
            throw new InvalidDataException("JSON object structure is incomplete.");
    }
}

internal enum CaptureRecoveryOnlyRecoveryIntent
{
    QueryTerminal,
    CloseReservedBeforeDispatch,
}

internal sealed record CaptureRecoveryOnlyDurableSnapshot
{
    public required string SchemaVersion { get; init; }

    public required CaptureRecoveryOnlyRecoveryIntent RecoveryIntent { get; init; }

    public required DualHardwareCaptureRecoveryOnlyRequest? PendingRequest { get; init; }
}

/// <summary>
/// Separate durable state from the ordinary rig/stitch transaction snapshot. A
/// discriminator is therefore never inferred from nullable rig fields.
/// </summary>
internal sealed class CaptureRecoveryOnlyTransactionSnapshotStore
{
    private const string SchemaVersion = "a0.hardware-dual-capture-recovery-only-snapshot.v1";
    private const long MaximumStateBytes = 192 * 1024;
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        RespectRequiredConstructorParameters = true,
        WriteIndented = true,
        MaxDepth = 12,
        Converters = { new JsonStringEnumConverter() },
    };

    private readonly string _artifactRoot;
    private readonly string _stateDirectory;
    private readonly string _statePath;
    private readonly string _lockPath;

    public CaptureRecoveryOnlyTransactionSnapshotStore(string artifactRoot)
    {
        if (string.IsNullOrWhiteSpace(artifactRoot))
            throw new ArgumentException("A CaptureRecoveryOnly artifact root is required.", nameof(artifactRoot));
        _artifactRoot = Path.GetFullPath(artifactRoot);
        _stateDirectory = Path.Combine(_artifactRoot, "recovery-state");
        _statePath = Path.Combine(_stateDirectory, "pending-capture-recovery-only.json");
        _lockPath = Path.Combine(_stateDirectory, "pending-capture-recovery-only.lock");
    }

    public CaptureRecoveryOnlyDurableSnapshot? Load()
    {
        if (!Directory.Exists(_stateDirectory))
            return null;
        EnsureDirectoryIsSafe();
        using var stateLock = AcquireLock();
        return LoadCore();
    }

    public void Save(DualHardwareCaptureRecoveryOnlyRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateRequest(request);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_stateDirectory);
        Directory.CreateDirectory(_stateDirectory);
        EnsureDirectoryIsSafe();
        using var stateLock = AcquireLock();
        var existing = LoadCore()?.PendingRequest;
        if (existing is not null && existing != request)
            throw new InvalidOperationException("Another CaptureRecoveryOnly transaction is already pending.");
        Write(new CaptureRecoveryOnlyDurableSnapshot
        {
            SchemaVersion = SchemaVersion,
            RecoveryIntent = CaptureRecoveryOnlyRecoveryIntent.QueryTerminal,
            PendingRequest = request,
        });
    }

    public void MarkCloseReservedBeforeDispatch(Guid expectedTransactionId)
    {
        EnsureDirectoryIsSafe();
        using var stateLock = AcquireLock();
        var state = LoadCore();
        if (state?.PendingRequest?.TransactionId != expectedTransactionId)
            throw new InvalidOperationException("The expected CaptureRecoveryOnly transaction is no longer pending.");
        Write(state with { RecoveryIntent = CaptureRecoveryOnlyRecoveryIntent.CloseReservedBeforeDispatch });
    }

    public void Clear(Guid expectedTransactionId)
    {
        EnsureDirectoryIsSafe();
        using var stateLock = AcquireLock();
        var state = LoadCore();
        if (state?.PendingRequest?.TransactionId != expectedTransactionId)
            throw new InvalidOperationException("The expected CaptureRecoveryOnly transaction is no longer pending.");
        Write(new CaptureRecoveryOnlyDurableSnapshot
        {
            SchemaVersion = SchemaVersion,
            RecoveryIntent = CaptureRecoveryOnlyRecoveryIntent.QueryTerminal,
            PendingRequest = null,
        });
    }

    private CaptureRecoveryOnlyDurableSnapshot? LoadCore()
    {
        if (!File.Exists(_statePath))
            return null;
        EnsureRegularFile(_statePath);
        using var stream = new FileStream(
            _statePath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 4096,
            FileOptions.SequentialScan);
        if (stream.Length is <= 0 or > MaximumStateBytes)
            throw new InvalidDataException("CaptureRecoveryOnly recovery snapshot size is invalid.");
        var bytes = new byte[(int)stream.Length];
        stream.ReadExactly(bytes);
        HardwareDualCaptureRecoveryOnlyProfileFile.RejectDuplicatePropertyNames(bytes, maxDepth: 12);
        try
        {
            var state = JsonSerializer.Deserialize<CaptureRecoveryOnlyDurableSnapshot>(bytes, JsonOptions)
                ?? throw new InvalidDataException("CaptureRecoveryOnly recovery snapshot is empty.");
            ValidateState(state);
            return state;
        }
        catch (JsonException exception)
        {
            throw new InvalidDataException("CaptureRecoveryOnly recovery snapshot is malformed.", exception);
        }
        catch (DualCameraFlowException exception)
        {
            throw new InvalidDataException("CaptureRecoveryOnly recovery snapshot contains an invalid approval profile.", exception);
        }
    }

    private void Write(CaptureRecoveryOnlyDurableSnapshot state)
    {
        ValidateState(state);
        var partial = Path.Combine(_stateDirectory, $"pending-capture-recovery-only-{Guid.NewGuid():N}.partial");
        try
        {
            using (var stream = new FileStream(
                       partial,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       bufferSize: 4096,
                       FileOptions.WriteThrough))
            {
                JsonSerializer.Serialize(stream, state, JsonOptions);
                stream.Flush(flushToDisk: true);
            }
            WindowsDurableFilePublisher.Publish(partial, _statePath, replaceExisting: true);
        }
        catch
        {
            if (File.Exists(partial))
                File.Delete(partial);
            throw;
        }
    }

    private void ValidateState(CaptureRecoveryOnlyDurableSnapshot state)
    {
        if (!string.Equals(state.SchemaVersion, SchemaVersion, StringComparison.Ordinal) ||
            !Enum.IsDefined(state.RecoveryIntent) ||
            (state.PendingRequest is null &&
             state.RecoveryIntent != CaptureRecoveryOnlyRecoveryIntent.QueryTerminal))
            throw new InvalidDataException("CaptureRecoveryOnly recovery snapshot state is invalid.");
        if (state.PendingRequest is not null)
            ValidateRequest(state.PendingRequest);
    }

    private void ValidateRequest(DualHardwareCaptureRecoveryOnlyRequest request)
    {
        if (request.TransactionId == Guid.Empty || !request.IdentitySnapshot.IsReady ||
            request.OperatorConfirmations.AllConfirmed != true ||
            request.StartedAtUtc.Offset != TimeSpan.Zero || request.WatchdogDeadlineUtc.Offset != TimeSpan.Zero ||
            request.WatchdogDeadlineUtc - request.StartedAtUtc != TimeSpan.FromSeconds(180))
            throw new InvalidDataException("CaptureRecoveryOnly recovery request is incomplete or unsafe.");
        request.CaptureProfileSnapshot.Validate();
        var expectedDirectory = Path.GetFullPath(Path.Combine(
            _artifactRoot,
            "transactions",
            request.TransactionId.ToString("N")));
        if (!string.Equals(
                Path.GetFullPath(request.TransactionDirectory),
                expectedDirectory,
                StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("CaptureRecoveryOnly transaction directory is outside the artifact root.");
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(expectedDirectory);
    }

    private FileStream AcquireLock()
    {
        if (File.Exists(_lockPath))
            EnsureRegularFile(_lockPath);
        try
        {
            return new FileStream(
                _lockPath,
                FileMode.OpenOrCreate,
                FileAccess.ReadWrite,
                FileShare.None,
                bufferSize: 1,
                FileOptions.WriteThrough);
        }
        catch (IOException exception)
        {
            throw new InvalidOperationException("CaptureRecoveryOnly recovery snapshot is locked.", exception);
        }
    }

    private void EnsureDirectoryIsSafe()
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_stateDirectory);
        if ((File.GetAttributes(_stateDirectory) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException("CaptureRecoveryOnly recovery directory cannot be a reparse point.");
    }

    private static void EnsureRegularFile(string path)
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(path);
        if ((File.GetAttributes(path) & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
            throw new InvalidDataException("CaptureRecoveryOnly recovery snapshot must be a regular file.");
    }
}

/// <summary>
/// Executes only capture/recovery. It never invokes the M2 stitcher, never retries
/// Reserve or Start, and on an ambiguous response can only query the same ID.
/// </summary>
internal sealed class HardwareDualCaptureRecoveryOnlyWorkflow : IHardwareDualCaptureRecoveryOnlyWorkflow
{
    private static readonly TimeSpan Watchdog = TimeSpan.FromSeconds(180);
    private static readonly IReadOnlyList<string> OrderedAliases = Array.AsReadOnly(["CAM-A", "CAM-B"]);

    private readonly string _artifactRoot;
    private readonly IDualHardwareCaptureOperations _pairOperations;
    private readonly IDualHardwareCaptureRecoveryOnlyOperations _captureOperations;
    private readonly HardwareDualCaptureRecoveryOnlyProfile _captureProfile;
    private readonly CaptureRecoveryOnlyTransactionSnapshotStore _store;
    private readonly TimeProvider _timeProvider;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private CaptureRecoveryOnlyDurableSnapshot? _pending;

    public HardwareDualCaptureRecoveryOnlyWorkflow(
        string artifactRoot,
        IDualHardwareCaptureOperations pairOperations,
        IDualHardwareCaptureRecoveryOnlyOperations captureOperations,
        HardwareDualCaptureRecoveryOnlyProfile captureProfile,
        TimeProvider? timeProvider = null,
        CaptureRecoveryOnlyTransactionSnapshotStore? store = null)
    {
        if (string.IsNullOrWhiteSpace(artifactRoot))
            throw new ArgumentException("A CaptureRecoveryOnly artifact root is required.", nameof(artifactRoot));
        _artifactRoot = Path.GetFullPath(artifactRoot);
        _pairOperations = pairOperations ?? throw new ArgumentNullException(nameof(pairOperations));
        _captureOperations = captureOperations ?? throw new ArgumentNullException(nameof(captureOperations));
        _captureProfile = captureProfile ?? throw new ArgumentNullException(nameof(captureProfile));
        _timeProvider = timeProvider ?? TimeProvider.System;
        _store = store ?? new CaptureRecoveryOnlyTransactionSnapshotStore(_artifactRoot);
        _pending = _store.Load();
    }

    public bool HasPendingRecovery => _pending?.PendingRequest is not null;

    public string TransactionRoot => Path.Combine(_artifactRoot, "transactions");

    public Guid? PendingTransactionId => _pending?.PendingRequest?.TransactionId;

    public bool CanStartNewCapture
    {
        get
        {
            if (HasPendingRecovery)
                return false;
            try
            {
                _captureProfile.Validate();
                return true;
            }
            catch (DualCameraFlowException)
            {
                return false;
            }
        }
    }

    public string NewCaptureBlocker => HasPendingRecovery
        ? "A prior CaptureRecoveryOnly transaction requires same-ID recovery."
        : CanStartNewCapture
            ? string.Empty
            : "The approved CaptureRecoveryOnly profile is invalid.";

    public async Task<HardwareDualCaptureRecoveryOnlyExecution> CaptureAsync(
        DualCameraIdentitySnapshot identitySnapshot,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(identitySnapshot);
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (HasPendingRecovery)
                return Pending("A prior CaptureRecoveryOnly result remains unknown; only same-ID recovery is allowed.");

            var startedAtUtc = _timeProvider.GetUtcNow();
            var evaluatedIdentity = identitySnapshot.EvaluateAt(startedAtUtc);
            if (!evaluatedIdentity.IsReady || evaluatedIdentity.ObservedAtUtc > startedAtUtc ||
                evaluatedIdentity.ExpiresAtUtc <= startedAtUtc)
                return Failed(
                    Guid.Empty,
                    DualHardwareCaptureTerminalState.HardwarePending,
                    DualCameraFailureCode.IdentityNotReady,
                    "The current same-session CAM-A/CAM-B binding is not Ready.");
            _captureProfile.Validate();

            var transactionId = Guid.NewGuid();
            var transactionDirectory = Path.GetFullPath(Path.Combine(
                _artifactRoot,
                "transactions",
                transactionId.ToString("N")));
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(transactionDirectory);
            var request = new DualHardwareCaptureRecoveryOnlyRequest(
                transactionId,
                transactionDirectory,
                evaluatedIdentity with { },
                _captureProfile with { },
                new HardwareDualCaptureRecoveryOnlyConfirmations(true, true, true, true, true),
                startedAtUtc,
                startedAtUtc + Watchdog);

            try
            {
                await _captureOperations
                    .EnsureCaptureRecoveryOnlyAvailableAsync(cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                return Failed(
                    transactionId,
                    DualHardwareCaptureTerminalState.HardwarePending,
                    DualCameraFailureCode.HardwarePending,
                    $"CaptureRecoveryOnly capability preflight failed before reservation: {exception.Message}",
                    transactionDirectory);
            }

            _pending = new CaptureRecoveryOnlyDurableSnapshot
            {
                SchemaVersion = "a0.hardware-dual-capture-recovery-only-snapshot.v1",
                RecoveryIntent = CaptureRecoveryOnlyRecoveryIntent.QueryTerminal,
                PendingRequest = request,
            };
            try
            {
                _store.Save(request);
                _pending = _store.Load();
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                return Pending(
                    $"The frozen CaptureRecoveryOnly request could not be persisted before reservation; " +
                    $"no Agent operation was sent: {exception.Message}");
            }

            try
            {
                if (!await _pairOperations.ReservePairTransactionAsync(transactionId, cancellationToken)
                        .ConfigureAwait(false))
                    return Pending(
                        "The one-time pair reservation was not accepted; only the same-ID state may be queried.");
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                return Pending($"The one-time pair reservation outcome is unknown: {exception.Message}");
            }

            DualHardwareCaptureRecoveryOnlyDispatchResult dispatch;
            try
            {
                dispatch = await _captureOperations
                    .StartReservedCaptureRecoveryOnlyAsync(request, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                return Pending($"CaptureRecoveryOnly dispatch outcome is unknown: {exception.Message}");
            }

            if (dispatch.State == DualHardwareDispatchState.ConfirmedUndispatched)
                return await CloseConfirmedUndispatchedAsync(
                        request,
                        "CaptureRecoveryOnly was confirmed undispatched.",
                        cancellationToken)
                    .ConfigureAwait(false);
            if (dispatch.State == DualHardwareDispatchState.Completed && dispatch.Result is not null)
                return await CompleteAsync(request, dispatch.Result, recovered: false, cancellationToken)
                    .ConfigureAwait(false);

            return await QueryPendingAsync(request, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }

    public async Task<HardwareDualCaptureRecoveryOnlyExecution> RecoverAsync(
        CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var state = _pending ?? _store.Load();
            _pending = state;
            if (state?.PendingRequest is not { } request)
                return Failed(
                    Guid.Empty,
                    DualHardwareCaptureTerminalState.HardwarePending,
                    DualCameraFailureCode.HardwarePending,
                    "No CaptureRecoveryOnly transaction is pending.");
            if (state.RecoveryIntent == CaptureRecoveryOnlyRecoveryIntent.CloseReservedBeforeDispatch)
                return await CloseConfirmedUndispatchedAsync(
                        request,
                        "The exact reserved transaction still requires close confirmation.",
                        cancellationToken)
                    .ConfigureAwait(false);
            return await QueryPendingAsync(request, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }

    private async Task<HardwareDualCaptureRecoveryOnlyExecution> QueryPendingAsync(
        DualHardwareCaptureRecoveryOnlyRequest request,
        CancellationToken cancellationToken)
    {
        try
        {
            var query = await _captureOperations
                .QueryCaptureRecoveryOnlyTransactionAsync(request.TransactionId, cancellationToken)
                .ConfigureAwait(false);
            if (query.State == DualHardwarePairQueryState.Terminal && query.Result is not null)
                return await CompleteAsync(request, query.Result, recovered: true, cancellationToken)
                    .ConfigureAwait(false);
            if (query.State == DualHardwarePairQueryState.Reserved)
                return await CloseConfirmedUndispatchedAsync(
                        request,
                        "The same-ID query confirmed that CaptureRecoveryOnly was not dispatched.",
                        cancellationToken)
                    .ConfigureAwait(false);
            if (query.State == DualHardwarePairQueryState.ClosedBeforeDispatch)
            {
                if (!TryClear(request.TransactionId, out var clearFailure))
                    return Pending(clearFailure!);
                return Failed(
                    request.TransactionId,
                    DualHardwareCaptureTerminalState.HardwarePending,
                    DualCameraFailureCode.HardwarePending,
                    "The exact reservation was already closed before dispatch; no capture occurred.",
                    request.TransactionDirectory);
            }
            if (query.State == DualHardwarePairQueryState.NotFound)
            {
                if (!TryClear(request.TransactionId, out var clearFailure))
                    return Pending(clearFailure!);
                return Failed(
                    request.TransactionId,
                    DualHardwareCaptureTerminalState.HardwarePending,
                    DualCameraFailureCode.HardwarePending,
                    "The same-ID journal was not found; no reservation or capture was recorded.",
                    request.TransactionDirectory);
            }
            return Pending("The same transaction is not terminal; Reserve and Start were not sent again.");
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            return Pending($"The same-ID CaptureRecoveryOnly query remains unknown: {exception.Message}");
        }
    }

    private async Task<HardwareDualCaptureRecoveryOnlyExecution> CompleteAsync(
        DualHardwareCaptureRecoveryOnlyRequest request,
        DualHardwareCaptureRecoveryOnlyResult result,
        bool recovered,
        CancellationToken cancellationToken)
    {
        var verified = new List<CanonicalJpegOriginal>(2);
        try
        {
            ValidateTerminal(request, result);
            foreach (var original in result.Originals)
            {
                var expected = Path.GetFullPath(Path.Combine(
                    request.TransactionDirectory,
                    original.Alias,
                    "original.jpg"));
                WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(expected);
                await using var stream = HardwareArtifactVerifier.OpenStableRead(expected);
                var inspected = await HardwareArtifactVerifier.InspectJpegAsync(
                        stream,
                        cancellationToken,
                        (7360, 4912))
                    .ConfigureAwait(false);
                verified.Add(new CanonicalJpegOriginal(
                    original.Alias,
                    expected,
                    inspected.SizeBytes,
                    inspected.Sha256,
                    7360,
                    4912,
                    true));
            }
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            return new(
                request.TransactionId,
                DualHardwareCaptureTerminalState.FailedPartial,
                DualCameraFailureCode.InvalidOriginal,
                recovered
                    ? $"Recovered terminal evidence did not match the durable request: {exception.Message}"
                    : $"CaptureRecoveryOnly terminal evidence or original verification failed: {exception.Message}",
                new ReadOnlyCollection<CanonicalJpegOriginal>(verified),
                request.TransactionDirectory,
                // A malformed or unverifiable terminal must never release the
                // gate for a new shutter. The only legal follow-up is a same-ID
                // query, even when the bad evidence arrived on the first reply.
                RecoveryPending: true,
                result.Evidence.AutomaticRetryCount);
        }

        if (!TryClear(request.TransactionId, out var clearFailure))
            return new(
                request.TransactionId,
                result.TerminalState,
                DualCameraFailureCode.AgentResponseUnknown,
                clearFailure,
                new ReadOnlyCollection<CanonicalJpegOriginal>(verified),
                request.TransactionDirectory,
                RecoveryPending: true,
                result.Evidence.AutomaticRetryCount);

        return new(
            request.TransactionId,
            result.TerminalState,
            result.FailureCode,
            result.FailureCode == DualCameraFailureCode.None
                ? null
                : $"CaptureRecoveryOnly ended with {result.FailureCode}.",
            new ReadOnlyCollection<CanonicalJpegOriginal>(verified),
            request.TransactionDirectory,
            RecoveryPending: false,
            result.Evidence.AutomaticRetryCount);
    }

    private async Task<HardwareDualCaptureRecoveryOnlyExecution> CloseConfirmedUndispatchedAsync(
        DualHardwareCaptureRecoveryOnlyRequest request,
        string reason,
        CancellationToken cancellationToken)
    {
        try
        {
            _store.MarkCloseReservedBeforeDispatch(request.TransactionId);
            _pending = _store.Load();
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            return Pending($"The exact reserved close intent could not be persisted: {exception.Message}");
        }

        var close = await TryCloseAsync(request.TransactionId, cancellationToken).ConfigureAwait(false);
        if (close != DualHardwareCloseState.ClosedBeforeDispatch)
            return Pending("The exact reserved transaction close remains unconfirmed; no new transaction is allowed.");
        if (!TryClear(request.TransactionId, out var clearFailure))
            return Pending(clearFailure!);
        return Failed(
            request.TransactionId,
            DualHardwareCaptureTerminalState.HardwarePending,
            DualCameraFailureCode.HardwarePending,
            reason + " The exact reservation was closed without capture.",
            request.TransactionDirectory);
    }

    private async Task<DualHardwareCloseState> TryCloseAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        try
        {
            return await _pairOperations
                .CloseReservedPairTransactionAsync(transactionId, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception) when (!cancellationToken.IsCancellationRequested)
        {
            return DualHardwareCloseState.ResponseUnknown;
        }
    }

    private void ValidateTerminal(
        DualHardwareCaptureRecoveryOnlyRequest request,
        DualHardwareCaptureRecoveryOnlyResult result)
    {
        if (result.TransactionId != request.TransactionId ||
            result.CapturePurpose != HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose ||
            result.StitchOutcome != HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome ||
            result.A0QualityApproval != HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval ||
            result.Evidence.TerminalState != result.TerminalState ||
            result.Evidence.IdentitySnapshot != request.IdentitySnapshot ||
            result.Evidence.CaptureProfileSchemaVersion != request.CaptureProfileSnapshot.SchemaVersion ||
            result.Evidence.CaptureProfileApprovalBasis != request.CaptureProfileSnapshot.ApprovalBasis ||
            result.Evidence.CameraModel != request.CaptureProfileSnapshot.CameraModel ||
            result.Evidence.ImageFormat != request.CaptureProfileSnapshot.ImageFormat ||
            result.Evidence.ImageSize != request.CaptureProfileSnapshot.ImageSize ||
            result.Evidence.PixelDimensions != request.CaptureProfileSnapshot.PixelDimensions ||
            result.Evidence.WatchdogStartedAtUtc != request.StartedAtUtc ||
            result.Evidence.WatchdogDeadlineUtc != request.WatchdogDeadlineUtc ||
            result.Evidence.WatchdogDeadlineUtc - result.Evidence.WatchdogStartedAtUtc != Watchdog ||
            result.Evidence.AutomaticRetryCount != 0 ||
            !result.Evidence.LiveViewStopAndCloseConfirmed ||
            result.Evidence.CompletedAtUtc > result.Evidence.WatchdogDeadlineUtc ||
            (result.TerminalState == DualHardwareCaptureTerminalState.Succeeded) !=
                (result.FailureCode == DualCameraFailureCode.None))
            throw new InvalidDataException("CaptureRecoveryOnly terminal evidence does not match the frozen request.");

        if (result.TerminalState == DualHardwareCaptureTerminalState.WatchdogExpired ||
            !result.Evidence.WatchdogCompletedInTime)
            throw new InvalidDataException("CaptureRecoveryOnly watchdog expired.");
        if (result.Originals.Select(item => item.Alias).Distinct(StringComparer.Ordinal).Count() !=
            result.Originals.Count ||
            result.Originals.Any(item => item.Alias is not ("CAM-A" or "CAM-B")) ||
            !result.Originals.Select(item => item.Alias).SequenceEqual(
                OrderedAliases.Take(result.Originals.Count),
                StringComparer.Ordinal))
            throw new InvalidDataException("CaptureRecoveryOnly returned an invalid original order or alias set.");

        foreach (var original in result.Originals)
        {
            var expected = Path.GetFullPath(Path.Combine(
                request.TransactionDirectory,
                original.Alias,
                "original.jpg"));
            if (!string.Equals(
                    Path.GetFullPath(original.CanonicalOriginalPath),
                    expected,
                    StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("CaptureRecoveryOnly returned an original outside its transaction directory.");
        }

        if (result.TerminalState == DualHardwareCaptureTerminalState.Succeeded &&
            (result.Originals.Count != 2 ||
             result.Originals.Any(item => !item.ExactRecoveredObjectDeleted || !item.SpoolEmptyAfterDelete) ||
             !result.Evidence.ExactDeleteConfirmedForEveryRetainedOriginal ||
             !result.Evidence.BothSpoolsEmptyAfter))
            throw new InvalidDataException("Successful CaptureRecoveryOnly cleanup evidence is incomplete.");
    }

    private bool TryClear(Guid transactionId, out string? failureReason)
    {
        try
        {
            _store.Clear(transactionId);
            _pending = _store.Load();
            failureReason = null;
            return true;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            failureReason = $"Validated terminal state could not clear the durable CaptureRecoveryOnly snapshot: {exception.Message}";
            return false;
        }
    }

    private HardwareDualCaptureRecoveryOnlyExecution Pending(string reason)
    {
        var request = _pending?.PendingRequest;
        return new(
            request?.TransactionId ?? Guid.Empty,
            DualHardwareCaptureTerminalState.ResponseUnknown,
            DualCameraFailureCode.AgentResponseUnknown,
            reason,
            Array.Empty<CanonicalJpegOriginal>(),
            request?.TransactionDirectory ?? string.Empty,
            RecoveryPending: true,
            AutomaticRetryCount: 0);
    }

    private static HardwareDualCaptureRecoveryOnlyExecution Failed(
        Guid transactionId,
        DualHardwareCaptureTerminalState terminalState,
        DualCameraFailureCode failureCode,
        string reason,
        string transactionDirectory = "") =>
        new(
            transactionId,
            terminalState,
            failureCode,
            reason,
            Array.Empty<CanonicalJpegOriginal>(),
            transactionDirectory,
            RecoveryPending: terminalState == DualHardwareCaptureTerminalState.ResponseUnknown,
            AutomaticRetryCount: 0);
}
