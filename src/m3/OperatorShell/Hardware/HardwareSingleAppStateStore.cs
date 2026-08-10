using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

public static class HardwareSingleAppStateProtocol
{
    public const string SchemaVersion = "a0.hardware-single-app-state.v2";
}

public sealed record HardwarePendingTransaction
{
    public required string OperatingMode { get; init; }

    public required string TransactionId { get; init; }

    public required string CameraAlias { get; init; }

    public required string RequiredCameraAlias { get; init; }

    public required string CaptureProfileId { get; init; }

    public required uint CaptureProfileVersion { get; init; }

    public required string CaptureProfileSha256 { get; init; }

    public required DateTimeOffset CaptureProfileExpiresAtUtc { get; init; }

    public required bool LiveViewHandoffRequested { get; init; }

    public required bool CaptureRequestDispatchAttempted { get; init; }

    public required DateTimeOffset StartedAtUtc { get; init; }
}

public sealed record HardwareSingleAppState
{
    public required string SchemaVersion { get; init; }

    public required HardwarePendingTransaction? PendingTransaction { get; init; }
}

public interface IHardwareSingleAppStateStore
{
    Task<HardwarePendingTransaction?> LoadPendingAsync(CancellationToken cancellationToken = default);

    Task SavePendingAsync(
        HardwarePendingTransaction pendingTransaction,
        CancellationToken cancellationToken = default);

    Task MarkCaptureRequestDispatchAttemptedAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default);

    Task MarkCaptureRequestNotDispatchedAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default);

    Task ClearPendingAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default);
}

public sealed class HardwareSingleAppStateStore : IHardwareSingleAppStateStore
{
    private const long MaximumStateBytes = 64 * 1024;
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        RespectRequiredConstructorParameters = true,
        WriteIndented = true,
        MaxDepth = 4,
    };
    private readonly string _stateDirectory;
    private readonly string _statePath;
    private readonly string _lockPath;

    public HardwareSingleAppStateStore(string stateDirectory)
    {
        if (string.IsNullOrWhiteSpace(stateDirectory))
        {
            throw new ArgumentException("A hardware application-state directory is required.", nameof(stateDirectory));
        }

        _stateDirectory = Path.GetFullPath(stateDirectory);
        _statePath = Path.Combine(_stateDirectory, "app-state.json");
        _lockPath = Path.Combine(_stateDirectory, "app-state.lock");
    }

    public async Task<HardwarePendingTransaction?> LoadPendingAsync(
        CancellationToken cancellationToken = default)
    {
        if (!Directory.Exists(_stateDirectory))
        {
            return null;
        }

        EnsureDirectoryIsNotReparsePoint(_stateDirectory);
        await using var stateLock = AcquireStateLock();
        return await LoadPendingCoreAsync(cancellationToken).ConfigureAwait(false);
    }

    private async Task<HardwarePendingTransaction?> LoadPendingCoreAsync(
        CancellationToken cancellationToken)
    {
        if (!File.Exists(_statePath))
        {
            return null;
        }

        EnsureRegularLocalStateFile(_statePath);
        await using var stream = new FileStream(
            _statePath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 4096,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        if (stream.Length is <= 0 or > MaximumStateBytes)
        {
            throw new InvalidDataException("Hardware application state size is invalid.");
        }

        var bytes = new byte[(int)stream.Length];
        await stream.ReadExactlyAsync(bytes, cancellationToken).ConfigureAwait(false);
        HardwareSingleAppState state;
        try
        {
            RejectDuplicatePropertyNames(bytes);
            state = JsonSerializer.Deserialize<HardwareSingleAppState>(bytes, JsonOptions)
                ?? throw new InvalidDataException("Hardware application state is empty.");
        }
        catch (JsonException exception)
        {
            throw new InvalidDataException("Hardware application state is malformed.", exception);
        }

        ValidateState(state);
        return state.PendingTransaction;
    }

    public Task SavePendingAsync(
        HardwarePendingTransaction pendingTransaction,
        CancellationToken cancellationToken = default) =>
        SavePendingCoreWithLockAsync(pendingTransaction, cancellationToken);

    private async Task SavePendingCoreWithLockAsync(
        HardwarePendingTransaction pendingTransaction,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(pendingTransaction);
        ValidatePending(pendingTransaction);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_stateDirectory);
        Directory.CreateDirectory(_stateDirectory);
        EnsureDirectoryIsNotReparsePoint(_stateDirectory);
        await using var stateLock = AcquireStateLock();
        var existing = await LoadPendingCoreAsync(cancellationToken).ConfigureAwait(false);
        if (existing is not null && existing != pendingTransaction)
        {
            throw new InvalidOperationException(
                "Another hardware transaction is already present; its state was not overwritten.");
        }

        await WriteStateAsync(
            new HardwareSingleAppState
            {
                SchemaVersion = HardwareSingleAppStateProtocol.SchemaVersion,
                PendingTransaction = pendingTransaction,
            },
            cancellationToken).ConfigureAwait(false);
    }

    public async Task ClearPendingAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default)
    {
        ValidateTransactionId(expectedTransactionId);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_stateDirectory);
        Directory.CreateDirectory(_stateDirectory);
        EnsureDirectoryIsNotReparsePoint(_stateDirectory);
        await using var stateLock = AcquireStateLock();
        var existing = await LoadPendingCoreAsync(cancellationToken).ConfigureAwait(false);
        if (existing is null ||
            !string.Equals(existing.TransactionId, expectedTransactionId, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                "The expected hardware transaction is no longer the durable current transaction.");
        }

        await WriteStateAsync(
            new HardwareSingleAppState
            {
                SchemaVersion = HardwareSingleAppStateProtocol.SchemaVersion,
                PendingTransaction = null,
            },
            cancellationToken).ConfigureAwait(false);
    }

    public async Task MarkCaptureRequestDispatchAttemptedAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default)
    {
        ValidateTransactionId(expectedTransactionId);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_stateDirectory);
        Directory.CreateDirectory(_stateDirectory);
        EnsureDirectoryIsNotReparsePoint(_stateDirectory);
        await using var stateLock = AcquireStateLock();
        var existing = await LoadPendingCoreAsync(cancellationToken).ConfigureAwait(false);
        if (existing is null ||
            !string.Equals(existing.TransactionId, expectedTransactionId, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                "The expected hardware transaction is no longer the durable current transaction.");
        }

        if (existing.CaptureRequestDispatchAttempted)
        {
            return;
        }

        await WriteStateAsync(
            new HardwareSingleAppState
            {
                SchemaVersion = HardwareSingleAppStateProtocol.SchemaVersion,
                PendingTransaction = existing with { CaptureRequestDispatchAttempted = true },
            },
            cancellationToken).ConfigureAwait(false);
    }

    public async Task MarkCaptureRequestNotDispatchedAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default)
    {
        ValidateTransactionId(expectedTransactionId);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_stateDirectory);
        Directory.CreateDirectory(_stateDirectory);
        EnsureDirectoryIsNotReparsePoint(_stateDirectory);
        await using var stateLock = AcquireStateLock();
        var existing = await LoadPendingCoreAsync(cancellationToken).ConfigureAwait(false);
        if (existing is null ||
            !string.Equals(existing.TransactionId, expectedTransactionId, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                "The expected hardware transaction is no longer the durable current transaction.");
        }

        if (!existing.CaptureRequestDispatchAttempted)
        {
            return;
        }

        await WriteStateAsync(
            new HardwareSingleAppState
            {
                SchemaVersion = HardwareSingleAppStateProtocol.SchemaVersion,
                PendingTransaction = existing with { CaptureRequestDispatchAttempted = false },
            },
            cancellationToken).ConfigureAwait(false);
    }

    private async Task WriteStateAsync(
        HardwareSingleAppState state,
        CancellationToken cancellationToken)
    {
        ValidateState(state);
        var partialPath = Path.Combine(_stateDirectory, $"app-state-{Guid.NewGuid():N}.partial");
        try
        {
            await using (var stream = new FileStream(
                             partialPath,
                             FileMode.CreateNew,
                             FileAccess.Write,
                             FileShare.None,
                             bufferSize: 4096,
                             FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await JsonSerializer.SerializeAsync(stream, state, JsonOptions, cancellationToken)
                    .ConfigureAwait(false);
                await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
                stream.Flush(flushToDisk: true);
            }

            WindowsDurableFilePublisher.Publish(partialPath, _statePath, replaceExisting: true);
        }
        catch
        {
            if (File.Exists(partialPath))
            {
                File.Delete(partialPath);
            }

            throw;
        }
    }

    private static void ValidateState(HardwareSingleAppState state)
    {
        if (!string.Equals(
                state.SchemaVersion,
                HardwareSingleAppStateProtocol.SchemaVersion,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException("Hardware application-state schema is unsupported.");
        }

        if (state.PendingTransaction is not null)
        {
            ValidatePending(state.PendingTransaction);
        }
    }

    private static void ValidatePending(HardwarePendingTransaction pending)
    {
        ValidateTransactionId(pending.TransactionId);
        if (!string.Equals(pending.OperatingMode, "SingleCamera", StringComparison.Ordinal) ||
            pending.CameraAlias is not ("CAM-A" or "CAM-B") ||
            !string.Equals(pending.RequiredCameraAlias, pending.CameraAlias, StringComparison.Ordinal) ||
            !IsSafeToken(pending.CaptureProfileId, 128) ||
            pending.CaptureProfileVersion == 0 ||
            !IsLowerHex(pending.CaptureProfileSha256, 64) ||
            pending.CaptureProfileExpiresAtUtc.Offset != TimeSpan.Zero ||
            pending.StartedAtUtc.Offset != TimeSpan.Zero)
        {
            throw new InvalidDataException("Pending hardware transaction state is invalid.");
        }
    }

    private static bool IsSafeToken(string value, int maximumLength) =>
        !string.IsNullOrEmpty(value) && value.Length <= maximumLength &&
        value.All(character =>
            character is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or '-' or '_' or '.');

    private static bool IsLowerHex(string value, int exactLength) =>
        value.Length == exactLength && value.All(character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f');

    private FileStream AcquireStateLock()
    {
        if (File.Exists(_lockPath))
        {
            EnsureRegularLocalStateFile(_lockPath);
        }

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
            throw new InvalidOperationException(
                "Hardware application state is locked by another process; no state was changed.",
                exception);
        }
    }

    private static void ValidateTransactionId(string transactionId)
    {
        if (transactionId.Length != 32 || !transactionId.All(character =>
                character is >= '0' and <= '9' or >= 'a' and <= 'f'))
        {
            throw new InvalidDataException("Hardware transaction ID is invalid.");
        }
    }

    private static void EnsureRegularLocalStateFile(string path)
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(path);
        var attributes = File.GetAttributes(path);
        if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new InvalidDataException("Hardware application state must be a regular local file.");
        }
    }

    private static void EnsureDirectoryIsNotReparsePoint(string path)
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(path);
        var attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException("Hardware application-state directory cannot be a reparse point.");
        }
    }

    private static void RejectDuplicatePropertyNames(ReadOnlySpan<byte> json)
    {
        var reader = new Utf8JsonReader(
            json,
            new JsonReaderOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 4,
            });
        var objectProperties = new Stack<HashSet<string>>();
        while (reader.Read())
        {
            switch (reader.TokenType)
            {
                case JsonTokenType.StartObject:
                    objectProperties.Push(new HashSet<string>(StringComparer.Ordinal));
                    break;
                case JsonTokenType.PropertyName:
                    var propertyName = reader.GetString()
                        ?? throw new InvalidDataException("Hardware application state contains an invalid property name.");
                    if (objectProperties.Count == 0 || !objectProperties.Peek().Add(propertyName))
                    {
                        throw new InvalidDataException(
                            $"Hardware application state contains a duplicate property: {propertyName}");
                    }

                    break;
                case JsonTokenType.EndObject:
                    if (objectProperties.Count == 0)
                    {
                        throw new InvalidDataException("Hardware application state object structure is invalid.");
                    }

                    objectProperties.Pop();
                    break;
            }
        }

        if (objectProperties.Count != 0)
        {
            throw new InvalidDataException("Hardware application state object structure is incomplete.");
        }
    }
}
