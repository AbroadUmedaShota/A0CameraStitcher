using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace A0CameraStitcher.M3.Foundation;

public sealed class DurableSimulatedCaptureCoordinator
{
    private static readonly JsonSerializerOptions JournalSerializerOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        RespectRequiredConstructorParameters = true,
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() },
    };

    private readonly string _rootDirectory;
    private readonly ISimulatedCaptureSource _captureSource;
    private readonly TimeProvider _timeProvider;
    private readonly SemaphoreSlim _transactionGate = new(1, 1);

    public DurableSimulatedCaptureCoordinator(
        string rootDirectory,
        ISimulatedCaptureSource captureSource,
        TimeProvider? timeProvider = null)
    {
        if (string.IsNullOrWhiteSpace(rootDirectory))
        {
            throw new ArgumentException("A transaction root is required.", nameof(rootDirectory));
        }

        _rootDirectory = Path.GetFullPath(rootDirectory);
        _captureSource = captureSource ?? throw new ArgumentNullException(nameof(captureSource));
        _timeProvider = timeProvider ?? TimeProvider.System;
    }

    public async Task<IReadOnlyList<SimulatedTransactionJournal>> InitializeAsync(
        CancellationToken cancellationToken = default)
    {
        await _transactionGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            Directory.CreateDirectory(_rootDirectory);
            using var rootLease = AcquireRootLease();
            var recovered = new List<SimulatedTransactionJournal>();
            foreach (var transactionDirectory in EnumerateTransactionDirectories())
            {
                cancellationToken.ThrowIfCancellationRequested();
                var transactionId = ParseTransactionId(transactionDirectory);
                var journalPath = GetJournalPath(transactionId);
                var partialArtifacts = Directory.EnumerateFiles(
                    transactionDirectory,
                    "*.partial",
                    SearchOption.AllDirectories).ToArray();

                if (!File.Exists(journalPath))
                {
                    if (partialArtifacts.Length == 0)
                    {
                        continue;
                    }

                    var partialJournal = CreateJournal(transactionId);
                    Transition(partialJournal, SimulatedTransactionState.FailedPartial);
                    partialJournal.TerminalReason = "RestartDetectedPartialArtifact";
                    partialJournal.Originals.AddRange(
                        await DiscoverRetainedOriginalsAsync(
                            transactionId,
                            cancellationToken).ConfigureAwait(false));
                    await SaveJournalAsync(partialJournal, cancellationToken).ConfigureAwait(false);
                    recovered.Add(partialJournal);
                    continue;
                }

                var journal = await LoadJournalAtPathAsync(journalPath, cancellationToken).ConfigureAwait(false);
                if (IsTerminal(journal.State))
                {
                    if (journal.State == SimulatedTransactionState.FailedPartial)
                    {
                        recovered.Add(journal);
                    }
                    continue;
                }

                Transition(journal, SimulatedTransactionState.FailedPartial);
                journal.TerminalReason = partialArtifacts.Length == 0
                    ? "RestartDetectedIncompleteTransaction"
                    : "RestartDetectedIncompleteTransactionWithPartialArtifact";
                var discoveredOriginals = await DiscoverRetainedOriginalsAsync(
                    transactionId,
                    cancellationToken).ConfigureAwait(false);
                foreach (var discoveredOriginal in discoveredOriginals)
                {
                    if (journal.Originals.All(original =>
                            !string.Equals(original.Alias, discoveredOriginal.Alias, StringComparison.Ordinal)))
                    {
                        journal.Originals.Add(discoveredOriginal);
                    }
                }

                await SaveJournalAsync(journal, cancellationToken).ConfigureAwait(false);
                recovered.Add(journal);
            }

            return recovered;
        }
        finally
        {
            _transactionGate.Release();
        }
    }

    public async Task<SimulatedTransactionJournal> RecordLiveViewStopFailureAsync(
        Guid transactionId,
        CancellationToken cancellationToken = default)
    {
        ValidateTransactionId(transactionId);

        await _transactionGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            using var rootLease = AcquireRootLease();
            EnsureTransactionCanStart(transactionId);

            var journal = CreateJournal(transactionId);
            await SaveJournalAsync(journal, cancellationToken).ConfigureAwait(false);
            await MarkFailedPartialAsync(journal, "LiveViewStopFailed").ConfigureAwait(false);
            return journal;
        }
        finally
        {
            _transactionGate.Release();
        }
    }

    public async Task<SimulatedTransactionJournal> ExecuteAsync(
        Guid transactionId,
        SimulatedCrashPoint crashPoint = SimulatedCrashPoint.None,
        CancellationToken cancellationToken = default)
    {
        ValidateTransactionId(transactionId);

        await _transactionGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            using var rootLease = AcquireRootLease();
            EnsureTransactionCanStart(transactionId);

            var journal = CreateJournal(transactionId);
            await SaveJournalAsync(journal, cancellationToken).ConfigureAwait(false);

            try
            {
                await CaptureAndPersistAsync(journal, "CAM-A", cancellationToken).ConfigureAwait(false);
                if (crashPoint == SimulatedCrashPoint.AfterPersistA)
                {
                    throw new SimulatedProcessCrashException(transactionId);
                }

                await CaptureAndPersistAsync(journal, "CAM-B", cancellationToken).ConfigureAwait(false);
                Transition(journal, SimulatedTransactionState.Complete);
                await SaveJournalAsync(journal, cancellationToken).ConfigureAwait(false);
                return journal;
            }
            catch (SimulatedProcessCrashException)
            {
                // Intentionally leave the durable journal incomplete. The next InitializeAsync call closes it.
                throw;
            }
            catch (OperationCanceledException)
            {
                await MarkFailedPartialAsync(journal, "OperationCanceled").ConfigureAwait(false);
                throw;
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                await MarkFailedPartialAsync(journal, "CaptureOrPersistenceFailure").ConfigureAwait(false);
                return journal;
            }
        }
        finally
        {
            _transactionGate.Release();
        }
    }

    public Task<SimulatedTransactionJournal> LoadAsync(
        Guid transactionId,
        CancellationToken cancellationToken = default) =>
        LoadJournalAtPathAsync(GetJournalPath(transactionId), cancellationToken);

    public string GetOriginalPath(Guid transactionId, string alias)
    {
        ValidateAlias(alias);
        return Path.Combine(GetTransactionDirectory(transactionId), alias, "original.simulated");
    }

    private async Task CaptureAndPersistAsync(
        SimulatedTransactionJournal journal,
        string alias,
        CancellationToken cancellationToken)
    {
        ValidateAlias(alias);
        Transition(
            journal,
            alias == "CAM-A" ? SimulatedTransactionState.CaptureA : SimulatedTransactionState.CaptureB);
        await SaveJournalAsync(journal, cancellationToken).ConfigureAwait(false);

        var content = await _captureSource.CaptureAsync(
            alias,
            journal.TransactionId,
            cancellationToken).ConfigureAwait(false);
        if (!content.Text.Contains(SimulatedTransactionProtocol.Marker, StringComparison.Ordinal))
        {
            throw new InvalidDataException("Simulated capture content is missing its marker.");
        }

        Transition(
            journal,
            alias == "CAM-A" ? SimulatedTransactionState.PersistA : SimulatedTransactionState.PersistB);
        await SaveJournalAsync(journal, cancellationToken).ConfigureAwait(false);

        var record = await PersistOriginalAsync(
            journal.TransactionId,
            alias,
            content,
            cancellationToken).ConfigureAwait(false);
        journal.Originals.Add(record);
        journal.UpdatedAtUtc = _timeProvider.GetUtcNow();
        await SaveJournalAsync(journal, cancellationToken).ConfigureAwait(false);
    }

    private async Task<SimulatedOriginalRecord> PersistOriginalAsync(
        Guid transactionId,
        string alias,
        SimulatedCaptureContent content,
        CancellationToken cancellationToken)
    {
        var targetPath = GetOriginalPath(transactionId, alias);
        Directory.CreateDirectory(Path.GetDirectoryName(targetPath)!);
        var partialPath = targetPath + $".{Guid.NewGuid():N}.partial";
        var bytes = Encoding.UTF8.GetBytes(content.Text);

        await WriteDurablyAsync(partialPath, bytes, cancellationToken).ConfigureAwait(false);
        File.Move(partialPath, targetPath, overwrite: true);

        return new SimulatedOriginalRecord
        {
            Alias = alias,
            RelativePath = Path.GetRelativePath(GetTransactionDirectory(transactionId), targetPath),
            Size = bytes.LongLength,
            Sha256 = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
            Marker = SimulatedTransactionProtocol.Marker,
        };
    }

    private async Task MarkFailedPartialAsync(SimulatedTransactionJournal journal, string reason)
    {
        if (!IsTerminal(journal.State))
        {
            Transition(journal, SimulatedTransactionState.FailedPartial);
        }

        journal.TerminalReason = reason;
        await SaveJournalAsync(journal, CancellationToken.None).ConfigureAwait(false);
    }

    private SimulatedTransactionJournal CreateJournal(Guid transactionId)
    {
        var now = _timeProvider.GetUtcNow();
        return new SimulatedTransactionJournal
        {
            SchemaVersion = SimulatedTransactionProtocol.SchemaVersion,
            Simulation = true,
            Marker = SimulatedTransactionProtocol.Marker,
            TransactionId = transactionId,
            State = SimulatedTransactionState.Idle,
            TransitionHistory = [SimulatedTransactionState.Idle],
            Originals = [],
            AutomaticRetryCount = 0,
            TerminalReason = null,
            UpdatedAtUtc = now,
        };
    }

    private void Transition(SimulatedTransactionJournal journal, SimulatedTransactionState nextState)
    {
        var expectedNextState = journal.State switch
        {
            SimulatedTransactionState.Idle => SimulatedTransactionState.CaptureA,
            SimulatedTransactionState.CaptureA => SimulatedTransactionState.PersistA,
            SimulatedTransactionState.PersistA => SimulatedTransactionState.CaptureB,
            SimulatedTransactionState.CaptureB => SimulatedTransactionState.PersistB,
            SimulatedTransactionState.PersistB => SimulatedTransactionState.Complete,
            _ => (SimulatedTransactionState?)null,
        };

        var validFailureTransition =
            nextState == SimulatedTransactionState.FailedPartial && !IsTerminal(journal.State);
        if (nextState != expectedNextState && !validFailureTransition)
        {
            throw new InvalidOperationException($"Invalid simulated transition {journal.State} -> {nextState}.");
        }

        journal.State = nextState;
        journal.TransitionHistory.Add(nextState);
        journal.UpdatedAtUtc = _timeProvider.GetUtcNow();
    }

    private async Task SaveJournalAsync(
        SimulatedTransactionJournal journal,
        CancellationToken cancellationToken)
    {
        ValidateJournal(journal);
        var journalPath = GetJournalPath(journal.TransactionId);
        Directory.CreateDirectory(Path.GetDirectoryName(journalPath)!);
        var partialPath = journalPath + $".{Guid.NewGuid():N}.partial";
        var bytes = JsonSerializer.SerializeToUtf8Bytes(journal, JournalSerializerOptions);
        await WriteDurablyAsync(partialPath, bytes, cancellationToken).ConfigureAwait(false);
        File.Move(partialPath, journalPath, overwrite: true);
    }

    private static async Task WriteDurablyAsync(
        string path,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(
            path,
            FileMode.Create,
            FileAccess.Write,
            FileShare.None,
            bufferSize: 4096,
            FileOptions.Asynchronous | FileOptions.WriteThrough);
        await stream.WriteAsync(bytes, cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
        stream.Flush(flushToDisk: true);
    }

    private static bool IsTerminal(SimulatedTransactionState state) =>
        state is SimulatedTransactionState.Complete or SimulatedTransactionState.FailedPartial;

    private async Task<SimulatedTransactionJournal> LoadJournalAtPathAsync(
        string journalPath,
        CancellationToken cancellationToken)
    {
        var json = await File.ReadAllTextAsync(journalPath, cancellationToken).ConfigureAwait(false);
        var journal = JsonSerializer.Deserialize<SimulatedTransactionJournal>(json, JournalSerializerOptions)
            ?? throw new InvalidDataException("The simulated transaction journal is empty.");
        ValidateJournal(journal);
        return journal;
    }

    private async Task<IReadOnlyList<SimulatedOriginalRecord>> DiscoverRetainedOriginalsAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        var records = new List<SimulatedOriginalRecord>();
        foreach (var alias in new[] { "CAM-A", "CAM-B" })
        {
            var path = GetOriginalPath(transactionId, alias);
            if (!File.Exists(path))
            {
                continue;
            }

            var bytes = await File.ReadAllBytesAsync(path, cancellationToken).ConfigureAwait(false);
            var text = Encoding.UTF8.GetString(bytes);
            if (!text.Contains(SimulatedTransactionProtocol.Marker, StringComparison.Ordinal))
            {
                throw new InvalidDataException("A retained simulated original is missing its marker.");
            }

            records.Add(new SimulatedOriginalRecord
            {
                Alias = alias,
                RelativePath = Path.GetRelativePath(GetTransactionDirectory(transactionId), path),
                Size = bytes.LongLength,
                Sha256 = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
                Marker = SimulatedTransactionProtocol.Marker,
            });
        }

        return records;
    }

    private bool HasUnresolvedTransaction()
    {
        foreach (var transactionDirectory in EnumerateTransactionDirectories())
        {
            var transactionId = ParseTransactionId(transactionDirectory);
            var journalPath = GetJournalPath(transactionId);
            if (!File.Exists(journalPath))
            {
                if (Directory.EnumerateFileSystemEntries(
                    transactionDirectory,
                    "*",
                    SearchOption.AllDirectories).Any())
                {
                    return true;
                }

                continue;
            }

            var journal = LoadJournalAtPathAsync(journalPath, CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            if (!IsTerminal(journal.State))
            {
                return true;
            }
        }

        return false;
    }

    private void EnsureTransactionCanStart(Guid transactionId)
    {
        if (File.Exists(GetJournalPath(transactionId)))
        {
            throw new InvalidOperationException("An existing transaction cannot be resumed or retried.");
        }

        if (Directory.Exists(GetTransactionDirectory(transactionId)) &&
            Directory.EnumerateFileSystemEntries(
                GetTransactionDirectory(transactionId),
                "*",
                SearchOption.AllDirectories).Any())
        {
            throw new InvalidOperationException("A transaction with diagnostic remnants cannot be resumed or retried.");
        }

        if (HasUnresolvedTransaction())
        {
            throw new InvalidOperationException("An incomplete transaction must be closed by InitializeAsync before a new transaction starts.");
        }
    }

    private static void ValidateTransactionId(Guid transactionId)
    {
        if (transactionId == Guid.Empty)
        {
            throw new ArgumentException("A non-empty transaction ID is required.", nameof(transactionId));
        }
    }

    private IEnumerable<string> EnumerateTransactionDirectories()
    {
        if (!Directory.Exists(_rootDirectory))
        {
            return [];
        }

        return Directory.EnumerateDirectories(_rootDirectory)
            .Where(directory => Guid.TryParseExact(Path.GetFileName(directory), "N", out _));
    }

    private static Guid ParseTransactionId(string transactionDirectory) =>
        Guid.TryParseExact(Path.GetFileName(transactionDirectory), "N", out var transactionId)
            ? transactionId
            : throw new InvalidDataException("The transaction directory name is invalid.");

    private RootExecutionLease AcquireRootLease() =>
        RootExecutionLease.Acquire(Path.Combine(_rootDirectory, ".simulated-transaction.lock"));

    private static void ValidateJournal(SimulatedTransactionJournal journal)
    {
        if (!string.Equals(
                journal.SchemaVersion,
                SimulatedTransactionProtocol.SchemaVersion,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException("The simulated transaction journal version is unsupported.");
        }

        if (!journal.Simulation ||
            !string.Equals(journal.Marker, SimulatedTransactionProtocol.Marker, StringComparison.Ordinal))
        {
            throw new InvalidDataException("The simulated transaction journal marker is invalid.");
        }

        if (journal.AutomaticRetryCount != 0)
        {
            throw new InvalidDataException("Automatic retry is not permitted.");
        }

        if (journal.Originals.Any(original =>
                !string.Equals(original.Marker, SimulatedTransactionProtocol.Marker, StringComparison.Ordinal) ||
                !original.RelativePath.EndsWith(".simulated", StringComparison.OrdinalIgnoreCase)))
        {
            throw new InvalidDataException("A retained original is not visibly simulated.");
        }
    }

    private string GetJournalPath(Guid transactionId) =>
        Path.Combine(GetTransactionDirectory(transactionId), "transaction.json");

    private string GetTransactionDirectory(Guid transactionId) =>
        Path.Combine(_rootDirectory, transactionId.ToString("N"));

    private static void ValidateAlias(string alias)
    {
        if (alias is not ("CAM-A" or "CAM-B"))
        {
            throw new ArgumentOutOfRangeException(nameof(alias));
        }
    }

    private sealed class RootExecutionLease : IDisposable
    {
        private const long LockOffset = 0;
        private const long LockLength = 1;
        private readonly FileStream _stream;
        private bool _disposed;

        private RootExecutionLease(FileStream stream)
        {
            _stream = stream;
        }

        public static RootExecutionLease Acquire(string path)
        {
            if (OperatingSystem.IsMacOS())
            {
                throw new PlatformNotSupportedException("The simulated transaction file lock is not supported on macOS.");
            }

            var stream = new FileStream(
                path,
                FileMode.OpenOrCreate,
                FileAccess.ReadWrite,
                FileShare.ReadWrite | FileShare.Delete,
                bufferSize: 4096,
                FileOptions.WriteThrough);
            try
            {
                stream.Lock(LockOffset, LockLength);
                stream.Position = 0;
                stream.SetLength(0);
                using var writer = new StreamWriter(stream, Encoding.UTF8, bufferSize: 1024, leaveOpen: true);
                writer.WriteLine(SimulatedTransactionProtocol.Marker);
                writer.WriteLine("root ownership for one simulated transaction");
                writer.Flush();
                stream.Flush(flushToDisk: true);
                return new RootExecutionLease(stream);
            }
            catch (IOException exception)
            {
                stream.Dispose();
                throw new InvalidOperationException(
                    "Another process or service owns the simulated transaction root.",
                    exception);
            }
            catch
            {
                stream.Dispose();
                throw;
            }
        }

        public void Dispose()
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            try
            {
                if (!OperatingSystem.IsMacOS())
                {
                    _stream.Unlock(LockOffset, LockLength);
                }
            }
            finally
            {
                _stream.Dispose();
            }
        }
    }
}
