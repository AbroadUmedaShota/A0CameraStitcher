using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using A0CameraStitcher.M3.Foundation.DualCamera;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal static class HardwareDualTransactionSnapshotProtocol
{
    public const string SchemaVersion = "a0.hardware-dual-transaction-snapshot.v3";
    public const string PreviousSchemaVersion = "a0.hardware-dual-transaction-snapshot.v2";
    public const string LegacySchemaVersion = "a0.hardware-dual-transaction-snapshot.v1";
}

internal sealed record HardwareDualDurableSnapshot
{
    public required string SchemaVersion { get; init; }

    public required DualHardwareRecoveryIntent RecoveryIntent { get; init; }

    public required DualHardwareCaptureRequest? PendingRequest { get; init; }
}

internal sealed record HardwareDualDurableSnapshotV1
{
    public required string SchemaVersion { get; init; }

    public required bool DispatchMayHaveOccurred { get; init; }

    public required DualHardwareCaptureRequest? PendingRequest { get; init; }
}

internal sealed class HardwareDualTransactionSnapshotStore : IDualHardwareRecoveryStore
{
    private const long MaximumStateBytes = 256 * 1024;
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        RespectRequiredConstructorParameters = true,
        WriteIndented = true,
        MaxDepth = 16,
        Converters = { new JsonStringEnumConverter() },
    };

    private readonly string _artifactRoot;
    private readonly string _stateDirectory;
    private readonly string _statePath;
    private readonly string _lockPath;

    public HardwareDualTransactionSnapshotStore(string artifactRoot)
    {
        if (string.IsNullOrWhiteSpace(artifactRoot))
            throw new ArgumentException("A HardwareDual artifact root is required.", nameof(artifactRoot));
        _artifactRoot = Path.GetFullPath(artifactRoot);
        _stateDirectory = Path.Combine(_artifactRoot, "recovery-state");
        _statePath = Path.Combine(_stateDirectory, "pending-transaction.json");
        _lockPath = Path.Combine(_stateDirectory, "pending-transaction.lock");
    }

    public DualHardwareCaptureRequest? LoadPending()
    {
        try
        {
            EnsureDirectoryIsSafe();
        }
        catch (Exception exception) when (
            exception is FileNotFoundException or DirectoryNotFoundException)
        {
            return null;
        }
        using var stateLock = AcquireStateLock();
        return LoadCore()?.PendingRequest;
    }

    public void SavePending(DualHardwareCaptureRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateRequest(request);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_stateDirectory);
        Directory.CreateDirectory(_stateDirectory);
        EnsureDirectoryIsSafe();
        using var stateLock = AcquireStateLock();
        var existing = LoadCore()?.PendingRequest;
        if (existing is not null && existing != request)
            throw new InvalidOperationException("Another HardwareDual transaction snapshot is already pending.");
        WriteState(new HardwareDualDurableSnapshot
        {
            SchemaVersion = HardwareDualTransactionSnapshotProtocol.SchemaVersion,
            RecoveryIntent = DualHardwareRecoveryIntent.ReservationOutcomeUnknown,
            PendingRequest = request,
        });
    }

    public void MarkMayHaveDispatched(Guid expectedTransactionId)
    {
        if (expectedTransactionId == Guid.Empty)
            throw new InvalidDataException("The expected HardwareDual transaction ID is invalid.");
        EnsureDirectoryIsSafe();
        using var stateLock = AcquireStateLock();
        var state = LoadCore();
        if (state?.PendingRequest?.TransactionId != expectedTransactionId ||
            state.RecoveryIntent != DualHardwareRecoveryIntent.ReservationOutcomeUnknown)
            throw new InvalidOperationException("The expected reservation-unknown HardwareDual transaction is no longer pending.");
        WriteState(state with
        {
            SchemaVersion = HardwareDualTransactionSnapshotProtocol.SchemaVersion,
            RecoveryIntent = DualHardwareRecoveryIntent.MayHaveDispatched,
        });
    }

    public DualHardwareRecoveryIntent LoadPendingIntent(Guid expectedTransactionId)
    {
        if (expectedTransactionId == Guid.Empty)
            throw new InvalidDataException("The expected HardwareDual transaction ID is invalid.");
        try
        {
            EnsureDirectoryIsSafe();
        }
        catch (Exception exception) when (
            exception is FileNotFoundException or DirectoryNotFoundException)
        {
            throw new InvalidOperationException("No HardwareDual transaction snapshot is pending.", exception);
        }
        using var stateLock = AcquireStateLock();
        var state = LoadCore();
        if (state?.PendingRequest?.TransactionId != expectedTransactionId)
            throw new InvalidOperationException("The expected HardwareDual transaction is no longer pending.");
        return state.RecoveryIntent;
    }

    public void MarkCloseReservedBeforeDispatch(Guid expectedTransactionId)
    {
        if (expectedTransactionId == Guid.Empty)
            throw new InvalidDataException("The expected HardwareDual transaction ID is invalid.");
        EnsureDirectoryIsSafe();
        using var stateLock = AcquireStateLock();
        var state = LoadCore();
        if (state?.PendingRequest?.TransactionId != expectedTransactionId)
            throw new InvalidOperationException("The expected HardwareDual transaction is no longer pending.");
        WriteState(state with
        {
            SchemaVersion = HardwareDualTransactionSnapshotProtocol.SchemaVersion,
            RecoveryIntent = DualHardwareRecoveryIntent.CloseReservedBeforeDispatch,
        });
    }

    public void ClearPending(Guid expectedTransactionId)
    {
        if (expectedTransactionId == Guid.Empty)
            throw new InvalidDataException("The expected HardwareDual transaction ID is invalid.");
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_stateDirectory);
        Directory.CreateDirectory(_stateDirectory);
        EnsureDirectoryIsSafe();
        using var stateLock = AcquireStateLock();
        var existing = LoadCore()?.PendingRequest;
        if (existing?.TransactionId != expectedTransactionId)
            throw new InvalidOperationException("The expected HardwareDual transaction is no longer the durable pending transaction.");
        WriteState(new HardwareDualDurableSnapshot
        {
            SchemaVersion = HardwareDualTransactionSnapshotProtocol.SchemaVersion,
            RecoveryIntent = DualHardwareRecoveryIntent.MayHaveDispatched,
            PendingRequest = null,
        });
    }

    private HardwareDualDurableSnapshot? LoadCore()
    {
        try
        {
            EnsureRegularStateFile(_statePath);
        }
        catch (Exception exception) when (
            exception is FileNotFoundException or DirectoryNotFoundException)
        {
            return null;
        }
        // Check the size against the stream before allocating/reading, not
        // after (File.ReadAllBytes would otherwise read an oversized file in
        // full first, which the ViewModel's catch filters do not treat as a
        // recoverable OutOfMemoryException). Mirrors HardwareSingleAppState-
        // Store.LoadPendingAsync's stream.Length-first check.
        using var stream = new FileStream(
            _statePath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 4096,
            FileOptions.SequentialScan);
        if (stream.Length is <= 0 or > MaximumStateBytes)
            throw new InvalidDataException("HardwareDual transaction snapshot size is invalid.");
        var bytes = new byte[(int)stream.Length];
        stream.ReadExactly(bytes);
        try
        {
            RejectDuplicatePropertyNames(bytes);
            using var document = JsonDocument.Parse(bytes);
            if (!document.RootElement.TryGetProperty("schemaVersion", out var schemaElement) ||
                schemaElement.ValueKind != JsonValueKind.String)
                throw new InvalidDataException("HardwareDual transaction snapshot schema is missing.");
            var schemaVersion = schemaElement.GetString();
            HardwareDualDurableSnapshot state;
            if (string.Equals(
                    schemaVersion,
                    HardwareDualTransactionSnapshotProtocol.SchemaVersion,
                    StringComparison.Ordinal))
            {
                state = JsonSerializer.Deserialize<HardwareDualDurableSnapshot>(bytes, JsonOptions)
                    ?? throw new InvalidDataException("HardwareDual transaction snapshot is empty.");
            }
            else if (string.Equals(
                         schemaVersion,
                         HardwareDualTransactionSnapshotProtocol.PreviousSchemaVersion,
                         StringComparison.Ordinal))
            {
                var previous = JsonSerializer.Deserialize<HardwareDualDurableSnapshot>(bytes, JsonOptions)
                    ?? throw new InvalidDataException("Previous HardwareDual transaction snapshot is empty.");
                if (!Enum.IsDefined(previous.RecoveryIntent) ||
                    previous.RecoveryIntent == DualHardwareRecoveryIntent.ReservationOutcomeUnknown ||
                    (previous.PendingRequest is null &&
                     previous.RecoveryIntent != DualHardwareRecoveryIntent.MayHaveDispatched))
                    throw new InvalidDataException("Previous HardwareDual transaction snapshot dispatch state is invalid.");
                state = previous with
                {
                    SchemaVersion = HardwareDualTransactionSnapshotProtocol.SchemaVersion,
                };
            }
            else if (string.Equals(
                         schemaVersion,
                         HardwareDualTransactionSnapshotProtocol.LegacySchemaVersion,
                         StringComparison.Ordinal))
            {
                var legacy = JsonSerializer.Deserialize<HardwareDualDurableSnapshotV1>(bytes, JsonOptions)
                    ?? throw new InvalidDataException("Legacy HardwareDual transaction snapshot is empty.");
                if (legacy.DispatchMayHaveOccurred != (legacy.PendingRequest is not null))
                    throw new InvalidDataException("Legacy HardwareDual transaction snapshot dispatch state is invalid.");
                state = new HardwareDualDurableSnapshot
                {
                    SchemaVersion = HardwareDualTransactionSnapshotProtocol.SchemaVersion,
                    RecoveryIntent = DualHardwareRecoveryIntent.MayHaveDispatched,
                    PendingRequest = legacy.PendingRequest,
                };
            }
            else
            {
                throw new InvalidDataException("HardwareDual transaction snapshot schema is unsupported.");
            }
            ValidateState(state);
            return state;
        }
        catch (JsonException exception)
        {
            throw new InvalidDataException("HardwareDual transaction snapshot is malformed.", exception);
        }
    }

    private void WriteState(HardwareDualDurableSnapshot state)
    {
        ValidateState(state);
        var partialPath = Path.Combine(_stateDirectory, $"pending-transaction-{Guid.NewGuid():N}.partial");
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
                JsonSerializer.Serialize(stream, state, JsonOptions);
                stream.Flush(flushToDisk: true);
            }
            WindowsDurableFilePublisher.Publish(partialPath, _statePath, replaceExisting: true);
        }
        catch
        {
            if (File.Exists(partialPath)) File.Delete(partialPath);
            throw;
        }
    }

    private void ValidateState(HardwareDualDurableSnapshot state)
    {
        if (!string.Equals(state.SchemaVersion, HardwareDualTransactionSnapshotProtocol.SchemaVersion, StringComparison.Ordinal) ||
            (state.PendingRequest is null &&
             state.RecoveryIntent != DualHardwareRecoveryIntent.MayHaveDispatched) ||
            !Enum.IsDefined(state.RecoveryIntent))
            throw new InvalidDataException("HardwareDual transaction snapshot schema or dispatch state is invalid.");
        if (state.PendingRequest is not null) ValidateRequest(state.PendingRequest);
    }

    private void ValidateRequest(DualHardwareCaptureRequest request)
    {
        if (request.TransactionId == Guid.Empty || !request.IdentitySnapshot.IsReady ||
            request.StartedAtUtc.Offset != TimeSpan.Zero || request.WatchdogDeadlineUtc.Offset != TimeSpan.Zero ||
            request.WatchdogDeadlineUtc - request.StartedAtUtc != TimeSpan.FromSeconds(180) ||
            request.OperatorConfirmations.AllConfirmed != true)
            throw new InvalidDataException("The durable HardwareDual request is incomplete or unsafe.");
        request.CaptureProfileSnapshot.Validate(request.StartedAtUtc);
        request.RigProfileSnapshot.Validate(request.StartedAtUtc);
        var expectedDirectory = Path.GetFullPath(Path.Combine(
            _artifactRoot,
            "transactions",
            request.TransactionId.ToString("N")));
        if (!string.Equals(Path.GetFullPath(request.TransactionDirectory), expectedDirectory, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The durable HardwareDual transaction directory is outside the product artifact root.");
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(expectedDirectory);
    }

    private FileStream AcquireStateLock()
    {
        if (File.Exists(_lockPath)) EnsureRegularStateFile(_lockPath);
        try
        {
            return new FileStream(_lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 1, FileOptions.WriteThrough);
        }
        catch (IOException exception)
        {
            throw new InvalidOperationException("HardwareDual transaction snapshot is locked by another process.", exception);
        }
    }

    private void EnsureDirectoryIsSafe()
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_stateDirectory);
        if ((File.GetAttributes(_stateDirectory) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException("HardwareDual transaction snapshot directory cannot be a reparse point.");
    }

    private static void EnsureRegularStateFile(string path)
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(path);
        if ((File.GetAttributes(path) & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
            throw new InvalidDataException("HardwareDual transaction snapshot must be a regular local file.");
    }

    private static void RejectDuplicatePropertyNames(ReadOnlySpan<byte> json)
    {
        var reader = new Utf8JsonReader(json, new JsonReaderOptions
        {
            AllowTrailingCommas = false,
            CommentHandling = JsonCommentHandling.Disallow,
            MaxDepth = 16,
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
                var name = reader.GetString() ?? throw new InvalidDataException("Snapshot property name is invalid.");
                if (objects.Count == 0 || !objects.Peek().Add(name))
                    throw new InvalidDataException($"HardwareDual transaction snapshot contains a duplicate property: {name}");
            }
            else if (reader.TokenType == JsonTokenType.EndObject)
            {
                if (objects.Count == 0) throw new InvalidDataException("Snapshot object structure is invalid.");
                objects.Pop();
            }
        }
        if (objects.Count != 0) throw new InvalidDataException("Snapshot object structure is incomplete.");
    }
}
